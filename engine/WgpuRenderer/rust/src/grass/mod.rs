mod blade_atlas;

use crate::ffi::{
    WGR_GRASS_DOWNWASH_COUNT, WGR_GRASS_TRACK_COUNT, WgrGrassDownwash, WgrGrassParams,
    WgrGrassTrack, WgrTerrainParams,
};
use crate::gfx3d::{DEPTH_FORMAT, NORMAL_FORMAT};
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
    _pad_density: f32,
    tint_procedural: [f32; 4],
    tint_photo: [f32; 4],
    // Renderer-derived from the uploaded atlas, never from the caller.
    photo_layer_bounds: [f32; 36],
    photo_layer_means: [f32; 36],
}

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
const STATS_BYTES: u64 = 16; // three u32 counts at offsets 0/4/8, padded

enum StatsSlot {
    Idle,
    Pending,
    InFlight(std::sync::mpsc::Receiver<Result<(), wgpu::BufferAsyncError>>),
}

pub struct Grass {
    enabled: bool,
    casts_shadows: bool,
    far_enabled: bool,
    terrain_params: wgpu::Buffer,
    grass_params: wgpu::Buffer,
    placement_count: wgpu::Buffer,
    indirect: wgpu::Buffer,
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
    tuft_bounds: [f32; 36],
    // Mean covered colour per layer, the contrast pivot.
    tuft_means: [f32; 36],
    // Last pushed params, so set_tuft can re-publish them with the tuft flag set
    // without waiting for the Grass tab to touch a slider.
    last_params: GrassParams,
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
            _pad0: 0.0,
            _pad1: 0.0,
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
            .unwrap_or(0.99)
            .clamp(0.0, 1.0);
        let radius = std::env::var("WGR_GRASS_DISTANCE")
            .ok()
            .and_then(|v| v.parse::<f32>().ok())
            .unwrap_or(50.0)
            .clamp(8.0, 60.0);
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
            saturation: 1.0,
            dry_patches: 0.35,
            dry_patch_scale: 0.030,
            mid_radius: 100.0,
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
            photo_tuft_brightness: 1.25,
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
            mid_density: 0.95,
            far_density: 1.0,
            _pad_density: 0.0,
            tint_procedural: [1.0, 1.0, 1.0, 1.0],
            tint_photo: [1.0, 1.0, 1.0, 1.0],
            photo_layer_bounds: default_layer_bounds(),
            photo_layer_means: default_layer_means(),
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
            ],
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
        let indirect = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("wgr_grass_indirect"),
            size: 16,
            usage: wgpu::BufferUsages::INDIRECT | wgpu::BufferUsages::COPY_DST,
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
            size: 16,
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
        let make_pipeline = |label: &str,
                             vertex_entry: &str,
                             entry: &str,
                             target: wgpu::TextureFormat,
                             depth_write: bool| {
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
                    format: DEPTH_FORMAT,
                    depth_write_enabled: Some(depth_write),
                    depth_compare: Some(wgpu::CompareFunction::GreaterEqual),
                    stencil: Default::default(),
                    bias: Default::default(),
                }),
                multisample: wgpu::MultisampleState {
                    count: sample_count,
                    ..Default::default()
                },
                fragment: Some(wgpu::FragmentState {
                    module: &shader,
                    entry_point: Some(entry),
                    compilation_options: Default::default(),
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
        let color_pipeline = make_pipeline(
            "wgr_grass_color",
            "vs_grass",
            "fs_grass",
            surface_format,
            true,
        );
        let color_no_write_pipeline = make_pipeline(
            "wgr_grass_color_no_write",
            "vs_grass",
            "fs_grass",
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
            "fs_grass_prepass",
            NORMAL_FORMAT,
            true,
        );
        let mid_prepass_pipeline = make_pipeline(
            "wgr_grass_mid_prepass",
            "vs_grass_mid",
            "fs_grass_prepass",
            NORMAL_FORMAT,
            true,
        );
        let mid_color_pipeline = make_pipeline(
            "wgr_grass_mid_color",
            "vs_grass_mid",
            "fs_grass",
            surface_format,
            true,
        );
        let mid_color_no_write_pipeline = make_pipeline(
            "wgr_grass_mid_color_no_write",
            "vs_grass_mid",
            "fs_grass",
            surface_format,
            false,
        );
        let mid_tuft_prepass_pipeline = make_pipeline(
            "wgr_grass_mid_tuft_prepass",
            "vs_grass_mid_tuft",
            "fs_grass_mid_tuft_prepass",
            NORMAL_FORMAT,
            true,
        );
        let mid_tuft_color_pipeline = make_pipeline(
            "wgr_grass_mid_tuft_color",
            "vs_grass_mid_tuft",
            "fs_grass_mid_tuft",
            surface_format,
            true,
        );
        let mid_tuft_color_no_write_pipeline = make_pipeline(
            "wgr_grass_mid_tuft_color_no_write",
            "vs_grass_mid_tuft",
            "fs_grass_mid_tuft",
            surface_format,
            false,
        );
        let near_tuft_prepass_pipeline = make_pipeline(
            "wgr_grass_near_tuft_prepass",
            "vs_grass_mid_tuft",
            "fs_grass_mid_tuft_prepass",
            NORMAL_FORMAT,
            true,
        );
        let near_tuft_color_pipeline = make_pipeline(
            "wgr_grass_near_tuft_color",
            "vs_grass_mid_tuft",
            "fs_grass_mid_tuft",
            surface_format,
            true,
        );
        let near_tuft_color_no_write_pipeline = make_pipeline(
            "wgr_grass_near_tuft_color_no_write",
            "vs_grass_mid_tuft",
            "fs_grass_mid_tuft",
            surface_format,
            false,
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
            fragment: None,
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
            casts_shadows: true,
            // SetGrassSettings publishes GrassSettings' 800 m far ring on the
            // first frame; start disabled only until that initial upload.
            far_enabled: false,
            terrain_params,
            grass_params,
            placement_count,
            indirect,
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
            have_tuft: false,
            tuft_enabled: false,
            near_grid_dim: 256,
            mid_grid_dim: 512,
            tuft_bounds: default_layer_bounds(),
            tuft_means: default_layer_means(),
            last_params: params,
        }
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
        queue.write_buffer(&self.grass_params, 0, bytemuck::bytes_of(&self.last_params));
        // Uploads normally arrive after the game has pushed GrassSettings. Keep
        // that opt-in live immediately instead of requiring a UI slider nudge.
        self.tuft_enabled = self.last_params.use_photo_tuft != 0.0;
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
        );
    }

    pub fn set_params(&mut self, queue: &wgpu::Queue, params: WgrGrassParams) {
        // Spacing and radius are now independent. The lower spacing bound is the
        // real density ceiling ("boost the coverage far more"): 0.10 m limited a
        // clump grid to 0.32 m centres no matter what the Grass tab asked for.
        let spacing = params.spacing.clamp(0.02, 0.75);
        let near_radius = params.near_radius.clamp(8.0, 200.0);
        // Must mirror cs_place's `near_spacing`.
        let near_spacing = spacing
            * if params.clump_renderer != 0.0 {
                3.20
            } else {
                1.72
            };
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
        // The mid grid needs no equivalent cap: MID_GRID_MAX * MID_PLACEMENT_SPACING
        // / 2 is 1000.4 m against a MID_RING_MAX of 1000, so it always spans its
        // disc. Adding a cap here anyway would diverge from mid_ring_end(), which
        // recomputes the reach in the shader.
        let mid_radius = params.mid_radius.clamp(8.0, MID_RING_MAX);
        let mid_natural = (near_radius + 10.0).max((near_radius * 2.5).min(MID_RING_MAX));
        let mid_end = if mid_radius > near_radius {
            mid_radius
        } else {
            mid_natural
        }
        .clamp(near_radius + 10.0, MID_RING_MAX);
        // MID_PLACEMENT_SPACING in grass.wgsl.
        let want_mid = (2.0 * mid_end / 0.82).ceil().max(0.0) as u32;
        let mid_grid_dim = want_mid
            .div_ceil(8)
            .saturating_mul(8)
            .clamp(MID_GRID_MIN, MID_GRID_MAX);
        let params = GrassParams {
            density: params.density.clamp(0.0, 1.0),
            spacing,
            near_radius,
            enabled: if params.enabled != 0.0 { 1.0 } else { 0.0 },
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
            use_photo_tuft: params.use_photo_tuft,
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
            // Anything outside 0..8 means "no override"; the shader tests < -0.5.
            photo_force_layer: if (0.0..=8.0).contains(&params.photo_force_layer) {
                params.photo_force_layer.round()
            } else {
                -1.0
            },
            photo_saturation: params.photo_saturation.clamp(0.0, 2.0),
            track_lifetime: params.track_lifetime.clamp(5.0, 3600.0),
            imprint_depth: params.imprint_depth.clamp(0.0, 0.95),
            photo_layer_weights: params.photo_layer_weights.map(|w| w.clamp(0.0, 1.0)),
            near_density: params.near_density.clamp(0.0, 1.0),
            mid_density: params.mid_density.clamp(0.0, 1.0),
            far_density: params.far_density.clamp(0.0, 1.0),
            _pad_density: 0.0,
            tint_procedural: clamp_tint(params.tint_procedural),
            tint_photo: clamp_tint(params.tint_photo),
            // Always the renderer's own measurement of the loaded atlas.
            photo_layer_bounds: self.tuft_bounds,
            photo_layer_means: self.tuft_means,
        };
        self.near_grid_dim = near_grid_dim;
        self.mid_grid_dim = mid_grid_dim;
        self.last_params = params;
        self.enabled = params.enabled != 0.0;
        self.casts_shadows = params.cast_shadows != 0.0;
        // A zero far radius means the outer ring is off: skip its whole dispatch
        // rather than running 147k candidate threads that all reject.
        self.far_enabled = params.far_radius > 1.0;
        // The photo cards need BOTH the texture and the dev-tool opt-in.
        self.tuft_enabled = self.have_tuft && params.use_photo_tuft != 0.0;
        queue.write_buffer(&self.grass_params, 0, bytemuck::bytes_of(&params));
    }

    pub fn prepare_terrain(
        &mut self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        terrain: &Terrain,
    ) {
        let params = terrain.params();
        queue.write_buffer(&self.terrain_params, 0, bytemuck::bytes_of(&params));
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
        encoder.copy_buffer_to_buffer(&self.mid_placement_count, 0, &self.mid_indirect, 4, 4);
        encoder.copy_buffer_to_buffer(&self.far_placement_count, 0, &self.far_indirect, 4, 4);
        // Same three counters into a mappable slot for the Grass tab's counts.
        if let Some((buf, state)) = self
            .stats_buffers
            .iter_mut()
            .find(|(_, s)| matches!(s, StatsSlot::Idle))
        {
            encoder.copy_buffer_to_buffer(&self.placement_count, 0, buf, 0, 4);
            encoder.copy_buffer_to_buffer(&self.mid_placement_count, 0, buf, 4, 4);
            encoder.copy_buffer_to_buffer(&self.far_placement_count, 0, buf, 8, 4);
            *state = StatsSlot::Pending;
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
        let photo_near = self.tuft_enabled && self.last_params.clump_renderer != 0.0;
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
        queue.write_buffer(&self.placement_count, 0, bytemuck::bytes_of(&0u32));
        queue.write_buffer(&self.mid_placement_count, 0, bytemuck::bytes_of(&0u32));
        queue.write_buffer(&self.far_placement_count, 0, bytemuck::bytes_of(&0u32));
        // GRS-030 doubles blades per clump and spaces centres farther apart.
        let near_verts: u32 = if self.tuft_enabled && self.last_params.clump_renderer != 0.0 {
            12
        } else if self.last_params.clump_renderer != 0.0 {
            360
        } else {
            180
        };
        queue.write_buffer(
            &self.indirect,
            0,
            bytemuck::cast_slice(&[near_verts, 0, 0, 0]),
        );
        // Mid LOD is either six opaque three-segment ribbons per clump, or two
        // crossed, alpha-tested photo cards (2 * 6 vertices) when a compatible
        // tuft has been explicitly enabled.
        let mid_verts: u32 = if self.tuft_enabled { 12 } else { 108 };
        queue.write_buffer(
            &self.mid_indirect,
            0,
            bytemuck::cast_slice(&[mid_verts, 0, 0, 0]),
        );
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
        pass.draw_indirect(&self.mid_indirect, 0);
        let near_pipeline = match (
            kind,
            self.tuft_enabled && self.last_params.clump_renderer != 0.0,
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
        pass.draw_indirect(&self.indirect, 0);
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
        timers.begin_pass(pass, Region::GrassShadow);
        let near_shadow_pipeline = if self.tuft_enabled && self.last_params.clump_renderer != 0.0 {
            &self.tuft_shadow_pipeline
        } else {
            &self.shadow_pipeline
        };
        pass.set_pipeline(near_shadow_pipeline);
        pass.set_bind_group(0, shadow_bind, &[shadow_offset]);
        pass.set_bind_group(1, &self.terrain_bind, &[]);
        pass.set_bind_group(2, &self.data_bind, &[]);
        pass.draw_indirect(&self.indirect, 0);
        if self.tuft_enabled {
            pass.set_pipeline(&self.tuft_shadow_pipeline);
            pass.set_bind_group(2, &self.mid_data_bind, &[]);
            pass.draw_indirect(&self.mid_indirect, 0);
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
fn default_layer_means() -> [f32; 36] {
    let mut out = [0.0f32; 36];
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
fn default_layer_bounds() -> [f32; 36] {
    let mut out = [0.0f32; 36];
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
        ],
    })
}
