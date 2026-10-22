// Unattended walk through an Arma 2 campaign, to check the campaign flow: which mission
// follows which end, with the campaign's description.ext. The ordinary test mode
// (--arma2-test-seconds) plays a mission alone and never reaches it.
//
// OP_A2_CAMPAIGN="<seconds>[,<end>[,<missions>]]", with --arma2-test-seconds 0 (played mode):
//   - intros, outros and award cutscenes are skipped after a few seconds,
//   - the briefing is confirmed,
//   - each mission is ended with endMission "<end>" (default END1) after <seconds> s
//     (wall clock, from the moment the mission display opens); <end> may be a sequence
//     "end2:end1:loser" -- the n-th mission takes the n-th end, the last one repeats,
//   - the debriefing is confirmed, and the campaign goes on to its next mission,
//   - after <missions> missions (default 2) the game quits.
// Each step is logged ("Arma 2 campaign auto: ..."). Nothing here runs without the variable.

#include <Poseidon/Core/Application.hpp>
#include <Poseidon/Core/Global.hpp>
#include <Poseidon/Core/resincl.hpp>
#include <Poseidon/Foundation/Framework/AppFrame.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/Game/Compat/Arma1Compat.hpp>
#include <Poseidon/UI/Controls/UIControls.hpp>
#include <Poseidon/UI/Map/UIMapBase.hpp>
#include <Poseidon/World/World.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

using namespace Poseidon;

namespace
{
struct CampaignAuto
{
    bool on = false;
    float seconds = 30.0f;
    std::vector<std::pair<EndMode, std::string>> ends{{EMEnd1, "end1"}}; // by mission, last repeats
    int missions = 2;
};

bool ParseEnd(std::string end, std::pair<EndMode, std::string>& out)
{
    for (char& ch : end)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (end.size() == 4 && end.rfind("end", 0) == 0 && end[3] >= '1' && end[3] <= '6')
    {
        out = {static_cast<EndMode>(EMEnd1 + (end[3] - '1')), end};
        return true;
    }
    if (end == "loser" || end == "lost")
    {
        out = {EMLoser, "loser"};
        return true;
    }
    return false;
}

const CampaignAuto& Settings()
{
    static const CampaignAuto settings = [] {
        CampaignAuto s;
        const char* e = std::getenv("OP_A2_CAMPAIGN");
        if (!e || !*e)
            return s;
        s.on = true;
        std::string v = e;
        const size_t c1 = v.find(',');
        const float secs = static_cast<float>(std::atof(v.substr(0, c1).c_str()));
        if (secs > 0.0f)
            s.seconds = secs;
        if (c1 != std::string::npos)
        {
            const size_t c2 = v.find(',', c1 + 1);
            const std::string list = v.substr(c1 + 1, c2 == std::string::npos ? std::string::npos : c2 - c1 - 1);
            std::vector<std::pair<EndMode, std::string>> ends;
            size_t pos = 0;
            while (pos <= list.size())
            {
                const size_t colon = list.find(':', pos);
                std::pair<EndMode, std::string> one;
                if (ParseEnd(list.substr(pos, colon == std::string::npos ? std::string::npos : colon - pos), one))
                    ends.push_back(one);
                if (colon == std::string::npos)
                    break;
                pos = colon + 1;
            }
            if (!ends.empty())
                s.ends = ends;
            if (c2 != std::string::npos)
            {
                const int n = std::atoi(v.substr(c2 + 1).c_str());
                if (n > 0)
                    s.missions = n;
            }
        }
        return s;
    }();
    return settings;
}

const char* DisplayName(int idd)
{
    switch (idd)
    {
        case IDD_MAIN: return "main menu";
        case IDD_INTRO: return "intro";
        case IDD_OUTRO: return "outro";
        case IDD_AWARD: return "award";
        case IDD_INTEL_GETREADY: return "briefing";
        case IDD_MISSION: return "mission";
        case IDD_DEBRIEFING: return "debriefing";
        case IDD_MISSION_END: return "mission end (killed)";
        case IDD_CAMPAIGN: return "campaign";
        case IDD_CAMPAIGN_LOAD: return "campaign book";
        default: return "display";
    }
}
} // namespace

bool Arma2CampaignAutoActive()
{
    return Settings().on;
}

// Called once per frame from World::Simulate, after the menus had their turn.
void Arma2CampaignAutoStep()
{
    const CampaignAuto& s = Settings();
    if (!s.on || !GWorld || !GApp)
        return;
    Display* disp = CurrentDisplay();
    if (!disp)
        return;

    static Display* seen = nullptr;
    static int seenIdd = -1;
    static DWORD since = 0;
    static bool ended = false;
    static int missionsPlayed = 0;
    const DWORD now = Poseidon::Foundation::GlobalTickCount();
    const int idd = disp->IDD();
    if (disp != seen || idd != seenIdd)
    {
        seen = disp;
        seenIdd = idd;
        since = now;
        ended = false;
        if (idd == IDD_MISSION)
        {
            GWorld->ForceEnd(false); // set by this mode to end the previous mission
            if (missionsPlayed >= s.missions)
            {
                LOG_INFO(Core, "Arma 2 campaign auto: {} missions played, quitting before '{}.{}'", missionsPlayed,
                         (const char*)Glob.header.filename, (const char*)Glob.header.worldname);
                ended = true;
                GApp->m_exitCode = 0;
                GApp->m_closeRequest = true;
                return;
            }
            ++missionsPlayed;
        }
        LOG_INFO(Core, "Arma 2 campaign auto: {} ({}) of '{}.{}'", DisplayName(idd), idd,
                 (const char*)Glob.header.filename, (const char*)Glob.header.worldname);
    }
    if (ended)
        return; // acted on this display already; it closes on its own
    const DWORD shown = now - since;
    switch (idd)
    {
        case IDD_INTRO:
        case IDD_OUTRO:
        case IDD_AWARD:
            if (shown >= 6000)
            {
                LOG_INFO(Core, "Arma 2 campaign auto: {} skipped after 6 s", DisplayName(idd));
                ended = true;
                disp->Exit(IDC_CANCEL);
            }
            break;
        case IDD_INTEL_GETREADY:
            if (shown >= 3000)
            {
                LOG_INFO(Core, "Arma 2 campaign auto: briefing confirmed");
                ended = true;
                disp->Exit(IDC_OK);
            }
            break;
        case IDD_DEBRIEFING:
            if (shown >= 3000)
            {
                LOG_INFO(Core, "Arma 2 campaign auto: debriefing confirmed (end {})",
                         static_cast<int>(GWorld->GetEndMode()));
                ended = true;
                disp->Exit(IDC_CANCEL); // "Continue": on to the outro and the next mission
            }
            break;
        case IDD_MISSION:
        {
            // wall-clock seconds: a mission that simulates slowly (C0 at ~1 tick/s) still ends
            const float played = static_cast<float>(shown) / 1000.0f;
            if (played >= s.seconds && GWorld->GetEndMode() == EMContinue)
            {
                ended = true;
                const size_t which = std::min<size_t>(static_cast<size_t>(std::max(missionsPlayed - 1, 0)), s.ends.size() - 1);
                const auto& [endMode, endName] = s.ends[which];
                LOG_INFO(Core, "Arma 2 campaign auto: endMission \"{}\" after {:.0f} s (wall clock) of '{}.{}'", endName, played,
                         (const char*)Glob.header.filename, (const char*)Glob.header.worldname);
                GWorld->SetEndMode(endMode);
                GWorld->ForceEnd(true); // through a running camera or title effect
            }
            break;
        }
        case IDD_MISSION_END:
            if (shown >= 2000)
            {
                LOG_INFO(Core, "Arma 2 campaign auto: player killed in '{}.{}', quitting",
                         (const char*)Glob.header.filename, (const char*)Glob.header.worldname);
                ended = true;
                GApp->m_exitCode = 2;
                GApp->m_closeRequest = true;
            }
            break;
        case IDD_MAIN:
            if (missionsPlayed > 0 && shown >= 2000)
            {
                LOG_INFO(Core, "Arma 2 campaign auto: back at the main menu after {} mission(s), quitting",
                         missionsPlayed);
                ended = true;
                GApp->m_exitCode = 4;
                GApp->m_closeRequest = true;
            }
            break;
        default:
            if (shown >= 60000)
            {
                LOG_ERROR(Core, "Arma 2 campaign auto: stuck at {} ({}) for 60 s, quitting", DisplayName(idd), idd);
                ended = true;
                GApp->m_exitCode = 3;
                GApp->m_closeRequest = true;
            }
            break;
    }
}
