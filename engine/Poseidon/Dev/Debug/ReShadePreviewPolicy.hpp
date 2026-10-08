// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>

// Optional add-on controls, independent of the engine renderer ABI. A UI
// selection is session-only and never loads/downloads a new external effect.
namespace OpReShadePreview
{
enum : unsigned { Off, ColorLook, SceneDepth, DhDepth, DhGi, External };
constexpr bool UsesDepth(unsigned mode) { return mode >= SceneDepth && mode <= DhGi; }
// Use actual scene/output target sizes and launch-active MSAA, never the legacy
// GL-style preference getters or a pending next-launch sample-count request.
constexpr bool SupportsDepthSettings(bool hdr, int renderWidth, int renderHeight,
                                     int outputWidth, int outputHeight, int activeSamples)
{
    return hdr && renderWidth > 0 && renderHeight > 0 && outputWidth > 0 && outputHeight > 0 &&
        renderWidth == outputWidth && renderHeight == outputHeight &&
        (activeSamples == 1 || activeSamples == 4);
}

constexpr const char* Preset(unsigned mode)
{
    constexpr const char* paths[] = {"", "OpenPoseidon.ini", "Presets/OpenPoseidon-Depth-Scene-Probe.ini",
        "Presets/OpenPoseidon-DH-Scene-Debug.ini", "Presets/OpenPoseidon-DH-Scene-Preview.ini"};
    return mode <= DhGi ? paths[mode] : "";
}
struct Asset { const char* path; const char* digest; };
inline constexpr std::array<Asset, 7> DepthAssets{{
    {"Shaders/OpenPoseidonDepthProbe.fx", "1496367bb8ff05527608037921864302746a2b53141b8ca9924ec6db3c5a7393"},
    {"Shaders/OpenPoseidonDH.fx", "c9be672baabc3814ee19183bbadd3e131ba51889b131691f6c9614576eacca98"},
    {"Shaders/OpenPoseidonSceneDepth.fxh", "5d59fbc169f0457e394f7f30f380a0884912d9e6d48d95ae6955a53d94acf03a"},
    {"Shaders/ReShade.fxh", "6dabfbbaf968c3871905d2ea17f96572ff7b1cec01310b5d0e5252b66b30174f"},
    {"Sources/dh_uber_rt.fx", "c14f19eb7b0359cb4042749e52c28c6a18c2df89b9fd40f5ab9bd209e36e5320"},
    {"Licenses/DH-UBER-RT-GPL-2.0.txt", "8177f97513213526df2cf6184d8ff986c675afb514d4e68a404010521b880643"},
    {"Presets/OpenPoseidon-Depth-Scene-Probe.ini", "4d86a36a5645a3e80c40b99f4f9286c147331ce353ceb420cd359acb7cfad25f"}
}};
inline constexpr Asset DhDebugPreset{"Presets/OpenPoseidon-DH-Scene-Debug.ini", "e3930a361201673b79cabebf798b4ec3b10dd2222d6b46412e2e8be0f79afd37"};
inline constexpr Asset DhGiPreset{"Presets/OpenPoseidon-DH-Scene-Preview.ini", "44d1a1428a7f7558b2e28c9f11cc2cba1c3869af5c446df53babbc77beeeddd5"};
}
