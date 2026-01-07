#!/bin/bash
# Build and install modified Vita packages for VitaPlex
# This script builds curl, ffmpeg, and mpv with HTTP streaming fixes

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"
SWITCHFIN_RAW="https://raw.githubusercontent.com/dragonflylee/switchfin/dev/scripts/vita"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo -e "${GREEN}=== VitaPlex Vita Packages Builder ===${NC}"
echo ""

# Check for VitaSDK
if [ -z "$VITASDK" ]; then
    echo -e "${RED}Error: VITASDK environment variable not set${NC}"
    echo "Please install VitaSDK and set VITASDK=/path/to/vitasdk"
    exit 1
fi

echo -e "Using VitaSDK: ${GREEN}$VITASDK${NC}"
echo ""

# Create build directory
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# ============================================
# Download patches
# ============================================
echo -e "${YELLOW}[1/6] Downloading patches...${NC}"

curl -L -s -o ffmpeg.patch "$SWITCHFIN_RAW/ffmpeg/ffmpeg.patch"
echo "  Downloaded ffmpeg.patch"

curl -L -s -o gxm.patch "$SWITCHFIN_RAW/mpv/gxm.patch"
echo "  Downloaded gxm.patch"

# ============================================
# Build curl
# ============================================
echo ""
echo -e "${YELLOW}[2/6] Building curl...${NC}"

CURL_VER="8.11.0"
if [ ! -f "curl-${CURL_VER}.tar.xz" ]; then
    curl -L -o "curl-${CURL_VER}.tar.xz" "https://curl.se/download/curl-${CURL_VER}.tar.xz"
fi

rm -rf "curl-${CURL_VER}"
tar xf "curl-${CURL_VER}.tar.xz"
cd "curl-${CURL_VER}"

cmake -B build -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake \
    -DCMAKE_INSTALL_PREFIX=$VITASDK/arm-vita-eabi \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_CURL_EXE=OFF \
    -DBUILD_SHARED_LIBS=OFF \
    -DBUILD_TESTING=OFF \
    -DBUILD_EXAMPLES=OFF \
    -DBUILD_LIBCURL_DOCS=OFF \
    -DBUILD_MISC_DOCS=OFF \
    -DENABLE_CURL_MANUAL=OFF \
    -DHTTP_ONLY=ON \
    -DCURL_USE_MBEDTLS=ON \
    -DCURL_DISABLE_PROGRESS_METER=ON \
    -DENABLE_IPV6=OFF \
    -DENABLE_THREADED_RESOLVER=ON \
    -DUSE_NGHTTP2=OFF \
    -DUSE_LIBIDN2=OFF \
    -DCURL_BROTLI=OFF \
    -DCURL_ZSTD=OFF \
    -DCURL_USE_LIBSSH2=OFF \
    -DCURL_USE_LIBPSL=OFF

# Remove HAVE_PIPE2 if present
sed -i 's/#define HAVE_PIPE2 1//g' build/lib/curl_config.h 2>/dev/null || true

cmake --build build -j$(nproc)
cmake --install build

echo -e "${GREEN}  curl installed to $VITASDK${NC}"
cd "$BUILD_DIR"

# ============================================
# Build FFmpeg
# ============================================
echo ""
echo -e "${YELLOW}[3/6] Building FFmpeg...${NC}"

FFMPEG_VER="n6.0"
if [ ! -f "FFmpeg-${FFMPEG_VER}.tar.gz" ]; then
    curl -L -o "FFmpeg-${FFMPEG_VER}.tar.gz" "https://github.com/FFmpeg/FFmpeg/archive/${FFMPEG_VER}.tar.gz"
fi

rm -rf "FFmpeg-${FFMPEG_VER}"
tar xf "FFmpeg-${FFMPEG_VER}.tar.gz"
cd "FFmpeg-${FFMPEG_VER}"

# Apply patch
patch --strip=1 --input="$BUILD_DIR/ffmpeg.patch"

./configure \
    --prefix=$VITASDK/arm-vita-eabi \
    --enable-vita \
    --target-os=vita \
    --enable-cross-compile \
    --cross-prefix=$VITASDK/bin/arm-vita-eabi- \
    --disable-runtime-cpudetect \
    --disable-armv5te \
    --disable-shared \
    --enable-static \
    --disable-programs \
    --disable-doc \
    --disable-autodetect \
    --disable-iconv \
    --disable-lzma \
    --disable-sdl2 \
    --disable-xlib \
    --disable-avdevice \
    --enable-swscale \
    --enable-swresample \
    --enable-network \
    --enable-libass \
    --enable-mbedtls \
    --enable-version3 \
    --enable-pthreads \
    \
    --enable-protocol=file \
    --enable-protocol=http \
    --enable-protocol=https \
    --enable-protocol=tcp \
    --enable-protocol=hls \
    --enable-protocol=crypto \
    \
    --enable-demuxer=hls \
    --enable-demuxer=flac \
    --enable-demuxer=flv \
    --enable-demuxer=aac \
    --enable-demuxer=ac3 \
    --enable-demuxer=h264 \
    --enable-demuxer=hevc \
    --enable-demuxer=mp3 \
    --enable-demuxer=wav \
    --enable-demuxer=ogg \
    --enable-demuxer=mov \
    --enable-demuxer=mpegts \
    --enable-demuxer=mpegps \
    --enable-demuxer=mjpeg \
    --enable-demuxer=matroska \
    --enable-demuxer=ass \
    --enable-demuxer=srt \
    \
    --enable-decoder=h264 \
    --enable-decoder=h264_vita \
    --enable-decoder=aac \
    --enable-decoder=aac_latm \
    --enable-decoder=ac3 \
    --enable-decoder=eac3 \
    --enable-decoder=bmp \
    --enable-decoder=flv \
    --enable-decoder=flac \
    --enable-decoder=mjpeg \
    --enable-decoder=mp3 \
    --enable-decoder=vorbis \
    --enable-decoder=opus \
    --enable-decoder=pcm_s16le \
    --enable-decoder=pcm_s24le \
    --enable-decoder=srt \
    --enable-decoder=subrip \
    --enable-decoder=ssa \
    --enable-decoder=ass \
    \
    --enable-parser=h264 \
    --enable-parser=hevc \
    --enable-parser=aac \
    --enable-parser=aac_latm \
    --enable-parser=ac3 \
    --enable-parser=flac \
    --enable-parser=mpegaudio \
    --enable-parser=vorbis \
    --enable-parser=opus \
    \
    --disable-encoders \
    --disable-muxers \
    --disable-bsfs \
    --disable-filters \
    --enable-bsf=h264_mp4toannexb \
    --enable-bsf=hevc_mp4toannexb \
    --enable-filter=scale \
    --enable-filter=aresample

# Vita SDK doesn't have gai_strerror, so we MUST disable getaddrinfo
# This means FFmpeg can't resolve hostnames - must use IP addresses or pre-resolve with curl
sed -i 's/#define HAVE_GETADDRINFO 1/#define HAVE_GETADDRINFO 0/g' config.h

make -j$(nproc)
make install

echo -e "${GREEN}  FFmpeg installed to $VITASDK${NC}"
cd "$BUILD_DIR"

# ============================================
# Build MPV
# ============================================
echo ""
echo -e "${YELLOW}[4/6] Building MPV...${NC}"

MPV_VER="0.36.0"
if [ ! -f "mpv-${MPV_VER}.tar.gz" ]; then
    curl -L -o "mpv-${MPV_VER}.tar.gz" "https://github.com/mpv-player/mpv/archive/v${MPV_VER}.tar.gz"
fi

rm -rf "mpv-${MPV_VER}"
tar xf "mpv-${MPV_VER}.tar.gz"
cd "mpv-${MPV_VER}"

# Apply GXM patch
patch --strip=1 --input="$BUILD_DIR/gxm.patch"

# Create cross file for meson
cat > crossfile.txt << 'CROSSFILE'
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
c_link_args = ['-Wl,-q', '-Wl,-z,nocopyreloc']
cpp_link_args = ['-Wl,-q', '-Wl,-z,nocopyreloc']
CROSSFILE

meson setup build --prefix=$VITASDK/arm-vita-eabi --cross-file crossfile.txt \
    --default-library static \
    -Diconv=disabled \
    -Dlua=disabled \
    -Djpeg=disabled \
    -Dopensles=disabled \
    -Dlibavdevice=disabled \
    -Dmanpage-build=disabled \
    -Dhtml-build=disabled \
    -Dsdl2=disabled \
    -Dlibmpv=true \
    -Dgxm=enabled \
    -Dvitashark=disabled \
    -Dcplayer=false

meson compile -C build
meson install -C build

echo -e "${GREEN}  MPV installed to $VITASDK${NC}"
cd "$BUILD_DIR"

# ============================================
# Cleanup (optional)
# ============================================
echo ""
echo -e "${YELLOW}[5/6] Cleaning up...${NC}"
# Uncomment to remove build files:
# rm -rf "$BUILD_DIR"
echo "  Build files kept in $BUILD_DIR"
echo "  Run 'rm -rf $BUILD_DIR' to remove them"

# ============================================
# Done
# ============================================
echo ""
echo -e "${GREEN}[6/6] Build complete!${NC}"
echo ""
echo "Packages installed to: $VITASDK/arm-vita-eabi"
echo ""
echo "Changes made:"
echo "  - curl: Enabled threaded resolver"
echo "  - FFmpeg: Explicit HTTP/HLS protocols enabled"
echo "  - MPV: No changes (uses FFmpeg for network)"
echo ""
echo "NOTE: Vita doesn't support DNS in FFmpeg (no gai_strerror)."
echo "      HTTP streaming must use IP addresses OR use curl to download first."
echo ""
echo "Next: Rebuild VitaPlex with:"
echo "  cd /path/to/Vita_plex"
echo "  rm -rf build && mkdir build && cd build"
echo "  cmake .. && make"
echo ""
