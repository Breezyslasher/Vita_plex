cmake_minimum_required(VERSION 3.16)

# Set Borealis library path FIRST (required by cmake includes)
set(BOREALIS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/lib/borealis)
set(BOREALIS_LIBRARY ${BOREALIS_DIR}/library)

# Set platform for Borealis BEFORE including cmake files
set(PLATFORM_DESKTOP OFF CACHE BOOL "Build for Desktop" FORCE)
set(PLATFORM_SWITCH OFF CACHE BOOL "Build for Switch" FORCE)
set(PLATFORM_PSV ON CACHE BOOL "Build for PS Vita" FORCE)
set(PLATFORM_PS4 OFF CACHE BOOL "Build for PS4" FORCE)
set(PLATFORM_IOS OFF CACHE BOOL "Build for iOS" FORCE)
set(PLATFORM_ANDROID OFF CACHE BOOL "Build for Android" FORCE)

# Enable native GXM graphics for PS Vita (must be BOREALIS_USE_GXM)
set(BOREALIS_USE_GXM ON CACHE BOOL "Use GXM for PS Vita" FORCE)
# Disable VitaShark for now - requires libshacccg.suprx module which we don't have
# Uses precompiled shaders instead
set(USE_VITA_SHARK OFF CACHE BOOL "Use VitaShark for shader compilation" FORCE)

# Disable features not needed for Vita
set(USE_LIBROMFS OFF CACHE BOOL "Use libromfs" FORCE)

# Include Borealis cmake files (like switchfin does)
# This properly sets up platform options and toolchain
include(${BOREALIS_LIBRARY}/cmake/commonOption.cmake)
include(${BOREALIS_LIBRARY}/cmake/toolchain.cmake)

# Project definition (AFTER toolchain include)
project(VitaPlex VERSION 2.0.0 LANGUAGES C CXX)

# C++17 required for Borealis
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Enable RTTI and exceptions (required by Borealis)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -frtti -fexceptions")

# App metadata
set(VITA_APP_NAME "VitaPlex")
set(VITA_TITLEID "VPLEX0001")
set(VITA_VERSION "02.00")
# ATTRIBUTE2=12 enables maximum heap size mode - CRITICAL for memory-intensive apps
set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d PARENTAL_LEVEL=1 -d ATTRIBUTE2=12")

# Apply patches to borealis before building
# Fix: psv_platform.cpp uses wrong macro (__SDL2__ instead of BOREALIS_USE_GXM)
set(PSV_PLATFORM_PATCH ${CMAKE_CURRENT_SOURCE_DIR}/patches/psv_platform.cpp)
set(PSV_PLATFORM_TARGET ${BOREALIS_LIBRARY}/lib/platforms/psv/psv_platform.cpp)
if(EXISTS ${PSV_PLATFORM_PATCH})
    message(STATUS "Applying patch: psv_platform.cpp")
    configure_file(${PSV_PLATFORM_PATCH} ${PSV_PLATFORM_TARGET} COPYONLY)
else()
    message(FATAL_ERROR "Patch file not found: ${PSV_PLATFORM_PATCH}")
endif()

# Add Borealis library subdirectory
add_subdirectory(${BOREALIS_LIBRARY})

# Application source files
set(APP_SOURCES
    # Main entry
    src/main.cpp

    # Application
    src/app/application.cpp
    src/app/plex_client.cpp
    src/app/downloads_manager.cpp
    src/app/music_queue.cpp

    # Activities
    src/activity/main_activity.cpp
    src/activity/login_activity.cpp
    src/activity/player_activity.cpp

    # Views
    src/view/home_tab.cpp
    src/view/library_tab.cpp
    src/view/library_section_tab.cpp
    src/view/search_tab.cpp
    src/view/settings_tab.cpp
    src/view/debug_tab.cpp
    src/view/livetv_tab.cpp
    src/view/media_detail_view.cpp
    src/view/media_item_cell.cpp
    src/view/recycling_grid.cpp
    src/view/video_view.cpp
    src/view/downloads_tab.cpp
    src/view/music_tab.cpp
    src/view/progress_dialog.cpp
    src/view/overlay_dialog.cpp
    src/view/horizontal_scroll_row.cpp

    # Player
    src/player/mpv_player.cpp

    # Utils
    src/utils/http_client.cpp
    src/utils/image_loader.cpp
    src/utils/jwt_auth.cpp
    src/utils/vita_stubs.c
)

# Include directories
set(APP_INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${BOREALIS_LIBRARY}/include
    ${BOREALIS_LIBRARY}/include/borealis/extern
)

# Create executable
add_executable(${PROJECT_NAME} ${APP_SOURCES})

target_include_directories(${PROJECT_NAME} PRIVATE ${APP_INCLUDES})

# Wrap pthread_create to enforce a minimum 512KB stack size.
# Vita's default pthread stack is only 32KB, which is too small for MPV's
# software H.264 decoder threads (causes stack overflow crash).
target_link_options(${PROJECT_NAME} PRIVATE "-Wl,--wrap=pthread_create")

# Link libraries - borealis + app platform libs from toolchain
# Order matters: libraries that PROVIDE symbols must come AFTER libraries that NEED them
target_link_libraries(${PROJECT_NAME}
    borealis
    ${APP_PLATFORM_LIB}

    # External libraries first (they depend on Vita stubs)
    # FFmpeg has circular dependencies - list avcodec multiple times
    curl
    mpv
    archive
    zstd
    avfilter
    avformat
    avcodec
    swresample
    swscale
    avutil
    avcodec
    ass
    freetype
    harfbuzz
    fribidi
    png
    bz2
    lzma
    mbedtls
    mbedcrypto
    mbedx509

    # Vita SDK stubs (provide symbols for above libraries)
    vita2d
    SceNet_stub
    SceNetCtl_stub
    SceHttp_stub
    SceSsl_stub
    ScePgf_stub
    SceRtc_stub
    SceShellSvc_stub
    SceAudiodec_stub
    SceVideodec_stub
    SceAvcodec_stub
    SceCodecEngine_stub
    SceAudio_stub
    SceVshBridge_stub
    SceAppMgr_stub

    # Standard libraries last (m is provided by libstdc++)
    z
    c
)

# Create Vita SELF (UNSAFE NOASLR flags required for homebrew compatibility)
vita_create_self(${PROJECT_NAME}.self ${PROJECT_NAME} UNSAFE NOASLR)

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
    FILE ${BOREALIS_DIR}/resources/i18n/en-US/hints.json resources/i18n/en-US/hints.json
    FILE resources/i18n/en-US/main.json resources/i18n/en-US/main.json

    # App resources - XML layouts
    FILE resources/xml/activity/main.xml resources/xml/activity/main.xml
    FILE resources/xml/activity/login.xml resources/xml/activity/login.xml
    FILE resources/xml/activity/player.xml resources/xml/activity/player.xml
    FILE resources/xml/activity/reader.xml resources/xml/activity/reader.xml
    FILE resources/xml/tabs/home.xml resources/xml/tabs/home.xml
    FILE resources/xml/tabs/library.xml resources/xml/tabs/library.xml
    FILE resources/xml/tabs/search.xml resources/xml/tabs/search.xml
    FILE resources/xml/tabs/settings.xml resources/xml/tabs/settings.xml
    FILE resources/xml/tabs/extensions.xml resources/xml/tabs/extensions.xml
    FILE resources/xml/view/media_detail.xml resources/xml/view/media_detail.xml
    FILE resources/xml/view/media_item.xml resources/xml/view/media_item.xml
    FILE resources/xml/view/chapter_item.xml resources/xml/view/chapter_item.xml
    FILE resources/xml/view/manga_detail.xml resources/xml/view/manga_detail.xml
    FILE resources/xml/view/manga_item.xml resources/xml/view/manga_item.xml

    # Icons
    FILE resources/icons/a.png resources/icons/a.png
    FILE resources/icons/az.png resources/icons/az.png
    FILE resources/icons/back.png resources/icons/back.png
    FILE resources/icons/book-arrow-down.png resources/icons/book-arrow-down.png
    FILE resources/icons/book-multiple.png resources/icons/book-multiple.png
    FILE resources/icons/book-open-page-variant.png resources/icons/book-open-page-variant.png
    FILE resources/icons/checkbox.png resources/icons/checkbox.png
    FILE resources/icons/checkbox_checked.png resources/icons/checkbox_checked.png
    FILE resources/icons/checkbox_crossed.png resources/icons/checkbox_crossed.png
    FILE resources/icons/circle.png resources/icons/circle.png
    FILE resources/icons/cross.png resources/icons/cross.png
    FILE resources/icons/download.png resources/icons/download.png
    FILE resources/icons/filter-menu-outline.png resources/icons/filter-menu-outline.png
    FILE resources/icons/format-list-group.png resources/icons/format-list-group.png
    FILE resources/icons/hide.png resources/icons/hide.png
    FILE resources/icons/history.png resources/icons/history.png
    FILE resources/icons/hot.png resources/icons/hot.png
    FILE resources/icons/import.png resources/icons/import.png
    FILE resources/icons/left.png resources/icons/left.png
    FILE resources/icons/menu.png resources/icons/menu.png
    FILE resources/icons/mini_star.png resources/icons/mini_star.png
    FILE resources/icons/options.png resources/icons/options.png
    FILE resources/icons/radio.png resources/icons/radio.png
    FILE resources/icons/radio_checked.png resources/icons/radio_checked.png
    FILE resources/icons/refresh.png resources/icons/refresh.png
    FILE resources/icons/right.png resources/icons/right.png
    FILE resources/icons/search.png resources/icons/search.png
    FILE resources/icons/show.png resources/icons/show.png
    FILE resources/icons/sort-1-9.png resources/icons/sort-1-9.png
    FILE resources/icons/sort-9-1.png resources/icons/sort-9-1.png
    FILE resources/icons/sort-calendar-ascending.png resources/icons/sort-calendar-ascending.png
    FILE resources/icons/sort-calendar-descending.png resources/icons/sort-calendar-descending.png
    FILE resources/icons/sort-clock-ascending.png resources/icons/sort-clock-ascending.png
    FILE resources/icons/sort-clock-descending.png resources/icons/sort-clock-descending.png
    FILE resources/icons/star.png resources/icons/star.png
    FILE resources/icons/tag.png resources/icons/tag.png
    FILE resources/icons/web.png resources/icons/web.png
    FILE resources/icons/za.png resources/icons/za.png
    FILE resources/icons/play.png resources/icons/play.png
    FILE resources/icons/pause.png resources/icons/pause.png
    FILE resources/icons/skip-next.png resources/icons/skip-next.png
    FILE resources/icons/skip-previous.png resources/icons/skip-previous.png
    FILE resources/icons/subtitles.png resources/icons/subtitles.png
    FILE resources/icons/video-image.png resources/icons/video-image.png
    FILE resources/icons/translate.png resources/icons/translate.png
    FILE resources/icons/fast-forward-5.png resources/icons/fast-forward-5.png
    FILE resources/icons/fast-forward-10.png resources/icons/fast-forward-10.png
    FILE resources/icons/fast-forward-15.png resources/icons/fast-forward-15.png
    FILE resources/icons/fast-forward-30.png resources/icons/fast-forward-30.png
    FILE resources/icons/fast-forward-45.png resources/icons/fast-forward-45.png
    FILE resources/icons/fast-forward-60.png resources/icons/fast-forward-60.png
    FILE resources/icons/rewind-5.png resources/icons/rewind-5.png
    FILE resources/icons/rewind-10.png resources/icons/rewind-10.png
    FILE resources/icons/rewind-15.png resources/icons/rewind-15.png
    FILE resources/icons/rewind-30.png resources/icons/rewind-30.png
    FILE resources/icons/rewind-45.png resources/icons/rewind-45.png
    FILE resources/icons/rewind-60.png resources/icons/rewind-60.png
    FILE resources/icons/shuffle-variant.png resources/icons/shuffle-variant.png
    FILE resources/icons/shuffle-disabled.png resources/icons/shuffle-disabled.png
    FILE resources/icons/repeat.png resources/icons/repeat.png
    FILE resources/icons/repeat-once.png resources/icons/repeat-once.png
    FILE resources/icons/repeat-off.png resources/icons/repeat-off.png

    # Images
    FILE resources/images/circle_button.png resources/images/circle_button.png
    FILE resources/images/cross_button.png resources/images/cross_button.png
    FILE resources/images/dpad.png resources/images/dpad.png
    FILE resources/images/gradient.png resources/images/gradient.png
    FILE resources/images/l_button.png resources/images/l_button.png
    FILE resources/images/logo-small.png resources/images/logo-small.png
    FILE resources/images/logo.png resources/images/logo.png
    FILE resources/images/r_button.png resources/images/r_button.png
    FILE resources/images/select_button.png resources/images/select_button.png
    FILE resources/images/square_button.png resources/images/square_button.png
    FILE resources/images/start_button.png resources/images/start_button.png
    FILE resources/images/triangle_button.png resources/images/triangle_button.png

    # Fonts (using simpler DejaVu fonts for Vita compatibility)
    FILE resources/font/font.ttf resources/font/font.ttf
    FILE resources/font/icon.ttf resources/font/icon.ttf
    FILE resources/font/switch_font.ttf resources/font/switch_font.ttf
    FILE resources/font/switch_icons.ttf resources/font/switch_icons.ttf
    FILE resources/material/MaterialIcons-Regular.ttf resources/material/MaterialIcons-Regular.ttf
)
