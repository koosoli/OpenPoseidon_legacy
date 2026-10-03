use wgpu::util::DeviceExt;

fn probe_source() -> String {
    let source = include_str!("shader3d.wgsl");
    let helper = source.split("// BEGIN_UNIFORM_CLOTH_HELPERS").nth(1).unwrap()
        .split_once('\n').unwrap().1.split("// END_UNIFORM_CLOTH_HELPERS").next().unwrap();
    let material = source.split("struct Material {").nth(1).unwrap().split("};").next().unwrap();
    let color = include_str!("../shaders/color.wgsl").lines()
        .filter(|line| !line.starts_with('#')).collect::<Vec<_>>().join("\n");
    format!(r#"{color}
{helper}
struct Material {{{material}}};
@group(0) @binding(0) var<storage,read> inputs: array<vec4<f32>>;
@group(0) @binding(1) var<storage,read_write> outputs: array<vec4<f32>>;
@compute @workgroup_size(1)
fn cs_probe(@builtin(global_invocation_id) id: vec3<u32>) {{
    let v = inputs[id.x];
    let rgb = uniform_cloth_color(vec3<f32>(0.02,0.15,0.6),v.z,v.xy,vec2<f32>(v.w));
    outputs[id.x] = vec4<f32>(rgb, uniform_cloth_mask(v.xy,v.z,vec2<f32>(v.w)));
}}
"#)
}

fn parsed_probe() -> naga::Module {
    let source = probe_source();
    let module = naga::front::wgsl::parse_str(&source)
        .unwrap_or_else(|error| panic!("{}",error.emit_to_string(&source)));
    naga::valid::Validator::new(naga::valid::ValidationFlags::all(),naga::valid::Capabilities::empty())
        .validate(&module).expect("production cloth helper must validate");
    module
}

fn textile_helpers() -> &'static str {
    include_str!("../shaders/shading.wgsl")
        .split("// BEGIN_WET_TEXTILE_HELPERS").nth(1).unwrap()
        .split_once('\n').unwrap().1.split("// END_WET_TEXTILE_HELPERS").next().unwrap()
}

#[test]
fn actual_wet_textile_helpers_validate_and_paths_keep_source_admission() {
    parsed_probe(); // Includes the actual shared absorption/sheen mask decoder.
    let helpers = textile_helpers();
    let module = naga::front::wgsl::parse_str(helpers).unwrap();
    naga::valid::Validator::new(naga::valid::ValidationFlags::all(), naga::valid::Capabilities::empty())
        .validate(&module).expect("actual wet textile helpers must validate");
    let direct = include_str!("shader3d.wgsl");
    let retained = include_str!("gpu_driven.wgsl");
    assert!(direct.contains("m.wet_cloth = uniform_cloth_wetness(material.local_specular.w, in.uv, uniform_uv_footprint);"));
    assert!(retained.contains("m.wet_cloth = 0.0;"));
    assert!(!retained.contains("uniform_cloth_wetness("));
    let shared = include_str!("../shaders/shading.wgsl");
    assert!(shared.contains("if (mat.wet_cloth > 0.0 && linear > 0.5)"));
    assert!(shared.contains("let cloth_n = surface_normal(geo_normal, world_pos, dwx, dwy);"));
    assert!(shared.contains("* amb_ao * cavity * response.y"));
    assert!(shared.contains("* sun_vis;"));
    assert!(shared.contains("rgb = rgb * (1.0 - response.x * response.y)"));
}

#[test]
fn actual_wet_textile_response_is_broad_bounded_and_reallocates_energy_on_gpu() {
    let (device, queue) = super::cull::tests::headless().expect("wet textile proof needs a device");
    let source = format!(r#"{}
@group(0) @binding(0) var<storage, read> inputs: array<vec4<f32>>;
@group(0) @binding(1) var<storage, read_write> outputs: array<vec4<f32>>;
@compute @workgroup_size(1)
fn cs_probe(@builtin(global_invocation_id) id: vec3<u32>) {{
    let v = inputs[id.x];
    let r = wet_textile_response(v.x, v.y, v.z);
    outputs[id.x * 2u] = r;
    let lobe = wet_textile_sun_lobe(v.w, 1.0, r);
    let back = wet_textile_sun_lobe(v.w, -1.0, r);
    // Unit white substrate under unit uniform sky: reflected share replaces
    // the matching underlying energy exactly, even at the grazing ceiling.
    let white_sky = (1.0 - r.x * r.y) + r.x * r.y;
    outputs[id.x * 2u + 1u] = vec4<f32>(lobe, back, white_sky, 1.0 - r.x * r.y);
}}
"#, textile_helpers());
    let shader = device.create_shader_module(wgpu::ShaderModuleDescriptor {
        label: Some("actual bounded wet textile response"), source: wgpu::ShaderSource::Wgsl(source.into()),
    });
    let input: Vec<[f32;4]> = vec![
        [0.0, 0.5, 0.001, 1.0], [-1.0, 0.5, 0.001, 1.0],
        [0.5, 1.0, 0.001, 1.0], [1.0, 0.0, 0.001, 1.0],
        [1.0, 1.0, 0.08, 1.0], [1.0, 0.5, 0.001, 0.8],
        [1.0, 1.0, 0.001, -1.0], [0.38, 0.5, 0.04, 1.0],
    ];
    let inputs = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
        label: Some("wet textile angle/footprint cases"), contents: bytemuck::cast_slice(&input),
        usage: wgpu::BufferUsages::STORAGE,
    });
    let bytes = (input.len() * 32) as u64;
    let outputs = device.create_buffer(&wgpu::BufferDescriptor {
        label: Some("wet textile actual outputs"), size: bytes, mapped_at_creation: false,
        usage: wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_SRC,
    });
    let staging = device.create_buffer(&wgpu::BufferDescriptor {
        label: Some("wet textile readback"), size: bytes, mapped_at_creation: false,
        usage: wgpu::BufferUsages::MAP_READ | wgpu::BufferUsages::COPY_DST,
    });
    let pipeline = device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
        label: Some("actual wet textile probe"), layout: None, module: &shader,
        entry_point: Some("cs_probe"), compilation_options: Default::default(), cache: None,
    });
    let bindings = device.create_bind_group(&wgpu::BindGroupDescriptor {
        label: Some("wet textile cases/results"), layout: &pipeline.get_bind_group_layout(0),
        entries: &[
            wgpu::BindGroupEntry { binding: 0, resource: inputs.as_entire_binding() },
            wgpu::BindGroupEntry { binding: 1, resource: outputs.as_entire_binding() },
        ],
    });
    let mut encoder = device.create_command_encoder(&wgpu::CommandEncoderDescriptor::default());
    {
        let mut pass = encoder.begin_compute_pass(&wgpu::ComputePassDescriptor::default());
        pass.set_pipeline(&pipeline); pass.set_bind_group(0, &bindings, &[]);
        pass.dispatch_workgroups(input.len() as u32, 1, 1);
    }
    encoder.copy_buffer_to_buffer(&outputs, 0, &staging, 0, bytes);
    queue.submit(Some(encoder.finish()));
    let slice = staging.slice(..); let (sender, receiver) = std::sync::mpsc::channel();
    slice.map_async(wgpu::MapMode::Read, move |r| { let _ = sender.send(r); });
    device.poll(wgpu::PollType::wait_indefinitely()).unwrap(); receiver.recv().unwrap().unwrap();
    let data = slice.get_mapped_range(); let actual: &[[f32;4]] = bytemuck::cast_slice(&data);
    for (i, v) in input.iter().enumerate() {
        let response = actual[i * 2]; let light = actual[i * 2 + 1];
        assert!(response.iter().chain(light.iter()).all(|x| x.is_finite()));
        if v[0] <= 0.0 { assert_eq!(response, [0.0;4]); assert_eq!(light[0], 0.0); }
        else {
            assert!((response[0] - v[0] * (2.0 - v[0])).abs() < 0.00001);
            assert!(response[1] >= 0.019999 && response[1] <= 0.120001);
            assert!(response[2] >= 8.0 && response[2] <= 16.0);
            assert!(response[3] > 0.0 && response[3] < 0.015);
        }
        assert_eq!(light[1], 0.0, "backlit cloth must not acquire direct wet sheen");
        assert!((light[2] - 1.0).abs() < 0.000001, "white uniform sky gained energy");
        assert!(light[3] >= 0.879999 && light[3] <= 1.0);
        assert!(light[0] >= 0.0 && light[0] < 0.015);
    }
    assert_eq!(actual[4][2], 16.0); assert_eq!(actual[8][2], 8.0);
    assert!(actual[11][0] > 0.0 && actual[11][0] < actual[7][0]);
    assert_eq!(actual[13][0], 0.0);
    drop(data); staging.unmap();
}

#[test]
fn actual_uniform_helper_preserves_material_tail_layout() {
    let module = parsed_probe();
    let (_, material) = module.types.iter().find(|(_,ty)|ty.name.as_deref()==Some("Material")).unwrap();
    let naga::TypeInner::Struct {members,span} = &material.inner else {panic!("Material must be struct")};
    assert_eq!(*span as usize,std::mem::size_of::<super::MaterialUbo>());
    let tail = members.iter().find(|m|m.name.as_deref()==Some("local_specular")).unwrap();
    assert_eq!(tail.offset as usize,std::mem::offset_of!(super::MaterialUbo,local_specular));
    assert_eq!(tail.offset,112);
    assert_eq!(*span,128); // existing .w at byte124, no public ABI extension
}

#[test]
fn real_uniform_shader_preserves_skin_and_cloth_linear_hue_on_gpu() {
    // Probe calls the ACTUAL production WGSL helpers, not a copied Rust mask.
    // Headless GPU absence is a failure, never silently successful acceptance.
    let (device,queue)=super::cull::tests::headless().expect("uniform shader proof needs a test device");
    let cases: Vec<([f32;4],f32)> = vec![
        ([0.38,0.10,0.0,0.001],0.0), // exact dry
        ([0.38,0.10,0.5,0.001],0.5), // separate authored cloth
        ([0.38,0.10,2.08,0.001],0.08), // first actual damp history band
        ([0.38,0.10,2.2,0.001],0.2), // wet enough for installed admission gate
        ([0.38,0.10,2.38,0.001],0.38), // earlier subtle installed WB proof
        ([0.38,0.10,1.0,0.001],1.0),
        ([0.38,0.10,3.0,0.001],1.0), // west main uniform
        ([0.10,0.10,3.0,0.001],1.0),
        ([0.28,0.28,3.0,0.001],0.0), // photographed west hands
        ([0.48,0.28,3.0,0.001],0.0),
        ([0.52,0.28,3.0,0.001],0.0),
        ([0.73,0.28,3.0,0.001],0.0),
        ([0.38,0.28,3.0,0.001],1.0), // pelvis cloth between hands must still wet
        ([0.62,0.28,3.0,0.001],1.0),
        ([0.80,0.42,3.0,0.001],0.0), // helmet
        ([0.90,0.65,3.0,0.001],0.0), // boots
        ([0.10,0.80,3.0,0.001],0.0), // baked far LOD face/body
        ([0.65,0.10,5.0,0.001],1.0), // east main uniform
        ([0.10,0.10,5.0,0.001],1.0),
        ([0.53,0.29,5.0,0.001],0.0), // photographed east hands
        ([0.73,0.29,5.0,0.001],0.0),
        ([0.78,0.29,5.0,0.001],0.0),
        ([0.97,0.29,5.0,0.001],0.0),
        ([0.62,0.29,5.0,0.001],1.0), // pelvis cloth between hands
        ([0.88,0.29,5.0,0.001],1.0),
        ([0.38,0.45,5.0,0.001],0.0), // east boots
        ([0.65,0.48,5.0,0.001],0.0),
        ([0.60,0.65,5.0,0.001],0.0),
        ([0.10,0.80,5.0,0.001],0.0), // baked far LOD face/body
        ([0.28,0.28,3.0,0.05],0.0), // minification never re-admits skin
        ([0.53,0.29,5.0,0.05],0.0),
    ];
    let input: Vec<[f32;4]>=cases.iter().map(|(v,_)|*v).collect();
    let bytes=(input.len()*16) as u64;
    let input_buffer=device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
        label:Some("cloth_uv_cases"),contents:bytemuck::cast_slice(&input),usage:wgpu::BufferUsages::STORAGE,
    });
    let output=device.create_buffer(&wgpu::BufferDescriptor {
        label:Some("cloth_actual_results"),size:bytes,mapped_at_creation:false,
        usage:wgpu::BufferUsages::STORAGE|wgpu::BufferUsages::COPY_SRC,
    });
    let staging=device.create_buffer(&wgpu::BufferDescriptor {
        label:Some("cloth_readback"),size:bytes,mapped_at_creation:false,
        usage:wgpu::BufferUsages::MAP_READ|wgpu::BufferUsages::COPY_DST,
    });
    let shader=device.create_shader_module(wgpu::ShaderModuleDescriptor {
        label:Some("actual_uniform_cloth_helper"),
        source:wgpu::ShaderSource::Naga(std::borrow::Cow::Owned(parsed_probe())),
    });
    let pipeline=device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
        label:Some("cloth_probe"),layout:None,module:&shader,entry_point:Some("cs_probe"),
        compilation_options:Default::default(),cache:None,
    });
    let bind=device.create_bind_group(&wgpu::BindGroupDescriptor {
        label:Some("cloth_probe_inputs"),layout:&pipeline.get_bind_group_layout(0),
        entries:&[
            wgpu::BindGroupEntry {binding:0,resource:input_buffer.as_entire_binding()},
            wgpu::BindGroupEntry {binding:1,resource:output.as_entire_binding()},
        ],
    });
    let mut encoder=device.create_command_encoder(&Default::default());
    {
        let mut pass=encoder.begin_compute_pass(&Default::default());
        pass.set_pipeline(&pipeline);pass.set_bind_group(0,&bind,&[]);
        pass.dispatch_workgroups(cases.len() as u32,1,1);
    }
    encoder.copy_buffer_to_buffer(&output,0,&staging,0,bytes);
    queue.submit(Some(encoder.finish()));
    let slice=staging.slice(..);let (sender,receiver)=std::sync::mpsc::channel();
    slice.map_async(wgpu::MapMode::Read,move |result| {let _=sender.send(result);});
    device.poll(wgpu::PollType::wait_indefinitely()).unwrap();receiver.recv().unwrap().unwrap();
    let data=slice.get_mapped_range();let actual: &[[f32;4]]=bytemuck::cast_slice(&data);
    let decode=|x:f32|if x<=0.04045 {x/12.92} else {((x+0.055)/1.055).powf(2.4)};
    for (i,((input,wet),row)) in cases.iter().zip(actual).enumerate() {
        for channel in 0..3 {
            let original=[0.02,0.15,0.6][channel];
            if *wet==0.0 {
                assert_eq!(row[channel],original,"excluded/dry changed at case{i}: {input:?}");
            } else {
                let ratio=decode(row[channel])/decode(original);
                let expected = 1.0-0.55*wet*(2.0-wet);
                assert!((ratio-expected).abs()<0.00005,
                    "linear cloth hue/brightness changed at case{i} channel{channel}: ratio{ratio}");
                if (*wet-0.38).abs()<0.00001 {
                    assert!(ratio<0.67,"ordinary retained dampness must have visible absorption, not the old8% linear change");
                }
            }
        }
    }
    drop(data);staging.unmap();
}
