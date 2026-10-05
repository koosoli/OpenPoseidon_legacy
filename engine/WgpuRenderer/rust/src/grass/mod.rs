mod blade_atlas;
mod wind;
#[cfg(test)]
mod footprint_tests;

use crate::ffi::{
    WGR_GRASS_DOWNWASH_COUNT, WGR_GRASS_TRACK_COUNT, WgrGrassDownwash, WgrGrassParams,
    WgrGrassTrack, WgrTerrainParams,
};
use crate::gfx3d::{depth_format, NORMAL_FORMAT};
use crate::terrain::Terrain;

// The near placement grid is SIZED FROM THE RADIUS, not fixed. A fixed grid had
// to widen its spacing to reach a longer radius, so moving the near-radius
// slider thinned the field instead of extending it: past ~30 m the accepted
// instance count is constant (pi*r^2 / (k*r)^2), which is exactly the reported
// "the slider stops doing anything, and the disc around the player is thinner
// than the mid ring beyond it". Spacing is now radius-independent and the grid
// grows to cover whatever radius is asked for, up to this cap.
const NEAR_GRID_MIN: u32 = 64;
const NEAR_GRID_MAX: u32 = 1536;
// Compacted output cap. Independent of the candidate grid: a 1536^2 grid only
// reaches this if nearly every candidate is accepted.
const MAX_INSTANCES: u32 = 1_048_576;
// A separate coarse ring follows the reference project's distance LOD idea,
// but stays GPU-generated and camera-snapped rather than using moving CPU tiles.
const FAR_GRID_DIM: u32 = 384;
const MAX_FAR_INSTANCES: u32 = FAR_GRID_DIM * FAR_GRID_DIM;
// The middle ring keeps a real blade silhouette between the dense cards and
// the single-triangle distance field.  It is intentionally separate from the
// far ring: a three-level field avoids the obvious 25-50 m quality cliff.
// A fixed 768² grid lets the mid ring keep one placement spacing while its
// radius is adjusted in the developer tools; changing the radius must change
// coverage, not thin or thicken the field.
// The mid grid is sized from the mid radius for the same reason as the near one,
// except that here the spacing was already fixed: a 768-cell grid at 0.82 m spans
// only +-315 m, so a 400 m mid radius would have been a 315 m square. Sizing it
// from the radius also makes the common short-radius case cheaper than the old
// fixed grid rather than more expensive.
const MID_GRID_MIN: u32 = 64;
// Enough cells to span the full mid reach at MID_PLACEMENT_SPACING (2 * 1000 /
// 0.82), so the ring is never a square smaller than its radius.
const MID_GRID_MAX: u32 = 2440;
// Deliberately NOT the grid area: a 2440^2 candidate grid would imply a 190 MB
// instance buffer permanently allocated for a radius almost nobody sets. The
// compute pass pins its counter at this cap instead, so a mid ring asked for
// more clumps than the buffer holds thins out rather than overruns.
const MAX_MID_INSTANCES: u32 = 1_048_576;
// Outer bound on the mid ring, mirroring MID_RING_MAX in grass.wgsl and
// GrassSettings::midRadius' slider maximum.
const MID_RING_MAX: f32 = 1000.0;
// MID_PLACEMENT_SPACING in grass.wgsl: the procedural mid ring's fixed spacing.
const MID_PLACEMENT_SPACING: f32 = 0.82;
// Photo-card placement spacing (both rings, when they draw cards; see
// card_placement_spacing in grass.wgsl). One card stands for one clutter object,
// so the unit is the map's own clutter grid: Arma's world configs place clutter
// on `clutterGrid`, 1.11 m on the A2/A3 worlds whose atlases this path draws,
// and thin it by the surface character's probability, which the geography bake
// already applies. The blade grid this replaced (0.16 m) put ~90 plates over
// every point of ground -- the "far too dense" report, and most of the flicker
// with it, since every one of those plate edges shimmered in the wind.
// WGR_GRASS_CARD_SPACING overrides for an A/B; clamped to CARD_SPACING_RANGE,
// whose floor is the old blade grid so the "before" density is reproducible in
// one binary (0.16 also puts the MID ring on that grid, which the 2440-cell cap
// then limits to a ~195 m square -- fine for a density A/B, not a setting).
const CARD_SPACING_DEFAULT: f32 = 1.11;
const CARD_SPACING_RANGE: (f32, f32) = (0.06, 4.0);

/// Near-ring candidate spacing. Must mirror `near_placement_spacing` in
/// grass.wgsl: the shader decides where candidates land, this decides how many
/// cells the dispatch has -- disagree and the ring is either a square smaller
/// than its radius or pays for cells it never fills.
fn near_placement_spacing(
    spacing: f32,
    clump_renderer: bool,
    cards: bool,
    card_spacing: f32,
) -> f32 {
    if cards {
        card_spacing
    } else {
        spacing * if clump_renderer { 3.20 } else { 1.72 }
    }
}

/// Index list for a ribbon of `blades` blades of `segments` quads each, mapping the six
/// corners of every quad onto the two ROWS the quad shares with its neighbours.
///
/// Non-indexed, a five-segment blade issues 30 vertices; only 12 of them are distinct,
/// because grass.wgsl's vertex shader derives every output from the row (`t`) and the
/// side (`left`) and nothing else -- the duplicated corners were computing bit-identical
/// results. This table is what lets the post-transform cache collapse them, so the
/// geometry is unchanged by construction rather than by inspection.
///
/// Corner order must match the winding the shader replaced: corners 2, 4 and 5 are the
/// upper row and corners 0, 3 and 5 are the left side.
fn build_blade_indices(blades: u32, segments: u32) -> Vec<u32> {
    let unique_per_blade = (segments + 1) * 2;
    let mut out = Vec::with_capacity((blades * segments * 6) as usize);
    for blade in 0..blades {
        for segment in 0..segments {
            for corner in 0..6u32 {
                let upper = corner == 2 || corner == 4 || corner == 5;
                let left = corner == 0 || corner == 3 || corner == 5;
                let row = segment + u32::from(upper);
                out.push(blade * unique_per_blade + row * 2 + u32::from(!left));
            }
        }
    }
    out
}

/// 0, 1, 2, ... -- an indexed draw that reproduces a non-indexed one exactly. The photo
/// card entry points (`vs_grass_mid_tuft`) decode `vertex_index` their own way and are
/// deliberately not re-derived; giving them identity indices keeps one draw path for both
/// modes instead of two indirect-argument layouts that must be kept in step.
fn identity_indices(count: u32) -> Vec<u32> {
    (0..count).collect()
}

/// Mid-ring candidate spacing. Must mirror `mid_placement_spacing` in grass.wgsl.
fn mid_placement_spacing(cards: bool, card_spacing: f32) -> f32 {
    if cards {
        card_spacing
    } else {
        MID_PLACEMENT_SPACING
    }
}

// Above-one coverage adds candidates instead of saturating a retained fraction.
// Keep these divisors identical to the two WGSL spacing helpers. Fixed grid and
// instance caps continue to bound the workload at the densest settings.
fn coverage_grid_scale(master: f32, ring: f32) -> f32 {
    (master.clamp(1.0, 2.0) * ring.clamp(1.0, 50.0)).sqrt()
}

// Preserve the requested disc at the candidate-grid budget. The shader widens
// clumps for the residual coverage, rather than shrinking the grass radius.
fn bounded_mid_spacing(base: f32, master: f32, ring: f32, radius: f32) -> f32 {
    (base / coverage_grid_scale(master, ring))
        .max(radius / (MID_GRID_MAX as f32 * 0.5 - 2.0))
}

/// Live override of the photo-card placement grid.
///
/// The uniform lane and the dispatch dims are both recomputed per frame from
/// `self.card_spacing`, so this takes effect on the next frame with no
/// reallocation -- the instance buffers are capped independently of the grid.
///
/// Deliberately its own entry point rather than a `WgrGrassParams` field, for
/// the same reason `wgr_set_cloud_shadow_strength` is: that struct's size is
/// part of the ABI handshake. The "renderer-owned, never from the caller" note
/// on the uniform still holds for MISSION and CONFIG data -- this is the dev
/// panel reaching in, which is the one caller that should be able to.
impl Grass {
    pub fn set_card_spacing(&mut self, spacing: f32) {
        if spacing.is_finite() {
            self.card_spacing = spacing.clamp(CARD_SPACING_RANGE.0, CARD_SPACING_RANGE.1);
        }
    }

    /// RFG-090: photo-card tone on/off (see `card_tone_off`). Applies at the next
    /// set_params, which C++ pushes every frame.
    pub fn set_card_tone_enabled(&mut self, enabled: bool) {
        self.card_tone_off = !enabled;
    }

    /// RFG-090: the near ring draws cards only when cards are on, the clump renderer is
    /// on AND the caller has not asked for blades on the near ring (native Reforger
    /// worlds with their own blade atlas). Mirrors `cards_on_near` in grass.wgsl and the
    /// placement-side `cards_near`; the three used to be computed separately and the
    /// vertex path stayed on cards after placement had moved to blades.
    fn near_draws_cards(&self) -> bool {
        self.tuft_enabled
            && self.last_params.clump_renderer != 0.0
            && self.last_params.near_blades_only == 0.0
    }
}

fn card_spacing_from_env() -> f32 {
    std::env::var("WGR_GRASS_CARD_SPACING")
        .ok()
        .and_then(|v| v.parse::<f32>().ok())
        .filter(|v| v.is_finite())
        .unwrap_or(CARD_SPACING_DEFAULT)
        .clamp(CARD_SPACING_RANGE.0, CARD_SPACING_RANGE.1)
}

/// PHOTO CARD TONE (uniform `card_tone` in grass.wgsl; renderer-owned, never
/// from the caller).
///
/// `.x` target: the corrected mean luma every plate is lifted TO, `.y` the gain
/// cap. This stands in for the clutter MATERIAL, which the card path never
/// reads: Arma's clutter rvmats carry large lighting gains -- Takistan's
/// c_plants.rvmat / c_grass_desert.rvmat are ambient 10, forcedDiffuse 2.5,
/// diffuse 0; its bunch rvmats ambient 2.3, forcedDiffuse 0.65 -- and the
/// plates are authored dark to be multiplied by them. Decoded with Bohemia's own
/// ImageToPAA, the seven Takistan plates have opaque linear luma means of
/// 0.054 (c_grassgreen_grouphard = TK_BrushHard), 0.056 (weed2), 0.076
/// (thistle), 0.107 (plants_white), 0.139 (grassgreen), 0.159 (bunch) and 0.243
/// (grassdry), against 0.63 for the A3 stock plate the tone chain was tuned on.
/// Under the same sun the terrain renders at ~0.44 linear, so an unlifted TK
/// card is a flat dark silhouette -- in BOTH rings; the "near ring is fine"
/// reading was Takistan's object bushes at 5-15 m, not cards.
///
/// The gain is per LAYER, from the plate mean the renderer already measures
/// (`layer_means`), never below 1 (no plate is darkened, so an A3 atlas whose
/// plates sit at or above the target is untouched) and capped at `.y`. The
/// exact per-class material gains would need the C++ atlas builder to read
/// each clutter class's rvmat and pass a per-layer gain across the ABI; until
/// then this is the data-driven stand-in. WGR_GRASS_CARD_TONE=<target> (0 =
/// off, i.e. the old look), WGR_GRASS_CARD_TONE_MAX=<cap>.
///
/// `.z` card flutter: how much of the wind field's TURBULENCE term the cards
/// take (0..1). The turbulence is blade-tip flutter: a 0.105 /m noise scrolled
/// at ~97 m/s (gust_scroll * 2.1) is a ~10 Hz signal, +-3..6 cm at the tip.
/// On a 5 cm blade tip that is flutter; on a rigid 1-2 m plate it is the whole
/// plant vibrating at 10 Hz -- ~1 px at 50 m, ~8 px at 5 m -- under a 1 px
/// alpha transition, which is temporal aliasing by construction: exactly the
/// "flickering a lot" a fixed camera sees with nothing else changing. Cards
/// default to the gust-only sway (0); WGR_GRASS_CARD_FLUTTER=1 is the A/B back.
const CARD_TONE_TARGET_DEFAULT: f32 = 0.63;
const CARD_TONE_GAIN_MAX_DEFAULT: f32 = 8.0;
const CARD_FLUTTER_DEFAULT: f32 = 0.0;

#[derive(Clone, Copy, Debug, PartialEq)]
struct CardTone {
    target: f32,
    gain_max: f32,
    flutter: f32,
}

impl CardTone {
    fn from_env() -> Self {
        Self::from_env_values(
            std::env::var("WGR_GRASS_CARD_TONE").ok().as_deref(),
            std::env::var("WGR_GRASS_CARD_TONE_MAX").ok().as_deref(),
            std::env::var("WGR_GRASS_CARD_FLUTTER").ok().as_deref(),
        )
    }

    fn from_env_values(
        target: Option<&str>,
        gain_max: Option<&str>,
        flutter: Option<&str>,
    ) -> Self {
        let parse = |v: Option<&str>, default: f32| {
            v.and_then(|s| s.trim().parse::<f32>().ok())
                .filter(|f| f.is_finite())
                .unwrap_or(default)
        };
        Self {
            // 0 switches the lift off; anything else is a luma target.
            target: parse(target, CARD_TONE_TARGET_DEFAULT).clamp(0.0, 2.0),
            gain_max: parse(gain_max, CARD_TONE_GAIN_MAX_DEFAULT).clamp(1.0, 16.0),
            flutter: parse(flutter, CARD_FLUTTER_DEFAULT).clamp(0.0, 1.0),
        }
    }
}

#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
struct GrassParams {
    density: f32,
    spacing: f32,
    near_radius: f32,
    enabled: f32,
    blade_height: f32,
    wind_strength: f32,
    wind_direction: f32,
    far_radius: f32,
    interactor_x: f32,
    interactor_z: f32,
    interactor_radius: f32,
    interactor_strength: f32,
    tracks: [WgrGrassTrack; WGR_GRASS_TRACK_COUNT],
    downwash: [WgrGrassDownwash; WGR_GRASS_DOWNWASH_COUNT],
    debug_ignore_geography_exclusions: f32,
    clumping: f32,
    color_variation: f32,
    transmission: f32,
    cast_shadows: f32,
    apply_fog: f32,
    density_noise_scale: f32,
    density_noise_strength: f32,
    // Species mix; grass takes whatever the two do not. Kept as a trailing vec4
    // so the UBO stays 16-byte aligned (1632 B = 102 * 16).
    weed_percent: f32,
    flower_percent: f32,
    blade_width_scale: f32,
    use_photo_tuft: f32,
    // Albedo saturation about luma; 1.0 = untouched. Trailing vec4 keeps the
    // UBO 16-byte aligned (1648 B = 103 * 16).
    saturation: f32,
    dry_patches: f32,
    dry_patch_scale: f32,
    mid_radius: f32,
    // Shape and card controls. Trailing vec4 pair keeps the UBO 16-byte aligned.
    //   shape_mix = (variety, taper jitter, bend jitter, blade texture strength)
    //   cards     = (alpha cards on, alpha cutoff, card widening, spare)
    shape_variety: f32,
    taper_jitter: f32,
    bend_jitter: f32,
    blade_texture_strength: f32,
    alpha_cards: f32,
    alpha_cutoff: f32,
    card_widen: f32,
    blade_arch: f32,
    clump_renderer: f32,
    photo_tuft_brightness: f32,
    photo_tuft_mix: f32,
    photo_tuft_patch_size: f32,
    // Photo-card look block; mirrors `photo` in grass.wgsl.
    photo_contrast: f32,
    photo_contour: f32,
    photo_self_shadow: f32,
    photo_root_ao: f32,
    // Placement block; mirrors `place` in grass.wgsl.
    near_grid_dim: f32,
    mid_grid_dim: f32,
    lod_blend: f32,
    photo_alpha_cutoff: f32,
    // Layer-mix block; mirrors `photo_mix` and `layer_weights` in grass.wgsl.
    photo_force_layer: f32,
    photo_saturation: f32,
    track_lifetime: f32,
    imprint_depth: f32,
    photo_layer_weights: [f32; 8],
    // Per-LOD coverage block; mirrors `lod_density` in grass.wgsl.
    near_density: f32,
    mid_density: f32,
    far_density: f32,
    // Photo-card placement spacing in metres (lod_density.w). Renderer-owned:
    // CARD_SPACING_DEFAULT or WGR_GRASS_CARD_SPACING, never from the caller.
    card_spacing: f32,
    tint_procedural: [f32; 4],
    tint_photo: [f32; 4],
    // Photo-card coverage block; mirrors `card_look` in grass.wgsl.
    //   .x = card coverage 0..1 (fraction of clutter cells that grow a card)
    //   .y = auto: 1 = authored cells keep the map's own density and .x is only
    //        the fallback for unanswered cells; 0 = .x multiplies everywhere
    //   .z = card size multiplier (1 = stock), .w spare
    card_coverage: f32,
    card_coverage_auto: f32,
    card_scale: f32,
    // card_look.w: shadow blade stride (see SHADOW_STRIDE_DEFAULT). Renderer-owned.
    shadow_blade_stride: f32,
    // Procedural-blade look block; mirrors `blade_look` in grass.wgsl. Near
    // blades and mid ribbons only. Defaults are the pre-existing look.
    //   .x = blade self shadow, .y = contrast, .z = hue variation, .w = root shade
    blade_self_shadow: f32,
    blade_contrast: f32,
    blade_hue_variation: f32,
    blade_root_shade: f32,
    // Photo-card tone block; mirrors `card_tone` in grass.wgsl and
    // grass_shadow.wgsl. Renderer-owned (see CardTone), never from the caller.
    //   .x = per-layer tone target luma (0 = off), .y = tone gain cap,
    //   .z = card wind flutter 0..1, .w = wind noise-field scroll multiplier
    // The .w lane is NOT renderer-owned like the other three: it is the caller's
    // `wind_scroll`, passed through. It rides in this block because card_tone.w was
    // the one free lane the shaders already had, which keeps the whole slider off
    // the ABI struct layout.
    card_tone_target: f32,
    card_tone_gain_max: f32,
    card_flutter: f32,
    wind_scroll: f32,
    // RFG-090: mirrors `native` in grass.wgsl. .x = near ring keeps its blades while
    // cards are on, and the mid ring's cards begin at the camera; .yzw spare.
    near_blades_only: f32,
    native_tint: [f32; 3],
    // RFG-091: `native2`: .x tint lerp; .y gust variation (-1 = old gusts), .z front metres; .w spare.
    native_tint_lerp: f32,
    _native2_pad: [f32; 3],
    // Renderer-derived from the uploaded atlas, never from the caller.
    photo_layer_bounds: [f32; 128],
    photo_layer_means: [f32; 128],
    wind_advection: [f32; 4],
    wind_flutter: [f32; 4],
    // Eight source-opacity-weighted linear means of blade_tex, not tuft_tex.
    // Renderer-private; w=0 leaves empty/unavailable layers on the sampled path.
    blade_layer_means: [f32; 32],
}

/// Owner's fallback for photo-card coverage where the map's own clutter density
/// is unknown (every OFP world, and any Arma cell the mask bake left unanswered).
/// A card then grows in roughly one clutter cell in four; on the 1.11 m grid that
/// is ~0.19 cards/m^2, or one card every ~2.3 m on average.
pub const CARD_COVERAGE_DEFAULT: f32 = 0.24;

/// Mirrors `GrassInstance` in grass.wgsl and grass_shadow.wgsl (32 B). `packed`
/// carries what the compute placement pass resolves once per blade -- currently
/// the flattening direction/strength that both vertex shaders used to rebuild
/// per vertex from the 96-entry track ring.
#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
struct GrassInstance {
    pos_seed: [f32; 4],
    packed: [u32; 4],
}

#[derive(Clone, Copy)]
pub enum GrassPass {
    Color,
    ColorNoWrite,
    Prepass,
}

/// GRS-A instance accounting. Counts are the compacted instance totals read back
/// from the three atomic placement counters; the candidate totals are the fixed
/// dispatch sizes, so `accepted / candidates` is the placement acceptance rate.
#[derive(Clone, Copy, Default)]
pub struct GrassStats {
    pub near_instances: u32,
    pub mid_instances: u32,
    pub far_instances: u32,
    pub near_candidates: u32,
    pub mid_candidates: u32,
    pub far_candidates: u32,
    pub near_vertices: u32,
    pub mid_vertices: u32,
    pub far_vertices: u32,
}

// Round-robin readback of the three placement counters. Same non-blocking
// discipline as gpu_timers: copy in the frame encoder, map_async after submit,
// drain with a Poll next frame. A saturated ring just skips a sample.
const STATS_SLOTS: usize = 3;
// Three u32 counts at offsets 0/4/8 (padded to 16), then the fragment census
// lanes (FRAG_WORDS u32s) that WGR_GRASS_COUNT_FRAGMENTS fills.
const STATS_BYTES: u64 = 16 + FRAG_WORDS as u64 * 4;
// Fragment census layout; mirrors frag_counts / count_fragment in grass.wgsl.
// Lane 0 near prepass, 1 mid prepass, 2 near colour, 3 mid colour, 4 near shadow
// (summed over every cascade that drew grass), 5..8 spare.
const FRAG_LANES: usize = 8;
const FRAG_STRIPES: usize = 32;
const FRAG_WORDS: usize = FRAG_LANES * FRAG_STRIPES;
const FRAG_LANE_NEAR_PREPASS: usize = 0;
const FRAG_LANE_MID_PREPASS: usize = 1;
const FRAG_LANE_NEAR_COLOR: usize = 2;
const FRAG_LANE_MID_COLOR: usize = 3;
const FRAG_LANE_NEAR_SHADOW: usize = 4;
// How often the census line goes to the game log (harvests, roughly frames).
const CENSUS_PERIOD: u32 = 240;

/// SHADOW BLADE STRIDE. The cascade pass has no fragment shader and still costs the
/// same per triangle as the prepass (perf_combat, 1600x900: 0.22 ns/tri in both), so
/// it is bound by triangles, and near-ring shadows are 120 triangles per clump into
/// every cascade the mask keeps -- two of the four on the reference scene, i.e. the
/// near clumps are transformed FOUR times a frame and the two shadow copies are the
/// largest single grass region once the per-cascade timer rows are added up. In the
/// shadow map a clump is a coverage stipple, not twelve resolvable blades: cascade 1's
/// texel is about a blade wide. So the shadow draws every stride-th blade of the real
/// clump, widened by the stride (vs_grass_shadow in grass_shadow.wgsl), which keeps
/// the coverage the ground receives and divides the shadow triangles by the stride.
/// WGR_GRASS_SHADOW_STRIDE=1 is the old behaviour exactly, for the A/B on one binary.
const SHADOW_STRIDE_DEFAULT: u32 = 2;
const SHADOW_STRIDE_MAX: u32 = 12;

fn shadow_stride_from_env() -> u32 {
    std::env::var("WGR_GRASS_SHADOW_STRIDE")
        .ok()
        .and_then(|v| v.trim().parse::<u32>().ok())
        .unwrap_or(SHADOW_STRIDE_DEFAULT)
        .clamp(1, SHADOW_STRIDE_MAX)
}

/// Fragment census summed from the striped counters; see FRAG_LANE_*.
#[derive(Clone, Copy, Default)]
struct FragStats {
    lanes: [u64; FRAG_LANES],
}

enum StatsSlot {
    Idle,
    Pending,
    InFlight(std::sync::mpsc::Receiver<Result<(), wgpu::BufferAsyncError>>),
}

pub struct Grass {
    enabled: bool,
    // `WGR_GRASS=0` as an AUTHORITATIVE kill switch, not merely an initial default.
    // The default it seeded in `new()` was overwritten by the first
    // `SetGrassSettings` upload a few frames later, so a capture launched with
    // WGR_GRASS=0 still drew a full field -- which silently invalidates any A/B
    // that uses it to expose the ground under the grass.
    force_off: bool,
    casts_shadows: bool,
    far_enabled: bool,
    terrain_params: wgpu::Buffer,
    grass_params: wgpu::Buffer,
    placement_count: wgpu::Buffer,
    indirect: wgpu::Buffer,
    near_index: wgpu::Buffer,
    mid_index: wgpu::Buffer,
    // Which table each index buffer currently holds, so it is rewritten only when the
    // renderer mode actually changes rather than every frame.
    near_index_mode: std::cell::Cell<u8>,
    mid_index_mode: std::cell::Cell<u8>,
    mid_placement_count: wgpu::Buffer,
    mid_indirect: wgpu::Buffer,
    far_placement_count: wgpu::Buffer,
    far_indirect: wgpu::Buffer,
    terrain_layout: wgpu::BindGroupLayout,
    terrain_bind: wgpu::BindGroup,
    data_bind: wgpu::BindGroup,
    mid_data_bind: wgpu::BindGroup,
    far_data_bind: wgpu::BindGroup,
    heightmap_view: wgpu::TextureView,
    geography: wgpu::Texture,
    geography_view: wgpu::TextureView,
    terrain_generation: u64,
    // Sinkhole W1: the placement's own copy of the terrain hole edges, refreshed when the terrain's changes.
    holes: wgpu::Buffer,
    holes_gen: u64,
    have_heightmap: bool,
    place_pipeline: wgpu::ComputePipeline,
    mid_place_pipeline: wgpu::ComputePipeline,
    far_place_pipeline: wgpu::ComputePipeline,
    color_pipeline: wgpu::RenderPipeline,
    color_no_write_pipeline: wgpu::RenderPipeline,
    // Alpha cut-out variants. Separate pipelines rather than a flag inside
    // fs_grass: a `discard` anywhere in the shader disables early-Z for the whole
    // pipeline even when the branch never fires, which measured as +67% on the
    // grass colour pass with the feature switched off.
    cards_color_pipeline: wgpu::RenderPipeline,
    cards_color_no_write_pipeline: wgpu::RenderPipeline,
    prepass_pipeline: wgpu::RenderPipeline,
    mid_prepass_pipeline: wgpu::RenderPipeline,
    mid_color_pipeline: wgpu::RenderPipeline,
    mid_color_no_write_pipeline: wgpu::RenderPipeline,
    // The photo-tuft path needs its own pipelines: entry points are immutable
    // pipeline state, so rebinding a newly uploaded tuft alone must never leave
    // the mid ring drawing the procedural shader (GRS-E regression guard).
    // Under MSAA these six are built with alpha_to_coverage and the *_a2c
    // fragment entries (see `card_a2c`); at 1x they are the hard-cutout entries.
    mid_tuft_prepass_pipeline: wgpu::RenderPipeline,
    mid_tuft_color_pipeline: wgpu::RenderPipeline,
    mid_tuft_color_no_write_pipeline: wgpu::RenderPipeline,
    near_tuft_prepass_pipeline: wgpu::RenderPipeline,
    near_tuft_color_pipeline: wgpu::RenderPipeline,
    near_tuft_color_no_write_pipeline: wgpu::RenderPipeline,
    far_color_pipeline: wgpu::RenderPipeline,
    far_color_no_write_pipeline: wgpu::RenderPipeline,
    shadow_pipeline: wgpu::RenderPipeline,
    tuft_shadow_pipeline: wgpu::RenderPipeline,
    // The blade shadow draw has its own index table (every stride-th blade) and its
    // own indirect args (fewer indices per instance, same instance count), so the
    // colour draw's buffers are untouched by the stride. See SHADOW_STRIDE_DEFAULT.
    shadow_index: wgpu::Buffer,
    shadow_indirect: wgpu::Buffer,
    shadow_index_mode: std::cell::Cell<u8>,
    shadow_stride: u32,
    // Fragment census (WGR_GRASS_COUNT_FRAGMENTS): the striped atomic counters the
    // *_count fragment entries write, and what the last readback summed them to.
    frag_counts: wgpu::Buffer,
    count_frags: bool,
    frag_stats: FragStats,
    // draw_shadow calls this frame (one per cascade that received grass), and the
    // last completed frame's total: the "Grass shadow" timer row reports ONE cascade,
    // so this is the multiplier a reader needs to turn it into the frame's cost.
    shadow_draws: std::cell::Cell<u32>,
    shadow_draws_latest: u32,
    census_ticks: u32,
    stats_buffers: Vec<(wgpu::Buffer, StatsSlot)>,
    stats: GrassStats,
    // GRS-E: kept so the three data binds can be rebuilt when the tuft arrives
    // (a bind group holds its resources, so a new texture needs new groups).
    data_layout: wgpu::BindGroupLayout,
    instances: wgpu::Buffer,
    mid_instances: wgpu::Buffer,
    far_instances: wgpu::Buffer,
    blade_view: wgpu::TextureView,
    blade_sampler: wgpu::Sampler,
    have_blade_atlas: bool,
    tuft_view: wgpu::TextureView,
    // Pixels per radian of the current view: (viewport_height/2) * proj[1][1].
    // Set from render_frame, where both are already in scope. 0 = not yet known,
    // which disables the projected-size clamp rather than collapsing the ring.
    px_per_radian: f32,
    // The last params the game pushed, unmodified. Kept so a resize can re-derive
    // the clamp without waiting for the next push -- the derivation runs forward
    // from the raw slider values, and re-clamping an already-clamped radius would
    // ratchet it down every frame.
    last_raw_params: WgrGrassParams,
    have_tuft: bool,
    // have_tuft = the PAA loaded; tuft_enabled = the Grass tab also asked for it.
    // Default off: the procedural mid ribbons are the known-good look.
    tuft_enabled: bool,
    // Near candidate grid edge in cells, derived in set_params from radius and
    // spacing. Drives the compute dispatch, so a short radius costs less than a
    // long one instead of every radius paying for one fixed 512^2 grid.
    near_grid_dim: u32,
    // Mid candidate grid edge, derived the same way from the mid ring's reach.
    mid_grid_dim: u32,
    // Opaque bounds of the uploaded photo atlas, (u0, u1, v0, v1) per layer.
    tuft_bounds: [f32; 128],
    // Mean covered colour per layer, the contrast pivot.
    tuft_means: [f32; 128],
    // Last pushed params, so set_tuft can re-publish them with the tuft flag set
    // without waiting for the Grass tab to touch a slider.
    last_params: GrassParams,
    wind_phase: wind::WindPhase,
    // Photo-card placement spacing in metres; see CARD_SPACING_DEFAULT.
    card_spacing: f32,
    // The card pipelines were built with alpha-to-coverage (MSAA scene and
    // WGR_GRASS_CARD_A2C != 0). Recorded for the log line and the stats panel;
    // the pipelines themselves are fixed at construction.
    #[allow(dead_code)]
    card_a2c: bool,
    // Photo-card tone lift and wind flutter; see CardTone.
    card_tone: CardTone,
    /// RFG-090: the photo-card tone (target luma + gain) exists for flatly lit A3 plates.
    /// A Reforger clutter atlas is authored BCR albedo and was being pushed to luma 0.63
    /// at up to 8x gain -- a field of white plants. Off while that atlas is the source.
    card_tone_off: bool,
    // Lines for the GAME log, drained by lib.rs (which owns the LogSink) after
    // every set_params / set_tufts. eprintln! goes to a stderr the launcher
    // discards, so a lever announced only there cannot be proven from a run.
    pending_log: Vec<String>,
    // The last card state announced, so set_params (called every frame) only
    // writes a line when something actually changes.
    announced_cards: Option<CardStateKey>,
}

/// What the card-state log line keys on. Any change re-announces.
#[derive(Clone, Copy, PartialEq, Debug)]
struct CardStateKey {
    have_tuft: bool,
    opt_in: bool,
    cards_mid: bool,
    cards_near: bool,
    near_grid_dim: u32,
    mid_grid_dim: u32,
    coverage_centi: u32,
    coverage_auto: bool,
}

impl Grass {
    pub fn new(
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        camera_layout: &wgpu::BindGroupLayout,
        shadow_pass_layout: &wgpu::BindGroupLayout,
        surface_format: wgpu::TextureFormat,
        sample_count: u32,
        composer: &mut naga_oil::compose::Composer,
    ) -> Self {
        let enabled = std::env::var("WGR_GRASS").map(|v| v != "0").unwrap_or(true);
        let terrain_params = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("wgr_grass_terrain_params"),
            size: std::mem::size_of::<WgrTerrainParams>() as u64,
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        let terrain_default = WgrTerrainParams {
            world_origin: glam::Vec2::ZERO,
            land_grid: 1.0,
            terrain_grid: 1.0,
            hm_width: 1,
            hm_height: 1,
            land_range: 1,
            data_scale: 1.0,
            sea_level: 0.0,
            time: 0.0,
            swash_speed: 0.0,
            swash_amp: 0.0,
            wet_height: 0.0,
            wet_darken: 1.0,
            enfusion_ground: 0.0,
            snowline_height: -1.0,
            snowline_range: 25.0,
            snowline_depth: 0.06,
            _pad1: 0.0,
            _pad2: 0.0,
            _pad3: 0.0,
            _pad4: 0.0,
        };
        queue.write_buffer(&terrain_params, 0, bytemuck::bytes_of(&terrain_default));
        let grass_params = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("wgr_grass_params"),
            size: std::mem::size_of::<GrassParams>() as u64,
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        let density = std::env::var("WGR_GRASS_DENSITY")
            .ok()
            .and_then(|v| v.parse::<f32>().ok())
            .unwrap_or(0.5)
            .clamp(0.0, 2.0);
        let radius = std::env::var("WGR_GRASS_DISTANCE")
            .ok()
            .and_then(|v| v.parse::<f32>().ok())
            .unwrap_or(50.0)
            .clamp(8.0, 60.0);
        let card_spacing = card_spacing_from_env();
        // Alpha-to-coverage for the photo cards: needs MSAA (wgpu rejects it at
        // 1x), default-on there like the object foliage's WGR_FOLIAGE_A2C;
        // WGR_GRASS_CARD_A2C=0 is the A/B back to the hard cutout.
        let card_a2c = sample_count > 1
            && std::env::var("WGR_GRASS_CARD_A2C")
                .map(|v| v != "0")
                .unwrap_or(true);
        let card_tone = CardTone::from_env();
        let shadow_stride = shadow_stride_from_env();
        let count_frags = std::env::var("WGR_GRASS_COUNT_FRAGMENTS")
            .map(|v| v != "0")
            .unwrap_or(false);
        // Announced through the GAME log (pending_log, drained by lib.rs on the
        // first set_params): the stderr copy below is discarded by the launcher,
        // and a lever that only announces there cannot be proven from a run.
        let cards_line = format!(
            "Wgpu grass cards: spacing {card_spacing:.2} m (WGR_GRASS_CARD_SPACING), a2c={} \
             (WGR_GRASS_CARD_A2C, msaa {sample_count}x), coverage default {CARD_COVERAGE_DEFAULT:.2}, \
             tone target {:.2} cap x{:.1} (WGR_GRASS_CARD_TONE/_MAX), flutter {:.2} (WGR_GRASS_CARD_FLUTTER)",
            card_a2c as u8, card_tone.target, card_tone.gain_max, card_tone.flutter
        );
        eprintln!("[wgr] {cards_line}");
        let shadow_line = format!(
            "Wgpu grass shadow: blade stride {shadow_stride} (WGR_GRASS_SHADOW_STRIDE; 1 = every \
             blade), fragment census {} (WGR_GRASS_COUNT_FRAGMENTS; timings from a counting run \
             are not quotable)",
            if count_frags { "on" } else { "off" }
        );
        eprintln!("[wgr] {shadow_line}");
        let params = GrassParams {
            density,
            // Dense 0.25 m grid; the 512x512 placement buffer supports this
            // without clipping the visible field at the old 65,536-instance cap.
            spacing: 0.25,
            near_radius: radius,
            enabled: if enabled { 1.0 } else { 0.0 },
            blade_height: 1.0,
            wind_strength: 0.75,
            wind_direction: 0.0,
            // The far ring's accept band opens past the mid ring's reach, so a
            // far radius equal to the detail radius emits nothing at all.
            far_radius: 0.0,
            interactor_x: 0.0,
            interactor_z: 0.0,
            interactor_radius: 0.0,
            interactor_strength: 0.0,
            tracks: [WgrGrassTrack {
                x: 0.0,
                z: 0.0,
                radius: 0.0,
                age: 0.0,
            }; WGR_GRASS_TRACK_COUNT],
            downwash: [WgrGrassDownwash {
                x: 0.0,
                z: 0.0,
                radius: 0.0,
                strength: 0.0,
            }; WGR_GRASS_DOWNWASH_COUNT],
            debug_ignore_geography_exclusions: 0.0,
            clumping: 0.55,
            color_variation: 0.35,
            transmission: 0.45,
            cast_shadows: 1.0,
            apply_fog: 1.0,
            density_noise_scale: 0.075,
            density_noise_strength: 0.55,
            weed_percent: 0.12,
            flower_percent: 0.05,
            blade_width_scale: 1.0,
            use_photo_tuft: 0.0,
            saturation: 0.72,
            dry_patches: 0.08,
            dry_patch_scale: 0.003,
            mid_radius: 500.0,
            // Shape variety defaults ON: four grass species sharing one
            // silhouette is the defect, not a look worth preserving by default.
            // 0 still reproduces it exactly for A/B.
            shape_variety: 1.0,
            taper_jitter: 0.35,
            bend_jitter: 0.30,
            blade_texture_strength: 0.0,
            // Alpha cards OFF by default: they cost early-Z and add overdraw, so
            // adopting them is a measured decision, not a default.
            alpha_cards: 0.0,
            alpha_cutoff: 0.5,
            card_widen: 1.6,
            blade_arch: 1.0,
            clump_renderer: 1.0,
            // Halved 2026-08-27 to match Engine.hpp's GrassSettings default; the C++ side
            // normally overwrites this, so a stale value here only shows up on a path that
            // never uploads settings -- which is exactly where a silent mismatch hides.
            photo_tuft_brightness: 0.625,
            photo_tuft_mix: 1.0,
            photo_tuft_patch_size: 18.0,
            photo_contrast: 1.35,
            photo_contour: 1.85,
            photo_self_shadow: 1.0,
            photo_root_ao: 0.45,
            near_grid_dim: 256.0,
            mid_grid_dim: 512.0,
            lod_blend: 30.0,
            photo_alpha_cutoff: 0.5,
            photo_force_layer: -1.0,
            photo_saturation: 0.62,
            track_lifetime: 600.0,
            imprint_depth: 0.55,
            photo_layer_weights: [1.0; 8],
            near_density: 1.0,
            mid_density: 4.0,
            far_density: 1.0,
            card_spacing,
            tint_procedural: [1.0, 1.0, 1.0, 1.0],
            tint_photo: [1.0, 1.0, 1.0, 1.0],
            card_coverage: CARD_COVERAGE_DEFAULT,
            // Coverage applies on every world, authored or not: one card is two to
            // three of Arma's clutter plants in plate area, and the measured
            // popping rate on Takistan falls 3.68% -> 1.66% between the map's own
            // density and 0.25. The C++ side owns the default (GrassSettings::
            // photoCoverageAuto, now false); 1.0 here is only the value a renderer
            // built without the game would start from.
            card_coverage_auto: 0.0,
            card_scale: 1.0,
            shadow_blade_stride: shadow_stride as f32,
            blade_self_shadow: 0.0,
            blade_contrast: 1.0,
            blade_hue_variation: 0.0,
            blade_root_shade: 0.70,
            card_tone_target: card_tone.target,
            card_tone_gain_max: card_tone.gain_max,
            card_flutter: card_tone.flutter,
            wind_scroll: 1.0,
            near_blades_only: 0.0,
            native_tint: [0.0; 3],
            native_tint_lerp: 0.0,
            _native2_pad: [1.0, 45.0, 0.0],
            photo_layer_bounds: default_layer_bounds(),
            photo_layer_means: default_layer_means(),
            wind_advection: [0.0; 4],
            wind_flutter: [0.0, 0.0, 1.0, 0.0],
            blade_layer_means: [0.0; 32],
        };
        queue.write_buffer(&grass_params, 0, bytemuck::bytes_of(&params));

        let heightmap = device.create_texture(&wgpu::TextureDescriptor {
            label: Some("wgr_grass_heightmap_standin"),
            size: wgpu::Extent3d {
                width: 1,
                height: 1,
                depth_or_array_layers: 1,
            },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: wgpu::TextureFormat::R32Float,
            usage: wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::COPY_DST,
            view_formats: &[],
        });
        queue.write_texture(
            wgpu::TexelCopyTextureInfo {
                texture: &heightmap,
                mip_level: 0,
                origin: wgpu::Origin3d::ZERO,
                aspect: wgpu::TextureAspect::All,
            },
            bytemuck::bytes_of(&0.0f32),
            wgpu::TexelCopyBufferLayout {
                offset: 0,
                bytes_per_row: Some(4),
                rows_per_image: Some(1),
            },
            wgpu::Extent3d {
                width: 1,
                height: 1,
                depth_or_array_layers: 1,
            },
        );
        let heightmap_view = heightmap.create_view(&wgpu::TextureViewDescriptor::default());
        let geography = make_geography_texture(device, 1, 1);
        queue.write_texture(
            wgpu::TexelCopyTextureInfo {
                texture: &geography,
                mip_level: 0,
                origin: wgpu::Origin3d::ZERO,
                aspect: wgpu::TextureAspect::All,
            },
            bytemuck::bytes_of(&0u32),
            wgpu::TexelCopyBufferLayout {
                offset: 0,
                bytes_per_row: Some(4),
                rows_per_image: Some(1),
            },
            wgpu::Extent3d {
                width: 1,
                height: 1,
                depth_or_array_layers: 1,
            },
        );
        let geography_view = geography.create_view(&wgpu::TextureViewDescriptor::default());

        let terrain_layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("wgr_grass_terrain_layout"),
            entries: &[
                wgpu::BindGroupLayoutEntry {
                    binding: 0,
                    visibility: wgpu::ShaderStages::VERTEX
                        | wgpu::ShaderStages::FRAGMENT
                        | wgpu::ShaderStages::COMPUTE,
                    ty: wgpu::BindingType::Buffer {
                        ty: wgpu::BufferBindingType::Uniform,
                        has_dynamic_offset: false,
                        min_binding_size: wgpu::BufferSize::new(
                            std::mem::size_of::<WgrTerrainParams>() as u64,
                        ),
                    },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 1,
                    visibility: wgpu::ShaderStages::VERTEX | wgpu::ShaderStages::COMPUTE,
                    ty: wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Float { filterable: false },
                        view_dimension: wgpu::TextureViewDimension::D2,
                        multisampled: false,
                    },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 2,
                    // Fragment-stage geography tests prevent the outer
                    // terrain-coverage LOD from colouring across roads.
                    visibility: wgpu::ShaderStages::FRAGMENT | wgpu::ShaderStages::COMPUTE,
                    ty: wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Uint,
                        view_dimension: wgpu::TextureViewDimension::D2,
                        multisampled: false,
                    },
                    count: None,
                },
                // Sinkhole W1: terrain hole edges (TerrainHoles in grass.wgsl); placement skips holes.
                wgpu::BindGroupLayoutEntry {
                    binding: 3,
                    visibility: wgpu::ShaderStages::COMPUTE,
                    ty: wgpu::BindingType::Buffer {
                        ty: wgpu::BufferBindingType::Storage { read_only: true },
                        has_dynamic_offset: false,
                        min_binding_size: wgpu::BufferSize::new(crate::terrain::TERRAIN_HOLES_BYTES),
                    },
                    count: None,
                },
            ],
        });
        let holes = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("wgr_grass_terrain_holes"),
            size: crate::terrain::TERRAIN_HOLES_BYTES,
            usage: wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        let instances = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("wgr_grass_instances"),
            size: (MAX_INSTANCES as usize * std::mem::size_of::<GrassInstance>()) as u64,
            usage: wgpu::BufferUsages::STORAGE,
            mapped_at_creation: false,
        });
        let mid_instances = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("wgr_grass_mid_instances"),
            size: (MAX_MID_INSTANCES as usize * std::mem::size_of::<GrassInstance>()) as u64,
            usage: wgpu::BufferUsages::STORAGE,
            mapped_at_creation: false,
        });
        let far_instances = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("wgr_grass_far_instances"),
            size: (MAX_FAR_INSTANCES as usize * std::mem::size_of::<GrassInstance>()) as u64,
            usage: wgpu::BufferUsages::STORAGE,
            mapped_at_creation: false,
        });
        let placement_count = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("wgr_grass_placement_count"),
            // Binding 2's declared minimum is 16 B; only word 0 is the atomic
            // instance count, while the remaining words are padding.
            size: 16,
            usage: wgpu::BufferUsages::STORAGE
                | wgpu::BufferUsages::COPY_SRC
                | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        // 20 bytes, not 16: draw_indexed_indirect takes five u32s
        // (index_count, instance_count, first_index, base_vertex, first_instance).
        // `instance_count` stays at offset 4 in both layouts, which is what the
        // placement-count copy writes, so that plumbing is untouched.
        let indirect = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("wgr_grass_indirect"),
            size: 20,
            usage: wgpu::BufferUsages::INDIRECT | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        // Sized for the largest table either ring can need: 12 blades x 5 segments x 6.
        let near_index = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("wgr_grass_near_index"),
            size: 12 * 5 * 6 * 4,
            usage: wgpu::BufferUsages::INDEX | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        let mid_index = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("wgr_grass_mid_index"),
            size: 6 * 3 * 6 * 4,
            usage: wgpu::BufferUsages::INDEX | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        let mid_placement_count = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("wgr_grass_mid_placement_count"),
            size: 16,
            usage: wgpu::BufferUsages::STORAGE
                | wgpu::BufferUsages::COPY_SRC
                | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        let mid_indirect = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("wgr_grass_mid_indirect"),
            size: 20,
            usage: wgpu::BufferUsages::INDIRECT | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        let far_placement_count = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("wgr_grass_far_placement_count"),
            size: 16,
            usage: wgpu::BufferUsages::STORAGE
                | wgpu::BufferUsages::COPY_SRC
                | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        let far_indirect = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("wgr_grass_far_indirect"),
            size: 16,
            usage: wgpu::BufferUsages::INDIRECT | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        // Blade shadow draw: its own index table (every stride-th blade of the near
        // clump) and indirect args. Sized like near_index; the instance count is
        // copied from the same placement counter the colour draw uses.
        let shadow_index = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("wgr_grass_shadow_index"),
            size: 12 * 5 * 6 * 4,
            usage: wgpu::BufferUsages::INDEX | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        let shadow_indirect = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("wgr_grass_shadow_indirect"),
            size: 20,
            usage: wgpu::BufferUsages::INDIRECT | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        // Fragment census counters; zeroed every frame in dispatch() after the
        // previous frame's totals are copied out. Bound whether or not the census
        // is on (the layout needs an entry either way); only the *_count entries
        // touch it.
        let frag_counts = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("wgr_grass_frag_counts"),
            size: FRAG_WORDS as u64 * 4,
            usage: wgpu::BufferUsages::STORAGE
                | wgpu::BufferUsages::COPY_SRC
                | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        let data_layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("wgr_grass_data_layout"),
            entries: &[
                wgpu::BindGroupLayoutEntry {
                    binding: 0,
                    visibility: wgpu::ShaderStages::VERTEX
                        | wgpu::ShaderStages::FRAGMENT
                        | wgpu::ShaderStages::COMPUTE,
                    ty: wgpu::BindingType::Buffer {
                        ty: wgpu::BufferBindingType::Uniform,
                        has_dynamic_offset: false,
                        min_binding_size: wgpu::BufferSize::new(
                            std::mem::size_of::<GrassParams>() as u64
                        ),
                    },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 1,
                    visibility: wgpu::ShaderStages::VERTEX | wgpu::ShaderStages::COMPUTE,
                    ty: wgpu::BindingType::Buffer {
                        ty: wgpu::BufferBindingType::Storage { read_only: false },
                        has_dynamic_offset: false,
                        min_binding_size: wgpu::BufferSize::new(
                            std::mem::size_of::<GrassInstance>() as u64,
                        ),
                    },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 2,
                    visibility: wgpu::ShaderStages::COMPUTE,
                    ty: wgpu::BindingType::Buffer {
                        ty: wgpu::BufferBindingType::Storage { read_only: false },
                        has_dynamic_offset: false,
                        min_binding_size: wgpu::BufferSize::new(16),
                    },
                    count: None,
                },
                // GRS-D blade albedo. Fragment-only, so the shadow pipeline (which
                // shares this layout but has no fragment stage) simply omits them:
                // a shader's bindings only need to be a subset of the layout.
                wgpu::BindGroupLayoutEntry {
                    binding: 3,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Float { filterable: true },
                        view_dimension: wgpu::TextureViewDimension::D2Array,
                        multisampled: false,
                    },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 4,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Filtering),
                    count: None,
                },
                // GRS-E photographed tuft for the mid LOD's crossed cards.
                wgpu::BindGroupLayoutEntry {
                    binding: 5,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Float { filterable: true },
                        view_dimension: wgpu::TextureViewDimension::D2Array,
                        multisampled: false,
                    },
                    count: None,
                },
                // Fragment census counters (frag_counts in both shaders). Fragment
                // stage only, like the textures: the compute entries never touch it.
                wgpu::BindGroupLayoutEntry {
                    binding: 6,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Buffer {
                        ty: wgpu::BufferBindingType::Storage { read_only: false },
                        has_dynamic_offset: false,
                        min_binding_size: wgpu::BufferSize::new(FRAG_WORDS as u64 * 4),
                    },
                    count: None,
                },
            ],
        });
        let blade_view = blade_atlas::create(device, queue);
        let tuft_view = blade_atlas::create_tuft_placeholder(device, queue);
        let blade_sampler = device.create_sampler(&wgpu::SamplerDescriptor {
            label: Some("wgr_grass_blade_sampler"),
            // Repeat across the blade width so the edge lift wraps cleanly;
            // clamp along its length so the tip row never bleeds to the root.
            address_mode_u: wgpu::AddressMode::Repeat,
            address_mode_v: wgpu::AddressMode::ClampToEdge,
            address_mode_w: wgpu::AddressMode::ClampToEdge,
            mag_filter: wgpu::FilterMode::Linear,
            min_filter: wgpu::FilterMode::Linear,
            mipmap_filter: wgpu::MipmapFilterMode::Linear,
            anisotropy_clamp: 8,
            ..Default::default()
        });
        let terrain_bind = make_terrain_bind(
            device,
            &terrain_layout,
            &terrain_params,
            &heightmap_view,
            &geography_view,
            &holes,
        );
        let data_bind = make_data_bind(
            device,
            &data_layout,
            &grass_params,
            &instances,
            &placement_count,
            &blade_view,
            &blade_sampler,
            &tuft_view,
            &frag_counts,
        );
        let mid_data_bind = make_data_bind(
            device,
            &data_layout,
            &grass_params,
            &mid_instances,
            &mid_placement_count,
            &blade_view,
            &blade_sampler,
            &tuft_view,
            &frag_counts,
        );
        let far_data_bind = make_data_bind(
            device,
            &data_layout,
            &grass_params,
            &far_instances,
            &far_placement_count,
            &blade_view,
            &blade_sampler,
            &tuft_view,
            &frag_counts,
        );
        let shader = crate::shaders::make_module(
            device,
            composer,
            "wgr_grass_shader",
            include_str!("grass.wgsl"),
            "grass/grass.wgsl",
        );
        let layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("wgr_grass_pipeline_layout"),
            bind_group_layouts: &[
                Some(camera_layout),
                Some(&terrain_layout),
                Some(&data_layout),
            ],
            immediate_size: 0,
        });
        let place_pipeline = device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
            label: Some("wgr_grass_place"),
            layout: Some(&layout),
            module: &shader,
            entry_point: Some("cs_place"),
            compilation_options: Default::default(),
            cache: None,
        });
        let mid_place_pipeline = device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
            label: Some("wgr_grass_place_mid"),
            layout: Some(&layout),
            module: &shader,
            entry_point: Some("cs_place_mid"),
            compilation_options: Default::default(),
            cache: None,
        });
        let far_place_pipeline = device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
            label: Some("wgr_grass_place_far"),
            layout: Some(&layout),
            module: &shader,
            entry_point: Some("cs_place_far"),
            compilation_options: Default::default(),
            cache: None,
        });
        let filter_subpixel = std::env::var("WGR_GRASS_FILTER_SUBPIXEL")
            .map_or(true, |value| value != "0");
        eprintln!("Wgpu grass subpixel shading: {} (WGR_GRASS_FILTER_SUBPIXEL)", filter_subpixel);
        let medium_lighting = std::env::var("WGR_GRASS_MEDIUM_LIGHTING")
            .map_or(true, |value| value != "0");
        eprintln!("Wgpu grass medium lighting: {} (WGR_GRASS_MEDIUM_LIGHTING)", medium_lighting);
        let shading_constants = [
            ("GRASS_FILTER_SUBPIXEL", if filter_subpixel { 1.0 } else { 0.0 }),
            ("GRASS_MEDIUM_LIGHTING", if medium_lighting { 1.0 } else { 0.0 }),
        ];
        // `a2c`: alpha-to-coverage for the photo-card pipelines. Only ever true
        // under MSAA (wgpu validation rejects it at 1x), and only for entries
        // that write a vec4 whose .a is the coverage (fs_grass_mid_tuft_a2c and
        // its prepass twin). Everything else passes false.
        let make_pipeline_a2c = |label: &str,
                                 vertex_entry: &str,
                                 entry: &str,
                                 target: wgpu::TextureFormat,
                                 depth_write: bool,
                                 a2c: bool| {
            device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
                label: Some(label),
                layout: Some(&layout),
                vertex: wgpu::VertexState {
                    module: &shader,
                    entry_point: Some(vertex_entry),
                    compilation_options: Default::default(),
                    buffers: &[],
                },
                primitive: wgpu::PrimitiveState {
                    topology: wgpu::PrimitiveTopology::TriangleList,
                    cull_mode: None,
                    ..Default::default()
                },
                depth_stencil: Some(wgpu::DepthStencilState {
                    format: depth_format(device),
                    depth_write_enabled: Some(depth_write),
                    depth_compare: Some(wgpu::CompareFunction::GreaterEqual),
                    stencil: Default::default(),
                    bias: Default::default(),
                }),
                multisample: wgpu::MultisampleState {
                    count: sample_count,
                    alpha_to_coverage_enabled: a2c,
                    ..Default::default()
                },
                fragment: Some(wgpu::FragmentState {
                    module: &shader,
                    entry_point: Some(entry),
                    compilation_options: wgpu::PipelineCompilationOptions {
                        constants: &shading_constants,
                        ..Default::default()
                    },
                    targets: &[Some(wgpu::ColorTargetState {
                        format: target,
                        blend: None,
                        write_mask: wgpu::ColorWrites::ALL,
                    })],
                }),
                multiview_mask: None,
                cache: None,
            })
        };
        let make_pipeline = |label: &str,
                             vertex_entry: &str,
                             entry: &str,
                             target: wgpu::TextureFormat,
                             depth_write: bool| {
            make_pipeline_a2c(label, vertex_entry, entry, target, depth_write, false)
        };
        // The card entries: A2C twins under MSAA, hard cutout otherwise. Chosen
        // once here so both rings' six pipelines cannot disagree.
        let (tuft_fs, tuft_prepass_fs) = if card_a2c {
            ("fs_grass_mid_tuft_a2c", "fs_grass_mid_tuft_prepass_a2c")
        } else {
            ("fs_grass_mid_tuft", "fs_grass_mid_tuft_prepass")
        };
        // The blade entries, or their fragment-census twins under
        // WGR_GRASS_COUNT_FRAGMENTS. Chosen once so near and mid cannot disagree.
        // The alpha-cutout card path (fs_grass_cards) and the photo tufts are not
        // counted; both are off by default and neither is what the census is for.
        let (fs_color_near, fs_color_mid, fs_prepass_near, fs_prepass_mid) = if count_frags {
            (
                "fs_grass_count_near",
                "fs_grass_count_mid",
                "fs_grass_prepass_count_near",
                "fs_grass_prepass_count_mid",
            )
        } else {
            ("fs_grass", "fs_grass", "fs_grass_prepass", "fs_grass_prepass")
        };
        let color_pipeline = make_pipeline(
            "wgr_grass_color",
            "vs_grass",
            fs_color_near,
            surface_format,
            true,
        );
        let color_no_write_pipeline = make_pipeline(
            "wgr_grass_color_no_write",
            "vs_grass",
            fs_color_near,
            surface_format,
            false,
        );
        let cards_color_pipeline = make_pipeline(
            "wgr_grass_color_cards",
            "vs_grass",
            "fs_grass_cards",
            surface_format,
            true,
        );
        let cards_color_no_write_pipeline = make_pipeline(
            "wgr_grass_color_cards_no_write",
            "vs_grass",
            "fs_grass_cards",
            surface_format,
            false,
        );
        let prepass_pipeline = make_pipeline(
            "wgr_grass_prepass",
            "vs_grass",
            fs_prepass_near,
            NORMAL_FORMAT,
            true,
        );
        let mid_prepass_pipeline = make_pipeline(
            "wgr_grass_mid_prepass",
            "vs_grass_mid",
            fs_prepass_mid,
            NORMAL_FORMAT,
            true,
        );
        let mid_color_pipeline = make_pipeline(
            "wgr_grass_mid_color",
            "vs_grass_mid",
            fs_color_mid,
            surface_format,
            true,
        );
        let mid_color_no_write_pipeline = make_pipeline(
            "wgr_grass_mid_color_no_write",
            "vs_grass_mid",
            fs_color_mid,
            surface_format,
            false,
        );
        let mid_tuft_prepass_pipeline = make_pipeline_a2c(
            "wgr_grass_mid_tuft_prepass",
            "vs_grass_mid_tuft",
            tuft_prepass_fs,
            NORMAL_FORMAT,
            true,
            card_a2c,
        );
        let mid_tuft_color_pipeline = make_pipeline_a2c(
            "wgr_grass_mid_tuft_color",
            "vs_grass_mid_tuft",
            tuft_fs,
            surface_format,
            true,
            card_a2c,
        );
        let mid_tuft_color_no_write_pipeline = make_pipeline_a2c(
            "wgr_grass_mid_tuft_color_no_write",
            "vs_grass_mid_tuft",
            tuft_fs,
            surface_format,
            false,
            card_a2c,
        );
        let near_tuft_prepass_pipeline = make_pipeline_a2c(
            "wgr_grass_near_tuft_prepass",
            "vs_grass_mid_tuft",
            tuft_prepass_fs,
            NORMAL_FORMAT,
            true,
            card_a2c,
        );
        let near_tuft_color_pipeline = make_pipeline_a2c(
            "wgr_grass_near_tuft_color",
            "vs_grass_mid_tuft",
            tuft_fs,
            surface_format,
            true,
            card_a2c,
        );
        let near_tuft_color_no_write_pipeline = make_pipeline_a2c(
            "wgr_grass_near_tuft_color_no_write",
            "vs_grass_mid_tuft",
            tuft_fs,
            surface_format,
            false,
            card_a2c,
        );
        let far_color_pipeline = make_pipeline(
            "wgr_grass_far_color",
            "vs_grass_far",
            "fs_grass_far",
            surface_format,
            false,
        );
        let far_color_no_write_pipeline = make_pipeline(
            "wgr_grass_far_color_no_write",
            "vs_grass_far",
            "fs_grass_far",
            surface_format,
            false,
        );
        let shadow_shader = crate::shaders::make_module(
            device,
            composer,
            "wgr_grass_shadow_shader",
            include_str!("grass_shadow.wgsl"),
            "grass/grass_shadow.wgsl",
        );
        let shadow_layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("wgr_grass_shadow_pipeline_layout"),
            bind_group_layouts: &[
                Some(shadow_pass_layout),
                Some(&terrain_layout),
                Some(&data_layout),
            ],
            immediate_size: 0,
        });
        let shadow_pipeline = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
            label: Some("wgr_grass_shadow"),
            layout: Some(&shadow_layout),
            vertex: wgpu::VertexState {
                module: &shadow_shader,
                entry_point: Some("vs_grass_shadow"),
                compilation_options: Default::default(),
                buffers: &[],
            },
            primitive: wgpu::PrimitiveState {
                topology: wgpu::PrimitiveTopology::TriangleList,
                cull_mode: None,
                ..Default::default()
            },
            depth_stencil: Some(wgpu::DepthStencilState {
                format: wgpu::TextureFormat::Depth32Float,
                depth_write_enabled: Some(true),
                depth_compare: Some(wgpu::CompareFunction::LessEqual),
                stencil: Default::default(),
                bias: wgpu::DepthBiasState {
                    constant: 4,
                    slope_scale: 2.5,
                    clamp: 0.0,
                },
            }),
            multisample: Default::default(),
            // Depth-only in production. The census build attaches a fragment stage
            // with no colour targets whose only work is the counter atomic.
            fragment: if count_frags {
                Some(wgpu::FragmentState {
                    module: &shadow_shader,
                    entry_point: Some("fs_grass_shadow_count"),
                    compilation_options: Default::default(),
                    targets: &[],
                })
            } else {
                None
            },
            multiview_mask: None,
            cache: None,
        });
        let tuft_shadow_pipeline = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
            label: Some("wgr_grass_tuft_shadow"),
            layout: Some(&shadow_layout),
            vertex: wgpu::VertexState {
                module: &shadow_shader,
                entry_point: Some("vs_grass_tuft_shadow"),
                compilation_options: Default::default(),
                buffers: &[],
            },
            primitive: wgpu::PrimitiveState {
                topology: wgpu::PrimitiveTopology::TriangleList,
                cull_mode: None,
                ..Default::default()
            },
            depth_stencil: Some(wgpu::DepthStencilState {
                format: wgpu::TextureFormat::Depth32Float,
                depth_write_enabled: Some(true),
                depth_compare: Some(wgpu::CompareFunction::LessEqual),
                stencil: Default::default(),
                bias: wgpu::DepthBiasState {
                    constant: 4,
                    slope_scale: 2.5,
                    clamp: 0.0,
                },
            }),
            multisample: Default::default(),
            fragment: Some(wgpu::FragmentState {
                module: &shadow_shader,
                entry_point: Some("fs_grass_tuft_shadow"),
                compilation_options: Default::default(),
                targets: &[],
            }),
            multiview_mask: None,
            cache: None,
        });
        Self {
            enabled,
            force_off: !enabled,
            casts_shadows: true,
            // SetGrassSettings publishes GrassSettings' 800 m far ring on the
            // first frame; start disabled only until that initial upload.
            far_enabled: false,
            terrain_params,
            grass_params,
            placement_count,
            indirect,
            near_index,
            mid_index,
            near_index_mode: std::cell::Cell::new(u8::MAX),
            mid_index_mode: std::cell::Cell::new(u8::MAX),
            mid_placement_count,
            mid_indirect,
            far_placement_count,
            far_indirect,
            terrain_layout,
            terrain_bind,
            data_bind,
            mid_data_bind,
            far_data_bind,
            heightmap_view,
            geography,
            geography_view,
            terrain_generation: u64::MAX,
            holes,
            holes_gen: 0,
            have_heightmap: false,
            place_pipeline,
            mid_place_pipeline,
            far_place_pipeline,
            color_pipeline,
            color_no_write_pipeline,
            cards_color_pipeline,
            cards_color_no_write_pipeline,
            prepass_pipeline,
            mid_prepass_pipeline,
            mid_color_pipeline,
            mid_color_no_write_pipeline,
            mid_tuft_prepass_pipeline,
            mid_tuft_color_pipeline,
            mid_tuft_color_no_write_pipeline,
            near_tuft_prepass_pipeline,
            near_tuft_color_pipeline,
            near_tuft_color_no_write_pipeline,
            far_color_pipeline,
            far_color_no_write_pipeline,
            shadow_pipeline,
            tuft_shadow_pipeline,
            shadow_index,
            shadow_indirect,
            shadow_index_mode: std::cell::Cell::new(u8::MAX),
            shadow_stride,
            frag_counts,
            count_frags,
            frag_stats: FragStats::default(),
            shadow_draws: std::cell::Cell::new(0),
            shadow_draws_latest: 0,
            census_ticks: 0,
            stats_buffers: (0..STATS_SLOTS)
                .map(|i| {
                    (
                        device.create_buffer(&wgpu::BufferDescriptor {
                            label: Some(&format!("wgr_grass_stats_readback_{i}")),
                            size: STATS_BYTES,
                            usage: wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::MAP_READ,
                            mapped_at_creation: false,
                        }),
                        StatsSlot::Idle,
                    )
                })
                .collect(),
            stats: GrassStats {
                near_candidates: 256 * 256,
                mid_candidates: 512 * 512,
                far_candidates: MAX_FAR_INSTANCES,
                ..Default::default()
            },
            data_layout,
            instances,
            mid_instances,
            far_instances,
            blade_view,
            blade_sampler,
            have_blade_atlas: false,
            tuft_view,
            px_per_radian: 0.0,
            last_raw_params: bytemuck::Zeroable::zeroed(),
            have_tuft: false,
            tuft_enabled: false,
            near_grid_dim: 256,
            mid_grid_dim: 512,
            tuft_bounds: default_layer_bounds(),
            tuft_means: default_layer_means(),
            last_params: params,
            wind_phase: wind::WindPhase::default(),
            card_spacing,
            card_a2c,
            card_tone,
            card_tone_off: false,
            pending_log: vec![cards_line, shadow_line],
            announced_cards: None,
        }
    }

    /// Whether the card pipelines run alpha-to-coverage (MSAA scene, not opted out).
    /// Not yet surfaced through the C ABI; the tests and the Grass tab are the readers.
    #[allow(dead_code)]
    pub fn card_a2c(&self) -> bool {
        self.card_a2c
    }

    /// Lines queued for the game log since the last drain. lib.rs calls this after
    /// set_params / set_tufts and forwards each through the LogSink.
    pub fn drain_log(&mut self) -> Vec<String> {
        std::mem::take(&mut self.pending_log)
    }

    /// The card state as the shader will see it this frame; re-announced only on
    /// change. `drawn` is what the draw() pipeline choice keys on, `near_grid` /
    /// `mid_grid` are the dispatch sizes -- on the 1.11 m card grid a 41 m near
    /// ring is an 80-cell grid, on the 0.16 m blade grid it is 520, so the line
    /// says on its own whether the card spacing engaged.
    fn announce_cards(&mut self, key: CardStateKey) {
        if self.announced_cards == Some(key) {
            return;
        }
        self.announced_cards = Some(key);
        let near_spacing = if key.cards_near {
            self.card_spacing
        } else {
            near_placement_spacing(
                self.last_params.spacing,
                self.last_params.clump_renderer != 0.0,
                false,
                self.card_spacing,
            )
        } / coverage_grid_scale(self.last_params.density, 1.0);
        let mid_spacing = bounded_mid_spacing(mid_placement_spacing(key.cards_mid, self.card_spacing),
            self.last_params.density, self.last_params.mid_density, self.last_params.mid_radius);
        // How far the mid grid's square actually reaches. On the CARD grid this is
        // the ceiling the requested mid radius is clamped to (mid_ring_end in
        // grass.wgsl): past ~45x card density it is SHORTER than the radius the
        // Grass tab asks for, which is the number that explains "denser made the
        // field smaller". Same one-cell jitter margin as the shader.
        let mid_reach = (key.mid_grid_dim as f32 * 0.5 - 1.0).max(1.0) * mid_spacing;
        self.pending_log.push(format!(
            "Wgpu grass cards: drawn={} (atlas resident={}, opt-in={}), near ring {} at {:.2} m \
             ({}x{} grid), mid ring {} at {:.2} m ({}x{} grid, reach {:.0} m), a2c={}, \
             card coverage {:.2} auto={}",
            key.cards_mid as u8,
            key.have_tuft as u8,
            key.opt_in as u8,
            if key.cards_near { "cards" } else { "blades" },
            near_spacing,
            key.near_grid_dim,
            key.near_grid_dim,
            if key.cards_mid { "cards" } else { "ribbons" },
            mid_spacing,
            key.mid_grid_dim,
            key.mid_grid_dim,
            mid_reach,
            self.card_a2c as u8,
            key.coverage_centi as f32 / 100.0,
            key.coverage_auto as u8,
        ));
    }

    /// Photo-card placement spacing in metres (CARD_SPACING_DEFAULT or the env override).
    #[allow(dead_code)]
    pub fn card_spacing(&self) -> f32 {
        self.card_spacing
    }

    /// GRS-E — receive the game's decoded grass-tuft PAA. Rebuilds the three data
    /// bind groups, since a bind group captures the texture view it was made with.
    pub fn set_tuft(
        &mut self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        width: u32,
        height: u32,
        rgba: &[u8],
    ) {
        self.set_tufts(device, queue, width, height, 1, rgba);
    }

    pub fn set_tufts(
        &mut self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        width: u32,
        height: u32,
        layers: u32,
        rgba: &[u8],
    ) {
        let Some(view) = blade_atlas::create_tufts(device, queue, width, height, layers, rgba)
        else {
            return;
        };
        self.tuft_view = view;
        self.have_tuft = true;
        // Trim bounds are a property of the atlas, so they are computed here and
        // republished with the params below -- never taken from the caller.
        let bounds = blade_atlas::tuft_bounds(width, height, layers, rgba);
        for (layer, b) in bounds.iter().enumerate() {
            self.tuft_bounds[layer * 4..layer * 4 + 4].copy_from_slice(b);
        }
        let means = blade_atlas::tuft_layer_means(width, height, layers, rgba);
        for (layer, m) in means.iter().enumerate() {
            self.tuft_means[layer * 4..layer * 4 + 4].copy_from_slice(m);
        }
        self.last_params.photo_layer_bounds = self.tuft_bounds;
        self.last_params.photo_layer_means = self.tuft_means;
        // Which slices can flicker under the alpha test, answered at upload so
        // nobody has to step through "Force one family" to find out by eye. A
        // plate with a high partial-alpha fraction (JPEG opacity map) or any
        // speckle fraction (one-texel stem fragments) is a flicker source; a
        // hard-authored cutout reads ~0 on both.
        let report = blade_atlas::tuft_alpha_edge_report(width, height, layers, rgba);
        let mut worst: Vec<(usize, &blade_atlas::TuftAlphaEdgeReport)> = report
            .iter()
            .enumerate()
            .filter(|(_, r)| r.covered > 0)
            .collect();
        worst.sort_by(|a, b| {
            (b.1.speckle_fraction + b.1.partial_fraction * 0.25)
                .partial_cmp(&(a.1.speckle_fraction + a.1.partial_fraction * 0.25))
                .unwrap_or(std::cmp::Ordering::Equal)
        });
        let listed: Vec<String> = worst
            .iter()
            .map(|(layer, r)| {
                format!(
                    "L{layer} partial {:.2} speckle {:.3} edge {:.2}",
                    r.partial_fraction, r.speckle_fraction, r.edge_fraction
                )
            })
            .collect();
        self.pending_log.push(format!(
            "Wgpu grass atlas alpha edges ({layers} layers {width}x{height}, worst first; partial = \
             fraction of covered texels with intermediate alpha, speckle = isolated texels above \
             the cutoff, edge = texels on the cutoff boundary): [{}]",
            listed.join(", ")
        ));
        // Uploads normally arrive after the game has pushed GrassSettings. Keep
        // that opt-in live immediately instead of requiring a UI slider nudge.
        //
        // Re-derive from the raw params rather than patching the flag: the
        // uniform's use_photo_tuft is gated on have_tuft (so the shader's card
        // spacing only engages once plates exist), and the candidate grids are
        // sized from that spacing -- both come out of set_params. Before the game
        // has pushed anything there is nothing to derive from; publish the
        // bounds/means and let the first push do the rest.
        if self.last_raw_params.near_radius > 0.0 {
            let raw = self.last_raw_params;
            self.set_params(queue, raw);
        } else {
            queue.write_buffer(&self.grass_params, 0, bytemuck::bytes_of(&self.last_params));
            self.tuft_enabled = self.last_params.use_photo_tuft != 0.0;
        }
        let rebuild = |instances: &wgpu::Buffer, count: &wgpu::Buffer| {
            make_data_bind(
                device,
                &self.data_layout,
                &self.grass_params,
                instances,
                count,
                &self.blade_view,
                &self.blade_sampler,
                &self.tuft_view,
                &self.frag_counts,
            )
        };
        self.data_bind = rebuild(&self.instances, &self.placement_count);
        self.mid_data_bind = rebuild(&self.mid_instances, &self.mid_placement_count);
        self.far_data_bind = rebuild(&self.far_instances, &self.far_placement_count);
    }

    /// Replace the procedural near-blade surface atlas with supplied opaque
    /// photo layers. A bind group captures its texture view, so all LOD binds
    /// must be rebuilt even though only the near shader samples this texture.
    pub fn set_blade_atlas(
        &mut self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        width: u32,
        height: u32,
        layers: u32,
        rgba: &[u8],
    ) {
        let Some(view) =
            blade_atlas::create_from_images(device, queue, width, height, layers, rgba)
        else {
            return;
        };
        self.blade_view = view;
        self.have_blade_atlas = true;
        let means = blade_atlas::blade_layer_means(width, height, layers, rgba);
        for (layer, mean) in means.iter().enumerate() {
            self.last_params.blade_layer_means[layer * 4..layer * 4 + 4].copy_from_slice(mean);
        }
        queue.write_buffer(&self.grass_params, 0, bytemuck::bytes_of(&self.last_params));
        let rebuild = |instances: &wgpu::Buffer, count: &wgpu::Buffer| {
            make_data_bind(
                device,
                &self.data_layout,
                &self.grass_params,
                instances,
                count,
                &self.blade_view,
                &self.blade_sampler,
                &self.tuft_view,
                &self.frag_counts,
            )
        };
        self.data_bind = rebuild(&self.instances, &self.placement_count);
        self.mid_data_bind = rebuild(&self.mid_instances, &self.mid_placement_count);
        self.far_data_bind = rebuild(&self.far_instances, &self.far_placement_count);
    }

    pub fn set_geography(
        &mut self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        width: u32,
        height: u32,
        values: &[u32],
    ) {
        if width == 0 || height == 0 || values.len() < width as usize * height as usize {
            return;
        }
        let geography = make_geography_texture(device, width, height);
        queue.write_texture(
            wgpu::TexelCopyTextureInfo {
                texture: &geography,
                mip_level: 0,
                origin: wgpu::Origin3d::ZERO,
                aspect: wgpu::TextureAspect::All,
            },
            bytemuck::cast_slice(&values[..width as usize * height as usize]),
            wgpu::TexelCopyBufferLayout {
                offset: 0,
                bytes_per_row: Some(width * 4),
                rows_per_image: Some(height),
            },
            wgpu::Extent3d {
                width,
                height,
                depth_or_array_layers: 1,
            },
        );
        self.geography = geography;
        self.geography_view = self
            .geography
            .create_view(&wgpu::TextureViewDescriptor::default());
        self.terrain_bind = make_terrain_bind(
            device,
            &self.terrain_layout,
            &self.terrain_params,
            &self.heightmap_view,
            &self.geography_view,
            &self.holes,
        );
    }

    pub fn set_params(&mut self, queue: &wgpu::Queue, params: WgrGrassParams) {
        // Spacing and radius are now independent. The lower spacing bound is the
        // real density ceiling ("boost the coverage far more"): 0.10 m limited a
        // clump grid to 0.32 m centres no matter what the Grass tab asked for.
        self.last_raw_params = params;
        let spacing = params.spacing.clamp(0.02, 0.75);
        let near_radius = params.near_radius.clamp(8.0, 200.0);
        // Stop the near ring where its blades stop being resolvable.
        //
        // The near clump is 12 five-segment ribbons (360 vertices) against the mid
        // clump's 6 three-segment ones (108), and that detail is only worth paying
        // for while a blade is at least a pixel wide. A blade is 32-86 mm across
        // (`mix(0.016, 0.043)` half-width in grass.wgsl), so it subtends one pixel
        // at `width * px_per_radian` metres: about 31 m at 800x600, 55 m at 1080p
        // and 74 m at 1440p, against a slider that goes to 200.
        //
        // A FIXED metric radius is therefore wrong at every resolution but one,
        // which is the actual defect here -- not the vertex count. Deriving it
        // makes the crossover invisible by construction: nothing that could be
        // resolved is ever removed.
        //
        // Applied BEFORE the grid derivation below on purpose. That is where the
        // saving is: a smaller radius means a smaller candidate grid, so the
        // placement dispatch shrinks too rather than merely rejecting more
        // candidates. It is also what makes the clamp visible without new
        // plumbing -- `near_candidates` in GrassStats is near_grid_dim squared.
        let near_radius = {
            // Mean authored half-width, scaled by the same slider the shader uses.
            let half_width = 0.0295 * params.blade_width_scale.clamp(0.25, 6.0);
            let resolvable = half_width * 2.0 * self.px_per_radian;
            // A zero or non-finite projection, and a zero viewport height during a
            // resize, must fall through to the slider rather than collapse the
            // ring. The 8 m floor is the slider's own lower bound.
            if self.px_per_radian.is_finite() && self.px_per_radian > 0.0 && resolvable.is_finite()
            {
                near_radius.min(resolvable.max(8.0))
            } else {
                near_radius
            }
        };
        // Photo cards need the texture AND the opt-in. Resolved here, once, and
        // written into the uniform as `use_photo_tuft`, so the shader's card
        // spacing (card_placement_spacing) can only engage when cards will
        // actually be drawn -- otherwise a world with no plates would place its
        // procedural ribbons on the sparse card grid.
        let clump_renderer = params.clump_renderer != 0.0;
        let cards_mid = self.have_tuft && params.use_photo_tuft != 0.0;
        // RFG-090: a native world keeps blades on the near ring (cards move to the mid
        // ring, which then starts at the camera -- see cs_place_mid).
        let cards_near = cards_mid && clump_renderer && params.near_blades_only == 0.0;
        // Must mirror cs_place's `near_spacing` (near_placement_spacing in grass.wgsl).
        let near_spacing =
            near_placement_spacing(spacing, clump_renderer, cards_near, self.card_spacing)
                / coverage_grid_scale(params.density, 1.0);
        // The grid has to span the whole disc, so its edge is the diameter. Round
        // up to the workgroup size and cap.
        let want = (2.0 * near_radius / near_spacing).ceil().max(0.0) as u32;
        let near_grid_dim = want
            .div_ceil(8)
            .saturating_mul(8)
            .clamp(NEAR_GRID_MIN, NEAR_GRID_MAX);
        // At the cap the grid cannot span the requested disc, and the field was
        // then bounded by the grid's SQUARE -- a straight edge across the world,
        // camera-locked, with no dissolve. It bites at exactly the settings the
        // dense presets use: the reach is 768 * spacing * 3.20, so 0.02 m spacing
        // asks for 200 m and gets a 49 m square.
        //
        // Cap the radius at what the grid actually covers instead. The near ring
        // then ends in its own circular dissolve, strictly inside the square, and
        // the field does not get shorter -- the mid ring already reaches 1000 m
        // and simply takes over earlier. Costs nothing: the grid, the candidate
        // count and the dispatch are all unchanged.
        //
        // One cell of margin because near candidates jitter by up to +-0.925 of a
        // cell out of their own square.
        let near_reach = (near_grid_dim as f32 * 0.5 - 1.0).max(1.0) * near_spacing;
        let near_radius = near_radius.min(near_reach);
        // Mirrors mid_ring_end() in grass.wgsl. Both must agree: the shader
        // decides which candidates to accept, this decides how many exist -- and
        // both now read the same capped `near_radius`, since the shader takes it
        // from these params rather than recomputing it.
        //
        // Keep the complete requested mid disc inside the fixed grid budget.
        // Extra coverage beyond it broadens the rendered clumps, avoiding both
        // a smaller grass field and an unbounded candidate/instance allocation.
        let mid_radius = params.mid_radius.clamp(8.0, MID_RING_MAX);
        let mid_natural = (near_radius + 10.0).max((near_radius * 2.5).min(MID_RING_MAX));
        let mid_end = if mid_radius > near_radius {
            mid_radius
        } else {
            mid_natural
        }
        .clamp(near_radius + 10.0, MID_RING_MAX);
        // mid_placement_spacing in grass.wgsl: the fixed ribbon spacing, or the
        // card grid when the mid ring draws cards.
        let mid_spacing = bounded_mid_spacing(mid_placement_spacing(cards_mid, self.card_spacing),
            params.density, params.mid_density, mid_end);
        let want_mid = (2.0 * (mid_end / mid_spacing + 2.0)).ceil().max(0.0) as u32;
        let mid_grid_dim = want_mid
            .div_ceil(8)
            .saturating_mul(8)
            .clamp(MID_GRID_MIN, MID_GRID_MAX);
        let params = GrassParams {
            density: params.density.clamp(0.0, 2.0),
            spacing,
            near_radius,
            enabled: if params.enabled != 0.0 && !self.force_off {
                1.0
            } else {
                0.0
            },
            blade_height: params.blade_height.clamp(0.10, 3.0),
            wind_strength: params.wind_strength.clamp(0.0, 3.0),
            wind_direction: params.wind_direction,
            far_radius: params.far_radius.clamp(8.0, 5000.0),
            interactor_x: params.interactor_x,
            interactor_z: params.interactor_z,
            interactor_radius: params.interactor_radius.clamp(0.0, 32.0),
            // 1.5 is a renderer-side marker for the controlled helicopter's
            // rotor wash; grass.wgsl clamps its physical crush to one.
            interactor_strength: params.interactor_strength.clamp(0.0, 1.5),
            tracks: params.tracks,
            downwash: params.downwash,
            debug_ignore_geography_exclusions: params.debug_ignore_geography_exclusions,
            clumping: params.clumping.clamp(0.0, 1.0),
            color_variation: params.color_variation.clamp(0.0, 1.0),
            transmission: params.transmission.clamp(0.0, 1.0),
            cast_shadows: if params.cast_shadows != 0.0 { 1.0 } else { 0.0 },
            apply_fog: if params.apply_fog != 0.0 { 1.0 } else { 0.0 },
            density_noise_scale: params.density_noise_scale.clamp(0.002, 0.5),
            density_noise_strength: params.density_noise_strength.clamp(0.0, 1.0),
            // Clamp the pair so grass never goes negative if both are pushed up.
            weed_percent: params.weed_percent.clamp(0.0, 1.0),
            flower_percent: params
                .flower_percent
                .clamp(0.0, 1.0 - params.weed_percent.clamp(0.0, 1.0)),
            blade_width_scale: params.blade_width_scale.clamp(0.25, 6.0),
            use_photo_tuft: if cards_mid { 1.0 } else { 0.0 },
            saturation: params.saturation.clamp(0.0, 2.0),
            dry_patches: params.dry_patches.clamp(0.0, 1.0),
            dry_patch_scale: params.dry_patch_scale.clamp(0.002, 0.3),
            mid_radius,
            shape_variety: params.shape_variety.clamp(0.0, 1.0),
            taper_jitter: params.taper_jitter.clamp(0.0, 1.0),
            bend_jitter: params.bend_jitter.clamp(0.0, 1.0),
            blade_texture_strength: params.blade_texture_strength.clamp(0.0, 1.0),
            alpha_cards: if params.alpha_cards != 0.0 { 1.0 } else { 0.0 },
            // A cutoff of 0 with cards on would discard nothing and merely pay the
            // early-Z cost, and 1 would discard everything; keep it inside both.
            alpha_cutoff: params.alpha_cutoff.clamp(0.05, 0.95),
            card_widen: params.card_widen.clamp(1.0, 4.0),
            blade_arch: params.blade_arch.clamp(0.0, 3.0),
            clump_renderer: if params.clump_renderer != 0.0 {
                1.0
            } else {
                0.0
            },
            photo_tuft_brightness: params.photo_tuft_brightness.clamp(0.5, 2.5),
            photo_tuft_mix: params.photo_tuft_mix.clamp(0.0, 1.0),
            photo_tuft_patch_size: params.photo_tuft_patch_size.clamp(4.0, 80.0),
            photo_contrast: params.photo_contrast.clamp(0.5, 2.5),
            photo_contour: params.photo_contour.clamp(0.0, 2.0),
            photo_self_shadow: params.photo_self_shadow.clamp(0.0, 1.0),
            photo_root_ao: params.photo_root_ao.clamp(0.0, 1.0),
            near_grid_dim: near_grid_dim as f32,
            mid_grid_dim: mid_grid_dim as f32,
            lod_blend: params.lod_blend.clamp(0.0, 40.0),
            photo_alpha_cutoff: params.photo_alpha_cutoff.clamp(0.05, 0.95),
            // Anything outside 0..31 means "no override"; the shader tests < -0.5.
            // 31, not 8: the map clutter atlas has 32 layers (TUFT_LAYERS), and
            // finding the one plate that flickers means being able to force any of
            // them, not just the nine loose-card slots.
            photo_force_layer: if (0.0..=31.0).contains(&params.photo_force_layer) {
                params.photo_force_layer.round()
            } else {
                -1.0
            },
            photo_saturation: params.photo_saturation.clamp(0.0, 2.0),
            track_lifetime: params.track_lifetime.clamp(5.0, 3600.0),
            imprint_depth: params.imprint_depth.clamp(0.0, 0.95),
            photo_layer_weights: params.photo_layer_weights.map(|w| w.clamp(0.0, 1.0)),
            near_density: params.near_density.clamp(0.0, 1.0),
            mid_density: params.mid_density.clamp(0.0, 50.0),
            far_density: params.far_density.clamp(0.0, 1.0),
            card_spacing: self.card_spacing,
            tint_procedural: clamp_tint(params.tint_procedural),
            tint_photo: clamp_tint(params.tint_photo),
            // A zeroed struct (a caller that predates the field) must not delete
            // the cards: non-finite or non-positive falls back to the default.
            card_coverage: if params.card_coverage.is_finite() && params.card_coverage > 0.0 {
                params.card_coverage.clamp(0.01, 1.0)
            } else {
                CARD_COVERAGE_DEFAULT
            },
            card_coverage_auto: if params.card_coverage_auto != 0.0 {
                1.0
            } else {
                0.0
            },
            // 0 (a zeroed / older caller) reads as the stock size, not as no card.
            card_scale: if params.card_scale > 0.0 {
                params.card_scale.clamp(0.25, 2.0)
            } else {
                1.0
            },
            // Renderer-owned, like card_spacing: the caller has no say.
            shadow_blade_stride: self.shadow_stride as f32,
            blade_self_shadow: params.blade_self_shadow.clamp(0.0, 1.0),
            // 0 contrast would collapse the palette to its pivot; a zeroed struct
            // (old caller) reads as "untouched" instead.
            blade_contrast: if params.blade_contrast > 0.0 {
                params.blade_contrast.clamp(0.25, 3.0)
            } else {
                1.0
            },
            blade_hue_variation: params.blade_hue_variation.clamp(0.0, 1.0),
            blade_root_shade: params.blade_root_shade.clamp(0.0, 0.95),
            // Renderer-owned, like card_spacing: the caller has no say.
            card_tone_target: if self.card_tone_off { 0.0 } else { self.card_tone.target },
            card_tone_gain_max: self.card_tone.gain_max,
            card_flutter: self.card_tone.flutter,
            // Passed through from the caller, clamped rather than trusted: 0 would
            // freeze the field into a static crumple and a large value aliases the
            // noise into strobing. 0 (an untouched struct) reads as the shipped 1.0.
            wind_scroll: if params.wind_scroll > 0.0 {
                params.wind_scroll.clamp(0.02, 4.0)
            } else {
                1.0
            },
            near_blades_only: params.near_blades_only.max(0.0),
            native_tint: params.native_tint,
            native_tint_lerp: params.native_tint_lerp.clamp(0.0, 1.0),
            _native2_pad: [
                if params.tint_procedural[3].is_finite() && params.tint_procedural[3] < 0.0 { -1.0 }
                else if params.tint_procedural[3].is_finite() && params.tint_procedural[3] >= 1.0 {
                    (params.tint_procedural[3] - 1.0).clamp(0.0, 1.0)
                } else { 1.0 },
                if params.tint_photo[3].is_finite() && params.tint_photo[3] >= 10.0 {
                    params.tint_photo[3].clamp(10.0, 200.0)
                } else { 45.0 },
                0.0,
            ],
            // Always the renderer's own measurement of the loaded atlas.
            photo_layer_bounds: self.tuft_bounds,
            photo_layer_means: self.tuft_means,
            wind_advection: self.last_params.wind_advection,
            wind_flutter: self.last_params.wind_flutter,
            blade_layer_means: self.last_params.blade_layer_means,
        };
        self.near_grid_dim = near_grid_dim;
        self.mid_grid_dim = mid_grid_dim;
        self.last_params = params;
        self.enabled = params.enabled != 0.0;
        self.casts_shadows = params.cast_shadows != 0.0;
        // A zero far radius means the outer ring is off: skip its whole dispatch
        // rather than running 147k candidate threads that all reject.
        self.far_enabled = params.far_radius > 1.0;
        // The photo cards need BOTH the texture and the dev-tool opt-in; the
        // uniform's use_photo_tuft was derived from the same pair above.
        self.tuft_enabled = cards_mid;
        queue.write_buffer(&self.grass_params, 0, bytemuck::bytes_of(&params));
        self.announce_cards(CardStateKey {
            have_tuft: self.have_tuft,
            opt_in: self.last_raw_params.use_photo_tuft != 0.0,
            cards_mid,
            cards_near,
            near_grid_dim,
            mid_grid_dim,
            coverage_centi: (params.card_coverage * 100.0).round() as u32,
            coverage_auto: params.card_coverage_auto != 0.0,
        });
    }

    /// Pixels per radian of the current view: `(viewport_height / 2) * proj[1][1]`.
    ///
    /// One number rather than a matrix and a viewport, because that is all the
    /// near-ring clamp needs and it keeps the projection convention on the side
    /// that owns it. Safe to call every frame: it only re-derives the params when
    /// the value actually moves, so a steady resolution costs one comparison.
    ///
    /// Passing 0, a negative, or a non-finite value disables the clamp and leaves
    /// the slider radius alone -- which is what a zero viewport height mid-resize,
    /// or an orthographic/degenerate projection, must do.
    pub fn set_screen_metrics(&mut self, queue: &wgpu::Queue, px_per_radian: f32) {
        let clean = if px_per_radian.is_finite() && px_per_radian > 0.0 {
            px_per_radian
        } else {
            0.0
        };
        // Only on a material change: 1% of the metric is far under a pixel of
        // crossover movement, and re-deriving every frame would be pointless work.
        if (clean - self.px_per_radian).abs() <= self.px_per_radian.max(1.0) * 0.01 {
            return;
        }
        self.px_per_radian = clean;
        let raw = self.last_raw_params;
        // Zeroed until the game pushes params once; deriving from that would size
        // the candidate grid off a zero radius.
        if raw.near_radius > 0.0 {
            self.set_params(queue, raw);
        }
    }

    pub fn prepare_terrain(
        &mut self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        terrain: &Terrain,
    ) {
        let params = terrain.params();
        let (advection, flutter) = self.wind_phase.advance(params.time,
            self.last_params.wind_strength, self.last_params.wind_direction, self.last_params.wind_scroll);
        self.last_params.wind_advection = advection;
        self.last_params.wind_flutter = flutter;
        queue.write_buffer(&self.grass_params, 0, bytemuck::bytes_of(&self.last_params));
        queue.write_buffer(&self.terrain_params, 0, bytemuck::bytes_of(&params));
        // Sinkhole W1: the placement's copy of the terrain holes follows the terrain's.
        if self.holes_gen != terrain.holes_gen() {
            self.holes_gen = terrain.holes_gen();
            queue.write_buffer(&self.holes, 0, bytemuck::cast_slice(terrain.holes_data()));
        }
        self.have_heightmap = terrain.has_heightmap();
        if self.terrain_generation != terrain.heightmap_gen() {
            self.terrain_generation = terrain.heightmap_gen();
            self.heightmap_view = terrain.heightmap_view();
            self.terrain_bind = make_terrain_bind(
                device,
                &self.terrain_layout,
                &self.terrain_params,
                &self.heightmap_view,
                &self.geography_view,
                &self.holes,
            );
        }
    }

    pub fn dispatch(
        &mut self,
        encoder: &mut wgpu::CommandEncoder,
        camera_bind: &wgpu::BindGroup,
        camera_offset: u32,
        timers: &crate::gpu_timers::GpuTimers,
    ) {
        if !self.enabled || !self.have_heightmap {
            return;
        }
        use crate::gpu_timers::Region;
        timers.begin(encoder, Region::GrassPlaceNear);
        let mut pass = encoder.begin_compute_pass(&wgpu::ComputePassDescriptor {
            label: Some("wgr_grass_place"),
            timestamp_writes: None,
        });
        pass.set_pipeline(&self.place_pipeline);
        pass.set_bind_group(0, camera_bind, &[camera_offset]);
        pass.set_bind_group(1, &self.terrain_bind, &[]);
        pass.set_bind_group(2, &self.data_bind, &[]);
        pass.dispatch_workgroups(self.near_grid_dim / 8, self.near_grid_dim / 8, 1);
        drop(pass);
        timers.end(encoder, Region::GrassPlaceNear);
        timers.begin(encoder, Region::GrassPlaceMid);
        let mut mid_pass = encoder.begin_compute_pass(&wgpu::ComputePassDescriptor {
            label: Some("wgr_grass_place_mid"),
            timestamp_writes: None,
        });
        mid_pass.set_pipeline(&self.mid_place_pipeline);
        mid_pass.set_bind_group(0, camera_bind, &[camera_offset]);
        mid_pass.set_bind_group(1, &self.terrain_bind, &[]);
        mid_pass.set_bind_group(2, &self.mid_data_bind, &[]);
        mid_pass.dispatch_workgroups(self.mid_grid_dim / 8, self.mid_grid_dim / 8, 1);
        drop(mid_pass);
        timers.end(encoder, Region::GrassPlaceMid);
        if self.far_enabled {
            timers.begin(encoder, Region::GrassPlaceFar);
            let mut far_pass = encoder.begin_compute_pass(&wgpu::ComputePassDescriptor {
                label: Some("wgr_grass_place_far"),
                timestamp_writes: None,
            });
            far_pass.set_pipeline(&self.far_place_pipeline);
            far_pass.set_bind_group(0, camera_bind, &[camera_offset]);
            far_pass.set_bind_group(1, &self.terrain_bind, &[]);
            far_pass.set_bind_group(2, &self.far_data_bind, &[]);
            far_pass.dispatch_workgroups(FAR_GRID_DIM / 8, FAR_GRID_DIM / 8, 1);
            drop(far_pass);
            timers.end(encoder, Region::GrassPlaceFar);
        }
        // The render pass consumes a dedicated indirect-argument buffer. Keeping the
        // atomic placement counter separate avoids a STORAGE_READ_WRITE/INDIRECT
        // conflict in wgpu's command-encoder validation.
        encoder.copy_buffer_to_buffer(&self.placement_count, 0, &self.indirect, 4, 4);
        // The shadow draw is the same near instances with the stride's index table.
        encoder.copy_buffer_to_buffer(&self.placement_count, 0, &self.shadow_indirect, 4, 4);
        encoder.copy_buffer_to_buffer(&self.mid_placement_count, 0, &self.mid_indirect, 4, 4);
        encoder.copy_buffer_to_buffer(&self.far_placement_count, 0, &self.far_indirect, 4, 4);
        // Same three counters into a mappable slot for the Grass tab's counts. The
        // fragment census rides along: this runs before the frame's render passes,
        // so the counters hold the PREVIOUS frame's fragments -- copied out here and
        // then zeroed for the passes about to be recorded. Zeroed every frame, slot
        // or no slot, or a skipped sample would carry two frames into the next one.
        if let Some((buf, state)) = self
            .stats_buffers
            .iter_mut()
            .find(|(_, s)| matches!(s, StatsSlot::Idle))
        {
            encoder.copy_buffer_to_buffer(&self.placement_count, 0, buf, 0, 4);
            encoder.copy_buffer_to_buffer(&self.mid_placement_count, 0, buf, 4, 4);
            encoder.copy_buffer_to_buffer(&self.far_placement_count, 0, buf, 8, 4);
            if self.count_frags {
                encoder.copy_buffer_to_buffer(&self.frag_counts, 0, buf, 16, FRAG_WORDS as u64 * 4);
            }
            *state = StatsSlot::Pending;
        }
        if self.count_frags {
            encoder.clear_buffer(&self.frag_counts, 0, None);
        }
    }

    /// Kick map_async on freshly copied slots and drain completed ones. Call once
    /// per frame after queue.submit, alongside GpuTimers::harvest.
    pub fn harvest_stats(&mut self, device: &wgpu::Device) {
        for (buf, state) in &mut self.stats_buffers {
            if matches!(state, StatsSlot::Pending) {
                let (tx, rx) = std::sync::mpsc::channel();
                buf.slice(..).map_async(wgpu::MapMode::Read, move |r| {
                    let _ = tx.send(r);
                });
                *state = StatsSlot::InFlight(rx);
            }
        }
        let _ = device.poll(wgpu::PollType::Poll);
        for (buf, state) in &mut self.stats_buffers {
            let StatsSlot::InFlight(rx) = state else {
                continue;
            };
            match rx.try_recv() {
                Ok(Ok(())) => {
                    {
                        let data = buf.slice(..).get_mapped_range();
                        let word = |i: usize| {
                            u32::from_le_bytes(data[i * 4..i * 4 + 4].try_into().unwrap())
                        };
                        // The compute pass keeps counting past capacity, so clamp
                        // to the buffer size the draw is actually limited to.
                        self.stats.near_instances = word(0).min(MAX_INSTANCES);
                        self.stats.mid_instances = word(1).min(MAX_MID_INSTANCES);
                        self.stats.far_instances = word(2).min(MAX_FAR_INSTANCES);
                        if self.count_frags {
                            for lane in 0..FRAG_LANES {
                                self.frag_stats.lanes[lane] = (0..FRAG_STRIPES)
                                    .map(|s| u64::from(word(4 + lane * FRAG_STRIPES + s)))
                                    .sum();
                            }
                        }
                    }
                    buf.unmap();
                    *state = StatsSlot::Idle;
                }
                Ok(Err(_)) | Err(std::sync::mpsc::TryRecvError::Disconnected) => {
                    *state = StatsSlot::Idle;
                }
                Err(std::sync::mpsc::TryRecvError::Empty) => {}
            }
        }
        // Vertex counts mirror reset_indirect's per-LOD vertex-per-instance values.
        let photo_near = self.near_draws_cards();
        self.stats.near_vertices = self.stats.near_instances
            * if photo_near {
                12
            } else if self.last_params.clump_renderer != 0.0 {
                360
            } else {
                180
            };
        self.stats.mid_vertices =
            self.stats.mid_instances * if self.tuft_enabled { 12 } else { 108 };
        self.stats.far_vertices = self.stats.far_instances * 6;
        // Cascades that drew grass this frame (reset_indirect zeroes the counter at
        // the next frame's start, so at harvest it is this frame's total).
        self.shadow_draws_latest = self.shadow_draws.get();
        self.census_ticks += 1;
        if self.census_ticks >= CENSUS_PERIOD
            && (self.stats.near_instances > 0 || self.stats.mid_instances > 0)
        {
            self.census_ticks = 0;
            self.pending_log.push(self.census_line());
        }
    }

    /// The denominators under the three grass timer rows, for the game log.
    ///
    /// The rows are milliseconds; without these a reader cannot tell a per-blade
    /// cost from a per-pixel one, and the "Grass shadow" row reports ONE cascade's
    /// draw (gpu_timers.rs: a region opened twice in a frame reports the last
    /// bracket), so its frame cost is that row times `cascades`. Triangles are
    /// what the passes were measured to be bound by; vertex invocations are the
    /// indexed count (the post-transform cache collapses the rest); fragments are
    /// the previous frame's census when WGR_GRASS_COUNT_FRAGMENTS is on.
    fn census_line(&self) -> String {
        let s = self.stats;
        let clump = self.last_params.clump_renderer != 0.0;
        let near_cards = self.near_draws_cards();
        let near_blades: u32 = if clump { 12 } else { 6 };
        // Triangles and distinct vertices per instance, by representation.
        let (near_tris, near_verts) = if near_cards { (4u32, 12u32) } else { (near_blades * 10, near_blades * 12) };
        let (mid_tris, mid_verts) = if self.tuft_enabled { (4u32, 12u32) } else { (36u32, 48u32) };
        let shadow_blades = (near_blades / self.shadow_stride.max(1)).max(1);
        let shadow_tris = if near_cards { 4 } else { shadow_blades * 10 };
        let cascades = u64::from(self.shadow_draws_latest);
        let near = u64::from(s.near_instances);
        let mid = u64::from(s.mid_instances);
        let far = u64::from(s.far_instances);
        let pass_tris = near * u64::from(near_tris) + mid * u64::from(mid_tris) + far * 2;
        let shadow_total = cascades * near * u64::from(shadow_tris);
        // Prepass + colour draw every ring; the cascades draw the near ring only
        // (mid cards too, but only on the photo-tuft path, which is counted above
        // as near-sized for want of a separate counter).
        let frame_tris = pass_tris * 2 + shadow_total;
        let frags = if self.count_frags {
            let l = &self.frag_stats.lanes;
            format!(
                "fragments (previous frame): prepass near {} mid {}, colour near {} mid {}, \
                 shadow {} over {} cascade(s)",
                l[FRAG_LANE_NEAR_PREPASS],
                l[FRAG_LANE_MID_PREPASS],
                l[FRAG_LANE_NEAR_COLOR],
                l[FRAG_LANE_MID_COLOR],
                l[FRAG_LANE_NEAR_SHADOW],
                cascades
            )
        } else {
            "fragments: not counted (WGR_GRASS_COUNT_FRAGMENTS=1)".to_string()
        };
        format!(
            "Wgpu grass census: near {near} clumps x {near_tris} tris ({near_verts} verts) = {} tris/pass, \
             mid {mid} x {mid_tris} ({mid_verts}) = {} tris/pass, far {far} x 2; \
             prepass+colour {} tris; shadow {cascades} cascade(s) x {near} x {shadow_tris} tris \
             (stride {}) = {shadow_total}; frame {frame_tris} grass tris; {frags}",
            near * u64::from(near_tris),
            mid * u64::from(mid_tris),
            pass_tris * 2,
            self.shadow_stride,
        )
    }

    pub fn stats(&self) -> GrassStats {
        // Candidate totals are the live dispatch sizes, not constants: both the
        // near and mid grids are sized from their radius so density no longer
        // varies with it.
        let near_candidates = self.near_grid_dim * self.near_grid_dim;
        let mid_candidates = self.mid_grid_dim * self.mid_grid_dim;
        if !self.enabled || !self.have_heightmap {
            return GrassStats {
                near_candidates,
                mid_candidates,
                far_candidates: MAX_FAR_INSTANCES,
                ..Default::default()
            };
        }
        GrassStats {
            near_candidates,
            mid_candidates,
            ..self.stats
        }
    }

    pub fn reset_indirect(&self, queue: &wgpu::Queue) {
        self.shadow_draws.set(0);
        queue.write_buffer(&self.placement_count, 0, bytemuck::bytes_of(&0u32));
        queue.write_buffer(&self.mid_placement_count, 0, bytemuck::bytes_of(&0u32));
        queue.write_buffer(&self.far_placement_count, 0, bytemuck::bytes_of(&0u32));
        // GRS-030 doubles blades per clump and spaces centres farther apart.
        let near_verts: u32 = if self.near_draws_cards() {
            12
        } else if self.last_params.clump_renderer != 0.0 {
            360
        } else {
            180
        };
        // Indexed args: index_count first, then instance_count (which the placement copy
        // overwrites), then first_index / base_vertex / first_instance.
        queue.write_buffer(
            &self.indirect,
            0,
            bytemuck::cast_slice(&[near_verts, 0u32, 0u32, 0u32, 0u32]),
        );
        // The blade table collapses 30 draw slots onto 12 distinct vertices; the card
        // path keeps identity indices because its vertex shader decodes vertex_index
        // itself. Mode 0 = blades, 1 = cards.
        let near_mode: u8 = u8::from(self.near_draws_cards());
        if self.near_index_mode.get() != near_mode {
            self.near_index_mode.set(near_mode);
            let table = if near_mode == 1 {
                identity_indices(near_verts)
            } else {
                // 12 blades when the clump renderer is on, 6 when it is not; the shader
                // reads the same blade count from grass.renderer.x.
                let blades = if self.last_params.clump_renderer != 0.0 { 12 } else { 6 };
                build_blade_indices(blades, 5)
            };
            queue.write_buffer(&self.near_index, 0, bytemuck::cast_slice(&table));
        }
        // Blade shadow draw: every stride-th blade of the near clump, same instance
        // count (copied in dispatch), fewer indices per instance. Cards keep the
        // identity table: vs_grass_tuft_shadow decodes vertex_index itself.
        let near_blades: u32 = if self.last_params.clump_renderer != 0.0 { 12 } else { 6 };
        let shadow_blades = (near_blades / self.shadow_stride.max(1)).max(1);
        let shadow_index_count: u32 = if near_mode == 1 { near_verts } else { shadow_blades * 30 };
        queue.write_buffer(
            &self.shadow_indirect,
            0,
            bytemuck::cast_slice(&[shadow_index_count, 0u32, 0u32, 0u32, 0u32]),
        );
        // Keyed on cards-vs-blades AND the blade count: clump_renderer can flip live.
        let shadow_mode: u8 = if near_mode == 1 { 1 } else { 2 + shadow_blades as u8 };
        if self.shadow_index_mode.get() != shadow_mode {
            self.shadow_index_mode.set(shadow_mode);
            let table = if near_mode == 1 {
                identity_indices(near_verts)
            } else {
                build_blade_indices(shadow_blades, 5)
            };
            queue.write_buffer(&self.shadow_index, 0, bytemuck::cast_slice(&table));
        }
        // Mid LOD is either six opaque three-segment ribbons per clump, or two
        // crossed, alpha-tested photo cards (2 * 6 vertices) when a compatible
        // tuft has been explicitly enabled.
        let mid_verts: u32 = if self.tuft_enabled { 12 } else { 108 };
        queue.write_buffer(
            &self.mid_indirect,
            0,
            bytemuck::cast_slice(&[mid_verts, 0u32, 0u32, 0u32, 0u32]),
        );
        let mid_mode: u8 = u8::from(self.tuft_enabled);
        if self.mid_index_mode.get() != mid_mode {
            self.mid_index_mode.set(mid_mode);
            let table = if mid_mode == 1 {
                identity_indices(mid_verts)
            } else {
                build_blade_indices(6, 3)
            };
            queue.write_buffer(&self.mid_index, 0, bytemuck::cast_slice(&table));
        }
        // Far LOD is one terrain-conforming coverage quad per compacted cell.
        queue.write_buffer(
            &self.far_indirect,
            0,
            bytemuck::cast_slice(&[6u32, 0, 0, 0]),
        );
    }

    pub fn draw(
        &self,
        pass: &mut wgpu::RenderPass<'_>,
        camera_bind: &wgpu::BindGroup,
        camera_offset: u32,
        kind: GrassPass,
        timers: &crate::gpu_timers::GpuTimers,
    ) {
        if !self.enabled || !self.have_heightmap {
            return;
        }
        use crate::gpu_timers::Region;
        // Grass draws are ops inside the shared 3D pass, so these brackets need
        // TIMESTAMP_QUERY_INSIDE_PASSES; they no-op (row reads "n/a") without it.
        let region = match kind {
            GrassPass::Prepass => Region::GrassPrepass,
            _ => Region::GrassColor,
        };
        timers.begin_pass(pass, region);
        // Cut-out cards swap only the COLOUR pipelines. The prepass keeps the
        // non-discarding shader: it writes depth and normals for a silhouette the
        // colour pass is about to carve holes in, but a prepass that discarded
        // would have to pay the same early-Z loss to remove texels the depth
        // buffer already handles conservatively.
        let cutout = self.last_params.alpha_cards > 0.5;
        let pipeline = match kind {
            GrassPass::Color if cutout => &self.cards_color_pipeline,
            GrassPass::ColorNoWrite if cutout => &self.cards_color_no_write_pipeline,
            GrassPass::Color => &self.color_pipeline,
            GrassPass::ColorNoWrite => &self.color_no_write_pipeline,
            GrassPass::Prepass => &self.prepass_pipeline,
        };
        // Far grass skips the normal/depth prepass: it is a sparse, one-triangle
        // visual fill. Draw it before the dense near cards so near blades retain
        // normal depth writing and naturally cover the transition.
        if self.far_enabled && !matches!(kind, GrassPass::Prepass) {
            let far_pipeline = match kind {
                GrassPass::Color => &self.far_color_pipeline,
                GrassPass::ColorNoWrite => &self.far_color_no_write_pipeline,
                GrassPass::Prepass => unreachable!(),
            };
            pass.set_pipeline(far_pipeline);
            pass.set_bind_group(0, camera_bind, &[camera_offset]);
            pass.set_bind_group(1, &self.terrain_bind, &[]);
            pass.set_bind_group(2, &self.far_data_bind, &[]);
            pass.draw_indirect(&self.far_indirect, 0);
        }
        let mid_pipeline = match (kind, self.tuft_enabled) {
            (GrassPass::Color, true) => &self.mid_tuft_color_pipeline,
            (GrassPass::ColorNoWrite, true) => &self.mid_tuft_color_no_write_pipeline,
            (GrassPass::Prepass, true) => &self.mid_tuft_prepass_pipeline,
            (GrassPass::Color, false) => &self.mid_color_pipeline,
            (GrassPass::ColorNoWrite, false) => &self.mid_color_no_write_pipeline,
            (GrassPass::Prepass, false) => &self.mid_prepass_pipeline,
        };
        pass.set_pipeline(mid_pipeline);
        pass.set_bind_group(0, camera_bind, &[camera_offset]);
        pass.set_bind_group(1, &self.terrain_bind, &[]);
        pass.set_bind_group(2, &self.mid_data_bind, &[]);
        pass.set_index_buffer(self.mid_index.slice(..), wgpu::IndexFormat::Uint32);
        pass.draw_indexed_indirect(&self.mid_indirect, 0);
        let near_pipeline = match (
            kind,
            self.near_draws_cards(),
        ) {
            (GrassPass::Color, true) => &self.near_tuft_color_pipeline,
            (GrassPass::ColorNoWrite, true) => &self.near_tuft_color_no_write_pipeline,
            (GrassPass::Prepass, true) => &self.near_tuft_prepass_pipeline,
            _ => pipeline,
        };
        pass.set_pipeline(near_pipeline);
        pass.set_bind_group(0, camera_bind, &[camera_offset]);
        pass.set_bind_group(1, &self.terrain_bind, &[]);
        pass.set_bind_group(2, &self.data_bind, &[]);
        pass.set_index_buffer(self.near_index.slice(..), wgpu::IndexFormat::Uint32);
        pass.draw_indexed_indirect(&self.indirect, 0);
        timers.end_pass(pass, region);
    }

    /// Near grass always casts. When photographed clump cards are enabled, the
    /// mid cards also enter the cascade: otherwise they look ungrounded and
    /// cannot shade each other or the terrain at their visible range.
    pub fn draw_shadow(
        &self,
        pass: &mut wgpu::RenderPass<'_>,
        shadow_bind: &wgpu::BindGroup,
        shadow_offset: u32,
        timers: &crate::gpu_timers::GpuTimers,
    ) {
        if !self.casts_shadows || !self.enabled || !self.have_heightmap {
            return;
        }
        use crate::gpu_timers::Region;
        // One call per cascade the mask keeps; the census reports the count because
        // the timer row below only ever shows the last of them.
        self.shadow_draws.set(self.shadow_draws.get() + 1);
        timers.begin_pass(pass, Region::GrassShadow);
        let near_shadow_pipeline = if self.near_draws_cards() {
            &self.tuft_shadow_pipeline
        } else {
            &self.shadow_pipeline
        };
        pass.set_pipeline(near_shadow_pipeline);
        pass.set_bind_group(0, shadow_bind, &[shadow_offset]);
        pass.set_bind_group(1, &self.terrain_bind, &[]);
        pass.set_bind_group(2, &self.data_bind, &[]);
        // The stride's own table and args (reset_indirect); identical to the colour
        // draw's at stride 1 and on the card path.
        pass.set_index_buffer(self.shadow_index.slice(..), wgpu::IndexFormat::Uint32);
        pass.draw_indexed_indirect(&self.shadow_indirect, 0);
        if self.tuft_enabled {
            pass.set_pipeline(&self.tuft_shadow_pipeline);
            pass.set_bind_group(2, &self.mid_data_bind, &[]);
            pass.set_index_buffer(self.mid_index.slice(..), wgpu::IndexFormat::Uint32);
            pass.draw_indexed_indirect(&self.mid_indirect, 0);
        }
        timers.end_pass(pass, Region::GrassShadow);
    }

    /// Whether a photographed clump atlas was actually uploaded. The Grass tab
    /// uses this to say so plainly instead of leaving the user with a ticked box
    /// and procedural grass; the renderer falls back either way.
    pub fn have_photo_clumps(&self) -> bool {
        self.have_tuft
    }

    pub fn casts_shadows(&self) -> bool {
        self.casts_shadows && self.enabled && self.have_heightmap
    }
}

/// Tints multiply albedo, so a negative would flip a channel and a large value
/// would blow the field out past anything the sun does. The alpha slot is unused
/// and pinned to 1 so the vec4 stays predictable in the shader.
fn clamp_tint(tint: [f32; 4]) -> [f32; 4] {
    [
        tint[0].clamp(0.0, 4.0),
        tint[1].clamp(0.0, 4.0),
        tint[2].clamp(0.0, 4.0),
        1.0,
    ]
}

/// Neutral contrast pivots until a real atlas arrives. 0.34 is the constant the
/// pivot used to be hardcoded to, so an un-measured layer behaves as before.
fn default_layer_means() -> [f32; 128] {
    let mut out = [0.0f32; 128];
    for layer in 0..blade_atlas::TUFT_LAYERS {
        out[layer * 4] = 0.34;
        out[layer * 4 + 1] = 0.34;
        out[layer * 4 + 2] = 0.34;
        out[layer * 4 + 3] = 1.0;
    }
    out
}

/// Untrimmed card bounds: (u0, u1, v0, v1) = the full quad for every layer. What
/// the shader sees until a real atlas arrives, and what it falls back to for any
/// layer whose plate has no transparent margin.
fn default_layer_bounds() -> [f32; 128] {
    let mut out = [0.0f32; 128];
    for layer in 0..blade_atlas::TUFT_LAYERS {
        out[layer * 4] = 0.0;
        out[layer * 4 + 1] = 1.0;
        out[layer * 4 + 2] = 0.0;
        out[layer * 4 + 3] = 1.0;
    }
    out
}

fn make_geography_texture(device: &wgpu::Device, width: u32, height: u32) -> wgpu::Texture {
    device.create_texture(&wgpu::TextureDescriptor {
        label: Some("wgr_grass_geography"),
        size: wgpu::Extent3d {
            width,
            height,
            depth_or_array_layers: 1,
        },
        mip_level_count: 1,
        sample_count: 1,
        dimension: wgpu::TextureDimension::D2,
        format: wgpu::TextureFormat::R32Uint,
        usage: wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::COPY_DST,
        view_formats: &[],
    })
}

fn make_terrain_bind(
    device: &wgpu::Device,
    layout: &wgpu::BindGroupLayout,
    params: &wgpu::Buffer,
    heightmap: &wgpu::TextureView,
    geography: &wgpu::TextureView,
    holes: &wgpu::Buffer,
) -> wgpu::BindGroup {
    device.create_bind_group(&wgpu::BindGroupDescriptor {
        label: Some("wgr_grass_terrain_bind"),
        layout,
        entries: &[
            wgpu::BindGroupEntry {
                binding: 0,
                resource: params.as_entire_binding(),
            },
            wgpu::BindGroupEntry {
                binding: 1,
                resource: wgpu::BindingResource::TextureView(heightmap),
            },
            wgpu::BindGroupEntry {
                binding: 2,
                resource: wgpu::BindingResource::TextureView(geography),
            },
            wgpu::BindGroupEntry {
                binding: 3,
                resource: holes.as_entire_binding(),
            },
        ],
    })
}

fn make_data_bind(
    device: &wgpu::Device,
    layout: &wgpu::BindGroupLayout,
    params: &wgpu::Buffer,
    instances: &wgpu::Buffer,
    placement_count: &wgpu::Buffer,
    blade_view: &wgpu::TextureView,
    blade_sampler: &wgpu::Sampler,
    tuft_view: &wgpu::TextureView,
    frag_counts: &wgpu::Buffer,
) -> wgpu::BindGroup {
    device.create_bind_group(&wgpu::BindGroupDescriptor {
        label: Some("wgr_grass_data_bind"),
        layout,
        entries: &[
            wgpu::BindGroupEntry {
                binding: 0,
                resource: params.as_entire_binding(),
            },
            wgpu::BindGroupEntry {
                binding: 1,
                resource: instances.as_entire_binding(),
            },
            wgpu::BindGroupEntry {
                binding: 2,
                resource: placement_count.as_entire_binding(),
            },
            wgpu::BindGroupEntry {
                binding: 3,
                resource: wgpu::BindingResource::TextureView(blade_view),
            },
            wgpu::BindGroupEntry {
                binding: 4,
                resource: wgpu::BindingResource::Sampler(blade_sampler),
            },
            wgpu::BindGroupEntry {
                binding: 5,
                resource: wgpu::BindingResource::TextureView(tuft_view),
            },
            wgpu::BindGroupEntry {
                binding: 6,
                resource: frag_counts.as_entire_binding(),
            },
        ],
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    const SHADER: &str = include_str!("grass.wgsl");
    const SHADOW: &str = include_str!("grass_shadow.wgsl");

    // Colour and shadow must consume the same integrated wind phase.
    #[test]
    fn both_grass_shaders_consume_integrated_wind_phases()
    {
        for (name, src) in [("grass.wgsl", SHADER), ("grass_shadow.wgsl", SHADOW)]
        {
            for (term, phase) in [("broad_scroll", "grass.wind_advection.xy"),
                                  ("gust_scroll", "grass.wind_advection.zw"),
                                  ("flutter_scroll", "grass.wind_flutter.xy")]
            {
                let head = format!("let {term} =");
                let start = src
                    .find(&head)
                    .unwrap_or_else(|| panic!("{name} has no `{head}`"));
                let end = start + src[start..].find(';').expect("unterminated statement");
                assert!(
                    src[start..end].contains(phase),
                    "{name}: `{term}` does not read the integrated phase"
                );
            }
        }
    }

    #[test]
    fn colour_and_shadow_uniform_offsets_match_the_uploaded_rust_struct() {
        for src in [SHADER, SHADOW] {
            let mut declarations = String::new();
            for name in ["GrassTrack", "GrassDownwash", "GrassParams"] {
                let start = src.find(&format!("struct {name} {{")).unwrap();
                let end = start + src[start..].find('}').unwrap() + 1;
                declarations.push_str(&src[start..end]);
                declarations.push(';');
            }
            let module = naga::front::wgsl::parse_str(&declarations).unwrap();
            let ty = module.types.iter().find(|(_, ty)| ty.name.as_deref() == Some("GrassParams")).unwrap().1;
            let naga::TypeInner::Struct { members, span } = &ty.inner else { panic!("not a struct"); };
            assert_eq!(*span as usize, std::mem::size_of::<GrassParams>());
            for (name, expected) in [
                ("layer_bounds", std::mem::offset_of!(GrassParams, photo_layer_bounds)),
                ("layer_means", std::mem::offset_of!(GrassParams, photo_layer_means)),
                ("wind_advection", std::mem::offset_of!(GrassParams, wind_advection)),
                ("blade_layer_means", std::mem::offset_of!(GrassParams, blade_layer_means)),
                ("wind_flutter", std::mem::offset_of!(GrassParams, wind_flutter)),
            ] {
                let field = members.iter().find(|m| m.name.as_deref() == Some(name)).unwrap();
                assert_eq!(field.offset as usize, expected, "{name}");
            }
        }
    }

    // The card flicker fix is alpha-to-coverage, and A2C is only correct if the
    // prepass and the colour pass emit the SAME coverage from the SAME sample:
    // otherwise the prepass writes depth to samples the colour pass never shades.
    #[test]
    fn card_a2c_entries_share_one_sample_and_one_coverage_function() {
        assert!(SHADER.contains("#import gbuffer::{oct_encode, a2c_coverage}"));
        assert!(SHADER.contains("fn tuft_coverage(tex_a: f32) -> f32 {"));
        assert!(SHADER.contains("return a2c_coverage(tex_a, tuft_alpha_cutoff());"));
        for entry in ["fs_grass_mid_tuft_a2c", "fs_grass_mid_tuft_prepass_a2c"] {
            let start = SHADER
                .find(&format!("fn {entry}("))
                .unwrap_or_else(|| panic!("{entry} missing"));
            let body = &SHADER[start..start + 400];
            assert!(
                body.contains("tuft_coverage("),
                "{entry} must use tuft_coverage"
            );
            assert!(
                body.contains("tuft_sample(in)"),
                "{entry} must sample via tuft_sample"
            );
            // Coverage before discard: fwidth needs uniform control flow.
            let cov = body.find("tuft_coverage(").unwrap();
            let disc = body.find("discard").unwrap();
            assert!(cov < disc, "{entry} computes coverage after its discard");
        }
        // The hard-cutout entries stay for 1x and the WGR_GRASS_CARD_A2C=0 A/B.
        assert!(SHADER.contains("fn fs_grass_mid_tuft(in: VsOut)"));
        assert!(SHADER.contains("fn fs_grass_mid_tuft_prepass(in: VsOut)"));
        // Every card entry samples through tuft_sample: exactly two raw fetches of
        // tuft_tex remain (tuft_sample itself and tuft_luma's contour taps).
        assert_eq!(SHADER.matches("textureSampleBias(tuft_tex").count(), 2);
        // The shadow twin keeps the same cutoff, so the A2C 50% edge and the
        // shadow silhouette sit on the same texel.
        assert!(SHADOW.contains("clamp(grass.place.w, 0.05, 0.95)"));
        assert!(SHADER.contains("return clamp(grass.place.w, 0.05, 0.95);"));
    }

    // Density: photo cards place on their own grid, in BOTH rings, and the
    // renderer's grid sizing mirrors the shader's spacing choice.
    #[test]
    fn card_placement_spacing_mirrors_the_shader() {
        assert!(SHADER.contains("const MID_PLACEMENT_SPACING: f32 = 0.82;"));
        assert_eq!(MID_PLACEMENT_SPACING, 0.82);
        assert!(SHADER.contains(&format!(
            "return clamp(grass.lod_density.w, {:.2}, {:.1});",
            CARD_SPACING_RANGE.0, CARD_SPACING_RANGE.1
        )));
        // The two compute entries take their spacing from the shared helpers,
        // and the blade formula appears exactly once (inside the helper).
        assert!(SHADER.contains("let near_spacing = near_placement_spacing();"));
        assert!(SHADER.contains("let mid_spacing = mid_placement_spacing();"));
        assert_eq!(
            SHADER.matches("grass.spacing * select(1.72, 3.20").count(),
            1
        );
        // Rust side.
        let blade = near_placement_spacing(0.05, true, false, CARD_SPACING_DEFAULT);
        assert!((blade - 0.16).abs() < 1e-6);
        assert_eq!(
            near_placement_spacing(0.05, false, false, 1.11),
            0.05 * 1.72
        );
        assert_eq!(near_placement_spacing(0.05, true, true, 1.11), 1.11);
        assert_eq!(mid_placement_spacing(false, 1.11), MID_PLACEMENT_SPACING);
        assert_eq!(mid_placement_spacing(true, 1.11), 1.11);
        // Card mode: near and mid are one field -- same spacing, no join.
        assert_eq!(
            near_placement_spacing(0.05, true, true, CARD_SPACING_DEFAULT),
            mid_placement_spacing(true, CARD_SPACING_DEFAULT)
        );
    }

    // The mid ring must never be bounded by its candidate SQUARE. Its grid is
    // capped at MID_GRID_MAX cells, and on the card grid that cap bites well
    // inside the radius the Grass tab asks for -- which made raising the density
    // SHORTEN the field, in a camera-locked square with no dissolve. The near
    // ring has always clamped its radius to near_reach; mid_ring_end() now does
    // the same, and this pins both the arithmetic and the shader clamp that
    // implements it.
    #[test]
    fn mid_ring_radius_is_clamped_to_the_capped_grid_reach() {
        // The shader clamps, using the grid dim the renderer wrote into place.y.
        assert!(SHADER.contains(
            "let reach = max(grass.place.y * 0.5 - 1.0, 1.0) * mid_placement_spacing();"
        ));
        assert!(
            SHADER.contains(
                "return clamp(min(want, reach), grass.near_radius + 10.0, MID_RING_MAX);"
            )
        );
        // ...and the announce line reports the reach, so a shortened field is
        // readable from the game log rather than only visible on screen.
        assert!(SHADER.contains("fn mid_ring_end() -> f32 {"));

        // The arithmetic the shader is mirroring, at GrassSettings::midRadius'
        // default of 350 m. The wider the ring, the earlier the cap bites: a
        // 350 m ring needs 0.29 m spacing or wider, which is ~15x density.
        let reach_at = |density: f32| {
            let spacing = (CARD_SPACING_DEFAULT / density.sqrt())
                .clamp(CARD_SPACING_RANGE.0, CARD_SPACING_RANGE.1);
            let dim = ((2.0 * 350.0 / spacing).ceil() as u32)
                .div_ceil(8)
                .saturating_mul(8)
                .clamp(MID_GRID_MIN, MID_GRID_MAX);
            (dim as f32 * 0.5 - 1.0).max(1.0) * spacing
        };
        // The ribbon grid always spans its disc, which is why this was missed.
        assert!(
            (MID_GRID_MAX as f32 * 0.5) * MID_PLACEMENT_SPACING > MID_RING_MAX,
            "the 0.82 m grid should still span the whole 1000 m ring"
        );
        // The card grid does not: at the panel's own maximum the ring is a third
        // of the radius the panel is showing.
        assert!(reach_at(1.0) >= 349.0, "{}", reach_at(1.0));
        assert!(reach_at(15.0) >= 349.0, "{}", reach_at(15.0));
        assert!(reach_at(25.0) < 300.0, "{}", reach_at(25.0));
        assert!(reach_at(300.0) < 90.0, "{}", reach_at(300.0));
    }

    // "Far too dense": the plates stacked over any one point of ground. A card
    // spans ~1.5 m; on the 0.16 m blade grid that is ~90 candidates under it,
    // on the clutter grid it is a handful. The default must be in the range the
    // map's own clutter uses, and the shrink must be an order of magnitude.
    #[test]
    fn card_grid_is_an_order_of_magnitude_sparser_than_the_blade_grid() {
        assert_eq!(CARD_SPACING_DEFAULT, 1.11);
        assert!(
            CARD_SPACING_DEFAULT >= CARD_SPACING_RANGE.0
                && CARD_SPACING_DEFAULT <= CARD_SPACING_RANGE.1
        );
        let blade = near_placement_spacing(0.05, true, false, CARD_SPACING_DEFAULT);
        let card = near_placement_spacing(0.05, true, true, CARD_SPACING_DEFAULT);
        let card_area = std::f32::consts::PI * 0.75f32 * 0.75;
        let plates_before = card_area / (blade * blade);
        let plates_after = card_area / (card * card);
        assert!(plates_before > 60.0, "{plates_before}");
        assert!(plates_after < 2.5, "{plates_after}");
        assert!(plates_before / plates_after > 40.0);
    }

    // The uniform's use_photo_tuft is what the shader's spacing keys on, so it
    // must never say "cards" without a texture to draw them with.
    #[test]
    fn card_spacing_only_engages_with_a_resident_atlas() {
        assert!(SHADER.contains("fn cards_on_mid() -> bool { return grass.species_mix.w > 0.5; }"));
        assert!(SHADER.contains(
            "fn cards_on_near() -> bool { return cards_on_mid() && grass.renderer.x > 0.5 && grass.native.x <= 0.0; }"
        ));
        let src = include_str!("mod.rs");
        assert!(src.contains("let cards_mid = self.have_tuft && params.use_photo_tuft != 0.0;"));
        assert!(src.contains("use_photo_tuft: if cards_mid { 1.0 } else { 0.0 },"));
    }

    // Dark cards: the A2/OA plates are authored dark for their rvmat gains, so
    // the card path lifts each layer to the target from the mean it measured,
    // never darkens, and applies the same lift to the sample, its contrast pivot
    // and the far proxy. Off is the raw look.
    #[test]
    fn card_tone_lift_is_per_layer_and_reaches_sample_pivot_and_far_proxy() {
        assert!(SHADER.contains("card_tone: vec4<f32>,"));
        assert!(SHADOW.contains("card_tone: vec4<f32>,"));
        assert!(SHADER.contains("fn card_tone_gain(layer: u32) -> f32 {"));
        assert!(SHADER.contains("if (tone_target <= 0.001) { return 1.0; }"));
        assert!(SHADER.contains(
            "return clamp(tone_target / max(plate_luma, 0.005), 1.0, max(grass.card_tone.y, 1.0));"
        ));
        // Applied to the sample AND its pivot in tuft_shade, and to layer 0 in
        // fs_grass_far. The correction vector is one constant everywhere.
        assert!(SHADER.contains("let tone_gain = card_tone_gain(u32(in.blade_uv.z));"));
        assert!(SHADER.contains("tex.rgb * PHOTO_TONE_CORRECTION * photo_brightness * tone_gain;"));
        assert_eq!(
            SHADER
                .matches("PHOTO_TONE_CORRECTION * photo_brightness * tone_gain;")
                .count(),
            2,
            "sample and pivot"
        );
        assert!(SHADER.contains("clamp(grass.renderer.y, 0.5, 2.5) * card_tone_gain(0u);"));
        assert_eq!(SHADER.matches("vec3<f32>(0.74, 1.22, 0.56)").count(), 1);
        // `target` is a WGSL reserved word; the shader must not have grown one.
        assert!(!SHADER.contains("let target ="));
        // Rust defaults and env parsing.
        let d = CardTone::from_env_values(None, None, None);
        assert_eq!(d.target, CARD_TONE_TARGET_DEFAULT);
        assert_eq!(d.gain_max, CARD_TONE_GAIN_MAX_DEFAULT);
        assert_eq!(d.flutter, CARD_FLUTTER_DEFAULT);
        assert_eq!(CARD_FLUTTER_DEFAULT, 0.0);
        let off = CardTone::from_env_values(Some("0"), Some("99"), Some("1"));
        assert_eq!(off.target, 0.0);
        assert_eq!(off.gain_max, 16.0);
        assert_eq!(off.flutter, 1.0);
        let junk = CardTone::from_env_values(Some("nan"), Some("x"), Some("-3"));
        assert_eq!(junk.target, CARD_TONE_TARGET_DEFAULT);
        assert_eq!(junk.gain_max, CARD_TONE_GAIN_MAX_DEFAULT);
        assert_eq!(junk.flutter, 0.0);
        // What the default does to the measured Takistan plates: the gain the
        // shader will compute for c_grassgreen_grouphard (TK_BrushHard, corrected
        // luma 0.054) hits the cap; c_grassdry (0.243) is lifted ~2.6x; the A3
        // stock plate (0.63) is untouched.
        let gain = |luma: f32| (d.target / luma.max(0.005)).clamp(1.0, d.gain_max);
        assert_eq!(gain(0.054), d.gain_max);
        assert!((gain(0.243) - 2.59).abs() < 0.02);
        assert_eq!(gain(0.63), 1.0);
        assert_eq!(gain(0.9), 1.0);
    }

    // Flicker: cards take the gust sway and only card_tone.z of the blade-tip
    // turbulence, in the colour vertex shader AND the shadow twin, from the same
    // uniform slot -- or the shadow would jitter under a still card.
    #[test]
    fn card_flutter_is_one_uniform_read_by_both_card_vertex_shaders() {
        for (name, src, entry, turb) in [
            (
                "grass.wgsl",
                SHADER,
                "fn vs_grass_mid_tuft(",
                "wind.turbulence * card_flutter",
            ),
            (
                "grass_shadow.wgsl",
                SHADOW,
                "fn vs_grass_tuft_shadow(",
                "wind.w * card_flutter",
            ),
        ] {
            let start = src.find(entry).unwrap_or_else(|| panic!("{name}: {entry}"));
            let body = &src[start..];
            let end = body.find("\n@").unwrap_or(body.len());
            let body = &body[..end];
            assert!(
                body.contains("let card_flutter = clamp(grass.card_tone.z, 0.0, 1.0);"),
                "{name}: card_flutter must come from card_tone.z"
            );
            assert!(
                body.contains(turb),
                "{name}: turbulence must be scaled by card_flutter"
            );
            // The gust term is untouched: 0.030 + 0.18 * gust, as before.
            assert!(body.contains("(0.030 + 0.18 * wind."), "{name}");
        }
        // The blade shaders keep their flutter: only the two card entries scale it.
        assert_eq!(SHADER.matches("* card_flutter").count(), 1);
        assert_eq!(SHADOW.matches("* card_flutter").count(), 1);
        assert!(SHADER.contains("(0.035 + 0.21 * wind.gust + wind.turbulence)"));
        assert!(SHADER.contains("(0.030 + 0.18 * wind.gust + wind.turbulence);"));
        // Contour tilt is bounded (per-pixel sun flip = sparkle under motion).
        assert!(
            SHADER.contains("let bounded = tilt * (min(tilt_len, 1.2) / max(tilt_len, 1e-5));")
        );
    }

    pub(super) fn headless() -> Option<(wgpu::Device, wgpu::Queue)> {
        let instance =
            wgpu::Instance::new(wgpu::InstanceDescriptor::new_without_display_handle_from_env());
        let adapter = pollster::block_on(instance.request_adapter(&wgpu::RequestAdapterOptions {
            power_preference: wgpu::PowerPreference::default(),
            compatible_surface: None,
            force_fallback_adapter: false,
        }))
        .ok()?;
        // The instance buffers are storage read_write in the VERTEX stage, as in production.
        if !adapter
            .features()
            .contains(wgpu::Features::VERTEX_WRITABLE_STORAGE)
        {
            return None;
        }
        pollster::block_on(adapter.request_device(&wgpu::DeviceDescriptor {
            required_features: wgpu::Features::VERTEX_WRITABLE_STORAGE,
            // Use the production sampled-texture limit: the shared camera group
            // includes fog/lighting resources, so a grass pipeline needs 19
            // sampled textures rather than the headless default of 16.
            required_limits: wgpu::Limits {
                max_sampled_textures_per_shader_stage: adapter
                    .limits()
                    .max_sampled_textures_per_shader_stage
                    .clamp(16, 64),
                ..Default::default()
            },
            ..Default::default()
        }))
        .ok()
    }

    fn shadow_pass_layout(device: &wgpu::Device) -> wgpu::BindGroupLayout {
        // Shape of gfx3d's shadow-pass DynUbo layout: one dynamic-offset uniform, vertex only.
        device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("wgr_grass_test_shadow_pass_layout"),
            entries: &[wgpu::BindGroupLayoutEntry {
                binding: 0,
                visibility: wgpu::ShaderStages::VERTEX,
                ty: wgpu::BindingType::Buffer {
                    ty: wgpu::BufferBindingType::Uniform,
                    has_dynamic_offset: true,
                    min_binding_size: None,
                },
                count: None,
            }],
        })
    }

    fn params(spacing: f32, near_radius: f32, use_photo_tuft: f32) -> WgrGrassParams {
        let mut p: WgrGrassParams = bytemuck::Zeroable::zeroed();
        p.density = 1.0;
        p.spacing = spacing;
        p.near_radius = near_radius;
        p.enabled = 1.0;
        p.blade_height = 1.4;
        p.mid_radius = 210.0;
        p.blade_width_scale = 0.35;
        p.clump_renderer = 1.0;
        p.use_photo_tuft = use_photo_tuft;
        p.near_density = 1.0;
        p.mid_density = 0.95;
        p.far_density = 1.0;
        p.lod_blend = 30.0;
        p.photo_alpha_cutoff = 0.5;
        p.photo_force_layer = -1.0;
        p
    }

    #[test]
    fn coverage_above_one_adds_candidates_and_respects_grid_caps() {
        let Some((device, queue)) = headless() else { return; };
        let camera_layout = crate::gfx3d::camera_layout_for_tests(&device);
        let shadow_layout = shadow_pass_layout(&device);
        let mut composer = crate::shaders::build_composer();
        let mut grass = Grass::new(&device, &queue, &camera_layout, &shadow_layout,
            crate::HDR_FORMAT, 1, &mut composer);
        let mut p = params(0.1, 41.0, 0.0);
        p.density = 0.5;
        p.mid_density = 1.0;
        grass.set_params(&queue, p);
        let mid_one = grass.mid_grid_dim;
        let near_one = grass.near_grid_dim;
        assert_eq!(grass.last_params.density, 0.5);
        p.mid_density = 2.0;
        grass.set_params(&queue, p);
        let mid_two = grass.mid_grid_dim;
        assert!(mid_two > mid_one);
        assert_eq!(grass.near_grid_dim, near_one);
        assert_eq!(grass.last_params.mid_density, 2.0);
        p.mid_density = 4.0;
        grass.set_params(&queue, p);
        assert!(grass.mid_grid_dim > mid_two);
        assert!((grass.mid_grid_dim as f32 / mid_one as f32 - 2.0).abs() < 0.03);
        let mid_four = grass.mid_grid_dim;
        p.mid_density = 50.0;
        grass.set_params(&queue, p);
        assert_eq!(grass.last_params.mid_density, 50.0);
        assert!(grass.mid_grid_dim > mid_four);
        p.density = 2.0;
        grass.set_params(&queue, p);
        assert!(grass.near_grid_dim > near_one);
        assert_eq!(grass.last_params.density, 2.0);
        p.near_radius = 200.0;
        p.mid_radius = 1000.0;
        p.spacing = 0.02;
        grass.set_params(&queue, p);
        assert!(grass.near_grid_dim <= NEAR_GRID_MAX);
        assert!(grass.mid_grid_dim <= MID_GRID_MAX);
        device.poll(wgpu::PollType::wait_indefinitely()).unwrap();
    }

    #[test]
    fn dense_mid_cover_preserves_reach_with_a_fixed_grid_and_instance_budget() {
        for radius in [100.0_f32, 500.0, 1000.0] {
            for base in [0.06_f32, MID_PLACEMENT_SPACING, CARD_SPACING_DEFAULT] {
                for ring in [4.0_f32, 10.0, 50.0] {
                    let spacing = bounded_mid_spacing(base, 0.5, ring, radius);
                    let dim = ((2.0 * (radius / spacing + 2.0)).ceil() as u32)
                        .div_ceil(8).saturating_mul(8).clamp(MID_GRID_MIN, MID_GRID_MAX);
                    let reach = (dim as f32 * 0.5 - 1.0) * spacing;
                    assert!(reach >= radius - 0.001, "{radius} m, {ring}x: {reach}");
                    assert!(dim <= MID_GRID_MAX);
                    assert!(spacing >= base / coverage_grid_scale(0.5, ring));
                }
            }
        }
        // At the default reach, 50 really differs from 4 without 50x buffers.
        let spacing = bounded_mid_spacing(MID_PLACEMENT_SPACING, 0.5, 50.0, 500.0);
        assert!(spacing / (MID_PLACEMENT_SPACING / 50.0_f32.sqrt()) > 3.0);
        assert_eq!(MAX_MID_INSTANCES, 1_048_576);
    }

    // Blade and photographed-tuft textures are independent resources. A later
    // tuft upload or parameter re-derivation must not erase valid blade means.
    #[test]
    fn blade_means_follow_upload_and_survive_tuft_and_parameter_updates() {
        let Some((device, queue)) = headless() else { return; };
        let camera_layout = crate::gfx3d::camera_layout_for_tests(&device);
        let shadow_layout = shadow_pass_layout(&device);
        let mut composer = crate::shaders::build_composer();
        let mut grass = Grass::new(&device, &queue, &camera_layout, &shadow_layout,
            crate::HDR_FORMAT, 1, &mut composer);
        assert_eq!(grass.last_params.blade_layer_means, [0.0; 32]);
        let mut rgba = Vec::new();
        for layer in 0..8u8 { rgba.extend_from_slice(&[32 + layer * 16, 128, 64, 128]); }
        let expected = blade_atlas::blade_layer_means(1, 1, 8, &rgba);
        grass.set_blade_atlas(&device, &queue, 1, 1, 8, &rgba);
        let want: Vec<f32> = expected.into_iter().flatten().collect();
        assert_eq!(grass.last_params.blade_layer_means.as_slice(), want.as_slice());
        grass.set_params(&queue, params(0.05, 41.0, 1.0));
        grass.set_tufts(&device, &queue, 1, 1, 1, &[255; 4]);
        assert_eq!(grass.last_params.blade_layer_means.as_slice(), want.as_slice());
        assert_ne!(&grass.last_params.photo_layer_means[..32], want.as_slice());
        // Failed atlas replacement retains the previous valid atlas and means.
        grass.set_blade_atlas(&device, &queue, 0, 1, 8, &[]);
        assert_eq!(grass.last_params.blade_layer_means.as_slice(), want.as_slice());
        device.poll(wgpu::PollType::wait_indefinitely()).unwrap();
    }

    // Builds every grass pipeline on a real device at 4x (alpha-to-coverage
    // card pipelines) and at 1x (hard cutout), which is where an A2C pipeline
    // over an Rg16Float prepass target or a vec4-vs-vec2 entry mismatch would
    // be rejected. Then drives the card spacing through its real gate: the
    // uniform says "cards" only once an atlas is resident, and the near grid
    // shrinks to the card spacing when it does.
    #[test]
    fn card_pipelines_build_at_4x_and_1x_and_card_spacing_waits_for_the_atlas() {
        let Some((device, queue)) = headless() else {
            return;
        };
        let camera_layout = crate::gfx3d::camera_layout_for_tests(&device);
        let shadow_layout = shadow_pass_layout(&device);
        for sample_count in [4u32, 1u32] {
            let mut composer = crate::shaders::build_composer();
            let mut grass = Grass::new(
                &device,
                &queue,
                &camera_layout,
                &shadow_layout,
                crate::HDR_FORMAT,
                sample_count,
                &mut composer,
            );
            assert_eq!(grass.card_a2c(), sample_count > 1);
            // Cards requested, no atlas yet: blade spacing, uniform says no cards.
            grass.set_params(&queue, params(0.05, 41.0, 1.0));
            assert!(!grass.tuft_enabled);
            assert_eq!(grass.last_params.use_photo_tuft, 0.0);
            let blade_grid = grass.near_grid_dim;
            // 2 * 41 / 0.16 = 512.5 -> 520
            assert_eq!(blade_grid, 520);
            // A tiny atlas arrives: cards engage and the grid re-derives itself.
            let rgba = vec![255u8; 4 * 4 * 4];
            grass.set_tufts(&device, &queue, 4, 4, 1, &rgba);
            assert!(grass.tuft_enabled);
            assert_eq!(grass.last_params.use_photo_tuft, 1.0);
            let card = grass.card_spacing();
            let want = ((2.0 * 41.0 / card).ceil() as u32).div_ceil(8) * 8;
            assert_eq!(grass.near_grid_dim, want.max(NEAR_GRID_MIN));
            assert!(grass.near_grid_dim < blade_grid / 4);
            // Mid grid follows the same spacing in card mode.
            let want_mid = ((2.0 * (210.0 / card + 2.0)).ceil() as u32).div_ceil(8) * 8;
            assert_eq!(grass.mid_grid_dim, want_mid.max(MID_GRID_MIN));
            // Turning cards off restores the blade grid exactly.
            grass.set_params(&queue, params(0.05, 41.0, 0.0));
            assert!(!grass.tuft_enabled);
            assert_eq!(grass.near_grid_dim, blade_grid);
        }
        device.poll(wgpu::PollType::wait_indefinitely()).unwrap();
    }

    // The announcement path: the card state reaches the GAME log (via lib.rs
    // draining pending_log), and only on change -- a per-frame set_params must
    // not write a line per frame.
    #[test]
    fn card_state_is_announced_through_the_pending_log_on_change_only() {
        let Some((device, queue)) = headless() else {
            return;
        };
        let camera_layout = crate::gfx3d::camera_layout_for_tests(&device);
        let shadow_layout = shadow_pass_layout(&device);
        let mut composer = crate::shaders::build_composer();
        let mut grass = Grass::new(
            &device,
            &queue,
            &camera_layout,
            &shadow_layout,
            crate::HDR_FORMAT,
            4,
            &mut composer,
        );
        // Construction queues the config lines (spacing / a2c / coverage default,
        // then the shadow stride and census state).
        let boot = grass.drain_log();
        assert_eq!(boot.len(), 2, "{boot:?}");
        assert!(boot[0].starts_with("Wgpu grass cards: spacing 1.11 m"));
        assert!(boot[0].contains("coverage default 0.24"));
        assert!(boot[1].starts_with("Wgpu grass shadow: blade stride"), "{}", boot[1]);
        // First push: cards requested, no atlas -> drawn=0, blade grid.
        grass.set_params(&queue, params(0.05, 41.0, 1.0));
        let first = grass.drain_log();
        assert_eq!(first.len(), 1, "{first:?}");
        assert!(
            first[0].contains("drawn=0 (atlas resident=0, opt-in=1)"),
            "{}",
            first[0]
        );
        assert!(
            first[0].contains("near ring blades at 0.16 m (520x520 grid)"),
            "{}",
            first[0]
        );
        assert!(
            first[0].contains("card coverage 0.24 auto=0"),
            "{}",
            first[0]
        );
        // Same params again: silence.
        grass.set_params(&queue, params(0.05, 41.0, 1.0));
        assert!(grass.drain_log().is_empty());
        // Atlas arrives: re-announced with cards drawn on the card grid, plus the
        // per-layer alpha-edge report.
        let rgba = vec![255u8; 4 * 4 * 4];
        grass.set_tufts(&device, &queue, 4, 4, 1, &rgba);
        let after = grass.drain_log();
        assert!(
            after
                .iter()
                .any(|l| l.contains("drawn=1 (atlas resident=1, opt-in=1)")
                    && l.contains("near ring cards at 1.11 m (80x80 grid)")
                    && l.contains("mid ring cards at 1.11 m")),
            "{after:?}"
        );
        assert!(
            after
                .iter()
                .any(|l| l.starts_with("Wgpu grass atlas alpha edges (1 layers 4x4")),
            "{after:?}"
        );
        device.poll(wgpu::PollType::wait_indefinitely()).unwrap();
    }

    // Owner: photo cards "far too dense"; default coverage 0.24 where the map's
    // density is unknown, the map's own where it is; the "photo grass mix"
    // slider must reach BOTH selection paths (it only reached the loose-card
    // one, which is why it "does not work anymore" on Takistan); and the
    // procedural look controls must be identity at their defaults.
    #[test]
    fn card_coverage_mix_and_blade_look_are_wired_in_both_shaders() {
        // Coverage: applied to cards in both placement passes, keyed on the
        // authored bit, with the manual value as the fallback.
        assert_eq!(SHADER.matches("card_thinned(cell_id, geo)").count(), 2);
        assert!(SHADER.contains("if (cards_on_near() && card_thinned(cell_id, geo)) { return; }"));
        assert!(SHADER.contains("if (cards_on_mid() && card_thinned(cell_id, geo)) { return; }"));
        assert!(
            SHADER.contains("return select(manual, 1.0, authored && grass.card_look.y > 0.5);")
        );
        assert_eq!(CARD_COVERAGE_DEFAULT, 0.24);
        // Mix: the per-surface run path consults it too, colour AND shadow.
        for (name, src) in [("grass.wgsl", SHADER), ("grass_shadow.wgsl", SHADOW)] {
            let start = src.find("fn pick_photo_layer_run(").unwrap();
            let end = src[start..].find("fn pick_photo_layer(").unwrap() + start;
            let body = &src[start..end];
            assert!(
                body.contains("return min(run.x, 31u);"),
                "{name}: single-variety branch missing"
            );
            assert!(
                body.contains("hash11(patch_cell + vec2<f32>(19.0, 71.0))"),
                "{name}"
            );
        }
        // Force layer reaches every atlas layer, not just the nine loose slots.
        assert!(
            include_str!("mod.rs").contains("(0.0..=31.0).contains(&params.photo_force_layer)")
        );
        // Blade look: defaults are the pre-existing shading.
        assert!(
            SHADER.contains(
                "let root = mix(1.0 - blade_root_shade(), 1.0, in.height_t * in.height_t);"
            )
        );
        assert!(SHADER.contains("fn blade_self_shadow_terms("));
        let mut p: WgrGrassParams = bytemuck::Zeroable::zeroed();
        p.blade_root_shade = 0.70;
        // A zeroed caller (or a saved struct that predates the fields) must read
        // as untouched, not as "contrast 0 / coverage 0".
        assert_eq!(p.card_coverage, 0.0);
        assert_eq!(p.blade_contrast, 0.0);
        let Some((device, queue)) = headless() else {
            return;
        };
        let camera_layout = crate::gfx3d::camera_layout_for_tests(&device);
        let shadow_layout = shadow_pass_layout(&device);
        let mut composer = crate::shaders::build_composer();
        let mut grass = Grass::new(
            &device,
            &queue,
            &camera_layout,
            &shadow_layout,
            crate::HDR_FORMAT,
            1,
            &mut composer,
        );
        let mut raw = params(0.05, 41.0, 0.0);
        raw.blade_root_shade = 0.70;
        grass.set_params(&queue, raw);
        assert_eq!(grass.last_params.card_coverage, CARD_COVERAGE_DEFAULT);
        assert_eq!(grass.last_params.blade_contrast, 1.0);
        assert_eq!(grass.last_params.blade_self_shadow, 0.0);
        assert_eq!(grass.last_params.blade_hue_variation, 0.0);
        assert_eq!(grass.last_params.blade_root_shade, 0.70);
        // Explicit values pass through, clamped.
        raw.card_coverage = 0.5;
        raw.card_coverage_auto = 1.0;
        raw.blade_contrast = 9.0;
        raw.blade_self_shadow = 0.7;
        grass.set_params(&queue, raw);
        assert_eq!(grass.last_params.card_coverage, 0.5);
        assert_eq!(grass.last_params.card_coverage_auto, 1.0);
        assert_eq!(grass.last_params.blade_contrast, 3.0);
        assert_eq!(grass.last_params.blade_self_shadow, 0.7);
        device.poll(wgpu::PollType::wait_indefinitely()).unwrap();
    }
}

#[cfg(test)]
mod blade_index_tests
{
    use super::*;

    // The old, non-indexed decode, kept verbatim as the oracle: this is exactly what
    // grass.wgsl computed from a draw-order slot before the index buffer existed.
    fn legacy_row_and_side(segments: u32, slot: u32) -> (u32, bool) {
        let verts_per_blade = segments * 6;
        let packed = slot % verts_per_blade;
        let segment = packed / 6;
        let corner = packed % 6;
        let upper = corner == 2 || corner == 4 || corner == 5;
        let left = corner == 0 || corner == 3 || corner == 5;
        (segment + u32::from(upper), left)
    }

    // THE claim the whole change rests on: every draw slot still resolves to the same
    // (row, side) it did before, so the geometry is identical rather than similar.
    // Asserted for both ribbon shapes the renderer draws.
    #[test]
    fn indices_resolve_to_the_same_row_and_side_as_the_non_indexed_decode() {
        for (blades, segments) in [(12u32, 5u32), (6, 5), (6, 3)] {
            let indices = build_blade_indices(blades, segments);
            assert_eq!(indices.len() as u32, blades * segments * 6);
            let unique_per_blade = (segments + 1) * 2;
            for (slot, &index) in indices.iter().enumerate() {
                let slot = slot as u32;
                // What the shader now derives from the index value.
                let blade = index / unique_per_blade;
                let packed = index % unique_per_blade;
                let (row, left) = (packed / 2, (packed % 2) == 0);
                // What it used to derive from the draw slot.
                let (want_row, want_left) = legacy_row_and_side(segments, slot);
                assert_eq!(blade, slot / (segments * 6), "blade at slot {slot}");
                assert_eq!(row, want_row, "row at slot {slot} of {blades}x{segments}");
                assert_eq!(left, want_left, "side at slot {slot} of {blades}x{segments}");
            }
        }
    }

    // The saving, stated as a test so a future change to the ribbon shape cannot quietly
    // give it back: distinct indices are what the post-transform cache can collapse to.
    #[test]
    fn the_table_collapses_thirty_slots_onto_twelve_vertices() {
        let near = build_blade_indices(12, 5);
        let distinct: std::collections::BTreeSet<u32> = near.iter().copied().collect();
        assert_eq!(near.len(), 12 * 30);
        assert_eq!(distinct.len(), 12 * 12, "near blade must have 12 unique vertices");

        let mid = build_blade_indices(6, 3);
        let distinct_mid: std::collections::BTreeSet<u32> = mid.iter().copied().collect();
        assert_eq!(mid.len(), 6 * 18);
        assert_eq!(distinct_mid.len(), 6 * 8, "mid blade must have 8 unique vertices");
    }

    // Identity indices must reproduce a non-indexed draw exactly -- that is the whole
    // reason the card path can share one draw call with the blades.
    #[test]
    fn identity_indices_are_the_non_indexed_order() {
        let ids = identity_indices(12);
        assert_eq!(ids, (0..12).collect::<Vec<u32>>());
    }
}
