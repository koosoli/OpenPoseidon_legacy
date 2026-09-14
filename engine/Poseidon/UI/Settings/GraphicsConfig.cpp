#include <Poseidon/UI/Settings/GraphicsConfig.hpp>

#include <Poseidon/IO/ParamFile/ParamFile.hpp>
#include <Poseidon/UI/Settings/SettingsFile.hpp>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <Poseidon/Foundation/Strings/RString.hpp>

namespace Poseidon
{

namespace
{
// Per-tier bundles for the four preset-driven rows.  Indexed by Preset
// (Low/Medium/High/Ultra); Custom is never used here.
struct TierBundle
{
    GraphicsConfig::Tier terrain;
    GraphicsConfig::Tier objectLod;
    GraphicsConfig::Tier shadow;
    GraphicsConfig::Tier particles;
    GraphicsConfig::Tier grass;
};
constexpr TierBundle kTierBundles[4] = {
    // Low — grass Off. It is 28.6% of the GPU frame at Ultra, so it is the
    // first thing a Low preset should stop paying for.
    {GraphicsConfig::TierLow, GraphicsConfig::TierLow, GraphicsConfig::TierOff, GraphicsConfig::TierOff,
     GraphicsConfig::TierOff},
    // Medium
    {GraphicsConfig::TierMedium, GraphicsConfig::TierMedium, GraphicsConfig::TierLow, GraphicsConfig::TierLow,
     GraphicsConfig::TierLow},
    // High
    {GraphicsConfig::TierHigh, GraphicsConfig::TierHigh, GraphicsConfig::TierMedium, GraphicsConfig::TierLow,
     GraphicsConfig::TierHigh},
    // Ultra — every row at the top. Shadow and particles are ON/OFF only at the engine
    // (GraphicsApply.cpp), so those two are honest labelling rather than added quality;
    // terrain, object LOD and grass are the rows with teeth.
    {GraphicsConfig::TierUltra, GraphicsConfig::TierUltra, GraphicsConfig::TierUltra, GraphicsConfig::TierUltra,
     GraphicsConfig::TierUltra},
};

bool IsValidTier(int v, bool allowOff)
{
    if (allowOff && v == GraphicsConfig::TierOff)
        return true;
    return v == GraphicsConfig::TierLow || v == GraphicsConfig::TierMedium || v == GraphicsConfig::TierHigh ||
           v == GraphicsConfig::TierUltra;
}

bool IsValidVsync(int v)
{
    return v == GraphicsConfig::VsyncOff || v == GraphicsConfig::VsyncOn || v == GraphicsConfig::VsyncAdaptive;
}

int NearestAllowedFps(int v)
{
    // Allowed: 0 (Unlimited), 30, 60, 90, 120, 144, 240.  Snap any
    // out-of-set value to the nearest allowed (treating 0 as
    // Unlimited and matching the row's stepper choices).
    constexpr int kAllowed[] = {0, 30, 60, 90, 120, 144, 240};
    if (v <= 0)
        return 0;
    int best = kAllowed[0];
    int bestDiff = std::abs(v - best);
    for (int allowed : kAllowed)
    {
        int diff = std::abs(v - allowed);
        if (diff < bestDiff)
        {
            best = allowed;
            bestDiff = diff;
        }
    }
    return best;
}

bool IsAllowedFps(int v)
{
    constexpr int kAllowed[] = {0, 30, 60, 90, 120, 144, 240};
    for (int allowed : kAllowed)
        if (v == allowed)
            return true;
    return false;
}
} // namespace

void GraphicsConfig::LoadDefaults()
{
    *this = GraphicsConfig{};
}

int GraphicsConfig::FpsCapForRefreshRate(int refreshHz)
{
    // Above any common refresh rate: with no display to pace to, this only has
    // to stop runaway submission.
    constexpr int kUnknownRefreshCap = 144;
    constexpr std::array kAllowed{30, 60, 90, 120, 144, 240};

    if (refreshHz <= 0)
        return kUnknownRefreshCap;
    for (int allowed : kAllowed)
        if (allowed >= refreshHz)
            return allowed;
    return kAllowed.back();
}

bool GraphicsConfig::Migrate(int refreshHz)
{
    if (version >= kVersion)
        return false;

    // 1.0 is the pre-v1 stamped default; any other value was chosen and stays.
    if (version < 1 && gamma == 1.0f)
        gamma = 1.2f;

    // A cap of 0 below v2 is indistinguishable from the old default, so it is
    // taken as never set.  From v2 on it is the user's Unlimited and survives --
    // which is why this is guarded by the version and not just by the value. It was
    // unguarded while kVersion was 2, where `version >= kVersion` above made it
    // unreachable for a v2 file; the v3 bump below made it reachable again and it
    // would have silently converted every deliberate "Unlimited" into a cap.
    if (version < 2 && fpsCap == 0)
        fpsCap = FpsCapForRefreshRate(refreshHz);

    // v3: the tier rows became live.  Until now LoadAndApplyGraphicsConfig overwrote
    // them with the Ultra bundle on EVERY boot, so whatever a pre-v3 file says, what
    // the player has actually been looking at is Ultra -- including on first boot,
    // where the RAM autodetect was written to disk and then overridden in the same
    // function.  Stamping the bundle here preserves the picture they have today; from
    // v3 on the file is honoured, so the Options page and the autodetect both work.
    if (version < 3)
    {
        ApplyPresetToTiers(PresetUltra);
        qualityPreset = PresetUltra;
        msaaSamples = 4;
        alphaToCoverage = true;
        renderScale = 1.0f;
    }

    version = kVersion;
    return true;
}

GraphicsConfig::Preset GraphicsConfig::PickPresetFromRam(int ramMB)
{
    // GFX-001. The owner asked for Ultra to be the default, so a first boot now starts there
    // and this only steps DOWN for a machine that genuinely cannot hold the working set.
    //
    // The buckets are RAM, and RAM is a proxy for the wrong thing -- the measured cost of
    // Ultra on this content is GPU-bound and grass-dominated (about 28% of the frame), not
    // memory-bound. A 32 GB machine with a weak GPU was already being told "Ultra" by this
    // function and a 12 GB machine with a strong one was being held at High. Keeping only the
    // low end means the guess is made where it is least likely to be wrong: below 8 GB the
    // machine really will struggle, and above it the player can move one slider.
    //
    // ramMB == 0 means "could not read it", and that now means Ultra rather than High -- an
    // unknown machine gets the default like everyone else, and the Options page is one click
    // away if it is too much.
    if (ramMB > 0 && ramMB < 4 * 1024)
        return PresetLow;
    if (ramMB > 0 && ramMB < 8 * 1024)
        return PresetMedium;
    return PresetUltra;
}

void GraphicsConfig::ApplyPresetToTiers(Preset preset)
{
    if (preset < PresetLow || preset > PresetUltra)
        return;
    const TierBundle& b = kTierBundles[preset];
    terrainDetail = b.terrain;
    objectLod = b.objectLod;
    shadowQuality = b.shadow;
    particlesQuality = b.particles;
    grassQuality = b.grass;
}

GraphicsConfig::Preset GraphicsConfig::DerivePresetFromTiers() const
{
    for (int p = PresetLow; p <= PresetUltra; ++p)
    {
        const TierBundle& b = kTierBundles[p];
        if (terrainDetail == b.terrain && objectLod == b.objectLod && shadowQuality == b.shadow &&
            particlesQuality == b.particles && grassQuality == b.grass)
            return static_cast<Preset>(p);
    }
    return PresetCustom;
}

bool GraphicsConfig::Normalize(const Environment& /*env*/)
{
    bool changed = false;

    // Quality Preset.  Out-of-range → re-derive from current tiers
    // (lets "Custom" be self-validating even if it slipped in via a
    // hand-edited file).
    if (qualityPreset < PresetLow || qualityPreset > PresetCustom)
    {
        qualityPreset = DerivePresetFromTiers();
        changed = true;
    }

    // Tier rows.  Off allowed only for Shadow + Particles.
    if (terrainDetail != TierExtreme && !IsValidTier(terrainDetail, /*allowOff=*/false))
    {
        terrainDetail = TierUltra;
        changed = true;
    }
    if (!IsValidTier(objectLod, /*allowOff=*/false))
    {
        objectLod = TierUltra;
        changed = true;
    }
    if (!IsValidTier(shadowQuality, /*allowOff=*/true))
    {
        shadowQuality = TierHigh;
        changed = true;
    }
    if (!IsValidTier(particlesQuality, /*allowOff=*/true))
    {
        particlesQuality = TierHigh;
        changed = true;
    }
    // Off is allowed: no grass at all is a legitimate choice, and on a weak GPU
    // it is worth 28.6% of the frame.
    if (!IsValidTier(grassQuality, /*allowOff=*/true))
    {
        grassQuality = TierUltra;
        changed = true;
    }

    // Per-user knobs.
    if (!IsValidVsync(vsync))
    {
        vsync = VsyncOn;
        changed = true;
    }
    if (!IsAllowedFps(fpsCap))
    {
        fpsCap = NearestAllowedFps(fpsCap);
        changed = true;
    }

    // Float clamps with "changed" iff the value moved.
    auto clampFloat = [&changed](float& v, float lo, float hi)
    {
        float c = std::clamp(v, lo, hi);
        if (c != v)
        {
            v = c;
            changed = true;
        }
    };
    clampFloat(brightness, 0.4f, 1.8f);
    clampFloat(gamma, 0.5f, 2.3f);
    clampFloat(renderScale, 1.0f, 2.0f);
    if (msaaSamples != 0 && msaaSamples != 2 && msaaSamples != 4 && msaaSamples != 8)
    {
        msaaSamples = msaaSamples >= 8 ? 8 : msaaSamples >= 4 ? 4 : msaaSamples >= 2 ? 2 : 0;
        changed = true;
    }

    return changed;
}

bool GraphicsConfig::Load(const std::string& path)
{
    ParamFile cfg;
    if (!ReadSettingsFile(path, cfg))
        return false;

    version = 0;
    if (auto* e = cfg.FindEntry("version"))
        version = (int)*e;

    version = 0;
    if (auto* e = cfg.FindEntry("version"))
        version = (int)*e;

    if (auto* e = cfg.FindEntry("qualityPreset"))
        qualityPreset = static_cast<Preset>((int)*e);
    if (auto* e = cfg.FindEntry("terrainDetail"))
        terrainDetail = static_cast<Tier>((int)*e);
    if (auto* e = cfg.FindEntry("objectLod"))
        objectLod = static_cast<Tier>((int)*e);
    if (auto* e = cfg.FindEntry("shadowQuality"))
        shadowQuality = static_cast<Tier>((int)*e);
    if (auto* e = cfg.FindEntry("particlesQuality"))
        particlesQuality = static_cast<Tier>((int)*e);
    if (auto* e = cfg.FindEntry("grassQuality"))
        grassQuality = static_cast<Tier>((int)*e);
    if (auto* e = cfg.FindEntry("vsync"))
        vsync = static_cast<VsyncMode>((int)*e);
    if (auto* e = cfg.FindEntry("fpsCap"))
        fpsCap = (int)*e;
    if (auto* e = cfg.FindEntry("alphaToCoverage"))
        alphaToCoverage = (int)*e != 0;
    if (auto* e = cfg.FindEntry("renderScale"))
        renderScale = (float)*e;
    if (auto* e = cfg.FindEntry("msaaSamples"))
        msaaSamples = (int)*e;
    if (auto* e = cfg.FindEntry("dlssMode"))
        dlssMode = (int)*e;
    if (auto* e = cfg.FindEntry("upscalerQuality"))
        upscalerQuality = (int)*e;
    if (auto* e = cfg.FindEntry("fsrSharpness"))
        fsrSharpness = (float)*e;
    if (auto* e = cfg.FindEntry("dlssSharpen"))
        dlssSharpen = (int)*e;
    if (auto* e = cfg.FindEntry("jitterPhases"))
        jitterPhases = (int)*e;
    if (auto* e = cfg.FindEntry("enfusionCompressedTextures"))
        enfusionCompressedTextures = (int)*e;
    if (auto* e = cfg.FindEntry("enfusionLayerTint"))
        enfusionLayerTint = (int)*e;
    if (auto* e = cfg.FindEntry("enfusionTintMultiply"))
        enfusionTintMultiply = (int)*e;
    if (auto* e = cfg.FindEntry("enfusionMultiLayers"))
        enfusionMultiLayers = (int)*e;
    if (auto* e = cfg.FindEntry("multitexturing"))
        multitexturing = (int)*e != 0;
    if (auto* e = cfg.FindEntry("brightness"))
        brightness = (float)*e;
    if (auto* e = cfg.FindEntry("gamma"))
        gamma = (float)*e;

    return true;
}

bool GraphicsConfig::Save(const std::string& path) const
{
    ParamFile cfg;
    cfg.Add("qualityPreset", static_cast<int>(qualityPreset));
    cfg.Add("terrainDetail", static_cast<int>(terrainDetail));
    cfg.Add("objectLod", static_cast<int>(objectLod));
    cfg.Add("shadowQuality", static_cast<int>(shadowQuality));
    cfg.Add("particlesQuality", static_cast<int>(particlesQuality));
    cfg.Add("grassQuality", static_cast<int>(grassQuality));
    cfg.Add("vsync", static_cast<int>(vsync));
    cfg.Add("fpsCap", fpsCap);
    cfg.Add("alphaToCoverage", alphaToCoverage ? 1 : 0);
    cfg.Add("renderScale", renderScale);
    cfg.Add("msaaSamples", msaaSamples);
    cfg.Add("dlssMode", dlssMode);
    cfg.Add("upscalerQuality", upscalerQuality);
    cfg.Add("fsrSharpness", fsrSharpness);
    cfg.Add("dlssSharpen", dlssSharpen);
    cfg.Add("jitterPhases", jitterPhases);
    cfg.Add("enfusionCompressedTextures", enfusionCompressedTextures);
    cfg.Add("enfusionLayerTint", enfusionLayerTint);
    cfg.Add("enfusionTintMultiply", enfusionTintMultiply);
    cfg.Add("enfusionMultiLayers", enfusionMultiLayers);
    cfg.Add("multitexturing", multitexturing ? 1 : 0);
    cfg.Add("brightness", brightness);
    cfg.Add("gamma", gamma);
    cfg.Add("version", version);

    return WriteSettingsFile(path, cfg);
}

} // namespace Poseidon
