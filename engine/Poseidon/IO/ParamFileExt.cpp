#include <Poseidon/Core/Application.hpp>
#include <Poseidon/IO/ParamFileExt.hpp>

#include <Poseidon/IO/Streams/QBStream.hpp>

#include <Poseidon/UI/Locale/StringtableExt.hpp>
#include <Poseidon/UI/LocalMapWorlds.hpp>
#include <Poseidon/Foundation/Platform/AppConfig.hpp>
#include <algorithm>
#include <string>
#include <Poseidon/Foundation/Strings/Mbcs.hpp>
#include <string.h>
#include <Poseidon/Foundation/Common/FltOpts.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/Foundation/platform.hpp>

#ifndef ACCESS_ONLY
#include <Poseidon/IO/Serialization/ParamArchive.hpp>
#endif

namespace Poseidon
{

FontID GetFontID(RString baseName)
{
    int langID = English;
    // Strip path prefix (some resources use "fonts\X" or "\fonts\X" instead of just "X")
    const char* name = baseName;
    if (name[0] == '\\' || name[0] == '/')
        name++;
    if (_strnicmp(name, "fonts\\", 6) == 0 || _strnicmp(name, "fonts/", 6) == 0)
        name += 6;
    RString lookupName = name;

    const ParamEntry* cls = (Pars >> "CfgFonts").FindEntry(GLanguage);
    if (cls)
    {
        const ParamEntry* entry = cls->FindEntry(lookupName);
        if (entry)
        {
            lookupName = *entry;
            langID = Poseidon::Foundation::GetLangID();
        }
    }
    return FontID(GetDefaultName(lookupName, "fonts\\", ""), langID);
}

PackedColor GetPackedColor(const ParamEntry& entry)
{
    if (entry.GetSize() != 4)
    {
        return PackedWhite;
    }
    int r8 = toInt(entry[0].GetFloat() * 255);
    int g8 = toInt(entry[1].GetFloat() * 255);
    int b8 = toInt(entry[2].GetFloat() * 255);
    int a8 = toInt(entry[3].GetFloat() * 255);
    saturate(r8, 0, 255);
    saturate(g8, 0, 255);
    saturate(b8, 0, 255);
    saturate(a8, 0, 255);
    return PackedColor(r8, g8, b8, a8);
}

Color GetColor(const ParamEntry& entry)
{
    if (entry.GetSize() != 4)
    {
        return HWhite;
    }
    return Color(entry[0].GetFloat(), entry[1].GetFloat(), entry[2].GetFloat(), entry[3].GetFloat());
}

bool GetValue(SoundPars& pars, const ParamEntry& entry)
{
    if (entry.GetSize() < 3)
    {
        const static SoundPars nil = {};
        pars = nil;
        return false;
    }
    pars.name = GetSoundName(entry[0]);
    pars.vol = entry[1].GetFloat();
    pars.freq = entry[2].GetFloat();
    pars.freqRnd = 0;
    pars.volRnd = 0.05;
    return true;
}

bool GetValue(SoundPars& pars, const IParamArrayValue& entry)
{
    if (entry.GetItemCount() < 3)
    {
        const static SoundPars nil = {};
        pars = nil;
        return false;
    }
    pars.name = GetSoundName(entry[0]);
    pars.vol = entry[1].GetFloat();
    pars.freq = entry[2].GetFloat();
    pars.freqRnd = 0;
    pars.volRnd = 0.05;
    return true;
}

bool GetValue(PackedColor& val, const ParamEntry& entry)
{
    if (entry.GetSize() != 4)
    {
        val = PackedWhite;
        return false;
    }
    val = PackedColor(Color(entry[0].GetFloat(), entry[1].GetFloat(), entry[2].GetFloat(), entry[3].GetFloat()));
    return true;
}

bool GetValue(Color& val, const ParamEntry& entry)
{
    if (entry.GetSize() != 4)
    {
        val = HWhite;
        return false;
    }
    val = Color(entry[0].GetFloat(), entry[1].GetFloat(), entry[2].GetFloat(), entry[3].GetFloat());
    return true;
}

RString GetDefaultName(RString baseName, const char* dDir, const char* dExt)
{
    if (!baseName || !baseName[0])
    {
        return "";
    }
    char buf[256];
    *buf = 0;

    const char* name = baseName;
    // Leading slash: caller already supplied a fully-qualified path
    if (name[0] == '\\' || name[0] == '/')
    {
        name++;
    }
    else
    {
        // Skip the prefix if the name already starts with dDir (case-insensitive,
        // accepting '/' as an alternative to dDir's trailing '\\').
        // Mission description.ext entries like "sound/s01r01.ogg" otherwise
        // become "sound\sound/s01r01.ogg" and fail to resolve.
        const size_t dDirLen = strlen(dDir);
        const bool alreadyPrefixed =
            dDirLen > 0 && (_strnicmp(name, dDir, dDirLen) == 0 ||
                            (dDir[dDirLen - 1] == '\\' && _strnicmp(name, dDir, dDirLen - 1) == 0 &&
                             (name[dDirLen - 1] == '/' || name[dDirLen - 1] == '\\')));
        if (!alreadyPrefixed)
        {
            strncat(buf, dDir, sizeof(buf) - strlen(buf) - 1);
        }
    }
    strncat(buf, name, sizeof(buf) - strlen(buf) - 1);

    const char* ext = strchr(name, '.');
    if (!ext)
    {
        strncat(buf, dExt, sizeof(buf) - strlen(buf) - 1);
    }
    strlwr(buf);
    return buf;
}

RString GetShapeName(RString baseName)
{
    // MODEL paths are the one default-directory case where a name carrying a SUBDIRECTORY must
    // not be prefixed. `data3d\` is the OFP-era model folder and it is FLAT: every model that
    // legitimately lives there is `data3d\<name>.p3d`. Later BI games root their model paths at
    // the addon instead (`dz\structures\furniture\beds\postel_panelak1.p3d`), and those names
    // reach here from ODOL PROXY records, which -- unlike config `model=` entries -- are not
    // always written with a leading backslash. Prefixing one produced
    // `data3d\dz\structures\...`, which resolves to nothing.
    //
    // The failure was silent and it is what "the buildings are empty inside" looks like: proxies
    // are how an Arma 2 / DayZ house carries its furniture, so every bed, chair, cupboard, ladder
    // and interior sign attached this way vanished while the shell of the building rendered
    // perfectly. Measured across this repo's captured logs: 318 distinct data3d lookups, of which
    // the 303 flat ones all resolved and all 15 nested ones were addon paths that all failed.
    //
    // Deliberately NOT done in GetDefaultName: the sound path relies on the opposite behaviour
    // (a mission's `sound/s01r01.ogg` is relative to the mission and must keep its prefix), and
    // the reasoning above is about the model folder's flatness specifically.
    const char* name = baseName;
    if (name && name[0] && name[0] != '\\' && name[0] != '/' &&
        (strchr(name, '\\') != nullptr || strchr(name, '/') != nullptr))
    {
        return GetDefaultName(baseName, "", ".p3d");
    }
    return GetDefaultName(baseName, "data3d\\", ".p3d");
}
RString GetAnimationName(RString baseName)
{
    return GetDefaultName(baseName, "anim\\", ".rtm");
}
RString GetPictureName(RString baseName)
{
    return GetDefaultName(baseName, "data\\", ".paa");
}
RString GetSoundName(RString baseName)
{
    return GetDefaultName(baseName, "sound\\", ".wss");
}

#ifndef ACCESS_ONLY
LSError SoundPars::Serialize(ParamArchive& ar)
{
    PARAM_CHECK(ar.Serialize("name", name, 1))
    PARAM_CHECK(ar.Serialize("vol", vol, 1))
    PARAM_CHECK(ar.Serialize("freq", freq, 1))
    PARAM_CHECK(ar.Serialize("volRnd", volRnd, 1))
    PARAM_CHECK(ar.Serialize("freqRnd", freqRnd, 1))
    return LSOK;
}
#endif

RString GetWorldName(RString baseName)
{
    if (IsLocalMapWorldIdentifier(baseName.Data()))
    {
        // Preserve reserved identities even when their source metadata is missing.
        // Falling back to initWorld would silently load a saved foreign mission on OFP terrain.
        std::string alias = baseName.Data();
        std::replace(alias.begin(), alias.end(), '/', '\\');
        if (_strnicmp(alias.c_str(), "worlds\\", 7) == 0)
            return RString(alias.c_str());
        return RString(("worlds\\" + alias + ".wrp").c_str());
    }
    const ParamEntry* entry = (Pars >> "CfgWorlds").FindEntry(baseName);
    if (!entry)
    {
        baseName = Pars >> "CfgWorlds" >> "initWorld";
        entry = (Pars >> "CfgWorlds").FindEntry(baseName);
    }
    RString world = (*entry) >> "worldName";
    return GetDefaultName(world, "worlds\\", ".wrp");
}

RString SelectMenuInitWorld(RString initWorld, RString demoWorld, bool preferDemoWorld, bool initWorldExists,
                            bool demoWorldExists)
{
    if (preferDemoWorld && demoWorld.GetLength() > 0)
        return demoWorld;

    if (initWorldExists || demoWorld.GetLength() == 0 || !demoWorldExists)
        return initWorld;

    return demoWorld;
}

bool WorldInstalled(RString worldClass)
{
    if (IsLocalMapWorldIdentifier(worldClass.Data()))
    {
        const std::string& active = Foundation::AppConfig::Instance().GetLocalMapWorldId();
        return !active.empty() && FindLocalMapWorld(active) == FindLocalMapWorld(worldClass.Data()) &&
               IsLocalMapWorldAvailable(worldClass.Data());
    }
    if (worldClass.GetLength() == 0)
        return false;

    const ParamEntry* entry = (Pars >> "CfgWorlds").FindEntry(worldClass);
    if (!entry)
        return false;

    RString world = (*entry) >> "worldName";
    if (world.GetLength() == 0)
        return false;

    return QIFStreamB::FileExist(GetDefaultName(world, "worlds\\", ".wrp"));
}

RString GetMenuInitWorld()
{
    RString initWorld = Pars >> "CfgWorlds" >> "initWorld";
    RString demoWorld = Pars >> "CfgWorlds" >> "demoWorld";
    const bool preferDemoWorld = GApp && GApp->UseDemoWorld();
    const bool initWorldExists = WorldInstalled(initWorld);
    const bool demoWorldExists = WorldInstalled(demoWorld);

    RString selected = SelectMenuInitWorld(initWorld, demoWorld, preferDemoWorld, initWorldExists, demoWorldExists);
    if (!preferDemoWorld && selected != initWorld)
    {
        LOG_INFO(Core, "Menu intro world '{}' missing; using configured demoWorld '{}'", (const char*)initWorld,
                 (const char*)selected);
    }
    return selected;
}

static RString PathFirstFolder(const char* path)
{
    const char* next = strchr(path, '/');
    if (!next)
    {
        return "";
    }
    return RString(path, next - path);
}

} // namespace Poseidon

#include <Poseidon/Foundation/Algorithms/Crc.hpp>

using Poseidon::ParamEntry;
using Poseidon::ParamFile;

// PASumCalculator must be in namespace Poseidon because ParamFile.hpp
// declares the virtual interface using Poseidon::PASumCalculator.
namespace Poseidon
{

class PASumCalculator : public Foundation::CRCCalculator
{
};

} // namespace Poseidon

// At global scope so its RTTI name stays unmangled.
class PASumCalculatorFunctions : public Poseidon::CRCFunctions
{
  public:
    void Add(Poseidon::PASumCalculator& sum, const void* buffer, int size) override;
} GPASumCalculatorFunctions;

void PASumCalculatorFunctions::Add(Poseidon::PASumCalculator& sum, const void* buffer, int size)
{
    sum.Add(buffer, size);
}

const ParamEntry* FindConfigParamEntry(const char* path)
{
    if (*path == 0)
    {
        return nullptr;
    }
    // check path base
    const ParamEntry* entry = nullptr;
    Poseidon::Foundation::RString base = Poseidon::PathFirstFolder(path);
    if (!strcmpi(base, "cfg"))
    {
        entry = &Pars;
    }
    else if (!strcmpi(base, "rsc"))
    {
        entry = &Res;
    }
    else
    {
        LOG_DEBUG(Core, "Invalid base in {}", path);
        return nullptr;
    }
    path += strlen(base) + 1;

    while (strlen(path) > 0)
    {
        Poseidon::Foundation::RString base = Poseidon::PathFirstFolder(path);
        if (base.GetLength() <= 0)
        {
            ParamEntry* nEntry = entry->FindEntry(path);
            if (!nEntry)
            {
                break;
            }
            if (!nEntry->IsClass())
            {
                break;
            }
            entry = nEntry;
            break;
        }
        else
        {
            ParamEntry* nEntry = entry->FindEntry(base);
            if (!nEntry)
            {
                break;
            }
            if (!nEntry->IsClass())
            {
                break;
            }
            entry = nEntry;
            path += strlen(base) + 1;
        }
    }
    return entry;
}
unsigned int CalculateConfigCRC(const char* path)
{
    // check path base
    const ParamEntry* entry = FindConfigParamEntry(path);
    if (!entry)
    {
        Poseidon::PASumCalculator sum;
        sum.Reset();
        Pars.CalculateCheckValue(sum);
        Res.CalculateCheckValue(sum);
        return sum.GetResult();
    }

    Poseidon::PASumCalculator sum;
    sum.Reset();
    entry->CalculateCheckValue(sum);
    return sum.GetResult();
}

// Explicit registration — call once from program startup (typically via
// Poseidon::InitDefaults()). Registering here wins over any static-init-order
// clobber from ParamFile's dynamic default init.
void InitParamFileExtDefaults()
{
    ParamFile::SetDefaultCRCFunctions(&GPASumCalculatorFunctions);
}
