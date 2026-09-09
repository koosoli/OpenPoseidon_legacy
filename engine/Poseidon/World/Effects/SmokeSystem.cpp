#include <Poseidon/World/Effects/SmokeSystem.hpp>

#include <Poseidon/World/Effects/SmokeWorldQuery.hpp>
#include <Poseidon/World/World.hpp>
#include <Poseidon/World/Scene/Scene.hpp>
#include <Poseidon/Graphics/Rendering/Effects/Smokes.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/Core/Global.hpp>

#include <algorithm>
#include <cstdlib>

namespace Poseidon
{
namespace
{

/// The shared billboard shape both systems draw. Taking it from the scene's
/// preloaded set rather than loading our own is what guarantees the A/B
/// comparison is about simulation and batching, not about texture or blend
/// state: the legacy cloudlets draw this exact shape.
LODShapeWithShadow* CloudletShape()
{
    return GScene != nullptr ? GScene->Preloaded(CloudletBasic) : nullptr;
}

/// The flame billboard the legacy explosion/fire sources draw with. Null when
/// the data lacks it; the volume then simply has no fire layer.
LODShapeWithShadow* FireShape()
{
    return GScene != nullptr ? GScene->Preloaded(CloudletFire) : nullptr;
}

/// A legacy smoke source that does NOT blow up first.
///
/// `SmokeSourceVehicle::Simulate` calls `SimulateExplosion()` before it emits
/// anything, and a default-constructed `_explosionTime` is already <= the
/// mission clock — so a plain `new SmokeSourceVehicle(...)` detonates a
/// FuelExplosion on its very first tick, complete with sound and light. That is
/// correct for its real caller (a wreck that is *supposed* to cook off) and
/// useless as a smoke plume for comparison.
///
/// `SmokeSourceOnVehicle` solves this in its own constructor by pushing the
/// explosion past the end of time; this does the same. `_explosionTime` is
/// protected, which is why this has to be a subclass rather than a setter call.
class LegacyPlumeSource : public SmokeSourceVehicle
{
  public:
    LegacyPlumeSource(LODShapeWithShadow* shape, float density, float size)
        : SmokeSourceVehicle(shape, density, size, nullptr)
    {
        _explosionTime = TIME_MAX;
    }
};

} // namespace

const char* SmokeColorPresetName(SmokeColorPreset preset)
{
    switch (preset)
    {
        case SmokeColorPreset::White:
            return "White";
        case SmokeColorPreset::Black:
            return "Black";
        case SmokeColorPreset::Red:
            return "Red";
        case SmokeColorPreset::Green:
            return "Green";
        case SmokeColorPreset::Blue:
            return "Blue";
        case SmokeColorPreset::Yellow:
            return "Yellow";
        case SmokeColorPreset::Purple:
            return "Purple";
        case SmokeColorPreset::Orange:
            return "Orange";
    }
    return "White";
}

void ApplySmokeColorPreset(SmokeParams& params, SmokeColorPreset preset)
{
    switch (preset)
    {
        case SmokeColorPreset::White:
            params.red = 1.00f, params.green = 1.00f, params.blue = 1.00f;
            break;
        case SmokeColorPreset::Black:
            // Soot. Not (0,0,0): the tint multiplies a LIT billboard, and a true
            // zero produces a silhouette that reads as a hole in the world rather
            // than as smoke -- it loses the sun rim and the self-shadow shape that
            // make it a volume. This is as dark as it can go while still being lit
            // by something; the plume reads black because the opacity and the
            // self-shadow do the rest, not because the albedo is zero.
            params.red = 0.045f, params.green = 0.045f, params.blue = 0.05f;
            // Density is deliberately NOT set here: this function is what the
            // panel's Colour dropdown calls, and a colour control that silently
            // reset your thickness and self-shadow would be a nasty surprise.
            // The whole-look buttons (Burning wreck, INFERNO) set those.
            break;
        case SmokeColorPreset::Red:
            params.red = 1.00f, params.green = 0.16f, params.blue = 0.14f;
            break;
        case SmokeColorPreset::Green:
            params.red = 0.20f, params.green = 0.90f, params.blue = 0.30f;
            break;
        case SmokeColorPreset::Blue:
            params.red = 0.22f, params.green = 0.42f, params.blue = 1.00f;
            break;
        case SmokeColorPreset::Yellow:
            params.red = 1.00f, params.green = 0.88f, params.blue = 0.18f;
            break;
        case SmokeColorPreset::Purple:
            params.red = 0.68f, params.green = 0.26f, params.blue = 0.90f;
            break;
        case SmokeColorPreset::Orange:
            params.red = 1.00f, params.green = 0.50f, params.blue = 0.10f;
            break;
    }
}

int SmokeSystem::Spawn(Vector3Par position, const SmokeParams& params, float duration)
{
    LODShapeWithShadow* shape = CloudletShape();
    if (GWorld == nullptr || shape == nullptr)
    {
        // Loudly, because the failure is otherwise completely silent: the panel
        // button appears to work, -1 goes nowhere, and no smoke appears with no
        // clue as to why. Scene::Preloaded returns null whenever the preloader
        // has not run or the cloudlet model is missing from the data.
        LOG_WARN(World, "Smoke spawn refused: world={} cloudlet shape={}", GWorld != nullptr ? "ok" : "NULL",
                 shape != nullptr ? "ok" : "NULL (Scene::Preloaded(CloudletBasic))");
        return -1;
    }

    SmokeVolume* volume = new SmokeVolume(shape, FireShape(), params);
    volume->SetEmitterPosition(position);
    volume->SetPosition(position);
    volume->SetDuration(duration);

    // The world's cloudlet list owns it from here: it drives Simulate and Draw,
    // and it honours the `_delete` flag the volume raises when it is spent. The
    // registry below holds only a weak link, so a volume removed by the world
    // (mission end, teardown) cannot leave a dangling entry.
    GWorld->AddCloudlet(volume);

    Entry entry;
    entry.id = _nextId++;
    entry.legacy = false;
    entry.entity = volume;
    _entries.push_back(entry);

    LOG_INFO(World, "Smoke volume {} spawned at {:.1f},{:.1f},{:.1f} shape='{}' rate={:.0f}/s max={} duration={:.0f}",
             entry.id, position.X(), position.Y(), position.Z(), (const char*)shape->Name(), params.rate,
             params.maxParticles, duration);

    return entry.id;
}

int SmokeSystem::SpawnLegacy(Vector3Par position, float density, float size, float duration)
{
    LODShapeWithShadow* shape = CloudletShape();
    if (GWorld == nullptr || shape == nullptr)
    {
        LOG_WARN(World, "Legacy smoke spawn refused: world={} cloudlet shape={}", GWorld != nullptr ? "ok" : "NULL",
                 shape != nullptr ? "ok" : "NULL (Scene::Preloaded(CloudletBasic))");
        return -1;
    }

    // SmokeSourceVehicle is the legacy engine's standalone emitter — the thing a
    // burning wreck carries. It is the fairest available comparison: there is no
    // legacy "just a smoke plume" entity that is any simpler.
    LegacyPlumeSource* source = new LegacyPlumeSource(shape, density, size);
    source->SetPosition(position);
    // in / live / out. A long `out` matters: without it the legacy plume stops
    // dead rather than thinning, and the comparison would flatter the new one.
    source->SetSourceTimes(1.0f, duration < 0.0f ? 1e6f : duration, 6.0f);

    GWorld->AddCloudlet(source);

    LOG_INFO(World, "Legacy smoke source {} spawned at {:.1f},{:.1f},{:.1f} density={:.2f} size={:.2f}", _nextId,
             position.X(), position.Y(), position.Z(), density, size);

    Entry entry;
    entry.id = _nextId++;
    entry.legacy = true;
    entry.entity = source;
    _entries.push_back(entry);

    return entry.id;
}

bool SmokeSystem::Extinguish(int id)
{
    for (Entry& entry : _entries)
    {
        if (entry.id != id)
        {
            continue;
        }

        Entity* entity = entry.entity;
        if (entity == nullptr)
        {
            return false;
        }

        if (SmokeVolume* volume = dynamic_cast<SmokeVolume*>(entity))
        {
            volume->Extinguish();
        }
        else if (SmokeSourceVehicle* source = dynamic_cast<SmokeSourceVehicle*>(entity))
        {
            // The legacy source has no "stop" — its lifetime is the countdown it
            // was constructed with. Re-arming it with a zero live time and a
            // short fade is the only way to end it that does not simply delete
            // the entity out from under the cloudlets it has already dropped.
            source->SetSourceTimes(0.0f, 0.0f, 2.0f);
        }
        return true;
    }
    return false;
}

void SmokeSystem::ExtinguishAll()
{
    for (const Entry& entry : _entries)
    {
        Extinguish(entry.id);
    }
}

void SmokeSystem::Prune()
{
    _entries.erase(std::remove_if(_entries.begin(), _entries.end(),
                                  [](const Entry& entry) { return entry.entity == nullptr; }),
                   _entries.end());
}

SmokeStats SmokeSystem::Sample() const
{
    SmokeStats stats;
    stats.sweeps = SmokeSweepCount();

    for (const Entry& entry : _entries)
    {
        Entity* entity = entry.entity;
        if (entity == nullptr)
        {
            continue;
        }

        const SmokeVolume* volume = dynamic_cast<const SmokeVolume*>(entity);
        if (volume == nullptr)
        {
            continue;
        }

        ++stats.volumes;
        stats.particles += static_cast<int>(volume->ParticleCount());
        stats.simulateMicroseconds += volume->LastSimulateMicroseconds();
        stats.drawMicroseconds += volume->LastDrawMicroseconds();
    }

    if (GWorld != nullptr)
    {
        // Every legacy cloudlet in the world, minus our own volumes (which live
        // in the same list). Not filtered by source — the legacy system carries
        // no tag to filter on — so this is only meaningful on a quiet scene.
        stats.legacyCloudlets = GWorld->NCloudlets() - stats.volumes;
        if (stats.legacyCloudlets < 0)
        {
            stats.legacyCloudlets = 0;
        }
    }

    return stats;
}

void SmokeSystem::BeginFrame()
{
    ResetSmokeSweepCount();
}

void SmokeSystem::CollectShadowBlobs(std::vector<ShadowBlob>& out, std::size_t maxBlobs, bool force) const
{
    out.clear();
    if (!force && _groundShadowStrength <= 0.0f)
    {
        return;
    }
    for (const Entry& entry : _entries)
    {
        Entity* entity = entry.entity;
        const SmokeVolume* volume = entity != nullptr ? dynamic_cast<const SmokeVolume*>(entity) : nullptr;
        if (volume == nullptr)
        {
            continue;
        }
        volume->AppendShadowBlobs(out, !force);
    }
    // SMK-036: and the game's OWN smoke. Every grenade, wreck and shell burst is a legacy
    // cloudlet, none of which was ever in this pass -- which is why the dev smoke darkened
    // the grass under it and a thrown smoke grenade did not. The world's cloudlet list also
    // carries SmokeVolumes (already handled above), script particles and Things, so
    // SampleCloudletShadow rejects everything that is not a cloudlet.
    LegacySmokeShading& shading = GLegacySmokeShading();
    shading.blobs = 0;
    if ((force || shading.groundShadow) && GWorld != nullptr)
    {
        CloudletShadowSample sample;
        const int n = GWorld->NCloudlets();
        for (int i = 0; i < n; i++)
        {
            if (!SampleCloudletShadow(GWorld->GetCloudlet(i), sample))
            {
                continue;
            }
            ShadowBlob b{};
            b.x = sample.pos.X();
            b.y = sample.pos.Y();
            b.z = sample.pos.Z();
            b.radius = sample.radius;
            b.density = sample.density;
            b.heightAboveGround = sample.heightAboveGround;
            if (out.size() >= maxBlobs)
            {
                // The cap was declared and never enforced on this branch. SMK-038 raises it
                // to 2048 for injection, which makes overrunning it a real possibility
                // rather than a theoretical one.
                break;
            }
            out.push_back(b);
            ++shading.blobs;
        }
    }
    {
        // SMK-036 diagnostic. It must be able to say "the pass ran and found nothing"
        // separately from "the pass never ran", so it logs the world's cloudlet count too.
        static float nextReport = 0.0f;
        const float now = Glob.time.toFloat();
        if (now >= nextReport)
        {
            nextReport = now + 2.0f;
            LOG_INFO(World, "SMK-036 ground shadow: legacy={} strength={:.2f} cloudlets={} blobs={} total={}",
                     shading.groundShadow, _groundShadowStrength, GWorld != nullptr ? GWorld->NCloudlets() : -1,
                     shading.blobs, static_cast<long long>(out.size()));
        }
    }
    if (out.size() > maxBlobs)
    {
        // Keep the densest: partial sort so the cheap cap does not drop the
        // core of a plume in favour of its faint rim.
        std::nth_element(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(maxBlobs), out.end(),
                         [](const ShadowBlob& a, const ShadowBlob& b) { return a.density > b.density; });
        out.resize(maxBlobs);
    }
}

SmokeSystem& GSmokeSystemInstance()
{
    static SmokeSystem instance;
    // POSEIDON_SMOKE_GROUND_SHADOW=<f> overrides the global strength from a
    // script, so an A/B capture can be taken without the panel.
    static const bool seeded = []
    {
        if (const char* v = std::getenv("POSEIDON_SMOKE_GROUND_SHADOW"))
        {
            instance.SetGroundShadowStrength(static_cast<float>(std::atof(v)));
        }
        return true;
    }();
    (void)seeded;
    return instance;
}

} // namespace Poseidon
