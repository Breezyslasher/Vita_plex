# Rebuilding Vita Packages for VitaPlex

This guide explains how to rebuild mpv, ffmpeg, and curl with modifications to fix HTTP streaming issues.

## Prerequisites

### 1. Install VitaSDK

```bash
# Install VitaSDK (if not already installed)
git clone https://github.com/vitasdk/vdpm
cd vdpm
./bootstrap-vitasdk.sh
./install-all.sh

# Add to your shell profile
export VITASDK=/usr/local/vitasdk
export PATH=$VITASDK/bin:$PATH
```

### 2. Install vita-makepkg

```bash
# vita-makepkg should be included with vdpm
# If not, install it:
pip3 install vita-makepkg
```

### 3. Install build dependencies

```bash
# Install required packages first
vdpm mbedtls zlib libass harfbuzz fribidi freetype libpng libwebp
```

## Building the Packages

### Order of Build (dependencies matter!)

1. **mbedtls** (if not installed)
2. **curl**
3. **ffmpeg**
4. **mpv**

### Build Commands

```bash
cd /path/to/Vita_plex/scripts/vita-packages

# Build curl
cd curl
vita-makepkg
vdpm -i curl-*.tar.xz
cd ..

# Build ffmpeg
cd ffmpeg
vita-makepkg
vdpm -i ffmpeg-*.tar.xz
cd ..

# Build mpv
cd mpv
vita-makepkg
vdpm -i mpv-*.tar.xz
cd ..
```

## What We Changed

### FFmpeg Changes
- **Removed `HAVE_GETADDRINFO=0` hack** - This was disabling DNS resolution
- **Explicitly enabled HTTP/HTTPS/HLS protocols** - Ensure network protocols are available
- **Added TCP protocol** - Required for HTTP connections

### Curl Changes
- **Enabled threaded resolver** - Better async DNS handling
- **Kept HTTP_ONLY** - We only need HTTP/HTTPS

### MPV Changes
- **No changes needed** - MPV uses FFmpeg for network, our changes there should help

## Testing

After rebuilding and installing the packages:

1. Rebuild VitaPlex:
   ```bash
   cd /path/to/Vita_plex
   mkdir -p build && cd build
   cmake ..
   make
   ```

2. Test direct HTTP streaming (without download workaround)

## Reverting

If the new packages cause issues, reinstall the original ones:
```bash
vdpm -i /path/to/original/ffmpeg-*.tar.xz
vdpm -i /path/to/original/mpv-*.tar.xz
vdpm -i /path/to/original/curl-*.tar.xz
```

## Troubleshooting

### Build fails with missing dependencies
```bash
vdpm mbedtls zlib libass harfbuzz fribidi freetype libpng
```

### Network still crashes
The issue might be deeper in Vita's network stack. In that case, keep using the download workaround for audio and try HLS for video.
