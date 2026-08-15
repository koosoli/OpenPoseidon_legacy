// EXP-ASSET-002 — far-field cost against a real, shipped placement set.
//
// Loads the binary produced by mkplacements.py (see its docstring), uploads every
// placement once, and each frame runs one compute that culls the whole array and
// compacts survivors into an indirect draw. No CPU per-object work, no residency
// budget, no scene objects.

use crate::gpu::{write_bmp, Gpu, Stat, Timer};
use crate::raymarch::{read_texture, target};
use bytemuck::{Pod, Zeroable};
use glam::{Mat4, Vec3};

#[repr(C)]
#[derive(Clone, Copy, Pod, Zeroable, Default)]
struct CullParams {
    view_proj: [[f32; 4]; 4],
    cam: [f32; 4],
    cfg: [f32; 4],
    classes: [f32; 4],
}

#[repr(C)]
#[derive(Clone, Copy, Pod, Zeroable, Default)]
struct DrawParams {
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

#[repr(C)]
#[derive(Clone, Copy, Pod, Zeroable)]
struct Src {
    pos: [f32; 3],
    class_id: u32,
}

pub struct Placements {
    pub src: Vec<Src>,
    pub counts: [u32; 3],
    pub min: [f32; 3],
    pub max: [f32; 3],
}

pub fn load(path: &str) -> std::io::Result<Placements> {
    let bytes = std::fs::read(path)?;
    let n = u32::from_le_bytes(bytes[0..4].try_into().unwrap()) as usize;
    let mut src = Vec::with_capacity(n);
    let mut counts = [0u32; 3];
    let mut min = [f32::MAX; 3];
    let mut max = [f32::MIN; 3];
    for i in 0..n {
        let o = 4 + i * 13;
        let x = f32::from_le_bytes(bytes[o..o + 4].try_into().unwrap());
        let y = f32::from_le_bytes(bytes[o + 4..o + 8].try_into().unwrap());
        let z = f32::from_le_bytes(bytes[o + 8..o + 12].try_into().unwrap());
        let c = bytes[o + 12] as u32;
        counts[(c as usize).min(2)] += 1;
        for (k, v) in [x, y, z].iter().enumerate() {
            min[k] = min[k].min(*v);
            max[k] = max[k].max(*v);
        }
        src.push(Src { pos: [x, y, z], class_id: c });
    }
    Ok(Placements { src, counts, min, max })
}

pub fn cmd_real(gpu: &Gpu, path: &str, c: &crate::Common) {
    let p = match load(path) {
        Ok(p) => p,
        Err(e) => {
            eprintln!("could not read {path}: {e}");
            eprintln!("generate it with: python mkplacements.py <objects.txt> <out.bin>");
            return;
        }
    };
    std::fs::create_dir_all(&c.out).ok();
    let n = p.src.len() as u32;
    eprintln!(
        "placements {} ({} veg / {} struct / {} other), extent {:.0} x {:.0} m, source buffer {:.1} MB",
        n, p.counts[0], p.counts[1], p.counts[2],
        p.max[0] - p.min[0], p.max[2] - p.min[2],
        (n as f64 * 16.0) / 1.048576e6
    );

    let m_cull = gpu.shader("real-cull", include_str!("real_cull.wgsl"));
    let m_draw = gpu.shader("real-draw", include_str!("veg_draw.wgsl"));

    let buf = |label: &str, size: u64, usage: wgpu::BufferUsages| {
        gpu.device.create_buffer(&wgpu::BufferDescriptor {
            label: Some(label),
            size,
            usage,
            mapped_at_creation: false,
        })
    };
    let src_buf = buf(
        "src",
        (n as u64) * 16,
        wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_DST,
    );
    gpu.queue.write_buffer(&src_buf, 0, bytemuck::cast_slice(&p.src));
    let inst = buf("inst", 4_000_000 * 32, wgpu::BufferUsages::STORAGE);
    let args = buf(
        "args",
        20,
        wgpu::BufferUsages::STORAGE
            | wgpu::BufferUsages::INDIRECT
            | wgpu::BufferUsages::COPY_DST
            | wgpu::BufferUsages::COPY_SRC,
    );
    let cull_ubo = buf(
        "cull-ubo",
        std::mem::size_of::<CullParams>() as u64,
        wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
    );
    let draw_ubo = buf(
        "draw-ubo",
        std::mem::size_of::<DrawParams>() as u64,
        wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
    );

    let sb = |b: u32, ro: bool| wgpu::BindGroupLayoutEntry {
        binding: b,
        visibility: wgpu::ShaderStages::COMPUTE,
        ty: wgpu::BindingType::Buffer {
            ty: wgpu::BufferBindingType::Storage { read_only: ro },
            has_dynamic_offset: false,
            min_binding_size: None,
        },
        count: None,
    };
    let cull_bgl = gpu
        .device
        .create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("real-cull-bgl"),
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
                sb(1, true),
                sb(2, false),
                sb(3, false),
            ],
        });
    let draw_bgl = gpu
        .device
        .create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("real-draw-bgl"),
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
    let cull_pl = gpu
        .device
        .create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("real-cull-pl"),
            bind_group_layouts: &[Some(&cull_bgl)],
            immediate_size: 0,
        });
    let draw_pl = gpu
        .device
        .create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("real-draw-pl"),
            bind_group_layouts: &[Some(&draw_bgl)],
            immediate_size: 0,
        });
    let cull_pipe = gpu
        .device
        .create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
            label: Some("real-cull"),
            layout: Some(&cull_pl),
            module: &m_cull,
            entry_point: Some("cull"),
            compilation_options: Default::default(),
            cache: None,
        });

    let mk_draw = |vs: &str, fs: &str, label: &str| {
        gpu.device
            .create_render_pipeline(&wgpu::RenderPipelineDescriptor {
                label: Some(label),
                layout: Some(&draw_pl),
                vertex: wgpu::VertexState {
                    module: &m_draw,
                    entry_point: Some(vs),
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
                    entry_point: Some(fs),
                    compilation_options: Default::default(),
                    targets: &[Some(wgpu::ColorTargetState {
                        format: wgpu::TextureFormat::Rgba8Unorm,
                        blend: None,
                        write_mask: wgpu::ColorWrites::ALL,
                    })],
                }),
                multiview_mask: None,
                cache: None,
            })
    };
    let pipe_card = mk_draw("vs", "fs", "real-cards");
    let pipe_box = mk_draw("vs_box", "fs_box", "real-boxes");

    let cull_bg = gpu.device.create_bind_group(&wgpu::BindGroupDescriptor {
        label: Some("real-cull-bg"),
        layout: &cull_bgl,
        entries: &[
            wgpu::BindGroupEntry { binding: 0, resource: cull_ubo.as_entire_binding() },
            wgpu::BindGroupEntry { binding: 1, resource: src_buf.as_entire_binding() },
            wgpu::BindGroupEntry { binding: 2, resource: inst.as_entire_binding() },
            wgpu::BindGroupEntry { binding: 3, resource: args.as_entire_binding() },
        ],
    });
    let draw_bg = gpu.device.create_bind_group(&wgpu::BindGroupDescriptor {
        label: Some("real-draw-bg"),
        layout: &draw_bgl,
        entries: &[
            wgpu::BindGroupEntry { binding: 0, resource: draw_ubo.as_entire_binding() },
            wgpu::BindGroupEntry { binding: 1, resource: inst.as_entire_binding() },
        ],
    });

    let (color, color_view) = target(gpu, c.width, c.height, wgpu::TextureFormat::Rgba8Unorm);
    let depth = gpu.device.create_texture(&wgpu::TextureDescriptor {
        label: Some("real-depth"),
        size: wgpu::Extent3d { width: c.width, height: c.height, depth_or_array_layers: 1 },
        mip_level_count: 1,
        sample_count: 1,
        dimension: wgpu::TextureDimension::D2,
        format: wgpu::TextureFormat::Depth32Float,
        usage: wgpu::TextureUsages::RENDER_ATTACHMENT,
        view_formats: &[],
    });
    let depth_view = depth.create_view(&wgpu::TextureViewDescriptor::default());
    let args_init = buf(
        "args-init",
        20,
        wgpu::BufferUsages::COPY_SRC | wgpu::BufferUsages::COPY_DST,
    );

    // World centre and a ground height taken from the data itself.
    let cx = (p.min[0] + p.max[0]) * 0.5;
    let cz = (p.min[2] + p.max[2]) * 0.5;
    let ground = p.max[1] * 0.25;

    println!();
    println!("EXP-ASSET-002  real placement set, GPU-culled and compacted every frame");
    println!(
        "{:<11} {:>7} {:>7} {:>9} {:>10} {:>10} {:>10} {:>10}",
        "view", "far", "px", "geom", "visible", "triangles", "cull ms", "draw ms"
    );

    for &(vname, alt, pitch) in &[
        ("ground", 2.0f32, -1.0f32),
        ("hill", 300.0, -4.0),
        ("helicopter", 1000.0, -9.0),
        ("aircraft", 4000.0, -16.0),
    ] {
        for &far in &[2000.0f32, 5000.0, 12000.0] {
            for &px in &[0.0f32, 2.0] {
                for &(gname, boxes) in &[("card", false), ("prism", true)] {
                    let view = crate::camera::View {
                        eye: Vec3::new(cx, ground + alt, cz - 4000.0),
                        yaw: 0.0,
                        pitch: pitch.to_radians(),
                        fov_y: 55.0f32.to_radians(),
                    };
                    let b = view.basis();
                    let aspect = c.width as f32 / c.height as f32;
                    let vp = Mat4::perspective_infinite_reverse_rh(view.fov_y, aspect, 1.0)
                        * Mat4::look_to_rh(Vec3::ZERO, b.fwd, b.up);

                    gpu.queue.write_buffer(
                        &cull_ubo,
                        0,
                        bytemuck::bytes_of(&CullParams {
                            view_proj: vp.to_cols_array_2d(),
                            cam: [view.eye.x, view.eye.y, view.eye.z, 0.0],
                            cfg: [n as f32, far, px, c.height as f32 / (2.0 * view.tan_half_fov())],
                            classes: [12.0, 9.0, 3.0, 0.0],
                        }),
                    );
                    gpu.queue.write_buffer(
                        &draw_ubo,
                        0,
                        bytemuck::bytes_of(&DrawParams {
                            view_proj: vp.to_cols_array_2d(),
                            cam: [view.eye.x, view.eye.y, view.eye.z, 0.0],
                            fwd: [b.fwd.x, b.fwd.y, b.fwd.z, 0.0],
                            right: [b.right.x, b.right.y, b.right.z, 0.0],
                            up: [b.up.x, b.up.y, b.up.z, 0.0],
                            world: [0.0; 4],
                            cfg: [0.0; 4],
                            sun: [0.42, 0.72, 0.55, if boxes { 1.0 } else { 0.0 }],
                            cover_scale: [40.0, 60.0, 1.0, 0.0],
                        }),
                    );
                    gpu.queue.write_buffer(
                        &args_init,
                        0,
                        bytemuck::cast_slice(&[if boxes { 36u32 } else { 6u32 }, 0, 0, 0, 0]),
                    );

                    let t_cull = Timer::new(gpu);
                    let t_draw = Timer::new(gpu);
                    let mut cull_ms = Vec::new();
                    let mut draw_ms = Vec::new();
                    let mut visible = 0u32;

                    for _ in 0..c.frames {
                        let mut enc = gpu
                            .device
                            .create_command_encoder(&wgpu::CommandEncoderDescriptor { label: None });
                        enc.copy_buffer_to_buffer(&args_init, 0, &args, 0, 20);
                        {
                            let mut cp = enc.begin_compute_pass(&wgpu::ComputePassDescriptor {
                                label: Some("real-cull"),
                                timestamp_writes: t_cull.writes().map(|w| {
                                    wgpu::ComputePassTimestampWrites {
                                        query_set: w.query_set,
                                        beginning_of_pass_write_index: w
                                            .beginning_of_pass_write_index,
                                        end_of_pass_write_index: w.end_of_pass_write_index,
                                    }
                                }),
                            });
                            cp.set_pipeline(&cull_pipe);
                            cp.set_bind_group(0, &cull_bg, &[]);
                            cp.dispatch_workgroups(n.div_ceil(64), 1, 1);
                        }
                        {
                            let mut rp = enc.begin_render_pass(&wgpu::RenderPassDescriptor {
                                label: Some("real-draw"),
                                color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                                    view: &color_view,
                                    depth_slice: None,
                                    resolve_target: None,
                                    ops: wgpu::Operations {
                                        load: wgpu::LoadOp::Clear(wgpu::Color {
                                            r: 0.72, g: 0.80, b: 0.90, a: 1.0,
                                        }),
                                        store: wgpu::StoreOp::Store,
                                    },
                                })],
                                depth_stencil_attachment: Some(
                                    wgpu::RenderPassDepthStencilAttachment {
                                        view: &depth_view,
                                        depth_ops: Some(wgpu::Operations {
                                            load: wgpu::LoadOp::Clear(0.0),
                                            store: wgpu::StoreOp::Store,
                                        }),
                                        stencil_ops: None,
                                    },
                                ),
                                timestamp_writes: t_draw.writes(),
                                occlusion_query_set: None,
                                multiview_mask: None,
                            });
                            rp.set_pipeline(if boxes { &pipe_box } else { &pipe_card });
                            rp.set_bind_group(0, &draw_bg, &[]);
                            rp.draw_indirect(&args, 0);
                        }
                        t_cull.resolve_into(&mut enc);
                        t_draw.resolve_into(&mut enc);
                        gpu.queue.submit([enc.finish()]);
                        gpu.device.poll(wgpu::PollType::wait_indefinitely()).unwrap();
                        if let Some(v) = t_cull.read_ms(gpu) { cull_ms.push(v); }
                        if let Some(v) = t_draw.read_ms(gpu) { draw_ms.push(v); }
                        let a = gpu.read_buffer(&args, 20);
                        visible = u32::from_le_bytes(a[4..8].try_into().unwrap());
                    }

                    let tris = visible as u64 * if boxes { 12 } else { 2 };
                    println!(
                        "{:<11} {:>7.0} {:>7.1} {:>9} {:>10} {:>10} {:>10.3} {:>10.3}",
                        vname, far, px, gname, visible, tris,
                        Stat::of(cull_ms).min, Stat::of(draw_ms).min
                    );

                    if c.shots && far > 11000.0 && px > 0.0 && boxes {
                        let rgba = read_texture(gpu, &color, c.width, c.height, 4);
                        write_bmp(
                            &format!("{}/EXP-ASSET-002-{}.bmp", c.out, vname),
                            c.width, c.height, &rgba,
                        ).ok();
                    }
                }
            }
        }
    }
}
