# Minimal reimplementation of the few D3DX8 functions the game uses.
# Microsoft only ever shipped d3dx8.lib for x86, so 64-bit builds link this instead.
add_library(deps_d3dx8compat STATIC D3DX8Compat.cpp D3DX8Surface.cpp D3DX8Shader.cpp)
target_include_directories(deps_d3dx8compat PRIVATE ${dx8_SOURCE_DIR})
target_compile_definitions(deps_d3dx8compat PRIVATE BUILD_WITH_D3D8 NOMINMAX)
