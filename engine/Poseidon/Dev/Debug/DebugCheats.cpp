
#include <Poseidon/Dev/Debug/DebugCheats.hpp>
#include <Poseidon/Dev/Debug/DebugCommands.hpp>
#include <Poseidon/Game/Mission/MissionInfo.hpp>
#include <Poseidon/World/World.hpp>
#include <Poseidon/AI/AI.hpp>
#include <Poseidon/Foundation/Logging/Logging.hpp>
#include <Poseidon/Core/Global.hpp>
#include <Poseidon/IO/Streams/QBStream.hpp>

#include <cstring>
#include <SDL3/SDL_error.h>
#include <stdio.h>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/Foundation/Math/Math3D.hpp>
#include <Poseidon/Foundation/Math/Math3DP.hpp>
#include <Poseidon/Foundation/Strings/RString.hpp>
// Forward-declare both helpers rather than pulling in optionsUICommon.hpp
// (which drags in displayUI.hpp and breaks isolation).  IDS_SAVE_GAME is
// a runtime-initialized int registered by stringtableExt.cpp from
// stringids.hpp's STRING(SAVE_GAME).
namespace Poseidon
{
RString GetSaveDirectory();
}

// Save-path normalization — tri's isolated test config can produce a
// GetSaveDirectory() result like
//   "C:\...\.tmpXYZ\Users\josef\Saved\C:\...\.tmpXYZ\mission-smoke\...\"
// where an absolute drive-letter path appears embedded mid-string.
// Windows CreateFile silently resolves the second drive prefix and
// SaveBin succeeds, but ParamArchiveLoad::LoadBin treats it as the
// literal path and fails to open the file.  This normalizer finds the
// LAST "X:" occurrence and trims everything before it, recovering the
// path SaveBin actually wrote to so LoadBin can find the same file.
//
// No-op on a normal path (no embedded ':' past offset 2).
static RString NormalizeSavePath(RString name)
{
    const char* s = (const char*)name;
    if (!s)
        return name;
    int lastDriveAt = -1;
    int len = (int)strlen(s);
    // Start at index 2 — we expect the FIRST "X:" near the start, and
    // we want to detect any EMBEDDED later occurrence.
    for (int i = 2; i + 1 < len; i++)
    {
        if (s[i + 1] == ':' && ((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= 'a' && s[i] <= 'z')))
            lastDriveAt = i;
    }
    if (lastDriveAt <= 0)
        return name;
    return RString(s + lastDriveAt);
}
extern int IDS_SAVE_GAME;
extern int IDS_LOAD_GAME;
namespace Poseidon
{
bool DebugUnlockAllCampaigns();
}

namespace Poseidon::Dev
{
namespace DebugCheats
{
namespace
{
bool MultiplayerSession()
{
    return GWorld && GWorld->GetMode() == GModeNetware;
}
} // namespace


// Cmd_God — sticky toggle.  IsActive() is hot-pathed (queried by
// Object::SetDammage for the real player on every damage event) so
// it checks the live session as well as the toggle. SetActive logs the transition.
namespace Cmd_God
{
namespace
{
bool s_active = false;
} // namespace

bool Available()
{
    return !MultiplayerSession() && MissionInfo::IsActive();
}

bool IsActive()
{
    return s_active && !MultiplayerSession();
}

void SetActive(bool active)
{
    active = active && !MultiplayerSession();
    if (s_active == active)
        return;
    s_active = active;
    LOG_INFO(Mission, "DebugCheats::God -> {}", active ? "ON" : "OFF");
}

void Invoke(std::string_view args, std::string& out)
{
    if (MultiplayerSession())
    {
        out = "developer cheats disabled in multiplayer";
        return;
    }
    // Accept "1"/"0"/"on"/"off"/"true"/"false"/"" (the bare command toggles).
    bool target = !s_active;
    if (args == "1" || args == "on" || args == "true")
        target = true;
    else if (args == "0" || args == "off" || args == "false")
        target = false;
    else if (!args.empty())
    {
        out = "god: expected 1|0|on|off (or empty to toggle), got: " + std::string(args);
        return;
    }
    SetActive(target);
    out = std::string("god: ") + (s_active ? "ON" : "OFF");
}
} // namespace Cmd_God

// Cmd_InfiniteAmmo — sticky toggle.  Same shape as Cmd_God; refunds
// the burst in EntityAI::FireWeapon when the player fires.
namespace Cmd_InfiniteAmmo
{
namespace
{
bool s_active = false;
} // namespace

bool Available()
{
    return !MultiplayerSession() && MissionInfo::IsActive();
}
bool IsActive()
{
    return s_active && !MultiplayerSession();
}

void SetActive(bool active)
{
    active = active && !MultiplayerSession();
    if (s_active == active)
        return;
    s_active = active;
    LOG_INFO(Mission, "DebugCheats::InfiniteAmmo -> {}", active ? "ON" : "OFF");
}

void Invoke(std::string_view args, std::string& out)
{
    if (MultiplayerSession())
    {
        out = "developer cheats disabled in multiplayer";
        return;
    }
    bool target = !s_active;
    if (args == "1" || args == "on" || args == "true")
        target = true;
    else if (args == "0" || args == "off" || args == "false")
        target = false;
    else if (!args.empty())
    {
        out = "infammo: expected 1|0|on|off (or empty to toggle), got: " + std::string(args);
        return;
    }
    SetActive(target);
    out = std::string("infammo: ") + (s_active ? "ON" : "OFF");
}
} // namespace Cmd_InfiniteAmmo

// Cmd_InfiniteFuel — sticky toggle.  Refund inside Transport::ConsumeFuel
// when the player is the firing vehicle's occupant.
namespace Cmd_InfiniteFuel
{
namespace
{
bool s_active = false;
} // namespace

bool Available()
{
    return !MultiplayerSession() && MissionInfo::IsActive();
}
bool IsActive()
{
    return s_active && !MultiplayerSession();
}

void SetActive(bool active)
{
    active = active && !MultiplayerSession();
    if (s_active == active)
        return;
    s_active = active;
    LOG_INFO(Mission, "DebugCheats::InfiniteFuel -> {}", active ? "ON" : "OFF");
}

void Invoke(std::string_view args, std::string& out)
{
    if (MultiplayerSession())
    {
        out = "developer cheats disabled in multiplayer";
        return;
    }
    bool target = !s_active;
    if (args == "1" || args == "on" || args == "true")
        target = true;
    else if (args == "0" || args == "off" || args == "false")
        target = false;
    else if (!args.empty())
    {
        out = "inffuel: expected 1|0|on|off (or empty to toggle), got: " + std::string(args);
        return;
    }
    SetActive(target);
    out = std::string("inffuel: ") + (s_active ? "ON" : "OFF");
}
} // namespace Cmd_InfiniteFuel

// Cmd_ShowAllUnits — sticky toggle.  Adds an unconditional DrawUnits
// pass for every AICenter inside CStaticMapMain::DrawExt.  Available
// whenever a mission is running.
namespace Cmd_ShowAllUnits
{
namespace
{
bool s_active = false;
}

bool Available()
{
    return !MultiplayerSession() && MissionInfo::IsActive();
}
bool IsActive()
{
    return s_active && !MultiplayerSession();
}

void SetActive(bool active)
{
    active = active && !MultiplayerSession();
    if (s_active == active)
        return;
    s_active = active;
    LOG_INFO(Mission, "DebugCheats::ShowAllUnits -> {}", active ? "ON" : "OFF");
}

void Invoke(std::string_view args, std::string& out)
{
    if (MultiplayerSession())
    {
        out = "developer cheats disabled in multiplayer";
        return;
    }
    bool target = !s_active;
    if (args == "1" || args == "on" || args == "true")
        target = true;
    else if (args == "0" || args == "off" || args == "false")
        target = false;
    else if (!args.empty())
    {
        out = "showall: expected 1|0|on|off (or empty to toggle), got: " + std::string(args);
        return;
    }
    SetActive(target);
    out = std::string("showall: ") + (s_active ? "ON" : "OFF");
}
} // namespace Cmd_ShowAllUnits

// Cmd_MapTeleport — sticky toggle.  Intercepts CStaticMapMain::
// OnLButtonClick to teleport the player's vehicle instead of issuing
// a move/watch order.  When off the click handler runs normally.
namespace Cmd_MapTeleport
{
namespace
{
bool s_active = false;
}

bool Available()
{
    return !MultiplayerSession() && MissionInfo::IsActive();
}
bool IsActive()
{
    return s_active && !MultiplayerSession();
}

void SetActive(bool active)
{
    active = active && !MultiplayerSession();
    if (s_active == active)
        return;
    s_active = active;
    LOG_INFO(Mission, "DebugCheats::MapTeleport -> {}", active ? "ON" : "OFF");
}

void Invoke(std::string_view args, std::string& out)
{
    if (MultiplayerSession())
    {
        out = "developer cheats disabled in multiplayer";
        return;
    }
    bool target = !s_active;
    if (args == "1" || args == "on" || args == "true")
        target = true;
    else if (args == "0" || args == "off" || args == "false")
        target = false;
    else if (!args.empty())
    {
        out = "mapteleport: expected 1|0|on|off (or empty to toggle), got: " + std::string(args);
        return;
    }
    SetActive(target);
    out = std::string("mapteleport: ") + (s_active ? "ON" : "OFF");
}
} // namespace Cmd_MapTeleport

// Cmd_InfiniteArmor — sticky toggle.  Drop damage to the vehicle the
// player is currently in (same SetDammage hook as god, different
// target predicate).
namespace Cmd_InfiniteArmor
{
namespace
{
bool s_active = false;
} // namespace

bool Available()
{
    return !MultiplayerSession() && MissionInfo::IsActive();
}
bool IsActive()
{
    return s_active && !MultiplayerSession();
}

void SetActive(bool active)
{
    active = active && !MultiplayerSession();
    if (s_active == active)
        return;
    s_active = active;
    LOG_INFO(Mission, "DebugCheats::InfiniteArmor -> {}", active ? "ON" : "OFF");
}

void Invoke(std::string_view args, std::string& out)
{
    if (MultiplayerSession())
    {
        out = "developer cheats disabled in multiplayer";
        return;
    }
    bool target = !s_active;
    if (args == "1" || args == "on" || args == "true")
        target = true;
    else if (args == "0" || args == "off" || args == "false")
        target = false;
    else if (!args.empty())
    {
        out = "infarmor: expected 1|0|on|off (or empty to toggle), got: " + std::string(args);
        return;
    }
    SetActive(target);
    out = std::string("infarmor: ") + (s_active ? "ON" : "OFF");
}
} // namespace Cmd_InfiniteArmor

// Cmd_SkipTime — one-shot action.  Calls GWorld->SkipTime which queues
// a time delta to be applied during the next World::Simulate tick.
// Negative values rewind; very large values just snap further forward.
namespace Cmd_SkipTime
{

bool Available()
{
    return !MultiplayerSession() && MissionInfo::IsActive();
}

void InvokeHours(float hours, std::string& out)
{
    if (MultiplayerSession())
    {
        out = "developer cheats disabled in multiplayer";
        return;
    }
    if (!GWorld)
    {
        out = "skiptime: no active world";
        return;
    }
    GWorld->SkipTime(hours * OneHour);
    char buf[64];
    snprintf(buf, sizeof(buf), "skiptime: %+.2f hours queued", hours);
    out = buf;
    LOG_INFO(Mission, "DebugCheats::SkipTime -> {}", out);
}

void Invoke(std::string_view args, std::string& out)
{
    if (MultiplayerSession())
    {
        out = "developer cheats disabled in multiplayer";
        return;
    }
    // Parse a signed float; whitespace already trimmed by Run().
    char tmp[32];
    size_t n = args.size();
    if (n >= sizeof(tmp))
    {
        out = "skiptime: argument too long";
        return;
    }
    std::memcpy(tmp, args.data(), n);
    tmp[n] = 0;
    float hours = 0.0f;
    if (n > 0 && sscanf(tmp, "%f", &hours) != 1)
    {
        out = "skiptime: expected <hours> (signed float), got: " + std::string(args);
        return;
    }
    InvokeHours(hours, out);
}

} // namespace Cmd_SkipTime

// Cmd_SetWeather — instant overcast change via World::SetWeather.
// Fog is set to 0. The original justification given here was that "there is no
// public getter to preserve the existing fog level"; that is not true —
// Landscape::GetFog() exists. The behaviour is kept anyway because the console
// command and triCheatWeather both depend on it, but the reason is now simply
// that a one-argument overcast cheat has no fog to preserve. InvokeWeather
// below is the two-axis entry point.
namespace Cmd_SetWeather
{

bool Available()
{
    return !MultiplayerSession() && MissionInfo::IsActive();
}

void InvokeOvercast(float overcast, std::string& out)
{
    if (MultiplayerSession())
    {
        out = "developer cheats disabled in multiplayer";
        return;
    }
    if (!GWorld)
    {
        out = "weather: no active world";
        return;
    }
    if (overcast < 0.0f)
        overcast = 0.0f;
    if (overcast > 1.0f)
        overcast = 1.0f;
    GWorld->SetWeather(overcast, 0.0f, 0.0f); // immediate, fog=0
    char buf[64];
    snprintf(buf, sizeof(buf), "weather: overcast %.2f, fog 0", overcast);
    out = buf;
    LOG_INFO(Mission, "DebugCheats::SetWeather -> {}", out);
}

void Invoke(std::string_view args, std::string& out)
{
    if (MultiplayerSession())
    {
        out = "developer cheats disabled in multiplayer";
        return;
    }
    char tmp[32];
    size_t n = args.size();
    if (n == 0 || n >= sizeof(tmp))
    {
        out = "weather: expected <overcast 0..1>";
        return;
    }
    std::memcpy(tmp, args.data(), n);
    tmp[n] = 0;
    float overcast = 0.0f;
    if (sscanf(tmp, "%f", &overcast) != 1)
    {
        out = "weather: expected <overcast>, got: " + std::string(args);
        return;
    }
    InvokeOvercast(overcast, out);
}

void InvokeWeather(float overcast, float fog, float transitionSeconds, std::string& out)
{
    if (MultiplayerSession())
    {
        out = "developer cheats disabled in multiplayer";
        return;
    }
    if (!GWorld)
    {
        out = "weather: no active world";
        return;
    }
    overcast = std::clamp(overcast, 0.0f, 1.0f);
    fog = std::clamp(fog, 0.0f, 1.0f);
    // Negative would be meaningless to the engine's interpolation; 0 is "now".
    transitionSeconds = std::max(transitionSeconds, 0.0f);
    GWorld->SetWeather(overcast, fog, transitionSeconds);
    char buf[96];
    snprintf(buf, sizeof(buf), "weather: overcast %.2f, fog %.2f over %.0fs", overcast, fog, transitionSeconds);
    out = buf;
    LOG_INFO(Mission, "DebugCheats::SetWeather -> {}", out);
}

} // namespace Cmd_SetWeather

// Cmd_SetTimeOfDay — absolute clock control built on the relative skip the
// engine actually provides.
namespace Cmd_SetTimeOfDay
{

bool Available()
{
    return !MultiplayerSession() && MissionInfo::IsActive();
}

void InvokeHour(float hour, std::string& out)
{
    if (MultiplayerSession())
    {
        out = "developer cheats disabled in multiplayer";
        return;
    }
    if (!GWorld)
    {
        out = "settime: no active world";
        return;
    }
    // Wrap rather than clamp: 24.0 is midnight, not "one minute to".
    hour = std::fmod(hour, 24.0f);
    if (hour < 0.0f)
        hour += 24.0f;
    const float now = Glob.clock.GetTimeOfDay() * 24.0f;
    // Forward-only. A negative skip is not known to be safe for the day/night
    // and lighting state, and wrapping forward past midnight still reaches
    // every hour, so the shortest FORWARD delta is always the right move.
    float delta = hour - now;
    if (delta < 0.0f)
        delta += 24.0f;
    GWorld->SkipTime(delta * OneHour);
    char buf[96];
    snprintf(buf, sizeof(buf), "settime: %05.2f -> %05.2f (+%.2fh)", now, hour, delta);
    out = buf;
    LOG_INFO(Mission, "DebugCheats::SetTimeOfDay -> {}", out);
}

} // namespace Cmd_SetTimeOfDay

// Cmd_TimeMultiplier — write GWorld->_acceleratedTime.  Read-back uses
// the same getter so a `Cmd_TimeMultiplier::Get()` round-trip reports
// the actual saturated value the engine accepted.
namespace Cmd_TimeMultiplier
{

bool Available()
{
    return !MultiplayerSession() && MissionInfo::IsActive();
}

float Get()
{
    return GWorld ? GWorld->GetAcceleratedTime() : 1.0f;
}

void SetValue(float mult, std::string& out)
{
    if (MultiplayerSession())
    {
        out = "developer cheats disabled in multiplayer";
        return;
    }
    if (!GWorld)
    {
        out = "timemult: no active world";
        return;
    }
    GWorld->SetAcceleratedTime(mult);
    char buf[64];
    snprintf(buf, sizeof(buf), "timemult: requested %.2fx, engine reports %.2fx", mult, GWorld->GetAcceleratedTime());
    out = buf;
    LOG_INFO(Mission, "DebugCheats::TimeMultiplier -> {}", out);
}

void Invoke(std::string_view args, std::string& out)
{
    if (MultiplayerSession())
    {
        out = "developer cheats disabled in multiplayer";
        return;
    }
    char tmp[32];
    size_t n = args.size();
    if (n == 0 || n >= sizeof(tmp))
    {
        out = "timemult: expected <mult> (e.g. 1.0 / 2.0 / 4.0)";
        return;
    }
    std::memcpy(tmp, args.data(), n);
    tmp[n] = 0;
    float mult = 0.0f;
    if (sscanf(tmp, "%f", &mult) != 1)
    {
        out = "timemult: expected <mult>, got: " + std::string(args);
        return;
    }
    SetValue(mult, out);
}

} // namespace Cmd_TimeMultiplier

// Cmd_UnlockCampaign — one-shot action.  Unlocks EVERY campaign found
// on disk by writing each one's CampaignHistory directly to
// <TmpSaveDir>/<name>.sqc — the same file DisplayCampaignLoad reads at
// open.  Works from any screen; when the Campaign Load display happens
// to be active, the bridge also refreshes its in-memory _campaigns so
// the visible list updates without a reopen.
namespace Cmd_UnlockCampaign
{

bool Available()
{
    // Available outside multiplayer, without display gating. If no campaigns are
    // installed at all the Invoke will fail and report it.
    return !MultiplayerSession();
}

void Invoke(std::string_view /*args*/, std::string& out)
{
    if (MultiplayerSession())
    {
        out = "developer cheats disabled in multiplayer";
        return;
    }
    if (DebugUnlockAllCampaigns())
    {
        out = "unlockcampaign: every mission of every campaign marked available";
        LOG_INFO(Mission, "DebugCheats::UnlockCampaign -> {}", out);
    }
    else
    {
        out = "unlockcampaign: no campaigns found";
    }
}

} // namespace Cmd_UnlockCampaign

// Cmd_SaveGame — one-shot action.  Mirrors the original OFP SAVEGAME
// word cheat: dumps the live World state to <SaveDir>/save.fps
// via World::SaveBin, the same path the engine uses for normal saves.
// Available whenever a mission is running — no "save allowed" gating.
namespace Cmd_SaveGame
{

bool Available()
{
    return !MultiplayerSession() && MissionInfo::IsActive();
}

void Invoke(std::string_view /*args*/, std::string& out)
{
    if (MultiplayerSession())
    {
        out = "developer cheats disabled in multiplayer";
        return;
    }
    if (!GWorld)
    {
        out = "save: no active world";
        return;
    }
    RString name = NormalizeSavePath(GetSaveDirectory() + RString("save.fps"));
    const bool ok = GWorld->SaveBin(name, IDS_SAVE_GAME);
    out = ok ? (std::string("save: written to ") + (const char*)name) : "save: failed (SaveBin returned false)";
    LOG_INFO(Mission, "DebugCheats::SaveGame -> {}", out);
}

} // namespace Cmd_SaveGame

// Cmd_StorePosition — the original OFP INSERT dev hotkey.  Logs
// the camera + player position and copies a copy-pasteable text
// block to the system clipboard via SDL_SetClipboardText.
} // namespace DebugCheats
} // namespace Poseidon::Dev

#include <Poseidon/World/Scene/Scene.hpp>
#include <Poseidon/World/Scene/Camera/Camera.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>
#include <SDL3/SDL_clipboard.h>

namespace Poseidon::Dev
{
namespace DebugCheats
{
namespace Cmd_StorePosition
{

bool Available()
{
    // Camera is the minimum requirement — exists as soon as the scene
    // is up (including the menu demo loop).  Player is optional and
    // adds a second line of output when present.
    return !MultiplayerSession() && GScene != nullptr && GScene->GetCamera() != nullptr;
}

void Invoke(std::string_view /*args*/, std::string& out)
{
    if (MultiplayerSession())
    {
        out = "developer cheats disabled in multiplayer";
        return;
    }
    if (!Available())
    {
        out = "storepos: no scene / camera";
        return;
    }
    const Camera* cam = GScene->GetCamera();
    Vector3 cp = cam->Position();
    Vector3 cd = cam->Direction();
    Vector3 cu = cam->DirectionUp();

    // triSetView pins the render camera to the exact captured world-space pose,
    // so the view reproduces without a mission camera object (the camCreate
    // path does not drive the render under headless tri).  Values are
    // raw engine [X, up, Z] — paste verbatim.
    float tod = Glob.clock.GetTimeOfDay();
    int todHour = static_cast<int>(tod * 24.0f);
    int todMin = static_cast<int>((tod * 24.0f - todHour) * 60.0f);

    char buf[1536];
    int written = snprintf(
        buf, sizeof(buf),
        "// camera dump — paste into mission init.sqs / .test.sqf\n"
        "// island=%s  time=%02d:%02d  (match the test folder .<island> and the mission .sqm Intel)\n"
        "triSetView [%.3f, %.3f, %.3f, %.4f, %.4f, %.4f, %.4f, %.4f, %.4f];   // pos, dir, up (raw world)\n",
        Glob.header.worldname, todHour, todMin, cp.X(), cp.Y(), cp.Z(), cd.X(), cd.Y(), cd.Z(), cu.X(), cu.Y(), cu.Z());

    // Append the player teleport when a mission is up.  SQF position arrays are
    // [east(X), north(Z), altitude-above-ground]; setPos re-adds the terrain
    // height, so emit Y as captured height minus the surface.
    if (GWorld && GWorld->GetRealPlayer())
    {
        Vector3 pp = GWorld->GetRealPlayer()->Position();
        float surface = GLandscape ? GLandscape->SurfaceYAboveWater(pp.X(), pp.Z()) : 0.0f;
        int n = snprintf(buf + written, sizeof(buf) - written, "this setPos [%.2f, %.2f, %.2f];   // player\n", pp.X(),
                         pp.Z(), pp.Y() - surface);
        if (n > 0)
            written += n;
    }

    // Log the whole block (multi-line) for archival in stdout, AND
    // push to the system clipboard for one-click paste.
    LOG_INFO(Mission, "DebugCheats::StorePosition ->\n{}", buf);
    if (!SDL_SetClipboardText(buf))
    {
        LOG_WARN(Core, "SDL_SetClipboardText failed: {}", SDL_GetError());
        out = std::string("storepos: written to log; clipboard FAILED");
        return;
    }
    out = "storepos: written to log + copied to clipboard";
}

} // namespace Cmd_StorePosition

// Cmd_LoadGame — inverse of Cmd_SaveGame.  Calls World::LoadBin which
// rehydrates the world from <SaveDir>/save.fps in place.  The path is
// the one normal "Load Game" menus also go through.
namespace Cmd_LoadGame
{

bool Available()
{
    // Pre-checking the file via QIFStreamB::FileExist turned out to be
    // unreliable across isolated test configs (the save path can end up
    // with embedded colons / mixed slashes that confuse the lookup even
    // when the file is on disk).  Just require a mission running; the
    // Invoke surfaces "no save file" through its status string if
    // LoadBin can't find anything.
    return !MultiplayerSession() && MissionInfo::IsActive();
}

void Invoke(std::string_view /*args*/, std::string& out)
{
    if (MultiplayerSession())
    {
        out = "developer cheats disabled in multiplayer";
        return;
    }
    if (!GWorld)
    {
        out = "load: no active world";
        return;
    }
    RString name = NormalizeSavePath(GetSaveDirectory() + RString("save.fps"));
    const bool ok = GWorld->LoadBin(name, IDS_LOAD_GAME);
    out = ok ? (std::string("load: restored from ") + (const char*)name)
             : (std::string("load: failed (no save at ") + (const char*)name + ")");
    LOG_INFO(Mission, "DebugCheats::LoadGame -> {}", out);
}

} // namespace Cmd_LoadGame

namespace Cmd_EndMission
{

bool Available()
{
    // A mission must be active (real player assigned) AND not already
    // ending — clicking End again mid-teardown is at best a no-op.
    return !MultiplayerSession() && MissionInfo::IsActive() && GWorld->GetEndMode() == EMContinue;
}

std::vector<std::string> AvailableOutcomes()
{
    if (!Available())
        return {};
    // Three universal outcomes always work — every mission can be
    // force-won (EMEnd1), force-lost (EMLoser), or force-killed
    // (EMKilled) without needing a script-defined sensor.  Append
    // scripted endings (end1..end6) for those the mission declares.
    std::vector<std::string> out{"win", "lose", "killed"};
    const auto scripted = MissionInfo::AvailableEndings();
    out.insert(out.end(), scripted.begin(), scripted.end());
    return out;
}

void Invoke(std::string_view outcome, std::string& out)
{
    if (MultiplayerSession())
    {
        out = "developer cheats disabled in multiplayer";
        return;
    }
    if (!GWorld)
    {
        out = "no active world";
        return;
    }
    EndMode mode = EMEnd1;
    if (outcome == "win" || outcome == "end1")
        mode = EMEnd1;
    else if (outcome == "lose")
        mode = EMLoser;
    else if (outcome == "killed")
        mode = EMKilled;
    else if (outcome == "end2")
        mode = EMEnd2;
    else if (outcome == "end3")
        mode = EMEnd3;
    else if (outcome == "end4")
        mode = EMEnd4;
    else if (outcome == "end5")
        mode = EMEnd5;
    else if (outcome == "end6")
        mode = EMEnd6;
    else
    {
        out = "unknown outcome: " + std::string(outcome) + " (expected win|lose|killed|end1..end6)";
        return;
    }
    // Match the original OFP ENDMISSION word-cheat semantics exactly:
    // just flip `_endMission`.  The engine's mission-end transition
    // picks it up on the next `World::Simulate` tick for normal
    // gameplay missions (GModeArcade).
    //
    // Intros / outros / the demo loop run with a camera or title
    // effect active, which gates the transition in displayUIMenus.cpp
    // unless `IsEndForced()` is set.  Do not force it here with
    // `ForceEnd(true)`: the demo loop isn't a real mission, so its
    // background-scene teardown path is asymmetric and crashes.
    // Flipping `_endMission` is the right trade-off — the cheat works
    // in real gameplay missions where it matters and is a no-op (button
    // greys, mission continues) in the menu demo loop, matching the
    // original.
    GWorld->SetEndMode(mode);
    out = "ending mission: " + std::string(outcome);
    LOG_INFO(Mission, "DebugCheats::EndMission -> {}", out);
}

} // namespace Cmd_EndMission

// Self-registration with the DebugCommands registry.  The console
// dispatches by name through Run().  UI buttons + tri verbs call the
// Cmd_X::Invoke() functions directly so argument typing stays strong.
namespace
{
struct RegisterAll
{
    RegisterAll()
    {
        DebugCommands::Register({"endmission", "End the current mission (win|lose|killed|end1..end6).",
                                 Cmd_EndMission::Available,
                                 [](std::string_view args, std::string& out) { Cmd_EndMission::Invoke(args, out); }});
        DebugCommands::Register({"unlockcampaign",
                                 "Unlock all missions in the campaign (requires Campaign Load screen).",
                                 Cmd_UnlockCampaign::Available, [](std::string_view args, std::string& out)
                                 { Cmd_UnlockCampaign::Invoke(args, out); }});
        DebugCommands::Register({"storepos", "Log camera/player position + copy to clipboard.",
                                 Cmd_StorePosition::Available, [](std::string_view args, std::string& out)
                                 { Cmd_StorePosition::Invoke(args, out); }});
        DebugCommands::Register({"save", "Save current game state to <SaveDir>/save.fps.", Cmd_SaveGame::Available,
                                 [](std::string_view args, std::string& out) { Cmd_SaveGame::Invoke(args, out); }});
        DebugCommands::Register({"load", "Load game state from <SaveDir>/save.fps.", Cmd_LoadGame::Available,
                                 [](std::string_view args, std::string& out) { Cmd_LoadGame::Invoke(args, out); }});
        DebugCommands::Register({"showall", "Toggle showing all units on the in-mission map (1|0|on|off).",
                                 Cmd_ShowAllUnits::Available,
                                 [](std::string_view args, std::string& out) { Cmd_ShowAllUnits::Invoke(args, out); }});
        DebugCommands::Register({"mapteleport", "Toggle click-to-teleport on the in-mission map (1|0|on|off).",
                                 Cmd_MapTeleport::Available,
                                 [](std::string_view args, std::string& out) { Cmd_MapTeleport::Invoke(args, out); }});
        DebugCommands::Register({"weather", "Set overcast 0..1 instantly (fog cleared).", Cmd_SetWeather::Available,
                                 [](std::string_view args, std::string& out) { Cmd_SetWeather::Invoke(args, out); }});
        DebugCommands::Register({"timemult", "Set time multiplier (1 / 2 / 4 — saturated to engine range).",
                                 Cmd_TimeMultiplier::Available, [](std::string_view args, std::string& out)
                                 { Cmd_TimeMultiplier::Invoke(args, out); }});
        DebugCommands::Register({"skiptime", "Advance / rewind in-game time by <hours> (signed float).",
                                 Cmd_SkipTime::Available,
                                 [](std::string_view args, std::string& out) { Cmd_SkipTime::Invoke(args, out); }});
        DebugCommands::Register({"god", "Toggle god mode (1|0|on|off; empty toggles).", Cmd_God::Available,
                                 [](std::string_view args, std::string& out) { Cmd_God::Invoke(args, out); }});
        DebugCommands::Register({"infammo", "Toggle infinite ammo for the real player (1|0|on|off).",
                                 Cmd_InfiniteAmmo::Available,
                                 [](std::string_view args, std::string& out) { Cmd_InfiniteAmmo::Invoke(args, out); }});
        DebugCommands::Register({"inffuel", "Toggle infinite fuel for the player's vehicle (1|0|on|off).",
                                 Cmd_InfiniteFuel::Available,
                                 [](std::string_view args, std::string& out) { Cmd_InfiniteFuel::Invoke(args, out); }});
        DebugCommands::Register({"infarmor", "Toggle infinite armor for the player's vehicle (1|0|on|off).",
                                 Cmd_InfiniteArmor::Available, [](std::string_view args, std::string& out)
                                 { Cmd_InfiniteArmor::Invoke(args, out); }});
    }
};
RegisterAll g_registerAll;
} // namespace

} // namespace DebugCheats
} // namespace Poseidon::Dev
