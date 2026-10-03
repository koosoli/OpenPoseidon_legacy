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
//! W6: boat wakes (`wake.rs`, `tw_wake_kernel.wgsl`, `tw_wake.wgsl`) for the two boats nearest the
//! camera, and (W6i) their bow spray (`boat_spray.rs`) into the spray ring's CPU tail.
//!
//! Not yet: inland water bodies are not drawn in this mode (see TW-WATER-01, open question for the
//! owner).

mod boat_spray;
mod impacts;
pub(crate) mod probe;
mod breakers;
mod cdlod;
mod fft;
mod lace;
mod shore_field;
mod shore_sim;
pub mod spray;
mod underwater;
mod caustics;
mod textures;
mod wake;
mod rotor_wash;

/// What a region's stations were traced from: centre, terrain generation, sea level in 0.1 m steps.
type StationKey = ([f32; 2], u64, i32);
/// Recent station traces kept for reuse (W7c).
const STATION_CACHE: usize = 8;

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
    "\n",
    include_str!("tw_caustics.wgsl"),
    "\n",
    include_str!("tw_wake.wgsl"),
    "\n",
    include_str!("tw_impact.wgsl"),
    "\n",
    include_str!("tw_probe.wgsl"),
);

/// W6: the wake simulation kernels (tw_wake_kernel.wgsl) with the shared declarations.
const TW_WAKE_KERNEL_WGSL: &str = concat!(include_str!("tw_common.wgsl"), "\n", include_str!("tw_wake_kernel.wgsl"));

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

/// `WGR_TW_SPRAY_EARLY=1`: the spray drawn inside the water pass, before the clouds (the order
/// before W12a), for A/B captures of the clouds painted over the plumes.
fn spray_early() -> bool {
    static V: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *V.get_or_init(|| std::env::var("WGR_TW_SPRAY_EARLY").is_ok_and(|v| v.trim() == "1"))
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

/// `WGR_TW_BOAT_SPRAY=off` turns the boats' bow spray off (W6i; A/B captures); a number sets the
/// steady white-water gain instead (boat_spray.rs, W6i.1).
fn boat_spray_enabled() -> bool {
    static V: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *V.get_or_init(|| std::env::var("WGR_TW_BOAT_SPRAY").map_or(true, |v| v.trim() != "off"))
}

/// Narrow rotor-normal ablation; boat wakes and ammunition remain enabled.
fn rotor_wash_enabled() -> bool {
    static V: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *V.get_or_init(|| std::env::var("WGR_TW_ROTOR_WASH").map_or(true, |v| v == "1"))
}

fn rotor_trace_enabled() -> bool {
    static V: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *V.get_or_init(|| std::env::var("WGR_TW_ROTOR_TRACE").is_ok_and(|v| v == "1"))
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
    // W6: the wake readers' lanes, 4 per slot (tw_wake.wgsl)
    wake: [[f32; 4]; wake::SLOTS * 4],
    // W8b: explosions in the surface, 2 lanes per slot (tw_impact.wgsl)
    impacts: [[f32; 4]; impacts::SURFACE_SLOTS * 2],
    // W9a: the camera height probe's grid (tw_probe.wgsl)
    probe: [f32; 4],
    rotor_domain: [f32; 4],
    rotor_control: [f32; 4],
    rotor_sources: [[f32; 4]; rotor_wash::SOURCES],
}

const _: () = assert!(std::mem::size_of::<TwSurface>() == 16 * (20 + cdlod::LEVELS + wake::SLOTS * 4 + impacts::SURFACE_SLOTS * 2 + rotor_wash::SOURCES));

/// Whether the shore simulation runs (Water tab, and only once the shore waves have a field).
/// Breaker spray on (W4a): emission gain and sprite intensity above zero (the caller also needs
/// an active shore simulation and stations).
/// W10: `WGR_TW_LIP=<opacity>` overrides the Water tab's breaker lip opacity (0 = no lips) for the
/// session, for A/B captures of the curling breakers.
fn lip_override() -> Option<f32> {
    static V: std::sync::OnceLock<Option<f32>> = std::sync::OnceLock::new();
    *V.get_or_init(|| std::env::var("WGR_TW_LIP").ok().and_then(|v| v.trim().parse::<f32>().ok()))
}

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
    // W5b: the caustic layers' tile sizes (fine, broad), m
    caustic_tiles: [f32; 2],
    // W6: the wake readers' lanes
    wake: [[f32; 4]; wake::SLOTS * 4],
    impacts: [[f32; 4]; impacts::SURFACE_SLOTS * 2],
    // W9a: the probe grid, and the surface height under the camera read back from it
    probe: [f32; 4],
    cam_surface: Option<f32>,
    rotor: rotor_wash::RotorWash,
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
    // W9a: in this mode the level is the drawn surface's own under the camera (the probe's
    // readback) when there is one: the material's view side then agrees with the surface drawn.
    let cam_water = f.cam_surface.unwrap_or(if p.optics_params[3] > 0.0 { p.optics_params[2] } else { p.sea_level });
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
        brk: [f.stations as f32, lip_override().unwrap_or(p.tidewater_shore[2][3]).clamp(0.0, 2.0), f.caustic_tiles[0], f.caustic_tiles[1]],
        morph: *f.morph,
        wake: f.wake,
        impacts: f.impacts,
        probe: f.probe,
        rotor_domain: f.rotor.domain,
        rotor_control: f.rotor.control,
        rotor_sources: f.rotor.sources,
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
    rotor: rotor_wash::RotorWash,
    detail_offset: [f32; 2],
    rotor_trace: rotor_wash::RotorTrace,
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
    brk_slots: [Option<(StationKey, Arc<Vec<breakers::Station>>)>; shore_sim::K],
    // W7c: stations are traced on a worker thread (a trace is ~11 ms of CPU); recent traces are
    // kept so a key that comes back (sea level flickering over a 0.1 m step) costs nothing
    st_tx: std::sync::mpsc::Sender<(usize, StationKey, Vec<breakers::Station>)>,
    st_rx: std::sync::mpsc::Receiver<(usize, StationKey, Vec<breakers::Station>)>,
    st_pending: Vec<(usize, StationKey)>,
    st_cache: Vec<(usize, StationKey, Arc<Vec<breakers::Station>>)>,
    // W4a spray
    spray: spray::SprayGpu,
    // W5a underwater composite
    uw: underwater::TwUnderwater,
    linear: bool,
    // W5b caustics
    caustics: caustics::Caustics,
    // W6 boat wakes
    wake: wake::WakeGpu,
    wake_lanes: [[f32; 4]; wake::SLOTS * 4],
    // W6i boat bow spray
    boat_spray: boat_spray::BoatSpray,
    // W8b: splashes from the engine's water-interaction events
    impacts: impacts::Impacts,
    // W9a: the surface around the camera, as drawn (underwater composite, view side, gate)
    probe: probe::Probe,
    // Swimming: the exact lanes recorded alongside each probe readback.
    probe_lanes: [f32; 4],
    // W5e: the frame's spot lights (the diver's torch is picked from them in prepare)
    spots: Vec<crate::ffi::WgrLight>,
    // W7b: prepare CPU ms: sum, max, station update max, CDLOD selection sum, frames;
    // W7c: traces started, cache hits
    prep_stats: [f32; 7],
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
                // W5b caustics: fine, broad
                tex(23, D::D2, flt),
                tex(24, D::D2, flt),
                // W6 wakes: display (h, slopes, foam), near-field template, aeration
                tex(25, D::D2Array, flt),
                tex(26, D::D2Array, flt),
                tex(27, D::D2Array, flt),
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
        let probe = probe::Probe::new(device, &layout, &shader);
        let caustics = caustics::Caustics::new(device);
        let wake = wake::WakeGpu::new(device, TW_WAKE_KERNEL_WGSL);

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

        let (st_tx, st_rx) = std::sync::mpsc::channel();
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
            rotor: Default::default(),
            detail_offset: [0.0; 2],
            rotor_trace: Default::default(),
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
            st_tx,
            st_rx,
            st_pending: Vec::new(),
            st_cache: Vec::new(),
            spots: Vec::new(),
            impacts: Default::default(),
            probe,
            probe_lanes: [0.0; 4],
            spray,
            uw,
            linear: linear > 0.5,
            caustics,
            wake,
            wake_lanes: [[0.0; 4]; wake::SLOTS * 4],
            boat_spray: Default::default(),
            prep_stats: [0.0; 7],
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
                wgpu::BindGroupEntry { binding: 23, resource: R::TextureView(self.caustics.fine_view()) },
                wgpu::BindGroupEntry { binding: 24, resource: R::TextureView(self.caustics.broad_view()) },
                wgpu::BindGroupEntry { binding: 25, resource: R::TextureView(self.wake.display_view()) },
                wgpu::BindGroupEntry { binding: 26, resource: R::TextureView(self.wake.near_view()) },
                wgpu::BindGroupEntry { binding: 27, resource: R::TextureView(self.wake.aer_view()) },
            ],
        });
        self.wake.rebuild_bind(device, &self.heightmap_view, &self.conform_ubo);
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
        self.caustics.rebuild_bind(device, self.fft.derivatives_view(), &self.smp_linear_repeat);
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
    /// CDLOD selection for this frame's water camera, plus the per-frame uniforms (timed, W7b).
    pub fn prepare(&mut self, device: &wgpu::Device, queue: &wgpu::Queue, camera: Option<&WgrCamera>) {
        let t0 = std::time::Instant::now();
        self.prepare_inner(device, queue, camera);
        let ms = t0.elapsed().as_secs_f32() * 1000.0;
        self.prep_stats[0] += ms;
        self.prep_stats[1] = self.prep_stats[1].max(ms);
        self.prep_stats[4] += 1.0;
    }

    fn prepare_inner(&mut self, device: &wgpu::Device, queue: &wgpu::Queue, camera: Option<&WgrCamera>) {
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
        let ts = std::time::Instant::now();
        self.update_stations(queue, p.sea_level);
        self.prep_stats[2] = self.prep_stats[2].max(ts.elapsed().as_secs_f32() * 1000.0);
        // W4a: the spray lives where the breakers are (the simulated regions)
        let spray_on = (spray_on(&p) || spray_override().is_some_and(|(g, i)| g > 0.0 && i > 0.0))
            && self.sim.any_active()
            && self.brk.ns >= 2;
        self.spray.poll_stats();
        self.probe.poll();
        let cp = [cam.cam_pos[0], cam.cam_pos[1], cam.cam_pos[2]];
        let (gain, intensity) = spray_override().unwrap_or((p.tidewater_fx[0], p.tidewater_fx[1]));
        // W6i: the boats' bow spray into the ring's CPU tail, and their hulls as collision bodies
        let spray_boats = boat_spray::SprayBoat::from_params(&p);
        let mut requests = Vec::new();
        if boat_spray_enabled() {
            self.boat_spray.update(&spray_boats, dt, &mut requests, spray::MAX_REQUESTS);
        }
        // W8b: shells, rockets, bombs and bullets hitting the water (the engine's events)
        self.impacts.update(dt, cp, &mut requests, spray::MAX_REQUESTS);
        let bodies: Vec<spray::Body> = spray_boats.iter().flatten().map(|b| b.body()).collect();
        self.spray.set_frame(queue, dt, p.time, cp, gain, intensity, spray_on, &requests, &bodies);
        // W6: the boats' wakes (visual only; see wake.rs)
        self.wake_lanes = self.wake.update(queue, wake::WakeBoat::from_params(&p), dt, p.time, p.sea_level);
        let mut inputs = FrameInputs {
            detail_offset: self.detail_offset,
            morph: &self.cdlod.morph,
            shore: self.shore_grid,
            sim: self.sim.lanes(),
            dt,
            stations: self.brk.ns,
            caustic_tiles: self.caustics.tiles,
            wake: self.wake_lanes,
            impacts: self.impacts.lanes(),
            probe: probe::Probe::lanes(cp),
            cam_surface: self.probe.camera_height(),
            rotor: self.rotor,
        };
        let block0 = surface_block(&p, &inputs);
        self.detail_offset[0] += block0.wind[0] * block0.sea[2] * 0.7 * dt;
        self.detail_offset[1] += block0.wind[1] * block0.sea[2] * 0.7 * dt;
        inputs.detail_offset = self.detail_offset;
        let block = surface_block(&p, &inputs);
        queue.write_buffer(&self.surface_ubo, 0, bytemuck::bytes_of(&block));
        let rotor_count = (block.rotor_control[1] as usize).min(rotor_wash::SOURCES);
        if rotor_trace_enabled() && self.rotor_trace.observe(block.sea[1],block.rotor_domain,rotor_count) {
            // Exact copied private UBO lanes, not a proposal or synthetic event.
            // stderr is captured by the installed runner; no callback/ABI change.
            eprintln!("TW_ROTOR_UBO {{\"time\":{},\"phaseTime\":{},\"domain\":{:?},\"count\":{},\"sources\":{:?},\"readonly\":true}}",
                block.sea[1],block.rotor_control[0],block.rotor_domain,rotor_count,
                &block.rotor_sources[..rotor_count]);
        }

        let spec = fft::SpectrumInputs::from_params(&p);
        {
            let d = cam.sun_dir_world;
            let l = (d[0] * d[0] + d[1] * d[1] + d[2] * d[2]).sqrt().max(1e-6);
            self.caustics.set_frame(queue, [-d[0] / l, -d[1] / l, -d[2] / l], spec.sizes);
        }
        // W5a: the underwater composite's block (used only while OP's compositor is engaged)
        {
            let glow = glow_override().unwrap_or(p.tidewater_fx[2]);
            let torch = underwater::pick_torch(&self.spots, [cam.cam_pos[0], cam.cam_pos[1], cam.cam_pos[2]]);
            let mut uwp = underwater::UwParams::new(cam, &p, self.linear, glow, spec.sizes, self.caustics.tiles, WATER_ABSORPTION, WATER_SCATTERING, p.underwater_gate[0] > 0.5, torch);
            // W9a: the drawn surface around the camera (the grid is drawn in `update`, before the
            // composite reads it; until the first one exists the composite uses the FFT alone)
            uwp.probe = if self.probe.ready || !self.bind_dirty { probe::Probe::lanes(cp) } else { [0.0; 4] };
            self.probe_lanes = probe::Probe::lanes(cp);
            self.uw.set_frame(queue, &uwp);
        }
        self.fft.set_frame(queue, spec, p.tidewater[0][1], p.time, dt);

        let cam_pos = glam::DVec3::new(cam.cam_pos[0] as f64, cam.cam_pos[1] as f64, cam.cam_pos[2] as f64);
        let clip = glam::Mat4::from_cols_array(&cam.proj) * glam::Mat4::from_cols_array(&cam.view);
        let planes = cdlod::SidePlanes::from_clip(clip);
        let tc = std::time::Instant::now();
        let nodes = cdlod::Selection::new(&self.cdlod, cam_pos, p.sea_level, Some(planes)).run();
        self.prep_stats[3] += tc.elapsed().as_secs_f32() * 1000.0;
        self.instance_count = nodes.len() as u32;
        if !nodes.is_empty() {
            queue.write_buffer(&self.instance_buf, 0, bytemuck::cast_slice(&nodes));
        }
    }

    /// Per-frame compute before the water draw: the FFT and its mip chain.
    pub fn update(&mut self, encoder: &mut wgpu::CommandEncoder, timers: &crate::gpu_timers::GpuTimers) {
        if self.last_params.is_some() {
            self.fft.dispatch(encoder, timers);
            // W5b: the caustics follow this frame's FFT (W7h: timed)
            if !self.bind_dirty {
                timers.begin(encoder, crate::gpu_timers::Region::TwCaustics);
                self.caustics.dispatch(encoder);
                timers.end(encoder, crate::gpu_timers::Region::TwCaustics);
            }
            // W6: the boat wakes (before the water draw reads them)
            if !self.bind_dirty {
                timers.begin(encoder, crate::gpu_timers::Region::TwWake);
                self.wake.dispatch(encoder);
                timers.end(encoder, crate::gpu_timers::Region::TwWake);
            }
            if self.sim.any_active() && !self.bind_dirty {
                timers.begin(encoder, crate::gpu_timers::Region::TwShoreSim);
                self.sim.dispatch(encoder);
                timers.end(encoder, crate::gpu_timers::Region::TwShoreSim);
                // W7h: the crest finder, the breakers' spray emitters and the spray update on their own
                timers.begin(encoder, crate::gpu_timers::Region::TwBreakers);
                self.brk.dispatch(encoder, true, self.spray.breakers_on, self.spray.on);
                self.spray.copy_stats(encoder);
                timers.end(encoder, crate::gpu_timers::Region::TwBreakers);
            } else if self.spray.on && !self.bind_dirty {
                // W6i: the boats' spray offshore, where no shore simulation runs
                timers.begin(encoder, crate::gpu_timers::Region::TwBreakers);
                self.brk.dispatch(encoder, false, false, true);
                self.spray.copy_stats(encoder);
                timers.end(encoder, crate::gpu_timers::Region::TwBreakers);
            }
            // W9a: the surface around the camera, after everything that displaces it
            if !self.bind_dirty {
                self.probe.record(encoder, &self.bind, self.probe_lanes);
            }
        }
    }

    /// W9a: the drawn surface's height under the camera (absolute m), read back from the probe
    /// 1-3 frames late; None until the first readback, or when it is stale.
    pub fn camera_surface_height(&self) -> Option<f32> {
        self.probe.camera_height()
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
        self.uw.render(device, encoder, source, depth, destination, self.fft.displacement_view(), (self.caustics.fine_view(), self.caustics.broad_view()), &self.probe.view);
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

    /// W6: the wakes' slots, for the log.
    /// W8b: the engine's water-interaction events (explosions and bullets with their ammo) become
    /// splashes; the rest are Current OP's (ripples, wakes) and ignored here.
    pub fn submit_interactions(&mut self, events: &[crate::ffi::WgrWaterInteractionEvent]) {
        let sea = self.last_params.map_or(0.0, |p| p.sea_level);
        self.impacts.submit(events, sea);
        let frozen = self.rotor_frozen();
        self.rotor.submit_frame(events.iter().map(|e| [e.position_radius,e.velocity_kind,
            e.time_life_foam_mass,e.direction_depth_flags]),frozen,rotor_wash_enabled());
    }

    fn rotor_frozen(&self) -> bool {
        self.last_params.is_some_and(|p| p.fft_control[2].to_bits() & (1 << 1) != 0)
    }

    // Called before events even when their count is zero: a stopped engine
    // cannot retain the preceding frame's rotor descriptor.
    pub fn set_interaction_params(&mut self, p: crate::ffi::WgrWaterInteractionParams) {
        let frozen = self.rotor_frozen();
        self.rotor.frame(p.domain,p.misc[1],frozen,rotor_wash_enabled());
    }

    /// W5e: this frame's lights; the spot lights are kept for the diver's torch (picked in
    /// `prepare`, where the water camera is known).
    pub fn set_lights(&mut self, lights: &[crate::ffi::WgrLight]) {
        self.spots.clear();
        self.spots.extend(lights.iter().filter(|l| l.dir[3] > 0.5).take(32).copied());
    }

    /// W6j.1: the bow-spray diagnostics windows since the last call, one line each (drained).
    pub fn take_spray_diag(&mut self) -> Vec<String> {
        self.boat_spray
            .history
            .drain(..)
            .map(|(t, id, d)| {
                let n = d.frames.max(1) as f32;
                format!(
                    "TW spray diag t {t:.2} boat {id}: drops {:.1} mist {:.1} lig {:.1} white {:.1} sheet {:.1} /frame; burst {:.2} impact {:.2}; stem depth {:.2}..{:.2} rate {:.2}; origin-sea {:.2} draft {:.2} fb {:.2}",
                    d.kinds[0] as f32 / n,
                    d.kinds[1] as f32 / n,
                    d.kinds[2] as f32 / n,
                    d.kinds[3] as f32 / n,
                    d.kinds[4] as f32 / n,
                    d.burst_max,
                    d.imp_sum / n,
                    d.stem_depth[0],
                    d.stem_depth[1],
                    d.rate_max,
                    d.pose[0],
                    d.pose[1],
                    d.pose[2]
                )
            })
            .collect()
    }

    pub fn wake_stats(&self) -> String {
        let (particles, requests) = self.boat_spray.stats;
        let mut diag = String::new();
        for (id, d) in self.boat_spray.diag.iter().flatten() {
            let n = d.frames.max(1) as f32;
            diag += &format!(
                " | spray diag boat {id}: {} frames, per frame drops {:.1} mist {:.1} lig {:.1} white {:.1} sheet {:.1}; burst max {:.2}, impact share {:.2}; stem depth {:.2}..{:.2} m, rate max {:.2} m/s; origin-sea {:.2} draft {:.2} freeboard {:.2} up.y {:.3}",
                d.frames,
                d.kinds[0] as f32 / n,
                d.kinds[1] as f32 / n,
                d.kinds[2] as f32 / n,
                d.kinds[3] as f32 / n,
                d.kinds[4] as f32 / n,
                d.burst_max,
                d.imp_sum / n,
                d.stem_depth[0],
                d.stem_depth[1],
                d.rate_max,
                d.pose[0],
                d.pose[1],
                d.pose[2],
                d.pose[3]
            );
        }
        format!(
            "{}; bow spray: {particles} particles in {requests} requests last frame, spray on {}{diag}; (W8b) water impacts: {} received, {} emitting, {} particles last frame",
            self.wake.stats(),
            self.spray.on,
            self.impacts.received,
            self.impacts.active(),
            self.impacts.particles
        )
    }

    /// W7b: the CPU cost of `prepare` since the last call (mean / max ms, the largest shore
    /// station re-trace, the CDLOD selection mean), then reset.
    pub fn take_prepare_stats(&mut self) -> String {
        let n = self.prep_stats[4].max(1.0);
        let s = format!(
            "TW prepare CPU ms: mean {:.3} max {:.3} over {} frames | station update max {:.3} ({} traces on the worker, {} from cache) | CDLOD selection mean {:.3} | (W7h) last frame: {} CDLOD nodes, {} triangles (grid {}), {} simulated shore regions",
            self.prep_stats[0] / n,
            self.prep_stats[1],
            self.prep_stats[4] as u32,
            self.prep_stats[2],
            self.prep_stats[5] as u32,
            self.prep_stats[6] as u32,
            self.prep_stats[3] / n,
            self.instance_count,
            self.submitted_triangles(),
            cdlod::grid(),
            self.sim.regions.iter().flatten().count()
        );
        self.prep_stats = [0.0; 7];
        // W7j: where the shore field found coast (16 samples), to pick a second coast for captures
        let n = self.coast.len();
        let samples: Vec<String> = (0..16.min(n)).map(|i| {
            let c = self.coast[i * n / 16.min(n).max(1)];
            format!("{:.0},{:.0}", c[0], c[1])
        }).collect();
        // W9c: the sea state the FFT draws, and W9a: the probe's surface under the camera
        let sea = match (self.last_params, self.fft.spectrum()) {
            (Some(p), Some(spec)) => {
                let (var_h, mss) = fft::sea_stats(spec.sizes, spec.local, spec.swell, spec.depth);
                format!(
                    " | (W9c) sea: wind {:.1} m/s fetch {:.0} km, Hs {:.2} m (x amplitude {:.2}), rms slope {:.3}, foam bias {:.3} | (W9a) surface under the camera {}",
                    spec.local.wind_speed,
                    spec.local.fetch_km,
                    4.0 * var_h.max(0.0).sqrt() * p.tidewater[0][0],
                    p.tidewater[0][0],
                    mss.max(0.0).sqrt(),
                    spec.foam_bias,
                    self.probe.camera_height().map_or("none".to_string(), |h| format!("{h:.2} m (sea {:.2}, read {} frames ago)", p.sea_level, self.probe.age))
                )
            }
            _ => String::new(),
        };
        format!("{s} | coast blocks {n}: {}{sea}", samples.join(" "))
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
        // (the spray over both -- Tidewater: renderOrder 20, after the lips -- is `draw_spray`)
        if spray_early() {
            self.spray.draw(pass, camera, offset, &self.bind);
        }
    }

    /// W12a: the spray sprites (breakers, bow spray, the water explosions' plumes), recorded by
    /// the renderer after the cloud, god-ray and smoke composites, with the other transparents.
    /// Drawn inside the water pass they came before the cloud composite, whose march stops at the
    /// scene depth: sprites write none, so wherever a plume stood against the sky the clouds
    /// behind it were painted over it (owner: "water explosions have the clouds clip through them
    /// when looked at from an angle"). The engine's own order is the same: clouds are background.
    pub fn draw_spray(&self, pass: &mut wgpu::RenderPass<'_>, camera: &wgpu::BindGroup, offset: u32) {
        if !self.has_spray_pass() {
            return;
        }
        self.spray.draw(pass, camera, offset, &self.bind);
    }

    pub fn has_spray_pass(&self) -> bool {
        self.spray.on && !self.bind_dirty && !spray_early()
    }

    /// Shoreline stations for the breakers, traced per simulated region when the region, the
    /// terrain or the sea level changes (W3d); the others are kept. W7c: the trace runs on a
    /// worker thread and lands a frame or two later (a new region starts its 3 s warm-up
    /// anyway); recent traces are reused from a small cache.
    fn update_stations(&mut self, queue: &wgpu::Queue, sea_level: f32) {
        let Some((heights, tp, generation)) = &self.terrain else {
            if self.brk.ns != 0 {
                self.brk.set_stations(queue, &[]);
            }
            self.brk_slots = [const { None }; shore_sim::K];
            return;
        };
        if tp.hm_width < 2 || tp.hm_height < 2 || tp.terrain_grid <= 0.0 {
            return;
        }
        let sea_q = (sea_level / 0.1).round() as i32;
        let mut changed = false;
        // finished traces: into the cache, and into their slot if it still wants that key
        while let Ok((i, k, st)) = self.st_rx.try_recv() {
            self.st_pending.retain(|p| !(p.0 == i && p.1 == k));
            let st = Arc::new(st);
            self.st_cache.retain(|c| !(c.0 == i && c.1 == k));
            self.st_cache.push((i, k, st.clone()));
            if self.st_cache.len() > STATION_CACHE {
                self.st_cache.remove(0);
            }
            if self.sim.regions[i].map(|r| (r.centre, *generation, sea_q)) == Some(k) {
                self.brk_slots[i] = Some((k, st));
                changed = true;
            }
        }
        for (i, r) in self.sim.regions.iter().enumerate() {
            let key = r.map(|r| (r.centre, *generation, sea_q));
            let have = self.brk_slots[i].as_ref().map(|s| s.0);
            if key == have {
                continue;
            }
            let Some(k) = key else {
                self.brk_slots[i] = None;
                changed = true;
                continue;
            };
            if let Some(c) = self.st_cache.iter().find(|c| c.0 == i && c.1 == k) {
                self.brk_slots[i] = Some((k, c.2.clone()));
                self.prep_stats[6] += 1.0;
                changed = true;
                continue;
            }
            // until the trace lands the slot has no stations (not the old region's)
            if self.brk_slots[i].is_some() {
                self.brk_slots[i] = None;
                changed = true;
            }
            if self.st_pending.iter().any(|p| p.0 == i && p.1 == k) {
                continue;
            }
            self.st_pending.push((i, k));
            self.prep_stats[5] += 1.0;
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
            let tx = self.st_tx.clone();
            let job = move || {
                let h = |x: f32, z: f32| shore_field::height_at(&inp, x, z);
                let min = [k.0[0] - shore_sim::SIZE * 0.5, k.0[1] - shore_sim::SIZE * 0.5];
                // runs are numbered per slot (512 apart), so no lip joins two regions
                let st = breakers::trace_stations(&h, min, shore_sim::SIZE, inp.sea_level, i as u32 * 512);
                // a closed receiver (backend switched away) just drops the result
                let _ = tx.send((i, k, st));
            };
            if std::thread::Builder::new().name("tw-stations".into()).spawn(job).is_err() {
                // no thread: forget it, the next frame asks again
                self.st_pending.retain(|p| !(p.0 == i && p.1 == k));
            }
        }
        if changed {
            let all: Vec<breakers::Station> = self.brk_slots.iter().flatten().flat_map(|s| s.1.iter().copied()).collect();
            self.brk.set_stations(queue, &all);
        }
    }


    /// Triangles submitted by the last draw (for the §7 harness; W7h: in the prepare stats line).
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
        // W6: the wake readers' lanes after the CDLOD morph table
        assert_eq!(std::mem::offset_of!(TwSurface, wake), 528);
        assert_eq!(std::mem::offset_of!(TwSurface, impacts), 656);
        // W9a: the probe grid last
        assert_eq!(std::mem::offset_of!(TwSurface, probe), 784);
        assert_eq!(std::mem::offset_of!(TwSurface, rotor_domain), 800);
        assert_eq!(std::mem::offset_of!(TwSurface, rotor_control), 816);
        assert_eq!(std::mem::offset_of!(TwSurface, rotor_sources), 832);
        assert_eq!(std::mem::size_of::<TwSurface>(), 960);
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
