//! Execute the production sampling functions on a single analytic wave. No
//! copied CPU implementation of the shader under test, and no game readback.
use wgpu::util::DeviceExt;

fn block<'a>(source: &'a str, declaration: &str) -> &'a str {
    let start = source.find(declaration).expect(declaration);
    let open = start + source[start..].find('{').unwrap();
    let mut depth = 0;
    for (offset, byte) in source[open..].bytes().enumerate() {
        if byte == b'{' {
            depth += 1;
        }
        if byte == b'}' {
            depth -= 1;
            if depth == 0 {
                return &source[start..=open + offset];
            }
        }
    }
    panic!("unclosed {declaration}")
}

#[test]
fn gpu_ocean_scale_normals_and_foam_follow_displacement() {
    let instance =
        wgpu::Instance::new(wgpu::InstanceDescriptor::new_without_display_handle_from_env());
    let Ok(adapter) = pollster::block_on(instance.request_adapter(&Default::default())) else {
        eprintln!("SKIP ocean GPU scale probe: no adapter (not runtime acceptance)");
        return;
    };
    if !adapter
        .features()
        .contains(wgpu::Features::FLOAT32_FILTERABLE)
    {
        eprintln!("SKIP ocean GPU scale probe: float32 filtering unavailable");
        return;
    }
    let (device, queue) = pollster::block_on(adapter.request_device(&wgpu::DeviceDescriptor {
        required_features: wgpu::Features::FLOAT32_FILTERABLE,
        ..Default::default()
    }))
    .unwrap();
    eprintln!("ocean scale probe: {:?}", adapter.get_info());
    let surface = include_str!("water.wgsl");
    let foam = include_str!("foam.wgsl");
    let mut source = String::from(
        r#"
struct WaterParams {
    wave_scale: f32, warp_amp: f32, sea_level: f32,
    fft_cascade_lengths: vec4<f32>, sea_params: vec4<f32>, debug_params: vec4<f32>,
    fft_control: vec4<f32>, fft_wind_sea: vec4<f32>, optics_params: vec4<f32>,
}
struct Frame { proj: mat4x4<f32>, cam_pos: vec4<f32>, }
var<private> wp: WaterParams;
var<private> water: WaterParams;
var<private> frame: Frame;
struct Config { enabled: u32, whitecap_threshold: f32, foam_scale: f32, }
struct Configs { config: array<Config,4>, }
var<private> cascades: Configs;
@group(0) @binding(0) var fft_displacement: texture_2d_array<f32>;
@group(0) @binding(1) var fft_dynamics: texture_2d_array<f32>;
@group(0) @binding(2) var fft_auxiliary: texture_2d_array<f32>;
@group(0) @binding(3) var fft_samp: sampler;
@group(0) @binding(4) var<storage,read_write> results: array<vec4<f32>>;
fn whitecap_wind_factor(wind: f32) -> f32 { return 1.0; }
"#,
    );
    source.push_str(
        &include_str!("fft_sampling.wgsl").replace("#define_import_path water_fft_sampling", ""),
    );
    source.push_str(block(surface, "struct CascadeWeights"));
    for name in [
        "cubic_weights",
        "texture_bicubic_displacement_at",
        "fft_sample",
        "compute_cascade_weights",
        "texture_bicubic_dynamics",
        "sample_fft_dynamics_filtered",
        "fft_normal_with_weights",
    ] {
        source.push_str(block(surface, &format!("fn {name}(")));
    }
    source.push_str(&block(foam, "fn fft_source(").replace("field_sampler", "fft_samp"));
    source.push_str(r#"
@compute @workgroup_size(32)
fn probe(@builtin(global_invocation_id) id: vec3<u32>) {
    if (id.x >= 288u) { return; }
    let scales = array<f32,3>(0.5,1.0,2.0);
    let scale = scales[id.x / 96u];
    let phase_sample = f32(id.x % 96u) + 0.37;
    wp = WaterParams();
    wp.wave_scale=scale;
    wp.fft_cascade_lengths=vec4<f32>(64.0,0.0,0.0,0.0);
    wp.debug_params.w=720.0;
    wp.fft_control.x=1.0;
    water=wp;
    frame.proj=mat4x4<f32>(vec4<f32>(1,0,0,0),vec4<f32>(0,1,0,0),vec4<f32>(0,0,1,0),vec4<f32>(0,0,0,1));
    cascades.config[0]=Config(1u,1.0,1.0);
    let p=vec2<f32>(phase_sample * 64.0 / 96.0 * scale, 0.0);
    let eps=0.02*scale;
    let disp=fft_sample(p,0,0.01);
    let n=fft_normal_with_weights(p,0.01,vec3<f32>(0.0,1.0,0.0));
    let fd=(fft_sample(p+vec2<f32>(eps,0),0,0.01).y-fft_sample(p-vec2<f32>(eps,0),0,0.01).y)/(2.0*eps);
    results[id.x*2u]=vec4<f32>(disp.y,-n.x/n.y,fd,fft_source(p));
    results[id.x*2u+1u]=vec4<f32>(p,scale,0.0);
}
"#);
    let shader = device.create_shader_module(wgpu::ShaderModuleDescriptor {
        label: Some("production ocean scale probe"),
        source: wgpu::ShaderSource::Wgsl(source.into()),
    });
    let pipeline = device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
        label: Some("ocean scale probe"),
        layout: None,
        module: &shader,
        entry_point: Some("probe"),
        compilation_options: Default::default(),
        cache: None,
    });
    let texture = |kind| {
        let texture = device.create_texture(&wgpu::TextureDescriptor {
            label: Some("single wave fixture"),
            size: wgpu::Extent3d {
                width: 256,
                height: 4,
                depth_or_array_layers: 4,
            },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: wgpu::TextureFormat::Rgba32Float,
            usage: wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::COPY_DST,
            view_formats: &[],
        });
        let data: Vec<[f32; 4]> = (0..256 * 4 * 4)
            .map(|i| {
                let phase = (i % 256) as f32 * std::f32::consts::TAU / 256.0;
                let h = phase.sin() * 2.0;
                match kind {
                    0 => [0.0, h, 0.0, h.max(0.0)],
                    1 => [
                        phase.cos() * 2.0 * std::f32::consts::TAU / 64.0,
                        0.0,
                        0.0,
                        0.0,
                    ],
                    _ => [1.0 - h.max(0.0) * 0.35, 0.0, 0.0, 0.0],
                }
            })
            .collect();
        queue.write_texture(
            texture.as_image_copy(),
            bytemuck::cast_slice(&data),
            wgpu::TexelCopyBufferLayout {
                offset: 0,
                bytes_per_row: Some(256 * 16),
                rows_per_image: Some(4),
            },
            texture.size(),
        );
        texture.create_view(&wgpu::TextureViewDescriptor {
            dimension: Some(wgpu::TextureViewDimension::D2Array),
            ..Default::default()
        })
    };
    let views = [texture(0), texture(1), texture(2)];
    let sampler = device.create_sampler(&wgpu::SamplerDescriptor {
        address_mode_u: wgpu::AddressMode::Repeat,
        address_mode_v: wgpu::AddressMode::Repeat,
        mag_filter: wgpu::FilterMode::Linear,
        min_filter: wgpu::FilterMode::Linear,
        ..Default::default()
    });
    let size = 288 * 2 * 16;
    let out = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
        label: Some("probe output"),
        contents: &vec![0; size],
        usage: wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_SRC,
    });
    let read = device.create_buffer(&wgpu::BufferDescriptor {
        label: Some("probe readback"),
        size: size as u64,
        usage: wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::MAP_READ,
        mapped_at_creation: false,
    });
    let group = device.create_bind_group(&wgpu::BindGroupDescriptor {
        label: None,
        layout: &pipeline.get_bind_group_layout(0),
        entries: &[
            wgpu::BindGroupEntry {
                binding: 0,
                resource: wgpu::BindingResource::TextureView(&views[0]),
            },
            wgpu::BindGroupEntry {
                binding: 1,
                resource: wgpu::BindingResource::TextureView(&views[1]),
            },
            wgpu::BindGroupEntry {
                binding: 2,
                resource: wgpu::BindingResource::TextureView(&views[2]),
            },
            wgpu::BindGroupEntry {
                binding: 3,
                resource: wgpu::BindingResource::Sampler(&sampler),
            },
            wgpu::BindGroupEntry {
                binding: 4,
                resource: out.as_entire_binding(),
            },
        ],
    });
    let mut encoder = device.create_command_encoder(&Default::default());
    {
        let mut pass = encoder.begin_compute_pass(&Default::default());
        pass.set_pipeline(&pipeline);
        pass.set_bind_group(0, &group, &[]);
        pass.dispatch_workgroups(9, 1, 1);
    }
    encoder.copy_buffer_to_buffer(&out, 0, &read, 0, size as u64);
    queue.submit([encoder.finish()]);
    let (tx, rx) = std::sync::mpsc::channel();
    read.slice(..)
        .map_async(wgpu::MapMode::Read, move |result| {
            tx.send(result).unwrap();
        });
    device.poll(wgpu::PollType::wait_indefinitely()).unwrap();
    rx.recv().unwrap().unwrap();
    let bytes = read.slice(..).get_mapped_range();
    let values: &[[f32; 4]] = bytemuck::cast_slice(&bytes);
    let mut errors = Vec::new();
    for (index, scale) in [0.5, 1.0, 2.0].into_iter().enumerate() {
        let mut normal_error = 0.0_f32;
        let mut foam_error = 0.0_f32;
        for i in 0..96 {
            let r = values[(index * 96 + i) * 2];
            normal_error = normal_error.max((r[1] - r[2]).abs());
            // Fixture's auxiliary Jacobian encodes injection on positive crests.
            let expected = (r[0] / scale).max(0.0) * 0.35 * 7.5;
            foam_error = foam_error.max((r[3] - expected).abs());
        }
        eprintln!(
            "scale={scale}: slope error={normal_error:.6}, crest/source error={foam_error:.6}"
        );
        if normal_error > 0.002 || foam_error > 0.005 {
            errors.push((scale, normal_error, foam_error));
        }
    }
    assert!(
        errors.is_empty(),
        "displacement/normal/foam mismatch: {errors:?}"
    );
}

#[test]
fn gpu_foam_coverage_equal_elapsed_time_and_pause() {
    let instance =
        wgpu::Instance::new(wgpu::InstanceDescriptor::new_without_display_handle_from_env());
    let Ok(adapter) = pollster::block_on(instance.request_adapter(&Default::default())) else {
        eprintln!("SKIP foam GPU probe: no adapter");
        return;
    };
    let (device, queue) = pollster::block_on(adapter.request_device(&Default::default())).unwrap();
    let mut source = block(include_str!("foam.wgsl"), "fn foam_coverage_step").to_owned();
    source.push_str(
        r#"
@group(0) @binding(0) var<storage,read_write> result: array<vec4<f32>>;
@compute @workgroup_size(1)
fn probe(@builtin(global_invocation_id) id: vec3<u32>) {
    let hz = array<u32,4>(30u,60u,120u,20u)[id.x];
    let dt = 1.0 / f32(hz);
    var c = vec3<f32>(0.2);
    for (var i=0u; i<hz*4u; i=i+1u) {
        c.x = foam_coverage_step(c.x, 1.0, 3.9, dt);
        c.y = foam_coverage_step(c.y, 0.7, 3.1, dt);
        c.z = foam_coverage_step(c.z, 1.2, 4.7, dt);
    }
    result[id.x*2u] = vec4<f32>(c, foam_coverage_step(c.x, 20.0, 3.9, 0.0));
    for (var i=0u; i<hz*4u; i=i+1u) {
        c.x = foam_coverage_step(c.x, 0.0, 3.9, dt);
        c.y = foam_coverage_step(c.y, 0.0, 3.1, dt);
        c.z = foam_coverage_step(c.z, 0.0, 4.7, dt);
    }
    result[id.x*2u+1u] = vec4<f32>(c, 0.0);
}
"#,
    );
    let shader = device.create_shader_module(wgpu::ShaderModuleDescriptor {
        label: None,
        source: wgpu::ShaderSource::Wgsl(source.into()),
    });
    let pipeline = device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
        label: None,
        layout: None,
        module: &shader,
        entry_point: Some("probe"),
        compilation_options: Default::default(),
        cache: None,
    });
    let output = device.create_buffer(&wgpu::BufferDescriptor {
        label: None,
        size: 128,
        usage: wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_SRC,
        mapped_at_creation: false,
    });
    let read = device.create_buffer(&wgpu::BufferDescriptor {
        label: None,
        size: 128,
        usage: wgpu::BufferUsages::MAP_READ | wgpu::BufferUsages::COPY_DST,
        mapped_at_creation: false,
    });
    let group = device.create_bind_group(&wgpu::BindGroupDescriptor {
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
        pass.set_bind_group(0, &group, &[]);
        pass.dispatch_workgroups(4, 1, 1);
    }
    encoder.copy_buffer_to_buffer(&output, 0, &read, 0, 128);
    queue.submit([encoder.finish()]);
    let (tx, rx) = std::sync::mpsc::channel();
    read.slice(..).map_async(wgpu::MapMode::Read, move |r| {
        tx.send(r).unwrap();
    });
    device.poll(wgpu::PollType::wait_indefinitely()).unwrap();
    rx.recv().unwrap().unwrap();
    let bytes = read.slice(..).get_mapped_range();
    let values: &[[f32; 4]] = bytemuck::cast_slice(&bytes);
    for i in 0..4 {
        eprintln!("foam cadence {}: {:?}", [30, 60, 120, 20][i], values[i * 2]);
        assert_eq!(values[i * 2][0], values[i * 2][3], "pause changed coverage");
        for j in 0..3 {
            assert!((values[i * 2][j] - values[0][j]).abs() < 1e-5);
            assert!(values[i * 2 + 1][j] < 1e-5);
        }
    }
}

#[test]
fn gpu_surf_arrival_bounds_and_exposure() {
    let instance =
        wgpu::Instance::new(wgpu::InstanceDescriptor::new_without_display_handle_from_env());
    let Ok(adapter) = pollster::block_on(instance.request_adapter(&Default::default())) else {
        eprintln!("SKIP surf GPU probe");
        return;
    };
    let (device, queue) = pollster::block_on(adapter.request_device(&Default::default())).unwrap();
    let surf = include_str!("shore_surf.wgsl");
    let mut source = block(surf, "struct SurfState").to_owned();
    source.push_str(block(surf, "fn surf_cycle"));
    source.push_str(block(surf, "fn add_surf_displacement"));
    source.push_str(block(surf, "fn surf_bed"));
    source.push_str(block(surf, "fn coastal_surf"));
    source.push_str(r#"
@group(0) @binding(0) var<storage,read_write> result: array<vec4<f32>>;
@group(0) @binding(1) var bed: texture_2d<f32>;
@compute @workgroup_size(1)
fn probe(@builtin(global_invocation_id) id: vec3<u32>) {
    let phase = f32(id.x) * 6.28318530718 / 16.0;
    let p = vec2<f32>(-phase / 0.18, 32.0);
    let s = surf_cycle(p,0.0,0.4,vec2<f32>(-1.0,0.0),0.06,1.0,1.0);
    let left = surf_cycle(p-vec2<f32>(0.01,0.0),0.0,0.4,vec2<f32>(-1.0,0.0),0.06,1.0,1.0);
    let right = surf_cycle(p+vec2<f32>(0.01,0.0),0.0,0.4,vec2<f32>(-1.0,0.0),0.06,1.0,1.0);
    result[id.x] = vec4<f32>(s.height,s.breaking,s.wash,abs(s.slope.x-(right.height-left.height)/0.02));
    if (id.x==16u) {let q=surf_cycle(p,0.0,0.4,vec2<f32>(-1.0,0.0),0.06,0.0,1.0);result[id.x]=vec4<f32>(q.height,q.breaking,q.wash,q.weight);}
    if (id.x==17u) {let q=surf_cycle(p,0.0,0.4,vec2<f32>(1.0,0.0),0.06,1.0,1.0);result[id.x]=vec4<f32>(q.height,q.breaking,q.wash,q.weight);}
    if (id.x==18u) {let q=surf_cycle(p,0.0,0.4,vec2<f32>(-1.0,0.0),0.9,1.0,1.0);result[id.x]=vec4<f32>(q.height,q.breaking,q.wash,q.weight);}
    if (id.x>=20u) {
        let origin = select(vec2<f32>(0.0),vec2<f32>(10000.0,12000.0), id.x==21u);
        let q = coastal_surf(bed,origin+select(vec2<f32>(40.0,32.0),vec2<f32>(-1.0,32.0),id.x==22u),origin,1.0,1.0,0.0,0.0,select(1.0,0.0,id.x==23u));
        result[id.x]=vec4<f32>(q.height,q.breaking,q.wash,q.weight);
    }
    if (id.x>=24u && id.x<28u) {
        let positions = array<vec2<f32>,4>(vec2<f32>(40.0,10.0),vec2<f32>(40.0,50.0),vec2<f32>(32.5,23.0),vec2<f32>(40.0,28.0));
        let q=coastal_surf(bed,positions[id.x-24u],vec2<f32>(0.0),1.0,1.0,0.0,0.0,1.0);
        result[id.x]=vec4<f32>(q.height,q.breaking,q.wash,q.weight);
    }
    if (id.x==28u) {
        var q: SurfState; q.weight=1.0; q.height=0.30;
        result[id.x]=vec4<f32>(add_surf_displacement(vec3<f32>(2.0,3.0,-1.0),q),1.0);
    }
    if (id.x==19u) {let q=surf_cycle(p,0.0,10.0,vec2<f32>(-1.0,0.0),0.06,1.0,1.0);result[id.x]=vec4<f32>(q.height,q.breaking,q.wash,q.weight);}
}
"#);
    let shader = device.create_shader_module(wgpu::ShaderModuleDescriptor {
        label: None,
        source: wgpu::ShaderSource::Wgsl(source.into()),
    });
    let pipeline = device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
        label: None,
        layout: None,
        module: &shader,
        entry_point: Some("probe"),
        compilation_options: Default::default(),
        cache: None,
    });
    let output = device.create_buffer(&wgpu::BufferDescriptor {
        label: None,
        size: 464,
        usage: wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_SRC,
        mapped_at_creation: false,
    });
    let read = device.create_buffer(&wgpu::BufferDescriptor {
        label: None,
        size: 464,
        usage: wgpu::BufferUsages::MAP_READ | wgpu::BufferUsages::COPY_DST,
        mapped_at_creation: false,
    });
    let bed = device.create_texture(&wgpu::TextureDescriptor {
        label: None,
        size: wgpu::Extent3d {
            width: 256,
            height: 64,
            depth_or_array_layers: 1,
        },
        mip_level_count: 1,
        sample_count: 1,
        dimension: wgpu::TextureDimension::D2,
        format: wgpu::TextureFormat::R32Float,
        usage: wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::COPY_DST,
        view_formats: &[],
    });
    let heights: Vec<f32> = (0..256 * 64)
        .map(|i| {
            let x = (i % 256) as f32;
            let z = (i / 256) as f32;
            if z < 20.0 && x >= 65.0 && x <= 80.0 {
                2.0
            } else if z >= 20.0 && z <= 25.0 {
                (32.0 - x) * 0.9
            } else if z > 25.0 && z <= 30.0 {
                -0.4
            } else if z > 45.0 {
                (32.0 + 5.0 * (z * 0.1).sin() - x) * 0.06
            } else {
                (32.0 - x) * 0.06
            }
        })
        .collect();
    queue.write_texture(
        bed.as_image_copy(),
        bytemuck::cast_slice(&heights),
        wgpu::TexelCopyBufferLayout {
            offset: 0,
            bytes_per_row: Some(1024),
            rows_per_image: Some(64),
        },
        bed.size(),
    );
    let bed_view = bed.create_view(&Default::default());
    let group = device.create_bind_group(&wgpu::BindGroupDescriptor {
        label: None,
        layout: &pipeline.get_bind_group_layout(0),
        entries: &[
            wgpu::BindGroupEntry {
                binding: 0,
                resource: output.as_entire_binding(),
            },
            wgpu::BindGroupEntry {
                binding: 1,
                resource: wgpu::BindingResource::TextureView(&bed_view),
            },
        ],
    });
    let mut encoder = device.create_command_encoder(&Default::default());
    {
        let mut pass = encoder.begin_compute_pass(&Default::default());
        pass.set_pipeline(&pipeline);
        pass.set_bind_group(0, &group, &[]);
        pass.dispatch_workgroups(29, 1, 1);
    }
    encoder.copy_buffer_to_buffer(&output, 0, &read, 0, 464);
    queue.submit([encoder.finish()]);
    let (tx, rx) = std::sync::mpsc::channel();
    read.slice(..).map_async(wgpu::MapMode::Read, move |r| {
        tx.send(r).unwrap();
    });
    device.poll(wgpu::PollType::wait_indefinitely()).unwrap();
    rx.recv().unwrap().unwrap();
    let bytes = read.slice(..).get_mapped_range();
    let values: &[[f32; 4]] = bytemuck::cast_slice(&bytes);

    assert_eq!(values[28], [2.0,3.3,-1.0,1.0], "surf erased existing ocean displacement");
    for row in &values[..16] {
        assert!(row[0].abs() <= 0.36);
        assert!(row[2] >= 0.0 && row[2] <= 0.12);
        assert!(row[3] < 1e-4, "slope mismatch {row:?}");
        if row[0] < 0.0 {
            assert_eq!(row[1], 0.0);
        }
    }
    assert!(values[4][1] > 0.9, "no crest breaking");
    for row in &values[16..20] {
        assert_eq!(*row, [0.0; 4], "shelter/cliff/offshore leaked surf");
    }
    assert_eq!(
        values[20], values[21],
        "translated beach changed the local arrival"
    );
    assert!(values[20][3] > 0.9, "synthetic exposed slope was excluded");
    assert_eq!(values[22], [0.0; 4], "off-map pilot must fall back");
    assert_eq!(values[23], [0.0; 4], "disabled pilot changed water");
    assert_eq!(
        values[24], [0.0; 4],
        "upstream headland failed to shelter the beach"
    );
    assert!(values[25][3] > 0.5, "curved exposed beach lost coverage");
    assert_eq!(
        values[26], [0.0; 4],
        "steep terrain generated spilling surf"
    );
    assert_eq!(
        values[27], [0.0; 4],
        "flat shelf manufactured a shore direction"
    );
}

#[test]
fn gpu_curling_sheet_overhang_collapse_and_world_anchor() {
    let instance =
        wgpu::Instance::new(wgpu::InstanceDescriptor::new_without_display_handle_from_env());
    let Ok(adapter) = pollster::block_on(instance.request_adapter(&Default::default())) else {
        eprintln!("SKIP foam GPU probe: no adapter");
        return;
    };
    let (device, queue) = pollster::block_on(adapter.request_device(&Default::default())).unwrap();
    let mut source = include_str!("curling_breaker.wgsl").replace("#define_import_path water_curling_breaker", "");
    source.push_str(r#"
@group(0) @binding(0) var<storage,read_write> result: array<vec4<f32>>;
@compute @workgroup_size(1)
fn probe(@builtin(global_invocation_id) id: vec3<u32>) {
    let times=array<f32,4>(2.0,5.25,6.4,7.7);
    let time=times[id.x];
    let a=curl_point(vec2<f32>(0.80,0.5),time);
    let b=curl_point(vec2<f32>(0.801,0.5),time);
    let c=curl_point(vec2<f32>(0.98,0.5),time);
    let d=curl_point(vec2<f32>(0.981,0.5),time);
    result[id.x*2u]=vec4<f32>(a.position.y,c.position.y,b.position.x-a.position.x,d.position.x-c.position.x);
    let tail=curl_point(vec2<f32>(0.0,0.5),time);
    let repeated=curl_point(vec2<f32>(0.80,0.5),time+10.0);
    result[id.x*2u+1u]=vec4<f32>(tail.position.x,a.collapse,a.coverage,length(a.position-repeated.position));
}
"#);
    let shader = device.create_shader_module(wgpu::ShaderModuleDescriptor {
        label: None,
        source: wgpu::ShaderSource::Wgsl(source.into()),
    });
    let pipeline = device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
        label: None,
        layout: None,
        module: &shader,
        entry_point: Some("probe"),
        compilation_options: Default::default(),
        cache: None,
    });
    let output = device.create_buffer(&wgpu::BufferDescriptor {
        label: None,
        size: 128,
        usage: wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_SRC,
        mapped_at_creation: false,
    });
    let read = device.create_buffer(&wgpu::BufferDescriptor {
        label: None,
        size: 128,
        usage: wgpu::BufferUsages::MAP_READ | wgpu::BufferUsages::COPY_DST,
        mapped_at_creation: false,
    });
    let group = device.create_bind_group(&wgpu::BindGroupDescriptor {
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
        pass.set_bind_group(0, &group, &[]);
        pass.dispatch_workgroups(4, 1, 1);
    }
    encoder.copy_buffer_to_buffer(&output, 0, &read, 0, 128);
    queue.submit([encoder.finish()]);
    let (tx, rx) = std::sync::mpsc::channel();
    read.slice(..).map_async(wgpu::MapMode::Read, move |r| {
        tx.send(r).unwrap();
    });
    device.poll(wgpu::PollType::wait_indefinitely()).unwrap();
    rx.recv().unwrap().unwrap();
    let bytes = read.slice(..).get_mapped_range();
    let values: &[[f32; 4]] = bytemuck::cast_slice(&bytes);
    eprintln!("curl stages: {values:?}");
    // A single-valued heightfield cannot reverse horizontal tangent at its lip.
    assert!(values[2][2] < 0.0 && values[2][3] > 0.0, "no actual overhang");
    assert!(values[2][0] > 1.0 && values[2][1] > 0.05, "barrel collapsed prematurely");
    assert!(values[6][0] < 0.3 && values[6][1] < 0.3, "impact did not collapse into wash");
    assert!(values[3][0] < values[1][0] - 6.0, "crest did not approach coast");
    for i in 0..4 {
        assert!(values[i*2+1][3] < 0.01, "unstable repeating world anchor");
        for value in values[i*2] {assert!(value.is_finite());}
    }
}
