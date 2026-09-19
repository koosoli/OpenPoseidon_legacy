//! GPU metre calibration using the production FFT and shared preset constants.
use crate::ffi::{WgrWaterCascadeConfig, WgrWaterParams};
use bytemuck::Zeroable;
use wgpu::util::DeviceExt;

#[test]
fn gpu_reference_presets_have_distinct_meter_scales() {
    let instance =
        wgpu::Instance::new(wgpu::InstanceDescriptor::new_without_display_handle_from_env());
    let Ok(adapter) = pollster::block_on(instance.request_adapter(&Default::default())) else {
        eprintln!("SKIP FFT calibration: no GPU");
        return;
    };
    let (device, queue) = pollster::block_on(adapter.request_device(&wgpu::DeviceDescriptor {
        required_limits: adapter.limits(),
        ..Default::default()
    }))
    .unwrap();
    let mut water = WgrWaterParams::zeroed();
    water.time = 40.0;
    water.wave_amp = 1.0;
    water.wave_choppy = 1.0;
    water.wave_speed = 1.0;
    water.wave_scale = 1.0;
    water.fft_control = [1.0, 0.0, 0.0, 0.0];
    water.sea_params = [0.0, 1.0, 0.0, 0.0];
    water.fft_cascade_lengths = [257.0, 57.0, 16.0, 0.0];
    let params = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
        label: None,
        contents: bytemuck::bytes_of(&water),
        usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
    });
    let mut fft = super::fft::Fft::new(&device, &queue, &params, true, 512).unwrap();
    let timers = crate::gpu_timers::GpuTimers::new(&device, &queue, false, false);
    let shader=device.create_shader_module(wgpu::ShaderModuleDescriptor {label:None,source:wgpu::ShaderSource::Wgsl(r#"
@group(0) @binding(0) var disp: texture_2d_array<f32>;
@group(0) @binding(1) var samp: sampler;
@group(0) @binding(2) var<storage,read_write> result: array<vec2<f32>>;
@group(0) @binding(3) var auxiliary: texture_2d_array<f32>;
@compute @workgroup_size(8,8)
fn probe(@builtin(global_invocation_id) id: vec3<u32>) {
 let p=vec2<f32>(id.xy)*2.0;
 result[id.y*256u+id.x].x=textureSampleLevel(disp,samp,p/257.0,0,0.0).y+textureSampleLevel(disp,samp,p/57.0,1,0.0).y;
 result[id.y*256u+id.x].y=textureSampleLevel(auxiliary,samp,p/257.0,0,0.0).x;
}
"#.into())});
    let pipeline = device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
        label: None,
        layout: None,
        module: &shader,
        entry_point: Some("probe"),
        compilation_options: Default::default(),
        cache: None,
    });
    let sampler = device.create_sampler(&wgpu::SamplerDescriptor {
        address_mode_u: wgpu::AddressMode::Repeat,
        address_mode_v: wgpu::AddressMode::Repeat,
        mag_filter: wgpu::FilterMode::Linear,
        min_filter: wgpu::FilterMode::Linear,
        ..Default::default()
    });
    let output = device.create_buffer(&wgpu::BufferDescriptor {
        label: None,
        size: 524288,
        usage: wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_SRC,
        mapped_at_creation: false,
    });
    let read = device.create_buffer(&wgpu::BufferDescriptor {
        label: None,
        size: 524288,
        usage: wgpu::BufferUsages::MAP_READ | wgpu::BufferUsages::COPY_DST,
        mapped_at_creation: false,
    });
    let group = device.create_bind_group(&wgpu::BindGroupDescriptor {
        label: None,
        layout: &pipeline.get_bind_group_layout(0),
        entries: &[
            wgpu::BindGroupEntry {
                binding: 0,
                resource: wgpu::BindingResource::TextureView(fft.displacement_view()),
            },
            wgpu::BindGroupEntry {
                binding: 1,
                resource: wgpu::BindingResource::Sampler(&sampler),
            },
            wgpu::BindGroupEntry {
                binding: 2,
                resource: output.as_entire_binding(),
            },
            wgpu::BindGroupEntry {
                binding: 3,
                resource: wgpu::BindingResource::TextureView(fft.auxiliary_view()),
            },
        ],
    });
    let mut heights = Vec::new();
    for line in include_str!("../../../OceanReferencePresets.def")
        .lines()
        .filter(|l| l.starts_with("OCEAN_REFERENCE("))
        .chain(std::iter::once(
            "OCEAN_REFERENCE(6, 9.0f, 0.0f, 4.0f, 0.0f)",
        ))
    {
        let v: Vec<f32> = line
            .trim_start_matches("OCEAN_REFERENCE(")
            .trim_end_matches(')')
            .split(',')
            .map(|x| x.trim().trim_end_matches('f').parse().unwrap())
            .collect();
        let id = v[0] as u32;
        let mut c = WgrWaterCascadeConfig::zeroed();
        c.enabled = 1;
        c.resolution = 512;
        c.tile_length_x = 257.0;
        c.tile_length_y = 257.0;
        c.wind_speed = v[1];
        c.wind_direction_rad = std::f32::consts::PI;
        c.fetch_meters = 350000.0;
        c.water_depth_meters = 30.0;
        c.directional_spread = 0.12;
        c.swell = 0.95;
        c.short_wave_detail = 1.0;
        c.whitecap_threshold = 0.5;
        c.displacement_scale = v[2];
        c.horiz_displacement_scale = v[2];
        c.normal_scale = v[2];
        c.spectrum_seed = 1471;
        c.phase_offset_seconds = 120.0;
        c.update_rate_hz = 60.0;
        let mut chop = c;
        chop.tile_length_x = 57.0;
        chop.tile_length_y = 57.0;
        chop.wind_speed = v[3] * (0.3 + 0.7 * 8.0 / 12.0);
        chop.wind_direction_rad = std::f32::consts::FRAC_PI_2;
        chop.directional_spread = 0.5;
        chop.swell = 0.25;
        chop.spectrum_seed = 8623;
        if id == 3 {
            c.wind_speed *= 0.3 + 0.7 * 8.0 / 12.0;
            c.wind_direction_rad += std::f32::consts::FRAC_PI_2 - 0.349;
        }
        fft.set_cascade_config(&device, &queue, 0, c);
        fft.set_cascade_config(&device, &queue, 1, chop);
        fft.set_cascade_config(&device, &queue, 2, WgrWaterCascadeConfig::zeroed());
        fft.set_cascade_config(&device, &queue, 3, WgrWaterCascadeConfig::zeroed());
        fft.set_params(&water);
        let mut encoder = device.create_command_encoder(&Default::default());
        fft.dispatch(&mut encoder, &timers);
        {
            let mut pass = encoder.begin_compute_pass(&Default::default());
            pass.set_pipeline(&pipeline);
            pass.set_bind_group(0, &group, &[]);
            pass.dispatch_workgroups(32, 32, 1);
        }
        encoder.copy_buffer_to_buffer(&output, 0, &read, 0, 524288);
        queue.submit([encoder.finish()]);
        let (tx, rx) = std::sync::mpsc::channel();
        read.slice(..).map_async(wgpu::MapMode::Read, move |r| {
            tx.send(r).unwrap();
        });
        device.poll(wgpu::PollType::wait_indefinitely()).unwrap();
        rx.recv().unwrap().unwrap();
        {
            let bytes = read.slice(..).get_mapped_range();
            let pairs: &[[f32; 2]] = bytemuck::cast_slice(&bytes);
            let values: Vec<f32> = pairs.iter().map(|p| p[0]).collect();
            if id == 6 {
                assert!(
                    pairs.iter().all(|p| p[0] == 0.0 && p[1] == 1.0),
                    "zero displacement still produces breaking compression"
                );
            }
            let mean = values.iter().map(|v| *v as f64).sum::<f64>() / values.len() as f64;
            let hs = 4.0
                * (values
                    .iter()
                    .map(|v| (*v as f64 - mean).powi(2))
                    .sum::<f64>()
                    / values.len() as f64)
                    .sqrt();
            eprintln!("reference {id}: Hs={hs:.4}m mean={mean:.5}m");
            assert!(hs.is_finite());
            heights.push(hs);
        }
        read.unmap();
    }
    assert!(
        heights[0] < 0.3
            && heights[1] > 0.6
            && heights[1] < 1.2
            && heights[2] > 1.5
            && heights[2] < 2.5
    );
    assert!(heights[0] < heights[1] * 0.4);
    assert!(heights[2] > heights[1] * 1.4);
}
