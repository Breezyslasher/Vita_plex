# VitaPlex - Vita Packages with HTTP Streaming Support

This directory contains build scripts for FFmpeg and MPV with **working HTTP streaming** on PS Vita.

Based on [wiliwili's PSV build configuration](https://github.com/xfangfang/wiliwili/tree/yoga/scripts/psv).

## Key Difference from Standard Builds

The FFmpeg build explicitly enables HTTP protocols:

```bash
--enable-protocol='file,http,tcp,tls,hls,https,rtp,crypto,httpproxy'
```

This is **required** for HTTP streaming to work on PS Vita.

## Prerequisites

1. **VitaSDK** installed and configured
   ```bash
   export VITASDK=/usr/local/vitasdk
   export PATH=$VITASDK/bin:$PATH
   ```

2. **vita-makepkg** available (included with VitaSDK)

3. **Build dependencies** (should be in VitaSDK):
   - zlib
   - libass
   - freetype
   - harfbuzz
   - fribidi

## Build Order

Packages must be built in this order due to dependencies:

1. **mbedtls** - TLS/SSL library
2. **curl** - HTTP client (depends on mbedtls)
3. **ffmpeg** - Multimedia framework with HTTP protocols (depends on mbedtls)
4. **mpv** - Media player with GXM rendering (depends on ffmpeg)

## Quick Start

### Build All Packages

```bash
cd vita-packages
./build-all.sh
```

### Build Individual Package

```bash
./build-all.sh mbedtls
./build-all.sh curl
./build-all.sh ffmpeg
./build-all.sh mpv
```

### Clean Build Directories

```bash
./build-all.sh clean
```

## Manual Build (Alternative)

If you prefer to build manually:

```bash
# 1. Build mbedtls
cd mbedtls
vita-makepkg -i
cd ..

# 2. Build curl
cd curl
vita-makepkg -i
cd ..

# 3. Build ffmpeg (with HTTP support!)
cd ffmpeg
vita-makepkg -i
cd ..

# 4. Build mpv (with GXM rendering)
cd mpv
vita-makepkg -i
cd ..
```

## Package Details

### mbedtls (v3.4.1)
- TLS/SSL library for secure connections
- Required for HTTPS support

### curl (v8.9.1)
- HTTP client library
- Uses Vita's CA certificate store: `vs0:data/external/cert/CA_LIST.cer`

### ffmpeg (n6.0)
- **HTTP protocols enabled**: `http`, `https`, `tcp`, `tls`, `hls`, `httpproxy`
- Hardware decoders: `h264_vita`, `aac_vita`, `mp3_vita`
- Demuxers: `hls`, `flac`, `flv`, `aac`, `h264`, `hevc`, `mp3`, `wav`, `mov`, `matroska`

### mpv (v0.36.0)
- GXM rendering for PS Vita
- libmpv API enabled
- Hardware acceleration support

## After Building

Update your project's CMakeLists.txt to link against the new libraries:

```cmake
target_link_libraries(${PROJECT_NAME}
    # ... other libs ...
    mpv
    avformat
    avcodec
    avutil
    swresample
    swscale
    curl
    mbedtls
    mbedcrypto
    mbedx509
    # ... vita stubs ...
)
```

## Troubleshooting

### "vita-makepkg not found"
Make sure VitaSDK bin is in your PATH:
```bash
export PATH=$VITASDK/bin:$PATH
```

### Conflicting packages
If you have existing ffmpeg/mpv installed from another source, remove them first:
```bash
rm -rf $VITASDK/arm-vita-eabi/lib/libav*
rm -rf $VITASDK/arm-vita-eabi/lib/libmpv*
rm -rf $VITASDK/arm-vita-eabi/include/libav*
```

### Build fails with missing dependencies
Install missing dependencies via vdpm:
```bash
vdpm zlib
vdpm freetype
vdpm libass
```

## Sources

- [wiliwili PSV scripts](https://github.com/xfangfang/wiliwili/tree/yoga/scripts/psv)
- [VitaSDK](https://vitasdk.org/)
- [vita-makepkg](https://github.com/psvita/vita-makepkg)
