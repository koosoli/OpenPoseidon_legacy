// Procedural atmospheric sky (plan Stage 2a): a fullscreen pass that marches the
// view ray through a Hillaire-style LUT atmosphere and writes the scene target
// (HDR texture when the HDR path is on, else the swapchain) BEFORE geometry, so
// terrain/objects overdraw it; depth is neither tested nor written. See
// docs/procedural-sky-plan.md.
//
// Two small LUTs feed the main march — a transmittance LUT and an isotropic
// multi-scattering LUT — both depending only on the atmosphere parameters (the sun
// is a LUT axis), so they rebuild only when those params change (dirty-flagged),
// not every frame. Celestial + authored params arrive from C++ via wgr_set_sky.

use bytemuck::Zeroable;
use wgpu::util::DeviceExt;

use crate::ffi::WgrSky;

// CPU reference port of the atmosphere math, for objective colour unit tests.
#[cfg(test)]
mod reference;

// Guards every sky.wgsl edit: parse + validate the module offline so a WGSL error
// surfaces in CI/tests instead of as a pipeline-creation panic at runtime.
#[test]
fn sky_wgsl_validates() {
    let module = naga::front::wgsl::parse_str(include_str!("sky.wgsl")).expect("sky.wgsl parse");
    naga::valid::Validator::new(
        naga::valid::ValidationFlags::all(),
        naga::valid::Capabilities::all(),
    )
    .validate(&module)
    .expect("sky.wgsl validate");
}

// Same offline guard for the standalone SH-projection compute (create_shader_module'd, so it is
// not covered by the naga_oil entry-shader compose test).
#[test]
fn sky_sh_wgsl_validates() {
    let module =
        naga::front::wgsl::parse_str(include_str!("sky_sh.wgsl")).expect("sky_sh.wgsl parse");
    naga::valid::Validator::new(
        naga::valid::ValidationFlags::all(),
        naga::valid::Capabilities::all(),
    )
    .validate(&module)
    .expect("sky_sh.wgsl validate");
}

// Offline guard for the over-scene cloud composite shader (separate module, own binding namespace).
#[test]
fn cloud_composite_wgsl_validates() {
    let module = naga::front::wgsl::parse_str(include_str!("cloud_composite.wgsl"))
        .expect("cloud_composite.wgsl parse");
    naga::valid::Validator::new(
        naga::valid::ValidationFlags::all(),
        naga::valid::Capabilities::all(),
    )
    .validate(&module)
    .expect("cloud_composite.wgsl validate");
}

// LUT resolutions. Transmittance is smooth in both axes; multiscatter is very
// low-frequency, so a tiny map suffices (its build is the expensive one).
const TRANSMITTANCE_W: u32 = 256;
const TRANSMITTANCE_H: u32 = 64;
const MULTISCATTER_SIZE: u32 = 32;
const LUT_FORMAT: wgpu::TextureFormat = wgpu::TextureFormat::Rgba16Float;

// Aerial-perspective froxel volume: XY = screen, Z = distance (squared distribution).
// 32^3 is Hillaire's classic size — cheap to fill, soft enough for god-ray shafts.
const FROXEL_W: u32 = 32;
const FROXEL_H: u32 = 32;
const FROXEL_D: u32 = 32;
const FROXEL_FORMAT: wgpu::TextureFormat = wgpu::TextureFormat::Rgba16Float;

// Reflection environment map: an equirectangular (lat-long) bake of the disc-free sky radiance,
// sampled by the water surface in its reflected direction (water look plan Stage 4a). The sky is
// low-frequency, so a small map is plenty; 2:1 for the full sphere. Linear radiance (Rgba16Float).
const ENV_W: u32 = 256;
const ENV_H: u32 = 128;
const ENV_FORMAT: wgpu::TextureFormat = wgpu::TextureFormat::Rgba16Float;

// Cloud shape/detail noise: a tileable 3D texture sampled (Repeat) by the cloud march instead of
// evaluating analytic fBm per step. This is BOTH the perf fix (one texture tap vs ~dozens of hash
// evals) AND the moire fix (the old fract()-based hash lost precision at planet-scale / unbounded
// wind*time coordinates; a Repeat-sampled texture wraps at full precision). R = low-frequency
// shape fBm, G = higher-frequency detail fBm; all octave frequencies are powers of two that divide
// the texture size so the volume tiles seamlessly. 128^3 RGBA8 = 8 MB, generated once at startup.
const NOISE_N: u32 = 128;
const NOISE_FORMAT: wgpu::TextureFormat = wgpu::TextureFormat::Rgba8Unorm;

// Small integer hash -> u32 (Wang-style), for the tileable value-noise lattice.
fn cloud_hash(mut a: u32) -> u32 {
    a = (a ^ 61) ^ (a >> 16);
    a = a.wrapping_add(a << 3);
    a ^= a >> 4;
    a = a.wrapping_mul(0x27d4_eb2d);
    a ^= a >> 15;
    a
}

// Lattice value in [0,1) at integer cell (x,y,z), wrapped by `period` so the noise tiles.
fn cloud_lattice(x: i32, y: i32, z: i32, period: i32) -> f32 {
    let xi = x.rem_euclid(period) as u32;
    let yi = y.rem_euclid(period) as u32;
    let zi = z.rem_euclid(period) as u32;
    let h = cloud_hash(xi.wrapping_mul(1619) ^ yi.wrapping_mul(31337) ^ zi.wrapping_mul(6971));
    (h as f32) / (u32::MAX as f32)
}

// Tileable value noise at (u,v,w) in [0,1) with `freq` cells across the volume (freq must divide
// NOISE_N). Smoothstep-weighted trilinear interpolation of the wrapped lattice.
fn cloud_vnoise(u: f32, v: f32, w: f32, freq: i32) -> f32 {
    let f = freq as f32;
    let (px, py, pz) = (u * f, v * f, w * f);
    let (ix, iy, iz) = (px.floor(), py.floor(), pz.floor());
    let (fx, fy, fz) = (px - ix, py - iy, pz - iz);
    let sx = fx * fx * (3.0 - 2.0 * fx);
    let sy = fy * fy * (3.0 - 2.0 * fy);
    let sz = fz * fz * (3.0 - 2.0 * fz);
    let (x0, y0, z0) = (ix as i32, iy as i32, iz as i32);
    let c = |dx: i32, dy: i32, dz: i32| cloud_lattice(x0 + dx, y0 + dy, z0 + dz, freq);
    let lerp = |a: f32, b: f32, t: f32| a + (b - a) * t;
    let x00 = lerp(c(0, 0, 0), c(1, 0, 0), sx);
    let x10 = lerp(c(0, 1, 0), c(1, 1, 0), sx);
    let x01 = lerp(c(0, 0, 1), c(1, 0, 1), sx);
    let x11 = lerp(c(0, 1, 1), c(1, 1, 1), sx);
    lerp(lerp(x00, x10, sy), lerp(x01, x11, sy), sz)
}

// Tileable fBm normalised to [0,1]. base_freq and each octave (x2) must divide NOISE_N.
fn cloud_fbm(u: f32, v: f32, w: f32, base_freq: i32, octaves: i32) -> f32 {
    let mut sum = 0.0;
    let mut amp = 0.5;
    let mut freq = base_freq;
    let mut norm = 0.0;
    for _ in 0..octaves {
        sum += amp * cloud_vnoise(u, v, w, freq);
        norm += amp;
        freq *= 2;
        amp *= 0.5;
    }
    sum / norm
}

// Bake the RGBA8 cloud noise volume: R = shape (freq 4,8,16), G = detail (freq 8,16,32).
fn generate_cloud_noise() -> Vec<u8> {
    let n = NOISE_N as usize;
    let mut data = vec![0u8; n * n * n * 4];
    let inv = 1.0 / NOISE_N as f32;
    for z in 0..n {
        for y in 0..n {
            for x in 0..n {
                let u = (x as f32 + 0.5) * inv;
                let v = (y as f32 + 0.5) * inv;
                let w = (z as f32 + 0.5) * inv;
                let shape = cloud_fbm(u, v, w, 4, 3);
                let detail = cloud_fbm(u, v, w, 8, 3);
                let i = (z * n * n + y * n + x) * 4;
                data[i] = (shape.clamp(0.0, 1.0) * 255.0) as u8;
                data[i + 1] = (detail.clamp(0.0, 1.0) * 255.0) as u8;
            }
        }
    }
    data
}

// GPU uniform for the sky pass: the pushed WgrSky (8 vec4) plus the reconstructed
// inverse view-projection and an output-mode block. Must match `Sky` in sky.wgsl.
#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
struct SkyUniform {
    inv_view_proj: [[f32; 4]; 4],
    sun_dir: [f32; 4],
    moon_dir: [f32; 4],
    rayleigh: [f32; 4],
    mie: [f32; 4],
    ground_albedo: [f32; 4],
    params: [f32; 4],
    control: [f32; 4],
    fog_color: [f32; 4],
    night_zenith: [f32; 4],
    night_horizon: [f32; 4],
    night_params: [f32; 4],
    // Cloud shell params (mirror WgrSky::cloud0/1/2/3). See sky.wgsl's Sky struct.
    cloud0: [f32; 4],
    cloud1: [f32; 4],
    cloud2: [f32; 4],
    cloud3: [f32; 4],
    // Cloud EVOLUTION offsets (runtime): x = shape, y = detail, z = weather drift, w = pad.
    // Position matters — sky.wgsl declares cloud4 between cloud3 and output, and a mismatch here
    // is a silent layout shift, not a compile error. It surfaces as "buffer bound with size N
    // where the shader expects M" on every 3D draw at once.
    cloud4: [f32; 4],
    // x = linear output (1 = write linear radiance for the tonemap resolve; 0 =
    // self-tonemap for the LDR-direct path). y = star intensity, z = lens-flare gain,
    // w = second cloud layer (cirrus) mode: 0 off, 1 flat sheet, 2 volumetric shell march.
    // (The full-vs-cheap cloud split is by entry point — fs_sky vs fs_sky_env — not a
    // runtime flag.)
    output: [f32; 4],
    // xyz = absolute world camera position, so cs_froxel can turn a marched camera-
    // relative offset into a world position for the terrain sun-shadow mask lookup.
    cam_pos: [f32; 4],
    // CLD-020 cloud sun-transmittance map. xy = world-xz of the map's min corner, SNAPPED to the
    // texel grid (an unsnapped origin makes the shadow pattern crawl whenever the camera moves,
    // which reads as the clouds sliding across the ground). z = 1/span in metres, w = strength,
    // where 0 skips the pass and leaves every surface fully lit.
    cloud_shadow: [f32; 4],
}

// The atmosphere-only fields that determine the LUTs (sun/night/exposure excluded,
// since the sun is a LUT axis and those don't change the tables). A change here
// dirties the LUTs; per-frame celestial pushes do not.
type LutKey = [f32; 14];

fn lut_key(sky: &WgrSky) -> LutKey {
    [
        sky.rayleigh[0],
        sky.rayleigh[1],
        sky.rayleigh[2],
        sky.rayleigh[3],
        sky.mie[0],
        sky.mie[1],
        sky.mie[2],
        sky.mie[3],
        sky.ground_albedo[0],
        sky.ground_albedo[1],
        sky.ground_albedo[2],
        sky.params[2],  // planet radius
        sky.params[3],  // atmosphere thickness
        sky.control[3], // ozone strength
    ]
}

/// Cloud sun-transmittance map (CLD-020). One texel per `CLOUD_SHADOW_SPAN / CLOUD_SHADOW_DIM`
/// metres of world, centred on the camera.
///
/// 512 over 4 km is ~7.8 m per texel. That is coarse for a shadow and exactly right for THIS
/// shadow: a cloud deck edge is tens of metres of penumbra anyway, so finer texels would cost
/// march work to resolve detail the phenomenon does not have. 4 km covers the view distance that
/// matters without the map's edge becoming visible as a lighting seam.
const CLOUD_SHADOW_DIM: u32 = 512;
const CLOUD_SHADOW_SPAN: f32 = 4096.0;

/// Initial second-cloud-layer (cirrus) mode: 0 = off, 1 = flat sheet, 2 = thin-shell volumetric.
///
/// Default 2, matching `Engine::SkySettings`. `WGR_CIRRUS` overrides it, and exists because the
/// only honest way to price the volumetric path is a `--benchmark` run — which runs `--no-dev`,
/// so the Sky tab's toggle is not reachable. It prints what it selected so a benchmark log is
/// self-describing rather than something you have to take on trust.
fn cirrus_mode_from_env() -> f32 {
    match std::env::var("WGR_CIRRUS")
        .ok()
        .and_then(|v| v.parse::<f32>().ok())
    {
        Some(v) => {
            let m = v.clamp(0.0, 2.0);
            eprintln!(
                "[wgr] sky: cirrus mode {} from WGR_CIRRUS (0 off, 1 flat, 2 volumetric)",
                m
            );
            m
        }
        None => 2.0,
    }
}

pub struct Sky {
    // Main fullscreen sky pipeline (targets the scene format).
    sky_pipeline: wgpu::RenderPipeline,
    sky_bind: wgpu::BindGroup,
    // Reflection environment map: same group(0) as the sky pass (reuses sky_bind), fs_sky_env
    // entry, baked into env_view each frame. `_tex` keeps the texture alive.
    env_pipeline: wgpu::RenderPipeline,
    #[allow(dead_code)]
    env_tex: wgpu::Texture,
    env_view: wgpu::TextureView,
    // Tileable 3D cloud noise, sampled by the cloud march (bound into sky_bind). Held to keep the
    // texture alive (the view/sampler are owned by the bind group).
    #[allow(dead_code)]
    cloud_noise_tex: wgpu::Texture,
    // SH-9 projection of the env map into diffuse sky irradiance (sky_sh.wgsl). Computed each frame
    // after the env bake; the buffer is lent to the camera group so lit meshes + terrain read it.
    sh_pipeline: wgpu::ComputePipeline,
    sh_bind: wgpu::BindGroup,
    sh_buf: wgpu::Buffer,
    // LUT build pipelines (target LUT_FORMAT) + their target views/binds.
    transmittance_pipeline: wgpu::RenderPipeline,
    transmittance_bind: wgpu::BindGroup,
    transmittance_view: wgpu::TextureView,
    multiscatter_pipeline: wgpu::RenderPipeline,
    multiscatter_bind: wgpu::BindGroup,
    multiscatter_view: wgpu::TextureView,
    // Aerial-perspective froxel volume + its compute fill (see cs_froxel in sky.wgsl).
    // The `_tex` handle is held only to keep the texture alive; both bind groups view it.
    froxel_pipeline: wgpu::ComputePipeline,
    froxel_bind: wgpu::BindGroup,
    #[allow(dead_code)]
    froxel_tex: wgpu::Texture,
    froxel_view: wgpu::TextureView,
    cloud_shadow_tex: wgpu::Texture,
    cloud_shadow_view: wgpu::TextureView,
    cloud_shadow_pipeline: wgpu::ComputePipeline,
    cloud_shadow_strength: f32,
    cloud_shadow_map_params: [f32; 4],
    star_intensity: f32,
    /// Lens-flare gain. 0 disables the effect outright (the shader early-outs).
    lens_flare: f32,
    /// Second cloud layer (high cirrus) mode, written to the sky UBO's `output.w`:
    /// 0 = off, 1 = flat sheet, 2 = thin-shell volumetric march. Rides the spare lane of an
    /// existing vec4 rather than growing WgrSkyLook, for the reason stated on
    /// `wgr_set_cloud_shadow_strength`: the look struct's size is part of the ABI handshake.
    cirrus_mode: f32,
    // Group(1) of cs_froxel: the terrain sun-shadow mask, rebuilt each frame (the mask
    // texture is Terrain-owned and regenerated) from the lent view + these two.
    froxel_shadow_layout: wgpu::BindGroupLayout,
    mask_sampler: wgpu::Sampler,
    mapping_buf: wgpu::Buffer,
    csm_cmp_sampler: wgpu::Sampler,
    csm_ubo: wgpu::Buffer,
    uniform_buf: wgpu::Buffer,
    // Phase 1 depth-aware over-scene clouds: fs_cloud marches at LOW RES into cloud_lo (bounding at
    // the resolved scene depth via group(1)), then cloud_composite blends it over the lit scene.
    // cloud_lo resizes with the scene. Only used on the HDR (linear) path.
    cloud_pipeline: wgpu::RenderPipeline,
    cloud_depth_layout: wgpu::BindGroupLayout,
    cloud_lo_tex: Option<wgpu::Texture>,
    cloud_lo_view: Option<wgpu::TextureView>,
    cloud_lo_size: (u32, u32),
    cloud_composite_pipeline: wgpu::RenderPipeline,
    cloud_composite_layout: wgpu::BindGroupLayout,
    cloud_composite_bind: Option<wgpu::BindGroup>,
    cloud_composite_sampler: wgpu::Sampler,
    // 1 = scene target is the linear HDR texture (tonemap resolves later); 0 =
    // LDR-direct, so the sky self-tonemaps. Fixed at construction with the format.
    linear: f32,
    // LUTs are rebuilt only when the atmosphere key changes (None = never built).
    last_lut: Option<LutKey>,
    lut_dirty: bool,
}

impl Sky {
    pub fn new(
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        color_format: wgpu::TextureFormat,
        sample_count: u32,
    ) -> Self {
        let shader = device.create_shader_module(wgpu::ShaderModuleDescriptor {
            label: Some("wgr_sky_shader"),
            source: wgpu::ShaderSource::Wgsl(include_str!("sky.wgsl").into()),
        });

        // Bind-group entry templates (binding numbers match sky.wgsl's globals).
        let uniform_entry = |binding| wgpu::BindGroupLayoutEntry {
            binding,
            visibility: wgpu::ShaderStages::VERTEX | wgpu::ShaderStages::FRAGMENT,
            ty: wgpu::BindingType::Buffer {
                ty: wgpu::BufferBindingType::Uniform,
                has_dynamic_offset: false,
                min_binding_size: None,
            },
            count: None,
        };
        let sampler_entry = wgpu::BindGroupLayoutEntry {
            binding: 1,
            visibility: wgpu::ShaderStages::FRAGMENT,
            ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Filtering),
            count: None,
        };
        let tex_entry = |binding| wgpu::BindGroupLayoutEntry {
            binding,
            visibility: wgpu::ShaderStages::FRAGMENT,
            ty: wgpu::BindingType::Texture {
                sample_type: wgpu::TextureSampleType::Float { filterable: true },
                view_dimension: wgpu::TextureViewDimension::D2,
                multisampled: false,
            },
            count: None,
        };

        // Per-pass layouts: transmittance reads only the uniform; multiscatter also
        // reads the transmittance LUT; the main pass reads both LUTs.
        let transmittance_layout =
            device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
                label: Some("wgr_sky_transmittance_layout"),
                entries: &[uniform_entry(0)],
            });
        let multiscatter_layout =
            device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
                label: Some("wgr_sky_multiscatter_layout"),
                entries: &[uniform_entry(0), sampler_entry, tex_entry(2)],
            });
        // Cloud noise: a 3D texture at binding 4 + its own Repeat sampler at binding 6 (binding 5 is
        // the froxel storage image in the shared module). Only fs_sky / fs_sky_env reference these.
        let cloud_tex_entry = wgpu::BindGroupLayoutEntry {
            binding: 4,
            visibility: wgpu::ShaderStages::FRAGMENT,
            ty: wgpu::BindingType::Texture {
                sample_type: wgpu::TextureSampleType::Float { filterable: true },
                view_dimension: wgpu::TextureViewDimension::D3,
                multisampled: false,
            },
            count: None,
        };
        let cloud_sampler_entry = wgpu::BindGroupLayoutEntry {
            binding: 6,
            visibility: wgpu::ShaderStages::FRAGMENT,
            ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Filtering),
            count: None,
        };
        let sky_layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("wgr_sky_layout"),
            entries: &[
                uniform_entry(0),
                sampler_entry,
                tex_entry(2),
                tex_entry(3),
                cloud_tex_entry,
                cloud_sampler_entry,
            ],
        });
        // Only the main sky pass draws into the (MSAA) scene target; the transmittance +
        // multiscatter LUTs are single-sample offscreen renders, so each pipeline takes its
        // own sample count.
        let make_pipeline = |label: &str,
                             layout: &wgpu::BindGroupLayout,
                             fs: &str,
                             format: wgpu::TextureFormat,
                             samples: u32| {
            let pl = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
                label: Some(label),
                bind_group_layouts: &[Some(layout)],
                immediate_size: 0,
            });
            device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
                label: Some(label),
                layout: Some(&pl),
                vertex: wgpu::VertexState {
                    module: &shader,
                    entry_point: Some("vs_main"),
                    buffers: &[],
                    compilation_options: Default::default(),
                },
                primitive: wgpu::PrimitiveState::default(),
                depth_stencil: None,
                multisample: wgpu::MultisampleState {
                    count: samples,
                    ..Default::default()
                },
                fragment: Some(wgpu::FragmentState {
                    module: &shader,
                    entry_point: Some(fs),
                    targets: &[Some(wgpu::ColorTargetState {
                        format,
                        blend: None,
                        write_mask: wgpu::ColorWrites::ALL,
                    })],
                    compilation_options: Default::default(),
                }),
                multiview_mask: None,
                cache: None,
            })
        };

        let transmittance_pipeline = make_pipeline(
            "wgr_sky_transmittance",
            &transmittance_layout,
            "fs_transmittance",
            LUT_FORMAT,
            1,
        );
        let multiscatter_pipeline = make_pipeline(
            "wgr_sky_multiscatter",
            &multiscatter_layout,
            "fs_multiscatter",
            LUT_FORMAT,
            1,
        );
        let sky_pipeline =
            make_pipeline("wgr_sky", &sky_layout, "fs_sky", color_format, sample_count);
        // Env-map bake: same group(0) layout as the sky pass, single-sample, LUT_FORMAT target.
        let env_pipeline = make_pipeline("wgr_sky_env", &sky_layout, "fs_sky_env", ENV_FORMAT, 1);
        let env_tex = device.create_texture(&wgpu::TextureDescriptor {
            label: Some("wgr_sky_env"),
            size: wgpu::Extent3d {
                width: ENV_W,
                height: ENV_H,
                depth_or_array_layers: 1,
            },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: ENV_FORMAT,
            usage: wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::TEXTURE_BINDING,
            view_formats: &[],
        });
        let env_view = env_tex.create_view(&wgpu::TextureViewDescriptor::default());

        // SH-9 sky-irradiance projection: reads the env map (textureLoad, non-filtering) and writes
        // 9 vec4 RGB coefficients into sh_buf (also bound UNIFORM into the camera group). Zero-init
        // so a read before the first bake (or on the non-sky-lit path, where it's unread) is defined.
        let sh_shader = device.create_shader_module(wgpu::ShaderModuleDescriptor {
            label: Some("wgr_sky_sh_shader"),
            source: wgpu::ShaderSource::Wgsl(include_str!("sky_sh.wgsl").into()),
        });
        let sh_buf = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
            label: Some("wgr_sky_sh"),
            contents: &[0u8; 9 * 16],
            usage: wgpu::BufferUsages::STORAGE
                | wgpu::BufferUsages::UNIFORM
                | wgpu::BufferUsages::COPY_DST,
        });
        let sh_layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("wgr_sky_sh_layout"),
            entries: &[
                wgpu::BindGroupLayoutEntry {
                    binding: 0,
                    visibility: wgpu::ShaderStages::COMPUTE,
                    ty: wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Float { filterable: false },
                        view_dimension: wgpu::TextureViewDimension::D2,
                        multisampled: false,
                    },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 1,
                    visibility: wgpu::ShaderStages::COMPUTE,
                    ty: wgpu::BindingType::Buffer {
                        ty: wgpu::BufferBindingType::Storage { read_only: false },
                        has_dynamic_offset: false,
                        min_binding_size: None,
                    },
                    count: None,
                },
            ],
        });
        let sh_bind = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("wgr_sky_sh_bind"),
            layout: &sh_layout,
            entries: &[
                wgpu::BindGroupEntry {
                    binding: 0,
                    resource: wgpu::BindingResource::TextureView(&env_view),
                },
                wgpu::BindGroupEntry {
                    binding: 1,
                    resource: sh_buf.as_entire_binding(),
                },
            ],
        });
        let sh_pipeline = {
            let pl = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
                label: Some("wgr_sky_sh"),
                bind_group_layouts: &[Some(&sh_layout)],
                immediate_size: 0,
            });
            device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
                label: Some("wgr_sky_sh"),
                layout: Some(&pl),
                module: &sh_shader,
                entry_point: Some("cs_sky_sh"),
                compilation_options: Default::default(),
                cache: None,
            })
        };

        let make_lut = |label: &str, w: u32, h: u32| {
            let tex = device.create_texture(&wgpu::TextureDescriptor {
                label: Some(label),
                size: wgpu::Extent3d {
                    width: w,
                    height: h,
                    depth_or_array_layers: 1,
                },
                mip_level_count: 1,
                sample_count: 1,
                dimension: wgpu::TextureDimension::D2,
                format: LUT_FORMAT,
                usage: wgpu::TextureUsages::RENDER_ATTACHMENT
                    | wgpu::TextureUsages::TEXTURE_BINDING,
                view_formats: &[],
            });
            tex.create_view(&wgpu::TextureViewDescriptor::default())
        };
        let transmittance_view = make_lut(
            "wgr_sky_transmittance_lut",
            TRANSMITTANCE_W,
            TRANSMITTANCE_H,
        );
        let multiscatter_view = make_lut(
            "wgr_sky_multiscatter_lut",
            MULTISCATTER_SIZE,
            MULTISCATTER_SIZE,
        );

        let sampler = device.create_sampler(&wgpu::SamplerDescriptor {
            label: Some("wgr_sky_lut_sampler"),
            address_mode_u: wgpu::AddressMode::ClampToEdge,
            address_mode_v: wgpu::AddressMode::ClampToEdge,
            mag_filter: wgpu::FilterMode::Linear,
            min_filter: wgpu::FilterMode::Linear,
            ..Default::default()
        });

        // Tileable 3D cloud noise, baked once and uploaded. Repeat sampler so the march can sample
        // arbitrarily far / wind-scrolled coordinates without the analytic-hash precision moire.
        let cloud_noise_tex = device.create_texture_with_data(
            queue,
            &wgpu::TextureDescriptor {
                label: Some("wgr_sky_cloud_noise"),
                size: wgpu::Extent3d {
                    width: NOISE_N,
                    height: NOISE_N,
                    depth_or_array_layers: NOISE_N,
                },
                mip_level_count: 1,
                sample_count: 1,
                dimension: wgpu::TextureDimension::D3,
                format: NOISE_FORMAT,
                usage: wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::COPY_DST,
                view_formats: &[],
            },
            wgpu::util::TextureDataOrder::LayerMajor,
            &generate_cloud_noise(),
        );
        let cloud_noise_view = cloud_noise_tex.create_view(&wgpu::TextureViewDescriptor {
            label: Some("wgr_sky_cloud_noise_view"),
            dimension: Some(wgpu::TextureViewDimension::D3),
            ..Default::default()
        });
        let cloud_sampler = device.create_sampler(&wgpu::SamplerDescriptor {
            label: Some("wgr_sky_cloud_sampler"),
            address_mode_u: wgpu::AddressMode::Repeat,
            address_mode_v: wgpu::AddressMode::Repeat,
            address_mode_w: wgpu::AddressMode::Repeat,
            mag_filter: wgpu::FilterMode::Linear,
            min_filter: wgpu::FilterMode::Linear,
            ..Default::default()
        });

        let uniform_buf = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
            label: Some("wgr_sky_uniform"),
            contents: bytemuck::bytes_of(&SkyUniform::zeroed()),
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
        });

        let transmittance_bind = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("wgr_sky_transmittance_bind"),
            layout: &transmittance_layout,
            entries: &[wgpu::BindGroupEntry {
                binding: 0,
                resource: uniform_buf.as_entire_binding(),
            }],
        });
        let multiscatter_bind = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("wgr_sky_multiscatter_bind"),
            layout: &multiscatter_layout,
            entries: &[
                wgpu::BindGroupEntry {
                    binding: 0,
                    resource: uniform_buf.as_entire_binding(),
                },
                wgpu::BindGroupEntry {
                    binding: 1,
                    resource: wgpu::BindingResource::Sampler(&sampler),
                },
                wgpu::BindGroupEntry {
                    binding: 2,
                    resource: wgpu::BindingResource::TextureView(&transmittance_view),
                },
            ],
        });
        let sky_bind = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("wgr_sky_bind"),
            layout: &sky_layout,
            entries: &[
                wgpu::BindGroupEntry {
                    binding: 0,
                    resource: uniform_buf.as_entire_binding(),
                },
                wgpu::BindGroupEntry {
                    binding: 1,
                    resource: wgpu::BindingResource::Sampler(&sampler),
                },
                wgpu::BindGroupEntry {
                    binding: 2,
                    resource: wgpu::BindingResource::TextureView(&transmittance_view),
                },
                wgpu::BindGroupEntry {
                    binding: 3,
                    resource: wgpu::BindingResource::TextureView(&multiscatter_view),
                },
                wgpu::BindGroupEntry {
                    binding: 4,
                    resource: wgpu::BindingResource::TextureView(&cloud_noise_view),
                },
                wgpu::BindGroupEntry {
                    binding: 6,
                    resource: wgpu::BindingResource::Sampler(&cloud_sampler),
                },
            ],
        });

        // Froxel volume + compute fill. Same atmosphere inputs as the render passes
        // (uniform + both LUTs + sampler) at COMPUTE visibility, plus the 3D volume as a
        // write storage texture at binding 5. The volume is frustum-parameterised (not
        // screen-sized), so it is a fixed 32^3 built once here.
        let froxel_tex = device.create_texture(&wgpu::TextureDescriptor {
            label: Some("wgr_sky_froxel"),
            size: wgpu::Extent3d {
                width: FROXEL_W,
                height: FROXEL_H,
                depth_or_array_layers: FROXEL_D,
            },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D3,
            format: FROXEL_FORMAT,
            usage: wgpu::TextureUsages::STORAGE_BINDING | wgpu::TextureUsages::TEXTURE_BINDING,
            view_formats: &[],
        });
        let froxel_view = froxel_tex.create_view(&wgpu::TextureViewDescriptor {
            label: Some("wgr_sky_froxel_view"),
            dimension: Some(wgpu::TextureViewDimension::D3),
            ..Default::default()
        });

        // rgba8unorm, not r8unorm: R8Unorm is NOT a core WebGPU storage format, and asking for it
        // produced an invalid texture whose invalid view then failed BOTH the sky and the camera
        // bind groups -- a cascade whose first error names a bind group and never mentions the
        // format. Only .r is used; 512x512x4 = 1 MB. Also filterable when sampled, which the
        // single-channel float formats are not guaranteed to be.
        let cloud_shadow_tex = device.create_texture(&wgpu::TextureDescriptor {
            label: Some("wgr_cloud_shadow"),
            size: wgpu::Extent3d {
                width: CLOUD_SHADOW_DIM,
                height: CLOUD_SHADOW_DIM,
                depth_or_array_layers: 1,
            },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: wgpu::TextureFormat::Rgba8Unorm,
            usage: wgpu::TextureUsages::STORAGE_BINDING | wgpu::TextureUsages::TEXTURE_BINDING,
            view_formats: &[],
        });
        let cloud_shadow_view = cloud_shadow_tex.create_view(&Default::default());

        let compute_uniform = wgpu::BindGroupLayoutEntry {
            binding: 0,
            visibility: wgpu::ShaderStages::COMPUTE,
            ty: wgpu::BindingType::Buffer {
                ty: wgpu::BufferBindingType::Uniform,
                has_dynamic_offset: false,
                min_binding_size: None,
            },
            count: None,
        };
        let compute_sampler = wgpu::BindGroupLayoutEntry {
            binding: 1,
            visibility: wgpu::ShaderStages::COMPUTE,
            ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Filtering),
            count: None,
        };
        let compute_tex = |binding| wgpu::BindGroupLayoutEntry {
            binding,
            visibility: wgpu::ShaderStages::COMPUTE,
            ty: wgpu::BindingType::Texture {
                sample_type: wgpu::TextureSampleType::Float { filterable: true },
                view_dimension: wgpu::TextureViewDimension::D2,
                multisampled: false,
            },
            count: None,
        };
        let froxel_layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("wgr_sky_froxel_layout"),
            entries: &[
                compute_uniform,
                compute_sampler,
                compute_tex(2),
                compute_tex(3),
                wgpu::BindGroupLayoutEntry {
                    binding: 5,
                    visibility: wgpu::ShaderStages::COMPUTE,
                    ty: wgpu::BindingType::StorageTexture {
                        access: wgpu::StorageTextureAccess::WriteOnly,
                        format: FROXEL_FORMAT,
                        view_dimension: wgpu::TextureViewDimension::D3,
                    },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 7,
                    visibility: wgpu::ShaderStages::COMPUTE,
                    ty: wgpu::BindingType::StorageTexture {
                        access: wgpu::StorageTextureAccess::WriteOnly,
                        format: wgpu::TextureFormat::Rgba8Unorm,
                        view_dimension: wgpu::TextureViewDimension::D2,
                    },
                    count: None,
                },
                // cs_cloud_shadow marches the same cloud field the sky raymarch uses, so this
                // compute layout needs the noise volume and its Repeat sampler too. cs_froxel does
                // not touch them, but a layout is per-group and not per-entry-point.
                wgpu::BindGroupLayoutEntry {
                    binding: 4,
                    visibility: wgpu::ShaderStages::COMPUTE,
                    ty: wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Float { filterable: true },
                        view_dimension: wgpu::TextureViewDimension::D3,
                        multisampled: false,
                    },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 6,
                    visibility: wgpu::ShaderStages::COMPUTE,
                    ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Filtering),
                    count: None,
                },
            ],
        });
        // Group(1): the terrain sun-shadow mask (texture + sampler) + its world->UV mapping
        // uniform, so cs_froxel can occlude the fog by terrain. The mask is Terrain-owned and
        // lent by view (regenerated on heightmap change), so this bind rebuilds each frame in
        // render_froxel; the sampler + mapping buffer are owned here and created once.
        let froxel_shadow_layout =
            device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
                label: Some("wgr_sky_froxel_shadow_layout"),
                entries: &[
                    compute_tex(0),
                    wgpu::BindGroupLayoutEntry {
                        binding: 1,
                        visibility: wgpu::ShaderStages::COMPUTE,
                        ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Filtering),
                        count: None,
                    },
                    wgpu::BindGroupLayoutEntry {
                        binding: 2,
                        visibility: wgpu::ShaderStages::COMPUTE,
                        ty: wgpu::BindingType::Buffer {
                            ty: wgpu::BufferBindingType::Uniform,
                            has_dynamic_offset: false,
                            min_binding_size: wgpu::BufferSize::new(std::mem::size_of::<
                                crate::terrain::TerrainShadowMap,
                            >()
                                as u64),
                        },
                        count: None,
                    },
                    // Cascade shadow depth (D2Array) + comparison sampler + the cascade matrices,
                    // so cs_froxel occludes the fog by objects/terrain casters for crisp shafts.
                    wgpu::BindGroupLayoutEntry {
                        binding: 3,
                        visibility: wgpu::ShaderStages::COMPUTE,
                        ty: wgpu::BindingType::Texture {
                            sample_type: wgpu::TextureSampleType::Depth,
                            view_dimension: wgpu::TextureViewDimension::D2Array,
                            multisampled: false,
                        },
                        count: None,
                    },
                    wgpu::BindGroupLayoutEntry {
                        binding: 4,
                        visibility: wgpu::ShaderStages::COMPUTE,
                        ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Comparison),
                        count: None,
                    },
                    wgpu::BindGroupLayoutEntry {
                        binding: 5,
                        visibility: wgpu::ShaderStages::COMPUTE,
                        ty: wgpu::BindingType::Buffer {
                            ty: wgpu::BufferBindingType::Uniform,
                            has_dynamic_offset: false,
                            min_binding_size: wgpu::BufferSize::new(std::mem::size_of::<
                                crate::ffi::WgrCameraShadow,
                            >()
                                as u64),
                        },
                        count: None,
                    },
                ],
            });
        let csm_cmp_sampler = device.create_sampler(&wgpu::SamplerDescriptor {
            label: Some("wgr_sky_froxel_csm_sampler"),
            compare: Some(wgpu::CompareFunction::LessEqual),
            ..Default::default()
        });
        let csm_ubo = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("wgr_sky_froxel_csm"),
            size: std::mem::size_of::<crate::ffi::WgrCameraShadow>() as u64,
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        let mask_sampler = device.create_sampler(&wgpu::SamplerDescriptor {
            label: Some("wgr_sky_froxel_mask_sampler"),
            address_mode_u: wgpu::AddressMode::ClampToEdge,
            address_mode_v: wgpu::AddressMode::ClampToEdge,
            address_mode_w: wgpu::AddressMode::ClampToEdge,
            mag_filter: wgpu::FilterMode::Linear,
            min_filter: wgpu::FilterMode::Linear,
            ..Default::default()
        });
        let mapping_buf = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("wgr_sky_froxel_mapping"),
            size: std::mem::size_of::<crate::terrain::TerrainShadowMap>() as u64,
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        let froxel_pipeline = {
            let pl = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
                label: Some("wgr_sky_froxel"),
                bind_group_layouts: &[Some(&froxel_layout), Some(&froxel_shadow_layout)],
                immediate_size: 0,
            });
            device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
                label: Some("wgr_sky_froxel"),
                layout: Some(&pl),
                module: &shader,
                entry_point: Some("cs_froxel"),
                compilation_options: Default::default(),
                cache: None,
            })
        };
        // Same group(0) as the froxel fill -- it needs the sky uniform and the cloud noise, and
        // reusing the layout avoids a second bind group holding the same four resources. It takes
        // only group(0), so the pipeline layout stops there.
        let cloud_shadow_pipeline = {
            let pl = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
                label: Some("wgr_cloud_shadow"),
                bind_group_layouts: &[Some(&froxel_layout)],
                immediate_size: 0,
            });
            device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
                label: Some("wgr_cloud_shadow"),
                layout: Some(&pl),
                module: &shader,
                entry_point: Some("cs_cloud_shadow"),
                compilation_options: Default::default(),
                cache: None,
            })
        };
        let froxel_bind = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("wgr_sky_froxel_bind"),
            layout: &froxel_layout,
            entries: &[
                wgpu::BindGroupEntry {
                    binding: 0,
                    resource: uniform_buf.as_entire_binding(),
                },
                wgpu::BindGroupEntry {
                    binding: 1,
                    resource: wgpu::BindingResource::Sampler(&sampler),
                },
                wgpu::BindGroupEntry {
                    binding: 2,
                    resource: wgpu::BindingResource::TextureView(&transmittance_view),
                },
                wgpu::BindGroupEntry {
                    binding: 3,
                    resource: wgpu::BindingResource::TextureView(&multiscatter_view),
                },
                wgpu::BindGroupEntry {
                    binding: 5,
                    resource: wgpu::BindingResource::TextureView(&froxel_view),
                },
                wgpu::BindGroupEntry {
                    binding: 7,
                    resource: wgpu::BindingResource::TextureView(&cloud_shadow_view),
                },
                wgpu::BindGroupEntry {
                    binding: 4,
                    resource: wgpu::BindingResource::TextureView(&cloud_noise_view),
                },
                wgpu::BindGroupEntry {
                    binding: 6,
                    resource: wgpu::BindingResource::Sampler(&cloud_sampler),
                },
            ],
        });

        // ---- Phase 1: depth-aware over-scene cloud pass + composite ----
        // fs_cloud: low-res march (group(0) = the sky bind; group(1) = the resolved scene depth).
        let cloud_depth_layout =
            device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
                label: Some("wgr_cloud_depth_layout"),
                entries: &[wgpu::BindGroupLayoutEntry {
                    binding: 6,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Depth,
                        view_dimension: wgpu::TextureViewDimension::D2,
                        multisampled: false,
                    },
                    count: None,
                }],
            });
        let cloud_pipeline = {
            let pl = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
                label: Some("wgr_cloud"),
                bind_group_layouts: &[Some(&sky_layout), Some(&cloud_depth_layout)],
                immediate_size: 0,
            });
            device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
                label: Some("wgr_cloud"),
                layout: Some(&pl),
                vertex: wgpu::VertexState {
                    module: &shader,
                    entry_point: Some("vs_main"),
                    buffers: &[],
                    compilation_options: Default::default(),
                },
                primitive: wgpu::PrimitiveState::default(),
                depth_stencil: None,
                multisample: wgpu::MultisampleState::default(), // low-res buffer is single-sample
                fragment: Some(wgpu::FragmentState {
                    module: &shader,
                    entry_point: Some("fs_cloud"),
                    targets: &[Some(wgpu::ColorTargetState {
                        format: crate::HDR_FORMAT,
                        blend: None,
                        write_mask: wgpu::ColorWrites::ALL,
                    })],
                    compilation_options: Default::default(),
                }),
                multiview_mask: None,
                cache: None,
            })
        };

        // cloud_composite: upsample the low-res buffer + premultiplied blend over the MSAA scene.
        let composite_shader = device.create_shader_module(wgpu::ShaderModuleDescriptor {
            label: Some("wgr_cloud_composite_shader"),
            source: wgpu::ShaderSource::Wgsl(include_str!("cloud_composite.wgsl").into()),
        });
        let cloud_composite_layout =
            device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
                label: Some("wgr_cloud_composite_layout"),
                entries: &[
                    wgpu::BindGroupLayoutEntry {
                        binding: 0,
                        visibility: wgpu::ShaderStages::FRAGMENT,
                        ty: wgpu::BindingType::Texture {
                            sample_type: wgpu::TextureSampleType::Float { filterable: true },
                            view_dimension: wgpu::TextureViewDimension::D2,
                            multisampled: false,
                        },
                        count: None,
                    },
                    wgpu::BindGroupLayoutEntry {
                        binding: 1,
                        visibility: wgpu::ShaderStages::FRAGMENT,
                        ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Filtering),
                        count: None,
                    },
                    // Full-res resolved scene depth, for the depth-aware (bilateral) upsample.
                    wgpu::BindGroupLayoutEntry {
                        binding: 2,
                        visibility: wgpu::ShaderStages::FRAGMENT,
                        ty: wgpu::BindingType::Texture {
                            sample_type: wgpu::TextureSampleType::Depth,
                            view_dimension: wgpu::TextureViewDimension::D2,
                            multisampled: false,
                        },
                        count: None,
                    },
                ],
            });
        let cloud_composite_pipeline = {
            let pl = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
                label: Some("wgr_cloud_composite"),
                bind_group_layouts: &[Some(&cloud_composite_layout)],
                immediate_size: 0,
            });
            device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
                label: Some("wgr_cloud_composite"),
                layout: Some(&pl),
                vertex: wgpu::VertexState {
                    module: &composite_shader,
                    entry_point: Some("vs_main"),
                    buffers: &[],
                    compilation_options: Default::default(),
                },
                primitive: wgpu::PrimitiveState::default(),
                depth_stencil: None,
                multisample: wgpu::MultisampleState {
                    count: sample_count,
                    ..Default::default()
                },
                fragment: Some(wgpu::FragmentState {
                    module: &composite_shader,
                    entry_point: Some("fs_main"),
                    targets: &[Some(wgpu::ColorTargetState {
                        format: color_format,
                        // out = inscatter*1 + scene*src.a (src.a = cloud transmittance).
                        blend: Some(wgpu::BlendState {
                            color: wgpu::BlendComponent {
                                src_factor: wgpu::BlendFactor::One,
                                dst_factor: wgpu::BlendFactor::SrcAlpha,
                                operation: wgpu::BlendOperation::Add,
                            },
                            alpha: wgpu::BlendComponent {
                                src_factor: wgpu::BlendFactor::Zero,
                                dst_factor: wgpu::BlendFactor::One,
                                operation: wgpu::BlendOperation::Add,
                            },
                        }),
                        write_mask: wgpu::ColorWrites::ALL,
                    })],
                    compilation_options: Default::default(),
                }),
                multiview_mask: None,
                cache: None,
            })
        };
        let cloud_composite_sampler = device.create_sampler(&wgpu::SamplerDescriptor {
            label: Some("wgr_cloud_composite_sampler"),
            address_mode_u: wgpu::AddressMode::ClampToEdge,
            address_mode_v: wgpu::AddressMode::ClampToEdge,
            mag_filter: wgpu::FilterMode::Linear,
            min_filter: wgpu::FilterMode::Linear,
            ..Default::default()
        });

        let linear = if color_format == crate::HDR_FORMAT {
            1.0
        } else {
            0.0
        };

        Self {
            sky_pipeline,
            sky_bind,
            env_pipeline,
            env_tex,
            env_view,
            cloud_noise_tex,
            cloud_pipeline,
            cloud_depth_layout,
            cloud_lo_tex: None,
            cloud_lo_view: None,
            cloud_lo_size: (0, 0),
            cloud_composite_pipeline,
            cloud_composite_layout,
            cloud_composite_bind: None,
            cloud_composite_sampler,
            sh_pipeline,
            sh_bind,
            sh_buf,
            transmittance_pipeline,
            transmittance_bind,
            transmittance_view,
            multiscatter_pipeline,
            multiscatter_bind,
            multiscatter_view,
            froxel_pipeline,
            froxel_bind,
            froxel_tex,
            cloud_shadow_tex,
            cloud_shadow_view,
            cloud_shadow_pipeline,
            // Default ON at a moderate strength: clouds that do not shade the ground are the
            // thing a player notices, and the pass is one 512x512 dispatch.
            cloud_shadow_strength: 0.85,
            // Stars on by default: the night sky had nothing in it at all, which is the actual
            // complaint. Additive on top of the authored night floor, so it brightens nothing
            // during the day -- night_blend gates it to zero while the sun is up.
            star_intensity: 1.0,
            // Matches Engine::SkySettings' safe default.  The C++ side normally pushes this on
            // the first frame, but keep the renderer's own initial state safe as well.
            lens_flare: 0.0,
            // Second cloud layer on, volumetric, matching Engine::SkySettings' defaults. The env
            // override exists so the two modes can be A/B'd from a --benchmark run, which has no
            // dev panel: WGR_CIRRUS=0 off, 1 flat, 2 volumetric.
            cirrus_mode: cirrus_mode_from_env(),
            cloud_shadow_map_params: [0.0, 0.0, 1.0 / CLOUD_SHADOW_SPAN, 0.85],
            froxel_view,
            froxel_shadow_layout,
            mask_sampler,
            mapping_buf,
            csm_cmp_sampler,
            csm_ubo,
            uniform_buf,
            linear,
            last_lut: None,
            lut_dirty: true,
        }
    }

    /// World mapping for the cloud sun-transmittance map: a square of
    /// `CLOUD_SHADOW_SPAN` metres centred on the camera, with the min corner SNAPPED to
    /// the map's own texel grid.
    ///
    /// The snap is the whole trick. An origin that tracks the camera continuously
    /// re-rasterises the same clouds into different texels every frame, and the eye reads
    /// that as the shadows crawling over the ground independently of the wind. Snapping
    /// means a texel keeps covering the same square of world until it leaves the map.
    fn cloud_shadow_mapping(&self, cam_pos: [f32; 4]) -> [f32; 4] {
        let texel = CLOUD_SHADOW_SPAN / CLOUD_SHADOW_DIM as f32;
        let snap = |v: f32| (v / texel).floor() * texel;
        [
            snap(cam_pos[0] - CLOUD_SHADOW_SPAN * 0.5),
            snap(cam_pos[2] - CLOUD_SHADOW_SPAN * 0.5),
            1.0 / CLOUD_SHADOW_SPAN,
            self.cloud_shadow_strength,
        ]
    }

    /// The mapping last uploaded, so the frame bind group can publish the same numbers to
    /// the shaders that SAMPLE the map. Two independently computed mappings would drift.
    pub fn cloud_shadow_mapping_current(&self) -> [f32; 4] {
        self.cloud_shadow_map_params
    }

    pub fn set_lens_flare(&mut self, intensity: f32) {
        self.lens_flare = intensity.clamp(0.0, 4.0);
    }

    pub fn set_star_intensity(&mut self, intensity: f32) {
        self.star_intensity = intensity.clamp(0.0, 4.0);
    }

    pub fn set_cloud_shadow_strength(&mut self, strength: f32) {
        self.cloud_shadow_strength = strength.clamp(0.0, 1.0);
    }

    /// Second cloud layer (high cirrus): 0 = off, 1 = flat sheet, 2 = thin-shell volumetric march.
    pub fn set_cirrus_mode(&mut self, mode: u32) {
        self.cirrus_mode = (mode.min(2)) as f32;
    }

    pub fn cloud_shadow_view(&self) -> &wgpu::TextureView {
        &self.cloud_shadow_view
    }

    // Rebuild the sky uniform for this frame and flag the LUTs dirty if the
    // atmosphere parameters changed.
    pub fn upload(
        &mut self,
        queue: &wgpu::Queue,
        sky: &WgrSky,
        inv_view_proj: [[f32; 4]; 4],
        cam_pos: [f32; 4],
        shadow_map: &crate::terrain::TerrainShadowMap,
        csm: &crate::ffi::WgrCameraShadow,
    ) {
        let key = lut_key(sky);
        if self.last_lut != Some(key) {
            self.last_lut = Some(key);
            self.lut_dirty = true;
        }
        let u = SkyUniform {
            inv_view_proj,
            sun_dir: sky.sun_dir,
            moon_dir: sky.moon_dir,
            rayleigh: sky.rayleigh,
            mie: sky.mie,
            ground_albedo: sky.ground_albedo,
            params: sky.params,
            control: sky.control,
            fog_color: sky.fog_color,
            night_zenith: sky.night_zenith,
            night_horizon: sky.night_horizon,
            night_params: sky.night_params,
            cloud0: sky.cloud0,
            cloud1: sky.cloud1,
            cloud2: sky.cloud2,
            cloud3: sky.cloud3,
            cloud4: sky.cloud4,
            output: [
                self.linear,
                self.star_intensity,
                self.lens_flare,
                self.cirrus_mode,
            ],
            cam_pos,
            cloud_shadow: self.cloud_shadow_mapping(cam_pos),
        };
        self.cloud_shadow_map_params = u.cloud_shadow;
        queue.write_buffer(&self.uniform_buf, 0, bytemuck::bytes_of(&u));
        // Terrain sun-shadow mask mapping for cs_froxel's occlusion lookup (own copy so
        // the froxel fill doesn't depend on the graphics camera group's buffer).
        queue.write_buffer(&self.mapping_buf, 0, bytemuck::bytes_of(shadow_map));
        // The main camera's cascade matrices, for the froxel's near-field CSM occlusion.
        queue.write_buffer(&self.csm_ubo, 0, bytemuck::bytes_of(csm));
    }

    // Record the transmittance + multiscatter LUT passes when the atmosphere changed.
    // Must run before the main sky pass on the same encoder. `upload` must precede it.
    pub fn render_luts(&mut self, encoder: &mut wgpu::CommandEncoder) {
        if !self.lut_dirty {
            return;
        }
        self.lut_dirty = false;
        encoder.push_debug_group("wgr_sky_luts");
        self.lut_pass(
            encoder,
            "wgr_sky_transmittance",
            &self.transmittance_pipeline,
            &self.transmittance_bind,
            &self.transmittance_view,
        );
        // Multiscatter samples the transmittance LUT just rendered, so it runs after.
        self.lut_pass(
            encoder,
            "wgr_sky_multiscatter",
            &self.multiscatter_pipeline,
            &self.multiscatter_bind,
            &self.multiscatter_view,
        );
        encoder.pop_debug_group();
    }

    fn lut_pass(
        &self,
        encoder: &mut wgpu::CommandEncoder,
        label: &str,
        pipeline: &wgpu::RenderPipeline,
        bind: &wgpu::BindGroup,
        view: &wgpu::TextureView,
    ) {
        let mut pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
            label: Some(label),
            color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                view,
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
        pass.set_pipeline(pipeline);
        pass.set_bind_group(0, bind, &[]);
        pass.draw(0..3, 0..1);
    }

    // Draw the fullscreen sky into an already-begun render pass targeting the scene.
    pub fn render(&self, pass: &mut wgpu::RenderPass<'_>) {
        pass.set_pipeline(&self.sky_pipeline);
        pass.set_bind_group(0, &self.sky_bind, &[]);
        pass.draw(0..3, 0..1);
    }

    // Clouds are drawn only on the HDR path (the composite blends linear radiance over the HDR scene)
    // and when coverage is non-zero. lib.rs gates the cloud pass + composite on this.
    pub fn clouds_active(&self, sky: &WgrSky) -> bool {
        self.linear > 0.5 && sky.cloud0[0] > 0.001
    }

    // Phase 1: march the clouds at LOW RES into cloud_lo, bounding each ray at the resolved scene
    // depth so they occlude terrain / envelop the camera. (Re)allocates cloud_lo at half the scene
    // size on resize and rebuilds the composite bind. `upload` + `render_luts` must have run first.
    // depth_view is the single-sample resolved prepass depth (gfx3d.depth_sample_view()).
    pub fn render_cloud(
        &mut self,
        device: &wgpu::Device,
        encoder: &mut wgpu::CommandEncoder,
        depth_view: &wgpu::TextureView,
        width: u32,
        height: u32,
    ) {
        let lo = (width.div_ceil(2).max(1), height.div_ceil(2).max(1));
        if self.cloud_lo_size != lo || self.cloud_lo_view.is_none() {
            let tex = device.create_texture(&wgpu::TextureDescriptor {
                label: Some("wgr_cloud_lo"),
                size: wgpu::Extent3d {
                    width: lo.0,
                    height: lo.1,
                    depth_or_array_layers: 1,
                },
                mip_level_count: 1,
                sample_count: 1,
                dimension: wgpu::TextureDimension::D2,
                format: crate::HDR_FORMAT,
                usage: wgpu::TextureUsages::RENDER_ATTACHMENT
                    | wgpu::TextureUsages::TEXTURE_BINDING,
                view_formats: &[],
            });
            let view = tex.create_view(&wgpu::TextureViewDescriptor::default());
            self.cloud_lo_tex = Some(tex);
            self.cloud_lo_view = Some(view);
            self.cloud_lo_size = lo;
        }
        // The composite bind carries the low-res cloud buffer + the full-res scene depth (for the
        // bilateral upsample). The depth view is regenerated every frame, so rebuild the bind here
        // (cheap) rather than only on resize.
        self.cloud_composite_bind = Some(device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("wgr_cloud_composite_bind"),
            layout: &self.cloud_composite_layout,
            entries: &[
                wgpu::BindGroupEntry {
                    binding: 0,
                    resource: wgpu::BindingResource::TextureView(
                        self.cloud_lo_view.as_ref().unwrap(),
                    ),
                },
                wgpu::BindGroupEntry {
                    binding: 1,
                    resource: wgpu::BindingResource::Sampler(&self.cloud_composite_sampler),
                },
                wgpu::BindGroupEntry {
                    binding: 2,
                    resource: wgpu::BindingResource::TextureView(depth_view),
                },
            ],
        }));
        // group(1): the resolved scene depth, rebuilt each frame (the depth view is regenerated).
        let depth_bind = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("wgr_cloud_depth_bind"),
            layout: &self.cloud_depth_layout,
            entries: &[wgpu::BindGroupEntry {
                binding: 6,
                resource: wgpu::BindingResource::TextureView(depth_view),
            }],
        });
        let lo_view = self.cloud_lo_view.as_ref().unwrap();
        encoder.push_debug_group("wgr_cloud");
        let mut pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
            label: Some("wgr_cloud"),
            color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                view: lo_view,
                depth_slice: None,
                resolve_target: None,
                ops: wgpu::Operations {
                    load: wgpu::LoadOp::Clear(wgpu::Color::TRANSPARENT),
                    store: wgpu::StoreOp::Store,
                },
            })],
            depth_stencil_attachment: None,
            timestamp_writes: None,
            occlusion_query_set: None,
            multiview_mask: None,
        });
        pass.set_pipeline(&self.cloud_pipeline);
        pass.set_bind_group(0, &self.sky_bind, &[]);
        pass.set_bind_group(1, &depth_bind, &[]);
        pass.draw(0..3, 0..1);
        drop(pass);
        encoder.pop_debug_group();
    }

    // Composite the upsampled low-res clouds over the lit scene (premultiplied blend) in an
    // already-begun render pass targeting scene_view. Must run after render_cloud on the same frame.
    pub fn composite_cloud(&self, pass: &mut wgpu::RenderPass<'_>) {
        if let Some(bind) = self.cloud_composite_bind.as_ref() {
            pass.set_pipeline(&self.cloud_composite_pipeline);
            pass.set_bind_group(0, bind, &[]);
            pass.draw(0..3, 0..1);
        }
    }

    // Bake the disc-free sky radiance into the reflection env map (equirect) for this frame. Reuses
    // this frame's sky uniform + LUTs (so `upload` + `render_luts` must have run first) via the same
    // group(0) bind as the sky pass. Cheap (256x128); recorded once per frame before the water pass.
    pub fn render_env(&self, encoder: &mut wgpu::CommandEncoder) {
        let mut pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
            label: Some("wgr_sky_env"),
            color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                view: &self.env_view,
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
        pass.set_pipeline(&self.env_pipeline);
        pass.set_bind_group(0, &self.sky_bind, &[]);
        pass.draw(0..3, 0..1);
    }

    // The reflection env map view, lent to the water bind group (water look plan Stage 4a).
    pub fn env_view(&self) -> &wgpu::TextureView {
        &self.env_view
    }

    // Project the env map into SH-9 diffuse sky irradiance for this frame. Must run after render_env
    // (reads the freshly-baked env) and before the lit-mesh / terrain passes read `sh_buf`. One tiny
    // dispatch. Recorded on the same encoder so wgpu barriers env-write -> read and sh-write -> read.
    pub fn render_sh(&self, encoder: &mut wgpu::CommandEncoder) {
        encoder.push_debug_group("wgr_sky_sh");
        let mut pass = encoder.begin_compute_pass(&wgpu::ComputePassDescriptor {
            label: Some("wgr_sky_sh"),
            timestamp_writes: None,
        });
        pass.set_pipeline(&self.sh_pipeline);
        pass.set_bind_group(0, &self.sh_bind, &[]);
        pass.dispatch_workgroups(1, 1, 1);
        drop(pass);
        encoder.pop_debug_group();
    }

    // The SH-9 sky-irradiance buffer, lent to the camera bind group (frame group binding 9) so the
    // lit-mesh + terrain shaders evaluate directional sky ambient (frame::sky_irradiance).
    pub fn sh_buffer(&self) -> &wgpu::Buffer {
        &self.sh_buf
    }

    // Fill the aerial-perspective froxel volume for this frame (see cs_froxel). Reuses
    // this frame's sky uniform + LUTs, so `upload` + `render_luts` must have run first.
    // One thread per screen column marches the atmosphere front-to-back into the slices.
    pub fn render_froxel(
        &self,
        device: &wgpu::Device,
        encoder: &mut wgpu::CommandEncoder,
        shadow_mask_view: &wgpu::TextureView,
        csm_view: &wgpu::TextureView,
    ) {
        // Group(1) is rebuilt each frame because the terrain mask + CSM textures are owned
        // elsewhere and regenerated; cheap. upload() has already refreshed mapping_buf/csm_ubo.
        let shadow_bind = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("wgr_sky_froxel_shadow_bind"),
            layout: &self.froxel_shadow_layout,
            entries: &[
                wgpu::BindGroupEntry {
                    binding: 0,
                    resource: wgpu::BindingResource::TextureView(shadow_mask_view),
                },
                wgpu::BindGroupEntry {
                    binding: 1,
                    resource: wgpu::BindingResource::Sampler(&self.mask_sampler),
                },
                wgpu::BindGroupEntry {
                    binding: 2,
                    resource: self.mapping_buf.as_entire_binding(),
                },
                wgpu::BindGroupEntry {
                    binding: 3,
                    resource: wgpu::BindingResource::TextureView(csm_view),
                },
                wgpu::BindGroupEntry {
                    binding: 4,
                    resource: wgpu::BindingResource::Sampler(&self.csm_cmp_sampler),
                },
                wgpu::BindGroupEntry {
                    binding: 5,
                    resource: self.csm_ubo.as_entire_binding(),
                },
            ],
        });
        encoder.push_debug_group("wgr_sky_froxel");
        let mut pass = encoder.begin_compute_pass(&wgpu::ComputePassDescriptor {
            label: Some("wgr_sky_froxel"),
            timestamp_writes: None,
        });
        pass.set_pipeline(&self.froxel_pipeline);
        pass.set_bind_group(0, &self.froxel_bind, &[]);
        pass.set_bind_group(1, &shadow_bind, &[]);
        pass.dispatch_workgroups(FROXEL_W.div_ceil(8), FROXEL_H.div_ceil(8), 1);
        drop(pass);
        encoder.pop_debug_group();

        // CLD-020: the cloud sun-transmittance map, in the same encoder and reusing group(0).
        // Runs after the froxel fill only for tidiness -- the two are independent, and both read
        // the sky uniform this frame already wrote.
        //
        // The dispatch is unconditional even at strength 0, and that is deliberate: the shader
        // early-outs to "fully lit" per texel, so the map is always VALID. Skipping the dispatch
        // would leave whatever the last enabled frame wrote, and toggling the feature off would
        // freeze the old shadows on the ground instead of clearing them.
        encoder.push_debug_group("wgr_cloud_shadow");
        let mut cloud_pass = encoder.begin_compute_pass(&wgpu::ComputePassDescriptor {
            label: Some("wgr_cloud_shadow"),
            timestamp_writes: None,
        });
        cloud_pass.set_pipeline(&self.cloud_shadow_pipeline);
        cloud_pass.set_bind_group(0, &self.froxel_bind, &[]);
        cloud_pass.dispatch_workgroups(
            CLOUD_SHADOW_DIM.div_ceil(8),
            CLOUD_SHADOW_DIM.div_ceil(8),
            1,
        );
        drop(cloud_pass);
        encoder.pop_debug_group();
    }

    // The froxel volume view, lent to the camera bind group so the forward shaders
    // sample it (frame::froxel_fog).
    pub fn froxel_view(&self) -> &wgpu::TextureView {
        &self.froxel_view
    }
}

#[cfg(test)]
mod cloud_evolution_tests {
    // Cloud EVOLUTION must actually reach the noise lookup, on an axis the horizontal sampling
    // does not already vary. Wind only translates the field: the same clouds slide past forever,
    // which is what the sky did before. Drifting the volume's third axis walks through
    // uncorrelated slices, so banks build and clear in place.
    //
    // Pinned in the shader source because the alternative — a uniform lane that is uploaded,
    // plumbed through three structs, exposed in ImGui, and then never read — looks identical from
    // every side except the screen.
    #[test]
    fn evolution_offsets_drive_the_noise_lookup() {
        let src = include_str!("sky.wgsl");
        assert!(
            src.contains("cloud4: vec4<f32>"),
            "the sky uniform must carry the evolution offsets"
        );
        // Shape and detail drift on Y, which the world-position lookup otherwise holds fixed.
        assert!(
            src.contains("ws.y += sky.cloud4.x * sky.cloud1.z"),
            "cloud SHAPE must be sampled at a drifting slice"
        );
        assert!(
            src.contains("wd.y += sky.cloud4.y * sky.cloud1.w"),
            "cloud DETAIL must be sampled at a drifting slice"
        );
        // And where it is cloudy at all has to move, or coverage patches stay pinned to the world
        // forever and only the wisps inside them change.
        assert!(
            src.contains("0.5 + sky.cloud4.z * sky.cloud3.x"),
            "the coverage/weather field must drift too"
        );
    }
}

#[cfg(test)]
mod cirrus_tests {
    // The second cloud layer read as tiled bands of near-identical wisps marching in rows, and the
    // cause was arithmetic: ONE tap for the sheet and ONE for the fibres, both axis-aligned, both
    // at fixed slices of a volume that tiles with period 1 on every axis. That put the sheet's
    // repeat at 28 km in world X and 7 km in world Z; the fibres repeated at 2 km in Z, and
    // 7000/2000 = 7/2 is RATIONAL, so the two agreed on a common period of 14 km. From 6.5 km
    // below, 14 km of sheet is a few degrees of sky — four or five identical copies per frame.
    //
    // Pinned in the source because every one of these measures is invisible from outside: remove
    // any of them and the shader still compiles, still validates, still renders cirrus, and the
    // rows come back. Only a screenshot would tell you, and only if someone looked.
    #[test]
    fn the_cirrus_field_cannot_repeat_on_a_short_period() {
        let src = include_str!("sky.wgsl");
        let body = src
            .split_once("fn cirrus_field(")
            .expect("sky.wgsl declares cirrus_field")
            .1
            .split_once("\nfn ")
            .expect("cirrus_field is followed by another function")
            .0;
        // 1. Two sheet octaves whose scales are in the GOLDEN RATIO. An irrational ratio has no
        //    common period at all, so their sum never recurs at any distance. This is the measure
        //    that makes the repeat impossible rather than merely distant.
        assert!(
            body.contains("1.61803399"),
            "the sheet must be two octaves at an irrational scale ratio"
        );
        // 2. Every lookup rotated, and by DIFFERENT angles, so the octaves' lattices are not even
        //    parallel to each other -- and none of them parallel to the streaks.
        assert_eq!(
            body.matches("cirrus_rot(").count(),
            3,
            "each of the three field lookups must rotate its own domain"
        );
        // 3. Slice drift: the volume's third axis advances with world position, so two points a
        //    whole tile apart in X/Z read DIFFERENT slices.
        assert!(
            body.contains("let slice = (p.x * 0.61803399 + p.y * 0.41421356)"),
            "the field must walk the noise volume's third axis with world position"
        );
        // 4. The fibre lookup must not share a period with the sheet. 3.5 = 7/2 did; 3.301 does
        //    not. A rational ratio here is what let the two fields agree on a 14 km repeat.
        assert!(
            body.contains("p.y * 3.301"),
            "the fibre lookup's z ratio must be irrational against the sheet's"
        );
        assert!(
            !body.contains("* 3.5"),
            "3.5 = 7/2 shares a period with the sheet -- that was the original bug"
        );
        // And the domain warp, which is what stops the lattice being a straight line anywhere.
        assert!(
            src.contains("fn cirrus_warp("),
            "the cirrus field must be domain-warped"
        );
    }

    // The Sky tab's two toggles ride output.w rather than a new WgrSkyLook lane (that struct's
    // size is part of the ABI handshake). If the shader stops reading the lane the checkboxes go
    // dead silently -- the layer just renders whatever it renders.
    #[test]
    fn the_layer_mode_is_read_from_the_uniform() {
        let src = include_str!("sky.wgsl");
        assert!(
            src.contains("let mode = sky.output.w"),
            "cirrus_layer must read its mode (0 off / 1 flat / 2 volumetric) from output.w"
        );
        assert!(
            src.contains("if (mode < 0.5 || sky.cloud0.x <= 0.001 || dir.y <= 0.01)"),
            "mode 0 must return before any work -- the toggle has to cost nothing when off"
        );
    }
}

#[cfg(test)]
mod sky_uniform_layout_tests {
    // SkyUniform and sky.wgsl's `struct Sky` are the same buffer seen from two languages, and
    // nothing checks that they agree. Inserting a field in one and not the other is not a compile
    // error in either — it is a silent layout shift, and it surfaces as
    //
    //   "the buffer bound at binding index 0 is bound with size 336 where the shader expects 352"
    //
    // on EVERY 3D draw at once, which points at the bind group rather than at the field that
    // moved. That is exactly what adding cloud4 for cloud evolution did.
    //
    // Compare the field ORDER, not just the size: two vec4 fields swapped keeps the size identical
    // and silently reinterprets both.
    #[test]
    fn rust_and_wgsl_sky_structs_declare_the_same_fields_in_the_same_order() {
        let src = include_str!("sky.wgsl");
        let body = src
            .split_once("struct Sky {")
            .expect("sky.wgsl declares struct Sky")
            .1
            .split_once("};")
            .expect("struct Sky is terminated")
            .0;
        let wgsl: Vec<&str> = body
            .lines()
            .filter_map(|l| {
                let t = l.trim();
                if t.starts_with("//") {
                    return None;
                }
                t.split_once(':').map(|(name, _)| name.trim())
            })
            .filter(|n| !n.is_empty() && !n.contains(' '))
            .collect();

        // The Rust side, in declaration order. Kept as a literal list rather than derived,
        // because deriving it from the struct would mean the test could only ever agree with
        // itself — this list is the assertion.
        let rust = [
            "inv_view_proj",
            "sun_dir",
            "moon_dir",
            "rayleigh",
            "mie",
            "ground_albedo",
            "params",
            "control",
            "fog_color",
            "night_zenith",
            "night_horizon",
            "night_params",
            "cloud0",
            "cloud1",
            "cloud2",
            "cloud3",
            "cloud4",
            "output",
            "cam_pos",
            // CLD-020's cloud sun-transmittance mapping. It was added to BOTH structs but not to
            // this list, so the check that is supposed to catch a layout skew was itself skewed
            // and had been failing since.
            "cloud_shadow",
        ];
        assert_eq!(
            wgsl, rust,
            "sky.wgsl's Sky and Rust's SkyUniform must declare identical fields in identical order"
        );
        // 19 vec4 lanes + one mat4. If this moves, both lists above must have moved with it.
        assert_eq!(
            std::mem::size_of::<super::SkyUniform>(),
            64 + (rust.len() - 1) * 16
        );
    }
}
