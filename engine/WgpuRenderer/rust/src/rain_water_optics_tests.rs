//! Actual millimetre-depth material helper; no simulated water or appearance claim.
use wgpu::util::DeviceExt;

fn production() -> String {
    include_str!("shaders/rain_water_optics.wgsl")
        .lines()
        .skip(1)
        .collect::<Vec<_>>()
        .join("\n")
}

#[test]
fn actual_thin_water_helper_is_pure_and_full_fragment_preserves_support_guards() {
    let module = naga::front::wgsl::parse_str(&production()).unwrap();
    naga::valid::Validator::new(
        naga::valid::ValidationFlags::all(),
        naga::valid::Capabilities::all(),
    )
    .validate(&module)
    .unwrap();
    assert!(module.global_variables.is_empty());
    let mut composer = crate::shaders::build_composer();
    let composed = composer
        .make_naga_module(naga_oil::compose::NagaModuleDescriptor {
            source: include_str!("rain_water.wgsl"),
            file_path: "rain_water.wgsl",
            ..Default::default()
        })
        .unwrap_or_else(|e| panic!("{}", e.emit_to_string(&composer)));
    assert!(composed.functions.iter().any(|(_, f)| {
        f.name
            .as_deref()
            .is_some_and(|n| n.contains("rain_water_surface_optics"))
    }));
    let source = include_str!("rain_water.wgsl");
    assert!(source.contains("let local_depth = rain_water_raster_support_depth(water,bed,frame.ground_weather.z,in.world.y)"));
    assert!(source.contains("if (local_depth <= 0.001) { discard; }"));
    assert!(source.contains("if (cover <= 0.0 || snow > 0.002) { discard; }"));
    assert!(source.contains("let standing = rain_water_standing_fraction(local_depth)"));
    assert!(source.contains("(ripple.x + flow_slope.x)*standing"));
    assert!(source.contains("if (standing > 0.0)"));
    assert!(source.contains("rain_water_impact_normal(in.world.xz,rainwater.domain.w"));
    assert!(!source.contains("ground_puddle_ripple_normal"));
    assert!(source.contains("rain_water_sun_specular("));
    assert!(source.contains("shadow_strength(rel,vec3<f32>(0.0,1.0,0.0),fog,dwx,dwy)"));
    assert!(source.contains("cloud_sun_shadow(in.world.xz)"));
    assert!(
        source
            .contains("rain_water_surface_optics(local_depth,view_cosine,reflected,body,coverage)")
    );
    // One helper controls both coarse/fine fs_main: no alternative optical
    // cutoff. Medium metadata is a separate render resource, not simulation payload.
    assert_eq!(source.matches("fn fs_main(").count(), 1);
    assert_eq!(
        source
            .matches("rain_water_optics(local_depth, dot(n,view)")
            .count(),
        1
    );
}

#[test]
fn actual_gpu_irregular_impacts_are_continuous_filtered_and_sun_lobe_is_bounded() {
    let (device, queue) = crate::gfx3d::cull::tests::headless()
        .expect("Actual rain impact / sun lobe probe needs a GPU");
    let source = format!(
        r#"{}
@group(0) @binding(0) var<storage,read_write> result:array<vec4<f32>>;
@compute @workgroup_size(64)
fn cs_probe(@builtin(global_invocation_id) id:vec3<u32>){{
 let i=id.x;if(i>=64u){{return;}}
 let p=vec2<f32>(f32(i%8u)*0.75, f32(i/8u)*0.113-1.1);
 let n=rain_water_impact_normal(p,1.73,1.0,0.002);
 result[i*7u]=vec4<f32>(n,length(n));
 result[i*7u+1u]=vec4<f32>(rain_water_impact_normal(p+vec2<f32>(0.00001,0.0),1.73,1.0,0.002),0.0);
 result[i*7u+2u]=vec4<f32>(rain_water_impact_normal(p-vec2<f32>(0.00001,0.0),1.73,1.0,0.002),0.0);
 result[i*7u+3u]=vec4<f32>(rain_water_impact_normal(p,1.73001,1.0,0.002),0.0);
 result[i*7u+4u]=vec4<f32>(rain_water_impact_normal(p,1.73,0.0,0.002),0.0);
 result[i*7u+5u]=vec4<f32>(rain_water_impact_normal(p,1.73,1.0,0.16),0.0);
 let c=f32(i)/63.0;
 result[i*7u+6u]=vec4<f32>(rain_water_sun_specular(1.0,1.0,c,1.0,0.0),
 rain_water_sun_specular(1.0,1.0,c,1.0,0.5),
 rain_water_sun_specular(c,0.0,1.0,c,0.0),
 rain_water_sun_specular(0.0,c,1.0,c,0.0));
}}
"#,
        production()
    );
    let shader = device.create_shader_module(wgpu::ShaderModuleDescriptor {
        label: Some("actual_irregular_water_impacts_and_sun"),
        source: wgpu::ShaderSource::Wgsl(source.into()),
    });
    let pipeline = device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
        label: None,
        layout: None,
        module: &shader,
        entry_point: Some("cs_probe"),
        compilation_options: Default::default(),
        cache: None,
    });
    let bytes = 64 * 7 * 16;
    let output = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
        label: None,
        contents: &vec![0u8; bytes],
        usage: wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_SRC,
    });
    let readback = device.create_buffer(&wgpu::BufferDescriptor {
        label: None,
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
    let (tx, rx) = std::sync::mpsc::channel();
    readback
        .slice(..)
        .map_async(wgpu::MapMode::Read, move |r| tx.send(r).unwrap());
    device.poll(wgpu::PollType::wait_indefinitely()).unwrap();
    rx.recv().unwrap().unwrap();
    let mapped = readback.slice(..).get_mapped_range();
    let rows: Vec<[f32; 4]> = bytemuck::cast_slice(&mapped).to_vec();
    drop(mapped);
    readback.unmap();
    assert!(rows.iter().flatten().all(|v| v.is_finite()));
    let mut active = 0;
    for i in 0..64 {
        let n = rows[i * 7];
        assert!((n[3] - 1.0).abs() < 0.00001 && n[1] > 0.85);
        active += usize::from(n[0].abs() + n[2].abs() > 0.0001);
        for k in [0, 1, 2] {
            assert!(
                (rows[i * 7 + 1][k] - rows[i * 7 + 2][k]).abs() < 0.003,
                "cell seam at {i}/{k}"
            );
            assert!(
                (n[k] - rows[i * 7 + 3][k]).abs() < 0.003,
                "time jump at {i}/{k}"
            );
        }
        assert_eq!(rows[i * 7 + 4], [0.0, 1.0, 0.0, 0.0]);
        assert_eq!(rows[i * 7 + 5], [0.0, 1.0, 0.0, 0.0]);
        let sun = rows[i * 7 + 6];
        assert!(sun[0] >= 0.0 && sun[1] >= 0.0);
        assert_eq!(&sun[2..], &[0.0, 0.0]);
    }
    assert!(
        active > 3 && active < 60,
        "independent events need active and inactive areas: {active}"
    );
    // Broad finite normal-incidence sun peak; more unresolved variance lowers
    // that peak. No forced brighter body, sky multiplier or HDR colour clamp.
    let peak = rows[63 * 7 + 6];
    assert!(peak[0] > 0.1 && peak[0] < 1.0 && peak[1] < peak[0]);
    for i in 1..64 {
        assert!(rows[i * 7 + 6][0] >= rows[(i - 1) * 7 + 6][0]);
    }
}

#[test]
fn actual_gpu_millimetre_film_does_not_repeat_the_grazing_sky_coat() {
    let (device, queue) =
        crate::gfx3d::cull::tests::headless().expect("Actual thin-film optical probe needs a GPU");
    let source = format!(
        r#"{}
@group(0) @binding(0) var<storage,read_write> result:array<vec4<f32>>;
@compute @workgroup_size(32)
fn cs_probe(@builtin(global_invocation_id) id:vec3<u32>){{
 let i=id.x;if(i>=32u){{return;}}
 let depths=array<f32,16>(0.0,0.001,0.002,0.003,0.00799,0.008,0.00801,0.012,0.020,0.029,0.040,0.04999,0.050,0.05001,0.30,1.0);
 let depth=depths[i%16u];let mu=select(0.1,1.0,i>=16u);
 let body=vec3<f32>(0.05);let sky=vec3<f32>(20.0);let bed=vec3<f32>(0.10);
 let coverage=smoothstep(0.001,0.012,depth);
 let optics=rain_water_surface_optics(depth,mu,sky,body,coverage);
 result[i*3u]=optics;
 result[i*3u+1u]=vec4<f32>(optics.rgb*optics.a+bed*(1.0-optics.a),rain_water_standing_fraction(depth));
 result[i*3u+2u]=rain_water_surface_optics(depth,mu,sky,body,0.0);
}}
"#,
        production()
    );
    let shader = device.create_shader_module(wgpu::ShaderModuleDescriptor {
        label: Some("actual_thin_water_optics"),
        source: wgpu::ShaderSource::Wgsl(source.into()),
    });
    let pipeline = device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
        label: None,
        layout: None,
        module: &shader,
        entry_point: Some("cs_probe"),
        compilation_options: Default::default(),
        cache: None,
    });
    let bytes = 32 * 3 * 16;
    let output = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
        label: None,
        contents: &vec![0u8; bytes],
        usage: wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_SRC,
    });
    let readback = device.create_buffer(&wgpu::BufferDescriptor {
        label: None,
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
    let (tx, rx) = std::sync::mpsc::channel();
    readback
        .slice(..)
        .map_async(wgpu::MapMode::Read, move |r| tx.send(r).unwrap());
    device.poll(wgpu::PollType::wait_indefinitely()).unwrap();
    rx.recv().unwrap().unwrap();
    let mapped = readback.slice(..).get_mapped_range();
    let rows: Vec<[f32; 4]> = bytemuck::cast_slice(&mapped).to_vec();
    drop(mapped);
    readback.unmap();
    assert!(rows.iter().flatten().all(|v| v.is_finite() && *v >= 0.0));
    for i in 0..32 {
        assert_eq!(rows[i * 3 + 2], [0.0; 4]);
        assert!(rows[i * 3][3] <= 1.0);
    }
    // At <=8mm there is no second smooth mirror: the destination's already
    // shaded wet bed survives. This falsifies the captured 3mm/+1.047HDR defect.
    for i in 1..6 {
        assert!(rows[i * 3 + 1][0] <= 0.100001);
        assert_eq!(rows[i * 3 + 1][3], 0.0);
        assert_eq!(rows[i * 3 + 1], rows[(i + 16) * 3 + 1]);
    }
    assert!(rows[3 * 3 + 1][0] > 0.099 && rows[3 * 3 + 1][0] <= 0.1);
    assert!((rows[9 * 3 + 1][3] - 0.5).abs() < 1e-5);
    for i in 1..16 {
        assert!(rows[i * 3 + 1][3] >= rows[(i - 1) * 3 + 1][3]);
    }
    // No discontinuity where film leaves 8mm or reaches 50mm.
    assert!((rows[6 * 3 + 1][0] - rows[5 * 3 + 1][0]).abs() < 0.001);
    assert!((rows[13 * 3 + 1][0] - rows[12 * 3 + 1][0]).abs() < 0.001);
    // Genuine pools retain the old energy-conserving Schlick/Beer-Lambert
    // law exactly at top and low angles, including HDR and no-rain persistence.
    for (i, depth) in [(12, 0.05f32), (14, 0.30), (15, 1.0)] {
        for (base, mu) in [(0, 0.1f32), (16, 1.0)] {
            let fresnel = 0.025 + 0.975 * (1.0 - mu).powi(5);
            let transmission = (-3.0 * depth).exp();
            let expected = 20.0 * fresnel
                + (1.0 - fresnel) * (0.05 * (1.0 - transmission) + 0.10 * transmission);
            assert!((rows[(i + base) * 3 + 1][0] - expected).abs() < 2e-5);
            assert_eq!(rows[(i + base) * 3 + 1][3], 1.0);
        }
    }
}
