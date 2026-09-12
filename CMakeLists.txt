# nanobook — CMake build (equivalent to the Makefile; provided for IDEs and CI).
cmake_minimum_required(VERSION 3.20)
project(nanobook CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# The library is header-only: an INTERFACE target carries the include path and
# nothing needs compiling until a tool or test uses it.
add_library(nanobook INTERFACE)
target_include_directories(nanobook INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/cpp/include)
target_compile_options(nanobook INTERFACE
  -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wold-style-cast)

option(NANOBOOK_NATIVE "Tune for the host CPU (-march=native)" ON)
if(NANOBOOK_NATIVE AND CMAKE_BUILD_TYPE STREQUAL "Release")
  target_compile_options(nanobook INTERFACE -march=native)
endif()

foreach(tool gen_itch replay bench_structures)
  add_executable(${tool} cpp/tools/${tool}.cpp)
  target_link_libraries(${tool} PRIVATE nanobook)
endforeach()

file(GLOB NANOBOOK_TEST_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/cpp/tests/test_*.cpp)
add_executable(run_tests ${NANOBOOK_TEST_SRCS} cpp/tests/main.cpp)
target_link_libraries(run_tests PRIVATE nanobook)

enable_testing()
add_test(NAME unit COMMAND run_tests)
