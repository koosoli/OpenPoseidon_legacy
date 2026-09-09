#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

// THE POINT OF THIS FILE.
//
// The dev panel's search filters controls at the draw call, which is what lets
// it cover a new setting with nobody registering it anywhere. That only holds
// while every control actually goes through the wrapper. A raw
// ImGui::SliderFloat still compiles, still draws, and still works -- it is just
// invisible to the search, and invisible in a way that shows up as "the filter
// cannot find a setting I know exists", months later, with no error anywhere.
//
// So the rule is enforced here rather than left to reviewers. If this fails,
// the fix is to call the Dev:: wrapper, not to relax the test.
namespace
{
std::filesystem::path RepoFile(const char* relative)
{
    // Walked rather than counted: a fixed number of parent_path() calls is
    // silently wrong the day this file moves a directory.
    for (std::filesystem::path at = std::filesystem::path(__FILE__).parent_path(); !at.empty(); at = at.parent_path())
    {
        const std::filesystem::path candidate = at / relative;
        if (std::filesystem::is_regular_file(candidate))
            return candidate;
        if (at == at.root_path())
            break;
    }
    return {};
}

std::string ReadTextFile(const std::filesystem::path& p)
{
    std::ifstream f(p, std::ios::binary);
    if (!f.is_open())
        return {};
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

/// Line numbers of every occurrence, so a failure names the place to fix.
std::vector<int> LinesContaining(const std::string& src, const std::string& needle)
{
    std::vector<int> hits;
    std::stringstream ss(src);
    std::string line;
    int number = 0;
    while (std::getline(ss, line))
    {
        ++number;
        if (line.find(needle) != std::string::npos)
            hits.push_back(number);
    }
    return hits;
}
} // namespace

TEST_CASE("Dev panel controls all go through the search filter", "[dev][panel]")
{
    // Every file that draws panel controls, not just the big one. The Smoke tab
    // lives in its own translation unit and was invisible to the search for
    // exactly as long as this test only knew about DebugOverlay.cpp -- which is
    // the failure mode the whole design exists to prevent, reappearing one
    // directory over. A new tab in a new file must be added here.
    const char* const panelSources[] = {
        "engine/Poseidon/Dev/Debug/DebugOverlay.cpp",
        "engine/Poseidon/Dev/Diag/SmokeTab.cpp",
        "engine/Poseidon/Dev/Diag/MapEditorTab.cpp",
    };
    const std::filesystem::path overlay = RepoFile(panelSources[0]);
    INFO("searched upward from " << std::filesystem::path(__FILE__).parent_path().string()
                                 << " for engine/Poseidon/Dev/Debug/DebugOverlay.cpp");
    REQUIRE(std::filesystem::is_regular_file(overlay));
    std::string src;
    for (const char* relative : panelSources)
    {
        const std::filesystem::path at = RepoFile(relative);
        INFO("panel source " << relative);
        REQUIRE(std::filesystem::is_regular_file(at));
        src += ReadTextFile(at);
    }
    REQUIRE_FALSE(src.empty());

    // Every widget type the wrapper covers. ImGui::Text and ImGui::TextColored
    // are deliberately NOT here: those are live readouts and status lines, not
    // settings, and a frame-time number should not vanish because you searched
    // for something else.
    const char* const wrapped[] = {
        "ImGui::SliderFloat(",     "ImGui::SliderInt(",  "ImGui::Checkbox(",       "ImGui::Combo(",
        "ImGui::ColorEdit3(",      "ImGui::Button(",     "ImGui::SmallButton(",    "ImGui::RadioButton(",
        "ImGui::TextUnformatted(", "ImGui::SameLine(",   "ImGui::InputText(",      "ImGui::InputTextWithHint(",
        "ImGui::InputInt(",        "ImGui::BeginCombo(", "ImGui::SetItemTooltip(", "ImGui::IsItemHovered(",
    };
    for (const char* raw : wrapped)
    {
        const std::vector<int> hits = LinesContaining(src, raw);
        std::string where;
        for (const int line : hits)
            where += " line " + std::to_string(line);
        INFO("call " << raw << " through the Dev:: wrapper instead, at" << where);
        CHECK(hits.empty());
    }

    // The search box has to run before the tab bodies: it resets the per-frame
    // state the wrappers use to attribute help text to the control above it.
    const size_t search = src.find("Dev::DrawPanelSearch();");
    const size_t tabBar = src.find("ImGui::BeginTabBar(");
    // Tabs go through the wrapper too, so POSEIDON_PANEL_TAB can reach all of them.
    CHECK(LinesContaining(src, "ImGui::BeginTabItem(").empty());
    INFO("DrawPanelSearch must be called, and before the tab bar");
    CHECK(search != std::string::npos);
    CHECK(tabBar != std::string::npos);
    CHECK(search < tabBar);
}

TEST_CASE("Dev panel help text is searchable, not just labels", "[dev][panel]")
{
    // The panel's knowledge is in the explanations ("above ~8x the old flicker
    // returns"), so a search over labels alone would miss most of what someone
    // is looking for. PanelHelp records its text against the control's ID; this
    // pins that it still does, and that the panel uses it.
    const std::filesystem::path widgets = RepoFile("engine/Poseidon/Dev/Debug/DevPanelWidgets.cpp");
    REQUIRE(std::filesystem::is_regular_file(widgets));
    const std::string src = ReadTextFile(widgets);
    CHECK(src.find("stored += text;") != std::string::npos);
    CHECK(src.find("RememberForSearch(text);") != std::string::npos);
    CHECK(src.find("g_filter.PassFilter(it->second.c_str())") != std::string::npos);

    const std::filesystem::path overlay = RepoFile("engine/Poseidon/Dev/Debug/DebugOverlay.cpp");
    REQUIRE(std::filesystem::is_regular_file(overlay));
    const std::string panel = ReadTextFile(overlay);
    // Explanations must go through PanelHelp to be indexed. A handful of
    // ImGui::TextDisabled remain on purpose: they are table CELLS, whose content
    // would blank out if it were hidden with a filtered control.
    const std::vector<int> raw = LinesContaining(panel, "ImGui::TextDisabled(");
    INFO("ImGui::TextDisabled should be Dev::PanelHelp except inside table cells");
    CHECK(raw.size() <= 8);
    CHECK(LinesContaining(panel, "Dev::PanelHelp(").size() > 200);
}
