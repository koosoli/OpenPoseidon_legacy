use super::*;
use wgpu::util::DeviceExt;

const PROFILE_PROBE:&str=r#"
#import layered_fog_optics::{fog_segment_density, fog_phase, fog_transmittance, fog_transport, FogConsumer, fog_camera_matches, fog_ray_direction}
@group(0) @binding(0) var<storage, read_write> result:array<vec4<f32>>;
@compute @workgroup_size(1)
fn cs_probe() {
 let slab=vec4<f32>(10.0,20.0,2.0,0.01);
 result[0]=vec4<f32>(fog_segment_density(15.0,0.0,100.0,slab),
  fog_segment_density(0.0,1.0,30.0,slab),fog_segment_density(30.0,-1.0,30.0,slab),
  fog_segment_density(0.0,1.0,15.0,slab)+fog_segment_density(15.0,1.0,15.0,slab));
 let thin=vec4<f32>(50.0,50.05,0.01,0.01);
 result[1]=vec4<f32>(fog_segment_density(0.0,1.0,100.0,thin),
  fog_segment_density(100.0,-1.0,100.0,thin),fog_segment_density(60.0,0.0,100.0,thin),fog_transmittance(2.0));
 var normalized=vec3<f32>(0.0);
 for (var i=0u;i<8192u;i=i+1u) {
  let cosine=-1.0+2.0*(f32(i)+0.5)/8192.0;
  normalized+=vec3<f32>(fog_phase(cosine,0.0),fog_phase(cosine,0.6),fog_phase(cosine,-0.6))*(12.56637061436/8192.0);
 }
 result[2]=vec4<f32>(normalized,0.0);
 result[3]=vec4<f32>(fog_transport(vec3<f32>(12.0,6.0,3.0),vec4<f32>(1.0,2.0,3.0,0.25)),1.0);
 result[4]=vec4<f32>(fog_transport(vec3<f32>(12.0,6.0,3.0),vec4<f32>(0.0,0.0,0.0,1.0)),1.0);
 let identity=mat4x4<f32>(vec4<f32>(1.0,0.0,0.0,0.0),vec4<f32>(0.0,1.0,0.0,0.0),vec4<f32>(0.0,0.0,1.0,0.0),vec4<f32>(0.0,0.0,0.0,1.0));
 var packet:FogConsumer;packet.view=identity;packet.proj=identity;packet.inv_vp=identity;
 packet.camera=vec4<f32>(1.0,2.0,3.0,0.0);packet.control=vec4<f32>(1.0,100.0,0.5,0.0);
 var changed=identity;changed[0].x=0.9;
 result[5]=vec4<f32>(f32(fog_camera_matches(packet,identity,identity,packet.camera.xyz)),
  f32(fog_camera_matches(packet,changed,identity,packet.camera.xyz)),
  f32(fog_camera_matches(packet,identity,identity,vec3<f32>(1.0,2.0,3.1))),0.0);
 packet.control.x=0.0;result[5].w=f32(fog_camera_matches(packet,identity,identity,packet.camera.xyz));
 result[6]=vec4<f32>(fog_ray_direction(vec4<f32>(0.0,0.0,10.0,0.0)),length(fog_ray_direction(vec4<f32>(0.0))));
 result[7]=vec4<f32>(fog_ray_direction(vec4<f32>(0.0,0.0,10.0,1.0)).z,
  fog_ray_direction(vec4<f32>(0.0,0.0,10.0,-1.0)).z,
  fog_ray_direction(vec4<f32>(0.0,0.0,10.0,1e-30)).z,
  length(fog_ray_direction(vec4<f32>(bitcast<f32>(0x7fc00000u),0.0,10.0,1.0))));
}
"#;

fn read_probe(device:&wgpu::Device,queue:&wgpu::Queue,shader:&str,entries:&[wgpu::BindGroupEntry<'_>],count:usize)->Vec<f32> {
 let mut composer=crate::shaders::build_composer();
 let module=crate::shaders::make_module(device,&mut composer,"fog_probe",shader,"fog_probe.wgsl");
 let pipe=device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {label:Some("fog_probe"),layout:None,module:&module,
  entry_point:Some("cs_probe"),compilation_options:Default::default(),cache:None});
 let out=device.create_buffer(&wgpu::BufferDescriptor {label:Some("fog_probe_output"),size:(count*4) as u64,
  usage:wgpu::BufferUsages::STORAGE|wgpu::BufferUsages::COPY_SRC,mapped_at_creation:false});
 let staging=device.create_buffer(&wgpu::BufferDescriptor {label:Some("fog_probe_readback"),size:(count*4) as u64,
  usage:wgpu::BufferUsages::MAP_READ|wgpu::BufferUsages::COPY_DST,mapped_at_creation:false});
 let mut bindings=entries.to_vec();bindings.insert(0,wgpu::BindGroupEntry {binding:0,resource:out.as_entire_binding()});
 let bind=device.create_bind_group(&wgpu::BindGroupDescriptor {label:None,layout:&pipe.get_bind_group_layout(0),entries:&bindings});
 let mut encoder=device.create_command_encoder(&Default::default());
 {let mut pass=encoder.begin_compute_pass(&Default::default());pass.set_pipeline(&pipe);pass.set_bind_group(0,&bind,&[]);pass.dispatch_workgroups(1,1,1);}
 encoder.copy_buffer_to_buffer(&out,0,&staging,0,(count*4) as u64);queue.submit(Some(encoder.finish()));
 let slice=staging.slice(..);let(tx,rx)=std::sync::mpsc::channel();slice.map_async(wgpu::MapMode::Read,move|r|{tx.send(r).unwrap();});
 device.poll(wgpu::PollType::wait_indefinitely()).unwrap();rx.recv().unwrap().unwrap();
 bytemuck::cast_slice::<u8,f32>(&slice.get_mapped_range()).to_vec()
}

#[test]
fn actual_fog_profile_gpu_integrates_thin_slabs_phase_energy_and_camera_refusals() {
 let(device,queue)=crate::gfx3d::cull::tests::headless().expect("actual fog GPU proof requires device");
 let samples=read_probe(&device,&queue,PROFILE_PROBE,&[],32);
 assert!(samples.iter().all(|v|v.is_finite()));
 for(i,wanted)in [100.,12.,12.,12.].iter().enumerate(){assert!((samples[i]-wanted).abs()<2e-4);}
 assert!((samples[4]-0.06).abs()<1e-4 && (samples[5]-0.06).abs()<1e-4 && samples[6]==0.);
 assert!((samples[7]-(-2f32).exp()).abs()<1e-6);
 for s in &samples[8..11]{assert!((*s-1.).abs()<5e-4,"normalized phase {s}");}
 assert_eq!(&samples[12..16],&[4.,3.5,3.75,1.]);assert_eq!(&samples[16..20],&[12.,6.,3.,1.]);
 assert_eq!(&samples[20..24],&[1.,0.,0.,0.]);
 assert_eq!(&samples[24..28],&[0.,0.,1.,0.]);assert_eq!(&samples[28..32],&[1.,-1.,1.,0.]);
}

#[test]
fn actual_fog_far_closure_gpu_preserves_sky_and_gated_foreground() {
 let(device,queue)=crate::gfx3d::cull::tests::headless().expect("actual fog closure proof requires device");
 const PROBE:&str=r#"
 #import layered_fog_optics::{fog_legacy_closure,fog_transport}
 @group(0) @binding(0) var<storage,read_write> result:array<vec4<f32>>;
 @compute @workgroup_size(1) fn cs_probe(){
  let scene=vec3<f32>(12.0,6.0,3.0);let air=vec3<f32>(4.0,8.0,16.0);
  let local=vec4<f32>(1.0,2.0,3.0,0.25);let distant=vec4<f32>(0.5,1.0,2.0,0.1);
  result[0]=vec4<f32>(fog_legacy_closure(scene,air,1.0,local,distant,true),1.0);
  result[1]=vec4<f32>(fog_legacy_closure(scene,air,1.0,local,distant,false),1.0);
  result[2]=vec4<f32>(fog_transport(air,distant),1.0);
  result[3]=vec4<f32>(fog_legacy_closure(scene,air,0.0,local,distant,true),1.0);
  result[4]=vec4<f32>(fog_legacy_closure(scene,air,0.0,local,distant,false),1.0);
  result[5]=vec4<f32>(fog_legacy_closure(scene,air,0.5,local,distant,true),1.0);
  result[6]=vec4<f32>(fog_legacy_closure(scene,air,0.5,local,distant,false),1.0);
  result[7]=vec4<f32>(fog_transport(mix(scene,air,1.0),local),1.0);
 }
 "#;
 let samples=read_probe(&device,&queue,PROBE,&[],32);
 assert!(samples.iter().all(|v|v.is_finite()));
 assert_eq!(&samples[0..4],&samples[8..12]);assert_eq!(&samples[4..8],&samples[8..12]);
 assert_eq!(&samples[12..16],&[4.,3.5,3.75,1.]);assert_eq!(&samples[16..20],&[12.,6.,3.,1.]);
 for i in 0..3 {
  assert!((samples[20+i]-(samples[12+i]+samples[8+i])*0.5).abs()<1e-6);
  assert!((samples[24+i]-(samples[16+i]+samples[8+i])*0.5).abs()<1e-6);
 }
 assert!((samples[28]-samples[8]).abs()>1.,"Old closure counterexample must remain discriminating");
 let source=include_str!("shaders/frame.wgsl");
 let guard=source.find("if (!fog_camera_matches(layer_fog_packet").unwrap();
 assert!(guard<source.find("let surface_open = weather_map_coverage").unwrap());
 assert!(source[guard..].contains("{ return background; }"),"OFF retains the exact legacy mix before new sampling");
}

#[test]
fn actual_fog_volume_gpu_obeys_beer_lambert_native_roof_overflow_and_weather_gates() {
 let(device,queue)=crate::gfx3d::cull::tests::headless().expect("actual fog GPU proof requires device");
 let mut composer=crate::shaders::build_composer();
 let mut fog=LayeredFog::new(&device,&queue,&mut composer,crate::HDR_FORMAT,1);
 fn texture(device:&wgpu::Device,format:wgpu::TextureFormat,width:u32,height:u32,layers:u32,depth:bool)->wgpu::Texture {
  device.create_texture(&wgpu::TextureDescriptor {label:Some("actual_fog_fixture"),size:wgpu::Extent3d {width,height,depth_or_array_layers:layers},
   mip_level_count:1,sample_count:1,dimension:wgpu::TextureDimension::D2,format,
   usage:wgpu::TextureUsages::TEXTURE_BINDING|if depth {wgpu::TextureUsages::RENDER_ATTACHMENT}else{wgpu::TextureUsages::COPY_DST},view_formats:&[]})
 }
 let native=texture(&device,wgpu::TextureFormat::R32Float,2,2,1,false);
 let native_view=native.create_view(&Default::default());
 let write_native=|y:f32|queue.write_texture(wgpu::TexelCopyTextureInfo {texture:&native,mip_level:0,origin:wgpu::Origin3d::ZERO,aspect:wgpu::TextureAspect::All},
  bytemuck::cast_slice(&[y;4]),wgpu::TexelCopyBufferLayout {offset:0,bytes_per_row:Some(8),rows_per_image:Some(2)},native.size());
 write_native(0.);
 let env=texture(&device,wgpu::TextureFormat::Rgba8Unorm,1,1,1,false);let env_view=env.create_view(&Default::default());
 let cloud=texture(&device,wgpu::TextureFormat::Rgba8Unorm,1,1,1,false);let cloud_view=cloud.create_view(&Default::default());
 let write_byte=|tex:&wgpu::Texture,byte:u8|queue.write_texture(wgpu::TexelCopyTextureInfo {texture:tex,mip_level:0,origin:wgpu::Origin3d::ZERO,aspect:wgpu::TextureAspect::All},
  &[byte,byte,byte,255],wgpu::TexelCopyBufferLayout {offset:0,bytes_per_row:Some(4),rows_per_image:Some(1)},tex.size());
 write_byte(&env,0);write_byte(&cloud,255);
 let shadow=texture(&device,wgpu::TextureFormat::Rgba8Unorm,1,1,1,false);let shadow_view=shadow.create_view(&Default::default());
 write_byte(&shadow,0);
 let cover_depth=texture(&device,wgpu::TextureFormat::Depth32Float,4,4,1,true);
 let cover_view=cover_depth.create_view(&Default::default());
 let csm=texture(&device,wgpu::TextureFormat::Depth32Float,4,4,4,true);
 let csm_view=csm.create_view(&wgpu::TextureViewDescriptor {dimension:Some(wgpu::TextureViewDimension::D2Array),..Default::default()});
 let clear_cover=|value:f32| {
  let mut enc=device.create_command_encoder(&Default::default());
  {let _pass=enc.begin_render_pass(&wgpu::RenderPassDescriptor {label:None,color_attachments:&[],
   depth_stencil_attachment:Some(wgpu::RenderPassDepthStencilAttachment {view:&cover_view,depth_ops:Some(wgpu::Operations {
    load:wgpu::LoadOp::Clear(value),store:wgpu::StoreOp::Store}),stencil_ops:None}),
   timestamp_writes:None,occlusion_query_set:None,multiview_mask:None});}
  queue.submit(Some(enc.finish()));
 };
 clear_cover(1.);
 let mut height=TerrainConformParams::zeroed();height.origin=glam::Vec2::splat(-500.);height.terrain_grid=1000.;
 height.hm_width=2;height.hm_height=2;height.enabled=1.;
 let mut mapping=TerrainShadowMap::zeroed();mapping.cloud_shadow=glam::Vec4::new(-500.,-500.,0.001,1.);
 let mut camera=WgrCamera::zeroed();camera.cam_pos=[0.,20.,0.,0.];camera.sun_diffuse=[8.,4.,2.,1.];camera.sun_dir_world=[0.,-1.,0.,0.];
 camera.params.fog_enabled=1.;camera.params.fog_inv_range=1./200.;
 camera.view=glam::Mat4::IDENTITY.to_cols_array();
 camera.proj=glam::Mat4::perspective_lh(60f32.to_radians(),1.,0.1,200.).to_cols_array();
 // Actual orthographic world-xz footprint and forward depth (top at +2000m).
 let cover_matrix=[[0.001,0.,0.,0.],[0.,0.,-0.00025,0.],[0.,-0.001,0.,0.],[0.,0.,0.5,1.]];
 let counts=device.create_buffer_init(&wgpu::util::BufferInitDescriptor {label:None,contents:bytemuck::cast_slice(&[0u32;4]),
  usage:wgpu::BufferUsages::STORAGE|wgpu::BufferUsages::COPY_DST});
 let cover=|ready:f32,capacity:u32|WeatherInputs {views:[cover_view.clone(),cover_view.clone()],matrices:[cover_matrix;2],
  controls:[[ready,0.,0.001,1.];2],counts:[Some(counts.clone()),Some(counts.clone())],capacities:[capacity;2]};
 let mut settings=WgrLayeredFog::default();settings.control=[1.,0.,0.,0.];settings.layers=[[0.,40.,1.,0.01],[70.,100.,1.,0.]];
 const READ_VOLUME:&str=r#"
 @group(0) @binding(0) var<storage,read_write> result:array<vec4<f32>>;
 @group(0) @binding(1) var volume:texture_3d<f32>;
 @compute @workgroup_size(1) fn cs_probe(){for(var z=0;z<32;z=z+1){result[z]=textureLoad(volume,vec3<i32>(16,16,z),0);}}
 "#;
 // Borrow the fixed volume handle independently so the closure does not borrow mutable fog.
 let volume_view=fog.view.clone();
 let read=||read_probe(&device,&queue,READ_VOLUME,&[wgpu::BindGroupEntry {binding:1,resource:wgpu::BindingResource::TextureView(&volume_view)}],128);
 let run=|fog:&mut LayeredFog,camera:&WgrCamera,settings:&WgrLayeredFog,weather:f32,cover:WeatherInputs,generation:u64,below:bool| {
  fog.invalidate(&queue);let mut enc=device.create_command_encoder(&Default::default());
  let inverse=(glam::DMat4::from_cols_array(&camera.view.map(f64::from)).inverse()
   *glam::DMat4::from_cols_array(&camera.proj.map(f64::from)).inverse()).as_mat4().to_cols_array_2d();
  fog.render(&device,&queue,&mut enc,settings,weather,true,0.,camera,inverse,&height,&native_view,generation,
   &mapping,&shadow_view,&csm_view,&env_view,&cloud_view,cover,below);queue.submit(Some(enc.finish()));
 };
 run(&mut fog,&camera,&settings,1.,cover(1.,100),1,true);assert!(fog.ready);
 let positive=read();let mut previous=1.;
 for(z,row)in positive.chunks_exact(4).enumerate(){
  let distance=200.*((z as f32+0.5)/32.).powi(2);
  assert!((row[3]-(-0.01*distance).exp()).abs()<0.0007,"actual Beer-Lambert z={z}: {row:?}");
  assert!(row[3]<=previous);previous=row[3];assert_eq!(&row[..3],&[0.;3]);
 }
 // Exact production projection family: reversed-Z infinite far point has w=0.
 camera.proj=glam::Mat4::perspective_infinite_reverse_lh(60f32.to_radians(),1.,0.1).to_cols_array();
 let infinite=glam::Mat4::from_cols_array(&camera.proj).inverse()*glam::Vec4::new(0.,0.,0.,1.);
 assert_eq!(infinite.w,0.);
 run(&mut fog,&camera,&settings,1.,cover(1.,100),1,true);let reverse=read();
 for (a,b) in positive.iter().zip(&reverse){assert!(b.is_finite() && (a-b).abs()<0.0007,"finite/infinite projection {a}/{b}");}
 camera.proj=glam::Mat4::perspective_infinite_lh(60f32.to_radians(),1.,0.1).to_cols_array();
 run(&mut fog,&camera,&settings,1.,cover(1.,100),1,true);let forward_infinite=read();
 for (a,b) in positive.iter().zip(&forward_infinite){assert!(b.is_finite() && (a-b).abs()<0.0007,"finite/forward-infinite projection {a}/{b}");}
 settings.control[1]=0.95;
 run(&mut fog,&camera,&settings,1.,cover(1.,100),1,true);let lit=read();assert!(lit[124]>0.);
 write_byte(&cloud,0);run(&mut fog,&camera,&settings,1.,cover(1.,100),1,true);
 let cloudy=read();assert_eq!(&cloudy[124..127],&[0.;3]);assert_eq!(lit[127],cloudy[127]);write_byte(&cloud,255);
 clear_cover(0.);run(&mut fog,&camera,&settings,1.,cover(1.,100),1,true);
 for row in read().chunks_exact(4){assert_eq!(row,&[0.,0.,0.,1.],"actual roof refuses medium");}clear_cover(1.);
 write_native(100.);run(&mut fog,&camera,&settings,1.,cover(1.,100),1,true);
 for row in read().chunks_exact(4){assert_eq!(row,&[0.,0.,0.,1.],"native underground refuses medium");}write_native(0.);
 queue.write_buffer(&counts,0,bytemuck::cast_slice(&[101u32,0,0,0]));run(&mut fog,&camera,&settings,1.,cover(1.,100),1,true);
 for row in read().chunks_exact(4){assert_eq!(row,&[0.,0.,0.,1.],"retained overflow refuses medium");}
 queue.write_buffer(&counts,0,bytemuck::cast_slice(&[0u32;4]));
 for(weather,generation,below,ready)in [(0.,1,true,1.),(1.,0,true,1.),(1.,1,false,1.),(1.,1,true,0.)]{
  run(&mut fog,&camera,&settings,weather,cover(ready,100),generation,below);assert!(!fog.ready);
 }
 settings.control[0]=0.;run(&mut fog,&camera,&settings,1.,cover(1.,100),1,true);assert!(!fog.ready);
}

#[test]
fn actual_fog_depth_gpu_unprojects_renderer_reversed_depth_with_forward_projection() {
 let(device,queue)=crate::gfx3d::cull::tests::headless().expect("actual fog depth proof requires GPU");
 const PROBE:&str=r#"
 #import layered_fog_optics::fog_depth_distance
 struct Input { inverse:mat4x4<f32>, uv_depth_far:vec4<f32>, }
 @group(0) @binding(0) var<storage,read_write> result:array<vec4<f32>>;
 @group(0) @binding(1) var<uniform> input:Input;
 @compute @workgroup_size(1) fn cs_probe(){let p=input.uv_depth_far;
  result[0]=vec4<f32>(fog_depth_distance(input.inverse,p.xy,p.z,p.w),
   fog_depth_distance(input.inverse,p.xy,0.0,p.w),fog_depth_distance(input.inverse,p.xy,1e-30,p.w),0.0);}
 "#;
 for projection in [glam::Mat4::perspective_lh(1.,1.,0.1,200.),glam::Mat4::perspective_infinite_lh(1.,1.,0.1)] {
  for point in [glam::Vec3::new(0.,0.,40.),glam::Vec3::new(10.,5.,80.)] {
   let clip=projection*point.extend(1.);let depth=1.-clip.z/clip.w;
   let uv=glam::Vec2::new(clip.x/clip.w*0.5+0.5,-clip.y/clip.w*0.5+0.5);
   let mut data=projection.inverse().to_cols_array().to_vec();data.extend_from_slice(&[uv.x,uv.y,depth,200.]);
   let input=device.create_buffer_init(&wgpu::util::BufferInitDescriptor {label:None,contents:bytemuck::cast_slice(&data),usage:wgpu::BufferUsages::UNIFORM});
   let result=read_probe(&device,&queue,PROBE,&[wgpu::BindGroupEntry {binding:1,resource:input.as_entire_binding()}],4);
   assert!(result.iter().all(|v|v.is_finite()));assert!((result[0]-point.length()).abs()<0.01,"actual forward projection / reversed raster depth {result:?} vs {point:?}");
   assert_eq!(result[1],200.,"sky clear depth consumes exact distant slice");
   // A finite projection's far plane is ill-conditioned after f32 inversion.
   // Its independently evaluated homogeneous far point may be slightly inside
   // radial fogFar; an infinite far point must still refuse the divide.
   let p=projection.inverse()*glam::Vec4::new(uv.x*2.-1.,1.-uv.y*2.,1.,1.);
   let far_reference=if p.w.abs()<=1e-20 {200.}else{(p.truncate()/p.w).length().min(200.)};
   assert!((result[2]-far_reference).abs()<0.01,"finite/infinite far reference {result:?}/{far_reference}");
  }
 }
}

#[test]
fn settings_are_opt_in_and_refuse_unready_weather_or_interiors() {
 let mut p=WgrLayeredFog::default();assert!(!requested(&p,0.5,true,0.));
 p.control[0]=1.;assert!(requested(&p,0.5,true,0.));
 for weather in [0.,-1.,1.01,f32::NAN] {assert!(!requested(&p,weather,true,0.));}
 assert!(!requested(&p,0.5,false,0.));assert!(!requested(&p,0.5,true,0.001));
 p.layers[0][1]=p.layers[0][0];assert!(!requested(&p,0.5,true,0.));
 p=WgrLayeredFog::default();p.control[0]=1.;p.layers[1][3]=f32::INFINITY;
 assert!(!requested(&p,0.5,true,0.));
 assert_eq!(std::mem::size_of::<Consumer>(),224);assert_eq!(std::mem::size_of::<Volume>(),352);
}

#[test]
fn production_fog_cloud_and_surface_shaders_compose() {
 for (source,path) in [(include_str!("layered_fog.wgsl"),"layered_fog.wgsl"),
 (include_str!("layered_fog_background.wgsl"),"layered_fog_background.wgsl"),
 (include_str!("sky/cloud_composite.wgsl"),"cloud_composite.wgsl"),
 (include_str!("terrain/terrain.wgsl"),"terrain.wgsl"),
 (include_str!("gfx3d/shader3d.wgsl"),"shader3d.wgsl"),
 (include_str!("gfx3d/gpu_driven.wgsl"),"gpu_driven.wgsl"),
 (include_str!("grass/grass.wgsl"),"grass.wgsl")] {
  let mut composer=crate::shaders::build_composer();
  let m=composer.make_naga_module(naga_oil::compose::NagaModuleDescriptor {source,file_path:path,
   shader_defs:crate::shaders::shader_defs(),..Default::default()})
   .unwrap_or_else(|e|panic!("{path}: {}",e.emit_to_string(&composer)));
  naga::valid::Validator::new(naga::valid::ValidationFlags::all(),naga::valid::Capabilities::all())
   .validate(&m).unwrap_or_else(|e|panic!("{path}: {e:?}"));
 }
}
