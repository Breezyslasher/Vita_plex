#!/bin/bash
# Setup script to download patches from switchfin

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SWITCHFIN_RAW="https://raw.githubusercontent.com/dragonflylee/switchfin/dev/scripts/vita"

echo "=== VitaPlex Vita Packages Setup ==="
echo ""

# Download FFmpeg patch
echo "Downloading FFmpeg patch..."
mkdir -p "$SCRIPT_DIR/ffmpeg"
curl -L -o "$SCRIPT_DIR/ffmpeg/ffmpeg.patch" \
    "$SWITCHFIN_RAW/ffmpeg/ffmpeg.patch"
echo "  Downloaded ffmpeg.patch"

# Download MPV patch
echo "Downloading MPV patch..."
mkdir -p "$SCRIPT_DIR/mpv"
curl -L -o "$SCRIPT_DIR/mpv/gxm.patch" \
    "$SWITCHFIN_RAW/mpv/gxm.patch"
echo "  Downloaded gxm.patch"

# Download MPV crossfile
echo "Downloading MPV crossfile..."
curl -L -o "$SCRIPT_DIR/mpv/crossfile.txt" \
    "$SWITCHFIN_RAW/mpv/crossfile.txt" 2>/dev/null || {
    # Create a basic crossfile if not available
    cat > "$SCRIPT_DIR/mpv/crossfile.txt" << 'EOF'
[binaries]
c = 'arm-vita-eabi-gcc'
cpp = 'arm-vita-eabi-g++'
ar = 'arm-vita-eabi-ar'
strip = 'arm-vita-eabi-strip'
pkgconfig = 'arm-vita-eabi-pkg-config'

[host_machine]
system = 'vita'
cpu_family = 'arm'
cpu = 'cortex-a9'
endian = 'little'

[built-in options]
c_args = ['-Wl,-q', '-Wl,-z,nocopyreloc']
cpp_args = ['-Wl,-q', '-Wl,-z,nocopyreloc']
EOF
    echo "  Created default crossfile.txt"
}

echo ""
echo "=== Setup Complete ==="
echo ""
echo "Next steps:"
echo "  1. Ensure VitaSDK is installed and VITASDK env var is set"
echo "  2. Install dependencies: vdpm mbedtls zlib libass harfbuzz fribidi freetype libpng"
echo "  3. Build packages in order:"
echo "     cd $SCRIPT_DIR/curl && vita-makepkg && vdpm -i curl-*.tar.xz"
echo "     cd $SCRIPT_DIR/ffmpeg && vita-makepkg && vdpm -i ffmpeg-*.tar.xz"
echo "     cd $SCRIPT_DIR/mpv && vita-makepkg && vdpm -i mpv-*.tar.xz"
echo ""
