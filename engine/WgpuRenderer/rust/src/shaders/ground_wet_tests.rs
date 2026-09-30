use naga_oil::compose::NagaModuleDescriptor;
use wgpu::util::DeviceExt;

#[test]
fn soil_wetting_composes_with_actual_terrain_and_uses_physical_rain_proof() {
    let source = include_str!("../terrain/terrain.wgsl");
    let mut composer = super::build_composer();
    composer.make_naga_module(NagaModuleDescriptor {
        source, file_path: "terrain/terrain.wgsl", ..Default::default()
    }).unwrap_or_else(|e| panic!("{}", e.emit_to_string(&composer)));
    assert!(source.contains("soil_wet *= rain_proof"));
    assert!(source.contains("soil_wet_amount(mud, tp.rain_wetness, snow_cover("));
    assert!(source.contains("soil_wet * (1.0 - rain_puddle)"));
    assert!(source.contains("let moist_n = geometric_n"));
}

#[test]
fn actual_soil_wetting_gpu_is_dry_inert_source_bounded_snow_excluded_and_broad() {
    let (device, queue) = crate::gfx3d::cull::tests::headless()
        .expect("wet soil acceptance requires a real GPU device");
    let mut composer = super::build_composer();
    let source = r#"
#import ground_wet::{cultivated_soil_fraction, soil_wet_amount, soil_wet_fresnel, soil_wet_sun_lobe}
@group(0) @binding(0) var<storage, read_write> result: array<vec4<f32>>;
@compute @workgroup_size(64)
fn cs_probe(@builtin(global_invocation_id) id: vec3<u32>) {
    let i = id.x;
    let wet = f32(i) / 63.0;
    result[i * 3u] = vec4<f32>(soil_wet_amount(1.0, wet, 0.0),
        soil_wet_amount(1.0, 0.0, 0.0), soil_wet_amount(0.0, wet, 0.0),
        soil_wet_amount(1.0, wet, 0.02));
    result[i * 3u + 1u] = vec4<f32>(cultivated_soil_fraction(4u, vec2<f32>(0.5)),
        cultivated_soil_fraction(4u, vec2<f32>(0.1, 0.5)),
        cultivated_soil_fraction(0u, vec2<f32>(0.5)),
        cultivated_soil_fraction(4u, vec2<f32>(0.9, 0.5)));
    result[i * 3u + 2u] = vec4<f32>(soil_wet_fresnel(wet),
        soil_wet_sun_lobe(wet), soil_wet_sun_lobe(0.70710678),
        soil_wet_amount(0.25, wet, 0.0));
}
"#;
    let module = composer.make_naga_module(NagaModuleDescriptor {
        source, file_path: "actual_soil_wet_probe.wgsl", ..Default::default()
    }).unwrap_or_else(|e| panic!("{}", e.emit_to_string(&composer)));
    let shader = device.create_shader_module(wgpu::ShaderModuleDescriptor {
        label: Some("actual_soil_wet"), source: wgpu::ShaderSource::Naga(std::borrow::Cow::Owned(module)),
    });
    let pipeline = device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
        label: Some("actual_soil_wet"), layout: None, module: &shader,
        entry_point: Some("cs_probe"), compilation_options: Default::default(), cache: None,
    });
    let initial = [0.0f32; 64 * 12];
    let output = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
        label: Some("soil_wet_results"), contents: bytemuck::cast_slice(&initial),
        usage: wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_SRC,
    });
    let staging = device.create_buffer(&wgpu::BufferDescriptor {
        label: Some("soil_wet_readback"), size: (initial.len() * 4) as u64,
        usage: wgpu::BufferUsages::MAP_READ | wgpu::BufferUsages::COPY_DST, mapped_at_creation: false,
    });
    let bind = device.create_bind_group(&wgpu::BindGroupDescriptor {
        label: Some("soil_wet_results"), layout: &pipeline.get_bind_group_layout(0),
        entries: &[wgpu::BindGroupEntry { binding: 0, resource: output.as_entire_binding() }],
    });
    let mut encoder = device.create_command_encoder(&Default::default());
    {
        let mut pass = encoder.begin_compute_pass(&Default::default());
        pass.set_pipeline(&pipeline); pass.set_bind_group(0, &bind, &[]);
        pass.dispatch_workgroups(1, 1, 1);
    }
    encoder.copy_buffer_to_buffer(&output, 0, &staging, 0, (initial.len() * 4) as u64);
    queue.submit(Some(encoder.finish()));
    let slice = staging.slice(..);
    let (tx, rx) = std::sync::mpsc::channel();
    slice.map_async(wgpu::MapMode::Read, move |r| { tx.send(r).unwrap(); });
    device.poll(wgpu::PollType::wait_indefinitely()).unwrap(); rx.recv().unwrap().unwrap();
    let bytes = slice.get_mapped_range();
    let samples = bytemuck::cast_slice::<u8, f32>(&bytes);
    let mut previous = 0.0;
    for s in samples.chunks_exact(12) {
        assert!(s.iter().all(|v| v.is_finite()));
        assert!(s[0] >= previous && (0.0..=1.0).contains(&s[0])); previous = s[0];
        assert_eq!(&s[1..4], &[0.0; 3], "dry/hard/snow surfaces remain inert");
        assert_eq!(&s[4..8], &[1.0, 0.0, 0.0, 0.0], "exact source and world-cell interior");
        assert!((0.01999..=0.12001).contains(&s[8]) && (0.0..=0.06001).contains(&s[9]));
        assert!((s[10] - 0.00375).abs() < 1e-6, "45-degree response must be broad and low");
        assert!((s[11] - s[0] * 0.25).abs() < 1e-6, "mixed-source weighting remains bounded");
    }
    assert_eq!(previous, 1.0, "positive wet soil is exercised");
}
