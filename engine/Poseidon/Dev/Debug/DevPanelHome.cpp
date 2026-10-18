// SPDX-License-Identifier: GPL-3.0-or-later
#include <Poseidon/Dev/Debug/DevPanelHome.hpp>

#include <Poseidon/Foundation/platform.hpp>
// The engine's DebugLog macro (precompiled prelude) collides with ImGui::DebugLog.
#undef DebugLog
#include <imgui.h>

#include <Poseidon/Dev/Debug/DevPanelWidgets.hpp>
#include <Poseidon/Dev/NpcChat/NpcChatSettings.hpp>

#include <SDL3/SDL_keyboard.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace Poseidon::Dev
{
namespace
{
struct Page
{
    const char* tab;
    const char* blurb;
};

struct Group
{
    const char* title;
    std::vector<Page> pages;
};

// Tabs not listed here still appear under "Other" (PanelTabLabels() knows every tab the bar
// drew), so a new tab is reachable from Home before anyone files it into a group.
const std::vector<Group>& Groups()
{
    static const std::vector<Group> groups = {
        {"Play and edit the world",
         {{"Zeus", "Free camera, spawn units and vehicles, weather and time"},
          {"Map Editor", "Paint terrain, place caves"},
          {"Earth", "Real-world terrain streaming"},
          {"Game", "Player position, coordinates, teleport"},
          {"Cheats", "God mode, ammo, save, skip"},
          {"Mods", "Mounted mods, reload"},
          {"NPC Chat", "EXPERIMENTAL: talk to and command NPCs through a language model"}}},
        {"Behaviour and simulation",
         {{"AI", "Changes what the AI does"},
          {"Ballistics", "Bullet recorder and aim model"},
          {"Physics", "Probes, colliders, penetration"},
          {"Flight", "Aircraft handling"},
          {"Fixed Step", "Simulation tick and interpolation"},
          {"Particles", "Smoke, fire, emitters"},
          {"Audio", "Audio engine and HRTF"}}},
        {"Look",
         {{"Graphics", "Renderer overview"},
          {"Render", "Render switches"},
          {"Picture Mode", "Depth of field, framing"},
          {"Tonemap", "Exposure and colour"},
          {"Sky", "Sky, clouds, sun and moon"},
          {"Weather", "Rain, snow, fog, wind"},
          {"Water", "Ocean, rivers, debug views"},
          {"Shadows", "Shadow maps and cascades"},
          {"Amb. Occlusion", "Ambient occlusion"},
          {"GI", "Global illumination probes"},
          {"Interior Sky", "Sky light indoors"},
          {"Grass", "Grass and ground cover"},
          {"Foliage", "Trees and bushes"},
          {"Material Debug", "Per-material inspection"},
          {"Culling", "Visibility culling"},
          {"Font", "UI font tuning"},
          {"Aspect", "Window aspect ratio"}}},
        {"Measure and debug",
         {{"Perf", "Frame timing"},
          {"Profile", "CPU profile samples"},
          {"Memory", "Heap and residency"},
          {"Streaming", "Asset streaming"},
          {"Console", "Run SQF"},
          {"Mouse", "Input diagnostics"}}},
    };
    return groups;
}

bool TabExists(const char* tab)
{
    for (const std::string& label : PanelTabLabels())
        if (label == tab)
            return true;
    return false;
}

/// A row of jump buttons that wraps to the window width.
void JumpButtons(const std::vector<std::pair<std::string, std::string>>& pages)
{
    const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    const ImGuiStyle& style = ImGui::GetStyle();
    bool first = true;
    for (const auto& [tab, blurb] : pages)
    {
        const float width = ImGui::CalcTextSize(tab.c_str()).x + style.FramePadding.x * 2.0f;
        if (!first)
        {
            const float next = ImGui::GetItemRectMax().x + style.ItemSpacing.x + width;
            if (next < right)
                PanelSameLine();
        }
        first = false;
        if (Button(tab.c_str()))
            RequestPanelTab(tab.c_str());
        if (!blurb.empty() && PanelItemHovered())
            ImGui::SetTooltip("%s", blurb.c_str());
    }
}
} // namespace

void DrawHomeTab()
{
    ImGui::Text("Dev panel");
    ImGui::TextDisabled("Jump to any page. Hover a button for what it holds; the search box above filters every tab.");
    ImGui::Separator();

    std::vector<std::string> listed;
    for (const Group& group : Groups())
    {
        std::vector<std::pair<std::string, std::string>> pages;
        for (const Page& page : group.pages)
        {
            listed.emplace_back(page.tab);
            if (TabExists(page.tab))
                pages.emplace_back(page.tab, page.blurb);
        }
        if (pages.empty())
            continue;
        ImGui::SeparatorText(group.title);
        ImGui::PushID(group.title);
        JumpButtons(pages);
        ImGui::PopID();
    }

    std::vector<std::pair<std::string, std::string>> other;
    for (const std::string& label : PanelTabLabels())
        if (label != "Home" && std::find(listed.begin(), listed.end(), label) == listed.end())
            other.emplace_back(label, "");
    if (!other.empty())
    {
        ImGui::SeparatorText("Other");
        ImGui::PushID("other");
        JumpButtons(other);
        ImGui::PopID();
    }

    ImGui::SeparatorText("Shortcuts");
    ImGui::BulletText("Ctrl+`  (Ctrl+; on Czech layouts, or Ctrl+F8): show / hide this panel");
    ImGui::BulletText("Ctrl+G: throw (G alone while the Zeus camera is active)");
    ImGui::BulletText("Ctrl+Shift+W: cycle the water debug view");
    ImGui::BulletText("Ctrl+Shift+I / O / B: interior sky on/off, reach view, baked volumes");
    const NpcChat::Settings& chat = NpcChat::CurrentSettings();
    if (chat.enabled)
        ImGui::BulletText("NPC chat: %s opens the chat box, hold %s to talk",
                          SDL_GetScancodeName(static_cast<SDL_Scancode>(chat.chatKey)),
                          SDL_GetScancodeName(static_cast<SDL_Scancode>(chat.talkKey)));
    else
        ImGui::BulletText("NPC chat: off (enable it on the NPC Chat page)");
}

} // namespace Poseidon::Dev
