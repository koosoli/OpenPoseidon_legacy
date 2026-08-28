// SPDX-License-Identifier: GPL-3.0-or-later
#include <Poseidon/World/Entities/Weapons/Penetration.hpp>

#include <Poseidon/Foundation/Framework/Log.hpp>

#include <algorithm>
#include <mutex>
#include <set>
#include <string>

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace Poseidon::Penetration
{
namespace
{

std::atomic<bool>  g_enabled{[]
{
    const char* v = std::getenv("POSEIDON_PENETRATION");
    return v && std::strcmp(v, "0") != 0;
}()};
std::atomic<float> g_scale{1.0f};

// The table. Ordered by how much a material actually resists, which is the part
// that can be defended; the absolute values are estimates and the header says so.
//
// A useful anchor for reading them: a 5.56 round carries roughly 1700 J and
// presents about 24 mm^2. Against pine at 3e8 J/m^3 that is a little under 30 cm
// of wood before it stops -- the right order for a rifle round through timber,
// and the reason the constants are in this range rather than another.
//
// Matching is by TEXTURE PATH FRAGMENT because stock OFP declares no materials.
// First match wins, so the specific entries come before the general ones: `sklo`
// (glass) must be tested before `beton` in case a path contains both.
struct Entry
{
    const char* fragment;
    Material    material;
};

// Czech names, because that is what BIS used: sklo = glass, drevo = wood,
// plech = sheet metal, beton = concrete, cihla = brick, zelezo = iron.
const Entry kTable[] = {
    {"sklo", {"glass", 0.0f, true}},
    {"glass", {"glass", 0.0f, true}},
    {"okno", {"window", 0.0f, true}},
    {"listy", {"foliage", 0.0f, true}},
    {"tra", {"foliage", 0.0f, true}},
    {"latka", {"cloth", 2.0e7f, false}},
    {"drevo", {"wood", 3.0e8f, false}},
    {"wood", {"wood", 3.0e8f, false}},
    {"prkna", {"planks", 3.0e8f, false}},
    {"plech", {"sheet metal", 1.2e9f, false}},
    {"zelezo", {"iron", 4.0e9f, false}},
    {"kov", {"metal", 2.5e9f, false}},
    {"cihla", {"brick", 1.5e9f, false}},
    {"beton", {"concrete", 4.5e9f, false}},
    {"kamen", {"stone", 5.0e9f, false}},
    {"pisek", {"sand", 6.0e8f, false}},
    {"zem", {"earth", 5.0e8f, false}},
};

// Anything unrecognised. Deliberately HIGH rather than low: an unknown surface
// behaving like a wall keeps today's behaviour, while an unknown surface behaving
// like paper would let rounds through every model whose texture is not in the
// table above -- which is most of them.
const Material kDefault{"unknown", 4.0e9f, false};

} // namespace

const Material& Lookup(const char* texturePath)
{
    if (!texturePath || !*texturePath)
    {
        return kDefault;
    }
    for (const Entry& entry : kTable)
    {
        if (std::strstr(texturePath, entry.fragment))
        {
            return entry.material;
        }
    }

    // Log every texture that falls through, ONCE each.
    //
    // The table was written by guessing that OFP texture paths carry material
    // words. They largely do not -- stock textures are named after the OBJECT
    // (`hotel_door1_ca.paa`, `dvere_int3.paa`), so a wooden gate lands here and
    // resists like concrete, which is what the owner saw. Rather than guess more
    // words, this makes the game report what it actually meets: shoot around,
    // read PENMISS in the log, and the table can be built from the corpus instead
    // of from assumption.
    static std::mutex           mutex;
    static std::set<std::string> reported;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (reported.size() < 500 && reported.insert(texturePath).second)
        {
            LOG_WARN(World, "PENMISS: no material for '{}' -- resisting as unknown", texturePath);
        }
    }
    return kDefault;
}

Result Compute(const Material& material, float speed, float mass, float calibre, float thickness)
{
    Result result;
    if (speed <= 0.0f || mass <= 0.0f)
    {
        return result;
    }
    if (material.alwaysPasses)
    {
        // Glass and foliage: through, unchanged. Modelling a token loss here would
        // be a number nobody could defend for no visible gain.
        result.penetrated = true;
        result.exitSpeed = speed;
        return result;
    }
    if (thickness <= 0.0f)
    {
        result.penetrated = true;
        result.exitSpeed = speed;
        return result;
    }

    const float radius = calibre * 0.5f;
    const float area = 3.14159265f * radius * radius;
    const float energy = 0.5f * mass * speed * speed;
    const float cost = material.resistance * ResistanceScale() * area * thickness;

    result.energyLost = std::min(cost, energy);
    if (cost >= energy)
    {
        // Stopped inside. exitSpeed stays 0 and penetrated stays false.
        return result;
    }
    result.penetrated = true;
    result.exitSpeed = std::sqrt(2.0f * (energy - cost) / mass);
    return result;
}

bool  Enabled() { return g_enabled.load(std::memory_order_relaxed); }
void  SetEnabled(bool enabled) { g_enabled.store(enabled, std::memory_order_relaxed); }
float ResistanceScale() { return g_scale.load(std::memory_order_relaxed); }
void  SetResistanceScale(float scale) { g_scale.store(scale, std::memory_order_relaxed); }

} // namespace Poseidon::Penetration
