use wgpu::util::DeviceExt;

const PROBE: &str = r#"
#import ground_wet::{soil_wet_amount, soil_wet_fresnel, soil_wet_sky_fraction, soil_wet_ground_reflection}
#import ground_puddles::ground_puddle_rain_factor
#import rain_water_optics::rain_water_standing_fraction
struct Edge {surface:vec4<f32>,eye:vec4<f32>,gradient_depth_wet:vec4<f32>};
@group(0) @binding(0) var<storage,read> edges:array<Edge>;
@group(0) @binding(1) var<storage,read_write> result:array<vec4<f32>>;
fn coat(b:vec3<f32>,s:vec3<f32>,v:f32,h:f32,width:f32,w:f32)->vec3<f32> {
 let f=soil_wet_fresnel(v);let r=soil_wet_ground_reflection(s,b,h,width);
 return mix(b,b*(1.-f)+r*f,w);
}
@compute @workgroup_size(64)
fn cs_probe(@builtin(global_invocation_id) id:vec3<u32>) {
 let i=id.x;if(i>=arrayLength(&edges)){return;}let e=edges[i];
 let view=normalize(e.eye.xyz-e.surface.xyz);let n=normalize(vec3<f32>(-e.gradient_depth_wet.x,1.,-e.gradient_depth_wet.y));
 let ray=reflect(-view,n);let nv=dot(n,view);
 // A flat broad-plane CONTROL, supported by the separately recorded constant
 // native bed. It is not a readback of every game's 6.25m filtered stencil.
 let horizon=dot(ray,vec3<f32>(0.,1.,0.));
 let wet=soil_wet_amount(e.surface.w,e.gradient_depth_wet.w,0.);
 // Radiometric test inputs: actual representative env-probe downward colour,
 // and a synthetic lit substrate. Neither is an installed HDR pixel claim.
 let sky=vec3<f32>(0.76464844,1.0166016,1.2548828);let b=vec3<f32>(0.04,0.025,0.012);
 let f=soil_wet_fresnel(nv);let old=mix(b,b*(1.-f)+sky*f,wet);
 result[i*7u]=vec4<f32>(ray,soil_wet_sky_fraction(horizon,0.));
 result[i*7u+1u]=vec4<f32>(old,wet);
 result[i*7u+2u]=vec4<f32>(coat(b,sky,nv,horizon,0.,wet),rain_water_standing_fraction(e.gradient_depth_wet.z));
 result[i*7u+3u]=vec4<f32>(coat(b,sky,nv,horizon,0.02,wet),soil_wet_sky_fraction(horizon,0.02));
 result[i*7u+4u]=vec4<f32>(
  distance(coat(b,sky,nv,horizon,0.,soil_wet_amount(1.,0.,0.)),b),
  distance(coat(b,sky,nv,horizon,0.,soil_wet_amount(0.,1.,0.)),b),
  distance(coat(b,sky,nv,horizon,0.,soil_wet_amount(1.,1.,0.02)),b),
  distance(coat(b,sky,nv,horizon,0.,wet*ground_puddle_rain_factor(0.,1.)),b));
 result[i*7u+5u]=vec4<f32>(soil_wet_sky_fraction(-1.,0.),soil_wet_sky_fraction(1.,0.),
   soil_wet_sky_fraction(0.,0.1),soil_wet_sky_fraction(0.025,0.05));
 result[i*7u+6u]=vec4<f32>(
   soil_wet_sky_fraction(dot(vec3<f32>(0.8,-0.2,0.),vec3<f32>(0.6,0.8,0.)),0.),
   soil_wet_sky_fraction(dot(vec3<f32>(-0.8,0.2,0.),vec3<f32>(0.6,0.8,0.)),0.),
   length(soil_wet_ground_reflection(vec3<f32>(0.),vec3<f32>(0.),0.,0.1)),
   soil_wet_ground_reflection(vec3<f32>(100.),vec3<f32>(1.),1.,0.).x);
}
"#;

#[test]
fn local_reflection_composes_and_reuses_exact_normal_work() {
    let terrain = include_str!("../terrain/terrain.wgsl");
    let start = terrain.find("fn sample_mud_normals(").unwrap();
    let end = terrain[start..].find("fn sample_mud_normal(").unwrap() + start;
    let pair = &terrain[start..end];
    assert_eq!(pair.matches("sample_normal(").count(), 1);
    assert_eq!(pair.matches("mud_height_gradient(").count(), 2);
    assert_eq!(pair.matches("sand_height_gradient(").count(), 2);
    assert!(pair.contains(
        "normalize(vec3<f32>(base.x - delta.x * base.y, base.y, base.z - delta.y * base.y))"
    ));
    assert!(terrain.contains("return sample_mud_normals(world_xz, step, footprint).fine;"));
    assert!(terrain.contains("let moist_horizon_width = fwidth(moist_horizon);"));
    assert!(terrain.contains("soil_wet_ground_reflection(sky_reflected, rgb,"));
    assert!(terrain.contains("dot(samples[i].xyz, ground_normals.broad), moist_cone_horizon_width)"));
    assert_eq!(
        terrain.matches("soil_wet_ground_reflection(").count(),
        1,
        "Only moist soil changes"
    );
    let mut composer = crate::shaders::build_composer();
    for (source, file_path) in [
        (PROBE, "local_ground_reflection.wgsl"),
        (terrain, "terrain.wgsl"),
        (include_str!("../rain_water.wgsl"), "rain_water.wgsl"),
    ] {
        composer
            .make_naga_module(naga_oil::compose::NagaModuleDescriptor {
                source,
                file_path,
                ..Default::default()
            })
            .unwrap_or_else(|e| panic!("{}", e.emit_to_string(&composer)));
    }
}

#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
struct Edge {
    surface: [f32; 4],
    eye: [f32; 4],
    gradient_depth_wet: [f32; 4],
}
fn actual_edges() -> Vec<Edge> {
    let path =
        std::env::var("WGR_WET_SOIL_EDGE_RECEIPT").expect("Actual root-exported edge CSV required");
    let text = std::fs::read_to_string(path).unwrap();
    let mut rows = Vec::new();
    for line in text
        .lines()
        .filter(|l| !l.starts_with('#') && !l.trim().is_empty())
    {
        let v = line
            .split(',')
            .map(|v| v.parse::<f32>().unwrap())
            .collect::<Vec<_>>();
        assert_eq!(v.len(), 11);
        assert!(v.iter().all(|v| v.is_finite() && v.abs() < 1000000.));
        assert!(v[7] == 0. || v[7] == 1.);
        assert!((0. ..=1.).contains(&v[6]));
        assert!((0. ..=64.).contains(&v[5]));
        rows.push(Edge {
            surface: [v[0], v[1], v[2], v[7]],
            eye: [v[8], v[9], v[10], 0.],
            gradient_depth_wet: [v[3], v[4], v[5], v[6]],
        });
    }
    assert_eq!(rows.len(), 289);
    rows
}

#[test]
#[ignore = "Actual paused edge CSV required; root owns device"]
fn actual_gpu_recorded_edges_remove_unavailable_sky_without_extra_energy() {
    let edges = actual_edges();
    let (device, queue) = crate::gfx3d::cull::tests::headless().expect("Actual device required");
    let mut composer = crate::shaders::build_composer();
    let shader = crate::shaders::make_module(
        &device,
        &mut composer,
        "local_soil",
        PROBE,
        "local_soil.wgsl",
    );
    let pipeline = device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
        label: None,
        layout: None,
        module: &shader,
        entry_point: Some("cs_probe"),
        compilation_options: Default::default(),
        cache: None,
    });
    let input = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
        label: None,
        contents: bytemuck::cast_slice(&edges),
        usage: wgpu::BufferUsages::STORAGE,
    });
    let bytes = (edges.len() * 7 * 16) as u64;
    let output = device.create_buffer(&wgpu::BufferDescriptor {
        label: None,
        size: bytes,
        usage: wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_SRC,
        mapped_at_creation: false,
    });
    let staging = device.create_buffer(&wgpu::BufferDescriptor {
        label: None,
        size: bytes,
        usage: wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::MAP_READ,
        mapped_at_creation: false,
    });
    let bind = device.create_bind_group(&wgpu::BindGroupDescriptor {
        label: None,
        layout: &pipeline.get_bind_group_layout(0),
        entries: &[
            wgpu::BindGroupEntry {
                binding: 0,
                resource: input.as_entire_binding(),
            },
            wgpu::BindGroupEntry {
                binding: 1,
                resource: output.as_entire_binding(),
            },
        ],
    });
    let mut encoder = device.create_command_encoder(&Default::default());
    {
        let mut pass = encoder.begin_compute_pass(&Default::default());
        pass.set_pipeline(&pipeline);
        pass.set_bind_group(0, &bind, &[]);
        pass.dispatch_workgroups(5, 1, 1);
    }
    encoder.copy_buffer_to_buffer(&output, 0, &staging, 0, bytes);
    queue.submit([encoder.finish()]);
    let (tx, rx) = std::sync::mpsc::channel();
    staging
        .slice(..)
        .map_async(wgpu::MapMode::Read, move |r| tx.send(r).unwrap());
    device.poll(wgpu::PollType::wait_indefinitely()).unwrap();
    rx.recv().unwrap().unwrap();
    let data = staging.slice(..).get_mapped_range();
    let rows: &[[f32; 4]] = bytemuck::cast_slice(&data);
    let b = [0.04, 0.025, 0.012];
    let mut blocked = 0;
    let mut retained = 0;
    for i in 0..edges.len() {
        let r = &rows[i * 7..i * 7 + 7];
        assert!(r.iter().flatten().all(|v| v.is_finite()));
        assert!(r[3][3] >= 0. && r[3][3] <= 1.);
        assert_eq!(r[4], [0.; 4], "Dry/hard/snow/roof source gates at {i}");
        assert_eq!(
            r[2][3], 0.,
            "Recorded thin interface remains nonstanding at {i}"
        );
        assert_eq!(
            r[5],
            [0., 1., 0.5, 0.75],
            "Derivative-filtered horizon at {i}"
        );
        assert_eq!(
            r[6],
            [1., 0., 0., 100.],
            "Actual sloped hemisphere, night and unclamped HDR at {i}"
        );
        if r[0][1] < -0.000001 && r[1][3] > 0. {
            blocked += 1;
            assert_eq!(r[0][3], 0.);
            for c in 0..3 {
                assert!(
                    (r[2][c] - b[c]).abs() < 1e-6,
                    "Blocked sky uses existing substrate at {i}/{c}"
                );
                assert!(r[1][c] > r[2][c]);
            }
        }
        if r[0][1] > 0.000001 {
            retained += 1;
            for c in 0..3 {
                assert!(
                    (r[1][c] - r[2][c]).abs() < 1e-6,
                    "Available sky unchanged at {i}/{c}"
                );
            }
        }
        for c in 0..3 {
            assert!(
                r[3][c] >= b[c] - 1e-6 && r[3][c] <= r[1][c] + 1e-6,
                "Filtered convex radiance at {i}/{c}"
            );
        }
    }
    assert!(
        blocked > 0 && retained > 0,
        "Actual receipt must exercise causal blocked and available directions"
    );
    eprintln!(
        "WET_SOIL_LOCAL_GROUND_PROBE rows={} blocked={blocked} available={retained} scope=actual-owner-slopes-flat-native-plane-control-not-filtered-game-fragments",
        edges.len()
    );
    drop(data);
    staging.unmap();
}
