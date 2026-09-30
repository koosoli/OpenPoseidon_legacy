use naga_oil::compose::NagaModuleDescriptor;
use wgpu::util::DeviceExt;

// Exercise the actual GPU helpers, including complete removal of unresolved
// detail. These numbers are material acceptance, not installed scene proof.
const PROBE: &str = r#"
#import snow_material::{snow_powder_albedo, snow_powder_normal, snow_surface_coverage}
@group(0) @binding(0) var<storage, read_write> result: array<vec4<f32>>;
@compute @workgroup_size(32)
fn cs_probe(@builtin(global_invocation_id) id: vec3<u32>) {
    let i = id.x;
    let world = vec3<f32>(5018.51 + f32(i % 16u) * 0.19, 117.58, 4087.66);
    let footprint = select(0.01, 10.0, i >= 16u);
    let compacted = select(0.0, 1.0, i % 16u >= 8u);
    let albedo = snow_powder_albedo(world, footprint, compacted);
    let normal = snow_powder_normal(world, vec3<f32>(0.0, 1.0, 0.0), footprint);
    var coverage = snow_surface_coverage(0.1, 1.0, 1.0);
    if (i % 8u == 1u) { coverage = snow_surface_coverage(0.0, 1.0, 1.0); }
    if (i % 8u == 2u) { coverage = snow_surface_coverage(0.1, 0.0, 1.0); }
    if (i % 8u == 3u) { coverage = snow_surface_coverage(0.1, -1.0, 1.0); }
    if (i % 8u == 4u) { coverage = snow_surface_coverage(0.1, 1.0, 0.0); }
    if (i % 8u == 5u) { coverage = snow_surface_coverage(0.01, 0.55, 0.8); }
    result[i * 2u] = vec4<f32>(albedo, coverage);
    result[i * 2u + 1u] = vec4<f32>(normal, length(normal));
}
"#;

#[test]
fn snow_powder_gpu_filters_distance_and_obeys_surface_admission() {
    let (device, queue) = crate::gfx3d::cull::tests::headless()
        .expect("snow-material acceptance requires a real test device");
    let mut composer = super::build_composer();
    let module = composer.make_naga_module(NagaModuleDescriptor {
        source: PROBE, file_path: "snow_material_probe.wgsl", ..Default::default()
    }).unwrap_or_else(|e| panic!("{}", e.emit_to_string(&composer)));
    let shader = device.create_shader_module(wgpu::ShaderModuleDescriptor {
        label: Some("snow_material_actual_helpers"),
        source: wgpu::ShaderSource::Naga(std::borrow::Cow::Owned(module)),
    });
    let pipeline = device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
        label: Some("snow_material_numeric"), layout: None, module: &shader,
        entry_point: Some("cs_probe"), compilation_options: Default::default(), cache: None,
    });
    let initial = [0.0_f32; 32 * 8];
    let output = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
        label: Some("snow_material_results"), contents: bytemuck::cast_slice(&initial),
        usage: wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_SRC,
    });
    let staging = device.create_buffer(&wgpu::BufferDescriptor {
        label: Some("snow_material_readback"), size: 32 * 8 * 4,
        usage: wgpu::BufferUsages::MAP_READ | wgpu::BufferUsages::COPY_DST,
        mapped_at_creation: false,
    });
    let bind = device.create_bind_group(&wgpu::BindGroupDescriptor {
        label: Some("snow_material_probe"), layout: &pipeline.get_bind_group_layout(0),
        entries: &[wgpu::BindGroupEntry { binding: 0, resource: output.as_entire_binding() }],
    });
    let mut encoder = device.create_command_encoder(&Default::default());
    {
        let mut pass = encoder.begin_compute_pass(&Default::default());
        pass.set_pipeline(&pipeline); pass.set_bind_group(0, &bind, &[]);
        pass.dispatch_workgroups(1, 1, 1);
    }
    encoder.copy_buffer_to_buffer(&output, 0, &staging, 0, 32 * 8 * 4);
    queue.submit(Some(encoder.finish()));
    let slice = staging.slice(..);
    let (send, recv) = std::sync::mpsc::channel();
    slice.map_async(wgpu::MapMode::Read, move |r| { let _ = send.send(r); });
    device.poll(wgpu::PollType::wait_indefinitely()).unwrap();
    recv.recv().unwrap().unwrap();
    let bytes = slice.get_mapped_range();
    let samples = bytemuck::cast_slice::<u8, f32>(&bytes);
    for (i, sample) in samples.chunks_exact(8).enumerate() {
        assert!(sample.iter().all(|v| v.is_finite()));
        assert!(sample[..3].iter().all(|v| *v > 0.50 && *v < 0.90));
        assert!((sample[7] - 1.0).abs() < 1e-5);
        assert!(sample[5] >= 1.0 / (1.0_f32 + 0.16*0.16).sqrt() - 1e-6,
            "resolved powder tilt must remain within the explicit 9.1-degree bound");
        match i % 8 {
            1..=4 => assert_eq!(sample[3], 0.0, "dry/wall/underside/shelter must stay bare"),
            5 => assert!(sample[3] > 0.0 && sample[3] < 0.5),
            _ => assert_eq!(sample[3], 1.0),
        }
        if i >= 16 {
            let scale = if i % 16 >= 8 { 0.72 } else { 1.0 };
            for (value, mean) in sample[..3].iter().zip([0.78, 0.80, 0.82]) {
                assert!((*value - mean * scale).abs() < 1e-6);
            }
            assert_eq!(&sample[4..7], &[0.0, 1.0, 0.0], "unresolved powder must have its mean normal");
        }
    }
    assert!(samples[4].abs() + samples[6].abs() > 1e-4, "close powder must retain resolved relief");
    drop(bytes); staging.unmap();
}
