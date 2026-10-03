// Volumetric sun shafts (crepuscular rays / god rays).
//
// Two passes, both fullscreen triangles:
//   1. `fs_march` (godrays.wgsl) — a world-space ray march at a fraction of the screen resolution,
//      writing in-scattered sun radiance + the source depth into a small Rgba16Float target.
//   2. `fs_composite` (godrays_composite.wgsl) — a depth-aware upsample, added to the linear HDR
//      scene BEFORE bloom / auto-exposure / tonemap.
//
// See the header comments in the two shaders for why it is a march and not a radial blur, which
// occluders it uses, and why the composite has to be depth-aware.
//
// COST SHAPE. Everything expensive is in pass 1 and scales as
// (width/div) * (height/div) * steps * 3 texture taps. At 1080p with the defaults (div 2,
// 16 steps) that is 960x540x16x3 ~= 25M taps; pass 2 is 2.07M pixels x 5 taps. The two knobs that
// matter for a weak GPU are `res_div` and `steps`, and both are on the Sky tab and in WGR_GODRAYS
// for exactly that reason.
//
// OWNERSHIP NOTE. This module reads three resources it does not own — the sky's CLD-020 cloud
// transmittance map, the terrain's sun-shadow ceiling mask, and gfx3d's cascade shadow array — all
// through their existing public accessors. It adds no geometry pass and no new shadow render.

use bytemuck::Zeroable;
use wgpu::util::DeviceExt;

use crate::ffi::{WgrCameraShadow, WgrSky};
use crate::terrain::TerrainShadowMap;

/// Live look/quality knobs. Seeded from `WGR_GODRAYS`, then owned by the dev panel's Sky tab
/// (`wgr_set_god_rays`).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct GodRaySettings {
    pub enabled: bool,
    /// Multiplier on the physical single-scatter estimate. 1 = as computed.
    pub intensity: f32,
    /// ARTISTIC MULTIPLIER on the physical aerosol density, carried over the wire in the units the
    /// dev panel and `WGR_GODRAYS` already speak (1/m), where `DENSITY_REF` = 1.0x. The physical
    /// value comes from the atmosphere itself — see `aerosol_sigma`. This is deliberately not the
    /// density: shafts are scattering off aerosol and droplets, so a constant here meant the same
    /// shafts on a crystalline dry afternoon as in morning fog, which is the thing being fixed.
    pub density: f32,
    /// March cap (m). Two independent reasons the default is short rather than "as far as you can
    /// see". First, the CLD-020 cloud map spans 4 km CENTRED on the camera, so a ray leaves it
    /// somewhere between 2.0 and 2.9 km and reads fully lit past that — marching much further buys
    /// path with no cloud shape on it, which is the one thing this effect exists for. Second, the
    /// aerial-perspective froxel already produces sun-shafted haze (terrain- and cascade-occluded)
    /// out to the draw distance; overlapping it further out just double-counts the same
    /// scattering. What this pass adds over the froxel is cloud occlusion and resolution, and both
    /// are near/mid-field concerns.
    pub distance: f32,
    /// Henyey-Greenstein anisotropy. Higher = tighter, brighter beam toward the sun and a faster
    /// fade away from it.
    pub g: f32,
    /// How strongly the cloud deck shapes the shafts. 0 = geometry only (useful as an A/B).
    pub cloud_influence: f32,
    pub steps: u32,
    /// Screen-resolution divisor for the march target. 2 = half in each axis (quarter the pixels).
    pub res_div: u32,
}

impl Default for GodRaySettings {
    // Kept in sync with Engine::SkySettings on the C++ side (which pushes on every Sky-tab edit and
    // therefore wins the moment the panel is touched).
    fn default() -> Self {
        Self {
            enabled: true,
            intensity: 4.0,
            // ~1.08x DENSITY_REF — the Sky tab's "Air density (x1e-6)" reads 13.
            density: 1.3e-5,
            distance: 3000.0,
            g: 0.60,
            cloud_influence: 1.0,
            steps: 16,
            res_div: 2,
        }
    }
}

// ---------------------------------------------------------------------------------------------
// THE SCATTERING MEDIUM
//
// Crepuscular rays are single scattering off AEROSOL AND DROPLETS — haze, humidity, dust, mist.
// The shafts are not the light, they are the scatterers being lit by it. Molecular (Rayleigh)
// scattering off clean air is both far too weak and far too isotropic to draw a shaft against the
// sky, which is why genuinely clear dry air shows almost nothing and why the same scene after rain
// or under a low deck shows a great deal. So the density cannot be a constant; it has to follow the
// atmospheric state.
//
// EVERY input below is already in the WgrSky uniform this pass is handed. Nothing new is plumbed:
//   mie.x          — the authored aerosol scattering coefficient (1/m). The SAME number the sky's
//                    own Mie term uses, so the shafts are that scattering, resolved and occluded.
//   mie.z          — the aerosol scale height (m), default 1200. Short, as aerosol is: this is what
//                    thins the shafts out as the camera climbs.
//   cloud0.x       — cloud coverage, driven from Landscape::GetOvercast() by default. The humidity
//                    proxy: hygroscopic aerosol swells as relative humidity rises.
//   night_params.w — Scene::GetFogMaxRange(), the weather visibility in metres. The droplet proxy.
//   night_zenith.w — camera altitude ASL. (The march uses per-sample world Y instead, so this is
//                    only here for completeness — flying is handled by the profile, not a fudge.)
//
// mie.w (turbidity) is deliberately NOT used: it is a dead lane, read by nothing in sky.wgsl, and
// constant at 2.44 across every time-of-day preset. Multiplying by it would invent an effect the
// rest of the atmosphere does not have.
// ---------------------------------------------------------------------------------------------

/// The density this pass shipped with, and the unit the dev panel slider and `WGR_GODRAYS` still
/// speak in. `settings.density / DENSITY_REF` is the artistic multiplier, so 1.2e-5 is exactly
/// 1.0x — the "average conditions" anchor the calibration below is pinned to. It is NOT the
/// shipped default: 1.3e-5, i.e. ~1.08x (Sky tab "Air density (x1e-6)" = 13).
const DENSITY_REF: f32 = 1.2e-5;

/// What fraction of the authored clear-day Mie coefficient survives in genuinely pristine, dry air
/// (post-frontal arctic, desert winter). 0.25 of the 2.09e-5 the 17:00 preset authors is 5.2e-6;
/// with the ~1.2e-5 molecular term that is a Koschmieder visual range of ~230 km, which is about as
/// clean as the real atmosphere ever gets. Shafts there are barely perceptible — but not zero,
/// because there is always some aerosol.
const AEROSOL_DRY_FRACTION: f32 = 0.25;

/// Total multiplicative span from pristine-dry to saturated/foggy, applied as DRY * GROWTH^load.
///
/// Exponential rather than linear because that is the shape hygroscopic growth actually has: an
/// aerosol's scattering cross-section is near-flat up to ~70% RH and then climbs steeply as it
/// deliquesces, so most of the range lives at the wet end.
///
/// The number is pinned by calibration, not taste: at the engine's default weather (overcast 0.42,
/// no fog) the model must reproduce the 1.2e-5 that was captured and approved, so that the change
/// is a change in how the density RESPONDS, not a change in the look of an average day.
const AEROSOL_WET_GROWTH: f32 = 17.0;

/// Visibility (m) at or above which `Scene::GetFogMaxRange()` tells us nothing about fog.
///
/// That value is `min(MAX_FOG, tacRange)`, and on a clear day tacRange is simply the player's view
/// distance — so a large value is ambiguous and must be read as "clear". But Landscape's weather
/// update caps the fogged branch at `min(900, noFogVisibility) * (1 - fog*0.95)`, so a value BELOW
/// 900 m can only come from fog, heavy rain or night. All three mean more scatterers, or no sun to
/// scatter; none of them is a false positive worth worrying about.
const FOG_VISIBILITY_KNEE: f32 = 900.0;

/// The 0.95 in Landscape's `fogVisibility = 1 - fog * 0.95`. Inverting it recovers the world's fog
/// setting exactly whenever the fogged branch is the one that ran.
const FOG_VISIBILITY_DEPTH: f32 = 0.95;

/// Recover the world's fog setting (0..1) from the weather visibility the sky uniform carries.
///
/// Exact when the view distance is at or below 900 m, or whenever the fog is thick enough to take
/// Landscape's `defaultFogDistance` branch. Under-reads (fails toward CLEAR, never toward fog) for
/// light fog at a long view distance, which is the right direction to be wrong in: the alternative
/// is inventing haze on a clear day.
///
/// Known failure: a player who sets a view distance BELOW 900 m reads as permanently half-fogged.
/// The clean fix is to push `Landscape::GetFog()` directly; see the report's optional diff.
fn fog_fraction(sky: &WgrSky) -> f32 {
    let visibility = sky.night_params[3];
    if !(visibility > 0.0) || visibility >= FOG_VISIBILITY_KNEE {
        return 0.0; // no scene, or nothing learnable — assume clear
    }
    ((1.0 - visibility / FOG_VISIBILITY_KNEE) / FOG_VISIBILITY_DEPTH).clamp(0.0, 1.0)
}

/// Combined aerosol load 0..1: how much water this air is carrying, on the two signals the engine
/// actually has. Overcast is weighted below fog because an overcast sky means humid air, while fog
/// means the droplets have already condensed — the same distinction that separates a hazy day from
/// a morning you can see beams in.
fn aerosol_load(sky: &WgrSky) -> f32 {
    let overcast = sky.cloud0[0].clamp(0.0, 1.0);
    (0.7 * overcast + fog_fraction(sky)).clamp(0.0, 1.0)
}

/// Aerosol scattering coefficient at SEA LEVEL (1/m), and the scale height it thins out with.
///
/// Calibration, all at the 17:00 preset (mie.x = 2.09e-5, the textbook clear-day value) and with
/// the artistic multiplier at 1.0. Koschmieder visual ranges include the ~1.2e-5 molecular term:
///
///   load 0.00  clear + dry            5.2e-6   V ~ 227 km   barely perceptible
///   load 0.29  engine default weather 1.2e-5   V ~ 163 km   the captured/approved look
///   load 0.70  full overcast, no fog  3.7e-5   V ~  80 km   strong
///   load 1.00  overcast + fog         8.9e-5   V ~  39 km   very strong
///
/// HONEST LIMIT: those visual ranges are clear-to-hazy, not fog. Real fog is σ ≈ 4e-3, two orders
/// higher, and feeding that in would be wrong twice over — the march saturates (a uniform white
/// wash, not shafts), and the engine's own distance fog is already rendering the bulk visibility
/// loss. This pass only adds the DIRECTIONAL Mie single-scatter on top of that. So the structure is
/// physical (the engine's own aerosol coefficient, the engine's own aerosol scale height, the right
/// response shape and direction); the absolute ceiling is capped where the effect stops reading as
/// shafts at all.
fn aerosol_sigma(sky: &WgrSky, multiplier: f32) -> (f32, f32) {
    // The atmosphere's own aerosol coefficient. Floored rather than trusted blindly: a zeroed or
    // never-pushed sky block would otherwise switch the effect off silently.
    let authored = sky.mie[0].clamp(1.0e-7, 1.0e-3);
    let load = aerosol_load(sky);
    let sigma = authored * AEROSOL_DRY_FRACTION * AEROSOL_WET_GROWTH.powf(load) * multiplier;
    // Aerosol scale height. 1200 m by default and roughly 1-1.5 km in reality, an order below the
    // molecular 8 km — which is exactly why shafts fade out when you climb.
    let scale_height = sky.mie[2].clamp(100.0, 20000.0);
    (sigma.clamp(0.0, 8.0e-5), scale_height)
}

impl GodRaySettings {
    /// The dev panel's "Air density" value re-read as a multiplier on the physical density.
    fn density_multiplier(self) -> f32 {
        self.density / DENSITY_REF
    }

    fn sanitized(mut self) -> Self {
        self.intensity = self.intensity.clamp(0.0, 8.0);
        self.density = self.density.clamp(0.0, 2.0e-4);
        self.distance = self.distance.clamp(200.0, 20000.0);
        self.g = self.g.clamp(0.0, 0.95);
        self.cloud_influence = self.cloud_influence.clamp(0.0, 1.0);
        self.steps = self.steps.clamp(4, 48);
        self.res_div = self.res_div.clamp(1, 4);
        self
    }
}

/// `WGR_GODRAYS=<enabled>[,<intensity>[,<density x1e-6>[,<steps>[,<res divisor>]]]]`
///
/// Exists for the same reason `WGR_CIRRUS` does: an A/B capture runs `--no-dev`, so the Sky tab is
/// not reachable, and "is this feature costing me anything" has to be answerable from a command
/// line. `WGR_GODRAYS=0` is the ablation; everything after the first field is optional.
///
/// Prints what it selected, so a benchmark log is self-describing.
fn settings_from_env() -> GodRaySettings {
    let mut s = GodRaySettings::default();
    let Ok(raw) = std::env::var("WGR_GODRAYS") else {
        return s;
    };
    let f: Vec<f32> = raw
        .split(',')
        .filter_map(|p| p.trim().parse::<f32>().ok())
        .collect();
    if let Some(v) = f.first() {
        s.enabled = *v != 0.0;
    }
    if let Some(v) = f.get(1) {
        s.intensity = *v;
    }
    if let Some(v) = f.get(2) {
        s.density = *v * 1.0e-6;
    }
    if let Some(v) = f.get(3) {
        s.steps = *v as u32;
    }
    if let Some(v) = f.get(4) {
        s.res_div = *v as u32;
    }
    s = s.sanitized();
    eprintln!(
        "[wgr] god rays: enabled={} intensity={} density={}x steps={} div={} from WGR_GODRAYS \
         (density is a MULTIPLIER on the atmosphere's own aerosol coefficient)",
        s.enabled,
        s.intensity,
        s.density_multiplier(),
        s.steps,
        s.res_div
    );
    s
}

/// Mirrors `GodRays` in both shaders. Any field reorder here is a silent layout shift, not a
/// compile error, so the two declarations are written in the same order and commented the same.
#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
struct GodRayUniform {
    inv_view_proj: [[f32; 4]; 4],
    sun: [f32; 4],
    sun_color: [f32; 4],
    cam_pos: [f32; 4],
    tune0: [f32; 4],
    tune1: [f32; 4],
    cloud_map: [f32; 4],
    lo_res: [f32; 4],
    /// x = SMOKE influence on the shafts. Constant 1 for now: how much smoke is IN the
    /// map is already controlled per plume (SmokeParams::groundShadow) and globally
    /// (SmokeSystem::GroundShadowStrength), so a third multiplier here would be a third
    /// place to look when the shafts are wrong. yzw reserved.
    tune2: [f32; 4],
}

/// The per-frame camera state the march needs, captured where the sky pass already derives it (the
/// same camera the visible geometry drew with) rather than re-picking a camera later and drifting
/// out of step with the sky by a frame.
#[derive(Clone, Copy)]
pub struct GodRayFrame {
    /// Camera-relative (translation-free) inverse view-projection.
    pub inv_view_proj: [[f32; 4]; 4],
    /// Absolute world camera position.
    pub cam_pos: [f32; 4],
    /// Cascade matrices of that camera, for the near-field object occlusion.
    pub csm: WgrCameraShadow,
    /// Terrain sun-shadow mask mapping, for the long-range terrain occlusion.
    pub terrain: TerrainShadowMap,
}

/// Elevation fade [0,1]: 1 with the sun well up, 0 once it is below the horizon.
///
/// Requirement, not polish. The occlusion maps all early-out to "fully lit" below the horizon (see
/// `cs_cloud_shadow`), so without this the shafts would not merely persist after sunset — they
/// would brighten as the last occluder dropped out.
fn sun_elevation_fade(sun_y: f32) -> f32 {
    let t = ((sun_y + 0.03) / 0.13).clamp(0.0, 1.0);
    t * t * (3.0 - 2.0 * t)
}

/// Scene-referred sun radiance, reddened by the sun's own air mass.
///
/// The sky pass gets this from its transmittance LUT, which this pass has no binding for; a
/// flat-earth air mass (`scale_height / sin(elevation)`) through the same Rayleigh coefficients is
/// within a few percent of it above ~3 degrees and costs nothing on the CPU. It matters because a
/// white shaft at sunset is the single most obviously wrong thing this effect can do.
fn sun_radiance(sky: &WgrSky) -> [f32; 3] {
    let scale = sky.sun_dir[3] * sky.params[1];
    let sun_y = sky.sun_dir[1].max(0.05);
    let air_mass = sky.rayleigh[3].max(1.0) / sun_y;
    [
        scale * (-sky.rayleigh[0] * air_mass).exp(),
        scale * (-sky.rayleigh[1] * air_mass).exp(),
        scale * (-sky.rayleigh[2] * air_mass).exp(),
    ]
}

pub struct GodRays {
    march_pipeline: wgpu::RenderPipeline,
    march_layout: wgpu::BindGroupLayout,
    composite_pipeline: wgpu::RenderPipeline,
    composite_layout: wgpu::BindGroupLayout,
    params_buf: wgpu::Buffer,
    terrain_buf: wgpu::Buffer,
    csm_buf: wgpu::Buffer,
    linear_sampler: wgpu::Sampler,
    compare_sampler: wgpu::Sampler,
    // The low-res march target; (re)allocated when the screen size or the divisor changes.
    lo: Option<(wgpu::Texture, wgpu::TextureView)>,
    lo_size: (u32, u32),
    // Sinkhole W1b: how far underground the CAMERA is (0 = open air, 1 = deep in a cave), from
    // wgr_set_camera_underground. Sun shafts are an outdoor-air effect; underground they scale
    // away with this, in the screen-space march and in the per-surface scattering alike.
    pub underground: f32,
    settings: GodRaySettings,
}

const MARCH_FORMAT: wgpu::TextureFormat = wgpu::TextureFormat::Rgba16Float;

impl GodRays {
    /// `scene_format` is the HDR scene target's format; the composite blends into it. This is only
    /// constructed on the HDR path — an LDR-direct swapchain has already been tonemapped by the
    /// time anything could be added to it, so there is nothing sensible to add shafts to.
    /// `scene_samples` is the MSAA sample count of the target the COMPOSITE draws into. It is
    /// not cosmetic: the shafts are now composited inside the world segment, before the
    /// transparent draws (so a blended surface is not painted over by a march that cannot see
    /// it), and that target is the multisampled scene, not the resolved one. A pipeline built
    /// for one sample is rejected outright by the pass -- "Incompatible sample count" -- and the
    /// frame renders black.
    pub fn new(
        device: &wgpu::Device,
        scene_format: wgpu::TextureFormat,
        scene_samples: u32,
    ) -> Self {
        let march_shader = device.create_shader_module(wgpu::ShaderModuleDescriptor {
            label: Some("wgr_godrays_march_shader"),
            source: wgpu::ShaderSource::Wgsl(include_str!("godrays.wgsl").into()),
        });
        let composite_shader = device.create_shader_module(wgpu::ShaderModuleDescriptor {
            label: Some("wgr_godrays_composite_shader"),
            source: wgpu::ShaderSource::Wgsl(include_str!("godrays_composite.wgsl").into()),
        });

        let uniform_entry = |binding: u32| wgpu::BindGroupLayoutEntry {
            binding,
            visibility: wgpu::ShaderStages::FRAGMENT,
            ty: wgpu::BindingType::Buffer {
                ty: wgpu::BufferBindingType::Uniform,
                has_dynamic_offset: false,
                min_binding_size: None,
            },
            count: None,
        };
        let depth_entry = |binding: u32| wgpu::BindGroupLayoutEntry {
            binding,
            visibility: wgpu::ShaderStages::FRAGMENT,
            ty: wgpu::BindingType::Texture {
                sample_type: wgpu::TextureSampleType::Depth,
                view_dimension: wgpu::TextureViewDimension::D2,
                multisampled: false,
            },
            count: None,
        };
        let float_tex_entry = |binding: u32| wgpu::BindGroupLayoutEntry {
            binding,
            visibility: wgpu::ShaderStages::FRAGMENT,
            ty: wgpu::BindingType::Texture {
                sample_type: wgpu::TextureSampleType::Float { filterable: true },
                view_dimension: wgpu::TextureViewDimension::D2,
                multisampled: false,
            },
            count: None,
        };

        let march_layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("wgr_godrays_march_layout"),
            entries: &[
                uniform_entry(0),
                depth_entry(1),
                float_tex_entry(2),
                wgpu::BindGroupLayoutEntry {
                    binding: 3,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Filtering),
                    count: None,
                },
                float_tex_entry(4),
                wgpu::BindGroupLayoutEntry {
                    binding: 5,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Buffer {
                        ty: wgpu::BufferBindingType::Uniform,
                        has_dynamic_offset: false,
                        min_binding_size: wgpu::BufferSize::new(
                            std::mem::size_of::<TerrainShadowMap>() as u64,
                        ),
                    },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 6,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Depth,
                        view_dimension: wgpu::TextureViewDimension::D2Array,
                        multisampled: false,
                    },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 7,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Comparison),
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 8,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Buffer {
                        ty: wgpu::BufferBindingType::Uniform,
                        has_dynamic_offset: false,
                        min_binding_size: wgpu::BufferSize::new(
                            std::mem::size_of::<WgrCameraShadow>() as u64,
                        ),
                    },
                    count: None,
                },
            ],
        });

        let composite_layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("wgr_godrays_composite_layout"),
            entries: &[uniform_entry(0), depth_entry(1), float_tex_entry(2)],
        });

        let march_pl = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("wgr_godrays_march_pipeline_layout"),
            bind_group_layouts: &[Some(&march_layout)],
            immediate_size: 0,
        });
        let march_pipeline = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
            label: Some("wgr_godrays_march_pipeline"),
            layout: Some(&march_pl),
            vertex: wgpu::VertexState {
                module: &march_shader,
                entry_point: Some("vs_main"),
                buffers: &[],
                compilation_options: Default::default(),
            },
            primitive: wgpu::PrimitiveState::default(),
            depth_stencil: None,
            multisample: wgpu::MultisampleState::default(),
            fragment: Some(wgpu::FragmentState {
                module: &march_shader,
                entry_point: Some("fs_march"),
                targets: &[Some(wgpu::ColorTargetState {
                    format: MARCH_FORMAT,
                    blend: None,
                    write_mask: wgpu::ColorWrites::ALL,
                })],
                compilation_options: Default::default(),
            }),
            multiview_mask: None,
            cache: None,
        });

        let composite_pl = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("wgr_godrays_composite_pipeline_layout"),
            bind_group_layouts: &[Some(&composite_layout)],
            immediate_size: 0,
        });
        // Additive into the HDR scene. Alpha is left alone (the fragment writes 0 with an Add/One/
        // One alpha component, which is a no-op) so the scene target's coverage is untouched.
        let composite_pipeline = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
            label: Some("wgr_godrays_composite_pipeline"),
            layout: Some(&composite_pl),
            vertex: wgpu::VertexState {
                module: &composite_shader,
                entry_point: Some("vs_main"),
                buffers: &[],
                compilation_options: Default::default(),
            },
            primitive: wgpu::PrimitiveState::default(),
            depth_stencil: None,
            multisample: wgpu::MultisampleState {
                count: scene_samples,
                ..Default::default()
            },
            fragment: Some(wgpu::FragmentState {
                module: &composite_shader,
                entry_point: Some("fs_composite"),
                targets: &[Some(wgpu::ColorTargetState {
                    format: scene_format,
                    blend: Some(wgpu::BlendState {
                        color: wgpu::BlendComponent {
                            src_factor: wgpu::BlendFactor::One,
                            dst_factor: wgpu::BlendFactor::One,
                            operation: wgpu::BlendOperation::Add,
                        },
                        alpha: wgpu::BlendComponent {
                            src_factor: wgpu::BlendFactor::One,
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
        });

        let linear_sampler = device.create_sampler(&wgpu::SamplerDescriptor {
            label: Some("wgr_godrays_sampler"),
            address_mode_u: wgpu::AddressMode::ClampToEdge,
            address_mode_v: wgpu::AddressMode::ClampToEdge,
            address_mode_w: wgpu::AddressMode::ClampToEdge,
            mag_filter: wgpu::FilterMode::Linear,
            min_filter: wgpu::FilterMode::Linear,
            ..Default::default()
        });
        let compare_sampler = device.create_sampler(&wgpu::SamplerDescriptor {
            label: Some("wgr_godrays_csm_sampler"),
            compare: Some(wgpu::CompareFunction::LessEqual),
            ..Default::default()
        });

        let params_buf = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
            label: Some("wgr_godrays_params"),
            contents: bytemuck::bytes_of(&GodRayUniform::zeroed()),
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
        });
        let terrain_buf = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("wgr_godrays_terrain_map"),
            size: std::mem::size_of::<TerrainShadowMap>() as u64,
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        let csm_buf = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("wgr_godrays_csm"),
            size: std::mem::size_of::<WgrCameraShadow>() as u64,
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });

        Self {
            march_pipeline,
            march_layout,
            composite_pipeline,
            composite_layout,
            params_buf,
            terrain_buf,
            csm_buf,
            linear_sampler,
            compare_sampler,
            lo: None,
            lo_size: (0, 0),
            underground: 0.0,
            settings: settings_from_env(),
        }
    }

    pub fn set_settings(&mut self, s: GodRaySettings) {
        self.settings = s.sanitized();
    }

    /// Whether this frame is worth running at all. Checked before the depth resolve the march
    /// needs, so an off/night/overhead-sun frame costs literally nothing — not even the resolve.
    pub fn surface_params(&self, sky: &WgrSky) -> [[f32; 4]; 4] {
        if !self.active(sky) {
            return [[0.0; 4]; 4];
        }
        let s = self.settings;
        let light = sun_radiance(sky);
        let (sigma, height) = aerosol_sigma(sky, s.density_multiplier());
        [
            [sky.sun_dir[0], sky.sun_dir[1], sky.sun_dir[2], sun_elevation_fade(sky.sun_dir[1])],
            [light[0], light[1], light[2], height],
            [s.intensity * self.open_air(), sigma, s.distance, s.steps as f32],
            [s.g, s.cloud_influence, 0.0, 0.0],
        ]
    }

    /// 1 in the open air, 0 with the camera deep underground (see `underground`).
    pub fn open_air(&self) -> f32 {
        1.0 - self.underground.clamp(0.0, 1.0)
    }

    pub fn active(&self, sky: &WgrSky) -> bool {
        self.settings.enabled
            && self.settings.intensity * self.open_air() > 0.001
            && self.settings.density > 0.0
            && sky.control[0] != 0.0
            && sun_elevation_fade(sky.sun_dir[1]) > 0.001
    }

    fn ensure_target(&mut self, device: &wgpu::Device, width: u32, height: u32) {
        let div = self.settings.res_div.max(1);
        let w = (width / div).max(1);
        let h = (height / div).max(1);
        if self.lo.is_some() && self.lo_size == (w, h) {
            return;
        }
        let texture = device.create_texture(&wgpu::TextureDescriptor {
            label: Some("wgr_godrays_lo"),
            size: wgpu::Extent3d {
                width: w,
                height: h,
                depth_or_array_layers: 1,
            },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: MARCH_FORMAT,
            usage: wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::TEXTURE_BINDING,
            view_formats: &[],
        });
        let view = texture.create_view(&wgpu::TextureViewDescriptor::default());
        self.lo = Some((texture, view));
        self.lo_size = (w, h);
    }

    /// Record the march + the additive composite.
    ///
    /// `scene` is the SINGLE-SAMPLE linear HDR target the post chain reads (the MSAA resolve under
    /// MSAA, the scene target itself at 1x) — compositing after the resolve rather than into the
    /// multisampled target keeps the blend at one sample per pixel instead of `sample_count`, which
    /// on a 4x target is most of this feature's cost saved for nothing given the result is a smooth
    /// low-frequency add with no edges of its own to antialias.
    ///
    /// `depth` must be the resolved single-sample scene depth (`Gfx3d::water_depth_view`).
    #[allow(clippy::too_many_arguments)]
    pub fn render(
        &mut self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        encoder: &mut wgpu::CommandEncoder,
        scene: &wgpu::TextureView,
        depth: &wgpu::TextureView,
        cloud_shadow: &wgpu::TextureView,
        terrain_mask: &wgpu::TextureView,
        csm_map: &wgpu::TextureView,
        frame: &GodRayFrame,
        sky: &WgrSky,
        cloud_map: [f32; 4],
        width: u32,
        height: u32,
    ) {
        if width == 0 || height == 0 || !self.active(sky) {
            return;
        }
        self.ensure_target(device, width, height);
        let Some((_, lo_view)) = self.lo.as_ref() else {
            return;
        };
        let (lw, lh) = self.lo_size;
        let s = self.settings;
        let radiance = sun_radiance(sky);
        // The medium the shafts are made of, from the atmosphere's own state (see aerosol_sigma).
        // Pure CPU arithmetic on values the uniform already carries — no new plumbing, no per-frame
        // work beyond a powf.
        let (sigma_sea, aerosol_scale_height) = aerosol_sigma(sky, s.density_multiplier());
        let uniform = GodRayUniform {
            inv_view_proj: frame.inv_view_proj,
            sun: [
                sky.sun_dir[0],
                sky.sun_dir[1],
                sky.sun_dir[2],
                sun_elevation_fade(sky.sun_dir[1]),
            ],
            sun_color: [radiance[0], radiance[1], radiance[2], 0.0],
            // .w carries the aerosol scale height, paired with the position it is used against:
            // the march evaluates exp(-altitude / scale_height) per sample. Previously padding, so
            // the uniform's size and both shader declarations are unchanged.
            cam_pos: [
                frame.cam_pos[0],
                frame.cam_pos[1],
                frame.cam_pos[2],
                aerosol_scale_height,
            ],
            // tune0.y is now the SEA-LEVEL aerosol coefficient derived from the atmosphere, not a
            // constant. The shader applies the altitude profile on top of it per sample.
            tune0: [s.intensity * self.open_air(), sigma_sea, s.distance, s.steps as f32],
            // Terrain and cascade occlusion are held at full strength: unlike the aerial froxel's
            // "aerial sun shadow" slider they are not a look knob here, they are the difference
            // between a shaft and a wash. The Sky tab exposes cloud influence instead, because
            // that is the one an author actually wants to A/B.
            tune1: [s.g, s.cloud_influence, 1.0, 1.0],
            tune2: [1.0, 0.0, 0.0, 0.0],
            cloud_map,
            lo_res: [
                lw as f32,
                lh as f32,
                1.0 / lw.max(1) as f32,
                1.0 / lh.max(1) as f32,
            ],
        };
        queue.write_buffer(&self.params_buf, 0, bytemuck::bytes_of(&uniform));
        queue.write_buffer(&self.terrain_buf, 0, bytemuck::bytes_of(&frame.terrain));
        queue.write_buffer(&self.csm_buf, 0, bytemuck::bytes_of(&frame.csm));

        // Both bind groups are rebuilt per frame for the same reason the sky's froxel shadow group
        // is: the depth, cloud, mask and cascade views are owned elsewhere and reallocated on
        // resize / stream events, and tracking four generation counters to save two cheap object
        // creations is not a trade worth making.
        let march_bind = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("wgr_godrays_march_bind"),
            layout: &self.march_layout,
            entries: &[
                wgpu::BindGroupEntry {
                    binding: 0,
                    resource: self.params_buf.as_entire_binding(),
                },
                wgpu::BindGroupEntry {
                    binding: 1,
                    resource: wgpu::BindingResource::TextureView(depth),
                },
                wgpu::BindGroupEntry {
                    binding: 2,
                    resource: wgpu::BindingResource::TextureView(cloud_shadow),
                },
                wgpu::BindGroupEntry {
                    binding: 3,
                    resource: wgpu::BindingResource::Sampler(&self.linear_sampler),
                },
                wgpu::BindGroupEntry {
                    binding: 4,
                    resource: wgpu::BindingResource::TextureView(terrain_mask),
                },
                wgpu::BindGroupEntry {
                    binding: 5,
                    resource: self.terrain_buf.as_entire_binding(),
                },
                wgpu::BindGroupEntry {
                    binding: 6,
                    resource: wgpu::BindingResource::TextureView(csm_map),
                },
                wgpu::BindGroupEntry {
                    binding: 7,
                    resource: wgpu::BindingResource::Sampler(&self.compare_sampler),
                },
                wgpu::BindGroupEntry {
                    binding: 8,
                    resource: self.csm_buf.as_entire_binding(),
                },
            ],
        });

        encoder.push_debug_group("wgr_godrays");
        {
            let mut pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
                label: Some("wgr_godrays_march"),
                color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                    view: lo_view,
                    depth_slice: None,
                    resolve_target: None,
                    ops: wgpu::Operations {
                        // Cleared, not loaded: the pass writes every texel, and a Load would only
                        // add a read of contents nothing can use.
                        load: wgpu::LoadOp::Clear(wgpu::Color::TRANSPARENT),
                        store: wgpu::StoreOp::Store,
                    },
                })],
                depth_stencil_attachment: None,
                timestamp_writes: None,
                occlusion_query_set: None,
                multiview_mask: None,
            });
            pass.set_pipeline(&self.march_pipeline);
            pass.set_bind_group(0, &march_bind, &[]);
            pass.draw(0..3, 0..1);
        }

        let composite_bind = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("wgr_godrays_composite_bind"),
            layout: &self.composite_layout,
            entries: &[
                wgpu::BindGroupEntry {
                    binding: 0,
                    resource: self.params_buf.as_entire_binding(),
                },
                wgpu::BindGroupEntry {
                    binding: 1,
                    resource: wgpu::BindingResource::TextureView(depth),
                },
                wgpu::BindGroupEntry {
                    binding: 2,
                    resource: wgpu::BindingResource::TextureView(lo_view),
                },
            ],
        });
        {
            let mut pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
                label: Some("wgr_godrays_composite"),
                color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                    view: scene,
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
            pass.set_pipeline(&self.composite_pipeline);
            pass.set_bind_group(0, &composite_bind, &[]);
            pass.draw(0..3, 0..1);
        }
        encoder.pop_debug_group();
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // Naga-only validation. Necessary but NOT sufficient: it never sees a bind group layout, so a
    // binding whose declared type disagrees with the layout (a filtering sampler on an unfilterable
    // texture, a D2 view where the shader says D2Array) parses and validates here and then panics
    // at pipeline creation. `pipelines_build_on_a_real_device` below is the test that catches that.
    fn validate(src: &str, what: &str) {
        let module = naga::front::wgsl::parse_str(src).unwrap_or_else(|e| panic!("{what}: {e:?}"));
        naga::valid::Validator::new(
            naga::valid::ValidationFlags::all(),
            naga::valid::Capabilities::all(),
        )
        .validate(&module)
        .unwrap_or_else(|e| panic!("{what}: {e:?}"));
    }

    #[test]
    fn godrays_wgsl_validates() {
        validate(include_str!("godrays.wgsl"), "godrays.wgsl");
    }

    #[test]
    fn godrays_composite_wgsl_validates() {
        validate(
            include_str!("godrays_composite.wgsl"),
            "godrays_composite.wgsl",
        );
    }

    // Best-effort headless device; the test skips when no adapter is available (CI without a GPU).
    fn headless() -> Option<(wgpu::Device, wgpu::Queue)> {
        let instance =
            wgpu::Instance::new(wgpu::InstanceDescriptor::new_without_display_handle_from_env());
        let adapter = pollster::block_on(instance.request_adapter(&wgpu::RequestAdapterOptions {
            power_preference: wgpu::PowerPreference::default(),
            compatible_surface: None,
            force_fallback_adapter: false,
        }))
        .ok()?;
        pollster::block_on(adapter.request_device(&wgpu::DeviceDescriptor::default())).ok()
    }

    // The real gate. `GodRays::new` creates both pipelines against their bind group layouts, so
    // this fails on exactly the class of error naga cannot see — and it is the same failure that
    // would otherwise show up as the game exiting silently at InitializeGraphicsEngine.
    #[test]
    fn pipelines_build_on_a_real_device() {
        let Some((device, _queue)) = headless() else {
            return;
        };
        let _ = GodRays::new(&device, wgpu::TextureFormat::Rgba16Float, 1);
        device.poll(wgpu::PollType::wait_indefinitely()).unwrap();
    }

    // The uniform is written with bytemuck and read by two shaders that declare it independently.
    // Its size is the one thing a layout drift would change without any compiler noticing.
    #[test]
    fn uniform_layout_is_a_mat4_plus_eight_vec4() {
        // Eight since tune2 was added for the smoke occluder's influence lane.
        assert_eq!(std::mem::size_of::<GodRayUniform>(), 64 + 8 * 16);
        assert_eq!(std::mem::align_of::<GodRayUniform>(), 4);
    }

    // Below the horizon the effect must be OFF, and it must reach off smoothly. The occlusion maps
    // all early-out to "fully lit" at night, so a fade that failed open would make the shafts
    // BRIGHTER after sunset, which is the least defensible bug this feature could ship with.
    #[test]
    fn sun_below_the_horizon_fades_the_shafts_out() {
        assert_eq!(sun_elevation_fade(-0.20), 0.0);
        assert_eq!(sun_elevation_fade(-0.03), 0.0);
        assert!(sun_elevation_fade(0.02) > 0.0);
        assert!(sun_elevation_fade(0.02) < sun_elevation_fade(0.06));
        assert_eq!(sun_elevation_fade(0.50), 1.0);
        assert_eq!(sun_elevation_fade(1.00), 1.0);
    }

    // A low sun must throw ORANGE shafts. The air-mass reddening is the only thing making that
    // happen, and "it is warmer than the high-sun case" is the claim worth pinning.
    #[test]
    fn a_low_sun_reddens_the_shaft_colour() {
        let mut sky = WgrSky::default();
        sky.sun_dir = [0.0, 1.0, 0.0, 22.0];
        let high = sun_radiance(&sky);
        sky.sun_dir = [0.9, 0.08, 0.0, 22.0];
        let low = sun_radiance(&sky);
        // Blue is scattered out hardest, so the low sun is redder both absolutely and in ratio.
        assert!(low[2] < high[2]);
        assert!(low[0] / low[2] > high[0] / high[2]);
        assert!(low[0] > 0.0);
    }

    // The 17:00 time-of-day preset (EngineWgpu.cpp kSkyPresets), which is what autoToD pushes and
    // what the reference capture was taken under. mie.x there is 20.94e-6 — the textbook clear-day
    // aerosol coefficient — and mie.z stays at the SkySettings default of 1200 m.
    fn sky_at_1700(overcast: f32, visibility_m: f32) -> WgrSky {
        let mut sky = WgrSky::default();
        sky.mie = [20.94e-6, 0.857, 1200.0, 2.44];
        sky.cloud0 = [overcast, 0.06, 1200.0, 3500.0];
        sky.night_params = [0.052, -0.139, 0.02, visibility_m];
        sky
    }

    // THE CALIBRATION. Clear dry air must be barely perceptible, the engine's default weather must
    // reproduce the density that was captured and approved, and fog/overcast must be far stronger.
    // These are the numbers quoted in the report; if one moves, the report is wrong.
    #[test]
    fn density_tracks_the_atmosphere_across_its_calibrated_range() {
        let long_view = 6000.0; // clear day, no fog signal available

        // Pristine, dry, cloudless. Barely perceptible — but never zero: there is always aerosol.
        let (dry, _) = aerosol_sigma(&sky_at_1700(0.0, long_view), 1.0);
        assert!(dry > 0.0);
        assert!((dry - 5.2e-6).abs() < 0.3e-6, "clear+dry was {dry:e}");

        // The engine's default weather (overcast 0.42, no fog) must land on the shipped 1.2e-5, so
        // that this change alters how density RESPONDS, not how an average day looks.
        let (default_weather, _) = aerosol_sigma(&sky_at_1700(0.42, long_view), 1.0);
        assert!(
            (default_weather - DENSITY_REF).abs() < 0.1e-5,
            "default weather was {default_weather:e}, expected ~{DENSITY_REF:e}"
        );

        // Full overcast, still no fog: humid air, strong shafts.
        let (overcast, _) = aerosol_sigma(&sky_at_1700(1.0, long_view), 1.0);
        assert!(
            overcast > 3.0e-5 && overcast < 4.5e-5,
            "overcast {overcast:e}"
        );

        // Overcast AND fog: the strongest the model goes.
        let (foggy, _) = aerosol_sigma(&sky_at_1700(1.0, 100.0), 1.0);
        assert!(foggy > 8.0e-5 * 0.9, "foggy {foggy:e}");

        // Monotonic and a genuinely wide span — the whole point of the change.
        assert!(dry < default_weather && default_weather < overcast && overcast <= foggy);
        assert!(foggy / dry > 10.0, "span was only {}x", foggy / dry);
    }

    // The fog inversion must fire ONLY when the engine's visibility is unambiguously fog-driven.
    // A clear day at a long view distance reading as fog would put haze in every desert scene.
    #[test]
    fn fog_is_only_inferred_when_the_visibility_is_unambiguous() {
        // Clear day: visibility is just the player's view distance, tells us nothing.
        assert_eq!(fog_fraction(&sky_at_1700(0.0, 6000.0)), 0.0);
        assert_eq!(fog_fraction(&sky_at_1700(0.0, 900.0)), 0.0);
        // No scene / never pushed must not invent weather.
        assert_eq!(fog_fraction(&sky_at_1700(0.0, 0.0)), 0.0);
        // Landscape's fogged branch: vis = 900 * (1 - fog*0.95). Invert it back exactly.
        for fog in [0.2f32, 0.5, 0.8] {
            let vis = 900.0 * (1.0 - fog * 0.95);
            let got = fog_fraction(&sky_at_1700(0.0, vis));
            assert!((got - fog).abs() < 1e-3, "fog {fog} recovered as {got}");
        }
    }

    // Aerosol lives in the lowest kilometre, so the effect must weaken as the camera climbs —
    // he flies a lot. The scale height comes from the engine's own mie.z, not a magic number.
    #[test]
    fn aerosol_scale_height_comes_from_the_atmosphere_and_thins_with_altitude() {
        let (_, h) = aerosol_sigma(&sky_at_1700(0.42, 6000.0), 1.0);
        assert_eq!(h, 1200.0);
        // The profile the shader applies: exp(-altitude / h).
        let at = |alt: f32| (-alt / h).exp();
        assert!((at(0.0) - 1.0).abs() < 1e-6);
        assert!(at(1200.0) < 0.40 && at(1200.0) > 0.35); // ~1/e at one scale height
        assert!(at(3000.0) < 0.09); // a helicopter at 3 km is mostly above the medium
        assert!(at(6000.0) < 0.01); // an airliner sees essentially none
        // Molecular scale height is ~8 km; aerosol must be far shorter or altitude does nothing.
        assert!(h < 2000.0);
    }

    // A degenerate / never-pushed sky block must not switch the effect off silently, nor let a
    // garbage value make the march opaque.
    #[test]
    fn a_degenerate_sky_block_still_produces_a_sane_medium() {
        let zeroed = WgrSky::zeroed();
        let (sigma, h) = aerosol_sigma(&zeroed, 1.0);
        assert!(sigma > 0.0 && sigma <= 8.0e-5, "sigma {sigma:e}");
        assert!((100.0..=20000.0).contains(&h), "scale height {h}");

        let mut absurd = sky_at_1700(1.0, 10.0);
        absurd.mie[0] = 1.0e9;
        let (capped, _) = aerosol_sigma(&absurd, 1000.0);
        assert_eq!(capped, 8.0e-5, "the ceiling must hold against garbage");
    }

    // The dev-panel slider is a MULTIPLIER on the physical value, carried in the old units so no
    // C++ change was needed. DENSITY_REF is the 1.0x anchor; the shipped default sits just above it.
    #[test]
    fn the_density_slider_reads_as_a_multiplier_with_the_default_near_one() {
        let shipped = GodRaySettings::default().density_multiplier();
        assert!(
            (shipped - (1.3e-5 / DENSITY_REF)).abs() < 1e-4,
            "default multiplier was {shipped}"
        );
        let sky = sky_at_1700(0.42, 6000.0);
        let (base, _) = aerosol_sigma(&sky, 1.0);
        let doubled = GodRaySettings {
            density: DENSITY_REF * 2.0,
            ..Default::default()
        };
        let (scaled, _) = aerosol_sigma(&sky, doubled.density_multiplier());
        assert!((scaled - base * 2.0).abs() < 1e-9);
        // Zeroing it must switch the medium off, not floor it at something visible.
        let off = GodRaySettings {
            density: 0.0,
            ..Default::default()
        };
        assert_eq!(aerosol_sigma(&sky, off.density_multiplier()).0, 0.0);
    }

    // The env override is the A/B path for a --no-dev capture; it has to actually parse.
    #[test]
    fn settings_clamp_to_sane_ranges() {
        let s = GodRaySettings {
            enabled: true,
            intensity: 999.0,
            density: -1.0,
            distance: 5.0,
            g: 2.0,
            cloud_influence: 4.0,
            steps: 4000,
            res_div: 0,
        }
        .sanitized();
        assert_eq!(s.intensity, 8.0);
        assert_eq!(s.density, 0.0);
        assert_eq!(s.distance, 200.0);
        assert_eq!(s.g, 0.95);
        assert_eq!(s.cloud_influence, 1.0);
        assert_eq!(s.steps, 48);
        assert_eq!(s.res_div, 1);
    }
}
