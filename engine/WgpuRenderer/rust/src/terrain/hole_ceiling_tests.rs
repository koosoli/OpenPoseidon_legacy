//! Actual renderer predicates, extracted without rewriting their production bodies.
use wgpu::util::DeviceExt;

fn function(src: &str, name: &str) -> String {
    let start = src.find(&format!("fn {name}(" )).unwrap();
    let mut depth = 0;
    let mut opened = false;
    for (i, c) in src[start..].char_indices() {
        if c == '{' { depth += 1; opened = true; }
        if c == '}' { depth -= 1; if opened && depth == 0 { return src[start..start+i+1].into(); } }
    }
    panic!("Unterminated actual helper {name}");
}

fn probe(src: &str, cave: bool) -> String {
    let (name, variable, context, expression) = if cave {
        ("cave_daylight2", "cave_openings",
         "struct HmParams { enabled:f32 }; const hm_params = HmParams(1.0); fn bare_surface_y(xz:vec2<f32>)->f32 { return 100.0; }",
         "let v=cave_daylight2(points[id.x].xyz); result[id.x]=vec4<f32>(v,0.0,0.0);")
    } else { ("in_terrain_hole", "holes", "", "result[id.x]=vec4<f32>(select(0.0,1.0,in_terrain_hole(points[id.x].xyz)),0.0,0.0,0.0);") };
    format!("struct Holes {{ info:vec4<f32>, edges:array<vec4<f32>,64> }};\n\
      @group(0) @binding(0) var<storage,read> {variable}:Holes;\n\
      @group(0) @binding(1) var<storage,read> points:array<vec4<f32>>;\n\
      @group(0) @binding(2) var<storage,read_write> result:array<vec4<f32>>;\n\
      {context}\n{}\n\
      @compute @workgroup_size(64) fn cs_probe(@builtin(global_invocation_id) id:vec3<u32>) {{\n\
      if(id.x>=arrayLength(&points)){{return;}} {expression} }}", function(src,name))
}

fn rect(x: f32, z: f32) -> [[f32;4];4] {
    [[1.,0.,-x,0.],[0.,1.,-z,0.],[-1.,0.,x+2.,0.],[0.,-1.,z+2.,1.]]
}

#[test]
fn ceiling_actual_helpers_and_complete_shaders_validate() {
    for (src,cave) in [(include_str!("terrain.wgsl"),false),
        (include_str!("../grass/grass.wgsl"),false), (include_str!("../shaders/conform.wgsl"),true)] {
        let module=naga::front::wgsl::parse_str(&probe(src,cave)).expect("actual helper parses");
        naga::valid::Validator::new(naga::valid::ValidationFlags::all(),naga::valid::Capabilities::all())
            .validate(&module).expect("actual helper validates");
    }
    for (src,path) in [(include_str!("terrain.wgsl"),"terrain/terrain.wgsl"),
        (include_str!("../grass/grass.wgsl"),"grass/grass.wgsl"),
        (include_str!("../gfx3d/shader3d.wgsl"),"gfx3d/shader3d.wgsl"),
        (include_str!("../gfx3d/gpu_driven.wgsl"),"gfx3d/gpu_driven.wgsl")] {
        let mut composer=crate::shaders::build_composer();
        let module=composer.make_naga_module(naga_oil::compose::NagaModuleDescriptor {
            source:src,file_path:path,..Default::default()
        }).unwrap_or_else(|e|panic!("{}",e.emit_to_string(&composer)));
        naga::valid::Validator::new(naga::valid::ValidationFlags::all(),naga::valid::Capabilities::all())
            .validate(&module).expect("actual complete shader validates");
    }
}

#[test]
fn ceiling_gl_and_camera_use_the_same_metadata_contract() {
    let gl=include_str!("../../../../PoseidonGL33/EngineGL33_Shaders.cpp");
    assert_eq!(gl.matches("if (e.w == 2.0) { bounded = true; ceiling = e.z; continue; }").count(),3);
    assert_eq!(gl.matches("if (inside && (!bounded || vWorldRel.y <= ceiling)) return true;").count(),3);
    assert_eq!(gl.matches("            bounded = false;").count(),3);
    assert!(gl.contains("e[3] == 2.0f ? e[2] - cy : e[2] + e[0] * cx + e[1] * cz"));
    let cpp=include_str!("../../../EngineWgpu.cpp");
    assert!(cpp.contains("if (e[3] == 2.0f)") && cpp.contains("if (valid && !bounded)"));
    assert_eq!(super::TERRAIN_HOLES_BYTES,1040); // no new binding, stride or capacity
    let terrain=include_str!("terrain.wgsl");
    assert_eq!(terrain.matches("in_terrain_hole(in.world_pos + frame.cam_pos.xyz)").count(),2);
    let grass=include_str!("../grass/grass.wgsl");
    assert_eq!(grass.matches("in_terrain_hole(vec3<f32>(world_xz.x, y, world_xz.y))").count(),3);
    assert_eq!(function(terrain,"in_terrain_hole"),function(grass,"in_terrain_hole"));
}

fn gpu_values(device:&wgpu::Device,queue:&wgpu::Queue,source:&str,edges:&[[f32;4]],points:&[[f32;4]])->Vec<[f32;4]> {
    assert!(edges.len()<=64);
    let module=naga::front::wgsl::parse_str(source).unwrap();
    let shader=device.create_shader_module(wgpu::ShaderModuleDescriptor {label:Some("actual cave ceiling helper"),source:wgpu::ShaderSource::Naga(std::borrow::Cow::Owned(module))});
    let pipeline=device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {label:None,layout:None,module:&shader,entry_point:Some("cs_probe"),compilation_options:Default::default(),cache:None});
    let mut data=vec![[0.;4];65]; data[0][0]=edges.len() as f32;data[1..1+edges.len()].copy_from_slice(edges);
    let input=device.create_buffer_init(&wgpu::util::BufferInitDescriptor {label:None,contents:bytemuck::cast_slice(&data),usage:wgpu::BufferUsages::STORAGE});
    let queries=device.create_buffer_init(&wgpu::util::BufferInitDescriptor {label:None,contents:bytemuck::cast_slice(points),usage:wgpu::BufferUsages::STORAGE});
    let bytes=(points.len()*16) as u64;
    let output=device.create_buffer(&wgpu::BufferDescriptor {label:None,size:bytes,usage:wgpu::BufferUsages::STORAGE|wgpu::BufferUsages::COPY_SRC,mapped_at_creation:false});
    let staging=device.create_buffer(&wgpu::BufferDescriptor {label:None,size:bytes,usage:wgpu::BufferUsages::MAP_READ|wgpu::BufferUsages::COPY_DST,mapped_at_creation:false});
    let bind=device.create_bind_group(&wgpu::BindGroupDescriptor {label:None,layout:&pipeline.get_bind_group_layout(0),entries:&[
        wgpu::BindGroupEntry{binding:0,resource:input.as_entire_binding()},wgpu::BindGroupEntry{binding:1,resource:queries.as_entire_binding()},wgpu::BindGroupEntry{binding:2,resource:output.as_entire_binding()}]});
    let mut encoder=device.create_command_encoder(&Default::default());
    {let mut pass=encoder.begin_compute_pass(&Default::default());pass.set_pipeline(&pipeline);pass.set_bind_group(0,&bind,&[]);pass.dispatch_workgroups((points.len() as u32).div_ceil(64),1,1);}
    encoder.copy_buffer_to_buffer(&output,0,&staging,0,bytes);queue.submit([encoder.finish()]);
    let(tx,rx)=std::sync::mpsc::channel();staging.slice(..).map_async(wgpu::MapMode::Read,move|r|tx.send(r).unwrap());
    device.poll(wgpu::PollType::wait_indefinitely()).unwrap();rx.recv().unwrap().unwrap();
    let mapped=staging.slice(..).get_mapped_range();let result=bytemuck::cast_slice::<u8,[f32;4]>(&mapped).to_vec();drop(mapped);staging.unmap();result
}

#[test]
fn actual_gpu_ceiling_preserves_mountain_legacy_overlap_and_cap() {
    let(device,queue)=crate::gfx3d::cull::tests::headless().expect("actual ceiling GPU required");
    let mut mixed=vec![[0.,0.,10.,2.]];mixed.extend(rect(0.,0.));mixed.extend(rect(20.,20.));
    // A second bounded footprint overlaps the first, with an independently higher ceiling.
    mixed.push([0.,0.,12.,2.]);mixed.extend(rect(1.,1.));
    let points=[[0.5,9.,0.5,0.],[0.5,10.,0.5,0.],[0.5,10.001,0.5,0.],
        [0.5,1000.,0.5,0.],[21.,1000.,21.,0.],[1.5,11.,1.5,0.],[1.5,12.001,1.5,0.],[-1.,0.,1.,0.]];
    let expected=[1.,1.,0.,0.,1.,1.,0.,0.];
    let mut cap=Vec::new();for _ in 0..15 {cap.extend(rect(100.,100.));}
    cap.push([0.,0.,10.,2.]);cap.extend([[1.,0.,0.,0.],[0.,1.,0.,0.],[-0.70710677,-0.70710677,1.4142135,1.]]);
    assert_eq!(cap.len(),64);
    for (src,label) in [(include_str!("terrain.wgsl"),"terrain"),(include_str!("../grass/grass.wgsl"),"grass")] {
        let source=probe(src,false);
        for (i,value) in gpu_values(&device,&queue,&source,&mixed,&points).iter().enumerate() {assert_eq!(value[0],expected[i],"{label} mixed query {i}");}
        for (i,value) in gpu_values(&device,&queue,&source,&cap,&points[..4]).iter().enumerate() {assert_eq!(value[0],expected[i],"{label} 64-record query {i}");}
        for edges in [vec![],vec![[0.,0.,10.,2.]],vec![[0.,0.,10.,2.],[0.,0.,0.,1.]],vec![[0.,0.,0.,1.]]] {
            assert!(gpu_values(&device,&queue,&source,&edges,&points).iter().all(|v|v[0]==0.),"{label} empty/metadata/collapsed refusal");
        }
    }
}

#[test]
fn actual_gpu_bounded_mouth_is_not_vertical_daylight_aperture() {
    let(device,queue)=crate::gfx3d::cull::tests::headless().expect("actual aperture GPU required");
    let source=probe(include_str!("../shaders/conform.wgsl"),true);
    let points=[[1.,99.,1.,0.],[1.,101.,1.,0.],[21.,99.,21.,0.]];
    let mut bounded=vec![[0.,0.,99.5,2.]];bounded.extend(rect(0.,0.));
    let values=gpu_values(&device,&queue,&source,&bounded,&points);
    assert_eq!(&values[0][..2],&[0.,0.],"bounded mouth supplies no vertical sun/sky");
    assert_eq!(&values[1][..2],&[1.,1.],"mountain exterior unchanged");
    bounded.extend(rect(20.,20.));
    let mixed=gpu_values(&device,&queue,&source,&bounded,&points);
    assert_eq!(&mixed[2][..2],&[1.,1.],"metadata must reset before legacy skylight");
    let legacy=gpu_values(&device,&queue,&source,&rect(0.,0.),&points);
    assert_eq!(&legacy[0][..2],&[1.,1.],"original stairwell skylight unchanged");
}
