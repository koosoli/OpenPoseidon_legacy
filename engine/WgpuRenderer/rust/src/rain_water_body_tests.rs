//! Real production body-lighting helper probes, including the captured rain
//! lighting inputs. They accept radiometric contracts, not installed appearance.
use wgpu::util::DeviceExt;

fn body_source() -> &'static str {
    let source = include_str!("shaders/rain_water_body.wgsl");
    &source[source.find("fn rain_water_body_radiance(").unwrap()..]
}
fn optics_source() -> &'static str {
    let source = include_str!("rain_water.wgsl");
    let start = source.find("fn rain_water_optics(").unwrap();
    let end = start + source[start..].find("struct VsOut").unwrap();
    &source[start..end]
}
#[test]
fn actual_body_helper_is_resource_free_and_called_with_physical_sky_by_fragment() {
    let module = naga::front::wgsl::parse_str(body_source()).unwrap();
    naga::valid::Validator::new(
        naga::valid::ValidationFlags::all(),
        naga::valid::Capabilities::all(),
    )
    .validate(&module)
    .unwrap();
    assert!(module.global_variables.is_empty());
    let mut composer = crate::shaders::build_composer();
    let module = composer
        .make_naga_module(naga_oil::compose::NagaModuleDescriptor {
            source: include_str!("rain_water.wgsl"),
            file_path: "rain_water.wgsl",
            ..Default::default()
        })
        .unwrap_or_else(|error| panic!("{}", error.emit_to_string(&composer)));
    let fragment = &module
        .entry_points
        .iter()
        .find(|e| e.name == "fs_main")
        .unwrap()
        .function;
    fn calls(body: &naga::Block, module: &naga::Module, name: &str) -> bool {
        body.iter().any(|statement| match statement {
            naga::Statement::Call { function, .. } => module.functions[*function]
                .name
                .as_deref()
                .is_some_and(|n| n.contains(name)),
            naga::Statement::If { accept, reject, .. } => {
                calls(accept, module, name) || calls(reject, module, name)
            }
            _ => false,
        })
    }
    assert!(calls(&fragment.body, &module, "rain_water_body_radiance"));
    assert!(calls(&fragment.body, &module, "sky_irradiance"));
    // The production call uses the same surface normal and sky-mode/scale
    // lanes as terrain. A standalone test-only body is not sufficient proof.
    let source = include_str!("rain_water.wgsl");
    assert!(source.contains("sky_body = sky_irradiance(n)"));
    assert!(
        source.contains(
            "rain_water_body_radiance(frame.sun_diffuse.w, sky_body, frame.sun_ambient.w"
        )
    );
}

#[test]
fn actual_gpu_body_lighting_replays_rain_and_excludes_legacy_ambient_on_physical_path() {
    let (device, queue) =
        crate::gfx3d::cull::tests::headless().expect("Actual body-lighting probe needs a GPU");
    let source = format!(
        r#"{}
{}
@group(0) @binding(0) var<storage,read_write> result: array<vec4<f32>>;
@compute @workgroup_size(16)
fn cs_probe(@builtin(global_invocation_id) id:vec3<u32>) {{
    let i=id.x;if(i>=12u){{return;}}
    // Captured physical rain lighting from the installed 6cf9973e campaign.
    var legacy=vec3<f32>(22.1267,24.138218,32.184288);
    var sun=vec3<f32>(1.0355144,0.9062767,0.60597956);
    // A known explicit SH irradiance fixture, not a claim that the installed
    // campaign's unavailable SH coefficients equal these values.
    var sky=vec3<f32>(1.0,0.8,0.6);
    var physical=1.0;var cosine=0.0;var mu=1.0;var depth=0.54075;var coverage=1.0;
    var reflected=vec3<f32>(4.0,8.0,16.0);
    if(i==1u){{legacy*=10.0;}}
    if(i==2u){{physical=0.0;}}
    if(i==3u){{physical=0.0;legacy=vec3<f32>(0.0);sun=vec3<f32>(0.0);}}
    if(i==4u){{sky=vec3<f32>(0.001,0.002,0.003);sun=vec3<f32>(0.0);reflected=vec3<f32>(0.001,0.002,0.003);}}
    if(i==5u){{sun=vec3<f32>(20.714035,18.136463,12.143179);cosine=1.0;}}
    if(i==6u){{cosine=0.58894414;}}
    if(i==7u){{mu=0.15;}}
    if(i==8u){{depth=0.0;}}
    if(i==9u){{coverage=0.0;}}
    if(i==10u){{sky=vec3<f32>(-1.0);sun=vec3<f32>(-2.0);cosine=-1.0;}}
    if(i==11u){{sky=vec3<f32>(1e30);sun=vec3<f32>(0.0);}}
    let body=rain_water_body_radiance(physical,sky,1.35,legacy,sun,cosine);
    let optics=rain_water_optics(depth,mu,reflected,body,coverage);
    let bed=vec3<f32>(0.05,0.035,0.02);
    result[i*3u]=vec4<f32>(body,1.0);
    result[i*3u+1u]=optics;
    result[i*3u+2u]=vec4<f32>(optics.rgb*optics.a+bed*(1.0-optics.a),1.0);
}}
"#,
        body_source(),
        optics_source()
    );
    let shader = device.create_shader_module(wgpu::ShaderModuleDescriptor {
        label: Some("actual_rain_body_replay"),
        source: wgpu::ShaderSource::Wgsl(source.into()),
    });
    let pipeline = device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
        label: Some("actual_rain_body_replay"),
        layout: None,
        module: &shader,
        entry_point: Some("cs_probe"),
        compilation_options: Default::default(),
        cache: None,
    });
    let bytes = 36 * 16;
    let output = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
        label: Some("actual_rain_body_values"),
        contents: &vec![0u8; bytes],
        usage: wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_SRC,
    });
    let readback = device.create_buffer(&wgpu::BufferDescriptor {
        label: Some("actual_rain_body_readback"),
        size: bytes as u64,
        usage: wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::MAP_READ,
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
    encoder.copy_buffer_to_buffer(&output, 0, &readback, 0, bytes as u64);
    queue.submit([encoder.finish()]);
    let (sender, receiver) = std::sync::mpsc::channel();
    readback
        .slice(..)
        .map_async(wgpu::MapMode::Read, move |r| sender.send(r).unwrap());
    device.poll(wgpu::PollType::wait_indefinitely()).unwrap();
    receiver.recv().unwrap().unwrap();
    let mapped = readback.slice(..).get_mapped_range();
    let rows: Vec<[f32; 4]> = bytemuck::cast_slice(&mapped).to_vec();
    drop(mapped);
    readback.unmap();
    assert!(rows.iter().flatten().all(|v| v.is_finite() && *v >= 0.0));
    // Physical sky contract cannot be contaminated by the legacy flat fill.
    assert_eq!(rows[0], rows[3]);
    let expected = [0.06075, 0.0648, 0.04212];
    for channel in 0..3 {
        assert!((rows[0][channel] - expected[channel]).abs() < 1e-6);
        assert!(
            rows[6][channel] > rows[0][channel] * 10.0,
            "captured old flat fill must reproduce the bright body"
        );
        assert_eq!(rows[9][channel], 0.0);
        assert!(rows[12][channel] < 0.001);
        assert!(rows[15][channel] > rows[0][channel]);
        assert!(rows[18][channel] > rows[0][channel]);
        assert!(rows[18][channel] < rows[6][channel] * 0.15);
        assert_eq!(rows[30][channel], 0.0);
        assert!(rows[33][channel] > 1e28);
    }
    // The same actual optics retains deep/shallow/zero coverage and glancing
    // behaviour; fixing illumination cannot substitute an alpha or Fresnel hack.
    assert_eq!(rows[25], [0.0; 4]);
    assert_eq!(rows[28], [0.0; 4]);
    assert_eq!(rows[1][3], rows[7][3]);
    assert!(rows[22][3] > rows[1][3]);
    for channel in 0..3 {
        assert!(rows[8][channel] > rows[2][channel]);
    }
}
