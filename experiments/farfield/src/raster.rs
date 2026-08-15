use crate::camera::View;
use crate::cdlod::{node_dist_sq, Tree, LEAF_TEXELS};
use crate::gpu::{write_bmp, Gpu, Stat, Timer};
use crate::raymarch::{read_texture, target};
use crate::world::{World, WorldGpu, WorldSpec, COVER_SHIFT};
use bytemuck::{Pod, Zeroable};
use glam::{Mat4, Vec3, Vec4};

#[repr(C)]
#[derive(Clone, Copy, Pod, Zeroable, Default)]
struct Params {
    view_proj: [[f32; 4]; 4],
    cam: [f32; 4],
    world: [f32; 4],
    sun: [f32; 4],
    cover: [f32; 4],
    bounds: [f32; 4],
}

#[repr(C)]
#[derive(Clone, Copy, Pod, Zeroable, Default)]
struct Inst {
    origin: [f32; 2],
    size_level: [f32; 2],
    morph: [f32; 2],
}

const GRID_N: u32 = LEAF_TEXELS; // 32, matching TerrainWgpu

fn patch_mesh(gpu: &Gpu) -> (wgpu::Buffer, wgpu::Buffer, u32) {
    let side = GRID_N + 1;
    let mut verts = Vec::with_capacity((side * side) as usize);
    for z in 0..side {
        for x in 0..side {
            verts.push([x as f32 / GRID_N as f32, z as f32 / GRID_N as f32]);
        }
    }
    let mut idx: Vec<u32> = Vec::with_capacity((GRID_N * GRID_N * 6) as usize);
    for z in 0..GRID_N {
        for x in 0..GRID_N {
            let a = z * side + x;
            idx.extend_from_slice(&[a, a + side, a + 1, a + 1, a + side, a + side + 1]);
        }
    }
    let vb = buffer(gpu, "patch-vb", bytemuck::cast_slice(&verts), wgpu::BufferUsages::VERTEX);
    let ib = buffer(gpu, "patch-ib", bytemuck::cast_slice(&idx), wgpu::BufferUsages::INDEX);
    (vb, ib, idx.len() as u32)
}

fn buffer(gpu: &Gpu, label: &str, data: &[u8], usage: wgpu::BufferUsages) -> wgpu::Buffer {
    let b = gpu.device.create_buffer(&wgpu::BufferDescriptor {
        label: Some(label),
        size: data.len().max(4) as u64,
        usage: usage | wgpu::BufferUsages::COPY_DST,
        mapped_at_creation: false,
    });
    gpu.queue.write_buffer(&b, 0, data);
    b
}

pub struct RasterResult {
    pub ms: Stat,
    pub instances: usize,
    pub triangles: u64,
    pub select_us: f32,
    pub rgba: Vec<u8>,
}

pub struct Raster {
    pipeline: wgpu::RenderPipeline,
    sky: wgpu::RenderPipeline,
    bgl: wgpu::BindGroupLayout,
    ubo: wgpu::Buffer,
    vb: wgpu::Buffer,
    ib: wgpu::Buffer,
    index_count: u32,
    inst: wgpu::Buffer,
    inst_cap: usize,
    color: wgpu::Texture,
    color_view: wgpu::TextureView,
    depth_view: wgpu::TextureView,
    pub width: u32,
    pub height: u32,
}

impl Raster {
    pub fn new(gpu: &Gpu, width: u32, height: u32, inst_cap: usize) -> Self {
        let module = gpu.shader("raster", include_str!("raster.wgsl"));
        let bgl = gpu
            .device
            .create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
                label: Some("raster-bgl"),
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
                        binding: 1,
                        visibility: wgpu::ShaderStages::VERTEX_FRAGMENT,
                        ty: wgpu::BindingType::Texture {
                            sample_type: wgpu::TextureSampleType::Float { filterable: false },
                            view_dimension: wgpu::TextureViewDimension::D2,
                            multisampled: false,
                        },
                        count: None,
                    },
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
                ],
            });
        let pl = gpu
            .device
            .create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
                label: Some("raster-pl"),
                bind_group_layouts: &[Some(&bgl)],
                immediate_size: 0,
            });

        let color_state = wgpu::ColorTargetState {
            format: wgpu::TextureFormat::Rgba8Unorm,
            blend: None,
            write_mask: wgpu::ColorWrites::ALL,
        };
        // Reverse-Z: clear to 0, keep the greater depth.
        let depth_state = wgpu::DepthStencilState {
            format: wgpu::TextureFormat::Depth32Float,
            depth_write_enabled: Some(true),
            depth_compare: Some(wgpu::CompareFunction::Greater),
            stencil: Default::default(),
            bias: Default::default(),
        };

        let pipeline = gpu
            .device
            .create_render_pipeline(&wgpu::RenderPipelineDescriptor {
                label: Some("cdlod-terrain"),
                layout: Some(&pl),
                vertex: wgpu::VertexState {
                    module: &module,
                    entry_point: Some("vs"),
                    compilation_options: Default::default(),
                    buffers: &[
                        wgpu::VertexBufferLayout {
                            array_stride: 8,
                            step_mode: wgpu::VertexStepMode::Vertex,
                            attributes: &wgpu::vertex_attr_array![0 => Float32x2],
                        },
                        wgpu::VertexBufferLayout {
                            array_stride: std::mem::size_of::<Inst>() as u64,
                            step_mode: wgpu::VertexStepMode::Instance,
                            attributes: &wgpu::vertex_attr_array![1 => Float32x2, 2 => Float32x2, 3 => Float32x2],
                        },
                    ],
                },
                primitive: wgpu::PrimitiveState {
                    cull_mode: None,
                    ..Default::default()
                },
                depth_stencil: Some(depth_state.clone()),
                multisample: Default::default(),
                fragment: Some(wgpu::FragmentState {
                    module: &module,
                    entry_point: Some("fs"),
                    compilation_options: Default::default(),
                    targets: &[Some(color_state.clone())],
                }),
                multiview_mask: None,
                cache: None,
            });

        let sky = gpu
            .device
            .create_render_pipeline(&wgpu::RenderPipelineDescriptor {
                label: Some("sky"),
                layout: Some(&pl),
                vertex: wgpu::VertexState {
                    module: &module,
                    entry_point: Some("vs_sky"),
                    compilation_options: Default::default(),
                    buffers: &[],
                },
                primitive: Default::default(),
                depth_stencil: Some(wgpu::DepthStencilState {
                    depth_write_enabled: Some(false),
                    depth_compare: Some(wgpu::CompareFunction::Always),
                    ..depth_state
                }),
                multisample: Default::default(),
                fragment: Some(wgpu::FragmentState {
                    module: &module,
                    entry_point: Some("fs_sky"),
                    compilation_options: Default::default(),
                    targets: &[Some(color_state)],
                }),
                multiview_mask: None,
                cache: None,
            });

        let (vb, ib, index_count) = patch_mesh(gpu);
        let ubo = gpu.device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("raster-ubo"),
            size: std::mem::size_of::<Params>() as u64,
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        let inst = gpu.device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("raster-inst"),
            size: (inst_cap * std::mem::size_of::<Inst>()) as u64,
            usage: wgpu::BufferUsages::VERTEX | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        let (color, color_view) = target(gpu, width, height, wgpu::TextureFormat::Rgba8Unorm);
        let depth = gpu.device.create_texture(&wgpu::TextureDescriptor {
            label: Some("depth"),
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
            pipeline, sky, bgl, ubo, vb, ib, index_count, inst, inst_cap,
            color, color_view, depth_view, width, height,
        }
    }

    pub fn run(
        &self,
        gpu: &Gpu,
        w: &World,
        wg: &WorldGpu,
        tree: &Tree,
        view: &View,
        far: f32,
        frames: usize,
    ) -> RasterResult {
        let b = view.basis();
        let aspect = self.width as f32 / self.height as f32;
        // Camera-relative view (eye at origin) + reverse-Z infinite projection.
        let vm = Mat4::look_to_rh(Vec3::ZERO, b.fwd, b.up);
        let pm = Mat4::perspective_infinite_reverse_rh(view.fov_y, aspect, 1.0);
        let view_proj = pm * vm;

        // --- CPU CDLOD selection, timed on its own ---
        let t0 = std::time::Instant::now();
        let cam = [view.eye.x, view.eye.y, view.eye.z];
        // Frustum planes in camera-relative space (Gribb-Hartmann on view_proj).
        let m = view_proj.to_cols_array_2d();
        let row = |i: usize| Vec4::new(m[0][i], m[1][i], m[2][i], m[3][i]);
        let (r0, r1, r3) = (row(0), row(1), row(3));
        let planes = [r3 + r0, r3 - r0, r3 + r1, r3 - r1, r3];
        let mut norm_planes = [Vec4::ZERO; 5];
        for (i, p) in planes.iter().enumerate() {
            let n = Vec3::new(p.x, p.y, p.z);
            let len = n.length().max(1e-9);
            norm_planes[i] = *p / len;
        }
        let far2 = far * far;
        let visible = |n: &crate::cdlod::Node| -> bool {
            if node_dist_sq(n, cam) > far2 {
                return false;
            }
            let c = Vec3::new(
                n.origin[0] + n.size * 0.5,
                (n.min_y + n.max_y) * 0.5,
                n.origin[1] + n.size * 0.5,
            ) - view.eye;
            let dy = n.max_y - n.min_y;
            let radius = 0.5 * (2.0 * n.size * n.size + dy * dy).sqrt();
            norm_planes
                .iter()
                .all(|p| p.x * c.x + p.y * c.y + p.z * c.z + p.w >= -radius)
        };
        let mut sel = Vec::new();
        tree.select(cam, &visible, &mut sel);
        let select_us = t0.elapsed().as_secs_f32() * 1.0e6;

        let inst: Vec<Inst> = sel
            .iter()
            .take(self.inst_cap)
            .map(|s| Inst {
                origin: s.origin,
                size_level: [s.size, s.level as f32],
                morph: [s.morph_start, s.morph_end],
            })
            .collect();
        gpu.queue.write_buffer(&self.inst, 0, bytemuck::cast_slice(&inst));

        let cn = w.spec.n >> COVER_SHIFT;
        let p = Params {
            view_proj: view_proj.to_cols_array_2d(),
            cam: [view.eye.x, view.eye.y, view.eye.z, 0.0],
            world: [w.spec.grid, w.spec.n as f32, tree.levels as f32, w.spec.extent_m()],
            sun: [0.42, 0.72, 0.55, 0.0],
            cover: [
                cn as f32,
                w.spec.grid * (1 << COVER_SHIFT) as f32,
                crate::world::CANOPY_SCALE,
                crate::world::BUILDING_SCALE,
            ],
            bounds: [w.h_min, w.h_max, 0.0, 0.0],
        };
        gpu.queue.write_buffer(&self.ubo, 0, bytemuck::bytes_of(&p));

        let bg = gpu.device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("raster-bg"),
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
            ],
        });

        let timer = Timer::new(gpu);
        let mut samples = Vec::new();
        for _ in 0..frames {
            let mut enc = gpu
                .device
                .create_command_encoder(&wgpu::CommandEncoderDescriptor { label: None });
            {
                let mut rp = enc.begin_render_pass(&wgpu::RenderPassDescriptor {
                    label: Some("cdlod"),
                    color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                        view: &self.color_view,
                        depth_slice: None,
                        resolve_target: None,
                        ops: wgpu::Operations {
                            load: wgpu::LoadOp::Clear(wgpu::Color::BLACK),
                            store: wgpu::StoreOp::Store,
                        },
                    })],
                    depth_stencil_attachment: Some(wgpu::RenderPassDepthStencilAttachment {
                        view: &self.depth_view,
                        depth_ops: Some(wgpu::Operations {
                            load: wgpu::LoadOp::Clear(0.0), // reverse-Z
                            store: wgpu::StoreOp::Store,
                        }),
                        stencil_ops: None,
                    }),
                    timestamp_writes: timer.writes(),
                    occlusion_query_set: None,
                    multiview_mask: None,
                });
                rp.set_bind_group(0, &bg, &[]);
                rp.set_pipeline(&self.sky);
                rp.draw(0..3, 0..1);
                rp.set_pipeline(&self.pipeline);
                rp.set_vertex_buffer(0, self.vb.slice(..));
                rp.set_vertex_buffer(1, self.inst.slice(..));
                rp.set_index_buffer(self.ib.slice(..), wgpu::IndexFormat::Uint32);
                rp.draw_indexed(0..self.index_count, 0, 0..inst.len() as u32);
            }
            timer.resolve_into(&mut enc);
            gpu.queue.submit([enc.finish()]);
            gpu.device.poll(wgpu::PollType::wait_indefinitely()).unwrap();
            if let Some(ms) = timer.read_ms(gpu) {
                samples.push(ms);
            }
        }

        let rgba = read_texture(gpu, &self.color, self.width, self.height, 4);
        RasterResult {
            ms: Stat::of(samples),
            instances: inst.len(),
            triangles: inst.len() as u64 * (GRID_N as u64 * GRID_N as u64 * 2),
            select_us,
            rgba,
        }
    }
}

pub fn cmd_raster(gpu: &Gpu, c: &crate::Common) {
    std::fs::create_dir_all(&c.out).ok();
    let spec = WorldSpec { n: c.n, grid: c.grid, seed: c.seed, with_cover: false, veg_cell: 0.0 };
    let w = World::generate(spec);
    let wg = WorldGpu::upload(gpu, &w);

    let t0 = std::time::Instant::now();
    let tree = Tree::new(&w, 4.0, 2.0);
    eprintln!(
        "cdlod tree: {} nodes, {} levels, leaf {:.0} m, ranges {:.0}..{:.0} m, built in {:.0} ms",
        tree.nodes.len(),
        tree.levels,
        tree.leaf_size,
        tree.ranges[0],
        tree.ranges[tree.ranges.len() - 1],
        t0.elapsed().as_secs_f32() * 1000.0
    );

    let r = Raster::new(gpu, c.width, c.height, 200_000);
    println!();
    println!(
        "{:<24} {:<11} {:>9} {:>9} {:>10} {:>12} {:>11}",
        "experiment", "view", "ms(min)", "ms(med)", "instances", "triangles", "select us"
    );
    for far in [5_000.0f32, 20_000.0, 100_000.0] {
        for (vname, view) in crate::camera::standard_views(w.spec.extent_m(), w.h_max * 0.5) {
            let res = r.run(gpu, &w, &wg, &tree, &view, far, c.frames);
            println!(
                "{:<24} {:<11} {:>9.3} {:>9.3} {:>10} {:>12} {:>11.0}",
                format!("EXP-TERRAIN far={:.0}km", far / 1000.0),
                vname,
                res.ms.min,
                res.ms.median,
                res.instances,
                res.triangles,
                res.select_us
            );
            if c.shots && (far - 100_000.0).abs() < 1.0 {
                write_bmp(
                    &format!("{}/EXP-TERRAIN-{}.bmp", c.out, vname),
                    c.width,
                    c.height,
                    &res.rgba,
                )
                .ok();
            }
        }
    }
}
