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
  src/audio/wav_reader.cpp
  src/audio/wav_writer.cpp
  src/benchmark/offline_benchmark.cpp
  src/benchmark/streaming_benchmark.cpp
  src/dsp/causal_onset.cpp
  src/dsp/spectral_flux.cpp
  src/dsp/streaming_processor.cpp
  src/evaluation/onset_evaluation.cpp
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

add_executable(hero-audio-eval src/evaluate_main.cpp)
target_link_libraries(hero-audio-eval PRIVATE HEROAudio::core)

add_executable(hero-audio-bench src/benchmark_main.cpp)
target_link_libraries(hero-audio-bench PRIVATE HEROAudio::core)

add_executable(hero-audio-stream src/streaming_main.cpp)
target_link_libraries(hero-audio-stream PRIVATE HEROAudio::core)

if(APPLE)
  find_library(HERO_AUDIO_COREAUDIO_FRAMEWORK CoreAudio REQUIRED)
  find_library(HERO_AUDIO_AUDIOUNIT_FRAMEWORK AudioUnit REQUIRED)
  find_library(HERO_AUDIO_COREFOUNDATION_FRAMEWORK CoreFoundation REQUIRED)

  add_library(hero_audio_coreaudio src/audio/coreaudio_input.cpp)
  add_library(HEROAudio::coreaudio ALIAS hero_audio_coreaudio)
  target_link_libraries(hero_audio_coreaudio
    PUBLIC HEROAudio::core
    PRIVATE
      ${HERO_AUDIO_COREAUDIO_FRAMEWORK}
      ${HERO_AUDIO_AUDIOUNIT_FRAMEWORK}
      ${HERO_AUDIO_COREFOUNDATION_FRAMEWORK}
  )
  target_compile_options(hero_audio_coreaudio PRIVATE
    $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall;-Wextra;-Wpedantic;-Wconversion>
  )

  add_executable(hero-audio-live src/live_main.cpp)
  target_link_libraries(hero-audio-live PRIVATE HEROAudio::coreaudio)
endif()

if(HERO_AUDIO_BUILD_TESTS)
  include(CTest)
  add_executable(hero_audio_tests tests/test_fft.cpp)
  target_link_libraries(hero_audio_tests PRIVATE HEROAudio::core)
  add_test(NAME fft_correctness COMMAND hero_audio_tests)

  add_executable(wav_reader_tests tests/test_wav_reader.cpp)
  target_link_libraries(wav_reader_tests PRIVATE HEROAudio::core)
  add_test(NAME wav_reader_correctness COMMAND wav_reader_tests)

  add_executable(spectral_flux_tests tests/test_spectral_flux.cpp)
  target_link_libraries(spectral_flux_tests PRIVATE HEROAudio::core)
  add_test(NAME spectral_flux_correctness COMMAND spectral_flux_tests)

  add_executable(causal_onset_tests tests/test_causal_onset.cpp)
  target_link_libraries(causal_onset_tests PRIVATE HEROAudio::core)
  add_test(NAME causal_onset_correctness COMMAND causal_onset_tests)

  add_executable(onset_evaluation_tests tests/test_onset_evaluation.cpp)
  target_link_libraries(onset_evaluation_tests PRIVATE HEROAudio::core)
  add_test(NAME onset_evaluation_correctness COMMAND onset_evaluation_tests)

  add_executable(offline_benchmark_tests tests/test_offline_benchmark.cpp)
  target_link_libraries(offline_benchmark_tests PRIVATE HEROAudio::core)
  add_test(NAME offline_benchmark_correctness COMMAND offline_benchmark_tests)

  add_executable(streaming_processor_tests tests/test_streaming_processor.cpp)
  target_link_libraries(streaming_processor_tests PRIVATE HEROAudio::core)
  add_test(NAME streaming_processor_correctness COMMAND streaming_processor_tests)

  add_executable(live_support_tests tests/test_live_support.cpp)
  target_link_libraries(live_support_tests PRIVATE HEROAudio::core)
  add_test(NAME live_support_correctness COMMAND live_support_tests)

  add_test(
    NAME onset_evaluation_cli
    COMMAND hero-audio-eval
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/evaluation_predictions.csv
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/evaluation_references.csv
      --tolerance-ms 50
  )
  set_tests_properties(onset_evaluation_cli PROPERTIES
    PASS_REGULAR_EXPRESSION "f1: 0.800000"
  )

  if(APPLE)
    add_test(NAME live_cli_help COMMAND hero-audio-live --help)
    set_tests_properties(live_cli_help PROPERTIES
      PASS_REGULAR_EXPRESSION "Creates capture.wav"
    )
  endif()
endif()
