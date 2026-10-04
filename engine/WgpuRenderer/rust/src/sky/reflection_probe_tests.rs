//! Real Sky pipelines/resource bindings, raw linear GPU readback. The first
//! controls isolate the source mechanism; they are not an installed-frame replay.

const SAMPLES: [(u32,u32);6] = [(128,1),(128,55),(128,63),(128,64),(128,80),(128,126)];

// Control variants retain production imports/defines; raw WGSL parsing cannot
// resolve the shared fog optics used by the current real Sky construction.
fn compose_probe(source:&str)->naga::Module {
    let mut composer=crate::shaders::build_composer();
    composer.make_naga_module(naga_oil::compose::NagaModuleDescriptor {
        source,file_path:"sky/reflection_probe.wgsl",
        shader_defs:crate::shaders::shader_defs(),..Default::default()
    }).unwrap_or_else(|e|panic!("reflection control composition: {}",e.emit_to_string(&composer)))
}

fn control_source(interval: bool) -> String {
    let source=include_str!("sky.wgsl");
    let needle="bg, 12.0, 2, 0.0, 1e12";
    assert_eq!(source.matches(needle).count(),1,"Only actual env budget may change");
    if !interval { return source.replacen(needle,"bg, 128.0, 4, 0.0, 1e12",1); }
    let begin=source.find("fn fs_sky_env(").unwrap();
    let end=source[begin..].find("// ---- Depth-aware").unwrap()+begin;
    let mut probe=source[begin..end].replace("fn fs_sky_env(","fn fs_env_interval_probe(");
    probe=probe.replace("return vec4<f32>(color * cloud.trans + cloud.inscatter, 1.0);",
        "let interval=cloud_interval(pos,dir); return vec4<f32>(dir.y,deck.trans,interval.x,interval.y);");
    assert!(probe.contains("return vec4<f32>(dir.y,deck.trans,interval.x,interval.y)"));
    format!("{source}\n@fragment\n{probe}")
}

#[test]
fn actual_env_budget_and_interval_controls_parse_and_validate() {
    for src in [include_str!("sky.wgsl").to_string(),control_source(false),control_source(true)] {
        let module=compose_probe(&src);
        naga::valid::Validator::new(naga::valid::ValidationFlags::all(),naga::valid::Capabilities::all())
            .validate(&module).unwrap();
    }
}

fn half(h:u16)->f32 {
    let sign=u32::from(h&0x8000)<<16;let e=u32::from((h>>10)&31);let mut m=u32::from(h&1023);
    let bits=if e==0 {
        if m==0 {sign} else {let mut exp=113u32;while m&1024==0 {m<<=1;exp-=1;}sign|(exp<<23)|((m&1023)<<13)}
    }else if e==31 {sign|0x7f800000|(m<<13)}else{sign|((e+112)<<23)|(m<<13)};
    f32::from_bits(bits)
}
#[test]
fn probe_half_decode_retains_linear_subnormal_radiance() {
    assert_eq!(half(0x3c00),1.);assert_eq!(half(1),2f32.powi(-24));
    assert_eq!(half(0x0400),2f32.powi(-14));assert_eq!(half(0x8000).to_bits(),(-0f32).to_bits());
}

fn pipeline(device:&wgpu::Device,sky:&super::Sky,source:&str,entry:&str,format:wgpu::TextureFormat)->wgpu::RenderPipeline {
    let shader=device.create_shader_module(wgpu::ShaderModuleDescriptor{label:Some("actual_sky_env_control"),source:wgpu::ShaderSource::Naga(std::borrow::Cow::Owned(compose_probe(source)))});
    let bind=sky.env_pipeline.get_bind_group_layout(0);
    let layout=device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor{label:None,bind_group_layouts:&[Some(&bind)],immediate_size:0});
    device.create_render_pipeline(&wgpu::RenderPipelineDescriptor{label:Some(entry),layout:Some(&layout),
        vertex:wgpu::VertexState{module:&shader,entry_point:Some("vs_main"),buffers:&[],compilation_options:Default::default()},
        primitive:Default::default(),depth_stencil:None,multisample:Default::default(),
        fragment:Some(wgpu::FragmentState{module:&shader,entry_point:Some(entry),targets:&[Some(wgpu::ColorTargetState{format,blend:None,write_mask:wgpu::ColorWrites::ALL})],compilation_options:Default::default()}),
        multiview_mask:None,cache:None})
}

fn read(device:&wgpu::Device,queue:&wgpu::Queue,sky:&mut super::Sky,params:&crate::ffi::WgrSky,
        custom:Option<&wgpu::RenderPipeline>,format:wgpu::TextureFormat)->Vec<[f32;4]> {
    let size=wgpu::Extent3d{width:super::ENV_W,height:super::ENV_H,depth_or_array_layers:1};
    let tex=device.create_texture(&wgpu::TextureDescriptor{label:Some("env_probe_actual_linear"),size,mip_level_count:1,sample_count:1,
        dimension:wgpu::TextureDimension::D2,format,usage:wgpu::TextureUsages::RENDER_ATTACHMENT|wgpu::TextureUsages::COPY_SRC,view_formats:&[]});
    let view=tex.create_view(&Default::default());let pixel=if format==wgpu::TextureFormat::Rgba32Float {16}else{8};
    let row=super::ENV_W*pixel;
    let buffer=device.create_buffer(&wgpu::BufferDescriptor{label:None,size:u64::from(row*super::ENV_H),
        usage:wgpu::BufferUsages::COPY_DST|wgpu::BufferUsages::MAP_READ,mapped_at_creation:false});
    sky.upload(queue,params,glam::Mat4::IDENTITY.to_cols_array_2d(),[6423.81006,142.307526,7175.56396,0.],
        &bytemuck::Zeroable::zeroed(),&bytemuck::Zeroable::zeroed());
    let mut encoder=device.create_command_encoder(&Default::default());sky.render_luts(&mut encoder);
    {let mut pass=encoder.begin_render_pass(&wgpu::RenderPassDescriptor{label:Some("actual_env_readback"),
        color_attachments:&[Some(wgpu::RenderPassColorAttachment{view:&view,depth_slice:None,resolve_target:None,
            ops:wgpu::Operations{load:wgpu::LoadOp::Clear(wgpu::Color::BLACK),store:wgpu::StoreOp::Store}})],
        depth_stencil_attachment:None,timestamp_writes:None,occlusion_query_set:None,multiview_mask:None});
        pass.set_pipeline(custom.unwrap_or(&sky.env_pipeline));pass.set_bind_group(0,&sky.sky_bind,&[]);pass.draw(0..3,0..1);}
    encoder.copy_texture_to_buffer(wgpu::TexelCopyTextureInfo{texture:&tex,mip_level:0,origin:wgpu::Origin3d::ZERO,aspect:wgpu::TextureAspect::All},
        wgpu::TexelCopyBufferInfo{buffer:&buffer,layout:wgpu::TexelCopyBufferLayout{offset:0,bytes_per_row:Some(row),rows_per_image:Some(super::ENV_H)}},size);
    queue.submit([encoder.finish()]);let(tx,rx)=std::sync::mpsc::channel();
    buffer.slice(..).map_async(wgpu::MapMode::Read,move|r|tx.send(r).unwrap());
    device.poll(wgpu::PollType::wait_indefinitely()).unwrap();rx.recv().unwrap().unwrap();
    let bytes=buffer.slice(..).get_mapped_range();let mut result=Vec::new();
    for (x,y) in SAMPLES {let start=((y*super::ENV_W+x)*pixel) as usize;let mut rgba=[0.;4];
        for c in 0..4 {rgba[c]=if pixel==16 {f32::from_le_bytes(bytes[start+c*4..start+c*4+4].try_into().unwrap())}
            else{half(u16::from_le_bytes(bytes[start+c*2..start+c*2+2].try_into().unwrap()))};}result.push(rgba);}
    drop(bytes);buffer.unmap();result
}

#[test]
fn actual_gpu_sky_env_upward_horizon_downward_and_full_budget_control() {
    let(device,queue)=crate::gfx3d::cull::tests::headless().expect("Env optics requires actual device");
    let mut sky=super::Sky::new(&device,&queue,crate::HDR_FORMAT,1);
    let full=pipeline(&device,&sky,&control_source(false),"fs_sky_env",super::ENV_FORMAT);
    let interval=pipeline(&device,&sky,&control_source(true),"fs_env_interval_probe",wgpu::TextureFormat::Rgba32Float);
    let mut params=crate::ffi::WgrSky::default();
    params.sun_dir=[0.,0.59081435,0.8068076,22.];params.night_zenith[3]=142.307526;
    params.fog_color[3]=0.; // Isolate atmosphere/cloud direction, no horizon grade.
    for coverage in [0.,0.95] {
        params.cloud0[0]=coverage;
        let cheap=read(&device,&queue,&mut sky,&params,None,super::ENV_FORMAT);
        let dense=read(&device,&queue,&mut sky,&params,Some(&full),super::ENV_FORMAT);
        let gates=read(&device,&queue,&mut sky,&params,Some(&interval),wgpu::TextureFormat::Rgba32Float);
        for i in 0..SAMPLES.len() {
            assert!(cheap[i].iter().chain(dense[i].iter()).all(|v|v.is_finite()&&*v>=0.));
            assert!((0. ..=1.).contains(&gates[i][1]));
            let expected_y=((SAMPLES[i].1 as f64+0.5)/super::ENV_H as f64*std::f64::consts::PI).cos();
            assert!((gates[i][0] as f64-expected_y).abs()<0.00001,"Actual env UV direction at {i}");
            if coverage==0. {assert_eq!(cheap[i],dense[i],"Cloud-free original/full budgets must agree at {i}");}
            if i>=3 {assert!(gates[i][3]<=gates[i][2],"Planet must refuse downward deck ray at {i}");assert_eq!(gates[i][1],1.);}
            eprintln!("WET_SOIL_ENV_PROBE coverage={coverage} texel={:?} actualRayY={} cloudTrans={} interval={:?} env12={:?} env128={:?} scope=real-production-sky-pipeline-representative-settings-not-installed-frame",SAMPLES[i],gates[i][0],gates[i][1],&gates[i][2..],cheap[i],dense[i]);
        }
    }
}
