use bytemuck::Zeroable;
use wgpu::util::DeviceExt;

use crate::ffi::{WgrTerrainMaterial, WgrTerrainNode, WgrTerrainParams};
use crate::gfx3d::{NORMAL_FORMAT, depth_format};

mod shore_wet;
mod skyvis;
#[cfg(test)]
mod puddle_tests;

pub use shore_wet::TwWetParams;
fn wet_soil_debug_mode(mode: f32) -> f32 {
    if mode.is_finite() && (0.0..=4.0).contains(&mode) && mode.fract() == 0.0 { mode }
    else { 0.0 }
}
#[test]
fn wet_soil_debug_padding_refuses_non_enum_values_and_resets_normal() {
    assert_eq!(std::mem::size_of::<WgrTerrainParams>(),88);
    assert_eq!(std::mem::offset_of!(WgrTerrainParams,_pad3),80);
    assert_eq!(std::mem::size_of::<TerrainShadowMap>(),64);
    assert_eq!(std::mem::offset_of!(TerrainShadowMap,_pad2),44);
    for mode in [0.0,1.0,2.0,3.0,4.0] { assert_eq!(wet_soil_debug_mode(mode),mode); }
    for mode in [-1.0,0.5,5.0,f32::NAN,f32::INFINITY] { assert_eq!(wet_soil_debug_mode(mode),0.0); }
}

fn terrain_resource_reuse() -> bool {
    static ENABLED: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    // Installed edit A/B did not reduce stalls; retain only as an explicit experiment.
    *ENABLED.get_or_init(|| std::env::var("WGR_TERRAIN_RESOURCE_REUSE").is_ok_and(|v| v == "1"))
}

fn same_texture_size(old: (u32, u32), new: (u32, u32)) -> bool {
    old == new && new.0 != 0 && new.1 != 0
}

#[test]
fn terrain_resource_reuse_requires_matching_nonempty_dimensions() {
    assert!(same_texture_size((2048, 1024), (2048, 1024)));
    assert!(!same_texture_size((1024, 2048), (2048, 1024)));
    assert!(!same_texture_size((2048, 2048), (2048, 1024)));
    assert!(!same_texture_size((0, 1024), (0, 1024)));
    assert!(!same_texture_size((1024, 0), (1024, 0)));
}

// Which terrain pipeline a draw uses (docs/depth-prepass-plan.md). Color = the shading
// pass (depth write ON). ColorNoWrite = the prepassed segment's colour pass (depth
// already complete -> GreaterEqual/write-off). Prepass = the depth+normal G-buffer pass.
pub enum TerrainPass {
    Color,
    ColorNoWrite,
    Prepass,
    Planar,
}

// Grid mesh resolution: GRID_N quads per axis, (GRID_N+1)^2 vertices, u16 indices.
const GRID_N: u32 = 32;

/// Sinkhole W1: the terrain hole buffer -- a vec4 header (x = record count) + 64 vec4 records, matching
/// `TerrainHoles` in terrain.wgsl and grass.wgsl and the 64-edge cap of the GL33 path.
pub const TERRAIN_HOLES_MAX_EDGES: usize = 64;
pub const TERRAIN_HOLES_FLOATS: usize = 4 + TERRAIN_HOLES_MAX_EDGES * 4;
pub const TERRAIN_HOLES_BYTES: u64 = (TERRAIN_HOLES_FLOATS * 4) as u64;

// Capacity of the ground binding_array (and the device binding-array limit we
// request). Must match WGR_TERRAIN_MAX_GROUND_LAYERS in wgpu_renderer.hpp.
pub const TERRAIN_MAX_GROUND_LAYERS: u32 = 4096;

// Recompute the terrain sun-shadow mask only once the sun has moved past this
// angular threshold (cos of ~0.25°), so the amortized sweep skips most frames.
const SUN_MOVE_COS: f32 = 0.99999;

// Mask resolution multiplier over the heightmap grid: the occluder heightfield is
// coarse (~50 m texels) but the shadow boundary it casts is sharp, so a finer mask
// keeps that boundary crisp instead of smearing it over a heightmap texel. 2x is
// the sweet spot (higher shows no further improvement — the heightfield is the real
// limit). Capped so even large heightmaps stay within a sane VRAM budget.
const SHADOW_MASK_SCALE: u32 = 2;
const SHADOW_MASK_DIM_CAP: u32 = 4096;

// Uniform for the terrain sun-shadow compute sweep (terrain_shadow.wgsl). sun_dir
// is surface-to-light (the negated frame.sun_dir_world travel direction), so the
// sun above the horizon means sun_dir.y > 0. inv_scale maps a mask texel back to
// (fractional) heightfield-texel space. Layout matches the WGSL struct.
#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
struct ShadowSweep {
    world_origin: glam::Vec2,
    terrain_grid: f32,
    penumbra: f32,
    inv_scale: glam::Vec2,
    hm_width: u32,
    hm_height: u32,
    mask_width: u32,
    mask_height: u32,
    max_steps: u32,
    strength: f32,
    sun_dir: glam::Vec4,
}

// World-xz -> shadow-mask-UV mapping, uploaded into the shared frame group(0) so
// the lit-mesh shader can sample terrain shadow by an object's world position (the
// terrain shader still maps from its own params). Matches TerrainShadowMap in
// frame.wgsl. `enabled` gates the sample off until a real heightmap is loaded.
#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
pub struct TerrainShadowMap {
    pub origin: glam::Vec2,
    pub inv_span: glam::Vec2,
    pub half_texel: glam::Vec2,
    pub enabled: f32,
    // Sky-visibility (ambient-occlusion) controls, riding this shared group(0) uniform so all three
    // consumers (terrain/objects/water) read them without a new binding. strength scales the effect
    // (0 = off), floor keeps a minimum ambient in fully-occluded columns. See terrain_sky_visibility
    // in frame.wgsl and docs/sky-visibility-ambient-plan.md.
    pub sky_vis_strength: f32,
    pub sky_vis_floor: f32,
    // Debug: 1 = terrain fragments output the raw sky-view factor as greyscale (mask inspection).
    pub sky_vis_debug: f32,
    // Exaggeration exponent on the occlusion: occ = 1 - pow(V, contrast). 1 = physical/linear; >1
    // deepens the AO for the near-1 V that smooth heightfields produce (so it is actually visible).
    pub sky_vis_contrast: f32,
    // Existing offset44 padding now carries opted-in wet-soil diagnostic mode.
    // Main frame uploads this mapping even when terrain shadows are disabled.
    pub _pad2: f32,
    // CLD-020 cloud sun-transmittance mapping, riding this same shared group(0) uniform for the
    // same reason the sky-visibility controls do: terrain, objects and grass all need it and none
    // of them wants another binding. xy = the map's snapped world-xz min corner, z = 1/span in
    // metres, w = strength. w = 0 means every surface reads fully lit, which is also what an
    // off-map lookup returns -- missing data must never invent shadow.
    pub cloud_shadow: glam::Vec4,
}

// Terrain height-sampling params for the mesh conform pass (vegetation): the world->
// heightmap-texel mapping matching Landscape::SurfaceY / the terrain shader's
// `sample_height`. Bound (with the heightmap view) as the mesh conform group so an
// object vertex shader can conform ClipLand vegetation to the ground per vertex.
#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
pub struct TerrainConformParams {
    pub origin: glam::Vec2, // world xz of heightmap texel (0,0)
    pub terrain_grid: f32,  // world metres per heightmap texel
    pub enabled: f32,       // 1 when a heightmap is loaded, else 0
    pub hm_width: u32,
    pub hm_height: u32,
    // WLD-023(c) -- the CDLOD lattice the terrain RASTERISES, so a conformed decal can be
    // placed on the surface the terrain draws instead of the one it evaluates. See
    // conform.wgsl `surface_y_raster`. cdlod_base = ranges[0] (metres), cdlod_ratio = the
    // per-level range multiplier, cdlod_morph = the fraction of each band spent morphing,
    // cdlod_mode = 0 off (default, exact previous behaviour) / 1 on.
    pub cdlod_base: f32,
    pub cdlod_ratio: f32,
    pub cdlod_morph: f32,
    pub cdlod_mode: f32,
    pub _pad: [u32; 2],
}

// (baseMult, lodRatio, morphRegion, rasterConformMode) -- see conform_params().
static CONFORM_CDLOD_ENV: std::sync::OnceLock<(f32, f32, f32, f32)> = std::sync::OnceLock::new();

// Mask dimensions target `scale`x the heightmap, capped independently of source
// resolution. Large heightfields must not turn clamp's lower bound above its cap.
fn shadow_mask_dims(w: u32, h: u32, scale: u32, max_dim: u32) -> (u32, u32) {
    let cap = SHADOW_MASK_DIM_CAP.min(max_dim);
    let mw = w.saturating_mul(scale.max(1)).min(cap);
    let mh = h.saturating_mul(scale.max(1)).min(cap);
    (mw, mh)
}

#[test]
fn shadow_mask_large_heightfields_keep_a_bounded_shadow_allocation() {
    assert_eq!(shadow_mask_dims(2048, 2048, 2, 8192), (4096, 4096));
    assert_eq!(shadow_mask_dims(8192, 8192, 2, 8192), (4096, 4096));
    assert_eq!(shadow_mask_dims(16384, 8192, 1, 16384), (4096, 4096));
    assert_eq!(shadow_mask_dims(8192, 8192, 2, 2048), (2048, 2048));
    assert_eq!(shadow_mask_dims(1024, 512, 0, 8192), (1024, 512));
    assert_eq!(shadow_mask_dims(8192, 8192, u32::MAX, 8192), (4096, 4096));
}

// Source heightfield retained for re-running the sky-visibility scan when its ImGui-tuned options
// change (radius/K/downsample), without waiting for a fresh heightmap upload.
struct SkyvisSrc {
    heights: Vec<f32>,
    raw_visibility: Vec<f32>,
    w: u32,
    h: u32,
    terrain_grid: f32,
}

fn snow_surface_params(deposit: f32, height: f32, range: f32, depth: f32) -> [f32; 4] {
    let deposit = if deposit.is_finite() { deposit.max(0.0) } else { 0.0 };
    if height.is_finite() && height >= 0.0 && range.is_finite() && range > 0.0
        && depth.is_finite() && depth > 0.0 {
        [deposit, height, range, depth.clamp(0.0, 1.0)]
    } else {
        [deposit, -1.0, 0.0, 0.0]
    }
}

fn ground_weather_params(wetness: f32, rain: f32, sea_level: f32, time: f32) -> [f32; 4] {
    if !sea_level.is_finite() || !time.is_finite() { return [0.0; 4]; }
    let bounded = |v: f32| if v.is_finite() { v.clamp(0.0, 1.0) } else { 0.0 };
    [bounded(wetness), bounded(rain), sea_level, time]
}

fn valid_mud_view(data: &[f32]) -> bool {
    data.len() == 4 + 512 * 512 && data.iter().all(|v| v.is_finite())
        && data[2] == 0.125 && data[3] >= 0.0 && data[3] <= 0.35
        && data[4..].iter().all(|v| *v <= 0.0 && *v >= -data[3])
}

fn valid_sand_view(data: &[f32]) -> bool {
    data.len() == 4 + 512 * 512 && data.iter().all(|v| v.is_finite())
        && data[2] == 0.125 && data[3] >= 0.0 && data[3] <= 0.15
        && data[4..].iter().all(|v| *v >= -data[3] && *v <= if data[3] > 0.0 { 0.02 } else { 0.0 })
}

fn sand_upload_receipt(data: &[f32]) -> (u64, usize, usize, f32, f32) {
    let mut hash = 14695981039346656037u64;
    let (mut negative, mut positive, mut minimum, mut maximum) = (0, 0, 0.0f32, 0.0f32);
    for (i, value) in data.iter().enumerate() {
        for byte in value.to_le_bytes() {
            hash = (hash ^ u64::from(byte)).wrapping_mul(1099511628211);
        }
        if i >= 4 && value.is_finite() {
            negative += usize::from(*value < 0.0);
            positive += usize::from(*value > 0.0);
            minimum = minimum.min(*value);
            maximum = maximum.max(*value);
        }
    }
    (hash, negative, positive, minimum, maximum)
}

#[test]
fn sand_upload_accepts_signed_rims_and_rejects_unbounded_or_disabled_geometry() {
    let mut data = vec![0.0; 4 + 512 * 512];
    data[2] = 0.125; data[3] = 0.15; data[4] = -0.10; data[5] = 0.02;
    assert!(valid_sand_view(&data));
    data[5] = 0.021; assert!(!valid_sand_view(&data));
    data[5] = 0.0; data[4] = -0.151; assert!(!valid_sand_view(&data));
    data[4] = f32::NAN; assert!(!valid_sand_view(&data));
    data[4] = 0.0; data[3] = 0.0; data[5] = 0.01; assert!(!valid_sand_view(&data));
    data[5] = 0.0; assert!(valid_sand_view(&data));
    data[2] = 0.25; assert!(!valid_sand_view(&data));
    assert!(!valid_sand_view(&data[..data.len()-1]));
}

#[test]
fn mud_upload_rejects_nonfinite_positive_and_excess_depth_without_touching_snow() {
    let mut data = vec![0.0; 4 + 512 * 512];
    data[2] = 0.125; data[3] = 0.06; data[4] = -0.03;
    assert!(valid_mud_view(&data));
    data[4] = 0.01; assert!(!valid_mud_view(&data));
    data[4] = -0.061; assert!(!valid_mud_view(&data));
    data[4] = f32::NAN; assert!(!valid_mud_view(&data));
    data[4] = 0.0; data[2] = 0.25; assert!(!valid_mud_view(&data));
    data[2] = 0.125; data[3] = 0.35; data[4] = -0.24; assert!(valid_mud_view(&data));
    data[3] = 0.351; assert!(!valid_mud_view(&data));
    assert!(!valid_mud_view(&data[..data.len()-1]));
}

#[test]
fn ground_weather_preserves_drying_film_without_rain_and_rejects_invalid_state() {
    assert_eq!(ground_weather_params(0.6, 0.0, -2.0, 25.0), [0.6, 0.0, -2.0, 25.0]);
    assert_eq!(ground_weather_params(2.0, -1.0, 0.0, 25.0), [1.0, 0.0, 0.0, 25.0]);
    for invalid in [f32::NAN, f32::INFINITY, f32::NEG_INFINITY] {
        assert_eq!(ground_weather_params(invalid, invalid, 0.0, 25.0), [0.0, 0.0, 0.0, 25.0]);
        assert_eq!(ground_weather_params(1.0, 1.0, invalid, 25.0), [0.0; 4]);
        assert_eq!(ground_weather_params(1.0, 1.0, 0.0, invalid), [0.0; 4]);
    }
}

#[test]
fn object_snow_uses_actual_deposit_and_finite_surface_altitude_params() {
    assert_eq!(snow_surface_params(0.18, 200.0, 50.0, 0.8), [0.18, 200.0, 50.0, 0.8]);
    assert_eq!(snow_surface_params(0.0, -1.0, 50.0, 0.8), [0.0, -1.0, 0.0, 0.0]);
    for invalid in [f32::NAN, f32::INFINITY, -1.0] {
        assert_eq!(snow_surface_params(invalid, -1.0, 0.0, 0.0), [0.0, -1.0, 0.0, 0.0]);
        assert_eq!(snow_surface_params(0.18, 200.0, invalid, 0.8), [0.18, -1.0, 0.0, 0.0]);
        assert_eq!(snow_surface_params(0.18, 200.0, 50.0, invalid), [0.18, -1.0, 0.0, 0.0]);
    }
    assert_eq!(snow_surface_params(0.18, 200.0, 50.0, 2.0), [0.18, 200.0, 50.0, 1.0]);
}

pub struct Terrain {
    group1_layout: wgpu::BindGroupLayout,
    group2_layout: wgpu::BindGroupLayout,

    params_ubo: wgpu::Buffer,
    medium_source: crate::rain_water_medium::SourceLifetime,
    snow_buffer: wgpu::Buffer,
    // Actual mission deposit from the accepted snow header. Ground deficits contain tracks
    // and roof shelter and therefore must NEVER be applied to a raised object surface.
    snow_deposit_depth: f32,
    holes_buffer: wgpu::Buffer,
    // CPU copy of the hole edges (the grass placement keeps its own GPU copy) and a counter it
    // watches to know when to refresh it.
    holes_data: Vec<f32>,
    holes_gen: u64,
    params: WgrTerrainParams,
    #[allow(dead_code)] // kept alive: group1_bind references its view
    heightmap: wgpu::Texture,
    group1_bind: wgpu::BindGroup,
    have_heightmap: bool,
    // Persistent view of the current heightmap, lent to the mesh conform bind group so
    // object vertex shaders sample SurfaceY for terrain-conformed vegetation. mask_gen
    // bumps on realloc (heightmap + mask recreate together), gating the rebuild.
    heightmap_view: wgpu::TextureView,

    // Long-distance terrain sun-shadow mask + its amortized compute sweep. The
    // mask (Rgba8Unorm, same grid as the heightmap) is storage-written by the
    // sweep and sampled in fs_terrain; recompute is gated on the sun moving or the
    // heightmap changing. world_origin/terrain_grid/hm_* feed the sweep uniform.
    #[allow(dead_code)] // kept alive: group1_bind + sweep_bind reference its view
    shadow_mask: wgpu::Texture,
    // Persistent view of the current mask, lent to the shared frame group(0) so lit
    // meshes sample terrain shadow; mask_gen bumps on realloc so that bind rebuilds.
    shadow_mask_view: wgpu::TextureView,
    mask_gen: u64,

    // Sky-visibility (sky-view factor) mask: a COARSE, CPU-computed R8Unorm texture (V in [0,1] per
    // column), lent to the shared frame group(0) so terrain/objects/water modulate their ambient by
    // it. Computed once per heightmap (sun-independent) in set_heightmap; a 1x1 stand-in until then.
    // See skyvis.rs + docs/sky-visibility-ambient-plan.md.
    #[allow(dead_code)] // kept alive: skyvis_view references it
    skyvis_mask: wgpu::Texture,
    skyvis_view: wgpu::TextureView,
    sky_vis_strength: f32,
    sky_vis_floor: f32,
    sky_vis_debug: f32,
    sky_vis_contrast: f32,
    skyvis_opts: skyvis::SkyvisOptions,
    // Source heightfield kept so the ImGui-tuned scan options (radius/K/downsample) can re-run the
    // scan without a fresh heightmap upload. None until the first heightmap arrives.
    skyvis_src: Option<SkyvisSrc>,

    shadow_sweep_ubo: wgpu::Buffer,
    shadow_sweep_layout: wgpu::BindGroupLayout,
    shadow_sweep_bind: wgpu::BindGroup,
    shadow_pipeline: wgpu::ComputePipeline,
    mask_sampler: wgpu::Sampler,
    world_origin: glam::Vec2,
    terrain_grid: f32,
    max_height: f32,
    min_height: f32,
    hm_width: u32,
    hm_height: u32,
    mask_width: u32,
    mask_height: u32,
    shadow_scale: u32,
    shadow_max_steps: u32,
    shadow_penumbra: f32,
    shadow_strength_mul: f32,
    shadow_dirty: bool,
    last_sun_dir: glam::Vec3,

    // group2 resources; group2_bind holds views into all of them. Kept so any
    // one can be replaced and the bind group rebuilt.
    ground_views: Vec<wgpu::TextureView>,
    // Fills unused binding_array slots when PARTIALLY_BOUND is unavailable.
    pad_view: wgpu::TextureView,
    partially_bound: bool,
    index_map: wgpu::Texture,
    detail_view: wgpu::TextureView,
    jitter_map: wgpu::Texture,
    materials: wgpu::Buffer,
    ground_sampler: wgpu::Sampler,
    ground_clamp_sampler: wgpu::Sampler,
    group2_bind: wgpu::BindGroup,

    grid_vbuf: wgpu::Buffer,
    grid_ibuf: wgpu::Buffer,
    grid_index_count: u32,

    instance_buf: wgpu::Buffer,
    instance_cap: u64,
    instance_count: u32,

    pipeline: wgpu::RenderPipeline,
    // Depth-prepass companions (docs/depth-prepass-plan.md): the same shading pipeline
    // with depth-write OFF (colour pass of the prepassed segment), and the depth+normal
    // prepass pipeline (writes the view-space normal G-buffer + depth).
    pipeline_no_write: wgpu::RenderPipeline,
    prepass_pipeline: wgpu::RenderPipeline,
    // The reflection is already resolution-scaled and mip-filtered by water. Keeping a
    // dedicated 1x variant avoids multiplying that secondary view by the main scene's MSAA.
    planar_pipeline: wgpu::RenderPipeline,
    max_dim: u32,
    // TW-WATER W3b: Tidewater's wet sand (group 3; a placeholder with Current OP).
    shore_wet: shore_wet::ShoreWet,
}

impl Terrain {
    #[allow(clippy::too_many_arguments)]
    pub fn new(
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        camera_layout: &wgpu::BindGroupLayout,
        surface_format: wgpu::TextureFormat,
        sample_count: u32,
        partially_bound: bool,
        white_view: wgpu::TextureView,
        composer: &mut naga_oil::compose::Composer,
    ) -> Self {
        // group 1 (vertex): terrain params UBO + heightmap (R32Float, textureLoad).
        let group1_layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("wgr_terrain_group1_layout"),
            entries: &[
                wgpu::BindGroupLayoutEntry {
                    binding: 0,
                    // Vertex reads grid/terrain spacing; fragment reads land_grid for tiling UVs.
                    visibility: wgpu::ShaderStages::VERTEX_FRAGMENT,
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
                    // Vertex displaces by the height; fragment reads it again for a
                    // per-pixel, LOD/morph-independent normal (even lighting).
                    visibility: wgpu::ShaderStages::VERTEX_FRAGMENT,
                    ty: wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Float { filterable: false },
                        view_dimension: wgpu::TextureViewDimension::D2,
                        multisampled: false,
                    },
                    count: None,
                },
                // Long-distance sun-shadow mask (filterable; one bilinear tap) +
                // its clamping sampler. Written by the compute sweep, sampled here.
                wgpu::BindGroupLayoutEntry {
                    binding: 2,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Float { filterable: true },
                        view_dimension: wgpu::TextureViewDimension::D2,
                        multisampled: false,
                    },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 3,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Filtering),
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 4,
                    visibility: wgpu::ShaderStages::VERTEX_FRAGMENT,
                    ty: wgpu::BindingType::Buffer {
                        ty: wgpu::BufferBindingType::Storage { read_only: true },
                        has_dynamic_offset: false,
                        min_binding_size: wgpu::BufferSize::new((4 + 512 * 512) * 4),
                    },
                    count: None,
                },
                // Sinkhole W1: terrain hole edges (TerrainHoles in terrain.wgsl), read by the fragment
                // stages only -- the colour, prepass and planar passes discard the same pixels.
                wgpu::BindGroupLayoutEntry {
                    binding: 5,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Buffer {
                        ty: wgpu::BufferBindingType::Storage { read_only: true },
                        has_dynamic_offset: false,
                        min_binding_size: wgpu::BufferSize::new(TERRAIN_HOLES_BYTES),
                    },
                    count: None,
                },
            ],
        });

        // Compute-side layout for the sun-shadow sweep: sweep uniform + heightmap
        // (textureLoad) + the mask as a storage texture the sweep writes.
        let shadow_sweep_layout =
            device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
                label: Some("wgr_terrain_shadow_sweep_layout"),
                entries: &[
                    wgpu::BindGroupLayoutEntry {
                        binding: 0,
                        visibility: wgpu::ShaderStages::COMPUTE,
                        ty: wgpu::BindingType::Buffer {
                            ty: wgpu::BufferBindingType::Uniform,
                            has_dynamic_offset: false,
                            min_binding_size: wgpu::BufferSize::new(
                                std::mem::size_of::<ShadowSweep>() as u64,
                            ),
                        },
                        count: None,
                    },
                    wgpu::BindGroupLayoutEntry {
                        binding: 1,
                        visibility: wgpu::ShaderStages::COMPUTE,
                        ty: wgpu::BindingType::Texture {
                            sample_type: wgpu::TextureSampleType::Float { filterable: false },
                            view_dimension: wgpu::TextureViewDimension::D2,
                            multisampled: false,
                        },
                        count: None,
                    },
                    wgpu::BindGroupLayoutEntry {
                        binding: 2,
                        visibility: wgpu::ShaderStages::COMPUTE,
                        ty: wgpu::BindingType::StorageTexture {
                            access: wgpu::StorageTextureAccess::WriteOnly,
                            format: wgpu::TextureFormat::Rgba16Float,
                            view_dimension: wgpu::TextureViewDimension::D2,
                        },
                        count: None,
                    },
                ],
            });

        // group 2 (fragment): bindless ground texture binding_array + filtering
        // sampler + per-cell index map (uint, textureLoad) + high-frequency
        // detail noise texture + an edge-extending sampler for clamped
        // transition tiles (index-map bit 15) + per-grid-point jitter map.
        let group2_layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("wgr_terrain_group2_layout"),
            entries: &[
                wgpu::BindGroupLayoutEntry {
                    binding: 0,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Float { filterable: true },
                        view_dimension: wgpu::TextureViewDimension::D2,
                        multisampled: false,
                    },
                    count: Some(std::num::NonZeroU32::new(TERRAIN_MAX_GROUND_LAYERS).unwrap()),
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 1,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Filtering),
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 2,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Uint,
                        view_dimension: wgpu::TextureViewDimension::D2,
                        multisampled: false,
                    },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 3,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Float { filterable: true },
                        view_dimension: wgpu::TextureViewDimension::D2,
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
                wgpu::BindGroupLayoutEntry {
                    binding: 5,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Float { filterable: true },
                        view_dimension: wgpu::TextureViewDimension::D2,
                        multisampled: false,
                    },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 6,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Buffer {
                        ty: wgpu::BufferBindingType::Storage { read_only: true },
                        has_dynamic_offset: false,
                        min_binding_size: wgpu::BufferSize::new(std::mem::size_of::<
                            WgrTerrainMaterial,
                        >() as u64),
                    },
                    count: None,
                },
            ],
        });

        let params_ubo = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("wgr_terrain_params"),
            size: std::mem::size_of::<WgrTerrainParams>() as u64,
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        // Nonzero defaults so the shader never divides by zero before the first upload.
        let default_params = WgrTerrainParams {
            world_origin: glam::Vec2::ZERO,
            land_grid: 1.0,
            terrain_grid: 1.0,
            hm_width: 1,
            hm_height: 1,
            land_range: 1,
            data_scale: 1.0,
            sea_level: 0.0,
            time: 0.0,
            swash_speed: 0.15,
            swash_amp: 0.15,
            wet_height: 1.2,
            wet_darken: 0.55,
            enfusion_ground: 0.0,
            snowline_height: -1.0,
            snowline_range: 25.0,
            snowline_depth: 0.06,
            _pad1: 0.0,
            _pad2: 0.0,
            _pad3: 0.0,
            _pad4: 0.0,
        };
        queue.write_buffer(&params_ubo, 0, bytemuck::bytes_of(&default_params));

        // 1x1 stand-in heightmap + ground array so the bind groups are valid before
        // any upload; terrain never draws until a real heightmap arrives.
        let (heightmap, heightmap_view) = create_heightmap(device, 1, 1);
        queue.write_texture(
            texel_copy(&heightmap),
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
        // 1x1 stand-in mask + its clamping sampler; replaced (with the heightmap)
        // on the first real upload. Terrain never draws until then.
        let shadow_mask = create_shadow_mask(device, 1, 1);
        let shadow_mask_view = shadow_mask.create_view(&wgpu::TextureViewDescriptor::default());
        // 1x1 stand-in sky-visibility mask (fully visible = 255) until a heightmap loads.
        let (skyvis_mask, skyvis_view) = create_skyvis(device, queue, 1, 1, &[255u8]);
        // Default-ON with the user-tuned values (2026-07-12). Terrain params reach the renderer only
        // via the SetShadowMapTuning FFI push, so — like the sun-shadow sweep — the RENDERER default is
        // what makes it on out of the box; the C++ ShadowMapTuning defaults mirror these for ImGui.
        // Env overrides: WGR_SKY_VIS = strength (0 = off), WGR_SKY_VIS_FLOOR, WGR_SKY_VIS_CONTRAST.
        let sky_vis_strength = env_f32("WGR_SKY_VIS", 0.70);
        let sky_vis_floor = env_f32("WGR_SKY_VIS_FLOOR", 0.30);
        let mask_sampler = device.create_sampler(&wgpu::SamplerDescriptor {
            label: Some("wgr_terrain_shadow_mask_sampler"),
            address_mode_u: wgpu::AddressMode::ClampToEdge,
            address_mode_v: wgpu::AddressMode::ClampToEdge,
            address_mode_w: wgpu::AddressMode::ClampToEdge,
            mag_filter: wgpu::FilterMode::Linear,
            min_filter: wgpu::FilterMode::Linear,
            mipmap_filter: wgpu::MipmapFilterMode::Nearest,
            ..Default::default()
        });
        let snow_buffer = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("wgr_experimental_snow"),
            // Snow occupies the first view, signed mud the second, sand the third. Reusing
            // the storage binding keeps colour, depth and conformed meshes aligned.
            size: 3 * (4 + 512 * 512) * 4,
            usage: wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        let holes_buffer = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("wgr_terrain_holes"),
            size: TERRAIN_HOLES_BYTES,
            // UNIFORM too (Sinkhole W1b): the object shading reads the openings from the conform
            // group, and the GPU-driven path has no fragment storage slot left for it.
            usage: wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
            // Zero-initialised by wgpu: info.x = 0, no holes.
            mapped_at_creation: false,
        });
        let group1_bind = make_group1(
            device,
            &group1_layout,
            &params_ubo,
            &heightmap_view,
            &shadow_mask_view,
            &mask_sampler,
            &snow_buffer,
            &holes_buffer,
        );

        // Sun-shadow sweep: uniform + compute pipeline + bind group. Range cap =
        // max_steps * terrain_grid; both the step count and the penumbra (degrees)
        // are env-tunable, matching the other terrain knobs.
        let shadow_sweep_ubo = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("wgr_terrain_shadow_sweep"),
            size: std::mem::size_of::<ShadowSweep>() as u64,
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        let shadow_sweep_bind = make_sweep_bind(
            device,
            &shadow_sweep_layout,
            &shadow_sweep_ubo,
            &heightmap_view,
            &shadow_mask_view,
        );
        let shadow_shader = crate::shaders::make_module(
            device,
            composer,
            "wgr_terrain_shadow_shader",
            include_str!("terrain_shadow.wgsl"),
            "terrain/terrain_shadow.wgsl",
        );
        let shadow_pipeline_layout =
            device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
                label: Some("wgr_terrain_shadow_pipeline_layout"),
                bind_group_layouts: &[Some(&shadow_sweep_layout)],
                immediate_size: 0,
            });
        let shadow_pipeline = device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
            label: Some("wgr_terrain_shadow_pipeline"),
            layout: Some(&shadow_pipeline_layout),
            module: &shadow_shader,
            entry_point: Some("sweep"),
            compilation_options: wgpu::PipelineCompilationOptions::default(),
            cache: None,
        });
        let shadow_max_steps = std::env::var("WGR_TERRAIN_SHADOW_STEPS")
            .ok()
            .and_then(|v| v.parse::<u32>().ok())
            .unwrap_or(512)
            .max(1);
        let shadow_penumbra = std::env::var("WGR_TERRAIN_SHADOW_PENUMBRA")
            .ok()
            .and_then(|v| v.parse::<f32>().ok())
            .unwrap_or(1.0)
            .to_radians();
        let shadow_scale = std::env::var("WGR_TERRAIN_SHADOW_SCALE")
            .ok()
            .and_then(|v| v.parse::<u32>().ok())
            .unwrap_or(SHADOW_MASK_SCALE)
            .max(1);

        // Stand-ins so the bind group is valid before any upload. Ground = the
        // shared 1x1 white; detail alpha = 0.5 (so the shader's 2*alpha
        // modulation is a no-op); index map = layer 0.
        let ground_views = vec![white_view.clone()];
        let materials = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
            label: Some("wgr_terrain_materials"),
            contents: bytemuck::bytes_of(&WgrTerrainMaterial::zeroed()),
            usage: wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_DST,
        });
        let index_map = create_index_map(device, 1, 1);
        queue.write_texture(
            texel_copy(&index_map),
            bytemuck::bytes_of(&0u16),
            wgpu::TexelCopyBufferLayout {
                offset: 0,
                bytes_per_row: Some(2),
                rows_per_image: Some(1),
            },
            wgpu::Extent3d {
                width: 1,
                height: 1,
                depth_or_array_layers: 1,
            },
        );
        let detail = device.create_texture(&wgpu::TextureDescriptor {
            label: Some("wgr_terrain_detail_neutral"),
            size: wgpu::Extent3d {
                width: 1,
                height: 1,
                depth_or_array_layers: 1,
            },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: wgpu::TextureFormat::Rgba8Unorm,
            usage: wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::COPY_DST,
            view_formats: &[],
        });
        queue.write_texture(
            texel_copy(&detail),
            &[0x80, 0x80, 0x80, 0x80],
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
        // The view keeps the neutral stand-in texture alive.
        let detail_view = detail.create_view(&wgpu::TextureViewDescriptor::default());
        // Zero jitter until a real map arrives.
        let jitter_map = create_jitter_map(device, 1, 1);
        queue.write_texture(
            texel_copy(&jitter_map),
            &[0u8, 0u8],
            wgpu::TexelCopyBufferLayout {
                offset: 0,
                bytes_per_row: Some(2),
                rows_per_image: Some(1),
            },
            wgpu::Extent3d {
                width: 1,
                height: 1,
                depth_or_array_layers: 1,
            },
        );
        // 16x anisotropy: terrain is the worst case for isotropic mip selection
        // (large planes at grazing angles), and GL33's samplers already use it.
        let ground_sampler = device.create_sampler(&wgpu::SamplerDescriptor {
            label: Some("wgr_terrain_ground_sampler"),
            address_mode_u: wgpu::AddressMode::Repeat,
            address_mode_v: wgpu::AddressMode::Repeat,
            address_mode_w: wgpu::AddressMode::ClampToEdge,
            mag_filter: wgpu::FilterMode::Linear,
            min_filter: wgpu::FilterMode::Linear,
            mipmap_filter: wgpu::MipmapFilterMode::Linear,
            anisotropy_clamp: 16,
            ..Default::default()
        });
        // For clamped transition tiles: edge-extends the tile past its own cell
        // (GL33's ClampU|ClampV) instead of wrapping.
        let ground_clamp_sampler = device.create_sampler(&wgpu::SamplerDescriptor {
            label: Some("wgr_terrain_ground_clamp_sampler"),
            address_mode_u: wgpu::AddressMode::ClampToEdge,
            address_mode_v: wgpu::AddressMode::ClampToEdge,
            address_mode_w: wgpu::AddressMode::ClampToEdge,
            mag_filter: wgpu::FilterMode::Linear,
            min_filter: wgpu::FilterMode::Linear,
            mipmap_filter: wgpu::MipmapFilterMode::Linear,
            anisotropy_clamp: 16,
            ..Default::default()
        });
        let group2_bind = make_group2(
            device,
            &group2_layout,
            &ground_views,
            &white_view,
            partially_bound,
            &index_map,
            &detail_view,
            &jitter_map,
            &materials,
            &ground_sampler,
            &ground_clamp_sampler,
        );

        let (grid_vbuf, grid_ibuf, grid_index_count) = build_grid(device);

        let instance_cap = 64 * std::mem::size_of::<WgrTerrainNode>() as u64;
        let instance_buf = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("wgr_terrain_instances"),
            size: instance_cap,
            usage: wgpu::BufferUsages::VERTEX | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });

        let shader = crate::shaders::make_module(
            device,
            composer,
            "wgr_terrain_shader",
            include_str!("terrain.wgsl"),
            "terrain/terrain.wgsl",
        );
        // Override constants baked from the environment (see terrain.wgsl).
        let blend_width = std::env::var("WGR_TERRAIN_BLEND_WIDTH")
            .ok()
            .and_then(|v| v.parse::<f64>().ok())
            .unwrap_or(0.15);
        let skirt_k = std::env::var("WGR_TERRAIN_SKIRT_K")
            .ok()
            .and_then(|v| v.parse::<f64>().ok())
            .unwrap_or(4.0);
        // Strength of the authored terrain composition (WLD-019): the satellite is the
        // colour authority and the decoded surface stack modulates it. ON by default as of
        // 2026-08-12, on the owner's look call across Takistan, Chernarus and Stratis.
        //
        // This cannot touch Operation Flashpoint. OFP worlds carry no authored terrain
        // materials at all -- `_a3TerrainMaterials` is only ever filled by LoadOprwModern --
        // so their `surface_count` is 0 and sample_authored_cell early-returns to the legacy
        // cell path byte for byte, whatever this value is.
        //
        // `WGR_TERRAIN_A3_SURFACES=0` restores the pre-decode look on Arma worlds.
        let a3_surface_blend = std::env::var("WGR_TERRAIN_A3_SURFACES")
            .ok()
            .and_then(|v| v.parse::<f64>().ok())
            .unwrap_or(1.0);
        // MAT-040: authored-surface parallax depth, in the surface's own UV units. Height
        // is `_nopx` red, which only terrain surfaces carry, so this is inert on any
        // material whose normals are `_nohq`.
        let terrain_parallax = std::env::var("WGR_TERRAIN_PARALLAX")
            .ok()
            .and_then(|v| v.parse::<f64>().ok())
            .unwrap_or(0.02);
        // Authored-detail distance bands (see terrain.wgsl). Two bands, not one: the
        // COLOUR reaches far because the satellite it modulates is ~1 texel per metre and
        // is therefore being magnified over the whole mid field, while the per-surface
        // NORMAL -- one extra fetch per selected slot -- stops much sooner because its
        // micro-shading goes sub-pixel first. Keeping them separate is what holds the far
        // band at satellite + mask + colour instead of letting the fetch count grow with
        // the distance the band covers.
        //
        // A/B from a capture command line:
        //   WGR_TERRAIN_DETAIL_START=25 WGR_TERRAIN_DETAIL_END=120 \
        //   WGR_TERRAIN_DETAIL_NORMAL_START=25 WGR_TERRAIN_DETAIL_NORMAL_END=120
        // is the pre-change behaviour exactly.
        let env_f64 = |name: &str, fallback: f64| {
            std::env::var(name)
                .ok()
                .and_then(|v| v.parse::<f64>().ok())
                .unwrap_or(fallback)
        };
        let detail_fade_start = env_f64("WGR_TERRAIN_DETAIL_START", 600.0);
        // Never let the band invert: a smoothstep with end <= start is a step, which pops.
        let detail_fade_end = env_f64("WGR_TERRAIN_DETAIL_END", 900.0).max(detail_fade_start + 1.0);
        let detail_normal_start = env_f64("WGR_TERRAIN_DETAIL_NORMAL_START", 150.0);
        let detail_normal_end =
            env_f64("WGR_TERRAIN_DETAIL_NORMAL_END", 300.0).max(detail_normal_start + 1.0);
        // Whole-tile normal gain (TerrainSNX Stage14). Arma 3 authors one on every material
        // -- 478 of 478 on Stratis -- and it was parsed, bound and uploaded but never
        // sampled. 0 compiles the fetch out entirely, which is the "before" side of the A/B.
        let tile_normal_strength = env_f64("WGR_TERRAIN_TILE_NORMAL", 1.0).max(0.0);
        // How much of each authored surface's gain is taken from its own measured average
        // (its coarsest mip) instead of the 0.5 the x2 in terrain.wgsl assumes. The
        // assumption is right for OFP's `detail_dx.paa` (alpha mean 0.5085) and wrong for
        // the authored stack (0.105-0.144 on DayZ), where the x2 darkens the ground ~3.5x.
        //
        // DEFAULTS TO 0 -- the shipped behaviour, bit for bit. Flipping it changes the look
        // of every Arma 2 / Arma 3 / DayZ world, so it ships measured and off; make it the
        // default by changing this 0.0 to 1.0 (and the `override` in terrain.wgsl to match).
        let surface_normalise = env_f64("WGR_TERRAIN_SURFACE_NORMALISE", 0.0).clamp(0.0, 1.0);
        let vs_constants = [("skirt_k", skirt_k)];
        // HDR path: the color target is Rgba16Float only when HDR is on (see gfx3d),
        // so it signals linear shading (albedo/sun/fog decode + no clamp).
        let linear = if surface_format == wgpu::TextureFormat::Rgba16Float {
            1.0
        } else {
            0.0
        };
        let fs_constants = [
            ("blend_width", blend_width),
            ("a3_surface_blend", a3_surface_blend),
            ("terrain_parallax", terrain_parallax),
            ("detail_fade_start", detail_fade_start),
            ("detail_fade_end", detail_fade_end),
            ("detail_normal_start", detail_normal_start),
            ("detail_normal_end", detail_normal_end),
            ("tile_normal_strength", tile_normal_strength),
            ("surface_normalise", surface_normalise),
            ("linear", linear),
            // WGR_TERRAIN_MIP_FROM_TILE=1 restores mip selection from the JITTERED tiling UV,
            // whose derivative jumps at every cell boundary and stamps a hard square grid into
            // close, steeply-viewed ground. Default 0 = gradients from the smooth cell position.
            (
                "jitter_scale",
                std::env::var("WGR_TERRAIN_JITTER")
                    .ok()
                    .and_then(|v| v.parse::<f64>().ok())
                    .unwrap_or(1.0)
                    .clamp(0.0, 1.0),
            ),
        ];
        let shore_wet = shore_wet::ShoreWet::new(device, queue);
        let pipeline_layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("wgr_terrain_pipeline_layout"),
            bind_group_layouts: &[
                Some(camera_layout),
                Some(&group1_layout),
                Some(&group2_layout),
                Some(shore_wet.layout()),
            ],
            immediate_size: 0,
        });
        let grid_attrs = wgpu::vertex_attr_array![0 => Float32x3];
        let inst_attrs =
            wgpu::vertex_attr_array![1 => Float32x2, 2 => Float32, 3 => Uint32, 4 => Float32x2];
        let vbuf_layouts = [
            wgpu::VertexBufferLayout {
                array_stride: 12,
                step_mode: wgpu::VertexStepMode::Vertex,
                attributes: &grid_attrs,
            },
            wgpu::VertexBufferLayout {
                array_stride: std::mem::size_of::<WgrTerrainNode>() as u64,
                step_mode: wgpu::VertexStepMode::Instance,
                attributes: &inst_attrs,
            },
        ];
        // Three variants sharing the VS (skirt_k) + layout + geometry: the shading
        // pipeline (fs_terrain, depth-write ON), its write-off twin for the prepassed
        // colour pass, and the depth+normal prepass (fs_terrain_prepass -> NORMAL_FORMAT).
        let make_pipeline = |label: &str,
                             fs_entry: &str,
                             fs_constants: &[(&str, f64)],
                             target: wgpu::TextureFormat,
                             depth_write: bool,
                             pipeline_samples: u32| {
            device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
                label: Some(label),
                layout: Some(&pipeline_layout),
                vertex: wgpu::VertexState {
                    module: &shader,
                    entry_point: Some("vs_terrain"),
                    compilation_options: wgpu::PipelineCompilationOptions {
                        constants: &vs_constants,
                        ..Default::default()
                    },
                    buffers: &vbuf_layouts,
                },
                primitive: wgpu::PrimitiveState {
                    topology: wgpu::PrimitiveTopology::TriangleList,
                    cull_mode: None,
                    ..Default::default()
                },
                depth_stencil: Some(wgpu::DepthStencilState {
                    format: depth_format(device),
                    depth_write_enabled: Some(depth_write),
                    // Reversed-Z: nearer geometry has the larger depth value.
                    depth_compare: Some(wgpu::CompareFunction::GreaterEqual),
                    stencil: wgpu::StencilState::default(),
                    bias: wgpu::DepthBiasState::default(),
                }),
                multisample: wgpu::MultisampleState {
                    count: pipeline_samples,
                    ..Default::default()
                },
                fragment: Some(wgpu::FragmentState {
                    module: &shader,
                    entry_point: Some(fs_entry),
                    compilation_options: wgpu::PipelineCompilationOptions {
                        constants: fs_constants,
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
        let pipeline = make_pipeline(
            "wgr_terrain_pipeline",
            "fs_terrain",
            &fs_constants,
            surface_format,
            true,
            sample_count,
        );
        let pipeline_no_write = make_pipeline(
            "wgr_terrain_pipeline_no_write",
            "fs_terrain",
            &fs_constants,
            surface_format,
            false,
            sample_count,
        );
        let prepass_pipeline = make_pipeline(
            "wgr_terrain_prepass_pipeline",
            "fs_terrain_prepass",
            &[],
            NORMAL_FORMAT,
            true,
            sample_count,
        );
        let planar_pipeline = make_pipeline(
            "wgr_terrain_planar_pipeline",
            "fs_terrain",
            &fs_constants,
            surface_format,
            true,
            1,
        );

        Terrain {
            group1_layout,
            group2_layout,
            params_ubo,
            snow_buffer,
            snow_deposit_depth: 0.0,
            holes_buffer,
            holes_data: vec![0.0; TERRAIN_HOLES_FLOATS],
            holes_gen: 0,
            params: default_params,
            medium_source: Default::default(),
            heightmap,
            heightmap_view,
            group1_bind,
            have_heightmap: false,
            shadow_mask,
            shadow_mask_view,
            mask_gen: 0,
            skyvis_mask,
            skyvis_view,
            sky_vis_strength,
            sky_vis_floor,
            sky_vis_debug: if std::env::var("WGR_SKY_VIS_DEBUG").is_ok() {
                1.0
            } else {
                0.0
            },
            sky_vis_contrast: env_f32("WGR_SKY_VIS_CONTRAST", 6.5),
            skyvis_opts: skyvis::SkyvisOptions {
                k_azimuths: 12,
                blur_radius: env_f32("WGR_SKY_VIS_BLUR", 1.0).max(0.0) as u32,
                ..skyvis::SkyvisOptions::default()
            },
            skyvis_src: None,
            shadow_sweep_ubo,
            shadow_sweep_layout,
            shadow_sweep_bind,
            shadow_pipeline,
            mask_sampler,
            world_origin: glam::Vec2::ZERO,
            terrain_grid: 1.0,
            max_height: 0.0,
            min_height: 0.0,
            hm_width: 1,
            hm_height: 1,
            mask_width: 1,
            mask_height: 1,
            shadow_scale,
            shadow_max_steps,
            shadow_penumbra,
            shadow_strength_mul: 1.0,
            // Force a recompute on the first frame after a heightmap arrives.
            shadow_dirty: true,
            last_sun_dir: glam::Vec3::ZERO,
            ground_views,
            pad_view: white_view,
            partially_bound,
            index_map,
            detail_view,
            jitter_map,
            materials,
            ground_sampler,
            ground_clamp_sampler,
            group2_bind,
            grid_vbuf,
            grid_ibuf,
            grid_index_count,
            instance_buf,
            instance_cap,
            instance_count: 0,
            pipeline,
            pipeline_no_write,
            prepass_pipeline,
            planar_pipeline,
            max_dim: device.limits().max_texture_dimension_2d,
            shore_wet,
        }
    }

    // Cheap per-frame params refresh (no heightmap re-upload): the coast wet-band fields
    // (sea_level, time, swash, wet_*) animate every frame, and the static fields are re-sent
    // unchanged. Overwrites the whole params UBO.
    pub fn set_params(&mut self, queue: &wgpu::Queue, params: WgrTerrainParams) {
        queue.write_buffer(&self.params_ubo, 0, bytemuck::bytes_of(&params));
        self.params = params;
    }

    pub fn set_snow(&mut self, queue: &wgpu::Queue, data: &[f32]) {
        if data.len() == 4 + 512 * 512 && data.iter().all(|v| v.is_finite()) {
            queue.write_buffer(&self.snow_buffer, 0, bytemuck::cast_slice(data));
            self.snow_deposit_depth = data[3].max(0.0);
        }
    }

    pub fn set_mud(&mut self, queue: &wgpu::Queue, data: &[f32]) {
        if valid_mud_view(data) {
            queue.write_buffer(&self.snow_buffer, (4 + 512 * 512) * 4, bytemuck::cast_slice(data));
        }
    }

    pub fn snow_surface_params(&self) -> [f32; 4] {
        snow_surface_params(self.snow_deposit_depth, self.params.snowline_height,
            self.params.snowline_range, self.params.snowline_depth)
    }

    pub fn set_sand(&mut self, queue: &wgpu::Queue, data: &[f32]) {
        let accepted = valid_sand_view(data);
        if accepted {
            queue.write_buffer(&self.snow_buffer, 2 * (4 + 512 * 512) * 4, bytemuck::cast_slice(data));
        }
        static TRACE: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
        if *TRACE.get_or_init(|| std::env::var("WGR_SAND_UPLOAD_TRACE").is_ok_and(|v| v == "1")) {
            let (hash, negative, positive, minimum, maximum) = sand_upload_receipt(data);
            let header = data.get(..4).unwrap_or(&[]);
            // accepted/write is not a GPU completion receipt. Exact hashes pair
            // this renderer-owned admission with the queued C++ source window.
            eprintln!("SAND_UPLOAD_RECEIVED accepted={accepted} count={} header={header:?} negative={negative} positive={positive} min={minimum:.8} max={maximum:.8} hash={hash:016x} byteOffset={} bufferBytes={}",
                data.len(), 2 * (4 + 512 * 512) * 4, self.snow_buffer.size());
        }
    }

    pub fn snow_buffer(&self) -> &wgpu::Buffer {
        &self.snow_buffer
    }

    pub fn ground_weather_params(&self) -> [f32; 4] {
        ground_weather_params(self.params._pad1, self.params._pad2,
            self.params.sea_level, self.params.time)
    }

    /// Existing sky-visibility source, borrowed only for an explicit Tidewater activation.
    pub fn shore_height_source(&self) -> Option<(&[f32], WgrTerrainParams)> {
        let source = self.skyvis_src.as_ref()?;
        let mut params = self.params;
        params.hm_width = source.w;
        params.hm_height = source.h;
        params.terrain_grid = source.terrain_grid;
        Some((&source.heights, params))
    }

    /// Sinkhole W1: the terrain hole edges for this frame, `edges` = n float4 records: XZ edges {nx, nz, d, last} and optional {0,0,ceilingY,2}
    /// (Landscape::TerrainHoleDrawEdges). At most TERRAIN_HOLES_MAX_EDGES records are kept; an empty slice clears them.
    /// Writes only when the set changed, so a world without holes costs one comparison per frame.
    pub fn set_holes(&mut self, queue: &wgpu::Queue, edges: &[f32]) {
        let n = (edges.len() / 4).min(TERRAIN_HOLES_MAX_EDGES);
        let mut data = vec![0.0f32; TERRAIN_HOLES_FLOATS];
        if edges[..n * 4].iter().all(|v| v.is_finite()) {
            data[0] = n as f32;
            data[4..4 + n * 4].copy_from_slice(&edges[..n * 4]);
        }
        if data != self.holes_data {
            queue.write_buffer(&self.holes_buffer, 0, bytemuck::cast_slice(&data));
            self.holes_data = data;
            self.holes_gen += 1;
        }
    }

    pub fn holes_buffer(&self) -> &wgpu::Buffer {
        &self.holes_buffer
    }

    pub fn holes_data(&self) -> &[f32] {
        &self.holes_data
    }

    pub fn holes_gen(&self) -> u64 {
        self.holes_gen
    }

    pub fn set_heightmap(
        &mut self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        heights: &[f32],
        params: WgrTerrainParams,
    ) {
        self.params = params;
        let (w, h) = (params.hm_width, params.hm_height);
        self.medium_source.begin_height(&params,w>0 && h>0 && w<=self.max_dim && h<=self.max_dim
            && heights.len()>=(w as usize*h as usize));
        if w == 0 || h == 0 || w > self.max_dim || h > self.max_dim {
            return;
        }
        if heights.len() < (w as usize * h as usize) {
            return;
        }

        let reuse_height = terrain_resource_reuse() && self.have_heightmap
            && same_texture_size((self.hm_width, self.hm_height), (w, h));
        let (heightmap, view) = if reuse_height {
            (self.heightmap.clone(), self.heightmap_view.clone())
        } else {
            create_heightmap(device, w, h)
        };
        queue.write_texture(
            texel_copy(&heightmap),
            bytemuck::cast_slice(&heights[..(w as usize * h as usize)]),
            wgpu::TexelCopyBufferLayout {
                offset: 0,
                bytes_per_row: Some(w * 4),
                rows_per_image: Some(h),
            },
            wgpu::Extent3d {
                width: w,
                height: h,
                depth_or_array_layers: 1,
            },
        );
        queue.write_buffer(&self.params_ubo, 0, bytemuck::bytes_of(&params));

        // Keep same-size allocations through terrain edits. Queue ordering preserves
        // earlier frames; content generations and shadow recomputation still advance.
        let (mask_w, mask_h) = shadow_mask_dims(w, h, self.shadow_scale, self.max_dim);
        let reuse_shadow = terrain_resource_reuse() && self.have_heightmap
            && same_texture_size((self.mask_width, self.mask_height), (mask_w, mask_h));
        let (shadow_mask, mask_view) = if reuse_shadow {
            (self.shadow_mask.clone(), self.shadow_mask_view.clone())
        } else {
            let mask = create_shadow_mask(device, mask_w, mask_h);
            let view = mask.create_view(&wgpu::TextureViewDescriptor::default());
            (mask, view)
        };
        eprintln!("terrain upload: {w}x{h} height_reused={reuse_height} shadow_reused={reuse_shadow}");
        self.group1_bind = make_group1(
            device,
            &self.group1_layout,
            &self.params_ubo,
            &view,
            &mask_view,
            &self.mask_sampler,
            &self.snow_buffer,
            &self.holes_buffer,
        );
        self.shadow_sweep_bind = make_sweep_bind(
            device,
            &self.shadow_sweep_layout,
            &self.shadow_sweep_ubo,
            &view,
            &mask_view,
        );
        self.shadow_mask = shadow_mask;
        self.shadow_mask_view = mask_view;
        self.heightmap_view = view;
        self.mask_gen += 1;

        // Sky-visibility (sky-view factor): retain the source heightfield and (re)compute the coarse
        // AO grid from it. Sun-independent, so this is the only place it is produced from a new
        // heightmap. Phase A recomputes on every heightmap upload (including fractal subdivision) and
        // runs synchronously; Phase B adds the base-map-only gate + disk cache (plan §3/§4a).
        let previous_skyvis = self.skyvis_src.take();
        self.skyvis_src = Some(SkyvisSrc {
            heights: heights[..(w as usize * h as usize)].to_vec(),
            raw_visibility: Vec::new(),
            w,
            h,
            terrain_grid: params.terrain_grid,
        });
        self.rebuild_skyvis(device, queue, previous_skyvis.as_ref());
        self.world_origin = params.world_origin;
        self.terrain_grid = params.terrain_grid;
        // Tallest terrain point: lets the march stop once the ray climbs above all
        // possible occluders (the auto-adapting range in terrain_shadow.wgsl).
        (self.min_height,self.max_height) = heights[..(w as usize * h as usize)]
            .iter().copied().fold((f32::MAX,f32::MIN),|(lo,hi),h|(lo.min(h),hi.max(h)));
        self.hm_width = w;
        self.hm_height = h;
        self.mask_width = mask_w;
        self.mask_height = mask_h;
        self.shadow_dirty = true;
        self.heightmap = heightmap;
        self.have_heightmap = true;
    }

    // Amortized sun-shadow sweep: ray-march the heightfield toward the sun once
    // per texel into the mask, recomputing only when the heightmap changed or the
    // sun moved past SUN_MOVE_COS. `sun_to_light` is the (unit) surface-to-light
    // direction — the negation of frame.sun_dir_world. Records a compute pass into
    // the frame encoder before the render segments sample the mask.
    pub fn native_height_bounds(&self) -> [f32;2] { [self.min_height,self.max_height] }

    pub fn render_shadow_mask(
        &mut self,
        queue: &wgpu::Queue,
        encoder: &mut wgpu::CommandEncoder,
        sun_to_light: glam::Vec3,
    ) {
        if !self.have_heightmap {
            return;
        }
        let moved = self.last_sun_dir.dot(sun_to_light) < SUN_MOVE_COS;
        if !self.shadow_dirty && !moved {
            return;
        }

        let sweep = ShadowSweep {
            world_origin: self.world_origin,
            terrain_grid: self.terrain_grid,
            penumbra: self.shadow_penumbra,
            inv_scale: glam::Vec2::new(
                self.hm_width as f32 / self.mask_width as f32,
                self.hm_height as f32 / self.mask_height as f32,
            ),
            hm_width: self.hm_width,
            hm_height: self.hm_height,
            mask_width: self.mask_width,
            mask_height: self.mask_height,
            max_steps: self.shadow_max_steps,
            strength: self.shadow_strength_mul,
            sun_dir: sun_to_light.extend(self.max_height),
        };
        queue.write_buffer(&self.shadow_sweep_ubo, 0, bytemuck::bytes_of(&sweep));

        // Marker pushed here (not around the call site) so it only appears on frames
        // the amortized sweep actually records — see the caller in lib.rs.
        encoder.push_debug_group("wgr_terrain_shadow_mask");
        let mut cp = encoder.begin_compute_pass(&wgpu::ComputePassDescriptor {
            label: Some("wgr_terrain_shadow_sweep"),
            timestamp_writes: None,
        });
        cp.set_pipeline(&self.shadow_pipeline);
        cp.set_bind_group(0, &self.shadow_sweep_bind, &[]);
        cp.dispatch_workgroups(self.mask_width.div_ceil(8), self.mask_height.div_ceil(8), 1);
        drop(cp);
        encoder.pop_debug_group();

        self.shadow_dirty = false;
        self.last_sun_dir = sun_to_light;
    }

    // Live-tune the sun-shadow sweep (debug overlay). Strength/steps/penumbra feed
    // the sweep uniform; changing the mask scale reallocates the mask (and the binds
    // that view it). Any change marks the sweep dirty so it recomputes next frame.
    pub fn set_sun_shadow_params(
        &mut self,
        device: &wgpu::Device,
        strength: f32,
        scale: u32,
        max_steps: u32,
        penumbra_deg: f32,
    ) {
        self.shadow_strength_mul = strength.max(0.0);
        self.shadow_max_steps = max_steps.max(1);
        self.shadow_penumbra = penumbra_deg.max(0.0).to_radians();

        let scale = scale.max(1);
        if scale != self.shadow_scale {
            self.shadow_scale = scale;
            if self.have_heightmap {
                let (mw, mh) = shadow_mask_dims(self.hm_width, self.hm_height, scale, self.max_dim);
                let hview = self
                    .heightmap
                    .create_view(&wgpu::TextureViewDescriptor::default());
                let mask = create_shadow_mask(device, mw, mh);
                let mview = mask.create_view(&wgpu::TextureViewDescriptor::default());
                self.group1_bind = make_group1(
                    device,
                    &self.group1_layout,
                    &self.params_ubo,
                    &hview,
                    &mview,
                    &self.mask_sampler,
                    &self.snow_buffer,
                    &self.holes_buffer,
                );
                self.shadow_sweep_bind = make_sweep_bind(
                    device,
                    &self.shadow_sweep_layout,
                    &self.shadow_sweep_ubo,
                    &hview,
                    &mview,
                );
                self.shadow_mask = mask;
                self.shadow_mask_view = mview;
                self.mask_gen += 1;
                self.mask_width = mw;
                self.mask_height = mh;
            }
        }
        self.shadow_dirty = true;
    }

    // Shared-group(0) accessors: the mask view + world->UV mapping + a generation
    // that bumps on realloc, so the camera bind group (which lends this mask to lit
    // meshes) rebuilds only when the texture actually moved.
    pub fn shadow_mask_view(&self) -> wgpu::TextureView {
        self.shadow_mask_view.clone()
    }

    pub fn shadow_gen(&self) -> u64 {
        self.mask_gen
    }

    pub fn shadow_mapping(&self) -> TerrainShadowMap {
        TerrainShadowMap {
            origin: self.world_origin,
            inv_span: glam::Vec2::new(
                1.0 / (self.terrain_grid * self.hm_width as f32),
                1.0 / (self.terrain_grid * self.hm_height as f32),
            ),
            half_texel: glam::Vec2::new(
                0.5 / self.mask_width as f32,
                0.5 / self.mask_height as f32,
            ),
            enabled: if self.have_heightmap { 1.0 } else { 0.0 },
            sky_vis_strength: self.sky_vis_strength,
            sky_vis_floor: self.sky_vis_floor,
            sky_vis_debug: self.sky_vis_debug,
            sky_vis_contrast: self.sky_vis_contrast,
            _pad2: wet_soil_debug_mode(self.params._pad3),
            // Filled by the caller from the Sky pass, which owns the map and its snapping. The
            // terrain has no way to know where the cloud map was placed this frame.
            cloud_shadow: glam::Vec4::new(0.0, 0.0, 1.0, 0.0),
        }
    }

    // Sky-visibility mask view, lent to the shared frame group(0) at binding 10 so all three
    // consumers modulate ambient by it. Reuses mask_gen (recreated in the same set_heightmap call
    // that bumps it) as the rebuild gate.
    pub fn skyvis_view(&self) -> wgpu::TextureView {
        self.skyvis_view.clone()
    }

    // Re-run the sky-view scan from the retained source heightfield with the current options and
    // upload the fresh mask. No-op before any heightmap has loaded. Bumps mask_gen so the shared
    // camera bind group (gfx3d ensure()) rebinds to the NEW skyvis view — without this, a live
    // radius/downsample re-scan produces a texture nothing samples (the bind keeps the old view).
    fn rebuild_skyvis(
        &mut self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        previous: Option<&SkyvisSrc>,
    ) {
        let Some(src) = &mut self.skyvis_src else {
            return;
        };
        let previous = previous
            .filter(|old| old.w == src.w && old.h == src.h && old.terrain_grid == src.terrain_grid);
        let (sv_w, sv_h, raw, sv) = skyvis::compute_cached(
            &src.heights,
            src.w,
            src.h,
            src.terrain_grid,
            self.skyvis_opts,
            previous.map(|old| (old.heights.as_slice(), old.raw_visibility.as_slice())),
        );
        src.raw_visibility = raw;
        let sv_bytes: Vec<u8> = sv
            .iter()
            .map(|v| (v.clamp(0.0, 1.0) * 255.0 + 0.5) as u8)
            .collect();
        if terrain_resource_reuse() && same_texture_size(
            (self.skyvis_mask.width(), self.skyvis_mask.height()), (sv_w, sv_h)) {
            queue.write_texture(
                texel_copy(&self.skyvis_mask),
                &sv_bytes,
                wgpu::TexelCopyBufferLayout {
                    offset: 0, bytes_per_row: Some(sv_w), rows_per_image: Some(sv_h),
                },
                wgpu::Extent3d { width: sv_w, height: sv_h, depth_or_array_layers: 1 },
            );
        } else {
            let (skyvis_mask, skyvis_view) = create_skyvis(device, queue, sv_w, sv_h, &sv_bytes);
            self.skyvis_mask = skyvis_mask;
            self.skyvis_view = skyvis_view;
        }
        self.mask_gen += 1;
    }

    // Live sky-visibility tuning from the ImGui shadow tab (via wgr_terrain_set_sky_visibility).
    // strength/floor/contrast/debug are cheap (uniform values, applied next frame via
    // shadow_mapping()). The scan options (radius/K/downsample) only re-run the CPU scan when they
    // actually change, so idle frames cost nothing; a change recomputes synchronously.
    #[allow(clippy::too_many_arguments)]
    pub fn set_sky_visibility(
        &mut self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        strength: f32,
        contrast: f32,
        floor: f32,
        radius_m: f32,
        k_azimuths: u32,
        downsample: u32,
        debug: bool,
    ) {
        self.sky_vis_strength = strength;
        self.sky_vis_floor = floor;
        self.sky_vis_contrast = contrast.max(0.01);
        self.sky_vis_debug = if debug { 1.0 } else { 0.0 };
        let k = k_azimuths.max(1);
        let ds = downsample.max(1);
        let opts_changed = (self.skyvis_opts.radius_m - radius_m).abs() > 1e-3
            || self.skyvis_opts.k_azimuths != k
            || self.skyvis_opts.downsample != ds;
        if opts_changed {
            self.skyvis_opts.radius_m = radius_m;
            self.skyvis_opts.k_azimuths = k;
            self.skyvis_opts.downsample = ds;
            self.rebuild_skyvis(device, queue, None);
        }
    }

    // Mesh-conform (vegetation) accessors: the heightmap view + its sampling params,
    // lent to the mesh conform bind group so object vertex shaders sample SurfaceY.
    // Reuses mask_gen (heightmap + mask realloc together) as the rebuild gate.
    pub fn heightmap_view(&self) -> wgpu::TextureView {
        self.heightmap_view.clone()
    }

    pub fn heightmap_gen(&self) -> u64 {
        self.mask_gen
    }

    pub fn conform_params(&self) -> TerrainConformParams {
        // WLD-023(c). The CDLOD selector lives on the C++ side (TerrainWgpu::BuildQuadtree ->
        // ComputeCdlodRanges, CdlodMorphBand in World/Terrain/TerrainCdlod.hpp) and its three
        // inputs are process env vars, so the Rust side reads THE SAME VARIABLES rather than
        // inventing a second control. The defaults below are copied from TerrainWgpu.cpp:219-221
        // and must be kept in step with them; a divergence here does not fail, it silently
        // conforms decals to a lattice the terrain does not draw.
        //
        // ranges[0] = leafSize * baseMult and leafSize = TerrainGridN * terrain_grid
        // (TerrainCdlod.hpp BuildCdlodTree), with TerrainGridN == GRID_N == 32 here.
        //
        // Read ONCE: conform_params() runs every frame (lib.rs) and std::env::var takes a
        // process-wide lock on Windows -- the same cost DrawSectionTL already had to cache away
        // (EngineWgpu.cpp:2382-2385). The answer cannot change during a run.
        let (base_mult, ratio, morph, mode) = *CONFORM_CDLOD_ENV.get_or_init(|| {
            let f = |name: &str, fallback: f32| {
                std::env::var(name)
                    .ok()
                    .and_then(|v| v.parse::<f32>().ok())
                    .filter(|v| v.is_finite())
                    .unwrap_or(fallback)
            };
            // Default OFF: turning this on changes where every road/decal vertex sits on every
            // world, and the previous behaviour is the shipped one. WGR_CONFORM_RASTER=1 enables.
            let on = std::env::var("WGR_CONFORM_RASTER")
                .map(|v| !v.is_empty() && v != "0")
                .unwrap_or(false);
            (
                f("WGR_TERRAIN_LOD_BASE", 4.0),
                f("WGR_TERRAIN_LOD_RATIO", 2.0).max(1.001),
                f("WGR_TERRAIN_MORPH", 0.50).clamp(0.05, 1.0),
                if on { 1.0f32 } else { 0.0f32 },
            )
        });
        TerrainConformParams {
            origin: self.world_origin,
            terrain_grid: self.terrain_grid,
            enabled: if self.have_heightmap { 1.0 } else { 0.0 },
            hm_width: self.hm_width,
            hm_height: self.hm_height,
            cdlod_base: GRID_N as f32 * self.terrain_grid * base_mult,
            cdlod_ratio: ratio,
            cdlod_morph: morph,
            cdlod_mode: mode,
            _pad: [0, 0],
        }
    }

    pub fn set_rain_water_medium_source(&mut self,source:crate::ffi::WgrRainWaterSourceKey) {
        self.medium_source.set_source(source);
    }
    pub(crate) fn rain_water_medium_resources(&self)->crate::rain_water_medium::Resources<'_> {
        crate::rain_water_medium::Resources {params:&self.params_ubo,indices:&self.index_map,
            materials:&self.materials,revision:self.medium_source.revision(),control:self.medium_source.control()}
    }

    pub fn params(&self) -> WgrTerrainParams {
        self.params
    }

    pub fn has_heightmap(&self) -> bool {
        self.have_heightmap
    }

    // Ground layers as views into the shared texture registry (missing handles
    // already resolved to the white fallback by the caller). Truncated to the
    // binding_array capacity; the index-map upload clamps cell indices to match.
    pub fn set_ground_layers(&mut self, device: &wgpu::Device, mut views: Vec<wgpu::TextureView>) {
        views.truncate(TERRAIN_MAX_GROUND_LAYERS as usize);
        if views.is_empty() {
            views.push(self.pad_view.clone());
        }
        self.ground_views = views;
        self.rebuild_group2(device);
    }

    pub fn set_index_map(
        &mut self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        width: u32,
        height: u32,
        indices: &[u16],
    ) {
        self.medium_source.replace_index(width,height,width>0 && height>0 && width<=self.max_dim && height<=self.max_dim
            && indices.len()>=(width as usize*height as usize));
        if width == 0 || height == 0 || width > self.max_dim || height > self.max_dim {
            return;
        }
        if indices.len() < width as usize * height as usize {
            return;
        }
        let index_map = create_index_map(device, width, height);
        queue.write_texture(
            texel_copy(&index_map),
            bytemuck::cast_slice(&indices[..width as usize * height as usize]),
            wgpu::TexelCopyBufferLayout {
                offset: 0,
                bytes_per_row: Some(width * 2),
                rows_per_image: Some(height),
            },
            wgpu::Extent3d {
                width,
                height,
                depth_or_array_layers: 1,
            },
        );
        self.index_map = index_map;
        self.rebuild_group2(device);
    }

    pub fn set_materials(
        &mut self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        materials: &[WgrTerrainMaterial],
    ) {
        self.medium_source.replace_materials(!materials.is_empty());
        let upload = if materials.is_empty() {
            &[WgrTerrainMaterial::zeroed()][..]
        } else {
            materials
        };
        self.materials = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
            label: Some("wgr_terrain_materials"),
            contents: bytemuck::cast_slice(upload),
            usage: wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_DST,
        });
        let _ = queue;
        self.rebuild_group2(device);
    }

    pub fn set_jitter_map(
        &mut self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        width: u32,
        height: u32,
        offsets: &[i8],
    ) {
        if width == 0 || height == 0 || width > self.max_dim || height > self.max_dim {
            return;
        }
        if offsets.len() < 2 * width as usize * height as usize {
            return;
        }
        let jitter_map = create_jitter_map(device, width, height);
        queue.write_texture(
            texel_copy(&jitter_map),
            bytemuck::cast_slice(&offsets[..2 * width as usize * height as usize]),
            wgpu::TexelCopyBufferLayout {
                offset: 0,
                bytes_per_row: Some(width * 2),
                rows_per_image: Some(height),
            },
            wgpu::Extent3d {
                width,
                height,
                depth_or_array_layers: 1,
            },
        );
        self.jitter_map = jitter_map;
        self.rebuild_group2(device);
    }

    // Detail noise as a view into the shared texture registry.
    pub fn set_detail_layer(&mut self, device: &wgpu::Device, view: wgpu::TextureView) {
        self.detail_view = view;
        self.rebuild_group2(device);
    }

    fn rebuild_group2(&mut self, device: &wgpu::Device) {
        self.group2_bind = make_group2(
            device,
            &self.group2_layout,
            &self.ground_views,
            &self.pad_view,
            self.partially_bound,
            &self.index_map,
            &self.detail_view,
            &self.jitter_map,
            &self.materials,
            &self.ground_sampler,
            &self.ground_clamp_sampler,
        );
    }

    pub fn prepare(
        &mut self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        nodes: &[WgrTerrainNode],
    ) {
        self.instance_count = nodes.len() as u32;
        if nodes.is_empty() {
            return;
        }
        let needed = std::mem::size_of_val(nodes) as u64;
        if needed > self.instance_cap {
            let cap = needed.next_power_of_two();
            self.instance_buf = device.create_buffer(&wgpu::BufferDescriptor {
                label: Some("wgr_terrain_instances"),
                size: cap,
                usage: wgpu::BufferUsages::VERTEX | wgpu::BufferUsages::COPY_DST,
                mapped_at_creation: false,
            });
            self.instance_cap = cap;
        }
        queue.write_buffer(&self.instance_buf, 0, bytemuck::cast_slice(nodes));
    }

    /// TW-WATER W3b: Tidewater's wet sand for this frame (state, lace, generation, params), or
    /// None for OP's own coast wet band. See shore_wet.rs.
    pub fn set_shore_wetness(
        &mut self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        wet: Option<(&wgpu::TextureView, &wgpu::TextureView, u64, TwWetParams)>,
    ) {
        self.shore_wet.set(device, queue, wet);
    }

    // Draw one batch (a [first_node, first_node+node_count) run of the prepared
    // instances) with the given camera bind group + dynamic offset.
    pub fn draw(
        &self,
        pass: &mut wgpu::RenderPass<'_>,
        camera_bind: &wgpu::BindGroup,
        camera_offset: u32,
        first_node: u32,
        node_count: u32,
        kind: TerrainPass,
    ) {
        if !self.have_heightmap || node_count == 0 {
            return;
        }
        if first_node + node_count > self.instance_count {
            return;
        }
        let pipeline = match kind {
            TerrainPass::Color => &self.pipeline,
            TerrainPass::ColorNoWrite => &self.pipeline_no_write,
            TerrainPass::Prepass => &self.prepass_pipeline,
            TerrainPass::Planar => &self.planar_pipeline,
        };
        pass.set_pipeline(pipeline);
        pass.set_bind_group(0, camera_bind, &[camera_offset]);
        pass.set_bind_group(1, &self.group1_bind, &[]);
        pass.set_bind_group(2, &self.group2_bind, &[]);
        pass.set_bind_group(3, self.shore_wet.bind(), &[]);
        pass.set_vertex_buffer(0, self.grid_vbuf.slice(..));
        pass.set_vertex_buffer(1, self.instance_buf.slice(..));
        pass.set_index_buffer(self.grid_ibuf.slice(..), wgpu::IndexFormat::Uint16);
        pass.draw_indexed(
            0..self.grid_index_count,
            0,
            first_node..first_node + node_count,
        );
    }
}

fn texel_copy(texture: &wgpu::Texture) -> wgpu::TexelCopyTextureInfo<'_> {
    wgpu::TexelCopyTextureInfo {
        texture,
        mip_level: 0,
        origin: wgpu::Origin3d::ZERO,
        aspect: wgpu::TextureAspect::All,
    }
}

fn create_heightmap(device: &wgpu::Device, w: u32, h: u32) -> (wgpu::Texture, wgpu::TextureView) {
    let tex = device.create_texture(&wgpu::TextureDescriptor {
        label: Some("wgr_terrain_heightmap"),
        size: wgpu::Extent3d {
            width: w,
            height: h,
            depth_or_array_layers: 1,
        },
        mip_level_count: 1,
        sample_count: 1,
        dimension: wgpu::TextureDimension::D2,
        format: wgpu::TextureFormat::R32Float,
        usage: wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::COPY_DST,
        view_formats: &[],
    });
    let view = tex.create_view(&wgpu::TextureViewDescriptor::default());
    (tex, view)
}

// Rgba16Float mask on the heightmap grid: storage-written by the sweep (.r =
// shadow-ceiling world height, .g = penumbra half-width in metres, .b = strength),
// sampled with hardware bilinear in fs_terrain. Rgba16Float is the sweet spot —
// core-guaranteed storage-writable *and* filterable (no FLOAT32_FILTERABLE), with
// ample precision to store a world height (~0.5 m at 500 m).
fn create_shadow_mask(device: &wgpu::Device, w: u32, h: u32) -> wgpu::Texture {
    device.create_texture(&wgpu::TextureDescriptor {
        label: Some("wgr_terrain_shadow_mask"),
        size: wgpu::Extent3d {
            width: w,
            height: h,
            depth_or_array_layers: 1,
        },
        mip_level_count: 1,
        sample_count: 1,
        dimension: wgpu::TextureDimension::D2,
        format: wgpu::TextureFormat::Rgba16Float,
        usage: wgpu::TextureUsages::STORAGE_BINDING | wgpu::TextureUsages::TEXTURE_BINDING,
        view_formats: &[],
    })
}

// Coarse sky-visibility mask (R8Unorm, filterable): CPU-computed V in [0,1] per column, uploaded
// once per heightmap. R8Unorm avoids the f32->f16 conversion an R16Float upload would need and is
// ample for a smooth, low-frequency AO factor. `Queue::write_texture` repacks arbitrary row pitches,
// so the 1-byte-per-texel rows need no 256-byte alignment.
fn create_skyvis(
    device: &wgpu::Device,
    queue: &wgpu::Queue,
    w: u32,
    h: u32,
    bytes: &[u8],
) -> (wgpu::Texture, wgpu::TextureView) {
    let tex = device.create_texture(&wgpu::TextureDescriptor {
        label: Some("wgr_terrain_skyvis_mask"),
        size: wgpu::Extent3d {
            width: w,
            height: h,
            depth_or_array_layers: 1,
        },
        mip_level_count: 1,
        sample_count: 1,
        dimension: wgpu::TextureDimension::D2,
        format: wgpu::TextureFormat::R8Unorm,
        usage: wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::COPY_DST,
        view_formats: &[],
    });
    queue.write_texture(
        texel_copy(&tex),
        bytes,
        wgpu::TexelCopyBufferLayout {
            offset: 0,
            bytes_per_row: Some(w),
            rows_per_image: Some(h),
        },
        wgpu::Extent3d {
            width: w,
            height: h,
            depth_or_array_layers: 1,
        },
    );
    let view = tex.create_view(&wgpu::TextureViewDescriptor::default());
    (tex, view)
}

// Parse a float env var, falling back to `default` when unset or unparseable.
fn env_f32(name: &str, default: f32) -> f32 {
    std::env::var(name)
        .ok()
        .and_then(|v| v.parse::<f32>().ok())
        .unwrap_or(default)
}

fn make_group1(
    device: &wgpu::Device,
    layout: &wgpu::BindGroupLayout,
    params: &wgpu::Buffer,
    heightmap_view: &wgpu::TextureView,
    mask_view: &wgpu::TextureView,
    mask_sampler: &wgpu::Sampler,
    snow_buffer: &wgpu::Buffer,
    holes_buffer: &wgpu::Buffer,
) -> wgpu::BindGroup {
    device.create_bind_group(&wgpu::BindGroupDescriptor {
        label: Some("wgr_terrain_group1_bind"),
        layout,
        entries: &[
            wgpu::BindGroupEntry {
                binding: 0,
                resource: params.as_entire_binding(),
            },
            wgpu::BindGroupEntry {
                binding: 1,
                resource: wgpu::BindingResource::TextureView(heightmap_view),
            },
            wgpu::BindGroupEntry {
                binding: 2,
                resource: wgpu::BindingResource::TextureView(mask_view),
            },
            wgpu::BindGroupEntry {
                binding: 3,
                resource: wgpu::BindingResource::Sampler(mask_sampler),
            },
            wgpu::BindGroupEntry {
                binding: 4,
                resource: snow_buffer.as_entire_binding(),
            },
            wgpu::BindGroupEntry {
                binding: 5,
                resource: holes_buffer.as_entire_binding(),
            },
        ],
    })
}

fn make_sweep_bind(
    device: &wgpu::Device,
    layout: &wgpu::BindGroupLayout,
    sweep_ubo: &wgpu::Buffer,
    heightmap_view: &wgpu::TextureView,
    mask_view: &wgpu::TextureView,
) -> wgpu::BindGroup {
    device.create_bind_group(&wgpu::BindGroupDescriptor {
        label: Some("wgr_terrain_shadow_sweep_bind"),
        layout,
        entries: &[
            wgpu::BindGroupEntry {
                binding: 0,
                resource: sweep_ubo.as_entire_binding(),
            },
            wgpu::BindGroupEntry {
                binding: 1,
                resource: wgpu::BindingResource::TextureView(heightmap_view),
            },
            wgpu::BindGroupEntry {
                binding: 2,
                resource: wgpu::BindingResource::TextureView(mask_view),
            },
        ],
    })
}

fn create_index_map(device: &wgpu::Device, w: u32, h: u32) -> wgpu::Texture {
    device.create_texture(&wgpu::TextureDescriptor {
        label: Some("wgr_terrain_index_map"),
        size: wgpu::Extent3d {
            width: w,
            height: h,
            depth_or_array_layers: 1,
        },
        mip_level_count: 1,
        sample_count: 1,
        dimension: wgpu::TextureDimension::D2,
        format: wgpu::TextureFormat::R16Uint,
        usage: wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::COPY_DST,
        view_formats: &[],
    })
}

// Per-grid-point ground UV jitter (Landscape::_random), snorm UV offsets.
fn create_jitter_map(device: &wgpu::Device, w: u32, h: u32) -> wgpu::Texture {
    device.create_texture(&wgpu::TextureDescriptor {
        label: Some("wgr_terrain_jitter_map"),
        size: wgpu::Extent3d {
            width: w,
            height: h,
            depth_or_array_layers: 1,
        },
        mip_level_count: 1,
        sample_count: 1,
        dimension: wgpu::TextureDimension::D2,
        format: wgpu::TextureFormat::Rg8Snorm,
        usage: wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::COPY_DST,
        view_formats: &[],
    })
}

#[allow(clippy::too_many_arguments)]
fn make_group2(
    device: &wgpu::Device,
    layout: &wgpu::BindGroupLayout,
    ground_views: &[wgpu::TextureView],
    pad_view: &wgpu::TextureView,
    partially_bound: bool,
    index_map: &wgpu::Texture,
    detail_view: &wgpu::TextureView,
    jitter_map: &wgpu::Texture,
    materials: &wgpu::Buffer,
    sampler: &wgpu::Sampler,
    clamp_sampler: &wgpu::Sampler,
) -> wgpu::BindGroup {
    // Without PARTIALLY_BOUND_BINDING_ARRAY every declared slot must be bound,
    // so pad the tail; the shader never indexes past the real layer count.
    let mut ground_refs: Vec<&wgpu::TextureView> = ground_views.iter().collect();
    if !partially_bound {
        ground_refs.resize(TERRAIN_MAX_GROUND_LAYERS as usize, pad_view);
    }
    let index_view = index_map.create_view(&wgpu::TextureViewDescriptor::default());
    let jitter_view = jitter_map.create_view(&wgpu::TextureViewDescriptor::default());
    device.create_bind_group(&wgpu::BindGroupDescriptor {
        label: Some("wgr_terrain_group2_bind"),
        layout,
        entries: &[
            wgpu::BindGroupEntry {
                binding: 0,
                resource: wgpu::BindingResource::TextureViewArray(&ground_refs),
            },
            wgpu::BindGroupEntry {
                binding: 1,
                resource: wgpu::BindingResource::Sampler(sampler),
            },
            wgpu::BindGroupEntry {
                binding: 2,
                resource: wgpu::BindingResource::TextureView(&index_view),
            },
            wgpu::BindGroupEntry {
                binding: 3,
                resource: wgpu::BindingResource::TextureView(detail_view),
            },
            wgpu::BindGroupEntry {
                binding: 4,
                resource: wgpu::BindingResource::Sampler(clamp_sampler),
            },
            wgpu::BindGroupEntry {
                binding: 5,
                resource: wgpu::BindingResource::TextureView(&jitter_view),
            },
            wgpu::BindGroupEntry {
                binding: 6,
                resource: materials.as_entire_binding(),
            },
        ],
    })
}

// The reusable unit grid: (GRID_N+1)^2 vertices over [0,1]^2, two triangles per
// quad, plus a border skirt. Vertex is (u, v, skirt); the shader drops skirt
// vertices below the surface to wall off LOD-transition cracks.
fn build_grid(device: &wgpu::Device) -> (wgpu::Buffer, wgpu::Buffer, u32) {
    let side = GRID_N + 1;
    let unit = 1.0 / GRID_N as f32;
    let mut verts: Vec<[f32; 3]> = Vec::with_capacity((side * side) as usize);
    for z in 0..side {
        for x in 0..side {
            verts.push([x as f32 * unit, z as f32 * unit, 0.0]);
        }
    }
    let mut indices: Vec<u16> = Vec::with_capacity((GRID_N * GRID_N * 6) as usize);
    for z in 0..GRID_N {
        for x in 0..GRID_N {
            let i0 = (z * side + x) as u16;
            let i1 = i0 + 1;
            let i2 = i0 + side as u16;
            let i3 = i2 + 1;
            indices.extend_from_slice(&[i0, i2, i1, i1, i2, i3]);
        }
    }

    // One skirt wall per border edge segment: two triangles joining the edge pair
    // to a dropped duplicate. Winding is irrelevant (the pipeline culls nothing).
    let mut wall = |tops: [u16; 2], a: [f32; 2], b: [f32; 2]| {
        let s0 = verts.len() as u16;
        verts.push([a[0], a[1], 1.0]);
        let s1 = verts.len() as u16;
        verts.push([b[0], b[1], 1.0]);
        indices.extend_from_slice(&[tops[0], s0, tops[1], tops[1], s0, s1]);
    };
    for i in 0..GRID_N {
        let f = i as f32 * unit;
        let g = (i + 1) as f32 * unit;
        let b = GRID_N * side;
        // top (z=0) / bottom (z=GRID_N)
        wall([i as u16, (i + 1) as u16], [f, 0.0], [g, 0.0]);
        wall([(b + i) as u16, (b + i + 1) as u16], [f, 1.0], [g, 1.0]);
        // left (x=0) / right (x=GRID_N)
        wall(
            [(i * side) as u16, ((i + 1) * side) as u16],
            [0.0, f],
            [0.0, g],
        );
        wall(
            [(i * side + GRID_N) as u16, ((i + 1) * side + GRID_N) as u16],
            [1.0, f],
            [1.0, g],
        );
    }

    let vbuf = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
        label: Some("wgr_terrain_grid_vbuf"),
        contents: bytemuck::cast_slice(&verts),
        usage: wgpu::BufferUsages::VERTEX,
    });
    let ibuf = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
        label: Some("wgr_terrain_grid_ibuf"),
        contents: bytemuck::cast_slice(&indices),
        usage: wgpu::BufferUsages::INDEX,
    });
    (vbuf, ibuf, indices.len() as u32)
}

#[cfg(test)]
mod enfusion_ground_tests {
    use crate::ffi::{WgrTerrainMaterial, WgrTerrainParams};

    #[test]
    fn native_ground_ao_composes_with_and_without_ablation() {
        use naga_oil::compose::{NagaModuleDescriptor, ShaderDefValue};
        for disabled in [false, true] {
            let mut composer = crate::shaders::build_composer();
            let mut defs = std::collections::HashMap::new();
            if disabled {
                defs.insert("DISABLE_NATIVE_GROUND_AO".to_string(), ShaderDefValue::Bool(true));
            }
            let result = composer.make_naga_module(NagaModuleDescriptor {
                source: include_str!("terrain.wgsl"),
                file_path: "terrain/terrain.wgsl",
                shader_defs: defs,
                ..Default::default()
            });
            if let Err(error) = result {
                panic!("{}", error.emit_to_string(&composer));
            }
        }
        let source = include_str!("terrain.wgsl");
        assert!(source.contains("authored_ao = mix(authored_ao, 1.0, coverage)"));
    }

    #[test]
    fn native_ground_normals_use_raw_rg_and_skip_distant_fetches() {
        let src = include_str!("terrain.wgsl");
        let native = src.split("if (material.enfusion != 0u && tp.enfusion_ground > 0.0) {")
            .nth(1).unwrap().split("if (material.surface_count == 0u").next().unwrap();
        assert!(native.contains("material.tile_normal != 0u && detail_far < 1.0"));
        assert!(native.contains("let xy = packed.rg * 2.0"));
        assert!(!native.contains("srgb_to_linear(packed"));
        assert!(native.contains("vec3<f32>(-xy.x, xy.y,"));
        assert!(native.contains("clamp(tp.enfusion_ground, 0.0, 1.0)"));
        assert!(native.contains("return AuthoredCell(rgb, normal_ts, authored_ao, legacy_mud)"));
        assert!(native.contains("clamp(packed.a, 0.0, 1.0)"));
        assert!(src.contains("* normal_map_cavity(geometric_n, n) * authored_ao"));
        assert!(src.contains("authored_ao = c.ambient_occlusion"));
        assert!(src.contains("w11 * c11.ambient_occlusion"));
    }

    // RFG-065. The Enfusion block is read by OFFSET, not by name: a field added to the
    // WGSL struct in a different order than to the two Rust/C++ ones renders as the
    // wrong ground rather than as an error, which is the failure mode the existing size
    // assert exists to catch and cannot catch on its own. So pin the tail order too.
    #[test]
    fn the_shader_carries_the_enfusion_block_in_the_abi_order() {
        assert!(include_str!("terrain.wgsl").contains("#import terrain_material::{TerrainUv, TerrainMaterial, TERRAIN_SURFACE_SLOTS}"));
        let src = include_str!("../shaders/terrain_material.wgsl");
        let tail = src
            .split("surface_uvs: array<TerrainUv, 6>,")
            .nth(1)
            .expect("terrain.wgsl must declare the surface UV array");
        let fields: Vec<&str> = [
            "enfusion: u32,",
            "detail_scale: f32,",
            "middle: u32,",
            "middle_scale: f32,",
            "middle_blend: f32,",
            "detail_max: f32,",
            "detail_fade: f32,",
            "middle_color: vec3<f32>,",
        ]
        .into_iter()
        .collect();
        let mut at = 0usize;
        for field in fields {
            let found = tail[at..]
                .find(field)
                .unwrap_or_else(|| panic!("terrain.wgsl is missing `{field}` after surface_uvs"));
            at += found + field.len();
        }
        // 336 before the block, 48 after it: 32 of scalars, then MiddleColor on its own
        // 16-byte row (a WGSL vec3 aligns to 16). A skew here is silent on the GPU.
        assert_eq!(std::mem::size_of::<WgrTerrainMaterial>(), 384);
        assert_eq!(std::mem::offset_of!(WgrTerrainMaterial, middle_color), 368);
    }

    // The tint is a multiplier in LINEAR (RFG-071), and the ground blend runs in gamma
    // until the fold at the end of fs_main. If the multiply ever moves out of that
    // round-trip it becomes the "multiply in gamma" reading RFG-071 measured at 0.66x,
    // and Dirt_01's middle tile goes from 97/86/73 to 38/30/21.
    #[test]
    fn the_middle_tint_multiplies_in_linear_space() {
        let src = include_str!("terrain.wgsl");
        assert!(
            src.contains("ground_linear_to_srgb(srgb_to_linear(mid) * max(material.middle_color"),
            "MiddleColor must multiply the middle texel between a decode and a re-encode"
        );
    }

    #[test]
    fn legacy_normals_keep_ag_decode_cell_clamping_and_far_early_out() {
        let src = include_str!("terrain.wgsl");
        let helper = src.split("fn sample_legacy_normal(").nth(1).unwrap()
            .split("fn terrain_uv_gradient(").next().unwrap();
        assert!(helper.contains("vec2<f32>(texel.a, texel.g)"));
        assert!(helper.contains("material.surface_count != 0u"));
        assert!(helper.contains("tile_uv - cell"));
        assert!(helper.contains("material.legacy_detail_normal == 0u"));
        assert!(helper.contains("world_pos.xz * scale, world_ddx.xz * scale, world_ddy.xz * scale"));
        assert!(helper.contains("smoothstep(15.0, 35.0, length(view_pos))"));
        assert!(helper.contains("vec2<f32>(detail.a, detail.g)"));
        assert!(src.contains("material.legacy |= entry & CELL_CLAMPED;"));
        let far = src.split("if (far >= 0.999) {").nth(1).unwrap();
        let exit = far.find("return AuthoredCell(satellite,").unwrap();
        let normal = far.find("sample_legacy_normal(").unwrap();
        assert!(exit < normal, "far satmap must not sample detail normals");
    }

    // The master weight sits in the terrain params, not in a pipeline override, so the
    // dev panel can A/B it live. If it stops being read the switch becomes a no-op that
    // still moves in the UI.
    #[test]
    fn the_master_weight_is_a_per_frame_uniform_the_shader_reads() {
        let src = include_str!("terrain.wgsl");
        assert!(
            src.contains("enfusion_ground: f32,"),
            "TerrainParams must carry the master weight"
        );
        assert!(
            src.contains("tp.enfusion_ground > 0.0"),
            "the Enfusion ground branch must be gated on the per-frame weight"
        );
        assert!(
            src.contains("tp.enfusion_ground >= 0.999"),
            "full on must skip the legacy tap rather than blending against it"
        );
        // Zero-initialised, so no world that never sets it can take the branch.
        assert_eq!(std::mem::size_of::<WgrTerrainParams>(), 88);
    }
}

#[cfg(test)]
mod terrain_hole_tests {
    use naga_oil::compose::NagaModuleDescriptor;

    fn compose(source: &'static str, path: &'static str) -> naga::Module {
        let mut composer = crate::shaders::build_composer();
        let module = composer.make_naga_module(NagaModuleDescriptor {
            source,
            file_path: path,
            ..Default::default()
        });
        let module = match module {
            Ok(m) => m,
            Err(error) => panic!("{}", error.emit_to_string(&composer)),
        };
        naga::valid::Validator::new(naga::valid::ValidationFlags::all(), naga::valid::Capabilities::all())
            .validate(&module)
            .expect("shader validates");
        module
    }

    #[test]
    fn retained_cave_lighting_keeps_soft_ground_storage_out_of_fragments() {
        let module = compose(include_str!("../gfx3d/gpu_driven.wgsl"), "gfx3d/gpu_driven.wgsl");
        let info = naga::valid::Validator::new(
            naga::valid::ValidationFlags::all(), naga::valid::Capabilities::all())
            .validate(&module).expect("actual retained shader validates");
        let mut fragments = 0;
        let mut cave_used = false;
        for (index, entry) in module.entry_points.iter().enumerate() {
            if entry.stage != naga::ShaderStage::Fragment { continue; }
            fragments += 1;
            let usage = info.get_entry_point(index);
            let mut storage_count = 0;
            for (handle, global) in module.global_variables.iter() {
                let used = !usage[handle].is_empty();
                if matches!(global.space, naga::AddressSpace::Storage { .. }) && used {
                    storage_count += 1;
                }
                if let Some(binding) = &global.binding {
                    if binding.group == 4 && binding.binding == 2 {
                        assert!(!used, "soft ground is vertex-only in the retained layout");
                    }
                    if binding.group == 4 && binding.binding == 3 && used {
                        cave_used = true;
                    }
                }
            }
            assert!(storage_count <= 8, "actual fragment needs {storage_count} storage slots");
        }
        assert!(fragments > 0 && cave_used, "must exercise live retained cave lighting");
    }

    // Sinkhole W1: the hole test composes and validates in both shaders that use it.
    #[test]
    fn terrain_and_grass_compose_with_the_hole_test() {
        compose(include_str!("terrain.wgsl"), "terrain/terrain.wgsl");
        compose(include_str!("../grass/grass.wgsl"), "grass/grass.wgsl");
    }

    // The colour and prepass fragments must discard the same pixels, or the colour pass's depth-equal test
    // would draw ground over a hole wherever the prepass wrote depth.
    #[test]
    fn colour_and_prepass_both_discard_inside_holes() {
        let src = include_str!("terrain.wgsl");
        let colour = src.split("fn fs_terrain(").nth(1).unwrap();
        let prepass = src.split("fn fs_terrain_prepass(").nth(1).unwrap();
        assert!(colour[..colour.find("dpdx").unwrap()].contains("in_terrain_hole(in.world_pos + frame.cam_pos.xyz)"));
        assert!(prepass[..400].contains("in_terrain_hole(in.world_pos + frame.cam_pos.xyz)"));
        assert!(src.contains("@group(1) @binding(5) var<storage, read> holes: TerrainHoles;"));
    }

    #[test]
    fn grass_skips_holes_in_every_placement_pass() {
        let src = include_str!("../grass/grass.wgsl");
        assert_eq!(src.matches("if (in_terrain_hole(vec3<f32>(world_xz.x, y, world_xz.y))) { return; }").count(), 3);
        assert!(src.contains("@group(1) @binding(3) var<storage, read> holes: TerrainHoles;"));
    }

    // The underground sun test lives in the shared `shading` module; both object shaders that import it
    // must still compose and validate (they bind the conform heightmap group it reads).
    #[test]
    fn object_shaders_compose_with_the_underground_sun_test() {
        let src = include_str!("../shaders/shading.wgsl");
        assert!(src.contains("let underground_s = 1.0 - cave.x;"));
        assert!(src.contains("let underground_amb = mix(1.0, 0.03, underground_amb_s);"));
        let conform = include_str!("../shaders/conform.wgsl");
        assert!(conform.contains("fn cave_daylight(world: vec3<f32>) -> f32"));
        assert!(conform.contains("fn cave_daylight2(world: vec3<f32>) -> vec2<f32>"));
        assert!(conform.contains("@group(4) @binding(3) var<uniform> cave_openings: CaveOpenings;"));
        assert!(src.contains("* leaf_cavity * mix(mat.native_ao, 1.0, snow_cover) * underground_amb;"));
        compose(include_str!("../gfx3d/shader3d.wgsl"), "gfx3d/shader3d.wgsl");
        compose(include_str!("../gfx3d/gpu_driven.wgsl"), "gfx3d/gpu_driven.wgsl");
    }

    // A collapsed outline (zero-length edges) must never read as "inside everywhere".
    #[test]
    fn a_zero_length_edge_is_outside() {
        for src in [include_str!("terrain.wgsl"), include_str!("../grass/grass.wgsl")] {
            assert!(src.contains("if (dot(e.xy, e.xy) < 0.25 || e.x * xz.x + e.y * xz.y + e.z < 0.0) { inside = false; }"));
        }
    }

    #[test]
    fn the_hole_buffer_matches_the_shader_struct() {
        // vec4 header + 64 vec4 edges
        assert_eq!(super::TERRAIN_HOLES_BYTES, (16 + 64 * 16) as u64);
    }
}

#[cfg(test)]
mod hole_ceiling_tests;
