# PhantomLink N-Gage Arena plug-ins (docs/ARENA.md)
#
#   plarena.dll         shared bridge link (pla_core + GcTransport)
#   snapcomm.dll        replaces SnapComm.dll        (UID3 0x101fde38)
#   sc_lib.dll          replaces sc_lib.dll          (UID3 0x101fde44)
#   httpdllrequest.dll  replaces HttpDllRequest.dll  (UID3 0x10202464)
#
# Included from dll/CMakeLists.txt (needs the N-Gage toolchain).  Output:
# build/dll/out/arena/*.dll, to be copied over the game's own copies in
# E:\System\Libs together with plarena.dll.

set(PLA_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(PLA_OUT ${GC_OUT_DIR}/arena)

set(PLA_CORE_SOURCES
    ${GC_ROOT}/protocol/src/gc_codec.c
    ${GC_ROOT}/protocol/src/gc_link.c
    ${GC_ROOT}/protocol/src/gc_msg.c
    ${GC_ROOT}/protocol/src/gc_arena.c
    ${PLA_DIR}/core/pla_core.c
    ${PLA_DIR}/symbian/PlArena.cpp
    ${GC_DLL_DIR}/symbian/GcConfig.cpp
    ${GC_DLL_DIR}/symbian/GcTransport.cpp
    ${GC_DLL_DIR}/symbian/bt/BTServiceSearcher.cpp
    ${GC_DLL_DIR}/symbian/bt/MessageServiceSearcher.cpp
    ${GC_DLL_DIR}/symbian/bt/SdpAttributeParser.cpp)

# name, UID3, sources, libraries; each module in its own binary directory
# (dlltool writes temporary files with fixed names, see variant/CMakeLists.txt)
set(PLA_MODULES plarena snapcomm sc_lib httpdllrequest)
set(PLA_UID3_plarena 0x0fa4e7a1)
set(PLA_UID3_snapcomm 0x101fde38)
set(PLA_UID3_sc_lib 0x101fde44)
set(PLA_UID3_httpdllrequest 0x10202464)
set(PLA_SRC_plarena ${PLA_CORE_SOURCES})
set(PLA_SRC_snapcomm ${GC_ROOT}/protocol/src/gc_arena.c ${PLA_DIR}/symbian/PlSnapComm.cpp)
set(PLA_SRC_sc_lib ${GC_ROOT}/protocol/src/gc_arena.c ${PLA_DIR}/symbian/PlScLib.cpp)
set(PLA_SRC_httpdllrequest ${GC_ROOT}/protocol/src/gc_arena.c ${PLA_DIR}/symbian/PlHttpRequest.cpp)

foreach(m ${PLA_MODULES})
    set(PLA_MODULE ${m})
    add_subdirectory(module ${m})
endforeach()
