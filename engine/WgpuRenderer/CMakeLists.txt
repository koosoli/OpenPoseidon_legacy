# WgpuRenderer - modern Rust/wgpu graphics backend
#
# Two halves:
#   rust/        the `wgpu_renderer` crate, built as a cdylib via corrosion
#   *.cpp        the C++ Engine backend that drives the crate over the C ABI
#                (include/wgpu_renderer.hpp)

cmake_minimum_required(VERSION 3.25)

if(NOT DEFINED Rust_CARGO_TARGET)
    if(WIN32)
        if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "GNU")
            set(Rust_CARGO_TARGET "x86_64-pc-windows-gnu")
        else()
            set(Rust_CARGO_TARGET "x86_64-pc-windows-msvc")
        endif()
    else()
        set(Rust_CARGO_TARGET "x86_64-unknown-linux-gnu")
    endif()
endif()

# FetchContent is corrosion's recommended integration approach
include(FetchContent)
FetchContent_Declare(
    Corrosion
    GIT_REPOSITORY https://github.com/corrosion-rs/corrosion.git
    GIT_TAG v0.6.1
)
FetchContent_MakeAvailable(Corrosion)

# Profile names mirror rust/Cargo.toml (which mirrors Trident).
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(_wgr_cargo_profile dbg)
elseif(CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    set(_wgr_cargo_profile rwdi)
else()
    set(_wgr_cargo_profile rel)
endif()

# REN-TEMP-001M: -DPOSEIDON_DLSS=ON compiles the renderer with the proprietary NGX/DLSS
# path (runtime-gated by WGR_DLSS=1; needs the untracked .tmp-dlss-sdk checkout). OFF is
# the default and the only configuration that may ever be distributed — the SDK is
# GPL-incompatible (docs/temporal-upscaling-plan.md §0.1).
option(POSEIDON_DLSS "Build wgpu_renderer with the DLSS (NGX) feature" OFF)
if(POSEIDON_DLSS)
    set(_wgr_cargo_features FEATURES dlss)
else()
    set(_wgr_cargo_features)
endif()

corrosion_import_crate(
    MANIFEST_PATH ${CMAKE_CURRENT_SOURCE_DIR}/rust/Cargo.toml
    CRATE_TYPES cdylib
    PROFILE ${_wgr_cargo_profile}
    ${_wgr_cargo_features}
)

add_library(WgpuRenderer STATIC
    EngineWgpu.cpp
    EngineWgpuRetailPages.cpp
    EngineWgpuRetailInstances.cpp
    EngineWgpuRetailWorld.cpp
    EngineWgpuRetailWorldInstances.cpp
    EngineWgpuTextInput.cpp
    GraphicsBackendWgpu.cpp
    TerrainWgpu.cpp
    TerrainWgpuEnfusionClutter.cpp
    TerrainWgpuEnfusionSurface.cpp
    WaterWgpu.cpp
    TextureWgpu.cpp
    TextureBankWgpu.cpp
    CdlodDriver.hpp
    EngineWgpu.hpp
    TerrainWgpu.hpp
    WaterWgpu.hpp
    TextureWgpu.hpp
    TextureBankWgpu.hpp
)

target_include_directories(WgpuRenderer PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Pinned local CPU library. Only explicit fixture BeginClod calls it;
# no default initializer, frame bake, worker or paging policy is enabled.
include("${CMAKE_SOURCE_DIR}/cmake/MeshoptimizerClod.cmake")
target_link_libraries(WgpuRenderer PUBLIC Poseidon PRIVATE wgpu_renderer OpenPoseidon::MeshoptimizerClod)

set_target_properties(WgpuRenderer PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    OUTPUT_NAME "WgpuRenderer"
)

function(wgpu_renderer_stage_dll APP_TARGET)
    set(_stamp ${CMAKE_CURRENT_BINARY_DIR}/${APP_TARGET}_stage_wgpu.stamp)
    # The cdylib's PDB (dbg/rwdi only — absent in rel) sits next to the .dll byproduct.
    # Deploy it alongside so sampling profilers (Superluminal) resolve Rust symbols.
    set(_wgpu_pdb $<TARGET_FILE_DIR:wgpu_renderer-shared>/wgpu_renderer.pdb)
    set(_copy_if_exists ${CMAKE_SOURCE_DIR}/cmake/CopyIfExists.cmake)
    add_custom_command(
        OUTPUT ${_stamp}
        # The app's output directory normally exists because the exe linked into it, but the
        # staging step no longer depends on the exe (see DEPENDS below), so it can run first.
        COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:${APP_TARGET}>
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:wgpu_renderer-shared> $<TARGET_FILE_DIR:${APP_TARGET}>
        COMMAND ${CMAKE_COMMAND} -E make_directory ${DIST_DIR}
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:wgpu_renderer-shared> ${DIST_DIR}
        COMMAND ${CMAKE_COMMAND} -DSRC=${_wgpu_pdb} -DDST=$<TARGET_FILE_DIR:${APP_TARGET}> -P ${_copy_if_exists}
        COMMAND ${CMAKE_COMMAND} -DSRC=${_wgpu_pdb} -DDST=${DIST_DIR} -P ${_copy_if_exists}
        COMMAND ${CMAKE_COMMAND} -E touch ${_stamp}
        # cargo-build_wgpu_renderer must be here. Depending only on the imported target and
        # its file did NOT re-stage: corrosion's cdylib is produced by a utility target, so
        # the generator has no rule making that path and the stamp was treated as up to date
        # even after cargo had rewritten the DLL. Observed on 2026-08-03 with a dist/ copy
        # from the previous day and a PoseidonGameDemo stamp twelve days stale — every build
        # reported success while shipping a renderer that predated the source.
        #
        # A stale cdylib is close to undetectable at runtime: the C++ side keeps working
        # because the FFI exports still resolve, and appended WgrWaterParams lanes are simply
        # ignored by the older DLL, so the symptom is "some settings work and the new ones do
        # nothing" rather than a crash.
        #
        # The cargo utility runs every build, so this makes the copy run every build too. It
        # is copy_if_different, so an unchanged DLL costs a content compare and no write.
        #
        # ${APP_TARGET} is deliberately NOT a dependency: the app now depends on this stamp
        # (see add_dependencies below) and listing it both ways is a cycle.
        DEPENDS $<TARGET_FILE:wgpu_renderer-shared> wgpu_renderer-shared cargo-build_wgpu_renderer
        COMMENT "Staging wgpu_renderer cdylib for ${APP_TARGET}"
        VERBATIM
    )
    add_custom_target(${APP_TARGET}_stage_wgpu ALL DEPENDS ${_stamp})
    # ALL alone is not enough. `cmake --build <dir> --target PoseidonGame` builds that target
    # and its dependencies, and an ALL target that nothing depends on is not among them — so
    # the documented way to build the game skipped staging entirely and left whatever DLL was
    # in dist/ from the last bare `cmake --build`. That shipped a renderer a day older than
    # the source, with every build reporting success.
    add_dependencies(${APP_TARGET} ${APP_TARGET}_stage_wgpu)

    # The Rust cdylib ships beside each game executable.  Linux's dynamic loader
    # does not search the executable directory by default, so make that packaged
    # layout an explicit runtime contract instead of relying on a build-tree path.
    if(UNIX AND NOT APPLE)
        set_property(TARGET ${APP_TARGET} APPEND PROPERTY BUILD_RPATH "$ORIGIN")
        set_property(TARGET ${APP_TARGET} APPEND PROPERTY INSTALL_RPATH "$ORIGIN")
    endif()
endfunction()
