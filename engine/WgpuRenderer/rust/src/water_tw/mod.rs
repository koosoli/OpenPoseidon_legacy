//! Tidewater Native — the second water backend (Tidewater Water Plan v5), a port of
//! dgreenheck/tidewater @ 4811ba48 (MIT, © DRG Software Solutions LLC; see CREDITS.md).
//!
//! W2 (this file): the open sea. The multi-cascade FFT (`fft.rs`), its compute mip chain, the
//! sea-detail and foam-pattern textures (`textures.rs`), the CDLOD surface (`cdlod.rs`) and
//! the water material with SSR + sky/cloud panorama + absorption (`tw_water.wgsl`).
//!
//! Owns every resource it uses; `WaterBackend` drops the whole struct when switching away, so
//! nothing here runs or stays resident while Current OP is active (plan §4.2).
//!
//! W3a: the shoreline waves — ShoreField (`shore_field.rs`, computed on a worker thread from
//! the CPU heightmap) and ShoreWaves (`tw_shore.wgsl`): travel-time phase, Green's-law shoaling,
//! plunge, bore, swash run-up, the per-pixel swash front, crest translucency and the surf-zone
//! medium.
//!
//! W3b: ShoreSim (`shore_sim.rs`, `tw_sim.wgsl`, `tw_sim_kernel.wgsl`) in up to two regions on
//! the coast nearest the camera, the lace texture (`lace.rs`), the material's SIM path, and the
//! wet sand / stranded foam it hands to OP's terrain (`shore_wetness`).
//!
//! W3c: the surf-zone foam look (SurfFoam.js, `tw_surf_foam.wgsl`): world-space lace carried by the
//! simulated flow, whitewater lumps with self-shadowed crevices, foam lighting.
//!
//! W3d: Breakers (`breakers.rs`, `tw_breakers_kernel.wgsl`, `tw_lip.wgsl`): the thrown lip of
//! plunging breakers along the shoreline inside the simulated regions.
//!
//! W4a: spray (`spray.rs`, `tw_spray_emit.wgsl`, `tw_spray_update.wgsl`, `tw_spray.wgsl`): drops,
//! ligaments, dense spray and mist thrown by the breakers, integrated on the GPU and drawn as soft
//! sprites after the lips.
//!
//! Not yet, by design (later packages):
//! boat spray and RefractionPass (W4b), underwater and caustics (W5), boat wakes (W6). Inland water
//! bodies are not drawn in this mode yet (see TW-WATER-01, open question for the owner).

mod breakers;
mod cdlod;
mod fft;
mod lace;
mod shore_field;
mod shore_sim;
pub mod spray;
mod underwater;
mod textures;

use std::sync::Arc;

use crate::ffi::{WgrCamera, WgrWaterParams};

/// The water shader: the material plus the shoreline-wave module appended to it (one module).
const TW_WATER_WGSL: &str = concat!(
    include_str!("tw_water.wgsl"),
    "\n",
    include_str!("tw_common.wgsl"),
    "\n",
    include_str!("tw_shore.wgsl"),
    "\n",
    include_str!("tw_sim.wgsl"),
    "\n",
    include_str!("tw_surf_foam.wgsl"),
    "\n",
    include_str!("tw_lip.wgsl"),
    "\n",
    include_str!("tw_spray_common.wgsl"),
    "\n",
    include_str!("tw_spray.wgsl"),
);

/// The breaker crest finder with the spray emitters, and the spray update: shared declarations,
/// the shore waves, the spray helpers and emitters, the kernel, the update.
const TW_BRK_KERNEL_WGSL: &str = concat!(
    include_str!("tw_common.wgsl"),
    "\n",
    include_str!("tw_shore.wgsl"),
    "\n",
    include_str!("tw_spray_common.wgsl"),
    "\n",
    include_str!("tw_spray_emit.wgsl"),
    "\n",
    include_str!("tw_breakers_kernel.wgsl"),
    "\n",
    include_str!("tw_spray_update.wgsl"),
);

/// The shore simulation kernel: the shared declarations, the shore waves (shoreEvaluateWorld),
/// the state readers and the kernel itself.
const TW_SIM_KERNEL_WGSL: &str = concat!(
    include_str!("tw_common.wgsl"),
    "\n",
    include_str!("tw_shore.wgsl"),
    "\n",
    include_str!("tw_sim.wgsl"),
    "\n",
    include_str!("tw_sim_kernel.wgsl"),
);

/// Whether this build carries a working Tidewater backend.
pub const AVAILABLE: bool = true;

/// `WgrWaterParams::tidewater` defaults: Tidewater's own values (App.js / OceanFFT.js /
/// WaterMaterial.js / SeaDetail.js at 4811ba48). Lane meanings are in wgpu_renderer.hpp.
/// `WGR_TW_DEBUG=<n>` forces Tidewater's material debug view (the Water tab's "TW debug view":
/// 6 path length, 7 sea bed seen through, 8 sea depth, ...) for a session, so a capture script can
/// take the debug views without a Water-tab edit. Read once.
fn debug_view_override() -> Option<f32> {
    static V: std::sync::OnceLock<Option<f32>> = std::sync::OnceLock::new();
    *V.get_or_init(|| std::env::var("WGR_TW_DEBUG").ok().and_then(|v| v.trim().parse::<f32>().ok()))
}

/// `WGR_TW_SPRAY=<gain>,<intensity>`: overrides the Water tab's breaker spray (W4a), for captures.
fn spray_override() -> Option<(f32, f32)> {
    static V: std::sync::OnceLock<Option<(f32, f32)>> = std::sync::OnceLock::new();
    *V.get_or_init(|| {
        let v = std::env::var("WGR_TW_SPRAY").ok()?;
        let mut it = v.split(',').map(|x| x.trim().parse::<f32>().ok());
        Some((it.next()??, it.next().flatten().unwrap_or(1.0)))
    })
}

/// `WGR_TW_GLOW=<scale>`: overrides the Water tab's water glow (W3j), for A/B captures.
fn glow_override() -> Option<f32> {
    static V: std::sync::OnceLock<Option<f32>> = std::sync::OnceLock::new();
    *V.get_or_init(|| std::env::var("WGR_TW_GLOW").ok().and_then(|v| v.trim().parse::<f32>().ok()))
}

pub const DEFAULT_PARAMS: [[f32; 4]; 4] = [
    [1.0, 0.9, 0.48, 5.0],
    [1.0, 1.0, 1.0, 1.0],
    [1.0, 0.035, 1.0, 0.035],
    [1.0, 1.0, 0.3, 0.0],
];

/// `WgrWaterParams::tidewater_shore` defaults: ShoreWaves.js ShoreParams at 4811ba48.
pub const DEFAULT_SHORE_PARAMS: [[f32; 4]; 3] = [
    [1.0, 9.0, 0.34, 0.55],
    [0.78, 0.13, 1.0, 1.0],
    [0.16, 1.0, 28.0, 1.0],
];

/// `WgrWaterParams::tidewater_fx` defaults: Breakers `spray` and Spray `intensity` (W4), the
/// water glow (W3j: scale of the light scattered inside the water; 1 = Tidewater's formula on
/// OP's light, which reads pale cyan; 0, the owner's pick, = the surface's reflection and the
/// attenuated sea bed only).
pub const DEFAULT_FX_PARAMS: [f32; 4] = [1.0, 1.0, GLOW_DEFAULT, 0.0];
/// See `DEFAULT_FX_PARAMS`; must match `TidewaterLook::waterGlow` on the C++ side.
pub const GLOW_DEFAULT: f32 = 0.0;

/// The swell scale Tidewater's shore amplitude goes with (OceanFFT's swell system `scale`).
const DEFAULT_SWELL_SCALE: f32 = 0.48;

/// Shore field placement for the shader (origin, cell, centre); None until the first field.
#[derive(Clone, Copy, Debug, PartialEq)]
struct ShoreGrid {
    origin: [f32; 2],
    cell: f32,
    centre: [f32; 2],
}

/// Tidewater's `frame.waterAbsorption` / `frame.waterScattering` defaults (Frame.js), 1/m.
const WATER_ABSORPTION: [f32; 3] = [0.42, 0.075, 0.035];
const WATER_SCATTERING: [f32; 3] = [0.012, 0.018, 0.024];

/// `TwSurface` in tw_water.wgsl.
#[repr(C)]
#[derive(Clone, Copy, Debug, bytemuck::Pod, bytemuck::Zeroable)]
struct TwSurface {
    sea: [f32; 4],
    wind: [f32; 4],
    surface: [f32; 4],
    material0: [f32; 4],
    material1: [f32; 4],
    absorption: [f32; 4],
    scattering: [f32; 4],
    detail: [f32; 4],
    shore0: [f32; 4],
    shore1: [f32; 4],
    shore2: [f32; 4],
    shore_grid: [f32; 4],
    sim: [[f32; 4]; shore_sim::K],
    sim_p0: [f32; 4],
    sim_p1: [f32; 4],
    brk: [f32; 4],
    morph: [[f32; 4]; cdlod::LEVELS],
}

const _: () = assert!(std::mem::size_of::<TwSurface>() == 16 * (17 + cdlod::LEVELS));

/// Whether the shore simulation runs (Water tab, and only once the shore waves have a field).
/// Breaker spray on (W4a): emission gain and sprite intensity above zero (the caller also needs
/// an active shore simulation and stations).
fn spray_on(p: &WgrWaterParams) -> bool {
    p.tidewater_fx[0] > 0.0 && p.tidewater_fx[1] > 0.0
}

fn shore_sim_on(p: &WgrWaterParams, grid: Option<ShoreGrid>) -> bool {
    grid.is_some() && p.tidewater_shore[0][0] > 0.5 && p.tidewater_shore[2][1] > 0.5
}

/// ShoreSimParams (ShoreSim.js) for this frame.
fn sim_lanes(p: &WgrWaterParams, grid: Option<ShoreGrid>, dt: f32) -> ([f32; 4], [f32; 4]) {
    let dry = if p.tidewater_shore[2][2] > 1.0 { p.tidewater_shore[2][2] } else { shore_sim::DRY_TIME };
    (
        [dry, shore_sim::FOAM_LIFE, shore_sim::SURF_FOAM_LIFE, shore_sim::RESIDUE_LIFE],
        [shore_sim::FOAM_GEN, shore_sim::DEPOSIT_GAIN, dt, if shore_sim_on(p, grid) { 1.0 } else { 0.0 }],
    )
}

/// ShoreParams for the shader. The amplitude follows the swell scale relative to Tidewater's
/// default (an OP adaptation: Tidewater has one sea state, OP's weather and reference seas move
/// the swell), and the waves stay off until a shore field exists.
fn shore_lanes(p: &WgrWaterParams, grid: Option<ShoreGrid>) -> ([f32; 4], [f32; 4], [f32; 4], [f32; 4]) {
    let sh = p.tidewater_shore;
    let swell = if p.tidewater[0][2] >= 0.0 { p.tidewater[0][2] } else { DEFAULT_SWELL_SCALE };
    let amp = sh[0][2].max(0.0) * (swell / DEFAULT_SWELL_SCALE).clamp(0.25, 2.5);
    let enabled = if grid.is_some() && sh[0][0] > 0.5 { 1.0 } else { 0.0 };
    let period = if sh[0][1] > 0.5 { sh[0][1] } else { 9.0 };
    let gamma = if sh[1][0] > 0.05 { sh[1][0] } else { 0.78 };
    let span = if sh[1][1] > 0.01 { sh[1][1] } else { 0.13 };
    let g = grid.unwrap_or(ShoreGrid { origin: [0.0; 2], cell: 1.0, centre: [0.0; 2] });
    (
        [period, amp, sh[0][3].max(0.0), gamma],
        [span, sh[1][2].max(0.0), sh[1][3].max(0.0), enabled],
        [sh[2][0].max(0.0), p.tidewater[0][3].to_radians(), g.centre[0], g.centre[1]],
        [g.origin[0], g.origin[1], g.cell, 0.0],
    )
}

/// The swell's travel direction in the world (x, z): the swell system's heading, as fft.rs uses it.
fn swell_dir(p: &WgrWaterParams) -> [f32; 2] {
    let a = p.tidewater[0][3].to_radians();
    [a.cos(), a.sin()]
}

struct FrameInputs<'a> {
    detail_offset: [f32; 2],
    morph: &'a [[f32; 4]; cdlod::LEVELS],
    shore: Option<ShoreGrid>,
    sim: [[f32; 4]; shore_sim::K],
    dt: f32,
    stations: u32,
}

fn surface_block(p: &WgrWaterParams, f: &FrameInputs<'_>) -> TwSurface {
    let tw = p.tidewater;
    let detail_offset = f.detail_offset;
    let (shore0, shore1, shore2, shore_grid) = shore_lanes(p, f.shore);
    let (sim_p0, sim_p1) = sim_lanes(p, f.shore, f.dt);
    let wind_len = (p.fft_wind_sea[0].powi(2) + p.fft_wind_sea[1].powi(2)).sqrt();
    let wind_dir = if wind_len > 1e-4 {
        [p.fft_wind_sea[0] / wind_len, p.fft_wind_sea[1] / wind_len]
    } else {
        // Tidewater's default heading
        [0.3494, 0.9370]
    };
    let mut wind_speed = if p.fft_wind_sea[2] > 0.0 { p.fft_wind_sea[2] } else { 7.0 };
    // W3g: a sea-conditions preset brings its own wind (the spectrum uses it too, fft.rs)
    if p.tidewater_sea[0] > 0.0 {
        wind_speed = p.tidewater_sea[0];
    }
    // WRL-003: the level under the camera when the producer supplied one, else the sea.
    let cam_water = if p.optics_params[3] > 0.0 { p.optics_params[2] } else { p.sea_level };
    TwSurface {
        sea: [p.sea_level, p.time, wind_speed, cam_water],
        wind: [wind_dir[0], wind_dir[1], detail_offset[0], detail_offset[1]],
        // amplitude, slope scale, foam coverage, foam pattern scale (WaterSurface.js)
        surface: [tw[0][0], 1.0, tw[1][0], 0.09],
        // backscatter, sss, foam intensity, reflection strength
        material0: [tw[2][3], tw[2][2], tw[1][1], tw[1][3]],
        // roughness, ssr on, debug view, water glow (W3j; tidewater_fx.z)
        material1: [tw[2][1], tw[1][2], debug_view_override().unwrap_or(tw[3][3]), glow_override().unwrap_or(p.tidewater_fx[2]).clamp(0.0, 2.0)],
        absorption: [WATER_ABSORPTION[0], WATER_ABSORPTION[1], WATER_ABSORPTION[2], 0.0],
        scattering: [WATER_SCATTERING[0], WATER_SCATTERING[1], WATER_SCATTERING[2], 0.0],
        detail: [tw[3][0], tw[3][1], tw[3][2], 0.0],
        shore0,
        shore1,
        shore2,
        shore_grid,
        sim: f.sim,
        sim_p0,
        sim_p1,
        // Breakers: station count, lip opacity (Tidewater `sheet`, Water tab lane)
        brk: [f.stations as f32, p.tidewater_shore[2][3].clamp(0.0, 2.0), 0.0, 0.0],
        morph: *f.morph,
    }
}

/// Tidewater's wet sand for OP's terrain (App.js `terrain.wetness`): the shore simulation state
/// (wetness, stranded foam) and the lace the stranded foam is drawn with.
pub struct ShoreWetness<'a> {
    pub state: &'a wgpu::TextureView,
    pub lace: &'a wgpu::TextureView,
    /// changes when the views change (terrain rebuilds its bind group)
    pub generation: u64,
    /// per region: min xz, size, weight (as tw.sim)
    pub regions: [[f32; 4]; 2],
    pub sea_level: f32,
}

/// A texture with data, read by textureLoad only.
fn data_texture(
    device: &wgpu::Device,
    queue: &wgpu::Queue,
    label: &str,
    format: wgpu::TextureFormat,
    (w, h): (u32, u32),
    bytes_per_texel: u32,
    data: &[u8],
) -> wgpu::TextureView {
    let tex = device.create_texture(&wgpu::TextureDescriptor {
        label: Some(label),
        size: wgpu::Extent3d { width: w, height: h, depth_or_array_layers: 1 },
        mip_level_count: 1,
        sample_count: 1,
        dimension: wgpu::TextureDimension::D2,
        format,
        usage: wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::COPY_DST,
        view_formats: &[],
    });
    queue.write_texture(
        wgpu::TexelCopyTextureInfo { texture: &tex, mip_level: 0, origin: wgpu::Origin3d::ZERO, aspect: wgpu::TextureAspect::All },
        data,
        wgpu::TexelCopyBufferLayout { offset: 0, bytes_per_row: Some(w * bytes_per_texel), rows_per_image: Some(h) },
        wgpu::Extent3d { width: w, height: h, depth_or_array_layers: 1 },
    );
    tex.create_view(&Default::default())
}

/// TerrainGPU's placeholder until the shore field is set (1 texel, T = 1e4, no waves).
fn placeholder_shore_views(device: &wgpu::Device, queue: &wgpu::Queue) -> (wgpu::TextureView, wgpu::TextureView) {
    let t: [f32; 2] = [1e4, 1e4];
    let d: [i8; 4] = [0; 4];
    (
        data_texture(device, queue, "tw_shore_t_placeholder", wgpu::TextureFormat::Rg32Float, (1, 1), 8, bytemuck::cast_slice(&t)),
        data_texture(device, queue, "tw_shore_dir_placeholder", wgpu::TextureFormat::Rgba8Snorm, (1, 1), 4, bytemuck::cast_slice(&d)),
    )
}

fn dummy_view(device: &wgpu::Device, label: &str, format: wgpu::TextureFormat) -> wgpu::TextureView {
    device
        .create_texture(&wgpu::TextureDescriptor {
            label: Some(label),
            size: wgpu::Extent3d { width: 1, height: 1, depth_or_array_layers: 1 },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format,
            usage: wgpu::TextureUsages::TEXTURE_BINDING,
            view_formats: &[],
        })
        .create_view(&Default::default())
}

pub struct Tidewater {
    fft: fft::OceanFft,
    cdlod: cdlod::Cdlod,
    surface_ubo: wgpu::Buffer,
    conform_ubo: wgpu::Buffer,
    foam_view: wgpu::TextureView,
    detail_view: wgpu::TextureView,
    smp_linear_repeat: wgpu::Sampler,
    smp_aniso_repeat: wgpu::Sampler,
    smp_linear_clamp: wgpu::Sampler,
    env_sampler: wgpu::Sampler,
    scene_view: wgpu::TextureView,
    depth_view: wgpu::TextureView,
    env_view: wgpu::TextureView,
    heightmap_view: wgpu::TextureView,
    layout: wgpu::BindGroupLayout,
    bind: wgpu::BindGroup,
    bind_dirty: bool,
    pipeline: wgpu::RenderPipeline,
    grid_vbuf: wgpu::Buffer,
    grid_ibuf: wgpu::Buffer,
    grid_index_count: u32,
    instance_buf: wgpu::Buffer,
    instance_count: u32,
    last_params: Option<WgrWaterParams>,
    last_time: Option<f32>,
    detail_offset: [f32; 2],
    gens: [u64; 4], // depth, env, scene, heightmap
    // W3a shore field
    shore_t_view: wgpu::TextureView,
    shore_dir_view: wgpu::TextureView,
    shore_grid: Option<ShoreGrid>,
    shore_stamp: Option<shore_field::FieldStamp>,
    shore_worker: shore_field::ShoreFieldWorker,
    terrain: Option<(Arc<[f32]>, crate::ffi::WgrTerrainParams, u64)>,
    // W3b shore simulation
    sim: shore_sim::ShoreSim,
    coast: Vec<[f32; 2]>,
    // W3d breakers
    brk: breakers::BreakersGpu,
    // stations per region slot, with what they were traced from (centre, terrain generation,
    // sea level in 0.1 m steps): a slot is re-traced only when that changes
    brk_slots: [Option<(([f32; 2], u64, i32), Vec<breakers::Station>)>; shore_sim::K],
    // W4a spray
    spray: spray::SprayGpu,
    // W5a underwater composite
    uw: underwater::TwUnderwater,
    linear: bool,
}

impl Tidewater {
    pub fn new(
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        camera_layout: &wgpu::BindGroupLayout,
        surface_format: wgpu::TextureFormat,
        sample_count: u32,
    ) -> Self {
        use wgpu::util::DeviceExt;
        let fft = fft::OceanFft::new(device);
        let cdlod = cdlod::Cdlod::default();
        let surface_ubo = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("tw_surface"),
            size: std::mem::size_of::<TwSurface>() as u64,
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        let conform_ubo = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("tw_conform"),
            size: std::mem::size_of::<crate::terrain::TerrainConformParams>() as u64,
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        queue.write_buffer(
            &conform_ubo,
            0,
            bytemuck::bytes_of(&<crate::terrain::TerrainConformParams as bytemuck::Zeroable>::zeroed()),
        );
        let foam_view = textures::create_foam_texture(device, queue);
        let detail_view = textures::create_sea_detail(device, queue);
        let smp_linear_repeat = device.create_sampler(&wgpu::SamplerDescriptor {
            label: Some("tw_linear_repeat"),
            address_mode_u: wgpu::AddressMode::Repeat,
            address_mode_v: wgpu::AddressMode::Repeat,
            mag_filter: wgpu::FilterMode::Linear,
            min_filter: wgpu::FilterMode::Linear,
            mipmap_filter: wgpu::MipmapFilterMode::Linear,
            ..Default::default()
        });
        let smp_aniso_repeat = device.create_sampler(&wgpu::SamplerDescriptor {
            label: Some("tw_aniso4_repeat"),
            address_mode_u: wgpu::AddressMode::Repeat,
            address_mode_v: wgpu::AddressMode::Repeat,
            mag_filter: wgpu::FilterMode::Linear,
            min_filter: wgpu::FilterMode::Linear,
            mipmap_filter: wgpu::MipmapFilterMode::Linear,
            anisotropy_clamp: 4,
            ..Default::default()
        });
        let smp_linear_clamp = device.create_sampler(&wgpu::SamplerDescriptor {
            label: Some("tw_linear_clamp"),
            mag_filter: wgpu::FilterMode::Linear,
            min_filter: wgpu::FilterMode::Linear,
            ..Default::default()
        });
        let env_sampler = device.create_sampler(&wgpu::SamplerDescriptor {
            label: Some("tw_env"),
            address_mode_u: wgpu::AddressMode::Repeat,
            address_mode_v: wgpu::AddressMode::ClampToEdge,
            mag_filter: wgpu::FilterMode::Linear,
            min_filter: wgpu::FilterMode::Linear,
            mipmap_filter: wgpu::MipmapFilterMode::Linear,
            ..Default::default()
        });
        let scene_view = dummy_view(device, "tw_dummy_scene", wgpu::TextureFormat::Rgba16Float);
        let depth_view = dummy_view(device, "tw_dummy_depth", wgpu::TextureFormat::Depth32Float);
        let env_view = dummy_view(device, "tw_dummy_env", wgpu::TextureFormat::Rgba16Float);
        let heightmap_view = dummy_view(device, "tw_dummy_heightmap", wgpu::TextureFormat::R32Float);
        let (shore_t_view, shore_dir_view) = placeholder_shore_views(device, queue);
        let sim = shore_sim::ShoreSim::new(device, queue, TW_SIM_KERNEL_WGSL);

        let vf = wgpu::ShaderStages::VERTEX_FRAGMENT;
        let uniform = |binding| wgpu::BindGroupLayoutEntry {
            binding,
            visibility: vf,
            ty: wgpu::BindingType::Buffer {
                ty: wgpu::BufferBindingType::Uniform,
                has_dynamic_offset: false,
                min_binding_size: None,
            },
            count: None,
        };
        let tex = |binding, dim, sample_type| wgpu::BindGroupLayoutEntry {
            binding,
            visibility: vf,
            ty: wgpu::BindingType::Texture { sample_type, view_dimension: dim, multisampled: false },
            count: None,
        };
        let smp = |binding| wgpu::BindGroupLayoutEntry {
            binding,
            visibility: vf,
            ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Filtering),
            count: None,
        };
        use wgpu::TextureSampleType as S;
        use wgpu::TextureViewDimension as D;
        let flt = S::Float { filterable: true };
        let layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("tw_group1"),
            entries: &[
                uniform(0),
                uniform(1),
                tex(2, D::D2Array, flt),
                tex(3, D::D2Array, flt),
                smp(4),
                smp(5),
                tex(6, D::D2, flt),
                tex(7, D::D2, flt),
                tex(8, D::D2, flt),
                tex(9, D::D2, S::Depth),
                smp(10),
                tex(11, D::D2, flt),
                smp(12),
                tex(13, D::D2, S::Float { filterable: false }),
                uniform(14),
                tex(15, D::D2, S::Float { filterable: false }),
                tex(16, D::D2, flt),
                tex(17, D::D2Array, flt),
                tex(18, D::D2, flt),
                wgpu::BindGroupLayoutEntry {
                    binding: 19,
                    visibility: vf,
                    ty: wgpu::BindingType::Buffer {
                        ty: wgpu::BufferBindingType::Storage { read_only: true },
                        has_dynamic_offset: false,
                        min_binding_size: None,
                    },
                    count: None,
                },
                // W4a spray: the block, the ring (tw_spray.wgsl), the sprite texture
                uniform(20),
                wgpu::BindGroupLayoutEntry {
                    binding: 21,
                    visibility: vf,
                    ty: wgpu::BindingType::Buffer {
                        ty: wgpu::BufferBindingType::Storage { read_only: true },
                        has_dynamic_offset: false,
                        min_binding_size: None,
                    },
                    count: None,
                },
                tex(22, D::D2, flt),
            ],
        });

        let mut composer = crate::shaders::build_composer();
        let shader = crate::shaders::make_module(
            device,
            &mut composer,
            "tw_water_shader",
            TW_WATER_WGSL,
            "water_tw/tw_water.wgsl",
        );
        let linear = if surface_format == wgpu::TextureFormat::Rgba16Float { 1.0 } else { 0.0 };
        let pipeline_layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("tw_water_layout"),
            bind_group_layouts: &[Some(camera_layout), Some(&layout)],
            immediate_size: 0,
        });
        let grid_attrs = wgpu::vertex_attr_array![0 => Float32x2];
        let node_attrs = wgpu::vertex_attr_array![1 => Float32x4];
        let pipeline = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
            label: Some("tw_water_pipeline"),
            layout: Some(&pipeline_layout),
            vertex: wgpu::VertexState {
                module: &shader,
                entry_point: Some("vs_tidewater"),
                compilation_options: Default::default(),
                buffers: &[
                    wgpu::VertexBufferLayout {
                        array_stride: 8,
                        step_mode: wgpu::VertexStepMode::Vertex,
                        attributes: &grid_attrs,
                    },
                    wgpu::VertexBufferLayout {
                        array_stride: 16,
                        step_mode: wgpu::VertexStepMode::Instance,
                        attributes: &node_attrs,
                    },
                ],
            },
            primitive: wgpu::PrimitiveState {
                topology: wgpu::PrimitiveTopology::TriangleList,
                // WaterMaterial side: 'double' (the surface is seen from below as well)
                cull_mode: None,
                ..Default::default()
            },
            // The water pass attaches depth READ-ONLY (lib.rs): the sea depth-tests against the
            // coast like Current OP but cannot write depth there. Tidewater writes depth to
            // reject hidden wave faces; front-to-back node order keeps most of that benefit.
            depth_stencil: Some(wgpu::DepthStencilState {
                format: crate::gfx3d::depth_format(device),
                depth_write_enabled: Some(false),
                depth_compare: Some(wgpu::CompareFunction::GreaterEqual),
                stencil: wgpu::StencilState::default(),
                bias: wgpu::DepthBiasState::default(),
            }),
            multisample: wgpu::MultisampleState { count: sample_count, ..Default::default() },
            fragment: Some(wgpu::FragmentState {
                module: &shader,
                entry_point: Some("fs_tidewater"),
                compilation_options: wgpu::PipelineCompilationOptions {
                    constants: &[("linear", linear)],
                    ..Default::default()
                },
                // WaterMaterial: blending 'none' — the material composites the scene itself.
                targets: &[Some(wgpu::ColorTargetState {
                    format: surface_format,
                    blend: None,
                    write_mask: wgpu::ColorWrites::ALL,
                })],
            }),
            multiview_mask: None,
            cache: None,
        });

        let brk = breakers::BreakersGpu::new(
            device,
            TW_BRK_KERNEL_WGSL,
            camera_layout,
            &layout,
            &shader,
            surface_format,
            sample_count,
            linear,
        );
        let spray = spray::SprayGpu::new(device, queue, camera_layout, &layout, &shader, surface_format, sample_count, linear);
        let uw = underwater::TwUnderwater::new(device, surface_format);

        let (verts, indices) = cdlod::grid_mesh();
        let grid_vbuf = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
            label: Some("tw_grid_vertices"),
            contents: bytemuck::cast_slice(&verts),
            usage: wgpu::BufferUsages::VERTEX,
        });
        let grid_ibuf = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
            label: Some("tw_grid_indices"),
            contents: bytemuck::cast_slice(&indices),
            usage: wgpu::BufferUsages::INDEX,
        });
        let instance_buf = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("tw_nodes"),
            size: (cdlod::MAX_INSTANCES * 16) as u64,
            usage: wgpu::BufferUsages::VERTEX | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });

        let mut me = Self {
            bind: device.create_bind_group(&wgpu::BindGroupDescriptor {
                label: Some("tw_group1_placeholder"),
                layout: &device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
                    label: Some("tw_empty"),
                    entries: &[],
                }),
                entries: &[],
            }),
            fft,
            cdlod,
            surface_ubo,
            conform_ubo,
            foam_view,
            detail_view,
            smp_linear_repeat,
            smp_aniso_repeat,
            smp_linear_clamp,
            env_sampler,
            scene_view,
            depth_view,
            env_view,
            heightmap_view,
            layout,
            bind_dirty: true,
            pipeline,
            grid_vbuf,
            grid_ibuf,
            grid_index_count: indices.len() as u32,
            instance_buf,
            instance_count: 0,
            last_params: None,
            last_time: None,
            detail_offset: [0.0; 2],
            gens: [u64::MAX; 4],
            shore_t_view,
            shore_dir_view,
            shore_grid: None,
            shore_stamp: None,
            shore_worker: Default::default(),
            terrain: None,
            sim,
            coast: Vec::new(),
            brk,
            brk_slots: [const { None }; shore_sim::K],
            spray,
            uw,
            linear: linear > 0.5,
        };
        me.rebuild_bind(device);
        me
    }

    fn rebuild_bind(&mut self, device: &wgpu::Device) {
        use wgpu::BindingResource as R;
        self.bind = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("tw_group1"),
            layout: &self.layout,
            entries: &[
                wgpu::BindGroupEntry { binding: 0, resource: self.fft.params_buffer().as_entire_binding() },
                wgpu::BindGroupEntry { binding: 1, resource: self.surface_ubo.as_entire_binding() },
                wgpu::BindGroupEntry { binding: 2, resource: R::TextureView(self.fft.displacement_view()) },
                wgpu::BindGroupEntry { binding: 3, resource: R::TextureView(self.fft.derivatives_view()) },
                wgpu::BindGroupEntry { binding: 4, resource: R::Sampler(&self.smp_linear_repeat) },
                wgpu::BindGroupEntry { binding: 5, resource: R::Sampler(&self.smp_aniso_repeat) },
                wgpu::BindGroupEntry { binding: 6, resource: R::TextureView(&self.foam_view) },
                wgpu::BindGroupEntry { binding: 7, resource: R::TextureView(&self.detail_view) },
                wgpu::BindGroupEntry { binding: 8, resource: R::TextureView(&self.scene_view) },
                wgpu::BindGroupEntry { binding: 9, resource: R::TextureView(&self.depth_view) },
                wgpu::BindGroupEntry { binding: 10, resource: R::Sampler(&self.smp_linear_clamp) },
                wgpu::BindGroupEntry { binding: 11, resource: R::TextureView(&self.env_view) },
                wgpu::BindGroupEntry { binding: 12, resource: R::Sampler(&self.env_sampler) },
                wgpu::BindGroupEntry { binding: 13, resource: R::TextureView(&self.heightmap_view) },
                wgpu::BindGroupEntry { binding: 14, resource: self.conform_ubo.as_entire_binding() },
                wgpu::BindGroupEntry { binding: 15, resource: R::TextureView(&self.shore_t_view) },
                wgpu::BindGroupEntry { binding: 16, resource: R::TextureView(&self.shore_dir_view) },
                wgpu::BindGroupEntry { binding: 17, resource: R::TextureView(self.sim.state_view()) },
                wgpu::BindGroupEntry { binding: 18, resource: R::TextureView(&self.sim.lace_view) },
                wgpu::BindGroupEntry { binding: 19, resource: self.brk.crest_buffer().as_entire_binding() },
                wgpu::BindGroupEntry { binding: 20, resource: self.spray.params.as_entire_binding() },
                wgpu::BindGroupEntry { binding: 21, resource: self.spray.ring.as_entire_binding() },
                wgpu::BindGroupEntry { binding: 22, resource: R::TextureView(&self.spray.tex_view) },
            ],
        });
        self.brk.rebuild_bind(
            device,
            &self.surface_ubo,
            &self.conform_ubo,
            &self.heightmap_view,
            &self.shore_t_view,
            &self.shore_dir_view,
            self.fft.params_buffer(),
            self.fft.displacement_view(),
            &self.smp_linear_repeat,
            &self.spray,
        );
        self.sim.rebuild_bind(
            device,
            &self.surface_ubo,
            &self.conform_ubo,
            &self.heightmap_view,
            &self.shore_t_view,
            &self.shore_dir_view,
            &self.smp_linear_repeat,
        );
        self.bind_dirty = false;
    }

    pub fn set_params(&mut self, _queue: &wgpu::Queue, params: WgrWaterParams) {
        self.last_params = Some(params);
    }

    pub fn ocean_level(&self) -> Option<f32> {
        self.last_params.map(|p| p.sea_level)
    }

    /// `(local surface level, water clock, eye submersion depth)`, same meaning as Current OP.
    pub fn surface_params(&self) -> Option<(f32, f32, f32)> {
        self.last_params.map(|p| {
            let level = if p.optics_params[3] > 0.0 { p.optics_params[2] } else { p.sea_level };
            (level, p.time, p.fft_control[3].max(0.0))
        })
    }

    fn set_view(&mut self, slot: usize, view_gen: u64) -> bool {
        if self.gens[slot] == view_gen {
            return false;
        }
        self.gens[slot] = view_gen;
        self.bind_dirty = true;
        true
    }

    pub fn set_depth_view(&mut self, device: &wgpu::Device, depth: &wgpu::TextureView, view_gen: u64) {
        if self.set_view(0, view_gen) {
            self.depth_view = depth.clone();
            self.rebuild_bind(device);
        }
    }

    pub fn set_env_view(&mut self, device: &wgpu::Device, env: &wgpu::TextureView, view_gen: u64) {
        if self.set_view(1, view_gen) {
            self.env_view = env.clone();
            self.rebuild_bind(device);
        }
    }

    pub fn set_scene_view(&mut self, device: &wgpu::Device, scene: &wgpu::TextureView, view_gen: u64) {
        if self.set_view(2, view_gen) {
            self.scene_view = scene.clone();
            self.rebuild_bind(device);
        }
    }

    pub fn set_heightmap(
        &mut self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        heightmap: &wgpu::TextureView,
        view_gen: u64,
        params: &crate::terrain::TerrainConformParams,
    ) {
        queue.write_buffer(&self.conform_ubo, 0, bytemuck::bytes_of(params));
        if self.set_view(3, view_gen) {
            self.heightmap_view = heightmap.clone();
            self.rebuild_bind(device);
        }
    }

    /// The CPU heightmap for the shore field (see `WaterBackend::set_terrain_heights`).
    pub fn set_terrain_heights(&mut self, heights: Arc<[f32]>, params: crate::ffi::WgrTerrainParams, generation: u64) {
        self.terrain = Some((heights, params, generation));
    }

    /// Ask for a new shore field when its inputs changed; install one that has finished.
    fn update_shore_field(&mut self, device: &wgpu::Device, queue: &wgpu::Queue, p: &WgrWaterParams) {
        if let Some(field) = self.shore_worker.poll() {
            let (w, h) = (field.res_x, field.res_z);
            self.shore_t_view = data_texture(
                device, queue, "tw_shore_field_t", wgpu::TextureFormat::Rg32Float, (w, h), 8,
                bytemuck::cast_slice(&field.times),
            );
            self.shore_dir_view = data_texture(
                device, queue, "tw_shore_field_dir", wgpu::TextureFormat::Rgba8Snorm, (w, h), 4,
                bytemuck::cast_slice(&field.dirs),
            );
            self.shore_grid = Some(ShoreGrid { origin: field.origin, cell: field.cell, centre: field.centre() });
            self.shore_stamp = Some(field.stamp);
            self.coast = field.coast;
            self.rebuild_bind(device);
        }
        let Some((heights, tp, generation)) = &self.terrain else { return };
        if tp.hm_width < 2 || tp.hm_height < 2 || tp.terrain_grid <= 0.0 {
            return;
        }
        let input = shore_field::ShoreFieldInput {
            heights: heights.clone(),
            hm_width: tp.hm_width,
            hm_height: tp.hm_height,
            origin: [tp.world_origin.x, tp.world_origin.y],
            grid: tp.terrain_grid,
            sea_level: p.sea_level,
            swell_dir: swell_dir(p),
            terrain_gen: *generation,
        };
        self.shore_worker.request(input, self.shore_stamp);
    }

    /// CDLOD selection for this frame's water camera, plus the per-frame uniforms.
    pub fn prepare(&mut self, device: &wgpu::Device, queue: &wgpu::Queue, camera: Option<&WgrCamera>) {
        if let Some(p) = self.last_params {
            self.update_shore_field(device, queue, &p);
        }
        let (Some(p), Some(cam)) = (self.last_params, camera) else {
            self.instance_count = 0;
            return;
        };
        // water clock, pause-coherent like Current OP's (Glob.time)
        let dt = self.last_time.map_or(0.0, |t| (p.time - t).clamp(0.0, 0.1));
        self.last_time = Some(p.time);
        // SeaDetail.update: gust patterns drift downwind at ~0.7 x the wind speed
        // ShoreSim regions on the coast nearest the camera (W3b)
        if shore_sim_on(&p, self.shore_grid) {
            self.sim.update_regions(queue, &self.coast, [cam.cam_pos[0], cam.cam_pos[2]], dt);
        } else {
            self.sim.regions = [None; shore_sim::K];
        }
        self.brk.lips_on = p.tidewater_shore[2][3] > 0.0;
        self.update_stations(queue, p.sea_level);
        // W4a: the spray lives where the breakers are (the simulated regions)
        let spray_on = (spray_on(&p) || spray_override().is_some_and(|(g, i)| g > 0.0 && i > 0.0))
            && self.sim.any_active()
            && self.brk.ns >= 2;
        self.spray.poll_stats();
        let cp = [cam.cam_pos[0], cam.cam_pos[1], cam.cam_pos[2]];
        let (gain, intensity) = spray_override().unwrap_or((p.tidewater_fx[0], p.tidewater_fx[1]));
        self.spray.set_frame(queue, dt, p.time, cp, gain, intensity, spray_on);
        let mut inputs = FrameInputs {
            detail_offset: self.detail_offset,
            morph: &self.cdlod.morph,
            shore: self.shore_grid,
            sim: self.sim.lanes(),
            dt,
            stations: self.brk.ns,
        };
        let block0 = surface_block(&p, &inputs);
        self.detail_offset[0] += block0.wind[0] * block0.sea[2] * 0.7 * dt;
        self.detail_offset[1] += block0.wind[1] * block0.sea[2] * 0.7 * dt;
        inputs.detail_offset = self.detail_offset;
        let block = surface_block(&p, &inputs);
        queue.write_buffer(&self.surface_ubo, 0, bytemuck::bytes_of(&block));

        let spec = fft::SpectrumInputs::from_params(&p);
        // W5a: the underwater composite's block (used only while OP's compositor is engaged)
        {
            let glow = glow_override().unwrap_or(p.tidewater_fx[2]);
            let uwp = underwater::UwParams::new(cam, &p, self.linear, glow, spec.sizes, WATER_ABSORPTION, WATER_SCATTERING, p.underwater_gate[0] > 0.5);
            self.uw.set_frame(queue, &uwp);
        }
        self.fft.set_frame(queue, spec, p.tidewater[0][1], p.time, dt);

        let cam_pos = glam::DVec3::new(cam.cam_pos[0] as f64, cam.cam_pos[1] as f64, cam.cam_pos[2] as f64);
        let clip = glam::Mat4::from_cols_array(&cam.proj) * glam::Mat4::from_cols_array(&cam.view);
        let planes = cdlod::SidePlanes::from_clip(clip);
        let nodes = cdlod::Selection::new(&self.cdlod, cam_pos, p.sea_level, Some(planes)).run();
        self.instance_count = nodes.len() as u32;
        if !nodes.is_empty() {
            queue.write_buffer(&self.instance_buf, 0, bytemuck::cast_slice(&nodes));
        }
    }

    /// Per-frame compute before the water draw: the FFT and its mip chain.
    pub fn update(&mut self, encoder: &mut wgpu::CommandEncoder, timers: &crate::gpu_timers::GpuTimers) {
        if self.last_params.is_some() {
            self.fft.dispatch(encoder, timers);
            if self.sim.any_active() && !self.bind_dirty {
                timers.begin(encoder, crate::gpu_timers::Region::TwShoreSim);
                self.sim.dispatch(encoder);
                self.brk.dispatch(encoder, self.spray.on);
                self.spray.copy_stats(encoder);
                timers.end(encoder, crate::gpu_timers::Region::TwShoreSim);
            }
        }
    }

    /// W5a: the Water tab's underwater checkbox (the same lane as Current OP's).
    pub fn underwater_enabled(&self) -> bool {
        self.last_params.is_some_and(|p| p.underwater_gate[0] > 0.5)
    }

    /// W5a: the underwater composite (OP's backend underwater slot; see underwater.rs).
    pub fn render_underwater(
        &self,
        device: &wgpu::Device,
        encoder: &mut wgpu::CommandEncoder,
        source: &wgpu::TextureView,
        depth: &wgpu::TextureView,
        destination: &wgpu::TextureView,
    ) {
        self.uw.render(device, encoder, source, depth, destination, self.fft.displacement_view());
    }

    /// W4b: the lips' and the spray's coverage into the upscalers' history-control ("reactive")
    /// mask (R8, render size), after the engine has written its sky / water terms. `offset`: the
    /// water camera's slot in the camera group.
    pub fn draw_reactive(&self, encoder: &mut wgpu::CommandEncoder, mask: &wgpu::TextureView, camera: &wgpu::BindGroup, offset: u32) {
        if self.bind_dirty || self.last_params.is_none() || !(self.spray.on || (self.brk.ns >= 2 && self.brk.lips_on)) {
            return;
        }
        let mut pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
            label: Some("tw_reactive_mask"),
            color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                view: mask,
                depth_slice: None,
                resolve_target: None,
                ops: wgpu::Operations { load: wgpu::LoadOp::Load, store: wgpu::StoreOp::Store },
            })],
            depth_stencil_attachment: None,
            timestamp_writes: None,
            occlusion_query_set: None,
            multiview_mask: None,
        });
        self.brk.draw_mask(&mut pass, camera, offset, &self.bind);
        self.spray.draw_mask(&mut pass, camera, offset, &self.bind);
    }

    /// Spray diagnostics for the log: on this frame, stations, and the latest readback.
    pub fn spray_stats(&self) -> (bool, u32, Option<spray::SprayStats>) {
        (self.spray.on, self.brk.ns, self.spray.stats)
    }

    /// What OP's terrain needs for Tidewater's wet sand (None: keep OP's own coast wet band).
    pub fn shore_wetness(&self) -> Option<ShoreWetness<'_>> {
        let p = self.last_params?;
        if !shore_sim_on(&p, self.shore_grid) {
            return None;
        }
        Some(ShoreWetness {
            state: self.sim.state_view(),
            lace: &self.sim.lace_view,
            generation: self.sim.generation,
            regions: self.sim.lanes(),
            sea_level: p.sea_level,
        })
    }

    pub fn draw(&self, pass: &mut wgpu::RenderPass<'_>, camera: &wgpu::BindGroup, offset: u32) {
        if self.instance_count == 0 || self.bind_dirty {
            return;
        }
        pass.set_pipeline(&self.pipeline);
        pass.set_bind_group(0, camera, &[offset]);
        pass.set_bind_group(1, &self.bind, &[]);
        pass.set_vertex_buffer(0, self.grid_vbuf.slice(..));
        pass.set_vertex_buffer(1, self.instance_buf.slice(..));
        pass.set_index_buffer(self.grid_ibuf.slice(..), wgpu::IndexFormat::Uint32);
        pass.draw_indexed(0..self.grid_index_count, 0, 0..self.instance_count);
        // the breaker lips over the surface (Tidewater: the transparent pass after the water)
        self.brk.draw(pass, camera, offset, &self.bind);
        // the spray over both (Tidewater: renderOrder 20, after the lips)
        self.spray.draw(pass, camera, offset, &self.bind);
    }

    /// Shoreline stations for the breakers, traced per simulated region when the region, the
    /// terrain or the sea level changes (W3d); the others are kept.
    fn update_stations(&mut self, queue: &wgpu::Queue, sea_level: f32) {
        let Some((heights, tp, generation)) = &self.terrain else {
            if self.brk.ns != 0 {
                self.brk.set_stations(queue, &[]);
            }
            return;
        };
        if tp.hm_width < 2 || tp.hm_height < 2 || tp.terrain_grid <= 0.0 {
            return;
        }
        let sea_q = (sea_level / 0.1).round() as i32;
        let mut changed = false;
        for (i, r) in self.sim.regions.iter().enumerate() {
            let key = r.map(|r| (r.centre, *generation, sea_q));
            let have = self.brk_slots[i].as_ref().map(|s| s.0);
            if key == have {
                continue;
            }
            changed = true;
            self.brk_slots[i] = key.map(|k| {
                let inp = shore_field::ShoreFieldInput {
                    heights: heights.clone(),
                    hm_width: tp.hm_width,
                    hm_height: tp.hm_height,
                    origin: [tp.world_origin.x, tp.world_origin.y],
                    grid: tp.terrain_grid,
                    sea_level,
                    swell_dir: [1.0, 0.0],
                    terrain_gen: *generation,
                };
                let h = |x: f32, z: f32| shore_field::height_at(&inp, x, z);
                let min = [k.0[0] - shore_sim::SIZE * 0.5, k.0[1] - shore_sim::SIZE * 0.5];
                // runs are numbered per slot (512 apart), so no lip joins two regions
                (k, breakers::trace_stations(&h, min, shore_sim::SIZE, sea_level, i as u32 * 512))
            });
        }
        if changed {
            let all: Vec<breakers::Station> = self.brk_slots.iter().flatten().flat_map(|s| s.1.iter().copied()).collect();
            self.brk.set_stations(queue, &all);
        }
    }


    /// Triangles submitted by the last draw (for the §7 harness).
    #[allow(dead_code)]
    pub fn submitted_triangles(&self) -> u64 {
        self.instance_count as u64 * (self.grid_index_count as u64 / 3)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn surface_block_layout_matches_the_wgsl() {
        assert_eq!(std::mem::offset_of!(TwSurface, shore0), 128);
        assert_eq!(std::mem::offset_of!(TwSurface, shore_grid), 176);
        assert_eq!(std::mem::offset_of!(TwSurface, sim), 192);
        assert_eq!(std::mem::offset_of!(TwSurface, sim_p1), 240);
        assert_eq!(std::mem::offset_of!(TwSurface, brk), 256);
        assert_eq!(std::mem::offset_of!(TwSurface, morph), 272);
        assert_eq!(std::mem::size_of::<TwSurface>(), 464);
    }

    #[test]
    fn shore_waves_are_off_until_a_field_exists_and_follow_the_swell() {
        let mut p = <WgrWaterParams as bytemuck::Zeroable>::zeroed();
        p.tidewater = DEFAULT_PARAMS;
        p.tidewater_shore = DEFAULT_SHORE_PARAMS;
        let (s0, s1, _, _) = shore_lanes(&p, None);
        assert_eq!(s1[3], 0.0);
        assert_eq!(s0[0], 9.0);
        assert!((s0[1] - 0.34).abs() < 1e-6);
        let g = ShoreGrid { origin: [1.0, 2.0], cell: 4.0, centre: [3.0, 4.0] };
        let (_, s1, s2, sg) = shore_lanes(&p, Some(g));
        assert_eq!(s1[3], 1.0);
        assert_eq!([s2[2], s2[3]], [3.0, 4.0]);
        assert!((s2[1] - 5.0f32.to_radians()).abs() < 1e-6); // swell heading for the along-shore axis
        assert_eq!(sg, [1.0, 2.0, 4.0, 0.0]);
        p.tidewater[0][2] = 0.96; // twice the default swell
        let (s0, _, _, _) = shore_lanes(&p, Some(g));
        assert!((s0[1] - 0.68).abs() < 1e-5);
        p.tidewater_shore[0][0] = 0.0; // switched off in the Water tab
        let (_, s1, _, _) = shore_lanes(&p, Some(g));
        assert_eq!(s1[3], 0.0);
    }

    #[test]
    fn tidewater_water_shader_composes() {
        let mut composer = crate::shaders::build_composer();
        if let Err(e) = composer.make_naga_module(naga_oil::compose::NagaModuleDescriptor {
            source: TW_WATER_WGSL,
            file_path: "water_tw/tw_water.wgsl",
            ..Default::default()
        }) {
            panic!("{}", e.emit_to_string(&composer));
        }
    }

    #[test]
    fn default_params_are_tidewaters() {
        assert_eq!(DEFAULT_PARAMS[0][1], 0.9); // OceanFFT choppiness
        assert_eq!(DEFAULT_PARAMS[0][2], 0.48); // swell system scale
        assert_eq!(DEFAULT_PARAMS[2][1], 0.035); // WaterMaterial waterRoughness
        assert_eq!(DEFAULT_PARAMS[2][3], 0.035); // WaterMaterial backscatter
        assert_eq!(DEFAULT_PARAMS[3][2], 0.3); // SeaDetail streakAmount
        assert_eq!(DEFAULT_SHORE_PARAMS[0][1], 9.0); // ShoreWaves period
        assert_eq!(DEFAULT_SHORE_PARAMS[0][2], 0.34); // ShoreWaves amplitude
        assert_eq!(DEFAULT_SHORE_PARAMS[1][0], 0.78); // ShoreWaves gamma
        assert_eq!(DEFAULT_SHORE_PARAMS[2][0], 0.16); // ShoreWaves turbidity
        assert_eq!(DEFAULT_SHORE_PARAMS[2][2], 28.0); // ShoreSim dryTime
    }

    #[test]
    fn shore_sim_kernel_composes() {
        let mut composer = crate::shaders::build_composer();
        if let Err(e) = composer.make_naga_module(naga_oil::compose::NagaModuleDescriptor {
            source: TW_SIM_KERNEL_WGSL,
            file_path: "water_tw/tw_sim_kernel.wgsl",
            ..Default::default()
        }) {
            panic!("{}", e.emit_to_string(&composer));
        }
    }

    #[test]
    fn breaker_kernel_composes() {
        let mut composer = crate::shaders::build_composer();
        if let Err(e) = composer.make_naga_module(naga_oil::compose::NagaModuleDescriptor {
            source: TW_BRK_KERNEL_WGSL,
            file_path: "water_tw/tw_breakers_kernel.wgsl",
            ..Default::default()
        }) {
            panic!("{}", e.emit_to_string(&composer));
        }
    }

    #[test]
    fn shore_sim_runs_only_with_a_field_and_the_switch() {
        let mut p = <WgrWaterParams as bytemuck::Zeroable>::zeroed();
        p.tidewater = DEFAULT_PARAMS;
        p.tidewater_shore = DEFAULT_SHORE_PARAMS;
        let g = ShoreGrid { origin: [0.0; 2], cell: 4.0, centre: [0.0; 2] };
        assert!(!shore_sim_on(&p, None));
        assert!(shore_sim_on(&p, Some(g)));
        let (p0, p1) = sim_lanes(&p, Some(g), 0.016);
        assert_eq!(p0, [28.0, 4.5, 2.6, 5.0]);
        assert_eq!(p1, [1.0, 0.02, 0.016, 1.0]);
        p.tidewater_shore[2][1] = 0.0;
        assert!(!shore_sim_on(&p, Some(g)));
    }
}
