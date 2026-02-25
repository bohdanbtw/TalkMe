cmake_minimum_required(VERSION 3.15)
project(TalkMe LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)

# RNNoise: fetch from source (not in vcpkg for x64-windows)
include(FetchContent)
FetchContent_Declare(
  rnnoise
  GIT_REPOSITORY https://github.com/xiph/rnnoise.git
  GIT_TAG master
)
FetchContent_MakeAvailable(rnnoise)
# RNNoise doesn't define standard include directories in its CMake, so we force it:
target_include_directories(rnnoise PUBLIC ${rnnoise_SOURCE_DIR}/include)

# WebRTC APM: standalone CMake port (not in vcpkg on Windows)
FetchContent_Declare(
  webrtc_apm
  GIT_REPOSITORY https://github.com/shantanugoel/webrtc-audio-processing.git
  GIT_TAG master
)
# Disable tests/examples for the WebRTC build
set(WEBRTC_BUILD_TESTING OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(webrtc_apm)
# Ensure <modules/audio_processing/include/audio_processing.h> resolves
target_include_directories(webrtc_apm PUBLIC ${webrtc_apm_SOURCE_DIR})

# SpeexDSP: from vcpkg when using vcpkg toolchain; required for Speex noise suppression
find_package(speexdsp CONFIG QUIET)
if(NOT speexdsp_FOUND)
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(SPEEXDSP QUIET speexdsp)
    if(SPEEXDSP_FOUND)
      add_library(speexdsp::speexdsp INTERFACE IMPORTED)
      set_target_properties(speexdsp::speexdsp PROPERTIES
        INTERFACE_LINK_LIBRARIES "${SPEEXDSP_LINK_LIBRARIES}"
        INTERFACE_INCLUDE_DIRECTORIES "${SPEEXDSP_INCLUDE_DIRS}")
    endif()
  endif()
endif()

# Client executable (skeleton; add your source files as needed)
add_executable(TalkMe WIN32
  src/app/main.cpp
  # ... add other sources ...
)
target_include_directories(TalkMe PRIVATE src vendor)
target_link_libraries(TalkMe PRIVATE rnnoise webrtc_apm)
if(TARGET speexdsp::speexdsp)
  target_link_libraries(TalkMe PRIVATE speexdsp::speexdsp)
endif()
target_compile_definitions(TalkMe PRIVATE TALKME_USE_RNNOISE TALKME_USE_WEBRTC_APM)
