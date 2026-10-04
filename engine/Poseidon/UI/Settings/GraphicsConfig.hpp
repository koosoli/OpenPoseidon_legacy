#pragma once

// Graphics user-settings persisted to <user-dir>/graphics.cfg.
//
// Sibling to AudioConfig + DisplayConfig — same SectionConfig pattern:
// plain-data class with sensible defaults, Normalize(env) against a
// runtime Environment, Load + Save against a path.  System-global —
// one file per machine, not per game profile.
//
// Live-apply screen (no Apply button): values flow to the engine the
// frame they change.  Persistence on Unmount mirrors AudioPage.
//
// Field grouping:
//
//   Tier rows           — driven by Quality Preset; touching one
//                         drops the preset display to "Custom".
//                           terrainDetail, objectLod, shadowQuality,
//                           particlesQuality
//
//   Per-user knobs      — independent of preset, keep value across
//                         preset changes:
//                           msaaSamples, renderScale, alphaToCoverage,
//                           multitexturing, vsync, fpsCap, brightness,
//                           gamma
//
// Autodetect on first boot: GraphicsConfig::PickPresetFromRam()
// returns a tier from SDL_GetSystemRAM(); ApplyPresetToTiers fills the
// four tier rows from a known bundle.  No CPU benchmark loop; benchmarks are
// unreliable on modern thermal-throttled CPUs.

#include <string>


namespace Poseidon
{
class GraphicsConfig
{
public:
	enum Preset
	{
		PresetLow    = 0,
		PresetMedium = 1,
		PresetHigh   = 2,
		PresetUltra  = 3,
		PresetCustom = 4,   // sentinel — UI shows "Custom" when the four tier rows
		                     // don't match any of Low/Medium/High/Ultra's bundle
	};
	// Extreme is legal only for Terrain. Off is legal only for Shadow and
	// Particles.
	enum Tier
	{
		TierOff    = 0,
		TierLow    = 1,
		TierMedium = 2,
		TierHigh   = 3,
		TierUltra  = 4,
		TierExtreme = 5,
	};
	enum VsyncMode
	{
		VsyncOff      = 0,
		VsyncOn       = 1,
		VsyncAdaptive = 2,
	};

	// Persistable fields.
	Preset    qualityPreset    = PresetUltra;
	Tier      terrainDetail    = TierUltra;     // → grid 6.25 m
	Tier      objectLod        = TierUltra;
	// GFX-001: Ultra, at the owner's request that Ultra be the default across the board.
	// HONEST NOTE, because the row will read "Ultra" and buy nothing: shadow and particle
	// tiers are ON/OFF ONLY at the engine (GraphicsApply.cpp:135 and :140) -- Low, Medium,
	// High and Ultra are indistinguishable until a shadow-distance bias hook and a tiered
	// cloudlet density exist. These two are labelling, not quality; terrain, object LOD and
	// grass are the three rows that actually do something.
	Tier      shadowQuality    = TierUltra;
	Tier      particlesQuality = TierUltra;
    // Grass / ground clutter. Ultra uses the shared GrassSettings coverage
    // default and a 41m detail ring. Lower tiers scale that default down.
	//
	// This row exists because grass was measured at 9.55 ms of a 33.40 ms GPU
	// frame -- 28.6% -- on perf_abel at 1920x1080, and until now there was no way
	// to spend that anywhere else: no options row, no persisted field, only the
	// undocumented WGR_GRASS* environment variables.  It is the largest single
	// lever in the renderer and it was the one the player could not reach.
	Tier      grassQuality     = TierUltra;
	VsyncMode vsync            = VsyncOn;
	int       fpsCap           = 0;             // 0 = Unlimited; valid: 0/30/60/90/120/144/240
	bool      alphaToCoverage  = true;          // MSAA alpha-to-coverage on cutout draws (fence wire,
	                                             // foliage) — grades sub-pixel features instead of the
	                                             // hard alpha-test keeping/killing whole pixels
	// Upscaler choice: -1 = AUTO (DLSS when the driver supports it, else FSR 1),
	// 0 = off (pure native), 1 = DLSS, 2 = FSR 1. Persisted; seeds WGR_DLSS /
	// WGR_RENDER_SCALE at boot and live-applies through the temporal tuning.
	// Tolerated as absent in old files (stays -1). Key name kept for compat.
	int       dlssMode         = -1;
	// Upscaler quality = internal render scale in percent: -1 = AUTO (67 when an
	// upscaler is active), else 50/58/67. Distinct from renderScale, which stays
	// the legacy SSAA (>100%) control.
	int       upscalerQuality  = -1;
	// RCAS sharpness in stops (0 = sharpest, 2 = mildest); drives FSR 1 and the
	// post-DLSS sharpen pass alike.
	float     fsrSharpness     = 0.25f;
	// RCAS pass over the DLSS output (owner default: on).
	int       dlssSharpen      = 1;
	// Temporal jitter phase count: 0 = auto (frozen offset at native scale so
	// nothing shimmers, cycling when upscaling so DLSS/FSR receive distinct
	// samples), 1 = frozen offset always. Upscaled setups that still shimmer
	// pin 1. Persisted per user; tolerated as absent.
	int       jitterPhases     = 0;
	// RFG-047: hand Enfusion's BC7/BC5/BC4 textures to the GPU compressed instead of
	// decoding them to 32-bit (1 = on, the default; 0 = the old decode path). Read at
	// renderer creation -- the choice has to be made before any texture is loaded -- so
	// it is seeded from here into POSEIDON_ENFUSION_COMPRESSED alongside WGR_MSAA rather
	// than applied live. The dev panel's Materials tab writes it.
	int       enfusionCompressedTextures = 1;
	int       enfusionLayerTint = 1;            // RFG-070 shared-tile colour rescale
	int       enfusionTintMultiply = 1;         // RFG-071 Color_N multiplies in linear space
	int       enfusionMultiLayers = 1;          // RFG-072 native MatPBRMulti: mask on UV set 2, Color_N per layer
	float     renderScale      = 1.0f;          // 1.0 = AUTO (renderer default, incl. DLSS Quality).
	                                            // <1 upscaling pin, >1 SSAA pin; live on wgpu.
	                                             // 1.0 = off; up to 2.0.  The only general cure for
	                                             // sub-pixel OPAQUE geometry sparkle (fence bars)
	int       msaaSamples      = 4;             // MSAA on the frame target: 0 (off) / 2 / 4 / 8.
	                                            // Matches the wgpu renderer's own default; startup-
	                                            // fixed there, so changes apply on the next launch.
	bool      multitexturing   = true;          // Detail-texture / specular second stage on terrain and
	                                             // objects.  Off drops those draws to the base texture
	float     brightness       = 1.6f;          // 0.4..1.8
	float     gamma            = 1.2f;          // 0.5..2.3

	// Schema version of the persisted file.  Load resets this to 0 first, so a
	// file written before versioning reads back as 0 and Migrate can act on it.
	static constexpr int kVersion = 3;
	int       version          = kVersion;

	// Bring a loaded config up to kVersion.  Returns true when the caller needs
	// to persist the result.  `refreshHz` is 0 when it cannot be read.
	bool Migrate(int refreshHz);

	// Rounds up to an allowed value, so the cap never sits below the monitor.
	static int FpsCapForRefreshRate(int refreshHz);

	// Reset every field to factory defaults.
	void LoadDefaults();

	// Validation against the runtime environment.  Out-of-range tier
	// values reset to the per-row default; out-of-range float sliders
	// clamp to their edges; FPS cap not in the allowed set rounds to
	// the nearest allowed value or 0.  Each field validates
	// independently — a stale terrain tier doesn't affect FPS cap.
	// Returns true if any field changed.
	struct Environment
	{
		virtual ~Environment() = default;
		// Total system RAM in MB (0 = unknown).  Used by PickPresetFromRam
		// for autodetect on first boot.
		virtual int GetSystemRamMB() const = 0;
	};
	bool Normalize(const Environment& env);

	bool Load(const std::string& path);
	bool Save(const std::string& path) const;

	// Helpers — used by the boot path's autodetect-on-first-run flow,
	// the Quality Preset row's write-through behaviour, and the
	// "preset == Custom when tiers diverge" UI logic.

	// Picks Low / Medium / High / Ultra from the system RAM amount.
	// 0 (unknown) maps to High — middle of the road; on any modern
	// machine SDL_GetSystemRAM should return a valid value.
	static Preset PickPresetFromRam(int ramMB);

	// Stamps the four tier rows from a preset's bundle.  Per-user knobs
	// (vsync / fpsCap / brightness / gamma) are NOT touched.  No-op for
	// PresetCustom (the UI never writes Custom to the bundle).
	void ApplyPresetToTiers(Preset preset);

	// Returns the Preset whose bundle matches the current tier rows,
	// or PresetCustom if no preset matches.  Drives the Quality Preset
	// row's displayed value when the user has touched a tier row.
	Preset DerivePresetFromTiers() const;
};

} // namespace Poseidon
