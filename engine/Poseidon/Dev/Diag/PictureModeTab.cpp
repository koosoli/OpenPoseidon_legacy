// Picture Mode: the controls you want when composing a screenshot rather than playing.
//
// Everything here is OFF by default and none of it is a gameplay setting. It gets its own tab
// because these knobs have nothing to do with the diagnostics they would otherwise be buried
// among -- a focus distance is not a ballistics trace, and hunting for it in another tab would be
// its own small daily annoyance.

#include <Poseidon/Dev/Diag/PictureModeTab.hpp>

#include <Poseidon/Core/Global.hpp>
#include <Poseidon/Dev/Diag/DiagPause.hpp>
#include <Poseidon/Graphics/Core/Engine.hpp>
#include <Poseidon/World/World.hpp>
#include <Poseidon/World/Entities/Infantry/Person.hpp>
#include <Poseidon/World/Scene/Scene.hpp>
#include <Poseidon/World/Scene/Camera/Camera.hpp>
#include <Poseidon/World/Scene/ObjLine.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>
#include <algorithm>
#include <cmath>

// Foundation/Framework/DebugLog.hpp -- reached through the engine headers above and through the
// precompiled header, so include ORDER cannot avoid it -- defines `DebugLog` as a function-like
// macro. ImGui declares a member of the same name (imgui.h:1146), and the macro eats it.
// BallisticsTab.cpp sidesteps this by including neither Global.hpp nor Engine.hpp; this tab needs
// both, so the macro is dropped instead. Nothing here logs.
#undef DebugLog

#include <imgui.h>

#include <Poseidon/Dev/Debug/DevPanelWidgets.hpp>

namespace Poseidon::Dev
{
namespace
{

void DrawDepthOfFieldSection()
{
    ImGui::SeparatorText("Depth of field");

    if (!GEngine || !GEngine->SupportsDepthOfField())
    {
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "Not available on this backend");
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Depth of field is a wgpu post pass. Run with --render wgpu\n"
                              "(the default); GL33 has no post-processing chain.");
        }
        return;
    }

    Engine::DepthOfFieldSettings dof = GEngine->GetDepthOfFieldSettings();
    bool changed = false;

    ImGui::TextDisabled("Ctrl+click any slider to type an exact value.");

    changed |= Dev::Checkbox("Enable depth of field", &dof.enabled);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("OFF costs nothing at all -- the renderer does not create the\n"
                          "pass, its target or its shader until this is ticked.\n"
                          "\n"
                          "It runs before bloom, so an out-of-focus highlight blooms as a\n"
                          "disc rather than a point. That is what makes bokeh read as bokeh.");
    }

    ImGui::BeginDisabled(!dof.enabled);

    if (Dev::Checkbox("Keep player in focus", &dof.followPlayer))
    {
        if (dof.followPlayer) dof.focusOnClick = false;
        changed = true;
    }
    if (Dev::Checkbox("Click to focus (free-fly)", &dof.focusOnClick))
    {
        if (dof.focusOnClick) dof.followPlayer = false;
        changed = true;
    }
    Dev::PanelTooltip("Hide this panel and left-click a surface in free-fly. This replaces Zeus selection while enabled. Sky clicks keep the current focus.");

    ImGui::SetNextItemWidth(220.0f);
    changed |= Dev::SliderFloat("Focus distance (m)", &dof.focusDistance, 0.5f, 500.0f, "%.1f",
                                  ImGuiSliderFlags_Logarithmic);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Distance the sharp band is centred on. Logarithmic, because the\n"
                          "useful range runs from arm's length to the horizon.");
    }

    ImGui::SetNextItemWidth(220.0f);
    changed |= Dev::SliderFloat("Sharp band (+/- m)", &dof.focusRange, 0.0f, 100.0f, "%.1f",
                                  ImGuiSliderFlags_Logarithmic);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Half-width of the fully sharp zone. Everything within this many\n"
                          "metres of the focus distance is left completely untouched.\n"
                          "Set it to 0 for a knife-edge focal plane.");
    }

    ImGui::SetNextItemWidth(220.0f);
    changed |= Dev::SliderFloat("Max blur (px)", &dof.maxBlurPixels, 0.0f, 128.0f, "%.0f");
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Widest circle of confusion, in pixels at 1080p. The value is\n"
                          "scaled to the real render height, so a 4K screenshot is not\n"
                          "silently sharper than the same shot at 1080p.\n"
                          "\n"
                          "Past roughly 48 px the 32-sample gather begins to show as\n"
                          "structure in large smooth areas rather than as smooth blur.");
    }

    ImGui::SetNextItemWidth(220.0f);
    changed |= Dev::SliderFloat("Background amount", &dof.backgroundScale, 0.0f, 2.0f, "%.2f");
    ImGui::SetNextItemWidth(220.0f);
    changed |= Dev::SliderFloat("Foreground amount", &dof.foregroundScale, 0.0f, 2.0f, "%.2f");
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Behind and in front of the focal plane, separately.\n"
                          "\n"
                          "Foreground blur is far more intrusive than background blur -- a\n"
                          "soft distance reads as depth, a soft foreground reads as a\n"
                          "smeared lens. That is why these are two sliders and not one.");
    }

    ImGui::SetNextItemWidth(220.0f);
    changed |= Dev::SliderFloat("Transition (1/m)", &dof.transition, 0.002f, 1.0f, "%.3f",
                                  ImGuiSliderFlags_Logarithmic);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("How quickly blur opens up past the sharp band.\n"
                          "Small = a long gentle falloff. Large = a hard cut, which is\n"
                          "what gives the tilt-shift / miniature look.");
    }

    ImGui::SeparatorText("Bokeh");

    ImGui::SetNextItemWidth(220.0f);
    changed |= Dev::SliderFloat("Highlight boost", &dof.bokehBoost, 0.0f, 12.0f, "%.1f");
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
                          "0 gives a flat average, and a bright out-of-focus point comes out\n"
                          "as a faint smear -- its energy divided over the whole disc.\n"
                          "\n"
                          "Raising it lets bright samples outweigh their neighbours, so a\n"
                          "highlight prints as a bright DISC instead. That is what reads as\n"
                          "photographic bokeh.\n"
                          "\n"
                          "It works because this pass runs in linear HDR before the tonemap,\n"
                          "where a specular still carries a value above 1 rather than having\n"
                          "been clipped to white. After the tonemap there would be nothing\n"
                          "left to boost.");
    }

    ImGui::SetNextItemWidth(220.0f);
    changed |= Dev::SliderFloat("Highlight threshold", &dof.bokehThreshold, 0.0f, 4.0f, "%.2f");
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
                          "Linear HDR luminance above which a sample counts as a highlight.\n"
                          "Lower it until the discs appear; raise it if the whole image\n"
                          "starts to sparkle rather than just the bright spots.");
    }

    ImGui::SetNextItemWidth(220.0f);
    changed |= Dev::SliderFloat("Aperture blades", &dof.apertureBlades, 0.0f, 9.0f, "%.0f");
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
                          "0 = a perfect circle, as a mirror lens gives.\n"
                          "5..9 = a polygon, which is what a real iris prints: out of focus a\n"
                          "point takes the APERTURE shape, not a circle. 6 is the classic\n"
                          "hexagon.\n"
                          "\n"
                          "Below 3 blades the polygon is meaningless and the disc is kept.");
    }

    ImGui::Separator();

    // Quality. This is the entire cost of the pass, so it is a control and not a constant.
    static const char* qualityNames[] = {"Low (16)", "Medium (32)", "High (48)", "Ultra (96)"};
    static const int qualityCounts[] = {16, 32, 48, 96};
    int qualityIndex = 2;
    for (int i = 0; i < 4; ++i)
    {
        if (dof.sampleCount <= qualityCounts[i])
        {
            qualityIndex = i;
            break;
        }
    }
    ImGui::SetNextItemWidth(220.0f);
    if (Dev::Combo("Quality", &qualityIndex, qualityNames, 4))
    {
        dof.sampleCount = qualityCounts[qualityIndex];
        changed = true;
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Samples per pixel, and the entire cost of the effect: each one is a\n"
                          "colour fetch and a depth fetch. Ultra at 1080p is roughly 200 million\n"
                          "texture reads a frame -- fine for a paused screenshot, not for playing.\n"
                          "\n"
                          "Too few and the sampling spiral shows as noise in smooth gradients\n"
                          "such as sky. Raise it before you blame the blur.");
    }

    ImGui::Separator();
    // Starting points, not presets to live in: a focus distance only means something for the shot
    // actually being framed. They exist so the first click produces something recognisable rather
    // than a screen that looks broken.
    if (ImGui::Button("Portrait"))
    {
        dof.focusDistance = 6.0f;
        dof.focusRange = 1.5f;
        dof.maxBlurPixels = 32.0f;
        dof.backgroundScale = 1.0f;
        dof.foregroundScale = 0.8f;
        dof.transition = 0.12f;
        dof.sampleCount = 48;
        dof.bokehBoost = 5.0f;
        dof.apertureBlades = 6.0f;
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Landscape"))
    {
        dof.focusDistance = 120.0f;
        dof.focusRange = 60.0f;
        dof.maxBlurPixels = 16.0f;
        dof.backgroundScale = 0.7f;
        dof.foregroundScale = 1.0f;
        dof.transition = 0.01f;
        dof.sampleCount = 48;
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Miniature"))
    {
        // Tilt-shift-ish: a narrow band with a hard transition, which is what makes a real
        // landscape read as a model.
        dof.focusDistance = 40.0f;
        dof.focusRange = 6.0f;
        dof.maxBlurPixels = 48.0f;
        dof.backgroundScale = 1.4f;
        dof.foregroundScale = 1.4f;
        dof.transition = 0.20f;
        dof.sampleCount = 96; // wide discs need the samples or the spiral shows
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset"))
    {
        dof = Engine::DepthOfFieldSettings{};
        dof.enabled = true;
        changed = true;
    }

    ImGui::EndDisabled();

    if (changed)
    {
        GEngine->SetDepthOfFieldSettings(dof);
    }
}

void DrawCaptureSection()
{
    ImGui::SeparatorText("Composing");

    const bool available = DiagPauseAvailable();
    bool paused = DiagPauseActive();
    ImGui::BeginDisabled(!available);
    if (Dev::Checkbox("Pause simulation (Ctrl+P)", &paused))
    {
        SetDiagPause(paused);
    }
    ImGui::EndDisabled();
    if (!available)
    {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "unavailable");
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("No world loaded, or this is a network game.\n"
                              "World::IsSimulationEnabled takes a different branch in\n"
                              "GModeNetware and never reads the local pause, so pausing\n"
                              "here would do nothing (and must not: it would desync).");
        }
    }

    ImGui::TextDisabled("Free-fly: Zeus tab -> Enable free camera, then fly with the");
    ImGui::TextDisabled("movement keys. Pausing and flying work in either order, so a");
    ImGui::TextDisabled("frozen scene can be walked around and framed from any angle.");
}

} // namespace

void UpdatePictureModeFocus()
{
    if (!GEngine || !GWorld || !GScene || !GScene->GetCamera()) return;
    auto dof = GEngine->GetDepthOfFieldSettings();
    if (!dof.enabled || !dof.followPlayer) return;
    const auto* player = GWorld->GetRealPlayer();
    if (!player) return;
    const Camera& camera = *GScene->GetCamera();
    const float depth = (player->Position() + Vector3(0, 1, 0) - camera.Position()) * camera.Direction();
    if (std::isfinite(depth) && depth > camera.Near())
    {
        dof.focusDistance = depth;
        GEngine->SetDepthOfFieldSettings(dof);
    }
}

bool PictureModeClickFocusEnabled()
{
    if (!GEngine) return false;
    const auto dof = GEngine->GetDepthOfFieldSettings();
    return dof.enabled && dof.focusOnClick;
}

bool FocusPictureModeAtPixel(float x, float y)
{
    if (!PictureModeClickFocusEnabled() || !GLandscape || !GScene || !GScene->GetCamera()) return false;
    const Camera& camera = *GScene->GetCamera();
    const Matrix4& projection = camera.Projection();
    if (std::abs(projection(0, 0)) < 1e-6f || std::abs(projection(1, 1)) < 1e-6f) return false;
    const float sx = (x - projection(0, 2)) / projection(0, 0);
    const float sy = (y - projection(1, 2)) / projection(1, 1);
    const Vector3 direction = (camera.Direction() + camera.DirectionAside() * sx * camera.Left() +
                              camera.DirectionUp() * sy * camera.Top()).Normalized();
    const Vector3 origin = camera.Position();
    const float reach = std::min(camera.Far(), 50000.0f);
    Vector3 hit;
    float distance = GLandscape->IntersectWithGroundOrSea(&hit, origin, direction, 0, reach);
    bool found = distance >= 0;
    if (found) distance = (hit - origin).Size();
    CollisionBuffer objects;
    GLandscape->ObjectCollision(objects, nullptr, nullptr, origin, origin + direction * reach, 0, ObjIntersectView);
    for (int i = 0; i < objects.Size(); ++i)
    {
        const float t = objects[i].under;
        if (t >= 0 && t <= 1 && (!found || t * reach < distance))
        {
            distance = t * reach;
            found = true;
        }
    }
    const float depth = distance * (direction * camera.Direction());
    if (!found || !std::isfinite(depth) || depth <= camera.Near()) return false;
    auto dof = GEngine->GetDepthOfFieldSettings();
    dof.focusDistance = depth;
    GEngine->SetDepthOfFieldSettings(dof);
    return true;
}

void ResetPictureModeSettings()
{
    if (GEngine && GEngine->SupportsDepthOfField())
    {
        // Off, and back to the struct's own defaults. Depth of field is off by
        // default for a reason -- it is a screenshot tool, not a play setting.
        GEngine->SetDepthOfFieldSettings(Engine::DepthOfFieldSettings{});
    }
}

void DrawPictureModeTab()
{
    DrawDepthOfFieldSection();
    DrawCaptureSection();
}

} // namespace Poseidon::Dev
