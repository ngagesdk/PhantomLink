# Native part of the Android bridge: the shared protocol and bridge engine
# (the same sources as the ESP32 firmware and the PC bridge) plus the JNI
# wrapper. Built by the Android Gradle plugin; also builds on a host with
# a JDK for the host tests (android/tests/run_host_test.py).

cmake_minimum_required(VERSION 3.18)
project(phantomlink_android C)

set(CMAKE_C_STANDARD 11)
set(GC_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../../..)

add_library(gcbridge SHARED
    ${GC_ROOT}/protocol/src/gc_codec.c
    ${GC_ROOT}/protocol/src/gc_link.c
    ${GC_ROOT}/protocol/src/gc_msg.c
    ${GC_ROOT}/protocol/src/gc_ppp.c
    ${GC_ROOT}/protocol/src/gc_ip.c
    ${GC_ROOT}/protocol/src/gc_net.c
    ${GC_ROOT}/firmware/src/bridge_core.c
    jni_bridge.c)

target_include_directories(gcbridge PRIVATE ${GC_ROOT}/protocol/include ${GC_ROOT}/firmware/src)

if(NOT ANDROID)
    find_package(JNI REQUIRED)
    target_include_directories(gcbridge PRIVATE ${JNI_INCLUDE_DIRS})
endif()

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(gcbridge PRIVATE -Wall -Wextra)
endif()

if(ANDROID)
    # 16 KB page size support (required for new apps on Android 15+)
    target_link_options(gcbridge PRIVATE "-Wl,-z,max-page-size=16384")
endif()
