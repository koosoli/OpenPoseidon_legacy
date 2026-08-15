// EXP-VEG-002: GPU-placed far vegetation cards driven by a density field.
//
// Two passes, timed separately: a placement compute that appends accepted trees into
// an instance buffer and writes the indirect draw args, and one indirect draw of
// camera-facing cards. Nothing is streamed and nothing is stored per tree.

use crate::camera::View;
use crate::gpu::{write_bmp, Gpu, Stat, Timer};
use crate::raymarch::{read_texture, target};
use crate::world::{World, WorldGpu, WorldSpec, BUILDING_SCALE, CANOPY_SCALE, COVER_SHIFT};
use bytemuck::{Pod, Zeroable};
use glam::{Mat4, Vec3};

#[repr(C)]
#[derive(Clone, Copy, Pod, Zeroable, Default)]
struct Params {
    view_proj: [[f32; 4]; 4],
    cam: [f32; 4],
    fwd: [f32; 4],
    right: [f32; 4],
    up: [f32; 4],
    world: [f32; 4],
    cfg: [f32; 4],
    sun: [f32; 4],
    cover_scale: [f32; 4],
}

const INSTANCE_STRIDE: u64 = 32; // vec3 pos + f32 height + f32 seed + vec3 pad
const MAX_INSTANCES: u64 = 4_000_000;

pub struct Veg {
    place: wgpu::ComputePipeline,
    draw: wgpu::RenderPipeline,
    draw_box: wgpu::RenderPipeline,
    bgl: wgpu::BindGroupLayout,
    bgl_draw: wgpu::BindGroupLayout,
    ubo: wgpu::Buffer,
    instances: wgpu::Buffer,
    args: wgpu::Buffer,
    args_init: wgpu::Buffer,
    args_init_box: wgpu::Buffer,
    color_view: wgpu::TextureView,
    color: wgpu::Texture,
    depth_view: wgpu::TextureView,
    width: u32,
    height: u32,
}

impl Veg {
    pub fn new(gpu: &Gpu, width: u32, height: u32) -> Self {
        // Two modules on purpose: the placement compute binds the instance buffer
        // read_write, the draw binds it read-only. One module cannot do both without
        // the non-portable VERTEX_WRITABLE_STORAGE feature.
        let m_place = gpu.shader("veg-place", include_str!("veg_place.wgsl"));
        let m_draw = gpu.shader("veg-draw", include_str!("veg_draw.wgsl"));
        let st = |binding: u32, ro: bool| wgpu::BindGroupLayoutEntry {
            binding,
            visibility: wgpu::ShaderStages::COMPUTE,
            ty: wgpu::BindingType::Buffer {
                ty: wgpu::BufferBindingType::Storage { read_only: ro },
                has_dynamic_offset: false,
                min_binding_size: None,
            },
            count: None,
        };
        let bgl = gpu
            .device
            .create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
                label: Some("veg-bgl"),
                entries: &[
                    wgpu::BindGroupLayoutEntry {
                        binding: 0,
                        visibility: wgpu::ShaderStages::COMPUTE
                            | wgpu::ShaderStages::VERTEX
                            | wgpu::ShaderStages::FRAGMENT,
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
                        ty: wgpu::BindingType::Texture {
                            sample_type: wgpu::TextureSampleType::Float { filterable: false },
                            view_dimension: wgpu::TextureViewDimension::D2,
                            multisampled: false,
                        },
                        count: None,
                    },
                    // The instance buffer is written by the compute and read by the
                    // vertex stage, so it needs VERTEX_WRITABLE_STORAGE-free handling:
                    // declared read_write here, but the vertex stage only reads it.
                    st(3, false),
                    st(4, false),
                ],
            });
        let bgl_draw = gpu
            .device
            .create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
                label: Some("veg-draw-bgl"),
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
                    wgpu::BindGroupLayoutEntry {
                        binding: 1,
                        visibility: wgpu::ShaderStages::VERTEX,
                        ty: wgpu::BindingType::Buffer {
                            ty: wgpu::BufferBindingType::Storage { read_only: true },
                            has_dynamic_offset: false,
                            min_binding_size: None,
                        },
                        count: None,
                    },
                ],
            });
        let pl = gpu
            .device
            .create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
                label: Some("veg-pl"),
                bind_group_layouts: &[Some(&bgl)],
                immediate_size: 0,
            });
        let pl_draw = gpu
            .device
            .create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
                label: Some("veg-draw-pl"),
                bind_group_layouts: &[Some(&bgl_draw)],
                immediate_size: 0,
            });
        let place = gpu
            .device
            .create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
                label: Some("veg-place"),
                layout: Some(&pl),
                module: &m_place,
                entry_point: Some("place"),
                compilation_options: Default::default(),
                cache: None,
            });
        let draw = gpu
            .device
            .create_render_pipeline(&wgpu::RenderPipelineDescriptor {
                label: Some("veg-cards"),
                layout: Some(&pl_draw),
                vertex: wgpu::VertexState {
                    module: &m_draw,
                    entry_point: Some("vs"),
                    compilation_options: Default::default(),
                    buffers: &[],
                },
                primitive: wgpu::PrimitiveState { cull_mode: None, ..Default::default() },
                depth_stencil: Some(wgpu::DepthStencilState {
                    format: wgpu::TextureFormat::Depth32Float,
                    depth_write_enabled: Some(true),
                    depth_compare: Some(wgpu::CompareFunction::Greater),
                    stencil: Default::default(),
                    bias: Default::default(),
                }),
                multisample: Default::default(),
                fragment: Some(wgpu::FragmentState {
                    module: &m_draw,
                    entry_point: Some("fs"),
                    compilation_options: Default::default(),
                    targets: &[Some(wgpu::ColorTargetState {
                        format: wgpu::TextureFormat::Rgba8Unorm,
                        blend: None,
                        write_mask: wgpu::ColorWrites::ALL,
                    })],
                }),
                multiview_mask: None,
                cache: None,
            });

        let draw_box = gpu
            .device
            .create_render_pipeline(&wgpu::RenderPipelineDescriptor {
                label: Some("veg-boxes"),
                layout: Some(&pl_draw),
                vertex: wgpu::VertexState {
                    module: &m_draw,
                    entry_point: Some("vs_box"),
                    compilation_options: Default::default(),
                    buffers: &[],
                },
                primitive: wgpu::PrimitiveState { cull_mode: None, ..Default::default() },
                depth_stencil: Some(wgpu::DepthStencilState {
                    format: wgpu::TextureFormat::Depth32Float,
                    depth_write_enabled: Some(true),
                    depth_compare: Some(wgpu::CompareFunction::Greater),
                    stencil: Default::default(),
                    bias: Default::default(),
                }),
                multisample: Default::default(),
                fragment: Some(wgpu::FragmentState {
                    module: &m_draw,
                    entry_point: Some("fs_box"),
                    compilation_options: Default::default(),
                    targets: &[Some(wgpu::ColorTargetState {
                        format: wgpu::TextureFormat::Rgba8Unorm,
                        blend: None,
                        write_mask: wgpu::ColorWrites::ALL,
                    })],
                }),
                multiview_mask: None,
                cache: None,
            });
        let ubo = gpu.device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("veg-ubo"),
            size: std::mem::size_of::<Params>() as u64,
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        let instances = gpu.device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("veg-instances"),
            size: MAX_INSTANCES * INSTANCE_STRIDE,
            usage: wgpu::BufferUsages::STORAGE,
            mapped_at_creation: false,
        });
        let args = gpu.device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("veg-args"),
            size: 20,
            usage: wgpu::BufferUsages::STORAGE
                | wgpu::BufferUsages::INDIRECT
                | wgpu::BufferUsages::COPY_DST
                | wgpu::BufferUsages::COPY_SRC,
            mapped_at_creation: false,
        });
        let args_init = gpu.device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("veg-args-init"),
            size: 20,
            usage: wgpu::BufferUsages::COPY_SRC | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        // draw(vertex_count=6, instance_count=0, first_vertex=0, first_instance=0)
        gpu.queue
            .write_buffer(&args_init, 0, bytemuck::cast_slice(&[6u32, 0, 0, 0, 0]));
        let args_init_box = gpu.device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("veg-args-init-box"),
            size: 20,
            usage: wgpu::BufferUsages::COPY_SRC | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        gpu.queue
            .write_buffer(&args_init_box, 0, bytemuck::cast_slice(&[36u32, 0, 0, 0, 0]));

        let (color, color_view) = target(gpu, width, height, wgpu::TextureFormat::Rgba8Unorm);
        let depth = gpu.device.create_texture(&wgpu::TextureDescriptor {
            label: Some("veg-depth"),
            size: wgpu::Extent3d { width, height, depth_or_array_layers: 1 },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: wgpu::TextureFormat::Depth32Float,
            usage: wgpu::TextureUsages::RENDER_ATTACHMENT,
            view_formats: &[],
        });
        let depth_view = depth.create_view(&wgpu::TextureViewDescriptor::default());

        Self {
            place, draw, draw_box, bgl, bgl_draw, ubo, instances, args, args_init, args_init_box,
            color, color_view, depth_view, width, height,
        }
    }

    pub fn run(
        &self,
        gpu: &Gpu,
        w: &World,
        wg: &WorldGpu,
        view: &View,
        grid_dim: u32,
        spacing: f32,
        radius: f32,
        near_cutoff: f32,
        frames: usize,
        boxes: bool,
        accept_scale: f32,
    ) -> (Stat, Stat, u32, Vec<u8>) {
        let b = view.basis();
        let aspect = self.width as f32 / self.height as f32;
        let vm = Mat4::look_to_rh(Vec3::ZERO, b.fwd, b.up);
        let pm = Mat4::perspective_infinite_reverse_rh(view.fov_y, aspect, 1.0);
        let view_proj = pm * vm;
        let cn = w.spec.n >> COVER_SHIFT;

        let p = Params {
            view_proj: view_proj.to_cols_array_2d(),
            cam: [view.eye.x, view.eye.y, view.eye.z, near_cutoff],
            fwd: [b.fwd.x, b.fwd.y, b.fwd.z, 0.0],
            right: [b.right.x, b.right.y, b.right.z, 0.0],
            up: [b.up.x, b.up.y, b.up.z, 0.0],
            world: [
                w.spec.grid,
                w.spec.n as f32,
                cn as f32,
                w.spec.grid * (1 << COVER_SHIFT) as f32,
            ],
            cfg: [grid_dim as f32, spacing, radius, CANOPY_SCALE],
            sun: [0.42, 0.72, 0.55, if boxes { 1.0 } else { 0.0 }],
            cover_scale: [CANOPY_SCALE, BUILDING_SCALE, accept_scale, 0.0],
        };
        gpu.queue.write_buffer(&self.ubo, 0, bytemuck::bytes_of(&p));

        let bg_draw = gpu.device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("veg-draw-bg"),
            layout: &self.bgl_draw,
            entries: &[
                wgpu::BindGroupEntry { binding: 0, resource: self.ubo.as_entire_binding() },
                wgpu::BindGroupEntry { binding: 1, resource: self.instances.as_entire_binding() },
            ],
        });
        let bg = gpu.device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("veg-bg"),
            layout: &self.bgl,
            entries: &[
                wgpu::BindGroupEntry { binding: 0, resource: self.ubo.as_entire_binding() },
                wgpu::BindGroupEntry {
                    binding: 1,
                    resource: wgpu::BindingResource::TextureView(&wg.height_view),
                },
                wgpu::BindGroupEntry {
                    binding: 2,
                    resource: wgpu::BindingResource::TextureView(&wg.cover_view),
                },
                wgpu::BindGroupEntry { binding: 3, resource: self.instances.as_entire_binding() },
                wgpu::BindGroupEntry { binding: 4, resource: self.args.as_entire_binding() },
            ],
        });

        let t_place = Timer::new(gpu);
        let t_draw = Timer::new(gpu);
        let mut place_ms = Vec::new();
        let mut draw_ms = Vec::new();
        let mut count = 0u32;

        for _ in 0..frames {
            let mut enc = gpu
                .device
                .create_command_encoder(&wgpu::CommandEncoderDescriptor { label: None });
            let init = if boxes { &self.args_init_box } else { &self.args_init };
            enc.copy_buffer_to_buffer(init, 0, &self.args, 0, 20);
            {
                let mut cp = enc.begin_compute_pass(&wgpu::ComputePassDescriptor {
                    label: Some("veg-place"),
                    timestamp_writes: t_place.writes().map(|w| wgpu::ComputePassTimestampWrites {
                        query_set: w.query_set,
                        beginning_of_pass_write_index: w.beginning_of_pass_write_index,
                        end_of_pass_write_index: w.end_of_pass_write_index,
                    }),
                });
                cp.set_pipeline(&self.place);
                cp.set_bind_group(0, &bg, &[]);
                cp.dispatch_workgroups(grid_dim.div_ceil(8), grid_dim.div_ceil(8), 1);
            }
            {
                let mut rp = enc.begin_render_pass(&wgpu::RenderPassDescriptor {
                    label: Some("veg-cards"),
                    color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                        view: &self.color_view,
                        depth_slice: None,
                        resolve_target: None,
                        ops: wgpu::Operations {
                            load: wgpu::LoadOp::Clear(wgpu::Color {
                                r: 0.72, g: 0.80, b: 0.90, a: 1.0,
                            }),
                            store: wgpu::StoreOp::Store,
                        },
                    })],
                    depth_stencil_attachment: Some(wgpu::RenderPassDepthStencilAttachment {
                        view: &self.depth_view,
                        depth_ops: Some(wgpu::Operations {
                            load: wgpu::LoadOp::Clear(0.0),
                            store: wgpu::StoreOp::Store,
                        }),
                        stencil_ops: None,
                    }),
                    timestamp_writes: t_draw.writes(),
                    occlusion_query_set: None,
                    multiview_mask: None,
                });
                rp.set_pipeline(if boxes { &self.draw_box } else { &self.draw });
                rp.set_bind_group(0, &bg_draw, &[]);
                rp.draw_indirect(&self.args, 0);
            }
            t_place.resolve_into(&mut enc);
            t_draw.resolve_into(&mut enc);
            gpu.queue.submit([enc.finish()]);
            gpu.device.poll(wgpu::PollType::wait_indefinitely()).unwrap();
            if let Some(ms) = t_place.read_ms(gpu) {
                place_ms.push(ms);
            }
            if let Some(ms) = t_draw.read_ms(gpu) {
                draw_ms.push(ms);
            }
            let a = gpu.read_buffer(&self.args, 20);
            count = u32::from_le_bytes(a[4..8].try_into().unwrap());
        }

        let rgba = read_texture(gpu, &self.color, self.width, self.height, 4);
        (Stat::of(place_ms), Stat::of(draw_ms), count, rgba)
    }
}

pub fn cmd_veg_cards(gpu: &Gpu, c: &crate::Common) {
    std::fs::create_dir_all(&c.out).ok();
    let spec = WorldSpec { n: c.n, grid: c.grid, seed: c.seed, with_cover: true, veg_cell: 0.0 };
    let w = World::generate(spec);
    let wg = WorldGpu::upload(gpu, &w);
    let v = Veg::new(gpu, c.width, c.height);

    println!();
    println!("EXP-VEG-002  density-field vegetation cards, no per-tree storage");
    println!(
        "{:<10} {:>8} {:>9} {:>10} {:>10} {:>11} {:>10} {:>10}",
        "view", "radius", "spacing", "candidates", "placed", "trees/ha eq", "place ms", "draw ms"
    );
    // Candidate spacing of 12 m is ~69 trees/ha at full density: a plausible visible
    // stem density once individual trees are a few pixels tall.
    for &(radius, spacing) in &[(1000.0f32, 6.0f32), (2000.0, 8.0), (5000.0, 12.0)] {
        let dim = ((2.0 * radius / spacing).ceil() as u32).min(4096);
        for (vname, view) in crate::camera::standard_views(w.spec.extent_m(), w.h_max * 0.5) {
            let (pl, dr, n, rgba) = v.run(gpu, &w, &wg, &view, dim, spacing, radius, 0.0, c.frames, false, 1.0);
            println!(
                "{:<10} {:>8.0} {:>9.1} {:>10} {:>10} {:>11.0} {:>10.3} {:>10.3}",
                vname,
                radius,
                spacing,
                dim as u64 * dim as u64,
                n,
                10_000.0 / (spacing * spacing),
                pl.min,
                dr.min
            );
            if c.shots && (radius - 5000.0).abs() < 1.0 {
                write_bmp(
                    &format!("{}/EXP-VEG-002-{}.bmp", c.out, vname),
                    c.width,
                    c.height,
                    &rgba,
                )
                .ok();
            }
        }
    }
}

/// EXP-ASSET-001. The same GPU-placement machinery, but emitting extruded footprint
/// prisms instead of billboards -- the question being whether a far ASSET tier is as
/// cheap as a far vegetation tier once the geometry stops being a single quad.
pub fn cmd_buildings(gpu: &Gpu, c: &crate::Common) {
    std::fs::create_dir_all(&c.out).ok();
    let spec = WorldSpec { n: c.n, grid: c.grid, seed: c.seed, with_cover: true, veg_cell: 0.0 };
    let w = World::generate(spec);
    let wg = WorldGpu::upload(gpu, &w);
    let v = Veg::new(gpu, c.width, c.height);

    println!();
    println!("EXP-ASSET-001  far buildings as extruded footprint prisms (36 verts each)");
    println!(
        "{:<10} {:>8} {:>10} {:>10} {:>11} {:>10} {:>10}",
        "view", "radius", "candidates", "placed", "triangles", "place ms", "draw ms"
    );
    let views = crate::camera::standard_views(w.spec.extent_m(), w.h_max * 0.5);
    for &(radius, spacing) in &[(5000.0f32, 14.0f32), (20000.0, 20.0)] {
        let dim = ((2.0 * radius / spacing).ceil() as u32).min(4096);
        for &accept in &[1.0f32, 20.0, 120.0, 600.0] {
            let (vname, view) = &views[1];
            let (pl, dr, n, rgba) =
                v.run(gpu, &w, &wg, view, dim, spacing, radius, 0.0, c.frames, true, accept);
            println!(
                "{:<10} {:>8.0} {:>10} {:>10} {:>11} {:>10.3} {:>10.3}",
                format!("{}x{}", vname, accept as u32), radius,
                dim as u64 * dim as u64, n, n as u64 * 12, pl.min, dr.min
            );
            if c.shots && accept > 100.0 && accept < 200.0 {
                write_bmp(
                    &format!("{}/EXP-ASSET-001-{}-{}.bmp", c.out, vname, accept as u32),
                    c.width, c.height, &rgba,
                ).ok();
            }
        }
    }
}
