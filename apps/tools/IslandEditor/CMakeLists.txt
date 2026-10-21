project(PoseidonIslandEditor VERSION 1.0.0)

# Island Editor: a standalone editor for the islands (.wrp) of Cold War Assault /
# Operation Flashpoint.  It reads the game's archives through the engine library and draws
# with Direct3D 12 through the small device layer in dx12/ (Windows only; see the README of
# the export for what a port to another renderer has to replace).
#
# In the root CMakeLists.txt:
#     if(WIN32)
#         add_subdirectory(apps/tools/IslandEditor)
#     endif()
# In vcpkg.json the imgui port needs the feature "dx12-binding" (on Windows).

# ---- HLSL -> embedded bytecode (fxc from the Windows SDK) ----
set(_ie_sdk_bin_hints)
if(DEFINED ENV{WindowsSdkVerBinPath})
    list(APPEND _ie_sdk_bin_hints "$ENV{WindowsSdkVerBinPath}/x64" "$ENV{WindowsSdkVerBinPath}/x86")
endif()
file(GLOB _ie_sdk_bins LIST_DIRECTORIES true "C:/Program Files (x86)/Windows Kits/10/bin/10.*")
list(SORT _ie_sdk_bins ORDER DESCENDING)
foreach(_dir IN LISTS _ie_sdk_bins)
    list(APPEND _ie_sdk_bin_hints "${_dir}/x64" "${_dir}/x86")
endforeach()
find_program(ISLAND_EDITOR_FXC fxc HINTS ${_ie_sdk_bin_hints} REQUIRED)

set(_ie_shader_dir "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
set(_ie_shader_out "${CMAKE_CURRENT_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY "${_ie_shader_out}")
set(ISLAND_EDITOR_SHADER_HEADERS)
foreach(_name vsTerrain psTerrain psModel vsLine psLine)
    if(_name MATCHES "^vs")
        set(_profile vs_5_0)
    else()
        set(_profile ps_5_0)
    endif()
    set(_out "${_ie_shader_out}/${_name}.h")
    add_custom_command(
        OUTPUT "${_out}"
        COMMAND "${ISLAND_EDITOR_FXC}" /nologo /T ${_profile} /E main /O3 /Gec /Vn g_${_name} /Fh "${_out}"
                "${_ie_shader_dir}/${_name}.hlsl"
        DEPENDS "${_ie_shader_dir}/${_name}.hlsl"
        COMMENT "fxc ${_profile} ${_name}.hlsl"
        VERBATIM
    )
    list(APPEND ISLAND_EDITOR_SHADER_HEADERS "${_out}")
endforeach()

# ---- the Direct3D 12 device layer (Dx12::Device): needs nothing but the Windows SDK ----
add_library(IslandEditorDx12 STATIC
    dx12/PoseidonD3D12/Dx12Device.cpp
    dx12/PoseidonD3D12/Dx12Device.hpp
    dx12/PoseidonD3D12/Dx12Types.hpp
)
target_include_directories(IslandEditorDx12 PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/dx12")
target_compile_definitions(IslandEditorDx12 PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(IslandEditorDx12 PUBLIC d3d12 dxgi d3dcompiler dxguid)

add_executable(PoseidonIslandEditor
    main.cpp
    Island.cpp
    Island.hpp
    GameData.cpp
    GameData.hpp
    Renderer.cpp
    Renderer.hpp
    HeightMap.cpp
    HeightMap.hpp
    Reskin.cpp
    Reskin.hpp
    Builders.cpp
    Builders.hpp
    ${ISLAND_EDITOR_SHADER_HEADERS}
    ${CMAKE_SOURCE_DIR}/resources/icon-openposeidon.rc # the game's own icon (owner)
)

target_compile_definitions(PoseidonIslandEditor PRIVATE
    IMGUI_ENABLE_FREETYPE
    NOMINMAX
    $<$<CONFIG:Debug>:_DEBUG>
)

find_package(SDL3 CONFIG REQUIRED)
find_package(freetype CONFIG REQUIRED)
find_package(CLI11 CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)
find_package(Stb REQUIRED) # stb_image, for height maps (the engine library holds its implementation)

# as PoseidonStudio links: the engine library and the sound backend it refers to
target_link_libraries(PoseidonIslandEditor PRIVATE
    IslandEditorDx12
    Poseidon
    PoseidonOpenAL
    SDL3::SDL3
    Freetype::Freetype
    CLI11::CLI11
    imgui::imgui
)

target_link_libraries(PoseidonIslandEditor PRIVATE
    ws2_32 winmm version comctl32 ole32 shell32
    legacy_stdio_definitions.lib
)

target_include_directories(PoseidonIslandEditor PRIVATE
    ${CMAKE_SOURCE_DIR}/engine
    "${_ie_shader_out}"
    ${Stb_INCLUDE_DIR}
)

include(${CMAKE_SOURCE_DIR}/cmake/DistCopy.cmake)
dist_copy(PoseidonIslandEditor)
