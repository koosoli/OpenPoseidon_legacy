use wgpu::util::DeviceExt;

const PRODUCTION: &str = include_str!("gpu_driven.wgsl");

// Verbatim production types/function; no duplicate receiver implementation.
fn declaration<'a>(start: &str) -> &'a str {
    let begin = PRODUCTION.find(start).unwrap_or_else(|| panic!("missing {start}"));
    let brace = PRODUCTION[begin..].find('{').unwrap() + begin;
    let mut depth = 0;
    for (at, byte) in PRODUCTION.bytes().enumerate().skip(brace) {
        match byte {
            b'{' => depth += 1,
            b'}' => {
                depth -= 1;
                if depth == 0 { return &PRODUCTION[begin..=at]; }
            }
            _ => {}
        }
    }
    panic!("unterminated {start}")
}

fn probe_module() -> naga::Module {
    let mut source = String::new();
    for name in ["struct SectionMaterial", "struct VsOut"] {
        source.push_str(declaration(name));
        source.push_str(";\n");
    }
    for name in ["SECTION_REFLECTIVE", "SECTION_NIGHT_EMITTER", "SECTION_NO_OBJECT_SNOW"] {
        let begin = PRODUCTION.find(&format!("const {name}:")).unwrap();
        let end = PRODUCTION[begin..].find(';').unwrap()+begin+1;
        source.push_str(&PRODUCTION[begin..end]);
        source.push('\n');
    }
    source.push_str("@group(0) @binding(0) var<storage,read> section_materials:array<SectionMaterial>;\n");
    source.push_str("@group(0) @binding(1) var<storage,read_write> results:array<u32>;\n");
    source.push_str(declaration("fn retained_snow_receiver("));
    source.push_str(r#"
@compute @workgroup_size(1) fn cs_probe(@builtin(global_invocation_id) id:vec3<u32>) {
    var v:VsOut;
    v.section=id.x; v.surface_receivers=1u; v.normal=vec3<f32>(0.0,1.0,0.0);
    if (id.x==7u) { v.surface_receivers=2u; } // rain proof cannot imply snow proof
    if (id.x==9u) { v.normal=vec3<f32>(0.0); }
    results[id.x]=select(0u,1u,retained_snow_receiver(v));
}
"#);
    naga::front::wgsl::parse_str(&source).expect("actual receiver/type probe must parse")
}

#[test]
fn church_section_veto_uses_actual_types_and_shared_colour_prepass_receiver() {
    let module = probe_module();
    naga::valid::Validator::new(naga::valid::ValidationFlags::all(), naga::valid::Capabilities::all())
        .validate(&module).expect("actual receiver probe must validate");
    assert_eq!(crate::ffi::WGR_MODEL_SECTION_NO_OBJECT_SNOW, 128);
    assert!(PRODUCTION.matches("retained_snow_receiver(in)").count() >= 2,
        "colour and normal prepass must share the clock veto");
    assert!(!declaration("fn retained_snow_receiver(").contains("alpha_ref"));
}

#[test]
fn church_clock_bit128_actual_gpu_veto_preserves_fixed_solid_and_cutout() {
    let (device, queue) = super::cull::tests::headless()
        .expect("Church section veto acceptance requires an actual device");
    let module = probe_module();
    let material = module.types.iter().find(|(_, ty)| ty.name.as_deref()==Some("SectionMaterial"))
        .expect("actual production material type").1;
    let (members, stride) = match &material.inner {
        naga::TypeInner::Struct { members, span } => (members, *span as usize),
        _ => panic!("material must remain a struct"),
    };
    let offset=|name:&str| members.iter().find(|m|m.name.as_deref()==Some(name)).unwrap().offset as usize;
    let flags=[0u32,128,192,1,2,64,129,0,0,0];
    let mut bytes=vec![0u8;stride*flags.len()];
    for (i, flag) in flags.iter().enumerate() {
        bytes[i*stride+offset("flags")..i*stride+offset("flags")+4].copy_from_slice(&flag.to_le_bytes());
        // Positive0 is solid; positive5 is cutout. The original alpha still
        // drives discard/A2C independently of this RGB-only snow receiver.
        let alpha_ref=if i==0 { 0.0f32 } else { 0.5f32 };
        bytes[i*stride+offset("alpha_ref")..i*stride+offset("alpha_ref")+4].copy_from_slice(&alpha_ref.to_le_bytes());
    }
    bytes[8*stride+offset("emissive")..8*stride+offset("emissive")+4].copy_from_slice(&1f32.to_le_bytes());
    let materials=device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
        label:Some("actual_church_section_materials"),contents:&bytes,usage:wgpu::BufferUsages::STORAGE,
    });
    let output=device.create_buffer(&wgpu::BufferDescriptor {
        label:Some("church_section_veto_results"),size:40,usage:wgpu::BufferUsages::STORAGE|wgpu::BufferUsages::COPY_SRC,mapped_at_creation:false,
    });
    let staging=device.create_buffer(&wgpu::BufferDescriptor {
        label:None,size:40,usage:wgpu::BufferUsages::COPY_DST|wgpu::BufferUsages::MAP_READ,mapped_at_creation:false,
    });
    let shader=device.create_shader_module(wgpu::ShaderModuleDescriptor {
        label:Some("actual_church_section_receiver"),source:wgpu::ShaderSource::Naga(std::borrow::Cow::Owned(module)),
    });
    let pipeline=device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
        label:None,layout:None,module:&shader,entry_point:Some("cs_probe"),compilation_options:Default::default(),cache:None,
    });
    let bindings=device.create_bind_group(&wgpu::BindGroupDescriptor {
        label:None,layout:&pipeline.get_bind_group_layout(0),entries:&[
            wgpu::BindGroupEntry{binding:0,resource:materials.as_entire_binding()},
            wgpu::BindGroupEntry{binding:1,resource:output.as_entire_binding()},
        ],
    });
    let mut encoder=device.create_command_encoder(&Default::default());
    {
        let mut pass=encoder.begin_compute_pass(&Default::default());
        pass.set_pipeline(&pipeline);pass.set_bind_group(0,&bindings,&[]);pass.dispatch_workgroups(10,1,1);
    }
    encoder.copy_buffer_to_buffer(&output,0,&staging,0,40);queue.submit(Some(encoder.finish()));
    let slice=staging.slice(..);let (tx,rx)=std::sync::mpsc::channel();
    slice.map_async(wgpu::MapMode::Read,move|result|{let _=tx.send(result);});
    device.poll(wgpu::PollType::wait_indefinitely()).unwrap();rx.recv().unwrap().unwrap();
    let data=slice.get_mapped_range();
    assert_eq!(bytemuck::cast_slice::<u8,u32>(&data), &[1,0,0,0,0,1,0,0,0,0]);
    drop(data);staging.unmap();
}
