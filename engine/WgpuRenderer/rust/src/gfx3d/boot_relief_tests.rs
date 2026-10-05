use naga_oil::compose::NagaModuleDescriptor;

const PROBE: &str = r#"
#import boot_relief::{boot_relief_height, boot_relief_normal, boot_relief_parallax, boot_relief_multiplier, boot_relief_composite}
@group(0) @binding(0) var<storage,read_write> output: array<vec4<f32>>;
@compute @workgroup_size(1) fn cs_probe() {
    let n = vec3<f32>(0.0, 1.0, 0.0);
    let t = vec3<f32>(0.14, 0.0, 0.0);
    let b = vec3<f32>(0.0, 0.0, 0.28);
    output[0] = vec4<f32>(boot_relief_height(vec4<f32>(0.0,0.0,0.0,1.0)),
        boot_relief_height(vec4<f32>(1.0)), boot_relief_height(vec4<f32>(0.0)),
        boot_relief_height(vec4<f32>(0.0,0.0,0.0,0.5)));
    let tilted = boot_relief_normal(n,t,b,vec2<f32>(0.014,0.0));
    output[1] = vec4<f32>(tilted, dot(tilted,tilted));
    output[2] = vec4<f32>(boot_relief_normal(n,vec3<f32>(0.0),b,vec2<f32>(1.0)),1.0);
    output[3] = vec4<f32>(boot_relief_parallax(normalize(vec3<f32>(1.0,0.01,1.0)),n,t,b,-0.003),
        boot_relief_parallax(n,n,t,b,-0.003));
    let low = boot_relief_multiplier(n,normalize(vec3<f32>(-1.0,1.0,0.0)),normalize(vec3<f32>(1.0,1.0,0.0)),
                                    -0.003,0.25,1.0,1.0);
    output[4] = vec4<f32>(low,boot_relief_multiplier(n,tilted,n,-0.003,0.0,1.0,1.0),
        boot_relief_multiplier(n,tilted,n,-0.003,0.25,0.0,1.0),
        boot_relief_multiplier(n,n,n,0.0,1.0,1.0,1.0));
    output[5] = vec4<f32>(vec3<f32>(0.08,0.12,0.035)*low,0.37);
    let air = vec3<f32>(0.2,0.3,0.4);
    let ground = vec3<f32>(0.08,0.12,0.035);
    let composed = boot_relief_composite(low,air);
    output[6] = vec4<f32>(composed.rgb+(air+ground)*composed.a,composed.a);
    output[7] = vec4<f32>(composed.rgb+air*composed.a,1.0);
}
"#;

fn probe() -> naga::Module {
    let mut composer = crate::shaders::build_composer();
    composer
        .make_naga_module(NagaModuleDescriptor {
            source: PROBE,
            file_path: "actual_boot_relief_probe.wgsl",
            ..Default::default()
        })
        .unwrap_or_else(|error| panic!("{}", error.emit_to_string(&composer)))
}

#[test]
fn boot_relief_actual_shader_composes_and_blend_preserves_receiver() {
    probe();
    let mut composer = crate::shaders::build_composer();
    composer
        .make_naga_module(NagaModuleDescriptor {
            source: include_str!("shader3d.wgsl"),
            file_path: "gfx3d/shader3d.wgsl",
            shader_defs: crate::shaders::shader_defs(),
            ..Default::default()
        })
        .unwrap_or_else(|error| panic!("{}", error.emit_to_string(&composer)));
    let blend = super::Gfx3d::blend_state(crate::ffi::WgrBlend::ReliefMultiply as u8).unwrap();
    assert_eq!(blend.color.src_factor, wgpu::BlendFactor::One);
    assert_eq!(blend.color.dst_factor, wgpu::BlendFactor::SrcAlpha);
    assert_eq!(blend.alpha.src_factor, wgpu::BlendFactor::Zero);
    assert_eq!(blend.alpha.dst_factor, wgpu::BlendFactor::One);
    assert_eq!(
        super::direct_material_flags(crate::ffi::WGR_DRAW3D_BOOT_RELIEF, 0, false),
        16384
    );
}

#[test]
fn boot_relief_actual_gpu_depth_normal_parallax_and_neutral_colour() {
    let (device, queue) =
        super::cull::tests::headless().expect("boot relief proof needs a test device");
    let bytes = 8 * 16;
    let output = device.create_buffer(&wgpu::BufferDescriptor {
        label: Some("boot relief actual output"),
        size: bytes,
        mapped_at_creation: false,
        usage: wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_SRC,
    });
    let staging = device.create_buffer(&wgpu::BufferDescriptor {
        label: Some("boot relief readback"),
        size: bytes,
        mapped_at_creation: false,
        usage: wgpu::BufferUsages::MAP_READ | wgpu::BufferUsages::COPY_DST,
    });
    let shader = device.create_shader_module(wgpu::ShaderModuleDescriptor {
        label: Some("actual boot relief"),
        source: wgpu::ShaderSource::Naga(std::borrow::Cow::Owned(probe())),
    });
    let pipeline = device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
        label: None,
        layout: None,
        module: &shader,
        entry_point: Some("cs_probe"),
        compilation_options: Default::default(),
        cache: None,
    });
    let binding = device.create_bind_group(&wgpu::BindGroupDescriptor {
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
        pass.set_bind_group(0, &binding, &[]);
        pass.dispatch_workgroups(1, 1, 1);
    }
    encoder.copy_buffer_to_buffer(&output, 0, &staging, 0, bytes);
    queue.submit(Some(encoder.finish()));
    let slice = staging.slice(..);
    let (sender, receiver) = std::sync::mpsc::channel();
    slice.map_async(wgpu::MapMode::Read, move |result| {
        let _ = sender.send(result);
    });
    device.poll(wgpu::PollType::wait_indefinitely()).unwrap();
    receiver.recv().unwrap().unwrap();
    let data = slice.get_mapped_range();
    let actual: &[[f32; 4]] = bytemuck::cast_slice(&data);
    assert!(
        (actual[0][0] + 0.003).abs() < 0.000001
            && actual[0][1].abs() < 0.000001
            && actual[0][2] == 0.0
    );
    assert!((actual[0][3] + 0.0015).abs() < 0.000001);
    assert!(actual[1][0] < 0.0 && actual[1][1] > 0.99 && (actual[1][3] - 1.0).abs() < 0.00001);
    assert_eq!(actual[2], [0.0, 1.0, 0.0, 1.0]);
    assert!(actual[3][0].abs() <= 0.020001 && actual[3][1].abs() <= 0.020001);
    assert_eq!(&actual[3][2..], &[0.0, 0.0]);
    assert!(actual[4][0] > 0.88 && actual[4][0] < 1.0);
    assert_eq!(&actual[4][1..], &[1.0, 1.0, 1.0]);
    for (channel, base) in [0.08, 0.12, 0.035].iter().enumerate() {
        assert!((actual[5][channel] / base - actual[4][0]).abs() < 0.00001);
    }
    assert_eq!(actual[5][3], 0.37);
    for (channel, air) in [0.2, 0.3, 0.4].iter().enumerate() {
        assert!((actual[6][channel] - air - actual[5][channel]).abs() < 0.00001);
        assert!(
            (actual[7][channel] - air).abs() < 0.00001,
            "foreground airlight must survive the relief blend"
        );
    }
    assert!(actual.iter().flatten().all(|value| value.is_finite()));
}
