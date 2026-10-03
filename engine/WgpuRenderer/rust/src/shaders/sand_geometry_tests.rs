use naga_oil::compose::NagaModuleDescriptor;
use wgpu::util::DeviceExt;

const CONFORM: &str = include_str!("conform.wgsl");
const TERRAIN: &str = include_str!("../terrain/terrain.wgsl");
const GRID: usize = 512 * 512;
const SAMPLE: usize = 256 * 512 + 256;

// Copy complete production declarations verbatim, not a second implementation.
// Terrain is an entry-point shader, not an importable helper module. Its real
// full composition is checked below; extraction allows a small compute fixture
// to call the same height/normal functions without unrelated terrain resources.
fn declaration<'a>(source: &'a str, start: &str) -> &'a str {
    let begin = source
        .find(start)
        .unwrap_or_else(|| panic!("missing production {start}"));
    let brace = source[begin..].find('{').unwrap() + begin;
    let mut depth = 0;
    for (i, byte) in source.bytes().enumerate().skip(brace) {
        match byte {
            b'{' => depth += 1,
            b'}' => {
                depth -= 1;
                if depth == 0 {
                    return &source[begin..=i];
                }
            }
            _ => {}
        }
    }
    panic!("unterminated production {start}");
}

fn probe_source(terrain: bool) -> String {
    let mut source = String::new();
    if terrain {
        // The production fine normal now delegates to the shared broad/fine
        // pair. Extract its exact type and helper; keep every existing probe.
        for name in ["struct TerrainParams", "struct SnowData", "struct GroundNormalPair"] {
            source.push_str(declaration(TERRAIN, name));
            source.push_str(";\n");
        }
        // Same production terrain resource names/types; compact fixture group.
        source.push_str(
            "@group(1) @binding(0) var<uniform> tp: TerrainParams;\n\
            @group(1) @binding(1) var heightmap: texture_2d<f32>;\n\
            @group(1) @binding(4) var<storage, read> snow: SnowData;\n",
        );
        for name in [
            "hm_load",
            "sand_height_offset",
            "sand_height_gradient",
            "sample_height",
            "sample_normal",
            "sample_mud_normals",
            "sample_mud_normal",
            "mud_height_offset",
            "mud_height_gradient",
            "snow_depth",
            "snow_drift",
            "snow_deficit",
        ] {
            source.push_str(declaration(TERRAIN, &format!("fn {name}(")));
            source.push('\n');
        }
    } else {
        // Actual imported production conform helpers. Group 4 is remapped in
        // Naga solely to fit the default headless test device's group limit.
        source.push_str("#import conform::{sand_height_offset, sand_height_gradient, surface_y, surface_grad, snow_depth, snow_deficit, mud_height_offset, mud_height_gradient}\n");
    }
    source.push_str(
        "@group(0) @binding(0) var<storage, read_write> result: array<vec4<f32>>;\n\
        @compute @workgroup_size(1) fn cs_probe() {\n\
        let world = vec2<f32>(1032.125, 2032.125);\n",
    );
    let (height, normal) = if terrain {
        (
            "sample_height(world)",
            "sample_mud_normal(world, 0.5, 0.125)",
        )
    } else {
        (
            "surface_y(world)",
            "normalize(vec3<f32>(-surface_grad(world).x, 1.0, -surface_grad(world).y))",
        )
    };
    source.push_str(&format!(
        "let gradient = sand_height_gradient(world, 0.125);\n\
        let normal = {normal};\n\
        result[0] = vec4<f32>(sand_height_offset(world), gradient, snow_depth(world));\n\
        result[1] = vec4<f32>({height}, normal);\n\
        result[2] = vec4<f32>(length(normal), snow_deficit(world),\n\
            sand_height_offset(vec2<f32>(999.0, 2032.125)),\n\
            sand_height_offset(vec2<f32>(1064.0, 2032.125)));\n\
        result[3] = vec4<f32>(mud_height_offset(world), mud_height_gradient(world, 0.125), 0.0);\n\
        result[4] = vec4<f32>(sand_height_gradient(world, 0.01), sand_height_gradient(world, 0.09375));\n}}\n"
    ));
    source
}

fn compose_probe(terrain: bool) -> naga::Module {
    let mut composer = super::build_composer();
    let source = probe_source(terrain);
    let mut module = composer
        .make_naga_module(NagaModuleDescriptor {
            source: &source,
            file_path: "sand_geometry_actual_probe.wgsl",
            ..Default::default()
        })
        .unwrap_or_else(|e| panic!("{}", e.emit_to_string(&composer)));
    if !terrain {
        for (_, global) in module.global_variables.iter_mut() {
            if let Some(binding) = &mut global.binding {
                if binding.group == 4 {
                    binding.group = 1;
                }
            }
        }
    }
    module
}

#[test]
fn sand_geometry_actual_helpers_match_and_compose_in_real_terrain() {
    for name in [
        "sand_height_offset",
        "sand_height_gradient",
        "mud_height_offset",
        "mud_height_gradient",
        "snow_depth",
        "snow_drift",
        "snow_deficit",
    ] {
        let marker = format!("fn {name}(");
        assert_eq!(
            declaration(TERRAIN, &marker).replace("\r\n", "\n"),
            declaration(CONFORM, &marker).replace("\r\n", "\n"),
            "actual {name} paths diverged"
        );
    }
    compose_probe(false);
    compose_probe(true);
    let mut composer = super::build_composer();
    composer
        .make_naga_module(NagaModuleDescriptor {
            source: TERRAIN,
            file_path: "terrain/terrain.wgsl",
            ..Default::default()
        })
        .unwrap_or_else(|e| panic!("{}", e.emit_to_string(&composer)));
    // Terrain vertex geometry and both colour/prepass normals really call the
    // sampled functions. A correct isolated helper alone is not enough.
    assert!(TERRAIN.contains("let bare_height = sample_height(world_xz)"));
    assert!(TERRAIN.contains("let height_fine = sample_height(world_xz_fine)"));
    let colour = declaration(TERRAIN, "fn fs_terrain(");
    let prepass = declaration(TERRAIN, "fn fs_terrain_prepass(");
    assert!(colour.contains("let ground_normals = sample_mud_normals(in.world_xz, tp.terrain_grid,"));
    assert!(colour.contains("let geometric_n = ground_normals.fine;"));
    assert!(prepass.contains("sample_mud_normal(in.world_xz, tp.terrain_grid,"));
    assert!(declaration(TERRAIN, "fn sample_mud_normal(")
        .contains("return sample_mud_normals(world_xz, step, footprint).fine;"));
    assert_eq!(
        declaration(TERRAIN, "fn sample_height(")
            .matches("+ sand_height_offset(world_xz)")
            .count(),
        2
    );
    assert_eq!(
        declaration(CONFORM, "fn surface_y(")
            .matches("+ sand_height_offset(world_xz)")
            .count(),
        1
    );
    assert!(declaration(CONFORM, "fn surface_y(").contains("bare_surface_y(world_xz)"));
    assert!(declaration(CONFORM, "fn surface_grad(").contains("sand_height_gradient(world_xz"));
    assert!(declaration(TERRAIN, "fn sample_mud_normals(").contains("sand_height_gradient(world_xz"));
}

fn snapshot(case: usize) -> Vec<f32> {
    let mut data = vec![0.03_f32; 4 + GRID];
    data[..4].copy_from_slice(&[1000.0, 2000.0, 0.125, 0.1]);
    data.extend([1000.0, 2000.0, 0.125, 0.35]);
    data.resize(4 + GRID + 4 + GRID, -0.04);
    if case == 0 {
        return data;
    } // complete legacy Snow+Mud buffer, no Sand view
    data.extend([1000.0, 2000.0, 0.125, 0.15]);
    if case == 6 {
        return data;
    } // incomplete Sand body, must stay undeformed
    data.resize(4 + GRID + 4 + GRID + 4 + GRID, 0.0);
    let start = 4 + GRID + 4 + GRID + 4 + SAMPLE;
    for (offset, depth) in [(0, -0.06), (1, -0.08), (512, -0.12), (513, -0.14)] {
        data[start + offset] = match case {
            2 | 10 => {
                if case == 2 {
                    0.02
                } else {
                    1.0
                }
            }
            3 => match offset {
                0 => -0.15,
                512 => -0.03,
                _ => 0.02,
            },
            9 => -1.0,
            _ => depth,
        };
    }
    if case == 4 {
        data[4 + GRID + 4 + GRID + 3] = 0.0;
    }
    if case == 5 {
        data[4 + GRID + 4 + GRID + 2] = 0.25;
    }
    if case == 7 {
        data[4..4 + GRID].fill(0.07);
    }
    if case == 8 {
        data[4 + GRID + 4..4 + GRID + 4 + GRID].fill(-0.09);
    }
    if case == 11 {
        data[4 + GRID + 4 + GRID + 3] = -0.15;
    }
    data
}

fn uniform(terrain: bool) -> Vec<u8> {
    let mut bytes = vec![0_u8; if terrain { 96 } else { 48 }];
    for (offset, value) in [
        (0, 1000.0_f32),
        (4, 2000.0),
        (8, 0.5),
        (12, if terrain { 0.5 } else { 1.0 }),
    ] {
        bytes[offset..offset + 4].copy_from_slice(&value.to_le_bytes());
    }
    for offset in [16, 20] {
        bytes[offset..offset + 4].copy_from_slice(&2_u32.to_le_bytes());
    }
    bytes
}

#[test]
fn sand_geometry_gpu_samples_signed_depression_and_rim_without_corrupting_snow_or_mud() {
    let (device, queue) = crate::gfx3d::cull::tests::headless()
        .expect("sand geometry acceptance requires a real GPU device");
    let heightmap = device.create_texture(&wgpu::TextureDescriptor {
        label: Some("sand_geometry_flat_heightmap"),
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
            texture: &heightmap,
            mip_level: 0,
            origin: wgpu::Origin3d::ZERO,
            aspect: wgpu::TextureAspect::All,
        },
        bytemuck::cast_slice(&[10.0_f32; 4]),
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
    let view = heightmap.create_view(&Default::default());
    let mut paths = Vec::new();
    for terrain in [false, true] {
        let shader = device.create_shader_module(wgpu::ShaderModuleDescriptor {
            label: Some("sand_actual_geometry_helpers"),
            source: wgpu::ShaderSource::Naga(std::borrow::Cow::Owned(compose_probe(terrain))),
        });
        // Float height textures are non-filterable in the real terrain layout;
        // explicit layout avoids auto-layout requesting an unrelated feature.
        let resource_layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("sand_actual_height_resources"),
            entries: &[
                wgpu::BindGroupLayoutEntry {
                    binding: if terrain { 1 } else { 0 },
                    visibility: wgpu::ShaderStages::COMPUTE,
                    ty: wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Float { filterable: false },
                        view_dimension: wgpu::TextureViewDimension::D2,
                        multisampled: false,
                    },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: if terrain { 0 } else { 1 },
                    visibility: wgpu::ShaderStages::COMPUTE,
                    ty: wgpu::BindingType::Buffer {
                        ty: wgpu::BufferBindingType::Uniform,
                        has_dynamic_offset: false,
                        min_binding_size: None,
                    },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: if terrain { 4 } else { 2 },
                    visibility: wgpu::ShaderStages::COMPUTE,
                    ty: wgpu::BindingType::Buffer {
                        ty: wgpu::BufferBindingType::Storage { read_only: true },
                        has_dynamic_offset: false,
                        min_binding_size: None,
                    },
                    count: None,
                },
            ],
        });
        let output_layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
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
            bind_group_layouts: &[Some(&output_layout), Some(&resource_layout)],
            immediate_size: 0,
        });
        let pipeline = device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
            label: Some("sand_actual_geometry_compute"),
            layout: Some(&layout),
            module: &shader,
            entry_point: Some("cs_probe"),
            compilation_options: Default::default(),
            cache: None,
        });
        let params = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
            label: None,
            contents: &uniform(terrain),
            usage: wgpu::BufferUsages::UNIFORM,
        });
        let mut cases = Vec::new();
        for case in 0..12 {
            let snow = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
                label: Some("sand_actual_appended_view"),
                contents: bytemuck::cast_slice(&snapshot(case)),
                usage: wgpu::BufferUsages::STORAGE,
            });
            let output = device.create_buffer(&wgpu::BufferDescriptor {
                label: None,
                size: 80,
                usage: wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_SRC,
                mapped_at_creation: false,
            });
            let staging = device.create_buffer(&wgpu::BufferDescriptor {
                label: None,
                size: 80,
                usage: wgpu::BufferUsages::MAP_READ | wgpu::BufferUsages::COPY_DST,
                mapped_at_creation: false,
            });
            let resources = device.create_bind_group(&wgpu::BindGroupDescriptor {
                label: None,
                layout: &resource_layout,
                entries: &[
                    wgpu::BindGroupEntry {
                        binding: if terrain { 1 } else { 0 },
                        resource: wgpu::BindingResource::TextureView(&view),
                    },
                    wgpu::BindGroupEntry {
                        binding: if terrain { 0 } else { 1 },
                        resource: params.as_entire_binding(),
                    },
                    wgpu::BindGroupEntry {
                        binding: if terrain { 4 } else { 2 },
                        resource: snow.as_entire_binding(),
                    },
                ],
            });
            let results = device.create_bind_group(&wgpu::BindGroupDescriptor {
                label: None,
                layout: &output_layout,
                entries: &[wgpu::BindGroupEntry {
                    binding: 0,
                    resource: output.as_entire_binding(),
                }],
            });
            let mut encoder = device.create_command_encoder(&Default::default());
            {
                let mut pass = encoder.begin_compute_pass(&Default::default());
                pass.set_pipeline(&pipeline);
                pass.set_bind_group(0, &results, &[]);
                pass.set_bind_group(1, &resources, &[]);
                pass.dispatch_workgroups(1, 1, 1);
            }
            encoder.copy_buffer_to_buffer(&output, 0, &staging, 0, 80);
            queue.submit(Some(encoder.finish()));
            let slice = staging.slice(..);
            let (send, receive) = std::sync::mpsc::channel();
            slice.map_async(wgpu::MapMode::Read, move |r| {
                let _ = send.send(r);
            });
            device.poll(wgpu::PollType::wait_indefinitely()).unwrap();
            receive.recv().unwrap().unwrap();
            let bytes = slice.get_mapped_range();
            let values = bytemuck::cast_slice::<u8, f32>(&bytes).to_vec();
            assert!(
                values.iter().all(|v| v.is_finite()),
                "path={terrain} case={case}"
            );
            assert!((values[8] - 1.0).abs() < 1e-5);
            assert_eq!(
                &values[10..12],
                &[0.0, 0.0],
                "out-of-window support must stay undeformed"
            );
            let expected_offset = match case {
                1 | 7 | 8 => -0.10,
                2 | 10 => 0.02,
                3 => -0.035,
                9 => -0.15,
                _ => 0.0,
            };
            assert!(
                (values[0] - expected_offset).abs() < 1e-6,
                "actual signed sand depth path={terrain} case={case}"
            );
            let expected_mud = if case == 8 { -0.09 } else { -0.04 };
            assert!((values[12] - expected_mud).abs() < 1e-6);
            assert!(
                (values[4] - (10.0 + expected_mud + expected_offset)).abs() < 1e-5,
                "actual physical support must include independent Mud and signed Sand"
            );
            if case == 1 || case == 7 || case == 8 {
                assert!((values[1] + 0.04).abs() < 1e-5 && (values[2] + 0.12).abs() < 1e-5);
                // Four source values contain a narrow bilinear depression.
                // Differentiating that actual surface gives (-.16,-.48),
                // whereas the old compulsory 25cm stencil erased 75% of it.
                // This checks executed production WGSL, not a slope replica.
                assert!((values[16] + 0.16).abs() < 1e-5 && (values[17] + 0.48).abs() < 1e-5,
                    "resolved pixels must preserve the physical bilinear sand derivative");
                assert!((values[18] + 0.10).abs() < 1e-5 && (values[19] + 0.30).abs() < 1e-5,
                    "the half-cell transition must smoothly filter the resolved sand slope");
                assert!(
                    values[5].abs() + values[7].abs() > 0.05,
                    "actual sand relief must change surface normals"
                );
            } else if expected_offset == 0.0 {
                assert_eq!(&values[1..3], &[0.0, 0.0]);
                assert_eq!(&values[5..8], &[0.0, 1.0, 0.0]);
            }
            assert_eq!(
                &values[13..15],
                &[0.0, 0.0],
                "constant Mud must retain its own gradients"
            );
            assert!(values[16..20].iter().all(|v| v.abs() <= 1.36 + 1e-5),
                "bounded signed cells cannot create an unbounded resolved slope");
            cases.push(values);
            drop(bytes);
            staging.unmap();
        }
        for case in [1, 2, 3, 4, 5, 6, 8, 9, 10, 11] {
            assert_eq!(
                cases[case][3], cases[0][3],
                "Sand/Mud changes must not change Snow"
            );
            assert_eq!(
                cases[case][9], cases[0][9],
                "Snow deficit remains in first view"
            );
        }
        for case in [1, 2, 3, 4, 5, 6, 7, 9, 10, 11] {
            assert_eq!(
                &cases[case][12..16],
                &cases[0][12..16],
                "Sand/Snow changes must not change Mud"
            );
        }
        assert!((cases[1][3] - cases[7][3] - 0.04).abs() < 1e-5);
        assert!(
            cases[0][3] > 0.0,
            "fixture must exercise actual nonzero Snow"
        );
        assert_eq!(
            &cases[1][0..3],
            &cases[7][0..3],
            "Snow change must leave Sand unchanged"
        );
        assert_eq!(
            &cases[1][0..3],
            &cases[8][0..3],
            "Mud change must leave Sand unchanged"
        );
        assert!(
            (cases[2][4] - cases[0][4] - 0.02).abs() < 1e-5,
            "positive rim must actually raise physical geometry"
        );
        assert!(
            (cases[0][4] - cases[1][4] - 0.10).abs() < 1e-5,
            "negative sand must actually lower physical geometry"
        );
        assert!(
            cases[3][5].abs() + cases[3][7].abs() > 0.01,
            "mixed signed Sand must change the actual surface normal"
        );
        paths.push(cases);
    }
    for case in 0..12 {
        for (conform, terrain) in paths[0][case].iter().zip(&paths[1][case]) {
            assert!(
                (conform - terrain).abs() < 1e-5,
                "actual terrain and conformed Sand support/normal diverged in case {case}"
            );
        }
    }
}
