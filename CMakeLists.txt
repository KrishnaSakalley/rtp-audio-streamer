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

enable_testing()
add_subdirectory(tests)
