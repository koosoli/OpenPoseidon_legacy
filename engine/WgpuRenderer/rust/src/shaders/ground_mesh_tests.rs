use naga_oil::compose::NagaModuleDescriptor;
use wgpu::util::DeviceExt;

const PROBE: &str = r#"
#import conform::{bare_surface_y, bare_surface_valid, rigid_ground_fragment}
@group(0) @binding(0) var<storage, read_write> result: array<vec4<f32>>;
fn yes(value: bool) -> f32 { return select(0.0, 1.0, value); }
@compute @workgroup_size(1) fn cs_probe() {
    let a = vec2<f32>(1002.5, 2002.5);
    let b = vec2<f32>(1007.5, 2007.5);
    let up = vec3<f32>(0.0, 1.0, 0.0);
    let above = vec3<f32>(0.0, -1.0, 0.0);
    var heights = vec2<f32>(0.0);
    if (bare_surface_valid(a)) { heights = vec2<f32>(bare_surface_y(a), bare_surface_y(b)); }
    result[0] = vec4<f32>(heights, yes(bare_surface_valid(a)),
        yes(bare_surface_valid(vec2<f32>(999.0, 2002.5))));
    result[1] = vec4<f32>(
        yes(rigid_ground_fragment(vec3<f32>(a.x, 11.6, a.y), up, above)),
        yes(rigid_ground_fragment(vec3<f32>(a.x, 12.5, a.y), up, above)),
        yes(rigid_ground_fragment(vec3<f32>(a.x, 10.0, a.y), up, above)),
        yes(rigid_ground_fragment(vec3<f32>(a.x, 11.5, a.y), up, -above)));
    result[2] = vec4<f32>(
        yes(rigid_ground_fragment(vec3<f32>(a.x, 11.5, a.y), -up, above)),
        yes(rigid_ground_fragment(vec3<f32>(a.x, 11.5, a.y), vec3<f32>(0.0), above)),
        yes(rigid_ground_fragment(vec3<f32>(a.x, 11.5, a.y), vec3<f32>(1.0,1.0,0.0), above)),
        yes(rigid_ground_fragment(vec3<f32>(b.x, 21.5, b.y), up, above)));
    result[3] = vec4<f32>(
        yes(bare_surface_valid(vec2<f32>(1010.0, 2002.5))),
        yes(bare_surface_valid(vec2<f32>(1002.5, 2010.0))),
        yes(rigid_ground_fragment(vec3<f32>(a.x, 11.7, a.y), up, above)),
        yes(rigid_ground_fragment(vec3<f32>(a.x, 11.4, a.y), up, above)));
}
"#;

fn module() -> naga::Module {
    let mut composer = super::build_composer();
    let mut module = composer
        .make_naga_module(NagaModuleDescriptor {
            source: PROBE,
            file_path: "ground_mesh_actual_helper_probe.wgsl",
            ..Default::default()
        })
        .unwrap_or_else(|e| panic!("{}", e.emit_to_string(&composer)));
    // Probe only remaps resource group; all production function bodies remain verbatim.
    for (_, global) in module.global_variables.iter_mut() {
        if let Some(binding) = &mut global.binding {
            if binding.group == 4 {
                binding.group = 1;
            }
        }
    }
    module
}

#[test]
fn ground_mesh_actual_fragment_helper_and_both_routes_compose() {
    module();
    for (source, path) in [
        (
            include_str!("../gfx3d/shader3d.wgsl"),
            "gfx3d/shader3d.wgsl",
        ),
        (
            include_str!("../gfx3d/gpu_driven.wgsl"),
            "gfx3d/gpu_driven.wgsl",
        ),
    ] {
        assert!(source.contains(
            "rigid_ground_fragment(in.world_pos + frame.cam_pos.xyz, in.normal, in.world_pos)"
        ));
        let mut composer = super::build_composer();
        composer
            .make_naga_module(NagaModuleDescriptor {
                source,
                file_path: path,
                ..Default::default()
            })
            .unwrap_or_else(|e| panic!("{}", e.emit_to_string(&composer)));
    }
    let retained = include_str!("../gfx3d/gpu_driven.wgsl");
    assert!(retained.contains("(in.surface_receivers & 1u) != 0u"));
    assert!(retained.contains("(in.surface_receivers & 2u) == 0u"));
    assert!(!retained.contains("@location(15)"));
}

#[test]
fn ground_mesh_gpu_proves_each_fragment_triangle_side_and_valid_heightmap() {
    let (device, queue) = crate::gfx3d::cull::tests::headless()
        .expect("ground mesh acceptance requires an actual GPU device");
    let texture = device.create_texture(&wgpu::TextureDescriptor {
        label: Some("ground_mesh_nonplanar_bare_terrain"),
        size: wgpu::Extent3d {
            width: 2,
            height: 2,
            depth_or_array_layers: 1,
        },
        mip_level_count: 1,
        sample_count: 1,
        dimension: wgpu::TextureDimension::D2,
        format: wgpu::TextureFormat::R32Float,
        usage: wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::COPY_DST,
        view_formats: &[],
    });
    queue.write_texture(
        wgpu::TexelCopyTextureInfo {
            texture: &texture,
            mip_level: 0,
            origin: wgpu::Origin3d::ZERO,
            aspect: wgpu::TextureAspect::All,
        },
        bytemuck::cast_slice(&[10.0_f32, 12.0, 14.0, 30.0]),
        wgpu::TexelCopyBufferLayout {
            offset: 0,
            bytes_per_row: Some(8),
            rows_per_image: Some(2),
        },
        wgpu::Extent3d {
            width: 2,
            height: 2,
            depth_or_array_layers: 1,
        },
    );
    let view = texture.create_view(&Default::default());
    let resources = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
        label: None,
        entries: &[
            wgpu::BindGroupLayoutEntry {
                binding: 0,
                visibility: wgpu::ShaderStages::COMPUTE,
                ty: wgpu::BindingType::Texture {
                    sample_type: wgpu::TextureSampleType::Float { filterable: false },
                    view_dimension: wgpu::TextureViewDimension::D2,
                    multisampled: false,
                },
                count: None,
            },
            wgpu::BindGroupLayoutEntry {
                binding: 1,
                visibility: wgpu::ShaderStages::COMPUTE,
                ty: wgpu::BindingType::Buffer {
                    ty: wgpu::BufferBindingType::Uniform,
                    has_dynamic_offset: false,
                    min_binding_size: None,
                },
                count: None,
            },
        ],
    });
    // No SnowData binding here: actual helper must work without fragment storage.
    let results = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
        label: None,
        entries: &[wgpu::BindGroupLayoutEntry {
            binding: 0,
            visibility: wgpu::ShaderStages::COMPUTE,
            ty: wgpu::BindingType::Buffer {
                ty: wgpu::BufferBindingType::Storage { read_only: false },
                has_dynamic_offset: false,
                min_binding_size: None,
            },
            count: None,
        }],
    });
    let layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
        label: None,
        bind_group_layouts: &[Some(&results), Some(&resources)],
        immediate_size: 0,
    });
    let shader = device.create_shader_module(wgpu::ShaderModuleDescriptor {
        label: Some("ground_mesh_actual_helper"),
        source: wgpu::ShaderSource::Naga(std::borrow::Cow::Owned(module())),
    });
    let pipeline = device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
        label: None,
        layout: Some(&layout),
        module: &shader,
        entry_point: Some("cs_probe"),
        compilation_options: Default::default(),
        cache: None,
    });
    for case in 0..5 {
        let mut params = [0_u8; 48];
        for (at, value) in [
            (0, 1000.0_f32),
            (4, 2000.0),
            (
                8,
                if case == 2 {
                    0.0
                } else if case == 4 {
                    -10.0
                } else {
                    10.0
                },
            ),
            (12, if case == 1 { 0.0 } else { 1.0 }),
        ] {
            params[at..at + 4].copy_from_slice(&value.to_le_bytes());
        }
        params[16..20].copy_from_slice(&(if case == 3 { 3_u32 } else { 2_u32 }).to_le_bytes());
        params[20..24].copy_from_slice(&2_u32.to_le_bytes());
        let uniform = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
            label: None,
            contents: &params,
            usage: wgpu::BufferUsages::UNIFORM,
        });
        let input = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: None,
            layout: &resources,
            entries: &[
                wgpu::BindGroupEntry {
                    binding: 0,
                    resource: wgpu::BindingResource::TextureView(&view),
                },
                wgpu::BindGroupEntry {
                    binding: 1,
                    resource: uniform.as_entire_binding(),
                },
            ],
        });
        let output = device.create_buffer(&wgpu::BufferDescriptor {
            label: None,
            size: 64,
            usage: wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_SRC,
            mapped_at_creation: false,
        });
        let staging = device.create_buffer(&wgpu::BufferDescriptor {
            label: None,
            size: 64,
            usage: wgpu::BufferUsages::MAP_READ | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        let bind = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: None,
            layout: &results,
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
            pass.set_bind_group(1, &input, &[]);
            pass.dispatch_workgroups(1, 1, 1);
        }
        encoder.copy_buffer_to_buffer(&output, 0, &staging, 0, 64);
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
        let expected = if case == 0 {
            [
                11.5_f32, 21.5, 1.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0,
                1.0,
            ]
        } else {
            [0.0; 16]
        };
        for (i, (actual, expected)) in values.iter().zip(expected).enumerate() {
            assert!(
                actual.is_finite() && (actual - expected).abs() < 1e-5,
                "case={case} lane={i} actual={actual} expected={expected}"
            );
        }
        drop(bytes);
        staging.unmap();
    }
}
