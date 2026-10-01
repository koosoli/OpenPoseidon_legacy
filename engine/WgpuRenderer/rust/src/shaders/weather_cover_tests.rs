use naga_oil::compose::NagaModuleDescriptor;
use wgpu::util::DeviceExt;

const PROBE: &str = r#"
#import frame::{frame, interior_rain_coverage, interior_rain_reach, weather_map_coverage}
#import shading::object_snow_coverage
@group(1) @binding(0) var<storage, read_write> result: array<vec4<f32>>;
@compute @workgroup_size(1)
fn cs_probe() {
    let up = vec3<f32>(0.0,1.0,0.0);
    result[0] = vec4<f32>(interior_rain_coverage(frame.cam_pos.xyz),
        interior_rain_reach(frame.cam_pos.xyz),
        object_snow_coverage(vec3<f32>(0.0),up,true,0.0),
        object_snow_coverage(vec3<f32>(0.0),-up,true,0.0));
}
"#;

// Actual rasterised depth, with a half-map overhang and an alpha-like geometric
// opening. No pre-filled result buffer or CPU imitation of production exposure.
const ROOF: &str = r#"
struct Out { @builtin(position) pos: vec4<f32>, @location(0) uv: vec2<f32> };
@vertex fn vs(@builtin(vertex_index) id:u32) -> Out {
    let p = array<vec2<f32>,6>(vec2<f32>(-1.0,-1.0),vec2<f32>(0.0,-1.0),vec2<f32>(0.0,1.0),
        vec2<f32>(-1.0,-1.0),vec2<f32>(0.0,1.0),vec2<f32>(-1.0,1.0));
    var out:Out; out.pos=vec4<f32>(p[id],0.25,1.0); out.uv=p[id]; return out;
}
@fragment fn fs(in:Out) { if (abs(in.uv.y)<0.15) {discard;} }
"#;

#[test]
fn weather_depth_consumers_compose_and_keep_ordinary_override_off() {
    assert_eq!(
        crate::gfx3d::cull::CULL_VARIANT_COUNT,
        2,
        "physical Frame counter lane and overflow guard consume exactly two variants"
    );
    for (source, path) in [
        (
            include_str!("../gfx3d/shader3d.wgsl"),
            "gfx3d/shader3d.wgsl",
        ),
        (
            include_str!("../gfx3d/gpu_driven.wgsl"),
            "gfx3d/gpu_driven.wgsl",
        ),
    ] {
        let mut composer = super::build_composer();
        composer
            .make_naga_module(NagaModuleDescriptor {
                source,
                file_path: path,
                ..Default::default()
            })
            .unwrap_or_else(|e| panic!("{}", e.emit_to_string(&composer)));
        assert!(source.contains("override weather_depth: f32 = 0.0;"));
        assert!(source.contains("if (weather_depth > 0.5)"));
        assert!(source.contains("veg_sway_offset("));
        assert!(source.contains("frame.weather_vp * vec4<f32>"));
    }
    // The appended private lanes must occupy exactly 192 bytes. This uses the
    // actual composed Frame type rather than a duplicate Rust test struct.
    let mut composer = super::build_composer();
    let module = composer
        .make_naga_module(NagaModuleDescriptor {
            source: PROBE,
            file_path: "weather_cover_layout_probe.wgsl",
            ..Default::default()
        })
        .unwrap_or_else(|e| panic!("{}", e.emit_to_string(&composer)));
    let ty = module
        .global_variables
        .iter()
        .find_map(|(_, g)| {
            g.binding
                .as_ref()
                .is_some_and(|b| b.group == 0 && b.binding == 0)
                .then_some(g.ty)
        })
        .unwrap();
    let naga::TypeInner::Struct { members, span } = &module.types[ty].inner else {
        panic!()
    };
    let at = |name: &str| {
        members
            .iter()
            .find(|m| m.name.as_deref() == Some(name))
            .unwrap()
            .offset
    };
    assert_eq!(at("weather_vp"), at("ground_weather") + 16);
    assert_eq!(at("weather_cover"), at("weather_vp") + 64);
    assert_eq!(at("weather_gpu"), at("weather_cover") + 16);
    assert_eq!(at("weather_far_vp"), at("weather_gpu") + 16);
    assert_eq!(at("weather_far_cover"), at("weather_far_vp") + 64);
    assert_eq!(at("weather_far_gpu"), at("weather_far_cover") + 16);
    assert_eq!(*span, at("weather_vp") + 192);
}

#[test]
fn weather_cover_actual_gpu_far_roof_overhang_hole_underside_and_unknown() {
    let (device, queue) = crate::gfx3d::cull::tests::headless()
        .expect("physical-weather acceptance requires an actual GPU device");
    let mut composer = super::build_composer();
    let module = composer
        .make_naga_module(NagaModuleDescriptor {
            source: PROBE,
            file_path: "weather_cover_probe.wgsl",
            ..Default::default()
        })
        .unwrap_or_else(|e| panic!("{}", e.emit_to_string(&composer)));
    let frame_type = module
        .global_variables
        .iter()
        .find_map(|(_, g)| {
            g.binding
                .as_ref()
                .is_some_and(|b| b.group == 0 && b.binding == 0)
                .then_some(g.ty)
        })
        .unwrap();
    let (members, span) = match &module.types[frame_type].inner {
        naga::TypeInner::Struct { members, span } => (members.clone(), *span as usize),
        _ => panic!(),
    };
    let offset = |name: &str| {
        members
            .iter()
            .find(|m| m.name.as_deref() == Some(name))
            .unwrap()
            .offset as usize
    };
    let put = |bytes: &mut [u8], at: usize, values: &[f32]| {
        bytes[at..at + values.len() * 4].copy_from_slice(bytemuck::cast_slice(values))
    };
    let mut bytes = vec![0_u8; span];
    // Main map covers world ±640; the probe may be another camera inside it.
    let vp = glam::Mat4::from_diagonal(glam::Vec4::new(1.0 / 640.0, 1.0 / 640.0, 1.0, 1.0));
    put(&mut bytes, offset("weather_vp"), &vp.to_cols_array());
    put(&mut bytes, offset("snow_surface"), &[0.18, -1.0, 0.0, 0.0]);
    for i in 0..5 {
        put(
            &mut bytes,
            offset("skyvis_vp") + i * 64,
            &glam::Mat4::IDENTITY.to_cols_array(),
        );
        put(
            &mut bytes,
            offset("skyvis_dir") + i * 16,
            &crate::gfx3d::sky_vis::directions()[i].to_array(),
        );
    }
    put(&mut bytes, offset("skyvisb"), &[0.0, 0.0, 0.0, 0.0]);
    let uniform = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
        label: None,
        contents: &bytes,
        usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
    });
    // Same encoder copy as production: actual GPU counter bytes enter a uniform,
    // not a duplicate CPU exposure algorithm or extra fragment storage binding.
    let counters = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
        label: Some("weather_cull_counter_fixture"),
        contents: bytemuck::cast_slice(&[0_u32; 3]),
        usage: wgpu::BufferUsages::COPY_SRC | wgpu::BufferUsages::COPY_DST,
    });
    let outer_counters = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
        label: Some("weather_far_cull_counter_fixture"),
        contents: bytemuck::cast_slice(&[0_u32; 3]),
        usage: wgpu::BufferUsages::COPY_SRC | wgpu::BufferUsages::COPY_DST,
    });
    let shader = device.create_shader_module(wgpu::ShaderModuleDescriptor {
        label: None,
        source: wgpu::ShaderSource::Naga(std::borrow::Cow::Owned(module)),
    });
    let pipeline = device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
        label: None,
        layout: None,
        module: &shader,
        entry_point: Some("cs_probe"),
        compilation_options: Default::default(),
        cache: None,
    });
    let texture = |layers| {
        device.create_texture(&wgpu::TextureDescriptor {
            label: None,
            size: wgpu::Extent3d {
                width: 32,
                height: 32,
                depth_or_array_layers: layers,
            },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: wgpu::TextureFormat::Depth32Float,
            usage: wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::TEXTURE_BINDING,
            view_formats: &[],
        })
    };
    let far = texture(1);
    let far_view = far.create_view(&Default::default());
    let near = texture(5);
    let near_view = near.create_view(&wgpu::TextureViewDescriptor {
        dimension: Some(wgpu::TextureViewDimension::D2Array),
        ..Default::default()
    });
    let near_layer = near.create_view(&wgpu::TextureViewDescriptor {
        dimension: Some(wgpu::TextureViewDimension::D2),
        array_layer_count: Some(1),
        ..Default::default()
    });
    let sampler = device.create_sampler(&wgpu::SamplerDescriptor {
        compare: Some(wgpu::CompareFunction::LessEqual),
        mag_filter: wgpu::FilterMode::Linear,
        min_filter: wgpu::FilterMode::Linear,
        ..Default::default()
    });
    let result = device.create_buffer(&wgpu::BufferDescriptor {
        label: None,
        size: 16,
        usage: wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_SRC,
        mapped_at_creation: false,
    });
    let readback = device.create_buffer(&wgpu::BufferDescriptor {
        label: None,
        size: 16,
        usage: wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::MAP_READ,
        mapped_at_creation: false,
    });
    let camera = device.create_bind_group(&wgpu::BindGroupDescriptor {
        label: None,
        layout: &pipeline.get_bind_group_layout(0),
        entries: &[
            wgpu::BindGroupEntry {
                binding: 0,
                resource: uniform.as_entire_binding(),
            },
            wgpu::BindGroupEntry {
                binding: 2,
                resource: wgpu::BindingResource::Sampler(&sampler),
            },
            wgpu::BindGroupEntry {
                binding: 12,
                resource: wgpu::BindingResource::TextureView(&near_view),
            },
            wgpu::BindGroupEntry {
                binding: 18,
                resource: wgpu::BindingResource::TextureView(&far_view),
            },
            wgpu::BindGroupEntry {
                binding: 20,
                resource: wgpu::BindingResource::TextureView(&far_view),
            },
        ],
    });
    let output = device.create_bind_group(&wgpu::BindGroupDescriptor {
        label: None,
        layout: &pipeline.get_bind_group_layout(1),
        entries: &[wgpu::BindGroupEntry {
            binding: 0,
            resource: result.as_entire_binding(),
        }],
    });
    let roof = device.create_shader_module(wgpu::ShaderModuleDescriptor {
        label: None,
        source: wgpu::ShaderSource::Wgsl(ROOF.into()),
    });
    let roof_pipeline = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
        label: None,
        layout: None,
        vertex: wgpu::VertexState {
            module: &roof,
            entry_point: Some("vs"),
            buffers: &[],
            compilation_options: Default::default(),
        },
        fragment: Some(wgpu::FragmentState {
            module: &roof,
            entry_point: Some("fs"),
            targets: &[],
            compilation_options: Default::default(),
        }),
        primitive: Default::default(),
        depth_stencil: Some(wgpu::DepthStencilState {
            format: wgpu::TextureFormat::Depth32Float,
            depth_write_enabled: Some(true),
            depth_compare: Some(wgpu::CompareFunction::LessEqual),
            stencil: Default::default(),
            bias: Default::default(),
        }),
        multisample: Default::default(),
        multiview_mask: None,
        cache: None,
    });
    let outer_ready = std::cell::Cell::new(0.0_f32);
    let outer_counts = std::cell::Cell::new([0_u32;4]);
    let mut run = |position: [f32; 3], ready: f32, near_gate: f32, actual_counts: [u32; 3]| {
        put(
            &mut bytes,
            offset("cam_pos"),
            &[position[0], position[1], position[2], 0.0],
        );
        put(
            &mut bytes,
            offset("weather_cover"),
            &[ready, 0.08 / 2048.0, 1.5 / 32.0, 1.0],
        );
        put(&mut bytes, offset("weather_far_vp"),
            &glam::Mat4::from_diagonal(glam::Vec4::new(1.0/1600.0,1.0/1600.0,1.0,1.0)).to_cols_array());
        put(&mut bytes, offset("weather_far_cover"), &[outer_ready.get(),0.08/2048.0,1.5/32.0,1.0]);

        put(&mut bytes, offset("skyvis"), &[near_gate, 0.0, 0.0, 1.0]); // artistic strength0/floor1
        let near_vp =
            glam::Mat4::from_diagonal(glam::Vec4::new(0.98 / 500.0, 1.0 / 640.0, 1.0, 1.0));
        put(&mut bytes, offset("skyvis_vp"), &near_vp.to_cols_array());
        queue.write_buffer(&uniform, 0, &bytes);
        queue.write_buffer(
            &uniform,
            offset("weather_gpu") as u64 + 12,
            bytemuck::bytes_of(&8_u32),
        );
        queue.write_buffer(&counters, 0, bytemuck::cast_slice(&actual_counts));
        queue.write_buffer(&outer_counters,0,bytemuck::cast_slice(&outer_counts.get()[..3]));
        queue.write_buffer(&uniform,offset("weather_far_gpu") as u64+12,
            bytemuck::bytes_of(&outer_counts.get()[3]));
        let mut encoder = device.create_command_encoder(&Default::default());
        {
            let mut pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
                label: None,
                color_attachments: &[],
                depth_stencil_attachment: Some(wgpu::RenderPassDepthStencilAttachment {
                    view: &near_layer,
                    depth_ops: Some(wgpu::Operations {
                        load: wgpu::LoadOp::Clear(1.0),
                        store: wgpu::StoreOp::Store,
                    }),
                    stencil_ops: None,
                }),
                ..Default::default()
            });
            pass.set_pipeline(&roof_pipeline);
            pass.draw(0..6, 0..1);
        }
        {
            let mut pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
                label: None,
                color_attachments: &[],
                depth_stencil_attachment: Some(wgpu::RenderPassDepthStencilAttachment {
                    view: &far_view,
                    depth_ops: Some(wgpu::Operations {
                        load: wgpu::LoadOp::Clear(1.0),
                        store: wgpu::StoreOp::Store,
                    }),
                    stencil_ops: None,
                }),
                ..Default::default()
            });
            pass.set_pipeline(&roof_pipeline);
            pass.draw(0..6, 0..1);
        }
        encoder.copy_buffer_to_buffer(&counters, 0, &uniform, offset("weather_gpu") as u64, 12);
        encoder.copy_buffer_to_buffer(&outer_counters,0,&uniform,offset("weather_far_gpu") as u64,12);
        {
            let mut pass = encoder.begin_compute_pass(&Default::default());
            pass.set_pipeline(&pipeline);
            pass.set_bind_group(0, &camera, &[]);
            pass.set_bind_group(1, &output, &[]);
            pass.dispatch_workgroups(1, 1, 1);
        }
        encoder.copy_buffer_to_buffer(&result, 0, &readback, 0, 16);
        queue.submit([encoder.finish()]);
        let slice = readback.slice(..);
        let (send, recv) = std::sync::mpsc::channel();
        slice.map_async(wgpu::MapMode::Read, move |r| {
            let _ = send.send(r);
        });
        device.poll(wgpu::PollType::wait_indefinitely()).unwrap();
        recv.recv().unwrap().unwrap();
        let data = slice.get_mapped_range();
        let values: [f32; 4] = bytemuck::cast_slice(&data).try_into().unwrap();
        drop(data);
        readback.unmap();
        values
    };
    for distance in [200.0, 500.0] {
        let top = run([-distance, 400.0, 0.25], 1.0, 0.0, [8, 8, 16]);
        assert_eq!(
            top,
            [1.0, 1.0, 1.0, 0.0],
            "actual rasterised distant top with AO disabled: {top:?}"
        );
        let under = run([-distance, 400.0, 0.2501], 1.0, 0.0, [8, 8, 16]);
        assert_eq!(
            under,
            [1.0, 0.0, 0.0, 0.0],
            "20cm below thin roof must stay covered: {under:?}"
        );
        let open = run([distance, 400.0, 0.75], 1.0, 0.0, [0, 0, 0]);
        assert_eq!(
            open,
            [1.0, 1.0, 1.0, 0.0],
            "outside partial overhang stays exposed: {open:?}"
        );
        let hole = run([-distance, 0.0, 0.75], 1.0, 0.0, [0, 0, 0]);
        assert_eq!(
            hole,
            [1.0, 1.0, 1.0, 0.0],
            "raster discard hole survives: {hole:?}"
        );
        let pending = run([-distance, 400.0, 0.25], 0.0, 0.0, [0, 0, 0]);
        assert_eq!(
            pending,
            [0.0, 0.0, 0.0, 0.0],
            "requested/missing map is not open proof: {pending:?}"
        );
    }
    for overflow in [[9, 0, 0], [0, 9, 0], [0, 0, 17]] {
        assert_eq!(
            run([-500.0, 400.0, 0.25], 1.0, 0.0, overflow),
            [0.0, 0.0, 0.0, 0.0],
            "GPU cull capacity overflow must fail closed: {overflow:?}"
        );
    }
    let feather = run([-500.0, 400.0, 0.75], 1.0, 1.0, [0, 0, 0]);
    assert!(
        feather[0] == 1.0 && feather[1].abs() < 1e-6 && feather[2] == 0.0 && feather[3] == 0.0,
        "near OPEN feather must not override physically blocked far roof: {feather:?}"
    );
    assert_eq!(
        run([639.0, 0.0, 0.75], 1.0, 0.0, [0, 0, 0]),
        [0.0, 0.0, 0.0, 0.0],
        "map edge unknown stays closed"
    );
    assert_eq!(
        run([0.0, 0.0, 1.1], 1.0, 0.0, [0, 0, 0]),
        [0.0, 0.0, 0.0, 0.0],
        "vertical bounds unknown stays closed"
    );
    // Use the actual rasterised roof on binding20. The far VP changes which
    // world points sample it; no synthetic open result or CPU reach imitation.
    outer_ready.set(1.0); outer_counts.set([8,8,16,8]);
    for distance in [800.0,1200.0] {
        assert_eq!(run([-distance,400.0,0.25],1.0,0.0,[0,0,0]), [1.0,1.0,1.0,0.0]);
        assert_eq!(run([-distance,400.0,0.2501],1.0,0.0,[0,0,0]), [1.0,0.0,0.0,0.0]);
        assert_eq!(run([distance,400.0,0.75],1.0,0.0,[0,0,0]), [1.0,1.0,1.0,0.0]);
        assert_eq!(run([-distance,0.0,0.75],1.0,0.0,[0,0,0]), [1.0,1.0,1.0,0.0]);
    }
    assert_eq!(run([-500.0,400.0,0.25],0.0,0.0,[0,0,0]),[0.0;4],
        "pending near must never borrow ready coarser far coverage");
    for overflow in [[9,0,0,8],[0,9,0,8],[0,0,17,8]] {
        outer_counts.set(overflow);
        assert_eq!(run([-800.0,400.0,0.25],1.0,0.0,[0,0,0]),[0.0;4],
            "far's own GPU counter overflow fails closed");
    }
    outer_counts.set([0,0,0,8]); outer_ready.set(0.0);
    assert_eq!(run([-800.0,400.0,0.25],1.0,0.0,[0,0,0]),[0.0;4],"far pending fails closed");
    outer_ready.set(1.0);
    assert_eq!(run([1599.0,0.0,0.75],1.0,0.0,[0,0,0]),[0.0;4],"far border closed");
    assert_eq!(run([1800.0,0.0,0.75],1.0,0.0,[0,0,0]),[0.0;4],"far outside closed");

}
