# One Arena DLL (see ../CMakeLists.txt).  Untested: written against the
# same toolchain functions as dll/variant/CMakeLists.txt.
set(m ${PLA_MODULE})
set(lib pla_${m})

add_library(${lib} STATIC ${PLA_SRC_${m}})
target_compile_definitions(${lib} PRIVATE
    __DLL__ GC_LINK_QUEUE=16
    -D__SYMBIAN32__ -D__GCC32__ -D__EPOC32__ -D__MARM__ -D__MARM_ARMI__ -DNDEBUG -D_UNICODE)
target_include_directories(${lib} PRIVATE
    ${GC_ROOT}/protocol/include
    ${PLA_DIR}/core
    ${PLA_DIR}/symbian
    ${GC_DLL_DIR}/symbian
    ${GC_DLL_DIR}/symbian/bt)
target_compile_options(${lib} PRIVATE -O2 -Wall)

set(libs ${gc_libs})
if(NOT m STREQUAL "plarena")
    # the plug-ins import plarena.dll
    list(APPEND libs ${CMAKE_CURRENT_BINARY_DIR}/../plarena/pla_plarena.lib)
endif()

configure_file(${PLA_DIR}/def/${m}.def ${CMAKE_CURRENT_BINARY_DIR}/${lib}.def COPYONLY)
build_dll_ex(${lib} ${lib} dll ${GC_UID1} ${GC_UID2} ${PLA_UID3_${m}} "${libs}" ${CMAKE_CURRENT_BINARY_DIR}/${lib}.def)

add_dependencies(${lib}_tmp.exp ${lib})
add_dependencies(${lib}.bas     ${lib}_tmp.exp)
add_dependencies(${lib}.exp     ${lib}.bas)
add_dependencies(${lib}_tmp.lib ${lib}.exp)
add_dependencies(${lib}.map     ${lib}_tmp.lib)
add_dependencies(${lib}.dll     ${lib}.map)
if(NOT m STREQUAL "plarena")
    add_dependencies(${lib} pla_plarena.dll)
endif()

add_custom_target(${lib}_out ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory ${PLA_OUT}
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_BINARY_DIR}/${lib}.dll ${PLA_OUT}/${m}.dll)
add_dependencies(${lib}_out ${lib}.dll)
