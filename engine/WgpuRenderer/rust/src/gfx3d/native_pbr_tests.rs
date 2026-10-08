use naga_oil::compose::{NagaModuleDescriptor, ShaderDefValue};
use wgpu::util::DeviceExt;

const PRODUCTION: &str = include_str!("gpu_driven.wgsl");

fn channels_probe(mask: bool) -> naga::Module {
    // Execute the real production helper, not a Rust copy of the formula.
    let helper = if mask {
        "native_pbr_mask_channels"
    } else {
        "native_pbr_scaled_channels"
    };
    let start = PRODUCTION.find(&format!("fn {helper}(")).unwrap();
    let end = start + PRODUCTION[start..].find("\n}").unwrap() + 2;
    let mut source = PRODUCTION[start..end].to_owned();
    let scales = if mask {
        "vec4<f32>(0.0,0.0,0.0,0.0), vec4<f32>(1.0,1.0,1.0,1.0), vec4<f32>(0.0,0.5,2.0,1.0), vec4<f32>(2.0,3.0,0.0,1.0), vec4<f32>(1.0,1.0,0.0,0.0)"
    } else {
        "vec4<f32>(1.0,1.0,0.0,0.0), vec4<f32>(0.5,0.0,0.0,0.0), vec4<f32>(2.0,2.0,0.0,0.0), vec4<f32>(0.0,0.25,0.0,0.0), vec4<f32>(0.0,0.5,2.0,1.0)"
    };
    source.push_str(&format!(r#"
@group(0) @binding(0) var<storage,read_write> output:array<vec4<f32>>;
@compute @workgroup_size(1) fn cs_probe(@builtin(global_invocation_id) id:vec3<u32>) {{
    let channels = array<vec3<f32>,5>(vec3<f32>(0.8,0.7,0.6), vec3<f32>(0.8,0.7,0.6),
                                    vec3<f32>(0.8,0.7,0.6), vec3<f32>(0.8,0.7,0.6), vec3<f32>(0.8,0.7,0.6));
    let scales = array<vec4<f32>,5>({scales});
    output[id.x] = vec4<f32>({helper}(channels[id.x],scales[id.x]),0.37);
}}
"#));
    let module = naga::front::wgsl::parse_str(&source).unwrap();
    naga::valid::Validator::new(
        naga::valid::ValidationFlags::all(),
        naga::valid::Capabilities::all(),
    )
    .validate(&module)
    .unwrap();
    module
}

#[test]
fn native_pbr_basic_scaled_preview_and_legacy_shaders_compose() {
    for preview in [false, true] {
        let mut defs = crate::shaders::shader_defs();
        if preview {
            defs.insert("NATIVE_PBR_PREVIEW".to_owned(), ShaderDefValue::Bool(true));
        } else {
            defs.remove("NATIVE_PBR_PREVIEW");
        }
        for (source, path) in [
            (PRODUCTION, "gfx3d/gpu_driven.wgsl"),
            (
                include_str!("gpu_driven_shadow.wgsl"),
                "gfx3d/gpu_driven_shadow.wgsl",
            ),
            (include_str!("shader3d.wgsl"), "gfx3d/shader3d.wgsl"),
        ] {
            let mut composer = crate::shaders::build_composer();
            composer
                .make_naga_module(NagaModuleDescriptor {
                    source,
                    file_path: path,
                    shader_defs: defs.clone(),
                    ..Default::default()
                })
                .unwrap_or_else(|e| {
                    panic!("{path} preview={preview}: {}", e.emit_to_string(&composer))
                });
        }
    }
}

#[test]
fn native_pbr_basic_actual_gpu_preserves_identity_zero_ao_and_clamps_after_scaling() {
    let actual = gpu_channels_result(channels_probe(false));
    let expected = [
        0.8, 0.7, 0.6, 0.37, 0.4, 0.0, 0.6, 0.37, 1.0, 1.0, 0.6, 0.37, 0.08, 0.175, 0.6, 0.37, 0.8,
        0.7, 0.6, 0.37,
    ];
    for (actual, expected) in actual.iter().zip(expected) {
        assert!((actual - expected).abs() < 1e-6, "{actual} != {expected}");
    }
}

#[test]
fn native_multi_mask_actual_gpu_preserves_off_identity_zero_and_saturates_each_rgb_channel() {
    let actual = gpu_channels_result(channels_probe(true));
    let expected = [
        0.8, 0.7, 0.6, 0.37, 0.8, 0.7, 0.6, 0.37, 0.0, 0.35, 1.0, 0.37, 1.0, 1.0, 0.0, 0.37, 0.8,
        0.7, 0.6, 0.37,
    ];
    for (actual, expected) in actual.iter().zip(expected) {
        assert!((actual - expected).abs() < 1e-6, "{actual} != {expected}");
    }
}

fn gpu_channels_result(module: naga::Module) -> Vec<f32> {
    let (device, queue) =
        super::cull::tests::headless().expect("native PBR scale proof requires GPU");
    let output = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
        label: Some("native PBR scaled channels"),
        contents: &[0u8; 80],
        usage: wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_SRC,
    });
    let staging = device.create_buffer(&wgpu::BufferDescriptor {
        label: None,
        size: 80,
        mapped_at_creation: false,
        usage: wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::MAP_READ,
    });
    let shader = device.create_shader_module(wgpu::ShaderModuleDescriptor {
        label: Some("production native PBR scale probe"),
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
    let bind = device.create_bind_group(&wgpu::BindGroupDescriptor {
        label: None,
        layout: &pipeline.get_bind_group_layout(0),
        entries: &[wgpu::BindGroupEntry {
            binding: 0,
            resource: output.as_entire_binding(),
        }],
    });
    let mut encoder = device.create_command_encoder(&Default::default());
    {
        let mut pass = encoder.begin_compute_pass(&Default::default());
        pass.set_pipeline(&pipeline);
        pass.set_bind_group(0, &bind, &[]);
        pass.dispatch_workgroups(5, 1, 1);
    }
    encoder.copy_buffer_to_buffer(&output, 0, &staging, 0, 80);
    queue.submit([encoder.finish()]);
    let (tx, rx) = std::sync::mpsc::channel();
    staging.slice(..).map_async(wgpu::MapMode::Read, move |r| {
        let _ = tx.send(r);
    });
    device.poll(wgpu::PollType::wait_indefinitely()).unwrap();
    rx.recv().unwrap().unwrap();
    let data = staging.slice(..).get_mapped_range();
    let actual = bytemuck::cast_slice::<u8, f32>(&data).to_vec();
    drop(data);
    staging.unmap();
    actual
}
