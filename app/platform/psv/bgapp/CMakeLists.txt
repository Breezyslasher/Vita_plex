# VitaPlex PS Vita background helper (eboot2.bin) — built with VITASDK.
#
# A separate, tiny "background application" self that the main VitaPlex VPK
# bundles and launches via sceBgAppUtilStartBgApp(). It is marked as a
# background app entirely through its self boot param: vita_create_self's
# ATTRIBUTE 0x03 becomes the "ATTRIBUTE word in Control Info section 6" and
# MEMSIZE the system app memory budget (KiB) — exactly the boot param a Vita
# background app needs. vitasdk's vita-make-fself writes both (-at / -m), so
# no DolceSDK is required; this builds with the same toolchain as the main app.

cmake_minimum_required(VERSION 3.10)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
  endif()
endif()

project(vitaplex_bgapp C)

include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

add_executable(${PROJECT_NAME} bgapp.c)
target_link_libraries(${PROJECT_NAME}
  SceLibKernel_stub
  SceIofilemgr_stub
  SceSysmodule_stub
  SceProcessmgr_stub
  ScePower_stub
  SceNotificationUtil_stub
  SceAudio_stub          # sceAudioOut* — audio output
  m                      # sinf (tone) + minimp3 float math
)

# ATTRIBUTE 0x03 = background-app control-info attribute; MEMSIZE = KiB budget.
vita_create_self(eboot2.bin ${PROJECT_NAME} UNSAFE ATTRIBUTE 0x03 MEMSIZE 4096)
add_custom_target(bgapp_self ALL DEPENDS eboot2.bin)
