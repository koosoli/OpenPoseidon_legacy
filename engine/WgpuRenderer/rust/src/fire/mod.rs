//! FIRE-001: volumetric fire, stage 1 of porting dgreenheck/threejs-fire-pro (MIT, (c) 2026
//! Daniel Greenheck; see CREDITS.md and the shader headers for what each file ports).
//!
//! Up to `MAX_FIRES` fires near the camera each get a dense box of `NX x NY x NZ` cells
//! standing on the ground under the fire, sized to it. Each box runs Fire Pro's solver
//! (`fire_sim.wgsl`) at a fixed 60 Hz, at most one step a drawn frame (Fire Pro: it slows rather
//! than skipping), and one full-screen pass (`fire_march.wgsl`) draws them all into the lit
//! scene after the opaque world, the water and the clouds, beside the volumetric smoke.
//!
//! The engine reports its fires every frame (`wgr_set_fire_sources`); a box whose fire stops
//! being reported fades out over a second and is freed. `WGR_FIRE=0` turns the system off.

use std::time::Instant;

pub const MAX_FIRES: usize = 6;
const NX: u32 = 40;
const NY: u32 = 72;
const NZ: u32 = 40;
/// A smoke-only plume in a breeze gets a box turned to face the wind and long that way: PX cells
/// downwind, PY up, PZ across. The axis-aligned box was as wide as it was deep, so a drifting
/// plume hit its downwind wall within a few metres and the old puffs carried the rest.
const PX: u32 = 112;
const PY: u32 = 48;
const PZ: u32 = 48;
/// A wreck's column (kind 4) in a breeze: a longer, taller wind-turned box than a grenade's, so the
/// black smoke goes far before the 2001 puffs take it on (FireStackLengthRatio on the engine side).
const STACK_X: u32 = 176;
const STACK_Y: u32 = 56;
const STACK_Z: u32 = 48;
/// below this wind (m/s) a plume rises: the upright box
const PLUME_WIND: f32 = 0.8;
const JACOBI: u32 = 24;
const STEP: f32 = 1.0 / 60.0;
/// How long a smoke-only plume's box keeps the smoke in the air after its source stops (seconds;
/// WGR_FIRE_SMOKE_LINGER). The 2001 puffs a smoke grenade drops live their cloudletDuration after it
/// stops emitting; at the solver's 0.04/s dissipation 45 s leaves about a sixth, and the fade-out
/// takes the rest. Without it the cloud vanished a second after the grenade went out (owner, 2026-10-12:
/// "as long lifetime as our previous version"; 2026-10-13: "make the smoke really go far and live
/// long"). A wreck's column keeps its smoke half as long again.
fn smoke_linger() -> f32 {
    static V: std::sync::OnceLock<f32> = std::sync::OnceLock::new();
    *V.get_or_init(|| {
        std::env::var("WGR_FIRE_SMOKE_LINGER").ok().and_then(|v| v.trim().parse::<f32>().ok()).unwrap_or(45.0).clamp(0.0, 180.0)
    })
}

/// Beyond this a fire is not simulated or drawn (the legacy sprites still are).
const RANGE: f32 = 300.0;
const LUT_SIZE: usize = 512;

/// One fire, as the engine reports it (`WgrFireSource`, 864 bytes).
#[repr(C)]
#[derive(Clone, Copy, Debug, bytemuck::Pod, bytemuck::Zeroable)]
pub struct FireSource {
    /// base of the flames on the ground (world, m), w: radius of the bed (m)
    pub pos_radius: [f32; 4],
    /// x: intensity 0..1, y: smokiness 0..1, z: key (bit pattern of a u32), w: kind (0 fireplace, 1 wreck)
    pub params: [f32; 4],
    /// wind velocity (m/s), w: box width (m; 0 = the default for the bed)
    pub wind: [f32; 4],
    /// the smoke's colour (linear albedo), w: density scale
    pub color: [f32; 4],
    /// the box's 10 x 10 columns (+x then +z) are open from floor to ceiling (world y)
    pub floor: [[f32; 4]; 25],
    pub ceiling: [[f32; 4]; 25],
}

impl Default for FireSource {
    fn default() -> Self {
        bytemuck::Zeroable::zeroed()
    }
}

#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
struct SimParams {
    origin_cell: [f32; 4],
    dims: [u32; 4],
    step: [f32; 4],
    emitter: [f32; 4],
    emit: [f32; 4],
    flame: [f32; 4],
    motion: [f32; 4],
    losses: [f32; 4],
    wind: [f32; 4],
    /// x: flame (1) or smoke only (0), y: how much of the wind the gas takes up
    extra: [f32; 4],
    floor: [[f32; 4]; 25],
    ceiling: [[f32; 4]; 25],
}

#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
struct MarchBox {
    origin_cell: [f32; 4],
    dims_fade: [f32; 4],
    /// smoke colour (linear albedo), w: density (1/m per unit of smoke)
    smoke: [f32; 4],
    /// the box frame's pivot (camera-relative), w: its turn about +y (rad; local +x = world (cos, sin))
    frame: [f32; 4],
}

#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
struct MarchParams {
    boxes: [MarchBox; MAX_FIRES],
    inv_view_proj: [f32; 16],
    sizes: [f32; 4],
    sun_dir: [f32; 4],
    sun_radiance: [f32; 4],
    ambient: [f32; 4],
    smoke: [f32; 4],
    fire: [f32; 4],
}

type Tex = (wgpu::Texture, wgpu::TextureView);

fn make_tex(device: &wgpu::Device, label: &str, format: wgpu::TextureFormat, size: (u32, u32, u32)) -> Tex {
    let t = device.create_texture(&wgpu::TextureDescriptor {
        label: Some(label),
        size: wgpu::Extent3d { width: size.0, height: size.1, depth_or_array_layers: size.2 },
        mip_level_count: 1,
        sample_count: 1,
        dimension: wgpu::TextureDimension::D3,
        format,
        usage: wgpu::TextureUsages::STORAGE_BINDING | wgpu::TextureUsages::TEXTURE_BINDING,
        view_formats: &[],
    });
    let v = t.create_view(&wgpu::TextureViewDescriptor {
        dimension: Some(wgpu::TextureViewDimension::D3),
        ..Default::default()
    });
    (t, v)
}

struct Grid {
    key: u32,
    fields: [Tex; 2],
    vel: [Tex; 2],
    tmp: Tex,
    curl: Tex,
    expansion: Tex,
    div: Tex,
    pres: [Tex; 2],
    cur: usize,
    pcur: usize,
    params: wgpu::Buffer,
    /// grid min corner and cell size, in the box's frame: the world turned by `yaw` about the
    /// vertical through `pivot` (the emitter), so an upright box (yaw 0) is in world coordinates
    origin: [f32; 3],
    cell: f32,
    dims: [u32; 3],
    yaw: f32,
    pivot: [f32; 3],
    /// the width the engine probed the solids for (centred on the bed)
    width: f32,
    src: FireSource,
    fade: f32,
    /// A smoke-only plume whose source has stopped (a spent grenade): seconds it keeps simulating
    /// with no emission, so the cloud in the air thins out by the solver's own dissipation instead
    /// of vanishing with its source. Negative: the source is still reported.
    linger: f32,
    seen: bool,
    needs_clear: bool,
    time: f32,
}

/// Which textures a dispatch binds. `None` = a dummy.
struct Binds<'a> {
    fields: Option<&'a wgpu::TextureView>,
    vel: Option<&'a wgpu::TextureView>,
    aux: Option<&'a wgpu::TextureView>,
    p: Option<&'a wgpu::TextureView>,
    d: Option<&'a wgpu::TextureView>,
    out_rgba: Option<&'a wgpu::TextureView>,
    out_r: Option<&'a wgpu::TextureView>,
}

pub struct FireSystem {
    enabled: bool,
    sim_layout: wgpu::BindGroupLayout,
    clear: wgpu::ComputePipeline,
    advect_vel: wgpu::ComputePipeline,
    advect_fwd: wgpu::ComputePipeline,
    evolve: wgpu::ComputePipeline,
    curl: wgpu::ComputePipeline,
    forces: wgpu::ComputePipeline,
    divergence: wgpu::ComputePipeline,
    jacobi: wgpu::ComputePipeline,
    project: wgpu::ComputePipeline,
    sampler: wgpu::Sampler,
    dummy_rgba_in: Tex,
    dummy_r_in: Tex,
    dummy_rgba_out: Tex,
    dummy_r_out: Tex,
    march_layout: wgpu::BindGroupLayout,
    march: wgpu::RenderPipeline,
    mask: wgpu::RenderPipeline,
    march_params: wgpu::Buffer,
    /// the last march's bind group, reused by the reactive mask later in the frame
    last_bind: Option<wgpu::BindGroup>,
    _lut: wgpu::Texture,
    lut_view: wgpu::TextureView,
    grids: Vec<Grid>,
    sources: Vec<FireSource>,
    last: Option<Instant>,
    acc: f32,
    clock: f32,
    pub steps: u64,
    pub frames_drawn: u64,
    gain: f32,
    smoke_scale: f32,
}

impl FireSystem {
    pub fn new(device: &wgpu::Device, queue: &wgpu::Queue, color_format: wgpu::TextureFormat, sample_count: u32) -> Self {
        let enabled = std::env::var("WGR_FIRE").map_or(true, |v| v != "0");
        let gain = std::env::var("WGR_FIRE_GAIN").ok().and_then(|v| v.parse().ok()).unwrap_or(1.0);
        let smoke_scale = std::env::var("WGR_FIRE_SMOKE").ok().and_then(|v| v.parse().ok()).unwrap_or(1.0);
        let sim_shader = device.create_shader_module(wgpu::ShaderModuleDescriptor {
            label: Some("wgr_fire_sim"),
            source: wgpu::ShaderSource::Wgsl(include_str!("fire_sim.wgsl").into()),
        });
        let tex_entry = |binding: u32, filterable: bool| wgpu::BindGroupLayoutEntry {
            binding,
            visibility: wgpu::ShaderStages::COMPUTE,
            ty: wgpu::BindingType::Texture {
                sample_type: wgpu::TextureSampleType::Float { filterable },
                view_dimension: wgpu::TextureViewDimension::D3,
                multisampled: false,
            },
            count: None,
        };
        let storage_entry = |binding: u32, format: wgpu::TextureFormat| wgpu::BindGroupLayoutEntry {
            binding,
            visibility: wgpu::ShaderStages::COMPUTE,
            ty: wgpu::BindingType::StorageTexture {
                access: wgpu::StorageTextureAccess::WriteOnly,
                format,
                view_dimension: wgpu::TextureViewDimension::D3,
            },
            count: None,
        };
        let sim_layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("wgr_fire_sim_layout"),
            entries: &[
                wgpu::BindGroupLayoutEntry {
                    binding: 0,
                    visibility: wgpu::ShaderStages::COMPUTE,
                    ty: wgpu::BindingType::Buffer {
                        ty: wgpu::BufferBindingType::Uniform,
                        has_dynamic_offset: false,
                        min_binding_size: None,
                    },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 1,
                    visibility: wgpu::ShaderStages::COMPUTE,
                    ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Filtering),
                    count: None,
                },
                tex_entry(2, true),
                tex_entry(3, true),
                tex_entry(4, true),
                tex_entry(5, false),
                tex_entry(6, false),
                storage_entry(7, wgpu::TextureFormat::Rgba16Float),
                storage_entry(8, wgpu::TextureFormat::R32Float),
            ],
        });
        let sim_pl = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("wgr_fire_sim_pl"),
            bind_group_layouts: &[Some(&sim_layout)],
            immediate_size: 0,
        });
        let cp = |entry: &str| {
            device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
                label: Some(entry),
                layout: Some(&sim_pl),
                module: &sim_shader,
                entry_point: Some(entry),
                compilation_options: Default::default(),
                cache: None,
            })
        };
        let sampler = device.create_sampler(&wgpu::SamplerDescriptor {
            label: Some("wgr_fire_sampler"),
            address_mode_u: wgpu::AddressMode::ClampToEdge,
            address_mode_v: wgpu::AddressMode::ClampToEdge,
            address_mode_w: wgpu::AddressMode::ClampToEdge,
            mag_filter: wgpu::FilterMode::Linear,
            min_filter: wgpu::FilterMode::Linear,
            mipmap_filter: wgpu::MipmapFilterMode::Nearest,
            ..Default::default()
        });
        let one = (1, 1, 1);
        let dummy_rgba_in = make_tex(device, "wgr_fire_dummy_rgba_in", wgpu::TextureFormat::Rgba16Float, one);
        let dummy_r_in = make_tex(device, "wgr_fire_dummy_r_in", wgpu::TextureFormat::R32Float, one);
        let dummy_rgba_out = make_tex(device, "wgr_fire_dummy_rgba_out", wgpu::TextureFormat::Rgba16Float, one);
        let dummy_r_out = make_tex(device, "wgr_fire_dummy_r_out", wgpu::TextureFormat::R32Float, one);

        // --- the march
        let march_shader = device.create_shader_module(wgpu::ShaderModuleDescriptor {
            label: Some("wgr_fire_march"),
            source: wgpu::ShaderSource::Wgsl(include_str!("fire_march.wgsl").into()),
        });
        let frag_tex = |binding: u32, dim: wgpu::TextureViewDimension| wgpu::BindGroupLayoutEntry {
            binding,
            visibility: wgpu::ShaderStages::FRAGMENT,
            ty: wgpu::BindingType::Texture {
                sample_type: wgpu::TextureSampleType::Float { filterable: true },
                view_dimension: dim,
                multisampled: false,
            },
            count: None,
        };
        let march_layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("wgr_fire_march_layout"),
            entries: &[
                wgpu::BindGroupLayoutEntry {
                    binding: 0,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Buffer {
                        ty: wgpu::BufferBindingType::Uniform,
                        has_dynamic_offset: false,
                        min_binding_size: None,
                    },
                    count: None,
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
                        sample_type: wgpu::TextureSampleType::Depth,
                        view_dimension: wgpu::TextureViewDimension::D2,
                        multisampled: false,
                    },
                    count: None,
                },
                frag_tex(3, wgpu::TextureViewDimension::D2),
                frag_tex(4, wgpu::TextureViewDimension::D3),
                frag_tex(5, wgpu::TextureViewDimension::D3),
                frag_tex(6, wgpu::TextureViewDimension::D3),
                frag_tex(7, wgpu::TextureViewDimension::D3),
                frag_tex(8, wgpu::TextureViewDimension::D3),
                frag_tex(9, wgpu::TextureViewDimension::D3),
            ],
        });
        let march_pl = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("wgr_fire_march_pl"),
            bind_group_layouts: &[Some(&march_layout)],
            immediate_size: 0,
        });
        // dst = dst * transmittance + radiance (premultiplied, as the volumetric smoke)
        let blend = wgpu::BlendComponent {
            src_factor: wgpu::BlendFactor::One,
            dst_factor: wgpu::BlendFactor::SrcAlpha,
            operation: wgpu::BlendOperation::Add,
        };
        let march = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
            label: Some("wgr_fire_march"),
            layout: Some(&march_pl),
            vertex: wgpu::VertexState {
                module: &march_shader,
                entry_point: Some("vs_fullscreen"),
                compilation_options: Default::default(),
                buffers: &[],
            },
            primitive: wgpu::PrimitiveState {
                topology: wgpu::PrimitiveTopology::TriangleList,
                cull_mode: None,
                ..Default::default()
            },
            depth_stencil: None,
            multisample: wgpu::MultisampleState { count: sample_count, ..Default::default() },
            fragment: Some(wgpu::FragmentState {
                module: &march_shader,
                entry_point: Some("fs_fire"),
                compilation_options: Default::default(),
                targets: &[Some(wgpu::ColorTargetState {
                    format: color_format,
                    blend: Some(wgpu::BlendState { color: blend, alpha: blend }),
                    write_mask: wgpu::ColorWrites::ALL,
                })],
            }),
            multiview_mask: None,
            cache: None,
        });
        let mask = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
            label: Some("wgr_fire_mask"),
            layout: Some(&march_pl),
            vertex: wgpu::VertexState {
                module: &march_shader,
                entry_point: Some("vs_fullscreen"),
                compilation_options: Default::default(),
                buffers: &[],
            },
            primitive: wgpu::PrimitiveState::default(),
            depth_stencil: None,
            multisample: wgpu::MultisampleState::default(),
            fragment: Some(wgpu::FragmentState {
                module: &march_shader,
                entry_point: Some("fs_fire_mask"),
                compilation_options: Default::default(),
                targets: &[Some(crate::water_tw::spray::mask_target())],
            }),
            multiview_mask: None,
            cache: None,
        });
        let march_params = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("wgr_fire_march_params"),
            size: std::mem::size_of::<MarchParams>() as u64,
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        let lut = device.create_texture(&wgpu::TextureDescriptor {
            label: Some("wgr_fire_blackbody"),
            size: wgpu::Extent3d { width: LUT_SIZE as u32, height: 1, depth_or_array_layers: 1 },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: wgpu::TextureFormat::Rgba16Float,
            usage: wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::COPY_DST,
            view_formats: &[],
        });
        let table: Vec<u16> = blackbody_table()
            .iter()
            .map(|&v| crate::water_tw::textures::f32_to_f16(v))
            .collect();
        queue.write_texture(
            wgpu::TexelCopyTextureInfo { texture: &lut, mip_level: 0, origin: wgpu::Origin3d::ZERO, aspect: wgpu::TextureAspect::All },
            bytemuck::cast_slice(&table),
            wgpu::TexelCopyBufferLayout { offset: 0, bytes_per_row: Some(LUT_SIZE as u32 * 8), rows_per_image: None },
            wgpu::Extent3d { width: LUT_SIZE as u32, height: 1, depth_or_array_layers: 1 },
        );
        let lut_view = lut.create_view(&wgpu::TextureViewDescriptor::default());
        FireSystem {
            enabled,
            clear: cp("cs_clear"),
            advect_vel: cp("cs_advect_vel"),
            advect_fwd: cp("cs_advect_fwd"),
            evolve: cp("cs_evolve"),
            curl: cp("cs_curl"),
            forces: cp("cs_forces"),
            divergence: cp("cs_divergence"),
            jacobi: cp("cs_jacobi"),
            project: cp("cs_project"),
            sim_layout,
            sampler,
            dummy_rgba_in,
            dummy_r_in,
            dummy_rgba_out,
            dummy_r_out,
            march_layout,
            march,
            mask,
            march_params,
            last_bind: None,
            _lut: lut,
            lut_view,
            grids: Vec::new(),
            sources: Vec::new(),
            last: None,
            acc: 0.0,
            clock: 0.0,
            steps: 0,
            frames_drawn: 0,
            gain,
            smoke_scale,
        }
    }

    pub fn set_sources(&mut self, sources: &[FireSource]) {
        if !self.enabled {
            return;
        }
        self.sources.clear();
        self.sources.extend_from_slice(sources);
    }

    pub fn active(&self) -> bool {
        self.enabled && (!self.sources.is_empty() || !self.grids.is_empty())
    }

    pub fn grid_count(&self) -> usize {
        self.grids.len()
    }

    /// Match the reported fires to boxes: the nearest `MAX_FIRES` within range get one.
    fn assign(&mut self, device: &wgpu::Device, cam: [f32; 3]) {
        for g in &mut self.grids {
            g.seen = false;
        }
        let mut near: Vec<(f32, FireSource)> = self
            .sources
            .iter()
            .map(|s| {
                let d = [s.pos_radius[0] - cam[0], s.pos_radius[1] - cam[1], s.pos_radius[2] - cam[2]];
                ((d[0] * d[0] + d[1] * d[1] + d[2] * d[2]).sqrt(), *s)
            })
            .filter(|(d, s)| *d < RANGE && s.params[0] > 0.0)
            .collect();
        near.sort_by(|a, b| a.0.total_cmp(&b.0));
        near.truncate(MAX_FIRES);
        for (_, s) in near {
            let key = s.params[2].to_bits();
            if let Some(g) = self.grids.iter_mut().find(|g| g.key == key) {
                g.src = s;
                g.seen = true;
                g.linger = -1.0;
                continue;
            }
            if self.grids.len() >= MAX_FIRES {
                // replace a box that is fading out, if any
                if let Some(i) = self.grids.iter().position(|g| !g.seen && g.src.params[0] <= 0.0) {
                    self.grids.swap_remove(i);
                } else {
                    continue;
                }
            }
            self.grids.push(Grid::new(device, key, s));
        }
        for g in &mut self.grids {
            if !g.seen {
                // no longer reported: the bed stops burning and the box fades out -- after the
                // smoke it already put in the air has had its time (smoke-only plumes, see `linger`)
                g.src.params[0] = 0.0;
                if g.linger < 0.0 {
                    g.linger = if g.src.params[3] > 3.5 {
                        smoke_linger() * 1.5
                    } else if g.src.params[3] > 2.5 {
                        smoke_linger()
                    } else {
                        0.0
                    };
                }
            }
        }
    }

    fn bind(&self, device: &wgpu::Device, params: &wgpu::Buffer, b: Binds) -> wgpu::BindGroup {
        let v = |o: Option<&wgpu::TextureView>, d: &Tex| -> wgpu::TextureView { o.cloned().unwrap_or_else(|| d.1.clone()) };
        let views = [
            v(b.fields, &self.dummy_rgba_in),
            v(b.vel, &self.dummy_rgba_in),
            v(b.aux, &self.dummy_rgba_in),
            v(b.p, &self.dummy_r_in),
            v(b.d, &self.dummy_r_in),
            v(b.out_rgba, &self.dummy_rgba_out),
            v(b.out_r, &self.dummy_r_out),
        ];
        let mut entries = vec![
            wgpu::BindGroupEntry { binding: 0, resource: params.as_entire_binding() },
            wgpu::BindGroupEntry { binding: 1, resource: wgpu::BindingResource::Sampler(&self.sampler) },
        ];
        for (i, view) in views.iter().enumerate() {
            entries.push(wgpu::BindGroupEntry { binding: 2 + i as u32, resource: wgpu::BindingResource::TextureView(view) });
        }
        device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("wgr_fire_sim_bind"),
            layout: &self.sim_layout,
            entries: &entries,
        })
    }

    fn step_grid(&self, device: &wgpu::Device, queue: &wgpu::Queue, encoder: &mut wgpu::CommandEncoder, gi: usize) -> (usize, usize) {
        let g = &self.grids[gi];
        let s = &g.src;
        let intensity = s.params[0].clamp(0.0, 1.0);
        let smokiness = s.params[1].clamp(0.0, 1.0);
        let radius = s.pos_radius[3].max(0.2);
        // Fire Pro's campfire preset (editor/presets/campfire.json), the flame scaled to the bed
        // and the smoke to how much this fire smokes (a wreck's burning fuel and tyres, a
        // fireplace's wood). An explosion (kind 2) is a burst: a fireball off the ground, hotter,
        // rising faster, its gas expanding as it burns (Fire Pro's fuel-fireball look).
        let explosion = s.params[3] > 1.5 && s.params[3] < 2.5;
        let smoke_only = s.params[3] > 2.5;
        // FIRE-002: a wreck's black column (kind 4, Fire Pro's smoke stack) and a wreck's flames
        // (kind 1, Fire Pro's delayed ignition); see FireVolumeKind on the engine side
        let stack = s.params[3] > 3.5;
        let wreck = s.params[3] > 0.5 && s.params[3] < 1.5;
        // Froude scaling from the campfire the preset was tuned on (a 0.8 m bed): a fire twice
        // the size has flames twice as tall, rising and living sqrt(2) longer and faster. Big
        // beds also burn patchily (Fire Pro's burst variation), which breaks one tall tongue
        // into many flamelets that puff.
        let scale = (radius / 0.8).clamp(0.4, 5.0);
        let froude = scale.sqrt();
        // the delayed-ignition preset's source is a 0.65 m sphere
        let froude_di = (radius / 0.65).clamp(0.4, 5.0).sqrt();
        let (lift, heat, rise, life, expand, buoy) = if explosion {
            (0.55, 30.0, 3.0 + 1.5 * radius, 0.8 * froude, 1.6, 6.0)
        } else if wreck {
            // Fire Pro's delayed-ignition preset (editor/presets/delayed-ignition.json) instead of
            // the campfire: a weak source (heat 1.5) whose flame lives long (1.2 s), keeps heating
            // as it burns (4) and rises lazily (buoyancy 1.6), so a burning hull rolls up in big
            // slow tongues under dark smoke rather than flickering like a small flame
            (0.5, 1.5, 0.5 * froude_di, 1.2 * froude_di, 1.0, 1.6)
        } else {
            (0.5, 10.0, 0.92 * froude, 0.65 * froude, 0.3, 4.7)
        };
        let patchy = if smoke_only { 0.0 } else { ((radius - 0.6) / 1.2).clamp(0.0, 0.75) };
        // Fire Pro's industrial-smoke preset (editor/presets/industrial-smoke.json): no flame, a
        // little heat, a lot of smoke, a steady rise; it drifts with the wind. Scaled to the
        // smoke's 0.06/s dissipation (what stays is rate / dissipation) and cooler than a stack's:
        // a grenade's cloud rolls along a few metres up rather than climbing to the box's lid.
        // A wreck's column (stack) is the preset as a stack: hot enough to climb out of the box
        // and so dense it stands black against the sky (owner, 2026-10-13: "thick black smoke").
        let (heat, smoke_rate, rise, buoy) = if stack {
            (5.0, 7.0 * intensity.max(0.05), 2.5 * froude, 1.6)
        } else if smoke_only {
            (3.0, 4.0 * intensity.max(0.05), 1.2 * froude, 1.8)
        } else {
            (heat, 1.5 * smokiness * smokiness, rise, buoy)
        };
        // the soot the flame itself gives off as it burns (Fire Pro flame smokeRate): the
        // campfire's 1.2, the delayed ignition's 1, the fuel fireball's 5 (a burning vehicle's
        // fuel goes up black: the smokiest bursts lean that way)
        let flame_smoke = if explosion {
            0.6 + 1.2 * smokiness + 2.0 * smokiness.powi(4)
        } else if wreck {
            0.5 + smokiness
        } else {
            0.6 + 1.2 * smokiness
        };
        let p = SimParams {
            origin_cell: [g.origin[0], g.origin[1], g.origin[2], g.cell],
            dims: [g.dims[0], g.dims[1], g.dims[2], 1],
            // w: the solids' column size (the box width / 10)
            step: [STEP, g.time, (g.key % 97) as f32 * 1.7, g.width / 10.0],
            // the bed's flattened sphere stands on the ground (its half-height is 0.45 r)
            emitter: [s.pos_radius[0], s.pos_radius[1] + radius * lift, s.pos_radius[2], radius],
            emit: [intensity, heat, smoke_rate, rise],
            flame: [life, flame_smoke, if wreck { 4.0 } else { 3.0 }, expand],
            motion: [buoy, 0.12, 0.1, 3.2],
            // turbulence: stronger for a bigger fire, its eddies a metre or so whatever the size
            losses: if stack {
                // a column cools slower than a grenade's cloud (it climbs) and thins slower still:
                // at 0.02/s its smoke is still half there 35 s on, at the far end of its box
                [0.4, 0.02, 0.5 * froude, 1.6]
            } else if smoke_only {
                // stronger, larger eddies than a flame's: what makes a plume billow instead of
                // spreading as a smooth sheet; a grenade's smoke lasts (dissipation 0.06/s: at
                // 0.35 a drifting plume was down to an eighth six seconds out, and the box's far
                // end stood empty while the old puffs carried the cloud; 0.04 since 2026-10-13:
                // "make the smoke really go far and live long")
                [0.62, 0.04, 0.5 * froude, 1.6]
            } else if wreck {
                // delayed ignition: cooling 0.62, its smoke thinning at 0.35/s, turbulence 0.6
                [0.62 / froude_di, 0.35, 0.6 * froude_di, 1.1]
            } else {
                [0.62 / froude, 0.94 - 0.5 * smokiness, 0.8 * froude, 1.1]
            },
            // the wind bends the plume; Fire Pro pushes with a gentle constant force, OP relaxes the
            // gas toward the weather's wind (the one the sprite smoke drifts with). A flame lives
            // about a second, so it must take the wind up within that second to lean at all (it
            // was 0.15/s at half the wind: the flames stood straight while the smoke drifted).
            // The buoyant rise keeps its own vertical speed, so a flame leans, it does not lie down.
            // y: how patchily the bed burns (0 a steady bed)
            // the wind in the box's frame; extra.z: the frame's turn, for the world's solid columns
            wind: [
                s.wind[0] * g.yaw.cos() + s.wind[2] * g.yaw.sin(),
                patchy,
                -s.wind[0] * g.yaw.sin() + s.wind[2] * g.yaw.cos(),
                if smoke_only { 1.5 } else { 1.8 },
            ],
            extra: [if smoke_only { 0.0 } else { 1.0 }, if smoke_only { 1.0 } else { 0.7 }, g.yaw, 0.0],
            floor: s.floor,
            ceiling: s.ceiling,
        };
        queue.write_buffer(&g.params, 0, bytemuck::bytes_of(&p));
        let groups = (g.dims[0].div_ceil(4), g.dims[1].div_ceil(4), g.dims[2].div_ceil(4));
        let c = g.cur;
        let n = 1 - c;
        let mut pass = encoder.begin_compute_pass(&wgpu::ComputePassDescriptor { label: Some("wgr_fire_step"), timestamp_writes: None });
        let mut run = |pipeline: &wgpu::ComputePipeline, b: Binds| {
            let bg = self.bind(device, &g.params, b);
            pass.set_pipeline(pipeline);
            pass.set_bind_group(0, &bg, &[]);
            pass.dispatch_workgroups(groups.0, groups.1, groups.2);
        };
        let none = || Binds { fields: None, vel: None, aux: None, p: None, d: None, out_rgba: None, out_r: None };
        if g.needs_clear {
            for t in [&g.fields[0], &g.fields[1], &g.vel[0], &g.vel[1], &g.tmp, &g.curl] {
                run(&self.clear, Binds { out_rgba: Some(&t.1), ..none() });
            }
            for t in [&g.pres[0], &g.pres[1], &g.expansion, &g.div] {
                run(&self.clear, Binds { out_r: Some(&t.1), ..none() });
            }
        }
        // 1. velocity advects itself
        run(&self.advect_vel, Binds { vel: Some(&g.vel[c].1), out_rgba: Some(&g.vel[n].1), ..none() });
        // 2-3. MacCormack scalars, then the flame
        run(&self.advect_fwd, Binds { fields: Some(&g.fields[c].1), vel: Some(&g.vel[c].1), out_rgba: Some(&g.tmp.1), ..none() });
        run(&self.evolve, Binds {
            fields: Some(&g.fields[c].1),
            vel: Some(&g.vel[c].1),
            aux: Some(&g.tmp.1),
            out_rgba: Some(&g.fields[n].1),
            out_r: Some(&g.expansion.1),
            ..none()
        });
        // 4-5. curl, then the forces into the other velocity buffer
        run(&self.curl, Binds { vel: Some(&g.vel[n].1), out_rgba: Some(&g.curl.1), ..none() });
        run(&self.forces, Binds {
            fields: Some(&g.fields[n].1),
            vel: Some(&g.vel[n].1),
            aux: Some(&g.curl.1),
            out_rgba: Some(&g.vel[c].1),
            ..none()
        });
        // 6-8. divergence, pressure, projection back into vel[n]
        run(&self.divergence, Binds { vel: Some(&g.vel[c].1), d: Some(&g.expansion.1), out_r: Some(&g.div.1), ..none() });
        let mut pc = g.pcur;
        for _ in 0..JACOBI {
            run(&self.jacobi, Binds { p: Some(&g.pres[pc].1), d: Some(&g.div.1), out_r: Some(&g.pres[1 - pc].1), ..none() });
            pc = 1 - pc;
        }
        run(&self.project, Binds { vel: Some(&g.vel[c].1), p: Some(&g.pres[pc].1), out_rgba: Some(&g.vel[n].1), ..none() });
        drop(pass);
        (n, pc)
    }

    /// Simulate and draw, at the volumetric smoke's slot. `scene`: the HDR scene target;
    /// `depth`: the single-sample scene depth; `inv_view_proj`: camera-relative.
    #[allow(clippy::too_many_arguments)]
    pub fn record(
        &mut self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        encoder: &mut wgpu::CommandEncoder,
        scene: &wgpu::TextureView,
        depth: &wgpu::TextureView,
        render_size: (u32, u32),
        cam: [f32; 3],
        inv_view_proj: [f32; 16],
        sun_dir: [f32; 3],
        sun_radiance: [f32; 3],
        ambient: [f32; 3],
    ) {
        if !self.enabled {
            return;
        }
        let now = Instant::now();
        let dt = self.last.map_or(0.0, |l| (now - l).as_secs_f32()).min(0.25);
        self.last = Some(now);
        self.assign(device, cam);
        // fades: a second in and out
        for g in &mut self.grids {
            if !g.seen && g.linger > 0.0 {
                g.linger = (g.linger - dt).max(0.0);
            }
            let target = if g.src.params[0] > 0.0 || g.linger > 0.0 { 1.0 } else { 0.0 };
            // a burst is there at once; a bed fades in over half a second
            let rise = if g.src.params[3] > 1.5 { 1.0 } else { dt * 2.0 };
            g.fade += (target - g.fade).clamp(-dt, rise);
        }
        self.grids.retain(|g| g.seen || g.fade > 0.0);
        if self.grids.is_empty() {
            return;
        }
        self.acc = (self.acc + dt).min(STEP * 2.0);
        if self.acc >= STEP {
            self.acc -= STEP;
            self.clock += STEP;
            encoder.push_debug_group("wgr_fire_sim");
            for gi in 0..self.grids.len() {
                let (n, pc) = self.step_grid(device, queue, encoder, gi);
                let g = &mut self.grids[gi];
                g.cur = n;
                g.pcur = pc;
                g.needs_clear = false;
                g.time += STEP;
            }
            encoder.pop_debug_group();
            self.steps += 1;
        }

        // the march: nearest box first
        let mut order: Vec<usize> = (0..self.grids.len()).collect();
        let dist = |g: &Grid| {
            let c = [g.pivot[0] - cam[0], g.origin[1] - cam[1], g.pivot[2] - cam[2]];
            c[0] * c[0] + c[1] * c[1] + c[2] * c[2]
        };
        order.sort_by(|&a, &b| dist(&self.grids[a]).total_cmp(&dist(&self.grids[b])));
        let mut boxes = [MarchBox { origin_cell: [0.0; 4], dims_fade: [0.0; 4], smoke: [0.0; 4], frame: [0.0; 4] }; MAX_FIRES];
        for (slot, &gi) in order.iter().enumerate() {
            let g = &self.grids[gi];
            // the box frame's coordinates less the camera's world ones: the march turns a
            // camera-relative point about the pivot into the same space
            boxes[slot] = MarchBox {
                origin_cell: [g.origin[0] - cam[0], g.origin[1] - cam[1], g.origin[2] - cam[2], g.cell],
                dims_fade: [g.dims[0] as f32, g.dims[1] as f32, g.dims[2] as f32, g.fade],
                frame: [g.pivot[0] - cam[0], g.pivot[1] - cam[1], g.pivot[2] - cam[2], g.yaw],
                // the source's colour; Fire Pro's campfire density 7.11 / m for soot, its
                // industrial smoke's 6 for a plume
                smoke: [
                    g.src.color[0],
                    g.src.color[1],
                    g.src.color[2],
                    g.src.color[3].max(0.0) * if g.src.params[3] > 2.5 { 6.0 } else { 7.11 } * self.smoke_scale,
                ],
            };
        }
        let mp = MarchParams {
            boxes,
            inv_view_proj,
            sizes: [render_size.0 as f32, render_size.1 as f32, order.len() as f32, self.clock],
            sun_dir: [sun_dir[0], sun_dir[1], sun_dir[2], 0.38],
            sun_radiance: [sun_radiance[0], sun_radiance[1], sun_radiance[2], 5.0],
            ambient: [ambient[0], ambient[1], ambient[2], 1.0],
            // Fire Pro's smoke colour #25292e (linear), its campfire density 7.11 / m
            smoke: [0.0185, 0.0212, 0.0273, 7.11 * self.smoke_scale],
            fire: [3200.0, 0.9, 1.0, self.gain],
        };
        queue.write_buffer(&self.march_params, 0, bytemuck::bytes_of(&mp));
        let field = |slot: usize| -> wgpu::TextureView {
            order.get(slot).map_or_else(|| self.dummy_rgba_in.1.clone(), |&gi| {
                let g = &self.grids[gi];
                g.fields[g.cur].1.clone()
            })
        };
        let fields = [field(0), field(1), field(2), field(3), field(4), field(5)];
        let bg = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("wgr_fire_march_bind"),
            layout: &self.march_layout,
            entries: &[
                wgpu::BindGroupEntry { binding: 0, resource: self.march_params.as_entire_binding() },
                wgpu::BindGroupEntry { binding: 1, resource: wgpu::BindingResource::Sampler(&self.sampler) },
                wgpu::BindGroupEntry { binding: 2, resource: wgpu::BindingResource::TextureView(depth) },
                wgpu::BindGroupEntry { binding: 3, resource: wgpu::BindingResource::TextureView(&self.lut_view) },
                wgpu::BindGroupEntry { binding: 4, resource: wgpu::BindingResource::TextureView(&fields[0]) },
                wgpu::BindGroupEntry { binding: 5, resource: wgpu::BindingResource::TextureView(&fields[1]) },
                wgpu::BindGroupEntry { binding: 6, resource: wgpu::BindingResource::TextureView(&fields[2]) },
                wgpu::BindGroupEntry { binding: 7, resource: wgpu::BindingResource::TextureView(&fields[3]) },
                wgpu::BindGroupEntry { binding: 8, resource: wgpu::BindingResource::TextureView(&fields[4]) },
                wgpu::BindGroupEntry { binding: 9, resource: wgpu::BindingResource::TextureView(&fields[5]) },
            ],
        });
        let mut pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
            label: Some("wgr_fire_march"),
            color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                view: scene,
                depth_slice: None,
                resolve_target: None,
                ops: wgpu::Operations { load: wgpu::LoadOp::Load, store: wgpu::StoreOp::Store },
            })],
            depth_stencil_attachment: None,
            timestamp_writes: None,
            occlusion_query_set: None,
            multiview_mask: None,
        });
        pass.set_pipeline(&self.march);
        pass.set_bind_group(0, &bg, &[]);
        pass.draw(0..3, 0..1);
        drop(pass);
        self.last_bind = Some(bg);
        self.frames_drawn += 1;
    }

    /// The fire's coverage into the upscalers' reactive mask (R8, render size), after the
    /// march this frame; nothing when no fire was drawn.
    pub fn draw_mask(&mut self, encoder: &mut wgpu::CommandEncoder, mask: &wgpu::TextureView) {
        let Some(bg) = self.last_bind.take() else {
            return;
        };
        let mut pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
            label: Some("wgr_fire_mask"),
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
        pass.set_pipeline(&self.mask);
        pass.set_bind_group(0, &bg, &[]);
        pass.draw(0..3, 0..1);
    }
}

impl Grid {
    fn new(device: &wgpu::Device, key: u32, src: FireSource) -> Self {
        let radius = src.pos_radius[3].max(0.2);
        // the box: five bed radii wide (at least 2.4 m), standing two cells into the ground
        // the engine sends the width it probed the solids for
        let width = if src.wind[3] > 0.1 {
            src.wind[3]
        } else if src.params[3] > 1.5 {
            radius * 4.0
        } else {
            (radius * 5.0).max(2.4)
        };
        let (wx, wz) = (src.wind[0], src.wind[2]);
        let wlen = (wx * wx + wz * wz).sqrt();
        let pivot = [src.pos_radius[0], src.pos_radius[1], src.pos_radius[2]];
        if src.params[3] > 2.5 && wlen > PLUME_WIND {
            // a drifting plume: turned to the wind, `width` across, PX cells long, the source a
            // tenth of the way in from the upwind end (a wreck's column: the longer, taller box)
            let size = if src.params[3] > 3.5 { (STACK_X, STACK_Y, STACK_Z) } else { (PX, PY, PZ) };
            let cell = width / size.2 as f32;
            let length = size.0 as f32 * cell;
            let origin = [pivot[0] - 0.1 * length, pivot[1] - 2.0 * cell, pivot[2] - width * 0.5];
            return Grid::build(device, key, src, size, origin, cell, width, wz.atan2(wx), pivot);
        }
        let size = (NX, NY, NZ);
        let cell = width / NX as f32;
        // downwind: a plume leans with the wind, so the box gives it room on that side (the
        // engine probes the solids for the centred box; the shift is under a third of a column
        // set and the solids are sampled by world position, clamped to the edge columns)
        let lean = if src.params[3] > 2.5 { 0.3 } else { 0.32 };
        let shift = if wlen > 0.1 { (wlen * 0.9).min(width * lean) / wlen } else { 0.0 };
        let origin = [
            src.pos_radius[0] - width * 0.5 + wx * shift,
            src.pos_radius[1] - 2.0 * cell,
            src.pos_radius[2] - width * 0.5 + wz * shift,
        ];
        Grid::build(device, key, src, size, origin, cell, width, 0.0, pivot)
    }

    #[allow(clippy::too_many_arguments)]
    fn build(
        device: &wgpu::Device,
        key: u32,
        src: FireSource,
        size: (u32, u32, u32),
        origin: [f32; 3],
        cell: f32,
        width: f32,
        yaw: f32,
        pivot: [f32; 3],
    ) -> Self {
        let rgba = wgpu::TextureFormat::Rgba16Float;
        let r = wgpu::TextureFormat::R32Float;
        Grid {
            key,
            fields: [make_tex(device, "wgr_fire_fields_a", rgba, size), make_tex(device, "wgr_fire_fields_b", rgba, size)],
            vel: [make_tex(device, "wgr_fire_vel_a", rgba, size), make_tex(device, "wgr_fire_vel_b", rgba, size)],
            tmp: make_tex(device, "wgr_fire_tmp", rgba, size),
            curl: make_tex(device, "wgr_fire_curl", rgba, size),
            expansion: make_tex(device, "wgr_fire_expansion", r, size),
            div: make_tex(device, "wgr_fire_div", r, size),
            pres: [make_tex(device, "wgr_fire_p_a", r, size), make_tex(device, "wgr_fire_p_b", r, size)],
            cur: 0,
            pcur: 0,
            params: device.create_buffer(&wgpu::BufferDescriptor {
                label: Some("wgr_fire_sim_params"),
                size: std::mem::size_of::<SimParams>() as u64,
                usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
                mapped_at_creation: false,
            }),
            origin,
            cell,
            dims: [size.0, size.1, size.2],
            yaw,
            pivot,
            width,
            src,
            fade: 0.0,
            linger: -1.0,
            seen: true,
            needs_clear: true,
            time: 0.0,
        }
    }
}

/// Fire Pro's blackbody table (blackbody.ts): peak-normalised linear RGB and log2 radiance
/// relative to 1800 K, 500..10000 K, from Planck's law against the CIE 1931 observer fit of
/// Wyman, Sloan & Shirley (JCGT 2013).
pub fn blackbody_table() -> Vec<f32> {
    const LOBES: [&[[f64; 4]]; 3] = [
        &[[0.362, 442.0, 0.0624, 0.0374], [1.056, 599.8, 0.0264, 0.0323], [-0.065, 501.1, 0.049, 0.0382]],
        &[[0.821, 568.8, 0.0213, 0.0247], [0.286, 530.9, 0.0613, 0.0322]],
        &[[1.217, 437.0, 0.0845, 0.0278], [0.681, 459.0, 0.0385, 0.0725]],
    ];
    let observer: Vec<(f64, [f64; 3])> = (0..95)
        .map(|i| {
            let w = 360.0 + i as f64 * 5.0;
            let mut xyz = [0.0; 3];
            for (c, lobes) in LOBES.iter().enumerate() {
                for &[h, centre, left, right] in lobes.iter() {
                    let d = (w - centre) * if w < centre { left } else { right };
                    xyz[c] += h * (-0.5 * d * d).exp();
                }
            }
            (w, xyz)
        })
        .collect();
    let planck = |nm: f64, k: f64| {
        let (h, c, kb) = (6.62607015e-34, 299792458.0, 1.380649e-23);
        let w = nm * 1e-9;
        2.0 * h * c * c / (w.powi(5) * ((h * c) / (w * kb * k)).exp_m1())
    };
    let rgb = |k: f64| {
        let mut xyz = [0.0; 3];
        for (w, o) in &observer {
            let r = planck(*w, k) * 5e-9;
            for c in 0..3 {
                xyz[c] += o[c] * r;
            }
        }
        let [x, y, z] = xyz;
        [
            (3.2406 * x - 1.5372 * y - 0.4986 * z).max(0.0),
            (-0.9689 * x + 1.8758 * y + 0.0415 * z).max(0.0),
            (0.0557 * x - 0.204 * y + 1.057 * z).max(0.0),
        ]
    };
    let peak_of = |v: [f64; 3]| v[0].max(v[1]).max(v[2]);
    let reference = peak_of(rgb(1800.0));
    let mut out = Vec::with_capacity(LUT_SIZE * 4);
    for i in 0..LUT_SIZE {
        let k = 500.0 + (10000.0 - 500.0) * i as f64 / (LUT_SIZE - 1) as f64;
        let v = rgb(k);
        let peak = peak_of(v);
        if peak <= 0.0 {
            out.extend_from_slice(&[0.0, 0.0, 0.0, -32.0]);
        } else {
            out.extend_from_slice(&[
                (v[0] / peak) as f32,
                (v[1] / peak) as f32,
                (v[2] / peak) as f32,
                ((peak / reference).log2().max(-32.0)) as f32,
            ]);
        }
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn blackbody_is_warm_and_brightens_with_temperature() {
        let t = blackbody_table();
        let at = |k: f32| {
            let i = (((k - 500.0) / 9500.0) * (LUT_SIZE - 1) as f32).round() as usize;
            [t[i * 4], t[i * 4 + 1], t[i * 4 + 2], t[i * 4 + 3]]
        };
        let ember = at(1200.0);
        let flame = at(3200.0);
        // red-dominant at low temperature, and 1800 K is the reference brightness
        assert!(ember[0] >= ember[1] && ember[1] >= ember[2], "{ember:?}");
        assert!(at(1800.0)[3].abs() < 0.05);
        assert!(flame[3] > at(1800.0)[3] + 2.0, "{flame:?}");
    }

    #[test]
    fn shaders_validate() {
        for (name, src) in [("fire_sim", include_str!("fire_sim.wgsl")), ("fire_march", include_str!("fire_march.wgsl"))] {
            let module = naga::front::wgsl::parse_str(src).unwrap_or_else(|e| panic!("{name}: {}", e.emit_to_string(src)));
            naga::valid::Validator::new(naga::valid::ValidationFlags::all(), naga::valid::Capabilities::all())
                .validate(&module)
                .unwrap_or_else(|e| panic!("{name}: {e:?}"));
        }
    }

    #[test]
    fn source_layout_is_864_bytes() {
        assert_eq!(std::mem::size_of::<FireSource>(), 864);
    }
}
