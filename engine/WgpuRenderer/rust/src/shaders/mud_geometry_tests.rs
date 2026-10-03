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
            "mud_height_offset",
            "mud_height_gradient",
        "sand_height_offset",
        "sand_height_gradient",
            "sample_height",
            "sample_normal",
            "sample_mud_normals",
            "sample_mud_normal",
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
        source.push_str("#import conform::{mud_height_offset, mud_height_gradient, surface_y, surface_grad, snow_depth, snow_deficit}\n");
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
        "let gradient = mud_height_gradient(world, 0.125);\n\
        let normal = {normal};\n\
        result[0] = vec4<f32>(mud_height_offset(world), gradient, snow_depth(world));\n\
        result[1] = vec4<f32>({height}, normal);\n\
        result[2] = vec4<f32>(length(normal), snow_deficit(world),\n\
            mud_height_offset(vec2<f32>(999.0, 2032.125)),\n\
            mud_height_offset(vec2<f32>(1064.0, 2032.125)));\n}}\n"
    ));
    source
}

fn compose_probe(terrain: bool) -> naga::Module {
    let mut composer = super::build_composer();
    let source = probe_source(terrain);
    let mut module = composer
        .make_naga_module(NagaModuleDescriptor {
            source: &source,
            file_path: "mud_geometry_actual_probe.wgsl",
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
fn mud_geometry_actual_helpers_match_and_compose_in_real_terrain() {
    for name in [
        "mud_height_offset",
        "mud_height_gradient",
        "sand_height_offset",
        "sand_height_gradient",
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
            .matches("+ mud_height_offset(world_xz)")
            .count(),
        2
    );
    assert_eq!(
        declaration(CONFORM, "fn surface_y(")
            .matches("+ mud_height_offset(world_xz)")
            .count(),
        1
    );
    assert!(declaration(CONFORM, "fn surface_y(").contains("bare_surface_y(world_xz)"));
}

fn snapshot(case: usize) -> Vec<f32> {
    let mut data = vec![0.03_f32; 4 + GRID];
    data[..4].copy_from_slice(&[1000.0, 2000.0, 0.125, 0.1]);
    if case == 0 {
        return data;
    } // old snow-only buffer
    data.extend([1000.0, 2000.0, 0.125, 0.06]);
    if case == 7 {
        return data;
    } // appended header without a complete mud view
    data.resize(4 + GRID + 4 + GRID, 0.0);
    let start = 4 + GRID + 4 + SAMPLE;
    for (offset, depth) in [(0, -0.01), (1, -0.02), (512, -0.04), (513, -0.06)] {
        data[start + offset] = match case {
            4 => -1.0,
            5 => 1.0,
            _ => depth,
        };
    }
    if case == 2 {
        data[4 + GRID + 3] = 0.0;
    } // disabled mud
    if case == 3 {
        data[4 + GRID + 2] = 0.25;
    } // unsupported producer cell size
    if case == 6 {
        data[4..4 + GRID].fill(0.07);
    } // change only snow, not mud
    if case == 8 {
        data[4 + GRID + 3] = 0.35;
        for (offset, depth) in [(0, -0.10), (1, -0.15), (512, -0.20), (513, -0.24)] {
            data[start + offset] = depth;
        }
    } // decimetre layer: actual signed geometry and both normal consumers
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
fn mud_geometry_gpu_samples_signed_depression_without_corrupting_snow() {
    let (device, queue) = crate::gfx3d::cull::tests::headless()
        .expect("mud geometry acceptance requires a real GPU device");
    let heightmap = device.create_texture(&wgpu::TextureDescriptor {
        label: Some("mud_geometry_flat_heightmap"),
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
            label: Some("mud_actual_geometry_helpers"),
            source: wgpu::ShaderSource::Naga(std::borrow::Cow::Owned(compose_probe(terrain))),
        });
        // Float height textures are non-filterable in the real terrain layout;
        // explicit layout avoids auto-layout requesting an unrelated feature.
        let resource_layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("mud_actual_height_resources"),
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
            label: Some("mud_actual_geometry_compute"),
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
        for case in 0..9 {
            let snow = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
                label: Some("mud_actual_appended_view"),
                contents: bytemuck::cast_slice(&snapshot(case)),
                usage: wgpu::BufferUsages::STORAGE,
            });
            let output = device.create_buffer(&wgpu::BufferDescriptor {
                label: None,
                size: 48,
                usage: wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_SRC,
                mapped_at_creation: false,
            });
            let staging = device.create_buffer(&wgpu::BufferDescriptor {
                label: None,
                size: 48,
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
            encoder.copy_buffer_to_buffer(&output, 0, &staging, 0, 48);
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
                1 | 6 => -0.0325,
                4 => -0.06,
                8 => -0.1725,
                _ => 0.0,
            };
            assert!(
                (values[0] - expected_offset).abs() < 1e-6,
                "signed bilinear depth path={terrain} case={case}"
            );
            assert!(
                (values[4] - (10.0 + expected_offset)).abs() < 1e-5,
                "actual geometry must descend, not rise"
            );
            if case == 1 || case == 6 {
                assert!((values[1] + 0.03).abs() < 1e-5 && (values[2] + 0.07).abs() < 1e-5);
                assert!(
                    values[5].abs() + values[7].abs() > 0.05,
                    "real mud relief must change normals"
                );
            } else if expected_offset == 0.0 {
                assert_eq!(&values[1..3], &[0.0, 0.0]);
                assert_eq!(&values[5..8], &[0.0, 1.0, 0.0]);
            }
            cases.push(values);
            drop(bytes);
            staging.unmap();
        }
        for case in [1, 2, 3, 4, 5, 7, 8] {
            assert_eq!(
                cases[case][3], cases[0][3],
                "appended/disabled mud must not change existing snow"
            );
            assert_eq!(
                cases[case][9], cases[0][9],
                "snow deficit must remain in the first half"
            );
        }
        assert!(
            (cases[1][3] - cases[6][3] - 0.04).abs() < 1e-5,
            "changing only snow must not change mud depth"
        );
        assert!(cases[0][3] > 0.0, "fixture must exercise real nonzero snow");
        paths.push(cases);
    }
    for case in 0..9 {
        for (conform, terrain) in paths[0][case].iter().zip(&paths[1][case]) {
            assert!(
                (conform - terrain).abs() < 1e-5,
                "terrain and conformed support/normal diverged in case {case}"
            );
        }
    }
}
