use naga_oil::compose::NagaModuleDescriptor;
use wgpu::util::DeviceExt;

const DIRECT: &str = include_str!("../gfx3d/shader3d.wgsl");
const RETAINED: &str = include_str!("../gfx3d/gpu_driven.wgsl");
const SHADING: &str = include_str!("shading.wgsl");
const TERRAIN: &str = include_str!("../terrain/terrain.wgsl");

fn compact(source: &str) -> String {
    source.chars().filter(|c| !c.is_whitespace()).collect()
}

#[test]
fn ground_roads_use_physical_cover_only_for_actual_wet_ground_film() {
    let src = compact(SHADING);
    let admission = src
        .split("varrain_puddle=0.0;")
        .nth(1)
        .unwrap()
        .split("letterrain_s=")
        .next()
        .unwrap();
    assert!(admission.contains("if(ground_surface&&frame.ground_weather.x>0.18&&linear>0.5&&!is_foliage&&!is_cockpit&&snow_cover==0.0)"));
    let cover = admission
        .split("if(rain_puddle>0.0){")
        .nth(1)
        .expect("dry/mask-zero roads must skip the nine-tap shelter query");
    assert!(cover.contains("ground_puddle_rain_factor(interior_rain_reach(world_abs),interior_rain_coverage(world_abs))"));
    assert!(!admission.contains("interior_sky_reach("));
    assert!(!admission.contains("frame.skyvis.z") && !admission.contains("frame.skyvis.w"));
    assert!(
        admission
            .contains("ground_puddle_mask(world_abs.xz,ground_footprint,frame.ground_weather.x)")
    );
    assert!(admission.contains("ground_puddle_land_factor(surface_normal(geo_normal,world_pos,dwx,dwy).y,world_abs.y,frame.ground_weather.z,ground_snow_depth)"));
    assert!(src.contains("ground_puddle_ripple_normal(world_abs.xz,frame.ground_weather.w,frame.ground_weather.y,ground_footprint)"));
    let direct = compact(DIRECT);
    assert!(
        direct.contains("out.color=shade_fragment(in,in.conform_w>0.99)"),
        "only ground-pinned OnSurface draws enter road film"
    );
    assert_eq!(
        direct.matches("returnshade_fragment(in,direct_ground_receiver(in))").count(),
        2,
        "normal and early-Z object entries require explicit ground proof"
    );
    assert!(
        direct.contains("select(0.0,snow_depth(in.world_pos.xz+frame.cam_pos.xz),ground_surface)")
    );
    assert!(
        compact(RETAINED).contains("retained_tree_snow_cover(in),retained_ground_receiver(in),frame.snow_surface.x"),
        "retained objects require explicit ground proof"
    );
    let terrain = compact(TERRAIN);
    assert!(terrain.contains("letrain_reach=interior_rain_reach(in.world_pos+frame.cam_pos.xyz);"));
    assert!(terrain.contains("letrain_coverage=interior_rain_coverage(in.world_pos+frame.cam_pos.xyz);"));
    assert!(terrain.contains("ground_puddle_rain_factor(rain_reach,rain_coverage)"),
        "terrain and roads must undo the same open lighting fallback");
    // The complete real entry shader still composes through the common shade
    // signature. This is not proof of road producer/submission or appearance.
    let mut composer = super::build_composer();
    composer
        .make_naga_module(NagaModuleDescriptor {
            source: DIRECT,
            file_path: "gfx3d/shader3d.wgsl",
            ..Default::default()
        })
        .unwrap_or_else(|e| panic!("{}", e.emit_to_string(&composer)));
}

// The true geometric zenith reach is known for this fixture. The production
// interior map feathers it toward OPEN at its edge. Passing that feathered
// value directly into rain admission used to paint a covered edge falsely.
const PROBE: &str = r#"
#import ground_puddles::ground_puddle_rain_factor
@group(0) @binding(0) var<storage, read_write> result: array<vec4<f32>>;
@compute @workgroup_size(32)
fn cs_probe(@builtin(global_invocation_id) id: vec3<u32>) {
    let i = id.x;
    let validity = f32(i % 8u) / 7.0;
    var raw_reach = 0.0;
    if (i / 8u == 1u) { raw_reach = 1.0; }
    if (i / 8u == 2u) { raw_reach = 0.5; }
    if (i / 8u == 3u) { raw_reach = 0.75; }
    let feathered = mix(1.0, raw_reach, validity);
    result[i] = vec4<f32>(ground_puddle_rain_factor(feathered, validity), validity,
        validity * smoothstep(0.6, 0.95, feathered), raw_reach);
}
"#;

#[test]
fn ground_rain_gpu_rejects_roof_border_open_fallback() {
    let (device, queue) = crate::gfx3d::cull::tests::headless()
        .expect("ground rain-edge acceptance requires a real GPU device");
    let mut composer = super::build_composer();
    let module = composer
        .make_naga_module(NagaModuleDescriptor {
            source: PROBE,
            file_path: "ground_rain_border_actual_probe.wgsl",
            ..Default::default()
        })
        .unwrap_or_else(|e| panic!("{}", e.emit_to_string(&composer)));
    let shader = device.create_shader_module(wgpu::ShaderModuleDescriptor {
        label: Some("actual_ground_rain_border"),
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
    let output = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
        label: None,
        contents: bytemuck::cast_slice(&[0.0_f32; 32 * 4]),
        usage: wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_SRC,
    });
    let staging = device.create_buffer(&wgpu::BufferDescriptor {
        label: None,
        size: 32 * 4 * 4,
        usage: wgpu::BufferUsages::MAP_READ | wgpu::BufferUsages::COPY_DST,
        mapped_at_creation: false,
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
        pass.dispatch_workgroups(1, 1, 1);
    }
    encoder.copy_buffer_to_buffer(&output, 0, &staging, 0, 32 * 4 * 4);
    queue.submit(Some(encoder.finish()));
    let slice = staging.slice(..);
    let (send, receive) = std::sync::mpsc::channel();
    slice.map_async(wgpu::MapMode::Read, move |r| {
        let _ = send.send(r);
    });
    device.poll(wgpu::PollType::wait_indefinitely()).unwrap();
    receive.recv().unwrap().unwrap();
    let bytes = slice.get_mapped_range();
    let values = bytemuck::cast_slice::<u8, f32>(&bytes);
    let mut falsified_old_gate = false;
    for (i, sample) in values.chunks_exact(4).enumerate() {
        assert!(sample.iter().all(|v| v.is_finite()));
        let partial_t = (0.75_f32 - 0.6) / (0.95 - 0.6);
        let partial = partial_t * partial_t * (3.0 - 2.0 * partial_t);
        let expected = match i / 8 {
            0 | 2 => 0.0,
            1 => sample[1],
            _ => sample[1] * partial,
        };
        assert!(
            (sample[0] - expected).abs() < 1e-5,
            "actual rain cover case {i}"
        );
        if i / 8 == 0 {
            assert_eq!(
                sample[0], 0.0,
                "a roof stays dry at every map-edge validity"
            );
            falsified_old_gate |= sample[2] > 0.01;
        }
        if i % 8 == 0 {
            assert_eq!(sample[0], 0.0, "unknown coverage is never exposure proof");
        }
    }
    assert!(
        falsified_old_gate,
        "fixture must expose the old sheltered-edge wetness defect"
    );
    drop(bytes);
    staging.unmap();
}
