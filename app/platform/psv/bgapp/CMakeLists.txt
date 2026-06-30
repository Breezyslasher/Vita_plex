# VitaPlex PS Vita background helper (eboot2.bin) — built with DolceSDK ONLY.
#
# This is a separate, tiny "system mode" background application that the main
# (vitasdk-built) VitaPlex VPK bundles and launches via sceBgAppUtilStartBgApp().
# It MUST be built with DolceSDK rather than vitasdk because it needs boot
# params (attribute 0x03 = background app) + a CATEGORY=gdd SFO, which vitasdk's
# packaging can't emit. Output: eboot2.bin + param2.sfo, consumed by the main
# CMakeLists' vita_create_vpk via downloaded CI artifacts.
#
# Modelled on GrapheneCt/BG-App-PSV (the canonical main-app + background-app
# sample); MIT-spirited reference, our own code.

cmake_minimum_required(VERSION 3.0)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{DOLCESDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{DOLCESDK}/share/dolce.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define DOLCESDK to point to your SDK path!")
  endif()
endif()

project(vitaplex_bgapp C)

include("$ENV{DOLCESDK}/share/dolce.cmake" REQUIRED)

# attribute 0x03 in the boot param marks this self as a background application.
add_custom_command(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/boot_param2.bin
  COMMAND dolce-make-bootparam app_memsize 0x1000 attribute 0x03 ${CMAKE_CURRENT_BINARY_DIR}/boot_param2.bin
)
add_custom_target(bgapp_bootparam DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/boot_param2.bin)

# SceNotificationUtil isn't in DolceSDK's stub set — generate it from the NIDs.
dolce_gen_libs(gen_notif_stubs
  SceNotificationUtil.yml
  LIB SceNotificationUtil_stub_weak)

add_executable(${PROJECT_NAME} bgapp.c)
set_target_properties(${PROJECT_NAME} PROPERTIES COMPILE_FLAGS "-Wall")
add_dependencies(${PROJECT_NAME} bgapp_bootparam)
target_link_libraries(${PROJECT_NAME}
  SceLibKernel_stub
  SceIofilemgr_stub
  SceSysmodule_stub
  SceAppMgr_stub
  SceProcessmgr_stub
  SceNotificationUtil_stub_weak
)

dolce_create_self(eboot2.bin
  ${PROJECT_NAME}
  UNSAFE
  BOOT_PARAM ${CMAKE_CURRENT_BINARY_DIR}/boot_param2.bin
)

# param2.sfo: CATEGORY=gdd marks a background application; it installs into the
# main app's directory (VPLEX0001) and carries its own title id (VPLEX0002).
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/param2.sfo
  COMMAND dolce-mksfoex
    -s APP_VER=01.00
    -d ATTRIBUTE=17305600
    -s CATEGORY=gdd
    -s INSTALL_DIR_ADDCONT=VPLEX0001
    -s INSTALL_DIR_SAVEDATA=VPLEX0001
    -s TITLE_ID=VPLEX0002
    -s STITLE=VitaPlexBGM
    -s TITLE=VitaPlexBGM
    ${PROJECT_NAME} ${CMAKE_CURRENT_BINARY_DIR}/param2.sfo
)
add_custom_target(bgapp_pkg ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/param2.sfo eboot2.bin)
