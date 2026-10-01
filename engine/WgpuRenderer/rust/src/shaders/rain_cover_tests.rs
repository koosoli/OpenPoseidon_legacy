use naga_oil::compose::NagaModuleDescriptor;
use wgpu::util::DeviceExt;

// Import the real production helper. Synthetic directional depth maps isolate
// rain cover from diffuse light; this does not certify real native roof geometry.
const PROBE: &str = r#"
#import frame::{frame, interior_rain_reach, interior_sky_reach, interior_sky_ao}
@group(1) @binding(0) var<storage, read_write> result: array<vec4<f32>>;
@compute @workgroup_size(1)
fn cs_probe() {
    let rain = interior_rain_reach(frame.cam_pos.xyz);
    result[0] = vec4<f32>(rain, interior_sky_reach(frame.cam_pos.xyz),
        interior_sky_ao(frame.cam_pos.xyz, vec3<f32>(0.0, 1.0, 0.0)),
        smoothstep(0.6, 0.95, rain));
}
"#;

fn compose_probe() -> naga::Module {
    let mut composer = super::build_composer();
    composer.make_naga_module(NagaModuleDescriptor {
        source: PROBE,
        file_path: "rain_cover_probe.wgsl",
        ..Default::default()
    }).unwrap_or_else(|error| panic!("{}", error.emit_to_string(&composer)))
}

#[test]
fn real_rain_cover_helper_composes_with_its_actual_frame_bindings() {
    compose_probe();
}

#[test]
fn rain_cover_gpu_uses_zenith_not_diffuse_window_light() {
    // Absence of a test device is a failure, never a successful skipped proof.
    let (device, queue) = crate::gfx3d::cull::tests::headless()
        .expect("rain-cover GPU acceptance requires an available test device");
    let module = compose_probe();
    let frame_type = module.global_variables.iter().find_map(|(_, global)| {
        (global.binding.as_ref().is_some_and(|b| b.group == 0 && b.binding == 0))
            .then_some(global.ty)
    }).expect("composed helper must bind the real Frame UBO");
    let (members, span) = match &module.types[frame_type].inner {
        naga::TypeInner::Struct { members, span } => (members.clone(), *span as usize),
        _ => panic!("Frame must be a struct"),
    };
    let offset = |name: &str| members.iter().find(|m| m.name.as_deref() == Some(name))
        .unwrap_or_else(|| panic!("Frame missing {name}")).offset as usize;
    let mut bytes = vec![0_u8; span];
    let put = |bytes: &mut [u8], at: usize, values: &[f32]| {
        bytes[at..at+values.len()*4].copy_from_slice(bytemuck::cast_slice(values));
    };
    for i in 0..5 {
        put(&mut bytes, offset("skyvis_vp")+i*64, &glam::Mat4::IDENTITY.to_cols_array());
        put(&mut bytes, offset("skyvis_dir")+i*16, &crate::gfx3d::sky_vis::directions()[i].to_array());
    }
    put(&mut bytes, offset("skyvisb"), &[1.0/16.0, 0.0, 0.0, 0.0]);
    let uniform = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
        label: Some("rain_cover_actual_frame"), contents: &bytes,
        usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
    });
    let shader = device.create_shader_module(wgpu::ShaderModuleDescriptor {
        label: Some("rain_cover_actual_helper"),
        source: wgpu::ShaderSource::Naga(std::borrow::Cow::Owned(module)),
    });
    let pipeline = device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
        label: Some("rain_cover_probe"), layout: None, module: &shader,
        entry_point: Some("cs_probe"), compilation_options: Default::default(), cache: None,
    });
    let depth = device.create_texture(&wgpu::TextureDescriptor {
        label: Some("synthetic_directional_roofs"),
        size: wgpu::Extent3d { width: 8, height: 8, depth_or_array_layers: 5 },
        mip_level_count: 1, sample_count: 1, dimension: wgpu::TextureDimension::D2,
        format: wgpu::TextureFormat::Depth32Float,
        usage: wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::TEXTURE_BINDING,
        view_formats: &[],
    });
    let sample_view = depth.create_view(&wgpu::TextureViewDescriptor {
        dimension: Some(wgpu::TextureViewDimension::D2Array), ..Default::default()
    });
    let layer_views: Vec<_> = (0..5).map(|i| depth.create_view(&wgpu::TextureViewDescriptor {
        dimension: Some(wgpu::TextureViewDimension::D2), base_array_layer: i,
        array_layer_count: Some(1), ..Default::default()
    })).collect();
    let sampler = device.create_sampler(&wgpu::SamplerDescriptor {
        compare: Some(wgpu::CompareFunction::LessEqual),
        mag_filter: wgpu::FilterMode::Linear, min_filter: wgpu::FilterMode::Linear,
        ..Default::default()
    });
    let output = device.create_buffer(&wgpu::BufferDescriptor {
        label: Some("rain_cover_result"), size: 16,
        usage: wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_SRC, mapped_at_creation: false,
    });
    let staging = device.create_buffer(&wgpu::BufferDescriptor {
        label: Some("rain_cover_readback"), size: 16,
        usage: wgpu::BufferUsages::MAP_READ | wgpu::BufferUsages::COPY_DST, mapped_at_creation: false,
    });
    let bindings = device.create_bind_group(&wgpu::BindGroupDescriptor {
        label: None, layout: &pipeline.get_bind_group_layout(0), entries: &[
            wgpu::BindGroupEntry { binding: 0, resource: uniform.as_entire_binding() },
            wgpu::BindGroupEntry { binding: 2, resource: wgpu::BindingResource::Sampler(&sampler) },
            wgpu::BindGroupEntry { binding: 12, resource: wgpu::BindingResource::TextureView(&sample_view) },
            wgpu::BindGroupEntry { binding: 18, resource: wgpu::BindingResource::TextureView(&layer_views[0]) },
            wgpu::BindGroupEntry { binding: 20, resource: wgpu::BindingResource::TextureView(&layer_views[0]) },
        ],
    });
    let results = device.create_bind_group(&wgpu::BindGroupDescriptor {
        label: None, layout: &pipeline.get_bind_group_layout(1),
        entries: &[wgpu::BindGroupEntry { binding: 0, resource: output.as_entire_binding() }],
    });
    let mut probe = |depths: [f32; 5], position: [f32; 3], gate: f32, strength: f32, floor: f32| {
        put(&mut bytes, offset("cam_pos"), &[position[0], position[1], position[2], 0.0]);
        put(&mut bytes, offset("skyvis"), &[gate, 0.0, strength, floor]);
        queue.write_buffer(&uniform, 0, &bytes);
        let mut encoder = device.create_command_encoder(&Default::default());
        for i in 0..5 {
            let _pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
                label: Some("seed_directional_depth"), color_attachments: &[],
                depth_stencil_attachment: Some(wgpu::RenderPassDepthStencilAttachment {
                    view: &layer_views[i],
                    depth_ops: Some(wgpu::Operations { load: wgpu::LoadOp::Clear(depths[i]), store: wgpu::StoreOp::Store }),
                    stencil_ops: None,
                }), ..Default::default()
            });
        }
        {
            let mut pass = encoder.begin_compute_pass(&Default::default());
            pass.set_pipeline(&pipeline); pass.set_bind_group(0, &bindings, &[]);
            pass.set_bind_group(1, &results, &[]); pass.dispatch_workgroups(1,1,1);
        }
        encoder.copy_buffer_to_buffer(&output,0,&staging,0,16);
        queue.submit(Some(encoder.finish()));
        let slice = staging.slice(..);
        let (sender, receiver) = std::sync::mpsc::channel();
        slice.map_async(wgpu::MapMode::Read, move |result| { let _ = sender.send(result); });
        device.poll(wgpu::PollType::wait_indefinitely()).unwrap();
        receiver.recv().unwrap().unwrap();
        let data = slice.get_mapped_range();
        let values: [f32; 4] = bytemuck::cast_slice::<u8,f32>(&data).try_into().unwrap();
        drop(data); staging.unmap(); values
    };
    let centre = [0.0,0.0,0.75];
    for (strength,floor) in [(1.0,0.32),(0.0,0.0),(1.0,1.0)] {
        let canopy = probe([0.25,1.0,1.0,1.0,1.0],centre,1.0,strength,floor);
        assert!(canopy[0] < 0.001 && canopy[3] < 0.001, "covered zenith admitted rain: {canopy:?}");
        assert!(canopy[1] > 0.70 && canopy[1] < 0.74, "fixture must expose diffuse-roof bug: {canopy:?}");
        if strength == 0.0 || floor == 1.0 { assert!(canopy[2] > 0.999); }
    }
    let open_zenith = probe([1.0,0.25,0.25,0.25,0.25],centre,1.0,1.0,0.32);
    assert!(open_zenith[0] > 0.999 && open_zenith[3] > 0.999 && open_zenith[1] < 0.3,
            "tilted walls must not block open vertical rain: {open_zenith:?}");
    for (position,gate) in [(centre,0.0),([0.0,0.0,2.0],1.0),([1.0,0.0,0.75],1.0)] {
        let absent = probe([0.25;5],position,gate,1.0,0.32);
        assert!(absent[0] > 0.999, "documented absent/outside/edge open fallback changed: {absent:?}");
    }
}
