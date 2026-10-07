mod bloom;
#[cfg(feature = "dlss")]
mod dlss;
#[cfg(feature = "dlss")]
mod dlss_device;
mod dlss_status;
mod dof;
mod exposure;
mod far;
mod ffi;
mod gfx2d;
mod gfx3d;
mod godrays;
mod gpu_timers;
mod grass;
mod handles;
mod log;
mod main_camera_tuple;
mod post_optics_probe;
mod demand_view_snapshot;
mod planar_mips;
mod shaders;
mod smoke_volume;
mod soft_particles;
mod sky;
mod layered_fog;
mod temporal;
mod terrain;
mod textures;
mod tonemap;
mod underwater;
mod upscaler;
// REN-THR-009 — the Send + Sync upload surface for CPU-path meshes.
mod uploader;
mod water;
mod rain_water;
mod rain_water_medium;
mod rain_water_reflections;
// TW-WATER W1 — the selectable water backend (Current OP / Tidewater Native).
mod water_backend;
mod water_tw;
mod water_visibility;

use std::sync::{Arc, Mutex};

use crate::bloom::Bloom;
use crate::dof::{DepthOfField, DofSettings};
use crate::exposure::Exposure;
use crate::far::Far;
use crate::ffi::{
    WgrCamera, WgrCmd, WgrDraw2DBatch, WgrDraw3D, WgrFarInstance, WgrGrassBatch, WgrGrassParams,
    WgrInstance, WgrLight, WgrMat4, WgrMeshVertex, WgrModelLod, WgrModelMaterial, WgrModelSection,
    WgrOverlayDraw, WgrOverlayVertex, WgrShadowCaster, WgrShadowPass, WgrTerrainBatch,
    WgrTerrainMaterial, WgrTerrainNode, WgrTerrainParams, WgrVec4, WgrVertex2D, WgrWaterBatch,
    WgrWaterCascadeConfig, WgrWaterInteractionEvent, WgrWaterInteractionParams, WgrWaterNode,
    WgrWaterParams,
};
use crate::gfx2d::{Gfx2d, SoftGlobals};
use crate::gfx3d::{Gfx3d, env_f32};
use crate::godrays::{GodRayFrame, GodRaySettings, GodRays};
use crate::gpu_timers::{GpuTimers, Region as TimerRegion};
use crate::grass::{Grass, GrassPass};
use crate::log::{LogSink, log_level};
use crate::planar_mips::PlanarMips;
use crate::sky::Sky;
use crate::terrain::Terrain;
use crate::textures::{SharedTextures, TextureData, TextureFormat};
use crate::tonemap::Tonemap;
use crate::water_backend::WaterBackend;
use crate::water_visibility::WaterVisibility;

// Offscreen HDR scene target format (see docs/hdr-pipeline-plan.md §0.2). Alpha kept
// for blending; full float precision to avoid banding in dark skies at night.
const HDR_FORMAT: wgpu::TextureFormat = wgpu::TextureFormat::Rgba16Float;

struct PlanarTarget {
    _color: wgpu::Texture,
    color_view: wgpu::TextureView,
    _sampled: wgpu::Texture,
    sampled_view: wgpu::TextureView,
    mip_views: Vec<wgpu::TextureView>,
    _depth: wgpu::Texture,
    depth_view: wgpu::TextureView,
    // Always a single-sample DepthOnly view: clouds use it for their depth-aware march.
    depth_sample_view: wgpu::TextureView,
    size: (u32, u32),
}

#[derive(Clone, Copy)]
struct UnderwaterView {
    cam_above: f32,
    camera_pos: [f32; 3],
    inv_view_proj: [f32; 16],
    shallow_color_ext: [f32; 4],
    deep_color: [f32; 4],
    sun_dir: [f32; 3],
    sun_radiance: [f32; 3],
    camera_shadow: ffi::WgrCameraShadow,
    cascade_lengths: [f32; 4],
    active_layers: u32,
    warp_amp: f32,
    sea_level: f32,
    debug_view: f32,
    wave_scale: f32,
    // WRL-003: the camera's body scales the wavy-surface search (a lake has ripples, not swell).
    body_wave_scale: f32,
    // WRL-006: containment ellipse of the camera's body (rx == 0: none).
    body_ellipse: [f32; 4],
    // (cos, sin) of the body's heading, for the ellipse frame.
    body_frame: [f32; 2],
    // Water-tab underwater tuning: absorption density multiplier, colour bias 0..1, caustic gain.
    density: f32,
    color_bias: f32,
    caustic_gain: f32,
}

// Like env_f32 but keeps a 0 value (env_f32 filters to >0 for scales). Used for the
// tonemap mode/encode toggles where 0 is a meaningful "off".
fn env_f32_opt(name: &str, default: f32) -> f32 {
    std::env::var(name)
        .ok()
        .and_then(|v| v.parse::<f32>().ok())
        .unwrap_or(default)
}

// sRGB -> linear for a single channel (matches the shader `srgb_to_linear`), for
// linearizing the CPU-side clear colour that seeds the HDR target.
fn srgb_to_linear_ch(c: f32) -> f32 {
    if c <= 0.04045 {
        c / 12.92
    } else {
        ((c + 0.055) / 1.055).powf(2.4)
    }
}

#[derive(Default)]
struct RuntimeDiagnostics {
    device_loss: Mutex<Option<String>>,
    uncaptured_error: Mutex<Option<String>>,
    /// RFG-082: errors that arrived while `uncaptured_error` was already holding one.
    /// The slot used to be overwritten, so the FIRST error of a poll window -- the
    /// out-of-memory that made a buffer invalid -- was replaced by the hundreds of
    /// "Buffer ... is invalid" that followed it, and the log showed only the
    /// consequences. Measured on Everon with RFG-075: 1,695 invalid-buffer errors and
    /// not one allocation failure, because every one of them had been overwritten.
    suppressed_errors: std::sync::atomic::AtomicU32,
}

struct ScreenshotPixels {
    width: u32,
    height: u32,
    rgba: Vec<u8>,
}

impl RuntimeDiagnostics {
    fn take_messages(&self) -> (Option<String>, Option<String>) {
        let suppressed = self
            .suppressed_errors
            .swap(0, std::sync::atomic::Ordering::Relaxed);
        let error = self
            .uncaptured_error
            .lock()
            .expect("device diagnostics poisoned")
            .take()
            .map(|message| {
                if suppressed > 0 {
                    format!("{message} (+{suppressed} more errors in this poll window, not shown)")
                } else {
                    message
                }
            });
        (
            self.device_loss
                .lock()
                .expect("device diagnostics poisoned")
                .take(),
            error,
        )
    }

    /// Keeps the first error of a poll window; later ones only count.
    fn note_uncaptured_error(&self, message: String) {
        let mut slot = self
            .uncaptured_error
            .lock()
            .expect("device diagnostics poisoned");
        if slot.is_none() {
            *slot = Some(message);
        } else {
            self.suppressed_errors
                .fetch_add(1, std::sync::atomic::Ordering::Relaxed);
        }
    }
}

// Hardware support alone is insufficient: wgpu validates against enabled device features.
fn enabled_format_flags(
    format: wgpu::TextureFormat,
    features: wgpu::Features,
    adapter_flags: wgpu::TextureFormatFeatureFlags,
) -> wgpu::TextureFormatFeatureFlags {
    if features.contains(wgpu::Features::TEXTURE_ADAPTER_SPECIFIC_FORMAT_FEATURES) {
        adapter_flags
    } else {
        format.guaranteed_format_features(features).flags
    }
}

#[cfg(test)]
mod msaa_feature_tests {
    use super::enabled_format_flags;
    use wgpu::{Features, TextureFormat, TextureFormatFeatureFlags as Flags};

    #[test]
    fn hardware_eight_samples_requires_enabled_format_feature() {
        let hardware = Flags::MULTISAMPLE_X4 | Flags::MULTISAMPLE_X8;
        let portable = enabled_format_flags(TextureFormat::Rgba16Float, Features::empty(), hardware);
        assert!(portable.sample_count_supported(4));
        assert!(!portable.sample_count_supported(8));
        let enabled = enabled_format_flags(TextureFormat::Rgba16Float,
            Features::TEXTURE_ADAPTER_SPECIFIC_FORMAT_FEATURES, hardware);
        assert!(enabled.sample_count_supported(8));
    }

    #[test]
    fn enabled_feature_does_not_invent_hardware_support() {
        let enabled = enabled_format_flags(TextureFormat::Rgba16Float,
            Features::TEXTURE_ADAPTER_SPECIFIC_FORMAT_FEATURES, Flags::MULTISAMPLE_X4);
        assert!(!enabled.sample_count_supported(8));
    }
}

#[cfg(test)]
mod planar_coverage_tests {
    use super::{PlanarConfig, screen_coverage_of_water};
    use crate::ffi::{WgrCamera, WgrWaterNode};

    // Camera at `eye` looking down -Z, matching the engine's convention: `view` carries rotation
    // only (geometry is camera-relative) and `proj` is a plain RH perspective — reversed-Z only
    // rewrites clip.z, which coverage never reads.
    fn camera(eye: glam::Vec3) -> WgrCamera {
        let mut cam: WgrCamera = bytemuck::Zeroable::zeroed();
        cam.proj = glam::Mat4::perspective_rh(70f32.to_radians(), 16.0 / 9.0, 0.1, 10000.0)
            .to_cols_array();
        cam.view = glam::Mat4::IDENTITY.to_cols_array();
        cam.cam_pos = [eye.x, eye.y, eye.z, 1.0];
        cam
    }

    fn node(x: f32, z: f32, size: f32) -> WgrWaterNode {
        let mut n: WgrWaterNode = bytemuck::Zeroable::zeroed();
        n.origin = glam::Vec2::new(x, z);
        n.size = size;
        n
    }

    #[test]
    fn no_water_is_no_coverage() {
        assert_eq!(
            screen_coverage_of_water(&[], &camera(glam::Vec3::Y), 0.0),
            0.0
        );
    }

    // Water behind the eye must not hold the reflection on. The camera looks down -Z, so +Z is
    // behind it.
    #[test]
    fn water_behind_the_camera_is_ignored() {
        let cam = camera(glam::Vec3::new(0.0, 20.0, 0.0));
        let cov = screen_coverage_of_water(&[node(-50.0, 100.0, 100.0)], &cam, 0.0);
        assert_eq!(cov, 0.0, "water behind the eye reported {cov}");
    }

    // The case the gate exists for: the owner's Stratis camera is a hillside view where the sea
    // is a distant strip. A small far patch must read as a SMALL fraction, or the gate can never
    // fire and the 7.94 ms is never recovered.
    #[test]
    fn a_distant_patch_is_a_small_fraction() {
        let cam = camera(glam::Vec3::new(0.0, 60.0, 0.0));
        let cov = screen_coverage_of_water(&[node(-40.0, -2000.0, 80.0)], &cam, 0.0);
        assert!(cov > 0.0, "a visible patch must register at all");
        assert!(cov < 0.05, "distant 80 m patch covered {cov} of the screen");
    }

    // ...and the converse, which is the safety direction: standing in the shallows, the node
    // under the eye straddles the near plane and must saturate rather than produce a small
    // finite AABB. Getting this backwards would disable the reflection exactly where it is most
    // visible.
    #[test]
    fn water_at_the_camera_saturates() {
        let cam = camera(glam::Vec3::new(0.0, 1.5, 0.0));
        let cov = screen_coverage_of_water(&[node(-500.0, -500.0, 1000.0)], &cam, 0.0);
        assert_eq!(cov, 1.0, "water under the camera reported {cov}");
    }

    // The divisor is the cost knob; pin both ends and the direction. A gentler slope would
    // silently stop saving anything.
    // The fade must be a skip-hider, not a quality dial. If it ever reaches up to `full` again,
    // every coastal camera silently loses reflection strength — a visual regression disguised as
    // a performance change, and one nobody would attribute to this gate.
    #[test]
    fn fade_is_full_across_the_normal_viewing_range() {
        let c = PlanarConfig::default();
        assert_eq!(c.fade_for(0.0), 0.0, "below the cut the reflection is gone");
        assert_eq!(c.fade_for(c.cut), 0.0, "the fade reaches zero AT the cut");
        assert!(c.fade_for(c.cut * 2.0) > 0.0 && c.fade_for(c.cut * 2.0) < 1.0);
        assert_eq!(c.fade_for(c.cut * 3.0), 1.0);
        // Anything a player would call "there is sea in this shot" is untouched.
        for cov in [0.02f32, 0.05, 0.1, 0.3, 1.0] {
            assert_eq!(c.fade_for(cov), 1.0, "coverage {cov} was dimmed");
        }
    }

    #[test]
    fn divisor_grows_as_coverage_shrinks() {
        let c = PlanarConfig::default();
        assert_eq!(
            c.divisor_for(1.0),
            c.min_div,
            "full screen of water = best quality"
        );
        assert_eq!(c.divisor_for(c.full), c.min_div);
        assert_eq!(c.divisor_for(0.0), c.max_div, "a sliver = cheapest");
        let quarter = c.divisor_for(c.full * 0.25);
        assert!(
            quarter > c.min_div && quarter <= c.max_div,
            "quarter coverage gave divisor {quarter}"
        );
        // Monotone: a camera panning water off screen must never make the reflection MORE
        // expensive on the way out.
        let mut prev = 0u32;
        for i in 0..=20 {
            let d = c.divisor_for(c.full * (1.0 - i as f32 / 20.0));
            assert!(
                d >= prev,
                "divisor fell from {prev} to {d} as coverage dropped"
            );
            prev = d;
        }
    }
}

#[cfg(test)]
mod runtime_diagnostics_tests {
    use super::RuntimeDiagnostics;

    /// The startup gate summary is the answer to "is this feature actually on?", a question
    /// that cost a wrong conclusion in the RND-030 audit because each gate is decided across
    /// three layers (Rust default, C++ default, and the app setting the env var itself).
    /// Assert every gate is still named in it, so a rename or a deletion fails here rather
    /// than silently removing the only reliable answer.
    #[test]
    fn effective_gate_summary_names_every_gate() {
        let source = include_str!("lib.rs");
        let gates = [
            "hdr=",
            "prepass=",
            "indirect=",
            "gpu_driven=",
            "skin_bake=",
            "msaa=",
            "multi_draw_count=",
        ];
        // Check EVERY line mentioning the marker, not the first: include_str! pulls in this
        // test too, so the first match is this test's own search string.
        let found = source
            .lines()
            .filter(|l| l.contains("[wgr] effective gates:"))
            .any(|l| gates.iter().all(|g| l.contains(g)));
        assert!(
            found,
            "no startup gate summary names all of {gates:?} — a gate was renamed or dropped"
        );
    }

    #[test]
    fn reports_device_loss_and_uncaptured_error_once() {
        let diagnostics = RuntimeDiagnostics::default();
        *diagnostics.device_loss.lock().unwrap() = Some("device lost (Destroyed)".to_owned());
        *diagnostics.uncaptured_error.lock().unwrap() = Some("validation error".to_owned());

        assert_eq!(
            diagnostics.take_messages(),
            (
                Some("device lost (Destroyed)".to_owned()),
                Some("validation error".to_owned())
            )
        );
        // A frame must not keep reporting stale failures after it consumed them.
        assert_eq!(diagnostics.take_messages(), (None, None));
    }

    #[test]
    fn keeps_the_first_error_of_a_window_and_counts_the_rest() {
        let diagnostics = RuntimeDiagnostics::default();
        diagnostics.note_uncaptured_error("out of memory".to_owned());
        diagnostics.note_uncaptured_error("buffer is invalid".to_owned());
        diagnostics.note_uncaptured_error("buffer is invalid".to_owned());
        assert_eq!(
            diagnostics.take_messages().1,
            Some("out of memory (+2 more errors in this poll window, not shown)".to_owned())
        );
        assert_eq!(diagnostics.take_messages(), (None, None));
    }
}

/// Feature ablation for A/B measurement, from `WGR_ABLATE` (comma-separated, e.g.
/// `WGR_ABLATE=grass,ao`). Default: nothing ablated.
///
/// WHY THIS EXISTS AND WHY IT IS NOT JUST MORE `WGR_*` GATES. Several subsystems already had a
/// renderer-side env gate — `WGR_GRASS`, `WGR_SKY_VIS` — and every one of them is INERT at
/// runtime, because C++ rebuilds and pushes the corresponding settings struct every single frame
/// (`SetGrassSettings`, `wgr_set_render_params`). Those gates decide frame 0 and are then
/// overwritten, so "grass off" A/B runs measured grass ON and the difference read as noise. That
/// is a measurement trap, not a feature gap: it produces a plausible number that is wrong.
///
/// These are applied at the points where the per-frame push LANDS, so nothing downstream can undo
/// them, and they are read once and cached so a hot path never touches the environment.
#[derive(Clone, Copy, Default)]
struct Ablate {
    grass: bool,
    ao: bool,
    skyvis: bool,
    planar: bool,
    clouds: bool,
}

static ABLATE: std::sync::OnceLock<Ablate> = std::sync::OnceLock::new();

/// Planar-reflection cost policy, from `WGR_PLANAR=cut,full,min_div,max_div`.
///
/// PERF-002 measured the reflection at 7.94 ms of a 34 ms 1080p frame — 23% — and none of it
/// scaled with how much water was on screen. These four numbers make it scale.
#[derive(Clone, Copy)]
struct PlanarConfig {
    /// Screen coverage (0..1) below which the pass is skipped outright.
    cut: f32,
    /// Coverage at or above which the reflection runs at its cheapest divisor (`min_div`).
    /// This is the RESOLUTION ramp's reference only — it must not be confused with the fade
    /// band, which is deliberately much narrower (see `fade_for`).
    full: f32,
    /// Resolution divisor at full coverage. 2 = half res, the shipped behaviour.
    min_div: u32,
    /// Resolution divisor when water is a sliver — the reflection of a distant strip does not
    /// need the pixels, and the consumer samples a roughness-blurred mip chain regardless.
    max_div: u32,
}

impl Default for PlanarConfig {
    fn default() -> Self {
        // Deliberately conservative: at >=6% coverage nothing changes from the shipped
        // behaviour at all. The divisor only starts climbing below that, and the pass is only
        // skipped once water is under half a percent of the screen — by which point `planar_fade`
        // has already dissolved its contribution to zero, so the skip is invisible by
        // construction rather than by tuning.
        PlanarConfig {
            cut: 0.005,
            full: 0.06,
            min_div: 2,
            max_div: 6,
        }
    }
}

static PLANAR_CFG: std::sync::OnceLock<PlanarConfig> = std::sync::OnceLock::new();

/// Consecutive frames a new planar divisor must be wanted before it is committed. ~1/3 s at
/// 60 fps: long enough that walking toward the shore does not realloc repeatedly, short enough
/// that the resolution has caught up before the player has finished turning.
const PLANAR_DIV_HOLD: u32 = 20;

impl PlanarConfig {
    fn from_env() -> Self {
        let mut c = PlanarConfig::default();
        if let Ok(raw) = std::env::var("WGR_PLANAR") {
            let f: Vec<f32> = raw
                .split(',')
                .filter_map(|v| v.trim().parse::<f32>().ok())
                .collect();
            if let Some(v) = f.first() {
                c.cut = v.clamp(0.0, 0.5);
            }
            if let Some(v) = f.get(1) {
                c.full = v.clamp(0.0, 1.0);
            }
            if let Some(v) = f.get(2) {
                c.min_div = (*v as u32).clamp(1, 16);
            }
            if let Some(v) = f.get(3) {
                c.max_div = (*v as u32).clamp(1, 16);
            }
        }
        c.full = c.full.max(c.cut);
        c.max_div = c.max_div.max(c.min_div);
        eprintln!(
            "[wgr] planar reflection policy: cut={:.3} full={:.3} div={}..{} (WGR_PLANAR)",
            c.cut, c.full, c.min_div, c.max_div
        );
        c
    }

    /// Weight the planar reflection is blended at, 0..1.
    ///
    /// The dissolve band is deliberately NARROW and sits immediately above `cut` — it exists
    /// only to make the skip invisible, NOT as a quality dial. Spreading it up to `full` (6%)
    /// would have dimmed the reflection across the entire normal viewing range, which is a
    /// visual regression wearing a performance change's clothes: every coastal camera would have
    /// lost reflection strength for nothing, since below `full` the saving already comes from the
    /// resolution divisor. Above 3x `cut` the reflection is at full weight exactly as before.
    fn fade_for(&self, coverage: f32) -> f32 {
        let band_end = self.cut * 3.0;
        if coverage >= band_end {
            return 1.0;
        }
        if band_end <= self.cut {
            return 1.0;
        }
        ((coverage - self.cut) / (band_end - self.cut)).clamp(0.0, 1.0)
    }

    /// Resolution divisor for this coverage. Reflection pixels worth rendering scale with the
    /// water's screen AREA, so the linear divisor scales with 1/sqrt(coverage) — half the
    /// coverage, ~1.4x the divisor. Quantised to whole divisors by the caller's stickiness.
    fn divisor_for(&self, coverage: f32) -> u32 {
        if coverage >= self.full {
            return self.min_div;
        }
        let ratio = (self.full / coverage.max(1e-4)).sqrt();
        ((self.min_div as f32 * ratio).round() as u32).clamp(self.min_div, self.max_div)
    }
}

/// Fraction of the screen (0..1) the water surface covers for `cam`.
///
/// The water nodes are a non-overlapping CDLOD quadtree of flat quads at `sea`, so projecting
/// each node's corners and summing their clipped screen AABBs is a genuine coverage estimate for
/// a few microseconds of CPU. It OVER-estimates in three ways — screen AABB rather than the
/// projected trapezoid, no occlusion by terrain in front of the water, and neighbouring nodes'
/// AABBs can overlap slightly — and every one of those errs toward keeping the reflection ON,
/// which is the direction a cost gate must fail in.
fn screen_coverage_of_water(nodes: &[WgrWaterNode], cam: &WgrCamera, sea: f32) -> f32 {
    if nodes.is_empty() {
        return 0.0;
    }
    // `view` has its translation zeroed (geometry is camera-relative), so world points are
    // projected as `proj * view * (world - cam_pos)`. Reversed-Z only rewrites clip.z, which
    // coverage never reads.
    let vp = glam::Mat4::from_cols_array(&cam.proj) * glam::Mat4::from_cols_array(&cam.view);
    let eye = glam::Vec3::new(cam.cam_pos[0], cam.cam_pos[1], cam.cam_pos[2]);
    let mut area = 0.0f32;
    for node in nodes {
        let (x0, z0) = (node.origin[0], node.origin[1]);
        let (x1, z1) = (x0 + node.size, z0 + node.size);
        let mut lo = glam::Vec2::splat(f32::INFINITY);
        let mut hi = glam::Vec2::splat(f32::NEG_INFINITY);
        let mut behind = 0;
        for (cx, cz) in [(x0, z0), (x1, z0), (x0, z1), (x1, z1)] {
            let clip = vp * glam::Vec3::new(cx - eye.x, sea - eye.y, cz - eye.z).extend(1.0);
            if clip.w <= 1e-4 {
                behind += 1;
                continue;
            }
            let ndc = clip.truncate().truncate() / clip.w;
            lo = lo.min(ndc);
            hi = hi.max(ndc);
        }
        if behind == 4 {
            continue; // entirely behind the eye
        }
        if behind > 0 {
            // Straddles the near plane: the projected quad is unbounded and any finite AABB
            // would understate it. Treat the whole screen as covered — a node this close is
            // water at the camera's feet, which is exactly when the reflection must be at full
            // quality, so guessing large here is both safe and correct.
            return 1.0;
        }
        let w = (hi.x.min(1.0) - lo.x.max(-1.0)).max(0.0);
        let h = (hi.y.min(1.0) - lo.y.max(-1.0)).max(0.0);
        area += w * h;
        if area >= 4.0 {
            return 1.0; // NDC screen is 2x2; already saturated
        }
    }
    (area * 0.25).clamp(0.0, 1.0)
}

impl Ablate {
    fn from_env() -> Self {
        let mut a = Ablate::default();
        let Ok(raw) = std::env::var("WGR_ABLATE") else {
            return a;
        };
        let mut unknown: Vec<String> = Vec::new();
        for token in raw.split(',').map(str::trim).filter(|t| !t.is_empty()) {
            match token.to_ascii_lowercase().as_str() {
                "grass" => a.grass = true,
                "ao" | "gtao" => a.ao = true,
                "skyvis" | "interior_sky" => a.skyvis = true,
                "planar" | "reflection" => a.planar = true,
                "clouds" | "cloud" => a.clouds = true,
                other => unknown.push(other.to_string()),
            }
        }
        // Loud about a typo: a silently ignored ablation name is the same failure mode as the
        // clobbered gates above — the run looks valid and measures the wrong thing.
        if !unknown.is_empty() {
            eprintln!(
                "[wgr] WGR_ABLATE: UNKNOWN token(s) {} — nothing was ablated for them. \
                 Known: grass, ao, skyvis, planar, clouds",
                unknown.join(", ")
            );
        }
        eprintln!(
            "[wgr] WGR_ABLATE: grass={} ao={} skyvis={} planar={} clouds={}",
            a.grass, a.ao, a.skyvis, a.planar, a.clouds
        );
        a
    }
}

/// PERF-005 — per-frame accounting for the CPU-replayed object path (Plan3dOp::Draw3D).
/// `indirect` draws are ones the plan upgraded to a GPU indirect arg but that still cost a CPU
/// draw call in the replay; `direct` are the barrier draws (transparent, decal, skinned/baked,
/// non-standard depth) that can never be GPU-driven. Triangles are the SUBMITTED count
/// (index_count/3 x instances), not a post-cull count — nothing culls these.
#[derive(Clone, Copy, Default)]
pub struct DirectDrawStats {
    pub direct_calls: u32,
    pub indirect_calls: u32,
    pub instances: u32,
    pub tris: u32,
}

// REN-RES-003 — the dynamic residency budget, derived from the device instead of guessed.
//
// The measured problem (roadmap Phase 7, 2026-08-31, Reforger Everon traverse): the budget
// was a FIXED 2,048 MB and nothing ever compared it against the card. Tracked residency came
// in at 2,336 MB, so `over_budget` was permanently true and the texture LRU chased a target
// with no relation to the hardware — on an 8-12 GB card that was still mostly empty.
//
// Fraction of the device-local heap taken as the default budget. 0.70, and the 30% is not
// arbitrary padding — it is the memory this process does NOT account for and cannot see in
// `tracked_bytes`:
//   * the swapchain, depth, HDR and shadow targets, the GTAO/bloom/DoF chains and every other
//     render target the renderer itself allocates (none are counted in the residency total,
//     which covers only object textures and pool geometry);
//   * the desktop compositor's own surfaces and any other application's VRAM — a browser or a
//     second monitor's worth of composited windows is easily hundreds of MB;
//   * driver-side allocations (command buffers, descriptor heaps, shader/pipeline binaries,
//     paging reserve) that never appear in any wgpu accounting;
//   * the pool's own doubling transient — a growth holds BOTH buffers plus the copy at once,
//     which the module header measures at 3x the current size at the moment of the copy.
// A higher fraction risks the failure this codebase has already hit once (create_buffer
// returning an INVALID buffer on a DayZ world, i.e. wgpu's silent out-of-memory); a much lower
// one throws away the headroom the probe was added to find. Re-measure before moving it.
const RESIDENCY_BUDGET_FRACTION: f64 = 0.70;

// What the budget default falls back to when no probe is possible. This is the historical
// fixed value, kept deliberately: an unprobed backend must behave exactly as it did before.
const RESIDENCY_BUDGET_FALLBACK_MB: u64 = 2048;

/// Read the largest DEVICE_LOCAL heap the adapter reports, in bytes.
///
/// `Ok(bytes)` on success; `Err(reason)` names why not, and the reason is logged verbatim so a
/// fallback is never silent. The renderer already takes the wgpu HAL route for DLSS
/// (`dlss_device.rs`), so this is the same escape hatch, used read-only.
fn probe_device_local_bytes(adapter: &wgpu::Adapter) -> Result<u64, &'static str> {
    match adapter.get_info().backend {
        wgpu::Backend::Vulkan => {
            // SAFETY: read-only. We take no ownership of the physical device or instance, do
            // not call anything that mutates them, and the returned borrow ends here.
            let heap = unsafe {
                adapter
                    .as_hal::<wgpu::hal::api::Vulkan>()
                    .and_then(|hal_adapter| {
                        let props = hal_adapter
                            .shared_instance()
                            .raw_instance()
                            .get_physical_device_memory_properties(
                                hal_adapter.raw_physical_device(),
                            );
                        let count =
                            (props.memory_heap_count as usize).min(props.memory_heaps.len());
                        props.memory_heaps[..count]
                            .iter()
                            // VK_MEMORY_HEAP_DEVICE_LOCAL_BIT. Compared as a raw bit so this
                            // file does not have to name an `ash` type — `ash` is an OPTIONAL
                            // dependency here (dlss only), and this probe must work in the
                            // default build that does not pull it in.
                            .filter(|heap| heap.flags.as_raw() & 0x1 != 0)
                            .map(|heap| heap.size)
                            .max()
                    })
            };
            match heap {
                Some(bytes) if bytes > 0 => Ok(bytes),
                Some(_) => Err("Vulkan reported a zero-sized DEVICE_LOCAL heap"),
                None => Err("Vulkan adapter exposed no DEVICE_LOCAL heap"),
            }
        }
        #[cfg(windows)]
        wgpu::Backend::Dx12 => {
            // DXGI's Budget is what the OS says this process may use RIGHT NOW, so it already
            // subtracts other applications — unlike the Vulkan heap size, which is the whole
            // card. Applying the same fraction to it is therefore doubly conservative, which
            // is the safe direction and keeps one policy constant instead of two.
            // SAFETY: read-only; QueryVideoMemoryInfo does not mutate the adapter.
            let budget = unsafe {
                adapter.as_hal::<wgpu::hal::api::Dx12>().and_then(|a| {
                    // DXGI_MEMORY_SEGMENT_GROUP_LOCAL is 0, which is the derived Default.
                    a.raw_adapter()
                        .query_video_memory_info(Default::default())
                        .ok()
                        .map(|info| info.Budget)
                })
            };
            match budget {
                Some(bytes) if bytes > 0 => Ok(bytes),
                _ => Err("DXGI QueryVideoMemoryInfo returned no local budget"),
            }
        }
        // GL/Metal/Noop and (on non-Windows) DX12 have no portable heap query here.
        _ => Err("backend exposes no device-memory query"),
    }
}

/// Turn the env override and the probe result into a budget in bytes plus the line that must
/// be logged for it. Pure, so the four cases (override, override 0 = unlimited, probe present,
/// probe absent) are unit-testable without a device.
fn residency_budget_from(env_mb: Option<u64>, probe: Result<u64, &'static str>) -> (u64, String) {
    if let Some(mb) = env_mb {
        let bytes = mb.saturating_mul(1024 * 1024);
        let note = if mb == 0 { " = UNLIMITED" } else { "" };
        return (
            bytes,
            format!(
                "wgpu dynamic residency budget: {mb} MB{note} (WGR_DYNAMIC_VRAM_MB override; \
                 device probe: {})",
                match probe {
                    Ok(heap) => format!("{} MB device-local", heap / (1024 * 1024)),
                    Err(reason) => format!("unavailable — {reason}"),
                }
            ),
        );
    }
    match probe {
        Ok(heap) => {
            let bytes = (heap as f64 * RESIDENCY_BUDGET_FRACTION) as u64;
            (
                bytes,
                format!(
                    "wgpu dynamic residency budget: {} MB = {:.0}% of {} MB probed device-local \
                     heap (0 = unlimited via WGR_DYNAMIC_VRAM_MB)",
                    bytes / (1024 * 1024),
                    RESIDENCY_BUDGET_FRACTION * 100.0,
                    heap / (1024 * 1024),
                ),
            )
        }
        Err(reason) => (
            RESIDENCY_BUDGET_FALLBACK_MB.saturating_mul(1024 * 1024),
            format!(
                "wgpu dynamic residency budget: {RESIDENCY_BUDGET_FALLBACK_MB} MB FIXED FALLBACK \
                 — no device probe ({reason}); set WGR_DYNAMIC_VRAM_MB to override (0 = unlimited)"
            ),
        ),
    }
}

pub struct Renderer {
    // How much wider the planar reflection's frustum is than the screen's. 1 = the old behaviour,
    // where a grazing reflection ran off the edge of the reflection target and the reflected
    // clouds ended in a visible line.
    planar_reflection_pad: f32,
    // TW-WATER W4b: this frame's water camera slot (for Tidewater's reactive-mask pass, which runs
    // with the tonemap, after the frame's camera list is gone)
    water_camera_slot: Option<usize>,
    // DZ-005 — the world's river flow map, resolved to a bindless slot, and its
    // `StreamSpeedInfl`. One pair for the whole world, not one per material: Enoch's 804
    // river segments all name the same `enoch_river.emat`, and Chernarus's 4,044 water
    // placements name no flow map at all (its older `dz\water\` addon ships zero `.emat`),
    // which is what slot 0 means here. Set once per world by `wgr_water_set_flow_map`.
    water_flow_slot: u32,
    water_flow_strength: f32,
    log: LogSink,
    runtime_diagnostics: Arc<RuntimeDiagnostics>,
    // `'static` is sound because C++ keeps the window alive until after `wgr_destroy`.
    surface: wgpu::Surface<'static>,
    device: wgpu::Device,
    queue: wgpu::Queue,
    // REN-THR-009 — handed to C++ by `wgr_uploader_get` and used from there WITHOUT forming
    // `&mut *renderer`. Arc so the pointer stays valid for the renderer's whole life; the
    // C++ side borrows it and never releases it.
    uploader: std::sync::Arc<uploader::Uploader>,
    config: wgpu::SurfaceConfiguration,
    present_modes: Vec<wgpu::PresentMode>,
    textures: SharedTextures,
    gfx2d: Gfx2d,
    gfx3d: Gfx3d,
    terrain: Terrain,
    grass: Grass,
    // FAR INSTANCE TIER — one proxy per authored placement, beyond where real objects live.
    far: Far,
    // TW-WATER W1 — the active water backend. Owns the surface AND the underwater compositor
    // (Current OP: today's `Water` + `Underwater`, unchanged); see water_backend.rs.
    water: WaterBackend,
    rain_water: rain_water::RainWater,
    // Actual depth-tested water coverage from the previous completed frame. This corrects the
    // world-sized sea quadtree's conservative CPU bounds before paying for a planar reflection.
    water_visibility: WaterVisibility,
    // WTR-002 — GPU timestamp brackets around the water-pipeline passes (inert when the
    // adapter lacks TIMESTAMP_QUERY + TIMESTAMP_QUERY_INSIDE_ENCODERS).
    gpu_timers: GpuTimers,
    // PERF-005 — the CPU-replayed object path (Plan3dOp::Draw3D), counted per frame in the
    // colour sub-pass. The GPU-driven retained set is accounted for on the GPU (cull.wgsl
    // stats); these draws never reach that shader, so without this counter they are invisible
    // in the object totals and their share of the colour container reads as unexplained.
    // Cell because render_ops replays over &Renderer.
    direct_draws: std::cell::Cell<DirectDrawStats>,
    runtime_capabilities: u32,
    // Shared dynamic-resource budget. This first slice covers the two unbounded
    // owner-managed pools implicated by the DayZ exhaustion: authored object
    // textures and merged model geometry. Zero means observability-only.
    residency_budget_bytes: u64,
    // HDR pipeline (docs/hdr-pipeline-plan.md). When enabled, the 3D/terrain/2D
    // scene renders into `hdr` (linear once Stage 2 lands) and `tonemap` resolves it
    // to the swapchain; the dev overlay + (later) screen-space UI composite after.
    // All None/false = the LDR-direct-to-swapchain path, the A/B reference.
    hdr_enabled: bool,
    hdr: Option<(wgpu::Texture, wgpu::TextureView)>,
    // Single-sample resolve target for the MSAA scene colour (Some only when sample_count > 1).
    // The scene renders into the multisampled `hdr`; a resolve writes this, and the tonemap /
    // bloom / exposure sample it. At 1x this is None and those read `hdr` directly.
    hdr_resolve: Option<(wgpu::Texture, wgpu::TextureView)>,
    // Single-sample opaque-scene snapshot consumed by water before it writes HDR.
    water_scene: Option<(wgpu::Texture, wgpu::TextureView)>,
    planar: Option<PlanarTarget>,
    // (planar_active, coverage in half-percent buckets) at the last log line — see the gate in
    // render_frame. Bucketed so a drifting camera does not print every frame.
    planar_dbg_last: Option<(bool, u32)>,
    // Live resolution divisor for the reflection, and the sticky-selection state behind it.
    // Changing the divisor REALLOCATES the target and rebuilds water's bind group, so the choice
    // is held for `PLANAR_DIV_HOLD` consecutive frames of agreement before it is committed;
    // without that, a camera hovering at a bucket boundary would thrash a multi-megabyte texture
    // every frame. (That exact failure — a per-frame realloc hiding behind a size comparison —
    // is what the cloud low-res target was doing before PERF-001.)
    planar_div: u32,
    planar_div_want: u32,
    planar_div_hold: u32,
    planar_mips: PlanarMips,
    hdr_size: (u32, u32),
    // REN-TEMP-001B: fraction of the swapchain size the 3D scene renders at (WGR_RENDER_SCALE,
    // 0.5..=1.0; fixed at startup). Scene targets size to render_size(); swapchain, post chain
    // and UI stay at config.width/height. At 1.0 no upscale target exists and the chain is
    // bit-identical to the pre-split renderer.
    render_scale: f32,
    // Output-resolution single-sample HDR target the render-res scene is upscaled into before
    // bloom/exposure/tonemap. None at 100% render scale.
    hdr_upscaled: Option<(wgpu::Texture, wgpu::TextureView)>,
    // REN-TEMP-001L (plan Phase 13): the ordered upscaler chain - first Done wins.
    // [DLSS (dlss builds, when the NGX route came up), Bilinear]. All vendor state
    // (NGX objects, raw device handles, failure latches) lives inside the backends.
    upscalers: Vec<Box<dyn upscaler::TemporalUpscaler>>,
    // True when NGX came up at startup AND reported DLSS Super Sampling available
    // (feeds temporal_info's dlss_route).
    ngx_route: bool,
    // WHY DLSS is inactive, as one sentence for the dev panel and the log
    // (dlss_status.rs). Latched at the first failing startup gate; per-frame otherwise.
    dlss_status: dlss_status::DlssStatus,
    // REN-TEMP-001E/F/G: temporal camera state, jitter, static-world velocity + debug view.
    // WGR_TEMPORAL=1 to enable (off by default — jitter with no accumulator is shimmer).
    temporal: temporal::Temporal,
    // True when temporal / render scale were auto-enabled off the healthy DLSS route;
    // rolled back if the DLSS backend dies so the fallback is truly native.
    temporal_auto: bool,
    render_scale_auto: bool,
    // REN-TEMP-001 §6.7: live tuning (dev panel). Seeded from env; the panel overwrites.
    dlss_runtime_on: bool,
    dlss_auto_exposure: bool,
    dlss_jy_flip: bool,
    dlss_mv_render_space: bool,
    dlss_mv_flip: bool,
    dlss_active_frame: bool,
    dlss_last_quality: i32,
    // FSR 1 backend switches (live; env-seeded). Sharpness in RCAS stops.
    fsr_runtime_on: bool,
    fsr_sharpness: f32,
    // Optional RCAS pass over the DLSS output (NVIDIA removed DLSS's own sharpening;
    // this is the standard external answer). Uses fsr_sharpness for strength.
    dlss_sharpen: bool,
    // Real sway motion vectors for vegetation (REN-TEMP-001T); WGR_VEG_VELOCITY=0
    // reverts to camera-only reprojection under the canopy.
    veg_velocity_on: bool,
    // Dynamic camera-UBO offset of the MAIN scene camera this frame, for passes
    // recorded outside render_frame's camera loop (the gpu velocity twin).
    main_cam_off: u32,
    rcas_sharpener: upscaler::RcasSharpener,
    // Which backend produced the last upscaled frame (ffi::WgrTemporalInfo values).
    last_upscaler: u32,
    // Exact startup opt-in only; OFF has no tuple allocation/copy.
    main_camera_tuple: Option<Box<main_camera_tuple::Tracker>>,
    // Default OFF: bounded CPU tuple copies only for an explicitly enabled fixture.
    demand_view_snapshot: Option<Box<demand_view_snapshot::Tracker>>,
    // MSAA sample count of the scene targets (1 = off). Fixed at startup (WGR_MSAA); pipelines
    // and offscreen targets are built against it.
    sample_count: u32,
    tonemap: Option<Tonemap>,
    // Scene-wide effect scratch. HDR writes here before the HDR post chain; LDR uses
    // it as the scene target only on submerged frames, then composites to swapchain.
    underwater_target: Option<(wgpu::Texture, wgpu::TextureView)>,
    // Depth of field: a Picture Mode aid, off unless asked for. The pass, its target and the
    // settings are all created lazily, so a session that never opens Picture Mode pays nothing
    // beyond one Option check per frame.
    dof: Option<DepthOfField>,
    dof_target: Option<(wgpu::Texture, wgpu::TextureView)>,
    dof_size: (u32, u32),
    dof_settings: DofSettings,
    dof_depth_warned: bool,
    underwater_size: (u32, u32),
    // Last logged underwater-compositor engage state, so the transition is reported once
    // rather than every frame. The pass has had two independent triggers and a toggle that
    // did not reach one of them; "is it running right now" needs to be answerable from a log.
    underwater_engaged_logged: Option<bool>,
    // TW-WATER W5d: the eye is clearly under the water surface this frame (see the cloud composite).
    submerged_view: bool,
    post_source_underwater: bool,
    // Which view the bloom/exposure/tonemap chain is currently BOUND to. The rebind below is
    // conditional, so anything that swaps the post source has to be part of this state or its
    // output is written and then silently ignored -- which is exactly what happened when depth of
    // field was added and only the underwater flag was consulted.
    post_source_dof: bool,
    // Per-frame inputs for the underwater compositor's per-pixel waterline: camera height above the
    // local water surface (negative = submerged) and the unprojection matrix for view-ray
    // reconstruction. Held on the renderer because the compositor is invoked from three separate
    // places in the frame plan, and threading two more arguments through each of them (and through
    // run_tonemap) would be noise.
    underwater_view: UnderwaterView,
    // Volumetric sun shafts, composited into the linear HDR scene just before bloom (HDR path
    // only — there is nothing sensible to add light to after a tonemap). `godrays_frame` carries
    // the camera the SKY pass chose, captured at the sky upload and consumed in run_tonemap.
    godrays: Option<GodRays>,
    godrays_frame: Option<GodRayFrame>,
    // Sinkhole W1b: camera underground factor (0 open air .. 1 deep in a cave), wgr_set_camera_underground.
    cam_underground: f32,
    // SMK-037 SOFT PARTICLES. The scene-depth snapshot the cloudlet billboards fade against,
    // plus the two live knobs the Smoke tab pushes. `soft_enabled` gates BOTH the snapshot pass
    // and the C++ side's choice of depth mode, so with it off nothing here is recorded, no
    // pipeline changes, and the 2D path is byte-for-byte what it was.
    // SMK-038: the volumetric smoke field. Inert (and allocation-free beyond its pipelines)
    // until the Smoke tab selects it.
    pub(crate) smoke_volume: smoke_volume::SmokeVolume,
    smoke_sun_dir: [f32; 3],
    smoke_sun_radiance: [f32; 3],
    smoke_ambient: [f32; 3],
    soft_particles: soft_particles::SoftParticles,
    soft_enabled: bool,
    soft_fade: f32,
    soft_globals: SoftGlobals,
    /// The shafts have already been composited for this frame, inside the world segment and
    /// BEFORE the transparent draws. See the call site for why they moved there.
    godrays_composited: bool,
    // Set once the frame's single-sample depth resolve has run at a point where the 3D depth is
    // already final, so the god-ray march can reuse it instead of recording a second resolve.
    // Deliberately NOT set by the pre-water resolve: depth is still being written after that one.
    frame_depth_final_resolved: bool,
    // PERF: whether the FAR-reduce depth resolve currently in `water_depth_view` matches the depth
    // buffer as it now stands. A full-res resolve is not cheap (it reads every MSAA sample), and
    // the frame asked for it up to four times: water, clouds, the god rays, the underwater
    // compositor. This is a narrower claim than `frame_depth_final_resolved` above — that one says
    // "no more 3D depth will be written this frame", this one says "the resolve is not stale yet" —
    // and it is cleared by every pass that writes depth, which is what makes skipping provably
    // safe rather than a guess. See `ensure_far_depth_resolve`.
    far_depth_resolve_current: bool,
    // Bloom pyramid, built alongside the tonemap on the HDR path; the resolve adds it.
    bloom: Option<Bloom>,
    // Eye adaptation / auto-exposure; produces a 1x1 exposure scale the resolve applies.
    exposure: Option<Exposure>,
    exposure_params: ffi::WgrExposure,
    // Live tonemap/look params, pushed from the ImGui Tonemap tab (wgr_set_tonemap).
    // Seeded from WGR_* env for continuity; the tab is the source of truth once open.
    tonemap_params: ffi::WgrTonemap,
    // Procedural sky (docs/procedural-sky-plan.md): a fullscreen atmospheric pass
    // drawn into the scene target before geometry. Params pushed via wgr_set_sky
    // (celestial per frame, authored on edit); skipped when control.x (enabled) = 0.
    sky: Sky,
    layer_fog_settings: ffi::WgrLayeredFog,
    layer_fog_weather: f32,
    sky_params: ffi::WgrSky,
    /// REN-SKY-004: does the planar water reflection record its own low-step cloud pipeline?
    /// Pushed from the host (sky look `night_params.w`) so the dev panel can turn it off.
    cloud_reflection_cheap: bool,
    // Last terrain sun-shadow / sky-visibility blocks received via wgr_set_render_params.
    // The consolidated block is pushed every frame, but these two setters realloc the mask /
    // re-run the CPU scan (and set_sun_shadow_params dirties the sweep unconditionally), so we
    // only fan out to them when their values actually change. See render-params-consolidation-plan.md.
    last_sun_shadow: Option<ffi::WgrTerrainSunShadow>,
    last_sky_visibility: Option<ffi::WgrSkyVisibility>,
    // Foliage lighting knobs (docs/foliage-translucency-plan.md), pushed every frame into the
    // per-camera Frame UBO by gfx3d.prepare — cheap scalars, no diffing needed.
    foliage_params: ffi::WgrFoliage,
    // WGR_SKY_DEBUG: log the sky's camera count + chosen index when they change, to
    // catch frame-to-frame camera alternation (the suspected sun/haze stutter cause).
    sky_debug: bool,
    sky_dbg_last: (usize, usize),
    // One-shot GTAO input dump (WGR_GTAO_DEBUG). AO that comes back uniformly white means the
    // pass ran and found no horizons, and the arithmetic that decides that is all in these few
    // numbers — cheaper to print them once than to reason about the shader.
    gtao_dbg_logged: bool,
    // One-shot interior sky-visibility coverage report. A map that renders NOTHING clears to the
    // far plane, every comparison passes, reach is 1 and the feature is a silent no-op that is
    // indistinguishable from success in a log. This measures it instead. Counted in frames rather
    // than fired on the first one for the reason recorded on the GTAO dump below: an early frame
    // has no scene yet, and burning a one-shot flag on it means the diagnostic never fires.
    interior_sky_dbg_frames: u32,
    interior_sky_dbg_logged: bool,
    // The last WgrSkyVis::probe counter this renderer acted on. The one-shot above fires ~2 s
    // after the map goes active, which on a streamed world is the LOADING SCREEN: the retained
    // set is still filling and the camera is not where the player will stand. That made the only
    // measurement anyone had a measurement of the wrong moment, and nothing could ever ask again.
    // The dev panel's "Measure map coverage now" bumps the counter; a change re-arms the report.
    interior_sky_probe_seen: u32,
    // Depth+normal prepass (docs/depth-prepass-plan.md). Ships unconditionally on wgpu
    // (decision 8); WGR_PREPASS=0 is a TEMPORARY dev A/B for bring-up validation only,
    // not a shipped runtime flag. When on, the first (world) depth segment gets a
    // depth+normal prepass and its opaque colour draws early-Z with depth-write off.
    prepass_enabled: bool,
    // Per-frame gate for the retained GPU-driven world set (objects + their prepass).
    // The set is GPU-resident and would otherwise draw every frame regardless of the
    // per-frame 3D lists; C++ raises this (wgr_set_suppress_world_objects) while the
    // world must not be shown (mission editor, loading, shutdown) so the sides letterbox
    // to black instead of leaking clutter. Set explicitly by C++ each frame.
    suppress_world_objects: bool,
    // Debug: draw the GPU-driven frustum-cull spheres (ImGui Culling tab). Off by default.
    cull_debug_draw: bool,
    // A screenshot is requested by C++ before the next frame. The swapchain
    // texture can only be copied while it is acquired by render_frame, so the
    // synchronous readback completes there and C++ collects the RGBA bytes
    // immediately after presentation.
    screenshot_requested: bool,
    post_optics_probe: Option<post_optics_probe::State>,
    screenshot_pixels: Option<ScreenshotPixels>,
}

// Teardown order: fields drop in declaration order, and `device` is declared long
// before `upscalers`, so a backend that must release a vendor context against a live
// VkDevice (DLSS: ReleaseFeature + NGX Shutdown1) needs the chain cleared HERE, after
// the GPU has finished with the last frame and before the device goes.
impl Drop for Renderer {
    fn drop(&mut self) {
        if !self.upscalers.is_empty() {
            let _ = self.device.poll(wgpu::PollType::wait_indefinitely());
            self.upscalers.clear();
        }
    }
}

impl Renderer {
    /// Sinkhole W1b: see wgr_set_camera_underground.
    pub fn set_camera_underground(&mut self, underground: f32) {
        let u = if underground.is_finite() { underground.clamp(0.0, 1.0) } else { 0.0 };
        self.cam_underground = u;
        if let Some(gr) = self.godrays.as_mut() {
            gr.underground = u;
        }
    }

    fn new(desc: &ffi::WgrSurfaceDesc, log: LogSink) -> Result<Self, String> {
        let (raw_display_handle, raw_window_handle) = handles::build_handles(desc)?;

        // wgpu only enables VK_EXT_debug_utils — and thus emits our
        // push_debug_group markers + buffer/texture labels into a RenderDoc capture
        // — when the DEBUG instance flag is set. InstanceFlags::from_build_config()
        // (which the _from_env constructor seeds from) clears DEBUG in optimized
        // rwdi/release builds, so a profiling build shows an unlabelled, ungrouped
        // command stream. Force DEBUG on so captures stay legible; opt into the
        // validation layers separately via WGR_GPU_VALIDATION (they are expensive).
        let mut instance_desc = wgpu::InstanceDescriptor::new_without_display_handle_from_env();
        instance_desc.flags |= wgpu::InstanceFlags::DEBUG;
        if std::env::var("WGR_GPU_VALIDATION").is_ok() {
            instance_desc.flags |= wgpu::InstanceFlags::VALIDATION;
        }
        log.log(
            log_level::INFO,
            &format!("wgpu instance flags: {:?}", instance_desc.flags),
        );
        // REN-TEMP-001M: WGR_DLSS=1 (dlss builds only) routes instance/device creation
        // through ash + wgpu-hal so the NGX-required Vulkan extensions are enabled.
        // STRICT fallback: any failure logs and continues on the untouched native path.
        // DEFAULT ON in a dlss build (owner request 2026-08-30: "like other games, no
        // .bat"): the route falls back to the untouched native path on ANY failure, so
        // a default-on request is safe. WGR_DLSS=0 opts out.
        // The reason DLSS is inactive, decided gate by gate from here on (dlss_status.rs):
        // the first gate that fails latches its sentence for the panel and the log.
        let mut dlss_status = dlss_status::DlssStatus::default();
        #[cfg(not(feature = "dlss"))]
        dlss_status.latch(dlss_status::REASON_BUILD_WITHOUT_DLSS);
        // Where nvngx_dlss.dll was found, for the "could not load" sentence.
        #[cfg(feature = "dlss")]
        let dlss_snippet: Option<std::path::PathBuf>;
        #[cfg(feature = "dlss")]
        let dlss_requested = {
            let env_on = std::env::var("WGR_DLSS").map(|v| v != "0").unwrap_or(true);
            if !env_on {
                dlss_status.latch(dlss_status::REASON_OPTED_OUT);
                dlss_snippet = None;
                false
            } else {
                // The snippet gate comes BEFORE the hal route: without nvngx_dlss.dll
                // beside the exe NGX can only ever answer FeatureNotFound, and a device
                // built with the NGX extensions for nothing is a second device path for
                // no gain. The 2026-08-30 tester package held the exe and the renderer
                // DLL and no snippet, and every one of those machines reported "route
                // up, DLSS inactive".
                let dirs = dlss_status::snippet_search_dirs(
                    std::env::current_exe()
                        .ok()
                        .and_then(|p| p.parent().map(|d| d.to_path_buf())),
                    std::env::var("WGR_NGX_SNIPPET_DIR").ok(),
                );
                match dlss_status::find_snippet(&dirs) {
                    Some(p) => {
                        dlss_snippet = Some(p);
                        true
                    }
                    None => {
                        dlss_status.latch(dlss_status::snippet_missing_reason(&dirs));
                        dlss_snippet = None;
                        false
                    }
                }
            }
        };
        #[cfg(feature = "dlss")]
        let mut dlss_instance_active = false;
        let instance = 'inst: {
            #[cfg(feature = "dlss")]
            if dlss_requested {
                match dlss_device::create_instance(instance_desc.flags) {
                    Ok(i) => {
                        log.log(
                            log_level::INFO,
                            "wgpu instance created via hal route with NGX extensions (WGR_DLSS)",
                        );
                        dlss_instance_active = true;
                        break 'inst i;
                    }
                    Err(e) => {
                        log.log(
                            log_level::WARN,
                            &format!("WGR_DLSS instance path failed ({e}); native path instead"),
                        );
                        dlss_status
                            .latch(format!("Vulkan instance with the NGX extensions failed: {e}"));
                    }
                }
            }
            wgpu::Instance::new(instance_desc)
        };

        let surface: wgpu::Surface<'static> = unsafe {
            instance.create_surface_unsafe(wgpu::SurfaceTargetUnsafe::RawHandle {
                raw_display_handle: Some(raw_display_handle),
                raw_window_handle,
            })
        }
        .map_err(|e| format!("create_surface_unsafe failed: {e}"))?;

        let adapter = pollster::block_on(instance.request_adapter(&wgpu::RequestAdapterOptions {
            power_preference: wgpu::PowerPreference::HighPerformance,
            compatible_surface: Some(&surface),
            force_fallback_adapter: false,
        }))
        .map_err(|e| format!("request_adapter failed: {e}"))?;

        let info = adapter.get_info();
        log.log(
            log_level::INFO,
            &format!(
                "wgpu adapter: {} ({:?}, {:?})",
                info.name, info.backend, info.device_type
            ),
        );
        // The adapter is chosen with PowerPreference::HighPerformance, so a laptop's
        // discrete GPU wins when the driver exposes it. When it does not (iGPU only,
        // or the DX12 fallback), say so BEFORE building a device for NGX.
        #[cfg(feature = "dlss")]
        if dlss_instance_active {
            if let Some(reason) = dlss_status::adapter_reason(
                &info.name,
                info.vendor,
                &format!("{:?}", info.backend),
            ) {
                log.log(
                    log_level::WARN,
                    &format!("DLSS unavailable: {reason}; native device path"),
                );
                dlss_status.latch(reason);
                dlss_instance_active = false;
            }
        }

        let bc_features = adapter.features() & wgpu::Features::TEXTURE_COMPRESSION_BC;
        let bc_supported = !bc_features.is_empty();
        if !bc_supported {
            log.log(
                log_level::WARN,
                "wgpu adapter lacks TEXTURE_COMPRESSION_BC; DXT textures will fail to upload",
            );
        }

        // Terrain samples its ground textures through a bindless binding_array
        // (per-layer native sizes and formats), so descriptor indexing is a hard
        // requirement. Every DX12 resource-binding-tier-2+ / Vulkan 1.2 desktop
        // GPU has it; adapters without it fall back to GL33 at the factory level.
        let bindless = wgpu::Features::TEXTURE_BINDING_ARRAY
            | wgpu::Features::SAMPLED_TEXTURE_AND_STORAGE_BUFFER_ARRAY_NON_UNIFORM_INDEXING;
        if !adapter.features().contains(bindless) {
            return Err(format!(
                "adapter lacks binding-array features required for terrain (has {:?})",
                adapter.features() & bindless
            ));
        }
        // Optional: lets the terrain bind group carry fewer views than the
        // declared array size; without it unused slots are padded with a dummy.
        let partially_bound = adapter.features() & wgpu::Features::PARTIALLY_BOUND_BINDING_ARRAY;
        let partially_bound_enabled = !partially_bound.is_empty();

        // GPU-driven indirect draw (docs/gpu-culling-and-depth-plan.md Stage 2). Our
        // instancing model puts each bucket's base_instance in the indirect args'
        // first_instance, so INDIRECT_FIRST_INSTANCE is the gating feature. Stage 2 issues
        // single draw_indexed_indirect (core wgpu, portable incl. Metal), so nothing more
        // is needed here; the Stage-3 GPU-produced multi-draw will request its own feature.
        // Adapter-gated exactly like `partially_bound`; when absent the direct path stays.
        let indirect_avail = adapter.features() & wgpu::Features::INDIRECT_FIRST_INSTANCE;
        let indirect_first_instance = !indirect_avail.is_empty();

        // MULTI_DRAW_INDIRECT_COUNT (docs/gpu-culling-and-depth-plan.md Stage 3b-4): a GPU
        // count buffer that trims the empty tail of the compute-produced indirect args, so the
        // GPU-driven draw dispatches only the surviving sub-draws instead of the full
        // conservative per-variant capacity. Present on desktop Vulkan/DX12; Metal lacks it and
        // falls back to the no-op-tail multi_draw. Adapter-gated like the features above.
        let mdic_avail = adapter.features() & wgpu::Features::MULTI_DRAW_INDIRECT_COUNT;
        let multi_draw_count = !mdic_avail.is_empty();

        // Grass placement writes an instance buffer in compute and consumes it directly
        // from the vertex stage.  Desktop Vulkan/DX12 adapters expose this optional
        // WebGPU feature; request it explicitly so the grass bind layout is legal.
        let vertex_writable_storage = adapter.features() & wgpu::Features::VERTEX_WRITABLE_STORAGE;
        if vertex_writable_storage.is_empty() {
            return Err("adapter lacks VERTEX_WRITABLE_STORAGE required for GPU grass".to_string());
        }

        // WTR-002 — GPU timestamp instrumentation. Encoder-level brackets need BOTH
        // TIMESTAMP_QUERY and TIMESTAMP_QUERY_INSIDE_ENCODERS; adapter-gated exactly like
        // `partially_bound` (absent => the timers are inert and the FFI reports 0 regions).
        let ts_features =
            wgpu::Features::TIMESTAMP_QUERY | wgpu::Features::TIMESTAMP_QUERY_INSIDE_ENCODERS;
        let ts_enabled = adapter.features().contains(ts_features);
        // GRS-A: grass colour/prepass/shadow are ops inside shared render passes,
        // so isolating them needs in-pass timestamps. Requested separately — its
        // absence only costs the grass draw rows, not the encoder-level regions.
        let ts_inside_passes = ts_enabled
            && adapter
                .features()
                .contains(wgpu::Features::TIMESTAMP_QUERY_INSIDE_PASSES);
        let ts_request = if ts_enabled {
            if ts_inside_passes {
                ts_features | wgpu::Features::TIMESTAMP_QUERY_INSIDE_PASSES
            } else {
                ts_features
            }
        } else {
            wgpu::Features::empty()
        };

        log.log(
            log_level::INFO,
            &format!(
                "wgpu capabilities: bc={} bindless=true partially_bound={} indirect_first_instance={} multi_draw_count={} vertex_writable_storage=true timestamps={} timestamps_in_passes={}",
                bc_supported,
                partially_bound_enabled,
                indirect_first_instance,
                multi_draw_count,
                ts_enabled,
                ts_inside_passes,
            ),
        );

        // Bindless object textures (docs/bindless-textures-plan.md): one binding_array
        // covering all live object textures. Cap chosen so a non-PARTIALLY_BOUND adapter
        // doesn't pad an enormous array (a level with more unique textures overflows to
        // the white slot — 8192 comfortably covers OFP content). Must be >= terrain's 512.
        let object_texture_cap = 8192u32.max(terrain::TERRAIN_MAX_GROUND_LAYERS);
        // REN-RES-003: the residency budget used to be a flat 2,048 MB that was never
        // compared against the device. Probe the real device-local heap and derive the
        // default from it; the env var still overrides exactly as before.
        let probe = probe_device_local_bytes(&adapter);
        let (residency_budget_bytes, budget_log) = residency_budget_from(
            std::env::var("WGR_DYNAMIC_VRAM_MB")
                .ok()
                .and_then(|value| value.parse::<u64>().ok()),
            probe,
        );
        log.log(log_level::INFO, &budget_log);

        // BOTH binding-array limits DEFAULT TO 0 even on devices that fully support
        // binding arrays, so both must be requested explicitly or layout creation panics
        // ("limit is 0"). Deriving from adapter.limits() is unreliable (it can report the
        // 0 default); the wgpu docs guarantee any array-capable device supports >= 500k
        // resources / 1000 samplers, and we gate on array features above, so request the
        // fixed values we use. NB wgpu counts the sampler array's 8 elements against the
        // GENERAL elements limit too (not only the sampler limit), so the object pipeline
        // layout needs `object_texture_cap + 8`; request headroom above that.
        let required_limits = wgpu::Limits {
            max_binding_array_elements_per_shader_stage: object_texture_cap + 64,
            max_binding_array_sampler_elements_per_shader_stage: 8,
            // The lit mesh pipelines take a 5th bind group (group 4) for the terrain
            // heightmap used to conform vegetation on the GPU. Ample on desktop.
            max_bind_groups: 5,
            // REN-GI-001: the camera group carries 8 sampled textures now (the probe volume
            // joined the dome maps, GTAO, froxel, masks); with water's own group that passes
            // the default 16 per stage and the water pipeline layout silently became invalid.
            max_sampled_textures_per_shader_stage: adapter
                .limits()
                .max_sampled_textures_per_shader_stage
                .clamp(16, 64),
            // The geometry pool is one buffer for every resident mesh, and it is
            // bound as STORAGE (the skin-bake reads it) as well as VERTEX. The
            // defaults are 256 MB for a buffer and 128 MB for a storage binding,
            // so the pool's doubling growth hit the storage limit at 4M vertices
            // (4M * 36 B = 144 MB): create_buffer then returns an *invalid*
            // buffer rather than failing loudly, and every later write_buffer and
            // draw against it is rejected with "Buffer with 'wgr_geo_pool_vbuf'
            // label is invalid". OFP's geometry never approaches this; one Arma 3
            // world does.
            //
            // Unlike the binding-array limits above, buffer-size limits are
            // reported reliably by the adapter, so ask for what the device
            // actually supports. max() keeps the default as a floor.
            max_buffer_size: adapter
                .limits()
                .max_buffer_size
                .max(wgpu::Limits::default().max_buffer_size),
            max_storage_buffer_binding_size: adapter
                .limits()
                .max_storage_buffer_binding_size
                .max(wgpu::Limits::default().max_storage_buffer_binding_size),
            ..Default::default()
        };

        // REN-OBJ-004: forced early depth test for the prepassed cutout colour entries.
        // Adapter-gated like the others; absent means the early entries are not even compiled
        // (shaders::early_depth_supported) and WGR_OBJECT_EARLY_Z is inert.
        let early_depth = adapter.features() & wgpu::Features::SHADER_EARLY_DEPTH_TEST;
        shaders::set_early_depth_supported(!early_depth.is_empty());
        let requested_features = bc_features
            | bindless
            | partially_bound
            | indirect_avail
            | mdic_avail
            | vertex_writable_storage
            | ts_request
            | early_depth
            | (adapter.features()
                & (wgpu::Features::DEPTH32FLOAT_STENCIL8
                    | wgpu::Features::TEXTURE_ADAPTER_SPECIFIC_FORMAT_FEATURES));
        // REN-TEMP-001M: the initialised NGX context + raw handles, present only when
        // the hal device route succeeded AND NGX reported DLSS available. Same
        // strict-fallback rule as the instance: any failure names itself and the
        // renderer continues on the plain path.
        #[cfg(feature = "dlss")]
        let mut ngx_raw: Option<(dlss_device::NgxRawHandles, dlss::Ngx)> = None;
        let (device, queue) = 'dev: {
            #[cfg(feature = "dlss")]
            if dlss_instance_active {
                match dlss_device::create_device(&adapter, requested_features, &required_limits) {
                    Ok((d, q, raw)) => {
                        log.log(
                            log_level::INFO,
                            "wgpu device created via hal route with NGX extensions (WGR_DLSS)",
                        );
                        // The extension route proves nothing about DLSS itself — the
                        // NGX extensions are generic Vulkan, so an AMD/Intel box gets
                        // this far too. Ask the driver NOW whether Super Sampling is
                        // actually supported; without this gate such a box boots into
                        // the temporal defaults and shimmers through the bilinear
                        // fallback at 67% (tester reports, 2026-08-30).
                        match dlss::feature_requirements(raw.instance, raw.physical_device)
                        {
                            Ok(0) => {
                                log.log(
                                    log_level::INFO,
                                    "driver reports DLSS Super Sampling supported",
                                );
                                // NGX comes up HERE, not at the first upscaled frame:
                                // the verdict ("snippet did not load", "driver too old")
                                // is then in the startup log and the panel before any
                                // frame is drawn, the temporal/67% defaults are never
                                // auto-enabled for a DLSS that will not run, and a
                                // machine without DLSS never has its first frame split
                                // around a raw command buffer that goes nowhere.
                                match unsafe {
                                    dlss::Ngx::init(raw.instance, raw.physical_device, raw.device)
                                } {
                                    Ok(ngx) => {
                                        if ngx.supersampling_available() {
                                            log.log(
                                                log_level::INFO,
                                                "NGX initialised; DLSS Super Resolution available",
                                            );
                                            ngx_raw = Some((raw, ngx));
                                        } else {
                                            let reason =
                                                ngx.availability_reason(dlss_snippet.as_deref());
                                            log.log(
                                                log_level::WARN,
                                                &format!(
                                                    "DLSS unavailable: {reason} ({}); native path",
                                                    ngx.availability_diagnostics()
                                                ),
                                            );
                                            ngx.release();
                                            dlss_status.latch(reason);
                                        }
                                    }
                                    Err(r) => {
                                        let reason = dlss_status::init_failed_reason(r);
                                        log.log(
                                            log_level::WARN,
                                            &format!("DLSS unavailable: {reason}; native path"),
                                        );
                                        dlss_status.latch(reason);
                                    }
                                }
                            }
                            Ok(bits) => {
                                let reason = dlss_status::requirements_reason(bits, &info.name);
                                log.log(
                                    log_level::WARN,
                                    &format!("DLSS unsupported on this GPU/driver: {reason}; native path"),
                                );
                                dlss_status.latch(reason);
                            }
                            Err(r) => {
                                let reason = format!(
                                    "NGX GetFeatureRequirements failed ({})",
                                    dlss_status::ngx_code(r)
                                );
                                log.log(
                                    log_level::WARN,
                                    &format!("DLSS support probe failed: {reason}; native path"),
                                );
                                dlss_status.latch(reason);
                            }
                        }
                        break 'dev (d, q);
                    }
                    Err(e) => {
                        log.log(
                            log_level::WARN,
                            &format!("WGR_DLSS device path failed ({e}); native path instead"),
                        );
                        dlss_status
                            .latch(format!("Vulkan device with the NGX extensions failed: {e}"));
                    }
                }
            }
            pollster::block_on(adapter.request_device(&wgpu::DeviceDescriptor {
                required_features: requested_features,
                required_limits,
                ..Default::default()
            }))
            .map_err(|e| format!("request_device failed: {e}"))?
        };

        // Keep GPU failures diagnosable at the FFI boundary. Preview-0 does not
        // attempt in-process device recovery, but it must report why a restart
        // or the engine's normal fallback policy is required.
        let runtime_diagnostics = Arc::new(RuntimeDiagnostics::default());
        let lost_diagnostics = Arc::clone(&runtime_diagnostics);
        device.set_device_lost_callback(move |reason, message| {
            *lost_diagnostics
                .device_loss
                .lock()
                .expect("device diagnostics poisoned") =
                Some(format!("wgpu device lost ({reason:?}): {message}"));
        });
        let error_diagnostics = Arc::clone(&runtime_diagnostics);
        device.on_uncaptured_error(Arc::new(move |error| {
            error_diagnostics.note_uncaptured_error(format!("wgpu uncaptured error: {error}"));
        }));

        let present_modes = surface.get_capabilities(&adapter).present_modes;
        let mut config = surface
            .get_default_config(&adapter, desc.width.max(1), desc.height.max(1))
            .ok_or_else(|| "surface is not supported by the chosen adapter".to_string())?;

        // GL33 presents gamma-naive: the engine's already-sRGB 8-bit colors go
        // straight to the framebuffer. Render to a non-sRGB surface so
        // wgpu doesn't apply a second linear->sRGB encode on write.
        let linear = config.format.remove_srgb_suffix();
        if linear != config.format && surface.get_capabilities(&adapter).formats.contains(&linear) {
            config.format = linear;
        }
        // Screenshot capture copies the fully composited swapchain image into a
        // staging buffer. The default surface config is render-attachment-only;
        // explicitly request COPY_SRC so that copy is a real operation rather
        // than a backend-dependent black readback.
        config.usage |= wgpu::TextureUsages::COPY_SRC;

        surface.configure(&device, &config);

        // HDR path (docs/hdr-pipeline-plan.md). Now the default for the wgpu backend —
        // the procedural sky, aerial fog, sky-based lighting and tonemap/bloom/exposure
        // all live on this path, so running without it drops the renderer onto the legacy
        // gamma-naive fallback and looks broken. WGR_HDR=0 still forces it off for A/B.
        // When on, the scene subsystems target the offscreen HDR format and a tonemap pass
        // resolves to the swapchain; the overlay pipeline always targets the swapchain format.
        let prepass_enabled = std::env::var("WGR_PREPASS")
            .map(|v| v != "0")
            .unwrap_or(true);
        // Compute skin bake. Since 2026-08-30 it feeds ONLY the temporal velocity pass
        // (per-limb motion vectors for skinned characters); every draw pass VS-skins as
        // it always did — the original bake-as-draw-source routing shattered the palette
        // draws and is disconnected (see gfx3d::baked_draw_base). Defaults ON whenever
        // the temporal path will run (that is what consumes the poses); WGR_SKIN_BAKE
        // pins either way, and with the bake off skinned characters fall back to rigid/
        // camera velocity.
        let temporal_will_run = {
            let env_on = std::env::var("WGR_TEMPORAL")
                .map(|v| v != "0")
                .unwrap_or(false);
            #[cfg(feature = "dlss")]
            {
                env_on || ngx_raw.is_some()
            }
            #[cfg(not(feature = "dlss"))]
            {
                env_on
            }
        };
        // The 2026-08-30 corruption saga ended at a STRIDE: skin_bake.wgsl was
        // written (and validated) for the 9-word WgrMeshVertex and never updated when
        // the vertex grew to 17 words for the Multi material stages — every reader
        // then de-interleaved garbage (shattered draws, alien velocity triangles, the
        // owner's sky ghosting). Fixed in the shader (17-word verts, tangent frame
        // skinned too) and guarded by tests; the bake again feeds BOTH the draw passes
        // (identity-world routing) and the temporal velocity pass, and defaults on
        // with the temporal path. WGR_SKIN_BAKE pins either way.
        // Default back ON with the temporal path since 2026-08-31: the REN-TEMP-002
        // character erasure was the identity-world pack left disabled while draw
        // routing was reconnected (see object_gpu_for_draw in gfx3d), fixed and
        // verified on the campaign training mission with DLSS genuinely active.
        let skin_bake_enabled = std::env::var("WGR_SKIN_BAKE")
            .map(|v| v != "0")
            .unwrap_or(temporal_will_run);
        // Indirect draw is default-on when the adapter supports it; WGR_INDIRECT=0 forces
        // the direct draw_one path for A/B. Disabled outright without INDIRECT_FIRST_INSTANCE.
        let indirect_enabled = indirect_first_instance
            && std::env::var("WGR_INDIRECT")
                .map(|v| v != "0")
                .unwrap_or(true);
        // GPU-driven rendering (docs/gpu-culling-and-depth-plan.md Stage 3). Default-on now
        // that the path is built up; inert until C++ registers a retained scene (Stage 3b-3),
        // and needs first_instance for its indirect args. WGR_GPU_DRIVEN=0 forces it off.
        let gpu_driven_enabled = indirect_first_instance
            && std::env::var("WGR_GPU_DRIVEN")
                .map(|v| v != "0")
                .unwrap_or(true);
        let hdr_enabled = std::env::var("WGR_HDR").map(|v| v != "0").unwrap_or(true);
        // The one line a tester's log must carry: DLSS, and if not, why (dlss_status.rs).
        #[cfg(feature = "dlss")]
        let dlss_up = ngx_raw.is_some();
        #[cfg(not(feature = "dlss"))]
        let dlss_up = false;
        if dlss_up && !hdr_enabled {
            dlss_status.latch("HDR path off (WGR_HDR=0); DLSS runs on the HDR path only");
        }
        log.log(
            log_level::INFO,
            &if dlss_up && hdr_enabled {
                "DLSS status: NGX up, DLSS Super Resolution available (runs from the first upscaled frame)"
                    .to_owned()
            } else {
                format!("DLSS status: inactive - {}", dlss_status.text())
            },
        );
        let color_format = if hdr_enabled {
            HDR_FORMAT
        } else {
            config.format
        };
        // MSAA (WGR_MSAA, default 4x). Requires the HDR path: the multisampled scene colour is
        // resolved to a single-sample HDR target the tonemap samples, and WebGPU has no depth
        // resolve_target, so the LDR-direct-to-swapchain path stays 1x. Clamped to what the
        // adapter supports for every multisampled scene format (colour + depth + normal G-buffer).
        let msaa_req = std::env::var("WGR_MSAA")
            .ok()
            .and_then(|v| v.parse::<u32>().ok())
            .unwrap_or(4);
        let sample_count = if hdr_enabled && msaa_req > 1 {
            let ok = [color_format, gfx3d::depth_format(&device), gfx3d::NORMAL_FORMAT]
                .iter()
                .all(|&f| {
                    enabled_format_flags(f, device.features(), adapter.get_texture_format_features(f).flags)
                        .sample_count_supported(msaa_req)
                });
            if ok {
                msaa_req
            } else {
                log.log(
                    log_level::WARN,
                    &format!("wgpu MSAA {msaa_req}x unsupported for the scene formats; using 1x"),
                );
                1
            }
        } else {
            1
        };
        if sample_count > 1 {
            log.log(
                log_level::INFO,
                &format!("wgpu MSAA enabled: {sample_count}x (WGR_MSAA)"),
            );
        }
        log.log(
            log_level::INFO,
            &format!("wgpu scene depth: {:?}", gfx3d::depth_format(&device)),
        );
        if hdr_enabled {
            log.log(log_level::INFO, "wgpu HDR path enabled (WGR_HDR)");
        }
        // REN-TEMP-001B: 3D render scale (WGR_RENDER_SCALE, percent). The scene renders at
        // render_size() and is upscaled to the swapchain size before bloom/exposure/tonemap;
        // the UI phase is untouched. Requires the HDR path — the LDR-direct path draws straight
        // to the swapchain and has no resolve to rescale. 100 is bit-identical to the old chain.
        // 50..100 = upscaling (DLSS/bilinear), 100 = native, 101..200 = SSAA (the scene
        // renders ABOVE output resolution and the bilinear pass downsamples — the DLSS
        // backend skips itself above 100%).
        let render_scale = std::env::var("WGR_RENDER_SCALE")
            .ok()
            .and_then(|v| v.parse::<u32>().ok())
            .map(|pct| pct.clamp(50, 200))
            .unwrap_or(100) as f32
            / 100.0;
        if render_scale != 1.0 {
            if hdr_enabled {
                log.log(
                    log_level::INFO,
                    &format!(
                        "wgpu render scale {:.0}% (WGR_RENDER_SCALE): 3D at reduced resolution, UI native",
                        render_scale * 100.0
                    ),
                );
            } else {
                log.log(
                    log_level::WARN,
                    "WGR_RENDER_SCALE ignored: requires the HDR path (WGR_HDR=1)",
                );
            }
        }
        // Default DLSS experience = the owner-validated Quality point (67%), whenever the
        // NGX route came up and the env did not pin a scale. The dev panel's Temporal tab
        // changes it live; a plain (non-dlss) build never reaches this.
        #[cfg(feature = "dlss")]
        let (render_scale, render_scale_auto) =
            if ngx_raw.is_some() && hdr_enabled && std::env::var("WGR_RENDER_SCALE").is_err() {
                log.log(
                    log_level::INFO,
                    "wgpu render scale defaulting to 67% (DLSS route up; dev panel tunes it live)",
                );
                (0.67, true)
            } else {
                (render_scale, false)
            };
        #[cfg(not(feature = "dlss"))]
        let render_scale_auto = false;
        if !prepass_enabled {
            log.log(
                log_level::INFO,
                "wgpu depth prepass disabled (WGR_PREPASS=0)",
            );
        }
        if skin_bake_enabled {
            log.log(
                log_level::INFO,
                "wgpu compute skin bake enabled (WGR_SKIN_BAKE); VS skinning path bypassed",
            );
        }
        if indirect_enabled {
            log.log(
                log_level::INFO,
                "wgpu GPU-driven indirect draws enabled (WGR_INDIRECT)",
            );
        } else if !indirect_first_instance {
            log.log(
                log_level::WARN,
                "adapter lacks INDIRECT_FIRST_INSTANCE; indirect draws off, using direct path",
            );
        }

        let textures = SharedTextures::new(
            &device,
            &queue,
            bc_supported,
            object_texture_cap,
            partially_bound_enabled,
            // So the bindless slot-0 texel announces itself in the GAME log rather than on
            // stderr, which this harness never captures. See announce_fallback_texel.
            &log,
        );
        // One composer, pre-loaded with the shared shader modules, shared by the
        // 3D subsystems that #import them.
        let mut composer = shaders::build_composer();
        let gfx2d = Gfx2d::new(
            &device,
            &textures,
            color_format,
            config.format,
            sample_count,
        );
        let mut gfx3d = Gfx3d::new(
            &device,
            &textures,
            color_format,
            sample_count,
            &mut composer,
            skin_bake_enabled,
            indirect_enabled,
            gpu_driven_enabled,
            gpu_driven_enabled && multi_draw_count,
        );
        gfx3d.set_geometry_pressure_budget(residency_budget_bytes);
        // One line, every renderer gate, always printed — because "is this feature on?"
        // turned out to be genuinely hard to answer. Each gate is decided in up to three
        // layers: the Rust default here, a separate C++ default in EngineWgpu, and
        // ConfigureWgpuUltraEnvironment in GameApplication, which SETS the environment
        // variables before the engine is created and so overrides both. The layer furthest
        // from the renderer is the one that decides the shipped game, and neither the code
        // defaults nor the plan documents tell you what actually runs.
        //
        // Reading a status line instead of this produced a wrong conclusion in the RND-030
        // audit (see design notes).
        // eprintln! rather than log.log: it is what reaches captured stderr in a harness run.
        eprintln!(
            "[wgr] effective gates: hdr={} prepass={} indirect={} gpu_driven={} skin_bake={} msaa={}x multi_draw_count={}",
            hdr_enabled,
            prepass_enabled,
            indirect_enabled,
            gpu_driven_enabled,
            skin_bake_enabled,
            sample_count,
            multi_draw_count
        );
        // Directional-ambient normal mapping (WGR_AMBIENT_NORMAL_MAPPED). Announced through the
        // GAME log rather than stderr, and UNCONDITIONALLY rather than only when it differs from
        // the default: the whole value of the line is that a captured dawn screenshot can be
        // matched to which arm produced it.
        log.log(
            log_level::INFO,
            &crate::gfx3d::ambient_normal_mapped_log_line(),
        );
        // Normal-map relief under ambient light (WGR_NORMAL_CAVITY / WGR_SKY_SPECULAR). Same
        // reasoning, same destination: the two terms that answer "the normals do not read at
        // dawn", announced where a captured run can prove which arm produced it.
        log.log(log_level::INFO, &crate::gfx3d::normal_relief_log_line());
        log.log(log_level::INFO, &format!(
            "wgpu palette upload batch: {} (WGR_PALETTE_UPLOAD_BATCH; padded strides retain per-slot uploads)",
            crate::gfx3d::palette_upload_batch_enabled()
        ));
        if let Some(line) = crate::gfx3d::skin_bake_over_limit_log_line() {
            log.log(log_level::WARN, &line);
        }
        if let Some(line) = crate::gfx3d::pool::geometry_pool_log_line() {
            log.log(log_level::INFO, &line);
        }
        if let Some(line) = crate::gfx3d::storage_array_log_line() {
            log.log(log_level::INFO, &line);
        }
        if let Some(line) = crate::gfx3d::pool::geometry_pool_events_log_line() {
            log.log(log_level::INFO, &line);
        }
        if gpu_driven_enabled {
            log.log(
                log_level::INFO,
                "wgpu GPU-driven rendering enabled (WGR_GPU_DRIVEN); inert until a scene registers",
            );
            if !multi_draw_count {
                log.log(
                    log_level::INFO,
                    "adapter lacks MULTI_DRAW_INDIRECT_COUNT; GPU-driven draws use the conservative no-op-tail path",
                );
            }
        }
        let terrain = Terrain::new(
            &device,
            &queue,
            gfx3d.camera_layout(),
            color_format,
            sample_count,
            !partially_bound.is_empty(),
            textures.white_view().clone(),
            &mut composer,
        );
        let grass = Grass::new(
            &device,
            &queue,
            gfx3d.camera_layout(),
            gfx3d.shadow_pass_layout(),
            color_format,
            sample_count,
            &mut composer,
        );
        // FAR INSTANCE TIER. Pipelines only — its buffers (~42 MB of compacted output plus
        // the world's source rows) are not allocated until a world actually pushes
        // placements, so a world with no far tier and a build launched with WGR_FAR_TIER=0
        // pay nothing but three shader compiles.
        let far = Far::new(
            &device,
            gfx3d.camera_layout(),
            color_format,
            sample_count,
            &mut composer,
        );
        let fft_storage_supported = [
            wgpu::TextureFormat::Rgba32Float,
            wgpu::TextureFormat::Rgba16Float,
        ]
        .iter()
        .all(|&format| {
            adapter
                .get_texture_format_features(format)
                .allowed_usages
                .contains(wgpu::TextureUsages::STORAGE_BINDING)
        });
        let tonemap = hdr_enabled.then(|| Tonemap::new(&device, config.format));
        let water = WaterBackend::new(
            &device, &queue, gfx3d.camera_layout(), color_format, sample_count,
            &mut composer, fft_storage_supported,
        );
        let rain_water = rain_water::RainWater::new(&device, gfx3d.camera_layout(), color_format, sample_count, &mut composer);
        log.log(log_level::INFO, &format!("Water backend: initial {}", water.kind().label()));
        if let Some(op) = water.current_op() {
            if op.water.fft_enabled() {
                log.log(log_level::INFO, &format!(
                    "Hydro FFT ocean enabled: initial {0}x{0} cascade maps; live tiers 256/512/1024",
                    water::FFT_RESOLUTION));
            } else {
                log.log(log_level::WARN,
                    "Hydro FFT ocean unavailable; using the analytic Gerstner fallback");
            }
        }
        let bloom = hdr_enabled.then(|| Bloom::new(&device, HDR_FORMAT));
        // God rays composite into the HDR scene target, so like bloom they only exist on the
        // HDR path. Default ON; `WGR_GODRAYS=0` ablates it for an A/B capture.
        let godrays = hdr_enabled.then(|| GodRays::new(&device, HDR_FORMAT, sample_count));
        // SMK-037: created unconditionally (one pipeline, no target until a frame asks).
        let soft_particles = soft_particles::SoftParticles::new(&device);
        // SMK-038: same -- pipelines and the froxel texture, but no march target and no work
        // recorded until the mode is switched off Legacy.
        let smoke_volume = smoke_volume::SmokeVolume::new(&device, color_format, sample_count);
        let planar_mips = PlanarMips::new(&device, HDR_FORMAT);
        let exposure = hdr_enabled.then(|| Exposure::new(&device, &queue));
        // The sky targets the scene color format (HDR target or swapchain), matching
        // the scene pipelines, and self-tonemaps when that is an LDR-direct swapchain.
        let sky = Sky::new(&device, &queue, color_format, sample_count);
        // WTR-002 — the timestamp query set + readback ring (inert when unsupported).
        let gpu_timers = GpuTimers::new(&device, &queue, ts_enabled, ts_inside_passes);
        let water_visibility = WaterVisibility::new(&device);
        if gpu_timers.enabled() {
            log.log(
                log_level::INFO,
                "WTR-002 GPU timestamp instrumentation enabled",
            );
            log.log(
                log_level::INFO,
                if ts_inside_passes {
                    "GRS-A in-pass timestamps enabled; grass draw rows will report"
                } else {
                    "adapter lacks TIMESTAMP_QUERY_INSIDE_PASSES; grass draw rows read n/a \
                     (placement rows still report)"
                },
            );
        } else {
            log.log(
                log_level::WARN,
                "adapter lacks TIMESTAMP_QUERY(_INSIDE_ENCODERS); WTR-002 GPU timings unavailable",
            );
        }
        // Seed live params from the env knobs so behaviour is unchanged until the
        // ImGui tab pushes its own values (env_f32's >0 filter is fine for scales;
        // env_f32_opt keeps a 0 for the mode/encode toggles).
        let tonemap_params = ffi::WgrTonemap {
            exposure: env_f32("WGR_EXPOSURE", 1.0),
            mode: env_f32_opt("WGR_TONEMAP", 1.0),
            encode: env_f32_opt("WGR_HDR_ENCODE", 1.0),
            ..Default::default()
        };

        let runtime_capabilities = (if bc_supported { 1 } else { 0 })
            | (if partially_bound_enabled { 1 << 1 } else { 0 })
            | (if indirect_first_instance { 1 << 2 } else { 0 })
            | (if multi_draw_count { 1 << 3 } else { 0 })
            | (if ts_enabled { 1 << 4 } else { 0 })
            | (if ts_inside_passes { 1 << 5 } else { 0 })
            | (if hdr_enabled { 1 << 6 } else { 0 })
            | (if sample_count > 1 { 1 << 7 } else { 0 });

        #[allow(unused_mut)]
        let mut temporal_auto = false;
        Ok(Self {
            // 1.35 by default: enough to push the reflection-target edge outside a normal
            // grazing view, without giving away more angular resolution than the mip-filtered
            // planar sample can absorb.
            planar_reflection_pad: 1.35,
            water_camera_slot: None,
            // No flow until a world publishes one; slot 0 is the bindless white fallback and
            // the shader reads it as "this world has no river flow map".
            water_flow_slot: 0,
            water_flow_strength: 0.0,
            log,
            runtime_diagnostics,
            surface,
            uploader: std::sync::Arc::new(uploader::Uploader::new(queue.clone())),
            device,
            queue,
            config,
            present_modes,
            textures,
            gfx2d,
            gfx3d,
            terrain,
            grass,
            far,
            water,
            rain_water,
            water_visibility,
            gpu_timers,
            direct_draws: std::cell::Cell::new(DirectDrawStats::default()),
            runtime_capabilities,
            residency_budget_bytes,
            hdr_enabled,
            hdr: None,
            hdr_resolve: None,
            water_scene: None,
            planar: None,
            planar_dbg_last: None,
            planar_div: 2,
            planar_div_want: 2,
            planar_div_hold: 0,
            planar_mips,
            hdr_size: (0, 0),
            render_scale: if hdr_enabled { render_scale } else { 1.0 },
            hdr_upscaled: None,
            temporal: {
                let mut t = temporal::Temporal::from_env();
                #[cfg(feature = "dlss")]
                if ngx_raw.is_some() && std::env::var("WGR_TEMPORAL").is_err() {
                    // The NGX route is up and nothing said otherwise: DLSS is the
                    // default experience, and it needs the temporal contract.
                    t.enabled = true;
                    temporal_auto = true;
                }
                t
            },
            // True when temporal / the 67% scale were switched on purely because the
            // DLSS route looked healthy. If the DLSS backend later dies, run_tonemap
            // rolls exactly these back so the fallback is the untouched native path,
            // not a jittering bilinear one.
            temporal_auto,
            render_scale_auto,
            ngx_route: {
                #[cfg(feature = "dlss")]
                {
                    ngx_raw.is_some()
                }
                #[cfg(not(feature = "dlss"))]
                {
                    false
                }
            },
            upscalers: {
                let mut v: Vec<Box<dyn upscaler::TemporalUpscaler>> = Vec::new();
                #[cfg(feature = "dlss")]
                if let Some((raw, ngx)) = ngx_raw {
                    v.push(Box::new(upscaler::DlssUpscaler::new(raw, ngx)));
                }
                v.push(Box::new(upscaler::FsrUpscaler::new()));
                v.push(Box::new(upscaler::BilinearUpscaler::new()));
                v
            },
            dlss_status,
            dlss_runtime_on: true,
            dlss_auto_exposure: std::env::var("WGR_DLSS_AUTO_EXPOSURE")
                .map(|v| v != "0")
                .unwrap_or(true),
            dlss_jy_flip: std::env::var("WGR_DLSS_JY_FLIP")
                .map(|v| v != "0")
                .unwrap_or(true),
            dlss_mv_render_space: std::env::var("WGR_DLSS_MV_RENDER")
                .map(|v| v != "0")
                .unwrap_or(false),
            dlss_mv_flip: std::env::var("WGR_DLSS_MV_FLIP")
                .map(|v| v != "0")
                .unwrap_or(false),
            dlss_active_frame: false,
            dlss_last_quality: -1,
            // Default ON (owner verdict 2026-08-30: DLSS Quality alone reads softer
            // than native; RCAS at the 0.25-stop default closes the gap).
            dlss_sharpen: std::env::var("WGR_DLSS_SHARPEN")
                .map(|v| v != "0")
                .unwrap_or(true),
            rcas_sharpener: upscaler::RcasSharpener::new(),
            veg_velocity_on: std::env::var("WGR_VEG_VELOCITY")
                .map(|v| v != "0")
                .unwrap_or(true),
            main_cam_off: 0,
            fsr_runtime_on: std::env::var("WGR_FSR").map(|v| v != "0").unwrap_or(true),
            fsr_sharpness: std::env::var("WGR_FSR_SHARPNESS")
                .ok()
                .and_then(|v| v.parse::<f32>().ok())
                .unwrap_or(0.25)
                .clamp(0.0, 2.0),
            last_upscaler: 0,
            main_camera_tuple: (std::env::var("WGR_GEOMETRY_PAGE_CAMERA_TUPLE").ok().as_deref()==Some("1") &&
                std::env::var("WGR_GEOMETRY_PAGE_FIXTURE").ok().as_deref()==Some("1") &&
                (std::env::var("WGR_GEOMETRY_PAGE_SURFACE_CERTIFICATE").ok().as_deref()==Some("1") ||
                 std::env::var("WGR_GEOMETRY_PAGE_RETAIL_WORLD_VISIBLE").ok().as_deref()==Some("1")))
                .then(|| Box::new(main_camera_tuple::Tracker::new())),
            demand_view_snapshot: demand_view_snapshot::startup_enabled(
                std::env::var("WGR_GEOMETRY_PAGE_DEMAND_VIEWS").ok().as_deref(),
                std::env::var("WGR_GEOMETRY_PAGE_FIXTURE").ok().as_deref())
                .then(|| Box::new(demand_view_snapshot::Tracker::new())),
            sample_count,
            tonemap,
            underwater_target: None,
            dof: None,
            dof_target: None,
            dof_size: (0, 0),
            dof_settings: DofSettings::default(),
            dof_depth_warned: false,
            underwater_size: (0, 0),
            underwater_engaged_logged: None,
            submerged_view: false,
            post_source_underwater: false,
            post_source_dof: false,
            underwater_view: UnderwaterView {
                cam_above: -1.0,
                camera_pos: [0.0; 3],
                inv_view_proj: [0.0; 16],
                shallow_color_ext: [0.070, 0.290, 0.320, 0.16],
                deep_color: [0.014, 0.105, 0.240, 0.0],
                sun_dir: [0.0, 1.0, 0.0],
                sun_radiance: [1.0; 3],
                camera_shadow: unsafe { std::mem::zeroed() },
                cascade_lengths: [1.0; 4],
                active_layers: 0,
                warp_amp: 0.0,
                sea_level: 0.0,
                debug_view: 0.0,
                wave_scale: 1.0,
                body_wave_scale: 1.0,
                body_ellipse: [0.0; 4],
                body_frame: [1.0, 0.0],
                density: 1.0,
                color_bias: 1.0,
                caustic_gain: 1.0,
            },
            godrays,
            godrays_frame: None,
            cam_underground: 0.0,
            smoke_volume,
            smoke_sun_dir: [0.0, 1.0, 0.0],
            smoke_sun_radiance: [1.0, 1.0, 1.0],
            smoke_ambient: [0.2, 0.2, 0.24],
            soft_particles,
            soft_enabled: false,
            soft_fade: 1.5,
            soft_globals: SoftGlobals::default(),
            godrays_composited: false,
            frame_depth_final_resolved: false,
            far_depth_resolve_current: false,
            bloom,
            exposure,
            exposure_params: ffi::WgrExposure::default(),
            tonemap_params,
            sky,
            layer_fog_settings: layered_fog::settings(Default::default()),
            layer_fog_weather: 0.0,
            sky_params: ffi::WgrSky::default(),
            cloud_reflection_cheap: true,
            last_sun_shadow: None,
            last_sky_visibility: None,
            foliage_params: ffi::WgrFoliage::default(),
            sky_debug: std::env::var("WGR_SKY_DEBUG").is_ok(),
            sky_dbg_last: (usize::MAX, usize::MAX),
            gtao_dbg_logged: false,
            interior_sky_dbg_frames: 0,
            interior_sky_dbg_logged: false,
            interior_sky_probe_seen: 0,
            prepass_enabled,
            suppress_world_objects: false,
            cull_debug_draw: false,
            screenshot_requested: false,
            post_optics_probe: post_optics_probe::enabled().then(post_optics_probe::State::new),
            screenshot_pixels: None,
        })
    }

    fn request_screenshot(&mut self) {
        if let Some(probe)=self.post_optics_probe.as_mut(){probe.request(self.screenshot_requested);}
        self.screenshot_requested = true;
        self.screenshot_pixels = None;
    }

    fn runtime_capabilities(&self) -> u32 {
        self.runtime_capabilities
    }

    fn take_screenshot(&mut self, out: &mut [u8], width: &mut u32, height: &mut u32) -> u32 {
        let Some(pixels) = self.screenshot_pixels.take() else {
            return 0;
        };
        *width = pixels.width;
        *height = pixels.height;
        if out.len() < pixels.rgba.len() {
            self.screenshot_pixels = Some(pixels);
            return 0;
        }
        out[..pixels.rgba.len()].copy_from_slice(&pixels.rgba);
        pixels.rgba.len() as u32
    }

    // Consolidated ImGui-tweakable render params (wgr_set_render_params). Fans out to the
    // per-subsystem state. tonemap/exposure/sky-look are cheap re-assigns; the two terrain
    // setters are diffed against the last block because they realloc/re-scan (and the sun-shadow
    // setter dirties its sweep on every call). See docs/render-params-consolidation-plan.md.
    fn set_render_params(&mut self, p: ffi::WgrRenderParams) {
        let ablate = *ABLATE.get_or_init(Ablate::from_env);
        self.tonemap_params = p.tonemap;
        self.exposure_params = p.exposure;
        self.foliage_params = p.foliage;

        // Picture Mode depth of field. Translated rather than stored raw so the clamping and the
        // units live in one place (dof.rs), and so the ABI struct can change shape without the
        // pass caring.
        let d = &p.depth_of_field;
        self.dof_settings = DofSettings {
            enabled: d.enabled != 0,
            focus_distance: d.focus_distance,
            focus_range: d.focus_range,
            max_blur_pixels: d.max_blur_pixels,
            background_scale: d.background_scale,
            foreground_scale: d.foreground_scale,
            transition: d.transition,
            near_plane: d.near_plane,
            debug_view: d.debug_view,
            sample_count: d.sample_count,
            bokeh_boost: d.bokeh_boost,
            bokeh_threshold: d.bokeh_threshold,
            aperture_blades: d.aperture_blades,
        };

        // Write the LOOK half of the sky UBO, leaving the runtime slots set_sky_runtime owns
        // (sun/moon dir + phase, night factor, fog rgb, cam altitude, fog far) intact.
        let s = &mut self.sky_params;
        let l = &p.sky;
        s.sun_dir[3] = l.ground_sun[3]; // sun radiance scale (sunIntensity)
        s.rayleigh = l.rayleigh;
        s.mie = l.mie;
        s.ground_albedo[0] = l.ground_sun[0];
        s.ground_albedo[1] = l.ground_sun[1];
        s.ground_albedo[2] = l.ground_sun[2];
        s.params = l.params;
        s.control = l.control;
        s.fog_color[3] = l.night_zenith[3]; // horizon-haze strength
        s.night_zenith[0] = l.night_zenith[0];
        s.night_zenith[1] = l.night_zenith[1];
        s.night_zenith[2] = l.night_zenith[2];
        s.night_horizon = l.night_horizon; // xyz + aerial-shadow (w)
        s.night_params[0] = l.night_params[0];
        s.night_params[1] = l.night_params[1];
        s.night_params[2] = l.night_params[2];
        // REN-SKY-004: not a shader lane -- it picks which cloud PIPELINE the planar reflection
        // records, so it is kept on the renderer rather than copied into the sky uniform.
        self.cloud_reflection_cheap = l.night_params[3] > 0.5;
        // Cloud look. cloud1.xy (wind WORLD offset) is a runtime field owned by set_sky_runtime,
        // so copy only the shape/detail scale lanes (z,w) here, leaving xy intact.
        s.cloud0 = l.cloud0;
        s.cloud1[2] = l.cloud1[2]; // shape scale
        s.cloud1[3] = l.cloud1[3]; // detail scale
        s.cloud2 = l.cloud2;
        s.cloud3 = l.cloud3;

        if self.last_sun_shadow != Some(p.terrain_sun_shadow) {
            self.last_sun_shadow = Some(p.terrain_sun_shadow);
            let ss = &p.terrain_sun_shadow;
            self.terrain_set_sun_shadow(ss.strength, ss.scale, ss.max_steps, ss.penumbra_deg);
        }
        if self.last_sky_visibility != Some(p.sky_visibility) {
            self.last_sky_visibility = Some(p.sky_visibility);
            let sv = &p.sky_visibility;
            self.terrain_set_sky_visibility(
                sv.strength,
                sv.contrast,
                sv.floor,
                sv.radius_m,
                sv.k_azimuths,
                sv.downsample,
                sv.debug != 0,
            );
        }
        // GTAO. Unconditional (no last_* compare): it only writes a plain struct field, and the
        // dirty-tracking above exists because those setters can re-run a CPU horizon scan.
        let g = &p.gtao;
        self.gfx3d.set_gtao_settings(crate::gfx3d::GtaoSettings {
            enabled: g.enabled != 0 && !ablate.ao,
            radius_m: g.radius_m,
            strength: g.strength,
            slices: g.slices,
            steps: g.steps,
            max_radius_px: g.max_radius_px,
            thickness: g.thickness,
            blur_radius: g.blur_radius,
            blur_depth_scale: g.blur_depth_scale,
            blur_normal_power: g.blur_normal_power,
            debug_mode: g.debug,
            bent_normal: g.bent_normal != 0,
            max_mip: g.max_mip,
        });
        // Interior sky visibility (LIT-020). Same unconditional plain-field write as GTAO above;
        // the settings struct clamps, so a garbage push cannot allocate a 64k depth map.
        let mut sv = *self.gfx3d.interior_sky_settings();
        sv.apply(&p.interior_sky);
        sv.enabled &= !ablate.skyvis;
        self.gfx3d.set_interior_sky_settings(sv);
        self.gfx3d.set_gi_settings(&p.gi); // REN-GI-001
        self.layer_fog_settings = layered_fog::settings(p.layered_fog);
    }

    // Per-frame sky runtime (wgr_set_sky_runtime): the celestial + camera fields, written into
    // the runtime half of the sky UBO. The authored look half comes from set_render_params.
    fn set_planar_reflection_pad(&mut self, pad: f32) {
        self.planar_reflection_pad = pad.clamp(1.0, 3.0);
    }

    fn set_star_intensity(&mut self, intensity: f32) {
        self.sky.set_star_intensity(intensity);
    }

    fn set_lens_flare(&mut self, intensity: f32) {
        self.sky.set_lens_flare(intensity);
    }

    fn set_cloud_shadow_strength(&mut self, strength: f32) {
        self.sky.set_cloud_shadow_strength(strength);
    }

    fn set_smoke_shadow(&mut self, blobs: &[sky::SmokeShadowBlob], strength: f32) {
        self.sky.set_smoke_shadow(blobs, strength);
    }

    fn set_grass_card_spacing(&mut self, spacing: f32) {
        self.grass.set_card_spacing(spacing);
    }

    fn set_grass_card_tone_enabled(&mut self, enabled: bool) {
        self.grass.set_card_tone_enabled(enabled);
    }

    // Material Debug (dev panel, Materials tab) for the retained GPU-driven path.
    // `view`: 0 full, 1 base, 2 normal, 3 screen AO, 4 spec/gloss, 5 UV, 6 lighting.
    // `flags`: bit0 disable normal map, bit1 invert normal Y, bit2 compose Multi layers.
    fn set_material_debug(&mut self, view: u32, flags: u32) {
        self.gfx3d.set_material_debug(view, flags);
    }

    fn set_cirrus_mode(&mut self, mode: u32) {
        self.sky.set_cirrus_mode(mode);
    }

    fn set_cirrus_puffiness(&mut self, puffiness: f32, variation: f32) {
        self.sky.set_cirrus_puffiness(puffiness, variation);
    }

    fn set_cirrus_look(&mut self, amount: f32, match_deck: f32) {
        self.sky.set_cirrus_look(amount, match_deck);
    }

    fn set_cirrus_softness(&mut self, softness: f32) {
        self.sky.set_cirrus_softness(softness);
    }

    fn set_road_conform(&mut self, flat: f32, per_m: f32, max_frac: f32) {
        self.gfx3d.set_road_conform(flat, per_m, max_frac);
    }

    fn set_fog_far_close(&mut self, close: f32) {
        self.gfx3d.set_fog_far_close(close);
    }

    // Volumetric sun shafts (dev panel, Sky tab). Inert on the LDR-direct path, where the pass
    // does not exist at all.
    fn set_god_rays(&mut self, s: GodRaySettings) {
        if let Some(gr) = self.godrays.as_mut() {
            gr.set_settings(s);
        }
    }

    fn set_sky_runtime(&mut self, rt: ffi::WgrSkyRuntime) {
        let s = &mut self.sky_params;
        s.sun_dir[0] = rt.sun_dir[0]; // keep sun_dir.w (sunIntensity, a look field)
        s.sun_dir[1] = rt.sun_dir[1];
        s.sun_dir[2] = rt.sun_dir[2];
        self.gfx3d
            .set_sun_dir([rt.sun_dir[0], rt.sun_dir[1], rt.sun_dir[2]]); // REN-GI-002
        s.moon_dir = rt.moon_dir; // xyz dir + w illuminated fraction
        s.ground_albedo[3] = rt.misc[0]; // night factor
        s.fog_color[0] = rt.fog_color[0]; // keep fog_color.w (haze, a look field)
        s.fog_color[1] = rt.fog_color[1];
        s.fog_color[2] = rt.fog_color[2];
        s.night_zenith[3] = rt.misc[1]; // camera altitude ASL
        s.night_params[3] = rt.fog_color[3]; // fog far-range
        s.cloud1[0] = rt.misc[2]; // cloud wind world offset x (m, CPU-wrapped)
        s.cloud1[1] = rt.misc[3]; // cloud wind world offset z (m, CPU-wrapped)
        s.cloud4 = rt.cloud_evolve; // shape / detail / weather drift offsets (m, CPU-wrapped)
        s.moon_params = rt.moon_params; // radius / illuminated fraction / radiance / draw
        s.moon_sun = rt.moon_sun; // sub-solar direction + earthshine
        self.layer_fog_weather = if rt.layer_fog_weather[1] >= 0.5 && rt.layer_fog_weather[2] >= 0.5
            && rt.layer_fog_weather[0].is_finite() && (0.0..=1.0).contains(&rt.layer_fog_weather[0])
            { rt.layer_fog_weather[0] } else { 0.0 };
    }

    // Debug readback of the current auto-exposure scale (blocking; dev panel only).
    fn exposure_scale(&self) -> f32 {
        self.exposure
            .as_ref()
            .map(|e| e.read_scale(&self.device, &self.queue))
            .unwrap_or(1.0)
    }

    // WTR-002 — copy the latest completed-frame GPU timings into `out` (ms per region;
    // -1 = never measured / pass absent). Returns the region count, 0 when unsupported.
    // Non-blocking: values are harvested by render_frame, this is a plain copy.
    fn gpu_timings(&self, out: &mut [f32]) -> u32 {
        self.gpu_timers.timings(out)
    }

    // PERF-005 — per-region CPU ENCODE ms, same indices as gpu_timings. Always available
    // (needs no adapter feature); -1 = the region was not recorded this frame.
    fn cpu_timings(&self, out: &mut [f32]) -> u32 {
        self.gpu_timers.cpu_timings(out)
    }

    // PERF-005 — this frame's object accounting: the GPU cull's per-view counters plus the
    // CPU-replayed direct-draw tally.
    // LGT-026 — how many local-light shadow views re-rendered / were re-used last frame.
    fn local_shadow_view_stats(&self) -> (u32, u32) {
        self.gfx3d.local_shadow_view_stats()
    }

    fn object_stats(&self) -> (crate::gfx3d::cull::CullStats, DirectDrawStats, u32, [u32; 8]) {
        (
            self.gfx3d.object_stats(),
            self.direct_draws.get(),
            self.gfx3d.retained_instance_count(),
            self.gfx3d.object_fragment_census(),
        )
    }

    fn stale_instance_ops(&self) -> u64 {
        self.gfx3d.stale_instance_ops()
    }

    // GRS-A — latest grass instance counts (same async-readback discipline).
    fn grass_stats(&self) -> crate::grass::GrassStats {
        self.grass.stats()
    }

    fn memory_stats(&self) -> ffi::WgrMemoryStats {
        let (object_texture_bytes, object_texture_count) = self.textures.residency();
        let geometry = self.gfx3d.geometry_residency();
        let object_texture_retired_bytes = self.textures.retired_payload_bytes();
        let tracked_bytes = object_texture_bytes
            .saturating_add(object_texture_retired_bytes)
            .saturating_add(geometry.capacity_bytes)
            .saturating_add(geometry.retired_bytes);
        // Backend counters include fixed targets, staging and in-flight resources.
        // They do not include allocator slack or other applications, and some
        // backends leave counters unset. Do not silently redefine payload budgets.
        let counters = self.device.get_internal_counters().hal;
        let backend_allocation_bytes = (counters.buffer_memory.read().max(0) as u64)
            .saturating_add(counters.texture_memory.read().max(0) as u64);
        ffi::WgrMemoryStats {
            backend_allocation_bytes,
            tracked_bytes,
            budget_bytes: self.residency_budget_bytes,
            object_texture_bytes,
            geometry_live_bytes: geometry.live_bytes,
            geometry_capacity_bytes: geometry.capacity_bytes,
            geometry_retired_bytes: geometry.retired_bytes,
            object_texture_count,
            object_texture_retired_bytes,
            over_budget: u32::from(
                self.residency_budget_bytes != 0 && tracked_bytes > self.residency_budget_bytes,
            ),
        }
    }

    // Record the FAR-reduce MSAA depth resolve, unless one already recorded on this encoder is
    // still valid (see `far_depth_resolve_current`). No-op at 1x, where `water_depth_view` is the
    // depth target's own aspect and is always current.
    //
    // PERF: `Gfx3d::resolve_water_depth` is NOT idempotent — it unconditionally records a
    // full-screen pass that reads all `sample_count` depth samples per pixel. On a normal
    // above-water frame with clouds the old code recorded it twice back to back (water, then
    // clouds) with only the water draw in between, and that pass attaches depth READ-ONLY
    // (`depth_ops: None`), so the second resolve provably could not observe anything new.
    //
    // Every caller now goes through here, and every pass that writes depth clears the flag, so
    // this stays correct for the cases where a later resolve genuinely IS needed: a post-
    // `ClearDepth` segment (the weapon/near view) rewrites depth, and the underwater compositor
    // and god rays run after it.
    fn ensure_far_depth_resolve(&mut self, encoder: &mut wgpu::CommandEncoder) {
        if self.far_depth_resolve_current {
            return;
        }
        self.gfx3d.resolve_water_depth(encoder);
        self.far_depth_resolve_current = true;
    }

    // Record the god-ray march + additive composite into the single-sample HDR scene view.
    //
    // Everything it reads already exists and is already up to date at this point in the frame:
    // the sky's CLD-020 cloud transmittance map, the terrain sun-shadow ceiling mask, the cascade
    // shadow array, and the resolved scene depth. No new geometry pass, no new shadow render.
    fn render_god_rays(&mut self, encoder: &mut wgpu::CommandEncoder, scene: &wgpu::TextureView) {
        let Some(frame) = self.godrays_frame else {
            return; // the sky pass did not run this frame, so there is no camera to march with
        };
        if !self
            .godrays
            .as_ref()
            .is_some_and(|g| g.active(&self.sky_params))
        {
            return;
        }
        // The march samples a SINGLE-SAMPLE depth. Under MSAA that is the far-reduce resolve the
        // water and cloud passes also use; both of those run earlier, so on a normal frame this is
        // already done and the flag skips a second full-screen resolve. At 1x it is a no-op.
        if !self.frame_depth_final_resolved {
            self.ensure_far_depth_resolve(encoder);
            self.frame_depth_final_resolved = true;
        }
        let Some(depth) = self.gfx3d.water_depth_view().cloned() else {
            return;
        };
        let cloud = self.sky.cloud_shadow_view().clone();
        let cloud_map = self.sky.cloud_shadow_mapping_current();
        let mask = self.terrain.shadow_mask_view();
        let csm = self.gfx3d.shadow_sample_view().clone();
        // God rays march at the SCENE resolution: they composite into the render-res HDR
        // target and read the render-res depth resolve.
        let (width, height) = self.render_size();
        // Disjoint field borrows: the pass owns none of the resources it reads.
        let device = &self.device;
        let queue = &self.queue;
        let sky = &self.sky_params;
        let Some(gr) = self.godrays.as_mut() else {
            return;
        };
        gr.render(
            device, queue, encoder, scene, &depth, &cloud, &mask, &csm, &frame, sky, cloud_map,
            width, height,
        );
    }

    // Tonemap the HDR scene target onto `dst` (the swapchain). No-op if the tonemap
    // pass doesn't exist (LDR-direct path).
    fn run_tonemap(
        &mut self,
        encoder: &mut wgpu::CommandEncoder,
        source: &wgpu::TextureView,
        dst: &wgpu::TextureView,
        underwater_time: Option<f32>,
        draws3d: &[WgrDraw3D],
    ) {
        if self.tonemap.is_none() {
            return;
        }
        // REN-TEMP-001F: static-world velocity, computed here because every 3D depth write
        // is done by the Resolve op. Reads the frame-final far-reduce depth resolve (the
        // same freshness machinery the god rays use); the DLSS stage may later prefer the
        // nearest-sample resolve — that choice is confined to this call.
        if self.temporal.enabled {
            if !self.frame_depth_final_resolved {
                self.ensure_far_depth_resolve(encoder);
                self.frame_depth_final_resolved = true;
            }
            if let Some(depth) = self.gfx3d.water_depth_view().cloned() {
                let depth_gen = self.gfx3d.depth_gen();
                let render_size = self.render_size();
                self.temporal.render_velocity(
                    &self.device,
                    &self.queue,
                    encoder,
                    &depth,
                    depth_gen,
                    render_size,
                );
                // REN-TEMP-001J: the history-control ("reactive") mask — sky/clouds and
                // the water surface have no usable motion vectors; DLSS gets told so
                // instead of ghosting through them.
                {
                    let sea = self.water.underwater_params().map(|(s, _, _)| s);
                    self.temporal.render_history_control(
                        &self.device,
                        &self.queue,
                        encoder,
                        &depth,
                        depth_gen,
                        render_size,
                        sea,
                        self.sky.cloud_temporal_view(),
                    );
                    // TW-WATER W4b: Tidewater's lips and spray have no motion vectors either
                    if let (Some(slot), Some(mask), Some(cam)) = (
                        self.water_camera_slot,
                        self.temporal.history_control_view(),
                        self.gfx3d.camera_bind(),
                    ) {
                        let off = (slot as u64 * self.gfx3d.camera_stride()) as u32;
                        self.water.draw_reactive(encoder, mask, cam, off);
                    }
                }
                // REN-TEMP-001H: moving rigid objects overwrite their camera-only
                // reprojection with true per-object motion.
                if let (Some(obj_uniform), Some(vel_view)) = (
                    self.temporal.object_velocity_uniform(),
                    self.temporal.velocity_view().cloned(),
                ) {
                    let subset = self.temporal.object_velocity_subset(draws3d);
                    let skin_subset = self.temporal.skinned_velocity_subset(draws3d);
                    // REN-TEMP-001T: real sway motion for vegetation (WGR_VEG_VELOCITY=0
                    // falls back to the frozen-canopy behaviour for A/B).
                    let veg_uniform = if self.veg_velocity_on {
                        self.temporal.veg_velocity_uniform(&self.foliage_params)
                    } else {
                        None
                    };
                    let veg_subset = if veg_uniform.is_some() {
                        self.temporal.veg_velocity_subset(draws3d)
                    } else {
                        Vec::new()
                    };
                    if !veg_subset.is_empty() {
                        static VEG_LIVE: std::sync::Once = std::sync::Once::new();
                        VEG_LIVE.call_once(|| {
                            eprintln!(
                                "[wgr] vegetation sway velocity live: {} draw(s) this frame",
                                veg_subset.len()
                            );
                        });
                    }
                    // REN-TEMP-001T: the RETAINED set's vegetation (the distant trees
                    // the owner watched freeze) gets the same real sway motion via the
                    // gpu-driven velocity twin.
                    if let Some(veg_u) = veg_uniform.as_ref() {
                        static VEG_GPU_LIVE: std::sync::Once = std::sync::Once::new();
                        VEG_GPU_LIVE.call_once(|| {
                            eprintln!("[wgr] retained vegetation velocity twin active");
                        });
                        let gpu_params = temporal::gpu_veg_velocity_params(&obj_uniform, veg_u);
                        self.gfx3d.render_gpu_driven_velocity(
                            &self.device,
                            &self.queue,
                            encoder,
                            &vel_view,
                            &self.textures,
                            self.main_cam_off,
                            &gpu_params,
                        );
                    }
                    self.gfx3d.render_object_velocity(
                        &self.device,
                        &self.queue,
                        encoder,
                        &vel_view,
                        draws3d,
                        &subset,
                        &skin_subset,
                        &veg_subset,
                        &obj_uniform,
                        veg_uniform.as_ref(),
                    );
                }
            }
        }
        // MSAA: resolve the multisampled scene colour into the single-sample HDR target the
        // post-processing chain (bloom/exposure/tonemap) samples. An empty load/store pass with a
        // resolve_target performs the resolve at pass end; StoreOp::Discard drops the now-unneeded
        // multisampled contents. No-op at 1x (hdr_resolve is None, the chain reads `hdr` directly).
        if let (Some((_, msaa_view)), Some((_, resolve_view))) =
            (self.hdr.as_ref(), self.hdr_resolve.as_ref())
        {
            self.gpu_timers.begin(encoder, TimerRegion::HdrResolve);
            encoder.push_debug_group("wgr_hdr_resolve");
            let pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
                label: Some("wgr_hdr_resolve"),
                color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                    view: msaa_view,
                    depth_slice: None,
                    resolve_target: Some(resolve_view),
                    ops: wgpu::Operations {
                        load: wgpu::LoadOp::Load,
                        store: wgpu::StoreOp::Discard,
                    },
                })],
                depth_stencil_attachment: None,
                timestamp_writes: None,
                occlusion_query_set: None,
                multiview_mask: None,
            });
            drop(pass);
            encoder.pop_debug_group();
            self.gpu_timers.end(encoder, TimerRegion::HdrResolve);
        }
        // REN-TEMP-001K: temporal debug resolve (WGR_TEMPORAL_DEBUG=2) — accumulate the
        // resolved HDR frame along velocity. Diagnostic only; nothing downstream reads it.
        if self.temporal.enabled {
            let render_size = self.render_size();
            self.temporal.render_debug_resolve(
                &self.device,
                &self.queue,
                encoder,
                source,
                render_size,
            );
        }
        // Volumetric sun shafts, added to the LINEAR HDR scene here: after the MSAA resolve (so
        // the blend costs one sample per pixel, not `sample_count`) and before bloom / eye
        // adaptation / the tonemap curve, so the shafts bloom, drive exposure and roll off like
        // every other light in the frame. Skipped underwater — the submerged look has its own
        // compositor and its own shafts, and every occluder this pass reads is above the surface.
        // Fallback only. The shafts are normally composited inside the world segment, before
        // the transparent draws (see there); this covers a frame that never reached that point.
        if underwater_time.is_none() && !self.godrays_composited {
            self.gpu_timers.begin(encoder, TimerRegion::GodRays);
            self.render_god_rays(encoder, source);
            self.gpu_timers.end(encoder, TimerRegion::GodRays);
        }
        // The texture behind `post_source`, tracked for native interop (a raw-API
        // upscaler needs the VkImage, not just a view). Starts at the resolved target.
        let mut post_source_tex: Option<wgpu::Texture> = if self.hdr_resolve.is_some() {
            self.hdr_resolve.as_ref().map(|(t, _)| t.clone())
        } else {
            self.hdr.as_ref().map(|(t, _)| t.clone())
        };
        let post_source = if underwater_time.is_some() {
            // The compositor reads a single-sample reversed-Z depth target, including
            // when the HDR scene colour was MSAA-resolved above.
            self.ensure_far_depth_resolve(encoder);
            let (rw, rh) = self.render_size();
            self.ensure_underwater_target(rw, rh);
            let target = &self
                .underwater_target
                .as_ref()
                .expect("underwater target")
                .1;
            let depth = self
                .gfx3d
                .water_depth_view()
                .expect("underwater depth target");
            // WTR-002 — the compositor shader also evaluates the caustics, so that cost
            // rides this bracket until a dedicated caustics pass exists.
            self.gpu_timers
                .begin(encoder, TimerRegion::UnderwaterComposite);
            self.water
                .render_underwater(&self.device, encoder, source, depth, target);
            self.gpu_timers
                .end(encoder, TimerRegion::UnderwaterComposite);
            post_source_tex = self.underwater_target.as_ref().map(|(t, _)| t.clone());
            target.clone()
        } else {
            source.clone()
        };
        // Depth of field, between the god-ray composite and bloom. Returns `post_source`
        // untouched when Picture Mode has not enabled it.
        let (post_source, dof_applied) = self.run_depth_of_field(encoder, &post_source);
        if dof_applied {
            post_source_tex = self.dof_target.as_ref().map(|(t, _)| t.clone());
        }
        // REN-TEMP-001B: upscale the finished render-resolution HDR frame (scene + god rays +
        // underwater + DoF, all of which read render-res depth) to the output resolution the
        // post chain runs at. Bilinear, deliberately minimal — this pass is what a temporal
        // upscaler replaces. When scaling, bloom/exposure/tonemap are permanently bound to
        // `hdr_upscaled` (see ensure_hdr) and the source-swap rebind below must not run: the
        // swap happens on the blit's INPUT instead.
        let post_source = if let Some((up_tex, up_view)) = self.hdr_upscaled.as_ref() {
            // REN-TEMP-001L: run the upscaler chain (plan Phase 13) - DLSS first when
            // present, bilinear as the guaranteed terminal fallback.
            let up_tex = up_tex.clone();
            let up_view = up_view.clone();
            let render = self.render_size();
            let output = (self.config.width, self.config.height);
            let mut ctx = upscaler::UpscaleContext {
                device: &self.device,
                queue: &self.queue,
                encoder,
                color_tex: post_source_tex.clone(),
                color_view: post_source.clone(),
                depth: self
                    .gfx3d
                    .water_depth_texture()
                    .cloned()
                    .zip(self.gfx3d.water_depth_view().cloned()),
                velocity: self
                    .temporal
                    .velocity_texture()
                    .cloned()
                    .zip(self.temporal.velocity_view().cloned()),
                exposure: self
                    .exposure
                    .as_ref()
                    .map(|e| (e.scale_texture().clone(), e.scale_view().clone())),
                history_control: self
                    .temporal
                    .history_control_texture()
                    .cloned()
                    .zip(self.temporal.history_control_view().cloned()),
                output_tex: up_tex,
                output_view: up_view.clone(),
                render,
                output,
                jitter_px: self.temporal.jitter_px,
                reset: self.temporal.reset_this_frame(),
                reset_reason: self.temporal.last_reset_reason(),
                settings: upscaler::UpscaleSettings {
                    temporal_enabled: self.temporal.enabled,
                    fsr_on: self.fsr_runtime_on,
                    fsr_sharpness: self.fsr_sharpness,
                    dlss_runtime_on: self.dlss_runtime_on,
                    dlss_auto_exposure: self.dlss_auto_exposure,
                    jitter_y_flip: self.dlss_jy_flip,
                    mv_render_space: self.dlss_mv_render_space,
                    mv_flip: self.dlss_mv_flip,
                    // The MANUAL exposure factor the tonemap multiplies on top of the
                    // auto-exposure texture - a temporal backend must see the same total.
                    exposure_scale: self.tonemap_params.exposure.max(1e-4),
                },
                notes: Vec::new(),
            };
            // REN-UPS-001: the winner is carried as its CAPABILITIES, not its label.
            // Everything below asks the caps what to do; nothing tests which backend
            // won. `temporal_backend_died` is likewise a capability question, so a
            // future XeSS/FSR3 backend gets the same rollback for free.
            let mut winner: Option<(upscaler::UpscalerCaps, i32)> = None;
            let mut disabled: Vec<String> = Vec::new();
            let mut temporal_backend_died = false;
            let mut dlss_died: Option<String> = None;
            for u in self.upscalers.iter_mut() {
                let caps = u.caps();
                match u.evaluate(&mut ctx) {
                    upscaler::UpscaleOutcome::Done { quality } => {
                        winner = Some((caps, quality));
                        break;
                    }
                    upscaler::UpscaleOutcome::Skip => {}
                    upscaler::UpscaleOutcome::Disabled(msg) => {
                        disabled.push(format!("{} disabled: {}", u.label(), msg));
                        temporal_backend_died |= caps.needs_temporal_inputs;
                        if caps.info_id == upscaler::ID_DLSS {
                            dlss_died = Some(msg);
                        }
                    }
                }
            }
            let notes = std::mem::take(&mut ctx.notes);
            drop(ctx);
            for n in &notes {
                self.log.log(log_level::INFO, n);
            }
            for m in &disabled {
                self.log.log(log_level::WARN, m);
            }
            if let Some(reason) = dlss_died {
                // Permanent for this process (the backend latched itself off); the
                // panel and the log carry the same sentence.
                self.log
                    .log(log_level::INFO, &format!("DLSS status: inactive - {reason}"));
                self.dlss_status.latch(reason);
            }
            // A dying temporal backend takes its auto-defaults with it: jitter without
            // an accumulator is shimmer, and 67% bilinear is not the native look anyone
            // asked for. Explicit env pins and panel choices are left alone.
            if temporal_backend_died {
                if self.temporal_auto && self.temporal.enabled {
                    self.temporal.enabled = false;
                    self.log.log(
                        log_level::WARN,
                        "temporal path disabled again (was auto-enabled for DLSS)",
                    );
                }
                if self.render_scale_auto && (self.render_scale - 1.0).abs() > 1e-3 {
                    self.render_scale = 1.0;
                    self.temporal
                        .notify_reset("DLSS fallback: render scale back to 100%");
                    self.log.log(
                        log_level::WARN,
                        "render scale back to 100% (was auto-set to 67% for DLSS)",
                    );
                }
            }
            if let Some((caps, q)) = winner {
                if caps.info_id == upscaler::ID_DLSS {
                    // The `dlss_active` / `dlss_quality` panel readouts name DLSS
                    // specifically, so they stay tied to that id rather than to
                    // "some temporal backend ran".
                    self.dlss_active_frame = true;
                    self.dlss_last_quality = q;
                    self.dlss_status.set_active();
                }
                // The external RCAS pass is a CAPABILITY of the winning backend, not a
                // property of DLSS: FSR 1 sharpens itself, bilinear is diagnostic.
                if caps.wants_external_sharpen && self.dlss_sharpen {
                    let (tex, view) = self
                        .hdr_upscaled
                        .as_ref()
                        .map(|(t, v)| (t.clone(), v.clone()))
                        .expect("a backend ran, so the upscale target exists");
                    self.rcas_sharpener.sharpen(
                        &self.device,
                        &self.queue,
                        encoder,
                        &tex,
                        &view,
                        self.fsr_sharpness,
                    );
                }
            }
            self.last_upscaler = winner
                .map(|(caps, _)| caps.info_id)
                .unwrap_or(upscaler::ID_NATIVE);
            up_view
        } else {
            post_source
        };
        // Per-frame reasons: NGX is up but DLSS did not produce this frame. Recomputed
        // every frame so a panel toggle changes the sentence at once; a latched startup
        // or failure reason always wins over these.
        if self.ngx_route && !self.dlss_status.is_active() {
            let render = self.render_size();
            let out = (self.config.width, self.config.height);
            let reason = if !self.dlss_runtime_on {
                dlss_status::REASON_PANEL_OFF
            } else if !self.temporal.enabled {
                dlss_status::REASON_TEMPORAL_OFF
            } else if render.0 > out.0 || render.1 > out.1 {
                "render scale above 100% (SSAA); DLSS only upscales, the bilinear downsample runs"
            } else if self.hdr_upscaled.is_none() {
                "no upscale target this frame (native resolution without DLAA)"
            } else {
                "the DLSS backend did not run this frame"
            };
            self.dlss_status.frame_reason(reason);
        }
        let underwater = underwater_time.is_some();
        if self.hdr_upscaled.is_none()
            && (self.post_source_underwater != underwater || self.post_source_dof != dof_applied)
        {
            let bloom_view = self
                .bloom
                .as_ref()
                .and_then(|b| b.view())
                .unwrap_or(&post_source);
            let scale_view = self
                .exposure
                .as_ref()
                .map(|e| e.scale_view())
                .unwrap_or(&post_source);
            self.tonemap.as_mut().expect("tonemap").set_source(
                &self.device,
                &post_source,
                bloom_view,
                scale_view,
            );
            if let Some(bloom) = self.bloom.as_mut() {
                bloom.set_source(&self.device, &post_source);
            }
            if let Some(exposure) = self.exposure.as_mut() {
                exposure.set_source(&self.device, &post_source);
            }
            self.post_source_underwater = underwater;
            self.post_source_dof = dof_applied;
        }
        // Live params from the ImGui Tonemap tab (seeded from WGR_* at startup).
        self.tonemap
            .as_ref()
            .expect("tonemap")
            .upload_params(&self.queue, &self.tonemap_params, self.cam_underground);
        // Build the bloom pyramid from the finished HDR scene (already includes aerial
        // perspective) so the resolve can add it. Skipped when intensity is 0 (the
        // resolve then adds bloom*0, so stale mip contents are harmless).
        if self.tonemap_params.bloom_intensity > 0.0 {
            if let Some(bloom) = self.bloom.as_ref() {
                bloom.upload_params(
                    &self.queue,
                    self.tonemap_params.bloom_threshold,
                    self.tonemap_params.bloom_knee,
                    1.0,
                );
                self.gpu_timers.begin(encoder, TimerRegion::Bloom);
                bloom.render(encoder);
                self.gpu_timers.end(encoder, TimerRegion::Bloom);
            }
        }
        // Eye adaptation: reduce the scene to average luminance and ease the exposure
        // scale (the resolve multiplies exposure by it). Always run on the HDR path —
        // when disabled it just eases to 1.0 — the reduction is a few cheap passes.
        let mut exposure_published=false;
        if let Some(exposure) = self.exposure.as_ref() {
            exposure.upload_params(&self.queue, &cave_exposure(self.exposure_params, self.cam_underground));
            self.gpu_timers.begin(encoder, TimerRegion::ExposureAdapt);
            exposure_published=exposure.render(encoder);
            self.gpu_timers.end(encoder, TimerRegion::ExposureAdapt);
        }
        if self.screenshot_requested {
            if let Some(state)=self.post_optics_probe.as_mut(){
                let result=if !exposure_published{Err("exposure not published this encoder")}else if self.cam_underground>0.0{Err("underground tonemap unsupported by optics probe")}else if self.tonemap_params.nv_strength!=0.0{Err("night vision unsupported")}else{
                    self.tonemap.as_ref().ok_or("missing tonemap").and_then(|tone|self.exposure.as_ref().ok_or("missing exposure").and_then(|exposure|tone.probe(&self.device,encoder,exposure,state,(self.config.width,self.config.height))))
                };
                if let Err(reason)=result{state.pending=None;state.valid=false;self.log.log(log_level::WARN,&format!("PostOptics refused: {}",reason));}
            }
        }
        self.gpu_timers.begin(encoder, TimerRegion::TonemapResolve);
        encoder.push_debug_group("wgr_tonemap");
        let mut pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
            label: Some("wgr_tonemap"),
            color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                view: dst,
                depth_slice: None,
                resolve_target: None,
                ops: wgpu::Operations {
                    load: wgpu::LoadOp::Clear(wgpu::Color::BLACK),
                    store: wgpu::StoreOp::Store,
                },
            })],
            depth_stencil_attachment: None,
            timestamp_writes: None,
            occlusion_query_set: None,
            multiview_mask: None,
        });
        self.tonemap.as_ref().expect("tonemap").render(&mut pass);
        drop(pass);
        encoder.pop_debug_group();
        self.gpu_timers.end(encoder, TimerRegion::TonemapResolve);
        // REN-TEMP-001G: velocity debug overlay (WGR_TEMPORAL_DEBUG=1), over the tonemapped
        // frame and under the UI phase, at output resolution.
        let output_size = (self.config.width, self.config.height);
        let dst_format = self.config.format;
        self.temporal.render_debug(
            &self.device,
            &self.queue,
            encoder,
            dst,
            dst_format,
            output_size,
        );
    }

    fn resize(&mut self, width: u32, height: u32) {
        if width == 0 || height == 0 {
            return;
        }
        self.config.width = width;
        self.config.height = height;
        self.surface.configure(&self.device, &self.config);
    }

    // REN-TEMP-001 §6.7: live tuning from the dev panel. Everything here applies on
    // the NEXT frame — targets resize lazily, the DLSS feature recreates on any
    // extent/flag change, and a scale change raises an explicit history reset.
    /// SMK-037: the live soft-particle knobs. Cheap and idempotent -- nothing is created or
    /// destroyed here; `soft_enabled` is read once per frame when the sky camera is derived.
    /// SMK-038: the volumetric-smoke tuning, plus the frame's sun, which the engine already
    /// computes for the legacy per-particle shading -- taking it from there rather than from
    /// the sky UBO keeps one source of truth for "what is lighting the smoke".
    fn set_smoke_volume(&mut self, p: &ffi::WgrSmokeVolumeParams) {
        self.smoke_volume.set_tuning(&smoke_volume::Tuning {
            mode: p.mode,
            cell_size: p.cell_size,
            march_steps: p.march_steps,
            sun_steps: p.sun_steps,
            sun_step_len: p.sun_step_len,
            density: p.density,
            extinction: p.extinction,
            albedo: p.albedo,
            anisotropy: p.anisotropy,
            self_shadow: p.self_shadow,
            jitter: p.jitter,
            scale: p.scale,
        });
        self.smoke_sun_dir = [p.sun_dir[0], p.sun_dir[1], p.sun_dir[2]];
        self.smoke_sun_radiance = [p.sun_radiance[0], p.sun_radiance[1], p.sun_radiance[2]];
        self.smoke_ambient = [p.ambient[0], p.ambient[1], p.ambient[2]];
    }

    fn set_smoke_volume_blobs(&mut self, blobs: &[smoke_volume::SmokeBlob]) {
        self.smoke_volume.set_blobs(&self.queue, blobs);
    }

    fn set_soft_particles(&mut self, enabled: bool, fade_metres: f32) {
        self.soft_enabled = enabled;
        self.soft_fade = fade_metres.clamp(0.05, 20.0);
    }

    fn set_temporal_tuning(&mut self, t: &ffi::WgrTemporalTuning) {
        self.temporal.enabled = t.temporal_on != 0;
        self.dlss_runtime_on = t.dlss_on != 0;
        if self.hdr_enabled {
            let pct = t.render_scale_pct.clamp(50, 200);
            let new_scale = pct as f32 / 100.0;
            if (new_scale - self.render_scale).abs() > 1e-3 {
                self.render_scale = new_scale;
                self.temporal
                    .notify_reset("render-scale change (dev panel)");
            }
        }
        self.temporal.jitter_phase_override = u64::from(t.jitter_phases);
        self.temporal.reactive_sky = t.reactive_sky.clamp(0.0, 1.0);
        self.temporal.reactive_water = t.reactive_water.clamp(0.0, 1.0);
        // mip_bias > 0 is the "auto" sentinel (real biases are <= 0).
        self.gfx3d
            .set_mip_bias_override((t.mip_bias <= 0.0).then(|| t.mip_bias.clamp(-4.0, 0.0)));
        self.dlss_auto_exposure = t.flags & 1 != 0;
        self.dlss_jy_flip = t.flags & 2 != 0;
        self.dlss_mv_render_space = t.flags & 4 != 0;
        self.dlss_mv_flip = t.flags & 8 != 0;
        self.fsr_runtime_on = t.flags & 16 != 0;
        self.dlss_sharpen = t.flags & 32 != 0;
        self.fsr_sharpness = t.fsr_sharpness.clamp(0.0, 2.0);
    }

    // "active", or the sentence saying why not (dev panel, capture metrics).
    fn dlss_status_text(&self) -> &str {
        self.dlss_status.text()
    }

    fn temporal_info(&self) -> ffi::WgrTemporalInfo {
        let r = self.render_size();
        let dlss_route = u32::from(self.ngx_route);
        ffi::WgrTemporalInfo {
            render_width: r.0,
            render_height: r.1,
            output_width: self.config.width,
            output_height: self.config.height,
            temporal_active: u32::from(self.temporal.enabled),
            dlss_route,
            dlss_active: u32::from(self.dlss_active_frame),
            dlss_quality: self.dlss_last_quality,
            jitter_x: self.temporal.jitter_px[0],
            jitter_y: self.temporal.jitter_px[1],
            mip_bias_effective: self.gfx3d.mip_bias_effective(),
            reset_this_frame: u32::from(self.temporal.reset_this_frame()),
            msaa_samples: self.sample_count,
            active_upscaler: self.last_upscaler,
        }
    }

    // REN-TEMP-001B: the resolution the 3D scene renders at. Everything the scene phase owns
    // (HDR colour, depth/normals, GTAO, planar, god rays, clouds, water snapshot, DoF,
    // underwater) sizes to this; the swapchain, post chain, screenshots and UI stay at
    // config.width/height. LOD metrics also stay on config — LOD is about perceived size on
    // the DISPLAY, and letting it follow render scale would coarsen geometry on top of the
    // resolution drop and corrupt every A/B.
    fn render_size(&self) -> (u32, u32) {
        (
            ((self.config.width as f32 * self.render_scale).round() as u32).max(1),
            ((self.config.height as f32 * self.render_scale).round() as u32).max(1),
        )
    }

    // Mirrors the engine's SDL swap interval contract. Presentation is reconfigured
    // immediately, so Options -> Graphics changes take effect without a restart.
    fn set_present_mode(&mut self, interval: i32) -> bool {
        let requested = match interval {
            0 => wgpu::PresentMode::Immediate,
            1 => wgpu::PresentMode::Fifo,
            -1 => {
                if self.present_modes.contains(&wgpu::PresentMode::Mailbox) {
                    wgpu::PresentMode::Mailbox
                } else {
                    wgpu::PresentMode::Immediate
                }
            }
            _ => return false,
        };
        if !self.present_modes.contains(&requested) {
            self.log.log(
                log_level::WARN,
                &format!("wgpu present mode {requested:?} is unsupported"),
            );
            return false;
        }
        if self.config.present_mode != requested {
            self.config.present_mode = requested;
            self.surface.configure(&self.device, &self.config);
        }
        true
    }

    // (Re)allocate the offscreen HDR scene target to match the swapchain size, and
    // repoint the tonemap resolve at the new view. No-op when the HDR path is off or
    // the size is unchanged. Mirrors Gfx3d::ensure_depth.
    // REN-TEMP-001N: DLAA = the DLSS backend running at render == output (pure temporal
    // AA, no resolution win). True when the route is up and both switches are on; the
    // failure latch lives inside the backend, so after an NGX failure this stays true
    // and the frame degrades to a redundant bilinear copy — turn DLSS off (panel or
    // WGR_DLSS=0) to reclaim the byte-identical native path.
    fn dlaa_active(&self) -> bool {
        self.ngx_route && self.dlss_runtime_on && self.temporal.enabled
    }

    fn ensure_hdr(&mut self, width: u32, height: u32) {
        if !self.hdr_enabled || width == 0 || height == 0 {
            return;
        }
        // Early-out only when BOTH sizes still match: the scene size (hdr_size) and, under
        // render scale, the output size the upscale target was built for. Rounding can keep
        // the render size identical across an output resize, so the scene size alone is not
        // enough to prove the upscale target is current.
        let out = (self.config.width, self.config.height);
        // REN-TEMP-001N: at 100% scale the upscale target normally does not exist — except
        // for DLAA, where DLSS runs at render == output purely as a temporal AA pass and
        // still needs its own storage-image output to write into.
        let want_upscaled = (width, height) != out || self.dlaa_active();
        let upscaled_current = match self.hdr_upscaled.as_ref() {
            Some((t, _)) => want_upscaled && (t.width(), t.height()) == out,
            None => !want_upscaled,
        };
        if self.hdr.is_some() && self.hdr_size == (width, height) && upscaled_current {
            return;
        }
        let msaa = self.sample_count > 1;
        // The scene colour target. MSAA: RENDER_ATTACHMENT only — it is resolved, never sampled.
        // 1x: also TEXTURE_BINDING, since the tonemap/bloom/exposure sample it directly.
        let usage = if msaa {
            wgpu::TextureUsages::RENDER_ATTACHMENT
        } else {
            wgpu::TextureUsages::RENDER_ATTACHMENT
                | wgpu::TextureUsages::TEXTURE_BINDING
                | wgpu::TextureUsages::COPY_SRC
        };
        let texture = self.device.create_texture(&wgpu::TextureDescriptor {
            label: Some("wgr_hdr_target"),
            size: wgpu::Extent3d {
                width,
                height,
                depth_or_array_layers: 1,
            },
            mip_level_count: 1,
            sample_count: self.sample_count,
            dimension: wgpu::TextureDimension::D2,
            format: HDR_FORMAT,
            usage,
            view_formats: &[],
        });
        let view = texture.create_view(&wgpu::TextureViewDescriptor::default());
        // MSAA: a single-sample resolve target the scene colour is resolved into (run_tonemap
        // records the resolve); the post-processing chain samples it. 1x: none, and the chain
        // samples the scene target itself.
        let resolve = msaa.then(|| {
            let t = self.device.create_texture(&wgpu::TextureDescriptor {
                label: Some("wgr_hdr_resolve"),
                size: wgpu::Extent3d {
                    width,
                    height,
                    depth_or_array_layers: 1,
                },
                mip_level_count: 1,
                sample_count: 1,
                dimension: wgpu::TextureDimension::D2,
                format: HDR_FORMAT,
                usage: wgpu::TextureUsages::RENDER_ATTACHMENT
                    | wgpu::TextureUsages::TEXTURE_BINDING,
                view_formats: &[],
            });
            let v = t.create_view(&wgpu::TextureViewDescriptor::default());
            (t, v)
        });
        let water_scene_texture = self.device.create_texture(&wgpu::TextureDescriptor {
            label: Some("wgr_water_scene_snapshot"),
            size: wgpu::Extent3d {
                width,
                height,
                depth_or_array_layers: 1,
            },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: HDR_FORMAT,
            usage: wgpu::TextureUsages::RENDER_ATTACHMENT
                | wgpu::TextureUsages::TEXTURE_BINDING
                | wgpu::TextureUsages::COPY_DST,
            view_formats: &[],
        });
        let water_scene_view =
            water_scene_texture.create_view(&wgpu::TextureViewDescriptor::default());
        // REN-TEMP-001B: when the scene renders below output resolution, the post chain
        // (bloom/exposure/tonemap) reads an output-resolution target the scene is upscaled
        // into, not the render-resolution resolve. At 100% no such target exists and the
        // chain reads the resolve directly — the pre-split behaviour, byte for byte.
        let (out_w, out_h) = (self.config.width, self.config.height);
        self.hdr_upscaled = want_upscaled.then(|| {
            let t = self.device.create_texture(&wgpu::TextureDescriptor {
                label: Some("wgr_hdr_upscaled"),
                size: wgpu::Extent3d {
                    width: out_w,
                    height: out_h,
                    depth_or_array_layers: 1,
                },
                mip_level_count: 1,
                sample_count: 1,
                dimension: wgpu::TextureDimension::D2,
                format: HDR_FORMAT,
                // STORAGE: DLSS writes its output as a storage image (REN-TEMP-001M);
                // COPY_SRC: the optional RCAS sharpen pass copies it aside first.
                // Both harmless on the plain bilinear path.
                usage: wgpu::TextureUsages::RENDER_ATTACHMENT
                    | wgpu::TextureUsages::TEXTURE_BINDING
                    | wgpu::TextureUsages::STORAGE_BINDING
                    | wgpu::TextureUsages::COPY_SRC,
                view_formats: &[],
            });
            let v = t.create_view(&wgpu::TextureViewDescriptor::default());
            (t, v)
        });
        // The single-sample view the post-processing chain reads (resolve target under MSAA,
        // else the scene target directly).
        let sample_view = resolve.as_ref().map(|(_, v)| v).unwrap_or(&view).clone();
        // What bloom/exposure/tonemap bind to: the upscaled output-res target when scaling,
        // else the render-res resolve. They must also SIZE to the output resolution — the
        // pyramid bases derive from the texture they read.
        let post_view = self
            .hdr_upscaled
            .as_ref()
            .map(|(_, v)| v.clone())
            .unwrap_or_else(|| sample_view.clone());
        let (post_w, post_h) = if self.hdr_upscaled.is_some() {
            (out_w, out_h)
        } else {
            (width, height)
        };
        // Rebuild the bloom pyramid for the new size, then point the resolve at both
        // the HDR target and the bloom mip0. A 1x1 fallback keeps set_source valid if
        // the pyramid somehow has no mips.
        if let Some(bloom) = self.bloom.as_mut() {
            bloom.resize(&self.device, post_w, post_h, HDR_FORMAT, &post_view);
        }
        if let Some(exposure) = self.exposure.as_mut() {
            exposure.resize(&self.device, post_w, post_h, &post_view);
        }
        if let Some(tonemap) = self.tonemap.as_mut() {
            let bloom_view = self
                .bloom
                .as_ref()
                .and_then(|b| b.view())
                .unwrap_or(&post_view);
            let scale_view = self
                .exposure
                .as_ref()
                .map(|e| e.scale_view())
                .unwrap_or(&post_view);
            tonemap.set_source(&self.device, &post_view, bloom_view, scale_view);
        }
        self.hdr = Some((texture, view));
        self.hdr_resolve = resolve;
        self.water_scene = Some((water_scene_texture, water_scene_view));
        self.hdr_size = (width, height);
        // ensure_hdr rebinds every HDR postprocess stage to the normal scene source.
        self.post_source_underwater = false;
        self.post_source_dof = false;
    }

    fn ensure_underwater_target(&mut self, width: u32, height: u32) {
        if self.underwater_target.is_some() && self.underwater_size == (width, height) {
            return;
        }
        let texture = self.device.create_texture(&wgpu::TextureDescriptor {
            label: Some("wgr_underwater_target"),
            size: wgpu::Extent3d {
                width,
                height,
                depth_or_array_layers: 1,
            },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: if self.hdr_enabled {
                HDR_FORMAT
            } else {
                self.config.format
            },
            usage: wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::TEXTURE_BINDING,
            view_formats: &[],
        });
        let view = texture.create_view(&wgpu::TextureViewDescriptor::default());
        self.underwater_target = Some((texture, view));
        self.underwater_size = (width, height);
    }

    fn ensure_dof_target(&mut self, width: u32, height: u32) {
        if self.dof_target.is_some() && self.dof_size == (width, height) {
            return;
        }
        let texture = self.device.create_texture(&wgpu::TextureDescriptor {
            label: Some("wgr_dof_target"),
            size: wgpu::Extent3d {
                width,
                height,
                depth_or_array_layers: 1,
            },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: if self.hdr_enabled {
                HDR_FORMAT
            } else {
                self.config.format
            },
            usage: wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::TEXTURE_BINDING,
            view_formats: &[],
        });
        let view = texture.create_view(&wgpu::TextureViewDescriptor::default());
        self.dof_target = Some((texture, view));
        self.dof_size = (width, height);
    }

    /// Runs between the god-ray composite and bloom, so an out-of-focus highlight blooms as a
    /// disc. Returns the view the rest of the chain should read: unchanged when DoF is off, which
    /// is the whole of the "costs nothing when unused" claim.
    fn run_depth_of_field(
        &mut self,
        encoder: &mut wgpu::CommandEncoder,
        source: &wgpu::TextureView,
    ) -> (wgpu::TextureView, bool) {
        if !self.dof_settings.enabled {
            return (source.clone(), false);
        }
        // The gather reads a SINGLE-SAMPLE depth, which under MSAA is the far-reduce resolve --
        // the same one the god rays march. Reusing the frame flag means a frame that runs both
        // pays for the resolve once.
        if !self.frame_depth_final_resolved {
            self.ensure_far_depth_resolve(encoder);
            self.frame_depth_final_resolved = true;
        }
        let Some(depth) = self.gfx3d.water_depth_view().cloned() else {
            // A feature that silently does nothing is the worst kind. Say it once.
            if !self.dof_depth_warned {
                self.dof_depth_warned = true;
                self.log.log(
                    log_level::WARN,
                    "depth of field: enabled, but there is no single-sample depth view this frame --                      the pass is skipped and the image is unchanged",
                );
            }
            return (source.clone(), false);
        };
        if self.dof.is_none() {
            self.log
                .log(log_level::INFO, "depth of field: pass created and running");
            let format = if self.hdr_enabled {
                HDR_FORMAT
            } else {
                self.config.format
            };
            self.dof = Some(DepthOfField::new(&self.device, format));
        }
        let (rw, rh) = self.render_size();
        self.ensure_dof_target(rw, rh);
        let Some((_, target)) = self.dof_target.as_ref().map(|(t, v)| (t, v.clone())) else {
            return (source.clone(), false);
        };
        let settings = self.dof_settings;
        // CoC scaling is in pixels of the pass' own target, which is render-resolution.
        let height = rh;
        if let Some(dof) = self.dof.as_ref() {
            dof.upload(&self.queue, &settings, height);
            dof.render(&self.device, encoder, source, &depth, &target);
        }
        (target, true)
    }

    fn ensure_planar_target(&mut self) {
        // Divisor, not a fixed half: it tracks how much water is on screen (see PlanarConfig).
        // Based on the RENDER resolution — the reflection is sampled by the render-res water pass.
        let div = self.planar_div.max(1);
        let (rw, rh) = self.render_size();
        let size = ((rw.max(2) + div - 1) / div, (rh.max(2) + div - 1) / div);
        if self.planar.as_ref().is_some_and(|p| p.size == size) {
            return;
        }
        let mip_count = PlanarMips::mip_count(size.0, size.1);
        let color = self.device.create_texture(&wgpu::TextureDescriptor {
            label: Some("wgr_planar_color_attachment"),
            size: wgpu::Extent3d {
                width: size.0,
                height: size.1,
                depth_or_array_layers: 1,
            },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: HDR_FORMAT,
            // Keep the frequently written target attachment-only. Copying it into the sampled
            // mip chain after the reflection preserves framebuffer compression on the 1x path;
            // rendering directly into a mipped sampled texture was measurably pathological.
            usage: wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::COPY_SRC,
            view_formats: &[],
        });
        let sampled = self.device.create_texture(&wgpu::TextureDescriptor {
            label: Some("wgr_planar_color"),
            size: wgpu::Extent3d {
                width: size.0,
                height: size.1,
                depth_or_array_layers: 1,
            },
            mip_level_count: mip_count,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: HDR_FORMAT,
            usage: wgpu::TextureUsages::RENDER_ATTACHMENT
                | wgpu::TextureUsages::TEXTURE_BINDING
                | wgpu::TextureUsages::COPY_DST,
            view_formats: &[],
        });
        let depth = self.device.create_texture(&wgpu::TextureDescriptor {
            label: Some("wgr_planar_depth"),
            size: wgpu::Extent3d {
                width: size.0,
                height: size.1,
                depth_or_array_layers: 1,
            },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: crate::gfx3d::depth_format(&self.device),
            usage: wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::TEXTURE_BINDING,
            view_formats: &[],
        });
        let depth_view = depth.create_view(&Default::default());
        let depth_aspect = depth.create_view(&wgpu::TextureViewDescriptor {
            label: Some("wgr_planar_depth_sample"),
            aspect: wgpu::TextureAspect::DepthOnly,
            ..Default::default()
        });
        let depth_sample_view = depth_aspect;
        let mip_views: Vec<_> = (0..mip_count)
            .map(|level| {
                sampled.create_view(&wgpu::TextureViewDescriptor {
                    label: Some("wgr_planar_color_mip"),
                    base_mip_level: level,
                    mip_level_count: Some(1),
                    dimension: Some(wgpu::TextureViewDimension::D2),
                    ..Default::default()
                })
            })
            .collect();
        let color_view = color.create_view(&Default::default());
        self.planar = Some(PlanarTarget {
            color_view,
            _color: color,
            sampled_view: sampled.create_view(&Default::default()),
            _sampled: sampled,
            mip_views,
            depth_view,
            depth_sample_view,
            _depth: depth,
            size,
        });
    }

    // `None` = skip this frame
    fn acquire(&mut self, diagnostic_call_token: Option<u64>) -> Result<Option<wgpu::SurfaceTexture>, String> {
        // Existing caller-owned opt-in supplies the token; no second flag/config.
        // Legacy calls (None) take no new clock/counter/log path. This is CPU
        // acquisition scope, not GPU completion or completed-frame evidence.
        let diagnostic_start = diagnostic_call_token.map(|_| std::time::Instant::now());
        let (device_loss, uncaptured_error) = self.runtime_diagnostics.take_messages();
        if let Some(message) = device_loss {
            self.log.log(log_level::ERROR, &message);
        }
        if let Some(message) = uncaptured_error {
            self.log.log(log_level::ERROR, &message);
        }
        // RFG-082: the geometry-pool and storage-array rings were drained once, at
        // start-up, so a grow or retire that happened a minute later was never written
        // anywhere -- which is how "no grow events" was read as "the pool is not the
        // problem" while the pool was being re-created into a full heap. Drained here,
        // every frame, they land next to the error they explain.
        if let Some(line) = crate::gfx3d::pool::geometry_pool_events_log_line() {
            self.log.log(log_level::INFO, &line);
        }
        if let Some(line) = crate::gfx3d::storage_array_log_line() {
            self.log.log(log_level::INFO, &line);
        }
        use wgpu::CurrentSurfaceTexture as Cst;
        let preamble_ms = diagnostic_start.map(|start| start.elapsed().as_secs_f64() * 1.0e3);
        let surface_start = diagnostic_call_token.map(|_| std::time::Instant::now());
        let acquired = self.surface.get_current_texture();
        if let (Some(call_token), Some(preamble_ms), Some(surface_start)) =
            (diagnostic_call_token, preamble_ms, surface_start)
        {
            let surface_ms = surface_start.elapsed().as_secs_f64() * 1.0e3;
            let total_ms = preamble_ms + surface_ms;
            if total_ms >= 10.0 {
                use std::sync::atomic::{AtomicU32, Ordering};
                static ROWS: AtomicU32 = AtomicU32::new(0);
                let row = ROWS.fetch_update(Ordering::Relaxed, Ordering::Relaxed,
                    |n| if n < 65 { Some(n + 1) } else { None }).unwrap_or(65);
                if row < 64 {
                    let status = match &acquired {
                        Cst::Success(_) => "success", Cst::Suboptimal(_) => "suboptimal",
                        Cst::Outdated => "outdated", Cst::Lost => "lost",
                        Cst::Timeout => "timeout", Cst::Occluded => "occluded",
                        Cst::Validation => "validation",
                    };
                    self.log.log(log_level::INFO, &format!(
                        "acquire CPU split: callToken={call_token} surfaceStatus={status} preambleCpuMs={preamble_ms:.3} surfaceAcquireCpuMs={surface_ms:.3} measuredTotalCpuMs={total_ms:.3} row={} limit=64 thresholdMs=10 scope=acquisitionOnlyBeforeResultHandling",
                        row + 1));
                } else if row == 64 {
                    self.log.log(log_level::INFO,
                        "acquire CPU split truncated: limit=64 thresholdMs=10; observed diagnostic calls only, no GPU completion claim");
                }
            }
        }
        match acquired {
            Cst::Success(t) | Cst::Suboptimal(t) => Ok(Some(t)),
            Cst::Outdated => {
                self.log.log(
                    log_level::WARN,
                    "wgpu surface outdated; reconfiguring and skipping frame",
                );
                self.surface.configure(&self.device, &self.config);
                Ok(None)
            }
            Cst::Lost => {
                self.log.log(
                    log_level::ERROR,
                    "wgpu surface lost; reconfiguring once (device recovery requires restart)",
                );
                self.surface.configure(&self.device, &self.config);
                Ok(None)
            }
            Cst::Timeout => {
                self.log
                    .log(log_level::DEBUG, "wgpu surface timeout; skipping frame");
                Ok(None)
            }
            Cst::Occluded => {
                self.log.log(
                    log_level::DEBUG,
                    "wgpu surface occluded/minimized; skipping frame",
                );
                Ok(None)
            }
            Cst::Validation => Err("get_current_texture: validation error".to_string()),
        }
    }

    #[allow(clippy::too_many_arguments)]
    fn render_frame(
        &mut self,
        clear: [f32; 4],
        fog: [f32; 3],
        cameras: &[WgrCamera],
        draws3d: &[WgrDraw3D],
        verts: &[WgrVertex2D],
        batches: &[WgrDraw2DBatch],
        cmds: &[WgrCmd],
        palette: &[WgrMat4],
        shadow: &WgrShadowPass,
        shadow_casters: &[WgrShadowCaster],
        overlay_verts: &[WgrOverlayVertex],
        overlay_indices: &[u16],
        overlay_draws: &[WgrOverlayDraw],
        terrain_nodes: &[WgrTerrainNode],
        terrain_batches: &[WgrTerrainBatch],
        lights: &[WgrLight],
        water_nodes: &[WgrWaterNode],
        water_batches: &[WgrWaterBatch],
        grass_batches: &[WgrGrassBatch],
        mut call_timings: Option<&mut crate::gpu_timers::RenderCallCpuTimings>,
        main_count_request: Option<(u64, u32, u64)>,
    ) -> Result<bool, String> {
        if let Some(probe)=self.post_optics_probe.as_mut(){probe.begin();}
        if let Some(tuple)=self.main_camera_tuple.as_mut(){tuple.begin();} // Failed calls invalidate old completion.
        // The GPU-resident (retained / GPU-driven) world objects are drawn by every presented frame unless
        // suppressed, and the suppress latch is pushed only by World::Simulate. Frames presented from anywhere
        // else -- the progress splash while a world unloads or switches, the exit path -- inherited the last
        // world frame's `false` and drew the whole resident set from the stale camera over the black splash
        // background: the island "goes black and all objects float" for a moment (owner report). A frame that
        // submitted no terrain and no 3D draws has no world in it, so its resident objects stay hidden too.
        let suppress_world_objects = self.suppress_world_objects || (terrain_batches.is_empty() && draws3d.is_empty());
        self.gfx3d.begin_geometry_pass_facts(shadow);
        let setup_t0 = std::time::Instant::now();
        // REN-THR-009 — before anything this frame reads `bake_bind_cache` or allocates
        // from the pool. This is the frame boundary a deferred mesh destroy waits for.
        self.drain_deferred_mesh_destroys();
        // TW-WATER W1 — a Water-tab backend change takes effect here, at the frame boundary,
        // before anything this frame reads the water surface.
        // TW-WATER W7h: the backend-switch soak (plan §8): `WGR_WATER_SOAK=<frames>[,<switches>]`
        // flips the backend every <frames> frames, <switches> times (default 20), then hands the
        // choice back; every switch logs the backend's GPU allocations and live handle counts
        if let Some((every, total)) = water_soak() {
            let f = WATER_SOAK_FRAME.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
            let k = f / every;
            let pick = if k == 0 || k > total as u64 {
                None
            } else if k % 2 == 1 {
                Some(crate::water_backend::WaterBackendKind::Tidewater)
            } else {
                Some(crate::water_backend::WaterBackendKind::CurrentOp)
            };
            self.water.set_soak(pick);
        }
        let switches_before = self.water.switch_count();
        if self.water.needs_terrain_heights() {
            if let Some((heights, params)) = self.terrain.shore_height_source() {
                self.water.set_terrain_heights(heights, params);
            }
        }
        self.water
            .apply_pending_switch(&self.device, &self.queue, &self.log);
        if self.water.switch_count() != switches_before {
            let c = self.device.get_internal_counters().hal;
            let mb = ((c.buffer_memory.read().max(0) as u64).saturating_add(c.texture_memory.read().max(0) as u64)) as f64 / (1024.0 * 1024.0);
            self.log.log(
                log_level::INFO,
                &format!(
                    "W7 switch #{} to {}: backend allocations {:.1} MB | buffers {} textures {} views {} bind groups {} pipelines {} + {} shader modules {}",
                    self.water.switch_count(),
                    self.water.kind().label(),
                    mb,
                    c.buffers.read(),
                    c.textures.read(),
                    c.texture_views.read(),
                    c.bind_groups.read(),
                    c.render_pipelines.read(),
                    c.compute_pipelines.read(),
                    c.shader_modules.read()
                ),
            );
        }
        let screen = glam::Vec2::new(self.config.width as f32, self.config.height as f32);
        self.gfx2d
            .prepare(&self.device, &self.queue, screen, fog, verts);
        self.gfx2d
            .prepare_overlay(&self.device, &self.queue, overlay_verts, overlay_indices);
        self.gfx3d
            .ensure_depth(&self.device, self.render_size().0, self.render_size().1);
        // The post-tonemap UI composites to the swapchain, so its 1x depth is OUTPUT-sized.
        self.gfx3d
            .ensure_ui_depth(&self.device, self.config.width, self.config.height);
        // REN-TEMP-001D: render-scale-aware mip bias for every material sample this frame.
        self.gfx3d.set_render_scale_lane(self.render_scale);
        // AST-012A: size the mip-feedback buffer to the bindless array and step the rotation.
        // Straight after set_render_scale_lane, because it writes the two lanes that setter
        // zeroes, and well before the cameras are uploaded — the pair travels in the camera
        // uniform, so a frame that published it late would have the shader test last frame's
        // group index.
        let object_cap = self.textures.object_cap();
        self.gfx3d.ensure_mip_feedback(&self.device, object_cap);
        // Rebuild the bindless object-texture array if any texture was created/destroyed
        // since last frame (no-op on churn-free frames), so this frame's draws index a
        // current array.
        self.textures.ensure_bindless(&self.device);
        // Compute skin bake (docs/compute-skin-bake-plan.md): plan + upload BEFORE both
        // prepare_shadows and prepare so those pack an identity world for every baked
        // draw/caster. Spans both draws and casters (one bake per skinned mesh+pose).
        self.gfx3d
            .prepare_skin_bake(&self.device, &self.queue, draws3d, shadow_casters, palette);
        // Shadows first: prepare() binds the frame's final shadow target into
        // the camera group.
        self.gfx3d.prepare_shadows(
            &self.device,
            &self.queue,
            shadow,
            shadow_casters,
            palette,
            &self.grass,
        );
        // The terrain owns the sun-shadow mask; lend its view + world->UV mapping to
        // the shared camera group(0) so lit meshes receive terrain shadow too.
        let shadow_mask_view = self.terrain.shadow_mask_view();
        let shadow_mask_gen = self.terrain.shadow_gen();
        let mut shadow_mapping = self.terrain.shadow_mapping();
        // shadow_mapping.cloud_shadow is filled in below, once main_scene_cam is known --
        // see there for why it cannot be done here.
        // Lend the terrain heightmap to the mesh conform group (group 4) so object
        // vertex shaders conform ClipLand vegetation to SurfaceY without CPU rewrites.
        let heightmap_view = self.terrain.heightmap_view();
        let heightmap_gen = self.terrain.heightmap_gen();
        let conform_params = self.terrain.conform_params();
        // Sky-visibility (sky-view factor) mask, lent to the shared camera group(0) at binding 10 so
        // terrain/objects/water modulate ambient by terrain sky occlusion.
        let skyvis_view = self.terrain.skyvis_view();
        // Bucket the frame's 3D draws into instanced groups (see Gfx3d::plan_3d). The
        // plan's `order` drives the storage-array pack order in prepare(); its `ops`
        // replace the raw command stream in the replay loop below.
        let mut plan = self.gfx3d.plan_3d(cmds, draws3d);
        // Stage 2: turn the plan's instanceable buckets into CPU-built indirect draws over
        // the geometry pool (no-op when indirect is off). Tags each eligible op with its
        // args-buffer offset for the replay below.
        self.gfx3d
            .build_indirect(&self.device, &self.queue, draws3d, &mut plan.ops);
        let main_selection = main_camera_tuple::select(
            terrain_batches.first().map(|b| b.camera as usize), draws3d.first().map(|d| d.camera as usize),
            water_batches.first().map(|b| b.camera as usize), grass_batches.first().map(|b| b.camera as usize));
        let main_scene_cam = main_selection.index; // Same existing front-entry priority, not a new camera policy.
        if let Some(camera) = cameras.get(main_scene_cam) {
            self.rain_water.prepare_medium(&self.device,&self.queue,self.terrain.rain_water_medium_resources());
            self.rain_water.prepare(&self.device, &self.queue, camera, &self.terrain.heightmap_view(),
                self.terrain.heightmap_gen(), self.terrain.conform_params());
        } else {
            self.rain_water.invalidate_frame(&self.queue);
        }
        if self.main_camera_tuple.is_some() {
            let render=self.render_size(); let output=(self.config.width,self.config.height);
            self.main_camera_tuple.as_mut().unwrap().capture(main_selection,cameras,render,output);
        }
        // The Sky pass owns the cloud map and its texel snapping, so it is the only thing that
        // knows where the map landed this frame. Publishing the SAME numbers the compute pass
        // used is the point: two independently derived mappings would disagree by a texel and
        // the shadows would sit slightly off from the clouds casting them.
        //
        // ORDER IS THE WHOLE FIX. Sky::upload runs several hundred lines below, AFTER
        // gfx3d::prepare has already baked this struct into the camera UBO -- so reading the
        // mapping before it handed every sampler the PREVIOUS frame's square while
        // cs_cloud_shadow filled the map with THIS frame's. Snapping hid that at a standstill
        // (the square does not move) and showcased it in flight: on each frame the camera
        // crossed an 8 m snap line the two squares differed by exactly one texel, the whole
        // shadow field translated 8 m, and it came back the next frame. That is the flicker.
        // Fixing the square HERE -- from the same camera the sky pass resolves to, so the two
        // cannot disagree -- makes the frame self-consistent, and Sky::upload now consumes
        // this square rather than deriving another.
        if let Some(cam) = cameras.get(main_scene_cam) {
            // CLD-021: how far the map has to reach. fog_start + 1/fog_inv_range is the scene fog
            // MAX, which the engine also uses as the camera far plane and the terrain-grid cull
            // distance (see apply_fog in frame.wgsl) -- i.e. the furthest ground that can exist on
            // screen at all, and therefore exactly the radius the cloud shadow map has to cover.
            // 0 when this camera has no usable fog range; the sky then keeps its current span.
            let draw_distance = if cam.params.fog_inv_range > 0.0 {
                cam.params.fog_start + 1.0 / cam.params.fog_inv_range
            } else {
                0.0
            };
            let span_change = self
                .sky
                .begin_cloud_shadow_frame(cam.cam_pos, draw_distance);
            if let Some(line) = span_change {
                self.log.log(log_level::INFO, &line);
            }
        }
        shadow_mapping.cloud_shadow =
            glam::Vec4::from_array(self.sky.cloud_shadow_mapping_current());
        // Append a private reflected camera to the GPU upload only. It never crosses the
        // C++ ABI and has no cascade data: main-camera shadow matrices are not valid after
        // a mirror transform.
        let water_camera = water_batches
            .first()
            .map(|batch| batch.camera as usize)
            .unwrap_or(main_scene_cam);
        let planar_sea = self.water.underwater_params().map(|p| p.0);
        self.water_camera_slot = (!water_batches.is_empty()).then_some(water_camera);
        let ablate = *ABLATE.get_or_init(Ablate::from_env);
        // How much of the screen the water surface actually covers, 0..1. The planar reflection
        // re-renders terrain, the retained object set and a cloud march from a mirrored camera —
        // measured at 1080p as 7.94 ms, 23% of the frame — and NONE of that cost scaled with how
        // much water was on screen. At the owner's Stratis camera the sea is a distant strip on a
        // hillside view and the frame was paying full price for it.
        //
        // Estimated on the CPU from the water nodes rather than with an occlusion query: the
        // nodes are a non-overlapping CDLOD quadtree of flat quads at sea level, so projecting
        // their corners and summing clipped screen areas is a real coverage number, it costs
        // microseconds, and it is available BEFORE the reflection is recorded. An occlusion query
        // would be exact but only answers one frame late, which is the wrong shape for a gate that
        // decides whether to render at all.
        let cpu_water_coverage = planar_sea
            .and_then(|sea| cameras.get(water_camera).map(|cam| (sea, cam)))
            .map(|(sea, cam)| screen_coverage_of_water(water_nodes, cam, sea))
            .unwrap_or(0.0);
        // The quadtree estimate intentionally errs toward ON, but on large worlds its sea plane
        // extends below dry land and can report 100% for a frame with zero visible water. Once an
        // asynchronous depth-tested water query is available, it is the tighter upper bound.
        let water_coverage = self
            .water_visibility
            .coverage(
                self.render_size().0,
                self.render_size().1,
                self.sample_count,
            )
            .map(|visible| visible.min(cpu_water_coverage))
            .unwrap_or(cpu_water_coverage);
        let planar_cfg = *PLANAR_CFG.get_or_init(PlanarConfig::from_env);
        // Hysteresis-free by construction: the weight fades to 0 across a narrow band just above
        // `cut` BEFORE the pass is skipped below it, so the handover to the environment-map
        // reflection has already completed by the time the planar target stops being updated. A
        // bare threshold on a binary switch would pop, and the popping edge would be exactly
        // where a shoreline crosses the horizon — the most visible place it could happen.
        let planar_fade = planar_cfg.fade_for(water_coverage);
        let planar_active = planar_sea.is_some()
            && !ablate.planar
            && planar_fade > 0.0
            && !self.water.low_quality()
            && self.water.uses_planar_reflection()
            && !water_batches.is_empty()
            && cameras
                .get(water_camera)
                .is_some_and(|c| c.cam_pos[1] >= planar_sea.unwrap());
        self.gfx3d.geometry_pass_reflection_configured(planar_active);
        // Sticky resolution selection. `divisor_for` is a continuous function of a continuously
        // moving camera, so committing it directly would realloc the target on any frame the
        // coverage crossed a rounding boundary. Require PLANAR_DIV_HOLD consecutive frames of
        // agreement first.
        if planar_active {
            let want = planar_cfg.divisor_for(water_coverage);
            // There is no previous image to preserve on first use, so start at the correct
            // coverage-derived resolution immediately. Applying the normal 20-frame hold here
            // made low-coverage A3/DayZ cameras render an unnecessarily large reflection during
            // their entire warm-up, exactly when streaming pressure is already highest.
            if self.planar.is_none() {
                self.planar_div = want;
                self.planar_div_want = want;
                self.planar_div_hold = 0;
            } else if want == self.planar_div {
                self.planar_div_hold = 0;
            } else if want == self.planar_div_want {
                self.planar_div_hold += 1;
                if self.planar_div_hold >= PLANAR_DIV_HOLD {
                    self.planar_div = want;
                    self.planar_div_hold = 0;
                }
            } else {
                self.planar_div_want = want;
                self.planar_div_hold = 0;
            }
        }
        // One line per transition, not per frame: this is the number that decides whether a
        // quarter of the frame happens, so it has to be readable from a benchmark log without a
        // debugger — and unreadable-by-default, because a per-frame line would swamp the log.
        let coverage_bucket = (water_coverage * 200.0) as u32;
        if self.planar_dbg_last != Some((planar_active, coverage_bucket)) {
            self.planar_dbg_last = Some((planar_active, coverage_bucket));
            eprintln!(
                "[wgr] planar reflection {}: water covers {:.2}% of screen (cut {:.2}% full {:.2}%), fade {:.2}, div {}",
                if planar_active { "ON" } else { "SKIPPED" },
                water_coverage * 100.0,
                planar_cfg.cut * 100.0,
                planar_cfg.full * 100.0,
                planar_fade,
                self.planar_div,
            );
        }
        // DZ-005 — hand the object paths the engine's own water clock and this world's river
        // flow map, for the reflective (CalmWater) sections. Deliberately the SAME `time` the
        // ocean's FFT runs on rather than a wall clock private to gfx3d: it is `Glob.time`, so
        // a paused simulation freezes the pond exactly as it freezes the sea, and two runs of
        // the same test mission ripple identically at the same frame.
        self.gfx3d.set_water_anim(
            self.water
                .underwater_params()
                .map(|(_, time, _)| time)
                .unwrap_or(0.0),
            self.water_flow_slot,
            self.water_flow_strength,
        );
        // Deposit belongs to the whole mission; roof admission uses actual surface height
        // and cover in the object shader, never the terrain shelter/track deficit texture.
        if let Some(line) = self.gfx3d.set_snow_surface(self.terrain.snow_surface_params()) {
            self.log.log(log_level::INFO, &line);
        }
        self.gfx3d.set_ground_weather(self.terrain.ground_weather_params());
        let mut prepared_cameras = cameras.to_vec();
        // REN-TEMP-001E: temporal camera state + jitter. The UNJITTERED main camera is
        // snapshotted first (velocity and history math use it), then the jitter is folded
        // into the projection HERE — the single camera-ingest point — so the prepass,
        // colour, water, cloud and GPU-cull consumers all rasterise with the same
        // sub-pixel offset, while C++ (gameplay, culling truth, picking) and the shadow
        // cascade VPs never see a jittered matrix. The planar-reflection camera below is
        // copied from the ORIGINAL slice and stays unjittered on purpose (it feeds a
        // mip-blurred reflection; jitter there is pure shimmer).
        if let Some(cam) = prepared_cameras.get_mut(main_scene_cam) {
            self.temporal.set_output_width(self.config.width);
            self.temporal.begin_frame(cam, self.render_size());
            self.temporal.apply_jitter(&mut cam.proj);
        }
        // REN-TEMP-001H: absolute worlds of this frame's identified rigid draws, for the
        // object-velocity subset at tonemap time (and next frame's previous worlds).
        self.temporal.ingest_draws(draws3d);
        self.dlss_active_frame = false;
        self.dlss_status.begin_frame();
        // The "active upscaler" readout: default to native every frame; the upscale
        // chain overwrites it when it actually runs. Without this the panel kept
        // showing the LAST backend after switching to native (owner report - no
        // feedback that "everything off" took effect).
        self.last_upscaler = upscaler::ID_NATIVE;
        // REN-TEMP-001I: (object, mesh) -> baked base_vertex — the cross-frame identity
        // palette slots cannot provide. prepare_skin_bake already ran above.
        if self.temporal.enabled {
            let bases = self.gfx3d.collect_skin_bases(draws3d);
            self.temporal.ingest_skin_bases(bases);
        }
        let mut reflected_vp = [0.0f32; 16];
        let reflected_camera = if planar_active {
            let sea = planar_sea.unwrap();
            let mut reflected = cameras[water_camera];
            reflected.cam_pos[1] = 2.0 * sea - reflected.cam_pos[1];
            reflected.shadow = unsafe { std::mem::zeroed() };
            let mirror = glam::Mat4::from_scale(glam::Vec3::new(1.0, -1.0, 1.0));
            let view = glam::Mat4::from_cols_array(&reflected.view);
            // The sky/cloud projection convention is camera-relative and includes its
            // own vertical screen mapping, so retain the matched reflected basis used
            // by the reflected terrain and cloud passes.
            reflected.view = (mirror * view * mirror).to_cols_array();
            // WIDEN the reflected frustum. The reflected camera otherwise inherits the main
            // camera's projection unchanged, so it renders exactly the screen's field of view --
            // but a reflection needs directions the screen never looks at. At grazing angles the
            // reflected ray leaves that frustum, the lookup falls off the edge of the reflection
            // target, and the cloud reflection ends in a visible line across the water.
            //
            // Padding trades angular resolution for coverage: the same target now spans a wider
            // cone, so the reflection is slightly softer and the edge moves outside the view.
            // Softer is the right side of that trade here -- the planar reflection is already
            // sampled from a mip pyramid on purpose, because a razor-sharp second sky painted on
            // the sea is the artifact this system fought last time.
            //
            // Scaling the projection's x/y scale terms IS the FOV widening: for a perspective
            // matrix m00 = 1/(aspect*tan(fov/2)) and m11 = 1/tan(fov/2), so dividing both by the
            // pad multiplies tan(fov/2) by it. Applied BEFORE full_vp so the matrix the water
            // shader projects with is the one the reflection was actually rendered with; a
            // mismatch there would slide the whole reflection instead of widening it.
            let pad = self.planar_reflection_pad.clamp(1.0, 3.0);
            if pad > 1.0 {
                reflected.proj[0] /= pad;
                reflected.proj[5] /= pad;
            }
            let full_vp = glam::Mat4::from_cols_array(&reflected.proj)
                * glam::Mat4::from_cols_array(&reflected.view)
                * glam::Mat4::from_translation(-glam::Vec3::from_array([
                    reflected.cam_pos[0],
                    reflected.cam_pos[1],
                    reflected.cam_pos[2],
                ]));
            reflected_vp = full_vp.to_cols_array();
            prepared_cameras.push(reflected);
            Some(prepared_cameras.len() - 1)
        } else {
            None
        };
        self.ensure_hdr(self.render_size().0, self.render_size().1);
        if let Some((_, view)) = self.water_scene.as_ref() {
            let generation = self.hdr_size.0 as u64 | ((self.hdr_size.1 as u64) << 32);
            self.gfx3d.set_monitor_view(view, generation);
        }
        let surface_rays = self.godrays.as_ref()
            .map(|rays| rays.surface_params(&self.sky_params))
            .unwrap_or([[0.0; 4]; 4]);
        self.gfx3d.set_surface_rays(&self.queue, &surface_rays);
        static LATE_SURFACE_RAYS: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
        let late_surface_slots = gfx3d::late_surface_scattering_slots(&plan, draws3d,
            *LATE_SURFACE_RAYS.get_or_init(||
                std::env::var("WGR_LATE_SURFACE_RAYS").is_ok_and(|v| v == "1")
                && !std::env::var("WGR_EARLY_LATE_OPAQUE").is_ok_and(|v| v == "1" || v == "2")));
        if !late_surface_slots.is_empty() {
            static TRACE: std::sync::atomic::AtomicU32 = std::sync::atomic::AtomicU32::new(0);
            if TRACE.fetch_add(1, std::sync::atomic::Ordering::Relaxed) % 300 == 0 {
                self.log.log(log_level::INFO, &format!("late surface rays: {} material slots, intensity={}",
                    late_surface_slots.iter().filter(|v| **v).count(), surface_rays[2][0]));
            }
        }
        self.sky.fog.invalidate(&self.queue);
        self.gfx3d.set_layer_fog_requested(layered_fog::requested(&self.layer_fog_settings,
            self.layer_fog_weather,self.hdr_enabled,self.cam_underground));
        self.gfx3d.begin_lod_demand(); // before model-table upload; disabled/no request is a no-op
        self.gfx3d.prepare(
            &self.device,
            &self.queue,
            &self.textures,
            &prepared_cameras,
            draws3d,
            &plan.order,
            &late_surface_slots,
            palette,
            lights,
            &shadow_mask_view,
            shadow_mask_gen,
            &shadow_mapping,
            &heightmap_view,
            heightmap_gen,
            &conform_params,
            self.terrain.snow_buffer(),
            self.terrain.holes_buffer(),
            self.sky.froxel_view(),
            &self.sky.fog.view, &self.sky.fog.consumer, &self.sky.fog.solar_view, &heightmap_view,
            self.sky.sh_buffer(),
            &skyvis_view,
            self.sky.cloud_shadow_view(),
            // DZ-003: the same equirect reflection env map water already reflects, lent to
            // the object paths so a material declaring itself reflective (DayZ's CalmWater
            // family) samples the real sky instead of drawing as a flat near-black quad.
            self.sky.env_view(),
            &self.foliage_params,
            reflected_camera.zip(planar_sea),
            main_scene_cam,
        );
        self.terrain
            .prepare(&self.device, &self.queue, terrain_nodes);
        // TW-WATER W3i: the light the Tidewater water is lit with, logged every ~10 s while that
        // backend renders (colour calibration against Tidewater's units: sun E ~ 9, sky E/PI ~ 1).
        if self.water.tidewater().is_some() {
            static TW_LIGHT_LOG: std::sync::atomic::AtomicU32 = std::sync::atomic::AtomicU32::new(0);
            let n = TW_LIGHT_LOG.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
            if let (0, Some(c)) = (n % 600, prepared_cameras.get(water_camera)) {
                self.log.log(
                    log_level::INFO,
                    &format!(
                        "TW light: sun_diffuse {:?} sun_ambient {:?} fog {:?} sun_dir {:?}",
                        c.sun_diffuse, c.sun_ambient, c.fog_color, c.sun_dir_world
                    ),
                );
                if let Some(tw) = self.water.tidewater() {
                    let (on, stations, stats) = tw.spray_stats();
                    self.log.log(
                        log_level::INFO,
                        &format!("TW spray (W4a): on {on}, breaker stations {stations}, ring {stats:?}"),
                    );
                    self.log.log(log_level::INFO, &format!("TW wake (W6): {}", tw.wake_stats()));
                }
                // W7b: the Tidewater prepare's CPU cost since the last line
                if let Some(prep) = self.water.tidewater_mut().map(|tw| tw.take_prepare_stats()) {
                    self.log.log(log_level::INFO, &prep);
                }
                // W6j.1: the bow-spray diagnostics windows (WGR_TW_SPRAY_DIAG=1)
                for line in self.water.tidewater_mut().map(|tw| tw.take_spray_diag()).unwrap_or_default() {
                    self.log.log(log_level::INFO, &line);
                }
            }
        }
        // W5e: the spot lights, for the Tidewater diver's torch
        if let Some(tw) = self.water.tidewater_mut() {
            tw.set_lights(lights);
        }
        self.water.prepare(
            &self.device,
            &self.queue,
            water_nodes,
            prepared_cameras.get(water_camera),
        );
        // TW-WATER W3b: Tidewater's wet sand on the terrain (None: OP's own coast wet band).
        {
            let wet = self.water.shore_wetness();
            self.terrain.set_shore_wetness(
                &self.device,
                &self.queue,
                wet.as_ref().map(|w| {
                    (
                        w.state,
                        w.lace,
                        w.generation,
                        crate::terrain::TwWetParams { regions: w.regions, p: [1.0, w.sea_level, 0.0, 0.0] },
                    )
                }),
            );
        }
        // Arm only after retained-scene preparation, before prepare_cull uploads its
        // target-model uniform. A missing main camera leaves the request Unknown.
        let main_count_armed = if let (Some(&(token, model_id, source_generation)), Some(_)) =
            (main_count_request.as_ref(), cameras.get(main_scene_cam)) {
            self.gfx3d.arm_main_target_count(&self.device, token, model_id, source_generation)
        } else { false };
        if main_count_armed {
            if let Some((token, _, _)) = main_count_request {
                self.gfx3d.freeze_view_applicability(token, shadow, planar_active);
            }
        }
        if let Some(tracker)=self.demand_view_snapshot.as_mut() {
            self.gfx3d.configure_demand_view_snapshot(tracker,shadow,planar_active,main_count_request);
        }
        // GPU-driven rendering (Stage 3): upload the retained scene + this frame's cull
        // params from the main scene camera (the terrain camera, else the first 3D draw's).
        // No-op when disabled; inert until C++ registers a scene.
        if let Some(cam) = cameras.get(main_scene_cam) {
            self.gfx3d.prepare_cull(
                &self.device,
                &self.queue,
                cam,
                shadow,
                reflected_camera.map(|i| &prepared_cameras[i]),
                self.planar_div as f32 * self.planar_reflection_pad,
            );
        }
        self.ensure_hdr(self.render_size().0, self.render_size().1);
        // The compositor resolves a water path per pixel, so it also runs while the eye is just
        // above the surface. Pixels whose rays never enter water return the untouched scene;
        // downward rays begin extinction only after crossing the local water plane.
        // Live from the Water tab ("Engage band"), no longer a constant — the band decides how
        // far into open air the pass keeps running, which is exactly the knob you want when
        // tuning how the effect behaves around the waterline.
        let underwater_tuning = self.water.underwater_tuning();
        let underwater_near_surface_band = underwater_tuning.0;
        // The Water tab's checkbox. Without this the pass ran whatever the checkbox said: the
        // depth lane it used to rely on gates the water shader's tint, but the compositor also
        // engages on proximity to the surface, and a submerged camera is always proximate.
        let underwater_enabled = self.water.underwater_enabled();
        // TW-WATER W9a: Tidewater measures the surface it draws under the eye; that, not the
        // engine's submersion (Current OP's reference waves, shore waves included, which are not
        // the surface drawn in that mode: at the beach they put the eye metres under water while
        // it stood above it), decides the gate there.
        let cam_surface = self.water.camera_surface_height();
        let underwater_state =
            self.water
                .underwater_params()
                .map(|(sea_level, time, submersion)| match cam_surface {
                    Some(h) => (h, time, 0.0),
                    None => (sea_level, time, submersion),
                })
                .and_then(|(sea_level, time, submersion)| {
                    let player_submerged = submersion > 0.0;
                    // Use the water draw camera, not an unrelated terrain/scene batch. The visual
                    // submersion boundary is the actual camera crossing the gameplay sea plane.
                    // Prepared (jittered) camera, same reasoning as the sky ivp above.
                    prepared_cameras.get(water_camera).and_then(|cam| {
                        let cam_above = cam.cam_pos[1] - sea_level;
                        let engage = underwater_enabled
                            && (player_submerged || cam_above < underwater_near_surface_band);
                        engage.then(|| {
                            // Same separate-inverse-in-f64 treatment the frame UBO uses: the
                            // reversed-Z infinite-far projection is ill-conditioned in f32 and
                            // inverting the combined matrix smears its z-row into x/y.
                            let view = glam::DMat4::from_cols_array(&cam.view.map(f64::from));
                            let proj = glam::DMat4::from_cols_array(&cam.proj.map(f64::from));
                            let inv_vp =
                                (view.inverse() * proj.inverse()).as_mat4().to_cols_array();
                            // A displaced crest can submerge the eye while it is still above the
                            // flat sea datum, so the flat `cam_above` would call it air. Use the
                            // measured submersion depth instead.
                            //
                            // This used to snap to a fixed -0.08 the moment a boolean tripped,
                            // which popped the screen to full underwater colour as a crest passed.
                            // Depth makes it continuous: shallow submersion gives a shallow tint.
                            let effective_above = if player_submerged {
                                -submersion
                            } else {
                                cam_above
                            };
                            (time, effective_above, *cam, inv_vp)
                        })
                    })
                });
        let underwater_engaged = underwater_state.is_some();
        // TW-WATER W5d: more than a metre under the surface (flat datum or measured submersion).
        self.submerged_view = underwater_state.as_ref().is_some_and(|(_, above, _, _)| *above < -1.0);
        if self.underwater_engaged_logged != Some(underwater_engaged) {
            self.underwater_engaged_logged = Some(underwater_engaged);
            // eprintln! rather than self.log, matching the other [wgr] renderer diagnostics —
            // it is what actually reaches the captured stderr in a harness run.
            eprintln!(
                "[wgr] underwater compositor {} (enabled={} band={:.2}m)",
                if underwater_engaged { "ENGAGED" } else { "off" },
                underwater_enabled,
                underwater_near_surface_band,
            );
        }
        let underwater_time = underwater_state.map(|(time, _, _, _)| time);
        let underwater_body = self
            .water
            .underwater_body()
            .map(|(shallow, deep, ext)| {
                (
                    [shallow[0], shallow[1], shallow[2], ext],
                    [deep[0], deep[1], deep[2], 0.0],
                )
            })
            .unwrap_or(([0.070, 0.290, 0.320, 0.16], [0.014, 0.105, 0.240, 0.0]));
        let underwater_spectrum = self.water.underwater_spectrum();
        self.underwater_view = underwater_state
            .map(|(_, above, cam, inv_vp)| UnderwaterView {
                cam_above: above,
                camera_pos: [cam.cam_pos[0], cam.cam_pos[1], cam.cam_pos[2]],
                inv_view_proj: inv_vp,
                shallow_color_ext: underwater_body.0,
                deep_color: underwater_body.1,
                // WgrCamera carries the sun's travel direction; volumetric scattering
                // needs the surface-to-sun direction.
                sun_dir: [
                    -cam.sun_dir_world[0],
                    -cam.sun_dir_world[1],
                    -cam.sun_dir_world[2],
                ],
                sun_radiance: [cam.sun_diffuse[0], cam.sun_diffuse[1], cam.sun_diffuse[2]],
                camera_shadow: cam.shadow,
                cascade_lengths: underwater_spectrum.0,
                active_layers: underwater_spectrum.1,
                warp_amp: underwater_spectrum.2,
                sea_level: underwater_spectrum.3,
                debug_view: underwater_spectrum.4,
                wave_scale: underwater_spectrum.5,
                body_wave_scale: underwater_spectrum.6,
                body_ellipse: self.water.camera_body_ellipse([cam.cam_pos[0], cam.cam_pos[2]]).0,
                body_frame: self.water.camera_body_ellipse([cam.cam_pos[0], cam.cam_pos[2]]).1,
                density: underwater_tuning.1,
                color_bias: underwater_tuning.2,
                caustic_gain: underwater_tuning.3,
            })
            .unwrap_or(UnderwaterView {
                cam_above: -1.0,
                camera_pos: [0.0; 3],
                inv_view_proj: [0.0; 16],
                shallow_color_ext: underwater_body.0,
                deep_color: underwater_body.1,
                sun_dir: [0.0, 1.0, 0.0],
                sun_radiance: [1.0; 3],
                camera_shadow: unsafe { std::mem::zeroed() },
                cascade_lengths: underwater_spectrum.0,
                active_layers: underwater_spectrum.1,
                warp_amp: underwater_spectrum.2,
                sea_level: underwater_spectrum.3,
                debug_view: underwater_spectrum.4,
                wave_scale: underwater_spectrum.5,
                body_wave_scale: underwater_spectrum.6,
                body_ellipse: [0.0; 4],
                body_frame: [1.0, 0.0],
                density: underwater_tuning.1,
                color_bias: underwater_tuning.2,
                caustic_gain: underwater_tuning.3,
            });
        if underwater_time.is_some() && !self.hdr_enabled {
            let (rw, rh) = self.render_size();
            self.ensure_underwater_target(rw, rh);
        }

        if let Some(probe)=self.post_optics_probe.as_mut(){probe.capture(cameras.get(main_scene_cam),prepared_cameras.get(main_scene_cam),main_selection.source);}
        let acquire_t0 = std::time::Instant::now();
        let Some(frame) = self.acquire(call_timings.as_deref().map(|facts| facts.call_token))? else {
            if let Some(facts) = call_timings.as_deref_mut() { facts.skip(); }
            return Ok(false);
        };
        let acquire_ms = acquire_t0.elapsed().as_secs_f32() * 1.0e3;
        let screenshot_staging = if self.screenshot_requested {
            let width = self.config.width;
            let height = self.config.height;
            let bytes_per_row = width.saturating_mul(4);
            let padded_bytes_per_row = bytes_per_row.div_ceil(wgpu::COPY_BYTES_PER_ROW_ALIGNMENT)
                * wgpu::COPY_BYTES_PER_ROW_ALIGNMENT;
            let rgba_or_bgra = matches!(
                self.config.format,
                wgpu::TextureFormat::Rgba8Unorm
                    | wgpu::TextureFormat::Rgba8UnormSrgb
                    | wgpu::TextureFormat::Bgra8Unorm
                    | wgpu::TextureFormat::Bgra8UnormSrgb
            );
            if !rgba_or_bgra || width == 0 || height == 0 {
                self.log.log(
                    log_level::WARN,
                    "wgpu screenshot unavailable for the current surface format",
                );
                self.screenshot_requested = false;
                None
            } else {
                Some((
                    self.device.create_buffer(&wgpu::BufferDescriptor {
                        label: Some("wgr_screenshot_readback"),
                        size: padded_bytes_per_row as u64 * height as u64,
                        usage: wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::MAP_READ,
                        mapped_at_creation: false,
                    }),
                    width,
                    height,
                    bytes_per_row,
                    padded_bytes_per_row,
                    matches!(
                        self.config.format,
                        wgpu::TextureFormat::Bgra8Unorm | wgpu::TextureFormat::Bgra8UnormSrgb
                    ),
                ))
            }
        } else {
            None
        };

        let color = frame
            .texture
            .create_view(&wgpu::TextureViewDescriptor::default());
        // Scene (3D/terrain/interleaved 2D) renders here: the HDR target when the HDR
        // path is on, else straight to the swapchain. TextureView clones are cheap
        // (Arc), so cloning avoids holding a borrow of self across the segment loop.
        let scene_view = if self.hdr_enabled {
            self.hdr.as_ref().expect("HDR target").1.clone()
        } else if underwater_time.is_some() {
            self.underwater_target
                .as_ref()
                .expect("underwater target")
                .1
                .clone()
        } else {
            color.clone()
        };
        let depth = self
            .gfx3d
            .depth_view()
            .ok_or("depth target missing")?
            .clone();
        // UI depth must match both the swapchain's sample count AND output size.
        // Even at 1x the scene depth cannot serve UI when render scale is not 100%.
        let ui_depth = self.gfx3d.ui_depth_view().cloned();
        // The prepass' view-space normal G-buffer target (None when the prepass is
        // disabled). Cloned (Arc) so no borrow of self is held across the segment loop.
        let normal = if self.prepass_enabled {
            Some(
                self.gfx3d
                    .normal_view()
                    .ok_or("normal target missing")?
                    .clone(),
            )
        } else {
            None
        };
        let mut encoder = self
            .device
            .create_command_encoder(&wgpu::CommandEncoderDescriptor {
                label: Some("wgr_frame"),
            });
        // WTR-002 — new frame, new set of timestamp brackets.
        // Cleared per frame; set by the first depth resolve recorded after the 3D depth is final,
        // so the god-ray march can reuse it (see render_god_rays).
        self.frame_depth_final_resolved = false;
        self.godrays_composited = false;
        // New encoder, and the depth target is about to be cleared and rewritten: nothing
        // resolved on a previous frame's encoder can be reused.
        self.far_depth_resolve_current = false;
        self.gpu_timers.begin_frame();
        self.gpu_timers
            .cpu_record(crate::gpu_timers::Region::WorkerAcquire, acquire_ms);
        self.gpu_timers.cpu_record(
            crate::gpu_timers::Region::WorkerSetup,
            setup_t0.elapsed().as_secs_f32() * 1.0e3,
        );
        // PERF-024: the encode, split four ways (see gpu_timers::Region::WorkerEnc*).
        self.gpu_timers
            .cpu_begin(crate::gpu_timers::Region::WorkerEncEarly);
        self.direct_draws.set(DirectDrawStats::default());
        self.water_visibility.begin_frame();
        // Envelope every submitted pass so the Performance tab can distinguish
        // GPU completion from acquire/present pacing without summing overlapping
        // individual pass timers.
        self.gpu_timers.begin(&mut encoder, TimerRegion::FrameTotal);
        // PERF-005: zero every cull view's accounting slice BEFORE the frame's first cull (the
        // planar-reflection cull, further down, is the earliest). Whole-buffer, so a view that
        // does not run this frame reports zeros instead of repeating its last live frame.
        self.gfx3d.begin_frame_object_stats(&mut encoder);
        // AST-012A: zero the per-slot mip codes before the frame's first object draw, for the
        // same reason the cull stats are zeroed here — a slot nothing samples this frame must
        // read as unsampled, not repeat whatever the last live frame left in it.
        self.gfx3d.begin_frame_mip_feedback(&mut encoder);

        // Grass owns a compact camera-relative candidate grid.  Refresh its borrowed
        // terrain inputs before the placement compute so the pass sees the same terrain
        // state as the terrain draw later in this frame.
        self.grass
            .prepare_terrain(&self.device, &self.queue, &self.terrain);
        // Pixels per radian for the main scene camera: (viewport_height / 2) * proj[1][1].
        // `proj` is column-major so [1][1] is element 5, and that element is
        // 1/tan(fov_y/2) by construction: Camera.cpp sets _projectionNormal(1,1) = 1/cTop,
        // MatrixConversion maps (1,1) -> flat index 5, and EngineWgpu's reversed-Z
        // infinite-far rebuild touches only _33 and _43, leaving element 5 alone.
        //
        // The near ring uses this to stop where its blades stop being a pixel wide, which
        // is a DIFFERENT distance at every resolution -- a 32-86 mm blade goes sub-pixel at
        // roughly 24 m at 800x600 and 55 m at 1080p -- so a fixed metric radius is wrong at
        // all but one of them.
        // DISABLED pending a fix, and NOT for the reason first recorded. The metric itself
        // is correct: with the capture harness actually passing a resolution (it was
        // silently running every "1080p" capture at 800x600), the near grid scales exactly
        // with viewport height -- dim 152 at 600 and 272 at 1080, a ratio of 1.79 against
        // the viewport's 1.80. So proj[5] IS 1/tan(fov_y/2) here, as the source chain says
        // (Camera.cpp sets _projectionNormal(1,1) = 1/cTop; MatrixConversion maps it to flat
        // index 5; the reversed-Z rebuild touches only _33/_43).
        //
        // What is broken is downstream: with the clamp applied the near ring places almost
        // nothing -- 0 instances at 800x600 and 35 at 1080p, against 6,378 and 6,925
        // unclamped -- even though the radius lands at a perfectly reasonable ~24 m and
        // ~44 m. The grid is resized correctly and then fails to populate, so the fault is
        // in how a resized near grid is filled, not in the radius.
        //
        // if let Some(cam) = cameras.get(main_scene_cam) {
        //     self.grass
        //         .set_screen_metrics(&self.queue, self.config.height as f32 * 0.5 * cam.proj[5]);
        // }
        if let (Some(batch), Some(camera_bind)) = (grass_batches.first(), self.gfx3d.camera_bind())
        {
            self.grass.reset_indirect(&self.queue);
            let offset = (batch.camera as u64 * self.gfx3d.camera_stride()) as u32;
            self.grass
                .dispatch(&mut encoder, camera_bind, offset, &self.gpu_timers);
        }

        // Update the half-resolution reflected target every visible above-water frame. Reusing
        // it while the camera moves causes clouds to lag behind the projected water lookup.
        // CPU draw-stream matrices are main-camera-relative, but retained GPU-driven instances
        // are absolute-world transforms and receive an independent reflected cull below.
        if let Some(reflected_index) = reflected_camera {
            self.ensure_planar_target();
            let planar = self.planar.as_ref().expect("planar target");
            {
                let cam = &prepared_cameras[reflected_index];
                let view = glam::DMat4::from_cols_array(&cam.view.map(f64::from));
                let proj = glam::DMat4::from_cols_array(&cam.proj.map(f64::from));
                let m = (view.inverse() * proj.inverse()).as_mat4().to_cols_array();
                let ivp = [
                    [m[0], m[1], m[2], m[3]],
                    [m[4], m[5], m[6], m[7]],
                    [m[8], m[9], m[10], m[11]],
                    [m[12], m[13], m[14], m[15]],
                ];
                // Reflections must see the sky, not be clipped by the mirror plane.
                self.sky.set_ocean_level(None);
                self.sky.upload(
                    &self.queue,
                    &self.sky_params,
                    ivp,
                    cam.cam_pos,
                    &shadow_mapping,
                    &cam.shadow,
                );
                // Sky has no depth-stencil state, so it must not share the terrain pass's
                // depth attachment. Render it first, then depth-test reflected terrain over it.
                self.gpu_timers.begin(&mut encoder, TimerRegion::PlanarSky);
                let mut sky_pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
                    label: Some("wgr_planar_reflection_sky"),
                    color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                        view: &planar.color_view,
                        depth_slice: None,
                        resolve_target: None,
                        ops: wgpu::Operations {
                            load: wgpu::LoadOp::Clear(wgpu::Color::BLACK),
                            store: wgpu::StoreOp::Store,
                        },
                    })],
                    depth_stencil_attachment: None,
                    timestamp_writes: None,
                    occlusion_query_set: None,
                    multiview_mask: None,
                });
                self.sky.render_planar(&mut sky_pass);
                drop(sky_pass);
                self.gpu_timers.end(&mut encoder, TimerRegion::PlanarSky);
                self.gpu_timers
                    .begin(&mut encoder, TimerRegion::PlanarTerrain);
                let mut terrain_pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
                    label: Some("wgr_planar_reflection_terrain"),
                    color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                        view: &planar.color_view,
                        depth_slice: None,
                        resolve_target: None,
                        ops: wgpu::Operations {
                            load: wgpu::LoadOp::Load,
                            store: wgpu::StoreOp::Store,
                        },
                    })],
                    depth_stencil_attachment: Some(wgpu::RenderPassDepthStencilAttachment {
                        view: &planar.depth_view,
                        depth_ops: Some(wgpu::Operations {
                            load: wgpu::LoadOp::Clear(0.0),
                            store: wgpu::StoreOp::Store,
                        }),
                        stencil_ops: None,
                    }),
                    timestamp_writes: None,
                    occlusion_query_set: None,
                    multiview_mask: None,
                });
                if let Some(bind) = self.gfx3d.camera_bind() {
                    let off = (reflected_index as u64 * self.gfx3d.camera_stride()) as u32;
                    for batch in terrain_batches {
                        self.terrain.draw(
                            &mut terrain_pass,
                            bind,
                            off,
                            batch.first_node,
                            batch.node_count,
                            crate::terrain::TerrainPass::Planar,
                        );
                    }
                }
                drop(terrain_pass);
                self.gpu_timers
                    .end(&mut encoder, TimerRegion::PlanarTerrain);
                // The bracket includes the reflected cull dispatch: it exists solely to
                // produce this pass's indirect args, so it is part of the planar-objects cost.
                self.gpu_timers
                    .begin(&mut encoder, TimerRegion::PlanarObjects);
                let reflection_cull_recorded = self.gfx3d.cull_dispatch_reflection(&mut encoder);
                let mut object_pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
                    label: Some("wgr_planar_reflection_objects"),
                    color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                        view: &planar.color_view,
                        depth_slice: None,
                        resolve_target: None,
                        ops: wgpu::Operations {
                            load: wgpu::LoadOp::Load,
                            store: wgpu::StoreOp::Store,
                        },
                    })],
                    depth_stencil_attachment: Some(wgpu::RenderPassDepthStencilAttachment {
                        view: &planar.depth_view,
                        depth_ops: Some(wgpu::Operations {
                            load: wgpu::LoadOp::Load,
                            store: wgpu::StoreOp::Store,
                        }),
                        stencil_ops: None,
                    }),
                    timestamp_writes: None,
                    occlusion_query_set: None,
                    multiview_mask: None,
                });
                let off = (reflected_index as u64 * self.gfx3d.camera_stride()) as u32;
                let reflection_recorded = self.gfx3d
                    .draw_gpu_driven_reflection(&mut object_pass, &self.textures, off);
                drop(object_pass);
                if reflection_recorded {
                    self.gfx3d.record_geometry_reflection();
                    if reflection_cull_recorded {
                        self.gfx3d.record_reflection_target_draw_closed();
                    }
                }
                self.gpu_timers
                    .end(&mut encoder, TimerRegion::PlanarObjects);
                if !ablate.clouds && self.sky.clouds_active(&self.sky_params) {
                    self.gpu_timers
                        .begin(&mut encoder, TimerRegion::PlanarClouds);
                    self.sky.render_cloud(
                        &self.device,
                        &mut encoder,
                        &planar.depth_sample_view,
                        planar.size.0,
                        planar.size.1,
                        self.cloud_reflection_cheap,
                        false,
                    );
                    let mut cloud_pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
                        label: Some("wgr_planar_reflection_cloud_composite"),
                        color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                            view: &planar.color_view,
                            depth_slice: None,
                            resolve_target: None,
                            ops: wgpu::Operations {
                                load: wgpu::LoadOp::Load,
                                store: wgpu::StoreOp::Store,
                            },
                        })],
                        depth_stencil_attachment: None,
                        timestamp_writes: None,
                        occlusion_query_set: None,
                        multiview_mask: None,
                    });
                    self.sky.composite_cloud_planar(&mut cloud_pass);
                    drop(cloud_pass);
                    self.gpu_timers.end(&mut encoder, TimerRegion::PlanarClouds);
                }
                // Transfer the finished attachment into mip 0, then generate the roughness
                // chain sampled by water. The copy is cheaper than carrying main-scene MSAA
                // through the entire reflected scene and keeps the hot attachment dedicated.
                encoder.copy_texture_to_texture(
                    wgpu::TexelCopyTextureInfo {
                        texture: &planar._color,
                        mip_level: 0,
                        origin: wgpu::Origin3d::ZERO,
                        aspect: wgpu::TextureAspect::All,
                    },
                    wgpu::TexelCopyTextureInfo {
                        texture: &planar._sampled,
                        mip_level: 0,
                        origin: wgpu::Origin3d::ZERO,
                        aspect: wgpu::TextureAspect::All,
                    },
                    wgpu::Extent3d {
                        width: planar.size.0,
                        height: planar.size.1,
                        depth_or_array_layers: 1,
                    },
                );
                self.gpu_timers.begin(&mut encoder, TimerRegion::PlanarMips);
                self.planar_mips
                    .render(&self.device, &mut encoder, &planar.mip_views);
                self.gpu_timers.end(&mut encoder, TimerRegion::PlanarMips);
            }
            let planar = self.planar.as_ref().expect("planar target");
            let generation = planar.size.0 as u64 | ((planar.size.1 as u64) << 32);
            self.water.set_planar_view(
                &self.device,
                &self.queue,
                &planar.sampled_view,
                generation,
                reflected_vp,
                planar_fade,
            );
            // CORRECTNESS — close and submit the frame here, so the reflected sky uniform this
            // block just uploaded is the one these passes actually execute with.
            //
            // `Queue::write_buffer` is documented to begin GPU execution "only on the next call to
            // Queue::submit(), just before the explicitly submitted commands". Sky owns ONE uniform
            // buffer and the frame writes it twice: once above with the mirrored camera's
            // inv_view_proj / cam_pos / (zeroed) cascades, and once further down with the main
            // camera's. With a single submit at the end of the frame BOTH writes landed before ANY
            // command ran, so the last one won and the reflected sky, cloud march and cloud
            // composite were all rendered with the MAIN camera's rays — the mirrored basis built
            // above was computed, uploaded, and then silently overwritten before it was ever read.
            // The reflection still looked plausible (it is half-res and mip-blurred before water
            // samples it), which is why this survived: it reads as a slightly wrong sky, not as a
            // broken one.
            //
            // Splitting the submit is the fix available from here. The alternative — a second
            // uniform slice or a dynamic offset — lives in sky/mod.rs, which another workstream
            // owns; that is the tidier end state and should replace this when that file is free.
            // The extra submission is not pure cost: the reflection no longer waits for the rest of
            // the frame to be recorded before the GPU can start it.
            //
            // Safe because the sky uniform is the only buffer this frame writes twice (every other
            // upload — cameras, terrain, cull params, water, tonemap — is written once, and a write
            // flushed at an earlier submit than strictly needed is harmless). Timestamps are
            // queue-global and the query set is shared, so the FrameTotal bracket opened before
            // this submit still resolves correctly against its close in the second command buffer.
            let reflection_count_copied = main_count_armed &&
                self.gfx3d.copy_reflection_target_count(&mut encoder);
            let planar_encoder = std::mem::replace(
                &mut encoder,
                self.device
                    .create_command_encoder(&wgpu::CommandEncoderDescriptor {
                        label: Some("wgr_frame_main"),
                    }),
            );
            let planar_commands = planar_encoder.finish();
            let reflection_count_ready = reflection_count_copied &&
                main_count_request.is_some_and(|(token, _, _)|
                    self.gfx3d.reflection_target_count_before_submit(token));
            self.queue.submit(std::iter::once(planar_commands));
            if reflection_count_ready {
                if let Some((token, _, _)) = main_count_request {
                    self.gfx3d.reflection_target_count_submitted(token);
                }
            }
        } else {
            // The pass did not run this frame. The target still holds whatever it was last
            // rendered for, and `reflected_vp` in water's uniform is stale with it, so the fade
            // has to be driven to zero explicitly — the shader falls back to the sky env map,
            // which is exactly the source `planar_fade` has already dissolved toward.
            //
            // This branch did not exist before the coverage gate, and its absence was a latent
            // bug independent of it: the pass already stopped running when the camera dropped
            // below the surface or water left the draw stream, and on those frames the water
            // kept sampling a stale reflection through a stale matrix at full weight.
            self.water.set_planar_fade(&self.queue, 0.0);
        }

        // The local ripple field is independent of opaque depth and is consumed only by
        // water, so update it once before any render pass records this frame's water draw.
        self.water
            .update_interactions(&self.device, &mut encoder, &self.gpu_timers);

        // Compute skin bake runs first of all (docs/compute-skin-bake-plan.md): it writes
        // the shared skinned vertex buffer that the shadow cascades, the depth prepass, and
        // the forward pass all read, so it must precede them; wgpu inserts the storage->
        // vertex barrier. No-op when the bake is off or there is no skinned geometry.
        self.gfx3d.skin_bake(&mut encoder);

        // GPU cull compute (Stage 3): culls + LOD-selects the retained scene into the
        // indirect args the colour pass consumes. Recorded before the render passes so
        // wgpu barriers its storage writes -> the indirect reads. No-op when disabled.
        self.gfx3d.cull_dispatch(&mut encoder, &self.gpu_timers);
        // One extra cull dispatch per shadow cascade (§6 multi-view): each produces that
        // cascade's depth-pass indirect args, consumed by the GPU-driven shadow draw inside
        // render_shadow_passes. No-op when GPU-driven / shadows are off.
        self.gfx3d
            .cull_dispatch_shadows(&mut encoder, &self.gpu_timers);
        // And one for the interior sky-visibility map's ortho view (LIT-020). No-op unless the
        // feature is enabled and its target exists.
        self.gfx3d
            .cull_dispatch_interior_sky(&mut encoder, &self.gpu_timers);
        // FAR INSTANCE TIER — the proxy sweep, beside the object cull for the same reason:
        // recorded before the render passes so wgpu barriers its storage writes against this
        // frame's indirect reads. Deliberately NOT dispatched for the shadow cascades or the
        // planar reflection: a proxy is a silhouette stand-in, and casting shadows or
        // reflections from one puts a box's error somewhere the eye can compare it against
        // the real geometry beside it.
        if let Some(camera_bind) = self.gfx3d.camera_bind() {
            // Pixels per radian for THIS camera: (viewport_height / 2) * proj[1][1], the same
            // derivation the grass ring uses. The sub-pixel test is the tier's largest single
            // win and a fixed metric threshold is correct at exactly one resolution, so it is
            // fed the real screen metric or nothing.
            if let Some(cam) = cameras.get(main_scene_cam) {
                self.far
                    .set_view_metrics(&self.queue, self.config.height as f32 * 0.5 * cam.proj[5]);
            }
            // And this frame's REAL object cull coefficients, which are the tier's start rule:
            // it accepts exactly the placements those coefficients make gfx3d/cull.wgsl drop.
            // Refreshed here, next to the view metrics and immediately before the sweep, so a
            // framerate governor that moves lod_inv_width mid-session moves the seam with it —
            // and so this cannot read a value from before the frame's cull params were pushed.
            let cull_inputs = self.gfx3d.cull_inputs();
            self.far.set_cull_complement(&self.queue, cull_inputs);
            let offset = (main_scene_cam as u64 * self.gfx3d.camera_stride()) as u32;
            // REN-TEMP-001T: remembered for the velocity twin recorded in run_tonemap.
            self.main_cam_off = offset;
            if !suppress_world_objects {
                self.far.dispatch(
                    &self.queue,
                    &mut encoder,
                    camera_bind,
                    offset,
                    &self.gpu_timers,
                );
            }
        }

        for line in self.gfx3d.take_queued_log() {
            self.log.log(log_level::INFO, &line);
        }

        // Non-vacuity check on the map, once, ~2 s in. Reads the PREVIOUS frame's contents (this
        // frame's pass is recorded below and not yet submitted), which is exactly what we want:
        // a fully rendered map from a settled scene. 0.0% here means the pass drew nothing and
        // the whole feature is inert — the failure mode that otherwise looks identical to
        // success, since an empty map reads as "open sky everywhere".
        // An explicit request re-arms the one-shot, and does so WITHOUT the 120-frame wait: the
        // caller is standing where they want the answer and the scene around them is settled.
        {
            let want = self.gfx3d.interior_sky_settings().probe;
            if want != self.interior_sky_probe_seen {
                self.interior_sky_probe_seen = want;
                self.interior_sky_dbg_logged = false;
                self.interior_sky_dbg_frames = 121;
            }
        }
        if self.gfx3d.interior_sky_active() && !self.interior_sky_dbg_logged {
            self.interior_sky_dbg_frames += 1;
            if self.interior_sky_dbg_frames > 120 {
                // Which measurement this is. A startup line describes the loading screen and a
                // requested one describes where the player is standing; they are not comparable
                // and were previously indistinguishable in a log.
                let tag = if self.interior_sky_probe_seen > 0 {
                    "requested"
                } else {
                    "startup"
                };
                let st = self
                    .gfx3d
                    .interior_sky_debug_state(&self.device, &self.queue);
                if let Some((res, cov)) = self
                    .gfx3d
                    .interior_sky_map_coverage(&self.device, &self.queue)
                {
                    let total: f32 = cov.iter().sum();
                    let per_dir = cov
                        .iter()
                        .map(|c| format!("{:.2}%", c * 100.0))
                        .collect::<Vec<_>>()
                        .join(" ");
                    self.log.log(
                        if total > 0.0 {
                            log_level::INFO
                        } else {
                            log_level::WARN
                        },
                        &format!(
                            "[wgr] interior sky maps ({tag}): {res}x{res} x{} — occluder coverage per direction                              (zenith first): {per_dir} (args={} bind={} instances={} sub_draws per variant (solid+cutout)={}){}",
                            cov.len(),
                            st.0,
                            st.1,
                            st.2,
                            st.3.iter()
                                .map(|v| v.to_string())
                                .collect::<Vec<_>>()
                                .join("+"),
                            if total > 0.0 {
                                ""
                            } else if st.3.iter().all(|v| *v == 0) {
                                " — ALL EMPTY and the sky cull emitted no sub-draws: either nothing                                  retained is within the box (legitimate on open terrain — widen it                                  with WGR_INTERIOR_SKY_EXTENT to check) or the cull views are                                  broken. Sky reach is 1 everywhere either way."
                            } else {
                                " — ALL EMPTY despite sub-draws: the cull kept geometry but nothing                                  rasterised. That is a VP or pass fault, not an empty neighbourhood."
                            }
                        ),
                    );
                    self.interior_sky_dbg_logged = true;
                }
            }
        }

        // Interior sky-visibility depth map, recorded with the shadow depth passes and for the
        // same reason: every segment's shading samples it, so it must be complete before any of
        // them run, regardless of submission order.
        encoder.push_debug_group("wgr_interior_sky_map");
        self.gfx3d
            .render_interior_sky_pass(&mut encoder, &self.textures, &self.gpu_timers);
        // REN-GI-002: the sun proxy, when its view or the retained set changed.
        self.gfx3d
            .render_gi_rsm_pass(&mut encoder, &self.textures, &self.gpu_timers);
        // REN-GI-001: the probe batch, once the maps and cascades it reads are recorded.
        self.gfx3d
            .gi_dispatch(&mut encoder, self.main_cam_off, &self.gpu_timers);
        encoder.pop_debug_group();

        // TW-WATER W7e: in Tidewater mode there is no planar reflection, so nothing splits the
        // frame above and the GPU sits idle until the single submit at the end (~3 ms of CPU in
        // W7b's P1 rows, on top of the GPU time). The early work (the water simulation, culls, far
        // field, GI) is submitted here, as the planar split does, so the GPU runs it while the rest
        // of the frame is recorded (W7e A/B: P1 wall p50 ~9.2 -> ~6.9 ms, P3 ~7.7 -> ~6.2 ms; W7f
        // made it the default, WGR_TW_EARLY_SUBMIT=0 restores one submit). Every upload made so far
        // is flushed with it; an upload made later lands before the second submit only.
        if reflected_camera.is_none() && self.water.tidewater().is_some() && tw_early_submit() {
            let early = std::mem::replace(
                &mut encoder,
                self.device
                    .create_command_encoder(&wgpu::CommandEncoderDescriptor {
                        label: Some("wgr_frame_main"),
                    }),
            );
            self.queue.submit(std::iter::once(early.finish()));
        }

        // Cascade shadow depth passes run first so every segment's draws can
        // sample the completed map, regardless of submission order. Debug groups
        // (here and below) name each phase in a RenderDoc capture.
        self.gpu_timers
            .cpu_end(crate::gpu_timers::Region::WorkerEncEarly);
        self.gpu_timers
            .cpu_begin(crate::gpu_timers::Region::WorkerEncShadow);
        encoder.push_debug_group("wgr_shadow_cascades");
        self.gfx3d.render_shadow_passes(
            &self.device,
            &mut encoder,
            &self.textures,
            shadow,
            shadow_casters,
            &self.grass,
            &self.gpu_timers,
        );
        encoder.pop_debug_group();
        self.gpu_timers
            .cpu_end(crate::gpu_timers::Region::WorkerEncShadow);
        self.gpu_timers
            .cpu_begin(crate::gpu_timers::Region::WorkerEncMain);
        self.gpu_timers
            .cpu_begin(crate::gpu_timers::Region::WorkerEncSky);

        // Amortized terrain sun-shadow sweep (long-range heightfield self-shadow),
        // recorded before the render segments sample its mask. The sun direction is
        // uniform across cameras; sun_dir_world is the light travel direction, so
        // negate it for the surface-to-light march.
        if let Some(cam) = cameras.first() {
            let s = cam.sun_dir_world;
            let sun_to_light = glam::Vec3::new(-s[0], -s[1], -s[2]);
            // The mask sweep is amortized (recorded only when the heightmap changes
            // or the sun moves), so its debug group is pushed inside — wrapping it
            // here would leave an empty group on the frames it skips.
            self.terrain
                .render_shadow_mask(&self.queue, &mut encoder, sun_to_light);
        }

        // Replay the instancing plan. It splits into a scene phase and a UI phase at
        // the Resolve op (the engine's scene->UI seam, WGR_CMD_RESOLVE): scene draws
        // render into the HDR target; the tonemap then resolves it to the swapchain;
        // the UI phase draws display-referred straight to the swapchain (2D uses the
        // swapchain-format pipeline set). Segments additionally split at ClearDepth
        // (each clears depth). On the LDR-direct path there is no HDR target/tonemap,
        // and Resolve is a no-op (scene_view IS the swapchain throughout).
        use crate::gfx3d::Plan3dOp;
        let ops = &plan.ops;

        // The clear colour seeds the scene target. On the HDR path that target is
        // linear, so decode the gamma-space clear the engine supplies.
        let clear_rgb = if self.hdr_enabled {
            [
                srgb_to_linear_ch(clear[0]),
                srgb_to_linear_ch(clear[1]),
                srgb_to_linear_ch(clear[2]),
            ]
        } else {
            [clear[0], clear[1], clear[2]]
        };

        // Procedural sky (docs/procedural-sky-plan.md): a fullscreen atmospheric pass
        // into the scene target BEFORE any geometry, so terrain/objects overdraw it.
        // Depth is untouched (the pass has none). Skipped when disabled or camera-less;
        // when it runs it fills every pixel, so the first segment loads over it instead
        // of clearing. The legacy skydome meshes are suppressed on the C++ side.
        // Reconstruct the inverse view-projection (for world ray directions) from the
        // camera the VISIBLE GEOMETRY draws with — the first terrain batch, else the
        // first 3D draw — not cameras[0]. When several cameras are pushed in a frame
        // (e.g. flying: main view + cockpit/optics), cameras[0] can be a stale or
        // secondary view, which makes the sky (and thus the sun disc + horizon haze)
        // stutter against the terrain as the player moves. None = disabled / no camera.
        let sky_ivp = if self.sky_params.control[0] != 0.0 {
            let main_cam = terrain_batches
                .first()
                .map(|b| b.camera as usize)
                .or_else(|| draws3d.first().map(|d| d.camera as usize))
                .unwrap_or(main_scene_cam);
            if self.sky_debug {
                let cur = (cameras.len(), main_cam);
                if cur != self.sky_dbg_last {
                    self.sky_dbg_last = cur;
                    self.log.log(
                        log_level::INFO,
                        &format!("wgr_sky: cameras={} main_cam={}", cur.0, cur.1),
                    );
                }
            }
            // PREPARED cameras: the sky's rays must be reconstructed from the SAME
            // (jittered) projection the scene rasterises with, or sky/cloud pixels sit a
            // sub-pixel off from geometry every frame — invisible under MSAA, but a
            // temporal upscaler un-jitters the whole frame and the mismatch reads as the
            // sky (and horizon) faintly vibrating.
            prepared_cameras.get(main_cam).map(|cam| {
                // Reconstruct inv(proj*view) = inv(view) * inv(proj), inverting the two
                // matrices SEPARATELY and in f64. Our projection is reversed-Z with an
                // infinite far plane (ill-conditioned z-row); inverting the *combined*
                // f32 matrix smears that poor conditioning into the x/y ray components,
                // which shows up as horizon jitter when the orientation changes fast
                // (pitching while flying). inv(view) alone has no such pathology, and the
                // split keeps the projection's conditioning out of x/y. The view already
                // has its translation zeroed (see EngineWgpu::PushSceneCamera), so the
                // result is translation-invariant.
                let view = glam::DMat4::from_cols_array(&cam.view.map(f64::from));
                let proj = glam::DMat4::from_cols_array(&cam.proj.map(f64::from));
                let inv_vp = view.inverse() * proj.inverse();
                let m = inv_vp.as_mat4().to_cols_array();
                let ivp = [
                    [m[0], m[1], m[2], m[3]],
                    [m[4], m[5], m[6], m[7]],
                    [m[8], m[9], m[10], m[11]],
                    [m[12], m[13], m[14], m[15]],
                ];
                // Absolute world camera position (the froxel occlusion needs it to place a
                // marched camera-relative offset onto the world-space terrain shadow mask),
                // plus this camera's cascade matrices for the froxel's near-field CSM occlusion.
                (ivp, cam.cam_pos, cam.shadow)
            })
        } else {
            None
        };
        let sky_drawn = sky_ivp.is_some();
        // The god-ray march runs in the post chain, long after this block, but it must march with
        // the SAME camera the sky drew with — re-picking one down there is how the shafts end up
        // built against a different view than the sun and clouds casting them. None = no sky this
        // frame, and the pass skips entirely.
        self.godrays_frame = sky_ivp.map(|(ivp, cam_pos, cam_shadow)| GodRayFrame {
            inv_view_proj: ivp,
            cam_pos,
            csm: cam_shadow,
            terrain: shadow_mapping,
        });
        // SMK-037: the same camera, handed to the 2D pipeline so a cloudlet billboard can
        // unproject the scene depth under it and fade against the surface it is about to cut
        // into. Derived HERE, with the sky, for exactly the reason the god rays are: this is
        // the camera the visible geometry drew with, and re-picking one later drifts.
        //
        // The shader unprojects the sprite's OWN device depth with this same matrix rather
        // than trusting `rhw`, so nothing beyond the matrix and the render size is needed.
        self.soft_globals = SoftGlobals::default();
        if self.soft_enabled {
            if let Some((ivp, _, _)) = sky_ivp {
                let (rw, rh) = self.render_size();
                self.soft_globals = SoftGlobals {
                    active: true,
                    fade: self.soft_fade,
                    render: [rw as f32, rh as f32],
                    inv_view_proj: glam::Mat4::from_cols_array_2d(&ivp).to_cols_array(),
                };
            }
        }
        self.gfx2d
            .update_soft_globals(&self.queue, &self.soft_globals);
        if !self.soft_globals.active {
            // No camera, or the lever is off: unbind, so any batch the engine still marked
            // soft falls back to the plain depth-tested pipeline instead of fading against
            // a snapshot left over from an earlier frame.
            self.gfx2d.set_soft_depth(&self.device, None, 0);
            self.soft_particles.invalidate();
        }
        if let Some((ivp, cam_pos, cam_shadow)) = sky_ivp {
            self.sky.set_ocean_level(self.water.ocean_level());
            self.sky.upload(
                &self.queue,
                &self.sky_params,
                ivp,
                cam_pos,
                &shadow_mapping,
                &cam_shadow,
            );
            // Rebuild the transmittance + multiscatter LUTs first if the atmosphere
            // changed (no-op most frames), then draw the fullscreen sky.
            self.gpu_timers.begin(&mut encoder, TimerRegion::SkyLuts);
            self.sky.render_luts(&mut encoder);
            self.gpu_timers.end(&mut encoder, TimerRegion::SkyLuts);
            // Fill the aerial-perspective froxel volume from the fresh uniform + LUTs, now
            // occluded by the terrain sun-shadow mask (far) + cascade shadow map (near) so
            // the sun can't bleed through hills OR objects, and casters carve god-ray shafts.
            self.gpu_timers.begin(&mut encoder, TimerRegion::SkyFroxel);
            self.sky.render_froxel(
                &self.device,
                &mut encoder,
                &shadow_mask_view,
                self.gfx3d.shadow_sample_view(),
            );
            self.gpu_timers.end(&mut encoder, TimerRegion::SkyFroxel);
            // Bake the disc-free sky reflection env map (equirect) from the fresh uniform + LUTs, so
            // the water surface can reflect this frame's sky (Stage 4a). Cheap; before the sky pass.
            self.gpu_timers.begin(&mut encoder, TimerRegion::SkyEnv);
            self.sky.render_env(&mut encoder);
            self.gpu_timers.end(&mut encoder, TimerRegion::SkyEnv);
            // Project the env map into SH-9 diffuse sky irradiance (object + terrain ambient). Reads
            // the fresh env; the camera group already binds the resulting buffer (frame binding 9).
            self.gpu_timers.begin(&mut encoder, TimerRegion::SkySh);
            self.sky.render_sh(&mut encoder);
            self.gpu_timers.end(&mut encoder, TimerRegion::SkySh);
            let fog_cam=terrain_batches.first().map(|b|b.camera as usize)
                .or_else(||draws3d.first().map(|d|d.camera as usize)).unwrap_or(main_scene_cam);
            if let Some(camera)=prepared_cameras.get(fog_cam) {
                let below_clouds=self.layer_fog_settings.layers.iter().all(|l|
                    l[1]+l[2]<self.sky_params.cloud0[2]) || self.sky_params.cloud0[0]<=0.0;
                let cover=self.gfx3d.layer_fog_weather_inputs();
                let fog_environment=self.sky.env_view().clone();
                let fog_sky_sh=self.sky.sh_buffer().clone();
                let fog_clouds=self.sky.cloud_shadow_view().clone();
                let fog_ocean=if water_nodes.iter().any(|n|n.body==0.0){self.water.ocean_level()}else{None};
                self.gpu_timers.begin(&mut encoder,TimerRegion::LayeredFog);
                self.sky.fog.upload_local_lights(&self.queue, lights, camera);
                self.sky.fog.render(&self.device,&self.queue,&mut encoder,&self.layer_fog_settings,
                    self.layer_fog_weather,self.hdr_enabled,self.cam_underground,camera,ivp,
                    &conform_params,&heightmap_view,heightmap_gen,&shadow_mapping,&shadow_mask_view,
                    self.gfx3d.shadow_sample_view(),&fog_environment,&fog_sky_sh,&fog_clouds,cover,below_clouds,[self.sky_params.cloud0[0],self.sky_params.cloud0[2]],self.terrain.native_height_bounds(),fog_ocean);
                self.gpu_timers.end(&mut encoder,TimerRegion::LayeredFog);
            }
            self.sky.update_fog_chroma(&self.queue,self.sky.fog.is_ready(),self.layer_fog_weather);
            self.gpu_timers.begin(&mut encoder, TimerRegion::SkyDraw);
            encoder.push_debug_group("wgr_sky");
            let mut pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
                label: Some("wgr_sky"),
                color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                    view: &scene_view,
                    depth_slice: None,
                    resolve_target: None,
                    ops: wgpu::Operations {
                        load: wgpu::LoadOp::Clear(wgpu::Color {
                            r: clear_rgb[0] as f64,
                            g: clear_rgb[1] as f64,
                            b: clear_rgb[2] as f64,
                            a: clear[3] as f64,
                        }),
                        store: wgpu::StoreOp::Store,
                    },
                })],
                depth_stencil_attachment: None,
                timestamp_writes: None,
                occlusion_query_set: None,
                multiview_mask: None,
            });
            self.sky.render(&mut pass);
            self.sky.fog.render_background(&mut pass);
            drop(pass);
            encoder.pop_debug_group();
            self.gpu_timers.end(&mut encoder, TimerRegion::SkyDraw);
        }

        // The underwater volume is genuinely view-local work, so keep it completely
        // dormant above water. The FFT has already evolved and both shadow systems have
        // already rendered on this encoder; wgpu inserts the required compute-read barriers.
        if let (Some(time), Some(underwater), Some((fft_dynamics, fft_auxiliary))) = (
            underwater_time,
            self.water.underwater(),
            self.water.underwater_fft_views(),
        ) {
            let view = self.underwater_view;
            underwater.upload(
                &self.queue,
                time,
                view.cam_above,
                view.camera_pos,
                view.inv_view_proj,
                view.shallow_color_ext,
                view.deep_color,
                view.sun_dir,
                view.sun_radiance,
                view.cascade_lengths,
                view.active_layers,
                view.warp_amp,
                view.sea_level,
                view.debug_view,
                view.wave_scale,
                view.body_wave_scale,
                view.body_ellipse,
                view.body_frame,
                view.density,
                view.color_bias,
                view.caustic_gain,
                &shadow_mapping,
                &view.camera_shadow,
            );
            self.gpu_timers
                .begin(&mut encoder, TimerRegion::UnderwaterFroxel);
            underwater.render_froxel(
                &self.device,
                &mut encoder,
                &shadow_mask_view,
                self.gfx3d.shadow_sample_view(),
            );
            self.gpu_timers
                .end(&mut encoder, TimerRegion::UnderwaterFroxel);
            self.gpu_timers.begin(&mut encoder, TimerRegion::Caustics);
            underwater.render_caustics(
                &self.device,
                &mut encoder,
                &fft_dynamics,
                &fft_auxiliary,
            );
            self.gpu_timers.end(&mut encoder, TimerRegion::Caustics);
        }

        // Fog is now applied per-fragment in the forward shaders by sampling the aerial
        // froxel volume (filled above), so there is no deferred fog pass between the 3D
        // and 2D sub-passes — the 2D overlays simply never sample it.

        // `target` is where the current phase's segments render (HDR then swapchain);
        // `display_2d` picks the swapchain-format 2D pipelines in the UI phase.
        let mut target = scene_view.clone();
        let mut display_2d = false;
        // If the sky filled the target, segments load over it; else the first clears.
        let mut clear_color_next = !sky_drawn;
        let mut resolved = false;
        let mut start = 0usize;
        let mut seg_idx = 0usize;
        // Only the opt-in COUNT request consumes this witness. A cull dispatch alone
        // cannot prove that the main world colour draw reached a closed pass.
        let mut main_world_draw_closed = false;

        // Aerial perspective is a DEFERRED pass over the scene depth, so it must run after
        // all foggable 3D world geometry but before the 2D overlays (HUD / sights / scope),
        // which have no world depth and must never be fogged. 2D and 3D draws are
        // interleaved in the stream and which 2D draws exist changes frame to frame
        // (markers, icons), so a "split at the first 2D op" is unstable — it would strand
        // every later 3D object in the un-fogged tail, and flicker as those 2D draws come
        // and go. Instead PARTITION each segment: replay all non-2D ops, fog, then replay
        // all 2D ops. `want_2d` selects which side to draw (order preserved within each);
        // `display_2d` is threaded as a param (not captured) so the outer mutable changes.
        // `depth_write_off` = this is the prepassed segment's colour pass, so its opaque
        // set (objects + terrain) draws over the already-complete depth GreaterEqual/
        // write-off. False for post-ClearDepth segments (no prepass) and the 2D sub-pass.
        static CUTOUT_REPLAY: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
        let cutout_replay = *CUTOUT_REPLAY.get_or_init(||
            std::env::var("WGR_EARLY_LATE_OPAQUE").is_ok_and(|v| v == "2"));
        let render_ops = |renderer: &Renderer,
                          pass: &mut wgpu::RenderPass<'_>,
                          sub: &[Plan3dOp],
                          display_2d: bool,
                          want_2d: bool,
                          depth_write_off: bool,
                          prepassed_filter: Option<bool>,
                          water_opaque_filter: Option<bool>| {
            let mut st3d = crate::gfx3d::Pass3dState::default();
            for op in sub {
                if matches!(op, Plan3dOp::Draw2D(_)) != want_2d {
                    continue;
                }
                if let Some(want_prepassed) = prepassed_filter {
                    let is_prepassed = match op {
                        Plan3dOp::Draw3D { draw, .. } => draws3d.get(*draw as usize)
                            .is_some_and(|d| crate::gfx3d::draw_replays_before_rays(d, cutout_replay)),
                        _ => false,
                    };
                    if is_prepassed != want_prepassed { continue; }
                }
                if let Some(want_water_opaque) = water_opaque_filter {
                    let is_water_opaque = match op {
                        Plan3dOp::Draw3D { draw, .. } => draws3d.get(*draw as usize)
                            .is_some_and(crate::gfx3d::draw_replays_before_water),
                        _ => false,
                    };
                    if is_water_opaque != want_water_opaque { continue; }
                }
                match op {
                    Plan3dOp::Draw2D(arg) => {
                        st3d = crate::gfx3d::Pass3dState::default();
                        if let Some(b) = batches.get(*arg as usize) {
                            renderer
                                .gfx2d
                                .draw_one(&renderer.device, pass, &renderer.textures, b, display_2d);
                        }
                    }
                    Plan3dOp::Draw3D {
                        draw,
                        base,
                        count,
                        kind,
                    } => {
                        if let Some(d) = draws3d.get(*draw as usize) {
                            // PERF-005: counted in the COLOUR replay only. The prepass replay is
                            // a separate match arm above; counting both would double every
                            // figure while describing one set of objects.
                            let mut ds = renderer.direct_draws.get();
                            if matches!(kind, crate::gfx3d::DrawKind::Indirect(_)) {
                                ds.indirect_calls += 1;
                            } else {
                                ds.direct_calls += 1;
                                ds.instances += *count;
                                ds.tris += (d.index_count / 3) * (*count).max(1);
                            }
                            renderer.direct_draws.set(ds);
                            let mode = crate::gfx3d::Pass3dMode::Color { depth_write_off };
                            if let crate::gfx3d::DrawKind::Indirect(off) = kind {
                                renderer.gfx3d.draw_indirect(
                                    pass,
                                    &renderer.textures,
                                    d,
                                    *off,
                                    &mut st3d,
                                    mode,
                                );
                            } else {
                                renderer.gfx3d.draw_one(
                                    pass,
                                    &renderer.textures,
                                    d,
                                    *base,
                                    *count,
                                    &mut st3d,
                                    mode,
                                );
                            }
                        }
                    }
                    Plan3dOp::Terrain(arg) => {
                        st3d = crate::gfx3d::Pass3dState::default();
                        if let (Some(b), Some(cam)) = (
                            terrain_batches.get(*arg as usize),
                            renderer.gfx3d.camera_bind(),
                        ) {
                            let off = (b.camera as u64 * renderer.gfx3d.camera_stride()) as u32;
                            let kind = if depth_write_off {
                                crate::terrain::TerrainPass::ColorNoWrite
                            } else {
                                crate::terrain::TerrainPass::Color
                            };
                            renderer
                                .gpu_timers
                                .begin_pass(pass, TimerRegion::TerrainColor);
                            renderer
                                .terrain
                                .draw(pass, cam, off, b.first_node, b.node_count, kind);
                            renderer
                                .gpu_timers
                                .end_pass(pass, TimerRegion::TerrainColor);
                        }
                    }
                    Plan3dOp::Grass(arg) => {
                        st3d = crate::gfx3d::Pass3dState::default();
                        if let (Some(b), Some(cam)) = (
                            grass_batches.get(*arg as usize),
                            renderer.gfx3d.camera_bind(),
                        ) {
                            let off = (b.camera as u64 * renderer.gfx3d.camera_stride()) as u32;
                            let kind = if depth_write_off {
                                GrassPass::ColorNoWrite
                            } else {
                                GrassPass::Color
                            };
                            renderer
                                .grass
                                .draw(pass, cam, off, kind, &renderer.gpu_timers);
                        }
                    }
                    // Water is drawn in a dedicated pass after this sub-pass (it samples the
                    // opaque depth it also depth-tests against, which needs a read-only depth
                    // attachment the shared colour sub-pass can't give). Skipped here.
                    Plan3dOp::Water(_) => {}
                    Plan3dOp::ClearDepth | Plan3dOp::Resolve => {}
                }
            }
        };

        // MAT-052 diagnostic (temporary): WGR_PLAN_TRACE=1 prints one compact line per
        // frame describing the plan's segment structure -- op kinds in order, run-length
        // collapsed -- because every theory about WHERE the cockpit draws execute relative
        // to the cloud composite has died on assumption. Print the fact instead.
        {
            static PLAN_TRACE: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
            let on = *PLAN_TRACE.get_or_init(|| {
                std::env::var("WGR_PLAN_TRACE")
                    .map(|v| v != "0")
                    .unwrap_or(false)
            });
            static PRINTED: std::sync::atomic::AtomicU32 = std::sync::atomic::AtomicU32::new(0);
            let interesting = ops
                .iter()
                .any(|o| matches!(o, Plan3dOp::ClearDepth | Plan3dOp::Water(_)));
            if on && interesting && PRINTED.fetch_add(1, std::sync::atomic::Ordering::Relaxed) < 5 {
                let mut desc = String::new();
                let mut last = "";
                let mut run = 0usize;
                for op in ops.iter() {
                    let k = match op {
                        Plan3dOp::ClearDepth => "CLEARDEPTH",
                        Plan3dOp::Draw2D(_) => "2d",
                        Plan3dOp::Terrain(_) => "terr",
                        Plan3dOp::Water(_) => "WATER",
                        Plan3dOp::Grass(_) => "grass",
                        Plan3dOp::Draw3D { .. } => "3d",
                        Plan3dOp::Resolve => "RESOLVE",
                    };
                    if k == last {
                        run += 1;
                    } else {
                        if run > 0 {
                            desc.push_str(&format!("{last}x{run} "));
                        }
                        last = k;
                        run = 1;
                    }
                }
                if run > 0 {
                    desc.push_str(&format!("{last}x{run}"));
                }
                eprintln!("[wgr] PLAN: {desc}");
            }
        }
        loop {
            let end = ops[start..]
                .iter()
                .position(|o| matches!(o, Plan3dOp::ClearDepth | Plan3dOp::Resolve))
                .map(|p| start + p)
                .unwrap_or(ops.len());

            let color_load = if clear_color_next {
                wgpu::LoadOp::Clear(wgpu::Color {
                    r: clear_rgb[0] as f64,
                    g: clear_rgb[1] as f64,
                    b: clear_rgb[2] as f64,
                    a: clear[3] as f64,
                })
            } else {
                wgpu::LoadOp::Load
            };
            clear_color_next = false;

            let seg_label = format!("wgr_segment_{seg_idx}");
            seg_idx += 1;
            let seg_ops = &ops[start..end];
            let has_2d = seg_ops.iter().any(|o| matches!(o, Plan3dOp::Draw2D(_)));
            // This segment is about to write depth (prepass and/or colour sub-pass), so any
            // far-resolve from an earlier segment is now stale. Cleared HERE rather than after the
            // passes so an early `continue`/`break` cannot leave a stale resolve marked current.
            self.far_depth_resolve_current = false;
            // ...and so is the frame-final claim. The cloud block sets it with the comment "all
            // 3D depth is written by now", which is TRUE for the world segment and FALSE for the
            // frame: the post-ClearDepth cockpit segment rewrites depth after it. The god-ray
            // march trusted the stale claim, read SKY depth at every canopy-frame pixel, and
            // additively painted full-length sun shafts over the airframe -- the owner's
            // "cockpit is transparent, but only where the sky is behind it; looking down it is
            // fine", on the stock T72 and UH-60 alike (MAT-052, third and final mechanism).
            // Clearing it here makes the god-ray pass re-resolve AFTER the last depth writer, so
            // the shafts stop at the cockpit like they stop at everything else.
            self.frame_depth_final_resolved = false;

            // Depth+normal prepass over the FIRST (world) depth segment only
            // (docs/depth-prepass-plan.md, decision 5): start == 0 marks it. The prepass
            // replays the segment's opaque set (objects self-filter; terrain always) into
            // the normal G-buffer + depth (cleared 0.0 reversed-Z, stencil cleared 0). The
            // colour sub-pass below then LOADS this depth and draws the opaque set early-Z
            // with depth-write off. Later segments (near/weapon) keep the single-pass path.
            let do_prepass = start == 0;
            if let (true, Some(normal_view)) = (do_prepass, normal.as_ref()) {
                self.gpu_timers
            .cpu_end(crate::gpu_timers::Region::WorkerEncSky);
        self.gpu_timers
            .cpu_begin(crate::gpu_timers::Region::WorkerEncDraw);
        encoder.push_debug_group("wgr_depth_prepass");
                // PERF-005 CONTAINER region: the whole depth+normal prepass, so the leaves
                // inside it (terrain prepass, grass prepass, the GPU-driven solid/alpha
                // variants) can be subtracted to expose what the CPU-replayed Draw3D ops cost.
                // Containers OVERLAP their leaves by construction — the C++ reporter never sums
                // the two together, and neither should any later reader of these numbers.
                self.gpu_timers.cpu_begin(TimerRegion::ObjPrepassSegment);
                self.gpu_timers
                    .begin(&mut encoder, TimerRegion::ObjPrepassSegment);
                let mut pp = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
                    label: Some("wgr_depth_prepass"),
                    color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                        view: normal_view,
                        depth_slice: None,
                        resolve_target: None,
                        ops: wgpu::Operations {
                            load: wgpu::LoadOp::Clear(wgpu::Color::TRANSPARENT),
                            store: wgpu::StoreOp::Store,
                        },
                    })],
                    depth_stencil_attachment: Some(wgpu::RenderPassDepthStencilAttachment {
                        view: &depth,
                        depth_ops: Some(wgpu::Operations {
                            // Reversed-Z: far plane is 0
                            load: wgpu::LoadOp::Clear(0.0),
                            store: wgpu::StoreOp::Store,
                        }),
                        // Clear stencil to 0 here so the colour pass can LOAD it (the
                        // shadow-darken pass wants stencil == 0 to start).
                        stencil_ops: Some(wgpu::Operations {
                            load: wgpu::LoadOp::Clear(0),
                            store: wgpu::StoreOp::Store,
                        }),
                    }),
                    timestamp_writes: None,
                    occlusion_query_set: None,
                    multiview_mask: None,
                });
                let mut st3d = crate::gfx3d::Pass3dState::default();
                for op in seg_ops {
                    match op {
                        Plan3dOp::Draw3D {
                            draw,
                            base,
                            count,
                            kind,
                        } => {
                            if let Some(d) = draws3d.get(*draw as usize) {
                                let mode = crate::gfx3d::Pass3dMode::Prepass;
                                if let crate::gfx3d::DrawKind::Indirect(off) = kind {
                                    self.gfx3d.draw_indirect(
                                        &mut pp,
                                        &self.textures,
                                        d,
                                        *off,
                                        &mut st3d,
                                        mode,
                                    );
                                } else {
                                    self.gfx3d.draw_one(
                                        &mut pp,
                                        &self.textures,
                                        d,
                                        *base,
                                        *count,
                                        &mut st3d,
                                        mode,
                                    );
                                }
                            }
                        }
                        Plan3dOp::Terrain(arg) => {
                            st3d = crate::gfx3d::Pass3dState::default();
                            if let (Some(b), Some(cam)) =
                                (terrain_batches.get(*arg as usize), self.gfx3d.camera_bind())
                            {
                                let off = (b.camera as u64 * self.gfx3d.camera_stride()) as u32;
                                self.gpu_timers
                                    .begin_pass(&mut pp, TimerRegion::TerrainPrepass);
                                self.terrain.draw(
                                    &mut pp,
                                    cam,
                                    off,
                                    b.first_node,
                                    b.node_count,
                                    crate::terrain::TerrainPass::Prepass,
                                );
                                self.gpu_timers
                                    .end_pass(&mut pp, TimerRegion::TerrainPrepass);
                            }
                        }
                        Plan3dOp::Grass(arg) => {
                            st3d = crate::gfx3d::Pass3dState::default();
                            if let (Some(b), Some(cam)) =
                                (grass_batches.get(*arg as usize), self.gfx3d.camera_bind())
                            {
                                let off = (b.camera as u64 * self.gfx3d.camera_stride()) as u32;
                                self.grass.draw(
                                    &mut pp,
                                    cam,
                                    off,
                                    GrassPass::Prepass,
                                    &self.gpu_timers,
                                );
                            }
                        }
                        _ => {}
                    }
                }
                // GPU-driven opaque set into the SAME depth+normal prepass (reuses this
                // frame's cull out_args — the cull dispatch already ran before the passes).
                // Writes depth + view-space normals so the set gets early-Z + SSAO normals.
                if !suppress_world_objects {
                    let cam_off = (main_scene_cam as u64 * self.gfx3d.camera_stride()) as u32;
                    self.gfx3d.draw_gpu_driven_prepass(
                        &mut pp,
                        &self.textures,
                        cam_off,
                        &self.gpu_timers,
                    );
                }
                drop(pp);
                self.gpu_timers
                    .end(&mut encoder, TimerRegion::ObjPrepassSegment);
                self.gpu_timers.cpu_end(TimerRegion::ObjPrepassSegment);
                encoder.pop_debug_group();
            }
            // When the prepass ran, the depth (+ stencil 0) it wrote is complete, so the
            // colour sub-pass LOADS it; otherwise it clears as before.
            let prepassed = do_prepass && normal.is_some();

            // GPU Hi-Z occlusion (docs/gpu-culling-and-depth-plan.md §5): now that the prepass
            // depth is complete (terrain + CPU objects + the GPU-driven set), reduce it to a Hi-Z
            // pyramid and run the color-pass occlusion cull. Both no-op unless occlusion is active.
            // Recorded between the prepass and colour passes so wgpu barriers depth-write -> Hi-Z
            // read -> color-cull sample -> the colour draw's indirect read. The colour draw
            // (draw_gpu_driven, below) then consumes the occlusion-culled args.
            if prepassed {
                // PERF-005: the Hi-Z pyramid reduction and the occlusion cull that consumes it
                // are one logical cost — you cannot have the second without the first — so they
                // share one encoder-level region rather than reporting two numbers nobody can
                // act on independently.
                self.gpu_timers.cpu_begin(TimerRegion::ObjCullColor);
                self.gpu_timers
                    .begin(&mut encoder, TimerRegion::ObjCullColor);
                self.gfx3d.build_hiz(&self.device, &mut encoder);
                self.gfx3d.cull_dispatch_color(&mut encoder);
                self.gpu_timers.end(&mut encoder, TimerRegion::ObjCullColor);
                self.gpu_timers.cpu_end(TimerRegion::ObjCullColor);
                // Screen-space AO (docs/screen-space-ao-plan.md): the normal resolve, the GTAO
                // compute and its bilateral denoise, all off the prepass depth+normal. Recorded
                // here — after the prepass has completed the depth buffer, before the colour
                // sub-pass below samples the AO through frame @binding(11). Off by default;
                // no-op until the gate is on. main_scene_cam is the camera the prepass drew
                // with, so it is the one whose unprojection matches this depth buffer.
                // The one-shot flag is set only AFTER a successful log, not before the camera
                // lookup. Burning it on an early frame that has no camera yet is how a
                // "one-shot" diagnostic silently never fires — which is exactly what the first
                // version of this did, and it cost a whole launch to notice.
                if self.gfx3d.gtao_debug_on() && !self.gtao_dbg_logged {
                    if let Some(cam) = cameras.get(main_scene_cam) {
                        let proj = glam::DMat4::from_cols_array(&cam.proj.map(f64::from));
                        let inv_proj = proj.inverse().as_mat4();
                        // Reproduce the shader's own arithmetic at the screen centre for a few
                        // depths, so a bad projection shows up as a nonsense view-space Z or a
                        // pixel radius pinned to its 2.0 floor (which reads as "no AO anywhere").
                        let (w, h) = self.gfx3d.render_size();
                        let mut report = String::new();
                        // STORED (reversed-Z) depths: 1 = near, 0 = far/sky. Spread over the
                        // range so the printed distances span near field to horizon.
                        for d in [0.5_f32, 0.1, 0.01, 0.001] {
                            // 1.0 - d, matching view_pos in gtao.wgsl (see its note on why).
                            let hp = inv_proj * glam::Vec4::new(0.0, 0.0, 1.0 - d, 1.0);
                            let p = hp.truncate() / hp.w.abs().max(1e-6) * hp.w.signum();
                            let dist = p.length().max(1e-3);
                            // Read the LIVE tuning, not literals: this line exists to show what
                            // the shader will actually do, and hardcoded values silently go stale
                            // the moment a default moves (they already did once).
                            let g = self.gfx3d.gtao_settings();
                            let px_r = (g.radius_m / dist * cam.proj[5] * h as f32 * 0.5)
                                .clamp(2.0, g.max_radius_px.max(2.0));
                            report.push_str(&format!(
                                " | d={d} -> viewZ={:.2} dist={:.2} px_radius={px_r:.1}",
                                p.z, dist
                            ));
                        }
                        self.log.log(
                            log_level::INFO,
                            &format!(
                                "[wgr] gtao inputs: {w}x{h} proj_yy={:.4} proj_xx={:.4} radius={:.2}m cap={:.0}px{report}",
                                cam.proj[5],
                                cam.proj[0],
                                self.gfx3d.gtao_settings().radius_m,
                                self.gfx3d.gtao_settings().max_radius_px,
                            ),
                        );
                        self.gtao_dbg_logged = true;
                    }
                }
                self.gfx3d.render_gtao(
                    &self.device,
                    &self.queue,
                    &mut encoder,
                    main_scene_cam,
                    &self.gpu_timers,
                );
            }

            // Depth attachment for this segment's sub-passes. Post-tonemap (resolved) the target is
            // the 1x swapchain, so under MSAA attach the single-sample UI depth instead of the
            // multisampled scene depth (matching sample counts). Pre-resolve, and always at 1x,
            // it's the scene depth.
            let seg_depth: &wgpu::TextureView = if resolved {
                ui_depth.as_ref().unwrap_or(&depth)
            } else {
                &depth
            };

            // WATER vs. THE TRANSPARENT SET. The engine draws in a definite order:
            // Landscape::Draw (which submits the sea) runs to completion and is
            // flushed BEFORE Scene::ObjectsDrawn, which is where the sorted
            // transparent objects, the cloudlets and the rain streaks go. The
            // renderer used to ignore that: it hoisted every Water op out of the
            // colour sub-pass to a dedicated pass at the END of the segment,
            // because water samples the opaque depth it also tests against and
            // therefore needs a READ-ONLY depth attachment the shared sub-pass
            // cannot give. The side effect was that the sea composited over
            // everything alpha-blended in front of it -- rain streaks falling
            // into it, spray, smoke, glass, the alpha halves of trees -- none of
            // which write depth, so nothing stopped the water covering them.
            //
            // So the sub-pass breaks where the op stream says it should: draws
            // recorded before the last Water op, then water in its own pass, then
            // the rest in a second colour sub-pass that loads what water left.
            // The plan's order is preserved exactly; only the pass boundary moved.
            let water_split = if resolved {
                None
            } else {
                seg_ops
                    .iter()
                    .rposition(|o| matches!(o, Plan3dOp::Water(_)))
                    .map(|i| i + 1)
            };
            let (ops_pre_water, ops_post_water) = match water_split {
                Some(i) => seg_ops.split_at(i),
                None => (seg_ops, &seg_ops[seg_ops.len()..]),
            };
            // Classic transparent sorting can submit promoted cutouts/opaque objects
            // after Water. Their prepass depth already occludes the atmosphere, but a
            // late colour write would erase its scattering. Replay that exact set before shafts;
            // never move glass, decals, particles or post-ClearDepth cockpit segments.
            static EARLY_OPAQUE: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
            let early_opaque = start == 0 && prepassed && water_split.is_some()
                && *EARLY_OPAQUE.get_or_init(||
                    std::env::var("WGR_EARLY_LATE_OPAQUE").is_ok_and(|v| v == "1" || v == "2"))
                && ops_post_water.iter().any(|op| match op {
                    Plan3dOp::Draw3D { draw, .. } => draws3d.get(*draw as usize)
                        .is_some_and(|d| crate::gfx3d::draw_replays_before_rays(d, cutout_replay)),
                    _ => false,
                });
            // Diagnose colour writes after atmospheric compositing without changing order.
            // Sample throughout a run: its first frames can belong to the loading/menu scene.
            if start == 0 {
                static TRACE: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
                static FRAMES: std::sync::atomic::AtomicU32 = std::sync::atomic::AtomicU32::new(0);
                if *TRACE.get_or_init(|| std::env::var("WGR_LATE_OBJECT_TRACE").is_ok_and(|v| v == "1"))
                    && FRAMES.fetch_add(1, std::sync::atomic::Ordering::Relaxed) % 300 == 0
                {
                    let mut opaque = 0;
                    let mut cutout = 0;
                    let mut alpha = 0;
                    let mut other = 0;
                    for op in ops_post_water {
                        if let Plan3dOp::Draw3D { draw, .. } = op {
                            if let Some(d) = draws3d.get(*draw as usize) {
                                if d.blend == ffi::WgrBlend::Opaque && d.depth == ffi::WgrDepthMode::TestWrite {
                                    if d.alpha_ref > 0.0 { cutout += 1; } else { opaque += 1; }
                                } else if d.blend == ffi::WgrBlend::Alpha {
                                    alpha += 1;
                                } else { other += 1; }
                            }
                        }
                    }
                    eprintln!("[wgr] late objects: opaque={opaque} cutout={cutout} alpha={alpha} other={other}");
                }
            }
            // Draw2D is replayed by the dedicated 2D sub-pass further down (this
            // pass runs with want_2d = false), so a tail of nothing but 2D ops is
            // not worth a render pass of its own.
            let has_post_water_3d = ops_post_water.iter().any(|o| {
                !matches!(
                    o,
                    Plan3dOp::Draw2D(_) | Plan3dOp::ClearDepth | Plan3dOp::Resolve
                )
            });
            // Hoisted out of the sub-pass block: the container now closes after the
            // SECOND colour sub-pass, so the transparent replay that moved behind
            // water is still inside the number that is supposed to account for it.
            // On a water frame it therefore also spans the water pass, which keeps
            // its own WaterDraw row -- containers overlap their leaves here by
            // construction and the C++ reporter never sums the two.
            let time_segment = start == 0;

            // 3D sub-pass: all non-2D draws. Depth/stencil are cleared here only when the
            // prepass didn't already fill them (stencil to 0 so shadow draws EQUAL 0 /
            // INCR darken each pixel once); colour per the load.
            let mut main_world_draw_recorded = false;
            {
                // PERF-005 CONTAINER: the WORLD 3D colour sub-pass. Its leaves are the terrain
                // colour, grass colour and the two GPU-driven colour variants; container minus
                // leaves is what the CPU-replayed Draw3D ops (transparent, skinned, decal and
                // any indirect-eligible fallback) actually cost. Measured this way rather than
                // per-op because one region can only hold ONE begin/end pair per frame — timing
                // each of hundreds of Draw3D ops would report only the last one's cost.
                //
                // Gated on `start == 0` for exactly that reason. This block is re-entered once
                // per plan segment, and an ungated bracket lets the LAST segment win the query
                // slot — which is the post-Resolve UI segment. Measured on Stratis that reported
                // 0.004 ms for a sub-pass containing 0.4 ms of terrain alone, i.e. a container
                // smaller than its own contents. Same gate the GPU-driven colour draw uses, so
                // the container and its leaves always describe the same pass.
                if time_segment {
                    self.gpu_timers.cpu_begin(TimerRegion::ObjColorSegment);
                    self.gpu_timers
                        .begin(&mut encoder, TimerRegion::ObjColorSegment);
                }
                let mut pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
                    label: Some(&seg_label),
                    color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                        view: &target,
                        depth_slice: None,
                        resolve_target: None,
                        ops: wgpu::Operations {
                            load: color_load,
                            store: wgpu::StoreOp::Store,
                        },
                    })],
                    depth_stencil_attachment: Some(wgpu::RenderPassDepthStencilAttachment {
                        view: seg_depth,
                        depth_ops: Some(wgpu::Operations {
                            load: if prepassed {
                                wgpu::LoadOp::Load
                            } else {
                                // Reversed-Z: far plane is 0
                                wgpu::LoadOp::Clear(0.0)
                            },
                            store: wgpu::StoreOp::Store,
                        }),
                        stencil_ops: Some(wgpu::Operations {
                            load: if prepassed {
                                wgpu::LoadOp::Load
                            } else {
                                wgpu::LoadOp::Clear(0)
                            },
                            store: wgpu::StoreOp::Store,
                        }),
                    }),
                    timestamp_writes: None,
                    occlusion_query_set: None,
                    multiview_mask: None,
                });
                // GPU-driven opaque world objects (Stage 3), in the world segment only
                // (start == 0). Drawn BEFORE the CPU ops: it is depth-tested opaque so its order
                // vs the CPU OPAQUE set is irrelevant, but the CPU set also contains alpha-BLENDED
                // draws (fences, glass) that read the framebuffer colour — those must blend
                // against the GPU-driven objects behind them, not the background, so the
                // GPU-driven colour has to be present first (else the sky shows through).
                if start == 0 && !suppress_world_objects {
                    let cam_off = (main_scene_cam as u64 * self.gfx3d.camera_stride()) as u32;
                    main_world_draw_recorded = self.gfx3d.draw_gpu_driven(
                        &mut pass,
                        &self.textures,
                        cam_off,
                        &self.gpu_timers,
                    );
                    // FAR INSTANCE TIER, immediately after the real objects and before the
                    // CPU-replayed ops. After, because the two overlap in the transition band
                    // and the real geometry should win the depth test on equal terms rather
                    // than by draw order; before render_ops for the reason the GPU-driven set
                    // is there — the CPU set contains alpha-BLENDED draws that read the
                    // framebuffer, and they must blend against whatever is actually behind
                    // them. Its own gate is inside: no proxies, or the tier switched off, and
                    // this costs one branch.
                    if let Some(camera_bind) = self.gfx3d.camera_bind() {
                        self.far
                            .draw(&mut pass, camera_bind, cam_off, &self.gpu_timers);
                    }
                }
                // CONTAINER: this replay also draws terrain and grass, which have their own
                // regions. Its point is the CPU-replayed direct draws, which cannot have one
                // (Plan3dOp::Draw3D is per-draw), so they read as this minus those two.
                //
                // Gated on `time_segment` for the reason spelled out at the segment container
                // above: this block runs once per plan segment and an ungated bracket lets the
                // LAST one -- the post-Resolve UI segment -- win the single query slot. Left
                // ungated it reported 0.000 ms for a replay containing 3.8 ms of grass.
                if time_segment {
                    self.gpu_timers
                        .begin_pass(&mut pass, TimerRegion::OpsPreWater);
                }
                render_ops(self, &mut pass, ops_pre_water, display_2d, false, prepassed, None, None);
                // Solid vehicle/object colour can arrive in the legacy sorted tail even though
                // its depth was prepassed. Draw that subset before transparent water so the sea
                // covers the submerged hull and leaves a real depth-correct waterline.
                if water_split.is_some() {
                    // Also move proven ground-conformed road colour here. Its terrain
                    // receiver is already in depth, so water/clouds must composite over
                    // it rather than have the late alpha queue repaint roads in the sky.
                    render_ops(
                        self,
                        &mut pass,
                        ops_post_water,
                        display_2d,
                        false,
                        true,
                        None,
                        Some(true),
                    );
                }
                if time_segment {
                    self.gpu_timers
                        .end_pass(&mut pass, TimerRegion::OpsPreWater);
                }
                // Debug cull-sphere wireframes (ImGui Culling tab) LAST in the sub-pass: their
                // depth test is Always, but anything drawn after them (terrain in render_ops)
                // would still overwrite their colour — so they must follow every world draw to
                // actually show on top. When a post-water sub-pass follows, that one is the last
                // world draw and they belong there instead.
                if start == 0
                    && !suppress_world_objects
                    && self.cull_debug_draw
                    && !has_post_water_3d
                {
                    let cam_off = (main_scene_cam as u64 * self.gfx3d.camera_stride()) as u32;
                    self.gfx3d.draw_cull_spheres(&mut pass, cam_off);
                }
                drop(pass);
                if main_count_armed && main_world_draw_recorded {
                    main_world_draw_closed = true;
                }
            }

            // Simulation-owned land water is independent of whether the ocean
            // quadtree contributed a Water op. Draw once in the world segment,
            // after opaque colour and before ocean/transparent sorted tails.
            if start == 0 && self.rain_water.drawable() {
                // Private current-world snapshot BEFORE land water. Ocean/monitor
                // snapshots below retain their original content and ordering.
                if self.rain_water.reflections_needed() && self.hdr_enabled {
                    self.ensure_far_depth_resolve(&mut encoder);
                    if let (Some((source, view)), Some(depth)) =
                        (self.hdr.as_ref(), self.gfx3d.water_depth_view().cloned()) {
                        self.rain_water.record_reflections(&self.device, &self.queue,
                            &mut encoder, source, view, &depth, self.gfx3d.depth_gen());
                    }
                }
                if let Some(camera) = self.gfx3d.camera_bind() {
                    let mut rain_pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
                        label: Some("wgr_rain_water_pass"),
                        color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                            view: &target, depth_slice: None, resolve_target: None,
                            ops: wgpu::Operations { load: wgpu::LoadOp::Load, store: wgpu::StoreOp::Store },
                        })],
                        depth_stencil_attachment: Some(wgpu::RenderPassDepthStencilAttachment {
                            view: seg_depth, depth_ops: None, stencil_ops: None,
                        }), timestamp_writes: None, occlusion_query_set: None, multiview_mask: None,
                    });
                    let offset = (main_scene_cam as u64 * self.gfx3d.camera_stride()) as u32;
                    self.rain_water.draw(&mut rain_pass, camera, offset);
                }
            }

            // Dedicated water pass. Water is transparent and reconstructs the seabed by SAMPLING
            // the opaque prepass depth — which it also depth-tests against — so its depth
            // attachment must be READ-ONLY (depth_ops/stencil_ops = None). The shared colour
            // sub-pass can't be read-only (the GPU-driven opaque pipeline writes depth), so water
            // draws here instead, after the opaque set, loading colour + read-only depth. Water
            // still depth-tests vs the coast (GreaterEqual) and writes no depth. Pre-resolve only
            // (the world segment); the resolved MSAA depth that water samples is filled by the
            // depth-resolve run before this segment's colour pass.
            let has_water = seg_ops.iter().any(|o| matches!(o, Plan3dOp::Water(_)));
            if has_water && !resolved {
                // Water reconstructs the seabed from the FARTHEST-sample depth resolve (not the
                // Hi-Z near resolve): a nearest resolve reads A2C foliage/rotor edges as the seabed
                // and rings them with foam. Record that resolve, then point water at it.
                self.ensure_far_depth_resolve(&mut encoder);
                let dgen = self.gfx3d.depth_gen();
                if let Some(dv) = self.gfx3d.water_depth_view() {
                    self.water.set_depth_view(&self.device, dv, dgen);
                }
                // Freeze the completed scene before water writes `target`. Sampling this
                // separate texture is legal; sampling the active colour attachment is not.
                if let (
                    true,
                    Some((hdr_texture, hdr_view)),
                    Some((snapshot_texture, snapshot_view)),
                ) = (
                    self.hdr_enabled,
                    self.hdr.as_ref(),
                    self.water_scene.as_ref(),
                ) {
                    if self.sample_count > 1 {
                        let pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
                            label: Some("wgr_water_scene_resolve"),
                            color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                                view: hdr_view,
                                depth_slice: None,
                                resolve_target: Some(snapshot_view),
                                ops: wgpu::Operations {
                                    load: wgpu::LoadOp::Load,
                                    store: wgpu::StoreOp::Store,
                                },
                            })],
                            depth_stencil_attachment: None,
                            timestamp_writes: None,
                            occlusion_query_set: None,
                            multiview_mask: None,
                        });
                        drop(pass);
                    } else {
                        encoder.copy_texture_to_texture(
                            wgpu::TexelCopyTextureInfo {
                                texture: hdr_texture,
                                mip_level: 0,
                                origin: wgpu::Origin3d::ZERO,
                                aspect: wgpu::TextureAspect::All,
                            },
                            wgpu::TexelCopyTextureInfo {
                                texture: snapshot_texture,
                                mip_level: 0,
                                origin: wgpu::Origin3d::ZERO,
                                aspect: wgpu::TextureAspect::All,
                            },
                            wgpu::Extent3d {
                                // Scene-target size, which under render scale is NOT the
                                // swapchain size.
                                width: self.hdr_size.0,
                                height: self.hdr_size.1,
                                depth_or_array_layers: 1,
                            },
                        );
                    }
                    let scene_gen = self.hdr_size.0 as u64 ^ ((self.hdr_size.1 as u64) << 32);
                    self.water
                        .set_scene_view(&self.device, snapshot_view, scene_gen);
                }
                // Lend Sky's reflection env map to water (Stage 4a). The env texture never resizes,
                // so gen 0 binds it once; a no-op thereafter.
                self.water
                    .set_env_view(&self.device, self.sky.env_view(), 0);
                // Lend the terrain heightmap to water's vertex stage, so a wave trough cannot
                // displace the surface below the seabed and be cut away by the depth test.
                self.water.set_heightmap(
                    &self.device,
                    &self.queue,
                    &self.terrain.heightmap_view(),
                    self.terrain.heightmap_gen(),
                    &self.terrain.conform_params(),
                );
                if let Some(cam) = self.gfx3d.camera_bind() {
                    // WTR-002 — the water draw includes the in-shader SSR + refraction cost
                    // (they are fragment work, not separable passes; see gpu_timers.rs).
                    self.gpu_timers.begin(&mut encoder, TimerRegion::WaterDraw);
                    encoder.push_debug_group("wgr_water");
                    let visibility_query = self.water_visibility.claim();
                    let mut wpass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
                        label: Some("wgr_water_pass"),
                        color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                            view: &target,
                            depth_slice: None,
                            resolve_target: None,
                            ops: wgpu::Operations {
                                load: wgpu::LoadOp::Load,
                                store: wgpu::StoreOp::Store,
                            },
                        })],
                        depth_stencil_attachment: Some(wgpu::RenderPassDepthStencilAttachment {
                            view: seg_depth,
                            depth_ops: None,
                            stencil_ops: None,
                        }),
                        timestamp_writes: None,
                        occlusion_query_set: visibility_query
                            .then_some(self.water_visibility.query_set()),
                        multiview_mask: None,
                    });
                    if visibility_query {
                        wpass.begin_occlusion_query(0);
                    }
                    for op in seg_ops {
                        if let Plan3dOp::Water(arg) = op {
                            if let Some(b) = water_batches.get(*arg as usize) {
                                let off = (b.camera as u64 * self.gfx3d.camera_stride()) as u32;
                                self.water
                                    .draw(&mut wpass, cam, off, b.first_node, b.node_count);
                            }
                        }
                    }
                    if visibility_query {
                        wpass.end_occlusion_query();
                    }
                    drop(wpass);
                    if self.water.has_curling_breaker() {
                        // Folded sheets must occlude themselves. The dedicated bind group
                        // deliberately excludes scene depth, so this pass may write it safely.
                        let mut curl = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
                            label:Some("wgr_curling_breaker_pass"),
                            color_attachments:&[Some(wgpu::RenderPassColorAttachment {view:&target,depth_slice:None,
                                resolve_target:None,ops:wgpu::Operations {load:wgpu::LoadOp::Load,store:wgpu::StoreOp::Store}})],
                            depth_stencil_attachment:Some(wgpu::RenderPassDepthStencilAttachment {view:seg_depth,
                                depth_ops:Some(wgpu::Operations {load:wgpu::LoadOp::Load,store:wgpu::StoreOp::Store}),stencil_ops:None}),
                            timestamp_writes:None,occlusion_query_set:None,multiview_mask:None,
                        });
                        for op in seg_ops {
                            if let Plan3dOp::Water(arg)=op {
                                if let Some(b)=water_batches.get(*arg as usize) {
                                    if b.first_node==0 {
                                        let off=(b.camera as u64*self.gfx3d.camera_stride()) as u32;
                                        self.water.draw_curling(&mut curl,cam,off);
                                    }
                                }
                            }
                        }
                        drop(curl);
                        self.far_depth_resolve_current=false;
                        self.frame_depth_final_resolved=false;
                    }
                    encoder.pop_debug_group();
                    self.gpu_timers.end(&mut encoder, TimerRegion::WaterDraw);
                }
            }

            // Depth-aware over-scene clouds (plan Phase 1): march at LOW RES bounded by the resolved
            // scene depth, then composite over the lit scene (premultiplied blend) so clouds occlude
            // terrain and envelop the camera when flown through — not a sky-only element. World segment
            // only, HDR + coverage>0; after water so it composites over water too.
            //
            // AND BEFORE THE TRANSPARENT REPLAY BELOW, which is the same lesson the water pass
            // taught one layer down. This composite is depth-aware, and alpha-BLENDED draws write no
            // depth: for a pixel covered by an iron railing standing against the sky, the resolved
            // depth is the sky, so the march runs the full distance and `dst * trans + inscatter`
            // paints cloud straight over the railing. The owner caught it on a Nogova pier, where
            // every fence post above the horizon was eaten by the cloud deck behind it.
            //
            // Compositing here instead matches the engine's own order exactly: Landscape::Draw
            // submits terrain, water and DrawClouds, the queues are flushed, and only then does
            // Scene::ObjectsDrawn emit the sorted transparents. Clouds are background. The trade is
            // that a cloud NEARER than a transparent no longer occludes it — which in practice means
            // flying a cloud between the eye and a fence ten metres away, against every near
            // transparent in the game being erased by the sky behind it.
            // Terrain-conformed road decals are replayed before water above, and
            // excluded from the late replay: their receiver depth correctly bounds
            // this march, unlike a foreground glass/fence with no alpha depth.
            if start == 0
                && !resolved
                && sky_drawn
                && !ablate.clouds
                && self.sky.clouds_active(&self.sky_params)
                // TW-WATER W5d: not from under the sea. The composite is depth-aware against the
                // resolved scene depth, and the water surface writes none, so from below every
                // pixel of the surface whose scene behind it is sky was painted with cloud -- the
                // clouds hung in front of Snell's window and showed through total internal
                // reflection. (The refracted sky inside the window is the water shader's own.)
                && !self.submerged_view
            {
                // Ensure a resolved single-sample scene depth exists (genuinely idempotent now: the
                // water pass above already recorded it and attaches depth read-only, so the flag
                // makes this a no-op instead of a second full-res resolve). Farthest-sample, as
                // water uses.
                self.ensure_far_depth_resolve(&mut encoder);
                // All 3D depth is written by now, so this resolve is the FINAL one: the god-ray
                // march in the post chain can reuse it rather than recording its own.
                self.frame_depth_final_resolved = true;
                if let Some(depth_view) = self.gfx3d.water_depth_view().cloned() {
                    // Encoder-level brackets: render_cloud records its own passes, and the
                    // composite below opens one here. Both were unmeasured and both sit inside
                    // the colour sub-pass whose 9 ms gap this is chasing.
                    self.gpu_timers.begin(&mut encoder, TimerRegion::CloudMarch);
                    self.sky.render_cloud(
                        &self.device,
                        &mut encoder,
                        &depth_view,
                        self.render_size().0,
                        self.render_size().1,
                        false,
                        true,
                    );
                    self.gpu_timers.end(&mut encoder, TimerRegion::CloudMarch);
                    self.gpu_timers
                        .begin(&mut encoder, TimerRegion::CloudComposite);
                    encoder.push_debug_group("wgr_cloud_composite");
                    let mut cpass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
                        label: Some("wgr_cloud_composite"),
                        color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                            view: &target,
                            depth_slice: None,
                            resolve_target: None,
                            ops: wgpu::Operations {
                                load: wgpu::LoadOp::Load,
                                store: wgpu::StoreOp::Store,
                            },
                        })],
                        depth_stencil_attachment: None,
                        timestamp_writes: None,
                        occlusion_query_set: None,
                        multiview_mask: None,
                    });
                    self.sky.composite_cloud(&mut cpass);
                    drop(cpass);
                    encoder.pop_debug_group();
                    self.gpu_timers
                        .end(&mut encoder, TimerRegion::CloudComposite);
                }
            }

            if early_opaque {
                // Keep both water/refraction and cloud ordering unchanged. A short separate
                // pass is necessary: neither composite's framebuffer state belongs to objects.
                let mut pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
                    label: Some("wgr_late_opaque_before_shafts"),
                    color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                        view: &target,
                        depth_slice: None,
                        resolve_target: None,
                        ops: wgpu::Operations { load: wgpu::LoadOp::Load, store: wgpu::StoreOp::Store },
                    })],
                    depth_stencil_attachment: Some(wgpu::RenderPassDepthStencilAttachment {
                        view: seg_depth,
                        depth_ops: None,
                        stencil_ops: None,
                    }),
                    timestamp_writes: None,
                    occlusion_query_set: None,
                    multiview_mask: None,
                });
                render_ops(
                    self,
                    &mut pass,
                    ops_post_water,
                    display_2d,
                    false,
                    true,
                    Some(true),
                    Some(false),
                );
            }

            // VOLUMETRIC SHAFTS, composited HERE -- after the opaque world, after water and
            // clouds, and BEFORE the transparent draws below.
            //
            // They used to run in the post chain, after everything. That is wrong for the same
            // reason it was wrong for the cockpit: the march stops at the SCENE DEPTH, and an
            // alpha-blended surface writes no depth. So at every pixel covered by glass, a road
            // sign, or the first-person iron sight, the march read the sky BEHIND the surface and
            // additively painted a full-length shaft over it -- which against a bright sky reads
            // as the surface being transparent. The owner confirmed the mechanism by switching
            // the god rays off: sign solid, iron sight solid, everything else unchanged.
            //
            // No amount of tuning fixes that from the post chain, because the information the
            // march needs (that something opaque-looking is in front) does not exist in the
            // depth buffer and never will for a blended draw. Ordering is the fix: volumetrics
            // belong BEHIND transparents, so transparents composite over them.
            //
            // The cost is that the additive blend now runs on the MSAA target rather than the
            // resolved one, i.e. once per sample instead of once per pixel. The pass is a
            // low-resolution march upsampled by one full-screen triangle, so this is a small
            // absolute number -- and it buys the one thing the post-chain position cannot.
            //
            // Once per frame, in the world segment only: `godrays_composited` makes the post
            // chain's call a no-op rather than double-adding the shafts.
            if start == 0 && !self.godrays_composited && underwater_time.is_none() {
                self.godrays_composited = true;
                self.gpu_timers.begin(&mut encoder, TimerRegion::GodRays);
                self.render_god_rays(&mut encoder, &target);
                self.gpu_timers.end(&mut encoder, TimerRegion::GodRays);
            }

            // SMK-037: freeze the scene depth for the soft-particle fade, HERE -- after the
            // opaque world, water and clouds have all written depth, and before the
            // transparent replay below, which is where the cloudlets are. The copy exists
            // because that replay attaches depth read-write and so cannot also sample it;
            // see soft_particles.wgsl. Recorded only while the lever is on, and only in the
            // world segment (a post-ClearDepth weapon segment has its own depth, and no
            // smoke).
            // SMK-038: the volumetric smoke, composited HERE for exactly the reasons the
            // clouds and the god rays are: the march stops at the SCENE depth, and an
            // alpha-blended draw writes none -- so running it after the transparents would
            // paint smoke straight over every window, fence and iron sight in front of it.
            // After the opaque world, water and clouds; before the transparent replay.
            if start == 0 && !resolved && self.smoke_volume.active() {
                self.ensure_far_depth_resolve(&mut encoder);
                if let (Some(depth_view), Some(frame)) = (
                    self.gfx3d.water_depth_view().cloned(),
                    self.godrays_frame,
                ) {
                    let (rw, rh) = self.render_size();
                    let cam = [frame.cam_pos[0], frame.cam_pos[1], frame.cam_pos[2]];
                    // Flatten the camera-relative inverse view-projection the sky derived.
                    let m = frame.inv_view_proj;
                    let mut ivp = [0.0f32; 16];
                    for c in 0..4 {
                        for r in 0..4 {
                            ivp[c * 4 + r] = m[c][r];
                        }
                    }
                    self.smoke_volume.record(
                        &self.device,
                        &self.queue,
                        &mut encoder,
                        &target,
                        &depth_view,
                        (rw, rh),
                        cam,
                        ivp,
                        self.smoke_sun_dir,
                        self.smoke_sun_radiance,
                        self.smoke_ambient,
                    );
                }
            }

            if start == 0 && !resolved && self.soft_globals.active {
                self.ensure_far_depth_resolve(&mut encoder);
                if let Some(depth_view) = self.gfx3d.water_depth_view().cloned() {
                    let (rw, rh) = self.render_size();
                    let dgen = self.gfx3d.depth_gen();
                    self.soft_particles.record(
                        &self.device,
                        &mut encoder,
                        &depth_view,
                        dgen,
                        rw,
                        rh,
                    );
                    if self.soft_particles.valid() {
                        let view = self.soft_particles.view().cloned();
                        self.gfx2d
                            .set_soft_depth(&self.device, view.as_ref(), dgen);
                    }
                }
            }

            // TW-WATER W12a: Tidewater's spray sprites (breakers, bow spray, the water explosions'
            // plumes), here -- after the clouds, the god rays and the smoke, before the other
            // transparents -- for the reason all three composites above are placed where they
            // are: they stop at the scene depth and blended sprites write none. Drawn inside the
            // water pass, a plume standing against the sky had the clouds behind it painted over
            // it. Depth read-only, as in the water pass.
            if self.water.has_spray_pass() && seg_ops.iter().any(|o| matches!(o, Plan3dOp::Water(_))) {
                if let Some(cam) = self.gfx3d.camera_bind() {
                    let mut spass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
                        label: Some("wgr_tw_spray_pass"),
                        color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                            view: &target,
                            depth_slice: None,
                            resolve_target: None,
                            ops: wgpu::Operations { load: wgpu::LoadOp::Load, store: wgpu::StoreOp::Store },
                        })],
                        depth_stencil_attachment: Some(wgpu::RenderPassDepthStencilAttachment {
                            view: seg_depth,
                            depth_ops: None,
                            stencil_ops: None,
                        }),
                        timestamp_writes: None,
                        occlusion_query_set: None,
                        multiview_mask: None,
                    });
                    for op in seg_ops {
                        if let Plan3dOp::Water(arg) = op {
                            if let Some(b) = water_batches.get(*arg as usize) {
                                if b.first_node == 0 {
                                    let off = (b.camera as u64 * self.gfx3d.camera_stride()) as u32;
                                    self.water.draw_spray(&mut spass, cam, off);
                                }
                            }
                        }
                    }
                    drop(spass);
                }
            }

            // The rest of the plan's world draws, now that water and clouds are down.
            // This is where Scene::ObjectsDrawn's output lands: the depth-sorted
            // transparent objects, the cloudlets, the rain streaks. They blend
            // against the sea and the sky instead of being painted over by both.
            // Colour and depth both LOAD -- the pass writes no new opaque geometry
            // in practice, but nothing here forbids it either, and loading is what
            // makes the two sub-passes one logical pass split in the middle.
            if has_post_water_3d {
                let mut pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
                    label: Some(&format!("{seg_label}_post_water")),
                    color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                        view: &target,
                        depth_slice: None,
                        resolve_target: None,
                        ops: wgpu::Operations {
                            load: wgpu::LoadOp::Load,
                            store: wgpu::StoreOp::Store,
                        },
                    })],
                    depth_stencil_attachment: Some(wgpu::RenderPassDepthStencilAttachment {
                        view: seg_depth,
                        depth_ops: Some(wgpu::Operations {
                            load: wgpu::LoadOp::Load,
                            store: wgpu::StoreOp::Store,
                        }),
                        stencil_ops: Some(wgpu::Operations {
                            load: wgpu::LoadOp::Load,
                            store: wgpu::StoreOp::Store,
                        }),
                    }),
                    timestamp_writes: None,
                    occlusion_query_set: None,
                    multiview_mask: None,
                });
                if time_segment {
                    self.gpu_timers
                        .begin_pass(&mut pass, TimerRegion::OpsPostWater);
                }
                render_ops(
                    self,
                    &mut pass,
                    ops_post_water,
                    display_2d,
                    false,
                    prepassed,
                    early_opaque.then_some(false),
                    Some(false),
                );
                if time_segment {
                    self.gpu_timers
                        .end_pass(&mut pass, TimerRegion::OpsPostWater);
                }
                if start == 0 && !suppress_world_objects && self.cull_debug_draw {
                    let cam_off = (main_scene_cam as u64 * self.gfx3d.camera_stride()) as u32;
                    self.gfx3d.draw_cull_spheres(&mut pass, cam_off);
                }
                drop(pass);
            }
            if time_segment {
                self.gpu_timers
                    .end(&mut encoder, TimerRegion::ObjColorSegment);
                self.gpu_timers.cpu_end(TimerRegion::ObjColorSegment);
            }

            // 2D sub-pass: the overlays, over the fogged colour, loading the 3D depth +
            // stencil so any depth-tested 2D still occludes and stencil state carries over.
            if has_2d {
                let mut pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
                    label: Some(&seg_label),
                    color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                        view: &target,
                        depth_slice: None,
                        resolve_target: None,
                        ops: wgpu::Operations {
                            load: wgpu::LoadOp::Load,
                            store: wgpu::StoreOp::Store,
                        },
                    })],
                    depth_stencil_attachment: Some(wgpu::RenderPassDepthStencilAttachment {
                        view: seg_depth,
                        depth_ops: Some(wgpu::Operations {
                            load: wgpu::LoadOp::Load,
                            store: wgpu::StoreOp::Store,
                        }),
                        stencil_ops: Some(wgpu::Operations {
                            load: wgpu::LoadOp::Load,
                            store: wgpu::StoreOp::Store,
                        }),
                    }),
                    timestamp_writes: None,
                    occlusion_query_set: None,
                    multiview_mask: None,
                });
                // Encoder-level, not in-pass: this 2D segment can be entered more than once
                // per frame and an in-pass bracket would hand the slot to whichever ran last.
                // Bracketing the whole pass keeps the region meaning "the 2D work in this
                // frame" rather than "the last 2D sub-pass".
                self.gpu_timers.begin_pass(&mut pass, TimerRegion::Ui2d);
                render_ops(self, &mut pass, seg_ops, display_2d, true, false, None, None);
                self.gpu_timers.end_pass(&mut pass, TimerRegion::Ui2d);
                drop(pass);
                // The 2D sub-pass attaches depth Load/Store, so a depth-tested 2D draw can write
                // it. That is after this segment's water/cloud resolves, so mark them stale.
                self.far_depth_resolve_current = false;
            }

            // Freeze the completed world for the held LLDR screen, before the
            // first-person segment paints the device over it. Reuse water's snapshot.
            let has_monitor = draws3d.iter().any(|d| d.flags & ffi::WGR_DRAW3D_MONITOR != 0);
            if !resolved && has_monitor && matches!(ops.get(end), Some(Plan3dOp::ClearDepth)) {
                if let (Some((source, source_view)), Some((snapshot, snapshot_view))) =
                    (self.hdr.as_ref(), self.water_scene.as_ref()) {
                    if self.sample_count > 1 {
                        let pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
                            label: Some("wgr_monitor_scene_resolve"),
                            color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                                view: source_view,
                                depth_slice: None,
                                resolve_target: Some(snapshot_view),
                                ops: wgpu::Operations { load: wgpu::LoadOp::Load, store: wgpu::StoreOp::Store },
                            })],
                            depth_stencil_attachment: None,
                            timestamp_writes: None,
                            occlusion_query_set: None,
                            multiview_mask: None,
                        });
                        drop(pass);
                    } else {
                        encoder.copy_texture_to_texture(
                            source.as_image_copy(), snapshot.as_image_copy(),
                            wgpu::Extent3d { width: self.hdr_size.0, height: self.hdr_size.1, depth_or_array_layers: 1 },
                        );
                    }
                }
            }
            if end >= ops.len() {
                break;
            }
            // Scene->UI seam: resolve the HDR scene and switch to display-referred UI.
            if matches!(ops[end], Plan3dOp::Resolve) && self.tonemap.is_some() && !resolved {
                let hdr_source = self
                    .hdr_resolve
                    .as_ref()
                    .map(|(_, v)| v.clone())
                    .unwrap_or_else(|| scene_view.clone());
                self.run_tonemap(&mut encoder, &hdr_source, &color, underwater_time, draws3d);
                resolved = true;
                target = color.clone();
                display_2d = true;
                clear_color_next = false; // UI loads the tonemapped scene
            } else if matches!(ops[end], Plan3dOp::Resolve)
                && underwater_time.is_some()
                && !resolved
            {
                self.ensure_far_depth_resolve(&mut encoder);
                let depth = self
                    .gfx3d
                    .water_depth_view()
                    .expect("underwater depth target");
                self.gpu_timers
                    .begin(&mut encoder, TimerRegion::UnderwaterComposite);
                self.water
                    .render_underwater(&self.device, &mut encoder, &scene_view, depth, &color);
                self.gpu_timers
                    .end(&mut encoder, TimerRegion::UnderwaterComposite);
                resolved = true;
                target = color.clone();
                display_2d = true;
                clear_color_next = false;
            }
            start = end + 1;
        }

        // Fallback: an HDR frame that never emitted the Resolve marker still needs
        // resolving so the scene reaches the swapchain.
        self.gpu_timers
            .cpu_end(crate::gpu_timers::Region::WorkerEncDraw);
        self.gpu_timers
            .cpu_begin(crate::gpu_timers::Region::WorkerEncPost);
        if self.tonemap.is_some() && !resolved {
            let hdr_source = self
                .hdr_resolve
                .as_ref()
                .map(|(_, v)| v.clone())
                .unwrap_or_else(|| scene_view.clone());
            self.run_tonemap(&mut encoder, &hdr_source, &color, underwater_time, draws3d);
        } else if underwater_time.is_some() && !resolved {
            self.ensure_far_depth_resolve(&mut encoder);
            let depth = self
                .gfx3d
                .water_depth_view()
                .expect("underwater depth target");
            self.gpu_timers
                .begin(&mut encoder, TimerRegion::UnderwaterComposite);
            self.water
                .render_underwater(&self.device, &mut encoder, &scene_view, depth, &color);
            self.gpu_timers
                .end(&mut encoder, TimerRegion::UnderwaterComposite);
        }

        // Dev-panel overlay composites over the finished frame, no depth.
        if !overlay_draws.is_empty() {
            encoder.push_debug_group("wgr_overlay");
            let mut pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
                label: Some("wgr_overlay"),
                color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                    view: &color,
                    depth_slice: None,
                    resolve_target: None,
                    ops: wgpu::Operations {
                        load: wgpu::LoadOp::Load,
                        store: wgpu::StoreOp::Store,
                    },
                })],
                depth_stencil_attachment: None,
                timestamp_writes: None,
                occlusion_query_set: None,
                multiview_mask: None,
            });
            self.gfx2d.render_overlay(
                &self.device,
                &mut pass,
                &self.textures,
                overlay_draws,
                self.config.width,
                self.config.height,
            );
            drop(pass);
            encoder.pop_debug_group();
        }

        // WTR-002 — resolve this frame's timestamp brackets into a readback slot (recorded
        // last so every bracket above is covered), then after submit kick/drain the
        // non-blocking readbacks.
        if let Some((buffer, width, height, _bytes_per_row, padded_bytes_per_row, _is_bgra)) =
            &screenshot_staging
        {
            encoder.copy_texture_to_buffer(
                wgpu::TexelCopyTextureInfo {
                    texture: &frame.texture,
                    mip_level: 0,
                    origin: wgpu::Origin3d::ZERO,
                    aspect: wgpu::TextureAspect::All,
                },
                wgpu::TexelCopyBufferInfo {
                    buffer,
                    layout: wgpu::TexelCopyBufferLayout {
                        offset: 0,
                        bytes_per_row: Some(*padded_bytes_per_row),
                        rows_per_image: Some(*height),
                    },
                },
                wgpu::Extent3d {
                    width: *width,
                    height: *height,
                    depth_or_array_layers: 1,
                },
            );
        }
        self.gpu_timers.end(&mut encoder, TimerRegion::FrameTotal);
        self.gpu_timers.resolve(&mut encoder);
        // PERF-005: after every cull dispatch has written its slice, before submit.
        self.gpu_timers
            .cpu_end(crate::gpu_timers::Region::WorkerEncPost);
        self.gpu_timers
            .cpu_end(crate::gpu_timers::Region::WorkerEncMain);
        self.gpu_timers
            .cpu_begin(crate::gpu_timers::Region::WorkerEncTail);
        self.gfx3d.resolve_object_stats(&mut encoder);
        let main_count_copied = main_count_armed
            && main_world_draw_closed
            && self.gfx3d.copy_main_target_count(&mut encoder);
        // Each solar cascade copies its own counter only after its depth pass
        // closed. A missing cascade, skipped cull, or missing draw remains Unknown.
        let mut shadow_count_copied = [false; 4];
        if main_count_copied {
            for cascade in 0..4 {
                shadow_count_copied[cascade] =
                    self.gfx3d.copy_shadow_target_count(&mut encoder, cascade);
            }
        }
        // GI RSM view 5 has its own COUNT word. A cached RSM, disabled GI,
        // failed cull or missing indirect draw stays Unknown for this request.
        let gi_count_copied = main_count_copied &&
            self.gfx3d.copy_gi_target_count(&mut encoder);
        // Zenith is sky view 0, separate from GI view 5. A cached layer,
        // skipped cull or missing indirect depth draw leaves this Unknown.
        let sky0_count_copied = main_count_copied &&
            self.gfx3d.copy_sky0_target_count(&mut encoder);
        // Tile 0 is a separate local-light view; its cull index moves with the
        // number of active solar cascades and can be zero at night. Only a fresh
        // tile pass, never a cache reuse, authorizes this private counter copy.
        let local0_count_copied = main_count_copied &&
            self.gfx3d.copy_local0_target_count(&mut encoder);
        let batch_count_copied = main_count_copied &&
            self.gfx3d.copy_batch_target_count(&mut encoder);
        self.gfx3d.resolve_lod_demand();
        self.gfx3d.resolve_mip_feedback(&mut encoder);
        self.water_visibility.resolve(&mut encoder);
        self.gpu_timers
            .cpu_end(crate::gpu_timers::Region::WorkerEncTail);
        self.gpu_timers
            .cpu_begin(crate::gpu_timers::Region::WorkerSubmit);
        let main_commands = encoder.finish();
        let main_count_ready = main_count_copied && main_count_request.is_some_and(|(token, _, _)|
            self.gfx3d.main_target_count_before_submit(token));
        let mut shadow_count_ready = [false; 4];
        if main_count_ready {
            if let Some((token, _, _)) = main_count_request {
                for cascade in 0..4 {
                    shadow_count_ready[cascade] = shadow_count_copied[cascade] &&
                        self.gfx3d.shadow_target_count_before_submit(token, cascade);
                }
            }
        }
        let gi_count_ready = gi_count_copied && main_count_ready &&
            main_count_request.is_some_and(|(token, _, _)|
                self.gfx3d.shadow_target_count_before_submit(token, 4));
        let sky0_count_ready = sky0_count_copied && main_count_ready &&
            main_count_request.is_some_and(|(token, _, _)|
                self.gfx3d.sky0_target_count_before_submit(token));
        let local0_count_ready = local0_count_copied && main_count_ready &&
            main_count_request.is_some_and(|(token, _, _)|
                self.gfx3d.local0_target_count_before_submit(token));
        let batch_count_ready = batch_count_copied && main_count_ready &&
            main_count_request.is_some_and(|(token, _, _)|
                self.gfx3d.batch_target_count_before_submit(token));
        self.queue.submit(std::iter::once(main_commands));
        self.gfx3d.interior_publications_submitted();
        self.gfx3d.gi_rsm_publication_submitted();
        self.gfx3d.local_publications_submitted();
        let main_count_submitted = main_count_ready && main_count_request.is_some_and(|(token, _, _)|
            self.gfx3d.main_target_count_submitted(token, &self.queue));
        if let Some((token, _, _)) = main_count_request {
            for cascade in 0..4 {
                if shadow_count_ready[cascade] {
                    self.gfx3d.shadow_target_count_submitted(token, cascade);
                }
            }
            if gi_count_ready {
                self.gfx3d.shadow_target_count_submitted(token, 4);
            }
            if sky0_count_ready {
                self.gfx3d.sky0_target_count_submitted(token);
            }
            if local0_count_ready {
                self.gfx3d.local0_target_count_submitted(token);
            }
            if batch_count_ready {
                self.gfx3d.batch_target_count_submitted(token);
            }
        }
        self.gpu_timers
            .cpu_end(crate::gpu_timers::Region::WorkerSubmit);
        // Geometry-pool growth may have happened after an earlier pass recorded
        // the old pool buffer. Submission is the safe boundary for releasing
        // those superseded application-side handles; wgpu retains resources for
        // command buffers that are still executing on the GPU.
        self.gfx3d.frame_submitted(&self.device, &self.queue);
        // REN-THR-009 — the other site where the pool epoch can move: a shrink reallocates
        // the vertex buffer exactly as a growth does. Republish before the producer's next
        // upload, which happens between frames, not at the next frame's start.
        self.publish_pool_to_uploader();
        // Same boundary for bindless texture slots: a slot freed by texture_destroy
        // this frame may only be reused for creates in later frames.
        self.textures.frame_submitted(&self.queue);
        // Load-time sky bakes are submitted without waiting; their readbacks complete
        // during polls/submissions and are drained here, 1-2 frames after registration.
        self.gfx3d.collect_sky_bakes(&self.device);
        self.gpu_timers
            .cpu_begin(crate::gpu_timers::Region::WorkerPresent);
        frame.present();
        self.gpu_timers
            .cpu_end(crate::gpu_timers::Region::WorkerPresent);
        // REN-TEMP-001E: this frame's camera becomes the previous frame's history.
        self.temporal.end_frame();
        if let Some((buffer, width, height, bytes_per_row, padded_bytes_per_row, is_bgra)) =
            screenshot_staging
        {
            let slice = buffer.slice(..);
            let (tx, rx) = std::sync::mpsc::channel();
            slice.map_async(wgpu::MapMode::Read, move |result| {
                let _ = tx.send(result);
            });
            if self
                .device
                .poll(wgpu::PollType::wait_indefinitely())
                .is_ok()
                && matches!(rx.recv(), Ok(Ok(())))
            {
                let mapped = slice.get_mapped_range();
                let mut rgba = vec![0; bytes_per_row as usize * height as usize];
                for row in 0..height as usize {
                    let source =
                        &mapped[row * padded_bytes_per_row as usize..][..bytes_per_row as usize];
                    let dest = &mut rgba[row * bytes_per_row as usize..][..bytes_per_row as usize];
                    dest.copy_from_slice(source);
                    if is_bgra {
                        for pixel in dest.chunks_exact_mut(4) {
                            pixel.swap(0, 2);
                        }
                    }
                }
                drop(mapped);
                buffer.unmap();
                self.screenshot_pixels = Some(ScreenshotPixels {
                    width,
                    height,
                    rgba,
                });
                self.log
                    .log(log_level::INFO, "wgpu screenshot readback completed");
                if let Some(state)=self.post_optics_probe.as_mut(){
                    if let Some(pending)=state.pending.take(){match pending.finish(&self.device){
                        Ok(lines)=>for line in lines{self.log.log(log_level::INFO,&line);},
                        Err(reason)=>self.log.log(log_level::WARN,&format!("PostOptics refused: {}",reason)),
                    }}else{self.log.log(log_level::WARN,"PostOptics refused: no same-frame float packet");}
                }
            } else {
                self.log
                    .log(log_level::ERROR, "wgpu screenshot readback failed");
                if let Some(state)=self.post_optics_probe.as_mut(){state.pending=None;}
            }
            self.screenshot_requested = false;
        }
        self.gpu_timers
            .cpu_begin(crate::gpu_timers::Region::WorkerHarvest);
        self.gpu_timers.harvest(&self.device);
        self.gfx3d.harvest_object_stats(&self.device);
        self.gfx3d.harvest_lod_demand(&self.device);
        self.gfx3d.harvest_mip_feedback(&self.device);
        self.water_visibility.harvest(&self.device);
        self.grass.harvest_stats(&self.device);
        self.gpu_timers
            .cpu_end(crate::gpu_timers::Region::WorkerHarvest);
        if let Some(facts) = call_timings { facts.complete(&self.gpu_timers); }
        if let Some(tracker)=self.demand_view_snapshot.as_mut() {
            tracker.capture_main(main_selection,cameras,self.hdr_size);
            self.gfx3d.capture_demand_view_snapshot(tracker,shadow,
                reflected_camera.map(|i|&prepared_cameras[i]),self.planar.as_ref().map(|p|p.size),
                cameras.get(main_scene_cam));
        }
        if let Some(tuple)=self.main_camera_tuple.as_mut(){tuple.complete();} // CPU returned frame, not GPU completion.
        Ok(main_count_submitted)
    }

    fn texture_create(
        &mut self,
        width: u32,
        height: u32,
        format: TextureFormat,
        mip_count: u32,
        gen_mips: bool,
        data: &[u8],
    ) -> u64 {
        let handle = self.textures.create(
            &self.device,
            &self.queue,
            &TextureData {
                width,
                height,
                format,
                mip_count,
                gen_mips,
                bytes: data,
            },
        );
        if handle != 0 { self.gfx3d.texture_content_changed(); }
        handle
    }

    // REN-RES-001: upload into a slot leased by the caller's logical texture, so an
    // evicted-and-re-uploaded texture keeps the bindless index every registered model
    // already baked. See the `bindless_leased` note in textures.rs.
    #[allow(clippy::too_many_arguments)]
    fn texture_create_in_slot(
        &mut self,
        width: u32,
        height: u32,
        format: TextureFormat,
        mip_count: u32,
        gen_mips: bool,
        data: &[u8],
        slot: u32,
    ) -> u64 {
        let handle = self.textures.create_in_slot(
            &self.device,
            &self.queue,
            &TextureData {
                width,
                height,
                format,
                mip_count,
                gen_mips,
                bytes: data,
            },
            slot,
        );
        if handle != 0 {
            self.gfx3d.invalidate_texture_feedback(self.textures.texture_slot(handle));
            self.gfx3d.texture_content_changed();
        }
        handle
    }

    fn texture_slot_acquire(&mut self) -> u32 {
        let slot = self.textures.slot_acquire();
        self.gfx3d.invalidate_texture_feedback(slot);
        slot
    }

    fn texture_slot_release(&mut self, slot: u32) {
        if self.textures.slot_release(slot) {
            self.gfx3d.texture_content_changed();
        }
    }

    fn texture_update(&mut self, handle: u64, data: &[u8]) {
        if self.textures.update_rgba(&self.queue, handle, data) {
            self.gfx3d.texture_content_changed();
        }
    }

    fn texture_destroy(&mut self, handle: u64) {
        if self.textures.destroy(handle) {
            self.gfx3d.texture_content_changed();
        }
    }

    fn mesh_create(&mut self, verts: &[WgrMeshVertex], indices: &[u32]) -> u64 {
        let id = self
            .gfx3d
            .mesh_create(&self.device, &self.queue, verts, indices);
        // REN-THR-009 — one of the two production sites where the pool epoch can move (a
        // growth reallocates the vertex buffer), so the uploader's cached buffer is
        // republished here. Then the new mesh's vbase/vert_count, which are fixed for its
        // life, so an off-handle update never has to ask the renderer anything.
        if id != 0 {
            self.publish_pool_to_uploader();
            if let Some((vbase, vert_count)) = self.gfx3d.mesh_upload_meta(id) {
                self.uploader
                    .register_mesh(id, uploader::MeshUpload { vbase, vert_count });
            }
        }
        id
    }

    /// The upload surface `wgr_uploader_get` hands to C++. See the lifetime contract there.
    pub(crate) fn uploader(&self) -> &std::sync::Arc<uploader::Uploader> {
        &self.uploader
    }

    /// Republish the pool's current vertex buffer to the uploader. Idempotent and cheap —
    /// it clones only when the epoch actually moved — so it can be called unconditionally.
    fn publish_pool_to_uploader(&self) {
        self.uploader
            .publish_pool(self.gfx3d.pool_generation(), self.gfx3d.pool_vbuf());
    }

    /// Run the destroys `wgr_uploader_mesh_destroy` parked. Called at the TOP of a frame:
    /// `Gfx3d::mesh_destroy` removes `bake_bind_cache` entries the draw path reads and
    /// returns ranges to the pool's free lists, so it may not land mid-frame.
    fn drain_deferred_mesh_destroys(&mut self) {
        for id in self.uploader.take_pending_destroys() {
            self.gfx3d.mesh_destroy(id);
        }
    }

    fn mesh_update(&mut self, handle: u64, verts: &[WgrMeshVertex]) {
        self.gfx3d.mesh_update(&self.queue, handle, verts);
    }

    fn mesh_set_skin(&mut self, handle: u64, bones: &[u8], weights: &[u8]) {
        self.gfx3d
            .mesh_set_skin(&self.device, handle, bones, weights);
    }

    fn mesh_destroy(&mut self, handle: u64) {
        // Keep the uploader's metadata table from outliving the mesh, so a stale handle
        // reaching `wgr_uploader_mesh_update` is ignored rather than writing into ranges a
        // later create may already own.
        self.uploader.forget_mesh(handle);
        self.gfx3d.mesh_destroy(handle);
    }

    // --- GPU-driven retained scene (docs/gpu-culling-and-depth-plan.md Stage 3b) ---

    fn model_register(
        &mut self,
        bounding_sphere: f32,
        lods: &[WgrModelLod],
        sections: &[WgrModelSection],
        materials: &[WgrModelMaterial],
        name: Option<&str>,
    ) -> u32 {
        self.gfx3d.register_model(
            &self.device,
            &self.queue,
            bounding_sphere,
            lods,
            sections,
            materials,
            &self.textures,
            name,
        )
    }

    fn register_crown_centres(&mut self, centres: &[WgrVec4]) -> u32 {
        self.gfx3d.register_crown_centres(centres)
    }

    fn instance_add(&mut self, inst: &WgrInstance) -> u32 {
        self.gfx3d.instance_add(inst)
    }

    fn instance_update(&mut self, slot: u32, inst: &WgrInstance) {
        self.gfx3d.instance_update(slot, inst);
    }

    fn instance_remove(&mut self, slot: u32) {
        self.gfx3d.instance_remove(slot);
    }

    fn set_dynamic(&mut self, instances: &[WgrInstance]) {
        self.gfx3d.set_dynamic(instances);
    }

    // Push the engine's per-frame cull + LOD inputs (the real Scene::LevelFromDistance2 values).
    fn set_cull_inputs(
        &mut self,
        objects_z: f32,
        lod_scale: f32,
        lod_inv_width: f32,
        pixel_limit: f32,
    ) {
        self.gfx3d
            .set_cull_inputs(objects_z, lod_scale, lod_inv_width, pixel_limit);
    }

    // Per-frame: suppress the retained GPU-driven world set (objects + prepass) so the
    // editor/loading/shutdown frames don't leak clutter behind the 2D UI. Resources stay
    // resident; only this frame's draw submission is skipped. C++ sets it every frame.
    fn set_suppress_world_objects(&mut self, suppress: bool) {
        self.suppress_world_objects = suppress;
    }

    // FAR INSTANCE TIER — the whole proxy set, uploaded once per world. Logged with the
    // card/prism split because the two have a measured ~4x draw-cost difference, so the
    // classifier's output is the number that predicts what this tier costs, not the total.
    fn far_set_instances(&mut self, instances: &[WgrFarInstance]) {
        let (cards, prisms) = self.far.set_instances(&self.device, &self.queue, instances);
        if instances.is_empty() {
            self.log.log(log_level::INFO, "[wgr] far tier: released");
            return;
        }
        self.log.log(
            log_level::INFO,
            &format!(
                "[wgr] far tier: {} proxies ({} cards, {} prisms), {:.1} MB of source rows, \
                 start rule = {}",
                instances.len(),
                cards,
                prisms,
                (instances.len() * std::mem::size_of::<WgrFarInstance>()) as f64
                    / (1024.0 * 1024.0),
                // Logged because WGR_FAR_MODE is read on BOTH sides of the ABI (here for the
                // predicate, in EngineWgpu.cpp for the matching near-cutoff default) and this
                // line sits next to the C++ one, so a disagreement is visible rather than
                // silent.
                if self.far.complement_mode() {
                    "complement of the object cull"
                } else {
                    "legacy distance (WGR_FAR_MODE=distance)"
                }
            ),
        );
    }

    fn far_set_params(
        &mut self,
        near_cutoff_m: f32,
        far_distance_m: f32,
        pixel_limit: f32,
        enabled: bool,
    ) {
        self.far.set_params(
            &self.queue,
            near_cutoff_m,
            far_distance_m,
            pixel_limit,
            enabled,
        );
    }

    // ImGui Culling tab (wgr_set_cull_debug): draw the cull-sphere wireframes, skip the GPU
    // frustum test, and toggle GPU Hi-Z occlusion. First two are diagnostics for the GPU-driven
    // "objects vanish / float" investigation; occlusion is the §5 Hi-Z cull.
    fn set_cull_debug(&mut self, draw_spheres: bool, no_frustum: bool, occlusion: bool) {
        self.cull_debug_draw = draw_spheres;
        self.gfx3d.set_cull_no_frustum(no_frustum);
        self.gfx3d.set_occlusion_enabled(occlusion);
    }

    fn terrain_set_heightmap(&mut self, heights: &[f32], params: WgrTerrainParams) {
        self.rain_water.invalidate_source();
        self.log.log(log_level::INFO, &format!(
            "Terrain heightfield: {}x{}, device limit {}, samples {}",
            params.hm_width, params.hm_height, self.device.limits().max_texture_dimension_2d, heights.len()));
        // REN-TEMP-001 Phase 5: a new heightmap IS a world load — the one explicit reset
        // event the renderer can see today without an ABI change. Camera history from the
        // previous world must not reproject into this one.
        self.temporal.notify_reset("world load");
        self.terrain
            .set_heightmap(&self.device, &self.queue, heights, params);
        // TW-WATER W3: Tidewater's shore field is computed from the heights (on a worker thread).
        self.water.set_terrain_heights(heights, params);
        self.log.log(log_level::INFO, &format!("Terrain heightfield ready={}", self.terrain.has_heightmap()));
    }

    fn water_set_params(&mut self, params: WgrWaterParams) {
        self.water.set_params(&self.queue, params);
    }

    fn rain_water_set_grid(&mut self, params: ffi::WgrRainWaterParams, cells: &[WgrVec4], revision: u64) {
        let accepted = self.rain_water.set_grid(&self.device, &self.queue, params, cells, revision);
        static TRACE: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
        let trace = *TRACE.get_or_init(|| std::env::var("WGR_RAIN_WATER_TRACE").as_deref()==Ok("1"));
        if (!cells.is_empty() && trace) || !accepted {
            let max_depth = cells.iter().map(|c| c[1]).fold(0.0f32, f32::max);
            self.log.log(log_level::INFO, &format!(
                "Rain water grid: generation={} revision={revision} width={} height={} copied={} enabled={} accepted={accepted} maxDepth={max_depth:.6} independent=1",
                params.generation, params.control[0], params.control[1], cells.len(), params.control[3]));
        }
    }

    fn rain_water_set_source(&mut self, source: ffi::WgrRainWaterSourceKey) {
        self.terrain.set_rain_water_medium_source(source);
        self.rain_water.set_source(source);
    }

    fn rain_water_set_publication(
        &mut self,
        publication: ffi::WgrRainWaterPublication,
        coarse: &[WgrVec4],
        fine: &[ffi::WgrRainWaterFineCell],
    ) {
        let accepted = self.rain_water.set_publication(
            &self.device, &self.queue, publication, coarse, fine,
        );
        static TRACE: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
        let trace = *TRACE.get_or_init(|| std::env::var("WGR_RAIN_WATER_TRACE").as_deref() == Ok("1"));
        if (trace && (!coarse.is_empty() || !fine.is_empty())) || !accepted {
            let (cached_coarse, cached_fine, tiles) = self.rain_water.publication_counts();
            let source = publication.source;
            self.log.log(log_level::INFO, &format!(
                "Rain water publication: worldToken={} generation={} heightRevision={} terrainRange={} terrainSpacing={} revision={} flags={} copiedCoarse={} copiedFine={} cachedCoarse={} cachedFine={} tiles={} enabled={} accepted={}",
                source.world_token, source.generation, source.height_revision,
                source.terrain_range, source.terrain_spacing, publication.revision,
                publication.flags, coarse.len(), fine.len(), cached_coarse, cached_fine,
                tiles, publication.coarse.control[3], accepted,
            ));
        }
    }

    fn water_set_cascade_config(&mut self, index: u32, config: WgrWaterCascadeConfig) {
        self.water
            .set_cascade_config(&self.device, &self.queue, index, config);
    }

    fn water_set_interaction_params(&mut self, params: WgrWaterInteractionParams) {
        self.water.set_interaction_params(&self.queue, params);
    }

    fn water_submit_interactions(&mut self, events: &[WgrWaterInteractionEvent]) {
        self.water.submit_interactions(&self.queue, events);
    }

    fn terrain_set_params(&mut self, params: WgrTerrainParams) {
        self.terrain.set_params(&self.queue, params);
    }

    #[allow(clippy::too_many_arguments)]
    fn terrain_set_sky_visibility(
        &mut self,
        strength: f32,
        contrast: f32,
        floor: f32,
        radius_m: f32,
        k_azimuths: u32,
        downsample: u32,
        debug: bool,
    ) {
        self.terrain.set_sky_visibility(
            &self.device,
            &self.queue,
            strength,
            contrast,
            floor,
            radius_m,
            k_azimuths,
            downsample,
            debug,
        );
    }

    fn terrain_set_ground_layers(&mut self, handles: &[u64]) {
        let views: Vec<wgpu::TextureView> = handles
            .iter()
            .map(|&h| self.textures.texture_view(h).clone())
            .collect();
        self.terrain.set_ground_layers(&self.device, views);
    }

    fn terrain_set_index_map(&mut self, width: u32, height: u32, indices: &[u16]) {
        self.terrain
            .set_index_map(&self.device, &self.queue, width, height, indices);
    }

    fn terrain_set_materials(&mut self, materials: &[WgrTerrainMaterial]) {
        self.terrain
            .set_materials(&self.device, &self.queue, materials);
    }

    fn grass_set_geography(&mut self, width: u32, height: u32, values: &[u32]) {
        self.grass
            .set_geography(&self.device, &self.queue, width, height, values);
    }

    // GRS-E — the game's decoded grass-tuft PAA for the mid LOD's crossed cards.
    fn grass_set_tuft(&mut self, width: u32, height: u32, rgba: &[u8]) {
        self.grass
            .set_tuft(&self.device, &self.queue, width, height, rgba);
        self.drain_grass_log();
    }

    fn grass_set_tufts(&mut self, width: u32, height: u32, layers: u32, rgba: &[u8]) {
        self.grass
            .set_tufts(&self.device, &self.queue, width, height, layers, rgba);
        self.drain_grass_log();
    }

    // The grass module has no LogSink of its own; it queues lines and they are
    // forwarded here so the GAME log carries them (stderr is discarded by the
    // launcher, which is how the card-spacing lever went unannounced).
    fn drain_grass_log(&mut self) {
        for line in self.grass.drain_log() {
            self.log.log(log_level::INFO, &line);
        }
    }

    fn grass_have_photo_clumps(&self) -> bool {
        self.grass.have_photo_clumps()
    }

    fn grass_set_blade_atlas(&mut self, width: u32, height: u32, layers: u32, rgba: &[u8]) {
        self.grass
            .set_blade_atlas(&self.device, &self.queue, width, height, layers, rgba);
    }

    // AO march resolution (wgr_set_gtao_scale / wgr_get_gtao_scale). Lives on Gfx3d next to the
    // pass it configures; see `Gfx3d::set_gtao_scale` for why it is not part of GtaoSettings.
    fn set_gtao_scale(&mut self, scale: u32) {
        self.gfx3d.set_gtao_scale(scale);
    }

    fn set_sky_bake_gate(&mut self, min_enclosed: f32, budget_mb: u32) {
        self.gfx3d.set_sky_bake_gate(min_enclosed, budget_mb);
    }

    fn gtao_scale(&self) -> u32 {
        self.gfx3d.gtao_scale()
    }

    fn grass_set_params(&mut self, mut params: WgrGrassParams) {
        // Ablation, applied HERE rather than in grass/mod.rs on purpose: C++ rebuilds and pushes
        // this whole struct every frame from GrassSettings, so the renderer-side `WGR_GRASS` gate
        // only ever decided frame 0. See `Ablate`.
        if ABLATE.get_or_init(Ablate::from_env).grass {
            params.enabled = 0.0;
        }
        self.grass.set_params(&self.queue, params);
        self.drain_grass_log();
    }

    fn terrain_set_jitter_map(&mut self, width: u32, height: u32, offsets: &[i8]) {
        self.terrain
            .set_jitter_map(&self.device, &self.queue, width, height, offsets);
    }

    fn terrain_set_sun_shadow(
        &mut self,
        strength: f32,
        scale: u32,
        max_steps: u32,
        penumbra_deg: f32,
    ) {
        self.terrain
            .set_sun_shadow_params(&self.device, strength, scale, max_steps, penumbra_deg);
    }

    fn terrain_set_detail_layer(&mut self, handle: u64) {
        if handle == 0 {
            return;
        }
        let view = self.textures.texture_view(handle).clone();
        self.terrain.set_detail_layer(&self.device, view);
    }

    fn shadow_map_read(&mut self, layer: u32, out: &mut [f32]) -> u32 {
        self.gfx3d
            .shadow_map_read(&self.device, &self.queue, layer, out)
    }

    fn shadow_depth_probe(
        &mut self,
        light_vp: &[f32; 16],
        verts_xyz: &[f32],
        res: u32,
        out: &mut [f32],
    ) -> bool {
        self.gfx3d.shadow_depth_probe(
            &self.device,
            &self.queue,
            &self.textures,
            light_vp,
            verts_xyz,
            res,
            out,
        )
    }
}

// REN-RES-003 — the residency budget must be a measurement, not a guess. These pin the
// derivation itself (pure); the probe against real hardware is exercised by the startup log
// line, which names the heap it read.
#[cfg(test)]
mod residency_budget_tests {
    use super::{RESIDENCY_BUDGET_FALLBACK_MB, RESIDENCY_BUDGET_FRACTION, residency_budget_from};

    const MB: u64 = 1024 * 1024;

    // Measured on this workstation 2026-08-31 (RTX 3070, headless adapter, no game run):
    //   Vulkan — DEVICE_LOCAL heap 8017 MB -> budget 5611 MB
    //   Dx12   — DXGI local Budget 7249 MB -> budget 5074 MB (lower because DXGI's Budget
    //            already subtracts what other processes hold; the Vulkan heap is the card)
    //   Gl     — no query -> the 2048 MB fixed fallback, with the reason in the log
    // i.e. the old fixed 2,048 MB was under-budgeting this card by ~3.5 GB.
    #[test]
    fn probed_heap_becomes_a_fraction_of_itself() {
        let heap = 8192 * MB; // an 8 GB card
        let (bytes, log) = residency_budget_from(None, Ok(heap));
        assert_eq!(bytes, (heap as f64 * RESIDENCY_BUDGET_FRACTION) as u64);
        // The measured failure this replaces: 2,336 MB tracked against a 2,048 MB budget on
        // a card with room to spare. A probed 8 GB card must clear that mark comfortably.
        assert!(
            bytes > 2336 * MB,
            "a probed 8 GB card must budget above the 2,336 MB that was measured over budget \
             (got {} MB)",
            bytes / MB
        );
        assert!(log.contains("probed device-local heap"), "log was: {log}");
        assert!(log.contains("8192 MB"), "log must name the heap it read: {log}");
    }

    #[test]
    fn absent_probe_keeps_the_old_fixed_budget_and_says_why() {
        let (bytes, log) =
            residency_budget_from(None, Err("backend exposes no device-memory query"));
        assert_eq!(bytes, RESIDENCY_BUDGET_FALLBACK_MB * MB);
        assert!(log.contains("FIXED FALLBACK"), "log was: {log}");
        assert!(
            log.contains("backend exposes no device-memory query"),
            "the fallback REASON must be in the log, not just the fact: {log}"
        );
    }

    #[test]
    fn env_override_wins_over_a_successful_probe() {
        let (bytes, log) = residency_budget_from(Some(900), Ok(8192 * MB));
        assert_eq!(bytes, 900 * MB, "the override must be exact, not scaled");
        assert!(log.contains("override"), "log was: {log}");
        // The probe is still reported, so a run with an override can still be read against
        // what the card actually has.
        assert!(log.contains("8192 MB device-local"), "log was: {log}");
    }

    #[test]
    fn env_override_of_zero_is_still_unlimited() {
        for probe in [Ok(8192 * MB), Err("backend exposes no device-memory query")] {
            let (bytes, log) = residency_budget_from(Some(0), probe);
            assert_eq!(bytes, 0, "0 must stay the unlimited sentinel");
            assert!(log.contains("UNLIMITED"), "log was: {log}");
        }
    }

    #[test]
    fn a_tiny_probe_does_not_wrap_or_produce_an_accidental_unlimited() {
        // A software/virtual adapter can report a very small heap. The budget must stay a
        // small positive number: 0 would silently mean "unlimited", the exact opposite.
        let (bytes, _) = residency_budget_from(None, Ok(64 * MB));
        assert!(bytes > 0 && bytes < 64 * MB, "got {bytes} bytes");
    }
}

/// TW-WATER W7e/W7f: the early part of a Tidewater frame is submitted on its own;
/// `WGR_TW_EARLY_SUBMIT=0` keeps the frame one submission (A/B, or if a pass is ever found reading
/// an upload made after the split).
fn tw_early_submit() -> bool {
    static V: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *V.get_or_init(|| std::env::var("WGR_TW_EARLY_SUBMIT").map_or(true, |v| v.trim() != "0"))
}

/// TW-WATER W7h: `WGR_WATER_SOAK=<frames>[,<switches>]` (the backend-switch soak).
fn water_soak() -> Option<(u64, u32)> {
    static V: std::sync::OnceLock<Option<(u64, u32)>> = std::sync::OnceLock::new();
    *V.get_or_init(|| {
        let v = std::env::var("WGR_WATER_SOAK").ok()?;
        let mut it = v.split(',').map(|x| x.trim().parse::<u64>().ok());
        let every = it.next().flatten()?.max(2);
        let total = it.next().flatten().unwrap_or(20).clamp(1, 1000) as u32;
        Some((every, total))
    })
}
static WATER_SOAK_FRAME: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);

// Sinkhole W1b: eye adaptation underground. The shipped profile keeps auto-exposure on a short
// leash (a small max scale) so daylight never pumps; in a cave that leash is what keeps a
// campfire or a flashlight from ever reading. With the camera underground the ceiling rises
// toward WGR_CAVE_EXPOSURE_MAX (default 6) -- the eye opens in the dark and closes again at the
// mouth, at the profile's own adaptation rate. Above ground the params are untouched.
pub(crate) fn cave_exposure_max() -> f32 {
    static V: std::sync::OnceLock<f32> = std::sync::OnceLock::new();
    *V.get_or_init(|| {
        std::env::var("WGR_CAVE_EXPOSURE_MAX")
            .ok()
            .and_then(|v| v.parse::<f32>().ok())
            .filter(|v| v.is_finite() && *v > 0.0)
            .unwrap_or(6.0)
    })
}

pub(crate) fn cave_exposure(mut ep: ffi::WgrExposure, underground: f32) -> ffi::WgrExposure {
    let u = underground.clamp(0.0, 1.0);
    if u > 0.0 {
        let cave_max = cave_exposure_max().max(ep.max_scale);
        ep.max_scale += (cave_max - ep.max_scale) * u;
    }
    ep
}

#[cfg(test)]
mod cave_exposure_tests {
    use super::*;

    #[test]
    fn cave_exposure_only_lifts_the_ceiling_underground() {
        let mut ep = ffi::WgrExposure::default();
        ep.max_scale = 1.1;
        assert_eq!(cave_exposure(ep, 0.0).max_scale, 1.1);
        let deep = cave_exposure(ep, 1.0).max_scale;
        assert!((deep - cave_exposure_max().max(1.1)).abs() < 1e-6);
        let half = cave_exposure(ep, 0.5).max_scale;
        assert!(half > 1.1 && half < deep);
        // a profile already above the cave ceiling is never lowered
        ep.max_scale = 10.0;
        assert_eq!(cave_exposure(ep, 1.0).max_scale, 10.0);
    }
}
