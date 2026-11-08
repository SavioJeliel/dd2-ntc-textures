cmake_minimum_required(VERSION 3.24)
project(ntc_gpu_bench LANGUAGES C CXX)

set(BRIDGE "${CMAKE_CURRENT_SOURCE_DIR}/../bridge")
set(TOOLS "${CMAKE_CURRENT_SOURCE_DIR}/../tools")

set(BUILD_SHARED_LIBS OFF)
set(BUILD_TOOLS OFF)
set(BUILD_SAMPLE OFF)
set(BUILD_DX11 ON)   # DirectXTex precisa disso para o encoder BC6H por GPU.
set(BUILD_DX12 OFF)
set(BC_USE_OPENMP OFF) # Igual à DLL, para a linha de base de CPU ser comparável.
# O passo de shaders do DirectXTex falha com espaço no caminho: use os pré-compilados por build-shaders.cmd.
set(USE_PREBUILT_SHADERS ON)
set(COMPILED_SHADERS "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
add_subdirectory("${TOOLS}/DirectXTex/source" "${CMAKE_CURRENT_BINARY_DIR}/DirectXTex" EXCLUDE_FROM_ALL)

add_executable(ntc_gpu_bench
    bench.cpp
    "${BRIDGE}/decode.cpp"
    "${BRIDGE}/tex_recipe.cpp"
    "${BRIDGE}/gpu_tex.cpp"
    "${BRIDGE}/build/bc7enc.cpp")
target_compile_features(ntc_gpu_bench PRIVATE cxx_std_17)
target_compile_definitions(ntc_gpu_bench PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(ntc_gpu_bench PRIVATE
    "${BRIDGE}"
    "${BRIDGE}/build"
    "${TOOLS}/REFramework-source/REFramework-1.5.9.1/include"
    "${TOOLS}/RTXNTC/libraries/RTXNTC-Library/include")
target_link_libraries(ntc_gpu_bench PRIVATE DirectXTex d3d12 d3d11 dxgi dxguid)
