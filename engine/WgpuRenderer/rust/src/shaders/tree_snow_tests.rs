use naga_oil::compose::{NagaModuleDescriptor, ShaderDefValue};
use wgpu::util::DeviceExt;

const PROBE: &str = r#"
#import tree_snow::{tree_snow_height, tree_snow_cover_depth}
@group(0) @binding(0) var<storage, read_write> result: array<vec4<f32>>;
@compute @workgroup_size(8)
fn cs_probe(@builtin(global_invocation_id) id: vec3<u32>) {
    let i = id.x;
    var y = 14.0;
    if (i == 1u) { y = -6.0; }
    if (i == 2u) { y = 3.0; }
    if (i == 3u) { y = 7.0; }
    let proof = select(1.0, 0.0, i == 4u);
    let h = tree_snow_height(y, -6.0, 0.05, proof);
    let depth = select(0.18, 0.0, i == 5u);
    let exposed = select(1.0, 0.0, i == 6u);
    result[i] = vec4<f32>(h, tree_snow_cover_depth(h, depth, exposed),
        tree_snow_height(y, -6.0, 0.0, proof), tree_snow_cover_depth(h, 0.01, 0.8));
}
"#;

#[test]
fn tree_consumers_compose_enabled_and_disabled() {
    for disabled in [false, true] {
        for (source,path) in [
            (include_str!("../gfx3d/shader3d.wgsl"),"gfx3d/shader3d.wgsl"),
            (include_str!("../gfx3d/gpu_driven.wgsl"),"gfx3d/gpu_driven.wgsl"),
        ] {
            let mut composer = super::build_composer();
            let mut defs = std::collections::HashMap::new();
            if disabled { defs.insert("DISABLE_TREE_SNOW".into(),ShaderDefValue::Bool(true)); }
            defs.insert("NATIVE_AUTHORED_NORMALS".into(),ShaderDefValue::Bool(true));
            composer.make_naga_module(NagaModuleDescriptor{
                source,file_path:path,shader_defs:defs,..Default::default()
            }).unwrap_or_else(|e| panic!("{}",e.emit_to_string(&composer)));
        }
    }
}

#[test]
fn forest_coverage_diagnostic_composes_only_after_actual_cutout_discard() {
    let source=include_str!("../gfx3d/gpu_driven.wgsl");
    for disabled in [false,true] {
        let mut composer=super::build_composer();
        let mut defs=std::collections::HashMap::new();
        defs.insert("FOREST_SNOW_COVER_DEBUG".into(),ShaderDefValue::Bool(true));
        defs.insert("NATIVE_AUTHORED_NORMALS".into(),ShaderDefValue::Bool(true));
        if disabled {defs.insert("DISABLE_TREE_SNOW".into(),ShaderDefValue::Bool(true));}
        composer.make_naga_module(NagaModuleDescriptor {
            source,file_path:"gfx3d/gpu_driven.wgsl",shader_defs:defs,..Default::default()
        }).unwrap_or_else(|e|panic!("{}",e.emit_to_string(&composer)));
    }
    let debug=source.find("#ifdef FOREST_SNOW_COVER_DEBUG").unwrap();
    assert!(source.find("out_a = a2c_coverage(cut_a, sm.alpha_ref)").unwrap()<debug);
    assert!(source[..debug].contains("discard;"));
    let branch=&source[debug..source[debug..].find("#endif").unwrap()+debug];
    assert!(branch.contains("in.tree_snow.y == -2.0"));
    assert!(branch.contains("retained_tree_snow_cover(in)"));
    assert!(branch.contains("return vec4<f32>(coat_debug.rgb, out_a)"));
    let prepass=&source[source.find("fn fs_gpu_prepass").unwrap()..];
    assert!(!prepass.contains("forest_snow_diagnostic_colour"));
}

#[test]
fn forest_coverage_diagnostic_actual_gpu_keeps_source_negatives_and_raw_coat() {
    let (device,queue)=crate::gfx3d::cull::tests::headless().expect("forest coverage diagnostic requires an actual GPU");
    let source=r#"
#import tree_snow::{forest_snow_diagnostic_colour, forest_snow_map_cover}
@group(0) @binding(0) var<storage,read_write> result: array<vec4<f32>>;
@compute @workgroup_size(8) fn cs_probe(@builtin(global_invocation_id) id: vec3<u32>) {
    let i=id.x;
    var uv=vec2<f32>(0.375,0.10); var category=3u;
    var cover=forest_snow_map_cover(1.0,0.18,1.0,1.0);
    switch i {
        case 1u: {cover=forest_snow_map_cover(1.0,0.18,1.0,0.0);}
        case 2u: {cover=forest_snow_map_cover(1.0,0.18,0.0,1.0);}
        case 3u: {uv=vec2<f32>(0.375,0.375);}
        case 4u: {uv=vec2<f32>(0.9,0.9);}
        case 5u: {category=0u;}
        case 6u: {cover=0.25;}
        case 7u: {cover=bitcast<f32>(0x7fc00000u);}
        default: {}
    }
    result[i]=forest_snow_diagnostic_colour(uv,category,cover);
}
"#;
    let mut composer=super::build_composer();
    let module=composer.make_naga_module(NagaModuleDescriptor{source,file_path:"forest_cover_debug_probe.wgsl",..Default::default()})
        .unwrap_or_else(|e|panic!("{}",e.emit_to_string(&composer)));
    let shader=device.create_shader_module(wgpu::ShaderModuleDescriptor{label:Some("actual_forest_cover_debug"),source:wgpu::ShaderSource::Naga(std::borrow::Cow::Owned(module))});
    let pipeline=device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor{label:None,layout:None,module:&shader,entry_point:Some("cs_probe"),compilation_options:Default::default(),cache:None});
    let output=device.create_buffer_init(&wgpu::util::BufferInitDescriptor{label:None,contents:bytemuck::cast_slice(&[0_f32;32]),usage:wgpu::BufferUsages::STORAGE|wgpu::BufferUsages::COPY_SRC});
    let staging=device.create_buffer(&wgpu::BufferDescriptor{label:None,size:128,usage:wgpu::BufferUsages::MAP_READ|wgpu::BufferUsages::COPY_DST,mapped_at_creation:false});
    let bind=device.create_bind_group(&wgpu::BindGroupDescriptor{label:None,layout:&pipeline.get_bind_group_layout(0),entries:&[wgpu::BindGroupEntry{binding:0,resource:output.as_entire_binding()}]});
    let mut encoder=device.create_command_encoder(&Default::default());
    {let mut pass=encoder.begin_compute_pass(&Default::default());pass.set_pipeline(&pipeline);pass.set_bind_group(0,&bind,&[]);pass.dispatch_workgroups(1,1,1);}
    encoder.copy_buffer_to_buffer(&output,0,&staging,0,128);queue.submit(Some(encoder.finish()));
    let slice=staging.slice(..);let (send,recv)=std::sync::mpsc::channel();
    slice.map_async(wgpu::MapMode::Read,move|r|{let _=send.send(r);});device.poll(wgpu::PollType::wait_indefinitely()).unwrap();recv.recv().unwrap().unwrap();
    let bytes=slice.get_mapped_range();let samples=bytemuck::cast_slice::<u8,f32>(&bytes);
    let expected=[[1.,1.,1.,1.],[0.,0.,0.,1.],[0.,0.,0.,1.],[0.;4],[0.;4],[0.;4],[0.25,0.25,0.25,1.],[0.,0.,0.,1.]];
    for (actual,want) in samples.chunks_exact(4).zip(expected) {for (a,b) in actual.iter().zip(want){assert!(a.is_finite() && (*a-b).abs()<1e-6);}}
    drop(bytes);staging.unmap();
}

#[test]
fn tree_crown_actual_gpu_height_depth_shelter_and_ablation() {
    let (device, queue) = crate::gfx3d::cull::tests::headless()
        .expect("tree crown acceptance requires a real GPU test device");
    for disabled in [false, true] {
        let mut composer = super::build_composer();
        let mut defs = std::collections::HashMap::new();
        if disabled { defs.insert("DISABLE_TREE_SNOW".into(), ShaderDefValue::Bool(true)); }
        let module = composer.make_naga_module(NagaModuleDescriptor {
            source: PROBE, file_path: "tree_snow_probe.wgsl", shader_defs: defs, ..Default::default()
        }).unwrap_or_else(|e| panic!("{}", e.emit_to_string(&composer)));
        let shader = device.create_shader_module(wgpu::ShaderModuleDescriptor {
            label: Some("actual_tree_snow_functions"),
            source: wgpu::ShaderSource::Naga(std::borrow::Cow::Owned(module)),
        });
        let pipeline = device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
            label: Some("tree_snow_production_numeric"), layout: None, module: &shader,
            entry_point: Some("cs_probe"), compilation_options: Default::default(), cache: None,
        });
        let output = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
            label: Some("tree_snow_results"), contents: bytemuck::cast_slice(&[0_f32;32]),
            usage: wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_SRC,
        });
        let staging = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("tree_snow_readback"), size: 128,
            usage: wgpu::BufferUsages::MAP_READ | wgpu::BufferUsages::COPY_DST, mapped_at_creation: false,
        });
        let bind = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: None, layout: &pipeline.get_bind_group_layout(0),
            entries: &[wgpu::BindGroupEntry{ binding:0, resource:output.as_entire_binding() }],
        });
        let mut encoder = device.create_command_encoder(&Default::default());
        {
            let mut pass = encoder.begin_compute_pass(&Default::default());
            pass.set_pipeline(&pipeline); pass.set_bind_group(0,&bind,&[]); pass.dispatch_workgroups(1,1,1);
        }
        encoder.copy_buffer_to_buffer(&output,0,&staging,0,128);
        queue.submit(Some(encoder.finish()));
        let slice = staging.slice(..);
        let (send,recv) = std::sync::mpsc::channel();
        slice.map_async(wgpu::MapMode::Read,move |r| { let _ = send.send(r); });
        device.poll(wgpu::PollType::wait_indefinitely()).unwrap(); recv.recv().unwrap().unwrap();
        let bytes = slice.get_mapped_range();
        let samples = bytemuck::cast_slice::<u8,f32>(&bytes);
        for (i,sample) in samples.chunks_exact(4).enumerate() {
            assert!(sample.iter().all(|v| v.is_finite()));
            assert_eq!(sample[2],0.0,"invalid loaded model height fails closed");
            if !disabled && i == 2 {
                // Exactly .45 in the model envelope rounds differently on the
                // GPU's multiply/smoothstep; allow only sub-micro coverage here.
                assert!(sample[1].abs() < 1e-6, "lower crown boundary must remain bare");
            } else if disabled || [1,4,5,6].contains(&i) {
                assert_eq!(sample[1],0.0,"dry/lower-crown/unowned/shelter/off must not coat");
            } else if i == 3 {
                assert!((sample[0]-0.5).abs()<1e-5 && (sample[1]-0.5).abs()<1e-5);
            } else {
                assert_eq!(sample[0],1.0); assert_eq!(sample[1],1.0);
            }
            if disabled { assert_eq!(sample[3],0.0); }
            else if sample[0] > 0.0 { assert!(sample[3]>0.0 && sample[3]<0.2); }
        }
        drop(bytes); staging.unmap();
    }
}

#[test]
fn actual_tree_shader_consumers_keep_alpha_wind_normals_and_source_authority() {
    let direct = include_str!("../gfx3d/shader3d.wgsl");
    let retained = include_str!("../gfx3d/gpu_driven.wgsl");
    let shared = include_str!("shading.wgsl");
    let crown = include_str!("tree_snow.wgsl");
    for source in [direct,retained] {
        assert!(source.contains("tree_snow_height(pos.y,"),"mask uses authored pre-wind height");
        assert!(source.contains("tree_snow_coverage(in.world_pos, in.tree_snow.x, in.tree_snow.y)"));
        assert!(source.contains("veg_sway_offset("));
        assert!(source.contains("a2c_coverage("));
        assert!(source.contains("discard;"));
    }
    assert!(direct.contains("instance, 0.0, vec2<f32>(0.0)"),"skinned output cannot inherit crown proof");
    assert!(retained.contains("let tree_proof = select(0.0, inst.conform2.w, canopy == INST_CANOPY_TREE)"),
        "individual model-height masks do not leak into merged forests or bushes");
    assert!(direct.contains("alpha_ref <= 0.0")); assert!(retained.contains("sm.alpha_ref <= 0.0"));
    assert!(shared.contains("object_snow_normal(world_pos, geo_normal, nrm, dwx, dwy, object_cover)"));
    assert!(shared.contains("(wrap_fill + trans) * fade * (1.0 - tree_snow_cover)"));
    assert!(shared.contains("m_spec_mask *= 1.0 - tree_snow_cover"));
    assert!(!crown.contains("snow_powder_normal") && !crown.contains("timeSeconds"));
    assert!(crown.contains("frame.snow_surface.y + frame.snow_surface.z, world_abs.y"));
}

const FOREST_PROBE: &str = r#"
#import tree_snow::{forest_snow_atlas_crown, forest_snow_map_cover, forest_snow_sample_uv}
@group(0) @binding(0) var<storage, read_write> result: array<vec4<f32>>;
@compute @workgroup_size(8)
fn cs_probe(@builtin(global_invocation_id) id: vec3<u32>) {
    let i = id.x;
    var uv = vec2<f32>(0.25, 0.1);
    var kind = 1u;
    switch i {
        case 1u: { uv.y = 0.6; }
        case 2u: { kind = 2u; }
        case 3u: { kind = 2u; uv.x = 0.75; }
        case 4u: { kind = 2u; uv = vec2<f32>(0.75,0.6); }
        case 5u: { kind = 2u; uv = vec2<f32>(0.75,0.9); }
        case 6u: { kind = 0u; }
        case 7u: { kind = 2u; uv.y = 0.8; }
        case 8u: { uv.y = 0.345; }
        case 9u: { kind = 2u; uv.y = 0.31; }
        case 10u: { kind = 2u; uv.x = 0.49; }
        case 11u: { kind = 2u; uv.x = 0.51; }
        case 12u: { uv.x = -0.003; }
        case 13u: { uv.y = -0.001; }
        case 14u: { uv.y = bitcast<f32>(0x7fc00000u); }
        case 15u: { kind = 255u; }
        case 16u: { kind = 3u; uv = vec2<f32>(0.375,0.1); }
        case 17u: { kind = 3u; uv = vec2<f32>(0.9,0.9); }
        case 18u: { kind = 3u; uv = vec2<f32>(0.375,0.375); }
        case 19u: { kind = 3u; uv = vec2<f32>(0.65,0.94); }
        case 20u: { kind = 4u; uv = vec2<f32>(0.625,0.25); }
        case 21u: { kind = 4u; uv = vec2<f32>(0.3,0.375); }
        case 22u: { kind = 4u; uv = vec2<f32>(0.375,0.8); }
        case 23u: { kind = 4u; uv = vec2<f32>(0.625,0.625); }
        case 24u: { kind = 5u; uv = vec2<f32>(0.5,0.7); }
        case 25u: { kind = 6u; uv = vec2<f32>(0.5,0.95); }
        case 26u: { kind = 7u; uv = vec2<f32>(0.5,0.8); }
        case 27u: { kind = 8u; uv = vec2<f32>(0.25,0.6); }
        case 28u: { kind = 9u; uv = vec2<f32>(0.1,0.23); }
        case 29u: { kind = 9u; uv = vec2<f32>(0.5,0.6); }
        case 30u: { uv = forest_snow_sample_uv(vec2<f32>(-0.75,0.1),2u); }
        case 31u: { uv = forest_snow_sample_uv(uv,8u); }
        case 40u: { uv = forest_snow_sample_uv(vec2<f32>(bitcast<f32>(0x7fc00000u),0.1),3u); }
        case 41u: { uv = forest_snow_sample_uv(vec2<f32>(0.1,bitcast<f32>(0x7f800000u)),3u); }
        case 42u: { kind = 4u; uv = vec2<f32>(0.1,0.47); }
        case 43u: { kind = 4u; uv = vec2<f32>(0.9,0.49); }
        case 44u: { kind = 3u; uv = vec2<f32>(0.15,0.21); }
        case 45u: { kind = 6u; uv = vec2<f32>(0.5,0.2); }
        case 46u: { kind = 9u; uv = vec2<f32>(0.5,0.32); }
        case 47u: { kind = 9u; uv = vec2<f32>(0.5,0.995); }
        case 48u: { kind = 10u; uv = vec2<f32>(0.5,0.1); }
        case 49u: { kind = 10u; uv = vec2<f32>(0.5,0.42); }
        case 50u: { kind = 10u; uv = vec2<f32>(0.5,0.7); }
        case 51u: { kind = 11u; uv = vec2<f32>(0.5,0.1); }
        case 52u: { kind = 11u; uv = vec2<f32>(0.5,0.65); }
        case 53u: { kind = 12u; uv = vec2<f32>(0.25,0.1); }
        case 54u: { kind = 12u; uv = vec2<f32>(0.25,0.8); }
        case 55u: { kind = 13u; uv = vec2<f32>(0.5,0.65); }
        case 56u: { kind = 13u; uv = vec2<f32>(0.5,0.1); }
        case 57u: { kind = 13u; uv = vec2<f32>(0.5,0.99); }
        case 58u: { kind = 14u; uv = vec2<f32>(0.5,0.1); }
        case 59u: { kind = 14u; uv = vec2<f32>(0.67,0.8); }
        case 60u: { kind = 15u; uv = vec2<f32>(0.2,0.3); }
        case 61u: { kind = 15u; uv = vec2<f32>(0.75,0.3); }
        case 62u: { kind = 15u; uv = vec2<f32>(0.75,0.98); }
        case 63u: { kind = 254u; }
        default: {}
    }
    if (i >= 32u && i < 40u) {
        kind = 2u;
        uv = forest_snow_sample_uv(vec2<f32>(1.25,1.1),i - 32u);
    }
    let crown = forest_snow_atlas_crown(uv, kind);
    result[i] = vec4<f32>(crown, forest_snow_map_cover(crown,0.18,0.0,1.0),
        forest_snow_map_cover(crown,0.18,1.0,0.0), forest_snow_map_cover(crown,0.18,1.0,0.8));
}
"#;

#[test]
fn forest_actual_gpu_audited_atlas_crowns_trunks_ground_shelter_and_ablation() {
    // Mandatory real device; unavailable GPU is not a silently passing acceptance.
    let (device, queue) = crate::gfx3d::cull::tests::headless().expect("forest atlas acceptance requires a GPU");
    for disabled in [false,true] {
        let mut composer = super::build_composer();
        let mut defs = std::collections::HashMap::new();
        if disabled { defs.insert("DISABLE_TREE_SNOW".into(),ShaderDefValue::Bool(true)); }
        let module = composer.make_naga_module(NagaModuleDescriptor {
            source: FOREST_PROBE, file_path:"forest_atlas_probe.wgsl", shader_defs:defs, ..Default::default()
        }).unwrap_or_else(|e| panic!("{}",e.emit_to_string(&composer)));
        let shader = device.create_shader_module(wgpu::ShaderModuleDescriptor {
            label:Some("actual_forest_atlas_functions"),
            source:wgpu::ShaderSource::Naga(std::borrow::Cow::Owned(module)),
        });
        let pipeline = device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
            label:Some("forest_atlas_numeric"),layout:None,module:&shader,entry_point:Some("cs_probe"),
            compilation_options:Default::default(),cache:None,
        });
        let output = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
            label:None,contents:bytemuck::cast_slice(&[0_f32;256]),
            usage:wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_SRC,
        });
        let staging = device.create_buffer(&wgpu::BufferDescriptor {
            label:None,size:1024,usage:wgpu::BufferUsages::MAP_READ | wgpu::BufferUsages::COPY_DST,mapped_at_creation:false,
        });
        let bind = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label:None,layout:&pipeline.get_bind_group_layout(0),
            entries:&[wgpu::BindGroupEntry{binding:0,resource:output.as_entire_binding()}],
        });
        let mut encoder = device.create_command_encoder(&Default::default());
        {
            let mut pass = encoder.begin_compute_pass(&Default::default());
            pass.set_pipeline(&pipeline);pass.set_bind_group(0,&bind,&[]);pass.dispatch_workgroups(8,1,1);
        }
        encoder.copy_buffer_to_buffer(&output,0,&staging,0,1024);queue.submit(Some(encoder.finish()));
        let slice = staging.slice(..);let (send,recv) = std::sync::mpsc::channel();
        slice.map_async(wgpu::MapMode::Read,move |r| {let _ = send.send(r);});
        device.poll(wgpu::PollType::wait_indefinitely()).unwrap();recv.recv().unwrap().unwrap();
        let bytes = slice.get_mapped_range();let samples = bytemuck::cast_slice::<u8,f32>(&bytes);
        for (i,sample) in samples.chunks_exact(4).enumerate() {
            let expected = match i {0|2|16|20|23|24|29|30|32|36|45|46|48|51|53|55|58|61=>1.0,
                8|9|10=>0.5,_=>0.0};
            assert!(sample.iter().all(|v| v.is_finite() && *v >= 0.0 && *v <= 1.0));
            assert!((sample[0]-expected).abs()<1e-5,"atlas sample {i}");
            assert_eq!(sample[1],0.0,"unpublished/outside/overflow physical map cannot certify openness");
            assert_eq!(sample[2],0.0,"actual overhead roof stays bare");
            // Coverage uses the production powder's exposure transition, rather
            // than multiplying raw rain reach. Evaluate that transition here
            // independently so neither roofs nor partial shelter are waived.
            let t = (0.8_f32 - 0.6) / (0.95 - 0.6);
            let exposure = t * t * (3.0 - 2.0 * t);
            let cover = if disabled {0.0} else {expected * exposure};
            assert!((sample[3]-cover).abs()<1e-5,
                "forest sample {i}, disabled={disabled}: actual={} expected={cover}",sample[3]);
        }
        drop(bytes);staging.unmap();
    }
}

#[test]
fn forest_transport_is_private_exact_stock_and_does_not_change_alpha_or_geometry() {
    let retained = include_str!("../gfx3d/gpu_driven.wgsl");
    let crown = include_str!("tree_snow.wgsl");
    let cpp = include_str!("../../../EngineWgpu.cpp");
    let policy = include_str!("../../../ForestSnowSurface.hpp");
    let forest = &crown[crown.find("fn forest_snow_atlas_crown").unwrap()..];
    assert!(retained.contains("canopy == INST_CANOPY_FOREST && inst.conform2.w == 2.0 && (mode == 0.0 || mode == 1.0)"));
    assert!(retained.contains("forest_snow_sample_uv(in.uv, sm.sampler_idx)"));
    assert!(retained.contains("(sm.flags >> 16u) & 255u"));
    assert!(policy.contains("AtlasShift = 16") && policy.contains("std::array<ModelPolicy, 25>"));
    assert!(policy.contains("std::array<TexturePolicy, 57>"));
    assert!(policy.contains("std::array<uint64_t,4> textureMasks"));
    assert!(policy.contains("uint64_t(1) << i"),"texture membership above bit31 stays intact");
    assert!(cpp.contains("forestAtlasEnabled && s->NProxies() == 0"));
    assert!(cpp.contains("typeid(obj) == typeid(ForestPlain) || typeid(obj) == typeid(Forest)"));
    assert!(cpp.contains("mat.texture_id == face->GpuHandle()"));
    assert!(forest.contains("weather_map_coverage(world_abs)") && forest.contains("weather_map_reach(world_abs)"));
    assert!(!forest.contains("interior_rain_reach") && !forest.contains("cached_exposure"));
    assert!(!forest.contains("timeSeconds") && !forest.contains("snow_powder_normal"));
    assert!(retained.contains("sm.alpha_ref <= 0.0") && retained.contains("a2c_coverage(") && retained.contains("discard;"));
    assert!(include_str!("../gfx3d/mod.rs").contains("flags: s.flags"),"register keeps private bits");
}

const TREE_FROST_PROBE: &str = r#"
#import tree_snow::tree_snow_albedo
@group(0) @binding(0) var<storage, read_write> result: array<vec4<f32>>;
@compute @workgroup_size(8)
fn cs_probe(@builtin(global_invocation_id) id: vec3<u32>) {
    var levels = array<f32,8>(0.0,0.001,0.005,0.02,0.04,0.08,0.16,0.64);
    let i = id.x;
    var authored = vec3<f32>(levels[i % 8u]);
    if (i >= 16u) { authored = vec3<f32>(0.0,levels[i % 8u] / 0.7152,0.0); }
    let footprint = select(0.01,10.0,(i % 16u) >= 8u);
    let frost = tree_snow_albedo(authored,vec3<f32>(5018.51,117.58,4087.66),footprint);
    result[i] = vec4<f32>(frost,dot(frost,vec3<f32>(0.2126,0.7152,0.0722)));
}
"#;

#[test]
fn tree_frost_actual_gpu_retains_authored_leaf_contrast_and_dark_recesses() {
    let (device,queue) = crate::gfx3d::cull::tests::headless().expect("textured frost acceptance requires a GPU");
    let mut composer = super::build_composer();
    let module = composer.make_naga_module(NagaModuleDescriptor {
        source:TREE_FROST_PROBE,file_path:"tree_frost_probe.wgsl",..Default::default()
    }).unwrap_or_else(|e| panic!("{}",e.emit_to_string(&composer)));
    let shader = device.create_shader_module(wgpu::ShaderModuleDescriptor {
        label:Some("actual_textured_tree_frost"),source:wgpu::ShaderSource::Naga(std::borrow::Cow::Owned(module)),
    });
    let pipeline = device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
        label:None,layout:None,module:&shader,entry_point:Some("cs_probe"),compilation_options:Default::default(),cache:None,
    });
    let output = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
        label:None,contents:bytemuck::cast_slice(&[0_f32;128]),usage:wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_SRC,
    });
    let staging = device.create_buffer(&wgpu::BufferDescriptor {
        label:None,size:512,usage:wgpu::BufferUsages::MAP_READ | wgpu::BufferUsages::COPY_DST,mapped_at_creation:false,
    });
    let bind = device.create_bind_group(&wgpu::BindGroupDescriptor {
        label:None,layout:&pipeline.get_bind_group_layout(0),
        entries:&[wgpu::BindGroupEntry{binding:0,resource:output.as_entire_binding()}],
    });
    let mut encoder = device.create_command_encoder(&Default::default());
    {
        let mut pass = encoder.begin_compute_pass(&Default::default());
        pass.set_pipeline(&pipeline);pass.set_bind_group(0,&bind,&[]);pass.dispatch_workgroups(4,1,1);
    }
    encoder.copy_buffer_to_buffer(&output,0,&staging,0,512);queue.submit(Some(encoder.finish()));
    let slice = staging.slice(..);let (send,recv) = std::sync::mpsc::channel();
    slice.map_async(wgpu::MapMode::Read,move |r| {let _=send.send(r);});
    device.poll(wgpu::PollType::wait_indefinitely()).unwrap();recv.recv().unwrap().unwrap();
    let bytes = slice.get_mapped_range();let samples = bytemuck::cast_slice::<u8,f32>(&bytes);
    let levels = [0.0,0.001,0.005,0.02,0.04,0.08,0.16,0.64];
    for (i,sample) in samples.chunks_exact(4).enumerate() {
        assert!(sample.iter().all(|v| v.is_finite() && *v >= 0.0 && *v < 0.85));
        if i % 8 < 2 {
            for v in &sample[..3] {assert!((*v-levels[i % 8]).abs()<1e-6,"deep photographed recesses remain authored");}
        }
        if i % 8 > 0 {assert!(sample[3] > samples[(i-1)*4+3],"authored contrast cannot collapse into uniform white");}
        if i % 16 >= 8 {
            let source = levels[i % 8];
            let t = ((source-0.001_f32)/(0.010-0.001)).clamp(0.0,1.0);
            let recess = t*t*(3.0-2.0*t);
            let relief = 0.30 + 0.70*(source/(source+0.05)).sqrt();
            for (v,mean) in sample[..3].iter().zip([0.78,0.80,0.82]) {
                let expected = source*(1.0-recess)+mean*relief*recess;
                assert!((*v-expected).abs()<1e-6,"unresolved broad mottling has exact source-driven mean");
            }
        }
        if i >= 16 {
            for channel in 0..4 {
                assert!((sample[channel] - samples[(i-16)*4+channel]).abs()<0.000002,
                    "equal-luminance covered green foliage must match neutral frost, never retain green recess hue");
            }
        }
    }
    assert!(samples[7*4+3] - samples[2*4+3] > 0.5,"photographic leaf/branch relief must survive full snow cover");
    assert!(samples[4*4+3] > levels[4],"resolved foliage still gains a visible neutral frost coat");
    assert!(samples[11*4+3] > 0.50,"fully coated .02 linear leaf must read as frost rather than remain near-black");
    drop(bytes);staging.unmap();
}
