use naga_oil::compose::NagaModuleDescriptor;
use wgpu::util::DeviceExt;

// Both queries execute the actual shared helpers. Fixture-only Frame light
// lanes describe a real world-space roof and the receiver normal; production
// Frame layout/producer matrices are otherwise used unchanged.
const PROBE: &str = r#"
#import frame::{frame, snow_receiver_reach, interior_rain_coverage, interior_rain_reach}
#import shading::object_snow_coverage
@group(1) @binding(0) var<storage, read_write> result: array<vec4<f32>>;
@compute @workgroup_size(1) fn cs_probe() {
    let n = frame.sun_dir_world.xyz;
    result[0] = vec4<f32>(snow_receiver_reach(frame.cam_pos.xyz,n),
        object_snow_coverage(vec3<f32>(0.0),n,true,0.0),
        object_snow_coverage(vec3<f32>(0.0),vec3<f32>(1.0,0.0,0.0),true,0.0),
        object_snow_coverage(vec3<f32>(0.0),vec3<f32>(0.0,-1.0,0.0),true,0.0));
    result[1] = vec4<f32>(interior_rain_reach(frame.cam_pos.xyz),
        interior_rain_coverage(frame.cam_pos.xyz),
        snow_receiver_reach(frame.cam_pos.xyz,vec3<f32>(bitcast<f32>(0x7fc00000u))),
        object_snow_coverage(vec3<f32>(0.0),vec3<f32>(bitcast<f32>(0x7f800000u)),true,1.0));
}
"#;

// An actual rasterised inclined opaque roof. The optional opening is fragment
// discard, not a seeded depth or CPU approximation of the exposure algorithm.
const ROOF: &str = r#"
#import frame::frame
struct Out { @builtin(position) pos: vec4<f32>, @location(0) local: vec2<f32> };
@vertex fn vs(@builtin(vertex_index) id:u32) -> Out {
    let xy=array<vec2<f32>,6>(vec2<f32>(-4.0,-4.0),vec2<f32>(4.0,-4.0),vec2<f32>(4.0,4.0),
        vec2<f32>(-4.0,-4.0),vec2<f32>(4.0,4.0),vec2<f32>(-4.0,4.0));
    let p=xy[id];
    let world=frame.sun_ambient.xyz+vec3<f32>(p.x,dot(p,frame.sun_diffuse.xy),p.y);
    var out:Out; out.local=p;
    if (frame.sun_ambient.w>0.5) { out.pos=frame.weather_vp*vec4<f32>(world,1.0); }
    else { out.pos=frame.skyvis_vp[0]*vec4<f32>(world,1.0); }
    return out;
}
@fragment fn fs(in:Out) {
    if (frame.sun_diffuse.w>0.5 && all(abs(in.local)<vec2<f32>(1.0))) { discard; }
}
"#;

fn compose(source: &str) -> naga::Module {
    let mut composer = super::build_composer();
    composer
        .make_naga_module(NagaModuleDescriptor {
            source,
            file_path: "snow_receiver_plane_fixture.wgsl",
            ..Default::default()
        })
        .unwrap_or_else(|e| panic!("{}", e.emit_to_string(&composer)))
}

#[test]
fn snow_receiver_actual_plane_helpers_compose_and_ground_query_stays_separate() {
    compose(PROBE);
    compose(ROOF);
    let frame = include_str!("frame.wgsl");
    let ground = frame
        .split("fn interior_rain_reach(")
        .nth(1)
        .unwrap()
        .split("// Snow asks")
        .next()
        .unwrap();
    assert!(!ground.contains("snow_receiver"));
    assert!(ground.contains("interior_sky_reach_dir(world_abs, vec3<f32>(0.0, 1.0, 0.0), 0)"));
    let material = include_str!("shading.wgsl");
    let coating = material
        .split("fn object_snow_coverage(")
        .nth(1)
        .unwrap()
        .split("fn object_snow_normal")
        .next()
        .unwrap();
    assert!(coating.contains("snow_receiver_reach(world_abs, geo_normal)"));
    assert!(!coating.contains("interior_rain_reach(world_abs)"));
}

#[test]
fn snow_receiver_actual_gpu_slopes_subtexels_handoff_and_thin_roof_rejections() {
    let (device, queue) = crate::gfx3d::cull::tests::headless()
        .expect("snow receiver-plane acceptance requires an actual GPU device");
    let module = compose(PROBE);
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
            .offset as usize
    };
    let put = |bytes: &mut [u8], name: &str, v: &[f32]| {
        let offset = at(name);
        bytes[offset..offset + v.len() * 4].copy_from_slice(bytemuck::cast_slice(v));
    };
    let mut bytes = vec![0u8; *span as usize];
    let uniform = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
        label: Some("snow_plane_actual_frame"),
        contents: &bytes,
        usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
    });
    let shader = device.create_shader_module(wgpu::ShaderModuleDescriptor {
        label: None,
        source: wgpu::ShaderSource::Naga(std::borrow::Cow::Owned(module.clone())),
    });
    let pipeline = device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
        label: None,
        layout: None,
        module: &shader,
        entry_point: Some("cs_probe"),
        compilation_options: Default::default(),
        cache: None,
    });
    let roof = device.create_shader_module(wgpu::ShaderModuleDescriptor {
        label: None,
        source: wgpu::ShaderSource::Naga(std::borrow::Cow::Owned(compose(ROOF))),
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
    let texture = |layers| {
        device.create_texture(&wgpu::TextureDescriptor {
            label: None,
            size: wgpu::Extent3d {
                width: 2048,
                height: 2048,
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
    let near = texture(5);
    let far = texture(1);
    let near_array = near.create_view(&wgpu::TextureViewDescriptor {
        dimension: Some(wgpu::TextureViewDimension::D2Array),
        ..Default::default()
    });
    let near_depth = near.create_view(&wgpu::TextureViewDescriptor {
        dimension: Some(wgpu::TextureViewDimension::D2),
        array_layer_count: Some(1),
        ..Default::default()
    });
    let far_depth = far.create_view(&Default::default());
    let sampler = device.create_sampler(&wgpu::SamplerDescriptor {
        compare: Some(wgpu::CompareFunction::LessEqual),
        mag_filter: wgpu::FilterMode::Linear,
        min_filter: wgpu::FilterMode::Linear,
        ..Default::default()
    });
    let result = device.create_buffer(&wgpu::BufferDescriptor {
        label: None,
        size: 32,
        usage: wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_SRC,
        mapped_at_creation: false,
    });
    let readback = device.create_buffer(&wgpu::BufferDescriptor {
        label: None,
        size: 32,
        usage: wgpu::BufferUsages::MAP_READ | wgpu::BufferUsages::COPY_DST,
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
                resource: wgpu::BindingResource::TextureView(&near_array),
            },
            wgpu::BindGroupEntry {
                binding: 18,
                resource: wgpu::BindingResource::TextureView(&far_depth),
            },
            wgpu::BindGroupEntry {
                binding: 20,
                resource: wgpu::BindingResource::TextureView(&far_depth),
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
    // Separate uniform for the depth draws: queue writes must not change the
    // matrix selector of a previously recorded draw before submission.
    let near_uniform = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
        label: None,
        contents: &bytes,
        usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
    });
    let far_uniform = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
        label: None,
        contents: &bytes,
        usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
    });
    let roof_camera = |buffer: &wgpu::Buffer| {
        device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: None,
            layout: &roof_pipeline.get_bind_group_layout(0),
            entries: &[wgpu::BindGroupEntry {
                binding: 0,
                resource: buffer.as_entire_binding(),
            }],
        })
    };
    let near_camera = roof_camera(&near_uniform);
    let far_camera = roof_camera(&far_uniform);
    let far_settings = crate::gfx3d::sky_vis::SkyVisSettings {
        extent: 640.0,
        height: 1024.0,
        ..Default::default()
    };
    let near_settings = crate::gfx3d::sky_vis::SkyVisSettings::default();
    let mut run = |distance: f32,
                   slope: [f32; 2],
                   subtexel: f32,
                   near_distance: f32,
                   below: f32,
                   flat_normal: bool,
                   near_on: bool,
                   ready: bool,
                   hole: bool,
                   overflow: bool| {
        let centre = glam::Vec3::new(distance, 100.0, 20.0);
        let p = centre + glam::Vec3::new(subtexel, slope[0] * subtexel - below, subtexel * 0.37);
        // Include BOTH slope components in the point's actual roof height.
        let p = glam::Vec3::new(p.x, p.y + slope[1] * subtexel * 0.37, p.z);
        let n = if flat_normal {
            glam::Vec3::Y
        } else {
            glam::Vec3::new(-slope[0], 1.0, -slope[1]).normalize()
        };
        let fv = crate::gfx3d::sky_vis::build_view_for(
            glam::Vec3::new(subtexel, 100.0, subtexel),
            glam::Vec3::Y,
            &far_settings,
        );
        let nv = crate::gfx3d::sky_vis::build_view_for(
            centre + glam::Vec3::new(near_distance + subtexel, 0.0, subtexel),
            glam::Vec3::Y,
            &near_settings,
        );
        put(&mut bytes, "weather_vp", &fv.view_proj.to_cols_array());
        put(&mut bytes, "skyvis_vp", &nv.view_proj.to_cols_array());
        put(&mut bytes, "skyvis_dir", &[0.0, 1.0, 0.0, 1.0]);
        put(
            &mut bytes,
            "weather_cover",
            &[
                if ready { 1.0 } else { 0.0 },
                0.08 / 2048.0,
                1.5 / 2048.0,
                1.0,
            ],
        );
        bytes[at("weather_gpu")..at("weather_gpu") + 16].copy_from_slice(bytemuck::cast_slice(
            &if overflow {
                [9u32, 0, 0, 8]
            } else {
                [0u32, 0, 0, 8]
            },
        ));
        put(
            &mut bytes,
            "skyvis",
            &[if near_on { 1.0 } else { 0.0 }, 0.0, 1.0, 0.32],
        );
        put(
            &mut bytes,
            "skyvisb",
            &[nv.kernel_uv, nv.bias_ndc, 0.0, 0.0],
        );
        put(&mut bytes, "snow_surface", &[0.18, -1.0, 0.0, 0.0]);
        put(&mut bytes, "cam_pos", &p.extend(0.0).to_array());
        put(&mut bytes, "sun_dir_world", &n.extend(0.0).to_array());
        put(
            &mut bytes,
            "sun_diffuse",
            &[slope[0], slope[1], 0.0, if hole { 1.0 } else { 0.0 }],
        );
        put(&mut bytes, "sun_ambient", &centre.extend(0.0).to_array());
        queue.write_buffer(&near_uniform, 0, &bytes);
        put(&mut bytes, "sun_ambient", &centre.extend(1.0).to_array());
        queue.write_buffer(&far_uniform, 0, &bytes);
        queue.write_buffer(&uniform, 0, &bytes);
        let mut encoder = device.create_command_encoder(&Default::default());
        for (view, binding) in [(&near_depth, &near_camera), (&far_depth, &far_camera)] {
            let mut pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
                label: None,
                color_attachments: &[],
                depth_stencil_attachment: Some(wgpu::RenderPassDepthStencilAttachment {
                    view,
                    depth_ops: Some(wgpu::Operations {
                        load: wgpu::LoadOp::Clear(1.0),
                        store: wgpu::StoreOp::Store,
                    }),
                    stencil_ops: None,
                }),
                ..Default::default()
            });
            pass.set_pipeline(&roof_pipeline);
            pass.set_bind_group(0, binding, &[]);
            pass.draw(0..6, 0..1);
        }
        {
            let mut pass = encoder.begin_compute_pass(&Default::default());
            pass.set_pipeline(&pipeline);
            pass.set_bind_group(0, &camera, &[]);
            pass.set_bind_group(1, &output, &[]);
            pass.dispatch_workgroups(1, 1, 1);
        }
        encoder.copy_buffer_to_buffer(&result, 0, &readback, 0, 32);
        queue.submit([encoder.finish()]);
        let slice = readback.slice(..);
        let (send, recv) = std::sync::mpsc::channel();
        slice.map_async(wgpu::MapMode::Read, move |r| {
            let _ = send.send(r);
        });
        device.poll(wgpu::PollType::wait_indefinitely()).unwrap();
        recv.recv().unwrap().unwrap();
        let data = slice.get_mapped_range();
        let values: [f32; 8] = bytemuck::cast_slice(&data).try_into().unwrap();
        drop(data);
        readback.unmap();
        assert_eq!(
            &values[6..8],
            &[0.0, 0.0],
            "NaN/Inf normals must close admission"
        );
        values
    };
    for distance in [200.0, 500.0] {
        for slope in [[0.0, 0.0], [1.0, 0.0], [1.15, 0.25], [-1.2, -0.2]] {
            for offset in [-0.29, -0.1, 0.0, 0.16, 0.31, 0.64] {
                for (near_distance, near_on) in [
                    (0.0, true),
                    (58.0, true),
                    (61.0, true),
                    (64.0, true),
                    (0.0, false),
                ] {
                    let top = run(
                        distance,
                        slope,
                        offset,
                        near_distance,
                        0.0,
                        false,
                        near_on,
                        true,
                        false,
                        false,
                    );
                    assert!((top[0]-1.0).abs()<1e-5,"self roof reach: distance={distance} slope={slope:?} offset={offset} near={near_distance}/{near_on} actual={top:?}");
                    let up = 1.0 / (1.0 + slope[0] * slope[0] + slope[1] * slope[1]).sqrt();
                    let t = ((up - 0.35) / 0.35).clamp(0.0, 1.0);
                    let expected = t * t * (3.0 - 2.0 * t);
                    assert!((top[1]-expected).abs()<1e-4,"only existing slope material policy may reduce coat: {top:?} expected={expected}");
                    assert_eq!(&top[2..4], &[0.0, 0.0], "wall/underside never coat");
                    let smooth = run(
                        distance,
                        slope,
                        offset,
                        near_distance,
                        0.0,
                        true,
                        near_on,
                        true,
                        false,
                        false,
                    );
                    assert!((smooth[0]-1.0).abs()<1e-5 && (smooth[1]-1.0).abs()<1e-5,
                        "actual occupied depth plane must beat a smooth/mismatched authored normal: {smooth:?}");
                    for flat in [false, true] {
                        let under = run(
                            distance,
                            slope,
                            offset,
                            near_distance,
                            0.2,
                            flat,
                            near_on,
                            true,
                            false,
                            false,
                        );
                        assert_eq!(&under[..4],&[0.0;4],"20cm below roof: slope={slope:?} offset={offset} near={near_distance}/{near_on} flat={flat} actual={under:?}");
                    }
                }
            }
        }
    }
    let old = run(
        200.0,
        [1.15, 0.25],
        0.0,
        0.0,
        0.0,
        false,
        true,
        true,
        false,
        false,
    );
    assert!(
        old[4] < 0.8 && old[0] > 0.99,
        "fixture must reproduce legacy artistic self-block while snow query repairs it: {old:?}"
    );
    for (ready, overflow) in [(false, false), (true, true)] {
        let absent = run(
            500.0,
            [1.0, 0.0],
            0.0,
            0.0,
            0.0,
            false,
            false,
            ready,
            false,
            overflow,
        );
        assert_eq!(
            &absent[..4],
            &[0.0; 4],
            "unready/overflow map stays closed: {absent:?}"
        );
    }
    let hole = run(
        500.0,
        [1.0, 0.0],
        0.0,
        0.0,
        2.0,
        true,
        false,
        true,
        true,
        false,
    );
    assert!(
        hole[0] > 0.99 && hole[1] > 0.99,
        "real discarded centre hole must remain open: {hole:?}"
    );
}
