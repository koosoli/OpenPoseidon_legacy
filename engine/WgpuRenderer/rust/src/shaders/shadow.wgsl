#define_import_path shadow

#import frame::{frame, shadow_map, shadow_samp}

// Correct each texel's reference depth before bilinear filtering. A hardware
// comparison sampler uses one reference for four depths; biasing that reference
// for the steepest texel leaks sunlight through thin roofs and wall junctions.
fn plane_compare(uv: vec2<f32>, layer: i32, reference: f32, gradient: vec2<f32>) -> f32 {
    let size = vec2<i32>(textureDimensions(shadow_map));
    let pixel = uv * vec2<f32>(size) - vec2<f32>(0.5);
    let base = vec2<i32>(floor(pixel));
    let fraction = fract(pixel);
    var result = 0.0;
    for (var y = 0; y < 2; y++) {
        for (var x = 0; x < 2; x++) {
            let coordinate = clamp(base + vec2<i32>(x, y), vec2<i32>(0), size - vec2<i32>(1));
            let centre = (vec2<f32>(coordinate) + vec2<f32>(0.5)) / vec2<f32>(size);
            let adjusted = reference + clamp(dot(centre - uv, gradient), -0.02, 0.02);
            let depth = textureLoad(shadow_map, coordinate, layer, 0);
            let weight = select(1.0 - fraction.x, fraction.x, x == 1)
                       * select(1.0 - fraction.y, fraction.y, y == 1);
            result += weight * select(0.0, 1.0, adjusted <= depth);
        }
    }
    return result;
}

// Cascaded shadow strength in [0,1] for a camera-relative position (0 = lit).
#ifdef COALESCED_PCF
// Nine overlapping bilinear tent taps use only sixteen distinct depth texels.
// Only called where neither receiver-plane clamp is active and the spacing is
// exactly one texel. Other filter widths and grazing slopes retain plane_compare.
fn plane_tent(uv: vec2<f32>, layer: i32, reference: f32, gradient: vec2<f32>) -> f32 {
    let size = vec2<i32>(textureDimensions(shadow_map));
    let pixel = uv * vec2<f32>(size) - vec2<f32>(0.5);
    let base = vec2<i32>(floor(pixel));
    let f = fract(pixel);
    let wx = vec4<f32>(1.0 - f.x, 2.0 - f.x, 1.0 + f.x, f.x);
    let wy = vec4<f32>(1.0 - f.y, 2.0 - f.y, 1.0 + f.y, f.y);
    var result = 0.0;
    for (var y = 0; y < 4; y++) {
        for (var x = 0; x < 4; x++) {
            let coordinate = clamp(base + vec2<i32>(x - 1, y - 1), vec2<i32>(0), size - vec2<i32>(1));
            let centre = (vec2<f32>(coordinate) + vec2<f32>(0.5)) / vec2<f32>(size);
            let depth = textureLoad(shadow_map, coordinate, layer, 0);
            let adjusted = reference + dot(centre - uv, gradient);
            result += wx[x] * wy[y] * select(0.0, 1.0, adjusted <= depth);
        }
    }
    return result / 16.0;
}
#endif

// Tier select (omni by 3D distance, frustum by eye-depth) with coverage
// fallthrough, cross-tier blend band, HW-PCF compare, far fade, fog dimming.
// dwx/dwy are screen-space derivatives of world_pos (receiver-plane bias). This
// is a port of the GL33 lit shader, mirroring the unit-tested
// ShadowMath::SampleShadow reference; both the lit mesh and terrain pipelines
// call it so they self-shadow identically.
fn shadow_strength(world_pos: vec3<f32>, normal_ws: vec3<f32>, fog: f32,
                   dwx: vec3<f32>, dwy: vec3<f32>) -> f32 {
    let n_cascades = i32(frame.shadow.ctl.x);
    if (n_cascades <= 0) {
        return 0.0;
    }
    let omni_n = i32(frame.shadow.ctl.y);
    let eye_depth = dot(world_pos, frame.shadow.cam_fwd.xyz);
    let dist3d = length(world_pos);

    var ci = n_cascades;
    for (var i = 0; i < 4; i++) {
        if (i >= n_cascades) {
            break;
        }
        let metric = select(eye_depth, dist3d, i < omni_n);
        if (metric <= frame.shadow.splits[i]) {
            ci = i;
            break;
        }
    }
    if (ci >= n_cascades) {
        return 0.0;
    }

    // Normal-offset receiver bias (ShadowMath::ShadowBias): push the receiver
    // along its normal by ~2 world-texels * sin(angle to the light).
    let cos_t = dot(normal_ws, -frame.shadow.sun_dir.xyz);
    let sin_t = sqrt(max(0.0, 1.0 - cos_t * cos_t));

    var prev_edge = 0.0;
    if (ci > 0) {
        prev_edge = frame.shadow.splits[ci - 1];
    }
    let ci_metric = select(eye_depth, dist3d, ci < omni_n);
    let band = (frame.shadow.splits[ci] - prev_edge) * 0.15;
    var bw = 0.0;
    if (ci + 1 < n_cascades) {
        bw = clamp((ci_metric - (frame.shadow.splits[ci] - band)) / max(band, 0.001), 0.0, 1.0);
    }

    let ts = frame.shadow.ctlb.x;
    var lit_sum = 0.0;
    var w_sum = 0.0;
    for (var p = 0; p < 4; p++) {
        let c = ci + p;
        if (c >= n_cascades) {
            break;
        }
        // p0 = primary, p1 = blend partner; while nothing has covered yet a
        // later p force-samples the next looser tier (coverage fallthrough).
        var w: f32;
        if (p == 0) {
            w = 1.0 - bw;
        } else if (w_sum <= 0.0) {
            w = 1.0;
        } else if (p == 1) {
            w = bw;
        } else {
            w = 0.0;
        }
        if (w <= 0.0) {
            continue;
        }

        let vp = frame.shadow.cascade_vp[c];
        // World metres per texel from the ortho x row (unit rotation * 2/width).
        let sx = max(length(vec3<f32>(vp[0][0], vp[1][0], vp[2][0])), 1e-6);
        let texel_world = 2.0 * ts / sx;
        let offset = frame.shadow.ctlb.z * 2.0 * texel_world * sin_t;

        let cp = vp * vec4<f32>(world_pos + normal_ws * offset, 1.0);
        let sc = cp.xyz / cp.w;
        // wgpu texture v runs top-down (vs GL's bottom-up in the GL33 kernel).
        let suv = vec2<f32>(sc.x * 0.5 + 0.5, 0.5 - sc.y * 0.5);
        if (suv.x > 0.0 && suv.x < 1.0 && suv.y > 0.0 && suv.y < 1.0 && sc.z > 0.0 && sc.z < 1.0) {
            // Receiver-plane depth bias (Isidoro): the receiver's light-space
            // depth gradient over shadow UV. Each comparison then tests against
            // the plane's exact depth at that tap, which kills the texel-
            // quantisation acne bands on surfaces nearly parallel to the sun —
            // an error no constant/slope knob can bound.
            let dsx = vp * vec4<f32>(dwx, 0.0);
            let dsy = vp * vec4<f32>(dwy, 0.0);
            let duv_dx = vec2<f32>(0.5 * dsx.x, -0.5 * dsx.y);
            let duv_dy = vec2<f32>(0.5 * dsy.x, -0.5 * dsy.y);
            let det = duv_dx.x * duv_dy.y - duv_dx.y * duv_dy.x;
            var dz_duv = vec2<f32>(0.0, 0.0);
            if (abs(det) > 1e-12) {
                dz_duv = vec2<f32>(dsx.z * duv_dy.y - dsy.z * duv_dx.y,
                                   dsy.z * duv_dx.x - dsx.z * duv_dy.x) / det;
            }
            // Near-grazing the solve goes singular and the gradient explodes;
            // cap the depth change per texel so the taps stay sane.
            let lim = 0.02 / max(ts, 1e-6);
            dz_duv = clamp(dz_duv, vec2<f32>(-lim, -lim), vec2<f32>(lim, lim));
            let bias = frame.shadow.ctl.w * f32(c + 1) * f32(c + 1);
            let ref_z = sc.z - bias;
            var lit: f32;
            let pcf = frame.shadow.ctlb.w;
#ifdef COALESCED_PCF
            let dimensions = textureDimensions(shadow_map);
            if (pcf == 1.0 && dimensions.x == dimensions.y
                && abs(ts * f32(dimensions.x) - 1.0) < 1e-6
                && dot(abs(dz_duv), vec2<f32>(ts)) < 0.005) {
                lit = plane_tent(suv, c, ref_z, dz_duv);
            } else {
#endif
            if (pcf >= 0.5) {
                // 3x3 tent, plane-corrected per tap: smooths the texel-quantised
                // silhouette teeth that grazing light stretches across receivers.
                let o = ts * pcf;
                var sum = 0.0;
                for (var dy = -1; dy <= 1; dy++) {
                    for (var dx = -1; dx <= 1; dx++) {
                        let off = vec2<f32>(f32(dx), f32(dy)) * o;
                        let wt = (2.0 - abs(f32(dx))) * (2.0 - abs(f32(dy)));
                        let adj = clamp(dot(off, dz_duv), -0.02, 0.02);
                        sum += wt * plane_compare(suv + off, c, ref_z + adj, dz_duv);
                    }
                }
                lit = sum / 16.0;
            } else {
                lit = plane_compare(suv, c, ref_z, dz_duv);
            }
#ifdef COALESCED_PCF
            }
#endif
            lit_sum += w * lit;
            w_sum += w;
        }
    }
    if (w_sum <= 0.0) {
        return 0.0;
    }
    let lit = lit_sum / w_sum;
    let last_split = frame.shadow.splits[n_cascades - 1];
    let fade = clamp((last_split - eye_depth) / max(frame.shadow.ctl.z, 0.001), 0.0, 1.0);
    return (1.0 - lit) * fade * clamp(fog, 0.0, 1.0);
}
