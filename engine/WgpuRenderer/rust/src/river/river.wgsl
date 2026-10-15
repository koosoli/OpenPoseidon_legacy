// RFG-162: a native Reforger world's rivers drawn as water.
//
// Until this, Everon's 28 rivers were opaque road ribbons in their MatRiver `Color` -- dark olive
// strips (the translucent ribbon route blew the per-draw geometry pool and stayed opt-in). In
// Reforger a river is a clear film over its gravel bed (HalfLightDistance 30 m, Fresnel 0.02)
// whose ripples run downstream (FlowSpeed), reflecting the sky and the banks.
//
// This draws the river's own ribbon (built by the loader from the spline, level across its width
// at the authored water level; the banks hide its edges) in the water pass, with OUR water's
// optics: the frozen scene behind it refracted through the ripples, absorbed along the real path
// through the water column (water_optics, the same extinction the sea uses, from the river's own
// colour and half-light distance), the sky and the screen-space scene reflected with Fresnel, and
// the sun's glint. The ripples are procedural, advected along the spline (flow coordinates in the
// vertex: metres along and across), so they follow every bend.

#import frame::{frame, reverse_z, apply_fog, cloud_sun_shadow, sky_irradiance}
#import water_optics::{water_transmittance}

struct RiverParams {
    // rgb = MatRiver Color (linear), w = HalfLightDistance (m)
    colour: vec4<f32>,
    // x = flow speed (m/s), y = ripple tiling (1/m), z = ripple strength, w = time (s)
    flow: vec4<f32>,
    // x = Fresnel F0, y = sun glint power, z = sun glint strength, w = refraction offset (m)
    optics: vec4<f32>,
    // x = shore fade (m of water), y = screen-space reflections on, z = scattering lift, w = debug
    misc: vec4<f32>,
    // x = WetFloor: how much darker the wet bed reads through the film (Reforger measured: gravel
    // under Everon's river at ~0.62 of its dry brightness), yzw spare
    bed: vec4<f32>,
};

@group(1) @binding(0) var<uniform> rp: RiverParams;
@group(1) @binding(1) var scene_depth: texture_depth_2d;
@group(1) @binding(2) var scene_color: texture_2d<f32>;
@group(1) @binding(3) var scene_samp: sampler;
@group(1) @binding(4) var sky_env: texture_2d<f32>;
@group(1) @binding(5) var sky_env_samp: sampler;

const TWO_PI: f32 = 6.28318530718;

struct VsIn {
    @location(0) pos: vec3<f32>,       // absolute world
    @location(1) along: f32,           // metres along the spline, downstream
    @location(2) flow: vec2<f32>,      // unit downstream direction (xz)
    @location(3) across: f32,          // metres across, signed from the centreline
    @location(4) half_width: f32,
};

struct VsOut {
    @builtin(position) clip: vec4<f32>,
    @location(0) world_rel: vec3<f32>,
    @location(1) flow_uv: vec2<f32>,   // (along, across) metres
    @location(2) flow_dir: vec2<f32>,
    @location(3) edge: f32,            // |across| / half width
};

@vertex
fn vs_river(v: VsIn) -> VsOut {
    var o: VsOut;
    let rel = v.pos - frame.cam_pos.xyz;
    o.clip = reverse_z(frame.proj * frame.view * vec4<f32>(rel, 1.0));
    o.world_rel = rel;
    o.flow_uv = vec2<f32>(v.along, v.across);
    o.flow_dir = v.flow;
    o.edge = abs(v.across) / max(v.half_width, 0.01);
    return o;
}

fn hash2(p: vec2<f32>) -> vec2<f32> {
    let q = vec2<f32>(dot(p, vec2<f32>(127.1, 311.7)), dot(p, vec2<f32>(269.5, 183.3)));
    return fract(sin(q) * 43758.5453) * 2.0 - 1.0;
}

// Gradient noise with its analytic derivative (value, d/dx, d/dy).
fn gnoise(p: vec2<f32>) -> vec3<f32> {
    let i = floor(p);
    let f = fract(p);
    let u = f * f * (3.0 - 2.0 * f);
    let du = 6.0 * f * (1.0 - f);
    let ga = hash2(i);
    let gb = hash2(i + vec2<f32>(1.0, 0.0));
    let gc = hash2(i + vec2<f32>(0.0, 1.0));
    let gd = hash2(i + vec2<f32>(1.0, 1.0));
    let va = dot(ga, f);
    let vb = dot(gb, f - vec2<f32>(1.0, 0.0));
    let vc = dot(gc, f - vec2<f32>(0.0, 1.0));
    let vd = dot(gd, f - vec2<f32>(1.0, 1.0));
    let value = va + u.x * (vb - va) + u.y * (vc - va) + u.x * u.y * (va - vb - vc + vd);
    let deriv = ga + u.x * (gb - ga) + u.y * (gc - ga) + u.x * u.y * (ga - gb - gc + gd) +
        du * (u.yx * (va - vb - vc + vd) + vec2<f32>(vb, vc) - va);
    return vec3<f32>(value, deriv);
}

// Ripple slope in flow space (d height / d along, d height / d across): two layers advected
// downstream at different speeds, stretched along the flow, plus a slow cross-flow wobble.
fn ripple_slope(uv: vec2<f32>, t: f32) -> vec2<f32> {
    let k = max(rp.flow.y, 0.05);
    let speed = rp.flow.x;
    let a = gnoise(vec2<f32>((uv.x - t * speed) * k * 0.55, uv.y * k * 1.1));
    let b = gnoise(vec2<f32>((uv.x - t * speed * 1.6) * k * 1.3 + 17.0, uv.y * k * 2.2 + 5.0));
    let c = gnoise(vec2<f32>((uv.x - t * speed * 0.6) * k * 0.21 + 3.0, uv.y * k * 0.35 - 11.0 + t * 0.05));
    return (a.yz * vec2<f32>(0.55, 1.1) + b.yz * vec2<f32>(1.3, 2.2) * 0.5 + c.yz * vec2<f32>(0.21, 0.35) * 1.5) * k *
        rp.flow.z * 0.05;
}

fn sky_env_sample(dir: vec3<f32>) -> vec3<f32> {
    let u = 0.5 + atan2(dir.z, dir.x) / TWO_PI;
    let v = acos(clamp(dir.y, -1.0, 1.0)) / (TWO_PI * 0.5);
    return textureSampleLevel(sky_env, sky_env_samp, vec2<f32>(u, v), 0.0).rgb;
}

// Camera-relative position of the opaque surface behind screen texel `uv`, or w = 0 for none.
fn opaque_at(uv: vec2<f32>) -> vec4<f32> {
    let dims = vec2<i32>(textureDimensions(scene_depth));
    let texel = clamp(vec2<i32>(uv * vec2<f32>(dims)), vec2<i32>(0), dims - vec2<i32>(1));
    let d = textureLoad(scene_depth, texel, 0);
    if (d <= 1e-6) {
        return vec4<f32>(0.0);
    }
    let ndc_xy = uv * vec2<f32>(2.0, -2.0) + vec2<f32>(-1.0, 1.0);
    let h = frame.inv_view_proj * vec4<f32>(ndc_xy, 1.0, 1.0) - frame.inv_view_proj[2] * d;
    return vec4<f32>(h.xyz / h.w, 1.0);
}

// The physically reflected ray against the opaque depth snapshot (as water.wgsl's
// reflected_scene, shortened for a river: banks and trees within ~60 m).
fn reflected_scene(surface_rel: vec3<f32>, dir: vec3<f32>) -> vec4<f32> {
    let dims = vec2<i32>(textureDimensions(scene_depth));
    for (var i = 0; i < 16; i = i + 1) {
        let ray_distance = 1.0 + f32(i) * 4.0;
        let ray_pos = surface_rel + dir * ray_distance;
        let clip = frame.proj * frame.view * vec4<f32>(ray_pos, 1.0);
        if (clip.w <= 1e-5) {
            break;
        }
        let ndc = clip.xyz / clip.w;
        let uv = ndc.xy * vec2<f32>(0.5, -0.5) + vec2<f32>(0.5, 0.5);
        if (any(uv <= vec2<f32>(0.001)) || any(uv >= vec2<f32>(0.999)) || ndc.z <= 0.0 || ndc.z >= 1.0) {
            break;
        }
        let texel = clamp(vec2<i32>(uv * vec2<f32>(dims)), vec2<i32>(0), dims - vec2<i32>(1));
        let opaque_depth = textureLoad(scene_depth, texel, 0);
        let ray_depth = 1.0 - ndc.z;
        if (opaque_depth > 1e-6 && opaque_depth >= ray_depth) {
            let h = frame.inv_view_proj * vec4<f32>(ndc.xy, 1.0 - opaque_depth, 1.0);
            let opaque_rel = h.xyz / h.w;
            if (length(opaque_rel) > 3.0 && length(opaque_rel - ray_pos) <= 1.0 + ray_distance * 0.05) {
                let edge = min(min(uv.x, uv.y), min(1.0 - uv.x, 1.0 - uv.y));
                let fade = smoothstep(0.0, 0.08, edge) * (1.0 - smoothstep(40.0, 64.0, ray_distance));
                return vec4<f32>(textureSampleLevel(scene_color, scene_samp, uv, 0.0).rgb, fade);
            }
        }
    }
    return vec4<f32>(0.0);
}

@fragment
fn fs_river(in: VsOut) -> @location(0) vec4<f32> {
    let dims = vec2<f32>(textureDimensions(scene_color));
    let uv = in.clip.xy / dims;
    let dist = max(length(in.world_rel), 0.001);
    let v = -in.world_rel / dist; // surface -> eye
    let t = rp.flow.w;

    // Ripple normal: flow-space slope rotated into world xz (along = flow_dir, across = its left).
    let slope = ripple_slope(in.flow_uv, t) * (1.0 - smoothstep(60.0, 220.0, dist));
    let fwd = normalize(vec2<f32>(in.flow_dir.x, in.flow_dir.y) + vec2<f32>(1e-6, 0.0));
    let side = vec2<f32>(-fwd.y, fwd.x);
    let grad = fwd * slope.x + side * slope.y;
    let n = normalize(vec3<f32>(-grad.x, 1.0, -grad.y));

    // The bed behind this pixel and the water path to it.
    let bed = opaque_at(uv);
    var path = 50.0;
    var column = 50.0;
    if (bed.w > 0.5) {
        path = max(length(bed.xyz) - dist, 0.0);
        column = max(in.world_rel.y - bed.y, 0.0);
    }
    // Refraction: the ripple pushes the lookup, scaled by how much water there is to bend through.
    let offset_m = rp.optics.w * clamp(column, 0.0, 1.5);
    let refr_rel = in.world_rel + vec3<f32>(n.x, 0.0, n.z) * offset_m;
    let refr_clip = frame.proj * frame.view * vec4<f32>(refr_rel - vec3<f32>(0.0, column, 0.0), 1.0);
    var refr_uv = uv;
    if (refr_clip.w > 1e-5) {
        refr_uv = clamp(refr_clip.xy / refr_clip.w * vec2<f32>(0.5, -0.5) + vec2<f32>(0.5), vec2<f32>(0.001), vec2<f32>(0.999));
    }
    // Reject a refracted lookup that lands on something IN FRONT of the water (a bank, a leg).
    let refr_bed = opaque_at(refr_uv);
    if (refr_bed.w < 0.5 || length(refr_bed.xyz) < dist + 0.02) {
        refr_uv = uv;
    }
    let dry = textureSampleLevel(scene_color, scene_samp, refr_uv, 0.0).rgb;
    let behind = dry * (1.0 - clamp(rp.bed.x, 0.0, 0.9) * smoothstep(0.0, 0.05, column));

    // Absorption along the path through the column (the extinction the sea uses: the colour's
    // hue, its strength from the half-light distance -- ln 2 / HalfLightDistance per metre).
    let half_light = max(rp.colour.w, 0.5);
    let hue = normalize(max(vec3<f32>(1.0) - rp.colour.rgb * 6.0, vec3<f32>(0.05))) * 1.7320508;
    let sigma = hue * (0.6931472 / half_light);
    let transmit = water_transmittance(sigma, path);
    // What the water itself scatters back: its colour under the sky and the sun.
    let sun_l = -frame.sun_dir_world.xyz;
    let cloud = cloud_sun_shadow(in.world_rel.xz + frame.cam_pos.xz);
    let light = sky_irradiance(vec3<f32>(0.0, 1.0, 0.0)) + frame.sun_diffuse.rgb * max(sun_l.y, 0.0) * cloud;
    let inscatter = rp.colour.rgb * light * rp.misc.z * (vec3<f32>(1.0) - transmit);
    let refracted = behind * transmit + inscatter;

    // Reflection: sky, then the screen-space scene where the reflected ray finds it.
    let r = reflect(-v, n);
    let r_up = normalize(vec3<f32>(r.x, max(r.y, 0.02), r.z));
    var reflected = sky_env_sample(r_up);
    if (rp.misc.y > 0.5) {
        let ssr = reflected_scene(in.world_rel, r_up);
        reflected = mix(reflected, ssr.rgb, ssr.a);
    }
    let cos_v = clamp(dot(n, v), 0.0, 1.0);
    let f0 = clamp(rp.optics.x, 0.0, 1.0);
    let fresnel = f0 + (1.0 - f0) * pow(1.0 - cos_v, 5.0);
    // Sun glint (SunPower is a Phong exponent).
    let h = normalize(sun_l + v);
    let glint = frame.sun_diffuse.rgb * pow(max(dot(n, h), 0.0), max(rp.optics.y, 8.0)) * rp.optics.z * cloud *
        step(0.0, sun_l.y);

    var colour = mix(refracted, reflected, fresnel) + glint;
    // Shore: where the film runs out the bed shows unaltered.
    let shore = smoothstep(0.0, max(rp.misc.x, 0.005), column);
    colour = mix(dry, colour, shore);
    if (rp.misc.w > 0.5) {
        colour = vec3<f32>(fresnel, clamp(path / 10.0, 0.0, 1.0), shore);
    }
    return vec4<f32>(apply_fog(colour, in.world_rel), 1.0);
}
