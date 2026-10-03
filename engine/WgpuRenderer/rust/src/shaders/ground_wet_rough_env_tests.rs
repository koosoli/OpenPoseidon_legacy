use wgpu::util::DeviceExt;
const PROBE: &str = r#"
#import ground_wet::{soil_wet_environment_samples, soil_wet_ground_reflection, soil_wet_sun_alpha, soil_wet_fresnel}
@group(0) @binding(0) var<storage,read_write> result:array<vec4<f32>>;
fn clear_feature(d:vec3<f32>, mirror:vec3<f32>)->vec3<f32> {
 return vec3<f32>(0.1,0.2,0.4)+vec3<f32>(8.,3.,1.)*pow(max(dot(d,mirror),0.),96.);
}
fn cloud_feature(d:vec3<f32>)->vec3<f32> {
 return vec3<f32>(0.2,0.22,0.25)+vec3<f32>(1.)*select(0.,1.,d.y>0.6);
}
@compute @workgroup_size(64)
fn cs_probe(@builtin(global_invocation_id) id:vec3<u32>){
 let i=id.x;if(i>=192u){return;}
 let wet=f32(i%3u)*0.5;let variation=f32((i/3u)%2u)*0.3;
 let cosine=array<f32,4>(0.03,0.2,0.6,1.)[(i/6u)%4u];let phi=f32((i/24u)%4u)*1.57079632679;
 let view=vec3<f32>(sqrt(1.-cosine*cosine)*cos(phi),cosine,sqrt(1.-cosine*cosine)*sin(phi));
 let n=normalize(select(vec3<f32>(0.,1.,0.),vec3<f32>(0.55,1.,0.1),i>=96u));
 let broad=vec3<f32>(0.,1.,0.);let mirror=reflect(-view,n);
 let samples=soil_wet_environment_samples(view,n,wet,variation);
 let b=vec3<f32>(0.04,0.025,0.012);var clear=vec3<f32>(0.);var cloud=vec3<f32>(0.);
 var black=vec3<f32>(0.);var hdr=vec3<f32>(0.);var sum=0.;var spread=0.;var blocked=0.;var min_weight=1.;
 for(var j=0u;j<8u;j+=1u){let s=samples[j];result[i*12u+j]=s;
  let horizon=dot(s.xyz,broad);let width=0.02;
  clear+=soil_wet_ground_reflection(clear_feature(s.xyz,mirror),b,horizon,width)*s.w;
  cloud+=soil_wet_ground_reflection(cloud_feature(s.xyz),b,horizon,width)*s.w;
  black+=soil_wet_ground_reflection(vec3<f32>(0.),vec3<f32>(0.),horizon,width)*s.w;
  hdr+=soil_wet_ground_reflection(vec3<f32>(100.),vec3<f32>(100.),horizon,width)*s.w;
  sum+=s.w;spread+=(1.-dot(mirror,s.xyz))*s.w;min_weight=min(min_weight,s.w);
  blocked+=select(0.,s.w,horizon<0.);
 }
 result[i*12u+8u]=vec4<f32>(clear,sum);
 result[i*12u+9u]=vec4<f32>(cloud,spread);
 result[i*12u+10u]=vec4<f32>(black,length(hdr));
 result[i*12u+11u]=vec4<f32>(blocked,dot(n,view),soil_wet_sun_alpha(wet),min_weight);
}
"#;

#[test]
fn rough_environment_composes_with_derivatives_outside_material_loops() {
    let mut composer = crate::shaders::build_composer();
    for (source, path) in [
        (PROBE, "rough_soil_env.wgsl"),
        (include_str!("../terrain/terrain.wgsl"), "terrain.wgsl"),
    ] {
        composer
            .make_naga_module(naga_oil::compose::NagaModuleDescriptor {
                source,
                file_path: path,
                ..Default::default()
            })
            .unwrap_or_else(|e| panic!("{}", e.emit_to_string(&composer)));
    }
    let terrain = include_str!("../terrain/terrain.wgsl");
    assert!(
        terrain.find("let moist_cone_horizon_width").unwrap()
            < terrain.find("if (soil_wet > 0.0)").unwrap()
    );
    let start = terrain
        .find("let samples = soil_wet_environment_samples")
        .unwrap();
    let end = terrain[start..].find("let half_vector").unwrap() + start;
    let body = &terrain[start..end];
    assert!(!body.contains("fwidth(") && !body.contains("sample_normal("));
    assert!(body.contains("dot(samples[i].xyz, ground_normals.broad)"));
    assert!(terrain.contains("rgb * (1.0 - fresnel) + reflected * fresnel"));
}

#[test]
fn actual_gpu_rough_soil_environment_is_filtered_convex_and_locally_visible() {
    let (device, queue) = crate::gfx3d::cull::tests::headless().expect("Actual device required");
    let mut composer = crate::shaders::build_composer();
    let shader = crate::shaders::make_module(
        &device,
        &mut composer,
        "soil_rough_env",
        PROBE,
        "soil_rough_env.wgsl",
    );
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
        contents: &vec![0u8; 192 * 12 * 16],
        usage: wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_SRC,
    });
    let staging = device.create_buffer(&wgpu::BufferDescriptor {
        label: None,
        size: 192 * 12 * 16,
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
        pass.dispatch_workgroups(3, 1, 1);
    }
    encoder.copy_buffer_to_buffer(&output, 0, &staging, 0, 192 * 12 * 16);
    queue.submit([encoder.finish()]);
    let (tx, rx) = std::sync::mpsc::channel();
    staging
        .slice(..)
        .map_async(wgpu::MapMode::Read, move |r| tx.send(r).unwrap());
    device.poll(wgpu::PollType::wait_indefinitely()).unwrap();
    rx.recv().unwrap().unwrap();
    let data = staging.slice(..).get_mapped_range();
    let rows: &[[f32; 4]] = bytemuck::cast_slice(&data);
    let mut ground_blocked = 0;
    let mut cloud_transition = 0;
    for i in 0..192 {
        let r = &rows[i * 12..i * 12 + 12];
        assert!(r.iter().flatten().all(|v| v.is_finite()), "case{i}");
        let sum = r[8][3];
        assert!(r[..8].iter().all(|s| s[3] >= 0. && s[3] <= 1.));
        if r[11][1] <= 0. {
            assert_eq!(sum, 0.);
            continue;
        }
        assert!((sum - 1.).abs() < 2e-6, "normalized case{i}");
        for s in &r[..8] {
            assert!(
                (s[0] * s[0] + s[1] * s[1] + s[2] * s[2] - 1.).abs() < 2e-6,
                "unit rays case{i}"
            );
        }
        let substrate = [0.04, 0.025, 0.012];
        let clear_max = [8.1, 3.2, 1.4];
        let cloud_max = [1.2, 1.22, 1.25];
        for c in 0..3 {
            assert!(
                r[8][c] >= substrate[c] - 2e-6 && r[8][c] <= clear_max[c] + 2e-6,
                "convex clear {i}/{c}"
            );
            assert!(
                r[9][c] >= substrate[c] - 2e-6 && r[9][c] <= cloud_max[c] + 2e-6,
                "convex cloudy {i}/{c}"
            );
        }
        assert_eq!(&r[10][..3], &[0.; 3], "zero-light case{i}");
        assert!(
            (r[10][3] - 100. * 3f32.sqrt()).abs() < 0.001,
            "unclamped HDR {i}"
        );
        if r[11][0] > 0. {
            ground_blocked += 1;
        }
        if r[9][0] > 0.20001 && r[9][0] < 1.19999 {
            cloud_transition += 1;
        }
    }
    assert!(
        ground_blocked > 0 && cloud_transition > 0,
        "Sloped local occlusion and actual kernel cloud-edge filtering exercised"
    );
    // Normal-view controls compare the same angular feature and no ground cutoff.
    // Wider GGX alpha / real derivative variance must spread the normalized lobe.
    for az in 0..4 {
        let narrow = (20 + 24 * az) * 12;
        let damp = (18 + 24 * az) * 12;
        let filtered = (23 + 24 * az) * 12;
        assert!(
            rows[damp + 9][3] > rows[narrow + 9][3] + 0.01,
            "alpha widens lobe az{az}"
        );
        assert!(
            rows[filtered + 9][3] > rows[narrow + 9][3] + 0.01,
            "derivative widens lobe az{az}"
        );
        assert!(
            rows[damp + 8][0] < rows[narrow + 8][0],
            "alpha filters sharp clear feature az{az}"
        );
        assert!(
            rows[filtered + 8][0] < rows[narrow + 8][0],
            "derivative filters sharp clear feature az{az}"
        );
    }
    eprintln!(
        "SOIL_ROUGH_ENV cases=192 flat/sloped=2 views=4 azimuths=4 wet-alpha=3 derivative=2 ground-blocked={ground_blocked} cloud-filtered={cloud_transition} scope=actual-kernel-controlled-radiance-not-installed-pixels"
    );
    drop(data);
    staging.unmap();
}
