cmake_minimum_required(VERSION 2.8)

set(CMAKE_C_COMPILER "gcc")

project(protothread C)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -fPIC")
set(PROJECT_DESCRIPTION "A protothread library written in C.")

if (("${CMAKE_C_COMPILER_ID}" STREQUAL "GNU") AND (CMAKE_C_COMPILER_VERSION VERSION_GREATER 4.0))
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wextra")
endif()

# the following is until we learn how to reorder the gcc arguments to correctly link on Ubuntu
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--no-as-needed")

add_library(protothread-static STATIC
    protothread_sem.c
    protothread_lock.c
    )

add_library(protothread.o OBJECT
    protothread_sem.c
    protothread_lock.c
    )

add_library(protothread-shared SHARED
    $<TARGET_OBJECTS:protothread.o>
    )

add_executable(pttest
    protothread_sem.c
    protothread_lock.c
    protothread_test.c
    )

# CMake doesn't allow targets with the same name.  This renames them properly afterward.
SET_TARGET_PROPERTIES(protothread-static PROPERTIES OUTPUT_NAME protothread CLEAN_DIRECT_OUTPUT 1)
SET_TARGET_PROPERTIES(protothread-shared PROPERTIES OUTPUT_NAME protothread CLEAN_DIRECT_OUTPUT 1)

# set the subst vars that configure_file() will use
set(PROJECT_VERSION 1.0)

set(PKG_CONFIG_LIBS
    "-lprotothread"
)

configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/pkg-config.pc.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}.pc"
)

install (TARGETS pttest DESTINATION bin)
install (TARGETS protothread-static protothread-shared DESTINATION lib)
install (FILES protothread.h protothread_lock.h protothread_sem.h DESTINATION include)

# Install pkgconfig files to libdata on BSD, otherwise lib
if(CMAKE_SYSTEM_NAME MATCHES "BSD")
    set(PKG_CONFIG_INSTALL_DIR "libdata/pkgconfig")
else()
    set(PKG_CONFIG_INSTALL_DIR "lib/pkgconfig")
endif()
install (FILES "${PROJECT_BINARY_DIR}/${PROJECT_NAME}.pc" DESTINATION ${PKG_CONFIG_INSTALL_DIR})
