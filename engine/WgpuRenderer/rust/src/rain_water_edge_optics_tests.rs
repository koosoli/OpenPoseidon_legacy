//! Replay of actual CPU source/geometry receipts through real GPU optics helpers.
//! This does not substitute CPU normals for filtered game fragments or prove SSR hits.
use wgpu::util::DeviceExt;
#[repr(C)]
#[derive(Clone,Copy,bytemuck::Pod,bytemuck::Zeroable)]
struct Edge { surface:[f32;4],eye:[f32;4],gradient_depth_wet:[f32;4] }
fn parse_rows(text:&str)->Result<Vec<Edge>,String> {
    let mut rows=Vec::new();
    for line in text.lines().filter(|l|!l.starts_with('#')&&!l.trim().is_empty()) {
        let v=line.split(',').map(|v|v.parse::<f32>().map_err(|_|"Invalid number".to_owned())).collect::<Result<Vec<_>,_>>()?;
        if v.len()!=11 || v.iter().any(|v|!v.is_finite()) || v[7]!=0.&&v[7]!=1. ||
            !(0. ..=1.).contains(&v[6]) || !(0. ..=64.).contains(&v[5]) ||
            v.iter().any(|v|v.abs()>1000000.) {return Err("Invalid bounded actual receipt row".into());}
        rows.push(Edge{surface:[v[0],v[1],v[2],v[7]],eye:[v[8],v[9],v[10],0.],gradient_depth_wet:[v[3],v[4],v[5],v[6]]});
        if rows.len()>289 {return Err("Too many actual edge rows".into());}
    }
    if rows.len()!=289 {return Err("Actual 17x17 edge receipt required".into());}Ok(rows)
}
fn probe_source()->&'static str {r#"
#import ground_wet::{soil_wet_amount, soil_wet_fresnel}
#import rain_water_optics::rain_water_standing_fraction
struct Edge {surface:vec4<f32>,eye:vec4<f32>,gradient_depth_wet:vec4<f32>};
@group(0) @binding(0) var<storage,read> edges:array<Edge>;
@group(0) @binding(1) var<storage,read_write> result:array<vec4<f32>>;
@compute @workgroup_size(64)
fn cs_probe(@builtin(global_invocation_id) id:vec3<u32>) {
 let i=id.x;if(i>=arrayLength(&edges)){return;}let e=edges[i];
 let view=normalize(e.eye.xyz-e.surface.xyz);
 let moist_n=normalize(vec3<f32>(-e.gradient_depth_wet.x,1.,-e.gradient_depth_wet.y));
 let ray=reflect(-view,moist_n);
 let wet=soil_wet_amount(e.surface.w,e.gradient_depth_wet.w,0.);
 let standing=rain_water_standing_fraction(e.gradient_depth_wet.z);
 result[i*2u]=vec4<f32>(ray,wet);
 result[i*2u+1u]=vec4<f32>(standing,select(0.,1.,standing>0.),soil_wet_fresnel(dot(moist_n,view)),dot(moist_n,view));
}
"#}
#[test]
fn actual_edge_probe_contract_and_csv_refusals() {
    let terrain=include_str!("terrain/terrain.wgsl");let water=include_str!("rain_water.wgsl").replace("\r\n","\n");
    // The actual coat now filters a cone around the same disturbed-soil normal;
    // its directions, rather than one sharp mirror ray, query the sky.
    assert!(terrain.contains("let moist_n = geometric_n;"));
    assert!(terrain.contains("soil_wet_environment_samples(view, moist_n,"));
    assert!(terrain.contains("ground_sky_reflection(samples[i].xyz)"));
    assert!(water.contains("if (standing > 0.0) {\n        scene=rain_water_scene_reflection"));
    let mut composer=crate::shaders::build_composer();
    composer.make_naga_module(naga_oil::compose::NagaModuleDescriptor{source:probe_source(),file_path:"actual_edge_optics.wgsl",..Default::default()})
        .unwrap_or_else(|e|panic!("{}",e.emit_to_string(&composer)));
    let row="6425.81,141.70752,7175.564,-0.04077,0.11008,0.002836,0.804,1,6423.81,142.30753,7175.564\n";
    let valid=row.repeat(289);assert_eq!(parse_rows(&valid).unwrap().len(),289);
    assert!(parse_rows(&row.repeat(288)).is_err());assert!(parse_rows(&row.repeat(290)).is_err());
    for invalid in [valid.replace("0.002836","NaN"),valid.replace(",1,6423",",2,6423"),valid.replace("0.804,","1.804,")] {assert!(parse_rows(&invalid).is_err());}
}

#[test]
#[ignore = "Requires exported actual paused 17x17 edge receipt; root runs GPU explicitly"]
fn actual_gpu_recorded_edge_direction_and_retained_interface_eligibility() {
    let path=std::env::var("WGR_WET_SOIL_EDGE_RECEIPT").expect("Actual CSV receipt required");
    let rows=parse_rows(&std::fs::read_to_string(&path).unwrap()).unwrap();
    let(device,queue)=crate::gfx3d::cull::tests::headless().expect("Edge optics requires actual device");
    let mut composer=crate::shaders::build_composer();
    let shader=crate::shaders::make_module(&device,&mut composer,"actual_edge_optics",probe_source(),"actual_edge_optics.wgsl");
    let pipeline=device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor{label:None,layout:None,module:&shader,entry_point:Some("cs_probe"),compilation_options:Default::default(),cache:None});
    let input=device.create_buffer_init(&wgpu::util::BufferInitDescriptor{label:None,contents:bytemuck::cast_slice(&rows),usage:wgpu::BufferUsages::STORAGE});
    let bytes=(rows.len()*2*16) as u64;
    let output=device.create_buffer(&wgpu::BufferDescriptor{label:None,size:bytes,usage:wgpu::BufferUsages::STORAGE|wgpu::BufferUsages::COPY_SRC,mapped_at_creation:false});
    let staging=device.create_buffer(&wgpu::BufferDescriptor{label:None,size:bytes,usage:wgpu::BufferUsages::MAP_READ|wgpu::BufferUsages::COPY_DST,mapped_at_creation:false});
    let bind=device.create_bind_group(&wgpu::BindGroupDescriptor{label:None,layout:&pipeline.get_bind_group_layout(0),entries:&[
        wgpu::BindGroupEntry{binding:0,resource:input.as_entire_binding()},wgpu::BindGroupEntry{binding:1,resource:output.as_entire_binding()}]});
    let mut encoder=device.create_command_encoder(&Default::default());
    {let mut pass=encoder.begin_compute_pass(&Default::default());pass.set_pipeline(&pipeline);pass.set_bind_group(0,&bind,&[]);pass.dispatch_workgroups(5,1,1);}
    encoder.copy_buffer_to_buffer(&output,0,&staging,0,bytes);queue.submit([encoder.finish()]);
    let(tx,rx)=std::sync::mpsc::channel();staging.slice(..).map_async(wgpu::MapMode::Read,move|r|tx.send(r).unwrap());
    device.poll(wgpu::PollType::wait_indefinitely()).unwrap();rx.recv().unwrap().unwrap();
    let data=staging.slice(..).get_mapped_range();let values:&[[f32;4]]=bytemuck::cast_slice(&data);
    let mut downward=0;let mut standing=0;let mut wet=0;let mut logged=0;
    for (i,e) in rows.iter().enumerate() {
        let view=(glam::Vec3::from_array(e.eye[..3].try_into().unwrap())-glam::Vec3::from_array(e.surface[..3].try_into().unwrap())).normalize();
        let n=glam::Vec3::new(-e.gradient_depth_wet[0],1.,-e.gradient_depth_wet[1]).normalize();let incident=-view;
        let expected=incident-2.*incident.dot(n)*n;let ray=glam::Vec3::from_array(values[i*2][..3].try_into().unwrap());
        assert!((ray-expected).length()<0.0001,"Actual recorded direction row {i}");
        assert!(values[i*2].iter().chain(values[i*2+1].iter()).all(|v|v.is_finite()));
        if e.surface[3]==0. {assert_eq!(values[i*2][3],0.);}
        if e.gradient_depth_wet[2]<=0.008 {assert_eq!(values[i*2+1][0],0.);assert_eq!(values[i*2+1][1],0.);}
        if ray.y<0. {downward+=1;}if values[i*2+1][0]>0. {standing+=1;}if values[i*2][3]>0. {wet+=1;}
        if i==144 || logged<16 && ray.y<0. && values[i*2][3]>0. {logged+=1;eprintln!("WET_SOIL_EDGE_PROBE row={i} position={:?} actualCpuSlopes={:?} nativeDepth={} reflected={:?} wet={} standing={} ssrInvocationEligible={} scope=actual-owner-slopes-gpu-helper-replay-not-filtered-fragment-or-SSR-hit",e.surface,&e.gradient_depth_wet[..2],e.gradient_depth_wet[2],ray,values[i*2][3],values[i*2+1][0],values[i*2+1][1]);}
    }
    eprintln!("WET_SOIL_EDGE_PROBE_SUMMARY rows={} downward={downward} wet={wet} standingEligible={standing} receipt={path}",rows.len());
    drop(data);staging.unmap();
}
