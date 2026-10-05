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
fn actual_fog_light_quadrature_gpu_stays_in_thin_air_and_conserves_sky_energy() {
 let(device,queue)=crate::gfx3d::cull::tests::headless().expect("fog quadrature proof requires device");
 const PROBE:&str=r#"
 #import layered_fog_optics::{fog_phase_sample,fog_segment_participant,fog_height_density,fog_phase}
 @group(0) @binding(0) var<storage,read_write> result:array<vec4<f32>>;
 @compute @workgroup_size(1) fn cs_probe(){
  let layer=vec4<f32>(48.0,49.0,0.01,1.0);
  let forward=fog_segment_participant(0.0,1.0,100.0,layer);
  let reverse=fog_segment_participant(100.0,-1.0,100.0,layer);
  result[0]=vec4<f32>(forward,reverse);
  result[1]=vec4<f32>(fog_height_density(forward.x,layer),fog_height_density(100.0-reverse.x,layer),
   fog_height_density(50.0,layer),fog_segment_participant(70.0,0.0,100.0,layer).y);
  for(var j=0u;j<7u;j=j+1u){
   let g=f32(j)*0.95/6.0;
   var maximum=0.0;var minimum=1.0;var unit_error=0.0;var old_maximum=0.0;
   for(var a=0u;a<36u;a=a+1u){for(var b=0u;b<19u;b=b+1u){
    let az=f32(a)*6.28318530718/36.0;let el=-1.57079632679+f32(b)*3.14159265359/18.0;
    let ray=vec3<f32>(cos(el)*cos(az),sin(el),cos(el)*sin(az));
    var energy=0.0;var old=0.0;
    for(var k=0u;k<8u;k=k+1u){let direction=fog_phase_sample(ray,g,k);
     unit_error=max(unit_error,abs(length(direction)-1.0));energy+=select(0.0,0.125,direction.y>0.0);}
    for(var k=0u;k<6u;k=k+1u){let y=(f32(k)+0.5)/6.0;let angle=f32(k)*2.39996322973;
     let r=sqrt(1.0-y*y);let direction=vec3<f32>(r*cos(angle),y,r*sin(angle));
     old+=fog_phase(dot(ray,direction),g)*(6.28318530718/6.0);}
    maximum=max(maximum,energy);minimum=min(minimum,energy);old_maximum=max(old_maximum,old);
   }}
   result[j+2u]=vec4<f32>(minimum,maximum,unit_error,old_maximum);
  }
 }
 "#;
 let samples=read_probe(&device,&queue,PROBE,&[],36);
 assert!(samples.iter().all(|v|v.is_finite()));
 assert!((samples[0]-48.5).abs()<1e-4 && (samples[2]-51.5).abs()<1e-4);
 assert!((samples[1]-1.01).abs()<1e-4 && (samples[3]-1.01).abs()<1e-4);
 assert_eq!(&samples[4..8],&[1.,1.,0.,0.],"Thin lighting sample must be in participating air, unlike original midpoint");
 for row in samples[8..].chunks_exact(4){assert!(row[0]>=0. && row[1]<=1. && row[2]<1e-5,"normalized unit sky: {row:?}");}
 assert!(samples[35]>2.,"Original high-g six-direction alias is a discriminating counterexample");
}

#[test]
fn actual_fog_neutral_chroma_gpu_matches_far_sky_and_preserves_luminance() {
 let(device,queue)=crate::gfx3d::cull::tests::headless().expect("fog chroma proof requires device");
 const PROBE:&str=r#"
 #import layered_fog_optics::{fog_neutral_radiance,fog_neutral_horizon,fog_legacy_closure,fog_transport}
 @group(0) @binding(0) var<storage,read_write> result:array<vec4<f32>>;
 @compute @workgroup_size(1) fn cs_probe(){
  let blue=vec3<f32>(1.0,2.0,8.0);let y=dot(blue,vec3<f32>(0.2126,0.7152,0.0722));
  result[0]=vec4<f32>(fog_neutral_radiance(blue,0.0),y);
  result[1]=vec4<f32>(fog_neutral_radiance(blue,0.85),y);
  for(var i=0u;i<5u;i=i+1u){let ry=f32(i)*0.15-0.15;
   let sky=fog_neutral_radiance(blue,fog_neutral_horizon(ry,0.85));
   let local=vec4<f32>(0.2,0.2,0.2,0.4);let far=vec4<f32>(0.4,0.4,0.4,0.25);
   let surface=fog_legacy_closure(vec3<f32>(0.1),sky,1.0,local,far,true);
   let background=fog_transport(sky,far);
   result[2u+i]=vec4<f32>(surface-background,fog_neutral_horizon(ry,0.85));
  }
 }
 "#;
 let samples=read_probe(&device,&queue,PROBE,&[],28);
 assert_eq!(&samples[0..3],&[1.,2.,8.],"Fog OFF must be exact identity");
 let neutral=&samples[4..7];let y=neutral[0]*0.2126+neutral[1]*0.7152+neutral[2]*0.0722;
 assert!((y-samples[7]).abs()<1e-5,"No luminance/exposure gain from fog chroma");
 assert!(((neutral[2]-neutral[0])-7.*0.15).abs()<1e-5,"Blue chroma reduced without altering material radiance");
 for row in samples[8..].chunks_exact(4){assert_eq!(&row[..3],&[0.;3],"Distant surface and fog horizon must match");}
 assert_eq!(samples[11],0.85);assert_eq!(samples[27],0.,"Zenith outside mist horizon remains unchanged");
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
 assert!(guard<source.find("let surface_open = fog_surface_open").unwrap());
 assert!(source[guard..].contains("{ return background; }"),"OFF retains the exact legacy mix before new sampling");
}

#[test]
fn actual_fog_receivers_gpu_keep_intervening_air_without_changing_alpha() {
 let(device,queue)=crate::gfx3d::cull::tests::headless().expect("actual vegetation fog proof requires device");
 const PROBE:&str=r#"
 #import layered_fog_optics::{fog_surface_open,fog_legacy_closure}
 @group(0) @binding(0) var<storage,read_write> result:array<vec4<f32>>;
 @compute @workgroup_size(1) fn cs_probe(){
  let scene=vec3<f32>(0.2,0.3,0.1);let sky=vec3<f32>(1.0,1.0,1.0);
  let local=vec4<f32>(0.4,0.4,0.4,0.5);let distant=vec4<f32>(0.7,0.7,0.7,0.2);
  // Real MapType vegetation contains cutout canopy AND opaque trunk sections.
  // A self-sheltered endpoint must not change either section's ray transport.
  result[0]=vec4<f32>(fog_legacy_closure(scene,sky,0.0,local,distant,fog_surface_open(1.0,true)),1.0);
  result[1]=vec4<f32>(fog_legacy_closure(scene,sky,0.0,local,distant,fog_surface_open(1.0,true)),1.0);
  result[2]=vec4<f32>(fog_legacy_closure(scene,sky,0.0,local,distant,fog_surface_open(1.0,true)),1.0);
  result[3]=vec4<f32>(f32(fog_surface_open(0.0,true)),f32(fog_surface_open(0.49,true)),
   f32(fog_surface_open(0.5,true)),f32(fog_surface_open(1.0,false)));
  result[4]=vec4<f32>(f32(fog_surface_open(1.0,true)),f32(fog_surface_open(1.0,true)),0.0,0.0);
  // Fog touches RGB only. Zero/partial/full authored alpha retain their
  // coverage; alpha does not participate in the vegetation role decision.
  result[5]=vec4<f32>(result[1].rgb,0.0);result[6]=vec4<f32>(result[1].rgb,0.35);
  result[7]=vec4<f32>(result[1].rgb,1.0);
  // A fully exposed opaque endpoint must match the same ray ending
  // at a sheltered wall or rock.
  result[8]=vec4<f32>(fog_legacy_closure(scene,sky,0.0,local,distant,fog_surface_open(1.0,true)),1.0);
 }
 "#;
 let samples=read_probe(&device,&queue,PROBE,&[],36);
 assert!(samples.iter().all(|v|v.is_finite()));
 assert_eq!(&samples[0..4],&samples[4..8],"Plant canopy shelter must not split foreground fog");
 assert_eq!(&samples[8..12],&samples[0..4],"Opaque wall/rock endpoint cannot erase admitted foreground mist");
 assert_eq!(&samples[12..16],&[1.,1.,1.,0.]);assert_eq!(&samples[16..20],&[1.,1.,0.,0.]);
 assert_eq!(samples[23],0.);assert_eq!(samples[27],0.35);assert_eq!(samples[31],1.);
 for row in samples[20..32].chunks_exact(4){assert_eq!(&row[..3],&samples[4..7]);}
 assert_eq!(&samples[32..36],&samples[8..12],"Same outdoor path has identical transport at rain-sheltered opaque endpoints");
 let frame=include_str!("shaders/frame.wgsl");let direct=include_str!("gfx3d/shader3d.wgsl");
 let retained=include_str!("gfx3d/gpu_driven.wgsl");let shared=include_str!("shaders/shading.wgsl");
 assert!(frame.contains("return apply_fog_receiver(rgb, world_pos_rel, true)"),"Generic source gates preserved");
 assert!(direct.contains("material.sun_ambient.w > 0.5,"),"Actual direct MapType role includes trunks");
 assert!(retained.contains("veg_cutout, false, veg_cutout, in.is_veg != 0u"),"Actual retained MapType role includes trunks");
 assert!(shared.contains("!is_cockpit && cave.y >= 0.999"),"Cockpit/native interior receiver exclusion is preserved");
 assert!(direct.contains("return vec4<f32>(out_rgb, out_a2)"));
 assert!(retained.contains("return vec4<f32>(out_rgb, out_a)"));
}

#[test]
fn fog_sky_projection_excludes_lower_hemisphere_and_is_camera_continuous() {
 let(device,queue)=crate::gfx3d::cull::tests::headless().expect("fog sky GPU proof requires device");
 let env=device.create_texture(&wgpu::TextureDescriptor {label:Some("upper_sky_fixture"),size:wgpu::Extent3d {width:128,height:64,depth_or_array_layers:1},mip_level_count:1,sample_count:1,dimension:wgpu::TextureDimension::D2,format:wgpu::TextureFormat::Rgba32Float,usage:wgpu::TextureUsages::COPY_DST|wgpu::TextureUsages::TEXTURE_BINDING,view_formats:&[]});
 let mut pixels=vec![0f32;128*64*4];
 for y in 0..64 { for x in 0..128 {let radiance=if y<32 {1.}else{100.};let i=(y*128+x)*4;pixels[i..i+4].copy_from_slice(&[radiance,radiance,radiance,1.]);}}
 queue.write_texture(wgpu::TexelCopyTextureInfo {texture:&env,mip_level:0,origin:wgpu::Origin3d::ZERO,aspect:wgpu::TextureAspect::All},bytemuck::cast_slice(&pixels),wgpu::TexelCopyBufferLayout {offset:0,bytes_per_row:Some(128*16),rows_per_image:Some(64)},env.size());
 let coefficients=device.create_buffer_init(&wgpu::util::BufferInitDescriptor {label:Some("fog_sky_coefficients"),contents:&[0u8;18*16],usage:wgpu::BufferUsages::STORAGE});
 let module=device.create_shader_module(wgpu::ShaderModuleDescriptor {label:Some("actual_sky_projection"),source:wgpu::ShaderSource::Wgsl(include_str!("sky/sky_sh.wgsl").into())});
 let pipe=device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {label:None,layout:None,module:&module,entry_point:Some("cs_sky_sh"),compilation_options:Default::default(),cache:None});
 let view=env.create_view(&Default::default());
 let bind=device.create_bind_group(&wgpu::BindGroupDescriptor {label:None,layout:&pipe.get_bind_group_layout(0),entries:&[wgpu::BindGroupEntry {binding:0,resource:wgpu::BindingResource::TextureView(&view)},wgpu::BindGroupEntry {binding:1,resource:coefficients.as_entire_binding()}]});
 let mut enc=device.create_command_encoder(&Default::default());
 {let mut pass=enc.begin_compute_pass(&Default::default());pass.set_pipeline(&pipe);pass.set_bind_group(0,&bind,&[]);pass.dispatch_workgroups(1,1,1);}
 queue.submit(Some(enc.finish()));
 let values=read_probe(&device,&queue,r#"
 #import layered_fog_optics::fog_sky_ambient
 struct Sky {c:array<vec4<f32>,18>};
 @group(0) @binding(0) var<storage,read_write> result:array<vec4<f32>>;
 @group(0) @binding(1) var<storage,read> sky:Sky;
 @compute @workgroup_size(1) fn cs_probe(){
   var c:array<vec4<f32>,9>;
   for(var i=0u;i<9u;i=i+1u){c[i]=sky.c[i+9u];}
   result[0]=vec4<f32>(sky.c[0].x,sky.c[9].x,fog_sky_ambient(vec3<f32>(0.0,-1.0,0.0),0.0,c).x,0.0);
   for(var i=0u;i<256u;i=i+1u){
     let angle=0.13+f32(i)*0.0001;
     let ray=vec3<f32>(sin(angle),-cos(angle),0.0);
     result[i+1u]=vec4<f32>(fog_sky_ambient(ray,0.65,c),0.0);
   }
 }
 "#,&[wgpu::BindGroupEntry {binding:1,resource:coefficients.as_entire_binding()}],257*4);
 assert!((values[0]-179.016).abs()<0.04,"existing full-sphere coefficients retained: {}",values[0]);
 assert!((values[1]-1.77245).abs()<0.001,"lower sky cannot enter fog: {}",values[1]);
 assert!((values[2]-0.5).abs()<0.001,"isotropic upper sky has half-sphere energy");
 let rows=values[4..].chunks_exact(4).collect::<Vec<_>>();
 for pair in rows.windows(2){assert!((pair[1][0]-pair[0][0]).abs()<0.0001,"nadir basis threshold must have no camera-following jump: {pair:?}");}
 assert!(values.iter().all(|v|v.is_finite()&&*v>=0.));
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
 let native_max=std::cell::Cell::new(0.);let native_min=std::cell::Cell::new(0.);
 let write_native=|y:f32|{native_max.set(y);native_min.set(y);queue.write_texture(wgpu::TexelCopyTextureInfo {texture:&native,mip_level:0,origin:wgpu::Origin3d::ZERO,aspect:wgpu::TextureAspect::All},
  bytemuck::cast_slice(&[y;4]),wgpu::TexelCopyBufferLayout {offset:0,bytes_per_row:Some(8),rows_per_image:Some(2)},native.size());};
 write_native(0.);
 let env=texture(&device,wgpu::TextureFormat::Rgba8Unorm,1,1,1,false);let env_view=env.create_view(&Default::default());
 let cloud=texture(&device,wgpu::TextureFormat::Rgba8Unorm,1,1,1,false);let cloud_view=cloud.create_view(&Default::default());
 let write_byte=|tex:&wgpu::Texture,byte:u8|queue.write_texture(wgpu::TexelCopyTextureInfo {texture:tex,mip_level:0,origin:wgpu::Origin3d::ZERO,aspect:wgpu::TextureAspect::All},
  &[byte,byte,byte,255],wgpu::TexelCopyBufferLayout {offset:0,bytes_per_row:Some(4),rows_per_image:Some(1)},tex.size());
 write_byte(&env,0);write_byte(&cloud,255);
 let sky_sh=device.create_buffer_init(&wgpu::util::BufferInitDescriptor {label:Some("fog_sky_sh_fixture"),contents:&[0u8;18*16],usage:wgpu::BufferUsages::STORAGE|wgpu::BufferUsages::COPY_DST});
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
 @compute @workgroup_size(1) fn cs_probe(){for(var z=0;z<32;z=z+1){result[z]=textureLoad(volume,vec3<i32>(vec2<i32>(textureDimensions(volume).xy/2u),z),0);}}
 "#;
 // Borrow the fixed volume handle independently so the closure does not borrow mutable fog.
 let volume_view=fog.view.clone();let solar_view=fog.solar_view.clone();
 let read_solar=||read_probe(&device,&queue,READ_VOLUME,&[wgpu::BindGroupEntry {binding:1,resource:wgpu::BindingResource::TextureView(&solar_view)}],128);
 let read=||read_probe(&device,&queue,READ_VOLUME,&[wgpu::BindGroupEntry {binding:1,resource:wgpu::BindingResource::TextureView(&volume_view)}],128);
 let cloud_limit=std::cell::Cell::new([0.,1000.]);
 let ocean=std::cell::Cell::new(None);
 let run=|fog:&mut LayeredFog,camera:&WgrCamera,settings:&WgrLayeredFog,weather:f32,cover:WeatherInputs,generation:u64,below:bool| {
  fog.invalidate(&queue);let mut enc=device.create_command_encoder(&Default::default());
  let inverse=(glam::DMat4::from_cols_array(&camera.view.map(f64::from)).inverse()
   *glam::DMat4::from_cols_array(&camera.proj.map(f64::from)).inverse()).as_mat4().to_cols_array_2d();
  fog.render(&device,&queue,&mut enc,settings,weather,true,0.,camera,inverse,&height,&native_view,generation,
   &mapping,&shadow_view,&csm_view,&env_view,&sky_sh,&cloud_view,cover,below,cloud_limit.get(),[native_min.get(),native_max.get()],ocean.get());queue.submit(Some(enc.finish()));
 };
 run(&mut fog,&camera,&settings,1.,cover(1.,100),1,true);assert!(fog.ready);
 let positive=read();let mut previous=1.;
 for(z,row)in positive.chunks_exact(4).enumerate(){
  let distance=200.*((z as f32+0.5)/32.).powi(2);
  assert!((row[3]-(-0.01*distance).exp()).abs()<0.0007,"actual Beer-Lambert z={z}: {row:?}");
  assert!(row[3]<=previous);previous=row[3];assert_eq!(&row[..3],&[0.;3]);
 }
 // Actual bound upper-sky coefficients must reach the encoded volume with
 // normalized scattering energy. No sun, local lamps or environment texture.
 let mut ambient_coefficients=[0f32;18*4];ambient_coefficients[36..39].fill(3.5449077);
 let saved_sun=camera.sun_diffuse;camera.sun_diffuse=[0.;4];
 queue.write_buffer(&sky_sh,0,bytemuck::cast_slice(&ambient_coefficients));
 settings.control[1]=1.;
 run(&mut fog,&camera,&settings,1.,cover(1.,100),1,true);
 let ambient_encoded=read();
 assert!(ambient_encoded[124]>0.8,"upper sky must actually illuminate the volume");
 for(a,b)in ambient_encoded.chunks_exact(4).zip(positive.chunks_exact(4)){
  assert!((a[0]-(1.-b[3])*settings.control[1]).abs()<0.001,"actual SH sky transport: {a:?}/{b:?}");
  assert_eq!(a[3],b[3],"ambient cannot change extinction");
 }
 queue.write_buffer(&sky_sh,0,&[0u8;18*16]);
 settings.control[1]=0.;
 camera.sun_diffuse=saved_sun;
 // A downward view must integrate the same ground fog while the camera changes
 // altitude. The old buried-midpoint refusal lost whole straddling intervals,
 // making their slice boundaries follow the camera as bright stepped rings.
 let saved_camera=camera;
 camera.view=glam::Mat4::look_at_lh(glam::Vec3::ZERO,glam::Vec3::NEG_Y,glam::Vec3::Z).to_cols_array();
 for altitude in [55.,63.,75.,89.,105.,130.,400.] {
  camera.cam_pos[1]=altitude;
  run(&mut fog,&camera,&settings,1.,cover(1.,100),1,true);
  let tau=-read()[127].ln();
  // Entire0..40m slab plus its upper1m feather, ending0.05m above ground.
  assert!((tau-0.4045).abs()<0.002,"Downward ground crossing at altitude{altitude}: tau={tau}");
 }
 // Oblique ocean rays complete the same thin column beyond the geometry sphere.
 ocean.set(Some(0.));
 for slope in [0.1,0.2,0.45] {
  camera.cam_pos[1]=400.;
  camera.view=glam::Mat4::look_at_lh(glam::Vec3::ZERO,glam::Vec3::new(0.,-slope,1.),glam::Vec3::Y).to_cols_array();
  let inv=(glam::Mat4::from_cols_array(&camera.proj)*glam::Mat4::from_cols_array(&camera.view)).inverse();
  let point=inv*glam::Vec4::new(1./64.,-1./64.,0.,1.);
  let ray=(point.truncate()/point.w).normalize();
  run(&mut fog,&camera,&settings,1.,cover(1.,100),1,true);
  let tau=-read()[127].ln();let expected=0.4045/(-ray.y);
  assert!((tau-expected).abs()<0.02,"Oblique full column slope{slope}: {tau} expected{expected}");
 }
 ocean.set(None);
 camera=saved_camera;
 // Certified current ocean uses its actual5m datum, not a submerged30m seabed.
 // It remains admitted beyond the native map, just as actual off-map water does.
 settings.terrain=[1.,0.,0.,0.];write_native(-30.);camera.cam_pos[1]=25.;ocean.set(Some(5.));
 run(&mut fog,&camera,&settings,1.,cover(1.,100),1,true);assert_eq!(read(),positive,"Actual ocean datum must retain above-water mist");
 camera.cam_pos[0]=750.;run(&mut fog,&camera,&settings,1.,cover(1.,100),1,true);
 assert_eq!(read(),positive,"Off-map actual ocean cannot lose its atmospheric column");
 ocean.set(None);run(&mut fog,&camera,&settings,1.,cover(1.,100),1,true);
 for row in read().chunks_exact(4){assert_eq!(row,&[0.,0.,0.,1.],"No ocean authority must retain native-source refusal");}
 ocean.set(Some(5.));camera.cam_pos[1]=0.;run(&mut fog,&camera,&settings,1.,cover(1.,100),1,true);
 for row in read().chunks_exact(4){assert_eq!(row,&[0.,0.,0.,1.],"Atmospheric mist must not enter ocean below its datum");}
 ocean.set(None);camera=saved_camera;settings.terrain=[0.;4];write_native(0.);
 // Equal flat outdoor columns keep their incident energy outside the solar cache.
 ocean.set(Some(0.));settings.control[1]=1.;
 run(&mut fog,&camera,&settings,1.,cover(1.,100),1,true);let cached_source=read()[124];
 camera.cam_pos[0]=750.;
 run(&mut fog,&camera,&settings,1.,cover(1.,100),1,true);let distant_source=read()[124];
 assert!((distant_source-cached_source).abs()<cached_source*0.02,"Solar cache edge cannot switch off extinction: {cached_source}/{distant_source}");
 ocean.set(None);camera=saved_camera;settings.control[1]=0.;

 // A current far map can replace a rebuilding near source without a whole
 // frame losing its fog. It still obeys the actual depth and overflow guards.
 let mut far_only=cover(1.,100);far_only.controls[0][0]=0.;
 run(&mut fog,&camera,&settings,1.,far_only,1,true);assert!(fog.ready);
 assert_eq!(read(),positive,"Current far coverage must preserve admitted near air");
 // Moving the rain-map footprint cannot move an atmospheric boundary over
 // certified open native terrain. Its Beer-Lambert integral stays constant.
 settings.patch[3]=50.;
 let mut last_tau=0.;
 for edge in 58..=64 {
  let mut inputs=cover(1.,100);inputs.controls[0][0]=0.;
  inputs.matrices[1][2][1]=-1./60.;inputs.matrices[1][3][1]=(edge as f32-60.)/60.;
  run(&mut fog,&camera,&settings,1.,inputs,1,true);
  let values=read();let tau=-values[127].ln();
  let expected_tau=-positive[127].ln();
  assert!(tau.is_finite() && (tau-expected_tau).abs()<0.003,"Open native air survives rain-map edge: {edge}/{tau}");
  if edge>58 {assert!((tau-last_tau).abs()<0.002,"Rain-map motion must not change open-air extinction: {last_tau}/{tau}");}
  last_tau=tau;
 }
 settings.patch[3]=0.;
 // The night air medium remains even without a directional emitter. Actual
 // player-style reflector packets light participating fog, not an additive cone.
 let day_sun=camera.sun_diffuse;let day_direction=camera.sun_dir_world;
 camera.sun_diffuse=[0.;4];camera.sun_dir_world=[0.;4];settings.control[1]=1.;
 fog.upload_local_lights(&queue,&[],&camera);
 run(&mut fog,&camera,&settings,1.,cover(1.,100),1,true);assert!(fog.ready);
 let night=read();assert_eq!(night,positive,"Absent sun must retain the same dark medium and extinction");
 let torch=crate::ffi::WgrLight {pos:[0.,20.,0.,2.],diffuse:[3.,3.,3.,0.9],ambient:[0.,0.,0.,1.],dir:[0.,0.,1.,1.]};
 camera.cam_pos[3]=1.;fog.upload_local_lights(&queue,&[torch],&camera);
 run(&mut fog,&camera,&settings,1.,cover(1.,100),1,true);let beam=read();
 assert!(beam[124]>0.001 && beam.iter().all(|v|v.is_finite()),"Real reflector packet must illuminate night mist");
 for(a,b)in beam.chunks_exact(4).zip(night.chunks_exact(4)){assert_eq!(a[3],b[3],"Lights cannot change optical depth");}
 camera.cam_pos[3]=2.;fog.upload_local_lights(&queue,&[torch,torch],&camera);
 run(&mut fog,&camera,&settings,1.,cover(1.,100),1,true);let doubled=read();
 assert!((doubled[124]-beam[124]*2.).abs()<0.001,"Two actual equal emitters double scene-linear scattering");
 camera.cam_pos[3]=1.;let mut away=torch;away.dir=[0.,0.,-1.,1.];
 fog.upload_local_lights(&queue,&[away],&camera);run(&mut fog,&camera,&settings,1.,cover(1.,100),1,true);
 assert_eq!(read(),night,"A torch aimed away cannot light the view ray");
 let mut outside=torch;outside.pos[0]=300.;fog.upload_local_lights(&queue,&[outside],&camera);
 run(&mut fog,&camera,&settings,1.,cover(1.,100),1,true);assert_eq!(read(),night,"Native light range bounds are preserved");
 // Actual local shadow atlas: slot0, first depth-array layer, a valid
 // projection into the atlas tile, and an occluding stored depth.
 let atlas_layer=csm.create_view(&wgpu::TextureViewDescriptor {dimension:Some(wgpu::TextureViewDimension::D2),base_array_layer:0,array_layer_count:Some(1),..Default::default()});
 let mut enc=device.create_command_encoder(&Default::default());
 {let _pass=enc.begin_render_pass(&wgpu::RenderPassDescriptor {label:None,color_attachments:&[],depth_stencil_attachment:Some(wgpu::RenderPassDepthStencilAttachment {
  view:&atlas_layer,depth_ops:Some(wgpu::Operations {load:wgpu::LoadOp::Clear(0.),store:wgpu::StoreOp::Store}),stencil_ops:None}),timestamp_writes:None,occlusion_query_set:None,multiview_mask:None});}
 queue.submit(Some(enc.finish()));camera.shadow.local_ctl=[1.,1.,0.001,0.];
 camera.shadow.local_vp[0]=[0.,0.,0.,0.,0.,0.,0.,0.,0.,0.,0.,0.,0.,0.,0.5,1.];
 let mut shadowed=torch;shadowed.dir[3]=2.;fog.upload_local_lights(&queue,&[shadowed],&camera);
 run(&mut fog,&camera,&settings,1.,cover(1.,100),1,true);assert_eq!(read(),night,"Actual stored local shadow blocks the beam");
 camera.shadow=crate::ffi::WgrCameraShadow::zeroed();camera.cam_pos[3]=0.;fog.upload_local_lights(&queue,&[torch],&camera);
 run(&mut fog,&camera,&settings,1.,cover(1.,100),1,true);assert_eq!(read(),night,"Stale light slice excluded by actual camera count");
 camera.sun_diffuse=day_sun;camera.sun_dir_world=day_direction;settings.control[1]=0.;
 run(&mut fog,&camera,&settings,1.,cover(1.,100),1,true);
 // Actual incoming solar T has a different path from camera T.
 let solar=read_solar();let inverse=glam::Mat4::from_cols_array(&camera.proj).inverse();
 let ray=(inverse*glam::Vec4::new(1./32.,-1./32.,0.,1.)).truncate().normalize();
 for(z,row)in solar.chunks_exact(4).enumerate(){let y=(41.*(z as f32/31.).powi(2)).max(0.1);let expected=(-0.01*(40.5-y).max(0.)).exp();
  assert!((row[0]-expected).abs()<0.0007,"incoming light column {z}: {row:?}/{expected}");}
 assert!((solar[124]-positive[127]).abs()>0.5,"sun T must never reuse camera T");
 // Replay the exact production surface helper with actual encoded solar texture
 // and actual consumer buffer. The wrapper only provides fixture weather receipt.
 let frame_source=include_str!("shaders/frame.wgsl");let helper=&frame_source[frame_source.find("fn fog_sun_reach(").unwrap()..];
 let receiver_shader=format!(r#"
 #import layered_fog_optics::{{FogConsumer,fog_camera_matches}}
 struct ProbeFrame {{view:mat4x4<f32>,proj:mat4x4<f32>,cam_pos:vec4<f32>,guard:vec4<f32>,receiver:vec4<f32>}}
 @group(0) @binding(0) var<storage,read_write> result:array<vec4<f32>>;
 @group(0) @binding(1) var<uniform> frame:ProbeFrame;
 @group(0) @binding(8) var froxel_samp:sampler;
 @group(0) @binding(22) var<uniform> layer_fog_packet:FogConsumer;
 @group(0) @binding(23) var layer_fog_solar_tex:texture_3d<f32>;
 @group(0) @binding(24) var layer_fog_native_tex:texture_2d<f32>;
 fn weather_map_coverage(p:vec3<f32>)->f32{{return frame.guard.x;}}
 fn weather_map_reach(p:vec3<f32>)->f32{{return frame.guard.y;}}
 {helper}
 @compute @workgroup_size(1) fn cs_probe(){{let t=fog_sun_reach(frame.receiver.xyz);
 result[0]=vec4<f32>(t,vec3<f32>(8.0,4.0,2.0)*frame.guard.z*t);}}
 "#);
 let mut receiver_data=camera.view.to_vec();receiver_data.extend_from_slice(&camera.proj);receiver_data.extend_from_slice(&camera.cam_pos);
 receiver_data.extend_from_slice(&[1.,1.,0.25,0.]);receiver_data.extend_from_slice(&(ray*20.).extend(0.).to_array());
 let receiver_buffer=device.create_buffer_init(&wgpu::util::BufferInitDescriptor {label:None,contents:bytemuck::cast_slice(&receiver_data),usage:wgpu::BufferUsages::UNIFORM|wgpu::BufferUsages::COPY_DST});
 let receiver_consumer=fog.consumer.clone();let receiver_sampler=fog.sampler.clone();
 let receiver_read=||read_probe(&device,&queue,&receiver_shader,&[
  wgpu::BindGroupEntry {binding:1,resource:receiver_buffer.as_entire_binding()},
  wgpu::BindGroupEntry {binding:8,resource:wgpu::BindingResource::Sampler(&receiver_sampler)},
  wgpu::BindGroupEntry {binding:22,resource:receiver_consumer.as_entire_binding()},
  wgpu::BindGroupEntry {binding:23,resource:wgpu::BindingResource::TextureView(&solar_view)},
  wgpu::BindGroupEntry {binding:24,resource:wgpu::BindingResource::TextureView(&native_view)}],4);
 let receiver=receiver_read();assert!(receiver[0]>0.7 && receiver[0]<0.9);
 receiver_data[37]=0.;queue.write_buffer(&receiver_buffer,0,bytemuck::cast_slice(&receiver_data));assert_eq!(receiver_read(),receiver,"Canopy rain shelter must not produce a bright sunlight island");receiver_data[37]=1.;queue.write_buffer(&receiver_buffer,0,bytemuck::cast_slice(&receiver_data));assert!((receiver[1]-receiver[0]*2.).abs()<1e-6,"one cloud factor, no extra energy");
 receiver_data[36]=0.;queue.write_buffer(&receiver_buffer,0,bytemuck::cast_slice(&receiver_data));
 assert_eq!(receiver_read(),receiver,"Rain-map coverage cannot move sunlight attenuation");receiver_data[36]=1.;
 let original=receiver_data[0];receiver_data[0]=0.9;queue.write_buffer(&receiver_buffer,0,bytemuck::cast_slice(&receiver_data));
 assert_eq!(receiver_read(),vec![1.,2.,1.,0.5],"mismatched camera remains neutral");receiver_data[0]=original;
 queue.write_buffer(&receiver_buffer,0,bytemuck::cast_slice(&receiver_data));
 // The incoming ray consumes actual patch density and actual native bowl
 // curvature, not merely the former midpoint vertical-column approximation.
 settings.terrain=[1.,0.,0.,0.];run(&mut fog,&camera,&settings,1.,cover(1.,100),1,true);
 let flat_solar=read_solar();settings.patch=[0.8,25.,5.,0.];
 run(&mut fog,&camera,&settings,1.,cover(1.,100),1,true);let patch_solar=read_solar();
 assert!(patch_solar.iter().step_by(4).zip(flat_solar.iter().step_by(4)).any(|(a,b)|(a-b).abs()>0.001),"actual sun ray must consume world patch density");
 for(a,b)in patch_solar.iter().step_by(4).zip(flat_solar.iter().step_by(4)){
  assert!(*a>=b.powf(1.8)-0.001 && *a<=b.powf(0.2)+0.001,"bounded incoming density {a}/{b}");}
 settings.patch=[0.;4];settings.terrain=[1.,0.,1.,150.];
 queue.write_texture(wgpu::TexelCopyTextureInfo {texture:&native,mip_level:0,origin:wgpu::Origin3d::ZERO,aspect:wgpu::TextureAspect::All},
  bytemuck::cast_slice(&[20f32,0.,0.,20.]),wgpu::TexelCopyBufferLayout {offset:0,bytes_per_row:Some(8),rows_per_image:Some(2)},native.size());native_max.set(20.);
 run(&mut fog,&camera,&settings,1.,cover(1.,100),1,true);
 assert!(read_solar()[0]<flat_solar[0]-0.01,"actual native bowl must attenuate incoming sun more than flat terrain");
 write_native(0.);settings.terrain=[0.;4];
 // Regression for the original view-ray counterexample: high camera,
 // actual native ground and1.8m receivers must retain a1..5m compact band.
 camera.cam_pos[1]=75.;settings.terrain=[1.,0.,0.,0.];settings.layers=[[1.,5.,0.35,0.018],[10.,20.,1.,0.]];
 run(&mut fog,&camera,&settings,1.,cover(1.,100),1,true);
 receiver_data[33]=75.;receiver_data[40]=0.;receiver_data[42]=150.;
 for(agl,expected)in [(0f32,(-0.018f32*4.35).exp()),(1.8,(-0.018f32*3.375).exp())]{
  receiver_data[41]=agl-75.;queue.write_buffer(&receiver_buffer,0,bytemuck::cast_slice(&receiver_data));
  let observed=receiver_read()[0];println!("COMPACT_SOLAR_FIXED highcamera75 agl={agl} expected={expected} actual={observed}");
  assert!((observed-expected).abs()<0.002,"actual Compact terrain/body must retain thin incoming extinction");
 }
 camera.cam_pos[1]=20.;receiver_data[33]=20.;receiver_data[40..44].copy_from_slice(&(ray*20.).extend(0.).to_array());
 queue.write_buffer(&receiver_buffer,0,bytemuck::cast_slice(&receiver_data));settings.layers=[[0.,40.,1.,0.01],[70.,100.,1.,0.]];settings.terrain=[0.;4];
 // Actual elevated native terrain must reproduce the same optical depth at
 // equal AGL, independent of camera height. The old ASL path must stay empty.
 write_native(100.);camera.cam_pos[1]=120.;settings.terrain=[1.,0.,0.,0.];
 run(&mut fog,&camera,&settings,1.,cover(1.,100),1,true);let elevated=read();
 for(a,b)in positive.iter().zip(&elevated){assert!((a-b).abs()<0.0007,"equal AGL {a}/{b}");}
 settings.terrain[0]=0.;run(&mut fog,&camera,&settings,1.,cover(1.,100),1,true);
 for row in read().chunks_exact(4){assert_eq!(row,&[0.,0.,0.,1.],"legacy ASL does not follow mountain");}
 settings.terrain[0]=1.;cloud_limit.set([1.,110.]);
 run(&mut fog,&camera,&settings,1.,cover(1.,100),1,false);assert!(fog.ready);
 for row in read().chunks_exact(4){assert_eq!(row,&[0.,0.,0.,1.],"following medium cannot enter cloud deck");}
 cloud_limit.set([0.,1000.]);settings.terrain=[0.;4];camera.cam_pos[1]=20.;write_native(0.);
 // Four native-sun intervals see a rising real plane block low-angle sunlight.
 settings.terrain=[1.,0.,0.,0.];camera.sun_dir_world=[-0.99995,-0.01,0.,0.];
 queue.write_texture(wgpu::TexelCopyTextureInfo {texture:&native,mip_level:0,origin:wgpu::Origin3d::ZERO,aspect:wgpu::TextureAspect::All},
  bytemuck::cast_slice(&[-500f32,500.,-500.,500.]),wgpu::TexelCopyBufferLayout {offset:0,bytes_per_row:Some(8),rows_per_image:Some(2)},native.size());native_max.set(500.);native_min.set(-500.);
 run(&mut fog,&camera,&settings,1.,cover(1.,100),1,true);
 assert_eq!(read_solar()[124],0.,"actual rising native mountain blocks incoming light");
 write_native(0.);camera.cam_pos[0]=490.;run(&mut fog,&camera,&settings,1.,cover(1.,100),1,true);
 assert_eq!(read_solar()[0],1.,"sun ray leaving native source cannot fabricate attenuation");
 camera.cam_pos[0]=0.;camera.sun_dir_world=[0.,-1.,0.,0.];settings.terrain=[0.;4];

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
 assert!(read_solar()[0]<0.9,"rain shelter cannot switch off the incoming sunlight medium");
 for row in read().chunks_exact(4){assert_eq!(row,&[0.,0.,0.,1.],"actual roof refuses medium");}clear_cover(1.);
 write_native(100.);run(&mut fog,&camera,&settings,1.,cover(1.,100),1,true);
 for row in read().chunks_exact(4){assert_eq!(row,&[0.,0.,0.,1.],"native underground refuses medium");}write_native(0.);
 queue.write_buffer(&counts,0,bytemuck::cast_slice(&[101u32,0,0,0]));run(&mut fog,&camera,&settings,1.,cover(1.,100),1,true);
 assert_eq!(read(),lit,"Overflowed shelter data cannot erase native outdoor air");
 queue.write_buffer(&counts,0,bytemuck::cast_slice(&[0u32;4]));
 run(&mut fog,&camera,&settings,1.,cover(0.,100),1,true);assert!(fog.ready);
 assert_eq!(read(),lit,"Rebuilding rain maps cannot erase native outdoor air");
 for(weather,generation,below,ready)in [(0.,1,true,1.),(1.,0,true,1.),(1.,1,false,1.)]{
  run(&mut fog,&camera,&settings,weather,cover(ready,100),generation,below);assert!(!fog.ready);
 }
 settings.control[0]=0.;run(&mut fog,&camera,&settings,1.,cover(1.,100),1,true);assert!(!fog.ready);
 assert_eq!(receiver_read(),vec![1.,2.,1.,0.5],"OFF/stale solar texture is never consumed");
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
 assert_eq!(std::mem::size_of::<Consumer>(),272);assert_eq!(std::mem::size_of::<Volume>(),432);
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

#[test]
fn actual_world_fog_density_gpu_is_coherent_bounded_and_terrain_qualified() {
 let(device,queue)=crate::gfx3d::cull::tests::headless().expect("density requires actual GPU");
 const PROBE:&str=r#"
 #import layered_fog_optics::{fog_world_noise,fog_patch_density,fog_valley_density,fog_domain_feather,fog_segment_density}
 @group(0) @binding(0) var<storage,read_write> result:array<vec4<f32>>;
 @compute @workgroup_size(1) fn cs_probe(){
  let p=vec3<f32>(150.0,35.0,-10.0);let patchiness=vec4<f32>(0.95,250.0,70.0,30.0);
  var lo=10.0;var hi=-10.0;
  for(var i=0u;i<256u;i=i+1u){let x=vec3<f32>(f32(i)*33.7,f32(i)*1.71,-f32(i)*23.13);
   let d=fog_patch_density(x,patchiness);lo=min(lo,d);hi=max(hi,d);}
  result[0]=vec4<f32>(lo,hi,fog_patch_density(p,vec4<f32>(0.0)),fog_patch_density(p,patchiness));
  result[1]=vec4<f32>(fog_world_noise(vec3<f32>(1.0-1e-5,0.7,-1.2)),fog_world_noise(vec3<f32>(1.0+1e-5,0.7,-1.2)),
   fog_world_noise(vec3<f32>(-1.0-1e-5,0.7,1.2)),fog_world_noise(vec3<f32>(-1.0+1e-5,0.7,1.2)));
  result[2]=vec4<f32>(fog_valley_density(100.0,vec4<f32>(100.0),2.0,150.0),
   fog_valley_density(100.0,vec4<f32>(80.0),2.0,150.0),fog_valley_density(100.0,vec4<f32>(115.0),2.0,150.0),
   fog_valley_density(100.0,vec4<f32>(150.0),2.0,150.0));
  result[3]=vec4<f32>(fog_domain_feather(-1.0,30.0),fog_domain_feather(15.0,30.0),fog_domain_feather(30.0,30.0),fog_domain_feather(0.0,0.0));
  let slab=vec4<f32>(2.0,5.0,1.0,0.01);
  // Ray and actual native slope rise together: relative altitude stays3m.
  result[4]=vec4<f32>(fog_segment_density(3.0,0.0,100.0,slab),fog_segment_density(103.0,0.0,100.0,slab),
   fog_patch_density(p,patchiness),fog_patch_density(p,patchiness));
 }
 "#;
 let r=read_probe(&device,&queue,PROBE,&[],20);assert!(r.iter().all(|v|v.is_finite()));
 assert!(r[0]>=0.05-1e-5 && r[1]<=1.95+1e-5 && r[1]-r[0]>0.3);assert_eq!(r[2],1.);
 assert!((r[4]-r[5]).abs()<1e-4 && (r[6]-r[7]).abs()<1e-4,"positive/negative cell seams {r:?}");
 assert_eq!(&r[8..12],&[1.,1.,3.,3.]);assert_eq!(&r[12..16],&[0.,0.5,1.,1.]);
 assert_eq!(&r[16..18],&[100.,0.]);assert_eq!(r[18],r[19]);
}

#[test]
fn density_controls_refuse_invalid_enabled_scales_and_terrain_source_is_explicit() {
 let mut p=WgrLayeredFog::default();p.control[0]=1.;assert!(valid_settings(&p));
 p.terrain=[1.,0.,1.,150.];p.patch=[0.45,250.,70.,30.];assert!(valid_settings(&p));
 for(i,v)in [(0,-0.1),(0,1.1),(1,f32::NAN),(2,3.1),(3,0.)]{let mut q=p;q.terrain[i]=v;assert!(!valid_settings(&q));}
 for(i,v)in [(0,0.96),(1,0.),(2,0.),(3,2001.),(3,f32::INFINITY)]{let mut q=p;q.patch[i]=v;assert!(!valid_settings(&q));}
 p.terrain=[0.;4];p.patch=[0.;4];assert!(valid_settings(&p),"dormant scales preserve legacy packet");
 let terrain=include_str!("terrain/terrain.wgsl");let frame=include_str!("shaders/frame.wgsl");
 assert!(terrain.contains("rgb = apply_fog_terrain(rgb, in.world_pos)"));
 assert!(frame.contains("fn apply_fog_terrain") && frame.contains("return apply_fog_receiver(rgb, world_pos_rel, true)"));
 assert!(frame.contains("return apply_fog_receiver(rgb, world_pos_rel, true)"));
 let compute=include_str!("layered_fog.wgsl");
 assert!(compute.contains("p.y <= native.x+0.05") && compute.contains("far_counts[0] > cap"));
}

#[test]
fn production_direct_light_uses_only_qualified_incoming_solar_transmission() {
 let frame=include_str!("shaders/frame.wgsl");let object=include_str!("shaders/shading.wgsl");let terrain=include_str!("terrain/terrain.wgsl");
 assert!(frame.contains("@group(0) @binding(23) var layer_fog_solar_tex"));
 assert!(object.contains("cloud_sun_shadow(world_abs.xz) * fog_sun_reach(world_pos)"));
 assert!(terrain.contains("var sun_diffuse = frame.sun_diffuse.rgb * fog_sun_reach(in.world_pos)"));
 let helper=&frame[frame.find("fn fog_sun_reach(").unwrap()..];
 assert!(helper.contains("!fog_camera_matches") && helper.contains("layer_fog_packet.native.w<0.5") && !helper.contains("weather_map_coverage(absolute)") && !helper.contains("weather_map_reach(absolute)"));
 assert!(helper.contains("textureSampleLevel(layer_fog_solar_tex") && !helper.contains("textureSampleLevel(layer_fog_tex"),"must not use view-ray T for sun-ray T");
 let volume=include_str!("layered_fog.wgsl");let incoming=&volume[volume.find("fn fog_sun_segment_tau").unwrap()..volume.find("@compute").unwrap()];
 assert!(incoming.contains("fog_patch_density(midpoint") && incoming.contains("fog_pooling(midpoint") && incoming.contains("fog_native_ground(end.xz)"));
 assert!(!incoming.contains("textureSampleLevel(clouds") && !incoming.contains("fog_light_visibility("),"cloud/CSM remain exactly once in direct visibility");
}

#[test]
fn actual_solar_consumer_layout_resources_and_native_rebinding_are_bounded() {
 assert_eq!(std::mem::size_of::<Consumer>(),272);
 assert_eq!(std::mem::offset_of!(Consumer,solar_domain),224);
 assert_eq!(std::mem::offset_of!(Consumer,native),240);
 assert_eq!(std::mem::offset_of!(Consumer,native_size),256);
 let binding=include_str!("gfx3d/mod.rs");
 assert!(binding.contains("self.bound_fog_native_gen != fog_native_gen") && binding.contains("self.bound_fog_native_gen = fog_native_gen"));
 assert!(binding.contains("fog_solar, fog_native, heightmap_gen"));
 let renderer=include_str!("lib.rs").replace("\r\n","\n");assert!(renderer.contains(".max_sampled_textures_per_shader_stage\n                .clamp(16, 64)"),"production requests actual adapter limit, not assumed32");
 for(source,path)in [(include_str!("terrain/terrain.wgsl"),"terrain.wgsl"),(include_str!("gfx3d/shader3d.wgsl"),"direct.wgsl"),(include_str!("gfx3d/gpu_driven.wgsl"),"retained.wgsl")]{
  let mut composer=crate::shaders::build_composer();let module=composer.make_naga_module(naga_oil::compose::NagaModuleDescriptor {source,file_path:path,shader_defs:crate::shaders::shader_defs(),..Default::default()}).unwrap();
  let info=naga::valid::Validator::new(naga::valid::ValidationFlags::all(),naga::valid::Capabilities::all()).validate(&module).unwrap();
  let mut solar_used=false;let mut native_used=false;
  for(i,entry)in module.entry_points.iter().enumerate(){if entry.stage!=naga::ShaderStage::Fragment{continue;}
   let usage=info.get_entry_point(i);let mut sampled=0;let mut arrays=0;
   for(handle,global)in module.global_variables.iter(){if usage[handle].is_empty(){continue;}
    match module.types[global.ty].inner {naga::TypeInner::Image {class:naga::ImageClass::Sampled {..}|naga::ImageClass::Depth {..},..}=>sampled+=1,
     naga::TypeInner::BindingArray {..}=>arrays+=1,_=>{}}
    if let Some(b)=&global.binding {solar_used|=b.group==0&&b.binding==23;native_used|=b.group==0&&b.binding==24;}
   }
   println!("ACTUAL_SOLAR_RESOURCE_CENSUS {path}/{} ordinarySampled={sampled} bindingArrays={arrays}",entry.name);
   assert!(sampled<=64,"must fit existing production adaptive upper cap; actual hardware must still pass installed pipeline");
  }
  assert!(solar_used&&native_used,"both actual surface paths must consume the qualified light field");
 }
}

#[test]
fn projected_shadow_gpu_preserves_foreground_airlight() {
 let(device,queue)=crate::gfx3d::cull::tests::headless().expect("shadow fog composition requires GPU");
 const PROBE:&str=r#"
 #import layered_fog_optics::fog_legacy_closure
 @group(0) @binding(0) var<storage,read_write> result:array<vec4<f32>>;
 @compute @workgroup_size(1) fn cs_probe(){
  let scene=vec3<f32>(12.0,6.0,3.0);let air=vec3<f32>(4.0,5.0,6.0);
  let local=vec4<f32>(1.0,2.0,3.0,0.25);let distant=vec4<f32>(2.0,3.0,4.0,0.1);
  for(var i=0u;i<3u;i=i+1u){
   let amount=f32(i)*0.5;let opacity=0.7;
   let receiver=fog_legacy_closure(scene,air,amount,local,distant,true);
   let shadow=fog_legacy_closure(vec3<f32>(0.0),air,amount,local,distant,true);
   let composited=receiver*(1.0-opacity)+shadow*opacity;
   let expected=fog_legacy_closure(scene*(1.0-opacity),air,amount,local,distant,true);
   result[i*2u]=vec4<f32>(composited,1.0);result[i*2u+1u]=vec4<f32>(expected,1.0);
  }
 }"#;
 let actual=read_probe(&device,&queue,PROBE,&[],24);
 for pair in actual.chunks_exact(8){for channel in 0..3{assert!((pair[channel]-pair[channel+4]).abs()<0.00001,"shadow must darken the receiver, not the intervening fog: {pair:?}");}}
}
