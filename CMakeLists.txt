cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Define la variable de entorno VITASDK (p. ej. C:/vitasdk)")
  endif()
endif()

project(VitaTwitch C)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "Twitch")
set(VITA_TITLEID  "VTWI00001")
set(VITA_VERSION  "01.33")

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -O2 -std=gnu11")

add_executable(${PROJECT_NAME}
  src/main.c
  src/http.c
  src/tls_trust.c
  src/twitch.c
  src/m3u8.c
  src/hls.c
  src/ts_demux.c
  src/pktqueue.c
  src/player.c
  src/playback_policy.c
  src/json.c
  src/auth.c
  src/task.c
  src/images.c
  src/ui.c
  src/log.c
  src/text.c
  src/chat.c
  src/chat_view.c
  src/i18n.c
  src/ime_utf8.c
  src/mp4_demux.c
)

# Generate the immutable Mozilla trust bundle at configure time. A developer
# build without the bundle can still read anonymously, but cannot send OAuth.
set(VT_CA_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/vt_ca_bundle.h")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(INPUT "${CMAKE_CURRENT_SOURCE_DIR}/assets/cacert.pem")
set(OUTPUT "${VT_CA_HEADER}")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_ca.cmake")
target_include_directories(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${INPUT}")

target_link_libraries(${PROJECT_NAME}
  vita2d
  png16 jpeg
  curl ssl crypto z zstd
  pthread m
  ScePgf_stub
  SceGxm_stub
  SceDisplay_stub
  SceCtrl_stub
  SceTouch_stub
  SceCommonDialog_stub
  SceAppUtil_stub
  SceAppMgr_stub
  SceSysmodule_stub
  SceVideodec_stub
  SceAudiodec_stub
  SceAudio_stub
  ScePower_stub
  SceRtc_stub
  SceNet_stub
  SceNetCtl_stub
  SceHttp_stub
  SceSsl_stub
  SceSysmem_stub
  SceKernelThreadMgr_stub
  SceKernelModulemgr_stub
  SceIofilemgr_stub
  SceProcessmgr_stub
  SceLibKernel_stub
)

vita_create_self(${PROJECT_NAME}.self ${PROJECT_NAME})
# The authenticated IRC connection MUST verify its server TLS chain. An
# explicitly supplied Mozilla CA bundle is packaged as app0:ca/cacert.pem.
# Without the file, anonymous read-only chat still works; sending is disabled.
set(VT_CERT_ASSETS "")
if(EXISTS "${CMAKE_SOURCE_DIR}/assets/cacert.pem")
  set(VT_CERT_ASSETS assets/cacert.pem ca/cacert.pem)
endif()
vita_create_vpk(${PROJECT_NAME}.vpk ${VITA_TITLEID} ${PROJECT_NAME}.self
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
  FILE sce_sys/icon0.png sce_sys/icon0.png
  FILE sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
  FILE sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
  FILE sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
  ${VT_CERT_ASSETS}
)

# vita-elf-create appends ~4 KB of SCE module data right after the text
# segment. When the code ended just below a 64 KB boundary that data overlapped
# the data segment and the tool crashed (0xc0000374). VitaSDK's linker script
# reserves this headroom before the data segment when __sce_headroom is defined.
target_link_options(${PROJECT_NAME} PRIVATE "-Wl,--defsym,__sce_headroom=0x4000")
