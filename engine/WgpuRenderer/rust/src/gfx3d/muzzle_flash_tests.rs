use wgpu::util::DeviceExt;

fn probe_source() -> String {
    let source = include_str!("shader3d.wgsl");
    let helper = source
        .split("// BEGIN_MUZZLE_FLASH_HELPERS")
        .nth(1)
        .unwrap()
        .split_once('\n')
        .unwrap()
        .1
        .split("// END_MUZZLE_FLASH_HELPERS")
        .next()
        .unwrap();
    let material = source
        .split("struct Material {")
        .nth(1)
        .unwrap()
        .split("};")
        .next()
        .unwrap();
    let color = include_str!("../shaders/color.wgsl")
        .lines()
        .filter(|line| !line.starts_with('#'))
        .collect::<Vec<_>>()
        .join("\n");
    format!(
        r#"{color}
{helper}
struct Material {{{material}}};
@group(0) @binding(0) var<storage,read> inputs: array<vec4<f32>>;
@group(0) @binding(1) var<storage,read_write> outputs: array<vec4<f32>>;
@compute @workgroup_size(1)
fn cs_probe(@builtin(global_invocation_id) id: vec3<u32>) {{
    let v = inputs[id.x];
    outputs[id.x] = vec4<f32>(stock_muzzle_flash_radiance(v.rgb,v.w),v.w);
}}
"#
    )
}

fn parsed_probe() -> naga::Module {
    let source = probe_source();
    let module = naga::front::wgsl::parse_str(&source)
        .unwrap_or_else(|error| panic!("{}", error.emit_to_string(&source)));
    naga::valid::Validator::new(
        naga::valid::ValidationFlags::all(),
        naga::valid::Capabilities::empty(),
    )
    .validate(&module)
    .expect("actual muzzle helper must validate");
    module
}

#[test]
fn muzzle_actual_helper_retains_material_abi_and_explicit_negative_lane() {
    let module = parsed_probe();
    let (_, material) = module
        .types
        .iter()
        .find(|(_, ty)| ty.name.as_deref() == Some("Material"))
        .unwrap();
    let naga::TypeInner::Struct { members, span } = &material.inner else {
        panic!("Material must be struct")
    };
    assert_eq!(*span as usize, std::mem::size_of::<super::MaterialUbo>());
    let tail = members
        .iter()
        .find(|m| m.name.as_deref() == Some("local_specular"))
        .unwrap();
    assert_eq!(
        tail.offset as usize,
        std::mem::offset_of!(super::MaterialUbo, local_specular)
    );
    assert_eq!(tail.offset, 112);
    assert_eq!(*span, 128); // existing scalar byte124; no new public layout
    let source = include_str!("shader3d.wgsl");
    assert!(
        source.contains("if (!(encoded > 0.0)) { return 0.0; }") &&
        source.contains("let wet = uniform_cloth_wetness(encoded, uv, footprint);") &&
        source.contains("if (wet <= 0.0) { return rgb; }"),
        "negative flash marker must not wet cloth"
    );
    assert!(source.contains("if (material.local_specular.w == -1.0)"));
    let override_at = source
        .find("rgb = stock_muzzle_flash_radiance(base.rgb, linear);")
        .unwrap();
    assert!(
        source.find("var rgb = shade(").unwrap() < override_at,
        "derivatives must remain evaluated uniformly"
    );
    assert!(source[override_at..].contains("rgb = apply_fog(rgb, in.world_pos);"));
    assert!(source[override_at..].contains("rgb = mix(fog_rgb, rgb, in.fog);"));
}

#[test]
fn muzzle_actual_gpu_srgb_transfer_and_bounded_self_emission() {
    let (device, queue) =
        super::cull::tests::headless().expect("muzzle helper proof needs a test device");
    let mut cases = Vec::new();
    for mode in [0.0, 1.0] {
        for value in 0..=255 {
            let v = value as f32 / 255.0;
            cases.push([v, v * 0.96, v * 0.80, mode]);
        }
        cases.push([-0.5, 1.5, 0.04, mode]);
        cases.push([1.0, 0.96, 0.80, mode]);
    }
    let bytes = (cases.len() * 16) as u64;
    let input = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
        label: Some("muzzle_authored_palette_cases"),
        contents: bytemuck::cast_slice(&cases),
        usage: wgpu::BufferUsages::STORAGE,
    });
    let output = device.create_buffer(&wgpu::BufferDescriptor {
        label: Some("muzzle_actual_results"),
        size: bytes,
        mapped_at_creation: false,
        usage: wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_SRC,
    });
    let staging = device.create_buffer(&wgpu::BufferDescriptor {
        label: Some("muzzle_readback"),
        size: bytes,
        mapped_at_creation: false,
        usage: wgpu::BufferUsages::MAP_READ | wgpu::BufferUsages::COPY_DST,
    });
    let shader = device.create_shader_module(wgpu::ShaderModuleDescriptor {
        label: Some("actual_stock_muzzle_helper"),
        source: wgpu::ShaderSource::Naga(std::borrow::Cow::Owned(parsed_probe())),
    });
    let pipeline = device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
        label: Some("muzzle_probe"),
        layout: None,
        module: &shader,
        entry_point: Some("cs_probe"),
        compilation_options: Default::default(),
        cache: None,
    });
    let bind = device.create_bind_group(&wgpu::BindGroupDescriptor {
        label: Some("muzzle_probe_inputs"),
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
        pass.dispatch_workgroups(cases.len() as u32, 1, 1);
    }
    encoder.copy_buffer_to_buffer(&output, 0, &staging, 0, bytes);
    queue.submit(Some(encoder.finish()));
    let slice = staging.slice(..);
    let (sender, receiver) = std::sync::mpsc::channel();
    slice.map_async(wgpu::MapMode::Read, move |result| {
        let _ = sender.send(result);
    });
    device.poll(wgpu::PollType::wait_indefinitely()).unwrap();
    receiver.recv().unwrap().unwrap();
    let data = slice.get_mapped_range();
    let actual: &[[f32; 4]] = bytemuck::cast_slice(&data);
    let decode = |x: f32| {
        if x <= 0.04045 {
            x / 12.92
        } else {
            ((x + 0.055) / 1.055).powf(2.4)
        }
    };
    for (i, (input, row)) in cases.iter().zip(actual).enumerate() {
        for channel in 0..3 {
            let bounded = input[channel].clamp(0.0, 1.0);
            let expected = if input[3] > 0.5 {
                decode(bounded) * 1.25
            } else {
                bounded
            };
            assert!(row[channel].is_finite() && row[channel] >= 0.0 && row[channel] <= 1.25001);
            assert!(
                (row[channel] - expected).abs() < 0.00005,
                "case{i} channel{channel}: {row:?} expected{expected}"
            );
        }
        assert_eq!(row[3], input[3]);
    }
    drop(data);
    staging.unmap();
}
