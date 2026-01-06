cmake_minimum_required(VERSION 3.16)

# Set platform for Borealis BEFORE project() - this is critical
# These must be cache variables set before any project() call
set(PLATFORM_DESKTOP OFF CACHE BOOL "Build for Desktop" FORCE)
set(PLATFORM_SWITCH OFF CACHE BOOL "Build for Switch" FORCE)
set(PLATFORM_PSV ON CACHE BOOL "Build for PS Vita" FORCE)
set(PLATFORM_PS4 OFF CACHE BOOL "Build for PS4" FORCE)
set(PLATFORM_IOS OFF CACHE BOOL "Build for iOS" FORCE)
set(PLATFORM_ANDROID OFF CACHE BOOL "Build for Android" FORCE)

# Disable features not needed for Vita
set(USE_LIBROMFS OFF CACHE BOOL "Use libromfs" FORCE)
set(USE_OPENGL OFF CACHE BOOL "Use OpenGL" FORCE)
set(BOREALIS_USE_STD_THREAD OFF CACHE BOOL "Use std::thread" FORCE)

# Enable native GXM graphics for PS Vita (required!)
# We set both USE_GXM and BOREALIS_USE_GXM because:
# - USE_GXM is the external flag expected by Borealis toolchain.cmake
# - BOREALIS_USE_GXM is what the library CMakeLists.txt actually checks
# When adding library directly (without parent CMakeLists.txt), both are needed
set(USE_GXM ON CACHE BOOL "Use GXM for PS Vita" FORCE)
set(BOREALIS_USE_GXM ON CACHE BOOL "Use GXM driver" FORCE)
set(USE_VITA_SHARK ON CACHE BOOL "Use VitaShark for shader compilation" FORCE)

# VitaSDK Configuration
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    if(DEFINED ENV{VITASDK})
        set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
    else()
        message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
    endif()
endif()

# Project definition
project(VitaPlex VERSION 2.0.0 LANGUAGES C CXX)

# C++17 required for Borealis
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Enable RTTI and exceptions (required by Borealis)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -frtti -fexceptions")

include("${VITASDK}/share/vita.cmake" REQUIRED)

# Define VITA macro for platform detection
add_definitions(-D__vita__ -DPLATFORM_PSV=1 -D__PSV__)

# App metadata
set(VITA_APP_NAME "VitaPlex")
set(VITA_TITLEID "VPLEX0001")
set(VITA_VERSION "02.00")
set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d PARENTAL_LEVEL=1")

# Borealis library
set(BOREALIS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/lib/borealis)
option(BOREALIS_LIBRARY "Build borealis as library" ON)

# Add Borealis subdirectory
add_subdirectory(${BOREALIS_DIR}/library)

# Application source files
set(APP_SOURCES
    # Main entry
    src/main.cpp

    # Application
    src/app/application.cpp
    src/app/plex_client.cpp

    # Activities
    src/activity/main_activity.cpp
    src/activity/login_activity.cpp
    src/activity/player_activity.cpp

    # Views
    src/view/home_tab.cpp
    src/view/library_tab.cpp
    src/view/search_tab.cpp
    src/view/settings_tab.cpp
    src/view/media_detail_view.cpp
    src/view/media_item_cell.cpp
    src/view/recycling_grid.cpp

    # Player
    src/player/mpv_player.cpp

    # Utils
    src/utils/http_client.cpp
    src/utils/image_loader.cpp
)

# Include directories
set(APP_INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${BOREALIS_DIR}/library/include
    ${BOREALIS_DIR}/library/include/borealis/extern
)

# Create executable
add_executable(${PROJECT_NAME} ${APP_SOURCES})

target_include_directories(${PROJECT_NAME} PRIVATE ${APP_INCLUDES})

# Link libraries
target_link_libraries(${PROJECT_NAME}
    borealis

    # Vita SDK libraries
    vita2d
    SceDisplay_stub
    SceCtrl_stub
    SceSysmodule_stub
    SceNet_stub
    SceNetCtl_stub
    SceHttp_stub
    SceSsl_stub
    SceAppUtil_stub
    ScePgf_stub
    SceCommonDialog_stub
    SceGxm_stub
    SceTouch_stub
    SceIme_stub
    SceAudio_stub
    ScePower_stub
    SceRtc_stub
    SceShellSvc_stub

    # External libraries
    curl
    ssl
    crypto
    z
    mpv
    avcodec
    avformat
    avutil
    swresample
    swscale

    # Standard libraries
    m
    c
    pthread
)

# Create Vita SELF
vita_create_self(${PROJECT_NAME}.self ${PROJECT_NAME} UNSAFE)

# Create VPK
vita_create_vpk(${PROJECT_NAME}.vpk ${VITA_TITLEID} ${PROJECT_NAME}.self
    VERSION ${VITA_VERSION}
    NAME ${VITA_APP_NAME}

    # System files
    FILE sce_sys/icon0.png sce_sys/icon0.png
    FILE sce_sys/pic0.png sce_sys/pic0.png
    FILE sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
    FILE sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
    FILE sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml

    # Borealis resources
    FILE ${BOREALIS_DIR}/library/lib/resources/i18n/en-US/brls.json resources/i18n/en-US/brls.json
    FILE resources/i18n/en-US/main.json resources/i18n/en-US/main.json

    # App resources
    FILE resources/xml/activity/main.xml resources/xml/activity/main.xml
    FILE resources/xml/activity/login.xml resources/xml/activity/login.xml
    FILE resources/xml/activity/player.xml resources/xml/activity/player.xml
    FILE resources/xml/tabs/home.xml resources/xml/tabs/home.xml
    FILE resources/xml/tabs/library.xml resources/xml/tabs/library.xml
    FILE resources/xml/tabs/search.xml resources/xml/tabs/search.xml
    FILE resources/xml/tabs/settings.xml resources/xml/tabs/settings.xml
    FILE resources/xml/view/media_detail.xml resources/xml/view/media_detail.xml
    FILE resources/xml/view/media_item.xml resources/xml/view/media_item.xml

    # Fonts
    FILE ${BOREALIS_DIR}/library/lib/resources/fonts/switch_icons.ttf resources/fonts/switch_icons.ttf
    FILE ${BOREALIS_DIR}/library/lib/resources/fonts/material_icons.ttf resources/fonts/material_icons.ttf
)
