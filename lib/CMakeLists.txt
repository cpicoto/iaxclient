cmake_minimum_required(VERSION 3.10)
project(iaxclient C)

#
# Build flags
#
add_definitions(-DBUILDING_DLL           # export symbols in DLL
                -DLIBIAX                 # IAX interior defs
                -DCODEC_GSM              # enable the G.SM codec
                -DHAS_STRING_H           # some older platforms
                -DHAS_STDLIB_H
                #-DVERBOSE
		-DSPEEX_PREPROCESS_GET_AGC_LOUDNESS)

if(APPLE)
  add_definitions(-DMACOSX)
endif()

option(ENABLE_SPEEX "Compile with Speex preprocessing & codec support" ON)  # Changed from OFF to ON

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
    # audio_openal.c        # disabled
    audio_portaudio.c    # use PortAudio backend
    pa_ringbuffer.c      # Add PortAudio ring buffer source
    portmixer/px_win_wmme/px_win_wmme.c # Add PortMixer Windows MME source
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
  # For MSYS2, get paths for static Speex libraries
  if(DEFINED ENV{MINGW_PREFIX})
    set(SPEEX_PATH $ENV{MINGW_PREFIX})
  else()
    set(SPEEX_PATH "C:/msys64/mingw64")
  endif()

  # Convert to CMake paths
  file(TO_CMAKE_PATH "${SPEEX_PATH}/lib/libspeex.a" SPEEX_LIBRARY_PATH)
  file(TO_CMAKE_PATH "${SPEEX_PATH}/lib/libspeexdsp.a" SPEEXDSP_LIBRARY_PATH)

  # Set the library paths
  set(SPEEX_LIBRARY "${SPEEX_LIBRARY_PATH}" CACHE FILEPATH "Path to Speex static library")
  set(SPEEXDSP_LIBRARY "${SPEEXDSP_LIBRARY_PATH}" CACHE FILEPATH "Path to SpeexDSP static library")

  # Check if libraries exist
  if(NOT EXISTS "${SPEEX_LIBRARY}" OR NOT EXISTS "${SPEEXDSP_LIBRARY}")
    # Try alternative paths
    set(SPEEX_LIBRARY "C:/msys64/mingw64/lib/libspeex.a" CACHE FILEPATH "Path to Speex static library" FORCE)
    set(SPEEXDSP_LIBRARY "C:/msys64/mingw64/lib/libspeexdsp.a" CACHE FILEPATH "Path to SpeexDSP static library" FORCE)
    
    if(NOT EXISTS "${SPEEX_LIBRARY}" OR NOT EXISTS "${SPEEXDSP_LIBRARY}")
      message(FATAL_ERROR "Speex static libraries not found. Make sure libspeex-devel and libspeexdsp-devel packages are installed.")
    endif()
  endif()
  
  message(STATUS "Using Speex static library: ${SPEEX_LIBRARY}")
  message(STATUS "Using SpeexDSP static library: ${SPEEXDSP_LIBRARY}")
  
  # Add this line to define HAVE_SPEEX_DSP when Speex is enabled
  add_definitions(-DHAVE_SPEEX_DSP)
endif()

# Use native path with CMake
file(TO_CMAKE_PATH "${MINGW_PATH}/lib/libportaudio.a" PORTAUDIO_LIBRARY_PATH)

# Set the PortAudio library path
set(PORTAUDIO_LIBRARY "${PORTAUDIO_LIBRARY_PATH}" CACHE FILEPATH "Path to PortAudio static library")

# Add debugging output
message(STATUS "Looking for PortAudio static library at: ${PORTAUDIO_LIBRARY}")
if(EXISTS "${PORTAUDIO_LIBRARY}")
  message(STATUS "Found PortAudio static library!")
else()
  # Try alternative path formats
  message(STATUS "PortAudio library not found at ${PORTAUDIO_LIBRARY}, trying alternatives...")
  
  # Try with direct MSYS2 path
  set(PORTAUDIO_LIBRARY "C:/msys64/mingw64/lib/libportaudio.a" CACHE FILEPATH "Path to PortAudio static library" FORCE)
  
  if(EXISTS "${PORTAUDIO_LIBRARY}")
    message(STATUS "Found PortAudio static library at alternative path: ${PORTAUDIO_LIBRARY}")
  else()
    message(FATAL_ERROR "PortAudio static library not found. Verify the file exists and is accessible.")
  endif()
endif()

# Add additional libraries needed for static linking with PortAudio
if(WIN32)
  set(PLATFORM_LIBS ${PLATFORM_LIBS} 
      setupapi
      ole32
      uuid
      winmm
      ksuser
      avrt
      mfplat
      mfuuid
      wmcodecdspuuid
      strmiids     # DirectShow interfaces
      oleaut32     # OLE Automation
      comdlg32     # Common dialog
    )
    
    # Define these flags to properly link the COM GUIDs
    add_definitions(-DINITGUID -DPSAPI_VERSION=1)
endif()

# Don't add setupapi again as it's already included above

#
# Link libraries
#
if(WIN32)
  target_link_libraries(iaxclient_lib
    ${CMAKE_THREAD_LIBS_INIT}
    Ws2_32
    ${PORTAUDIO_LIBRARY}
    winmm                 # Add the Windows Multimedia library
    $<$<BOOL:${ENABLE_SPEEX}>:${SPEEXDSP_LIBRARY}>
    $<$<BOOL:${ENABLE_SPEEX}>:${SPEEX_LIBRARY}>
    ${PLATFORM_LIBS}
  )
else()
  target_link_libraries(iaxclient_lib
    ${CMAKE_THREAD_LIBS_INIT}
    ${PORTAUDIO_LIBRARY}  # Changed from openal
    $<$<BOOL:${ENABLE_SPEEX}>:speexdsp>
    $<$<BOOL:${ENABLE_SPEEX}>:speex>
  )
endif()
# Give the DLL the base name "iaxclient" instead of "iaxclient_lib"
set_target_properties(iaxclient_lib PROPERTIES
   OUTPUT_NAME "iaxclient"
 )