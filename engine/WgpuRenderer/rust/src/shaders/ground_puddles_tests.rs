use naga_oil::compose::NagaModuleDescriptor;
use wgpu::util::DeviceExt;

// Exercise actual shared helpers on GPU; installed scenery acceptance remains separate.
const PROBE: &str = r#"
#import ground_puddles::{ground_puddle_mask, ground_puddle_land_factor, ground_puddle_ripple_normal}
@group(0) @binding(0) var<storage, read_write> result: array<vec4<f32>>;
@compute @workgroup_size(64)
fn cs_probe(@builtin(global_invocation_id) id: vec3<u32>) {
    let i = id.x;
    let world = vec2<f32>(9486.0 + f32(i % 8u) * 0.43, 3006.0 + f32(i / 8u) * 0.43);
    // Sparse independent rain events need several cells and times; the old
    // .63m square sampled a single repeating packet and can now be inactive.
    let time = 14.5 + f32(i / 8u) * 0.19;
    let mask_world = vec2<f32>(9480.0 + f32(i % 8u) * 0.75, 3000.0 + f32(i / 8u) * 0.75);
    result[i * 4u] = vec4<f32>(ground_puddle_mask(mask_world, 0.01, 0.0),
        ground_puddle_mask(mask_world, 0.01, 0.6), ground_puddle_mask(mask_world, 0.01, 1.0),
        ground_puddle_mask(mask_world, 1.0, 1.0));
    result[i * 4u + 1u] = vec4<f32>(ground_puddle_ripple_normal(world, time, 1.0, 0.01),
        ground_puddle_land_factor(1.0, 1.0, 0.0, 0.0));
    result[i * 4u + 2u] = vec4<f32>(ground_puddle_ripple_normal(world, time, 1.0, 0.16),
        ground_puddle_land_factor(select(1.0, 0.9, i % 8u == 0u),
            select(1.0, -0.1, i % 8u == 1u), 0.0, select(0.0, 0.03, i % 8u == 2u)));
    result[i * 4u + 3u] = vec4<f32>(ground_puddle_ripple_normal(world, time, 0.0, 0.01),
        distance(ground_puddle_ripple_normal(world, time, 1.0, 0.01),
                 ground_puddle_ripple_normal(world, time + 0.2, 1.0, 0.01)));
}
"#;

#[test]
fn ground_puddles_gpu_grow_with_wetness_filter_rain_and_obey_land_gates() {
    let (device, queue) = crate::gfx3d::cull::tests::headless()
        .expect("ground-puddle acceptance requires a real test device");
    let mut composer = super::build_composer();
    let module = composer.make_naga_module(NagaModuleDescriptor {
        source: PROBE, file_path: "ground_puddles_probe.wgsl", ..Default::default()
    }).unwrap_or_else(|e| panic!("{}", e.emit_to_string(&composer)));
    let shader = device.create_shader_module(wgpu::ShaderModuleDescriptor {
        label: Some("ground_puddles_actual_helpers"),
        source: wgpu::ShaderSource::Naga(std::borrow::Cow::Owned(module)),
    });
    let pipeline = device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
        label: Some("ground_puddles_numeric"), layout: None, module: &shader,
        entry_point: Some("cs_probe"), compilation_options: Default::default(), cache: None,
    });
    let initial = [0.0_f32; 64 * 16];
    let output = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
        label: Some("ground_puddles_results"), contents: bytemuck::cast_slice(&initial),
        usage: wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_SRC,
    });
    let staging = device.create_buffer(&wgpu::BufferDescriptor {
        label: Some("ground_puddles_readback"), size: 64 * 16 * 4,
        usage: wgpu::BufferUsages::MAP_READ | wgpu::BufferUsages::COPY_DST,
        mapped_at_creation: false,
    });
    let bind = device.create_bind_group(&wgpu::BindGroupDescriptor {
        label: Some("ground_puddles_probe"), layout: &pipeline.get_bind_group_layout(0),
        entries: &[wgpu::BindGroupEntry { binding: 0, resource: output.as_entire_binding() }],
    });
    let mut encoder = device.create_command_encoder(&Default::default());
    {
        let mut pass = encoder.begin_compute_pass(&Default::default());
        pass.set_pipeline(&pipeline); pass.set_bind_group(0, &bind, &[]);
        pass.dispatch_workgroups(1, 1, 1);
    }
    encoder.copy_buffer_to_buffer(&output, 0, &staging, 0, 64 * 16 * 4);
    queue.submit(Some(encoder.finish()));
    let slice = staging.slice(..);
    let (send, recv) = std::sync::mpsc::channel();
    slice.map_async(wgpu::MapMode::Read, move |r| { let _ = send.send(r); });
    device.poll(wgpu::PollType::wait_indefinitely()).unwrap();
    recv.recv().unwrap().unwrap();
    let bytes = slice.get_mapped_range();
    let samples = bytemuck::cast_slice::<u8, f32>(&bytes);
    let mut resolved_rain = false;
    let mut moving_rain = false;
    let mut growing_patch = false;
    for (i, sample) in samples.chunks_exact(16).enumerate() {
        assert!(sample.iter().all(|v| v.is_finite()));
        assert_eq!(sample[0], 0.0, "dry ground must have no standing-water film");
        assert_eq!(sample[3], 0.0, "unresolved film must not glitter");
        assert!(sample[2] >= sample[1] && (0.0..=1.0).contains(&sample[2]));
        growing_patch |= sample[2] > sample[1] + 0.01;
        assert_eq!(sample[7], 1.0, "dry lowland one metre above sea is eligible");
        assert_eq!(&sample[8..11], &[0.0, 1.0, 0.0], "unresolved rain must have the mean normal");
        assert_eq!(&sample[12..15], &[0.0, 1.0, 0.0], "no liquid rain means no ripple");
        assert_eq!(sample[11], if i % 8 <= 2 { 0.0 } else { 1.0 }, "slope/ocean/snow exclusions");
        let n = &sample[4..7];
        let len = n.iter().map(|v| v * v).sum::<f32>().sqrt();
        assert!((len - 1.0).abs() < 1e-5 && n[1] > 0.99,
            "rain normals must remain finite, upward and bounded");
        resolved_rain |= n[0].abs() + n[2].abs() > 1e-5;
        moving_rain |= sample[15] > 1e-5;
    }
    assert!(growing_patch, "fixture must exercise expansion, not empty-field equivalence");
    assert!(resolved_rain && moving_rain, "actual rain helper must create moving resolved ripples");
    drop(bytes); staging.unmap();
}
