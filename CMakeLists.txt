cmake_minimum_required(VERSION 3.25)

project(HEROAudio VERSION 0.1.0 LANGUAGES CXX)

option(HERO_AUDIO_ENABLE_FFTW "Build the official FFTW3f CPU backend" ON)
option(HERO_AUDIO_REQUIRE_FFTW "Fail configuration when FFTW3f is unavailable" OFF)
option(HERO_AUDIO_BUILD_TESTS "Build correctness tests" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

add_library(hero_audio_core
  src/fft/reference_backend.cpp
  src/fft/backend_factory.cpp
)
add_library(HEROAudio::core ALIAS hero_audio_core)

target_include_directories(hero_audio_core
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_compile_options(hero_audio_core PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall;-Wextra;-Wpedantic;-Wconversion>
)

if(HERO_AUDIO_ENABLE_FFTW)
  find_package(FFTW3f QUIET)
  if(FFTW3f_FOUND)
    target_sources(hero_audio_core PRIVATE src/fft/fftw_backend.cpp)
    target_link_libraries(hero_audio_core PRIVATE FFTW3f::fftw3f)
    target_compile_definitions(hero_audio_core PUBLIC HERO_AUDIO_HAS_FFTW3F=1)
    message(STATUS "HERO-Audio: FFTW3f backend enabled")
  elseif(HERO_AUDIO_REQUIRE_FFTW)
    message(FATAL_ERROR "FFTW3f is required. On macOS run: brew bundle")
  else()
    message(WARNING "FFTW3f not found; only the correctness-reference backend will be built")
  endif()
endif()

add_executable(hero-audio src/main.cpp)
target_link_libraries(hero-audio PRIVATE HEROAudio::core)

if(HERO_AUDIO_BUILD_TESTS)
  include(CTest)
  add_executable(hero_audio_tests tests/test_fft.cpp)
  target_link_libraries(hero_audio_tests PRIVATE HEROAudio::core)
  add_test(NAME fft_correctness COMMAND hero_audio_tests)
endif()
