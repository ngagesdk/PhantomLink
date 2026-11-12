# PhantomLink - Symbian application (game selection and bridge setup).
#
# Included from ../dll/CMakeLists.txt, which provides the toolchain, the
# gc_libs list and the per-variant gamecomms_vN_out targets.  Output:
#   <build>/app/PhantomLink.sis
# See docs/BUILD.md.

set(PL_UID3 0x1000a9f0)  # PhantomLink.app UID
string(SUBSTRING ${PL_UID3} 2 -1 PL_UID3_PKG)
set(PL_UID3_PKG 0x${PL_UID3_PKG})

set(PL_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src")
set(PL_RES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/res")
set(PL_INC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/inc")

set(pl_sources
    ${PL_SRC_DIR}/PhantomLinkApp.cpp
    ${PL_SRC_DIR}/PhantomLinkAppUi.cpp
    ${PL_SRC_DIR}/ArenaSetup.cpp
    ${PL_SRC_DIR}/PhantomLinkView.cpp
    ${PL_SRC_DIR}/BridgeClient.cpp
    ${PL_SRC_DIR}/BridgePicker.cpp
    ${PL_SRC_DIR}/BoardClient.cpp
    ${GC_ROOT}/protocol/src/gc_codec.c
    ${GC_ROOT}/protocol/src/gc_link.c
    ${GC_ROOT}/protocol/src/gc_msg.c
    ${PL_SRC_DIR}/GameList.cpp
    ${PL_SRC_DIR}/GameInstaller.cpp
    ${GC_DLL_DIR}/symbian/GcConfig.cpp
    ${GC_DLL_DIR}/symbian/GcTransport.cpp
    ${GC_DLL_DIR}/symbian/bt/BTServiceSearcher.cpp
    ${GC_DLL_DIR}/symbian/bt/MessageServiceSearcher.cpp
    ${GC_DLL_DIR}/symbian/bt/SdpAttributeParser.cpp)

# The SDK's C++ compiler launcher does not emit dependency files. Rebuild
# all app C++ objects when a local header or resource definition changes,
# so callers and implementations cannot disagree about class layouts.
file(GLOB_RECURSE pl_headers
    ${PL_SRC_DIR}/*.h ${PL_INC_DIR}/*.hrh
    ${GC_DLL_DIR}/symbian/*.h ${GC_DLL_DIR}/core/*.h
    ${GC_ROOT}/protocol/include/*.h)
foreach(src ${pl_sources})
    if(src MATCHES "\\.cpp$")
        set_property(SOURCE ${src} APPEND PROPERTY OBJECT_DEPENDS
            "${pl_headers};${PL_RES_DIR}/PhantomLink.rss")
    endif()
endforeach()

set(pl_libs
    ${EPOC_LIB}/euser.lib
    ${EPOC_LIB}/apparc.lib
    ${EPOC_LIB}/cone.lib
    ${EPOC_LIB}/eikcore.lib
    ${EPOC_LIB}/eikcoctl.lib
    ${EPOC_LIB}/eikdlg.lib
    ${EPOC_LIB}/eikctl.lib
    ${EPOC_LIB}/avkon.lib
    ${EPOC_LIB}/efsrv.lib
    ${EPOC_LIB}/bafl.lib
    ${EPOC_LIB}/esock.lib
    ${EPOC_LIB}/commdb.lib
    ${EPOC_LIB}/btregistry.lib
    ${EPOC_LIB}/bluetooth.lib
    ${EPOC_LIB}/btextnotifiers.lib
    ${EPOC_LIB}/sdpagent.lib
    ${EPOC_LIB}/sdpdatabase.lib
    ${EPOC_LIB}/charconv.lib
    ${EPOC_LIB}/ws32.lib
    ${EPOC_LIB}/gdi.lib
    ${EPOC_PLATFORM}/gcc/lib/gcc-lib/arm-epoc-pe/2.9-psion-98r2/libgcc.a)

add_library(PhantomLink STATIC ${pl_sources})
target_compile_definitions(PhantomLink PRIVATE
    __DLL__ UID1=${GC_UID1} UID2=0x100039ce UID3=${PL_UID3}
    -D__SYMBIAN32__ -D__GCC32__ -D__EPOC32__ -D__MARM__ -D__MARM_ARMI__ -DNDEBUG -D_UNICODE)
target_include_directories(PhantomLink PRIVATE
    ${PL_SRC_DIR}
    ${PL_INC_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
    ${GC_ROOT}/protocol/include
    ${GC_DLL_DIR}/core
    ${GC_DLL_DIR}/symbian
    ${GC_DLL_DIR}/symbian/bt)
target_compile_options(PhantomLink PRIVATE -O2 -Wall)

# An S60 application is a DLL with UID1 0x1000007a and UID2 0x100039ce.
build_dll(PhantomLink PhantomLink app 0x1000007a 0x100039ce ${PL_UID3} "${pl_libs}")

add_dependencies(PhantomLink_tmp.exp PhantomLink)
add_dependencies(PhantomLink.bas     PhantomLink_tmp.exp)
add_dependencies(PhantomLink.exp     PhantomLink.bas)
add_dependencies(PhantomLink_tmp.lib PhantomLink.exp)
add_dependencies(PhantomLink.map     PhantomLink_tmp.lib)
add_dependencies(PhantomLink.app     PhantomLink.map)

build_resource(${PL_RES_DIR} PhantomLink "")

# res/PhantomLink.mbm is committed; regenerate it after changing the icons
# with: python app/res/mkmbm.py app/res/PhantomLink.mbm icon42x29.bmp
# icon42x29_mask.bmp icon44x44.bmp icon44x44_mask.bmp (run in app/res).
build_aif(${PL_RES_DIR} PhantomLink ${PL_UID3})

# The resource header is needed by the application sources.
add_dependencies(PhantomLink PhantomLink.rsc)

# SIS installer: the application, one full DLL (the base variant) and a
# small patch per other variant (dlls\gcvN.pat, made by
# dll/variants/gcpatch.py).  The variants differ only in their export
# tables, so this saves about 580 KB on the phone.  GameInstaller.cpp
# rebuilds the exact variant when the user installs it for a game.
# The app's game list refers to every variant, so all of them are required.
set(PL_ALL_VARIANTS 1 2 3 5 6 7 8 9 10 11)
set(PL_BASE_VARIANT 6)
foreach(v ${PL_ALL_VARIANTS})
    if(NOT "${v}" IN_LIST GC_VARIANTS)
        message(FATAL_ERROR "PhantomLink.sis needs all DLL variants, but variant ${v} is not in GC_VARIANTS. Build all variants or use -DGC_BUILD_APP=OFF.")
    endif()
endforeach()
if(NOT Python3_FOUND)
    message(FATAL_ERROR "PhantomLink.sis needs Python 3 to make the DLL patches (dll/variants/gcpatch.py). Install Python 3 or use -DGC_BUILD_APP=OFF.")
endif()
set(PL_BIN ${CMAKE_CURRENT_BINARY_DIR})
set(pl_dll_dir ${CMAKE_CURRENT_BINARY_DIR}/dlls)
set(PL_DLL_LINES "")
set(pl_out_targets "")
set(pl_dll_files "")
foreach(v ${PL_ALL_VARIANTS})
    if(v EQUAL PL_BASE_VARIANT)
        set(f gcv${v}.dll)
    else()
        set(f gcv${v}.pat)
    endif()
    list(APPEND pl_dll_files ${pl_dll_dir}/${f})
    file(TO_NATIVE_PATH "${pl_dll_dir}/${f}" f_native)
    string(APPEND PL_DLL_LINES "\"${f_native}\"-\"!:\\System\\Apps\\PhantomLink\\dlls\\${f}\"\n")
    list(APPEND pl_out_targets gamecomms_v${v}_out)
endforeach()
# A custom target (not a custom command): the inputs come from the
# gamecomms_vN_out copy targets, which have no file-level outputs.
# gcpatch.py only rewrites files whose content changed.
add_custom_target(PhantomLink_dlls
    COMMAND ${Python3_EXECUTABLE} ${GC_DLL_DIR}/variants/gcpatch.py make
            ${GC_OUT_DIR} ${pl_dll_dir} ${PL_BASE_VARIANT} ${PL_ALL_VARIANTS}
    BYPRODUCTS ${pl_dll_files}
    COMMENT "Making the DLL variant patches for PhantomLink.sis")
add_dependencies(PhantomLink_dlls ${pl_out_targets})
file(TO_NATIVE_PATH "${PL_BIN}" PL_BIN_NATIVE)
set(PL_BIN ${PL_BIN_NATIVE})
configure_file(${PL_RES_DIR}/PhantomLink.pkg.in ${CMAKE_CURRENT_BINARY_DIR}/PhantomLink.pkg @ONLY)

build_sis(${CMAKE_CURRENT_BINARY_DIR} PhantomLink)
add_dependencies(PhantomLink.sis PhantomLink.app PhantomLink.rsc PhantomLink.aif PhantomLink_dlls)
