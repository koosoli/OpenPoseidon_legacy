// OFP Fusion: ambient loops per island. See EnvSoundRegions.hpp.

#include <Poseidon/World/EnvSoundRegions.hpp>

#include <Poseidon/Foundation/Logging/Logging.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace Poseidon
{
namespace
{
// metres either side of the halfway line over which two regions' loops crossfade (Fusion's islands have >= 3.2 km
// of water between them, so the blend stays at sea)
constexpr float kBlend = 1000.0f;
const char* const kClasses[EnvSoundSet::Count] = {"Sea", "Trees", "Meadows", "Hills", "Rain"};

struct Region
{
    float xMin, zMin, xMax, zMax;
    EnvSoundSet set;

    float Distance(float x, float z) const
    {
        const float dx = std::max({xMin - x, 0.0f, x - xMax});
        const float dz = std::max({zMin - z, 0.0f, z - zMax});
        return std::sqrt(dx * dx + dz * dz);
    }
};

std::vector<Region> GRegions;
} // namespace

void InitEnvSoundRegions(const ParamEntry& world)
{
    GRegions.clear();
    const ParamEntry* regions = world.FindEntry("SoundRegions");
    if (!regions)
    {
        return;
    }
    for (int i = 0; i < regions->GetEntryCount(); i++)
    {
        const ParamEntry& cls = regions->GetEntry(i);
        const ParamEntry* area = cls.IsClass() ? cls.FindEntry("area") : nullptr;
        const ParamEntry* env = cls.IsClass() ? cls.FindEntry("EnvSounds") : nullptr;
        if (!area || area->GetSize() < 4 || !env)
        {
            continue;
        }
        Region region;
        region.xMin = (*area)[0];
        region.zMin = (*area)[1];
        region.xMax = (*area)[2];
        region.zMax = (*area)[3];
        for (int k = 0; k < EnvSoundSet::Count; k++)
        {
            GetValue(region.set.day[k], *env >> kClasses[k] >> "sound");
            GetValue(region.set.night[k], *env >> kClasses[k] >> "soundNight");
        }
        GRegions.push_back(region);
    }
    LOG_INFO(Audio, "Ambient sound regions: {}", GRegions.size());
}

int EnvSoundRegionsAt(float x, float z, const EnvSoundSet* sets[2], float weights[2])
{
    if (GRegions.empty())
    {
        return 0;
    }
    // the nearest two (distance 0 inside)
    int a = -1, b = -1;
    float da = 0.0f, db = 0.0f;
    for (int i = 0; i < static_cast<int>(GRegions.size()); i++)
    {
        const float d = GRegions[i].Distance(x, z);
        if (a < 0 || d < da)
        {
            b = a, db = da;
            a = i, da = d;
        }
        else if (b < 0 || d < db)
        {
            b = i, db = d;
        }
    }
    // half each on the halfway line, the nearer one's alone kBlend metres closer to it
    const float wb = b < 0 ? 0.0f : 0.5f * (1.0f - std::clamp((db - da) / (2.0f * kBlend), 0.0f, 1.0f));
    sets[0] = &GRegions[a].set;
    weights[0] = 1.0f - wb;
    if (wb <= 0.0f)
    {
        return 1;
    }
    sets[1] = &GRegions[b].set;
    weights[1] = wb;
    return 2;
}

} // namespace Poseidon
