# PhantomLink's abtesrv.dll for SSX Out of Bounds (docs/SSX.md).
#
# Included from dll/CMakeLists.txt (needs the N-Gage toolchain).  Output:
# build/dll/out/ssx/abtesrv.dll. The app SIS installs it on C:, and the
# PhantomLink server can also hand it to the game (--ssx_billing_dll=PATH).

set(lib abtesrv)
set(ABTE_UID3 0x0fa4e7a2)

add_library(${lib} STATIC ${CMAKE_CURRENT_SOURCE_DIR}/AbteSrv.cpp)
target_compile_definitions(${lib} PRIVATE
    __DLL__ -D__SYMBIAN32__ -D__GCC32__ -D__EPOC32__ -D__MARM__ -D__MARM_ARMI__ -DNDEBUG -D_UNICODE)
target_compile_options(${lib} PRIVATE -O2 -Wall)

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/abtesrv.def ${CMAKE_CURRENT_BINARY_DIR}/${lib}.def COPYONLY)
build_dll_ex(${lib} ${lib} dll ${GC_UID1} ${GC_UID2} ${ABTE_UID3}
    "${EPOC_LIB}/euser.lib;${EPOC_PLATFORM}/gcc/lib/gcc-lib/arm-epoc-pe/2.9-psion-98r2/libgcc.a"
    ${CMAKE_CURRENT_BINARY_DIR}/${lib}.def)

add_dependencies(${lib}_tmp.exp ${lib})
add_dependencies(${lib}.bas     ${lib}_tmp.exp)
add_dependencies(${lib}.exp     ${lib}.bas)
add_dependencies(${lib}_tmp.lib ${lib}.exp)
add_dependencies(${lib}.map     ${lib}_tmp.lib)
add_dependencies(${lib}.dll     ${lib}.map)

add_custom_target(${lib}_out ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory ${GC_OUT_DIR}/ssx
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_BINARY_DIR}/${lib}.dll ${GC_OUT_DIR}/ssx/abtesrv.dll)
add_dependencies(${lib}_out ${lib}.dll)
