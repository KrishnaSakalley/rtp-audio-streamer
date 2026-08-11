cmake_minimum_required(VERSION 3.16)
project(rtp_audio_streamer CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

add_compile_options(-Wall -Wextra -Werror)

# ThreadSanitizer build for the Phase 6 GATE (PLAN.md §4): configure a
# separate build directory with -DRTP_ENABLE_TSAN=ON to instrument every
# target and exercise the threaded receiver for data races. Off by default
# since TSan and the normal optimized build are mutually exclusive.
option(RTP_ENABLE_TSAN "Build with ThreadSanitizer instrumentation" OFF)
if(RTP_ENABLE_TSAN)
  add_compile_options(-fsanitize=thread -g -O1)
  add_link_options(-fsanitize=thread)
endif()

find_package(Threads REQUIRED)

# Header-only interface target: every binary and test links against this to
# pick up include/rtp/* without repeating the include path everywhere.
add_library(rtp_core INTERFACE)
target_include_directories(rtp_core INTERFACE ${CMAKE_SOURCE_DIR}/include)

add_library(rtp_wav STATIC src/wav.cpp)
target_link_libraries(rtp_wav PUBLIC rtp_core)

add_library(rtp_net STATIC src/udp_socket.cpp)
target_link_libraries(rtp_net PUBLIC rtp_core)

add_library(rtp_packet STATIC src/rtp_packet.cpp)
target_link_libraries(rtp_packet PUBLIC rtp_core)

add_library(rtp_stats STATIC src/stats.cpp)
target_link_libraries(rtp_stats PUBLIC rtp_core)

add_library(rtp_codec STATIC src/codec_ulaw.cpp src/codec_adpcm.cpp)
target_link_libraries(rtp_codec PUBLIC rtp_core)

add_library(rtp_jitter STATIC src/jitter_buffer.cpp)
target_link_libraries(rtp_jitter PUBLIC rtp_core rtp_codec rtp_packet)

add_executable(rtp_sender src/sender.cpp)
target_link_libraries(rtp_sender PRIVATE rtp_wav rtp_net rtp_packet rtp_codec rtp_core)

add_executable(rtp_receiver src/receiver.cpp)
target_link_libraries(rtp_receiver PRIVATE rtp_wav rtp_net rtp_packet rtp_jitter rtp_core Threads::Threads)

add_executable(gen_test_tone tools/gen_test_tone.cpp)
target_link_libraries(gen_test_tone PRIVATE rtp_wav rtp_core)

add_executable(snr_harness tools/snr_harness.cpp)
target_link_libraries(snr_harness PRIVATE rtp_codec rtp_core)

add_executable(wav_snr tools/wav_snr.cpp)
target_link_libraries(wav_snr PRIVATE rtp_wav rtp_core)

add_executable(impair tools/impair.cpp)
target_link_libraries(impair PRIVATE rtp_net rtp_core Threads::Threads)

enable_testing()
add_subdirectory(tests)
