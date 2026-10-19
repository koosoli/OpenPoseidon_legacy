# Audio backend: Valve Steam Audio (spatialisation, Apache-2.0) + SDL3 device
# I/O + an in-engine software mixer and EAX-preset reverb.  Also provides the
# voice-over-net capture / speaker / loopback backend on SDL3.

set(POSEIDON_STEAMAUDIO_SOURCES
    Mixer.cpp
    Occlusion.cpp
    Reverb.cpp
    SoundSystemSteam.cpp
    WaveSteam.cpp
    Voice/VoiceBackendSDL.cpp
)

set(POSEIDON_STEAMAUDIO_HEADERS
    EFXPresets.hpp
    Mixer.hpp
    Occlusion.hpp
    Reverb.hpp
    SoundSystemSteam.hpp
    StreamingChunks.hpp
    WaveSteam.hpp
    Voice/VoiceBackendSDL.hpp
)

add_library(PoseidonSteamAudio STATIC
    ${POSEIDON_STEAMAUDIO_SOURCES}
    ${POSEIDON_STEAMAUDIO_HEADERS}
)

# The vcpkg steam-audio config does not find_dependency() the libraries its
# core target links, so resolve them first.
find_package(flatbuffers CONFIG REQUIRED)
find_package(pffft CONFIG REQUIRED)
find_package(mysofa CONFIG REQUIRED)
find_package(steam-audio CONFIG REQUIRED)

target_link_libraries(PoseidonSteamAudio
    PUBLIC Poseidon
    # Mixer.hpp exposes IPL* types, so phonon headers are part of the interface.
    PUBLIC unofficial::steam-audio::phonon
)

target_include_directories(PoseidonSteamAudio PUBLIC
    ${CMAKE_SOURCE_DIR}/engine
    # Reverb preset table (EFX), vendored — see thirdparty/README.md.
    ${CMAKE_SOURCE_DIR}/thirdparty/efx-presets
)

target_compile_definitions(PoseidonSteamAudio PRIVATE
    $<$<CONFIG:Debug>:_DEBUG>
)

set_target_properties(PoseidonSteamAudio PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    OUTPUT_NAME "PoseidonSteamAudio"
)
