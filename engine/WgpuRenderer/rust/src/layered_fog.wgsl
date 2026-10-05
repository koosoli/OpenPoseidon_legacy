#import layered_fog_optics::{fog_segment_density, fog_segment_participant, fog_air_interval, fog_height_density, fog_sky_ambient, fog_height_primitive, fog_phase, fog_transmittance, fog_ray_direction, fog_ray_far, fog_patch_density, fog_valley_density, fog_domain_feather, fog_neutral_radiance}
#import color::srgb_to_linear

struct FogVolume {
    inv_vp: mat4x4<f32>,
    camera: vec4<f32>, // absolute xyz, actual far distance
    control: vec4<f32>, // scattering albedo, HG g, weather amount, ready
    layers: array<vec4<f32>, 2>,
    light: vec4<f32>, // direction toward the actual main directional light
    radiance: vec4<f32>, // scene-linear directional irradiance, original solar path range
    cloud_map: vec4<f32>,
    near_vp: mat4x4<f32>,
    far_vp: mat4x4<f32>,
    near_ctl: vec4<f32>,
    far_ctl: vec4<f32>,
    capacity: vec4<u32>, // actual retained argument capacity near/far, local light count
    terrain: vec4<f32>, // follow fraction, reference ASL, valley gain, radius metres
    patchiness: vec4<f32>, // strength, horizontal/vertical metres, outer coverage feather metres
    sky_limit: vec4<f32>, // actual cloud enabled/base ASL, cached native max/min
    solar_domain: vec4<f32>, // snapped world XZ, inverse width, max AGL
    ocean: vec4<f32>, // actual producer sea level, current ocean nodes present
};
struct FogHeight {
    origin: vec2<f32>, grid: f32, enabled: f32,
    width: u32, height: u32, base: f32, ratio: f32,
    morph: f32, mode: f32, pad: vec2<f32>,
};
struct FogShadow { lanes: array<vec4<f32>, 4>, };
struct FogCsm {
    vp: array<mat4x4<f32>,4>, splits: vec4<f32>, radius: vec4<f32>,
    ctl: vec4<f32>, ctlb: vec4<f32>, forward: vec4<f32>, sun: vec4<f32>,
    local_vp: array<mat4x4<f32>,24>, local_ctl: vec4<f32>,
};
struct FogLocalLight { pos:vec4<f32>, diffuse:vec4<f32>, ambient:vec4<f32>, dir:vec4<f32>, };
@group(0) @binding(0) var<uniform> params: FogVolume;
@group(0) @binding(1) var result: texture_storage_3d<rgba16float, write>;
@group(0) @binding(2) var filter_samp: sampler;
@group(0) @binding(3) var environment: texture_2d<f32>;
@group(0) @binding(4) var clouds: texture_2d<f32>;
@group(0) @binding(5) var terrain_shadow: texture_2d<f32>;
@group(0) @binding(6) var<uniform> shadow: FogShadow;
@group(0) @binding(7) var cascades: texture_depth_2d_array;
@group(0) @binding(8) var depth_samp: sampler_comparison;
@group(0) @binding(9) var<uniform> csm: FogCsm;
@group(0) @binding(10) var native_height: texture_2d<f32>;
@group(0) @binding(11) var<uniform> height: FogHeight;
@group(0) @binding(12) var near_depth: texture_depth_2d;
@group(0) @binding(13) var far_depth: texture_depth_2d;
@group(0) @binding(14) var<storage, read> near_counts: array<u32>;
@group(0) @binding(15) var<storage, read> far_counts: array<u32>;
@group(0) @binding(16) var solar_result: texture_storage_3d<rgba16float, write>;
@group(0) @binding(17) var<storage, read> local_lights:array<FogLocalLight>;
@group(0) @binding(18) var solar_read: texture_3d<f32>;
struct FogSkySh { c: array<vec4<f32>,18>, };
@group(0) @binding(19) var<storage,read> sky_sh: FogSkySh;

fn fog_weather_weight(p: vec3<f32>) -> f32 {
    // The native height source owns the atmospheric domain. The camera-centred
    // rain maps only exclude known shelter; absent/outside/overflowed shelter
    // data must not carve a moving clear rectangle out of outdoor fog.
    let near_clip = params.near_vp * vec4<f32>(p, 1.0);
    let near_uv = near_clip.xy * vec2<f32>(0.5,-0.5) + 0.5;
    let edge = min(min(near_uv.x,1.0-near_uv.x),min(near_uv.y,1.0-near_uv.y));
    // Only current-frame shelter depths are sampled. The far depth map can
    // cover a near-map rebuild; stale/overflowed data never invents shelter.
    let outer = params.near_ctl.x < 0.5 || near_clip.z < 0.0 || near_clip.z > 1.0 || edge < params.near_ctl.z;
    var clip = near_clip;
    var ctl = params.near_ctl;
    var cap = params.capacity.x;
    if (outer) { clip = params.far_vp * vec4<f32>(p,1.0); ctl = params.far_ctl; cap = params.capacity.y; }
    if (ctl.x < 0.5 || clip.z < 0.0 || clip.z > 1.0) { return 1.0; }
    if (cap > 0u) {
        if (outer) {
            if (far_counts[0] > cap || far_counts[1] > cap || far_counts[2] > 2u*cap) { return 1.0; }
        } else {
            if (near_counts[0] > cap || near_counts[1] > cap || near_counts[2] > 2u*cap) { return 1.0; }
        }
    }
    let uv = clip.xy * vec2<f32>(0.5,-0.5) + 0.5;
    if (min(min(uv.x,1.0-uv.x),min(uv.y,1.0-uv.y)) < ctl.z) { return 1.0; }
    var exposed = 0.0;
    if (outer) { exposed = textureSampleCompareLevel(far_depth,depth_samp,uv,clip.z-ctl.y); }
    else { exposed = textureSampleCompareLevel(near_depth,depth_samp,uv,clip.z-ctl.y); }
    if (!outer) { return exposed; } // never feather a near/far handoff
    let map_scale = max(length(vec2<f32>(params.far_vp[0].x,params.far_vp[2].x)),
        length(vec2<f32>(params.far_vp[0].y,params.far_vp[2].y)))*0.5;
    let confidence=fog_domain_feather((min(min(uv.x,1.0-uv.x),min(uv.y,1.0-uv.y))-ctl.z)/max(map_scale,1e-10),params.patchiness.w);
    return mix(1.0,exposed,confidence);
}
// Return actual native triangle height, certified flag and distance inside domain.
fn fog_native_ground(xz: vec2<f32>) -> vec3<f32> {
    if (height.enabled < 0.5 || height.grid <= 0.0 || height.width < 2u || height.height < 2u) { return vec3<f32>(0.0); }
    let cell = (xz-height.origin)/height.grid;
    let extent = vec2<f32>(f32(height.width-1u),f32(height.height-1u));
    if (any(cell < vec2<f32>(0.0)) || any(cell >= extent)) {
        // The real water producer draws off-map ocean. Native terrain absence
        // cannot carve a camera-visible atmospheric hole over that same plane.
        if(params.ocean.y>0.5){return vec3<f32>(params.ocean.x,1.0,max(params.patchiness.w,1.0));}
        return vec3<f32>(0.0);
    }
    let ij=vec2<i32>(floor(cell)); let q=fract(cell);
    let a=textureLoad(native_height,ij,0).r; let b=textureLoad(native_height,ij+vec2<i32>(1,0),0).r;
    let c=textureLoad(native_height,ij+vec2<i32>(0,1),0).r; let d=textureLoad(native_height,ij+vec2<i32>(1,1),0).r;
    let ground=select(b+c-d+(d-c)*q.x+(d-b)*q.y,a+(b-a)*q.x+(c-a)*q.y,q.x+q.y<=1.0);
    if(params.ocean.y>0.5 && ground<=params.ocean.x){
        // Mist sits above water, not the buried seabed. No atmospheric fog below
        // the mean surface; submerged rendering retains its separate transport.
        return vec3<f32>(params.ocean.x,1.0,max(params.patchiness.w,1.0));
    }
    return vec3<f32>(ground,1.0,min(min(cell.x,extent.x-cell.x),min(cell.y,extent.y-cell.y))*height.grid);
}
fn fog_outdoor_sample(p: vec3<f32>) -> vec2<f32> {
    let native=fog_native_ground(p.xz);
    if (native.y < 0.5 || p.y <= native.x+0.05) { return vec2<f32>(native.x,0.0); }
    var weight=fog_weather_weight(p);
    weight *= fog_domain_feather(native.z,params.patchiness.w);
    // Terrain-following fog never enters the separately transported cloud deck.
    if (params.terrain.x > 0.0 && params.sky_limit.x > 0.0) {
        let feather=max(max(params.layers[0].z,params.layers[1].z),1.0);
        weight *= 1.0-smoothstep(params.sky_limit.y-feather,params.sky_limit.y,p.y);
    }
    return vec2<f32>(native.x,weight);
}
fn fog_pooling(p: vec3<f32>, ground: f32) -> f32 {
    if (params.terrain.z <= 0.0) { return 1.0; }
    let radius=params.terrain.w;
    let a=fog_native_ground(p.xz+vec2<f32>(radius,0.0));
    let b=fog_native_ground(p.xz-vec2<f32>(radius,0.0));
    let c=fog_native_ground(p.xz+vec2<f32>(0.0,radius));
    let d=fog_native_ground(p.xz-vec2<f32>(0.0,radius));
    if (min(min(a.y,b.y),min(c.y,d.y)) < 0.5) { return 1.0; }
    return fog_valley_density(ground,vec4<f32>(a.x,b.x,c.x,d.x),params.terrain.z,radius);
}
fn fog_light_visibility(p: vec3<f32>, rel: vec3<f32>) -> f32 {
    var visible = 1.0;
    if (shadow.lanes[1].z > 0.5) {
        let uv = (p.xz-shadow.lanes[0].xy)*shadow.lanes[0].zw+shadow.lanes[1].xy;
        if (all(uv >= vec2<f32>(0.0)) && all(uv <= vec2<f32>(1.0))) {
            let sm = textureSampleLevel(terrain_shadow,filter_samp,uv,0.0);
            visible *= 1.0-clamp(sm.b*(1.0-smoothstep(sm.r-sm.g,sm.r+sm.g+0.001,p.y)),0.0,1.0);
        }
    }
    let cloud_uv = (p.xz-params.cloud_map.xy)*params.cloud_map.z;
    if (params.cloud_map.w > 0.0 && all(cloud_uv>=vec2<f32>(0.0)) && all(cloud_uv<=vec2<f32>(1.0))) {
        visible *= mix(1.0,textureSampleLevel(clouds,filter_samp,cloud_uv,0.0).r,clamp(params.cloud_map.w,0.0,1.0));
    }
    let n = min(i32(csm.ctl.x),4);
    let eye = dot(rel,csm.forward.xyz);
    let radial = length(rel);
    for (var i = 0; i < n; i = i+1) {
        if (select(eye,radial,i<i32(csm.ctl.y)) > csm.splits[i]) { continue; }
        let cp = csm.vp[i]*vec4<f32>(rel,1.0);
        let sc = cp.xyz/cp.w;
        let uv = sc.xy*vec2<f32>(0.5,-0.5)+0.5;
        if (all(uv>vec2<f32>(0.0)) && all(uv<vec2<f32>(1.0)) && sc.z>0.0 && sc.z<1.0) {
            let lit = textureSampleCompareLevel(cascades,depth_samp,uv,i,sc.z-csm.ctl.w*f32((i+1)*(i+1)));
            let fade = clamp((csm.splits[n-1]-eye)/max(csm.ctl.z,0.001),0.0,1.0);
            visible *= mix(1.0,lit,fade);
        }
        break;
    }
    return visible;
}
// Integral for one certified native piecewise-linear segment. This is incoming
// light transport, independent of camera-path T; clouds are NOT multiplied here.
fn fog_sun_segment_tau(start: vec3<f32>, end: vec3<f32>, before: vec2<f32>, centre: vec2<f32>, after: vec2<f32>) -> f32 {
    if (min(min(before.y,centre.y),after.y) <= 0.0) { return 0.0; }
    let midpoint=(start+end)*0.5;
    let distance=length(end-start);let half=distance*0.5;
    let a=start.y-params.terrain.x*(before.x-params.terrain.y);
    let m=midpoint.y-params.terrain.x*(centre.x-params.terrain.y);
    let b=end.y-params.terrain.x*(after.x-params.terrain.y);
    var tau=0.0;
    for (var i=0u;i<2u;i=i+1u) {
        let layer=params.layers[i];
        tau += (fog_segment_density(a,(m-a)/half,half,layer)
            +fog_segment_density(m,(b-m)/half,half,layer))*layer.w*params.control.z;
    }
    if (tau <= 0.0) { return 0.0; }
    return tau*fog_patch_density(midpoint,params.patchiness)*fog_pooling(midpoint,centre.x)
        *min(min(before.y,centre.y),after.y);
}
// Rain interception by leaves does not remove the air carrying sunlight.
// Actual native terrain bounds this medium;
// object/surface CSM owns solid occlusion, view transport retains roof refusal.
fn fog_solar_air(p: vec3<f32>) -> vec2<f32> {
    let ground=fog_native_ground(p.xz);
    if(ground.y<0.5 || p.y<=ground.x+0.05){return vec2<f32>(ground.x,0.0);}
    return vec2<f32>(ground.x,fog_domain_feather(ground.z,params.patchiness.w));
}
fn fog_solar_transmission(p: vec3<f32>) -> f32 {
    if (params.light.y <= 0.001) { return 0.0; }
    let ground=fog_native_ground(p.xz);
    // Unknown/unready source and solid/underground endpoints are neutral for
    // surface lighting. Existing actual CSM/terrain visibility owns occlusion.
    let origin=fog_solar_air(p);
    if (ground.y < 0.5 || origin.y <= 0.0) { return 1.0; }
    if (params.terrain.x <= 0.0 && params.terrain.z <= 0.0 && params.patchiness.x <= 0.0 && params.patchiness.w <= 0.0) {
        var tau=0.0;
        for (var i=0u;i<2u;i=i+1u) {let layer=params.layers[i];
            tau += max(fog_height_primitive(layer.y+layer.z,layer)-fog_height_primitive(p.y,layer),0.0)
                *layer.w*params.control.z/params.light.y;
        }
        return fog_transmittance(tau);
    }
    // Native maximum is cached at authoritative height upload, not guessed
    // from camera elevation. Four bounded light-ray intervals retain analytic
    // thin-layer integration while sampling true terrain and coherent density.
    var top=-1e10;
    for (var i=0u;i<2u;i=i+1u) {if(params.layers[i].w>0.0){top=max(top,params.layers[i].y+params.layers[i].z);}}
    top += params.terrain.x*(params.sky_limit.z-params.terrain.y);
    if (params.terrain.x > 0.0 && params.sky_limit.x > 0.0) {top=min(top,params.sky_limit.y);}
    let distance=min(params.radiance.w,max((top-p.y)/params.light.y,0.0));
    if (distance <= 0.001) { return 1.0; }
    let step=distance*0.25;
    var tau=0.0;var before=origin;
    for (var i=0u;i<4u;i=i+1u) {
        let start=p+params.light.xyz*(f32(i)*step);
        let middle=start+params.light.xyz*(step*0.5);
        let end=start+params.light.xyz*step;
        let mid_ground=fog_native_ground(middle.xz);let end_ground=fog_native_ground(end.xz);
        if (mid_ground.y < 0.5 || end_ground.y < 0.5) { return 1.0; }
        // Real native obstruction blocks a shaft. No normal bending or fake
        // mountain reflection/deflection is introduced.
        if (middle.y <= mid_ground.x+0.05 || end.y <= end_ground.x+0.05) { return 0.0; }
        let centre=fog_solar_air(middle);let after=fog_solar_air(end);
        tau += fog_sun_segment_tau(start,end,before,centre,after);
        before=after;
    }
    return fog_transmittance(tau);
}
// Reuse the REAL local depth atlas and its authored slot encoding. Air has no
// surface normal to offset. A small perspective depth bias absorbs quantization;
// no shadow is invented for a light without a valid rendered slot.
fn fog_local_visibility(rel:vec3<f32>, slot:i32)->f32 {
    if(slot<0 || slot>=24 || !(csm.local_ctl.x>f32(slot)) || !(csm.local_ctl.z>0.0)) {return 1.0;}
    let clip=csm.local_vp[slot]*vec4<f32>(rel,1.0);
    if(!(clip.w>0.0) || !all(abs(clip)<vec4<f32>(1e30))) {return 1.0;}
    let ndc=clip.xyz/clip.w;
    if(any(abs(ndc.xy)>vec2<f32>(1.0)) || ndc.z<0.0 || ndc.z>1.0) {return 1.0;}
    let layer=i32(csm.local_ctl.w);
    if(layer<0 || layer>=i32(textureNumLayers(cascades))) {return 1.0;}
    let cell=vec2<f32>(f32(slot%8),f32(slot/8))*0.125;
    let texel=min(csm.local_ctl.z,0.125);
    let uv_tile=ndc.xy*vec2<f32>(0.5,-0.5)+0.5;
    let uv=clamp(cell+uv_tile*0.125,cell+texel*0.5,cell+vec2<f32>(0.125-texel*0.5));
    let visible=textureSampleCompareLevel(cascades,depth_samp,uv,layer,ndc.z-0.0004);
    return mix(1.0,visible,clamp(csm.local_ctl.y,0.0,1.0));
}
fn fog_local_cube_face(from_light:vec3<f32>)->i32 {
    let a=abs(from_light);
    if(a.x>=a.y && a.x>=a.z) {return select(1,0,from_light.x>0.0);}
    if(a.y>=a.z) {return select(3,2,from_light.y>0.0);}
    return select(5,4,from_light.z>0.0);
}
// Bounded source-to-participant optical depth. Certified clear air contributes
// no fog; actual native ground on the light path blocks transmission. Local
// building/foliage occlusion belongs to the atlas above, never the sun's CSM.
fn fog_local_transmission(p:vec3<f32>, light_pos:vec3<f32>)->f32 {
    let delta=light_pos-p;var tau=0.0;
    var before=fog_outdoor_sample(p);
    for(var j=0u;j<4u;j=j+1u) {
        let start=p+delta*(f32(j)*0.25);
        let middle=p+delta*((f32(j)+0.5)*0.25);
        let end=p+delta*((f32(j)+1.0)*0.25);
        let centre=fog_outdoor_sample(middle);let after=fog_outdoor_sample(end);
        let mid_ground=fog_native_ground(middle.xz);let end_ground=fog_native_ground(end.xz);
        if((mid_ground.y>0.5 && middle.y<=mid_ground.x+0.05) || (end_ground.y>0.5 && end.y<=end_ground.x+0.05)) {return 0.0;}
        tau+=fog_sun_segment_tau(start,end,before,centre,after);before=after;
    }
    return fog_transmittance(tau);
}
fn fog_local_incident(p:vec3<f32>, rel:vec3<f32>, ray:vec3<f32>)->vec3<f32> {
    var source=vec3<f32>(0.0);
    for(var i=0u;i<min(params.capacity.z,16u);i=i+1u) {
        let light=local_lights[i];let to_light=light.pos.xyz-p;
        let distance2=dot(to_light,to_light);let core2=light.pos.w*light.pos.w;
        let reach_multiplier=select(10.0,light.dir.x,light.dir.w<0.5 && light.dir.x>0.0);
        let end2=core2*reach_multiplier*reach_multiplier;
        if(distance2>=end2 || distance2<=1e-8 || core2<=0.0) {continue;}
        let direction=to_light*inverseSqrt(distance2);
        var cone=1.0;var slot=-1;
        if(light.dir.w>0.5) {
            let inside=-dot(direction,light.dir.xyz);
            if(inside<=0.0) {continue;}
            let cosine2=inside*inside;
            let outer2=select(0.95677279,light.diffuse.w,light.diffuse.w>0.0);
            if(cosine2<outer2) {continue;}
            let outer=acos(sqrt(clamp(outer2,0.0,1.0)));
            let angle=acos(clamp(inside,0.0,1.0));
            let t=clamp((outer-angle)/max(outer,1e-4),0.0,1.0);
            // Match the existing surface reflector's hot-centre shape exactly.
            cone=pow(t*t*(3.0-2.0*t),select(1.0,light.ambient.w,light.ambient.w>0.0));
            if(light.dir.w>1.5) {slot=i32(light.dir.w)-2;}
        } else if(light.dir.w< -0.5) {slot=i32(-light.dir.w)-1+fog_local_cube_face(-to_light);}
        if(cone<=0.0) {continue;}
        let attenuation=min(1.0,core2/max(distance2,1e-8))*(1.0-smoothstep(0.85,1.0,sqrt(distance2/end2)));
        let visible=fog_local_visibility(rel,slot);
        if(visible<=0.0) {continue;}
        // WgrLight carries the same CPU legacy colour packet used by HDR surface
        // lighting. Decode once here into scene-linear irradiance; no exposure,
        // additive screen cone, material ambient or artistic brightness gain.
        source+=srgb_to_linear(light.diffuse.rgb)*attenuation*cone*visible
            *fog_phase(dot(ray,direction),params.control.y)*fog_local_transmission(p,light.pos.xyz);
    }
    return source;
}
// Reuse the incoming-light field instead of marching the same sun path
// for every camera participant. Separate passes prohibit read/write aliasing.
fn fog_cached_solar(p:vec3<f32>,ground:f32,density:f32)->f32 {
    let uv=(p.xz-params.solar_domain.xy)*params.solar_domain.z;
    let agl=max(p.y-ground,0.0);
    var cached=1.0;var confidence=0.0;
    if(all(uv>=vec2<f32>(0.0)) && all(uv<=vec2<f32>(1.0)) && agl<params.solar_domain.w){
        let dims=textureDimensions(solar_read);
        let z=(sqrt(agl/params.solar_domain.w)*f32(dims.z-1u)+0.5)/f32(dims.z);
        cached=clamp(textureSampleLevel(solar_read,filter_samp,vec3<f32>(uv,z),0.0).r,0.0,1.0);
        let edge=min(min(uv.x,1.0-uv.x),min(uv.y,1.0-uv.y));
        confidence=smoothstep(0.0,2.0/f32(dims.x),edge);
        if(confidence>=1.0){return cached;}
    }
    // Leaving the camera-centred cache must not switch solar attenuation off.
    // The certified local column is an analytic fallback, with no ray samples.
    // Interior cache hits above keep their original work budget.
    var local_tau=0.0;
    let y=p.y-params.terrain.x*(ground-params.terrain.y);
    for(var i=0u;i<2u;i=i+1u){let layer=params.layers[i];
        local_tau+=max(fog_height_primitive(layer.y+layer.z,layer)-fog_height_primitive(y,layer),0.0)
            *layer.w*params.control.z;
    }
    let fallback=fog_transmittance(local_tau*density/max(params.light.y,0.001));
    return mix(fallback,cached,confidence);
}
@compute @workgroup_size(8,8,1)
fn cs_fog(@builtin(global_invocation_id) id: vec3<u32>) {
    let dims=textureDimensions(result);
    if (id.x>=dims.x || id.y>=dims.y) { return; }
    let uv=(vec2<f32>(id.xy)+0.5)/vec2<f32>(dims.xy);
    let ndc=uv*vec2<f32>(2.0,-2.0)+vec2<f32>(-1.0,1.0);
    let wp=params.inv_vp*vec4<f32>(ndc,0.0,1.0);
    let ray=fog_ray_direction(wp);
    if (dot(ray,ray) < 0.5) {
        for (var z=0u;z<dims.z;z=z+1u) {
            textureStore(result,vec3<i32>(i32(id.x),i32(id.y),i32(z)),vec4<f32>(0.0,0.0,0.0,1.0));

        }
        return;
    }
    let ray_far=fog_ray_far(params.camera.w,params.camera.y,ray.y,params.ocean.w,params.ocean.z);
    var upper:array<vec4<f32>,9>;
    for(var i=0u;i<9u;i=i+1u) { upper[i]=sky_sh.c[i+9u]; }
    // Reuse the once-per-frame sky projection. Eight view-relative sky fetches
    // and their discontinuous hemisphere/basis changes are no longer required.
    let ambient=fog_sky_ambient(ray,params.control.y,upper);
    var colour=vec3<f32>(0.0);
    var trans=1.0;
    var previous=0.0;
    for(var z=0u;z<dims.z;z=z+1u) {
        let w=(f32(z)+0.5)/f32(dims.z);
        let end_distance=ray_far*w*w;
        let step=(end_distance-previous)*0.25;
        for(var s=0u;s<4u;s=s+1u) {
            let start=previous+f32(s)*step;
            let midpoint=start+step*0.5;
            let rel=ray*midpoint;
            let p=params.camera.xyz+rel;
            let before=fog_outdoor_sample(params.camera.xyz+ray*start);
            let centre=fog_outdoor_sample(p);
            let after=fog_outdoor_sample(params.camera.xyz+ray*(start+step));
            // A ray interval crossing the coverage/ground boundary contains
            // valid air. Reject only an entirely excluded interval, and weight
            // each analytic participant at its own certified world position.
            // Taking the minimum of all three taps deleted complete intervals
            // as the camera moved, producing bright steps and flicker.
            if (max(max(before.y,centre.y),after.y) <= 0.0) { continue; }
            var tau=0.0;
            var source=vec3<f32>(0.0);
            for(var i=0u;i<2u;i=i+1u) {
                // Each linear native half has a connected slab intersection.
                // Sampling their combined centroid could land in a dry gap.
                let pieces=2u;
                for(var piece=0u;piece<pieces;piece=piece+1u) {
                    let length=step/f32(pieces);
                    let offset=f32(piece)*length;
                    let native_start=select(before.x,centre.x,piece==1u);
                    let native_end=select(centre.x,after.x,piece==1u);
                    let native_slope=(native_end-native_start)/length;
                    let world_start_y=params.camera.y+ray.y*(start+offset);
                    let air=fog_air_interval(world_start_y-native_start,ray.y-native_slope,length);
                    if(air.y<=0.0){continue;}
                    let slope=ray.y-params.terrain.x*native_slope;
                    let start_y=world_start_y-params.terrain.x*(native_start-params.terrain.y)+slope*air.x;
                    let participant=fog_segment_participant(start_y,slope,air.y,params.layers[i]);
                    if(participant.y<=0.0){continue;}
                    let sample_rel=ray*(start+offset+air.x+participant.x);
                    let sample_pos=params.camera.xyz+sample_rel;
                    let outdoor=fog_outdoor_sample(sample_pos);
                    if(outdoor.y<=0.0){continue;}
                    // Native ground can curve between the three old taps. A
                    // linear-half estimate must not light an actually empty slab.
                    let sample_height=sample_pos.y-params.terrain.x*(outdoor.x-params.terrain.y);
                    if(fog_height_density(sample_height,params.layers[i])<=0.0){continue;}
                    let density=fog_patch_density(sample_pos,params.patchiness)*fog_pooling(sample_pos,outdoor.x);
                    let weight=participant.y*params.layers[i].w*params.control.z*density*outdoor.y;
                    if(weight<=0.0){continue;}
                    let direct=params.radiance.rgb*fog_phase(dot(ray,params.light.xyz),params.control.y)
                        *fog_light_visibility(sample_pos,sample_rel)*fog_cached_solar(sample_pos,outdoor.x,density);
                    tau+=weight;
                    source+=weight*(ambient+direct+fog_local_incident(sample_pos,sample_rel,ray));
                }
            }
            if(tau<=0.0) { continue; }
            let segment_trans=fog_transmittance(tau);
            colour += trans*(1.0-segment_trans)*params.control.x*(source/tau);
            trans *= segment_trans;
        }
        previous=end_distance;
        // The medium changes chroma only: keep exact extinction, masks and
        // direct surface-light transmission. Never modify the environment map.
        let fog_colour=fog_neutral_radiance(colour,0.85*min(params.control.z*4.0,1.0));
        textureStore(result,vec3<i32>(i32(id.x),i32(id.y),i32(z)),vec4<f32>(fog_colour,trans));

    }
}

// The separate incoming-light field is parameterized by worldXZ and quadratic
// AGL, with an exact near-ground slice. Thin1..5m fog cannot disappear between
// coarse camera-ray samples when viewing ground from an airborne camera.
@compute @workgroup_size(8,8,1)
fn cs_solar(@builtin(global_invocation_id) id:vec3<u32>) {
    let dims=textureDimensions(solar_result);
    if (any(id>=dims)) {return;}
    let uv=(vec2<f32>(id.xy)+0.5)/vec2<f32>(dims.xy);
    let xz=params.solar_domain.xy+uv/params.solar_domain.z;
    let native=fog_native_ground(xz);
    var solar=1.0;
    if(native.y>=0.5){
        var top=-1e10;
        for(var i=0u;i<2u;i=i+1u){if(params.layers[i].w>0.0){top=max(top,params.layers[i].y+params.layers[i].z);}}
        let max_agl=max(top-params.terrain.x*params.terrain.y-(1.0-params.terrain.x)*params.sky_limit.w,1.0);
        let w=f32(id.z)/f32(dims.z-1u);
        let p=vec3<f32>(xz.x,native.x+max(0.1,max_agl*w*w),xz.y);
        solar=fog_solar_transmission(p);
    }
    textureStore(solar_result,vec3<i32>(id),vec4<f32>(solar,0.0,0.0,1.0));
}
