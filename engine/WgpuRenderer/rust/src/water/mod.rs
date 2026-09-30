use wgpu::util::DeviceExt;

use crate::ffi::{WgrWaterNode, WgrWaterParams};
use crate::gfx3d::depth_format;
mod interaction;
use interaction::Interaction;
mod fft;
pub use fft::FFT_RESOLUTION;
use fft::Fft;
mod foam;
#[cfg(test)]
mod scale_tests;
#[cfg(test)]
mod reference_tests;
#[cfg(test)]
mod foam_cadence_tests;
use crate::ffi::{WgrWaterInteractionEvent, WgrWaterInteractionParams};
use foam::Foam;

/// `WGR_WATER_SIM_ALWAYS=1` keeps the FFT / ripple / foam compute running on frames with no
/// water surface in the draw stream. Off by default (the sim is skipped); this exists so the
/// A/B for that skip is one binary and one env var, per the WGR_ABLATE rationale in lib.rs.
/// Read once and cached — `update_interactions` is a per-frame path.
fn water_sim_always() -> bool {
    static ALWAYS: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *ALWAYS.get_or_init(|| {
        std::env::var("WGR_WATER_SIM_ALWAYS")
            .map(|v| v != "0")
            .unwrap_or(false)
    })
}

// Grid mesh resolution: GRID_N quads per axis, (GRID_N+1)^2 vertices, u16 indices.
// This deliberately exceeds the CDLOD leaf span used by WaterWgpu, because FFT waves
// are geometric displacement: the original 32x32 mesh faceted crests into low-poly
// pyramids even though the spectrum texture itself was smooth.
//
// MUST match GRID_N in water.wgsl. The vertex shader derives the CDLOD morph target
// from it (grid_coarse = round(grid*GRID_N*0.5)*2/GRID_N); a mismatch morphs toward
// the wrong lattice and cracks every LOD boundary.
//
// Why 96 and not 192 (measured 2026-07-28, Everon, 800x600):
//   leafSize 200 m, ranges[0] 1600 m, ratio 2 -> a lod-L node is 200*2^L m wide and
//   lives out to 1600*2^L m. Node size and distance double together, so EVERY LOD
//   band bottoms out at the same projected quad size -- 0.39 px at 192. That is the
//   reason a per-LOD index-buffer ladder buys nothing here: the bands are already
//   equal in screen space, and striding the far ones would only make distant water
//   coarser than near water.
//   The real budget comes from the shader: compute_cascade_weights zeroes a cascade's
//   geometry_weight below ~1.5-4 projected px, and cascades shorter than 20 m carry no
//   displacement at all. At 192 the shortest surviving wavelength was sampled by ~10-19
//   vertices; 96 still gives ~5-10, well above the ~4 needed for a smooth crest, and
//   quarters the triangle count (186 nodes x 73,728 -> x 18,432).
//   Sub-pixel triangles also shade a full 2x2 fragment quad each, so this cuts the
//   water draw's fragment work as well as its vertex work.
const GRID_N: u32 = 96;
// Matches the GodotOceanWaves reference emitter.  These are procedural GPU instances
// rather than CPU-owned particles: only crests that pass the FFT breaking test reach
// the fragment stage, so the cost scales with visible whitewater rather than a CPU
// particle list.
// Matches the 128x128 world-anchored emitter in whitewater_render.wgsl. The old 45x45 field
// covered a 10 m box around the camera, so breaking waves further out shed no spray at all.
// Instances whose source is not breaking collapse to alpha 0 and are discarded in the fragment
// stage, so the cost still scales with visible whitewater rather than with this count.
const WHITEWATER_PARTICLE_COUNT: u32 = 16_384;

// A flat GPU CDLOD water surface: the shared grid mesh instanced per selected node,
// placed on a horizontal plane at the frame's sea level, drawn after opaque terrain +
// 3D and depth-cut by coastlines. Deliberately trimmed vs. Terrain — no heightmap,
// ground array, index/jitter maps or shadow sweep; water needs none of them here.
pub struct Water {
    params_ubo: wgpu::Buffer,
    group1_layout: wgpu::BindGroupLayout,
    // Holds group1 = { params UBO, scene depth, sky env map, env sampler }. The current depth +
    // env views (and the sampler) are retained so either setter can rebuild the combined bind
    // group without the other's view going stale; seeded with 1x1 dummies.
    group1_bind: wgpu::BindGroup,
    depth_view: wgpu::TextureView,
    env_view: wgpu::TextureView,
    env_sampler: wgpu::Sampler,
    scene_view: wgpu::TextureView,
    scene_sampler: wgpu::Sampler,
    planar_view: wgpu::TextureView,
    planar_sampler: wgpu::Sampler,
    planar_params: wgpu::Buffer,
    planar_gen: u64,
    // Terrain heightmap + its world->texel mapping, for the vertex-stage seabed clamp.
    heightmap_view: wgpu::TextureView,
    conform_params: wgpu::Buffer,
    heightmap_gen: u64,
    interaction: Interaction,
    fft: Option<Fft>,
    fft_storage_supported: bool,
    foam: Option<Foam>,
    fft_fallback_view: wgpu::TextureView,
    fft_sampler: wgpu::Sampler,
    foam_fallback_view: wgpu::TextureView,
    foam_sampler: wgpu::Sampler,
    // Generations the current group1_bind was built against (u64::MAX = still the dummy).
    depth_gen: u64,
    env_gen: u64,
    scene_gen: u64,
    grid_vbuf: wgpu::Buffer,
    grid_ibuf: wgpu::Buffer,
    grid_index_count: u32,
    instance_buf: wgpu::Buffer,
    instance_cap: u64,
    instance_count: u32,
    pipeline: wgpu::RenderPipeline,
    whitewater_pipeline: wgpu::RenderPipeline,
    curling_pipeline: Option<wgpu::RenderPipeline>,
    curling_layout: Option<wgpu::BindGroupLayout>,
    curling_bind: Option<wgpu::BindGroup>,
    // Set once wgr_water_set_params has run (i.e. a map is loaded); until then there
    // is nothing sensible to draw.
    have_params: bool,
    // Retained for the scene-wide underwater compositor; the water UBO remains the
    // sole source of sea level supplied by WaterWgpu.
    last_params: Option<WgrWaterParams>,
}

impl Water {
    pub fn new(
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        camera_layout: &wgpu::BindGroupLayout,
        surface_format: wgpu::TextureFormat,
        sample_count: u32,
        composer: &mut naga_oil::compose::Composer,
        fft_storage_supported: bool,
    ) -> Water {
        let group1_layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("wgr_water_group1_layout"),
            entries: &[
                wgpu::BindGroupLayoutEntry {
                    binding: 0,
                    visibility: wgpu::ShaderStages::VERTEX_FRAGMENT,
                    ty: wgpu::BindingType::Buffer {
                        ty: wgpu::BufferBindingType::Uniform,
                        has_dynamic_offset: false,
                        min_binding_size: None,
                    },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 4,
                    visibility: wgpu::ShaderStages::VERTEX_FRAGMENT,
                    ty: wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Float { filterable: true },
                        view_dimension: wgpu::TextureViewDimension::D2,
                        multisampled: false,
                    },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 5,
                    visibility: wgpu::ShaderStages::VERTEX_FRAGMENT,
                    ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Filtering),
                    count: None,
                },
                // Opaque scene depth (prepass), single-sample: the 1x depth aspect or the MSAA
                // resolved Depth32Float. textureLoad'd (no sampler) to reconstruct the seabed for
                // depth-based colour + the soft shoreline. One entry serves both formats.
                wgpu::BindGroupLayoutEntry {
                    binding: 1,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Depth,
                        view_dimension: wgpu::TextureViewDimension::D2,
                        multisampled: false,
                    },
                    count: None,
                },
                // Sky reflection environment map (equirect, Rgba16Float linear radiance) + its
                // sampler, for the Stage-4a real sky reflection. Sampled in the reflected view
                // direction. A 1x1 dummy seeds it until Sky's env view is bound each frame.
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
                    binding: 6,
                    visibility: wgpu::ShaderStages::VERTEX_FRAGMENT,
                    ty: wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Float { filterable: true },
                        view_dimension: wgpu::TextureViewDimension::D2Array,
                        multisampled: false,
                    },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 7,
                    visibility: wgpu::ShaderStages::VERTEX_FRAGMENT,
                    ty: wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Float { filterable: true },
                        view_dimension: wgpu::TextureViewDimension::D2Array,
                        multisampled: false,
                    },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 8,
                    visibility: wgpu::ShaderStages::VERTEX_FRAGMENT,
                    ty: wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Float { filterable: true },
                        view_dimension: wgpu::TextureViewDimension::D2Array,
                        multisampled: false,
                    },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 9,
                    visibility: wgpu::ShaderStages::VERTEX_FRAGMENT,
                    ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Filtering),
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 10,
                    visibility: wgpu::ShaderStages::VERTEX_FRAGMENT,
                    ty: wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Float { filterable: true },
                        view_dimension: wgpu::TextureViewDimension::D2,
                        multisampled: false,
                    },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 11,
                    visibility: wgpu::ShaderStages::VERTEX_FRAGMENT,
                    ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Filtering),
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 12,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Float { filterable: true },
                        view_dimension: wgpu::TextureViewDimension::D2,
                        multisampled: false,
                    },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 13,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Filtering),
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 14,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Float { filterable: true },
                        view_dimension: wgpu::TextureViewDimension::D2,
                        multisampled: false,
                    },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 15,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Filtering),
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 16,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Buffer {
                        ty: wgpu::BufferBindingType::Uniform,
                        has_dynamic_offset: false,
                        min_binding_size: wgpu::BufferSize::new(80),
                    },
                    count: None,
                },
                // The terrain heightmap, sampled in the VERTEX stage so the displaced surface
                // can be clamped to stay above the seabed. Without this the vertex shader has
                // no idea where the ground is — `seabed_depth()` reconstructs it from the depth
                // buffer, which only exists per fragment, far too late to move a vertex. A wave
                // trough could therefore sink under a shallow beach, where the depth test
                // (correctly) hid it and tore a moving hole in the shoreline.
                wgpu::BindGroupLayoutEntry {
                    binding: 17,
                    visibility: wgpu::ShaderStages::VERTEX_FRAGMENT,
                    ty: wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Float { filterable: false },
                        view_dimension: wgpu::TextureViewDimension::D2,
                        multisampled: false,
                    },
                    count: None,
                },
                // Its world->texel mapping. WgrWaterParams already carries world_origin /
                // terrain_grid / hm_width / hm_height, but those are filled independently by
                // WaterWgpu; binding the terrain's own conform params next to the terrain's own
                // texture means the sampling cannot silently disagree with the texture it reads.
                wgpu::BindGroupLayoutEntry {
                    binding: 18,
                    visibility: wgpu::ShaderStages::VERTEX_FRAGMENT,
                    ty: wgpu::BindingType::Buffer {
                        ty: wgpu::BufferBindingType::Uniform,
                        has_dynamic_offset: false,
                        min_binding_size: wgpu::BufferSize::new(std::mem::size_of::<
                            crate::terrain::TerrainConformParams,
                        >() as u64),
                    },
                    count: None,
                },
            ],
        });

        let params_ubo = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("wgr_water_params"),
            size: std::mem::size_of::<WgrWaterParams>() as u64,
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        // Seeded once; the C++ side pushes real look values every frame (Water tab).
        let default_params = WgrWaterParams {
            world_origin: crate::ffi::WgrVec2 { x: 0.0, y: 0.0 },
            terrain_grid: 1.0,
            sea_level: 0.0,
            hm_width: 1,
            hm_height: 1,
            time: 0.0,
            wave_amp: 1.0,
            wave_choppy: 0.5,
            wave_speed: 1.0,
            wave_scale: 1.0,
            fade_start: 200.0,
            fade_end: 2500.0,
            warp_amp: 3.0,
            spec_power: 240.0,
            spec_intensity: 14.0,
            alpha: 0.9,
            shadow_dim: 0.5,
            color_ext: 0.35,
            coast_fade: 0.6,
            shallow_color: [0.10, 0.28, 0.32, 0.0],
            deep_color: [0.004, 0.030, 0.055, 0.0],
            foam_width: 0.4,
            foam_intensity: 1.0,
            swash_amp: 0.15,
            swash_speed: 0.15,
            fft_control: [1.0, 1337.0, 12.0, 0.0],
            fft_wind_sea: [0.82, 0.57, 11.0, 0.55],
            fft_cascade_lengths: [48.0, 144.0, 432.0, 1296.0],
            flow_direction_speed: [0.0, 0.0, 0.0, 0.0],
            // x = debug view (0 = off), y = spray gate, z = spray activity, w = viewport
            // height in pixels (1080 fallback; the C++ side pushes the real height each frame).
            debug_params: [0.0, 0.0, 0.0, 1080.0],
            // WTR-LOOK — x = energy model (1 = physical composite), y/z/w = glitter / SSS /
            // reflection gains. The C++ side pushes the Water tab's values each frame.
            look_params: [1.0, 1.0, 1.0, 1.0],
            // WTR-LOOK — x = physical sea-state coupling on, y = residual spectrum amplitude
            // (1.0 because the coupling carries the energy), z = low quality off, w = shore gain.
            sea_params: [1.0, 1.0, 0.0, 1.0],
            // Engage band, density, colour bias, caustic gain — the tuned defaults.
            underwater_params: [1.5, 1.0, 1.0, 1.0],
            // Off until the Water tab says otherwise, matching WaterSettings' default.
            underwater_gate: [0.0, 0.0, 0.0, 0.0],
            // WRL-002 shared optics on; the C++ side pushes the Water tab's value each frame.
            optics_params: [1.0, 1.0, 0.0, 0.0],
            bodies: [[0.0; 4]; crate::ffi::WGR_WATER_MAX_BODIES * 3],
            water_backend: 1,
            water_backend_pad: [0; 3],
            tidewater: crate::water_tw::DEFAULT_PARAMS,
            tidewater_shore: crate::water_tw::DEFAULT_SHORE_PARAMS,
            tidewater_sea: [0.0, 0.0, 0.5, 0.0],
            tidewater_fx: crate::water_tw::DEFAULT_FX_PARAMS,
            tidewater_wake: [[0.0; 4]; 16],
        };
        queue.write_buffer(&params_ubo, 0, bytemuck::bytes_of(&default_params));

        // 1x1 single-sample depth stand-in so group1 is valid before the first ensure_depth.
        let dummy_depth = device
            .create_texture(&wgpu::TextureDescriptor {
                label: Some("wgr_water_dummy_depth"),
                size: wgpu::Extent3d {
                    width: 1,
                    height: 1,
                    depth_or_array_layers: 1,
                },
                mip_level_count: 1,
                sample_count: 1,
                dimension: wgpu::TextureDimension::D2,
                format: wgpu::TextureFormat::Depth32Float,
                usage: wgpu::TextureUsages::TEXTURE_BINDING,
                view_formats: &[],
            })
            .create_view(&wgpu::TextureViewDescriptor::default());

        // 1x1 dummy env map + its sampler so group1 is valid before Sky's env view is bound. The
        // sampler wraps in U (equirect azimuth seam) and clamps V (poles). Linear filter.
        let dummy_env = device
            .create_texture(&wgpu::TextureDescriptor {
                label: Some("wgr_water_dummy_env"),
                size: wgpu::Extent3d {
                    width: 1,
                    height: 1,
                    depth_or_array_layers: 1,
                },
                mip_level_count: 1,
                sample_count: 1,
                dimension: wgpu::TextureDimension::D2,
                format: wgpu::TextureFormat::Rgba16Float,
                usage: wgpu::TextureUsages::TEXTURE_BINDING,
                view_formats: &[],
            })
            .create_view(&wgpu::TextureViewDescriptor::default());
        let env_sampler = device.create_sampler(&wgpu::SamplerDescriptor {
            label: Some("wgr_water_env_sampler"),
            address_mode_u: wgpu::AddressMode::Repeat,
            address_mode_v: wgpu::AddressMode::ClampToEdge,
            address_mode_w: wgpu::AddressMode::ClampToEdge,
            mag_filter: wgpu::FilterMode::Linear,
            min_filter: wgpu::FilterMode::Linear,
            mipmap_filter: wgpu::MipmapFilterMode::Linear,
            ..Default::default()
        });
        let scene_view = device
            .create_texture(&wgpu::TextureDescriptor {
                label: Some("wgr_water_dummy_scene"),
                size: wgpu::Extent3d {
                    width: 1,
                    height: 1,
                    depth_or_array_layers: 1,
                },
                mip_level_count: 1,
                sample_count: 1,
                dimension: wgpu::TextureDimension::D2,
                format: wgpu::TextureFormat::Rgba16Float,
                usage: wgpu::TextureUsages::TEXTURE_BINDING,
                view_formats: &[],
            })
            .create_view(&Default::default());
        let scene_sampler = device.create_sampler(&wgpu::SamplerDescriptor {
            label: Some("wgr_water_scene_sampler"),
            address_mode_u: wgpu::AddressMode::ClampToEdge,
            address_mode_v: wgpu::AddressMode::ClampToEdge,
            mag_filter: wgpu::FilterMode::Linear,
            min_filter: wgpu::FilterMode::Linear,
            ..Default::default()
        });
        // 1x1 stand-in heightmap so the bind is valid before a world loads. Its conform
        // params default to enabled = 0, which makes the seabed clamp a no-op — open ocean
        // with no terrain behaves exactly as before.
        let heightmap_view = device
            .create_texture(&wgpu::TextureDescriptor {
                label: Some("wgr_water_dummy_heightmap"),
                size: wgpu::Extent3d {
                    width: 1,
                    height: 1,
                    depth_or_array_layers: 1,
                },
                mip_level_count: 1,
                sample_count: 1,
                dimension: wgpu::TextureDimension::D2,
                format: wgpu::TextureFormat::R32Float,
                usage: wgpu::TextureUsages::TEXTURE_BINDING,
                view_formats: &[],
            })
            .create_view(&Default::default());
        let conform_params = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("wgr_water_conform_params"),
            size: std::mem::size_of::<crate::terrain::TerrainConformParams>() as u64,
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        queue.write_buffer(
            &conform_params,
            0,
            bytemuck::bytes_of(
                &<crate::terrain::TerrainConformParams as bytemuck::Zeroable>::zeroed(),
            ),
        );
        let planar_view = device
            .create_texture(&wgpu::TextureDescriptor {
                label: Some("wgr_water_dummy_planar"),
                size: wgpu::Extent3d {
                    width: 1,
                    height: 1,
                    depth_or_array_layers: 1,
                },
                mip_level_count: 1,
                sample_count: 1,
                dimension: wgpu::TextureDimension::D2,
                format: wgpu::TextureFormat::Rgba16Float,
                usage: wgpu::TextureUsages::TEXTURE_BINDING,
                view_formats: &[],
            })
            .create_view(&Default::default());
        let planar_sampler = device.create_sampler(&wgpu::SamplerDescriptor {
            label: Some("wgr_water_planar_sampler"),
            address_mode_u: wgpu::AddressMode::ClampToEdge,
            address_mode_v: wgpu::AddressMode::ClampToEdge,
            mag_filter: wgpu::FilterMode::Linear,
            min_filter: wgpu::FilterMode::Linear,
            mipmap_filter: wgpu::MipmapFilterMode::Linear,
            ..Default::default()
        });
        let planar_params = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
            label: Some("wgr_water_planar_params"),
            contents: bytemuck::cast_slice(&[0.0f32; 20]),
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
        });
        let interaction = Interaction::new(device, composer);
        let fft_fallback_view = device
            .create_texture(&wgpu::TextureDescriptor {
                label: Some("wgr_water_fft_fallback"),
                size: wgpu::Extent3d {
                    width: 1,
                    height: 1,
                    depth_or_array_layers: 4,
                },
                mip_level_count: 1,
                sample_count: 1,
                dimension: wgpu::TextureDimension::D2,
                format: wgpu::TextureFormat::Rgba16Float,
                usage: wgpu::TextureUsages::TEXTURE_BINDING,
                view_formats: &[],
            })
            .create_view(&wgpu::TextureViewDescriptor {
                dimension: Some(wgpu::TextureViewDimension::D2Array),
                ..Default::default()
            });
        let fft_sampler = device.create_sampler(&wgpu::SamplerDescriptor {
            label: Some("wgr_water_fft_sampler"),
            address_mode_u: wgpu::AddressMode::Repeat,
            address_mode_v: wgpu::AddressMode::Repeat,
            mag_filter: wgpu::FilterMode::Linear,
            min_filter: wgpu::FilterMode::Linear,
            ..Default::default()
        });
        let fft = Fft::new(
            device,
            queue,
            &params_ubo,
            fft_storage_supported,
            FFT_RESOLUTION,
        );
        let foam_fallback_view = device
            .create_texture(&wgpu::TextureDescriptor {
                label: Some("wgr_water_foam_fallback"),
                size: wgpu::Extent3d {
                    width: 1,
                    height: 1,
                    depth_or_array_layers: 1,
                },
                mip_level_count: 1,
                sample_count: 1,
                dimension: wgpu::TextureDimension::D2,
                format: wgpu::TextureFormat::Rgba16Float,
                usage: wgpu::TextureUsages::TEXTURE_BINDING,
                view_formats: &[],
            })
            .create_view(&Default::default());
        let foam_sampler = device.create_sampler(&wgpu::SamplerDescriptor {
            label: Some("wgr_water_foam_material_sampler"),
            mag_filter: wgpu::FilterMode::Linear,
            min_filter: wgpu::FilterMode::Linear,
            address_mode_u: wgpu::AddressMode::ClampToEdge,
            address_mode_v: wgpu::AddressMode::ClampToEdge,
            ..Default::default()
        });
        let foam = fft.as_ref().map(|fft| {
            Foam::new(
                device,
                composer,
                &params_ubo,
                interaction.views(),
                fft.displacement_view(),
                fft.auxiliary_view(),
                fft.cascade_config_buffer(),
                &heightmap_view,
                &conform_params,
            )
        });
        let group1_bind = build_group1(
            device,
            &group1_layout,
            &params_ubo,
            &dummy_depth,
            &dummy_env,
            &env_sampler,
            interaction.view(),
            interaction.sampler(),
            fft.as_ref()
                .map_or(&fft_fallback_view, |f| f.displacement_view()),
            fft.as_ref()
                .map_or(&fft_fallback_view, |f| f.dynamics_view()),
            fft.as_ref()
                .map_or(&fft_fallback_view, |f| f.auxiliary_view()),
            &fft_sampler,
            foam.as_ref().map_or(&foam_fallback_view, |f| f.view()),
            foam.as_ref().map_or(&foam_sampler, |f| f.sampler()),
            &scene_view,
            &scene_sampler,
            &planar_view,
            &planar_sampler,
            &planar_params,
            &heightmap_view,
            &conform_params,
        );

        let (grid_vbuf, grid_ibuf, grid_index_count) = build_grid(device);

        let instance_cap = 64 * std::mem::size_of::<WgrWaterNode>() as u64;
        let instance_buf = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("wgr_water_instances"),
            size: instance_cap,
            usage: wgpu::BufferUsages::VERTEX | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });

        let shader = crate::shaders::make_module(
            device,
            composer,
            "wgr_water_shader",
            include_str!("water.wgsl"),
            "water/water.wgsl",
        );
        // Gerstner LOD-transitions are crack-free via the morph (adjacent levels agree at
        // the boundary), so skirts stay off by default — their walls would show through
        // the transparent surface as seams. Raise WGR_WATER_SKIRT_K if a seam appears.
        // (The wave/fade/warp/spec look params are UBO fields now, live-tuned by the
        // Water ImGui tab, so only the structural overrides remain here.)
        let skirt_k = std::env::var("WGR_WATER_SKIRT_K")
            .ok()
            .and_then(|v| v.parse::<f64>().ok())
            .unwrap_or(0.0);
        // HDR path: the color target is Rgba16Float only when HDR is on, so it signals
        // linear shading (tint/fog decode + un-clamped glint that blooms).
        let linear = if surface_format == wgpu::TextureFormat::Rgba16Float {
            1.0
        } else {
            0.0
        };
        // Shoreline foam A/B. The procedural foam field is analytic, so it has no mip chain and
        // aliased hard at distance; the fix band-limits every octave against the pixel footprint,
        // replaces the single-octave streak lattice, and cuts the band's gains. `=1` restores the
        // pre-fix look exactly (unfiltered noise, old lattice, old gains) so a before/after capture
        // needs one binary and a relaunch, not two builds.
        let shore_foam_legacy = std::env::var("WGR_WATER_SHORE_FOAM_LEGACY")
            .ok()
            .and_then(|v| v.parse::<f64>().ok())
            .unwrap_or(0.0);
        if shore_foam_legacy > 0.5 {
            eprintln!(
                "[wgr] shoreline foam: LEGACY (unfiltered, pre-WTR gains) from \
                 WGR_WATER_SHORE_FOAM_LEGACY"
            );
        }
        // Deep-side extent of the shoreline foam band; see `foam_band_outer` in water.wgsl for
        // what the number means. The owner reported the wash STILL reaching too far out to sea
        // after 1.95 -> 1.55 and again after 1.55 -> 1.10, so the default drops a third time, to
        // 0.80 (~0.70 m of column depth at the default foam_width of 0.4). The shader now also
        // derives the fade's START from this value instead of pinning it at 0.35, which is the
        // reason the previous cut read as smaller than its number: it shortened the faint outer
        // skirt without moving the bright part of the band at all. It is an env override and not a
        // literal because "how far out is too far" is a judgement about a picture, and settling it
        // should cost a relaunch rather than a rebuild.
        let foam_band_outer = std::env::var("WGR_WATER_FOAM_BAND_OUTER")
            .ok()
            .and_then(|v| v.parse::<f64>().ok())
            .unwrap_or(0.80);
        // Large-scale patchiness of the shoreline band, 0..1. `=0` restores the previous
        // stationary field on its own, which WGR_WATER_SHORE_FOAM_LEGACY=1 cannot do without also
        // giving back the unfiltered noise, the streak lattice and the old gains — so this is the
        // lever that isolates the patchiness in an A/B rather than confounding it with all of WTR.
        let foam_patch_gain = std::env::var("WGR_WATER_FOAM_PATCH")
            .ok()
            .and_then(|v| v.parse::<f64>().ok())
            .unwrap_or(1.0);
        // How far the band's DEEP edge meanders in and out along the coast, as a fraction of the
        // depth coordinate. The patchiness above varies how BRIGHT the wash is; this varies how
        // WIDE it is, which is the half of "it still looks tiled" that no amount of extra noise
        // inside a constant-width ribbon can fix. `=0` restores the straight-edged band on its own
        // for an A/B that does not also give back the old gains and the unfiltered noise.
        let foam_band_meander = std::env::var("WGR_WATER_FOAM_MEANDER")
            .ok()
            .and_then(|v| v.parse::<f64>().ok())
            .unwrap_or(0.34);
        let surf_pilot = if std::env::var("WGR_WATER_SURF_PILOT").ok().as_deref() == Some("1") {1.0} else {0.0};
        let vs_constants = [("skirt_k", skirt_k), ("surf_pilot", surf_pilot)];
        // `fs_constants` is shared with the whitewater pipeline below, whose shader declares only
        // `linear` — wgpu rejects a pipeline whose constant map names an override the module does
        // not have, so the surface's extra override needs its own array.
        let fs_constants = [("linear", linear)];
        let water_fs_constants = [
            ("linear", linear),
            ("breaker_cells", match std::env::var("WGR_BREAKER_CELLS").as_deref() {
                Ok("1") => 1.0,
                Ok("2") => 2.0,
                _ => 0.0,
            }),
            ("deep_breaker_recovery", std::env::var("WGR_DEEP_BREAKER_RECOVERY")
                .ok().and_then(|v| v.parse::<f64>().ok())
                .filter(|v| v.is_finite()).unwrap_or(1.0).clamp(0.0, 1.0)),
            ("shore_foam_legacy", shore_foam_legacy),
            ("surf_pilot", surf_pilot),
            ("foam_band_outer", foam_band_outer),
            ("foam_patch_gain", foam_patch_gain),
            ("foam_band_meander", foam_band_meander),
        ];

        // REN-GI-001 follow-up: name the reason when this layout fails validation (it did,
        // silently, when the camera group grew past the per-stage sampled-texture limit).
        let layout_scope = device.push_error_scope(wgpu::ErrorFilter::Validation);
        let pipeline_layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("wgr_water_pipeline_layout"),
            bind_group_layouts: &[Some(camera_layout), Some(&group1_layout)],
            immediate_size: 0,
        });
        if let Some(e) = pollster::block_on(layout_scope.pop()) {
            eprintln!("[wgr] water pipeline layout INVALID: {e}");
        }
        let grid_attrs = wgpu::vertex_attr_array![0 => Float32x3];
        let inst_attrs = wgpu::vertex_attr_array![
            1 => Float32x2, 2 => Float32, 3 => Uint32, 4 => Float32x2,
            5 => Float32x2, 6 => Float32, 7 => Float32
        ];
        let pipeline = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
            label: Some("wgr_water_pipeline"),
            layout: Some(&pipeline_layout),
            vertex: wgpu::VertexState {
                module: &shader,
                entry_point: Some("vs_water"),
                compilation_options: wgpu::PipelineCompilationOptions {
                    constants: &vs_constants,
                    ..Default::default()
                },
                buffers: &[
                    wgpu::VertexBufferLayout {
                        array_stride: 12,
                        step_mode: wgpu::VertexStepMode::Vertex,
                        attributes: &grid_attrs,
                    },
                    wgpu::VertexBufferLayout {
                        array_stride: std::mem::size_of::<WgrWaterNode>() as u64,
                        step_mode: wgpu::VertexStepMode::Instance,
                        attributes: &inst_attrs,
                    },
                ],
            },
            primitive: wgpu::PrimitiveState {
                topology: wgpu::PrimitiveTopology::TriangleList,
                cull_mode: None,
                ..Default::default()
            },
            // Transparent-ready: reversed-Z GreaterEqual test so coastlines (drawn
            // first, nearer) occlude water, but depth-write OFF so overlapping wave
            // tiles never self-occlude — nothing 3D draws behind water in-segment.
            depth_stencil: Some(wgpu::DepthStencilState {
                format: depth_format(device),
                depth_write_enabled: Some(false),
                depth_compare: Some(wgpu::CompareFunction::GreaterEqual),
                stencil: wgpu::StencilState::default(),
                bias: wgpu::DepthBiasState::default(),
            }),
            multisample: wgpu::MultisampleState {
                count: sample_count,
                ..Default::default()
            },
            fragment: Some(wgpu::FragmentState {
                module: &shader,
                entry_point: Some("fs_water"),
                compilation_options: wgpu::PipelineCompilationOptions {
                    constants: &water_fs_constants,
                    ..Default::default()
                },
                targets: &[Some(wgpu::ColorTargetState {
                    format: surface_format,
                    blend: Some(wgpu::BlendState::ALPHA_BLENDING),
                    write_mask: wgpu::ColorWrites::ALL,
                })],
            }),
            multiview_mask: None,
            cache: None,
        });

        // GodotOceanWaves renders sea spray as camera-facing quads emitted at FFT foam
        // crests.  Keep it in its own pipeline so the transparent ocean mesh remains
        // independent of the considerably sparser whitewater instances.
        let whitewater_shader = crate::shaders::make_module(
            device,
            composer,
            "wgr_whitewater_render_shader",
            include_str!("whitewater_render.wgsl"),
            "water/whitewater_render.wgsl",
        );
        let whitewater_pipeline = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
            label: Some("wgr_water_whitewater_pipeline"),
            layout: Some(&pipeline_layout),
            vertex: wgpu::VertexState {
                module: &whitewater_shader,
                entry_point: Some("vs_whitewater"),
                compilation_options: wgpu::PipelineCompilationOptions {
                    constants: &[("spray_world_anchor", if std::env::var("WGR_SPRAY_WORLD_ANCHOR")
                        .is_ok_and(|v| v == "0") { 0.0 } else { 1.0 }), ("surf_pilot", surf_pilot)],
                    ..Default::default()
                },
                buffers: &[],
            },
            primitive: wgpu::PrimitiveState {
                topology: wgpu::PrimitiveTopology::TriangleList,
                cull_mode: None,
                ..Default::default()
            },
            // Spray belongs above the surface but must still be hidden by terrain,
            // hulls, and other opaque scene geometry.
            depth_stencil: Some(wgpu::DepthStencilState {
                format: depth_format(device),
                depth_write_enabled: Some(false),
                depth_compare: Some(wgpu::CompareFunction::GreaterEqual),
                stencil: wgpu::StencilState::default(),
                bias: wgpu::DepthBiasState::default(),
            }),
            multisample: wgpu::MultisampleState {
                count: sample_count,
                ..Default::default()
            },
            fragment: Some(wgpu::FragmentState {
                module: &whitewater_shader,
                entry_point: Some("fs_whitewater"),
                compilation_options: wgpu::PipelineCompilationOptions {
                    constants: &fs_constants,
                    ..Default::default()
                },
                targets: &[Some(wgpu::ColorTargetState {
                    format: surface_format,
                    blend: Some(wgpu::BlendState::ALPHA_BLENDING),
                    write_mask: wgpu::ColorWrites::ALL,
                })],
            }),
            multiview_mask: None,
            cache: None,
        });

        // Explicit experiment only: no pipeline allocation or draw in normal play.
        let mut curling_layout = None;
        let mut curling_bind = None;
        let curling_pipeline = if std::env::var("WGR_WATER_CURLING_BREAKER").ok().as_deref() == Some("1") {
            let entries: Vec<_> = [0u32,2,3,6,9,12,13,17,18].into_iter().map(|binding| {
                let ty = match binding {
                    0|18 => wgpu::BindingType::Buffer {ty: wgpu::BufferBindingType::Uniform,has_dynamic_offset:false,min_binding_size:None},
                    3|9|13 => wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Filtering),
                    _ => wgpu::BindingType::Texture {sample_type:wgpu::TextureSampleType::Float {filterable:binding!=17},
                        view_dimension:if binding==6 {wgpu::TextureViewDimension::D2Array} else {wgpu::TextureViewDimension::D2},multisampled:false},
                };
                wgpu::BindGroupLayoutEntry {binding,visibility:wgpu::ShaderStages::VERTEX_FRAGMENT,ty,count:None}
            }).collect();
            let layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {label:Some("curl_no_scene_depth"),entries:&entries});
            curling_bind = Some(build_curl_group(device,&layout,&params_ubo,&dummy_env,&env_sampler,
                fft.as_ref().map_or(&fft_fallback_view,|f|f.displacement_view()),&fft_sampler,&heightmap_view,&conform_params,&scene_view,&scene_sampler));
            let pipeline_layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
                label:Some("curl_pipeline_layout"),bind_group_layouts:&[Some(camera_layout),Some(&layout)],immediate_size:0});
            curling_layout = Some(layout);
            Some(device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
                label: Some("wgr_curling_breaker"), layout: Some(&pipeline_layout),
                vertex: wgpu::VertexState { module: &shader, entry_point: Some("vs_curling_breaker"),
                    compilation_options: Default::default(), buffers: &[] },
                primitive: wgpu::PrimitiveState { cull_mode: None, ..Default::default() },
                depth_stencil: Some(wgpu::DepthStencilState { format: depth_format(device),
                    depth_write_enabled: Some(true), depth_compare: Some(wgpu::CompareFunction::GreaterEqual),
                    stencil: Default::default(), bias: Default::default() }),
                multisample: wgpu::MultisampleState {count: sample_count, ..Default::default()},
                fragment: Some(wgpu::FragmentState {module: &shader, entry_point: Some("fs_curling_breaker"),
                    compilation_options: wgpu::PipelineCompilationOptions {constants: &water_fs_constants, ..Default::default()},
                    targets: &[Some(wgpu::ColorTargetState {format: surface_format,
                        blend: Some(wgpu::BlendState::ALPHA_BLENDING),write_mask: wgpu::ColorWrites::ALL})]}),
                multiview_mask: None, cache: None,
            }))
        } else {None};

        Water {
            params_ubo,
            group1_layout,
            group1_bind,
            depth_view: dummy_depth,
            env_view: dummy_env,
            env_sampler,
            scene_view,
            scene_sampler,
            planar_view,
            planar_sampler,
            planar_params,
            planar_gen: u64::MAX,
            heightmap_view,
            conform_params,
            heightmap_gen: u64::MAX,
            interaction,
            fft,
            fft_storage_supported,
            foam,
            fft_fallback_view,
            fft_sampler,
            foam_fallback_view,
            foam_sampler,
            depth_gen: u64::MAX,
            env_gen: u64::MAX,
            scene_gen: u64::MAX,
            grid_vbuf,
            grid_ibuf,
            grid_index_count,
            instance_buf,
            instance_cap,
            instance_count: 0,
            pipeline,
            whitewater_pipeline,
            curling_pipeline,
            curling_layout,
            curling_bind,
            have_params: false,
            last_params: None,
        }
    }

    // Point group1 at the scene depth (opaque prepass) for the depth-based colour + soft
    // shoreline. Rebuilds the bind group only when the view was recreated (resize), tracked by
    // `gen` from Gfx3d::depth_gen(); a no-op otherwise. Called each frame before the water pass.
    pub fn set_depth_view(
        &mut self,
        device: &wgpu::Device,
        depth: &wgpu::TextureView,
        view_gen: u64,
    ) {
        if self.depth_gen == view_gen {
            return;
        }
        self.depth_view = depth.clone();
        self.depth_gen = view_gen;
        self.rebuild_group1(device);
    }

    // Point group1 at Sky's reflection env map (Stage 4a). The env texture is created once (never
    // resized), so `view_gen` is effectively constant and this rebuilds group1 exactly once.
    pub fn set_env_view(&mut self, device: &wgpu::Device, env: &wgpu::TextureView, view_gen: u64) {
        if self.env_gen == view_gen {
            return;
        }
        self.env_view = env.clone();
        self.env_gen = view_gen;
        self.rebuild_group1(device);
    }

    fn rebuild_group1(&mut self, device: &wgpu::Device) {
        if let Some(layout) = &self.curling_layout {
            self.curling_bind = Some(build_curl_group(device,layout,&self.params_ubo,&self.env_view,&self.env_sampler,
                self.fft.as_ref().map_or(&self.fft_fallback_view,|f|f.displacement_view()),&self.fft_sampler,&self.heightmap_view,&self.conform_params,&self.scene_view,&self.scene_sampler));
        }
        self.group1_bind = build_group1(
            device,
            &self.group1_layout,
            &self.params_ubo,
            &self.depth_view,
            &self.env_view,
            &self.env_sampler,
            self.interaction.view(),
            self.interaction.sampler(),
            self.fft
                .as_ref()
                .map_or(&self.fft_fallback_view, |f| f.displacement_view()),
            self.fft
                .as_ref()
                .map_or(&self.fft_fallback_view, |f| f.dynamics_view()),
            self.fft
                .as_ref()
                .map_or(&self.fft_fallback_view, |f| f.auxiliary_view()),
            &self.fft_sampler,
            self.foam
                .as_ref()
                .map_or(&self.foam_fallback_view, |f| f.view()),
            self.foam
                .as_ref()
                .map_or(&self.foam_sampler, |f| f.sampler()),
            &self.scene_view,
            &self.scene_sampler,
            &self.planar_view,
            &self.planar_sampler,
            &self.planar_params,
            &self.heightmap_view,
            &self.conform_params,
        );
    }

    pub fn set_params(&mut self, queue: &wgpu::Queue, mut params: WgrWaterParams) {
        // A device lacking float storage textures keeps the established Gerstner carrier.
        if self.fft.is_none() {
            params.fft_control[0] = 0.0;
        }
        queue.write_buffer(&self.params_ubo, 0, bytemuck::bytes_of(&params));
        if let Some(fft) = &mut self.fft {
            fft.set_params(&params);
        }
        self.have_params = true;
        self.last_params = Some(params);
    }

    pub fn set_cascade_config(
        &mut self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        index: u32,
        config: crate::ffi::WgrWaterCascadeConfig,
    ) {
        // Resolution is a real live quality control, not informational metadata.
        // Rebuild only when an enabled preset supplies one of the supported tiers;
        // disabled tail cascades use resolution=0 and must not switch us back.
        let requested = config.resolution;
        let valid_resolution = matches!(
            requested,
            fft::FFT_MIN_RESOLUTION | FFT_RESOLUTION | fft::FFT_MAX_RESOLUTION
        );
        let needs_rebuild = valid_resolution
            && self
                .fft
                .as_ref()
                .is_none_or(|current| current.resolution() != requested);
        if needs_rebuild && self.fft_storage_supported {
            self.fft = Fft::new(
                device,
                queue,
                &self.params_ubo,
                self.fft_storage_supported,
                requested,
            );
            if let (Some(fft), Some(params)) = (&mut self.fft, self.last_params.as_ref()) {
                fft.set_params(params);
            }
            self.rebuild_group1(device);
        }
        if let Some(fft) = &mut self.fft {
            fft.set_cascade_config(device, queue, index, config);
        }
    }

    /// Global ocean plane, not the containing inland water body's surface.
    pub fn ocean_level(&self) -> Option<f32> {
        self.last_params.map(|p| p.sea_level)
    }

    /// `(sea_level, time, eye submersion depth in metres)`. The depth lane is
    /// positive when the eye is under the local (wave-displaced) surface and zero
    /// when it is dry, so callers can ramp with it instead of switching on a flag.
    pub fn underwater_params(&self) -> Option<(f32, f32, f32)> {
        self.last_params
            .map(|p| (Self::local_surface_level(&p), p.time, p.fft_control[3].max(0.0)))
    }

    /// WRL-003: the mean surface under the camera. The producer writes the containing body's
    /// level (or sea level) into optics_params.z with the body's wave scale in .w; a producer
    /// that never wrote the lane (w == 0) means "the sea".
    fn local_surface_level(p: &WgrWaterParams) -> f32 {
        if p.optics_params[3] > 0.0 {
            p.optics_params[2]
        } else {
            p.sea_level
        }
    }

    /// WRL-006: the containment ellipse (cx, cz, rx, rz) of the body the camera is in, from
    /// the same table the surface draws, so the compositor can stop attenuating where a ray
    /// leaves the body through its side. rx == 0 means "no body: the sea".
    pub fn camera_body_ellipse(&self, cam_xz: [f32; 2]) -> ([f32; 4], [f32; 2]) {
        let Some(p) = self.last_params else { return ([0.0; 4], [1.0, 0.0]) };
        let mut best: Option<([f32; 4], [f32; 2])> = None;
        for i in 0..crate::ffi::WGR_WATER_MAX_BODIES {
            let e = p.bodies[3 * i];
            if e[2] <= 0.0 || e[3] <= 0.0 {
                continue;
            }
            let rot = p.bodies[3 * i + 2][3];
            let (s, c) = rot.sin_cos();
            let dx = cam_xz[0] - e[0];
            let dz = cam_xz[1] - e[1];
            let u = (c * dx + s * dz) / e[2];
            let v = (-s * dx + c * dz) / e[3];
            if u * u + v * v <= 1.0 && best.is_none_or(|b| e[2] * e[3] < b.0[2] * b.0[3]) {
                best = Some((e, [c, s]));
            }
        }
        best.unwrap_or(([0.0; 4], [1.0, 0.0]))
    }

    fn local_wave_scale(p: &WgrWaterParams) -> f32 {
        if p.optics_params[3] > 0.0 {
            p.optics_params[3]
        } else {
            1.0
        }
    }

    /// True when the live Water-tab performance mode has disabled reflection work.
    /// This is queried before frame encoding so the renderer can avoid creating the
    /// reflected camera and recording a planar pass the water shader cannot sample.
    pub fn low_quality(&self) -> bool {
        self.last_params
            .map(|p| p.sea_params[2] > 0.5)
            .unwrap_or(false)
    }

    /// The water's own body colour and extinction, so the underwater compositor can fog the scene
    /// in the SAME colour the surface is tinted with. It previously used a hardcoded cyan haze,
    /// which is why submerging looked like a different substance from the water you swam into.
    /// Returns (shallow rgb, deep rgb, color_ext), with both colours in gamma space.
    pub fn underwater_body(&self) -> Option<([f32; 3], [f32; 3], f32)> {
        self.last_params.map(|p| {
            (
                [p.shallow_color[0], p.shallow_color[1], p.shallow_color[2]],
                [p.deep_color[0], p.deep_color[1], p.deep_color[2]],
                p.color_ext,
            )
        })
    }

    /// FFT fields consumed by the underwater caustic compute pass. The fallback array
    /// is a valid zero texture, so the compositor remains operational on Gerstner-only
    /// adapters without a second binding path.
    pub fn underwater_fft_views(&self) -> (wgpu::TextureView, wgpu::TextureView) {
        (
            self.fft
                .as_ref()
                .map_or(&self.fft_fallback_view, |f| f.dynamics_view())
                .clone(),
            self.fft
                .as_ref()
                .map_or(&self.fft_fallback_view, |f| f.auxiliary_view())
                .clone(),
        )
    }

    /// Whether the Water tab's "Underwater effect" checkbox is on. The compositor must not
    /// run at all when it is off — the submersion depth alone cannot express this, because
    /// the compositor also engages on proximity to the surface and a submerged camera is
    /// always proximate.
    pub fn underwater_enabled(&self) -> bool {
        self.last_params.is_some_and(|p| p.underwater_gate[0] > 0.5)
    }

    /// Live underwater tuning from the Water tab: `(engage band m, density, colour bias,
    /// caustic gain)`. The defaults reproduce the tuned look, so a renderer that never
    /// receives water params behaves as it did before these became adjustable.
    pub fn underwater_tuning(&self) -> (f32, f32, f32, f32) {
        self.last_params
            .map(|p| {
                (
                    p.underwater_params[0],
                    p.underwater_params[1],
                    p.underwater_params[2],
                    p.underwater_params[3],
                )
            })
            .unwrap_or((1.5, 1.0, 1.0, 1.0))
    }

    /// Vertical FFT displacement, so the underwater compositor can find the wavy
    /// surface instead of assuming a flat plane at `sea_level`. Same fallback as
    /// `underwater_fft_views`: a valid zero array on Gerstner-only adapters, which
    /// degrades the compositor to the flat-plane behaviour rather than breaking it.
    pub fn underwater_displacement_view(&self) -> wgpu::TextureView {
        self.fft
            .as_ref()
            .map_or(&self.fft_fallback_view, |f| f.displacement_view())
            .clone()
    }

    /// Spectrum controls needed to map the camera-centred caustic field to the same
    /// aperiodic world coordinates as the visible water surface. `wave_scale` is the
    /// Water-tab lookup dilation; the compositor needs it for the same reason the
    /// surface shader does, or its idea of the waterline drifts from the drawn waves.
    pub fn underwater_spectrum(&self) -> ([f32; 4], u32, f32, f32, f32, f32, f32) {
        self.last_params
            .map(|p| {
                (
                    p.fft_cascade_lengths,
                    self.fft.as_ref().map_or(0, |f| f.active_layers()),
                    p.warp_amp,
                    Self::local_surface_level(&p),
                    p.debug_params[0],
                    p.wave_scale,
                    Self::local_wave_scale(&p),
                )
            })
            .unwrap_or(([1.0; 4], 0, 0.0, 0.0, 0.0, 1.0, 1.0))
    }

    pub fn fft_enabled(&self) -> bool {
        self.fft.is_some()
    }

    pub fn set_interaction_params(
        &mut self,
        queue: &wgpu::Queue,
        params: WgrWaterInteractionParams,
    ) {
        self.interaction.set_params(queue, params);
        if let Some(foam) = &self.foam {
            foam.set_params(queue, params);
        }
    }

    pub fn submit_interactions(
        &mut self,
        queue: &wgpu::Queue,
        events: &[WgrWaterInteractionEvent],
    ) {
        self.interaction.submit(queue, events);
    }

    pub fn update_interactions(
        &mut self,
        device: &wgpu::Device,
        encoder: &mut wgpu::CommandEncoder,
        timers: &crate::gpu_timers::GpuTimers,
    ) {
        use crate::gpu_timers::Region;
        // WTR-001 — deterministic water freeze. The dev-only `Engine::WaterSettings::Freeze`
        // block is packed by WaterWgpu into WgrWaterParams.fft_control.z as a WGR_WATER_FREEZE_*
        // bit mask. Skipping the dispatch entirely (rather than running it against time=const and
        // dt=0) keeps the captured frame truly frozen without the GPU cost; the choice is a perf
        // optimization on top of `freezeTime`, not a correctness lever — set_params already
        // substitutes the right UBO values, so the masked-out passes would be no-ops anyway.
        const FREEZE_FFT: u32 = 1 << 0;
        const FREEZE_INTERACTION: u32 = 1 << 1;
        const FREEZE_FOAM: u32 = 1 << 2;
        let mut freeze_mask: u32 = self
            .last_params
            .map(|p| p.fft_control[2].to_bits())
            .unwrap_or(0);
        // PERF: no water surface in this frame's draw stream => nothing consumes the FFT,
        // ripple or foam textures, so simulating them is pure cost. `instance_count` is the
        // CDLOD selection WaterWgpu::Prepare just published (frustum-culled AND below-sea
        // filtered, WaterWgpu.cpp:1095-1100); 0 means the water pass will not be recorded
        // either -- measured as `Water draw = -1` for a whole session on an inland Takistan
        // view while Spectrum evolve / FFT h+v / compose / Foam still charged
        // 0.108+0.215+0.213+0.115+0.191 = 0.842 ms of an 8.167 ms frame (10.3%).
        //
        // Safe to skip rather than throttle: the FFT spectrum is a pure function of absolute
        // time (fft.rs evolves from `time`, not from the previous frame), so it resumes exact
        // on the frame water returns. Ripple/foam are accumulators and resume from wherever
        // they were left -- with no surface on screen there is nothing to pop.
        //
        // WGR_WATER_SIM_ALWAYS=1 restores the unconditional simulation for A/B.
        if self.instance_count == 0 && !water_sim_always() {
            freeze_mask |= FREEZE_FFT | FREEZE_INTERACTION | FREEZE_FOAM;
        }
        if (freeze_mask & FREEZE_FFT) == 0 {
            if let Some(fft) = &mut self.fft {
                fft.dispatch(encoder, timers);
            }
        }
        if (freeze_mask & FREEZE_INTERACTION) == 0 {
            // WTR-002 — injection + propagation are one fused kernel today, so a single
            // bracket covers both spec rows (the split lands with the interaction rework).
            timers.begin(encoder, Region::Interaction);
            self.interaction.dispatch(encoder);
            timers.end(encoder, Region::Interaction);
        }
        if (freeze_mask & FREEZE_FOAM) == 0 {
            if let Some(foam) = &mut self.foam {
                timers.begin(encoder, Region::Foam);
                foam.dispatch(encoder, self.interaction.current());
                timers.end(encoder, Region::Foam);
            }
        }
        self.rebuild_group1(device);
    }

    // The snapshot is a distinct completed scene texture, never water's active target.
    pub fn set_scene_view(
        &mut self,
        device: &wgpu::Device,
        scene: &wgpu::TextureView,
        view_gen: u64,
    ) {
        if self.scene_gen == view_gen {
            return;
        }
        self.scene_view = scene.clone();
        self.scene_gen = view_gen;
        self.rebuild_group1(device);
    }

    // A separate completed reflected-camera target. Validity is explicit because black
    // reflected pixels are legitimate at night and must not be confused with a dummy.
    pub fn set_planar_view(
        &mut self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        planar: &wgpu::TextureView,
        view_gen: u64,
        full_vp: [f32; 16],
        // 0..1, not a bool: the coverage gate dissolves the planar contribution to zero BEFORE
        // it stops rendering the target, so the handover to the environment-map reflection is a
        // fade rather than a switch. See `PlanarConfig` in lib.rs.
        fade: f32,
    ) {
        let mut params = [0.0f32; 20];
        params[..16].copy_from_slice(&full_vp);
        params[16] = fade.clamp(0.0, 1.0);
        queue.write_buffer(&self.planar_params, 0, bytemuck::cast_slice(&params));
        if self.planar_gen != view_gen {
            self.planar_view = planar.clone();
            self.planar_gen = view_gen;
            self.rebuild_group1(device);
        }
    }

    // Write ONLY the planar fade, leaving the reflected view-projection and the bound target
    // alone. Used on frames the reflection pass is skipped: the target still holds the last
    // frame it was rendered for, and its matrix is stale, so the shader must be told to stop
    // weighting it. Without this the water kept sampling a stale reflection with `valid = 1`
    // whenever the pass stopped running — dropping below the surface, water leaving the draw
    // stream, or now the coverage gate.
    pub fn set_planar_fade(&self, queue: &wgpu::Queue, fade: f32) {
        // params[16] is the fade lane; offset 16 floats = 64 bytes.
        queue.write_buffer(
            &self.planar_params,
            64,
            bytemuck::bytes_of(&fade.clamp(0.0, 1.0)),
        );
    }

    // Lend the terrain heightmap to the water vertex stage so the surface can be clamped
    // above the seabed. The params are small and change with the world, so they are written
    // every call; the bind group is only rebuilt when the texture itself was reallocated.
    pub fn set_heightmap(
        &mut self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        heightmap: &wgpu::TextureView,
        view_gen: u64,
        params: &crate::terrain::TerrainConformParams,
    ) {
        queue.write_buffer(&self.conform_params, 0, bytemuck::bytes_of(params));
        if self.heightmap_gen == view_gen {
            return;
        }
        self.heightmap_view = heightmap.clone();
        self.heightmap_gen = view_gen;
        if let Some(foam) = &mut self.foam { foam.set_heightmap(device, &self.heightmap_view, &self.conform_params); }
        self.rebuild_group1(device);
    }

    pub fn prepare(&mut self, device: &wgpu::Device, queue: &wgpu::Queue, nodes: &[WgrWaterNode]) {
        self.instance_count = nodes.len() as u32;
        if nodes.is_empty() {
            return;
        }
        let needed = std::mem::size_of_val(nodes) as u64;
        if needed > self.instance_cap {
            let cap = needed.next_power_of_two();
            self.instance_buf = device.create_buffer(&wgpu::BufferDescriptor {
                label: Some("wgr_water_instances"),
                size: cap,
                usage: wgpu::BufferUsages::VERTEX | wgpu::BufferUsages::COPY_DST,
                mapped_at_creation: false,
            });
            self.instance_cap = cap;
        }
        queue.write_buffer(&self.instance_buf, 0, bytemuck::cast_slice(nodes));
    }

    pub fn has_curling_breaker(&self) -> bool {self.curling_pipeline.is_some() && self.have_params}
    pub fn draw_curling(&self, pass: &mut wgpu::RenderPass<'_>, camera: &wgpu::BindGroup, offset: u32) {
        if let (Some(pipeline),Some(bind))=(&self.curling_pipeline,&self.curling_bind) {
            pass.set_pipeline(pipeline);
            pass.set_bind_group(0,camera,&[offset]);
            pass.set_bind_group(1,bind,&[]);
            pass.draw(0..(64*48*6+512*6),0..1);
        }
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
    ) {
        if !self.have_params || node_count == 0 {
            return;
        }
        if first_node + node_count > self.instance_count {
            return;
        }
        pass.set_pipeline(&self.pipeline);
        pass.set_bind_group(0, camera_bind, &[camera_offset]);
        pass.set_bind_group(1, &self.group1_bind, &[]);
        pass.set_vertex_buffer(0, self.grid_vbuf.slice(..));
        pass.set_vertex_buffer(1, self.instance_buf.slice(..));
        pass.set_index_buffer(self.grid_ibuf.slice(..), wgpu::IndexFormat::Uint16);
        pass.draw_indexed(
            0..self.grid_index_count,
            0,
            first_node..first_node + node_count,
        );

        // The reference demo keeps one sea-spray emitter under its ocean object.
        // WaterWgpu normally submits one batch, but guard this draw so split CDLOD
        // batches do not duplicate the same camera-centred emitter.
        //
        // Only submit it when spray is actually enabled. The shader collapses disabled particles to
        // alpha 0, but that only saves the FRAGMENT cost — the vertex stage still runs for every
        // candidate, hashing, sampling four FFT cascades and the interaction field per instance.
        // At 16,384 candidates that is ~98k vertices of real work every frame with the feature
        // switched off. (An earlier comment here claimed the cost scales with visible whitewater;
        // that was only ever true of the fragment stage.)
        let spray_enabled = self
            .last_params
            .map(|p| p.debug_params[1] > 0.5 && p.sea_params[2] < 0.5)
            .unwrap_or(false);
        if first_node == 0 && spray_enabled {
            pass.set_pipeline(&self.whitewater_pipeline);
            pass.set_bind_group(0, camera_bind, &[camera_offset]);
            pass.set_bind_group(1, &self.group1_bind, &[]);
            pass.draw(0..6, 0..WHITEWATER_PARTICLE_COUNT);
        }
    }
}

// group1 = params/depth/env/interaction, shared four-layer FFT fields (6..9), and foam history.
// Rebuilt whenever the depth or env view changes; the params UBO + sampler are stable so they ride
// along.
fn build_group1(
    device: &wgpu::Device,
    layout: &wgpu::BindGroupLayout,
    params_ubo: &wgpu::Buffer,
    depth: &wgpu::TextureView,
    env: &wgpu::TextureView,
    env_sampler: &wgpu::Sampler,
    interaction: &wgpu::TextureView,
    interaction_sampler: &wgpu::Sampler,
    displacement: &wgpu::TextureView,
    dynamics: &wgpu::TextureView,
    auxiliary: &wgpu::TextureView,
    fft_sampler: &wgpu::Sampler,
    foam: &wgpu::TextureView,
    foam_sampler: &wgpu::Sampler,
    scene: &wgpu::TextureView,
    scene_sampler: &wgpu::Sampler,
    planar: &wgpu::TextureView,
    planar_sampler: &wgpu::Sampler,
    planar_params: &wgpu::Buffer,
    heightmap: &wgpu::TextureView,
    conform_params: &wgpu::Buffer,
) -> wgpu::BindGroup {
    device.create_bind_group(&wgpu::BindGroupDescriptor {
        label: Some("wgr_water_group1_bind"),
        layout,
        entries: &[
            wgpu::BindGroupEntry {
                binding: 0,
                resource: params_ubo.as_entire_binding(),
            },
            wgpu::BindGroupEntry {
                binding: 1,
                resource: wgpu::BindingResource::TextureView(depth),
            },
            wgpu::BindGroupEntry {
                binding: 2,
                resource: wgpu::BindingResource::TextureView(env),
            },
            wgpu::BindGroupEntry {
                binding: 3,
                resource: wgpu::BindingResource::Sampler(env_sampler),
            },
            wgpu::BindGroupEntry {
                binding: 4,
                resource: wgpu::BindingResource::TextureView(interaction),
            },
            wgpu::BindGroupEntry {
                binding: 5,
                resource: wgpu::BindingResource::Sampler(interaction_sampler),
            },
            wgpu::BindGroupEntry {
                binding: 6,
                resource: wgpu::BindingResource::TextureView(displacement),
            },
            wgpu::BindGroupEntry {
                binding: 7,
                resource: wgpu::BindingResource::TextureView(dynamics),
            },
            wgpu::BindGroupEntry {
                binding: 8,
                resource: wgpu::BindingResource::TextureView(auxiliary),
            },
            wgpu::BindGroupEntry {
                binding: 9,
                resource: wgpu::BindingResource::Sampler(fft_sampler),
            },
            wgpu::BindGroupEntry {
                binding: 10,
                resource: wgpu::BindingResource::TextureView(foam),
            },
            wgpu::BindGroupEntry {
                binding: 11,
                resource: wgpu::BindingResource::Sampler(foam_sampler),
            },
            wgpu::BindGroupEntry {
                binding: 12,
                resource: wgpu::BindingResource::TextureView(scene),
            },
            wgpu::BindGroupEntry {
                binding: 13,
                resource: wgpu::BindingResource::Sampler(scene_sampler),
            },
            wgpu::BindGroupEntry {
                binding: 14,
                resource: wgpu::BindingResource::TextureView(planar),
            },
            wgpu::BindGroupEntry {
                binding: 15,
                resource: wgpu::BindingResource::Sampler(planar_sampler),
            },
            wgpu::BindGroupEntry {
                binding: 16,
                resource: planar_params.as_entire_binding(),
            },
            wgpu::BindGroupEntry {
                binding: 17,
                resource: wgpu::BindingResource::TextureView(heightmap),
            },
            wgpu::BindGroupEntry {
                binding: 18,
                resource: conform_params.as_entire_binding(),
            },
        ],
    })
}

// The reusable unit grid: (GRID_N+1)^2 vertices over [0,1]^2, two triangles per
// quad, plus a border skirt. Vertex is (u, v, skirt); the shader drops skirt
// vertices below the surface to wall off LOD-transition cracks. Identical to the
// terrain grid (kept separate so the two modules stay decoupled).
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
        label: Some("wgr_water_grid_vbuf"),
        contents: bytemuck::cast_slice(&verts),
        usage: wgpu::BufferUsages::VERTEX,
    });
    let ibuf = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
        label: Some("wgr_water_grid_ibuf"),
        contents: bytemuck::cast_slice(&indices),
        usage: wgpu::BufferUsages::INDEX,
    });
    (vbuf, ibuf, indices.len() as u32)
}

#[cfg(test)]
mod tests {
    #[test]
    fn fading_ripples_preserve_underlying_ocean_slopes() {
        use glam::{Vec2, Vec3};

        let shader = include_str!("water.wgsl");
        assert!(shader.contains("n = normalize(n + vec3<f32>(combined_slope.x, 0.0, combined_slope.y) * n.y * 0.95)"));
        assert!(!shader.contains("interaction_weight"));
        let combine = |n: Vec3, slope: Vec2| {
            (n + Vec3::new(slope.x, 0.0, slope.y) * n.y * 0.95).normalize()
        };
        for ocean in [Vec3::Y, Vec3::new(0.4, 1.0, -0.7).normalize(),
                      Vec3::new(-1.0, 0.001, 0.2).normalize()] {
            // A flat residual offset can outlive visible ripples. Its absolute
            // height is irrelevant: equal neighbours always have zero slope.
            for height in [0.0_f32, 0.00001, 0.001, 0.1, -0.1] {
                let flat_slope = Vec2::splat(height - height) * (8.5 + 3.2);
                assert!(combine(ocean, flat_slope).abs_diff_eq(ocean, 1e-6));
            }
            let ripple = Vec2::new(0.3, -0.2);
            let active = combine(ocean, ripple);
            assert!(active.is_finite());
            assert!(active.abs_diff_eq(
                Vec3::new(ocean.x / ocean.y + ripple.x * 0.95, 1.0,
                          ocean.z / ocean.y + ripple.y * 0.95).normalize(), 1e-6));
            assert!(combine(ocean, ripple * 1e-7).abs_diff_eq(ocean, 1e-6));
        }
    }

    #[test]
    fn shoreline_geometry_uses_world_continuous_height_field_inputs() {
        let shader = include_str!("water.wgsl");
        assert!(shader.contains("let has_seabed = seabed_contains(base_xz)"));
        assert!(shader.contains("vertex_shore_factor = 1.0 - smoothstep(2.0, 30.0, local_depth)"));
        assert!(shader.contains("fft_geometry_disp(base_xz, dist, vertex_shore_factor)"));
        assert!(!shader.contains("fft_geometry_disp(base_xz, dist, shore_factor)"));
        assert!(shader.contains("disp.x * horizontal_keep"));
    }

    #[test]
    fn ggx_water_lobe_is_finite_and_broadens_with_slope_variance() {
        let roughness = |variance: f32, micro_slope: f32| {
            let legacy_floor = (2.0_f32 / 242.0).sqrt();
            (legacy_floor + variance.clamp(0.0, 0.25).sqrt() * 0.26 + micro_slope * 0.35)
                .clamp(0.075, 0.32)
        };
        let ggx = |roughness: f32| {
            let alpha_sq = roughness.powi(4);
            let d_base = (1.0_f32 * (alpha_sq - 1.0) + 1.0).max(1e-6);
            alpha_sq / (std::f32::consts::PI * d_base * d_base)
        };

        let calm = roughness(0.0, 0.0);
        let rough = roughness(0.20, 0.04);
        assert!((0.075..=0.32).contains(&calm));
        assert!((0.075..=0.32).contains(&rough));
        assert!(rough > calm);
        assert!(ggx(calm).is_finite() && ggx(rough).is_finite());
        assert!(ggx(rough) < ggx(calm));
    }

    #[test]
    fn foam_material_is_rough_diffuse_with_subdued_dielectric_specular() {
        let ggx = |roughness: f32, ndh: f32| {
            let alpha_sq = roughness.powi(4);
            let base = (ndh * ndh * (alpha_sq - 1.0) + 1.0).max(1e-6);
            alpha_sq / (std::f32::consts::PI * base * base)
        };
        let foam_roughness = 0.72;
        let foam_f0 = 0.02;
        let diffuse = 0.94 / std::f32::consts::PI;
        let specular = ggx(foam_roughness, 1.0) * foam_f0 * 0.08;

        assert!(foam_roughness > 0.5);
        assert!(specular.is_finite() && diffuse.is_finite());
        assert!(specular < diffuse * 0.1);
        assert!(ggx(foam_roughness, 0.7) > ggx(foam_roughness, 0.2));
    }

    #[test]
    fn planar_reflection_uses_stable_plane_projection_with_bounded_ssr_overlap() {
        let shader = include_str!("water.wgsl");
        assert!(shader.contains("fn planar_project"));
        assert!(
            shader.contains("let plane_point = vec3<f32>(absolute.x, wp.sea_level, absolute.z)")
        );
        assert!(!shader.contains("2.0 * wp.sea_level - absolute.y"));
        assert!(shader.contains("let distorted_uv = clamp(uv, texel, vec2<f32>(1.0) - texel)"));
        assert!(!shader.contains("let slope_projection = planar_project"));
        assert!(shader.contains("let max_mip = f32(textureNumLevels(planar_color) - 1u)"));
        assert!(
            shader.contains("let reflection_lod = (0.14 + 0.86 * roughness * roughness) * max_mip")
        );
        assert!(shader.contains("planar_refl.a * 0.68 * (1.0 - ssr.a * 0.80)"));
        assert!(shader.contains(
            "let ssr_distance_weight = 1.0 - smoothstep(180.0, 320.0, length(in.world_pos))"
        ));
        assert!(shader.contains("if (ssr_distance_weight > 0.002)"));

        let texel = 1.0 / 960.0_f32; // a representative half-res 1920px target
        let roughness = 0.20_f32;
        let max_warp = texel * (5.0 + roughness * 5.0);
        let projected_slope = 0.040_f32;
        let bounded = projected_slope.clamp(-max_warp, max_warp);
        assert!(bounded.is_finite());
        assert!(bounded > 5.0 * texel);
        assert!(bounded <= 10.0 * texel);

        let planar_weight = 1.0 - 1.0 * 0.80;
        assert!(planar_weight > 0.0 && planar_weight < 1.0);
    }

    #[test]
    fn procedural_water_detail_skips_zero_contribution_work() {
        let shader = include_str!("water.wgsl");
        assert!(shader.contains("if (strength <= 1e-5)"));
        assert!(shader.contains("if (foam_band > 0.001 && wp.foam_intensity > 0.001)"));
        assert!(shader.contains("let foam_history_sample = state.foam_history_sample"));
        assert!(shader.contains("if (crest_top > 0.001)"));
        assert!(shader.contains("if (unstructured_foam > 0.001)"));
        assert!(shader.contains("if (raw_length > 0.0)"));
        assert!(!shader.contains("lost_variance = lost_variance"));
    }

    #[test]
    fn whitecap_gain_is_independent_of_shore_foam_gain() {
        let shader = include_str!("water.wgsl");
        assert!(shader.contains("max(persistent_foam, breaker_foam) * wave_scale * raft_coverage"));
        assert!(!shader.contains("foam_scale * wave_scale"));
        let cpp = include_str!("../../../EngineWgpu.cpp");
        assert!(cpp.contains("out << \"whitecaps \" << s.waveFoamIntensity"));
        assert!(cpp.contains("key == \"whitecaps\""));
        assert!(cpp.contains("in >> _waterLook.waveFoamIntensity >> _waterLook.waveFoamDeepFalloff >> windGate"));
    }

    #[test]
    fn deep_breakers_recover_without_whitening_calm_swell_or_shore() {
        let readiness = |deep: f32, falloff: f32, breaking: f32, wind: f32| {
            let depth = 1.05 + ((1.05 + (0.12 - 1.05) * falloff) - 1.05) * deep;
            (depth + (1.05 - depth) * breaking) * wind
        };
        assert!((readiness(1.0, 1.0, 0.0, 1.0) - 0.12).abs() < 1e-6);
        assert!((readiness(1.0, 1.0, 1.0, 1.0) - 1.05).abs() < 1e-6);
        for step in 0..=100 {
            let breaking = step as f32 / 100.0;
            assert_eq!(readiness(1.0, 1.0, breaking, 0.0), 0.0);
            assert!((readiness(0.0, 1.0, breaking, 1.0) - 1.05).abs() < 1e-6);
            assert!((readiness(1.0, 0.0, breaking, 1.0) - 1.05).abs() < 1e-6);
        }
        let shader = include_str!("water.wgsl");
        assert!(shader.contains("jacobian_break * clamp(deep_breaker_recovery, 0.0, 1.0)) * wind_gate"));
    }

    #[test]
    fn resolved_breaker_cells_are_bounded_monotone_and_keep_filtered_distance() {
        let coverage = |amount: f32, structure: f32, footprint: f32| {
            let threshold = 0.15 + 0.70 * structure.clamp(0.0, 1.0);
            let edge = 0.08 + 0.25 * footprint.clamp(0.0, 1.0);
            let lo = (threshold - edge).max(0.001);
            let hi = (threshold + edge).min(0.999);
            let t = ((amount.clamp(0.0, 1.0) - lo) / (hi - lo)).clamp(0.0, 1.0);
            t * t * (3.0 - 2.0 * t)
        };
        for s in 0..=20 {
            for f in 0..=20 {
                let structure = s as f32 / 20.0;
                let footprint = f as f32 / 20.0;
                assert_eq!(coverage(0.0, structure, footprint), 0.0);
                assert_eq!(coverage(1.0, structure, footprint), 1.0);
                let mut previous = 0.0;
                for a in 0..=100 {
                    let c = coverage(a as f32 / 100.0, structure, footprint);
                    assert!(c >= previous && (0.0..=1.0).contains(&c));
                    previous = c;
                }
            }
        }
        assert_eq!(coverage(0.65, 0.5, 0.0), 1.0);
        let shader = include_str!("water.wgsl");
        assert!(shader.contains("override breaker_cells: f32 = 0.0;"));
        assert!(shader.contains("let cell_footprint = foam_fp * 2.7 * FOAM_FREQ;"));
        assert!(shader.contains("let detail = 1.0 - smoothstep(0.15, 0.55, cell_footprint);"));
        assert!(shader.contains("combined_foam = mix(combined_foam, max(shore, resolved), detail);"));
        assert!(shader.contains("if (detail > 0.001 && resolved > 0.02)"));
        assert!(shader.contains("vnoise_lod(foam_drift * 7.31, foam_fp * 7.31)"));
        assert!(shader.contains("var breaker_material_weight = 0.0;"));
        let helper = shader.split("fn resolved_breaker_coverage(").nth(1).unwrap().split("\n}").next().unwrap();
        assert!(!helper.contains("texture") && !helper.contains("vnoise"));
    }

    #[test]
    fn bubble_detail_cannot_create_foam_and_skips_unresolved_cells() {
        for r in 0..=100 {
            let rim = r as f32 / 100.0;
            for f in 0..=100 {
                let fresh = f as f32 / 100.0;
                let thin = 0.38 + 0.62 * rim;
                let thick = 0.78 + 0.22 * rim;
                let gain = thin + (thick - thin) * fresh;
                assert!((0.38..=1.0).contains(&gain));
                assert_eq!(0.0 * gain, 0.0);
                assert!((0.82..=1.0).contains(&(0.82 + 0.18 * rim)));
            }
        }
        let shader = include_str!("water.wgsl").replace("\r\n", "\n");
        let helper = shader.split("fn breaker_bubble_rim(").nth(1).unwrap()
            .split("\n}\n").next().unwrap();
        assert!(!helper.contains("texture"));
        assert!(helper.find("if (detail <= 0.001)").unwrap() < helper.find("hash2(").unwrap());
        assert!(helper.contains("footprint * frequency"));
        assert!(shader.contains("if (breaker_cells > 1.5)"));
    }

    #[test]
    fn spray_seed_follows_world_cells_when_the_camera_window_moves() {
        fn seed(anchor: [i32; 2], slot: [i32; 2]) -> u32 {
            let x = (anchor[0] + slot[0] - 64) as u32;
            let z = (anchor[1] + slot[1] - 64) as u32;
            x.wrapping_mul(1597334677).wrapping_add(z.wrapping_mul(3812015801))
        }
        for anchor in [[0, 0], [-1, -1], [13866, 3200], [106666, -106666]] {
            for dx in -8..=8 {
                for dz in -8..=8 {
                    let moved = [anchor[0] + dx, anchor[1] + dz];
                    let moved_slot = [64 - dx, 64 - dz];
                    assert_eq!(seed(anchor, [64, 64]), seed(moved, moved_slot));
                    if dx != 0 || dz != 0 {
                        assert_ne!(64 * 128 + 64, moved_slot[1] * 128 + moved_slot[0]);
                    }
                }
            }
        }
        let shader = include_str!("whitewater_render.wgsl");
        assert!(shader.contains("spray_cell_seed(world_cell)"));
        assert!(shader.contains("let random = hash21(seed);"));
        assert!(shader.contains("hash11(seed * 747796405u"));
        assert!(shader.contains("hash11(seed * 277803737u"));
        assert!(!shader.contains("hash11(instance_index *"));
    }

    // The shoreline foam patterned at distance because its noise is ANALYTIC — no mip chain — so
    // every octave kept full contrast at any number of cells per pixel, and each foam term then
    // thresholded that aliasing into hard speckle at a fixed world frequency. The streak term was
    // worse still: one octave of grid noise in the near-constant (alongshore, shoreward) frame,
    // i.e. a literal rectangular lattice locked to the coast. Lock both halves of the fix, and
    // lock that the legacy A/B path is still reachable.
    #[test]
    fn shoreline_foam_is_band_limited_and_has_no_fixed_frequency_lattice() {
        let shader = include_str!("water.wgsl");
        // Every procedural foam sample carries a footprint; none may go through raw vnoise.
        assert!(shader.contains("fn vnoise_lod(p: vec2<f32>, w: f32) -> f32"));
        assert!(
            shader.contains("fn foam_noise_lod(p_world: vec2<f32>, t: f32, w_world: f32) -> f32")
        );
        assert!(
            !shader.contains("fn foam_noise(p_world: vec2<f32>, t: f32) -> f32"),
            "the unfiltered entry point must be gone, not merely unused, or a later edit \
             will quietly call it again"
        );
        assert!(shader.contains(
            "let foam_footprint = max(length(dpdx(in.base_xz)), length(dpdy(in.base_xz)))"
        ));
        // The four fragment call sites each scale the footprint by their own position pre-scale.
        assert!(shader.contains("wp.time, foam_fp)"));
        assert!(shader.contains("wp.time, foam_fp * 1.5)"));
        assert!(shader.contains("wp.time, foam_fp * 4.6)"));
        assert!(shader.contains("wp.time, foam_fp * 2.7)"));
        // The lattice: two incommensurate alongshore octaves plus a warp, not one fixed 0.74.
        assert!(shader.contains("fn shoreline_streaks("));
        assert!(shader.contains("var s_warped = s + bend * 9.0"));
        assert!(
            !shader.contains("dot(in.base_xz, shoreline_tangent) * 0.74"),
            "the single-octave streak lattice must not survive"
        );
        // The second decorrelation pass went into the WARP, not into the octave sum. `combined` is
        // read through a threshold, so a third averaged octave would narrow the field's variance
        // and silently thin every streak; a domain warp leaves the one-point statistics alone.
        // Guard that intent explicitly — "add another octave" is the reflex a later edit will have.
        assert!(
            shader.contains("let combined = a * 0.58 + b * 0.42;"),
            "the streak octave sum must stay two-term: decorrelate via the warp, not the sum"
        );
        assert!(shader.contains("bend_long * 22.0"));
        assert!(shader.contains("d_warped = d + d_warp * 5.5"));
        // Both bend frequencies, and the shoreward warp, must stay off simple ratios for the same
        // reason the patch octaves below do: a "tidy" to round numbers rebuilds the beat.
        let bend_ratio = 0.031f32 / 0.0117f32;
        assert!(
            (bend_ratio - 2.0).abs() > 0.25 && (bend_ratio - 3.0).abs() > 0.25,
            "the two bend octaves must not sit at a simple ratio, got {bend_ratio}"
        );
        // Strength is a SEPARATE lever from the band limiting, and both keep a legacy path.
        assert!(shader.contains("let base_gain = select(0.20, 0.35, shore_foam_legacy > 0.5)"));
        assert!(shader.contains("let pattern_gain = select(0.66, 0.95, shore_foam_legacy > 0.5)"));
        // The deep-side extent became an override once the owner asked for it a second time:
        // pinning it as a literal here would mean a rebuild per candidate value, and the value is
        // settled by looking at a picture. The legacy arm keeps its literal 1.95.
        assert!(shader.contains("select(foam_band_outer, 1.95, shore_foam_legacy > 0.5)"));
        assert!(shader.contains("override foam_band_outer: f32 = 0.80;"));
        assert!(shader.contains("override shore_foam_legacy: f32 = 0.0;"));
        // The deep edge is ONE knob now: the fade's start is derived from band_outer instead of
        // being pinned at 0.35. That is what makes a cut to the number move the bright part of the
        // band and not only its faint outer skirt — the reason the previous pull-in read as
        // smaller than it was. The legacy arm keeps the literal 0.35 / 1.95 pair.
        assert!(shader.contains(
            "let band_fade_start = select(foam_band_outer * 0.32, 0.35, shore_foam_legacy > 0.5)"
        ));
        assert!(
            (1.10f32 * 0.32 - 0.35).abs() < 0.005,
            "the 0.32 factor must reproduce the previous 0.35 at the previous 1.10, or this is a \
             new band shape rather than a re-parametrisation of the old one"
        );
        // smoothstep(e, e, x) is undefined and WGR_WATER_FOAM_BAND_OUTER is a live env knob, so
        // the shader has to keep the two edges apart itself.
        assert!(shader.contains("band_fade_start + 0.02)"));

        // The band's deep edge MEANDERS: the anti-repetition work so far all changed what fills
        // the band, and a constant-width ribbon following a depth contour reads as stamped no
        // matter what is inside it. One-sided on purpose (the noise pair only ever scales ft UP),
        // so no stretch of coast gets foam further out than band_outer — the meander cannot undo
        // the pull-in the owner asked for.
        assert!(shader.contains("override foam_band_meander: f32 = 0.34;"));
        assert!(shader.contains("let meander_a = vnoise_lod(in.base_xz * 0.0817"));
        assert!(shader.contains("let meander_b = vnoise_lod(in.base_xz * 0.0341"));
        assert!(shader.contains("ft_deep = ft * (1.0 + max(foam_band_meander, 0.0) * meander)"));
        assert!(
            shader.contains("smoothstep(band_fade_start, band_outer, ft_deep)"),
            "the meander must feed the DEEP term only"
        );
        assert!(
            shader.contains("smoothstep(0.0, 0.06, ft) *"),
            "the LAND side must keep the raw ft: it carries the no-hard-line guarantee"
        );
        let meander_ratio = 0.0817f32 / 0.0341f32;
        assert!(
            (meander_ratio - 2.0).abs() > 0.25 && (meander_ratio - 3.0).abs() > 0.25,
            "the two meander octaves must not sit at a simple ratio, got {meander_ratio}"
        );
        // ...and must not sit at a simple ratio to the PATCH octaves either. Two independent
        // low-frequency fields at commensurate scales beat into one visible rhythm, which is the
        // lattice bug rebuilt an octave down out of two terms that each look fine alone.
        for meander in [0.0817f32, 0.0341] {
            for patch in [0.0461f32, 0.0290] {
                let r = meander / patch;
                assert!(
                    (r - 1.0).abs() > 0.1 && (r - 2.0).abs() > 0.1 && (r - 0.5).abs() > 0.05,
                    "meander {meander} and patch {patch} are commensurate (ratio {r})"
                );
            }
        }

        // Large-scale patchiness: two low-frequency octaves whose ratio is NOT a simple fraction
        // (0.0461 / 0.0290 = 1.59). Guard the ratio, not just the presence of the term — a later
        // "tidy" to 0.04 and 0.02 would restore exactly the commensurate beat the streak-lattice
        // fix removed, one scale up, and would look fine in a screenshot of a single beach.
        assert!(shader.contains("override foam_patch_gain: f32 = 1.0;"));
        assert!(shader.contains("let patch_a = vnoise_lod(in.base_xz * 0.0461"));
        assert!(shader.contains("let patch_b = vnoise_lod(in.base_xz * 0.0290"));
        let patch_ratio = 0.0461f32 / 0.0290f32;
        assert!(
            (patch_ratio - 2.0).abs() > 0.25 && (patch_ratio - 1.5).abs() > 0.05,
            "the two patch octaves must not sit at a simple ratio, got {patch_ratio}"
        );
        // The patch must be centred on 1.0 so it redistributes foam rather than adding it, and a
        // fully band-limited vnoise_lod returns 0.5 — so the far field must land on exactly 1.0
        // and switch the term off by construction rather than needing its own distance fade.
        let patch_at_full_filter: f64 = 0.52 + 0.96 * (0.5 * 0.62 + 0.5 * 0.38);
        assert!((patch_at_full_filter - 1.0).abs() < 1e-6);

        // The new gains must reduce peak drive but still saturate the strongest break, and the
        // legacy gains must reproduce the old 2.14 pre-clamp peak exactly.
        let peak = |base: f32, pattern: f32, brk: f32| (base + pattern) * (1.0 + brk);
        assert!((peak(0.35, 0.95, 0.65) - 2.145).abs() < 1e-4);
        let fixed = peak(0.20, 0.66, 0.45);
        assert!(fixed > 1.0, "the strongest surf must still reach white");
        assert!(
            fixed < 1.30,
            "but the band must not be clipped flat across most of its width"
        );

        // vnoise_lod returns the raw octave at a zero footprint, so the legacy path is unfiltered.
        assert!(shader.contains("if (detail >= 0.999) {"));
        let detail = |w: f32| 1.0 - smoothstep(0.30, 0.85, w);
        assert!((detail(0.0) - 1.0).abs() < 1e-6);
        assert!(
            detail(1.0) <= 0.001,
            "a sub-pixel octave must reach the field mean, not merely dim"
        );
        assert!(
            detail(0.5) > 0.0 && detail(0.5) < 1.0,
            "the handover must be gradual, not a step"
        );
    }

    fn smoothstep(e0: f32, e1: f32, x: f32) -> f32 {
        let t = ((x - e0) / (e1 - e0)).clamp(0.0, 1.0);
        t * t * (3.0 - 2.0 * t)
    }

    // naga_oil's compose test validates the WGSL but says nothing about pipeline-constant maps,
    // and wgpu REJECTS a pipeline whose `constants` names an override the module does not declare
    // — a panic during InitializeGraphicsEngine with no window and no log. `fs_constants` is
    // shared by the surface and whitewater pipelines, so adding `shore_foam_legacy` to it (rather
    // than to a second map) would take the renderer down. Compare the two constant maps against
    // the overrides the composed naga modules actually declare.
    //
    // (Building the real pipelines on a headless device also catches this, and was used to verify
    // it during development, but a fourth concurrent wgpu device in this process segfaults the
    // driver about one run in six. Composition reaches the same invariant with no GPU.)
    #[test]
    fn shoreline_foam_pipeline_constants_stay_within_their_own_shader() {
        use naga_oil::compose::NagaModuleDescriptor;

        let overrides_of = |source: &str, file_path: &str| -> Vec<String> {
            let mut composer = crate::shaders::build_composer();
            let module = composer
                .make_naga_module(NagaModuleDescriptor {
                    source,
                    file_path,
                    ..Default::default()
                })
                .unwrap_or_else(|e| panic!("{file_path}: {}", e.emit_to_string(&composer)));
            let mut names: Vec<String> = module
                .overrides
                .iter()
                .filter_map(|(_, o)| o.name.clone())
                .collect();
            names.sort();
            names
        };

        let surface = overrides_of(include_str!("water.wgsl"), "water/water.wgsl");
        let whitewater = overrides_of(
            include_str!("whitewater_render.wgsl"),
            "water/whitewater_render.wgsl",
        );

        // Both constant maps below are the literal arrays built in Water::new.
        for name in [
            "linear",
            "shore_foam_legacy",
            "foam_band_outer",
            "foam_patch_gain",
            "foam_band_meander",
        ] {
            assert!(
                surface.contains(&name.to_string()),
                "water_fs_constants names `{name}`, which water.wgsl must declare; \
                 it declares {surface:?}"
            );
        }
        assert!(whitewater.contains(&"linear".to_string()));
        assert!(
            !whitewater.contains(&"shore_foam_legacy".to_string()),
            "whitewater must keep the smaller fs_constants map, not water_fs_constants"
        );
        let module = include_str!("mod.rs");
        assert!(module.contains("let fs_constants = [(\"linear\", linear)];"));
        // water_fs_constants outgrew one line, so pin each ENTRY rather than the whole literal.
        // The invariant that matters is that every name checked against the shader above is
        // really in the array Water::new passes — not how rustfmt chose to wrap it. (Only
        // water_fs_constants has trailing commas per entry; the one-line fs_constants above does
        // not, so these cannot match it by accident.)
        for name in [
            "linear",
            "shore_foam_legacy",
            "foam_band_outer",
            "foam_patch_gain",
            "foam_band_meander",
        ] {
            assert!(
                module.contains(&format!("(\"{name}\", {name}),")),
                "water_fs_constants must pass `{name}` to the surface pipeline"
            );
        }
    }

    #[test]
    fn sunlight_catch_uses_the_godot_ocean_waves_light_model() {
        let shader = include_str!("water.wgsl");
        assert!(shader.contains("fn godot_smith_masking_shadowing"));
        assert!(shader.contains("fn godot_ggx_distribution"));
        assert!(shader.contains("fn godot_water_fresnel"));
        assert!(shader.contains("const GODOT_LIGHT_ROUGHNESS: f32 = 0.4"));
        assert!(shader.contains("let wave_height = state.displacement.y"));
        assert!(shader.contains("let sss_near = 0.5 * pow(godot_nv, 2.0)"));
    }

    // WTR-001 — the deterministic-freeze mask is bit-cast into WgrWaterParams.fft_control[2].
    // The legacy authored default (12.0 m minimum geometry wavelength) is preserved when no
    // freeze is requested: its bit pattern's low three bits are clean so it cannot accidentally
    // match a freeze bit, but encoding any non-zero mask rewrites the lane as the bit-cast u32.
    // This test locks both halves of the contract (legacy-safe + mask-decodable).
    #[test]
    fn freeze_mask_decodes_from_fft_control_z_without_breaking_legacy_default() {
        use bytemuck::Zeroable;
        const FREEZE_FFT: u32 = 1 << 0;
        const FREEZE_INTERACTION: u32 = 1 << 1;
        const FREEZE_FOAM: u32 = 1 << 2;

        // The legacy authored default is no freeze: 12.0f, whose IEEE-754 bits have low 3 bits
        // all zero (verified here so a re-encoded mask never collides with it via coincidence).
        let legacy_default = 12.0f32;
        assert_eq!(
            legacy_default.to_bits() & (FREEZE_FFT | FREEZE_INTERACTION | FREEZE_FOAM),
            0
        );

        // Encoding the all-freeze mask via the same std::mem::transmute-style path WaterWgpu uses
        // (bit-cast of the u32 mask to f32) yields a float whose .to_bits() returns the mask, so
        // Water::update_interactions round-trips the bits faithfully.
        let mut params = crate::ffi::WgrWaterParams::zeroed();
        params.fft_control[2] = f32::from_bits(FREEZE_FFT | FREEZE_INTERACTION | FREEZE_FOAM);
        let decoded = params.fft_control[2].to_bits();
        assert_eq!(decoded & FREEZE_FFT, FREEZE_FFT);
        assert_eq!(decoded & FREEZE_INTERACTION, FREEZE_INTERACTION);
        assert_eq!(decoded & FREEZE_FOAM, FREEZE_FOAM);

        // A masked-out (skipped) dispatch reads as the bit being ON; a normal frame reads as OFF.
        let normal = 0u32;
        assert_eq!(normal & FREEZE_FFT, 0);
        assert_eq!(normal & FREEZE_INTERACTION, 0);
        assert_eq!(normal & FREEZE_FOAM, 0);
    }

    // WTR-003 / WTR-LOOK — the debug-view selector rides WgrWaterParams.debug_params.x and the
    // surface energy model rides look_params.x, both appended at the struct end so every existing
    // lane keeps its offset. Lock the field offsets (192, 208) and the total size (224) so a
    // reorder on either side of the FFI boundary fails here, not as a silent UBO misread in the
    // shader. underwater_params (the Water tab's live underwater tuning) is the newest tail
    // lane and is locked the same way.
    // WRL-002 — Rust mirror of water_optics.wgsl, pinned against the analytical limits the
    // plan asks for (section 3.1): zero path, increasing depth, zero scattering, and the
    // reflection/transmission limits. If the WGSL constants change, change these too.
    mod optics_mirror {
        pub const NEUTRAL_HUE: [f32; 3] = [0.280, 0.065, 0.020];
        pub const NEUTRAL_MEAN: f32 = 0.1216;
        pub fn srgb_to_linear(c: f32) -> f32 {
            if c <= 0.04045 {
                c / 12.92
            } else {
                ((c + 0.055) / 1.055).powf(2.4)
            }
        }
        pub fn absorption_hue(deep: [f32; 3]) -> [f32; 3] {
            let lin = deep.map(|c| srgb_to_linear(c.clamp(1e-4, 1.0)));
            let absorb = lin.map(|c| -c.ln());
            let mean = (absorb[0] + absorb[1] + absorb[2]) / 3.0;
            absorb.map(|a| a * (NEUTRAL_MEAN / mean.max(1e-4)))
        }
        pub fn extinction(deep: [f32; 3], clarity: f32, bias: f32) -> [f32; 3] {
            let hue = absorption_hue(deep);
            let b = bias.clamp(0.0, 1.0);
            let scale = (clarity * 2.5).max(0.12);
            let scatter = clarity.max(0.0) * 1.6;
            [0, 1, 2].map(|i| (NEUTRAL_HUE[i] * (1.0 - b) + hue[i] * b) * scale + scatter)
        }
        pub fn transmittance(sigma_t: [f32; 3], path: f32) -> [f32; 3] {
            sigma_t.map(|s| (-s.max(0.0) * path.max(0.0)).exp())
        }
        pub fn inscatter(albedo: [f32; 3], light: [f32; 3], t: [f32; 3]) -> [f32; 3] {
            [0, 1, 2].map(|i| {
                albedo[i] * light[i] / std::f32::consts::PI * (1.0 - t[i].clamp(0.0, 1.0))
            })
        }
    }

    #[test]
    fn shared_optics_mirror_matches_the_wgsl_constants() {
        let wgsl = include_str!("water_optics.wgsl");
        assert!(wgsl.contains("#define_import_path water_optics"));
        assert!(wgsl.contains("vec3<f32>(0.280, 0.065, 0.020)"));
        assert!(wgsl.contains("NEUTRAL_ABSORPTION_MEAN: f32 = 0.1216"));
        assert!(wgsl.contains("max(clarity * 2.5, 0.12)"));
        assert!(wgsl.contains("max(clarity, 0.0) * 1.6"));
        // The neutral mean really is the mean of the neutral curve.
        let m = (0.280_f32 + 0.065 + 0.020) / 3.0;
        assert!((m - optics_mirror::NEUTRAL_MEAN).abs() < 1e-3);
    }

    #[test]
    fn shared_optics_analytical_limits() {
        use optics_mirror::*;
        let deep = [0.014_f32, 0.105, 0.240]; // shipped deep swatch
        let sigma_t = extinction(deep, 0.16, 1.0);
        for s in sigma_t {
            assert!(s > 0.0 && s.is_finite());
        }
        // Zero path: nothing transmitted is lost and nothing is scattered in — a dry ray is
        // the background exactly.
        let t0 = transmittance(sigma_t, 0.0);
        assert_eq!(t0, [1.0, 1.0, 1.0]);
        assert_eq!(inscatter([0.1, 0.3, 0.4], [1.0, 1.0, 1.0], t0), [0.0, 0.0, 0.0]);
        // A negative path (numerical noise on a dry ray) never amplifies.
        assert_eq!(transmittance(sigma_t, -3.0), [1.0, 1.0, 1.0]);
        // Monotone: more water, less background, more body; red dies before blue.
        let mut prev_t = t0;
        for path in [0.5_f32, 1.0, 2.0, 5.0, 10.0, 40.0] {
            let t = transmittance(sigma_t, path);
            for i in 0..3 {
                assert!(t[i] < prev_t[i], "T must fall with path ({path} m, ch {i})");
            }
            assert!(t[0] < t[2], "red must be absorbed faster than blue at {path} m");
            prev_t = t;
        }
        // Deep limit: the body saturates at albedo * light / PI and the background is gone.
        let t_deep = transmittance(sigma_t, 400.0);
        let body = inscatter([0.1, 0.3, 0.4], [3.0, 3.0, 3.0], t_deep);
        for i in 0..3 {
            assert!(t_deep[i] < 1e-6);
            let limit = [0.1_f32, 0.3, 0.4][i] * 3.0 / std::f32::consts::PI;
            assert!((body[i] - limit).abs() < 1e-5);
        }
        // Zero clarity: absorption floors at 0.12/m of the hue and scattering is exactly zero,
        // so the extinction is finite, positive, and no division by zero anywhere.
        let clear = extinction(deep, 0.0, 1.0);
        let hue = absorption_hue(deep);
        for i in 0..3 {
            assert!((clear[i] - hue[i] * 0.12).abs() < 1e-6);
        }
        // Bias 0 reproduces the retired neutral curve exactly (compatibility endpoint).
        let neutral = extinction(deep, 0.16, 0.0);
        for i in 0..3 {
            assert!((neutral[i] - (NEUTRAL_HUE[i] * 0.4 + 0.256)).abs() < 1e-6);
        }
        // A black swatch still yields a finite hue (the 1e-4 clamp), never NaN.
        for v in absorption_hue([0.0, 0.0, 0.0]) {
            assert!(v.is_finite());
        }
    }

    #[test]
    fn shared_optics_composite_owns_the_background_once() {
        let shader = include_str!("water.wgsl");
        // The physical path composites the background itself...
        assert!(shader.contains(
            "let shared_optics_transmitted = background * transmittance + body_radiance;"
        ));
        // ...falls back to the pixel's own opaque sample rather than dropping the background...
        assert!(shader.contains(
            "let background = select(own_background, refracted.color, refracted.valid > 0.5);"
        ));
        // ...and therefore returns geometric coverage only, never a Fresnel-raised alpha that
        // would blend the background in a second time.
        assert!(shader.contains("let physical_alpha = max(shore, combined_foam) * body_coverage;"));
        assert!(shader.contains(
            "let alpha = select(legacy_alpha, physical_alpha, physical_look && shared_optics);"
        ));
        // The surface fogs a submerged view only when the compositor does not own that segment.
        assert!(shader.contains("if (is_underwater && !(physical_look && shared_optics)) {"));
        // One extinction: no private curve survives in the surface shader.
        assert!(
            !shader.contains("vec3<f32>(0.280, 0.065, 0.020)"),
            "surface carries a private extinction curve"
        );
        assert!(shader.contains("#import water_optics::"));
    }

    #[test]
    fn bounded_bodies_draw_inside_their_ellipse_at_their_own_level() {
        let shader = include_str!("water.wgsl").replace("\r\n", "\n");
        // The vertex stage takes the level from the node's body, the ocean keeps sea_level...
        assert!(shader.contains("@location(7) body: f32,"));
        assert!(shader.contains("var surface_level = wp.sea_level;"));
        assert!(shader.contains("surface_level = b.x;"));
        // ...the seabed clamp caps at THAT level, not the sea's (or a lake above sea level would
        // be flattened onto the sea)...
        assert!(shader.contains("fn clamp_to_seabed_height(seabed_y: f32, y: f32, surface_level: f32)"));
        assert!(shader.contains("let floor_y = min(seabed_y + 0.02, surface_level);"));
        assert!(!shader.contains("min(seabed_y + 0.02, wp.sea_level)"));
        // ...the fragment stage discards outside the containment ellipse and fades the rim...
        assert!(shader.contains("if (d > 1.06) {\n            discard;"));
        assert!(shader.contains("body_coverage = 1.0 - smoothstep(0.94, 1.03, d);"));
        // ...and a body never samples the sea's planar reflection as its own.
        assert!(shader.contains("if (body_index == 0) {\n            planar_refl = planar_reflection("));
        // Rust mirror of the containment: box corner outside, axis ends inside, rim soft.
        let contains = |x: f32, z: f32, cx: f32, cz: f32, rx: f32, rz: f32| {
            let u = (x - cx) / rx;
            let v = (z - cz) / rz;
            u * u + v * v <= 1.06
        };
        assert!(contains(1099.0, 2000.0, 1000.0, 2000.0, 100.0, 50.0));
        assert!(!contains(1095.0, 2045.0, 1000.0, 2000.0, 100.0, 50.0));
    }

    #[test]
    fn rivers_advect_in_two_phases_and_end_at_the_sea() {
        let shader = include_str!("water.wgsl");
        // Oriented reach: containment and the compositor use the body's heading.
        assert!(shader.contains("let fc = cos(f.w);"));
        assert!(include_str!("../underwater.wgsl").contains("let fc = params.body_frame.x;"));
        assert!(include_str!("../underwater_froxel.wgsl").contains("let fc = params.body_frame.x;"));
        // Two phases half a period apart with triangle weights: at any time the weights sum to
        // one and neither phase is sampled at its own reset, so there is no pulse.
        assert!(shader.contains("let t2 = fract(wp.time / period + 0.5);"));
        assert!(shader.contains("let w1 = 1.0 - abs(2.0 * t1 - 1.0);"));
        for t in [0.0_f32, 0.1, 0.25, 0.5, 0.75, 0.99] {
            let t1 = t.fract();
            let t2 = (t + 0.5).fract();
            let w1 = 1.0 - (2.0 * t1 - 1.0).abs();
            let w2 = 1.0 - (2.0 * t2 - 1.0).abs();
            assert!((w1 + w2 - 1.0).abs() < 1e-5, "weights at {t}: {w1} + {w2}");
            // the phase at its reset (t == 0) carries zero weight
            if t1 < 1e-6 { assert!(w1 < 1e-5); }
        }
        // The mouth fade hands the last 0.6 m of fall to the sea.
        assert!(shader.contains("smoothstep(0.0, 0.6, in.body_level - wp.sea_level)"));
        // Streaks only on a moving reach, strongest at the bank.
        assert!(shader.contains("if (body_kind == 2 && body_flow_speed > 0.3) {"));
    }

    #[test]
    fn whitecaps_need_wind_at_every_source() {
        // The three whitecap sources read the same gate, and the prefix-declared WaterParams
        // structs in the foam and spray shaders reach optics_params (a prefix struct that
        // stops short of it would read zero and silently disable the gate).
        for (name, text) in [
            ("foam.wgsl", include_str!("foam.wgsl")),
            ("water.wgsl", include_str!("water.wgsl")),
            ("whitewater_render.wgsl", include_str!("whitewater_render.wgsl")),
        ] {
            assert!(text.contains("optics_params"), "{name} cannot see optics_params");
            assert!(text.contains("whitecap_wind_factor("), "{name} does not gate on wind");
            assert!(text.contains("water_optics::"), "{name} must import the shared gate, not copy it");
        }
        // Rust mirror of the gate's anchors: nothing at a calm 3 m/s, everything at the 12 m/s
        // reference sea, and monotone between.
        let gate = |u: f32| {
            let t = ((u - 3.5) / 8.5).clamp(0.0, 1.0);
            let s = t * t * (3.0 - 2.0 * t);
            s * s
        };
        assert_eq!(gate(3.0), 0.0);
        assert_eq!(gate(12.0), 1.0);
        assert!(gate(5.0) < 0.05, "a 5 m/s sea should carry almost no whitecaps: {}", gate(5.0));
        assert!(gate(8.0) > gate(6.0) && gate(10.0) > gate(8.0));
        let wgsl = include_str!("water_optics.wgsl");
        assert!(wgsl.contains("smoothstep(3.5, 12.0, max(wind_speed, 0.0))"));
    }

    #[test]
    fn bicubic_dynamics_matches_sixteen_tap_reference() {
        let shader = include_str!("water.wgsl");
        // A whole-file search used to pass when only displacement was fixed,
        // while dynamics (normal/glitter) retained the broken reconstruction.
        for name in ["texture_bicubic_displacement_at", "texture_bicubic_dynamics"] {
            let body = shader.split(&format!("fn {name}("))
                .nth(1).expect("bicubic function").split("\nfn ").next().unwrap();
            for term in ["wx.x + wx.y, wx.z + wx.w, wy.x + wy.y, wy.z + wy.w",
                         "wx.y / g.x", "wx.w / g.y", "wy.y / g.z", "wy.w / g.w",
                         "let w = g.yw / (g.xz + g.yw)"] {
                assert!(body.contains(term), "{name}: missing {term}");
            }
            assert!(!body.contains("wx.x + wx.z"), "{name}: non-adjacent taps");
        }
        fn weights(t: f64) -> [f64; 4] {
            [ (1.0-t).powi(3)/6.0, (3.0*t.powi(3)-6.0*t*t+4.0)/6.0,
              (-3.0*t.powi(3)+3.0*t*t+3.0*t+1.0)/6.0, t.powi(3)/6.0 ]
        }
        let texel = |x: i32, y: i32| ((x.rem_euclid(8)*13 + y.rem_euclid(8)*7) % 19) as f64;
        let bilinear = |x: f64, y: f64| {
            let ix = x.floor() as i32;
            let iy = y.floor() as i32;
            let fx = x-x.floor();
            let fy = y-y.floor();
            (texel(ix,iy)*(1.0-fx)+texel(ix+1,iy)*fx)*(1.0-fy)
                + (texel(ix,iy+1)*(1.0-fx)+texel(ix+1,iy+1)*fx)*fy
        };
        for x in [-1.00001_f64, -1.0, -0.99999, 0.0, 0.13, 0.49, 0.93, 7.99999, 8.0] {
            for y in [-1.00001_f64, -0.99999, 0.0, 0.07, 0.76, 7.99999, 8.0] {
                let wx = weights(x-x.floor());
                let wy = weights(y-y.floor());
                let gx = [wx[0]+wx[1], wx[2]+wx[3]];
                let gy = [wy[0]+wy[1], wy[2]+wy[3]];
                let hx = [x.floor()-1.0+wx[1]/gx[0], x.floor()+1.0+wx[3]/gx[1]];
                let hy = [y.floor()-1.0+wy[1]/gy[0], y.floor()+1.0+wy[3]/gy[1]];
                let mut four = 0.0;
                let mut sixteen = 0.0;
                for j in 0..2 { for i in 0..2 { four += bilinear(hx[i],hy[j])*gx[i]*gy[j]; } }
                for j in 0..4 { for i in 0..4 {
                    sixteen += texel(x.floor() as i32+i as i32-1, y.floor() as i32+j as i32-1)*wx[i]*wy[j];
                } }
                assert!((four-sixteen).abs() < 1e-10, "{x}/{y}: {four} vs {sixteen}");
            }
        }
    }

    #[test]
    fn debug_params_appended_without_shifting_existing_lanes() {
        use crate::ffi::WgrWaterParams;
        assert_eq!(std::mem::size_of::<WgrWaterParams>(), 1472);
        assert_eq!(
            std::mem::offset_of!(WgrWaterParams, debug_params),
            192,
            "debug_params must sit at the struct end so earlier lanes keep their offsets"
        );
        assert_eq!(
            std::mem::offset_of!(WgrWaterParams, look_params),
            208,
            "look_params must be appended after debug_params, not inserted before it"
        );
        assert_eq!(
            std::mem::offset_of!(WgrWaterParams, sea_params),
            224,
            "sea_params must be appended after look_params, not inserted before it"
        );
        assert_eq!(
            std::mem::offset_of!(WgrWaterParams, underwater_params),
            240,
            "underwater_params must be appended after sea_params, not inserted before it"
        );
        assert_eq!(
            std::mem::offset_of!(WgrWaterParams, underwater_gate),
            256,
            "underwater_gate must be appended after underwater_params, not inserted before it"
        );
        assert_eq!(
            std::mem::offset_of!(WgrWaterParams, optics_params),
            272,
            "optics_params must be appended after underwater_gate, not inserted before it"
        );
        assert_eq!(
            std::mem::offset_of!(WgrWaterParams, bodies),
            288,
            "the body table must be appended after optics_params, not inserted before it"
        );
        // The node's body lane replaced a padding float: same size, same offset as the pad.
        assert_eq!(std::mem::size_of::<crate::ffi::WgrWaterNode>(), 40);
        assert_eq!(std::mem::offset_of!(crate::ffi::WgrWaterNode, body), 36);
        // flow_direction_speed (the previous last field) must not have moved.
        assert_eq!(
            std::mem::offset_of!(WgrWaterParams, flow_direction_speed),
            176
        );
    }
}

fn build_curl_group(device:&wgpu::Device,layout:&wgpu::BindGroupLayout,params:&wgpu::Buffer,
    sky:&wgpu::TextureView,sky_sampler:&wgpu::Sampler,fft:&wgpu::TextureView,fft_sampler:&wgpu::Sampler,
    bed:&wgpu::TextureView,conform:&wgpu::Buffer,scene:&wgpu::TextureView,scene_sampler:&wgpu::Sampler)->wgpu::BindGroup {
    device.create_bind_group(&wgpu::BindGroupDescriptor {label:Some("curl_without_depth_feedback"),layout,
        entries:&[
            wgpu::BindGroupEntry {binding:0,resource:params.as_entire_binding()},
            wgpu::BindGroupEntry {binding:2,resource:wgpu::BindingResource::TextureView(sky)},
            wgpu::BindGroupEntry {binding:3,resource:wgpu::BindingResource::Sampler(sky_sampler)},
            wgpu::BindGroupEntry {binding:6,resource:wgpu::BindingResource::TextureView(fft)},
            wgpu::BindGroupEntry {binding:9,resource:wgpu::BindingResource::Sampler(fft_sampler)},
            wgpu::BindGroupEntry {binding:17,resource:wgpu::BindingResource::TextureView(bed)},
            wgpu::BindGroupEntry {binding:18,resource:conform.as_entire_binding()},
            wgpu::BindGroupEntry {binding:12,resource:wgpu::BindingResource::TextureView(scene)},
            wgpu::BindGroupEntry {binding:13,resource:wgpu::BindingResource::Sampler(scene_sampler)},
        ]})
}
