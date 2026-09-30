#define_import_path rain_water_reflections
#import frame::frame

@group(2) @binding(0) var puddle_scene_color: texture_2d<f32>;
@group(2) @binding(1) var puddle_scene_sampler: sampler;
@group(2) @binding(2) var puddle_scene_depth: texture_depth_2d;
@group(2) @binding(3) var<uniform> puddle_scene_control: vec4<f32>; // actual recorded readiness, near range, ray range, reserved

fn puddle_finite(v: vec3<f32>) -> bool {
    return all(v == v) && all(abs(v) < vec3<f32>(1000000.0));
}
struct PuddleSceneSample {
    valid: bool,
    uv: vec2<f32>,
    opaque: vec3<f32>, // camera-relative world position of the actual depth texel
    crossing: f32, // positive means the ray has crossed behind visible geometry
};
fn puddle_scene_sample(ray: vec3<f32>) -> PuddleSceneSample {
    var miss: PuddleSceneSample;
    miss.valid=false;
    let clip=frame.proj*frame.view*vec4<f32>(ray,1.0);
    if (!puddle_finite(ray) || !puddle_finite(clip.xyz) || !(clip.w>1e-5) || clip.w>=1000000.0) {return miss;}
    let ndc=clip.xyz/clip.w;
    let uv=ndc.xy*vec2<f32>(0.5,-0.5)+vec2<f32>(0.5);
    if (!puddle_finite(ndc) || any(uv<=vec2<f32>(0.001)) || any(uv>=vec2<f32>(0.999)) || ndc.z<=0.0 || ndc.z>=1.0) {return miss;}
    let dims=vec2<i32>(textureDimensions(puddle_scene_depth));
    let texel=clamp(vec2<i32>(uv*vec2<f32>(dims)),vec2<i32>(0),dims-vec2<i32>(1));
    let depth=textureLoad(puddle_scene_depth,texel,0);
    if (!(depth>1e-6) || depth>1.0) {return miss;}
    // Unproject the sampled texel CENTRE, not the ray's subpixel coordinate.
    let centre=(vec2<f32>(texel)+vec2<f32>(0.5))/vec2<f32>(dims);
    let h=frame.inv_view_proj*vec4<f32>(centre*vec2<f32>(2.0,-2.0)+vec2<f32>(-1.0,1.0),1.0-depth,1.0);
    if (!puddle_finite(h.xyz) || !(abs(h.w)>1e-7) || abs(h.w)>=1000000.0) {return miss;}
    let opaque=h.xyz/h.w;
    if (!puddle_finite(opaque)) {return miss;}
    return PuddleSceneSample(true,uv,opaque,depth-(1.0-ndc.z));
}

// Twenty-four bounded nonuniform steps favour nearby building/object surfaces.
// Four binary refinements and a tight world-position gate reject screen-depth
// discontinuities. A missing/offscreen/foreground hit retains the sky fallback.
fn rain_water_scene_reflection(surface: vec3<f32>, normal: vec3<f32>, normal_variation: f32, footprint: f32) -> vec4<f32> {
    if (puddle_scene_control.x<0.5 || !puddle_finite(surface) || !puddle_finite(normal) ||
        length(surface)<0.1 || length(normal)<0.5 || !(normal.y>0.0) ||
        any(textureDimensions(puddle_scene_color)!=textureDimensions(puddle_scene_depth))) {return vec4<f32>(0.0);}
    let near_weight=1.0-smoothstep(puddle_scene_control.y*0.5,puddle_scene_control.y,length(surface));
    let detail=1.0-smoothstep(0.15,0.7,footprint);
    let rough=1.0-smoothstep(0.06,0.25,normal_variation);
    if (!(near_weight*detail*rough>0.002)) {return vec4<f32>(0.0);}
    let n=normalize(normal);
    let direction=reflect(normalize(surface),n);
    let origin=surface+n*0.025;
    var previous=puddle_scene_sample(origin+direction*0.05);
    var previous_t=0.05;
    for (var i=0;i<24;i=i+1) {
        let fraction=f32(i+1)/24.0;
        let t=0.05+(min(puddle_scene_control.z,25.0)-0.05)*fraction*fraction;
        let sample=puddle_scene_sample(origin+direction*t);
        if (sample.valid && previous.valid && previous.crossing<0.0 && sample.crossing>=0.0) {
            var low=previous_t;
            var high=t;
            var hit=sample;
            var valid=true;
            for (var refine=0;refine<4;refine=refine+1) {
                let mid=(low+high)*0.5;
                let probe=puddle_scene_sample(origin+direction*mid);
                if (!probe.valid) {valid=false;break;}
                if (probe.crossing>=0.0) {high=mid;hit=probe;} else {low=mid;}
            }
            let ray=origin+direction*high;
            // No metre-wide thickness inflation, no reflection of the bed beneath
            // the local water plane, and no fixed 12m rejection of nearby houses.
            let tolerance=0.06+min(0.15,high*0.006);
            if (valid && distance(hit.opaque,ray)<=tolerance &&
                distance(hit.opaque,surface)>0.10 && hit.opaque.y>=surface.y-0.02) {
                let edge=min(min(hit.uv.x,hit.uv.y),min(1.0-hit.uv.x,1.0-hit.uv.y));
                let confidence=smoothstep(0.01,0.08,edge)*near_weight*detail*rough;
                // Black is a legitimate reflected surface at night: validity is
                // independent of RGB. No active color attachment is sampled.
                return vec4<f32>(textureSampleLevel(puddle_scene_color,puddle_scene_sampler,hit.uv,0.0).rgb,confidence);
            }
        }
        previous=sample;previous_t=t;
    }
    return vec4<f32>(0.0);
}
