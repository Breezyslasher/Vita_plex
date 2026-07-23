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
option(PLATFORM_IOS     "Build for iOS (iPhone / iPad)"                OFF)
option(PLATFORM_TVOS    "Build for tvOS (Apple TV)"                    OFF)

# Default to desktop when nothing is specified
set(_PLATFORM_COUNT 0)
foreach(_P PSV SWITCH PS4 DESKTOP ANDROID IOS TVOS)
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
set(PLATFORM_IOS     ${PLATFORM_IOS}     CACHE BOOL "" FORCE)
set(PLATFORM_TVOS    ${PLATFORM_TVOS}    CACHE BOOL "" FORCE)
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

# Xcode 15 / iOS SDK 17.5 libc++ workaround. <vector> transitively pulls
# in <__format/formatter_integral.h> -> <locale>, and <locale> uses
# struct tm's members (tm_min, tm_hour, tm_sec, …) without including
# <time.h> first. <wchar.h> only forward-declares tm, so without this
# clang reports "member access into incomplete type 'tm'" all over the
# codebase on every TU that pulls in borealis/core/event.hpp ->
# <algorithm> / <functional>.
#
# Force-include the C header <time.h> rather than the C++ wrapper
# <ctime>. <ctime> just does `using ::size_t _LIBCPP_USING_IF_EXISTS;`
# which silently no-ops when ::size_t isn't declared yet — and then
# <__type_traits/aligned_storage.h> can't resolve std::size_t and the
# whole build collapses. <time.h> actually *defines* size_t / tm /
# time_t in the global namespace, which makes libc++'s later
# using-declarations resolve cleanly. <time.h> is also valid C, so
# the SDL2 C sources (SDL_uikit_main.c et al.) don't need the
# COMPILE_LANGUAGE:CXX gate the previous iteration carried.
if(PLATFORM_IOS OR PLATFORM_TVOS)
    add_compile_options(-include time.h)
endif()

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

# ---------------------------------------------------------------------------
# Borealis fontstash patch (all platforms): caches kerning advances so
# stb_truetype's GPOS table walk doesn't run per glyph pair per frame.
# Big win for any view with lots of text (Live TV titles, library lists);
# rendering is unchanged visually, just much faster.
# ---------------------------------------------------------------------------
set(FONTSTASH_PATCH  ${CMAKE_CURRENT_SOURCE_DIR}/patches/fontstash.h)
set(FONTSTASH_TARGET ${BOREALIS_LIBRARY}/include/borealis/extern/nanovg/fontstash.h)
if(EXISTS ${FONTSTASH_PATCH})
    message(STATUS "Applying patch: fontstash.h")
    configure_file(${FONTSTASH_PATCH} ${FONTSTASH_TARGET} COPYONLY)
else()
    message(FATAL_ERROR "Patch file not found: ${FONTSTASH_PATCH}")
endif()

# ---------------------------------------------------------------------------
# Borealis nanovg patch (all platforms): adds nvgTextBatchBegin/End so
# views with many text labels of identical style (the EPG grid is the
# canonical case) can flush all of them in one render call instead of
# one per text line. Backward-compatible — code that doesn't call the
# new entry points keeps the original draw-per-text behaviour.
# ---------------------------------------------------------------------------
set(NANOVG_PATCH  ${CMAKE_CURRENT_SOURCE_DIR}/patches/nanovg.c)
set(NANOVG_TARGET ${BOREALIS_LIBRARY}/lib/extern/nanovg/nanovg.c)
if(EXISTS ${NANOVG_PATCH})
    message(STATUS "Applying patch: nanovg.c")
    configure_file(${NANOVG_PATCH} ${NANOVG_TARGET} COPYONLY)
else()
    message(FATAL_ERROR "Patch file not found: ${NANOVG_PATCH}")
endif()

# ---------------------------------------------------------------------------
# Borealis Application patch (all platforms): adds a hold-A-to-open-
# menu shim. Default behaviour is off, so non-Android paths keep the
# original press-time click semantics; main_activity.cpp flips it on
# at runtime only when SDL_IsAndroidTV() returns true so the bare
# 5-button TV remotes (no Menu/Guide key) can still reach the
# Options context menu via a long-press on OK.
# ---------------------------------------------------------------------------
set(BRLS_APP_HPP_PATCH  ${CMAKE_CURRENT_SOURCE_DIR}/patches/brls_application.hpp)
set(BRLS_APP_HPP_TARGET ${BOREALIS_LIBRARY}/include/borealis/core/application.hpp)
if(EXISTS ${BRLS_APP_HPP_PATCH})
    message(STATUS "Applying patch: borealis application.hpp")
    configure_file(${BRLS_APP_HPP_PATCH} ${BRLS_APP_HPP_TARGET} COPYONLY)
else()
    message(FATAL_ERROR "Patch file not found: ${BRLS_APP_HPP_PATCH}")
endif()

set(BRLS_APP_CPP_PATCH  ${CMAKE_CURRENT_SOURCE_DIR}/patches/brls_application.cpp)
set(BRLS_APP_CPP_TARGET ${BOREALIS_LIBRARY}/lib/core/application.cpp)
if(EXISTS ${BRLS_APP_CPP_PATCH})
    message(STATUS "Applying patch: borealis application.cpp")
    configure_file(${BRLS_APP_CPP_PATCH} ${BRLS_APP_CPP_TARGET} COPYONLY)
else()
    message(FATAL_ERROR "Patch file not found: ${BRLS_APP_CPP_PATCH}")
endif()

# ---------------------------------------------------------------------------
# GLFW keyboard mapping patch (desktop only): adds keyboard shortcuts
# for Start / Back / X / Y / LB / RB on the desktop GLFW backend so
# the dev workflow doesn't require a physical gamepad.
# ---------------------------------------------------------------------------
if(NOT PLATFORM_SWITCH AND NOT PLATFORM_PSV AND NOT PLATFORM_PS4 AND NOT PLATFORM_ANDROID
   AND NOT PLATFORM_IOS AND NOT PLATFORM_TVOS)
    set(GLFW_INPUT_PATCH  ${CMAKE_CURRENT_SOURCE_DIR}/patches/glfw_input.cpp)
    set(GLFW_INPUT_TARGET ${BOREALIS_LIBRARY}/lib/platforms/glfw/glfw_input.cpp)
    if(EXISTS ${GLFW_INPUT_PATCH})
        message(STATUS "Applying patch: glfw_input.cpp")
        configure_file(${GLFW_INPUT_PATCH} ${GLFW_INPUT_TARGET} COPYONLY)
    endif()

    # GLFW platform patch (desktop only): keep the main loop ticking while the
    # window is minimized so OS media-control transport (Linux MPRIS / multimedia
    # keys, posted onto the loop via brls::sync) still drives playback. Upstream
    # blocked in glfwWaitEvents() while iconified, freezing sync-task processing.
    set(GLFW_PLATFORM_PATCH  ${CMAKE_CURRENT_SOURCE_DIR}/patches/glfw_platform.cpp)
    set(GLFW_PLATFORM_TARGET ${BOREALIS_LIBRARY}/lib/platforms/glfw/glfw_platform.cpp)
    if(EXISTS ${GLFW_PLATFORM_PATCH})
        message(STATUS "Applying patch: glfw_platform.cpp")
        configure_file(${GLFW_PLATFORM_PATCH} ${GLFW_PLATFORM_TARGET} COPYONLY)
    endif()
endif()

# iOS / tvOS embed app resources through libromfs (borealis toolchain.cmake
# forces USE_LIBROMFS=ON for these platforms). libromfs's own CMakeLists
# fatals unless we tell it WHAT to embed and under what project name — set
# both here, before borealis adds the libromfs subdirectory at
# add_subdirectory(${BOREALIS_LIBRARY}) below. RESOURCE_LOCATION points at
# our resources/ tree; the generator walks it at build time and bakes every
# file into libromfs_resources.cpp, which is why RESOURCE_PREFIX is
# "romfs:/" for these targets (set further down).
if(PLATFORM_IOS OR PLATFORM_TVOS)
    # libromfs embeds a SINGLE directory tree, so merge our app resources
    # with borealis's system resources (i18n/hints.json, img/sys/) into a
    # staging dir and point the generator at that. The app tree is copied
    # first; borealis's is overlaid without clobbering (our i18n/en-US/
    # main.json must survive, but we still need borealis's hints.json and
    # img/sys/*). Done at configure time so the path exists before
    # libromfs's CMakeLists validates it.
    set(IOS_ROMFS_STAGE "${CMAKE_BINARY_DIR}/romfs_stage")
    file(MAKE_DIRECTORY "${IOS_ROMFS_STAGE}")
    file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/resources/" DESTINATION "${IOS_ROMFS_STAGE}")
    file(COPY "${BOREALIS_DIR}/resources/i18n" DESTINATION "${IOS_ROMFS_STAGE}")
    file(COPY "${BOREALIS_DIR}/resources/img"  DESTINATION "${IOS_ROMFS_STAGE}")
    set(LIBROMFS_PROJECT_NAME "VitaPlex" CACHE STRING "" FORCE)
    set(LIBROMFS_RESOURCE_LOCATION "${IOS_ROMFS_STAGE}" CACHE PATH "" FORCE)
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
    src/app/music_controller.cpp
    src/app/hint_icons.cpp
    src/app/synclounge_client.cpp
    src/app/synclounge_session.cpp

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
    src/view/long_press_gesture.cpp
    src/view/sidebar_editor.cpp

    # Player
    src/player/mpv_player.cpp

    # Utils
    src/utils/http_client.cpp
    src/utils/http_cache.cpp
    src/utils/https_proxy.cpp
    src/utils/image_loader.cpp
    src/utils/jwt_auth.cpp
    src/utils/now_playing.cpp
    src/utils/pip.cpp
)

# ---------------------------------------------------------------------------
# Platform abstraction layer
#
# Exactly one src/platform/platform_<name>.cpp implements the entire
# vitaplex::platform interface declared in include/platform/platform.hpp.
# This is the same pattern Vita_Suwayomi uses: select one TU at configure
# time so the rest of the codebase contains zero #ifdef PLATFORM_* blocks.
#
# platform_common.cpp is always compiled — it holds the orientation /
# viewport helpers that defer to brls::Application and are identical
# across every target.
# ---------------------------------------------------------------------------
list(APPEND APP_SOURCES src/platform/platform_common.cpp)
if(PLATFORM_PSV)
    list(APPEND APP_SOURCES src/platform/platform_psv.cpp)
elseif(PLATFORM_PS4)
    list(APPEND APP_SOURCES src/platform/platform_ps4.cpp)
elseif(PLATFORM_SWITCH)
    list(APPEND APP_SOURCES src/platform/platform_switch.cpp)
elseif(PLATFORM_ANDROID)
    list(APPEND APP_SOURCES src/platform/platform_android.cpp)
elseif(PLATFORM_IOS OR PLATFORM_TVOS)
    # Same source file serves both iOS and tvOS — it branches on
    # TARGET_OS_TV from <TargetConditionals.h>, and the constraints
    # tables differ between the two (phone-sized vs. living-room).
    list(APPEND APP_SOURCES src/platform/platform_ios.mm)
else()
    list(APPEND APP_SOURCES src/platform/platform_desktop.cpp)
endif()

# vita_stubs.c only needed on Vita (no-op stdio locks, SDL_OpenURL stub)
if(PLATFORM_PSV)
    list(APPEND APP_SOURCES src/utils/vita_stubs.c)
endif()

# Switch: userAppInit/userAppExit (romfs, sockets, system services init) +
# pthread_create wrapper (1 MB stack floor for libmpv/ffmpeg threads — libnx's
# 128 KB default overflows them, crashing at playback start).
if(PLATFORM_SWITCH)
    list(APPEND APP_SOURCES ${BOREALIS_LIBRARY}/lib/platforms/switch/switch_wrapper.c)
    list(APPEND APP_SOURCES src/utils/switch_stubs.c)
endif()

# Android asset extraction helper (used by platform_android.cpp)
if(PLATFORM_ANDROID)
    list(APPEND APP_SOURCES src/platform/android_assets.cpp)
    # Direct-surface JNI bridge (mpv vo=gpu → dedicated SurfaceView).
    # Inert until the Java MpvSurface view + VO-path flip land; see
    # docs/android-direct-surface-playback.md.
    list(APPEND APP_SOURCES src/platform/android_mpv_surface.cpp)
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

if(PLATFORM_IOS OR PLATFORM_TVOS)
    # CI builds a minimal static libcurl from source against the matrix's
    # SDK and passes both the headers dir and the .a path. Local builds
    # can do the same or fall back to a Homebrew curl install for the
    # headers (link will fail without LIBCURL_LIBRARY though — there's
    # no point hiding that).
    if(NOT LIBCURL_INCLUDE_DIR AND CURL_INCLUDE_DIR)
        # Accept the more common variable name as an alias.
        set(LIBCURL_INCLUDE_DIR "${CURL_INCLUDE_DIR}")
    endif()
    if(LIBCURL_INCLUDE_DIR)
        target_include_directories(${PROJECT_NAME} PRIVATE "${LIBCURL_INCLUDE_DIR}")
    elseif(EXISTS /opt/homebrew/opt/curl/include)
        target_include_directories(${PROJECT_NAME} PRIVATE /opt/homebrew/opt/curl/include)
    elseif(EXISTS /usr/local/opt/curl/include)
        target_include_directories(${PROJECT_NAME} PRIVATE /usr/local/opt/curl/include)
    endif()
    if(LIBCURL_LIBRARY)
        target_link_libraries(${PROJECT_NAME} "${LIBCURL_LIBRARY}")
    endif()
endif()

if(PLATFORM_PSV)
    target_compile_definitions(${PROJECT_NAME} PRIVATE RESOURCE_PREFIX=\"app0:resources/\")
elseif(PLATFORM_PS4)
    target_compile_definitions(${PROJECT_NAME} PRIVATE RESOURCE_PREFIX=\"/app0/resources/\")
elseif(PLATFORM_SWITCH OR PLATFORM_IOS OR PLATFORM_TVOS OR USE_LIBROMFS)
    # Switch and iOS/tvOS always use libromfs (set by borealis toolchain.cmake).
    # Android does NOT — resources are APK assets via SDL.
    target_compile_definitions(${PROJECT_NAME} PRIVATE RESOURCE_PREFIX=\"romfs:/\")
else()
    target_compile_definitions(${PROJECT_NAME} PRIVATE RESOURCE_PREFIX=\"resources/\")
endif()

# ---------------------------------------------------------------------------
# Application version
#
# The CI passes the resolved version via -DAPP_VERSION=... (sourced from the
# VERSION file, or a manual override). When it isn't supplied — e.g. a local
# dev build — fall back to the VERSION file directly so the value still
# matches what ships. The result is injected as the VITA_PLEX_VERSION
# compile definition, which include/app/application.hpp now honours instead
# of hard-coding a literal. This keeps the Settings → Version cell AND the
# X-Plex-Version header / User-Agent on every API call in sync with the
# actual build, rather than a stale "2.0.0" baked into the header.
# ---------------------------------------------------------------------------
if(NOT DEFINED APP_VERSION OR APP_VERSION STREQUAL "")
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")
        file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" APP_VERSION)
        string(STRIP "${APP_VERSION}" APP_VERSION)
    else()
        set(APP_VERSION "0.0.0")
    endif()
endif()

# CI can also pass a human-readable display version separately. APP_VERSION
# is the numeric flavour ("1.0.2" / "1.0.2.455") that goes into Vita SFOs,
# PS4 pkgs, deb changelogs and the X-Plex-Version API header (which Plex
# expects to look numeric). APP_DISPLAY_VERSION is what the Settings >
# Version cell shows the user ("Beta 1.0.2"). When CI doesn't supply it
# we fall back to APP_VERSION so local dev builds still get something
# sensible.
if(NOT DEFINED APP_DISPLAY_VERSION OR APP_DISPLAY_VERSION STREQUAL "")
    set(APP_DISPLAY_VERSION "${APP_VERSION}")
endif()
message(STATUS "App version (numeric): ${APP_VERSION}")
message(STATUS "App version (display): ${APP_DISPLAY_VERSION}")
target_compile_definitions(${PROJECT_NAME} PRIVATE
    VITA_PLEX_VERSION=\"${APP_VERSION}\"
    VITA_PLEX_DISPLAY_VERSION=\"${APP_DISPLAY_VERSION}\")


# ---------------------------------------------------------------------------
# Link libraries
# ---------------------------------------------------------------------------
if(PLATFORM_ANDROID)
    # Android requires libmpv for in-app playback.
    set(APP_FFMPEG_LIBS mpv)
    set(APP_FFMPEG_LINK_BLOCK ${APP_FFMPEG_LIBS})
elseif(PLATFORM_IOS OR PLATFORM_TVOS)
    # iOS / tvOS: link libmpv + ffmpeg from prebuilt MPVKit xcframeworks.
    # The Apple platforms have no Homebrew-cross-compiled libmpv, so —
    # exactly like the Vita / Switch builds pull a prebuilt mpv tarball
    # from switchfin's portlibs — the CI job downloads MPVKit's release
    # xcframeworks and points us at the unzipped directory via:
    #   -DAPPLE_MPV_XCFRAMEWORKS_DIR=/path/with/*.xcframework
    # We glob every *.xcframework there and link them all (Libmpv plus
    # the seven Libav* ffmpeg frameworks). With the Xcode generator,
    # target_link_libraries on a .xcframework resolves the correct
    # platform/arch slice automatically.
    #
    # Headers: MPVKit's Libmpv.framework ships an mpv/ header dir, so
    # adding each framework slice's Headers to the search path makes the
    # existing  #include <mpv/client.h>  resolve unchanged. The CI also
    # passes -DLIBMPV_INCLUDE pointing at the extracted mpv headers as a
    # belt-and-braces fallback.
    #
    # Empty FFMPEG link block — the frameworks ARE the ffmpeg libs, so we
    # don't also want a bare -lmpv / -lavcodec in the main link list.
    set(APP_FFMPEG_LIBS "")
    set(APP_FFMPEG_LINK_BLOCK "")
    if(APPLE_MPV_XCFRAMEWORKS_DIR)
        file(GLOB _MPV_XCFRAMEWORKS "${APPLE_MPV_XCFRAMEWORKS_DIR}/*.xcframework")
        if(NOT _MPV_XCFRAMEWORKS)
            message(FATAL_ERROR
                "APPLE_MPV_XCFRAMEWORKS_DIR=${APPLE_MPV_XCFRAMEWORKS_DIR} "
                "contains no *.xcframework bundles")
        endif()
        # Link every xcframework — Xcode resolves the right slice based on
        # SDKROOT. For headers we only want ONE thing on the include path:
        # Libmpv's Headers/, so #include <mpv/client.h> works. Adding the
        # Libav* frameworks' Headers shadows system headers (libavutil
        # ships a time.h that hijacks our -include <time.h> workaround,
        # which made every TU explode with the libc++ tm error again).
        # None of our code includes ffmpeg headers directly anyway.
        foreach(_xcf ${_MPV_XCFRAMEWORKS})
            target_link_libraries(${PROJECT_NAME} "${_xcf}")
            get_filename_component(_xcf_name "${_xcf}" NAME_WE)
            if(_xcf_name STREQUAL "Libmpv" OR _xcf_name STREQUAL "Libmpv-GPL")
                # Pick the FIRST slice's Headers — headers are identical
                # across slices (mpv's public API doesn't vary by arch).
                file(GLOB _slices LIST_DIRECTORIES TRUE "${_xcf}/*")
                foreach(_slice ${_slices})
                    if(IS_DIRECTORY "${_slice}")
                        file(GLOB _fw "${_slice}/*.framework")
                        foreach(_f ${_fw})
                            if(EXISTS "${_f}/Headers")
                                target_include_directories(${PROJECT_NAME} PRIVATE "${_f}/Headers")
                                message(STATUS "Using mpv headers from: ${_f}/Headers")
                                break()
                            endif()
                        endforeach()
                        if(_fw)
                            break()
                        endif()
                    endif()
                endforeach()
            endif()
        endforeach()
        message(STATUS "Linked MPVKit xcframeworks: ${_MPV_XCFRAMEWORKS}")
    elseif(LIBMPV_XCFRAMEWORK)
        # Single combined xcframework fallback.
        target_include_directories(${PROJECT_NAME} PRIVATE
            "${LIBMPV_XCFRAMEWORK}/Headers"
            "${LIBMPV_XCFRAMEWORK}")
        target_link_libraries(${PROJECT_NAME} "${LIBMPV_XCFRAMEWORK}")
    elseif(LIBMPV_LIBRARY)
        target_include_directories(${PROJECT_NAME} PRIVATE "${LIBMPV_INCLUDE}")
        target_link_libraries(${PROJECT_NAME} "${LIBMPV_LIBRARY}")
    endif()
    if(LIBMPV_INCLUDE)
        target_include_directories(${PROJECT_NAME} PRIVATE "${LIBMPV_INCLUDE}")
    endif()
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
    elseif(PLATFORM_SWITCH)
        # switchfin's switch-ffmpeg is built WITH the mbedtls (rtmp/tls) and
        # libssh2 (sftp) protocols, so libavformat.a pulls in mbedtls_* /
        # psa_crypto_init / libssh2_* symbols that devkitPro's lighter ffmpeg
        # didn't. Put ssh2 + mbedtls inside the SAME --start-group as ffmpeg so
        # the one-pass static linker resolves the cross-references (ffmpeg ↔
        # libssh2 ↔ mbedtls); listing mbedtls only in APP_NETWORK_LIBS (before
        # ffmpeg) left those refs undefined.
        set(APP_FFMPEG_LINK_BLOCK
            -Wl,--start-group
            ${APP_FFMPEG_LIBS}
            ssh2 mbedtls mbedx509 mbedcrypto z
            -Wl,--end-group)
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
elseif(PLATFORM_IOS OR PLATFORM_TVOS)
    # iOS / tvOS: no standalone render libs. borealis renders through its
    # OWN bundled nanovg + stb_truetype (no system freetype), and every
    # mpv-side dependency (libass, freetype, harfbuzz, fribidi, zlib, …)
    # is statically baked into MPVKit's Libmpv.xcframework. Linking bare
    # -lass / -lfreetype here would just fail — those dylibs don't exist
    # in the iOS SDK. libz IS in the SDK, so keep it for anything that
    # needs zlib directly.
    set(APP_RENDER_LIBS
        z
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

if(NOT PLATFORM_PS4 AND NOT WIN32 AND NOT PLATFORM_ANDROID AND NOT PLATFORM_IOS AND NOT PLATFORM_TVOS)
    list(APPEND APP_RENDER_LIBS
        zstd
        harfbuzz
    )
endif()

# iOS / tvOS link a prebuilt static libcurl.a supplied via -DLIBCURL_LIBRARY
# (built from source in the CI step against the matrix's SDK). The iOS
# Simulator SDK ships /usr/lib/libcurl.tbd, but tvOS Simulator does NOT —
# tvOS has a tighter public-library allowlist. Building static and linking
# it directly keeps both targets on the same code path AND avoids relying
# on whatever curl version Apple happens to ship that year. Empty
# APP_NETWORK_LIBS here so the bare "-lcurl" / "-lmbedtls" entries don't
# land in the link line.
if(PLATFORM_IOS OR PLATFORM_TVOS)
    set(APP_NETWORK_LIBS "")
else()
    set(APP_NETWORK_LIBS
        curl
    )
endif()

# mbedtls is curl's TLS backend on the consoles / desktop Homebrew builds.
# iOS / tvOS use Secure Transport instead, so mbedtls isn't in the link
# list there. Switch is also excluded HERE on purpose: its ffmpeg ALSO needs
# mbedtls, so mbedtls is placed inside the ffmpeg --start-group (after curl)
# instead — listing it here too would let CMake de-duplicate the in-group copy
# and re-break the static link order (ffmpeg's mbedtls refs going undefined).
if(NOT WIN32 AND NOT PLATFORM_IOS AND NOT PLATFORM_TVOS AND NOT PLATFORM_SWITCH)
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

if(NOT PLATFORM_SWITCH AND NOT PLATFORM_PS4 AND NOT WIN32 AND NOT PLATFORM_ANDROID AND NOT PLATFORM_IOS AND NOT PLATFORM_TVOS)
    target_link_libraries(${PROJECT_NAME} mp3lame)
endif()

# Android needs the android library for AAssetManager (used by asset extraction).
# Also explicitly link GLESv3/EGL so mpv_player.cpp's GL FBO setup and mpv's
# OpenGL render backend resolve at link time even if SDL2 doesn't pull them in
# as a transitive dependency.
if(PLATFORM_ANDROID)
    target_link_libraries(${PROJECT_NAME} android log GLESv3 EGL)
endif()

if(PLATFORM_PSV)
    # Required for __wrap_pthread_create in src/utils/vita_stubs.c
    target_link_options(${PROJECT_NAME} PRIVATE "-Wl,--wrap=pthread_create")

    target_link_libraries(${PROJECT_NAME}
        archive
        zstd
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
    # Required for __wrap_pthread_create in src/utils/switch_stubs.c — floors
    # libmpv/ffmpeg thread stacks above libnx's 128 KB default so they stop
    # overflowing the moment playback starts.
    target_link_options(${PROJECT_NAME} PRIVATE "-Wl,--wrap=pthread_create")

    target_link_libraries(${PROJECT_NAME}
        nx
        dav1d
        postproc
        placebo
        jpeg
        uam
    )
elseif(PLATFORM_PS4)
    target_link_libraries(${PROJECT_NAME}
        SceNet
    )
elseif(PLATFORM_IOS OR PLATFORM_TVOS)
    # System frameworks that mpv + SDL2 + borealis need on iOS/tvOS.
    # Metal is added even though we render with GLES because libplacebo
    # (mpv) and modern SDL2 reach for it for surface presentation.
    set(_APPLE_MOBILE_FRAMEWORKS
        "-framework UIKit"
        "-framework Foundation"
        "-framework CoreFoundation"
        "-framework CoreGraphics"
        "-framework QuartzCore"
        "-framework CoreMedia"
        "-framework CoreVideo"
        "-framework CoreImage"
        "-framework AVFoundation"
        "-framework AudioToolbox"
        "-framework CoreAudio"             # MPVKit Package.swift linkedFramework
        "-framework VideoToolbox"          # mpv hwdec=auto-safe -> VideoToolbox
        "-framework CoreText"              # libass ass_coretext.c CTFont* APIs
        "-framework OpenGLES"
        "-framework Metal"                 # MoltenVK / libplacebo backend
        "-framework MetalKit"
        "-framework IOSurface"             # MoltenVK swapchain surfaces
        "-framework GameController"
        "-framework CoreServices"
        "-framework Security"
        "-framework CFNetwork"             # system libcurl transitive
        "-framework SystemConfiguration"
    )
    target_link_libraries(${PROJECT_NAME} ${_APPLE_MOBILE_FRAMEWORKS})
    # Apple SDKs ship these as top-level dylibs, not frameworks.
    # bz2/iconv/expat/resolv mirror MPVKit's Package.swift linkedLibrary
    # settings; xml2 is libbluray's dep (SDK ships libxml2.tbd).
    target_link_libraries(${PROJECT_NAME}
        "-lc++" "-liconv" "-lbz2" "-lexpat" "-lresolv" "-lxml2")
elseif(PLATFORM_DESKTOP)
    find_package(Threads REQUIRED)
    target_link_libraries(${PROJECT_NAME} Threads::Threads)
    if(APPLE)
        target_link_libraries(${PROJECT_NAME} "-framework CoreWLAN")
    elseif(WIN32)
        # Windows: System Media Transport Controls (media overlay + media keys)
        # via WinRT. MinGW's WinRT support varies, so feature-probe by actually
        # compiling + linking a snippet; only enable if it builds, else fall back
        # to the no-op so the build never breaks. On failure the probe's compiler
        # output is printed so the toolchain gap is diagnosable from the CI log.
        set(_smtc_probe "${CMAKE_BINARY_DIR}/smtc_probe.cpp")
        file(WRITE "${_smtc_probe}" [==[
#include <windows.h>
#include <roapi.h>
#include <wrl.h>
#include <wrl/wrappers/corewrappers.h>
#include <windows.media.h>
#include <systemmediatransportcontrolsinterop.h>
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Wrappers::HStringReference;
using namespace ABI::Windows::Media;
typedef ABI::Windows::Foundation::ITypedEventHandler<
    SystemMediaTransportControls*, SystemMediaTransportControlsButtonPressedEventArgs*> BD;
struct H : BD {
    LONG r = 1;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** p) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(BD)) { *p = static_cast<BD*>(this); return S_OK; }
        *p = 0; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)++r; }
    ULONG STDMETHODCALLTYPE Release() override { return (ULONG)--r; }
    HRESULT STDMETHODCALLTYPE Invoke(ISystemMediaTransportControls*,
        ISystemMediaTransportControlsButtonPressedEventArgs* a) override {
        SystemMediaTransportControlsButton b; a->get_Button(&b); return S_OK;
    }
};
int main() {
    ComPtr<ISystemMediaTransportControlsInterop> interop;
    RoGetActivationFactory(HStringReference(
        RuntimeClass_Windows_Media_SystemMediaTransportControls).Get(),
        IID_PPV_ARGS(&interop));
    ComPtr<ISystemMediaTransportControls> smtc;
    interop->GetForWindow((HWND)0, IID_PPV_ARGS(&smtc));
    smtc->put_IsEnabled(true);
    smtc->put_IsPlayEnabled(true);
    smtc->put_IsNextEnabled(1);
    smtc->put_PlaybackStatus(MediaPlaybackStatus_Playing);
    ComPtr<ISystemMediaTransportControlsDisplayUpdater> up;
    smtc->get_DisplayUpdater(&up);
    up->put_Type(MediaPlaybackType_Music);
    ComPtr<IMusicDisplayProperties> mp;
    up->get_MusicProperties(&mp);
    mp->put_Title(HStringReference(L"x").Get());
    up->Update();
    EventRegistrationToken tok;
    H* cb = new H();
    smtc->add_ButtonPressed(cb, &tok);
    cb->Release();
    return 0;
}
]==])
        try_compile(VITAPLEX_HAVE_SMTC "${CMAKE_BINARY_DIR}/smtc_probe_bin"
            SOURCES "${_smtc_probe}"
            LINK_LIBRARIES runtimeobject ole32
            CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON
            OUTPUT_VARIABLE _smtc_out)
        if(VITAPLEX_HAVE_SMTC)
            message(STATUS "SMTC media controls enabled")
            target_sources(${PROJECT_NAME} PRIVATE src/utils/now_playing_smtc.cpp)
            target_compile_definitions(${PROJECT_NAME} PRIVATE VITAPLEX_SMTC=1)
            target_link_libraries(${PROJECT_NAME} runtimeobject ole32 user32)

            # Optional cover-art + album-title extension. Needs the WinRT URI +
            # RandomAccessStreamReference + IMusicDisplayProperties2 APIs, which
            # vary on MinGW — probe separately so a gap here only drops the
            # artwork, never the base SMTC controls above.
            set(_smtc_thumb_probe "${CMAKE_BINARY_DIR}/smtc_thumb_probe.cpp")
            file(WRITE "${_smtc_thumb_probe}" [==[
#include <windows.h>
#include <roapi.h>
#include <wrl.h>
#include <wrl/wrappers/corewrappers.h>
#include <windows.foundation.h>
#include <windows.storage.streams.h>
#include <windows.media.h>
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Wrappers::HStringReference;
using namespace ABI::Windows::Foundation;
using namespace ABI::Windows::Storage::Streams;
using namespace ABI::Windows::Media;
int main() {
    ComPtr<IUriRuntimeClassFactory> uf;
    RoGetActivationFactory(HStringReference(RuntimeClass_Windows_Foundation_Uri).Get(), IID_PPV_ARGS(&uf));
    ComPtr<IUriRuntimeClass> uri;
    uf->CreateUri(HStringReference(L"http://x").Get(), &uri);
    ComPtr<IRandomAccessStreamReferenceStatics> rs;
    RoGetActivationFactory(HStringReference(RuntimeClass_Windows_Storage_Streams_RandomAccessStreamReference).Get(), IID_PPV_ARGS(&rs));
    ComPtr<IRandomAccessStreamReference> ref;
    rs->CreateFromUri(uri.Get(), &ref);
    ComPtr<ISystemMediaTransportControlsDisplayUpdater> up;
    up->put_Thumbnail(ref.Get());
    ComPtr<IMusicDisplayProperties2> m2;
    m2->put_AlbumTitle(HStringReference(L"x").Get());
    return 0;
}
]==])
            try_compile(VITAPLEX_HAVE_SMTC_THUMB "${CMAKE_BINARY_DIR}/smtc_thumb_probe_bin"
                SOURCES "${_smtc_thumb_probe}"
                LINK_LIBRARIES runtimeobject ole32
                CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON
                OUTPUT_VARIABLE _smtc_thumb_out)
            if(VITAPLEX_HAVE_SMTC_THUMB)
                message(STATUS "SMTC cover art + album title enabled")
                target_compile_definitions(${PROJECT_NAME} PRIVATE VITAPLEX_SMTC_THUMB=1)
            else()
                message(STATUS "SMTC cover art unavailable — title/artist only. Probe output:\n${_smtc_thumb_out}")
            endif()

            # Optional repeat + shuffle toggles. These live on the revision-2
            # interface (ISystemMediaTransportControls2) + its change-requested
            # events, which vary on MinGW — probe separately so a gap here only
            # drops repeat/shuffle, never the base SMTC controls.
            set(_smtc_modes_probe "${CMAKE_BINARY_DIR}/smtc_modes_probe.cpp")
            file(WRITE "${_smtc_modes_probe}" [==[
#include <windows.h>
#include <roapi.h>
#include <wrl.h>
#include <wrl/wrappers/corewrappers.h>
#include <windows.media.h>
using Microsoft::WRL::ComPtr;
using namespace ABI::Windows::Media;
typedef ABI::Windows::Foundation::ITypedEventHandler<
    SystemMediaTransportControls*, AutoRepeatModeChangeRequestedEventArgs*> RD;
typedef ABI::Windows::Foundation::ITypedEventHandler<
    SystemMediaTransportControls*, ShuffleEnabledChangeRequestedEventArgs*> SD;
int main() {
    ComPtr<ISystemMediaTransportControls2> smtc2;
    smtc2->put_AutoRepeatMode(MediaPlaybackAutoRepeatMode_List);
    smtc2->put_ShuffleEnabled(true);
    EventRegistrationToken t1, t2;
    smtc2->add_AutoRepeatModeChangeRequested((RD*)0, &t1);
    smtc2->add_ShuffleEnabledChangeRequested((SD*)0, &t2);
    ComPtr<IAutoRepeatModeChangeRequestedEventArgs> ra;
    MediaPlaybackAutoRepeatMode m;
    ra->get_RequestedAutoRepeatMode(&m);
    ComPtr<IShuffleEnabledChangeRequestedEventArgs> sa;
    boolean b;
    sa->get_RequestedShuffleEnabled(&b);
    return 0;
}
]==])
            try_compile(VITAPLEX_HAVE_SMTC_MODES "${CMAKE_BINARY_DIR}/smtc_modes_probe_bin"
                SOURCES "${_smtc_modes_probe}"
                LINK_LIBRARIES runtimeobject ole32
                CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON
                OUTPUT_VARIABLE _smtc_modes_out)
            if(VITAPLEX_HAVE_SMTC_MODES)
                message(STATUS "SMTC repeat + shuffle enabled")
                target_compile_definitions(${PROJECT_NAME} PRIVATE VITAPLEX_SMTC_MODES=1)
            else()
                message(STATUS "SMTC repeat/shuffle unavailable. Probe output:\n${_smtc_modes_out}")
            endif()
        else()
            message(STATUS "WinRT SMTC unavailable — disabled. Probe output:\n${_smtc_out}")
        endif()

        # Embed a Win32 version resource so the .exe identifies itself as
        # "VitaPlex" (FileDescription/ProductName). Without it an unpackaged exe
        # has no identity and Windows labels the media overlay "Unknown app";
        # with it, the overlay (and Task Manager / file properties) shows the
        # app name. Compiled by windres under MinGW via CMake's RC language.
        enable_language(RC)
        target_sources(${PROJECT_NAME} PRIVATE "${CMAKE_SOURCE_DIR}/app/platform/windows/vitaplex.rc")
    else()
        # Linux desktop: expose MPRIS media controls over D-Bus so the multimedia
        # keys and the GNOME/KDE media widget drive playback even when VitaPlex
        # isn't the focused window. Optional — falls back to a no-op if dbus-1
        # isn't present at configure time.
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(DBUS IMPORTED_TARGET dbus-1)
            if(DBUS_FOUND)
                message(STATUS "MPRIS media controls enabled (dbus-1 ${DBUS_VERSION})")
                target_sources(${PROJECT_NAME} PRIVATE src/utils/now_playing_mpris.cpp)
                target_compile_definitions(${PROJECT_NAME} PRIVATE VITAPLEX_MPRIS=1)
                target_link_libraries(${PROJECT_NAME} PkgConfig::DBUS)
            else()
                message(STATUS "dbus-1 not found — MPRIS media controls disabled")
            endif()
        endif()
    endif()
endif()

# ---------------------------------------------------------------------------
# Platform packaging / post-build
# ---------------------------------------------------------------------------
if(PLATFORM_PSV)
    vita_create_self(${PROJECT_NAME}.self ${PROJECT_NAME} UNSAFE NOASLR)

    # App icons, XML layouts and the PSV hint-button art are globbed at
    # configure time so a newly added file can never be silently missing
    # from the VPK again — the hand-maintained FILE list drifted and
    # shipped builds with icons the UI references but couldn't load
    # (check-circle.png / account.png in the sort and settings menus,
    # among others). The multi-MB per-platform controller sets in
    # resources/images/ deliberately stay out: Vita only consumes
    # images/PSV (~60KB), and icons + xml total ~450KB.
    file(GLOB_RECURSE VITA_PACKED_RESOURCES RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/*.png
        ${CMAKE_CURRENT_SOURCE_DIR}/resources/xml/*.xml
        ${CMAKE_CURRENT_SOURCE_DIR}/resources/images/PSV/*.png)
    set(VITA_RES_FILE_ARGS)
    foreach(res IN LISTS VITA_PACKED_RESOURCES)
        list(APPEND VITA_RES_FILE_ARGS FILE ${res} ${res})
    endforeach()

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

        ${VITA_RES_FILE_ARGS}

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
            "VPLX00002"
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
        # Copy app resources EXCEPT the per-platform icon directories.
        # The pacbrew pkg-tool generates a .gp4 XML project from the
        # bundled file list; the "Keyboard & Mouse" directory's '&' is
        # an unescaped XML entity reference there, which crashes the
        # tool with "An error occurred while parsing EntityName".
        # PS4 also doesn't need Switch / Vita / Steam Deck / Touch
        # icons, so this both unbreaks packaging AND slims the .pkg.
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/resources/xml      ${CMAKE_BINARY_DIR}/ps4/resources/xml
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/resources/i18n     ${CMAKE_BINARY_DIR}/ps4/resources/i18n
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/resources/icons    ${CMAKE_BINARY_DIR}/ps4/resources/icons
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/resources/font     ${CMAKE_BINARY_DIR}/ps4/resources/font
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/resources/material ${CMAKE_BINARY_DIR}/ps4/resources/material
        COMMAND "${CMAKE_COMMAND}" -E copy
            ${CMAKE_CURRENT_SOURCE_DIR}/resources/cacert.pem ${CMAKE_BINARY_DIR}/ps4/resources/cacert.pem
        # Only the icon sets the PS4 build actually consumes — the
        # native Ps4/ set (used by HintIcons::getResPath on __PS4__),
        # plus the cross-platform Generic and Flairs sets.
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/resources/images/Ps4
            ${CMAKE_BINARY_DIR}/ps4/resources/images/Ps4
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/resources/images/Generic
            ${CMAKE_BINARY_DIR}/ps4/resources/images/Generic
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/resources/images/Flairs
            ${CMAKE_BINARY_DIR}/ps4/resources/images/Flairs
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

elseif(PLATFORM_IOS OR PLATFORM_TVOS)
    # iOS / tvOS: turn the executable into a proper .app bundle.
    # Resources are embedded via borealis's libromfs (RESOURCE_PREFIX
    # is "romfs:/", same as Switch), so we don't need a separate
    # resources/ copy step — the libromfs generator vacuums them up.
    #
    # The bundle ships an Info.plist + launch storyboard + asset
    # catalog from app/platform/ios/, with substituted version /
    # bundle-id strings. Code-signing is left to the developer (Xcode
    # auto-sign or a manual identity); CI just produces the unsigned
    # .app, which can be re-signed for sideloading via AltStore or
    # ldid for TrollStore devices.
    set_target_properties(${PROJECT_NAME} PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME            "VitaPlex"
        MACOSX_BUNDLE_BUNDLE_VERSION         "${APP_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING   "${APP_DISPLAY_VERSION}"
        MACOSX_BUNDLE_GUI_IDENTIFIER         "com.VitaPlex.app"
        MACOSX_BUNDLE_INFO_PLIST             "${CMAKE_SOURCE_DIR}/app/platform/ios/Info.plist.in"
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "com.VitaPlex.app"
    )
    if(PLATFORM_IOS)
        set_target_properties(${PROJECT_NAME} PROPERTIES
            XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
            XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "14.0")
    elseif(PLATFORM_TVOS)
        set_target_properties(${PROJECT_NAME} PROPERTIES
            XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "3"
            XCODE_ATTRIBUTE_TVOS_DEPLOYMENT_TARGET "14.0")
    endif()

    # Splash storyboard + asset catalog (icons). Both live under
    # app/platform/ios/ — iphoneos/Splash.storyboard for iOS,
    # tvos/Splash.storyboard for tvOS.
    if(PLATFORM_IOS)
        target_sources(${PROJECT_NAME} PRIVATE
            ${CMAKE_SOURCE_DIR}/app/platform/ios/iphoneos/Splash.storyboard)
        set_source_files_properties(
            ${CMAKE_SOURCE_DIR}/app/platform/ios/iphoneos/Splash.storyboard
            PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    elseif(PLATFORM_TVOS)
        target_sources(${PROJECT_NAME} PRIVATE
            ${CMAKE_SOURCE_DIR}/app/platform/ios/tvos/Splash.storyboard)
        set_source_files_properties(
            ${CMAKE_SOURCE_DIR}/app/platform/ios/tvos/Splash.storyboard
            PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    endif()
    if(EXISTS "${CMAKE_SOURCE_DIR}/app/platform/ios/Images.xcassets")
        target_sources(${PROJECT_NAME} PRIVATE
            ${CMAKE_SOURCE_DIR}/app/platform/ios/Images.xcassets)
        set_source_files_properties(
            ${CMAKE_SOURCE_DIR}/app/platform/ios/Images.xcassets
            PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    endif()

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
