#ifndef SURIKATA_SYNC_H
#define SURIKATA_SYNC_H

#include <stdbool.h>
#include <stddef.h>

/**
 * Surikata Sync Module for ShellBeats
 *
 * Syncs playlists and config to surikata.app via HTTPS API.
 * Authentication via Bearer token (sb_xxx).
 *
 * Dependencies: libcurl, cJSON
 */

// Max lengths
#define SB_TOKEN_LEN 44         // "sb_" + 40 hex chars + null
#define SB_URL_MAX 256
#define SB_PLAYLIST_NAME_MAX 100
#define SB_SONG_TITLE_MAX 255
#define SB_VIDEO_ID_MAX 20

// Error codes
typedef enum {
    SB_OK = 0,
    SB_ERR_NO_TOKEN,        // Token not configured
    SB_ERR_INVALID_TOKEN,   // Server returned 401
    SB_ERR_NETWORK,         // Connection failed
    SB_ERR_SERVER,          // Server returned 5xx
    SB_ERR_PARSE,           // JSON parse error
    SB_ERR_RATE_LIMITED,    // 429 Too Many Requests
    SB_ERR_FILE_IO          // Local file I/O error
} sb_error_t;

// Song structure (mirrors ShellBeats Song)
typedef struct {
    char title[SB_SONG_TITLE_MAX + 1];
    char video_id[SB_VIDEO_ID_MAX + 1];
    int duration;
} sb_song_t;

// Playlist structure for sync
typedef struct {
    char name[SB_PLAYLIST_NAME_MAX + 1];
    char type[16];          // "local" or "youtube"
    bool is_shared;
    sb_song_t *songs;
    int song_count;
} sb_playlist_t;

// Sync configuration (loaded from ~/.shellbeats/surikata.json)
typedef struct {
    char url[SB_URL_MAX];   // e.g. "https://surikata.app"
    char token[SB_TOKEN_LEN];
    bool enabled;           // Auto-sync on playlist change
    bool pull_on_startup;   // Pull playlists from server on app start
    bool sync_on_quit;      // Push all playlists on app exit
} sb_sync_config_t;

// Result from verify
typedef struct {
    bool success;
    char username[64];
    int user_id;
    int playlists_synced;
    char latest_version[32];    // Latest version from server (may be empty)
    char error_msg[256];
} sb_verify_result_t;

// Result from push
typedef struct {
    bool success;
    int synced_count;
    char error_msg[256];
} sb_push_result_t;

// Result from pull (caller must free playlists and their songs)
typedef struct {
    bool success;
    sb_playlist_t *playlists;
    int playlist_count;
    char *config_json;      // Raw JSON string, caller must free
    char error_msg[256];
} sb_pull_result_t;

// ── Core API Functions ──────────────────────────────────────────────

/**
 * Initialize the sync module (call once at startup).
 * Calls curl_global_init().
 */
void sb_sync_init(void);

/**
 * Cleanup the sync module (call at shutdown).
 * Calls curl_global_cleanup().
 */
void sb_sync_cleanup(void);

/**
 * Load sync config from ~/.shellbeats/surikata.json.
 * Separate file to avoid conflicts with ShellBeats config.json.
 */
sb_sync_config_t sb_load_config(const char *config_dir);

/**
 * Save sync config to ~/.shellbeats/surikata.json.
 */
sb_error_t sb_save_config(const char *config_dir, const sb_sync_config_t *cfg);

/**
 * Verify token with server. Returns username on success.
 */
sb_verify_result_t sb_verify(const sb_sync_config_t *cfg);

/**
 * Push a single playlist to server (incremental sync).
 */
sb_push_result_t sb_push_playlist(const sb_sync_config_t *cfg, const sb_playlist_t *playlist);

/**
 * Push all playlists + config to server (full sync).
 * config_json can be NULL to skip config sync.
 */
sb_push_result_t sb_push_all(const sb_sync_config_t *cfg,
                              const sb_playlist_t *playlists, int count,
                              const char *config_json);

/**
 * Pull all playlists + config from server.
 * Caller must free result with sb_free_pull_result().
 */
sb_pull_result_t sb_pull_all(const sb_sync_config_t *cfg);

/**
 * Delete a playlist from server.
 */
sb_error_t sb_delete_playlist(const sb_sync_config_t *cfg, const char *name);

/**
 * Free memory from pull result.
 */
void sb_free_pull_result(sb_pull_result_t *result);

/**
 * Free a playlist's songs array.
 */
void sb_free_playlist(sb_playlist_t *pl);

/**
 * Human-readable error message.
 */
const char *sb_error_str(sb_error_t err);

/**
 * Check latest version from server (public, no auth required).
 * Returns version string in out_version buffer, or empty string on failure.
 */
void sb_check_latest_version(const char *base_url, char *out_version, size_t out_size);

// ── Playlist Follow (additive-only sync) ─────────────────────────────

// Followed playlist info returned by check-follows
typedef struct {
    int playlist_id;
    char name[SB_PLAYLIST_NAME_MAX + 1];
    char type[16];
    char owner[64];
    sb_song_t *songs;
    int song_count;
} sb_followed_playlist_t;

// Result from check-follows
typedef struct {
    bool success;
    sb_followed_playlist_t *playlists;
    int count;
    char error_msg[256];
} sb_follow_check_result_t;

/**
 * Follow a public playlist by ID.
 */
sb_push_result_t sb_follow_playlist(const sb_sync_config_t *cfg, int playlist_id);

/**
 * Unfollow a playlist by ID.
 */
sb_push_result_t sb_unfollow_playlist(const sb_sync_config_t *cfg, int playlist_id);

/**
 * Check all followed playlists for new songs.
 * Caller must free result with sb_free_follow_check_result().
 */
sb_follow_check_result_t sb_check_follows(const sb_sync_config_t *cfg);

/**
 * Free memory from follow check result.
 */
void sb_free_follow_check_result(sb_follow_check_result_t *result);

#endif
