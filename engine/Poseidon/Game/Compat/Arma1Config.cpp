// SPDX-License-Identifier: GPL-3.0-or-later
//
// Arma 1 configuration, read into its own tree.
//
// The binarized Arma 1 configs (`\0raP`) are decoded here by an original reader,
// turned into config text and parsed with the engine's own ParamFile into a
// separate tree (`Arma1Pars`). That tree is never merged into Pars wholesale: CWA
// stays the source of gameplay. What it is used for:
//   - sound/radio/SFX/environment classes the mission scripts name (fallback after
//     mission, campaign and CWA config),
//   - Arma 1 stringtables (a separate, lowest-priority table),
//   - generated CfgVehicles classes that keep a CWA unit's simulation but draw the
//     Arma 1 model, and for soldiers the Arma 1 move set (CfgMovesMaleSdr, imported
//     under the private name A1_CfgMovesMaleSdr).

#include <Poseidon/Game/Compat/Arma1Compat.hpp>

#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/IO/ParamFileExt.hpp>
#include <Poseidon/IO/Streams/QBStream.hpp>
#include <Poseidon/IO/Streams/QStream.hpp>
#include <Poseidon/UI/Locale/Stringtable/Stringtable.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace Poseidon::Arma1Compat
{
const std::vector<std::string>& BankPrefixes(); // Arma1Compat.cpp
std::string RootFolder();                       // Arma1Compat.cpp

namespace
{
std::string Lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

ParamFile& Arma1Pars()
{
    static ParamFile pars;
    return pars;
}

bool& ConfigLoaded()
{
    static bool loaded = false;
    return loaded;
}

// ---------------------------------------------------------------------------
// raP -> config text
// ---------------------------------------------------------------------------

// Arma 1 keeps ManPos* as enum names; CWA's config evaluates them to these numbers
// (SoldierOld.hpp, enum ManPos).
int ManPosValue(const std::string& name)
{
    static const char* names[] = {"ManPosDead",       "ManPosWeapon",        "ManPosBinocLying", "ManPosLyingNoWeapon",
                                  "ManPosLying",      "ManPosHandGunLying",  "ManPosCrouch",     "ManPosHandGunCrouch",
                                  "ManPosCombat",     "ManPosHandGunStand",  "ManPosStand",      "ManPosNoWeapon",
                                  "ManPosBinoc",      "ManPosBinocStand"};
    for (int i = 0; i < static_cast<int>(sizeof(names) / sizeof(names[0])); i++)
        if (_stricmp(names[i], name.c_str()) == 0)
            return i;
    if (_stricmp(name.c_str(), "ManPosSwimming") == 0)
        return 10; // no swimming pose in CWA's enum: treat as standing
    return -1000;
}

class RapToText
{
    const std::vector<uint8_t>& _bytes;
    std::string _out;

    void Need(size_t at, size_t n) const
    {
        if (at + n > _bytes.size())
            throw std::runtime_error("truncated raP");
    }
    uint8_t Byte(size_t& at) const
    {
        Need(at, 1);
        return _bytes[at++];
    }
    uint32_t U32(size_t& at) const
    {
        Need(at, 4);
        const uint32_t v = uint32_t(_bytes[at]) | (uint32_t(_bytes[at + 1]) << 8) | (uint32_t(_bytes[at + 2]) << 16) |
                           (uint32_t(_bytes[at + 3]) << 24);
        at += 4;
        return v;
    }
    float F32(size_t& at) const
    {
        const uint32_t bits = U32(at);
        float f;
        std::memcpy(&f, &bits, sizeof(f));
        return f;
    }
    std::string Str(size_t& at) const
    {
        const size_t start = at;
        while (true)
        {
            if (Byte(at) == 0)
                break;
        }
        return std::string(reinterpret_cast<const char*>(_bytes.data() + start), at - start - 1);
    }
    uint32_t Count(size_t& at) const
    {
        uint32_t v = 0;
        for (unsigned shift = 0; shift <= 28; shift += 7)
        {
            const uint8_t b = Byte(at);
            v |= uint32_t(b & 0x7f) << shift;
            if (!(b & 0x80))
                return v;
        }
        throw std::runtime_error("bad raP count");
    }

    static std::string Quote(const std::string& s)
    {
        std::string q = "\"";
        for (char c : s)
        {
            if (c == '"')
                q += "\"\"";
            else if (c == '\r' || c == '\n')
                q += ' ';
            else
                q += c;
        }
        q += '"';
        return q;
    }
    static std::string Float(float f)
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "%.9g", f);
        if (strchr(buf, 'e') || strchr(buf, 'E') || strstr(buf, "inf") || strstr(buf, "nan"))
            snprintf(buf, sizeof(buf), "%.6f", static_cast<double>(f));
        return buf;
    }
    std::string ScalarString(const std::string& s) const
    {
        if (s.rfind("ManPos", 0) == 0)
        {
            const int v = ManPosValue(s);
            if (v != -1000)
                return std::to_string(v);
        }
        return Quote(s);
    }

    std::string Array(size_t& at, int depth)
    {
        if (depth > 32)
            throw std::runtime_error("raP array too deep");
        const uint32_t n = Count(at);
        std::string out = "{";
        for (uint32_t i = 0; i < n; i++)
        {
            if (i)
                out += ",";
            const uint8_t t = Byte(at);
            if (t == 0 || t == 4)
                out += ScalarString(Str(at));
            else if (t == 1)
                out += Float(F32(at));
            else if (t == 2)
                out += std::to_string(static_cast<int32_t>(U32(at)));
            else if (t == 3)
                out += Array(at, depth + 1);
            else
                throw std::runtime_error("bad raP array element");
        }
        return out + "}";
    }

    void Body(size_t at, int depth)
    {
        if (depth > 64)
            throw std::runtime_error("raP class too deep");
        (void)Str(at); // parent name, written by the caller with the class header
        const uint32_t n = Count(at);
        const std::string indent(depth, '\t');
        for (uint32_t i = 0; i < n; i++)
        {
            const uint8_t type = Byte(at);
            if (type == 0)
            {
                const std::string name = Str(at);
                size_t body = U32(at);
                size_t peek = body;
                const std::string base = Str(peek);
                _out += indent + "class " + name + (base.empty() ? "" : " : " + base) + "\n" + indent + "{\n";
                Body(body, depth + 1);
                _out += indent + "};\n";
            }
            else if (type == 1)
            {
                const uint8_t sub = Byte(at);
                const std::string name = Str(at);
                std::string value;
                if (sub == 0 || sub == 4)
                    value = ScalarString(Str(at));
                else if (sub == 1)
                    value = Float(F32(at));
                else if (sub == 2)
                    value = std::to_string(static_cast<int32_t>(U32(at)));
                else
                    throw std::runtime_error("bad raP value");
                _out += indent + name + "=" + value + ";\n";
            }
            else if (type == 2)
            {
                const std::string name = Str(at);
                _out += indent + name + "[]=" + Array(at, 0) + ";\n";
            }
            else if (type == 3)
                _out += indent + "class " + Str(at) + ";\n";
            else if (type == 4)
                (void)Str(at); // "delete X;" -- the CWA config grammar has no delete, and
                               // the classes Arma 1 deletes are UI ones this tree never uses
            else if (type == 5)
            {
                (void)U32(at); // array-append flag
                const std::string name = Str(at);
                _out += indent + name + "[]=" + Array(at, 0) + ";\n";
            }
            else
                throw std::runtime_error("bad raP entry");
        }
    }

  public:
    explicit RapToText(const std::vector<uint8_t>& bytes) : _bytes(bytes) {}
    std::string Convert()
    {
        if (_bytes.size() < 16 || std::memcmp(_bytes.data(), "\0raP", 4) != 0)
            throw std::runtime_error("not a raP config");
        Body(16, 0);
        return _out;
    }
};

std::vector<uint8_t> ReadArchiveFile(const std::string& name)
{
    std::vector<uint8_t> bytes;
    QIFStreamB in;
    in.AutoOpen(name.c_str());
    while (!in.fail())
    {
        const int c = in.get();
        if (in.eof() || c < 0)
            break;
        bytes.push_back(static_cast<uint8_t>(c));
    }
    return bytes;
}

QFBank* FindBank(const std::string& prefix)
{
    for (int i = 0; i < GFileBanks.Size(); i++)
    {
        if (_stricmp((const char*)GFileBanks[i].GetPrefix(), prefix.c_str()) == 0)
            return &GFileBanks[i];
    }
    return nullptr;
}

std::vector<std::string> BankFiles(QFBank& bank, bool (*want)(const std::string& lowerName))
{
    struct Ctx
    {
        std::vector<std::string> names;
        bool (*want)(const std::string&);
    } ctx{{}, want};
    bank.ForEach(
        [](const FileInfoO& fi, const FileBankType*, void* context)
        {
            auto* c = static_cast<Ctx*>(context);
            const std::string name = (const char*)fi.name;
            if (c->want(Lower(name)))
                c->names.push_back(name);
        },
        &ctx);
    std::sort(ctx.names.begin(), ctx.names.end());
    return ctx.names;
}

bool IsConfigBin(const std::string& n)
{
    return n == "config.bin" || (n.size() > 11 && n.compare(n.size() - 11, 11, "\\config.bin") == 0) ||
           (n.size() > 11 && n.compare(n.size() - 11, 11, "/config.bin") == 0);
}

bool IsStringtable(const std::string& n)
{
    const size_t slash = n.find_last_of("\\/");
    const std::string leaf = slash == std::string::npos ? n : n.substr(slash + 1);
    return leaf.rfind("stringtable", 0) == 0 && leaf.size() > 4 && leaf.compare(leaf.size() - 4, 4, ".csv") == 0;
}

struct LoadedConfig
{
    std::string source;
    std::unique_ptr<ParamFile> file;
    std::vector<std::string> patches;
    std::vector<std::string> requiredAddons;
    bool isBase = false;
};

void ImportIntoPars(const std::string& text, const char* what)
{
    ParamFile tmp;
    QIStream in(text.data(), static_cast<int>(text.size()));
    tmp.Parse(in);
    const ParamAccessMode access = Pars.GetAccessMode();
    Pars.SetAccessMode(PAReadAndWrite);
    for (int i = 0; i < tmp.GetEntryCount(); i++)
    {
        if (ParamEntry* top = Pars.FindEntryNoInheritance(tmp.GetEntry(i).GetName()))
            top->SetAccessMode(PAReadAndWrite);
    }
    Pars.Update(tmp);
    Pars.SetAccessMode(access);
    Pars.SetFile(&Pars);
    LOG_INFO(Mission, "Arma 1: imported {} ({} bytes of config)", what, text.size());
}

// Config text for a class of the merged Arma 1 tree. Not ParamEntry::Save: a merged
// tree keeps members in merge order, so a class can precede the sibling it derives
// from (bin defines DefaultDie, the anims addon adds Default later) and the text would
// not parse back. Here every class is written after the siblings its subtree derives
// from.
void QuoteConfigString(std::string& out, const char* value)
{
    out += '"';
    for (const char* c = value ? value : ""; *c; c++)
    {
        if (*c == '"')
            out += "\"\"";
        else if (*c == '\n' || *c == '\r')
            out += ' ';
        else
            out += *c;
    }
    out += '"';
}

void AppendFloat(std::string& out, float value)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.9g", value);
    if (!std::strpbrk(buf, ".eEnN"))
        std::strcat(buf, ".0");
    out += buf;
}

void AppendArrayValue(std::string& out, const IParamArrayValue& v)
{
    if (v.IsArrayValue())
    {
        out += '{';
        for (int i = 0; i < v.GetItemCount(); i++)
        {
            if (i)
                out += ',';
            AppendArrayValue(out, *v.GetItem(i));
        }
        out += '}';
    }
    else if (v.IsFloatValue())
        AppendFloat(out, v.GetFloat());
    else if (v.IsIntValue())
        out += std::to_string(v.GetInt());
    else
        QuoteConfigString(out, (const char*)v.GetValueRaw());
}

void CollectBaseNames(const ParamEntry& cls, std::set<std::string>& names)
{
    if (const ParamClass* c = cls.GetClassInterface())
        if (const char* base = c->GetBaseName(); base && *base)
            names.insert(Lower(base));
    for (int i = 0; i < cls.GetEntryCount(); i++)
        if (cls.GetEntry(i).IsClass())
            CollectBaseNames(cls.GetEntry(i), names);
}

void AppendEntry(std::string& out, const ParamEntry& e, int indent);

void AppendMembers(std::string& out, const ParamEntry& cls, int indent)
{
    const int n = cls.GetEntryCount();
    for (int i = 0; i < n; i++)
        if (!cls.GetEntry(i).IsClass())
            AppendEntry(out, cls.GetEntry(i), indent);
    std::map<std::string, int> byName;
    for (int i = 0; i < n; i++)
        if (cls.GetEntry(i).IsClass())
            byName[Lower((const char*)cls.GetEntry(i).GetName())] = i;
    std::vector<int> state(n, 0); // 0 todo, 1 in progress, 2 written
    std::function<void(int)> visit = [&](int i)
    {
        if (state[i])
            return;
        state[i] = 1;
        std::set<std::string> bases;
        CollectBaseNames(cls.GetEntry(i), bases);
        const std::string self = Lower((const char*)cls.GetEntry(i).GetName());
        for (const std::string& b : bases)
        {
            auto it = byName.find(b);
            if (it != byName.end() && b != self)
                visit(it->second);
        }
        AppendEntry(out, cls.GetEntry(i), indent);
        state[i] = 2;
    };
    for (int i = 0; i < n; i++)
        if (cls.GetEntry(i).IsClass())
            visit(i);
}

void AppendEntry(std::string& out, const ParamEntry& e, int indent)
{
    const std::string pad(static_cast<size_t>(indent), '\t');
    const std::string name = (const char*)e.GetName();
    if (e.IsClass())
    {
        out += pad + "class " + name;
        if (const ParamClass* c = e.GetClassInterface())
            if (const char* base = c->GetBaseName(); base && *base)
                out += std::string(": ") + base;
        out += "\r\n" + pad + "{\r\n";
        AppendMembers(out, e, indent + 1);
        out += pad + "};\r\n";
    }
    else if (e.IsArray())
    {
        out += pad + name + "[]={";
        for (int i = 0; i < e.GetSize(); i++)
        {
            if (i)
                out += ',';
            AppendArrayValue(out, e[i]);
        }
        out += "};\r\n";
    }
    else
    {
        out += pad + name + "=";
        if (e.IsFloatValue())
            AppendFloat(out, (float)e);
        else if (e.IsIntValue())
            out += std::to_string(e.GetInt());
        else
            QuoteConfigString(out, (const char*)e.GetValueRaw());
        out += ";\r\n";
    }
}

std::string SaveText(const ParamEntry& entry)
{
    std::string out;
    AppendEntry(out, entry, 0);
    return out;
}

void ReplaceFirst(std::string& text, const std::string& from, const std::string& to)
{
    const size_t at = text.find(from);
    if (at != std::string::npos)
        text.replace(at, from.size(), to);
}

bool MovesImported()
{
    static int state = 0; // 0 untried, 1 ok, -1 failed
    if (state != 0)
        return state > 0;
    state = -1;
    const ParamEntry* basic = Arma1Pars().FindEntryNoInheritance("CfgMovesBasic");
    const ParamEntry* male = Arma1Pars().FindEntryNoInheritance("CfgMovesMaleSdr");
    if (!basic || !male || !male->FindEntry("States"))
    {
        LOG_WARN(Mission, "Arma 1: CfgMovesMaleSdr not found, soldiers keep CWA models");
        return false;
    }
    std::string basicText = SaveText(*basic);
    std::string maleText = SaveText(*male);
    ReplaceFirst(basicText, "class CfgMovesBasic", "class A1_CfgMovesBasic");
    ReplaceFirst(maleText, "class CfgMovesMaleSdr: CfgMovesBasic", "class A1_CfgMovesMaleSdr: A1_CfgMovesBasic");
    ReplaceFirst(maleText, "class CfgMovesMaleSdr", "class A1_CfgMovesMaleSdr");
    // CWA vehicles name their crew poses through CfgVehicleActions (vehicleActions).
    const size_t open = maleText.find('{');
    if (open == std::string::npos)
        return false;
    maleText.insert(open + 1, "\r\nvehicleActions=\"A1_CfgVehicleActions\";\r\n");

    // A1_CfgVehicleActions: every CWA vehicle action -> the closest Arma 1 crew state.
    const ParamEntry& states = (*male) >> "States";
    const auto has = [&](const char* s) { return states.FindEntry(s) != nullptr; };
    std::string actions = "class A1_CfgVehicleActions\r\n{\r\n";
    if (const ParamEntry* cwa = Pars.FindEntry("CfgVehicleActions"))
    {
        for (int i = 0; i < cwa->GetEntryCount(); i++)
        {
            const ParamEntry& e = cwa->GetEntry(i);
            const std::string name = (const char*)e.GetName();
            const std::string value = e.IsClass() ? std::string() : std::string((const char*)(RStringB)e);
            const std::string n = Lower(name + " " + value);
            const auto any = [&](std::initializer_list<const char*> words)
            {
                for (const char* w : words)
                    if (n.find(w) != std::string::npos)
                        return true;
                return false;
            };
            const bool out = any({"out"});
            const bool cargo = any({"cargo", "codriver", "passenger"});
            const bool gunner = !cargo && any({"gunner"});
            const bool commander = !cargo && any({"commander"});
            const bool pilot = any({"pilot"});
            std::string pick;
            if (has(name.c_str()))
                pick = name;
            else if (!value.empty() && has(value.c_str()))
                pick = value;
            else if (any({"cobra", "ah1", "uh60", "mi17", "mi24", "heli", "ch47", "oh58", "kiowa", "hind", "hip"}))
                pick = cargo ? "UH60_Cargo01" : gunner ? "UH60_Gunner" : "UH60_Pilot";
            else if (any({"a10", "su25", "plane", "cessna", "camel", "harrier", "mig"}))
                pick = "A10_Pilot";
            else if (any({"boat", "zodiac", "pbr", "rhib"}))
                pick = cargo ? "Zodiac_Cargo01" : "Zodiac_Driver";
            else if (any({"tank", "t72", "t80", "t55", "m1a", "m60", "abrams", "bmp", "m113", "m2a2", "bradley",
                          "brdm", "zsu", "vulcan", "apc", "bmd", "shilka"}))
            {
                if (cargo)
                    pick = "BMP2_Cargo04";
                else if (commander)
                    pick = out ? "T72_CommanderOut" : "T72_Commander";
                else if (gunner)
                    pick = out ? "T72_GunnerOut" : "T72_Gunner";
                else
                    pick = out ? "T72_DriverOut" : "T72_Driver";
            }
            else if (any({"truck", "ural", "5t", "bus", "tractor"}))
                pick = cargo ? "Truck_Cargo01" : gunner ? "Truck_Gunner" : "Truck_Driver";
            else if (any({"static", "m2", "dshk", "mg", "d30", "mortar", "nest"}))
                pick = "M2_Gunner";
            else if (any({"bike", "motorcycle"}))
                pick = cargo ? "Bike_Cargo" : "Bike_Driver";
            else if (cargo)
                pick = "UAZ_Cargo01";
            else if (gunner)
                pick = "UAZ_Gunner01";
            else if (commander)
                pick = "T72_Commander";
            else if (pilot || any({"driver", "jeep", "uaz", "car", "skoda", "hmmwv", "hummer"}))
                pick = "UAZ_Driver";
            else
                pick = "Crew";
            if (!has(pick.c_str()))
                pick = gunner ? "BasicSittingGunner" : (cargo ? "Crew" : "BasicDriver");
            actions += name + "=\"" + pick + "\";\r\n";
        }
    }
    actions += "};\r\n";

    ImportIntoPars(basicText + maleText + actions, "CfgMovesMaleSdr as A1_CfgMovesMaleSdr");
    if (!Pars.FindEntry("A1_CfgMovesMaleSdr"))
        return false;
    state = 1;
    return true;
}

// Values an Arma 1 man class carries for engine-facing names (bones, memory
// points, selections, hit points). Copied verbatim into the generated class; the
// engine reads them where present and keeps OFP's names otherwise.
const char* const kManNameKeys[] = {
    "boneHead", "weaponBone", "memoryPointLStep", "memoryPointRStep", "memoryPointAim", "memoryPointCameraTarget",
    "memoryPointAimingAxis", "memoryPointHeadAxis", "selectionHeadWound", "selectionBodyWound",
    "selectionLArmWound", "selectionRArmWound", "selectionLLegWound", "selectionRLegWound", "selectionHeadHide",
    "selectionNeckHide"};

std::string StringValue(const ParamEntry& cls, const char* key)
{
    const ParamEntry* e = cls.FindEntry(key);
    if (!e || e->IsClass() || e->IsArray())
        return {};
    return (const char*)(RStringB)*e;
}

bool IsKindOf(const ParamEntry* cls, const char* base)
{
    const ParamEntry* vehicles = Arma1Pars().FindEntry("CfgVehicles");
    const ParamEntry* baseEntry = vehicles ? vehicles->FindEntry(base) : nullptr;
    const ParamClass* c = cls ? cls->GetClassInterface() : nullptr;
    const ParamClass* b = baseEntry ? baseEntry->GetClassInterface() : nullptr;
    return c && b && (c == b || c->IsDerivedFrom(*b));
}
} // namespace

void LoadConfig()
{
    if (!Active() || ConfigLoaded())
        return;
    ConfigLoaded() = true;

    std::vector<LoadedConfig> configs;
    int stringtables = 0;

    // Dta\bin.pbo: the base config and global stringtable. Opened as a private bank,
    // never added to GFileBanks, so CWA's own bin\ keeps answering CWA's lookups.
    {
        const std::string binPath = RootFolder() + "\\Dta\\bin";
        QFBank dta;
        if (dta.open(RString(binPath.c_str())))
        {
            dta.Lock();
            const auto readPrivate = [&](const std::string& name)
            {
                std::vector<uint8_t> bytes;
                Ref<IFileBuffer> buf = dta.Read(name.c_str());
                if (buf && buf->GetSize() > 0)
                    bytes.assign(reinterpret_cast<const uint8_t*>(buf->GetData()),
                                 reinterpret_cast<const uint8_t*>(buf->GetData()) + buf->GetSize());
                return bytes;
            };
            for (const std::string& name : BankFiles(dta, IsStringtable))
            {
                const std::vector<uint8_t> bytes = readPrivate(name);
                if (bytes.empty())
                    continue;
                LoadArma1StringtableFromMemory(("arma1dta:bin\\" + name).c_str(),
                                               reinterpret_cast<const char*>(bytes.data()), bytes.size());
                stringtables++;
            }
            for (const std::string& name : BankFiles(dta, IsConfigBin))
            {
                const std::string path = "Dta\\bin.pbo:" + name;
                try
                {
                    const std::string text = RapToText(readPrivate(name)).Convert();
                    LOG_INFO(Mission, "Arma 1: parsing {} ({} bytes)", path, text.size());
                    LoadedConfig cfg;
                    cfg.source = path;
                    cfg.isBase = true;
                    cfg.file = std::make_unique<ParamFile>();
                    QIStream in(text.data(), static_cast<int>(text.size()));
                    cfg.file->Parse(in);
                    configs.insert(configs.begin(), std::move(cfg));
                }
                catch (const std::exception& e)
                {
                    LOG_WARN(Mission, "Arma 1: config {} not read: {}", path, e.what());
                }
            }
            dta.close();
        }
        else
        {
            LOG_WARN(Mission, "Arma 1: {}.pbo not readable, Arma 1 base config missing", binPath);
        }
    }
    for (const std::string& prefix : BankPrefixes())
    {
        QFBank* bank = FindBank(prefix);
        if (!bank)
            continue;
        for (const std::string& name : BankFiles(*bank, IsStringtable))
        {
            LoadStringtable("Arma1", RString((prefix + name).c_str()), 0, false);
            stringtables++;
        }
        for (const std::string& name : BankFiles(*bank, IsConfigBin))
        {
            const std::string path = prefix + name;
            try
            {
                const std::string text = RapToText(ReadArchiveFile(path)).Convert();
                LOG_INFO(Mission, "Arma 1: parsing {} ({} bytes)", path, text.size());
                LoadedConfig cfg;
                cfg.source = path;
                cfg.file = std::make_unique<ParamFile>();
                QIStream in(text.data(), static_cast<int>(text.size()));
                cfg.file->Parse(in);
                if (const ParamEntry* patches = cfg.file->FindEntry("CfgPatches"))
                {
                    for (int i = 0; i < patches->GetEntryCount(); i++)
                    {
                        const ParamEntry& patch = patches->GetEntry(i);
                        if (!patch.IsClass())
                            continue;
                        cfg.patches.push_back(Lower((const char*)patch.GetName()));
                        if (const ParamEntry* req = patch.FindEntry("requiredAddons"))
                            for (int k = 0; k < req->GetSize(); k++)
                                cfg.requiredAddons.push_back(Lower((const char*)(RStringB)(*req)[k]));
                    }
                }
                configs.push_back(std::move(cfg));
            }
            catch (const std::exception& e)
            {
                LOG_WARN(Mission, "Arma 1: config {} not read: {}", path, e.what());
            }
        }
    }

    // requiredAddons order (Kahn), bin first; unknown requirements are ignored.
    std::set<std::string> known;
    for (const auto& c : configs)
        known.insert(c.patches.begin(), c.patches.end());
    std::set<std::string> done;
    std::vector<bool> merged(configs.size(), false);
    size_t mergedCount = 0;
    while (mergedCount < configs.size())
    {
        bool progress = false;
        for (size_t i = 0; i < configs.size(); i++)
        {
            if (merged[i])
                continue;
            bool ready = true;
            for (const std::string& r : configs[i].requiredAddons)
                if (known.count(r) && !done.count(r) &&
                    std::find(configs[i].patches.begin(), configs[i].patches.end(), r) == configs[i].patches.end())
                    ready = false;
            if (!ready)
                continue;
            LOG_INFO(Mission, "Arma 1: merging {}", configs[i].source);
            Arma1Pars().Update(*configs[i].file);
            done.insert(configs[i].patches.begin(), configs[i].patches.end());
            merged[i] = true;
            mergedCount++;
            progress = true;
        }
        if (!progress)
        {
            for (size_t i = 0; i < configs.size(); i++)
                if (!merged[i])
                {
                    LOG_WARN(Mission, "Arma 1: dependency cycle, merging {} anyway", configs[i].source);
                    Arma1Pars().Update(*configs[i].file);
                    merged[i] = true;
                    mergedCount++;
                }
        }
    }
    Arma1Pars().SetFile(&Arma1Pars());
    const ParamEntry* vehicles = Arma1Pars().FindEntry("CfgVehicles");
    LOG_INFO(Mission, "Arma 1: config read from {} archives ({} CfgVehicles classes, {} stringtables)",
             configs.size(), vehicles ? vehicles->GetEntryCount() : 0, stringtables);
}

const ParamEntry* FindConfigClass(const char* root, const char* name)
{
    if (!Active() || !ConfigLoaded() || !root || !name || !*name)
        return nullptr;
    const ParamEntry* cls = Arma1Pars().FindEntry(root);
    return cls ? cls->FindEntry(name) : nullptr;
}

RString Arma1ModelClass(const RString& missionClass, const RString& cwaClass)
{
    if (!Active() || !ConfigLoaded())
        return cwaClass;
    const ParamEntry* a1 = FindConfigClass("CfgVehicles", missionClass);
    const ParamEntry* cwaVehicles = Pars.FindEntry("CfgVehicles");
    const ParamEntry* cwa = cwaVehicles ? cwaVehicles->FindEntry(cwaClass) : nullptr;
    if (!a1 || !cwa)
        return cwaClass;
    std::string model = StringValue(*a1, "model");
    if (model.empty() || model == "\\" || model.find("bmp.p3d") != std::string::npos)
        return cwaClass;
    std::string shape = model;
    if (shape.size() < 4 || Lower(shape.substr(shape.size() - 4)) != ".p3d")
        shape += ".p3d";
    if (!QIFStreamB::FileExist(shape[0] == '\\' ? shape.c_str() + 1 : shape.c_str()))
        return cwaClass;

    const bool man = IsKindOf(a1, "CAManBase") || IsKindOf(a1, "Man");
    if (man && !MovesImported())
        return cwaClass;

    const std::string generated = "A1x_" + std::string(missionClass.Data());
    if (cwaVehicles->FindEntry(generated.c_str()))
        return RString(generated.c_str());

    std::string text = "class CfgVehicles\r\n{\r\n\tclass " + std::string(cwaClass.Data()) + ";\r\n\tclass " +
                       generated + ": " + std::string(cwaClass.Data()) + "\r\n\t{\r\n";
    text += "\t\tmodel=\"" + model + "\";\r\n";
    const std::string displayName = StringValue(*a1, "displayName");
    if (!displayName.empty())
        text += "\t\tdisplayName=\"" + displayName + "\";\r\n";
    if (man)
    {
        text += "\t\tmoves=\"A1_CfgMovesMaleSdr\";\r\n";
        for (const char* key : kManNameKeys)
        {
            const std::string value = StringValue(*a1, key);
            if (!value.empty() || a1->FindEntry(key))
                text += std::string("\t\t") + key + "=\"" + value + "\";\r\n";
        }
        static const char* hits[][2] = {{"HitHead", "hitNameHead"},
                                        {"HitBody", "hitNameBody"},
                                        {"HitHands", "hitNameHands"},
                                        {"HitLegs", "hitNameLegs"}};
        for (const auto& hit : hits)
        {
            const ParamEntry* h = a1->FindEntry(hit[0]);
            if (h && h->IsClass())
            {
                const std::string name = StringValue(*h, "name");
                if (!name.empty())
                    text += std::string("\t\t") + hit[1] + "=\"" + name + "\";\r\n";
            }
        }
        // Skeleton bones the OFP code names in Czech.
        text += "\t\tboneLauncher=\"launcher\";\r\n\t\tboneLeftHand=\"lefthand\";\r\n\t\tboneRightHand=\"righthand\";\r\n";
    }
    text += "\t};\r\n};\r\n";
    ImportIntoPars(text, generated.c_str());
    if (!(Pars >> "CfgVehicles").FindEntry(generated.c_str()))
        return cwaClass;
    LOG_INFO(Mission, "Arma 1: '{}' uses the Arma 1 model {} on CWA '{}'{}", missionClass.Data(), model,
             cwaClass.Data(), man ? " with the Arma 1 moves" : "");
    return RString(generated.c_str());
}
} // namespace Poseidon::Arma1Compat
