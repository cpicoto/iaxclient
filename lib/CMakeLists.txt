cmake_minimum_required(VERSION 3.10)
project(iaxclient C)

#
# Build flags
#
add_definitions(-DBUILDING_DLL           # export symbols in DLL
                -DLIBIAX                 # IAX interior defs
                -DCODEC_GSM              # enable the G.SM codec
                -DHAS_STRING_H           # some older platforms
                -DHAS_STDLIB_H)

if(APPLE)
  add_definitions(-DMACOSX)
endif()

option(ENABLE_SPEEX "Compile with Speex preprocessing & codec support" OFF)

#
# Include paths
#
include_directories(
  ${PROJECT_SOURCE_DIR}             # for your audio_openal.h, etc.
  ${PROJECT_SOURCE_DIR}/gsm/inc
  ${PROJECT_SOURCE_DIR}/libiax2/src
  ${PROJECT_SOURCE_DIR}/libspeex/include
  ${PROJECT_SOURCE_DIR}/portmixer/px_common
  /mingw64/include                  # so <AL/al.h> / <AL/alc.h> resolve
)

#
# Source files
#
set(IAXCLIENT_BASE_SOURCES
    audio_encode.c
    audio_file.c
    codec_alaw.c
    codec_gsm.c
    codec_ulaw.c
    iaxclient_lib.c
    audio_openal.c        # use OpenAL backend
    # audio_portaudio.c    # disabled
)

if(WIN32)
  list(APPEND IAXCLIENT_BASE_SOURCES winfuncs.c)
else()
  list(APPEND IAXCLIENT_BASE_SOURCES unixfuncs.c)
endif()

if(ENABLE_SPEEX)
  list(APPEND IAXCLIENT_BASE_SOURCES codec_speex.c)
endif()

file(GLOB GSM_SOURCES "${PROJECT_SOURCE_DIR}/gsm/src/*.c")

set(LIBIAX2_SOURCES
  libiax2/src/iax.c
  libiax2/src/iax2-parser.c
  libiax2/src/jitterbuf.c
  libiax2/src/md5.c
)

list(APPEND IAXCLIENT_BASE_SOURCES spandsp/plc.c)

#
# Build the DLL
#
add_library(iaxclient_lib SHARED
  ${IAXCLIENT_BASE_SOURCES}
  ${GSM_SOURCES}
  ${LIBIAX2_SOURCES}
)

find_package(Threads REQUIRED)

#
# Locate Speex libs if requested
#
if(ENABLE_SPEEX)
  find_library(SPEEX_LIBRARY    speex    HINTS /mingw64/lib)
  find_library(SPEEXDSP_LIBRARY speexdsp HINTS /mingw64/lib)
  if(NOT SPEEX_LIBRARY OR NOT SPEEXDSP_LIBRARY)
    message(FATAL_ERROR "ENABLE_SPEEX=ON but libspeex or libspeexdsp not found! Install the MSYS2 speex and speexdsp packages.")
  endif()
endif()

#
# Link libraries
#
if(WIN32)
  target_link_libraries(iaxclient_lib
    ${CMAKE_THREAD_LIBS_INIT}
    Ws2_32
    OpenAL32
    $<$<BOOL:${ENABLE_SPEEX}>:${SPEEXDSP_LIBRARY}>
    $<$<BOOL:${ENABLE_SPEEX}>:${SPEEX_LIBRARY}>
  )
else()
  target_link_libraries(iaxclient_lib
    ${CMAKE_THREAD_LIBS_INIT}
    openal
    $<$<BOOL:${ENABLE_SPEEX}>:speexdsp>
    $<$<BOOL:${ENABLE_SPEEX}>:speex>
  )
endif()
