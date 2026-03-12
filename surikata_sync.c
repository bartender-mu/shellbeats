/**
 * Surikata Sync Module for ShellBeats
 *
 * HTTPS sync of playlists and config to surikata.app.
 * Uses libcurl for HTTP and cJSON for JSON handling.
 */

#include "surikata_sync.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include <cjson/cJSON.h>

// ── Internal: curl response buffer ──────────────────────────────────

typedef struct {
    char *data;
    size_t size;
} response_buf_t;

static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t total = size * nmemb;
    response_buf_t *buf = (response_buf_t *)userp;

    char *tmp = realloc(buf->data, buf->size + total + 1);
    if (!tmp) return 0;

    buf->data = tmp;
    memcpy(buf->data + buf->size, contents, total);
    buf->size += total;
    buf->data[buf->size] = '\0';

    return total;
}

// ── Internal: HTTP request helper ───────────────────────────────────

typedef struct {
    long http_code;
    char *body;
    size_t body_len;
    sb_error_t error;
} http_result_t;

static http_result_t http_request(const sb_sync_config_t *cfg,
                                   const char *method,
                                   const char *endpoint,
                                   const char *json_body) {
    http_result_t result = {0};
    result.error = SB_OK;

    CURL *curl = curl_easy_init();
    if (!curl) {
        result.error = SB_ERR_NETWORK;
        return result;
    }

    // Build URL
    char url[512];
    snprintf(url, sizeof(url), "%s%s", cfg->url, endpoint);

    // Auth header
    char auth_header[128];
    snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", cfg->token);

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, auth_header);
    headers = curl_slist_append(headers, "Content-Type: application/json");

    response_buf_t resp = {NULL, 0};

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "ShellBeats/1.0");

    if (strcmp(method, "POST") == 0) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        if (json_body) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body);
        } else {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "");
        }
    } else if (strcmp(method, "DELETE") == 0) {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
    }

    CURLcode res = curl_easy_perform(curl);

    if (res != CURLE_OK) {
        result.error = SB_ERR_NETWORK;
        free(resp.data);
    } else {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.http_code);
        result.body = resp.data;
        result.body_len = resp.size;

        if (result.http_code == 401) {
            result.error = SB_ERR_INVALID_TOKEN;
        } else if (result.http_code == 429) {
            result.error = SB_ERR_RATE_LIMITED;
        } else if (result.http_code >= 500) {
            result.error = SB_ERR_SERVER;
        }
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    return result;
}

// ── Internal: JSON helpers ──────────────────────────────────────────

static cJSON *playlist_to_json(const sb_playlist_t *pl) {
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "name", pl->name);
    cJSON_AddStringToObject(obj, "type", pl->type);
    cJSON_AddBoolToObject(obj, "is_shared", pl->is_shared);

    cJSON *songs = cJSON_CreateArray();
    for (int i = 0; i < pl->song_count; i++) {
        cJSON *song = cJSON_CreateObject();
        cJSON_AddStringToObject(song, "title", pl->songs[i].title);
        cJSON_AddStringToObject(song, "video_id", pl->songs[i].video_id);
        cJSON_AddNumberToObject(song, "duration", pl->songs[i].duration);
        cJSON_AddItemToArray(songs, song);
    }
    cJSON_AddItemToObject(obj, "songs", songs);

    return obj;
}

static sb_playlist_t json_to_playlist(const cJSON *obj) {
    sb_playlist_t pl = {0};

    cJSON *name = cJSON_GetObjectItem(obj, "name");
    cJSON *type = cJSON_GetObjectItem(obj, "type");
    cJSON *shared = cJSON_GetObjectItem(obj, "is_shared");
    cJSON *songs = cJSON_GetObjectItem(obj, "songs");

    if (cJSON_IsString(name))
        snprintf(pl.name, sizeof(pl.name), "%s", name->valuestring);
    if (cJSON_IsString(type))
        snprintf(pl.type, sizeof(pl.type), "%s", type->valuestring);
    else
        snprintf(pl.type, sizeof(pl.type), "youtube");
    pl.is_shared = cJSON_IsTrue(shared);

    if (cJSON_IsArray(songs)) {
        int count = cJSON_GetArraySize(songs);
        pl.songs = calloc(count, sizeof(sb_song_t));
        if (pl.songs) {
            pl.song_count = count;
            for (int i = 0; i < count; i++) {
                cJSON *s = cJSON_GetArrayItem(songs, i);
                cJSON *title = cJSON_GetObjectItem(s, "title");
                cJSON *vid = cJSON_GetObjectItem(s, "video_id");
                cJSON *dur = cJSON_GetObjectItem(s, "duration");

                if (cJSON_IsString(title))
                    snprintf(pl.songs[i].title, sizeof(pl.songs[i].title), "%s", title->valuestring);
                if (cJSON_IsString(vid))
                    snprintf(pl.songs[i].video_id, sizeof(pl.songs[i].video_id), "%s", vid->valuestring);
                if (cJSON_IsNumber(dur))
                    pl.songs[i].duration = dur->valueint;
            }
        }
    }

    return pl;
}

// ── Public API ──────────────────────────────────────────────────────

void sb_sync_init(void) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

void sb_sync_cleanup(void) {
    curl_global_cleanup();
}

sb_sync_config_t sb_load_config(const char *config_dir) {
    sb_sync_config_t cfg = {0};
    cfg.enabled = false;

    char config_path[512];
    snprintf(config_path, sizeof(config_path), "%s/surikata.json", config_dir);

    FILE *f = fopen(config_path, "r");
    if (!f) return cfg;

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (len <= 0 || len > 65536) {
        fclose(f);
        return cfg;
    }

    char *content = malloc(len + 1);
    if (!content) {
        fclose(f);
        return cfg;
    }

    size_t read_len = fread(content, 1, len, f);
    fclose(f);
    content[read_len] = '\0';

    cJSON *root = cJSON_Parse(content);
    free(content);

    if (!root) return cfg;

    cJSON *url = cJSON_GetObjectItem(root, "url");
    cJSON *token = cJSON_GetObjectItem(root, "token");
    cJSON *enabled = cJSON_GetObjectItem(root, "sync_enabled");

    if (cJSON_IsString(url))
        snprintf(cfg.url, sizeof(cfg.url), "%s", url->valuestring);
    else
        snprintf(cfg.url, sizeof(cfg.url), "https://surikata.app");

    if (cJSON_IsString(token))
        snprintf(cfg.token, sizeof(cfg.token), "%s", token->valuestring);

    cfg.enabled = cJSON_IsTrue(enabled) || (cJSON_IsString(token) && strlen(token->valuestring) > 0);

    cJSON *pull_startup = cJSON_GetObjectItem(root, "pull_on_startup");
    cJSON *sync_quit = cJSON_GetObjectItem(root, "sync_on_quit");
    cfg.pull_on_startup = cJSON_IsTrue(pull_startup);
    cfg.sync_on_quit = cJSON_IsTrue(sync_quit);

    cJSON_Delete(root);
    return cfg;
}

sb_error_t sb_save_config(const char *config_dir, const sb_sync_config_t *cfg) {
    char config_path[512];
    snprintf(config_path, sizeof(config_path), "%s/surikata.json", config_dir);

    cJSON *root = cJSON_CreateObject();
    if (!root) return SB_ERR_FILE_IO;

    cJSON_AddStringToObject(root, "url", cfg->url);
    if (strlen(cfg->token) > 0) {
        cJSON_AddStringToObject(root, "token", cfg->token);
    }
    cJSON_AddBoolToObject(root, "sync_enabled", cfg->enabled);
    cJSON_AddBoolToObject(root, "pull_on_startup", cfg->pull_on_startup);
    cJSON_AddBoolToObject(root, "sync_on_quit", cfg->sync_on_quit);

    char *json_str = cJSON_Print(root);
    cJSON_Delete(root);

    if (!json_str) return SB_ERR_FILE_IO;

    FILE *f = fopen(config_path, "w");
    if (!f) {
        free(json_str);
        return SB_ERR_FILE_IO;
    }

    fputs(json_str, f);
    fclose(f);
    free(json_str);

    return SB_OK;
}

sb_verify_result_t sb_verify(const sb_sync_config_t *cfg) {
    sb_verify_result_t result = {0};

    if (strlen(cfg->token) == 0) {
        result.success = false;
        snprintf(result.error_msg, sizeof(result.error_msg), "No token configured");
        return result;
    }

    http_result_t resp = http_request(cfg, "GET", "/api/shellbeats/verify.php", NULL);

    if (resp.error != SB_OK) {
        result.success = false;
        snprintf(result.error_msg, sizeof(result.error_msg), "%s", sb_error_str(resp.error));
        free(resp.body);
        return result;
    }

    cJSON *json = cJSON_Parse(resp.body);
    free(resp.body);

    if (!json) {
        result.success = false;
        snprintf(result.error_msg, sizeof(result.error_msg), "Invalid server response");
        return result;
    }

    cJSON *success = cJSON_GetObjectItem(json, "success");
    cJSON *username = cJSON_GetObjectItem(json, "username");
    cJSON *user_id = cJSON_GetObjectItem(json, "user_id");
    cJSON *playlists = cJSON_GetObjectItem(json, "playlists_synced");
    cJSON *latest_ver = cJSON_GetObjectItem(json, "latest_version");

    result.success = cJSON_IsTrue(success);
    if (cJSON_IsString(username))
        snprintf(result.username, sizeof(result.username), "%s", username->valuestring);
    if (cJSON_IsNumber(user_id))
        result.user_id = user_id->valueint;
    if (cJSON_IsNumber(playlists))
        result.playlists_synced = playlists->valueint;
    if (cJSON_IsString(latest_ver))
        snprintf(result.latest_version, sizeof(result.latest_version), "%s", latest_ver->valuestring);

    if (!result.success) {
        cJSON *msg = cJSON_GetObjectItem(json, "message");
        if (cJSON_IsString(msg))
            snprintf(result.error_msg, sizeof(result.error_msg), "%s", msg->valuestring);
    }

    cJSON_Delete(json);
    return result;
}

sb_push_result_t sb_push_playlist(const sb_sync_config_t *cfg, const sb_playlist_t *playlist) {
    sb_push_result_t result = {0};

    if (strlen(cfg->token) == 0) {
        result.success = false;
        snprintf(result.error_msg, sizeof(result.error_msg), "No token configured");
        return result;
    }

    cJSON *body = playlist_to_json(playlist);
    char *json_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);

    if (!json_str) {
        result.success = false;
        snprintf(result.error_msg, sizeof(result.error_msg), "JSON serialization failed");
        return result;
    }

    http_result_t resp = http_request(cfg, "POST", "/api/shellbeats/push-playlist.php", json_str);
    free(json_str);

    if (resp.error != SB_OK) {
        result.success = false;
        snprintf(result.error_msg, sizeof(result.error_msg), "%s", sb_error_str(resp.error));
        free(resp.body);
        return result;
    }

    cJSON *json = cJSON_Parse(resp.body);
    free(resp.body);

    if (!json) {
        result.success = false;
        snprintf(result.error_msg, sizeof(result.error_msg), "Invalid server response");
        return result;
    }

    cJSON *success = cJSON_GetObjectItem(json, "success");
    result.success = cJSON_IsTrue(success);
    result.synced_count = result.success ? 1 : 0;

    if (!result.success) {
        cJSON *msg = cJSON_GetObjectItem(json, "message");
        if (cJSON_IsString(msg))
            snprintf(result.error_msg, sizeof(result.error_msg), "%s", msg->valuestring);
    }

    cJSON_Delete(json);
    return result;
}

sb_push_result_t sb_push_all(const sb_sync_config_t *cfg,
                              const sb_playlist_t *playlists, int count,
                              const char *config_json) {
    sb_push_result_t result = {0};

    if (strlen(cfg->token) == 0) {
        result.success = false;
        snprintf(result.error_msg, sizeof(result.error_msg), "No token configured");
        return result;
    }

    cJSON *body = cJSON_CreateObject();

    // Config (parse string to object, or null)
    if (config_json) {
        cJSON *config_obj = cJSON_Parse(config_json);
        if (config_obj) {
            // Remove token from config before sending
            cJSON_DeleteItemFromObject(config_obj, "surikata_token");
            cJSON_AddItemToObject(body, "config", config_obj);
        } else {
            cJSON_AddNullToObject(body, "config");
        }
    }

    // Playlists array
    cJSON *pl_array = cJSON_CreateArray();
    for (int i = 0; i < count; i++) {
        cJSON_AddItemToArray(pl_array, playlist_to_json(&playlists[i]));
    }
    cJSON_AddItemToObject(body, "playlists", pl_array);

    char *json_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);

    if (!json_str) {
        result.success = false;
        snprintf(result.error_msg, sizeof(result.error_msg), "JSON serialization failed");
        return result;
    }

    http_result_t resp = http_request(cfg, "POST", "/api/shellbeats/push.php", json_str);
    free(json_str);

    if (resp.error != SB_OK) {
        result.success = false;
        snprintf(result.error_msg, sizeof(result.error_msg), "%s", sb_error_str(resp.error));
        free(resp.body);
        return result;
    }

    cJSON *json = cJSON_Parse(resp.body);
    free(resp.body);

    if (!json) {
        result.success = false;
        snprintf(result.error_msg, sizeof(result.error_msg), "Invalid server response");
        return result;
    }

    cJSON *success = cJSON_GetObjectItem(json, "success");
    cJSON *synced = cJSON_GetObjectItem(json, "synced_playlists");

    result.success = cJSON_IsTrue(success);
    if (cJSON_IsNumber(synced))
        result.synced_count = synced->valueint;

    if (!result.success) {
        cJSON *msg = cJSON_GetObjectItem(json, "message");
        if (cJSON_IsString(msg))
            snprintf(result.error_msg, sizeof(result.error_msg), "%s", msg->valuestring);
    }

    cJSON_Delete(json);
    return result;
}

sb_pull_result_t sb_pull_all(const sb_sync_config_t *cfg) {
    sb_pull_result_t result = {0};

    if (strlen(cfg->token) == 0) {
        result.success = false;
        snprintf(result.error_msg, sizeof(result.error_msg), "No token configured");
        return result;
    }

    http_result_t resp = http_request(cfg, "GET", "/api/shellbeats/pull.php", NULL);

    if (resp.error != SB_OK) {
        result.success = false;
        snprintf(result.error_msg, sizeof(result.error_msg), "%s", sb_error_str(resp.error));
        free(resp.body);
        return result;
    }

    cJSON *json = cJSON_Parse(resp.body);
    free(resp.body);

    if (!json) {
        result.success = false;
        snprintf(result.error_msg, sizeof(result.error_msg), "Invalid server response");
        return result;
    }

    cJSON *success = cJSON_GetObjectItem(json, "success");
    result.success = cJSON_IsTrue(success);

    if (!result.success) {
        cJSON *msg = cJSON_GetObjectItem(json, "message");
        if (cJSON_IsString(msg))
            snprintf(result.error_msg, sizeof(result.error_msg), "%s", msg->valuestring);
        cJSON_Delete(json);
        return result;
    }

    // Parse config
    cJSON *config = cJSON_GetObjectItem(json, "config");
    if (config && !cJSON_IsNull(config)) {
        result.config_json = cJSON_PrintUnformatted(config);
    }

    // Parse playlists
    cJSON *playlists = cJSON_GetObjectItem(json, "playlists");
    if (cJSON_IsArray(playlists)) {
        int count = cJSON_GetArraySize(playlists);
        result.playlists = calloc(count, sizeof(sb_playlist_t));
        if (result.playlists) {
            result.playlist_count = count;
            for (int i = 0; i < count; i++) {
                result.playlists[i] = json_to_playlist(cJSON_GetArrayItem(playlists, i));
            }
        }
    }

    cJSON_Delete(json);
    return result;
}

sb_error_t sb_delete_playlist(const sb_sync_config_t *cfg, const char *name) {
    if (strlen(cfg->token) == 0) return SB_ERR_NO_TOKEN;

    // URL-encode the name
    CURL *curl = curl_easy_init();
    if (!curl) return SB_ERR_NETWORK;

    char *encoded = curl_easy_escape(curl, name, 0);
    curl_easy_cleanup(curl);

    if (!encoded) return SB_ERR_NETWORK;

    char endpoint[512];
    snprintf(endpoint, sizeof(endpoint), "/api/shellbeats/delete-playlist.php?name=%s", encoded);
    curl_free(encoded);

    http_result_t resp = http_request(cfg, "DELETE", endpoint, NULL);
    free(resp.body);

    return resp.error;
}

void sb_free_pull_result(sb_pull_result_t *result) {
    if (!result) return;

    free(result->config_json);
    result->config_json = NULL;

    if (result->playlists) {
        for (int i = 0; i < result->playlist_count; i++) {
            sb_free_playlist(&result->playlists[i]);
        }
        free(result->playlists);
        result->playlists = NULL;
    }
    result->playlist_count = 0;
}

void sb_free_playlist(sb_playlist_t *pl) {
    if (!pl) return;
    free(pl->songs);
    pl->songs = NULL;
    pl->song_count = 0;
}

const char *sb_error_str(sb_error_t err) {
    switch (err) {
        case SB_OK:              return "Success";
        case SB_ERR_NO_TOKEN:    return "No token configured. Run: shellbeats link <token>";
        case SB_ERR_INVALID_TOKEN: return "Token invalid or revoked. Generate a new one on surikata.app";
        case SB_ERR_NETWORK:     return "Network error. Check your internet connection";
        case SB_ERR_SERVER:      return "Server error. Try again later";
        case SB_ERR_PARSE:       return "Invalid response from server";
        case SB_ERR_RATE_LIMITED: return "Too many requests. Wait a moment and try again";
        case SB_ERR_FILE_IO:     return "File I/O error";
        default:                 return "Unknown error";
    }
}

void sb_check_latest_version(const char *base_url, char *out_version, size_t out_size) {
    if (!out_version || out_size == 0) return;
    out_version[0] = '\0';

    const char *url_base = base_url;
    if (!url_base || strlen(url_base) == 0)
        url_base = "https://surikata.app";

    CURL *curl = curl_easy_init();
    if (!curl) return;

    char url[512];
    snprintf(url, sizeof(url), "%s/api/shellbeats/version.php", url_base);

    response_buf_t resp = {NULL, 0};

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 3L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "ShellBeats/1.0");

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || !resp.data) {
        free(resp.data);
        return;
    }

    cJSON *json = cJSON_Parse(resp.data);
    free(resp.data);

    if (!json) return;

    cJSON *ver = cJSON_GetObjectItem(json, "latest_version");
    if (cJSON_IsString(ver) && ver->valuestring) {
        strncpy(out_version, ver->valuestring, out_size - 1);
        out_version[out_size - 1] = '\0';
    }

    cJSON_Delete(json);
}

// ── Playlist Follow functions ─────────────────────────────────────────

sb_push_result_t sb_follow_playlist(const sb_sync_config_t *cfg, int playlist_id) {
    sb_push_result_t result = {0};

    if (strlen(cfg->token) == 0) {
        result.success = false;
        snprintf(result.error_msg, sizeof(result.error_msg), "No token configured");
        return result;
    }

    cJSON *body = cJSON_CreateObject();
    cJSON_AddNumberToObject(body, "playlist_id", playlist_id);
    char *json_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);

    if (!json_str) {
        result.success = false;
        snprintf(result.error_msg, sizeof(result.error_msg), "JSON serialization failed");
        return result;
    }

    http_result_t resp = http_request(cfg, "POST", "/api/shellbeats/follow-playlist.php", json_str);
    free(json_str);

    if (resp.error != SB_OK) {
        result.success = false;
        snprintf(result.error_msg, sizeof(result.error_msg), "%s", sb_error_str(resp.error));
        free(resp.body);
        return result;
    }

    cJSON *json = cJSON_Parse(resp.body);
    free(resp.body);

    if (!json) {
        result.success = false;
        snprintf(result.error_msg, sizeof(result.error_msg), "Invalid server response");
        return result;
    }

    cJSON *success = cJSON_GetObjectItem(json, "success");
    cJSON *msg = cJSON_GetObjectItem(json, "message");
    result.success = cJSON_IsTrue(success);
    if (cJSON_IsString(msg))
        snprintf(result.error_msg, sizeof(result.error_msg), "%s", msg->valuestring);

    cJSON_Delete(json);
    return result;
}

sb_push_result_t sb_unfollow_playlist(const sb_sync_config_t *cfg, int playlist_id) {
    sb_push_result_t result = {0};

    if (strlen(cfg->token) == 0) {
        result.success = false;
        snprintf(result.error_msg, sizeof(result.error_msg), "No token configured");
        return result;
    }

    cJSON *body = cJSON_CreateObject();
    cJSON_AddNumberToObject(body, "playlist_id", playlist_id);
    char *json_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);

    if (!json_str) {
        result.success = false;
        snprintf(result.error_msg, sizeof(result.error_msg), "JSON serialization failed");
        return result;
    }

    http_result_t resp = http_request(cfg, "POST", "/api/shellbeats/unfollow-playlist.php", json_str);
    free(json_str);

    if (resp.error != SB_OK) {
        result.success = false;
        snprintf(result.error_msg, sizeof(result.error_msg), "%s", sb_error_str(resp.error));
        free(resp.body);
        return result;
    }

    cJSON *json = cJSON_Parse(resp.body);
    free(resp.body);

    if (!json) {
        result.success = false;
        snprintf(result.error_msg, sizeof(result.error_msg), "Invalid server response");
        return result;
    }

    cJSON *success = cJSON_GetObjectItem(json, "success");
    cJSON *msg = cJSON_GetObjectItem(json, "message");
    result.success = cJSON_IsTrue(success);
    if (cJSON_IsString(msg))
        snprintf(result.error_msg, sizeof(result.error_msg), "%s", msg->valuestring);

    cJSON_Delete(json);
    return result;
}

sb_follow_check_result_t sb_check_follows(const sb_sync_config_t *cfg) {
    sb_follow_check_result_t result = {0};

    if (strlen(cfg->token) == 0) {
        result.success = false;
        snprintf(result.error_msg, sizeof(result.error_msg), "No token configured");
        return result;
    }

    http_result_t resp = http_request(cfg, "GET", "/api/shellbeats/check-follows.php", NULL);

    if (resp.error != SB_OK) {
        result.success = false;
        snprintf(result.error_msg, sizeof(result.error_msg), "%s", sb_error_str(resp.error));
        free(resp.body);
        return result;
    }

    cJSON *json = cJSON_Parse(resp.body);
    free(resp.body);

    if (!json) {
        result.success = false;
        snprintf(result.error_msg, sizeof(result.error_msg), "Invalid server response");
        return result;
    }

    cJSON *success = cJSON_GetObjectItem(json, "success");
    result.success = cJSON_IsTrue(success);

    if (!result.success) {
        cJSON *msg = cJSON_GetObjectItem(json, "message");
        if (cJSON_IsString(msg))
            snprintf(result.error_msg, sizeof(result.error_msg), "%s", msg->valuestring);
        cJSON_Delete(json);
        return result;
    }

    cJSON *followed = cJSON_GetObjectItem(json, "followed_playlists");
    if (!cJSON_IsArray(followed)) {
        result.count = 0;
        cJSON_Delete(json);
        return result;
    }

    int count = cJSON_GetArraySize(followed);
    if (count <= 0) {
        result.count = 0;
        cJSON_Delete(json);
        return result;
    }

    result.playlists = calloc(count, sizeof(sb_followed_playlist_t));
    if (!result.playlists) {
        result.success = false;
        snprintf(result.error_msg, sizeof(result.error_msg), "Memory allocation failed");
        cJSON_Delete(json);
        return result;
    }

    result.count = count;
    for (int i = 0; i < count; i++) {
        cJSON *item = cJSON_GetArrayItem(followed, i);
        sb_followed_playlist_t *fp = &result.playlists[i];

        cJSON *pid = cJSON_GetObjectItem(item, "playlist_id");
        cJSON *name = cJSON_GetObjectItem(item, "name");
        cJSON *type = cJSON_GetObjectItem(item, "type");
        cJSON *owner = cJSON_GetObjectItem(item, "owner");
        cJSON *songs = cJSON_GetObjectItem(item, "songs");

        if (cJSON_IsNumber(pid)) fp->playlist_id = pid->valueint;
        if (cJSON_IsString(name))
            snprintf(fp->name, sizeof(fp->name), "%s", name->valuestring);
        if (cJSON_IsString(type))
            snprintf(fp->type, sizeof(fp->type), "%s", type->valuestring);
        if (cJSON_IsString(owner))
            snprintf(fp->owner, sizeof(fp->owner), "%s", owner->valuestring);

        if (cJSON_IsArray(songs)) {
            int sc = cJSON_GetArraySize(songs);
            fp->songs = calloc(sc, sizeof(sb_song_t));
            if (fp->songs) {
                fp->song_count = sc;
                for (int j = 0; j < sc; j++) {
                    cJSON *s = cJSON_GetArrayItem(songs, j);
                    cJSON *title = cJSON_GetObjectItem(s, "title");
                    cJSON *vid = cJSON_GetObjectItem(s, "video_id");
                    cJSON *dur = cJSON_GetObjectItem(s, "duration");

                    if (cJSON_IsString(title))
                        snprintf(fp->songs[j].title, sizeof(fp->songs[j].title), "%s", title->valuestring);
                    if (cJSON_IsString(vid))
                        snprintf(fp->songs[j].video_id, sizeof(fp->songs[j].video_id), "%s", vid->valuestring);
                    if (cJSON_IsNumber(dur))
                        fp->songs[j].duration = dur->valueint;
                }
            }
        }
    }

    cJSON_Delete(json);
    return result;
}

void sb_free_follow_check_result(sb_follow_check_result_t *result) {
    if (!result) return;
    if (result->playlists) {
        for (int i = 0; i < result->count; i++) {
            free(result->playlists[i].songs);
        }
        free(result->playlists);
        result->playlists = NULL;
    }
    result->count = 0;
}
