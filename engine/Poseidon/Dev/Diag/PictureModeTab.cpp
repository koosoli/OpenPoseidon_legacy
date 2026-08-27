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

// Foundation/Framework/DebugLog.hpp -- reached through the engine headers above and through the
// precompiled header, so include ORDER cannot avoid it -- defines `DebugLog` as a function-like
// macro. ImGui declares a member of the same name (imgui.h:1146), and the macro eats it.
// BallisticsTab.cpp sidesteps this by including neither Global.hpp nor Engine.hpp; this tab needs
// both, so the macro is dropped instead. Nothing here logs.
#undef DebugLog

#include <imgui.h>

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

    changed |= ImGui::Checkbox("Enable depth of field", &dof.enabled);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("OFF costs nothing at all -- the renderer does not create the\n"
                          "pass, its target or its shader until this is ticked.\n"
                          "\n"
                          "It runs before bloom, so an out-of-focus highlight blooms as a\n"
                          "disc rather than a point. That is what makes bokeh read as bokeh.");
    }

    ImGui::BeginDisabled(!dof.enabled);

    ImGui::SetNextItemWidth(220.0f);
    changed |= ImGui::SliderFloat("Focus distance (m)", &dof.focusDistance, 0.5f, 500.0f, "%.1f",
                                  ImGuiSliderFlags_Logarithmic);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Distance the sharp band is centred on. Logarithmic, because the\n"
                          "useful range runs from arm's length to the horizon.");
    }

    ImGui::SetNextItemWidth(220.0f);
    changed |= ImGui::SliderFloat("Sharp band (+/- m)", &dof.focusRange, 0.0f, 100.0f, "%.1f",
                                  ImGuiSliderFlags_Logarithmic);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Half-width of the fully sharp zone. Everything within this many\n"
                          "metres of the focus distance is left completely untouched.\n"
                          "Set it to 0 for a knife-edge focal plane.");
    }

    ImGui::SetNextItemWidth(220.0f);
    changed |= ImGui::SliderFloat("Max blur (px)", &dof.maxBlurPixels, 0.0f, 64.0f, "%.0f");
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
    changed |= ImGui::SliderFloat("Background amount", &dof.backgroundScale, 0.0f, 2.0f, "%.2f");
    ImGui::SetNextItemWidth(220.0f);
    changed |= ImGui::SliderFloat("Foreground amount", &dof.foregroundScale, 0.0f, 2.0f, "%.2f");
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Behind and in front of the focal plane, separately.\n"
                          "\n"
                          "Foreground blur is far more intrusive than background blur -- a\n"
                          "soft distance reads as depth, a soft foreground reads as a\n"
                          "smeared lens. That is why these are two sliders and not one.");
    }

    ImGui::SetNextItemWidth(220.0f);
    changed |= ImGui::SliderFloat("Transition (1/m)", &dof.transition, 0.002f, 1.0f, "%.3f",
                                  ImGuiSliderFlags_Logarithmic);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("How quickly blur opens up past the sharp band.\n"
                          "Small = a long gentle falloff. Large = a hard cut, which is\n"
                          "what gives the tilt-shift / miniature look.");
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
    if (ImGui::Checkbox("Pause simulation (Ctrl+P)", &paused))
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

void DrawPictureModeTab()
{
    DrawDepthOfFieldSection();
    DrawCaptureSection();
}

} // namespace Poseidon::Dev
