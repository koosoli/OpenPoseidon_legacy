use naga_oil::compose::{NagaModuleDescriptor, ShaderDefValue};
use wgpu::util::DeviceExt;

const PRODUCTION: &str = include_str!("gpu_driven.wgsl");

fn channels_probe(mask: bool) -> naga::Module {
    // Execute the real production helper, not a Rust copy of the formula.
    let helper = if mask {
        "native_pbr_mask_channels"
    } else {
        "native_pbr_scaled_channels"
    };
    let start = PRODUCTION.find(&format!("fn {helper}(")).unwrap();
    let end = start + PRODUCTION[start..].find("\n}").unwrap() + 2;
    let mut source = PRODUCTION[start..end].to_owned();
    let scales = if mask {
        "vec4<f32>(0.0,0.0,0.0,0.0), vec4<f32>(1.0,1.0,1.0,1.0), vec4<f32>(0.0,0.5,2.0,1.0), vec4<f32>(2.0,3.0,0.0,1.0), vec4<f32>(1.0,1.0,0.0,0.0)"
    } else {
        "vec4<f32>(1.0,1.0,0.0,0.0), vec4<f32>(0.5,0.0,0.0,0.0), vec4<f32>(2.0,2.0,0.0,0.0), vec4<f32>(0.0,0.25,0.0,0.0), vec4<f32>(0.0,0.5,2.0,1.0)"
    };
    source.push_str(&format!(r#"
@group(0) @binding(0) var<storage,read_write> output:array<vec4<f32>>;
@compute @workgroup_size(1) fn cs_probe(@builtin(global_invocation_id) id:vec3<u32>) {{
    let channels = array<vec3<f32>,5>(vec3<f32>(0.8,0.7,0.6), vec3<f32>(0.8,0.7,0.6),
                                    vec3<f32>(0.8,0.7,0.6), vec3<f32>(0.8,0.7,0.6), vec3<f32>(0.8,0.7,0.6));
    let scales = array<vec4<f32>,5>({scales});
    output[id.x] = vec4<f32>({helper}(channels[id.x],scales[id.x]),0.37);
}}
"#));
    let module = naga::front::wgsl::parse_str(&source).unwrap();
    naga::valid::Validator::new(
        naga::valid::ValidationFlags::all(),
        naga::valid::Capabilities::all(),
    )
    .validate(&module)
    .unwrap();
    module
}

fn ordered_mask_probe() -> naga::Module {
    let mut source = String::new();
    for helper in ["native_pbr_detail_mask", "native_pbr_ordered_weights"] {
        let start = PRODUCTION.find(&format!("fn {helper}(")).unwrap();
        let end = start + PRODUCTION[start..].find("\n}").unwrap() + 2;
        source.push_str(&PRODUCTION[start..end]);
        source.push('\n');
    }
    source.push_str(r#"
@group(0) @binding(0) var<storage,read_write> output:array<vec4<f32>>;
@compute @workgroup_size(1) fn cs_probe(@builtin(global_invocation_id) id:vec3<u32>) {
    let masks = array<vec3<f32>,5>(vec3<f32>(1.0,1.0,0.0), vec3<f32>(0.5,0.5,0.5),
        vec3<f32>(1.0,0.0,1.0), vec3<f32>(0.0), vec3<f32>(0.4,0.0,0.0));
    // Last case exercises the interior detail-remap value, not only endpoints.
    var mask = masks[id.x];
    if (id.x == 4u) { mask.x = native_pbr_detail_mask(mask.x,0.6,1.0,0.05); }
    output[id.x] = native_pbr_ordered_weights(mask);
}
"#);
    let module = naga::front::wgsl::parse_str(&source).unwrap();
    naga::valid::Validator::new(naga::valid::ValidationFlags::all(), naga::valid::Capabilities::all())
        .validate(&module).unwrap();
    module
}

#[test]
fn native_multi_ordered_preview_helpers_validate() {
    let _ = ordered_mask_probe();
}

#[test]
fn native_multi_ordered_actual_gpu_resolves_overlapping_masks_and_detail_remap() {
    let actual = gpu_channels_result(ordered_mask_probe());
    // Full R+G selects the later layer. Half R+G+B leaves 1/8 base,
    // 1/8 second, 1/4 third, 1/2 fourth; no normalized additive average.
    let expected = [0.0,0.0,1.0,0.0, 0.125,0.125,0.25,0.5,
                    0.0,0.0,0.0,1.0, 1.0,0.0,0.0,0.0, 0.5,0.5,0.0,0.0];
    for (actual, expected) in actual.iter().zip(expected) {
        assert!((actual - expected).abs() < 1e-6, "{actual} != {expected}");
    }
}

#[test]
fn native_pbr_basic_scaled_preview_and_legacy_shaders_compose() {
    for (preview,tangents) in [(false,false), (true,false), (true,true)] {
        let mut defs = crate::shaders::shader_defs();
        if preview {
            defs.insert("NATIVE_PBR_PREVIEW".to_owned(), ShaderDefValue::Bool(true));
        } else {
            defs.remove("NATIVE_PBR_PREVIEW");
        }
        if tangents { defs.insert("NATIVE_PBR_TANGENTS".to_owned(),ShaderDefValue::Bool(true)); }
        else { defs.remove("NATIVE_PBR_TANGENTS"); }
        for (source, path) in [
            (PRODUCTION, "gfx3d/gpu_driven.wgsl"),
            (
                include_str!("gpu_driven_shadow.wgsl"),
                "gfx3d/gpu_driven_shadow.wgsl",
            ),
            (include_str!("shader3d.wgsl"), "gfx3d/shader3d.wgsl"),
        ] {
            let mut composer = crate::shaders::build_composer();
            let module = composer
                .make_naga_module(NagaModuleDescriptor {
                    source,
                    file_path: path,
                    shader_defs: defs.clone(),
                    ..Default::default()
                })
                .unwrap_or_else(|e| {
                    panic!("{path} preview={preview}: {}", e.emit_to_string(&composer))
                });
            if path == "gfx3d/gpu_driven.wgsl" {
                let output = module.types.iter().find_map(|(_,ty)| {
                    if ty.name.as_deref()==Some("VsOut") {
                        if let naga::TypeInner::Struct {members,..}=&ty.inner {return Some(members);}
                    }
                    None
                }).unwrap();
                let locations:Vec<_>=output.iter().filter_map(|m| match &m.binding {
                    Some(naga::Binding::Location {location,..})=>Some(*location),_=>None
                }).collect();
                assert!(locations.len()<=16 && locations.iter().all(|&v|v<16));
                assert_eq!(output.iter().any(|m|m.name.as_deref()==Some("authored_tangent")),tangents);
            }
            if path != "gfx3d/shader3d.wgsl" {
                let layout = module.types.iter().find_map(|(_, ty)| {
                    if let naga::TypeInner::Struct { members, span } = &ty.inner {
                        members.iter().find(|m| m.name.as_deref() == Some("srgb_bcr_slots"))
                            .map(|m| (m.offset, *span))
                    } else { None }
                });
                assert_eq!(layout, Some((368, 416)), "{path} material stride");
                let normal_layout: Vec<_> = module.types.iter().filter_map(|(_, ty)| {
                    if let naga::TypeInner::Struct { members, .. } = &ty.inner {
                        let fields: Vec<_> = members.iter().filter_map(|m| {
                            match m.name.as_deref() {
                                Some("global_normal_uv") | Some("detail_normal_strength") => Some(m.offset),
                                _ => None,
                            }
                        }).collect();
                        if !fields.is_empty() { return Some(fields); }
                    }
                    None
                }).flatten().collect();
                assert_eq!(normal_layout, [384, 400], "{path} independent normal fields");
            }
        }
    }
}

fn ordered_normal_probe() -> naga::Module {
    let mut source = String::new();
    for helper in ["native_pbr_detail_normal", "native_pbr_global_normal",
                   "native_pbr_composed_normal", "native_pbr_ordered_weights"] {
        let start = PRODUCTION.find(&format!("fn {helper}(")).unwrap();
        let end = start + PRODUCTION[start..].find("\n}").unwrap() + 2;
        source.push_str(&PRODUCTION[start..end]);
        source.push('\n');
    }
    source.push_str(r#"
@group(0) @binding(0) var<storage,read_write> output:array<vec4<f32>>;
@compute @workgroup_size(1) fn cs_probe(@builtin(global_invocation_id) id:vec3<u32>) {
    var geo = vec3<f32>(0.0,0.0,1.0);
    var tangent = vec3<f32>(1.0,0.0,0.0);
    var bitangent = vec3<f32>(0.0,1.0,0.0);
    var global = native_pbr_global_normal(vec2<f32>(0.5),0.0);
    var detail = native_pbr_detail_normal(vec2<f32>(0.8,0.5),0.5);
    if (id.x == 1u) {
        global = native_pbr_global_normal(vec2<f32>(0.75,0.5),0.8);
        detail = native_pbr_detail_normal(vec2<f32>(0.5),1.0);
    }
    if (id.x == 2u) { detail = native_pbr_detail_normal(vec2<f32>(1.0,0.5),2.0); }
    if (id.x == 3u) {
        let w = native_pbr_ordered_weights(vec3<f32>(0.5));
        detail = native_pbr_detail_normal(vec2<f32>(1.0,0.5),2.0) * w.x +
            native_pbr_detail_normal(vec2<f32>(0.5),0.0) * w.y +
            native_pbr_detail_normal(vec2<f32>(0.5,1.0),0.5) * w.z +
            native_pbr_detail_normal(vec2<f32>(0.25,0.5),1.0) * w.w;
    }
    if (id.x == 4u) {
        geo = vec3<f32>(0.0,1.0,0.0);
        bitangent = vec3<f32>(0.0,0.0,-1.0);
        global = native_pbr_global_normal(vec2<f32>(0.8,0.5),1.0);
        detail = native_pbr_detail_normal(vec2<f32>(0.5,0.8),1.0);
    }
    output[id.x] = vec4<f32>(native_pbr_composed_normal(geo,tangent,bitangent,global,detail),1.0);
}
"#);
    let module = naga::front::wgsl::parse_str(&source).unwrap();
    naga::valid::Validator::new(naga::valid::ValidationFlags::all(), naga::valid::Capabilities::all())
        .validate(&module).unwrap();
    module
}

#[test]
fn native_ordered_normal_helpers_validate() {
    let _ = ordered_normal_probe();
}

#[test]
fn native_ordered_normal_actual_gpu_keeps_detail_strength_and_rotates_global_independently() {
    let actual = gpu_channels_result(ordered_normal_probe());
    let epsilon = 0.00001001358_f32;
    let global_x = 0.4 / (1.0 + epsilon).sqrt();
    let global_z = (0.84 + epsilon).sqrt() / (1.0 + epsilon).sqrt();
    let overlap_z = 0.125 + 0.75 * 0.75_f32.sqrt();
    let overlap_len = (0.125_f32.powi(2) + overlap_z.powi(2)).sqrt();
    let tilt_x = 0.6 / (1.0 + epsilon).sqrt();
    let tilt_y = (0.64 + epsilon).sqrt() / (1.0 + epsilon).sqrt();
    let expected = [0.3,0.0,0.91_f32.sqrt(),1.0,
        global_x,0.0,global_z,1.0, 1.0,0.0,0.0,1.0,
        0.0,0.125/overlap_len,overlap_z/overlap_len,1.0,
        0.8*tilt_x,0.8*tilt_y,-0.6,1.0];
    for (actual, expected) in actual.iter().zip(expected) {
        assert!((actual - expected).abs() < 2e-6, "{actual} != {expected}");
    }
}

#[test]
fn native_pbr_basic_actual_gpu_preserves_identity_zero_ao_and_clamps_after_scaling() {
    let actual = gpu_channels_result(channels_probe(false));
    let expected = [
        0.8, 0.7, 0.6, 0.37, 0.4, 0.0, 0.6, 0.37, 1.0, 1.0, 0.6, 0.37, 0.08, 0.175, 0.6, 0.37, 0.8,
        0.7, 0.6, 0.37,
    ];
    for (actual, expected) in actual.iter().zip(expected) {
        assert!((actual - expected).abs() < 1e-6, "{actual} != {expected}");
    }
}

#[test]
fn native_multi_mask_actual_gpu_preserves_off_identity_zero_and_saturates_each_rgb_channel() {
    let actual = gpu_channels_result(channels_probe(true));
    let expected = [
        0.8, 0.7, 0.6, 0.37, 0.8, 0.7, 0.6, 0.37, 0.0, 0.35, 1.0, 0.37, 1.0, 1.0, 0.0, 0.37, 0.8,
        0.7, 0.6, 0.37,
    ];
    for (actual, expected) in actual.iter().zip(expected) {
        assert!((actual - expected).abs() < 1e-6, "{actual} != {expected}");
    }
}

fn gpu_channels_result(module: naga::Module) -> Vec<f32> { gpu_channels_result_count(module,5) }

fn gpu_channels_result_count(module: naga::Module, count: u32) -> Vec<f32> {
    let bytes = count as u64 * 16;
    let (device, queue) =
        super::cull::tests::headless().expect("native PBR scale proof requires GPU");
    let output = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
        label: Some("native PBR scaled channels"),
        contents: &vec![0u8; bytes as usize],
        usage: wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_SRC,
    });
    let staging = device.create_buffer(&wgpu::BufferDescriptor {
        label: None,
        size: bytes,
        mapped_at_creation: false,
        usage: wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::MAP_READ,
    });
    let shader = device.create_shader_module(wgpu::ShaderModuleDescriptor {
        label: Some("production native PBR scale probe"),
        source: wgpu::ShaderSource::Naga(std::borrow::Cow::Owned(module)),
    });
    let pipeline = device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
        label: None,
        layout: None,
        module: &shader,
        entry_point: Some("cs_probe"),
        compilation_options: Default::default(),
        cache: None,
    });
    let bind = device.create_bind_group(&wgpu::BindGroupDescriptor {
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
        pass.set_bind_group(0, &bind, &[]);
        pass.dispatch_workgroups(count, 1, 1);
    }
    encoder.copy_buffer_to_buffer(&output, 0, &staging, 0, bytes);
    queue.submit([encoder.finish()]);
    let (tx, rx) = std::sync::mpsc::channel();
    staging.slice(..).map_async(wgpu::MapMode::Read, move |r| {
        let _ = tx.send(r);
    });
    device.poll(wgpu::PollType::wait_indefinitely()).unwrap();
    rx.recv().unwrap().unwrap();
    let data = staging.slice(..).get_mapped_range();
    let actual = bytemuck::cast_slice::<u8, f32>(&data).to_vec();
    drop(data);
    staging.unmap();
    actual
}

fn authored_tangent_probe() -> naga::Module {
    let mut source=String::new();
    for name in ["native_pbr_valid_axis", "native_pbr_authored_frame", "native_pbr_authored_composed_normal"] {
        let start=PRODUCTION.find(&format!("fn {name}(")).unwrap();
        let end=start+PRODUCTION[start..].find("\n}").unwrap()+2;
        source.push_str(&PRODUCTION[start..end]); source.push('\n');
    }
    source.push_str(r#"
@group(0) @binding(0) var<storage,read_write> output:array<vec4<f32>>;
@compute @workgroup_size(1) fn cs_probe(@builtin(global_invocation_id) id:vec3<u32>) {
    var m=mat3x3<f32>(vec3<f32>(2.0,0.0,0.0),vec3<f32>(0.0,1.0,0.0),vec3<f32>(0.0,0.0,0.5));
    var t=vec3<f32>(1.0,0.0,1.0);
    var b=vec3<f32>(0.0,1.0,0.0);
    if(id.x==1u) {m[0].x=-2.0;}
    if(id.x==2u) {m=mat3x3<f32>(vec3<f32>(1.0,0.0,0.0),vec3<f32>(0.0,1.0,0.0),vec3<f32>(0.0,0.0,1.0));b=vec3<f32>(0.1,1.0,0.1);}
    if(id.x==3u) {m[2]=vec3<f32>(0.0);}
    let frame=native_pbr_authored_frame(m,t,b,vec3<f32>(0.0,0.0,1.0));
    var value=frame[0];
    if(id.x==2u) {value=frame[1];}
    if(id.x==4u) {value=native_pbr_authored_composed_normal(vec3<f32>(0.0,0.0,0.8),vec3<f32>(1.0,0.0,0.0),vec3<f32>(0.0,1.0,0.0),vec3<f32>(0.4,0.0,0.9165151),vec3<f32>(0.3,0.2,0.9327379));}
    if(id.x==5u) {value=native_pbr_authored_composed_normal(vec3<f32>(0.0,0.0,-0.8),vec3<f32>(1.0,0.0,0.0),vec3<f32>(0.0,1.0,0.0),vec3<f32>(0.0,0.0,1.0),vec3<f32>(0.3,0.2,0.9327379));}
    output[id.x]=vec4<f32>(value,1.0);
}"#);
    let module=naga::front::wgsl::parse_str(&source).unwrap();
    naga::valid::Validator::new(naga::valid::ValidationFlags::all(),naga::valid::Capabilities::all()).validate(&module).unwrap();
    module
}
#[test]
fn authored_tangent_production_helpers_validate() {let _=authored_tangent_probe();}

#[test]
fn authored_tangent_actual_gpu_preserves_nonuniform_mirrored_and_interpolated_axes() {
    let actual=gpu_channels_result_count(authored_tangent_probe(),6);
    let x=2.0/4.25_f32.sqrt(); let z=0.5/4.25_f32.sqrt();
    let b=0.1/1.02_f32.sqrt(); let by=1.0/1.02_f32.sqrt();
    // Independent f64 quaternion oracle: interpolated N is deliberately not unit.
    let g=[0.4_f64,0.0,0.8*0.9165151];
    let length=(g[0]*g[0]+g[2]*g[2]).sqrt();let g=[g[0]/length,0.0,g[2]/length];
    let d=1.0+0.8*g[2]; let scale=1.0/(2.0*d).sqrt();
    let q=0.8*g[0]*scale;let w=d*scale;
    let v=[0.3_f64,0.2,0.8*0.9327379];
    let a=w*w-q*q;
    let rotated=[a*v[0]+2.0*w*q*v[2],(a+2.0*q*q)*v[1],a*v[2]-2.0*w*q*v[0]];
    let len=rotated.iter().map(|v|v*v).sum::<f64>().sqrt();
    let back_z=-0.8*0.9327379_f32;let back_len=(0.3_f32.powi(2)+0.2_f32.powi(2)+back_z*back_z).sqrt();
    let expected=[x,0.0,z,1.0,-x,0.0,z,1.0,b,by,b,1.0,0.0,0.0,0.0,1.0,
        (rotated[0]/len) as f32,(rotated[1]/len) as f32,(rotated[2]/len) as f32,1.0,
        0.3/back_len,0.2/back_len,back_z/back_len,1.0];
    for (actual,expected) in actual.iter().zip(expected) {assert!((actual-expected).abs()<2e-5,"{actual} != {expected}");}
}
