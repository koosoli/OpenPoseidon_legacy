#include <Poseidon/Foundation/PoseidonPCH.hpp>

#include <Poseidon/World/Terrain/TerrainSurfaceCatalog.hpp>

#include <Poseidon/Asset/Formats/Config/ArmaRap.hpp>

#include <algorithm>
#include <cctype>

namespace Poseidon
{
namespace
{
using Poseidon::Asset::Config::RapMember;
using Poseidon::Asset::Config::RapValue;

bool EqualsNoCase(const char* a, const char* b)
{
    if (a == nullptr || b == nullptr)
        return false;
    for (; *a != '\0' && *b != '\0'; ++a, ++b)
    {
        if (std::tolower(static_cast<unsigned char>(*a)) != std::tolower(static_cast<unsigned char>(*b)))
            return false;
    }
    return *a == *b;
}

const RapMember* FindClass(const std::vector<RapMember>& members, const char* name)
{
    for (const RapMember& m : members)
    {
        if (!m.body.empty() && EqualsNoCase(m.name.c_str(), name))
            return &m;
    }
    return nullptr;
}

const RapValue* FindValue(const RapMember& cls, const char* name)
{
    for (const RapMember& m : cls.body)
    {
        if (m.body.empty() && EqualsNoCase(m.name.c_str(), name))
            return &m.value;
    }
    return nullptr;
}

std::string ValueText(const RapMember& cls, const char* name)
{
    const RapValue* v = FindValue(cls, name);
    return v ? v->text : std::string();
}

float ValueNumber(const RapMember& cls, const char* name, float fallback)
{
    const RapValue* v = FindValue(cls, name);
    return v && v->array.empty() && v->text.empty() ? v->number : fallback;
}

// The basename without directory or extension, which is what a `files=` glob is
// written against: `ca\takistan\data\tk_trava_co.paa` -> `tk_trava_co`.
std::string PureName(const char* path)
{
    if (path == nullptr)
        return {};
    std::string s(path);
    const size_t slash = s.find_last_of("\\/");
    if (slash != std::string::npos)
        s.erase(0, slash + 1);
    const size_t dot = s.find_last_of('.');
    if (dot != std::string::npos)
        s.erase(dot);
    return s;
}

// The engine's own surface glob semantics (TextureBank's PatternMatch): `*` matches
// the rest, `?` matches exactly one character. Kept here rather than shared because
// the two callers disagree about the six-question-mark special case -- that one is a
// fix for pre-Arma data and has no business widening an Arma-generation match.
bool PatternMatch(const char* name, const char* pattern)
{
    for (;;)
    {
        if (*name != *pattern)
        {
            if (*pattern == '*')
                return true;
            if (*name == 0)
                return false;
            if (*pattern != '?')
                return false;
        }
        if (*pattern == 0)
            return true;
        ++name, ++pattern;
    }
}
} // namespace

bool TerrainSurfaceCatalog::AddConfig(const std::vector<uint8_t>& bytes)
{
    std::vector<RapMember> root;
    try
    {
        root = Asset::Config::ArmaRapReader(bytes).root();
    }
    catch (const std::exception&)
    {
        return false;
    }

    if (const RapMember* surfaces = FindClass(root, "CfgSurfaces"))
    {
        for (const RapMember& entry : surfaces->body)
        {
            if (entry.body.empty())
                continue;
            const std::string files = ValueText(entry, "files");
            if (files.empty())
                continue;
            Surface s;
            s.files = RStringB(files.c_str());
            const std::string character = ValueText(entry, "character");
            // "Empty" is how the data spells "this surface grows nothing", and it
            // is the majority: 8 of Takistan's 14 surfaces say it.
            if (!character.empty() && !EqualsNoCase(character.c_str(), "Empty"))
                s.character = RStringB(character.c_str());
            _surfaces.push_back(std::move(s));
        }
    }

    if (const RapMember* characters = FindClass(root, "CfgSurfaceCharacters"))
    {
        for (const RapMember& entry : characters->body)
        {
            if (entry.body.empty())
                continue;
            const RapValue* names = FindValue(entry, "names");
            const RapValue* probability = FindValue(entry, "probability");
            if (names == nullptr || names->array.empty())
                continue;
            CharacterEntry c;
            c.name = RStringB(entry.name.c_str());
            for (size_t i = 0; i < names->array.size(); ++i)
            {
                Clutter cl;
                cl.name = RStringB(names->array[i].text.c_str());
                // The two arrays are positional. A missing probability entry means
                // the data is malformed rather than that the clutter is certain, so
                // it contributes nothing rather than everything.
                cl.probability =
                    (probability != nullptr && i < probability->array.size()) ? probability->array[i].number : 0.0f;
                c.clutter.push_back(std::move(cl));
            }
            _characters.push_back(std::move(c));
        }
    }

    // `class clutter` hangs off the world class, and on Arma 3 it lives on CAWorld in
    // map_data while the world that uses it lives in its own addon -- so this walks
    // every CfgWorlds child rather than looking for one named world.
    if (const RapMember* worlds = FindClass(root, "CfgWorlds"))
    {
        for (const RapMember& world : worlds->body)
        {
            const RapMember* clutter = FindClass(world.body, "clutter");
            if (clutter == nullptr)
                continue;
            for (const RapMember& entry : clutter->body)
            {
                if (entry.body.empty())
                    continue;
                const std::string model = ValueText(entry, "model");
                if (model.empty())
                    continue;
                Clutter cl;
                cl.name = RStringB(entry.name.c_str());
                cl.model = RStringB(model.c_str());
                cl.scaleMin = ValueNumber(entry, "scaleMin", 1.0f);
                cl.scaleMax = ValueNumber(entry, "scaleMax", 1.0f);
                cl.affectedByWind = ValueNumber(entry, "affectedByWind", 0.0f);
                _clutter.push_back(std::move(cl));
            }
        }
    }
    return true;
}

RStringB TerrainSurfaceCatalog::Character(const char* surfaceTexture) const
{
    const std::string pure = PureName(surfaceTexture);
    if (pure.empty())
        return RStringB();
    for (const Surface& s : _surfaces)
    {
        if (PatternMatch(pure.c_str(), s.files.Data()))
            return s.character;
    }
    return RStringB();
}

bool TerrainSurfaceCatalog::Declares(const char* surfaceTexture) const
{
    const std::string pure = PureName(surfaceTexture);
    if (pure.empty())
        return false;
    for (const Surface& s : _surfaces)
    {
        if (PatternMatch(pure.c_str(), s.files.Data()))
            return true;
    }
    return false;
}

std::vector<TerrainSurfaceCatalog::Clutter> TerrainSurfaceCatalog::ClutterFor(const char* character) const
{
    if (character == nullptr || *character == '\0')
        return {};
    for (const CharacterEntry& c : _characters)
    {
        if (!EqualsNoCase(c.name.Data(), character))
            continue;
        std::vector<Clutter> out = c.clutter;
        for (Clutter& entry : out)
        {
            for (const Clutter& defined : _clutter)
            {
                if (EqualsNoCase(defined.name.Data(), entry.name.Data()))
                {
                    entry.model = defined.model;
                    entry.scaleMin = defined.scaleMin;
                    entry.scaleMax = defined.scaleMax;
                    entry.affectedByWind = defined.affectedByWind;
                    break;
                }
            }
        }
        return out;
    }
    return {};
}

float TerrainSurfaceCatalog::Coverage(const char* character) const
{
    float total = 0.0f;
    for (const Clutter& c : ClutterFor(character))
        total += c.probability;
    return total;
}

} // namespace Poseidon
