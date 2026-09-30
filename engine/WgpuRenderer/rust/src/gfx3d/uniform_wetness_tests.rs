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
                assert!((ratio-(1.0-0.22*wet)).abs()<0.00005,
                    "linear cloth hue/brightness changed at case{i} channel{channel}: ratio{ratio}");
            }
        }
    }
    drop(data);staging.unmap();
}
