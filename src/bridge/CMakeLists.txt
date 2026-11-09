cmake_minimum_required(VERSION 3.24)
project(dd2_ntc_bridge LANGUAGES C CXX)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Build the plugin for Windows x64.")
endif()

include(FetchContent)
FetchContent_Declare(minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG c3fcafdc10146beb5919319d0683e44e3c30d537
    GIT_SUBMODULES ""
)
set(BUILD_SHARED_LIBS OFF)
FetchContent_MakeAvailable(minhook)

set(BUILD_TOOLS OFF)
set(BUILD_SAMPLE OFF)
set(BUILD_DX11 OFF)
set(BUILD_DX12 OFF)
set(BC_USE_OPENMP OFF)
add_subdirectory("../tools/DirectXTex/source" "${CMAKE_CURRENT_BINARY_DIR}/DirectXTex" EXCLUDE_FROM_ALL)

set(stb_dxt "${CMAKE_CURRENT_BINARY_DIR}/stb_dxt.h")
set(stb_dxt_hash 807667ef98e0fd749cdb65cca0c2d980bc148109d2fed6f1873c81ae0f449933)
if(EXISTS "${stb_dxt}")
    file(SHA256 "${stb_dxt}" current_stb_dxt_hash)
endif()
if(NOT current_stb_dxt_hash STREQUAL stb_dxt_hash)
    file(DOWNLOAD
        https://raw.githubusercontent.com/nothings/stb/d14d46f6b44456eb88beac58cba833a4135d4c89/stb_dxt.h
        "${stb_dxt}" EXPECTED_HASH SHA256=${stb_dxt_hash} TLS_VERIFY ON
    )
endif()

set(bc7_revision b9438627eef73a1157e84201b6fa6eb2ffd6d9f0)
function(fetch_bc7_file name hash)
    set(destination "${CMAKE_CURRENT_BINARY_DIR}/${name}")
    if(EXISTS "${destination}")
        file(SHA256 "${destination}" current_hash)
    endif()
    if(NOT current_hash STREQUAL hash)
        file(DOWNLOAD "https://raw.githubusercontent.com/richgel999/bc7enc_rdo/${bc7_revision}/${name}"
            "${destination}" EXPECTED_HASH "SHA256=${hash}" TLS_VERIFY ON)
    endif()
endfunction()
fetch_bc7_file(bc7enc.cpp b90444a091530a13c61fa6375583fcc9bc9ca47e474fd71f818876de68c1b581)
fetch_bc7_file(bc7enc.h f40f4a956ed6a8845b7909e83831de09512424408b32ca29551bf49aee2fecf6)

add_library(dd2_ntc_bridge SHARED bridge.cpp decode.cpp gpu_tex.cpp preread.cpp signatures.cpp tex_recipe.cpp "${CMAKE_CURRENT_BINARY_DIR}/bc7enc.cpp")
target_compile_features(dd2_ntc_bridge PRIVATE cxx_std_17)
target_compile_definitions(dd2_ntc_bridge PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(dd2_ntc_bridge PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../tools/REFramework-source/REFramework-1.5.9.1/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../tools/RTXNTC/libraries/RTXNTC-Library/include"
)
target_link_libraries(dd2_ntc_bridge PRIVATE minhook DirectXTex d3d12 dxgi)

add_executable(batch_bc1 EXCLUDE_FROM_ALL ../batch/bc1.cpp)
target_compile_features(batch_bc1 PRIVATE cxx_std_17)
target_include_directories(batch_bc1 PRIVATE "${CMAKE_CURRENT_BINARY_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}")
