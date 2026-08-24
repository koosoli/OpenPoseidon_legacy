# PoseidonGame - Game client executable

set(GAME_SOURCES
    WinMain.cpp
    GameApplication.cpp
    HighPerformanceGpuHint.cpp
)


if(WIN32)
    list(APPEND GAME_SOURCES resource.rc)
    add_executable(PoseidonGame WIN32 ${GAME_SOURCES})
else()
    add_executable(PoseidonGame ${GAME_SOURCES})
endif()

# Executable-specific compile definitions
target_compile_definitions(PoseidonGame PRIVATE
    $<$<CONFIG:Debug>:_DEBUG>     # Enable debug mode (affects Assert/Fail macros in debugLog.hpp)
)
if(WIN32)
    target_compile_definitions(PoseidonGame PRIVATE _WINDOWS)
endif()

# Match MSVC compiler settings
if(MSVC OR (CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC"))
    target_compile_options(PoseidonGame PRIVATE
        /W3                    # Warning level 3
        /GR                    # Enable RTTI
        $<$<CONFIG:Debug>:/Od>     # Disable optimization (Debug only)
        $<$<CONFIG:Debug>:/MDd>    # Runtime library: MultiThreadedDebugDLL (Debug only)
    )
    
    # Disable SafeSEH for compatibility with external .obj files.
    target_link_options(PoseidonGame PRIVATE /SAFESEH:NO)
endif()

# Include paths: only need root and Poseidon (all libs/externals handled by Poseidon.lib)
target_include_directories(PoseidonGame PRIVATE
    ${CMAKE_SOURCE_DIR}/engine
    # resource.rc references resrc1.h/resource.h (Core) and versionNo.h
    # (Foundation/Platform) by bare name; expose those dirs to the RC compiler only.
    $<$<COMPILE_LANGUAGE:RC>:${CMAKE_SOURCE_DIR}/engine/Poseidon/Core>
    $<$<COMPILE_LANGUAGE:RC>:${CMAKE_SOURCE_DIR}/engine/Poseidon/Foundation/Platform>
)

set(WGPU_LIB "")
if(POSEIDON_ENABLE_WGPU)
    set(WGPU_LIB WgpuRenderer)
endif()

# Link against libraries (Poseidon transitively brings in folded support/runtime code and externals)
if(WIN32)
    target_link_libraries(PoseidonGame PRIVATE
        GameBase
        Poseidon
        PoseidonGL33
        ${WGPU_LIB}
        PoseidonOpenAL
        # Windows system libraries
        winmm.lib
        version.lib
        ws2_32.lib
        legacy_stdio_definitions.lib
    )
elseif(APPLE)
    target_link_libraries(PoseidonGame PRIVATE
        GameBase
        PoseidonGL33
        Poseidon
        PoseidonOpenAL
        pthread
        dl
    )
else()
    target_link_libraries(PoseidonGame PRIVATE
        -Wl,--start-group
        GameBase
        PoseidonGL33
        ${WGPU_LIB}
        Poseidon
        PoseidonOpenAL
        -Wl,--end-group
        pthread
        dl
    )
endif()

if(POSEIDON_ENABLE_WGPU)
    target_compile_definitions(PoseidonGame PRIVATE POSEIDON_ENABLE_WGPU=1)
    wgpu_renderer_stage_dll(PoseidonGame)
endif()

# Use WINDOWS subsystem (WIN32 flag); AttachConsole at runtime handles CLI output
if(MSVC OR (CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC"))
    set_target_properties(PoseidonGame PROPERTIES
        LINK_FLAGS "/IGNORE:4099"
    )
endif()

# The shipped client is OpenPoseidon.exe. The CMake TARGET stays PoseidonGame so every
# script, preset and CI job that builds it by target name keeps working; only the file on
# disk is renamed. One name, deliberately: this folder is shared by several worktrees, and
# an earlier attempt was parked because it ADDED a second name rather than replacing the
# first, which widened the mismatched-pair ambiguity instead of closing it.
set_target_properties(PoseidonGame PROPERTIES
    OUTPUT_NAME "OpenPoseidon"
)

include(${CMAKE_SOURCE_DIR}/cmake/DistCopy.cmake)
dist_copy(PoseidonGame)
# Loose runtime assets (assets/grass/*.png). Absence degrades silently, so stage it.
stage_assets(PoseidonGame)
