option(RTS_D3DX8_COMPAT "Use the bundled D3DX8 replacement instead of d3dx8.lib (always on for 64-bit builds)" OFF)

if(CMAKE_SIZEOF_VOID_P EQUAL 4 AND NOT RTS_D3DX8_COMPAT)
    set(RTS_USE_D3DX8_COMPAT FALSE)

    FetchContent_Declare(
        dx8
        GIT_REPOSITORY https://github.com/TheSuperHackers/min-dx8-sdk.git
        GIT_TAG        7bddff8c01f5fb931c3cb73d4aa8e66d303d97bc
    )

    FetchContent_MakeAvailable(dx8)
else()
    set(RTS_USE_D3DX8_COMPAT TRUE)

    # Only the headers of min-dx8-sdk are used here; its static libs are x86-only and include d3dx8.lib.
    #  - 64-bit: d3d8.lib is generated as an import library for Direct3DCreate8. At runtime d3d8.dll is
    #    either DXVK's d3d8 (Direct3D 8 -> Vulkan) placed next to the exe, or the system one.
    #  - 32-bit: the SDK's own d3d8.lib is used.
    #  - dinput8.lib / dxguid.lib come from the Windows SDK (64-bit) or the DX8 SDK (32-bit).
    #  - Dependencies/D3DX8Compat provides the subset of d3dx8 the game needs.
    FetchContent_Declare(
        dx8
        GIT_REPOSITORY https://github.com/TheSuperHackers/min-dx8-sdk.git
        GIT_TAG        7bddff8c01f5fb931c3cb73d4aa8e66d303d97bc
        SOURCE_SUBDIR  headers-only-do-not-add
    )
    FetchContent_MakeAvailable(dx8)

    if(CMAKE_SIZEOF_VOID_P EQUAL 4)
        set(_d3d8_libdir "${dx8_SOURCE_DIR}")
    else()
        set(_d3d8_libdir "${CMAKE_BINARY_DIR}/d3d8_x64")
        file(MAKE_DIRECTORY "${_d3d8_libdir}")
        file(WRITE "${_d3d8_libdir}/d3d8.def" "LIBRARY d3d8\nEXPORTS\n    Direct3DCreate8\n")
        if(NOT EXISTS "${_d3d8_libdir}/d3d8.lib")
            execute_process(
                COMMAND "${CMAKE_AR}" /nologo /machine:x64 "/def:${_d3d8_libdir}/d3d8.def" "/out:${_d3d8_libdir}/d3d8.lib"
                RESULT_VARIABLE _lib_result
            )
            if(NOT _lib_result EQUAL 0)
                message(FATAL_ERROR "Could not generate the x64 d3d8 import library")
            endif()
        endif()
    endif()

    add_subdirectory(Dependencies/D3DX8Compat)

    add_library(d3d8lib INTERFACE)
    target_include_directories(d3d8lib INTERFACE ${dx8_SOURCE_DIR})
    target_compile_definitions(d3d8lib INTERFACE BUILD_WITH_D3D8)
    target_link_directories(d3d8lib BEFORE INTERFACE ${_d3d8_libdir})
    target_link_libraries(d3d8lib INTERFACE d3d8 dinput8 dxguid deps_d3dx8compat legacy_stdio_definitions)
    if(CMAKE_SIZEOF_VOID_P EQUAL 4)
        target_link_options(d3d8lib INTERFACE /NODEFAULTLIB:libci.lib /SAFESEH:NO)
    endif()
endif()
