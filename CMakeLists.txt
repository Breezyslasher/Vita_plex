cmake_minimum_required(VERSION 3.16)

# Project configuration
set(APP_NAME "VitaPlex")
set(APP_TITLE "Plex for Vita")
set(APP_TITLEID "VPLEX0001")
set(APP_VERSION "01.60")

# Vita SDK setup
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    if(DEFINED ENV{VITASDK})
        set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
    else()
        message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
    endif()
endif()

project(${APP_NAME} LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include("${VITASDK}/share/vita.cmake" REQUIRED)

# Platform definitions - MPV only build (like switchfin)
add_definitions(-D__PSV__ -DPLATFORM_PSV -DUSE_MPV_PLAYER)

# Compiler flags optimized for Vita's Cortex-A9 (matching switchfin)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O3 -g -Wall -ffast-math -mtune=cortex-a9 -mfpu=neon -mfloat-abi=hard")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -g -Wall -ffast-math -mtune=cortex-a9 -mfpu=neon -mfloat-abi=hard -fno-exceptions -fno-rtti")

# Reduce binary size with function/data sections
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -ffunction-sections -fdata-sections")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -ffunction-sections -fdata-sections")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--gc-sections")

# Source files - MPV player only
set(APP_SOURCES
    src/main.cpp
    src/app.cpp
    src/utils/http_client.cpp
    src/player/mpv_player.cpp
)

# Include directories
include_directories(
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)

# Create executable
add_executable(${APP_NAME} ${APP_SOURCES})

# Link libraries - ORDER MATTERS for static linking!
# Based on switchfin's library order for Vita
target_link_libraries(${APP_NAME}
    # MPV (from switchfin vita-packages)
    mpv
    
    # FFmpeg libraries (from switchfin vita-packages, in dependency order)
    avfilter
    avformat
    avcodec
    swresample
    swscale
    avutil
    
    # Subtitle rendering (libass and dependencies)
    ass
    harfbuzz
    fribidi
    
    # Graphics - vita2d
    vita2d
    
    # GXM graphics stubs
    SceDisplay_stub
    SceGxm_stub
    SceShaccCg_stub
    SceShaccCgExt
    taihen_stub
    
    # System management
    SceSysmodule_stub
    SceAppUtil_stub
    SceAppMgr_stub
    SceCommonDialog_stub
    SceCtrl_stub
    SceTouch_stub
    ScePower_stub
    SceKernelThreadMgr_stub
    
    # Audio/Video hardware
    SceAvPlayer_stub
    SceAudio_stub
    SceAudiodec_stub
    SceVideodec_stub
    SceCodecEngine_stub
    SceKernelDmacMgr_stub
    
    # Networking
    SceNet_stub
    SceNetCtl_stub
    SceHttp_stub
    SceSsl_stub
    
    # Input
    SceIme_stub
    
    # TLS/Crypto libraries (from switchfin vita-packages)
    curl
    
    mbedtls
    mbedx509
    mbedcrypto
    
    # Compression
    zstd
    lzma
    
    
    
    # Image libraries
    png16
    jpeg
    webp
    z

    # Font
    freetype
    ScePgf_stub
    bz2

    # Standard libs (must be last)
    m
    c
    pthread
)

# Create .self with UNSAFE flag (required for network and extended memory)
vita_create_self(${APP_NAME}.self ${APP_NAME} UNSAFE)

# Create VPK package
vita_create_vpk(${APP_NAME}.vpk ${APP_TITLEID} ${APP_NAME}.self
    VERSION ${APP_VERSION}
    NAME ${APP_TITLE}
    FILE sce_sys/icon0.png sce_sys/icon0.png
    FILE sce_sys/pic0.png sce_sys/pic0.png
    FILE sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
    FILE sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
    FILE sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
)

