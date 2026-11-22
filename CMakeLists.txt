cmake_minimum_required(VERSION 3.20)
project(dromos LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

option(DROMOS_SANITIZE "Build with ASan and UBSan" OFF)
option(DROMOS_VERILATOR "Build Verilator testbenches (needs verilator)" OFF)

add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)
if(DROMOS_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

# ---- C++ reference engine (M1) -------------------------------------------
add_library(dromos_ref
  sw/src/refengine.cpp
  sw/src/naive_engine.cpp
  sw/src/orderflow.cpp)
target_include_directories(dromos_ref PUBLIC sw/include)

add_executable(test_engine sw/tests/test_engine.cpp)
target_link_libraries(test_engine PRIVATE dromos_ref)

add_executable(crosscheck sw/tools/crosscheck.cpp)
target_link_libraries(crosscheck PRIVATE dromos_ref)

add_executable(bench sw/tools/bench.cpp)
target_link_libraries(bench PRIVATE dromos_ref)

enable_testing()
add_test(NAME directed COMMAND test_engine)
add_test(NAME crosscheck_quick COMMAND crosscheck --seeds 8 --msgs 100000 --quiet 1)
add_test(NAME crosscheck_small_pool COMMAND crosscheck --seeds 4 --msgs 50000 --pool 64 --quiet 1)

# ---- Verilator testbenches (M2) --------------------------------------------
if(DROMOS_VERILATOR)
  add_subdirectory(sim/verilator)
endif()
