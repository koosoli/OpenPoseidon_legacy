# CheckSimRenderBoundary.cmake — keep simulation independent of any concrete renderer.
# Usage: cmake -DREPO=<repo root> -P CheckSimRenderBoundary.cmake
#
# ARCH-001 audited this boundary and found it already holds: the dedicated server
# links GameBase + Poseidon and NOTHING else, five binaries run headless through
# CreateEngineDummy(), and no gameplay path reads renderer state. The property is
# real and valuable -- it is what makes headless servers, deterministic simulation
# and renderer replacement reachable -- but nothing was ENFORCING it, so it held by
# accident of the link graph and could erode one innocent #include at a time.
#
# This is that enforcement. It is deliberately a boundary check, not a style check:
# it fails only on things that would actually couple simulation to a backend.

cmake_policy(SET CMP0007 NEW)

if(NOT DEFINED REPO)
    message(FATAL_ERROR "REPO not defined")
endif()

set(failures "")

# --- Rule 1 -----------------------------------------------------------------
# Simulation must not include a concrete graphics/audio backend. It may use the
# Engine INTERFACE (Poseidon/Graphics/Core/Engine.hpp) as much as it likes -- that
# is the boundary, and the dummy backend satisfies it at zero cost.
set(sim_dirs
    "engine/Poseidon/World"
    "engine/Poseidon/AI"
    "engine/Poseidon/Game"
    "engine/Poseidon/Network"
)
# Headers that only a concrete backend may include.
set(backend_headers
    "wgpu_renderer.hpp"
    "wgpu_renderer.h"
    "EngineWgpu.hpp"
    "TerrainWgpu.hpp"
    "WaterWgpu.hpp"
    "TextureBankWgpu.hpp"
    "EngineGL33.hpp"
    "glad/glad.h"
    "GL/gl.h"
    "SDL_opengl.h"
    "AL/al.h"
    "AL/alc.h"
)

foreach(dir ${sim_dirs})
    file(GLOB_RECURSE sources "${REPO}/${dir}/*.cpp" "${REPO}/${dir}/*.hpp" "${REPO}/${dir}/*.inc")
    foreach(src ${sources})
        file(READ "${src}" content)
        foreach(header ${backend_headers})
            # Only real include directives; a header NAMED in a comment is fine and
            # is often exactly how one of these files explains the boundary.
            string(REGEX MATCH "\n[ \t]*#[ \t]*include[ \t]*[<\"][^>\"]*${header}[>\"]" hit "\n${content}")
            if(NOT hit STREQUAL "")
                file(RELATIVE_PATH rel "${REPO}" "${src}")
                list(APPEND failures "${rel} includes ${header} -- simulation may only use the Engine interface")
            endif()
        endforeach()
    endforeach()
endforeach()

# --- Rule 2 -----------------------------------------------------------------
# The dedicated server must not link a graphics or audio backend. This is the
# property that makes headless real, and it is one CMakeLists edit away from being
# lost silently (it would still build -- it would just stop being headless).
file(READ "${REPO}/apps/cwr/Server/CMakeLists.txt" server_cmake)
foreach(lib "PoseidonGL33" "PoseidonOpenAL" "WGPU_LIB" "wgpu_renderer")
    string(FIND "${server_cmake}" "${lib}" pos)
    if(NOT pos EQUAL -1)
        list(APPEND failures "apps/cwr/Server/CMakeLists.txt references ${lib} -- the dedicated server must stay headless")
    endif()
endforeach()

# --- Report -----------------------------------------------------------------
list(LENGTH failures n)
if(n GREATER 0)
    message("")
    message("Simulation/renderer boundary violations (${n}):")
    foreach(f ${failures})
        message("  ERROR: ${f}")
    endforeach()
    message("")
    message("See design notes")
    message("Simulation talks to rendering through Poseidon/Graphics/Core/Engine.hpp and the")
    message("SceneObjectCreated/Moved/Removed push channel -- never to a concrete backend.")
    message(FATAL_ERROR "Simulation/renderer boundary check failed")
endif()

message("Simulation/renderer boundary OK: no backend includes in World/AI/Game/Network, server links no backend.")
