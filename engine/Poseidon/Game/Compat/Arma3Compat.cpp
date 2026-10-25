// SPDX-License-Identifier: GPL-3.0-or-later
//
// Arma 3 (generation 3) parts of the compat layer: what an Arma 3 class is carried by,
// and the Arma 3 mission.sqm reader. Everything else -- mounting, the private config,
// the generated A1x_ classes with the Arma 3 model -- is shared with Arma 1 and 2
// (Arma1Compat.cpp, Arma1Config.cpp).
//
// Arma 3 class names say little about what a class is ("B_MRAP_01_F", "O_T_LSV_02_armed_F",
// "I_E_APC_tracked_03_cannon_F"), so the kind is read from the class's ancestry in the Arma
// 3 config itself (Tank, Wheeled_APC_F, Truck_F, Helicopter, Plane, StaticWeapon, ...).

#include <Poseidon/Game/Compat/Arma1Compat.hpp>
#include <Poseidon/Game/Compat/Arma3Eden.hpp>

#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/IO/ParamFileExt.hpp>
#include <Poseidon/IO/Streams/QBStream.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <initializer_list>
#include <set>
#include <string>
#include <vector>

namespace Poseidon::Arma1Compat
{
namespace
{
std::string Lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

const ParamClass* A3Class(const char* name)
{
    const ParamEntry* e = FindConfigClass("CfgVehicles", name);
    return e && e->IsClass() ? e->GetClassInterface() : nullptr;
}

// True when `cls` is `base` or derives from it in the Arma 3 config.
bool A3IsKindOf(const ParamEntry* cls, const char* base)
{
    const ParamClass* c = cls && cls->IsClass() ? cls->GetClassInterface() : nullptr;
    const ParamClass* b = A3Class(base);
    return c && b && (c == b || c->IsDerivedFrom(*b));
}

bool A3IsAnyOf(const ParamEntry* cls, std::initializer_list<const char*> bases)
{
    for (const char* b : bases)
        if (A3IsKindOf(cls, b))
            return true;
    return false;
}

std::string StringEntry(const ParamEntry& cls, const char* key)
{
    const ParamEntry* e = cls.FindEntry(key);
    if (!e || e->IsClass() || e->IsArray())
        return {};
    return (const char*)(RStringB)*e;
}

// The first of `candidates` CWA defines.
RString FirstCwa(std::initializer_list<const char*> candidates)
{
    const ParamEntry* vehicles = Pars.FindEntry("CfgVehicles");
    if (!vehicles)
        return RString();
    for (const char* c : candidates)
        if (const ParamEntry* e = vehicles->FindEntryNoInheritance(c); e && e->IsClass())
            return RString(c);
    return RString();
}

// The name of the class and of every ancestor, lower case, one string ("b_mbt_01_cannon_f
// mbt_01_cannon_base_f mbt_01_base_f tank_f tank landvehicle ..."): for the few kinds the
// ancestry only names (anti-air, artillery, the transport helicopters).
std::string Lineage(const ParamEntry* cls)
{
    std::string out;
    const ParamClass* c = cls && cls->IsClass() ? cls->GetClassInterface() : nullptr;
    if (cls)
        out = Lower((const char*)cls->GetName());
    for (int guard = 0; c && guard < 48; guard++)
    {
        const char* base = c->GetBaseName();
        if (!base || !*base)
            break;
        out += " " + Lower(base);
        const ParamClass* next = A3Class(base);
        if (next == c)
            break;
        c = next;
    }
    return out;
}
} // namespace

std::string SimKind(const std::string& simulation)
{
    // Arma 3 renamed most simulations with a PhysX suffix: carx, tankx, helicopterrtd,
    // helicopterx, airplanex, shipx, submarinex, thingx. The kind is what is left.
    std::string s = Lower(simulation);
    if (s.size() > 3 && s.compare(s.size() - 3, 3, "rtd") == 0)
        s.resize(s.size() - 3);
    if (s.size() > 1 && s.back() == 'x' && s != "flagcarrier")
        s.pop_back();
    if (s == "submarine")
        s = "ship";
    return s;
}

bool Arma3IsAnimal(const RString& className)
{
    if (Generation() != 3)
        return false;
    const ParamEntry* cls = FindConfigClass("CfgVehicles", className.Data());
    return cls && A3IsAnyOf(cls, {"Animal", "Animal_Base_F"});
}

RString Arma3VehicleCarrier(const RString& className)
{
    if (Generation() != 3)
        return RString();
    const ParamEntry* cls = FindConfigClass("CfgVehicles", className.Data());
    if (!cls || !cls->IsClass())
        return RString();
    int side = 3;
    if (const ParamEntry* s = cls->FindEntry("side"); s && !s->IsClass() && !s->IsArray())
        side = std::clamp((int)*s, 0, 3);
    const bool east = side == 0, west = side == 1, guer = side == 2;
    const std::string kin = Lineage(cls);
    const auto named = [&](std::initializer_list<const char*> words)
    {
        for (const char* w : words)
            if (kin.find(w) != std::string::npos)
                return true;
        return false;
    };

    if (A3IsKindOf(cls, "ParachuteBase"))
        return FirstCwa({east ? "ParachuteEast" : west ? "ParachuteWest" : guer ? "ParachuteG" : "ParachuteC",
                         "ParachuteWest"});
    if (A3IsKindOf(cls, "StaticWeapon"))
        return FirstCwa({"M2StaticMG"});
    if (A3IsKindOf(cls, "Tank"))
    {
        if (named({"_aa", "aa_", "tigris", "cheetah"}))
            return east ? FirstCwa({"ZSU", "T72"}) : FirstCwa({"Vulcan", "M2A2", "M113"});
        if (named({"apc", "ifv", "afv"}))
            return east ? FirstCwa({"BMP2", "BMP", "T72"}) : west ? FirstCwa({"M2A2", "M113"}) : FirstCwa({"BMP", "M113"});
        if (named({"mbt", "tank"}))
            return east ? FirstCwa({"T80", "T72"}) : west ? FirstCwa({"M1Abrams", "M60"}) : FirstCwa({"T72", "T55"});
        return east ? FirstCwa({"BMP", "T72"}) : FirstCwa({"M113", "M2A2"});
    }
    if (A3IsKindOf(cls, "Wheeled_APC_F") || named({"wheeled_apc", "apc_wheeled", "afv_wheeled"}))
        return east ? FirstCwa({"BRDM", "BMP"}) : west ? FirstCwa({"M113", "M2A2"}) : FirstCwa({"BRDM", "M113"});
    if (A3IsKindOf(cls, "Truck_F") || named({"truck"}))
        return east || guer ? FirstCwa({"Ural", "Truck5t"}) : FirstCwa({"Truck5t", "Ural"});
    if (A3IsAnyOf(cls, {"Quadbike_01_base_F", "Kart_01_Base_F"}))
        return FirstCwa({"Kolo", "Skoda"});
    if (A3IsKindOf(cls, "Car"))
    {
        const bool armed = named({"hmg", "gmg", "_armed", "_mg", "_at"});
        if (side == 3)
            return named({"van", "truck"}) ? FirstCwa({"Truck5t", "Skoda"}) : FirstCwa({"Skoda", "SkodaBlue"});
        if (armed)
            return east ? FirstCwa({"UAZMG", "UAZ", "JeepMG"}) : FirstCwa({"JeepMG", "HMMWV", "Jeep"});
        return east ? FirstCwa({"UAZ"}) : west ? FirstCwa({"HMMWV", "Jeep"}) : FirstCwa({"GJeep", "Jeep"});
    }
    if (A3IsKindOf(cls, "Helicopter"))
    {
        if (named({"heli_attack", "attack"}))
            return east || guer ? FirstCwa({"Mi24", "Mi17"}) : FirstCwa({"Cobra", "UH60"});
        if (named({"heli_transport_03", "heli_transport_04", "chinook", "taru"}))
            return east ? FirstCwa({"Mi17"}) : FirstCwa({"CH47D", "UH60"});
        if (named({"heli_light"}))
            return east ? FirstCwa({"Mi17"}) : FirstCwa({"OH58", "UH60"});
        return east ? FirstCwa({"Mi17"}) : guer ? FirstCwa({"Mi17", "UH60"}) : FirstCwa({"UH60", "Mi17"});
    }
    if (A3IsKindOf(cls, "Plane"))
        return side == 3 ? FirstCwa({"Cessna", "A10"}) : east || guer ? FirstCwa({"Su25", "A10"}) : FirstCwa({"A10", "Su25"});
    if (A3IsKindOf(cls, "Ship"))
        return east ? FirstCwa({"BoatE", "BoatW"}) : FirstCwa({"BoatW", "BoatE"});
    return RString();
}

namespace
{
// The first sample of an Arma 3 sound shader (samples[] = {{"path", weight}, ...}).
std::string ShaderSample(const std::string& shader)
{
    const ParamEntry* cls = FindConfigClass("CfgSoundShaders", shader.c_str());
    const ParamEntry* samples = cls ? cls->FindEntry("samples") : nullptr;
    if (!samples || !samples->IsArray() || samples->GetSize() < 1)
        return std::string();
    const IParamArrayValue& first = (*samples)[0];
    if (first.IsArrayValue() && first.GetItemCount() > 0)
        return std::string((const char*)first[0].GetValue());
    return std::string((const char*)first.GetValue());
}

// The sample of an Arma 3 sound set: of its shaders, the close shot when there is one (a
// weapon's set lists the bolt's click -- "Closure" -- first, then close, mid and distant
// shots), else the first shader that has a sample.
std::string SoundSetSample(const std::string& set)
{
    const ParamEntry* cls = FindConfigClass("CfgSoundSets", set.c_str());
    const ParamEntry* shaders = cls ? cls->FindEntry("soundShaders") : nullptr;
    if (!shaders || !shaders->IsArray())
        return std::string();
    std::vector<std::string> names;
    for (int i = 0; i < shaders->GetSize(); i++)
        names.push_back((const char*)(*shaders)[i].GetValue());
    const auto rank = [](const std::string& name) {
        const std::string n = Lower(name);
        if (n.find("closure") != std::string::npos || n.find("tail") != std::string::npos ||
            n.find("dist") != std::string::npos)
            return 3;
        if (n.find("closeshot") != std::string::npos || n.find("close_shot") != std::string::npos)
            return 0;
        if (n.find("shot") != std::string::npos)
            return 1;
        return 2;
    };
    std::stable_sort(names.begin(), names.end(),
                     [&](const std::string& a, const std::string& b) { return rank(a) < rank(b); });
    for (const std::string& shader : names)
        if (std::string sample = ShaderSample(shader); !sample.empty())
            return sample;
    return std::string();
}

// A sound file Arma names without its extension: .wss, else .ogg, else .wav.
std::string ExistingSoundFile(std::string path)
{
    if (path.empty())
        return path;
    const std::string bare = path[0] == '\\' ? path.substr(1) : path;
    const size_t slash = bare.find_last_of("\\/");
    if (bare.find('.', slash == std::string::npos ? 0 : slash) != std::string::npos)
        return QIFStreamB::FileExist(bare.c_str()) ? "\\" + bare : std::string();
    for (const char* ext : {".wss", ".ogg", ".wav"})
        if (QIFStreamB::FileExist((bare + ext).c_str()))
            return "\\" + bare + ext;
    return std::string();
}

// How well a sound's name says "the engine heard from outside, under load".
int EngineScore(const std::string& lowerName, bool helicopter)
{
    if (lowerName.find("int") != std::string::npos && lowerName.find("ext") == std::string::npos)
        return -100;
    for (const char* bad : {"start", "stop", "tires", "rattl", "stress", "rain", "damage", "alarm", "siren",
                            "brake", "turn", "gear", "distant", "wind", "scrub", "slipping", "noise", "water",
                            "servo", "horn", "crash", "land", "transmission", "gun", "missile"})
        if (lowerName.find(bad) != std::string::npos)
            return -100;
    int score = 0;
    if (lowerName.find("engine") != std::string::npos)
        score += 4;
    if (helicopter && lowerName.find("rotor") != std::string::npos)
        score += 6;
    if (lowerName.find("ext") != std::string::npos)
        score += 2;
    if (lowerName.find("rpm2") != std::string::npos || lowerName.find("rpm1") != std::string::npos)
        score += 1;
    if (lowerName.find("idle") != std::string::npos)
        score -= 2;
    return score;
}
} // namespace

// The engine sound of an Arma 3 vehicle as one looped file, for CWA's soundEngine[] (the
// engine loops it and bends its pitch with speed). Arma 3 mixes engines from expressions
// over several samples (class Sounds: soundSetsExt[] of sound sets, or subclasses with a
// sound[] each); the one taken is the outside engine under load -- for a helicopter the
// rotor. Empty when the vehicle names nothing this install has.
std::string Arma3EngineSound(const ParamEntry& vehicle, bool helicopter)
{
    const ParamEntry* sounds = vehicle.FindEntry("Sounds");
    if (!sounds || !sounds->IsClass())
        return std::string();
    int best = 0;
    std::string pick;
    const auto consider = [&](const std::string& name, const std::string& sample) {
        if (sample.empty())
            return;
        const int score = EngineScore(Lower(name), helicopter);
        if (score > best)
        {
            if (std::string file = ExistingSoundFile(sample); !file.empty())
            {
                best = score;
                pick = file;
            }
        }
    };
    if (const ParamEntry* sets = sounds->FindEntry("soundSetsExt"); sets && sets->IsArray())
        for (int i = 0; i < sets->GetSize(); i++)
        {
            const std::string set = (const char*)(*sets)[i].GetValue();
            if (!set.empty())
                consider(set, SoundSetSample(set));
        }
    for (int i = 0; i < sounds->GetEntryCount(); i++)
    {
        const ParamEntry& e = sounds->GetEntry(i);
        if (!e.IsClass())
            continue;
        const ParamEntry* sound = e.FindEntry("sound");
        if (sound && sound->IsArray() && sound->GetSize() > 0)
            consider((const char*)e.GetName(), (const char*)(*sound)[0].GetValue());
    }
    return pick;
}

// The shot of an Arma 3 weapon as one file: its first fire mode's sound class (sounds[]:
// StandardSound) -- the first sample of its first soundSetShot[] set, else its begin1[] --
// or the mode's own sound[]. Empty when the install has none of it.
std::string Arma3WeaponShotSound(const ParamEntry& weapon)
{
    const ParamEntry* modes = weapon.FindEntry("modes");
    const ParamEntry* mode = &weapon;
    if (modes && modes->IsArray() && modes->GetSize() > 0)
    {
        const std::string first = (const char*)(*modes)[0].GetValue();
        if (Lower(first) != "this")
            if (const ParamEntry* m = weapon.FindEntry(first.c_str()); m && m->IsClass())
                mode = m;
    }
    std::vector<const ParamEntry*> candidates;
    if (const ParamEntry* sounds = mode->FindEntry("sounds"); sounds && sounds->IsArray() && sounds->GetSize() > 0)
        if (const ParamEntry* s = mode->FindEntry((const char*)(*sounds)[0].GetValue()); s && s->IsClass())
            candidates.push_back(s);
    candidates.push_back(mode);
    for (const ParamEntry* c : candidates)
    {
        if (const ParamEntry* sets = c->FindEntry("soundSetShot"); sets && sets->IsArray())
            for (int i = 0; i < sets->GetSize(); i++)
                if (std::string f = ExistingSoundFile(SoundSetSample((const char*)(*sets)[i].GetValue())); !f.empty())
                    return f;
        for (const char* key : {"begin1", "sound"})
            if (const ParamEntry* a = c->FindEntry(key); a && a->IsArray() && a->GetSize() > 0)
                if (std::string f = ExistingSoundFile((const char*)(*a)[0].GetValue()); !f.empty())
                    return f;
    }
    return std::string();
}

// A sound array of an Arma 2 config ({file, volume, pitch[, distance]}) as the three values
// an OFP sound takes, `"\\file.wss",volume,pitch`; empty when the file is not installed.
std::string Arma2SoundTriple(const ParamEntry* a)
{
    if (!a || !a->IsArray() || a->GetSize() < 3)
        return std::string();
    const std::string file = ExistingSoundFile((const char*)(*a)[0].GetValue());
    if (file.empty())
        return std::string();
    char tail[64];
    std::snprintf(tail, sizeof(tail), ",%g,%g", (float)(*a)[1], (float)(*a)[2]);
    return "\"" + file + "\"" + tail;
}

// An Arma 2 weapon's shot, from its first fire mode: soundBegin[] names the samples it picks
// from (begin1, begin2 ...; the first is taken), Arma 2 before 1.05 writes sound[]. With
// Arma 2's own volume and pitch; empty when none of them is installed.
std::string Arma2WeaponShot(const ParamEntry& weapon)
{
    const ParamEntry* mode = &weapon;
    if (const ParamEntry* modes = weapon.FindEntry("modes"); modes && modes->IsArray() && modes->GetSize() > 0)
    {
        const std::string first = (const char*)(*modes)[0].GetValue();
        if (Lower(first) != "this")
            if (const ParamEntry* m = weapon.FindEntry(first.c_str()); m && m->IsClass())
                mode = m;
    }
    for (const ParamEntry* c : {mode, &weapon})
    {
        if (const ParamEntry* begin = c->FindEntry("soundBegin"); begin && begin->IsArray())
            for (int i = 0; i < begin->GetSize(); i += 2)
                if (std::string t = Arma2SoundTriple(c->FindEntry((const char*)(*begin)[i].GetValue())); !t.empty())
                    return t;
        for (const char* key : {"begin1", "sound"})
            if (std::string t = Arma2SoundTriple(c->FindEntry(key)); !t.empty())
                return t;
    }
    return std::string();
}

// The ambient loop of the Arma 3 sound environment (CfgEnvSounds' soundSetEnvironment[]) that
// plays in CWA's EnvSounds slot: 0 sea, 1 trees, 2 meadows, 3 hills, 4 rain. Arma 3 mixes
// these sets by expressions over forest, houses, sea, wind and height; CWA's sound map
// names one slot per place, so each slot takes the set closest to it -- birds over forest
// and meadow by day, crickets on the meadow and the quiet forest by night, the high wind
// on hills. Empty when the install has none of the candidates (the world's own loop stays).
std::string Arma3EnvSound(int slot, bool night)
{
    if (Generation() != 3 || slot < 0 || slot > 4)
        return std::string();
    static const char* const kDay[5][3] = {
        {"Sea_SoundSet", "Coast_SoundSet", nullptr},
        {"Birds_Forest_Day_SoundSet", "Forest_Low_SoundSet", "Forest_High_SoundSet"},
        {"Birds_Meadows_Day_SoundSet", "Meadows_Low_SoundSet", "Meadows_High_SoundSet"},
        {"Wind_High_SoundSet", "Wind_Low_SoundSet", nullptr},
        {"Stratis_RainMeadows_Medium_SoundSet", "Stratis_RainMeadows_Low_SoundSet", nullptr}};
    static const char* const kNight[5][3] = {
        {"Sea_SoundSet", "Coast_SoundSet", nullptr},
        {"Forest_Low_SoundSet", "Forest_High_SoundSet", nullptr},
        {"Greek_Crickets_Night_SoundSet", "Meadows_Low_SoundSet", nullptr},
        {"Wind_High_SoundSet", "Wind_Low_SoundSet", nullptr},
        {"Stratis_RainMeadows_Medium_SoundSet", "Stratis_RainMeadows_Low_SoundSet", nullptr}};
    for (const char* set : night ? kNight[slot] : kDay[slot])
        if (set)
            if (std::string file = ExistingSoundFile(SoundSetSample(set)); !file.empty())
                return file;
    return std::string();
}

RString Arma3HeadForProxy(const char* proxyModel)
{
    if (Generation() != 3 || !proxyModel)
        return RString();
    std::string name = Lower(proxyModel);
    std::replace(name.begin(), name.end(), '/', '\\');
    if (name.size() >= 4 && name.compare(name.size() - 4, 4, ".p3d") == 0)
        name.resize(name.size() - 4);
    static const std::string bust = "\\heads\\bysta";
    if (name.size() < bust.size() || name.compare(name.size() - bust.size(), bust.size(), bust) != 0)
        return RString();
    static RString head;
    static bool looked = false;
    if (!looked)
    {
        looked = true;
        if (const ParamEntry* cls = FindConfigClass("CfgHeads", "Default_A3"); cls && cls->IsClass())
            head = RString(StringEntry(*cls, "model").c_str());
        LOG_INFO(Mission, "Arma 3: a man's head proxy is drawn with {}", head.GetLength() ? head.Data() : "(nothing)");
    }
    return head;
}

namespace
{
EdenKind KindOf(const std::string& className)
{
    const ParamEntry* cls = FindConfigClass("CfgVehicles", className.c_str());
    if (!cls || !cls->IsClass())
        return EdenKind::Unknown;
    if (A3IsKindOf(cls, "CAManBase"))
        return EdenKind::Man;
    if (A3IsKindOf(cls, "Air"))
        return EdenKind::Air;
    if (A3IsKindOf(cls, "Ship"))
        return EdenKind::Ship;
    if (A3IsKindOf(cls, "StaticWeapon"))
        return EdenKind::Static;
    if (A3IsKindOf(cls, "LandVehicle"))
        return EdenKind::Land;
    return EdenKind::Thing;
}

bool IsMissionSqmName(const char* fileName)
{
    const size_t n = std::strlen(fileName);
    return n >= 11 && _stricmp(fileName + n - 11, "mission.sqm") == 0;
}
} // namespace

bool Arma3MissionFileToText(const char* fileName, std::string& text)
{
    if (Generation() != 3 || !fileName || !*fileName || !IsMissionSqmName(fileName) ||
        !QIFStreamB::FileExist(fileName))
        return false;
    QIFStreamB in;
    in.AutoOpen(fileName);
    const int size = in.rest();
    if (in.fail() || size <= 0)
        return false;
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    in.read(bytes.data(), size);
    std::string source;
    const bool binarised = bytes.size() >= 4 && std::memcmp(bytes.data(), "\0raP", 4) == 0;
    if (binarised)
    {
        if (!RapBytesToText(bytes, source))
            return false;
    }
    else
        source.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());

    EdenEnvironment env;
    env.kind = &KindOf;
    env.markerExists = [](const std::string& name)
    {
        const ParamEntry* markers = Pars.FindEntry("CfgMarkers");
        return markers && markers->FindEntry(name.c_str()) != nullptr;
    };
    env.colorExists = [](const std::string& name)
    {
        const ParamEntry* colors = Pars.FindEntry("CfgMarkerColors");
        return colors && colors->FindEntry(name.c_str()) != nullptr;
    };
    EdenReport report;
    std::string converted;
    if (!ConvertArma3MissionText(source, converted, report, env))
    {
        if (!binarised)
            return false; // plain text in a layout the engine reads as it is
        text = std::move(source);
        return true;
    }
    if (report.eden)
        LOG_INFO(Mission,
                 "Arma 3: {} is an Eden mission: {} groups, {} units ({} logics), {} vehicles/objects, {} waypoints, "
                 "{} markers, {} triggers, {} links, {} entities without counterpart",
                 fileName, report.groups, report.units, report.logics, report.vehicles, report.waypoints,
                 report.markers, report.triggers, report.links, report.skipped);
    else
        LOG_INFO(Mission, "Arma 3: {}: pairwise synchronisations rewritten", fileName);
    text = std::move(converted);
    return true;
}
} // namespace Poseidon::Arma1Compat
