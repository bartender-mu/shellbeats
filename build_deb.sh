#!/usr/bin/env bash

# Exit immediately if a command exits with a non-zero status
set -e

echo "=== 1. Installing Build Dependencies ==="
sudo apt update
sudo apt install -y build-essential debhelper-compat devscripts git \
                    libncurses-dev libcurl4-openssl-dev libcjson-dev

# Define setup variables
REPO_URL="https://github.com/lalo-space/shellbeats.git"
BUILD_DIR="shellbeats-build"

# Clean up any leftover previous builds
if [ -d "$BUILD_DIR" ]; then
    echo "Cleaning up older build directory..."
    rm -rf "$BUILD_DIR"
fi

mkdir "$BUILD_DIR"
cd "$BUILD_DIR"

echo "=== 2. Cloning Repository ==="
git clone "$REPO_URL" shellbeats
cd shellbeats

# Dynamically determine version (Fallback to 0.7.1 if git tags are empty)
VERSION=$(git describe --tags --abbrev=0 2>/dev/null | sed 's/^v//')
if [ -z "$VERSION" ]; then
    VERSION="0.7.1"
fi
echo "Targeting version: $VERSION"

echo "=== 3. Patching Upstream Makefile ==="
# Crucial Fix: Force the Makefile to honor BOTH staging directory (DESTDIR) and PREFIX
sed -i 's|/usr/local/bin|$(DESTDIR)$(PREFIX)/bin|g' Makefile

echo "=== 4. Creating Tarball Archive ==="
cd ..
tar -czf "shellbeats_${VERSION}.orig.tar.gz" shellbeats
cd shellbeats

echo "=== 5. Structuring Debian Metadata ==="
mkdir -p debian

# Create debian/changelog
cat << EOF > debian/changelog
shellbeats (${VERSION}-1) trixie; urgency=low

  * Automated package build for Debian 13 Trixie.
  * Patched Makefile to fully respect staging DESTDIR sandbox parameters.

 -- Automated Builder <builder@local.internal>  $(date -R)
EOF

# Create debian/control
cat << EOF > debian/control
Source: shellbeats
Section: sound
Priority: optional
Maintainer: Automated Builder <builder@local.internal>
Build-Depends: debhelper-compat (= 13), build-essential, libncurses-dev, libcurl4-openssl-dev, libcjson-dev
Standards-Version: 4.6.2
Homepage: https://github.com/lalo-space/shellbeats

Package: shellbeats
Architecture: any
Depends: \${shlibs:Depends}, \${misc:Depends}, mpv, yt-dlp, ffmpeg, curl
Description: CLI music player for Linux to stream YouTube audio
 Minimal, fast, keyboard-driven terminal music player. 
 Under the hood it uses yt-dlp for searching and mpv for playback.
EOF

# Create debian/rules (Ensuring exact TAB indentation)
cat << 'EOF' > debian/rules
#!/usr/bin/make -f
%:
	dh $@

override_dh_auto_install:
	# Ensure the internal directory structure is ready before make install runs
	mkdir -p $(CURDIR)/debian/shellbeats/usr/bin
	$(MAKE) DESTDIR=$(CURDIR)/debian/shellbeats PREFIX=/usr install
EOF

# Make rules file executable
chmod +x debian/rules

echo "=== 6. Compiling and Building DEB Package ==="
# -us -uc disables cryptographic signing for seamless local compilation
debuild -us -uc

echo "================================================="
echo " SUCCESS! Packaging Complete."
echo "================================================="
echo "Your debian package files are located at:"
ls -l ../shellbeats_${VERSION}-1_*.deb
echo "================================================="
