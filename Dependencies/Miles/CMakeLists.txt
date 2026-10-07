option(RTS_MINIAUDIO "Use the bundled miniaudio based Miles replacement instead of loading mss32.dll (always on for 64-bit builds)" OFF)

if(CMAKE_SIZEOF_VOID_P EQUAL 8 OR RTS_MINIAUDIO)
    # mss32.dll is a 32-bit library, so 64-bit builds implement the Miles API they need on top of miniaudio.
    set(MILES_SRC
        mss/mss.h
        MiniMiles.cpp
        MilesLoader.h
    )
else()
    set(MILES_SRC
        mss/mss.h
        MilesLoader.cpp
        MilesLoader.h
    )
endif()

add_library(deps_miles STATIC)

target_sources(deps_miles PRIVATE ${MILES_SRC})

target_include_directories(deps_miles PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/mss
)

target_link_libraries(deps_miles PRIVATE
    deps_precompiled # Gives force included headers. Link early.
    deps_utility
)
