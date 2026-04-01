cmake_minimum_required(VERSION 3.16)

# ---------------------------------------------------------------------------
# Platform selection
# Pass -DPLATFORM_PSV=ON, -DPLATFORM_SWITCH=ON, or -DPLATFORM_DESKTOP=ON
# to cmake when configuring. Exactly one must be ON.
# Example: cmake -B build -DPLATFORM_DESKTOP=ON
# ---------------------------------------------------------------------------

option(PLATFORM_PSV     "Build for PlayStation Vita"                   OFF)
option(PLATFORM_SWITCH  "Build for Nintendo Switch"                    OFF)
option(PLATFORM_PS4     "Build for PlayStation 4"                      OFF)
option(PLATFORM_DESKTOP "Build for Desktop (Linux/macOS/Windows)"      OFF)
option(PLATFORM_ANDROID "Build for Android"                            OFF)

# Default to desktop when nothing is specified
set(_PLATFORM_COUNT 0)
foreach(_P PSV SWITCH PS4 DESKTOP ANDROID)
    if(PLATFORM_${_P})
        math(EXPR _PLATFORM_COUNT "${_PLATFORM_COUNT} + 1")
    endif()
endforeach()
if(_PLATFORM_COUNT EQUAL 0)
    message(STATUS "No platform selected — defaulting to PLATFORM_DESKTOP=ON")
    set(PLATFORM_DESKTOP ON CACHE BOOL "Build for Desktop" FORCE)
elseif(_PLATFORM_COUNT GREATER 1)
    message(FATAL_ERROR "More than one PLATFORM_* flag is ON. Set exactly one.")
endif()

# ---------------------------------------------------------------------------
# Borealis paths — must be set before the cmake includes below
# ---------------------------------------------------------------------------
set(BOREALIS_DIR     ${CMAKE_CURRENT_SOURCE_DIR}/lib/borealis)
set(BOREALIS_LIBRARY ${BOREALIS_DIR}/library)

# Forward into borealis-internal cache variables
set(PLATFORM_DESKTOP ${PLATFORM_DESKTOP} CACHE BOOL "" FORCE)
set(PLATFORM_SWITCH  ${PLATFORM_SWITCH}  CACHE BOOL "" FORCE)
set(PLATFORM_PSV     ${PLATFORM_PSV}     CACHE BOOL "" FORCE)
set(PLATFORM_PS4     ${PLATFORM_PS4}     CACHE BOOL "" FORCE)
set(PLATFORM_IOS     OFF                 CACHE BOOL "" FORCE)
set(PLATFORM_ANDROID ${PLATFORM_ANDROID} CACHE BOOL "" FORCE)

if(PLATFORM_PSV)
    set(BOREALIS_USE_GXM ON  CACHE BOOL "Use GXM renderer for PS Vita" FORCE)
    set(USE_VITA_SHARK  OFF  CACHE BOOL "Use VitaShark for shader compilation" FORCE)
endif()

set(USE_LIBROMFS OFF CACHE BOOL "Use libromfs")

# For Android: point LIBROMFS_PREBUILT_GENERATOR at an existing file so that
# borealis toolchain.cmake's check_libromfs_generator() does not fatal.
# We force USE_LIBROMFS OFF below, so the generator is never actually invoked.
if(PLATFORM_ANDROID)
    set(LIBROMFS_PREBUILT_GENERATOR "${CMAKE_CURRENT_LIST_FILE}" CACHE STRING "" FORCE)
endif()

include(${BOREALIS_LIBRARY}/cmake/commonOption.cmake)
include(${BOREALIS_LIBRARY}/cmake/toolchain.cmake)

# FIX: borealis toolchain.cmake forces USE_LIBROMFS=ON for Android, but we use
# APK-asset extraction instead (see src/platform/android_assets.cpp).  Override
# it back to OFF so borealis does not try to compile/link libromfs.
# Also set BRLS_PLATFORM_RESOURCES_PATH which borealis CMakeLists.txt leaves
# empty for Android — without it BRLS_RESOURCES is undefined and all file loads
# via fopen("resources/...") fail.
if(PLATFORM_ANDROID)
    set(USE_LIBROMFS OFF)
    set(BRLS_PLATFORM_RESOURCES_PATH "\"resources/\"")
endif()

# ---------------------------------------------------------------------------
# Project (after toolchain so cross-compilers are in effect)
# ---------------------------------------------------------------------------
project(VitaPlex VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -frtti -fexceptions")

# ---------------------------------------------------------------------------
# Vita VPK metadata
# ---------------------------------------------------------------------------
if(PLATFORM_PSV)
    set(VITA_APP_NAME "VitaPlex")
    set(VITA_TITLEID  "VPLEX0001")
    set(VITA_VERSION  "01.00")
    set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d PARENTAL_LEVEL=1 -d ATTRIBUTE2=12")
endif()

# ---------------------------------------------------------------------------
# PSV borealis patch
# ---------------------------------------------------------------------------
if(PLATFORM_PSV)
    set(PSV_PLATFORM_PATCH  ${CMAKE_CURRENT_SOURCE_DIR}/patches/psv_platform.cpp)
    set(PSV_PLATFORM_TARGET ${BOREALIS_LIBRARY}/lib/platforms/psv/psv_platform.cpp)
    if(EXISTS ${PSV_PLATFORM_PATCH})
        message(STATUS "Applying patch: psv_platform.cpp")
        configure_file(${PSV_PLATFORM_PATCH} ${PSV_PLATFORM_TARGET} COPYONLY)
    else()
        message(FATAL_ERROR "Patch file not found: ${PSV_PLATFORM_PATCH}")
    endif()
endif()

add_subdirectory(${BOREALIS_LIBRARY})

# ---------------------------------------------------------------------------
# Application sources
# ---------------------------------------------------------------------------
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
)

# vita_stubs.c only needed on Vita (no-op stdio locks, SDL_OpenURL stub)
if(PLATFORM_PSV)
    list(APPEND APP_SOURCES src/utils/vita_stubs.c)
endif()

# Switch: userAppInit/userAppExit (romfs, sockets, system services init)
if(PLATFORM_SWITCH)
    list(APPEND APP_SOURCES ${BOREALIS_LIBRARY}/lib/platforms/switch/switch_wrapper.c)
endif()

# Android asset extraction helper
if(PLATFORM_ANDROID)
    list(APPEND APP_SOURCES src/platform/android_assets.cpp)
endif()

set(APP_INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${BOREALIS_LIBRARY}/include
    ${BOREALIS_LIBRARY}/include/borealis/extern
)

# ---------------------------------------------------------------------------
# Target
# ---------------------------------------------------------------------------
if(PLATFORM_ANDROID)
    add_library(${PROJECT_NAME} SHARED ${APP_SOURCES})
else()
    add_executable(${PROJECT_NAME} ${APP_SOURCES})
endif()

target_include_directories(${PROJECT_NAME} PRIVATE ${APP_INCLUDES})
if(PLATFORM_DESKTOP AND APPLE)
    target_include_directories(${PROJECT_NAME} PRIVATE
        /opt/homebrew/include
        /usr/local/include
    )
    target_link_directories(${PROJECT_NAME} PRIVATE
        /opt/homebrew/lib
        /usr/local/lib
    )
endif()

if(PLATFORM_PSV)
    target_compile_definitions(${PROJECT_NAME} PRIVATE RESOURCE_PREFIX=\"app0:resources/\")
elseif(PLATFORM_PS4)
    target_compile_definitions(${PROJECT_NAME} PRIVATE RESOURCE_PREFIX=\"/app0/resources/\")
elseif(PLATFORM_SWITCH OR USE_LIBROMFS)
    # Switch always uses romfs. Android does NOT — resources are APK assets via SDL.
    target_compile_definitions(${PROJECT_NAME} PRIVATE RESOURCE_PREFIX=\"romfs:/\")
else()
    target_compile_definitions(${PROJECT_NAME} PRIVATE RESOURCE_PREFIX=\"resources/\")
endif()

# ---------------------------------------------------------------------------
# Link libraries
# ---------------------------------------------------------------------------
if(PLATFORM_ANDROID)
    # Android: manga reader only — libwebp handles animated WebP natively,
    # no video playback needed so skip mpv/ffmpeg entirely.
    set(APP_FFMPEG_LIBS)
    set(APP_FFMPEG_LINK_BLOCK)
elseif(WIN32)
    set(APP_FFMPEG_LIBS mpv)
    set(APP_FFMPEG_LINK_BLOCK ${APP_FFMPEG_LIBS})
else()
    set(APP_FFMPEG_LIBS
        mpv
        avfilter
        avformat
        avcodec
        swresample
        swscale
        avutil
    )
    if(APPLE OR PLATFORM_PS4)
        set(APP_FFMPEG_LINK_BLOCK ${APP_FFMPEG_LIBS})
    else()
        set(APP_FFMPEG_LINK_BLOCK -Wl,--start-group ${APP_FFMPEG_LIBS} -Wl,--end-group)
    endif()
endif()

if(PLATFORM_ANDROID)
    # Android dependency bundle intentionally omits libpng/libbz2/libzstd.
    # Keeping these out of the explicit link list avoids CI link failures when
    # those optional transitives are not packaged in jniLibs.
    set(APP_RENDER_LIBS
        ass
        freetype
        fribidi
        webp
        z
        c
        harfbuzz
    )
elseif(WIN32)
    set(APP_RENDER_LIBS
        bz2
        webp
        z
    )
else()
    set(APP_RENDER_LIBS
        ass
        freetype
        fribidi
        png
        bz2
        webp
        z
        c
    )
endif()

if(NOT PLATFORM_PS4 AND NOT WIN32 AND NOT PLATFORM_ANDROID)
    list(APPEND APP_RENDER_LIBS
        zstd
        harfbuzz
    )
endif()

set(APP_NETWORK_LIBS
    curl
)

if(NOT WIN32)
    list(APPEND APP_NETWORK_LIBS
        mbedtls
        mbedx509
        mbedcrypto
    )
endif()

target_link_libraries(${PROJECT_NAME}
    borealis
    ${APP_PLATFORM_LIB}
    ${APP_NETWORK_LIBS}
    ${APP_FFMPEG_LINK_BLOCK}
    ${APP_RENDER_LIBS}
)

if(NOT PLATFORM_SWITCH AND NOT PLATFORM_PS4 AND NOT WIN32 AND NOT PLATFORM_ANDROID)
    target_link_libraries(${PROJECT_NAME} mp3lame)
endif()

# Android needs the android library for AAssetManager (used by asset extraction)
if(PLATFORM_ANDROID)
    target_link_libraries(${PROJECT_NAME} android log)
endif()

if(PLATFORM_PSV)
    # Required for __wrap_pthread_create in src/utils/vita_stubs.c
    target_link_options(${PROJECT_NAME} PRIVATE "-Wl,--wrap=pthread_create")

    target_link_libraries(${PROJECT_NAME}
        archive
        vita2d
        SceNet_stub
        SceNetCtl_stub
        SceHttp_stub
        SceSsl_stub
        ScePgf_stub
        SceRtc_stub
        SceShellSvc_stub
        SceAppMgr_stub
        SceTouch_stub
        SceAudiodec_stub
        SceVideodec_stub
        SceAvcodec_stub
        SceCodecEngine_stub
        SceAudio_stub
        SceVshBridge_stub
    )
elseif(PLATFORM_SWITCH)
    target_link_libraries(${PROJECT_NAME} nx dav1d)
elseif(PLATFORM_PS4)
    target_link_libraries(${PROJECT_NAME}
        SceNet
    )
elseif(PLATFORM_DESKTOP)
    find_package(Threads REQUIRED)
    target_link_libraries(${PROJECT_NAME} Threads::Threads)
    if(APPLE)
        target_link_libraries(${PROJECT_NAME} "-framework CoreWLAN")
    endif()
endif()

# ---------------------------------------------------------------------------
# Platform packaging / post-build
# ---------------------------------------------------------------------------
if(PLATFORM_PSV)
    vita_create_self(${PROJECT_NAME}.self ${PROJECT_NAME} UNSAFE NOASLR)

    vita_create_vpk(${PROJECT_NAME}.vpk ${VITA_TITLEID} ${PROJECT_NAME}.self
        VERSION ${VITA_VERSION}
        NAME    ${VITA_APP_NAME}

        FILE app/platform/psv/sce_sys/icon0.png                      sce_sys/icon0.png
        FILE app/platform/psv/sce_sys/pic0.png                       sce_sys/pic0.png
        FILE app/platform/psv/sce_sys/livearea/contents/bg.png       sce_sys/livearea/contents/bg.png
        FILE app/platform/psv/sce_sys/livearea/contents/startup.png  sce_sys/livearea/contents/startup.png
        FILE app/platform/psv/sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml

        FILE ${BOREALIS_DIR}/resources/i18n/en-US/hints.json resources/i18n/en-US/hints.json
        FILE resources/i18n/en-US/main.json                  resources/i18n/en-US/main.json

        FILE ${BOREALIS_DIR}/resources/img/sys/battery_back_dark.png  resources/img/sys/battery_back_dark.png
        FILE ${BOREALIS_DIR}/resources/img/sys/battery_back_light.png resources/img/sys/battery_back_light.png
        FILE ${BOREALIS_DIR}/resources/img/sys/wifi_0_dark.png        resources/img/sys/wifi_0_dark.png
        FILE ${BOREALIS_DIR}/resources/img/sys/wifi_0_light.png       resources/img/sys/wifi_0_light.png
        FILE ${BOREALIS_DIR}/resources/img/sys/wifi_1_dark.png        resources/img/sys/wifi_1_dark.png
        FILE ${BOREALIS_DIR}/resources/img/sys/wifi_1_light.png       resources/img/sys/wifi_1_light.png
        FILE ${BOREALIS_DIR}/resources/img/sys/wifi_2_dark.png        resources/img/sys/wifi_2_dark.png
        FILE ${BOREALIS_DIR}/resources/img/sys/wifi_2_light.png       resources/img/sys/wifi_2_light.png
        FILE ${BOREALIS_DIR}/resources/img/sys/wifi_3_dark.png        resources/img/sys/wifi_3_dark.png
        FILE ${BOREALIS_DIR}/resources/img/sys/wifi_3_light.png       resources/img/sys/wifi_3_light.png
        FILE ${BOREALIS_DIR}/resources/img/sys/ethernet_dark.png      resources/img/sys/ethernet_dark.png
        FILE ${BOREALIS_DIR}/resources/img/sys/ethernet_light.png     resources/img/sys/ethernet_light.png

        FILE resources/xml/activity/main.xml     resources/xml/activity/main.xml
        FILE resources/xml/activity/login.xml    resources/xml/activity/login.xml
        FILE resources/xml/activity/reader.xml   resources/xml/activity/reader.xml
        FILE resources/xml/tabs/library.xml      resources/xml/tabs/library.xml
        FILE resources/xml/tabs/search.xml       resources/xml/tabs/search.xml
        FILE resources/xml/tabs/settings.xml     resources/xml/tabs/settings.xml
        FILE resources/xml/tabs/extensions.xml   resources/xml/tabs/extensions.xml
        FILE resources/xml/view/manga_detail.xml resources/xml/view/manga_detail.xml
        FILE resources/xml/view/manga_item.xml   resources/xml/view/manga_item.xml
        FILE resources/xml/view/chapter_item.xml resources/xml/view/chapter_item.xml

        FILE resources/icons/a.png                        resources/icons/a.png
        FILE resources/icons/az.png                       resources/icons/az.png
        FILE resources/icons/za.png                       resources/icons/za.png
        FILE resources/icons/back.png                     resources/icons/back.png
        FILE resources/icons/checkbox.png                 resources/icons/checkbox.png
        FILE resources/icons/checkbox_checked.png         resources/icons/checkbox_checked.png
        FILE resources/icons/checkbox_crossed.png         resources/icons/checkbox_crossed.png
        FILE resources/icons/circle.png                   resources/icons/circle.png
        FILE resources/icons/cross.png                    resources/icons/cross.png
        FILE resources/icons/download.png                 resources/icons/download.png
        FILE resources/icons/hide.png                     resources/icons/hide.png
        FILE resources/icons/history.png                  resources/icons/history.png
        FILE resources/icons/hot.png                      resources/icons/hot.png
        FILE resources/icons/import.png                   resources/icons/import.png
        FILE resources/icons/left.png                     resources/icons/left.png
        FILE resources/icons/menu.png                     resources/icons/menu.png
        FILE resources/icons/mini_star.png                resources/icons/mini_star.png
        FILE resources/icons/options.png                  resources/icons/options.png
        FILE resources/icons/radio.png                    resources/icons/radio.png
        FILE resources/icons/radio_checked.png            resources/icons/radio_checked.png
        FILE resources/icons/refresh.png                  resources/icons/refresh.png
        FILE resources/icons/right.png                    resources/icons/right.png
        FILE resources/icons/search.png                   resources/icons/search.png
        FILE resources/icons/show.png                     resources/icons/show.png
        FILE resources/icons/sort-1-9.png                 resources/icons/sort-1-9.png
        FILE resources/icons/sort-9-1.png                 resources/icons/sort-9-1.png
        FILE resources/icons/sort-clock-ascending.png     resources/icons/sort-clock-ascending.png
        FILE resources/icons/sort-clock-descending.png    resources/icons/sort-clock-descending.png
        FILE resources/icons/sort-calendar-ascending.png  resources/icons/sort-calendar-ascending.png
        FILE resources/icons/sort-calendar-descending.png resources/icons/sort-calendar-descending.png
        FILE resources/icons/book-open-page-variant.png   resources/icons/book-open-page-variant.png
        FILE resources/icons/book-multiple.png            resources/icons/book-multiple.png
        FILE resources/icons/book-arrow-down.png          resources/icons/book-arrow-down.png
        FILE resources/icons/star.png                     resources/icons/star.png
        FILE resources/icons/tag.png                      resources/icons/tag.png
        FILE resources/icons/web.png                      resources/icons/web.png
        FILE resources/icons/filter-menu-outline.png      resources/icons/filter-menu-outline.png
        FILE resources/icons/format-list-group.png        resources/icons/format-list-group.png

        FILE resources/images/circle_button.png   resources/images/circle_button.png
        FILE resources/images/cross_button.png    resources/images/cross_button.png
        FILE resources/images/dpad.png            resources/images/dpad.png
        FILE resources/images/gradient.png        resources/images/gradient.png
        FILE resources/images/l_button.png        resources/images/l_button.png
        FILE resources/images/logo-small.png      resources/images/logo-small.png
        FILE resources/images/logo.png            resources/images/logo.png
        FILE resources/images/r_button.png        resources/images/r_button.png
        FILE resources/images/select_button.png   resources/images/select_button.png
        FILE resources/images/square_button.png   resources/images/square_button.png
        FILE resources/images/start_button.png    resources/images/start_button.png
        FILE resources/images/triangle_button.png resources/images/triangle_button.png

        FILE resources/font/font.ttf                      resources/font/font.ttf
        FILE resources/font/icon.ttf                      resources/font/icon.ttf
        FILE resources/font/switch_font.ttf               resources/font/switch_font.ttf
        FILE resources/font/switch_icons.ttf              resources/font/switch_icons.ttf
        FILE resources/material/MaterialIcons-Regular.ttf resources/material/MaterialIcons-Regular.ttf
    )

elseif(PLATFORM_PS4)
    if(PS4)
        # PS4 variable is defined by the pacbrew toolchain
        add_self(${PROJECT_NAME})
        add_pkg(${PROJECT_NAME}
            ${CMAKE_BINARY_DIR}/ps4
            "VPLEX0002"
            "VitaPlex"
            "01.00"
            # https://psdevwiki.com/ps4/param.sfo#ATTRIBUTE - Support PS4 Pro
            8388608
        )
    endif()
    add_custom_target(${PROJECT_NAME}.data
        # Copy borealis PS4 base files (sce_module, about/right.sprx)
        COMMAND "${CMAKE_COMMAND}" -E copy_directory ${BOREALIS_DIR}/ps4 ${CMAKE_BINARY_DIR}/ps4
        # Override with our custom sce_sys (icon)
        COMMAND "${CMAKE_COMMAND}" -E copy_directory ${CMAKE_SOURCE_DIR}/app/platform/ps4 ${CMAKE_BINARY_DIR}/ps4
        # Copy app resources
        COMMAND "${CMAKE_COMMAND}" -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/resources ${CMAKE_BINARY_DIR}/ps4/resources
        # Copy borealis system resources
        COMMAND "${CMAKE_COMMAND}" -E copy_directory ${BOREALIS_DIR}/resources/i18n ${CMAKE_BINARY_DIR}/ps4/resources/i18n
        COMMAND "${CMAKE_COMMAND}" -E copy_directory ${BOREALIS_DIR}/resources/img ${CMAKE_BINARY_DIR}/ps4/resources/img
    )
    add_dependencies(${PROJECT_NAME} ${PROJECT_NAME}.data)

elseif(PLATFORM_SWITCH)
    # Build NRO with embedded romfs resources (matches switchfin approach).
    # nx_create_nro doesn't embed romfs or set metadata correctly, so we
    # call nacptool + elf2nro manually.
    if(USE_DEKO3D)
        gen_dksh("${CMAKE_CURRENT_SOURCE_DIR}/resources/shaders")
    endif()
    add_custom_target(${PROJECT_NAME}.nro ALL DEPENDS ${PROJECT_NAME}
        # Generate NACP metadata (author, version shown in hbmenu)
        COMMAND ${NX_NACPTOOL_EXE} --create "VitaPlex" "Breezyslasher"
            "${PROJECT_VERSION}" ${PROJECT_NAME}.nacp
        # Assemble romfs: app resources + borealis system resources
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/resources ${CMAKE_BINARY_DIR}/resources
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${BOREALIS_DIR}/resources/i18n ${CMAKE_BINARY_DIR}/resources/i18n
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${BOREALIS_DIR}/resources/img ${CMAKE_BINARY_DIR}/resources/img
        # Build NRO with icon, metadata, and romfs
        COMMAND ${NX_ELF2NRO_EXE} ${PROJECT_NAME}.elf ${PROJECT_NAME}.nro
            --icon=${CMAKE_CURRENT_SOURCE_DIR}/app/platform/icon_switch.jpg
            --nacp=${PROJECT_NAME}.nacp
            --romfsdir=${CMAKE_BINARY_DIR}/resources
    )

elseif(PLATFORM_DESKTOP)
    # Copy app resources next to the binary so relative paths resolve.
    # Also copy borealis system resources (i18n/hints.json, img/sys/) which
    # are not checked in to resources/ but are required by borealis at runtime.
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/resources
            $<TARGET_FILE_DIR:${PROJECT_NAME}>/resources
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${BOREALIS_DIR}/resources/i18n
            $<TARGET_FILE_DIR:${PROJECT_NAME}>/resources/i18n
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${BOREALIS_DIR}/resources/img
            $<TARGET_FILE_DIR:${PROJECT_NAME}>/resources/img
        COMMENT "Copying resources (app + borealis) for desktop build"
    )
endif()
