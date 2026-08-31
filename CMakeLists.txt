# https://github.com/LarryRuane/protothread
# Copyright (c) 2008-present Larry Ruane
# Distributed under the MIT software license, see the accompanying
# file LICENSE or https://opensource.org/licenses/MIT.
# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.10...3.31)

set(CMAKE_C_COMPILER "gcc")

# the version lives in protothread.h; read it so the two cannot drift
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/protothread.h" _pt_header)
foreach(_part MAJOR MINOR PATCH)
    string(REGEX MATCH "#define PT_VERSION_${_part} +([0-9]+)" _ "${_pt_header}")
    set(_pt_${_part} ${CMAKE_MATCH_1})
endforeach()
unset(_pt_header)

project(protothread VERSION ${_pt_MAJOR}.${_pt_MINOR}.${_pt_PATCH} LANGUAGES C)
message(STATUS "protothread version ${PROJECT_VERSION}")

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -fPIC")
set(PROJECT_DESCRIPTION "A protothread library written in C.")

if (("${CMAKE_C_COMPILER_ID}" STREQUAL "GNU") AND (CMAKE_C_COMPILER_VERSION VERSION_GREATER 4.0))
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wextra")
endif()

# Protothread is header-only: there is nothing to compile or link against,
# just headers to include. PT_DEBUG changes the layout of pt_thread_t and
# pt_func_t, so a prebuilt library could silently disagree with the
# application that includes these headers; header-only makes that impossible.
add_library(protothread INTERFACE)
target_include_directories(protothread INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:include>)

add_executable(pttest
    protothread_test.c
    )
target_link_libraries(pttest PRIVATE protothread)

# The benchmark compares against POSIX threads, so unlike the library it
# needs pthreads and clock_gettime. Off by default: it must not be a
# dependency of an ordinary build, least of all a cross-compiled one.
#   cmake -S . -B build -DPROTOTHREAD_BENCH=ON && ./build/ptbench
option(PROTOTHREAD_BENCH "Build the pthread comparison benchmark" OFF)
if(PROTOTHREAD_BENCH)
    find_package(Threads REQUIRED)
    add_executable(ptbench protothread_bench.c)
    target_link_libraries(ptbench PRIVATE protothread Threads::Threads)
    # benchmark what you would ship: no debug bookkeeping, optimized
    target_compile_definitions(ptbench PRIVATE PT_DEBUG=0)
    target_compile_options(ptbench PRIVATE -O2)
endif()

# The thread-pool example needs POSIX threads too; also off by default.
#   cmake -S . -B build -DPROTOTHREAD_EXAMPLES=ON && ./build/ptpool
option(PROTOTHREAD_EXAMPLES "Build the POSIX thread-pool example" OFF)
if(PROTOTHREAD_EXAMPLES)
    find_package(Threads REQUIRED)
    add_executable(ptpool protothread_pool_example.c)
    target_link_libraries(ptpool PRIVATE protothread Threads::Threads)
endif()

configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/pkg-config.pc.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}.pc"
  @ONLY
)

install (TARGETS pttest DESTINATION bin)
install (FILES protothread.h protothread_lock.h protothread_sem.h protothread_timer.h
         DESTINATION include)

# Install pkgconfig files to libdata on BSD, otherwise lib
if(CMAKE_SYSTEM_NAME MATCHES "BSD")
    set(PKG_CONFIG_INSTALL_DIR "libdata/pkgconfig")
else()
    set(PKG_CONFIG_INSTALL_DIR "lib/pkgconfig")
endif()
install (FILES "${PROJECT_BINARY_DIR}/${PROJECT_NAME}.pc" DESTINATION ${PKG_CONFIG_INSTALL_DIR})
