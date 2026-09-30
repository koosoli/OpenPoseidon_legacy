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
            if disabled || [1,2,4,5,6].contains(&i) {
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
    assert!(retained.contains("canopy == INST_CANOPY_TREE"),"merged forests and bushes stay excluded");
    assert!(direct.contains("alpha_ref <= 0.0")); assert!(retained.contains("sm.alpha_ref <= 0.0"));
    assert!(shared.contains("object_snow_normal(world_pos, geo_normal, nrm, dwx, dwy, object_cover)"));
    assert!(shared.contains("(wrap_fill + trans) * fade * (1.0 - tree_snow_cover)"));
    assert!(shared.contains("m_spec_mask *= 1.0 - tree_snow_cover"));
    assert!(!crown.contains("snow_powder_normal") && !crown.contains("timeSeconds"));
    assert!(crown.contains("frame.snow_surface.y + frame.snow_surface.z, world_abs.y"));
}
