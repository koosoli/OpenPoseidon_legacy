//! Exercise the real history pipeline, including its ping-pong and domain reset.
use crate::ffi::{WgrWaterInteractionParams, WgrWaterParams};
use bytemuck::Zeroable;
use wgpu::util::DeviceExt;
#[test]
fn gpu_foam_pipeline_pause_cadence_and_teleport() {
    let instance =
        wgpu::Instance::new(wgpu::InstanceDescriptor::new_without_display_handle_from_env());
    let Ok(adapter) = pollster::block_on(instance.request_adapter(&Default::default())) else {
        eprintln!("SKIP history GPU pipeline");
        return;
    };
    if !adapter
        .features()
        .contains(wgpu::Features::FLOAT32_FILTERABLE)
    {
        eprintln!("SKIP history GPU pipeline: float filtering");
        return;
    }
    let (device, queue) = pollster::block_on(adapter.request_device(&wgpu::DeviceDescriptor {
        required_features: wgpu::Features::FLOAT32_FILTERABLE,
        ..Default::default()
    }))
    .unwrap();
    let uniform = |bytes: &[u8]| {
        device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
            label: None,
            contents: bytes,
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
        })
    };
    let water = uniform(bytemuck::bytes_of(&WgrWaterParams::zeroed()));
    let cfg = uniform(&[0; 320]);
    let conform = uniform(&[0; 48]);
    let texture = |layers| {
        device.create_texture(&wgpu::TextureDescriptor {
            label: None,
            size: wgpu::Extent3d {
                width: 1,
                height: 1,
                depth_or_array_layers: layers,
            },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: wgpu::TextureFormat::Rgba32Float,
            usage: wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::COPY_DST,
            view_formats: &[],
        })
    };
    let fft = texture(4);
    let fft_view = fft.create_view(&wgpu::TextureViewDescriptor {
        dimension: Some(wgpu::TextureViewDimension::D2Array),
        ..Default::default()
    });
    let interaction = texture(1);
    queue.write_texture(
        interaction.as_image_copy(),
        bytemuck::cast_slice(&[0.0f32, 0.5, 0.2, 0.0]),
        wgpu::TexelCopyBufferLayout {
            offset: 0,
            bytes_per_row: Some(16),
            rows_per_image: Some(1),
        },
        interaction.size(),
    );
    let view = interaction.create_view(&Default::default());
    let blank = texture(1);
    let bed = blank.create_view(&Default::default());
    let shader=device.create_shader_module(wgpu::ShaderModuleDescriptor {label:None,source:wgpu::ShaderSource::Wgsl(r#"
@group(0) @binding(0) var history: texture_2d<f32>;
@group(0) @binding(1) var<storage,read_write> result: array<vec4<f32>>;
@compute @workgroup_size(1)
fn probe() {result[0]=textureLoad(history,vec2<i32>(512,512),0);result[1]=textureLoad(history,vec2<i32>(3,512),0);}
"#.into())});
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
        size: 32,
        usage: wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_SRC,
        mapped_at_creation: false,
    });
    let read = device.create_buffer(&wgpu::BufferDescriptor {
        label: None,
        size: 32,
        usage: wgpu::BufferUsages::MAP_READ | wgpu::BufferUsages::COPY_DST,
        mapped_at_creation: false,
    });
    let sample = |foam: &super::foam::Foam| -> [[f32; 4]; 2] {
        let group = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: None,
            layout: &pipeline.get_bind_group_layout(0),
            entries: &[
                wgpu::BindGroupEntry {
                    binding: 0,
                    resource: wgpu::BindingResource::TextureView(foam.view()),
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
            pass.set_bind_group(0, &group, &[]);
            pass.dispatch_workgroups(1, 1, 1);
        }
        encoder.copy_buffer_to_buffer(&output, 0, &read, 0, 32);
        queue.submit([encoder.finish()]);
        let (tx, rx) = std::sync::mpsc::channel();
        read.slice(..).map_async(wgpu::MapMode::Read, move |r| {
            tx.send(r).unwrap();
        });
        device.poll(wgpu::PollType::wait_indefinitely()).unwrap();
        rx.recv().unwrap().unwrap();
        let result = {
            let bytes = read.slice(..).get_mapped_range();
            *bytemuck::from_bytes::<[[f32; 4]; 2]>(&bytes)
        };
        read.unmap();
        result
    };
    let mut finals = Vec::new();
    for hz in [30, 60, 120] {
        let mut composer = crate::shaders::build_composer();
        let mut foam = super::foam::Foam::new(
            &device,
            &mut composer,
            &water,
            &[view.clone(), view.clone()],
            &fft_view,
            &fft_view,
            &cfg,
            &bed,
            &conform,
        );
        let mut params = WgrWaterInteractionParams::zeroed();
        params.domain = [0.0, 0.0, 256.0, 1.0 / 256.0];
        params.previous_domain = params.domain;
        for frame in 0..hz * 2 {
            // The C++ producer supplies fixed 1/60 steps, including zero-step 120Hz frames.
            params.grid = [
                256.0,
                if hz == 120 {
                    if frame % 2 == 0 { 0.0 } else { 1.0 / 60.0 }
                } else {
                    1.0 / hz as f32
                },
                0.0,
                0.0,
            ];
            foam.set_params(&queue, params);
            let mut encoder = device.create_command_encoder(&Default::default());
            foam.dispatch(&mut encoder, 0);
            queue.submit([encoder.finish()]);
        }
        let before = sample(&foam);
        eprintln!("real foam {hz}Hz: {before:?}");
        finals.push(before[0]);
        params.grid[1] = 0.0;
        for _ in 0..10 {
            foam.set_params(&queue, params);
            let mut encoder = device.create_command_encoder(&Default::default());
            foam.dispatch(&mut encoder, 0);
            queue.submit([encoder.finish()]);
        }
        assert_eq!(sample(&foam), before, "paused centre or edge drifted");
        params.domain[0] = 1024.0;
        params.grid[3] = 1.0;
        foam.set_params(&queue, params);
        let mut encoder = device.create_command_encoder(&Default::default());
        foam.dispatch(&mut encoder, 0);
        queue.submit([encoder.finish()]);
        assert_eq!(
            sample(&foam),
            [[0.0; 4]; 2],
            "teleport retained old-world foam"
        );
    }
    for row in &finals {
        for i in 0..3 {
            assert!(
                (row[i] - finals[0][i]).abs() < 0.005,
                "history cadence mismatch {finals:?}"
            );
        }
    }
}
