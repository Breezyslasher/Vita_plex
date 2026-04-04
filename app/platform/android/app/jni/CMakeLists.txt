cmake_minimum_required(VERSION 3.6)

project(jni)

set(APP_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../..")

if (NOT EXISTS "${APP_SOURCE_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR "APP_SOURCE_ROOT does not contain CMakeLists.txt: ${APP_SOURCE_ROOT}")
endif()

# armeabi-v7a requires cpufeatures library
# include(AndroidNdkModules)
# android_ndk_import_module_cpufeatures()

set(MPV_PREFIX "${APP_SOURCE_ROOT}/jniLibs/${ANDROID_ABI}")
set(ENV{PKG_CONFIG_SYSROOT_DIR} ${MPV_PREFIX})
set(ENV{PKG_CONFIG_LIBDIR} ${MPV_PREFIX}/lib/pkgconfig)
set(CMAKE_PREFIX_PATH ${MPV_PREFIX})
list(PREPEND CMAKE_FIND_ROOT_PATH ${MPV_PREFIX})
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)

link_directories(${MPV_PREFIX}/lib)
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -L${MPV_PREFIX}/lib")

set(APP_PLATFORM_LIB
    ${MPV_PREFIX}/lib/libmpv.so
    ${MPV_PREFIX}/lib/libavcodec.so
    ${MPV_PREFIX}/lib/libavdevice.so
    ${MPV_PREFIX}/lib/libavfilter.so
    ${MPV_PREFIX}/lib/libavformat.so
    ${MPV_PREFIX}/lib/libswresample.so
    ${MPV_PREFIX}/lib/libswscale.so
    ${MPV_PREFIX}/lib/libavutil.so
)

# FIX: Do NOT use libromfs for Android.
#
# Resources are packaged into the APK's assets/ folder by Gradle (see
# build.gradle syncBorealisResources task) and extracted to internal storage
# at app startup (see src/platform/android_assets.cpp).  After extraction,
# borealis loads everything via standard fopen("resources/...").
#
# The main CMakeLists.txt overrides USE_LIBROMFS back to OFF after the
# borealis toolchain include, and sets BRLS_PLATFORM_RESOURCES_PATH for
# Android.  The variables below just satisfy CMake-time checks in the
# borealis build scripts; they have no effect at runtime.
set(LIBROMFS_PROJECT_NAME "VitaSuwayomi" CACHE STRING "" FORCE)
set(LIBROMFS_RESOURCE_LOCATION "${APP_SOURCE_ROOT}/resources" CACHE PATH "" FORCE)

# app option
set(PLATFORM_ANDROID ON CACHE BOOL "")
set(USE_SYSTEM_CURL ON CACHE BOOL "")
set(USE_SYSTEM_SDL2 ON CACHE BOOL "")
set(USE_GLES3 ON CACHE BOOL "")
set(SIMPLE_HIGHLIGHT ON CACHE BOOL "")
# USE_LIBROMFS is forced OFF by the main CMakeLists.txt after toolchain.cmake runs.
# RESOURCE_PREFIX="resources/" — files are extracted from APK assets to internal
# storage at startup, so fopen("resources/...") resolves normally.

# https://github.com/curl/curl/issues/12086
# if (ANDROID_ABI STREQUAL armeabi-v7a)
#     set(HAVE_FSEEKO 0)
# endif()
add_subdirectory("${APP_SOURCE_ROOT}" "${CMAKE_BINARY_DIR}/vitasuwayomi")
