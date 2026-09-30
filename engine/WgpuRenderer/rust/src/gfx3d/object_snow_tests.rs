use naga_oil::compose::NagaModuleDescriptor;
use wgpu::util::DeviceExt;

// Execute the ACTUAL shared coating and normal helpers against synthetic zenith maps.
// This falsifies rain/side/fallback policy; it does not certify loaded roof geometry.
const PROBE: &str = r#"
#import frame::{frame, interior_rain_coverage}
#import shading::{object_snow_coverage, object_snow_normal, object_snow_baked_exposure}
@group(1) @binding(0) var<storage, read_write> result: array<vec4<f32>>;
@compute @workgroup_size(1)
fn cs_probe() {
    let p = vec3<f32>(0.0);
    let up = vec3<f32>(0.0, 1.0, 0.0);
    result[0] = vec4<f32>(object_snow_coverage(p, up, true, 0.0),
        object_snow_coverage(p, up, false, 1.0),
        object_snow_coverage(p, -up, true, 1.0),
        object_snow_coverage(vec3<f32>(0.0, 0.1, 0.0), up, true, 1.0));
    result[1] = vec4<f32>(object_snow_coverage(p, up, true, object_snow_baked_exposure(1.0, true)),
        object_snow_baked_exposure(1.0, false), object_snow_baked_exposure(0.72, true),
        dot(object_snow_normal(p, up, up, vec3<f32>(0.01,0.0,0.0),
            vec3<f32>(0.0,0.0,0.01), 0.0), up));
}
"#;

fn compose_probe() -> naga::Module {
    let mut composer = crate::shaders::build_composer();
    composer
        .make_naga_module(NagaModuleDescriptor {
            source: PROBE,
            file_path: "object_snow_probe.wgsl",
            ..Default::default()
        })
        .unwrap_or_else(|error| panic!("{}", error.emit_to_string(&composer)))
}

#[test]
fn actual_object_snow_helper_composes() {
    compose_probe();
}

#[test]
fn actual_object_snow_gpu_rejects_shelter_underside_unknown_and_ineligible() {
    // Absence of a test device is a failure, never a successful skipped proof.
    let (device, queue) = crate::gfx3d::cull::tests::headless()
        .expect("object-snow GPU acceptance requires an available test device");
    let module = compose_probe();
    let frame_type = module
        .global_variables
        .iter()
        .find_map(|(_, global)| {
            (global
                .binding
                .as_ref()
                .is_some_and(|b| b.group == 0 && b.binding == 0))
            .then_some(global.ty)
        })
        .expect("composed helper must bind the real Frame UBO");
    let (members, span) = match &module.types[frame_type].inner {
        naga::TypeInner::Struct { members, span } => (members.clone(), *span as usize),
        _ => panic!("Frame must be a struct"),
    };
    let offset = |name: &str| {
        members
            .iter()
            .find(|m| m.name.as_deref() == Some(name))
            .unwrap_or_else(|| panic!("Frame missing {name}"))
            .offset as usize
    };
    let mut bytes = vec![0_u8; span];
    let put = |bytes: &mut [u8], at: usize, values: &[f32]| {
        bytes[at..at + values.len() * 4].copy_from_slice(bytemuck::cast_slice(values));
    };
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
    put(&mut bytes, offset("skyvisb"), &[1.0 / 16.0, 0.0, 0.0, 0.0]);
    put(&mut bytes, offset("snow_surface"), &[0.18, -1.0, 0.0, 0.0]);
    let uniform = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
        label: Some("object_snow_actual_frame"),
        contents: &bytes,
        usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
    });
    let shader = device.create_shader_module(wgpu::ShaderModuleDescriptor {
        label: Some("object_snow_actual_helper"),
        source: wgpu::ShaderSource::Naga(std::borrow::Cow::Owned(module)),
    });
    let pipeline = device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
        label: Some("object_snow_probe"),
        layout: None,
        module: &shader,
        entry_point: Some("cs_probe"),
        compilation_options: Default::default(),
        cache: None,
    });
    let depth = device.create_texture(&wgpu::TextureDescriptor {
        label: Some("synthetic_directional_roofs"),
        size: wgpu::Extent3d {
            width: 8,
            height: 8,
            depth_or_array_layers: 5,
        },
        mip_level_count: 1,
        sample_count: 1,
        dimension: wgpu::TextureDimension::D2,
        format: wgpu::TextureFormat::Depth32Float,
        usage: wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::TEXTURE_BINDING,
        view_formats: &[],
    });
    let sample_view = depth.create_view(&wgpu::TextureViewDescriptor {
        dimension: Some(wgpu::TextureViewDimension::D2Array),
        ..Default::default()
    });
    let layer_views: Vec<_> = (0..5)
        .map(|i| {
            depth.create_view(&wgpu::TextureViewDescriptor {
                dimension: Some(wgpu::TextureViewDimension::D2),
                base_array_layer: i,
                array_layer_count: Some(1),
                ..Default::default()
            })
        })
        .collect();
    let sampler = device.create_sampler(&wgpu::SamplerDescriptor {
        compare: Some(wgpu::CompareFunction::LessEqual),
        mag_filter: wgpu::FilterMode::Linear,
        min_filter: wgpu::FilterMode::Linear,
        ..Default::default()
    });
    let output = device.create_buffer(&wgpu::BufferDescriptor {
        label: Some("object_snow_result"),
        size: 32,
        usage: wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_SRC,
        mapped_at_creation: false,
    });
    let staging = device.create_buffer(&wgpu::BufferDescriptor {
        label: Some("object_snow_readback"),
        size: 32,
        usage: wgpu::BufferUsages::MAP_READ | wgpu::BufferUsages::COPY_DST,
        mapped_at_creation: false,
    });
    let bindings = device.create_bind_group(&wgpu::BindGroupDescriptor {
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
                resource: wgpu::BindingResource::TextureView(&sample_view),
            },
        ],
    });
    let results = device.create_bind_group(&wgpu::BindGroupDescriptor {
        label: None,
        layout: &pipeline.get_bind_group_layout(1),
        entries: &[wgpu::BindGroupEntry {
            binding: 0,
            resource: output.as_entire_binding(),
        }],
    });
    let mut probe = |snow: [f32; 4],
                     depths: [f32; 5],
                     position: [f32; 3],
                     gate: f32,
                     strength: f32,
                     floor: f32| {
        put(&mut bytes, offset("snow_surface"), &snow);
        put(
            &mut bytes,
            offset("cam_pos"),
            &[position[0], position[1], position[2], 0.0],
        );
        put(&mut bytes, offset("skyvis"), &[gate, 0.0, strength, floor]);
        queue.write_buffer(&uniform, 0, &bytes);
        let mut encoder = device.create_command_encoder(&Default::default());
        for i in 0..5 {
            let _pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
                label: Some("seed_directional_depth"),
                color_attachments: &[],
                depth_stencil_attachment: Some(wgpu::RenderPassDepthStencilAttachment {
                    view: &layer_views[i],
                    depth_ops: Some(wgpu::Operations {
                        load: wgpu::LoadOp::Clear(depths[i]),
                        store: wgpu::StoreOp::Store,
                    }),
                    stencil_ops: None,
                }),
                ..Default::default()
            });
        }
        {
            let mut pass = encoder.begin_compute_pass(&Default::default());
            pass.set_pipeline(&pipeline);
            pass.set_bind_group(0, &bindings, &[]);
            pass.set_bind_group(1, &results, &[]);
            pass.dispatch_workgroups(1, 1, 1);
        }
        encoder.copy_buffer_to_buffer(&output, 0, &staging, 0, 32);
        queue.submit(Some(encoder.finish()));
        let slice = staging.slice(..);
        let (sender, receiver) = std::sync::mpsc::channel();
        slice.map_async(wgpu::MapMode::Read, move |result| {
            let _ = sender.send(result);
        });
        device.poll(wgpu::PollType::wait_indefinitely()).unwrap();
        receiver.recv().unwrap().unwrap();
        let data = slice.get_mapped_range();
        let values: [f32; 8] = bytemuck::cast_slice::<u8, f32>(&data).try_into().unwrap();
        drop(data);
        staging.unmap();
        values
    };
    let centre = [0.0, 0.0, 0.75];
    for (strength, floor) in [(1.0, 0.32), (0.0, 0.0), (1.0, 1.0)] {
        let roof = probe(
            [0.18, -1.0, 0.0, 0.0],
            [0.25, 1.0, 1.0, 1.0, 1.0],
            centre,
            1.0,
            strength,
            floor,
        );
        assert!(
            roof[0] < 0.001 && roof[4] < 0.001,
            "zenith shelter must beat known diffuse-open bake and artistic AO: {roof:?}"
        );
    }
    let open = probe([0.18, -1.0, 0.0, 0.0], [1.0; 5], centre, 1.0, 1.0, 0.32);
    assert!(
        open[0] > 0.99 && open[4] > 0.99,
        "exposed upward receiver must coat: {open:?}"
    );
    for index in [1, 2, 3, 5, 6] {
        assert!(open[index] < 0.001, "policy rejection {index}: {open:?}");
    }
    assert_eq!(
        open[7], 1.0,
        "dry/disabled normal must preserve the exact original normal"
    );
    for (position, gate) in [
        (centre, 0.0),
        ([0.0, 0.0, 2.0], 1.0),
        ([1.0, 0.0, 0.75], 1.0),
    ] {
        let absent = probe([0.18, -1.0, 0.0, 0.0], [0.25; 5], position, gate, 1.0, 0.32);
        assert!(
            absent[0] < 0.001 && absent[4] > 0.99,
            "unknown coverage must fail closed; proven fully-open bake may carry far roof: {absent:?}"
        );
    }
    // A covered point in the feather band has OPEN lighting fallback. Snow must undo that
    // fallback rather than paint a moving white rim on a sheltered interior.
    let edge = probe(
        [0.18, -1.0, 0.0, 0.0],
        [0.25; 5],
        [0.95, 0.0, 0.75],
        1.0,
        0.0,
        1.0,
    );
    assert!(
        edge[0] < 0.001,
        "coverage feather must not leak open fallback: {edge:?}"
    );
    // Raw deposit is zero. A raised surface with known open cover must use its actual
    // Y, not terrain altitude or origin. At quarter-ramp height, smoothstep gives
    // 0.15625 rather than linear 0.25: choose line depth that leaves coverage unsaturated
    // so the assertion detects a silently restored linear ramp.
    let ramp = [0.0, 100.0, 1.0, 0.04];
    let smooth = |lo: f32, hi: f32, v: f32| {
        let t = ((v - lo) / (hi - lo)).clamp(0.0, 1.0);
        t * t * (3.0 - 2.0 * t)
    };
    for fraction in [0.0_f32, 0.25, 0.5, 0.75, 1.0] {
        let actual = probe(ramp, [1.0; 5], [0.0, 100.0 + fraction, 0.75], 1.0, 0.0, 1.0);
        let expected = smooth(0.002, 0.04, smooth(100.0, 101.0, 100.0 + fraction) * 0.04);
        assert!(
            (actual[4] - expected).abs() < 1e-4,
            "actual raised surface altitude must use smooth snowline ramp: fraction={fraction} actual={actual:?} expected={expected}"
        );
        assert!(
            actual[0] < 0.001,
            "unproven far cover must remain closed at any altitude: {actual:?}"
        );
    }
    let below = probe(ramp, [1.0; 5], [0.0, 99.0, 0.75], 1.0, 1.0, 0.32);
    assert!(
        below[4] < 0.001,
        "zero-deposit surface below the snowline must remain dry: {below:?}"
    );
    let deposited = probe(
        [0.18, 100.0, 1.0, 0.04],
        [1.0; 5],
        [0.0, 99.0, 0.75],
        1.0,
        1.0,
        0.32,
    );
    assert!(
        deposited[4] > 0.99,
        "real deposit must survive below the altitude snowline: {deposited:?}"
    );
}

#[test]
fn snow_material_flag_and_prepass_contracts_remain_explicit() {
    let direct = include_str!("shader3d.wgsl");
    let retained = include_str!("gpu_driven.wgsl");
    let shared = include_str!("../shaders/shading.wgsl");
    assert_eq!(
        super::direct_material_flags(crate::ffi::WGR_DRAW3D_SNOW_RECEIVER, 0, false),
        2048
    );
    assert_eq!(
        super::direct_material_flags(
            crate::ffi::DRAW3D_ON_SURFACE | crate::ffi::DRAW3D_ZBIAS_MASK,
            0,
            false
        ),
        0
    );
    for src in [direct, retained] {
        assert_eq!(
            src.matches("surface_n = object_snow_normal(").count(),
            2,
            "opaque and A2C prepasses must use the same powder normal as colour"
        );
        assert!(src.contains("dot(in.normal, in.normal) > 1e-8"));
    }
    let raw = &retained[retained.find("fn retained_snow_exposure").unwrap()
        ..retained.find("fn fs_gpu_body").unwrap()];
    assert!(
        raw.contains("sky_volume_meta[m + 1u].w < 0.5")
            && raw.contains("count > len - min(base, len)")
    );
    assert!(!raw.contains("skyvisc.y") && !raw.contains("skyvisc.z"));
    let coverage = &shared[shared.find("fn object_snow_coverage").unwrap()
        ..shared.find("fn object_snow_normal").unwrap()];
    assert!(!coverage.contains("snow_depth(") && !coverage.contains("snow_deficit("));
    assert!(
        coverage.contains("smoothstep(frame.snow_surface.y")
            && coverage.contains("frame.snow_surface.y + frame.snow_surface.z, world_abs.y")
    );
    assert!(coverage.contains("dot(geo_normal, world_pos) > 0.0"));
}
