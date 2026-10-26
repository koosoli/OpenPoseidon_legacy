// SPDX-License-Identifier: GPL-3.0-or-later
// The dev-panel "Conquest" tab. Everything is read from the referee's Snapshot, refreshed once a
// second; nothing here changes the war.

#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/Foundation/platform.hpp>

// The PCH pulls in Logging.hpp, which #defines DebugLog() as a macro and
// collides with the ImGui::DebugLog() method. Same dance as DebugOverlay.cpp.
#ifdef DebugLog
#undef DebugLog
#endif

#include <imgui.h>

#include <Poseidon/Dev/Diag/ConquestTab.hpp>
#include <Poseidon/Dev/Debug/DevPanelWidgets.hpp>
#include <Poseidon/Game/Conquest/ConquestReferee.hpp>

namespace Poseidon::Dev
{
namespace
{
const char* SideName(const Conquest::Snapshot& s, int side)
{
    return side >= 0 && side < static_cast<int>(s.sideNames.size()) ? s.sideNames[side].c_str() : "-";
}
} // namespace

void DrawConquestTab()
{
    const Conquest::Snapshot& s = Conquest::GetSnapshot();
    if (!s.active)
    {
        ImGui::TextWrapped("No Conquest war running. A mission turns the mode on with class CfgConquest in its "
                           "description.ext (see content/default-packs/conquest/missions/conquest-everon.Eden).");
        return;
    }
    const int t = static_cast<int>(s.now);
    ImGui::Text("Game time %02d:%02d:%02d   referee tick %.2f ms (max %.2f)", t / 3600, (t / 60) % 60, t % 60,
                s.lastTickMs, s.maxTickMs);

    PanelHeading("Sides");
    if (ImGui::BeginTable("conquest_sides", 7, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
    {
        for (const char* h : {"Side", "Pool", "Income/min", "Score", "Live/cap", "Lost", "Bought"})
            ImGui::TableSetupColumn(h);
        ImGui::TableHeadersRow();
        for (int side = 0; side < s.war.sideCount; side++)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%s%s", SideName(s, side), s.war.defeated[side] ? " (defeated)" : "");
            ImGui::TableNextColumn();
            ImGui::Text("%.0f", s.war.pool[side]);
            ImGui::TableNextColumn();
            ImGui::Text("%.0f", Conquest::IncomePerMinute(s.war, side));
            ImGui::TableNextColumn();
            ImGui::Text("%.0f", s.war.score[side]);
            ImGui::TableNextColumn();
            ImGui::Text("%d/%d", s.live[side], s.cap);
            ImGui::TableNextColumn();
            ImGui::Text("%d", s.losses[side]);
            ImGui::TableNextColumn();
            ImGui::Text("%d", s.bought[side]);
        }
        ImGui::EndTable();
    }

    PanelHeading("Objectives");
    if (ImGui::BeginTable("conquest_objectives", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
    {
        for (const char* h : {"Objective", "Kind", "Owner", "Bar", "State", "Value"})
            ImGui::TableSetupColumn(h);
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < s.war.defs.size(); i++)
        {
            const Conquest::ObjectiveDef& d = s.war.defs[i];
            const Conquest::ObjectiveState& o = s.war.states[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(d.name.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(Conquest::KindName(d.kind));
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(SideName(s, o.owner));
            ImGui::TableNextColumn();
            ImGui::Text("%s %3.0f%%", SideName(s, o.holder), o.level * 100.0f);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(o.contested ? "contested" : "");
            ImGui::TableNextColumn();
            ImGui::Text("%.0f", d.value);
        }
        ImGui::EndTable();
    }

    PanelHeading("Groups");
    if (ImGui::BeginTable("conquest_groups", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
    {
        for (const char* h : {"Group", "Side", "Men", "Task", "Position"})
            ImGui::TableSetupColumn(h);
        ImGui::TableHeadersRow();
        for (const Conquest::GroupSummary& g : s.groups)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%d", g.id);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(SideName(s, g.side));
            ImGui::TableNextColumn();
            ImGui::Text("%d", g.men);
            ImGui::TableNextColumn();
            const char* where = g.objective >= 0 && g.objective < static_cast<int>(s.war.defs.size())
                                    ? s.war.defs[g.objective].name.c_str()
                                    : "";
            ImGui::Text("%s %s%s", Conquest::TaskName(g.task), where, g.stalled ? " (stalled)" : "");
            ImGui::TableNextColumn();
            ImGui::Text("%.0f, %.0f", g.x, g.y);
        }
        ImGui::EndTable();
    }

    PanelHeading("War log (newest first)");
    int shown = 0;
    for (auto it = s.log.rbegin(); it != s.log.rend() && shown < 40; ++it, ++shown)
        ImGui::TextUnformatted(it->c_str());
}
} // namespace Poseidon::Dev
