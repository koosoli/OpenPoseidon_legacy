#import layered_fog_optics::{fog_segment_density, fog_height_primitive, fog_phase, fog_transmittance}

struct FogVolume {
    inv_vp: mat4x4<f32>,
    camera: vec4<f32>, // absolute xyz, actual far distance
    control: vec4<f32>, // scattering albedo, HG g, weather amount, ready
    layers: array<vec4<f32>, 2>,
    light: vec4<f32>, // direction toward the actual main directional light
    radiance: vec4<f32>, // scene-linear directional irradiance
    cloud_map: vec4<f32>,
    near_vp: mat4x4<f32>,
    far_vp: mat4x4<f32>,
    near_ctl: vec4<f32>,
    far_ctl: vec4<f32>,
    capacity: vec4<u32>, // actual retained argument capacity, near/far
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
};
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

fn fog_weather_open(p: vec3<f32>) -> bool {
    let near_clip = params.near_vp * vec4<f32>(p, 1.0);
    let near_uv = near_clip.xy * vec2<f32>(0.5,-0.5) + 0.5;
    let edge = min(min(near_uv.x,1.0-near_uv.x),min(near_uv.y,1.0-near_uv.y));
    // A pending near map never borrows a far map at the same position.
    let outer = near_clip.z < 0.0 || near_clip.z > 1.0 || edge < params.near_ctl.z;
    var clip = near_clip;
    var ctl = params.near_ctl;
    var cap = params.capacity.x;
    if (outer) { clip = params.far_vp * vec4<f32>(p,1.0); ctl = params.far_ctl; cap = params.capacity.y; }
    if (ctl.x < 0.5 || clip.z < 0.0 || clip.z > 1.0) { return false; }
    if (cap > 0u) {
        if (outer) {
            if (far_counts[0] > cap || far_counts[1] > cap || far_counts[2] > 2u*cap) { return false; }
        } else {
            if (near_counts[0] > cap || near_counts[1] > cap || near_counts[2] > 2u*cap) { return false; }
        }
    }
    let uv = clip.xy * vec2<f32>(0.5,-0.5) + 0.5;
    if (min(min(uv.x,1.0-uv.x),min(uv.y,1.0-uv.y)) < ctl.z) { return false; }
    var exposed = 0.0;
    if (outer) { exposed = textureSampleCompareLevel(far_depth,depth_samp,uv,clip.z-ctl.y); }
    else { exposed = textureSampleCompareLevel(near_depth,depth_samp,uv,clip.z-ctl.y); }
    return exposed >= 0.999;
}
fn fog_outdoors(p: vec3<f32>) -> bool {
    if (height.enabled < 0.5 || height.grid <= 0.0 || height.width < 2u || height.height < 2u) { return false; }
    let cell = (p.xz - height.origin) / height.grid;
    if (any(cell < vec2<f32>(0.0)) || any(cell >= vec2<f32>(f32(height.width-1u),f32(height.height-1u)))) { return false; }
    let ij = vec2<i32>(floor(cell));
    let q = fract(cell);
    let a = textureLoad(native_height,ij,0).r;
    let b = textureLoad(native_height,ij+vec2<i32>(1,0),0).r;
    let c = textureLoad(native_height,ij+vec2<i32>(0,1),0).r;
    let d = textureLoad(native_height,ij+vec2<i32>(1,1),0).r;
    let ground = select(b+c-d+(d-c)*q.x+(d-b)*q.y,a+(b-a)*q.x+(c-a)*q.y,q.x+q.y<=1.0);
    // Underground samples, including horizontal bounded entrances, are refused.
    return p.y > ground + 0.05 && fog_weather_open(p);
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
fn fog_solar_transmission(y: f32) -> f32 {
    if (params.light.y <= 0.001) { return 0.0; }
    var tau = 0.0;
    for (var i=0u;i<2u;i=i+1u) {
        let layer=params.layers[i];
        let remainder=max(fog_height_primitive(layer.y+layer.z,layer)-fog_height_primitive(y,layer),0.0);
        tau += remainder*layer.w*params.control.z/params.light.y;
    }
    return fog_transmittance(tau);
}
@compute @workgroup_size(8,8,1)
fn cs_fog(@builtin(global_invocation_id) id: vec3<u32>) {
    let dims=textureDimensions(result);
    if (id.x>=dims.x || id.y>=dims.y) { return; }
    let uv=(vec2<f32>(id.xy)+0.5)/vec2<f32>(dims.xy);
    let ndc=uv*vec2<f32>(2.0,-2.0)+vec2<f32>(-1.0,1.0);
    let wp=params.inv_vp*vec4<f32>(ndc,0.0,1.0);
    let ray=normalize(wp.xyz/wp.w);
    var ambient=vec3<f32>(0.0);
    // Equal-solid-angle six-point quadrature of the UPWARD hemisphere. The
    // environment's artificial negative-Y sky floor is deliberately not light.
    for (var i=0u;i<6u;i=i+1u) {
        let y=(f32(i)+0.5)/6.0;
        let angle=f32(i)*2.39996322973;
        let r=sqrt(max(1.0-y*y,0.0));
        let dir=vec3<f32>(r*cos(angle),y,r*sin(angle));
        let euv=vec2<f32>(atan2(dir.z,dir.x)/6.28318530718+0.5,acos(dir.y)/3.14159265359);
        ambient += textureSampleLevel(environment,filter_samp,euv,0.0).rgb
            * fog_phase(dot(ray,dir),params.control.y)*(6.28318530718/6.0);
    }
    var colour=vec3<f32>(0.0);
    var trans=1.0;
    var previous=0.0;
    for(var z=0u;z<dims.z;z=z+1u) {
        let w=(f32(z)+0.5)/f32(dims.z);
        let end_distance=params.camera.w*w*w;
        let step=(end_distance-previous)*0.5;
        for(var s=0u;s<2u;s=s+1u) {
            let start=previous+f32(s)*step;
            let midpoint=start+step*0.5;
            let rel=ray*midpoint;
            let p=params.camera.xyz+rel;
            if (!fog_outdoors(p) || !fog_outdoors(params.camera.xyz+ray*start)
                || !fog_outdoors(params.camera.xyz+ray*(start+step))) { continue; }
            var tau=0.0;
            for(var i=0u;i<2u;i=i+1u) {
                tau += fog_segment_density(params.camera.y+ray.y*start,ray.y,step,params.layers[i])
                    * params.layers[i].w*params.control.z;
            }
            if(tau<=0.0) { continue; }
            let segment_trans=fog_transmittance(tau);
            let direct=params.radiance.rgb*fog_phase(dot(ray,params.light.xyz),params.control.y)
                * fog_light_visibility(p,rel)*fog_solar_transmission(p.y);
            colour += trans*(1.0-segment_trans)*params.control.x*(ambient+direct);
            trans *= segment_trans;
        }
        previous=end_distance;
        textureStore(result,vec3<i32>(i32(id.x),i32(id.y),i32(z)),vec4<f32>(colour,trans));
    }
}
