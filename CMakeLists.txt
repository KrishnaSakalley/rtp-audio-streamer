cmake_minimum_required(VERSION 3.16)
project(rtp_audio_streamer CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

add_compile_options(-Wall -Wextra -Werror)

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

add_executable(rtp_sender src/sender.cpp)
target_link_libraries(rtp_sender PRIVATE rtp_wav rtp_net rtp_packet rtp_core)

add_executable(rtp_receiver src/receiver.cpp)
target_link_libraries(rtp_receiver PRIVATE rtp_wav rtp_net rtp_packet rtp_stats rtp_core)

add_executable(gen_test_tone tools/gen_test_tone.cpp)
target_link_libraries(gen_test_tone PRIVATE rtp_wav rtp_core)

enable_testing()
add_subdirectory(tests)
