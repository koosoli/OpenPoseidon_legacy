//! MUD-001: mud thrown off wheels and boots as a viscous particle fluid (`mud_sim.wgsl`: a
//! clean-room position-based fluid with implicit viscosity, after the approach of
//! dgreenheck/threejs-particle-fluids, MIT; see CREDITS.md), drawn as lit wet blobs
//! (`mud_draw.wgsl`) into the HDR scene after the opaque world.
//!
//! The engine reports where mud is thrown each frame (`wgr_set_mud_emitters`: a wheel turning in
//! wet soft ground, a boot in it); the particles live in a ring of `CAPACITY` slots, so a heavy
//! spray recycles the oldest blobs rather than growing. Nothing is simulated or drawn once the
//! last blob has expired. `WGR_MUD=0` turns the system off.

use std::time::Instant;

pub const CAPACITY: u32 = 16384;
const MAX_EMITTERS: usize = 64;
const HASH: u32 = 65536;
const SUBSTEPS: u32 = 4;
const ITERATIONS: u32 = 2;
/// physical radius (m): spacing 2r at rest
const RADIUS: f32 = 0.02;
const SMOOTHING: f32 = 0.09;
/// kinematic viscosity (m^2/s on the reference's scale; honey ~20): thick mud
const VISCOSITY: f32 = 8.0;
const COMPLIANCE: f32 = 1.0e-8;
const LIFE: f32 = 7.0;
const FADE: f32 = 1.5;

/// One spray this frame, as the engine reports it (`WgrMudEmitter`, 48 bytes).
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, bytemuck::Pod, bytemuck::Zeroable)]
pub struct MudEmitter {
    /// where (world, m), w: how many blobs this frame
    pub pos_count: [f32; 4],
    /// launch velocity (m/s), w: its spread (m/s)
    pub vel_spread: [f32; 4],
    /// x: drawn blob radius (m), y: positional spread (m)
    pub size: [f32; 4],
}

#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
struct GpuEmitter {
    pos_start: [f32; 4],
    vel_spread: [f32; 4],
    size_count: [f32; 4],
}

#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
struct SimParams {
    dt: f32,
    h: f32,
    inv_rho0: f32,
    compliance: f32,
    visc_rate: f32,
    radius: f32,
    life: f32,
    count: u32,
    gravity: f32,
    time: f32,
    emit_total: u32,
    emitters: u32,
    hm_origin: [f32; 2],
    hm_grid: f32,
    hm_on: f32,
    hm_size: [u32; 2],
    volume: f32,
    _pad: f32,
}

#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
struct DrawParams {
    view_proj: [f32; 16],
    cam: [f32; 4],
    right: [f32; 4],
    up: [f32; 4],
    sun_dir: [f32; 4],
    sun_radiance: [f32; 4],
    ambient: [f32; 4],
    albedo: [f32; 4],
}

/// The rest density of a cubic lattice at spacing 2r under the poly6 kernel (unit mass).
fn rest_density() -> f32 {
    let h = SMOOTHING;
    let d = 2.0 * RADIUS;
    let n = (h / d).ceil() as i32 + 1;
    let mut rho = 0.0f32;
    for z in -n..=n {
        for y in -n..=n {
            for x in -n..=n {
                let r2 = ((x * x + y * y + z * z) as f32) * d * d;
                if r2 < h * h {
                    let q = h * h - r2;
                    rho += 315.0 / (64.0 * std::f32::consts::PI * h.powi(9)) * q * q * q;
                }
            }
        }
    }
    rho
}

pub struct MudSystem {
    enabled: bool,
    sim_layout: wgpu::BindGroupLayout,
    draw_layout: wgpu::BindGroupLayout,
    pipelines: Vec<(&'static str, wgpu::ComputePipeline)>,
    draw: wgpu::RenderPipeline,
    pos: wgpu::Buffer,
    vel: wgpu::Buffer,
    pred: wgpu::Buffer,
    lam: wgpu::Buffer,
    heads: wgpu::Buffer,
    next: wgpu::Buffer,
    vel2: wgpu::Buffer,
    emitters: wgpu::Buffer,
    params: wgpu::Buffer,
    draw_params: wgpu::Buffer,
    dummy_height: wgpu::TextureView,
    depth: Option<(wgpu::Texture, wgpu::TextureView, (u32, u32))>,
    sample_count: u32,
    pending: Vec<MudEmitter>,
    ring: u32,
    rho0: f32,
    last: Option<Instant>,
    clock: f32,
    /// seconds since the last blob was emitted (nothing to do once past LIFE)
    idle: f32,
    initialised: bool,
    emitted: u64,
}

fn storage(device: &wgpu::Device, label: &str, size: u64) -> wgpu::Buffer {
    device.create_buffer(&wgpu::BufferDescriptor {
        label: Some(label),
        size,
        usage: wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_DST,
        mapped_at_creation: false,
    })
}

impl MudSystem {
    pub fn new(device: &wgpu::Device, color_format: wgpu::TextureFormat, sample_count: u32) -> Self {
        let enabled = std::env::var("WGR_MUD").map(|v| v != "0").unwrap_or(true);
        let n = CAPACITY as u64;
        let pos = storage(device, "wgr_mud_pos", n * 16);
        let vel = storage(device, "wgr_mud_vel", n * 16);
        let pred = storage(device, "wgr_mud_pred", n * 16);
        let lam = storage(device, "wgr_mud_lambda", n * 16);
        let heads = storage(device, "wgr_mud_heads", HASH as u64 * 4);
        let next = storage(device, "wgr_mud_next", n * 4);
        let vel2 = storage(device, "wgr_mud_vel2", n * 16);
        let emitters = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("wgr_mud_emitters"),
            size: (MAX_EMITTERS * std::mem::size_of::<GpuEmitter>()) as u64,
            usage: wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        let params = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("wgr_mud_params"),
            size: std::mem::size_of::<SimParams>() as u64,
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        let draw_params = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("wgr_mud_draw_params"),
            size: std::mem::size_of::<DrawParams>() as u64,
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        let dummy = device.create_texture(&wgpu::TextureDescriptor {
            label: Some("wgr_mud_no_heightmap"),
            size: wgpu::Extent3d { width: 1, height: 1, depth_or_array_layers: 1 },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: wgpu::TextureFormat::R32Float,
            usage: wgpu::TextureUsages::TEXTURE_BINDING,
            view_formats: &[],
        });
        let dummy_height = dummy.create_view(&wgpu::TextureViewDescriptor::default());

        let rw = |binding| wgpu::BindGroupLayoutEntry {
            binding,
            visibility: wgpu::ShaderStages::COMPUTE,
            ty: wgpu::BindingType::Buffer {
                ty: wgpu::BufferBindingType::Storage { read_only: false },
                has_dynamic_offset: false,
                min_binding_size: None,
            },
            count: None,
        };
        let sim_layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("wgr_mud_sim_layout"),
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
                rw(1),
                rw(2),
                rw(3),
                rw(4),
                rw(5),
                rw(6),
                rw(7),
                wgpu::BindGroupLayoutEntry {
                    binding: 8,
                    visibility: wgpu::ShaderStages::COMPUTE,
                    ty: wgpu::BindingType::Buffer {
                        ty: wgpu::BufferBindingType::Storage { read_only: true },
                        has_dynamic_offset: false,
                        min_binding_size: None,
                    },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 9,
                    visibility: wgpu::ShaderStages::COMPUTE,
                    ty: wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Float { filterable: false },
                        view_dimension: wgpu::TextureViewDimension::D2,
                        multisampled: false,
                    },
                    count: None,
                },
            ],
        });
        let sim_shader = device.create_shader_module(wgpu::ShaderModuleDescriptor {
            label: Some("wgr_mud_sim"),
            source: wgpu::ShaderSource::Wgsl(include_str!("mud_sim.wgsl").into()),
        });
        let sim_pl = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("wgr_mud_sim_pl"),
            bind_group_layouts: &[Some(&sim_layout)],
            immediate_size: 0,
        });
        let pipelines = [
            "cs_emit",
            "cs_predict",
            "cs_clear",
            "cs_insert",
            "cs_lambda",
            "cs_delta",
            "cs_apply",
            "cs_velocity",
            "cs_visc_init",
            "cs_visc_a",
            "cs_visc_b",
            "cs_visc_commit",
        ]
        .into_iter()
        .map(|entry| {
            (
                entry,
                device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
                    label: Some(entry),
                    layout: Some(&sim_pl),
                    module: &sim_shader,
                    entry_point: Some(entry),
                    compilation_options: Default::default(),
                    cache: None,
                }),
            )
        })
        .collect();

        let ro_vs = |binding| wgpu::BindGroupLayoutEntry {
            binding,
            visibility: wgpu::ShaderStages::VERTEX,
            ty: wgpu::BindingType::Buffer {
                ty: wgpu::BufferBindingType::Storage { read_only: true },
                has_dynamic_offset: false,
                min_binding_size: None,
            },
            count: None,
        };
        let draw_layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("wgr_mud_draw_layout"),
            entries: &[
                wgpu::BindGroupLayoutEntry {
                    binding: 0,
                    visibility: wgpu::ShaderStages::VERTEX | wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Buffer {
                        ty: wgpu::BufferBindingType::Uniform,
                        has_dynamic_offset: false,
                        min_binding_size: None,
                    },
                    count: None,
                },
                ro_vs(1),
                ro_vs(2),
                wgpu::BindGroupLayoutEntry {
                    binding: 3,
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
        let draw_shader = device.create_shader_module(wgpu::ShaderModuleDescriptor {
            label: Some("wgr_mud_draw"),
            source: wgpu::ShaderSource::Wgsl(include_str!("mud_draw.wgsl").into()),
        });
        let draw_pl = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("wgr_mud_draw_pl"),
            bind_group_layouts: &[Some(&draw_layout)],
            immediate_size: 0,
        });
        let draw = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
            label: Some("wgr_mud_draw"),
            layout: Some(&draw_pl),
            vertex: wgpu::VertexState {
                module: &draw_shader,
                entry_point: Some("vs_mud"),
                buffers: &[],
                compilation_options: Default::default(),
            },
            fragment: Some(wgpu::FragmentState {
                module: &draw_shader,
                entry_point: Some("fs_mud"),
                targets: &[Some(wgpu::ColorTargetState {
                    format: color_format,
                    blend: None,
                    write_mask: wgpu::ColorWrites::ALL,
                })],
                compilation_options: Default::default(),
            }),
            primitive: wgpu::PrimitiveState::default(),
            depth_stencil: Some(wgpu::DepthStencilState {
                format: wgpu::TextureFormat::Depth32Float,
                depth_write_enabled: Some(true),
                depth_compare: Some(wgpu::CompareFunction::Greater),
                stencil: wgpu::StencilState::default(),
                bias: wgpu::DepthBiasState::default(),
            }),
            multisample: wgpu::MultisampleState { count: sample_count, ..Default::default() },
            multiview_mask: None,
            cache: None,
        });
        Self {
            enabled,
            sim_layout,
            draw_layout,
            pipelines,
            draw,
            pos,
            vel,
            pred,
            lam,
            heads,
            next,
            vel2,
            emitters,
            params,
            draw_params,
            dummy_height,
            depth: None,
            sample_count,
            pending: Vec::new(),
            ring: 0,
            rho0: rest_density(),
            last: None,
            clock: 0.0,
            idle: f32::MAX,
            initialised: false,
            emitted: 0,
        }
    }

    pub fn set_emitters(&mut self, emitters: &[MudEmitter]) {
        if !self.enabled {
            return;
        }
        // accumulate until the next record (the engine may report more than once a frame)
        self.pending.extend(emitters.iter().copied().filter(|e| e.pos_count[3] >= 1.0 && e.pos_count.iter().all(|v| v.is_finite())));
        self.pending.truncate(MAX_EMITTERS);
    }

    pub fn active(&self) -> bool {
        self.enabled && (!self.pending.is_empty() || self.idle < LIFE + 0.5)
    }

    pub fn emitted(&self) -> u64 {
        self.emitted
    }

    fn pipeline(&self, name: &str) -> &wgpu::ComputePipeline {
        &self.pipelines.iter().find(|(n, _)| *n == name).expect("mud pipeline").1
    }

    /// Simulate and draw. `scene`: the HDR scene target; `depth`: the single-sample resolved scene
    /// depth; `inv_view_proj`: camera-relative; `terrain`: the ground the blobs land on.
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
        terrain: &crate::terrain::Terrain,
    ) {
        if !self.active() {
            return;
        }
        let now = Instant::now();
        let dt = self.last.map_or(1.0 / 60.0, |l| (now - l).as_secs_f32()).clamp(1.0 / 240.0, 1.0 / 30.0);
        self.last = Some(now);
        self.clock += dt;
        if !self.initialised {
            // every slot free (age -1)
            let free = vec![[0.0f32, 0.0, 0.0, -1.0]; CAPACITY as usize];
            queue.write_buffer(&self.pos, 0, bytemuck::cast_slice(&free));
            self.initialised = true;
        }
        // this frame's emission: each emitter's particles take consecutive ring slots
        let mut gpu = Vec::with_capacity(self.pending.len());
        let mut total = 0u32;
        for e in self.pending.drain(..) {
            let count = (e.pos_count[3] as u32).min(2048);
            gpu.push(GpuEmitter {
                pos_start: [e.pos_count[0], e.pos_count[1], e.pos_count[2], total as f32],
                vel_spread: e.vel_spread,
                size_count: [e.size[0].clamp(0.005, 0.2), e.size[1].clamp(0.0, 2.0), count as f32, self.ring as f32],
            });
            self.ring = (self.ring + count) % CAPACITY;
            total += count;
        }
        if total > 0 {
            self.idle = 0.0;
            self.emitted += total as u64;
            queue.write_buffer(&self.emitters, 0, bytemuck::cast_slice(&gpu));
        } else {
            self.idle += dt;
        }
        let tp = terrain.params();
        let have_ground = terrain.has_heightmap();
        let h = SMOOTHING;
        let sub = dt / SUBSTEPS as f32;
        let p = SimParams {
            dt: sub,
            h,
            inv_rho0: 1.0 / self.rho0,
            compliance: COMPLIANCE,
            visc_rate: VISCOSITY * sub * 10.0 / (h * h),
            radius: RADIUS,
            life: LIFE,
            count: CAPACITY,
            gravity: 9.81,
            time: self.clock,
            emit_total: total,
            emitters: gpu.len() as u32,
            hm_origin: [tp.world_origin.x, tp.world_origin.y],
            hm_grid: tp.terrain_grid,
            hm_on: if have_ground { 1.0 } else { 0.0 },
            hm_size: [tp.hm_width.max(1), tp.hm_height.max(1)],
            volume: 1.0 / self.rho0,
            _pad: 0.0,
        };
        queue.write_buffer(&self.params, 0, bytemuck::bytes_of(&p));
        let height_view = if have_ground { terrain.heightmap_view() } else { self.dummy_height.clone() };
        let sim_bind = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("wgr_mud_sim_bind"),
            layout: &self.sim_layout,
            entries: &[
                wgpu::BindGroupEntry { binding: 0, resource: self.params.as_entire_binding() },
                wgpu::BindGroupEntry { binding: 1, resource: self.pos.as_entire_binding() },
                wgpu::BindGroupEntry { binding: 2, resource: self.vel.as_entire_binding() },
                wgpu::BindGroupEntry { binding: 3, resource: self.pred.as_entire_binding() },
                wgpu::BindGroupEntry { binding: 4, resource: self.lam.as_entire_binding() },
                wgpu::BindGroupEntry { binding: 5, resource: self.heads.as_entire_binding() },
                wgpu::BindGroupEntry { binding: 6, resource: self.next.as_entire_binding() },
                wgpu::BindGroupEntry { binding: 7, resource: self.vel2.as_entire_binding() },
                wgpu::BindGroupEntry { binding: 8, resource: self.emitters.as_entire_binding() },
                wgpu::BindGroupEntry { binding: 9, resource: wgpu::BindingResource::TextureView(&height_view) },
            ],
        });
        {
            let mut pass = encoder.begin_compute_pass(&wgpu::ComputePassDescriptor {
                label: Some("wgr_mud_sim"),
                timestamp_writes: None,
            });
            pass.set_bind_group(0, &sim_bind, &[]);
            let groups = CAPACITY.div_ceil(64);
            if total > 0 {
                pass.set_pipeline(self.pipeline("cs_emit"));
                pass.dispatch_workgroups(total.div_ceil(64), 1, 1);
            }
            for _ in 0..SUBSTEPS {
                let mut run = |name: &str, n: u32| {
                    pass.set_pipeline(self.pipeline(name));
                    pass.dispatch_workgroups(n, 1, 1);
                };
                run("cs_predict", groups);
                run("cs_clear", HASH.div_ceil(64));
                run("cs_insert", groups);
                for _ in 0..ITERATIONS {
                    run("cs_lambda", groups);
                    run("cs_delta", groups);
                    run("cs_apply", groups);
                }
                run("cs_velocity", groups);
                run("cs_visc_init", groups);
                for _ in 0..2 {
                    run("cs_visc_a", groups);
                    run("cs_visc_b", groups);
                }
                run("cs_visc_commit", groups);
            }
        }

        // the draw
        let size = (render_size.0.max(1), render_size.1.max(1));
        if self.depth.as_ref().map(|d| d.2) != Some(size) {
            let t = device.create_texture(&wgpu::TextureDescriptor {
                label: Some("wgr_mud_depth"),
                size: wgpu::Extent3d { width: size.0, height: size.1, depth_or_array_layers: 1 },
                mip_level_count: 1,
                sample_count: self.sample_count,
                dimension: wgpu::TextureDimension::D2,
                format: wgpu::TextureFormat::Depth32Float,
                usage: wgpu::TextureUsages::RENDER_ATTACHMENT,
                view_formats: &[],
            });
            let v = t.create_view(&wgpu::TextureViewDescriptor::default());
            self.depth = Some((t, v, size));
        }
        let ivp = glam::Mat4::from_cols_array(&inv_view_proj);
        let vp = ivp.inverse();
        let unproject = |x: f32, y: f32| {
            let p = ivp * glam::Vec4::new(x, y, 0.5, 1.0);
            p.truncate() / p.w
        };
        let o = unproject(0.0, 0.0);
        let right = (unproject(1.0, 0.0) - o).normalize_or_zero();
        let up = (unproject(0.0, 1.0) - o).normalize_or_zero();
        let s = glam::Vec3::from(sun_dir).normalize_or_zero();
        let dp = DrawParams {
            view_proj: vp.to_cols_array(),
            cam: [cam[0], cam[1], cam[2], size.0 as f32],
            right: [right.x, right.y, right.z, size.1 as f32],
            up: [up.x, up.y, up.z, FADE],
            sun_dir: [s.x, s.y, s.z, LIFE],
            sun_radiance: [sun_radiance[0], sun_radiance[1], sun_radiance[2], 0.0],
            ambient: [ambient[0], ambient[1], ambient[2], 0.0],
            // wet loam: dark brown body (linear), a glossy film
            albedo: [0.075, 0.052, 0.034, 0.55],
        };
        queue.write_buffer(&self.draw_params, 0, bytemuck::bytes_of(&dp));
        let draw_bind = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("wgr_mud_draw_bind"),
            layout: &self.draw_layout,
            entries: &[
                wgpu::BindGroupEntry { binding: 0, resource: self.draw_params.as_entire_binding() },
                wgpu::BindGroupEntry { binding: 1, resource: self.pos.as_entire_binding() },
                wgpu::BindGroupEntry { binding: 2, resource: self.vel.as_entire_binding() },
                wgpu::BindGroupEntry { binding: 3, resource: wgpu::BindingResource::TextureView(depth) },
            ],
        });
        let depth_view = &self.depth.as_ref().expect("mud depth").1;
        let mut pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
            label: Some("wgr_mud_draw"),
            color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                view: scene,
                depth_slice: None,
                resolve_target: None,
                ops: wgpu::Operations { load: wgpu::LoadOp::Load, store: wgpu::StoreOp::Store },
            })],
            depth_stencil_attachment: Some(wgpu::RenderPassDepthStencilAttachment {
                view: depth_view,
                depth_ops: Some(wgpu::Operations { load: wgpu::LoadOp::Clear(0.0), store: wgpu::StoreOp::Discard }),
                stencil_ops: None,
            }),
            timestamp_writes: None,
            occlusion_query_set: None,
            multiview_mask: None,
        });
        pass.set_pipeline(&self.draw);
        pass.set_bind_group(0, &draw_bind, &[]);
        pass.draw(0..CAPACITY * 6, 0..1);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn emitter_layout_is_48_bytes() {
        assert_eq!(std::mem::size_of::<MudEmitter>(), 48);
        assert_eq!(std::mem::size_of::<SimParams>() % 16, 0);
        assert_eq!(std::mem::size_of::<DrawParams>() % 16, 0);
    }

    #[test]
    fn rest_density_is_positive_and_finite() {
        let r = rest_density();
        assert!(r.is_finite() && r > 0.0, "{r}");
    }

    #[test]
    fn shaders_validate() {
        for (name, src) in [("mud_sim", include_str!("mud_sim.wgsl")), ("mud_draw", include_str!("mud_draw.wgsl"))] {
            let module = naga::front::wgsl::parse_str(src).unwrap_or_else(|e| panic!("{name}: {}", e.emit_to_string(src)));
            naga::valid::Validator::new(naga::valid::ValidationFlags::all(), naga::valid::Capabilities::all())
                .validate(&module)
                .unwrap_or_else(|e| panic!("{name}: {e:?}"));
        }
    }
}
