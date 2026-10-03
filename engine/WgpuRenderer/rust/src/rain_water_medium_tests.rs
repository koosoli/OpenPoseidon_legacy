use bytemuck::Zeroable;
use wgpu::util::DeviceExt;

#[test]
fn actual_material_row_is_shared_and_medium_keeps_physical_support() {
    let mut composer=crate::shaders::build_composer();
    let rain=composer.make_naga_module(naga_oil::compose::NagaModuleDescriptor{
        source:include_str!("rain_water.wgsl"),file_path:"rain_water.wgsl",..Default::default()
    }).unwrap_or_else(|e|panic!("{}",e.emit_to_string(&composer)));
    let terrain=composer.make_naga_module(naga_oil::compose::NagaModuleDescriptor{
        source:include_str!("terrain/terrain.wgsl"),file_path:"terrain/terrain.wgsl",..Default::default()
    }).unwrap_or_else(|e|panic!("{}",e.emit_to_string(&composer)));
    for module in [&rain,&terrain] {
        let (_,material)=module.types.iter().find(|(_,t)|t.name.as_deref().is_some_and(|n|n.starts_with("TerrainMaterial"))).unwrap();
        match &material.inner {
            naga::TypeInner::Struct{members,span}=>{
                assert_eq!(*span,std::mem::size_of::<crate::ffi::WgrTerrainMaterial>() as u32);
                assert_eq!(members.iter().find(|m|m.name.as_deref()==Some("puddle_flags")).unwrap().offset,380);
            }
            _=>panic!("Shared material must remain a struct"),
        }
    }
    let source=include_str!("rain_water.wgsl");
    for guard in ["rain_water_raster_support_depth(water,bed,frame.ground_weather.z,in.world.y)",
        "if (local_depth <= 0.001)","if (cover <= 0.0 || snow > 0.002)",
        "medium_control.yz != rainwater.identity.xy","entry >= arrayLength(&medium_materials)",
        "bitcast<vec2<u32>>(world_xz)","bitcast<vec2<u32>>(cell)","0x7f800000u",
        "let soil=rain_water_actual_soil(in.world.xz)","if (soil > 0.0)"] {
        assert!(source.contains(guard),"Missing actual source/support gate {guard}");
    }
    let finite_guard=source.find("bitcast<vec2<u32>>(world_xz)").unwrap();
    let cell_arithmetic=source.find("let cell=(world_xz-medium_terrain.world_origin)").unwrap();
    let source_load=source.find("let entry=textureLoad(medium_indices").unwrap();
    assert!(finite_guard < cell_arithmetic && cell_arithmetic < source_load,
        "Invalid world coordinates must be refused before arithmetic and material sampling");
    assert_eq!(source.matches("fn fs_main(").count(),1);
    assert!(!include_str!("shaders/rain_water_medium.wgsl").contains("rain_strength"));
}

fn actual_source_query()->String {
    let source=include_str!("rain_water.wgsl");
    let begin=source.find("// Prefix of the EXISTING terrain uniform").unwrap();
    let end=source[begin..].find("fn fine_record").unwrap()+begin;
    format!(r#"
#import terrain_material::TerrainMaterial
#import rain_water_medium::{{rain_water_medium_optics, rain_water_soil_fraction}}
#import rain_water_optics::rain_water_surface_optics
struct RainWaterParams {{domain:vec4<f32>,control:vec4<f32>,identity:vec4<u32>}};
@group(1) @binding(1) var<uniform> rainwater:RainWaterParams;
{}
@group(0) @binding(0) var<storage,read_write> result:array<vec4<f32>>;
@compute @workgroup_size(16)
fn cs_probe(@builtin(global_invocation_id) id:vec3<u32>) {{
 let i=id.x;if(i>=16u){{return;}}
 let points=array<vec2<f32>,16>(
 vec2<f32>(25.,25.),vec2<f32>(75.,25.),vec2<f32>(125.,25.),vec2<f32>(175.,25.),
 vec2<f32>(225.,25.),vec2<f32>(200.5,25.),vec2<f32>(-0.5,25.),vec2<f32>(250.5,25.),
 vec2<f32>(225.,225.),vec2<f32>(bitcast<f32>(0x7fc00000u),25.),
 vec2<f32>(bitcast<f32>(0x7f800000u),25.),vec2<f32>(207.,25.),
 vec2<f32>(25.,25.),vec2<f32>(25.,25.),vec2<f32>(25.,25.),vec2<f32>(25.,25.));
 let depths=array<f32,8>(0.,0.002,0.006652,0.014576,0.042370986,0.078471638,3.,1e30);
 let depth=depths[i%8u];let soil=rain_water_actual_soil(points[i]);
 let reflected=select(vec3<f32>(0.8,0.7,0.6),vec3<f32>(0.0),i==14u);
 let body=select(vec3<f32>(0.09,0.12,0.104),vec3<f32>(0.0),i==14u);
 result[i*3u]=vec4<f32>(soil,depth,0.,0.);
 result[i*3u+1u]=rain_water_medium_optics(depth,0.5,reflected,body,1.,soil);
 result[i*3u+2u]=rain_water_surface_optics(depth,0.5,reflected,body,1.);
}}
"#,&source[begin..end])
}

#[test]
fn actual_gpu_medium_source_bounds_energy_and_clear_counterexamples() {
    let (device,queue)=crate::gfx3d::cull::tests::headless().expect("Actual medium needs device");
    let mut composer=crate::shaders::build_composer();
    let shader=crate::shaders::make_module(&device,&mut composer,"actual_medium",&actual_source_query(),"actual_medium.wgsl");
    let pipeline=device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor{
        label:None,layout:None,module:&shader,entry_point:Some("cs_probe"),compilation_options:Default::default(),cache:None});
    let mut params:crate::ffi::WgrTerrainParams=bytemuck::Zeroable::zeroed();
    params.land_grid=50.;params.land_range=5;params.terrain_grid=6.25;params.hm_width=40;params.hm_height=40;
    let params_buffer=device.create_buffer_init(&wgpu::util::BufferInitDescriptor{label:None,
        contents:bytemuck::bytes_of(&params),usage:wgpu::BufferUsages::UNIFORM});
    let indices=device.create_texture(&wgpu::TextureDescriptor{label:None,size:wgpu::Extent3d{width:5,height:5,depth_or_array_layers:1},
        mip_level_count:1,sample_count:1,dimension:wgpu::TextureDimension::D2,format:wgpu::TextureFormat::R16Uint,
        usage:wgpu::TextureUsages::TEXTURE_BINDING|wgpu::TextureUsages::COPY_DST,view_formats:&[]});
    let mut ids:Vec<u16>=(0..25).map(|i|(i%5) as u16).collect();ids[24]=99;
    queue.write_texture(wgpu::TexelCopyTextureInfo{texture:&indices,mip_level:0,origin:wgpu::Origin3d::ZERO,aspect:wgpu::TextureAspect::All},
        bytemuck::cast_slice(&ids),wgpu::TexelCopyBufferLayout{offset:0,bytes_per_row:Some(10),rows_per_image:Some(5)},indices.size());
    let mut materials=vec![crate::ffi::WgrTerrainMaterial::zeroed();5];
    materials[0].puddle_flags=3;materials[1].puddle_flags=1;
    materials[2].surface_count=2;materials[2].puddle_flags=1|2|(1<<8);
    materials[3].puddle_flags=2; // Unowned soft flag cannot admit a ground receiver.
    materials[4].puddle_flags=5;
    let materials_buffer=device.create_buffer_init(&wgpu::util::BufferInitDescriptor{label:None,
        contents:bytemuck::cast_slice(&materials),usage:wgpu::BufferUsages::STORAGE});
    let mut rain:crate::ffi::WgrRainWaterParams=bytemuck::Zeroable::zeroed();rain.generation=3;
    let rain_buffer=device.create_buffer_init(&wgpu::util::BufferInitDescriptor{label:None,
        contents:bytemuck::bytes_of(&rain),usage:wgpu::BufferUsages::UNIFORM});
    let control=device.create_buffer_init(&wgpu::util::BufferInitDescriptor{label:None,
        contents:bytemuck::cast_slice(&[1u32,3,0,0]),usage:wgpu::BufferUsages::UNIFORM|wgpu::BufferUsages::COPY_DST});
    let bytes=16*3*16;
    let output=device.create_buffer(&wgpu::BufferDescriptor{label:None,size:bytes,usage:wgpu::BufferUsages::STORAGE|wgpu::BufferUsages::COPY_SRC,mapped_at_creation:false});
    let readback=device.create_buffer(&wgpu::BufferDescriptor{label:None,size:bytes,usage:wgpu::BufferUsages::COPY_DST|wgpu::BufferUsages::MAP_READ,mapped_at_creation:false});
    let result_bind=device.create_bind_group(&wgpu::BindGroupDescriptor{label:None,layout:&pipeline.get_bind_group_layout(0),
        entries:&[wgpu::BindGroupEntry{binding:0,resource:output.as_entire_binding()}]});
    let rain_bind=device.create_bind_group(&wgpu::BindGroupDescriptor{label:None,layout:&pipeline.get_bind_group_layout(1),
        entries:&[wgpu::BindGroupEntry{binding:1,resource:rain_buffer.as_entire_binding()}]});
    let empty=device.create_bind_group(&wgpu::BindGroupDescriptor{label:None,layout:&pipeline.get_bind_group_layout(2),entries:&[]});
    let medium_bind=device.create_bind_group(&wgpu::BindGroupDescriptor{label:None,layout:&pipeline.get_bind_group_layout(3),entries:&[
        wgpu::BindGroupEntry{binding:0,resource:params_buffer.as_entire_binding()},
        wgpu::BindGroupEntry{binding:1,resource:wgpu::BindingResource::TextureView(&indices.create_view(&Default::default()))},
        wgpu::BindGroupEntry{binding:2,resource:materials_buffer.as_entire_binding()},
        wgpu::BindGroupEntry{binding:3,resource:control.as_entire_binding()}]});
    for (ready,generation) in [(1u32,3u32),(0,3),(1,4)] {
        queue.write_buffer(&control,0,bytemuck::cast_slice(&[ready,generation,0,0]));
        let mut encoder=device.create_command_encoder(&Default::default());
        {let mut pass=encoder.begin_compute_pass(&Default::default());pass.set_pipeline(&pipeline);
            pass.set_bind_group(0,&result_bind,&[]);pass.set_bind_group(1,&rain_bind,&[]);
            pass.set_bind_group(2,&empty,&[]);pass.set_bind_group(3,&medium_bind,&[]);pass.dispatch_workgroups(1,1,1);}
        encoder.copy_buffer_to_buffer(&output,0,&readback,0,bytes);queue.submit([encoder.finish()]);
        let (tx,rx)=std::sync::mpsc::channel();readback.slice(..).map_async(wgpu::MapMode::Read,move|r|tx.send(r).unwrap());
        device.poll(wgpu::PollType::wait_indefinitely()).unwrap();rx.recv().unwrap().unwrap();
        let data=readback.slice(..).get_mapped_range();let values:&[[f32;4]]=bytemuck::cast_slice(&data);
        for i in 0..16 {
            let soil=values[i*3][0];let depth=values[i*3][1];let result=values[i*3+1];let clear=values[i*3+2];
            if ready==0 || generation!=3 || [1,2,3,5,6,7,8,9,10].contains(&i) {assert_eq!(soil,0.,"Source refusal i={i}, ready={ready}, generation={generation}");}
            else if i==11 {assert!((soil-0.352).abs()<0.0001,"Cultivated fraction i={i}, ready={ready}, generation={generation}, soil={soil}");}
            else {assert_eq!(soil,1.,"Pure soil i={i}, ready={ready}, generation={generation}");}
            if soil==0. {assert_eq!(result,clear,"Clear source must be exactly unchanged at {i}");}
            assert!(result.iter().all(|v|v.is_finite()&&*v>=0.));assert!(result[3]<=1.);
            let t=((depth as f64-0.008)/0.042).clamp(0.,1.);let standing=t*t*(3.-2.*t);
            let fresnel=standing*(0.025+0.975*0.5f64.powi(5));let transmission=(-(3.+15.*soil as f64)*(depth as f64).min(64.)).exp();
            let expected_alpha=fresnel+(1.-fresnel)*(1.-transmission);
            assert!((result[3] as f64-expected_alpha).abs()<0.00001,"Physical scalar energy at {i}");
            let ratio=[0.035/0.045,0.018/0.060,0.007/0.052];
            let body=if i==14 {[0.;3]}else{[0.09,0.12,0.104]};
            let reflected=if i==14 {[0.;3]}else{[0.8,0.7,0.6]};
            for c in 0..3 {let expected=reflected[c]*fresnel+body[c]*(1.+soil as f64*(ratio[c]-1.))*(1.-fresnel)*(1.-transmission);
                assert!((result[c] as f64*result[3] as f64-expected).abs()<0.00001,"No extra radiance at {i}/{c}");}
        }
        drop(data);readback.unmap();
    }
}
