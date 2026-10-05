//! Finite ASL fog. The old atmospheric volume and fog closure stay independent.
use crate::{ffi::{WgrCamera, WgrCameraShadow, WgrLayeredFog, WgrLight}, terrain::{TerrainConformParams, TerrainShadowMap}};
use bytemuck::{Pod, Zeroable};

pub const SIZE: u32 = 32;
const VIEW_SIZE: u32 = 64;
const MAX_LOCAL_LIGHTS: usize = 16;

// Aerial views must resolve the whole finite world fog field, even when weather
// has shortened geometry visibility. Stopping at the geometry fog sphere cuts a
// ground slab halfway and stamps that moving circle into synthetic sky closure.
// Ground-level cameras retain the existing range. Blend across the actual upper
// slab feather; incoming solar resolution/scene culling retain their own range.
fn view_transport_domain(far:f32,cam:&WgrCamera,settings:&WgrLayeredFog,height:&TerrainConformParams,bounds:[f32;2])->[f32;3] {
    if height.hm_width<2 || height.hm_height<2 || !height.terrain_grid.is_finite() || height.terrain_grid<=0.0{return [far,0.,0.];}
    let active=||settings.layers.iter().filter(|l|l[3]>0.0);
    if active().next().is_none(){return [far,0.,0.];}
    let follow=settings.terrain[0];let reference=settings.terrain[1];
    let top=active().map(|l|l[1]+l[2]).fold(f32::NEG_INFINITY,f32::max)+follow*(bounds[1]-reference);
    let bottom=active().map(|l|l[0]-l[2]).fold(f32::INFINITY,f32::min)+follow*(bounds[0]-reference);
    let feather=active().map(|l|l[2]).fold(1.0,f32::max);
    let u=((cam.cam_pos[1]-top)/feather).clamp(0.0,1.0);
    if u<=0.0{return [far,bottom,0.];}
    let extent=glam::Vec2::new((height.hm_width-1) as f32,(height.hm_height-1) as f32)*height.terrain_grid;
    let eye=glam::Vec2::new(cam.cam_pos[0],cam.cam_pos[2]);
    let a=(eye-height.origin).abs();let b=(eye-height.origin-extent).abs();
    let xz=a.max(b);
    let field_distance=(xz.length_squared()+(cam.cam_pos[1]-bottom).powi(2)).sqrt();
    let blend=u*u*(3.0-2.0*u);
    [far+(field_distance.max(far)-far)*blend,bottom,blend]
}

// Keep actual light metadata (including shadow slots) and reject malformed input
// before it reaches a compute shader. The cap bounds the cost per fog participant;
// prioritize incident-energy bounds so the nearby player's torch survives a town.
fn local_lights(lights: &[WgrLight], camera: [f32;4], far: f32) -> Vec<WgrLight> {
    if !camera.iter().all(|v|v.is_finite()) || !far.is_finite() || far<=0.0 {return Vec::new();}
    let eye=glam::Vec3::from_array([camera[0],camera[1],camera[2]]);
    let mut candidates=Vec::new();
    for (index,input) in lights.iter().enumerate() {
        if !input.pos.iter().chain(input.diffuse.iter()).chain(input.ambient.iter()).chain(input.dir.iter()).all(|v|v.is_finite()) {continue;}
        let energy=input.diffuse[..3].iter().copied().fold(0.0f32,f32::max);
        if input.pos[3]<=0.0 || input.pos[3]>10000.0 || energy<=0.0 || energy>1e6 || input.dir[3].abs()>26.0 || input.diffuse[..3].iter().any(|v|*v<0.0) {continue;}
        let mut light=*input;
        let multiplier=if light.dir[3]<0.5 && light.dir[0]>0.0 {light.dir[0]}else{10.0};
        let reach=light.pos[3]*multiplier;
        let position=glam::Vec3::from_array([light.pos[0],light.pos[1],light.pos[2]]);
        let distance=position.distance(eye);
        if !reach.is_finite() || reach<=0.0 || reach>100000.0 || !distance.is_finite() || distance>far+reach {continue;}
        if light.dir[3]>0.5 {
            let direction=glam::Vec3::from_array([light.dir[0],light.dir[1],light.dir[2]]).normalize_or_zero();
            if direction.length_squared()<0.5 || light.diffuse[3]<0.0 || light.diffuse[3]>1.0 || light.ambient[3]<0.0 {continue;}
            light.dir[..3].copy_from_slice(&direction.to_array());
        }
        let score=energy*(light.pos[3]/distance.max(light.pos[3])).powi(2);
        candidates.push((index,score,light));
    }
    candidates.sort_by(|a,b|b.1.total_cmp(&a.1).then(a.0.cmp(&b.0)));
    candidates.into_iter().take(MAX_LOCAL_LIGHTS).map(|v|v.2).collect()
}
#[repr(C)]
#[derive(Clone, Copy, Pod, Zeroable)]
pub struct Consumer {
    pub view: [f32;16], pub proj: [f32;16], pub inv_vp: [[f32;4];4],
    pub camera: [f32;4], pub control: [f32;4],
    pub solar_domain:[f32;4],pub native:[f32;4],pub native_size:[u32;4],
}
#[repr(C)]
#[derive(Clone, Copy, Pod, Zeroable)]
struct Volume {
    inv_vp: [[f32;4];4], camera: [f32;4], control: [f32;4], layers: [[f32;4];2],
    light: [f32;4], radiance: [f32;4], cloud_map: [f32;4],
    near_vp: [[f32;4];4], far_vp: [[f32;4];4],
    near_ctl: [f32;4], far_ctl: [f32;4], capacity: [u32;4],
    terrain: [f32;4], patch: [f32;4], sky_limit: [f32;4], solar_domain: [f32;4],
    ocean: [f32;4],
}
pub struct WeatherInputs {
    pub views: [wgpu::TextureView;2], pub matrices: [[[f32;4];4];2],
    pub controls: [[f32;4];2], pub counts: [Option<wgpu::Buffer>;2], pub capacities: [u32;2],
}
pub fn valid_settings(p: &WgrLayeredFog) -> bool {
    p.control.iter().chain(p.layers.iter().flatten()).chain(p.terrain.iter()).chain(p.patch.iter()).all(|v|v.is_finite())
        && (0.0..=1.0).contains(&p.terrain[0])
        && (-1000.0..=10000.0).contains(&p.terrain[1])
        && (0.0..=3.0).contains(&p.terrain[2])
        && (p.terrain[2]==0.0 || (25.0..=1000.0).contains(&p.terrain[3]))
        && (0.0..=0.95).contains(&p.patch[0])
        && (p.patch[0]==0.0 || ((25.0..=2000.0).contains(&p.patch[1]) && (5.0..=1000.0).contains(&p.patch[2])))
        && (0.0..=2000.0).contains(&p.patch[3])
        && (0.0..=1.0).contains(&p.control[1]) && p.control[2].abs()<=0.95
        && p.layers.iter().all(|l| l[0]>=-1000.0 && l[1]<=10000.0 && l[1]>l[0]
            && l[2]>=0.001 && l[2]<=1000.0 && l[3]>=0.0 && l[3]<=0.05)
}
pub fn settings(mut p: WgrLayeredFog) -> WgrLayeredFog {
    // The C++ shared SkySettings receives startup environment overrides once.
    // Do not re-pin them here: the live Weather selector must be authoritative.
    if !valid_settings(&p) { p.control[0]=0.0; }
    p
}
pub fn requested(p: &WgrLayeredFog, weather: f32, hdr: bool, underground:f32) -> bool {
    valid_settings(p) && p.control[0]>=0.5 && weather.is_finite() && weather>0.0 && weather<=1.0
        && hdr && underground==0.0 && p.layers.iter().any(|l|l[3]>0.0)
}
fn solar_max_agl(settings:&WgrLayeredFog,native_min:f32)->f32 {
    let top=settings.layers.iter().filter(|l|l[3]>0.0).map(|l|l[1]+l[2]).fold(f32::MIN,f32::max);
    (top-settings.terrain[0]*settings.terrain[1]-(1.0-settings.terrain[0])*native_min).max(1.0)
}
pub struct LayeredFog {
    pub view: wgpu::TextureView, pub solar_view: wgpu::TextureView, pub consumer: wgpu::Buffer,
    _texture: wgpu::Texture, _solar_texture: wgpu::Texture, params: wgpu::Buffer, height: wgpu::Buffer,
    shadow: wgpu::Buffer, csm: wgpu::Buffer, zero_counts: wgpu::Buffer,
    local_lights: wgpu::Buffer, local_count: u32,
    sampler: wgpu::Sampler, comparison: wgpu::Sampler,
    layout: wgpu::BindGroupLayout, solar_layout: wgpu::BindGroupLayout, compute: wgpu::ComputePipeline, solar_compute: wgpu::ComputePipeline,
    background: wgpu::RenderPipeline, background_bind: wgpu::BindGroup,
    ready: bool, trace: bool, frames: u64,
}
impl LayeredFog {
    pub fn is_ready(&self)->bool { self.ready }
    pub fn new(device:&wgpu::Device, queue:&wgpu::Queue, composer:&mut naga_oil::compose::Composer,
               format:wgpu::TextureFormat, samples:u32)->Self {
        let buffer=|name,size| device.create_buffer(&wgpu::BufferDescriptor {label:Some(name),size,
            usage:wgpu::BufferUsages::UNIFORM|wgpu::BufferUsages::COPY_DST,mapped_at_creation:false});
        let params=buffer("wgr_layer_fog_params",std::mem::size_of::<Volume>() as u64);
        let consumer=buffer("wgr_layer_fog_consumer",std::mem::size_of::<Consumer>() as u64);
        queue.write_buffer(&consumer,0,bytemuck::bytes_of(&Consumer::zeroed()));
        let height=buffer("wgr_layer_fog_height",std::mem::size_of::<TerrainConformParams>() as u64);
        let shadow=buffer("wgr_layer_fog_shadow",std::mem::size_of::<TerrainShadowMap>() as u64);
        let csm=buffer("wgr_layer_fog_csm",std::mem::size_of::<WgrCameraShadow>() as u64);
        let zero_counts=device.create_buffer(&wgpu::BufferDescriptor {label:Some("wgr_layer_fog_no_retained_counts"),size:16,
            usage:wgpu::BufferUsages::STORAGE,mapped_at_creation:false});
        let local_lights=device.create_buffer(&wgpu::BufferDescriptor {label:Some("wgr_layer_fog_local_lights"),
            size:(MAX_LOCAL_LIGHTS*std::mem::size_of::<WgrLight>()) as u64,
            usage:wgpu::BufferUsages::STORAGE|wgpu::BufferUsages::COPY_DST,mapped_at_creation:false});
        let texture=device.create_texture(&wgpu::TextureDescriptor {label:Some("wgr_layer_fog_transport"),
            size:wgpu::Extent3d {width:VIEW_SIZE,height:VIEW_SIZE,depth_or_array_layers:SIZE},mip_level_count:1,sample_count:1,
            dimension:wgpu::TextureDimension::D3,format:wgpu::TextureFormat::Rgba16Float,
            usage:wgpu::TextureUsages::STORAGE_BINDING|wgpu::TextureUsages::TEXTURE_BINDING,view_formats:&[]});
        let view=texture.create_view(&Default::default());
        let solar_texture=device.create_texture(&wgpu::TextureDescriptor {label:Some("wgr_layer_fog_solar"),
            size:wgpu::Extent3d {width:SIZE,height:SIZE,depth_or_array_layers:SIZE},mip_level_count:1,sample_count:1,
            dimension:wgpu::TextureDimension::D3,format:wgpu::TextureFormat::Rgba16Float,
            usage:wgpu::TextureUsages::STORAGE_BINDING|wgpu::TextureUsages::TEXTURE_BINDING,view_formats:&[]});
        let solar_view=solar_texture.create_view(&Default::default());
        let sampler=device.create_sampler(&wgpu::SamplerDescriptor {label:Some("wgr_layer_fog_linear"),
            mag_filter:wgpu::FilterMode::Linear,min_filter:wgpu::FilterMode::Linear,..Default::default()});
        let comparison=device.create_sampler(&wgpu::SamplerDescriptor {label:Some("wgr_layer_fog_depth"),
            compare:Some(wgpu::CompareFunction::LessEqual),
            mag_filter:wgpu::FilterMode::Linear,min_filter:wgpu::FilterMode::Linear,..Default::default()});
        let uniform=|binding|wgpu::BindGroupLayoutEntry {binding,visibility:wgpu::ShaderStages::COMPUTE,
            ty:wgpu::BindingType::Buffer {ty:wgpu::BufferBindingType::Uniform,has_dynamic_offset:false,min_binding_size:None},count:None};
        let image=|binding,dimension,kind|wgpu::BindGroupLayoutEntry {binding,visibility:wgpu::ShaderStages::COMPUTE,
            ty:wgpu::BindingType::Texture {sample_type:kind,view_dimension:dimension,multisampled:false},count:None};
        let storage=|binding|wgpu::BindGroupLayoutEntry {binding,visibility:wgpu::ShaderStages::COMPUTE,
            ty:wgpu::BindingType::Buffer {ty:wgpu::BufferBindingType::Storage {read_only:true},has_dynamic_offset:false,min_binding_size:None},count:None};
        let d2=wgpu::TextureViewDimension::D2;
        let float=wgpu::TextureSampleType::Float {filterable:true};
        let depth=wgpu::TextureSampleType::Depth;
        let entries=vec![
            uniform(0),wgpu::BindGroupLayoutEntry {binding:1,visibility:wgpu::ShaderStages::COMPUTE,
                ty:wgpu::BindingType::StorageTexture {access:wgpu::StorageTextureAccess::WriteOnly,format:wgpu::TextureFormat::Rgba16Float,view_dimension:wgpu::TextureViewDimension::D3},count:None},
            wgpu::BindGroupLayoutEntry {binding:2,visibility:wgpu::ShaderStages::COMPUTE,ty:wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Filtering),count:None},
            image(3,d2,float),image(4,d2,float),image(5,d2,float),uniform(6),image(7,wgpu::TextureViewDimension::D2Array,depth),
            wgpu::BindGroupLayoutEntry {binding:8,visibility:wgpu::ShaderStages::COMPUTE,ty:wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Comparison),count:None},
            uniform(9),image(10,d2,wgpu::TextureSampleType::Float {filterable:false}),uniform(11),
            image(12,d2,depth),image(13,d2,depth),storage(14),storage(15),
            storage(17),storage(19),wgpu::BindGroupLayoutEntry {binding:16,visibility:wgpu::ShaderStages::COMPUTE,
                ty:wgpu::BindingType::StorageTexture {access:wgpu::StorageTextureAccess::WriteOnly,format:wgpu::TextureFormat::Rgba16Float,view_dimension:wgpu::TextureViewDimension::D3},count:None}];
        let solar_layout=device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {label:Some("wgr_layer_fog_solar_layout"),entries:&entries});
        let mut view_entries:Vec<_>=entries.into_iter().filter(|e|e.binding!=16).collect();
        view_entries.push(image(18,wgpu::TextureViewDimension::D3,float));
        let layout=device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {label:Some("wgr_layer_fog_compute_layout"),entries:&view_entries});
        let module=crate::shaders::make_module(device,composer,"wgr_layer_fog",include_str!("layered_fog.wgsl"),"layered_fog.wgsl");
        let pipeline_layout=device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {label:Some("wgr_layer_fog_compute"),bind_group_layouts:&[Some(&layout)],immediate_size:0});
        let compute=device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {label:Some("wgr_layer_fog_compute"),layout:Some(&pipeline_layout),module:&module,
            entry_point:Some("cs_fog"),compilation_options:Default::default(),cache:None});
        let solar_pipeline_layout=device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {label:Some("wgr_layer_fog_solar_compute"),bind_group_layouts:&[Some(&solar_layout)],immediate_size:0});
        let solar_compute=device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {label:Some("wgr_layer_fog_solar_compute"),layout:Some(&solar_pipeline_layout),module:&module,
            entry_point:Some("cs_solar"),compilation_options:Default::default(),cache:None});
        let background_module=crate::shaders::make_module(device,composer,"wgr_layer_fog_background",include_str!("layered_fog_background.wgsl"),"layered_fog_background.wgsl");
        let background=device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {label:Some("wgr_layer_fog_background"),layout:None,
            vertex:wgpu::VertexState {module:&background_module,entry_point:Some("vs_main"),compilation_options:Default::default(),buffers:&[]},
            fragment:Some(wgpu::FragmentState {module:&background_module,entry_point:Some("fs_main"),compilation_options:Default::default(),targets:&[Some(wgpu::ColorTargetState {format,
                blend:Some(wgpu::BlendState {color:wgpu::BlendComponent {src_factor:wgpu::BlendFactor::One,dst_factor:wgpu::BlendFactor::SrcAlpha,operation:wgpu::BlendOperation::Add},alpha:wgpu::BlendComponent {src_factor:wgpu::BlendFactor::Zero,dst_factor:wgpu::BlendFactor::One,operation:wgpu::BlendOperation::Add}}),write_mask:wgpu::ColorWrites::ALL})]}),
            primitive:Default::default(),depth_stencil:None,multisample:wgpu::MultisampleState {count:samples,..Default::default()},multiview_mask:None,cache:None});
        let background_bind=device.create_bind_group(&wgpu::BindGroupDescriptor {label:Some("wgr_layer_fog_background_bind"),layout:&background.get_bind_group_layout(0),entries:&[
            wgpu::BindGroupEntry {binding:0,resource:wgpu::BindingResource::TextureView(&view)},
            wgpu::BindGroupEntry {binding:1,resource:wgpu::BindingResource::Sampler(&sampler)},
            wgpu::BindGroupEntry {binding:2,resource:consumer.as_entire_binding()}]});
        Self {view,solar_view,consumer,_texture:texture,_solar_texture:solar_texture,params,height,shadow,csm,zero_counts,local_lights,local_count:0,sampler,comparison,layout,solar_layout,compute,solar_compute,background,background_bind,
            ready:false,trace:std::env::var("WGR_LAYERED_FOG_TRACE").as_deref()==Ok("1"),frames:0}
    }
    pub fn invalidate(&mut self,queue:&wgpu::Queue) {
        if self.ready {queue.write_buffer(&self.consumer,0,bytemuck::bytes_of(&Consumer::zeroed()));}
        self.ready=false;
    }
    // Call once per frame with the SAME real lighting packet as surface rendering.
    // An empty packet clears the count; stale lamps cannot survive a scene change.
    pub fn upload_local_lights(&mut self,queue:&wgpu::Queue,lights:&[WgrLight],cam:&WgrCamera) {
        let far=cam.params.fog_start+if cam.params.fog_inv_range>0.0 {1.0/cam.params.fog_inv_range}else{0.0};
        // Surface rendering sees the per-camera count, capped by its 256 slots.
        let count=if cam.cam_pos[3].is_finite() {cam.cam_pos[3].clamp(0.0,256.0) as usize}else{0};
        let selected=local_lights(&lights[..lights.len().min(count)],cam.cam_pos,far);
        self.local_count=selected.len() as u32;
        if !selected.is_empty() {queue.write_buffer(&self.local_lights,0,bytemuck::cast_slice(&selected));}
    }
    #[allow(clippy::too_many_arguments)]
    pub fn render(&mut self,device:&wgpu::Device,queue:&wgpu::Queue,encoder:&mut wgpu::CommandEncoder,
        settings:&WgrLayeredFog,weather:f32,hdr:bool,underground:f32,cam:&WgrCamera,inv_vp:[[f32;4];4],
        height:&TerrainConformParams,height_view:&wgpu::TextureView,height_generation:u64,
        mapping:&TerrainShadowMap,shadow_view:&wgpu::TextureView,csm_view:&wgpu::TextureView,
        environment:&wgpu::TextureView,sky_sh:&wgpu::Buffer,clouds:&wgpu::TextureView,cover:WeatherInputs,below_clouds:bool,cloud_limit:[f32;2],native_bounds:[f32;2],ocean:Option<f32>) {
        self.frames=self.frames.wrapping_add(1);
        let far=cam.params.fog_start+if cam.params.fog_inv_range>0.0 {1.0/cam.params.fog_inv_range}else{0.0};
        let finite=cam.view.iter().chain(cam.proj.iter()).chain(cam.cam_pos.iter())
            .chain(inv_vp.iter().flatten()).chain(cam.sun_diffuse.iter()).chain(cam.sun_dir_world.iter()).all(|v|v.is_finite());
        let eligible=requested(settings,weather,hdr,underground) && finite && far.is_finite() && far>0.0
            && height.enabled>0.5 && height_generation>0
            && cloud_limit.iter().all(|v|v.is_finite()) && native_bounds.iter().all(|h|h.is_finite()) && native_bounds[0]<=native_bounds[1]
            && (below_clouds || settings.terrain[0]>0.0);
        if self.trace && (self.frames<=4 || self.frames%120==0) {
            eprintln!("[wgr] layered fog: frame={} requested={} encoded={} weather={:.6} far={:.3} heightGen={} nearReady={} farReady={} camera={:?} layers={:?} terrain={:?} patch={:?} cloud={:?}",
                self.frames,requested(settings,weather,hdr,underground),eligible,weather,far,height_generation,
                cover.controls[0][0],cover.controls[1][0],cam.cam_pos,settings.layers,settings.terrain,settings.patch,cloud_limit);
        }
        if !eligible {return;}
        let ocean=ocean.filter(|v|v.is_finite());
        // Atmospheric terrain follows the water surface, not its seabed.
        let air_bounds=match ocean {Some(level)=>[native_bounds[0].max(level),native_bounds[1].max(level)],None=>native_bounds};
        let [view_far,ray_floor,aerial_blend]=view_transport_domain(far,cam,settings,height,air_bounds);
        let direction=glam::Vec3::new(-cam.sun_dir_world[0],-cam.sun_dir_world[1],-cam.sun_dir_world[2]).normalize_or_zero();
        // A dark/absent directional light does not remove the night air medium.
        // The incoming solar field alone becomes zero; local lamps remain active.
        let radiance=if direction.y>0.001 && cam.sun_diffuse[3]>=0.5 {cam.sun_diffuse}else{[0.0;4]};
        // Snap the incoming-light grid in world space. Camera motion below one
        // cell no longer re-samples canopy/density at unrelated world positions.
        let solar_cell=2.0*far/(SIZE as f32-2.0);
        let solar_width=solar_cell*SIZE as f32;
        let solar_domain=[(cam.cam_pos[0]/solar_cell).floor()*solar_cell-solar_width*0.5,
            (cam.cam_pos[2]/solar_cell).floor()*solar_cell-solar_width*0.5,
            1.0/solar_width,solar_max_agl(settings,native_bounds[0])];
        let data=Volume {inv_vp,camera:[cam.cam_pos[0],cam.cam_pos[1],cam.cam_pos[2],view_far],
            control:[settings.control[1],settings.control[2],weather,1.0],layers:settings.layers,
            light:[direction.x,direction.y,direction.z,0.0],radiance:[radiance[0],radiance[1],radiance[2],far],cloud_map:mapping.cloud_shadow.to_array(),
            near_vp:cover.matrices[0],far_vp:cover.matrices[1],near_ctl:cover.controls[0],far_ctl:cover.controls[1],
            capacity:[cover.capacities[0],cover.capacities[1],self.local_count,0],terrain:settings.terrain,patch:settings.patch,
            sky_limit:[cloud_limit[0],cloud_limit[1],air_bounds[1],air_bounds[0]],solar_domain,
            ocean:match ocean.filter(|v|v.is_finite()){Some(level)=>[level,1.,aerial_blend,ray_floor],None=>[0.,0.,aerial_blend,ray_floor]}};
        queue.write_buffer(&self.params,0,bytemuck::bytes_of(&data));
        queue.write_buffer(&self.height,0,bytemuck::bytes_of(height));
        queue.write_buffer(&self.shadow,0,bytemuck::bytes_of(mapping));
        queue.write_buffer(&self.csm,0,bytemuck::bytes_of(&cam.shadow));
        fn image(binding:u32,view:&wgpu::TextureView)->wgpu::BindGroupEntry<'_> {
            wgpu::BindGroupEntry {binding,resource:wgpu::BindingResource::TextureView(view)}
        }
        fn buffer(binding:u32,buffer:&wgpu::Buffer)->wgpu::BindGroupEntry<'_> {
            wgpu::BindGroupEntry {binding,resource:buffer.as_entire_binding()}
        }
        let entries=vec![
            buffer(0,&self.params),image(1,&self.view),wgpu::BindGroupEntry {binding:2,resource:wgpu::BindingResource::Sampler(&self.sampler)},
            image(3,environment),image(4,clouds),image(5,shadow_view),buffer(6,&self.shadow),image(7,csm_view),
            wgpu::BindGroupEntry {binding:8,resource:wgpu::BindingResource::Sampler(&self.comparison)},buffer(9,&self.csm),
            image(10,height_view),buffer(11,&self.height),image(12,&cover.views[0]),image(13,&cover.views[1]),
            buffer(14,cover.counts[0].as_ref().unwrap_or(&self.zero_counts)),buffer(15,cover.counts[1].as_ref().unwrap_or(&self.zero_counts)),image(16,&self.solar_view),buffer(17,&self.local_lights),buffer(19,sky_sh)];
        let solar_bind=device.create_bind_group(&wgpu::BindGroupDescriptor {label:Some("wgr_layer_fog_solar_bind"),layout:&self.solar_layout,entries:&entries});
        let mut view_entries:Vec<_>=entries.into_iter().filter(|e|e.binding!=16).collect();
        view_entries.push(image(18,&self.solar_view));
        let bind=device.create_bind_group(&wgpu::BindGroupDescriptor {label:Some("wgr_layer_fog_compute_bind"),layout:&self.layout,entries:&view_entries});
        encoder.push_debug_group("wgr_layered_fog_transport");
        {let mut pass=encoder.begin_compute_pass(&wgpu::ComputePassDescriptor {label:Some("wgr_layered_fog_transport"),timestamp_writes:None});
            pass.set_pipeline(&self.solar_compute);pass.set_bind_group(0,&solar_bind,&[]);pass.dispatch_workgroups(SIZE.div_ceil(8),SIZE.div_ceil(8),SIZE);}
        {let mut pass=encoder.begin_compute_pass(&wgpu::ComputePassDescriptor {label:Some("wgr_layer_fog_view_transport"),timestamp_writes:None});
            pass.set_pipeline(&self.compute);pass.set_bind_group(0,&bind,&[]);pass.dispatch_workgroups(VIEW_SIZE.div_ceil(8),VIEW_SIZE.div_ceil(8),1);}
        encoder.pop_debug_group();
        let consumer=Consumer {view:cam.view,proj:cam.proj,inv_vp,camera:[cam.cam_pos[0],cam.cam_pos[1],cam.cam_pos[2],ray_floor],control:[1.0,view_far,weather,aerial_blend],
            solar_domain,
            native:[height.origin.x,height.origin.y,height.terrain_grid,height.enabled],
            native_size:[height.hm_width,height.hm_height,0,0]};
        queue.write_buffer(&self.consumer,0,bytemuck::bytes_of(&consumer));self.ready=true;
    }
    pub fn render_background(&self,pass:&mut wgpu::RenderPass<'_>) {
        if self.ready {pass.set_pipeline(&self.background);pass.set_bind_group(0,&self.background_bind,&[]);pass.draw(0..3,0..1);}
    }
}

#[cfg(test)]
#[path="layered_fog_tests.rs"]
mod tests;

#[cfg(test)]
mod local_light_tests {
    use super::*;
    fn point(x:f32)->WgrLight {WgrLight {pos:[x,2.,0.,1.],diffuse:[1.,1.,1.,0.],ambient:[0.;4],dir:[10.,0.,0.,0.]}}
    #[test]
    fn actual_light_packet_keeps_shadow_slots_and_rejects_invalid_emitters() {
        let mut spot=point(2.);spot.dir=[0.,0.,2.,7.];spot.diffuse[3]=0.98;spot.ambient[3]=0.8;
        let mut cube=point(3.);cube.dir[3]=-7.;
        let mut invalid=point(1.);invalid.pos[0]=f32::NAN;
        let mut zero=point(1.);zero.pos[3]=0.;
        let mut bad_axis=spot;bad_axis.dir[..3].copy_from_slice(&[0.;3]);
        let lights=local_lights(&[spot,cube,invalid,zero,bad_axis,point(10000.)],[0.;4],100.);
        assert_eq!(lights.len(),2);assert_eq!(lights[0].dir,[0.,0.,1.,7.]);
        assert_eq!(lights[0].diffuse,spot.diffuse);assert_eq!(lights[0].ambient,spot.ambient);
        assert_eq!(lights[1].dir,cube.dir);
        assert!(local_lights(&[spot],[0.;4],f32::NAN).is_empty());
        assert!(local_lights(&[],[0.;4],100.).is_empty());
    }
    #[test]
    fn bounded_energy_selection_retains_near_torch_and_native_point_range() {
        let mut lights:Vec<_>=(0..32).map(|n|point(20.+n as f32)).collect();
        let mut torch=point(0.);torch.dir=[0.,0.,1.,1.];torch.diffuse[3]=0.98;lights.push(torch);
        let chosen=local_lights(&lights,[0.;4],100.);assert_eq!(chosen.len(),MAX_LOCAL_LIGHTS);
        assert_eq!(chosen[0].dir,torch.dir);
        let mut long_range=point(150.);long_range.dir[0]=100.;
        assert_eq!(local_lights(&[long_range],[0.;4],100.).len(),1);
        assert!(local_lights(&[point(150.)],[0.;4],100.).is_empty());
    }
}
