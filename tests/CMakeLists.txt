# Host build of everything that can run on a PC: protocol library,
# server, bridge core, N-Gage engine core and all automated tests.
#
#   cmake -S tests -B build/tests && cmake --build build/tests && ctest --test-dir build/tests
#
# Option GC_SANITIZE=ON builds with AddressSanitizer + UBSan (GCC/Clang).

cmake_minimum_required(VERSION 3.10)
project(gamecomms_tests C)
set(CMAKE_C_STANDARD 99)
enable_testing()

option(GC_SANITIZE "Build with ASan/UBSan" OFF)
set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -g)
    if(GC_SANITIZE)
        add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address,undefined)
    endif()
endif()
if(NOT WIN32)
    add_compile_definitions(_POSIX_C_SOURCE=200809L)
endif()

add_library(gc_protocol STATIC ${ROOT}/protocol/src/gc_codec.c ${ROOT}/protocol/src/gc_link.c ${ROOT}/protocol/src/gc_msg.c
    ${ROOT}/protocol/src/gc_arena.c ${ROOT}/protocol/src/gc_ppp.c ${ROOT}/protocol/src/gc_ip.c ${ROOT}/protocol/src/gc_net.c)
target_include_directories(gc_protocol PUBLIC ${ROOT}/protocol/include)

add_library(gc_server_core STATIC ${ROOT}/server/src/gcs_core.c ${ROOT}/server/src/gcs_arena.c
    ${ROOT}/server/src/gcs_native.c ${ROOT}/server/src/gcs_accounts.c
    ${ROOT}/server/src/gcs_snap.c ${ROOT}/server/src/gcs_rifts.c ${ROOT}/server/src/gcs_rcodec.c ${ROOT}/server/src/gcs_direct.c)
target_include_directories(gc_server_core PUBLIC ${ROOT}/server/src)
target_link_libraries(gc_server_core PUBLIC gc_protocol)

# PhantomLink Arena plug-ins: portable core against the server core
add_library(gc_arena_core STATIC ${ROOT}/dll/arena/core/pla_core.c)
target_include_directories(gc_arena_core PUBLIC ${ROOT}/dll/arena/core)
target_link_libraries(gc_arena_core PUBLIC gc_protocol)
add_executable(test_pla arena/test_pla.c)
target_link_libraries(test_pla gc_arena_core gc_server_core)
add_test(NAME arena_core COMMAND test_pla)
add_executable(test_accounts server/test_accounts.c)
target_link_libraries(test_accounts gc_server_core)
target_include_directories(test_accounts PRIVATE ${ROOT}/protocol/tests)
add_test(NAME server_accounts COMMAND test_accounts)
add_executable(test_rcodec server/test_rcodec.c)
target_link_libraries(test_rcodec gc_server_core)
target_include_directories(test_rcodec PRIVATE ${ROOT}/protocol/tests)
add_test(NAME server_rifts_codec COMMAND test_rcodec)
add_executable(test_rifts server/test_rifts.c)
target_link_libraries(test_rifts gc_server_core)
target_include_directories(test_rifts PRIVATE ${ROOT}/protocol/tests)
add_test(NAME server_rifts COMMAND test_rifts)
add_executable(test_direct server/test_direct.c)
target_link_libraries(test_direct gc_server_core)
target_include_directories(test_direct PRIVATE ${ROOT}/protocol/tests)
add_test(NAME server_direct COMMAND test_direct)

add_library(gc_bridge_core STATIC ${ROOT}/firmware/src/bridge_core.c)
target_include_directories(gc_bridge_core PUBLIC ${ROOT}/firmware/src)
target_link_libraries(gc_bridge_core PUBLIC gc_protocol)

add_library(gc_dev_core STATIC ${ROOT}/dll/core/gc_dev.c)
target_include_directories(gc_dev_core PUBLIC ${ROOT}/dll/core)
target_link_libraries(gc_dev_core PUBLIC gc_protocol)

# the core sources must stay plain C89 (N-Gage GCC 2.9 / C++98 toolchain)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    set_source_files_properties(
        ${ROOT}/protocol/src/gc_codec.c ${ROOT}/protocol/src/gc_link.c ${ROOT}/protocol/src/gc_msg.c
        ${ROOT}/protocol/src/gc_arena.c ${ROOT}/protocol/src/gc_ppp.c ${ROOT}/protocol/src/gc_ip.c ${ROOT}/protocol/src/gc_net.c
        ${ROOT}/dll/core/gc_dev.c ${ROOT}/dll/arena/core/pla_core.c
        PROPERTIES COMPILE_OPTIONS "-std=c89;-pedantic;-Wno-long-long")
endif()

foreach(t codec link msg ppp ip)
    add_executable(test_${t} ${ROOT}/protocol/tests/test_${t}.c)
    target_link_libraries(test_${t} gc_protocol)
    add_test(NAME protocol_${t} COMMAND test_${t})
endforeach()
# PPP with the ESP32 settings: MRU asked down to 1100, frames written in
# 16 byte pieces (firmware/platformio.ini uses 256)
add_executable(test_ppp_esp32 ${ROOT}/protocol/tests/test_ppp.c ${ROOT}/protocol/src/gc_ppp.c)
target_include_directories(test_ppp_esp32 PRIVATE ${ROOT}/protocol/include ${ROOT}/protocol/tests)
target_compile_definitions(test_ppp_esp32 PRIVATE GC_PPP_MRU=1100 GC_PPP_TXBUF=16)
add_test(NAME protocol_ppp_esp32_config COMMAND test_ppp_esp32)

add_executable(test_server_core ${ROOT}/server/tests/test_core.c)
target_link_libraries(test_server_core gc_server_core)
add_test(NAME server_core COMMAND test_server_core)

add_executable(test_e2e ${ROOT}/tests/sim/sim.c ${ROOT}/tests/sim/test_e2e.c)
target_link_libraries(test_e2e gc_dev_core gc_bridge_core gc_server_core)
add_test(NAME e2e_simulated COMMAND test_e2e)

add_executable(test_ppp_e2e ${ROOT}/tests/sim/test_ppp_e2e.c)
target_compile_definitions(test_ppp_e2e PRIVATE GCS_WWW_DIR="${ROOT}/server/deploy/www")
target_include_directories(test_ppp_e2e PRIVATE ${ROOT}/protocol/tests)
target_link_libraries(test_ppp_e2e gc_bridge_core gc_server_core)
add_test(NAME e2e_ppp COMMAND test_ppp_e2e)
# the same with 512 byte TCP send buffers in the bridge, so that every
# answer exercises the back-pressure from the bridge to the server link
add_executable(test_ppp_e2e_small ${ROOT}/tests/sim/test_ppp_e2e.c ${ROOT}/firmware/src/bridge_core.c
    ${ROOT}/protocol/src/gc_codec.c ${ROOT}/protocol/src/gc_link.c ${ROOT}/protocol/src/gc_msg.c
    ${ROOT}/protocol/src/gc_arena.c ${ROOT}/protocol/src/gc_ppp.c ${ROOT}/protocol/src/gc_ip.c
    ${ROOT}/protocol/src/gc_net.c ${ROOT}/server/src/gcs_core.c ${ROOT}/server/src/gcs_arena.c
    ${ROOT}/server/src/gcs_native.c ${ROOT}/server/src/gcs_accounts.c
    ${ROOT}/server/src/gcs_snap.c ${ROOT}/server/src/gcs_rifts.c ${ROOT}/server/src/gcs_rcodec.c ${ROOT}/server/src/gcs_direct.c)
target_include_directories(test_ppp_e2e_small PRIVATE ${ROOT}/protocol/include ${ROOT}/protocol/tests
    ${ROOT}/firmware/src ${ROOT}/server/src)
target_compile_definitions(test_ppp_e2e_small PRIVATE GC_TCP_TXBUF=512 GC_TCP_CONNS=4)
add_test(NAME e2e_ppp_small_buffers COMMAND test_ppp_e2e_small)
# the ESP32 configuration (firmware/platformio.ini): MRU and MTU asked
# down to 1100, PPP frames written in 256 byte pieces
add_executable(test_ppp_e2e_esp32 ${ROOT}/tests/sim/test_ppp_e2e.c ${ROOT}/firmware/src/bridge_core.c
    ${ROOT}/protocol/src/gc_codec.c ${ROOT}/protocol/src/gc_link.c ${ROOT}/protocol/src/gc_msg.c
    ${ROOT}/protocol/src/gc_arena.c ${ROOT}/protocol/src/gc_ppp.c ${ROOT}/protocol/src/gc_ip.c
    ${ROOT}/protocol/src/gc_net.c ${ROOT}/server/src/gcs_core.c ${ROOT}/server/src/gcs_arena.c
    ${ROOT}/server/src/gcs_native.c ${ROOT}/server/src/gcs_accounts.c
    ${ROOT}/server/src/gcs_snap.c ${ROOT}/server/src/gcs_rifts.c ${ROOT}/server/src/gcs_rcodec.c ${ROOT}/server/src/gcs_direct.c)
target_include_directories(test_ppp_e2e_esp32 PRIVATE ${ROOT}/protocol/include ${ROOT}/protocol/tests
    ${ROOT}/firmware/src ${ROOT}/server/src)
target_compile_definitions(test_ppp_e2e_esp32 PRIVATE GC_TCP_TXBUF=1536 GC_TCP_CONNS=4 GC_PPP_MRU=1100
    GC_IP_MTU=1100 GC_PPP_TXBUF=256)
add_test(NAME e2e_ppp_esp32_config COMMAND test_ppp_e2e_esp32)

add_executable(test_tcp_output ${ROOT}/tests/sim/test_tcp_output.c)
target_include_directories(test_tcp_output PRIVATE ${ROOT}/firmware/src)
add_test(NAME firmware_tcp_output COMMAND test_tcp_output)

add_executable(test_bt_output ${ROOT}/tests/sim/test_bt_output.c)
target_include_directories(test_bt_output PRIVATE ${ROOT}/firmware/src ${ROOT}/protocol/include)
add_test(NAME firmware_bt_output COMMAND test_bt_output)

add_executable(test_game_navigation ${ROOT}/tests/sim/test_game_navigation.c)
target_include_directories(test_game_navigation PRIVATE ${ROOT}/app/src)
add_test(NAME app_game_navigation COMMAND test_game_navigation)

# Bridge parity: pcbridge and the ESP32 bridge, each with its own sizing and
# transport glue, against the same N-Gage engine and server on links with
# latency and limited bandwidth. The ESP32 copy of the link layer and bridge
# core is built with the -D flags from firmware/platformio.ini and renamed
# (tests/parity/esp32_symbols.h), so both run in one program.
file(STRINGS ${ROOT}/firmware/platformio.ini GC_PIO_FLAGS REGEX "^[ \t]*-D[A-Za-z_][A-Za-z0-9_]*(=[^ \t;]*)?[ \t]*$")
set(GC_ESP32_DEFS "")
foreach(flag ${GC_PIO_FLAGS})
    string(STRIP "${flag}" flag)
    string(REGEX REPLACE "^-D" "" flag "${flag}")
    if(NOT flag MATCHES "^CORE_DEBUG_LEVEL")
        list(APPEND GC_ESP32_DEFS ${flag})
    endif()
endforeach()
message(STATUS "ESP32 bridge sizing from platformio.ini: ${GC_ESP32_DEFS}")
add_library(gc_bridge_esp32 STATIC ${ROOT}/tests/parity/esp32_gc_link.c ${ROOT}/tests/parity/esp32_bridge_core.c
    ${ROOT}/tests/parity/esp32_gc_ip.c ${ROOT}/tests/parity/esp32_gc_ppp.c
    ${ROOT}/tests/parity/bridge_profile.c)
target_include_directories(gc_bridge_esp32 PRIVATE ${ROOT}/protocol/include ${ROOT}/firmware/src ${ROOT}/tests/parity)
target_compile_definitions(gc_bridge_esp32 PRIVATE BP_ESP32 ${GC_ESP32_DEFS})
target_link_libraries(gc_bridge_esp32 PUBLIC gc_protocol)
add_executable(test_parity ${ROOT}/tests/parity/test_parity.c ${ROOT}/tests/parity/bridge_profile.c)
target_include_directories(test_parity PRIVATE ${ROOT}/tests/parity)
target_link_libraries(test_parity gc_bridge_esp32 gc_dev_core gc_bridge_core gc_server_core)
add_test(NAME bridge_parity COMMAND test_parity)

# message storage of link queue and bridge FIFOs, default and ESP32 sizing
foreach(sizing default esp32)
    add_executable(test_queue_storage_${sizing} ${ROOT}/tests/parity/test_queue_storage.c
        ${ROOT}/firmware/src/bridge_core.c ${ROOT}/protocol/src/gc_codec.c
        ${ROOT}/protocol/src/gc_link.c ${ROOT}/protocol/src/gc_msg.c ${ROOT}/protocol/src/gc_ppp.c
        ${ROOT}/protocol/src/gc_ip.c ${ROOT}/protocol/src/gc_net.c)
    target_include_directories(test_queue_storage_${sizing} PRIVATE ${ROOT}/firmware/src ${ROOT}/protocol/include)
    if(sizing STREQUAL "esp32")
        target_compile_definitions(test_queue_storage_${sizing} PRIVATE ${GC_ESP32_DEFS})
    endif()
    add_test(NAME queue_storage_${sizing} COMMAND test_queue_storage_${sizing})
endforeach()

# real sockets: start gamecommsd, run gcsim against it
add_executable(gamecommsd ${ROOT}/server/src/gcs_main.c)
target_link_libraries(gamecommsd gc_server_core)
if(WIN32)
    target_link_libraries(gamecommsd bcrypt)
endif()
add_executable(gcsim ${ROOT}/tests/net/gcsim.c)
target_link_libraries(gcsim gc_dev_core gc_bridge_core)
# PC bridge (without Bluetooth outside Windows; tested with TCP devices)
if(WIN32)
    add_executable(gcbridge ${ROOT}/pcbridge/src/pcb_main.c ${ROOT}/pcbridge/src/pcb_bt_win.c
                   ${ROOT}/pcbridge/src/pcb_com_win.c)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(gcbridge ${ROOT}/pcbridge/src/pcb_main.c ${ROOT}/pcbridge/src/pcb_bt_linux.c
                   ${ROOT}/pcbridge/src/pcb_com_posix.c)
else()
    add_executable(gcbridge ${ROOT}/pcbridge/src/pcb_main.c ${ROOT}/pcbridge/src/pcb_bt_none.c
                   ${ROOT}/pcbridge/src/pcb_com_posix.c)
endif()
target_include_directories(gcbridge PRIVATE ${ROOT}/pcbridge/src)
target_link_libraries(gcbridge gc_bridge_core)
if(WIN32)
    target_link_libraries(gamecommsd ws2_32)
    target_link_libraries(gcsim ws2_32)
    target_link_libraries(gcbridge ws2_32 bcrypt)
endif()

find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    add_test(NAME e2e_network COMMAND ${Python3_EXECUTABLE} ${ROOT}/tests/net/run_net_test.py
             $<TARGET_FILE:gamecommsd> $<TARGET_FILE:gcsim>)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        # PPP mode with real processes; pppsim plays the N-Gage through a TUN
        # interface (needs root, skipped otherwise)
        add_executable(pppsim ${ROOT}/tests/net/pppsim.c)
        target_link_libraries(pppsim gc_protocol)
        add_test(NAME pcbridge_ppp COMMAND ${Python3_EXECUTABLE} ${ROOT}/tests/net/run_ppp_test.py
                 $<TARGET_FILE:gamecommsd> $<TARGET_FILE:pppsim> $<TARGET_FILE:gcbridge> --bluetooth=0
                 "--device_tcp={dport}" --server=127.0.0.1 "--port={sport}" --log_level=3)
        set_tests_properties(pcbridge_ppp PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
    endif()
    add_test(NAME direct_network COMMAND ${Python3_EXECUTABLE} ${ROOT}/tests/net/run_direct_test.py
             $<TARGET_FILE:gamecommsd>)
    add_test(NAME pcbridge_network COMMAND ${Python3_EXECUTABLE} ${ROOT}/tests/net/run_pcbridge_test.py
             $<TARGET_FILE:gamecommsd> $<TARGET_FILE:gcbridge> $<TARGET_FILE:gcsim>)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        # BlueZ backend against a fake BlueZ on a private D-Bus daemon (skipped without dbus-daemon / python dbus)
        add_test(NAME pcbridge_bluez COMMAND ${Python3_EXECUTABLE} ${ROOT}/tests/net/run_bluez_test.py
                 $<TARGET_FILE:gamecommsd> $<TARGET_FILE:gcbridge> $<TARGET_FILE:gcsim>)
        set_tests_properties(pcbridge_bluez PROPERTIES SKIP_RETURN_CODE 77)
    endif()
    add_test(NAME dll_variants COMMAND ${Python3_EXECUTABLE} ${ROOT}/tests/compat/check_variants.py)
    add_test(NAME dll_variant_patches COMMAND ${Python3_EXECUTABLE} ${ROOT}/tests/compat/check_gcpatch.py)

    # Android bridge: the app's engine (BridgeEngine.java) and JNI library on
    # the host, TCP test devices instead of Bluetooth (needs a JDK)
    find_package(Java 11 COMPONENTS Development QUIET)
    find_package(JNI QUIET)
    if(Java_FOUND AND JNI_FOUND)
        set(GC_ANDROID ${ROOT}/android)
        add_library(gcbridge_jni SHARED ${ROOT}/protocol/src/gc_codec.c ${ROOT}/protocol/src/gc_link.c
                    ${ROOT}/protocol/src/gc_msg.c ${ROOT}/protocol/src/gc_ppp.c ${ROOT}/protocol/src/gc_ip.c
                    ${ROOT}/protocol/src/gc_net.c ${ROOT}/firmware/src/bridge_core.c
                    ${GC_ANDROID}/app/src/main/cpp/jni_bridge.c)
        target_include_directories(gcbridge_jni PRIVATE ${ROOT}/protocol/include ${ROOT}/firmware/src ${JNI_INCLUDE_DIRS})
        # loaded into the JVM: no sanitizer runtime
        set_target_properties(gcbridge_jni PROPERTIES OUTPUT_NAME gcbridge
                              COMPILE_OPTIONS "" LINK_OPTIONS ""
                              LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/android_lib
                              RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/android_lib)
        if(WIN32)
            set_target_properties(gcbridge_jni PROPERTIES PREFIX "") # System.loadLibrary wants gcbridge.dll
        endif()
        file(GLOB GC_ANDROID_JAVA ${GC_ANDROID}/app/src/main/java/org/ngagesdk/phantomlink/bridge/*.java)
        set(GC_ANDROID_CLASSES ${CMAKE_CURRENT_BINARY_DIR}/android_classes)
        add_custom_target(android_host_classes ALL
            COMMAND ${CMAKE_COMMAND} -E make_directory ${GC_ANDROID_CLASSES}
            COMMAND ${Java_JAVAC_EXECUTABLE} -encoding UTF-8 -d ${GC_ANDROID_CLASSES} ${GC_ANDROID_JAVA}
                    ${GC_ANDROID}/tests/HostBridge.java
            DEPENDS ${GC_ANDROID_JAVA} ${GC_ANDROID}/tests/HostBridge.java)
        add_test(NAME android_bridge_network COMMAND ${Python3_EXECUTABLE} ${GC_ANDROID}/tests/run_host_test.py
                 $<TARGET_FILE:gamecommsd> $<TARGET_FILE:gcsim> $<TARGET_FILE_DIR:gcbridge_jni>
                 ${GC_ANDROID_CLASSES} ${Java_JAVA_EXECUTABLE})
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            # PPP mode through the Android engine (needs root for pppsim's TUN)
            add_test(NAME android_bridge_ppp COMMAND ${Python3_EXECUTABLE} ${ROOT}/tests/net/run_ppp_test.py
                     $<TARGET_FILE:gamecommsd> $<TARGET_FILE:pppsim> ${Java_JAVA_EXECUTABLE}
                     -Djava.library.path=$<TARGET_FILE_DIR:gcbridge_jni> -cp ${GC_ANDROID_CLASSES} HostBridge
                     "--device_tcp={dport}" --server=127.0.0.1 "--port={sport}" --log_level=3)
            set_tests_properties(android_bridge_ppp PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180)
        endif()
    endif()
endif()

# gc_ip against the Linux kernel's TCP/IP stack over a TUN interface
# (needs root; reported as skipped without /dev/net/tun)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(Threads)
    add_executable(test_ip_tun ${ROOT}/tests/net/ip_tun.c)
    target_link_libraries(test_ip_tun gc_protocol Threads::Threads)
    add_test(NAME net_ip_tun COMMAND test_ip_tun)
    set_tests_properties(net_ip_tun PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 90)
endif()
