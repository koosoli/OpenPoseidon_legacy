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

// Fixed disk: no frame/pixel random rotation, shared by every cascade and game.
fn contact_disk(i: u32) -> vec2<f32> {
    let points = array<vec2<f32>, 16>(
        vec2<f32>(0.1767767, 0.0000000),
        vec2<f32>(-0.2257722, 0.2068258),
        vec2<f32>(0.0345581, -0.3937712),
        vec2<f32>(0.2845712, 0.3711728),
        vec2<f32>(-0.5222232, -0.0923739),
        vec2<f32>(0.4946954, -0.3146847),
        vec2<f32>(-0.1654659, 0.6155250),
        vec2<f32>(-0.3155615, -0.6075944),
        vec2<f32>(0.6846422, 0.2500302),
        vec2<f32>(-0.7122561, 0.2940090),
        vec2<f32>(0.3433545, -0.7337286),
        vec2<f32>(0.2537302, 0.8089320),
        vec2<f32>(-0.7647459, -0.4431859),
        vec2<f32>(0.8971340, -0.1972324),
        vec2<f32>(-0.5475069, 0.7787722),
        vec2<f32>(-0.1264868, -0.9760897)
    );
    return points[i];
}

// Orthographic sun maps: normalized depth / length(VP depth row) is metres.
// Never divide by blocker depth as for a perspective light. Search is bounded
// to 100 m separation; casters beyond that bound are an explicit prototype limit.
fn contact_filter(uv: vec2<f32>, layer: i32, reference: f32,
                  gradient: vec2<f32>, world_to_uv: vec2<f32>,
                  depth_per_metre: f32) -> f32 {
    let slope = max(frame.shadow.sun_dir.w, 0.0);
    if (slope <= 0.0) {
        return contact_reconstruct(uv, layer, reference, gradient);
    }
    let budget = frame.shadow.cam_fwd.w > 1.5;
    let taps = select(16u, 8u, budget);
    let stride = select(1u, 2u, budget);
    let size = vec2<i32>(textureDimensions(shadow_map));
    let search_uv = 100.0 * slope * world_to_uv;
    var separation = 0.0;
    var blockers = 0.0;
    // Centre included even in the cheap mode so contact casters are not skipped.
    for (var i = 0u; i <= taps; i++) {
        var offset = vec2<f32>(0.0);
        if (i < taps) {
            // Concentrate the search near the receiver while retaining outer
            // coverage: wide emitters must not jump over small nearby blockers.
            let point = contact_disk(i * stride);
            offset = point * length(point) * search_uv;
        }
        let sample_uv = uv + offset;
        if (any(sample_uv <= vec2<f32>(0.0)) || any(sample_uv >= vec2<f32>(1.0))) { continue; }
        let pixel = vec2<i32>(sample_uv * vec2<f32>(size));
        let centre = (vec2<f32>(pixel) + vec2<f32>(0.5)) / vec2<f32>(size);
        let receiver = reference + clamp(dot(centre - uv, gradient), -0.02, 0.02);
        let depth = textureLoad(shadow_map, pixel, layer, 0);
        if (depth < receiver) {
            let gap = max(0.0, receiver - depth) / max(depth_per_metre, 1e-8);
            separation += gap;
            blockers += 1.0;
        }
    }
    if (blockers == 0.0) {
        // A sparse search cannot prove there is no thin alpha-tested blocker.
        return contact_reconstruct(uv, layer, reference, gradient);
    }
    // Standard PCSS mean blocker depth. A centre-depth minimum collapses the
    // entire footprint when one near receiver/self sample enters the search.
    let radius = min(separation / blockers, 100.0) * slope * world_to_uv;
    let radius_texels = max(radius.x * f32(size.x), radius.y * f32(size.y));
    if (radius_texels <= 0.5) {
        return contact_reconstruct(uv, layer, reference, gradient);
    }
    var lit = 0.0;
    for (var i = 0u; i < taps; i++) {
        let offset = contact_disk(i * stride) * radius;
        lit += plane_compare(uv + offset, layer,
                             reference + clamp(dot(offset, gradient), -0.02, 0.02), gradient);
    }
    var sample_count = taps;
    // Adaptive PCF: refine mixed visibility only. Fully lit/occluded footprints
    // keep the cheap disk; penumbrae complete the same 16-point reference disk.
    if (budget && lit > 0.0 && lit < f32(taps)) {
        for (var i = 0u; i < 8u; i++) {
            let offset = contact_disk(i * 2u + 1u) * radius;
            lit += plane_compare(uv + offset, layer,
                                 reference + clamp(dot(offset, gradient), -0.02, 0.02), gradient);
        }
        sample_count = 16u;
    }
    let filtered = lit / f32(sample_count);
    if (radius_texels < 1.5) {
        return mix(contact_reconstruct(uv, layer, reference, gradient), filtered,
                   smoothstep(0.5, 1.5, radius_texels));
    }
    return filtered;
}

// Cascaded shadow strength in [0,1] for a camera-relative position (0 = lit).
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

// Reconstruction is separate from the physical penumbra: a subtexel light
// footprint must not disable shadow-map antialiasing. Keep the baseline tent.
fn contact_reconstruct(uv: vec2<f32>, layer: i32, reference: f32,
                       gradient: vec2<f32>) -> f32 {
    let texel = 1.0 / vec2<f32>(textureDimensions(shadow_map));
    if (dot(abs(gradient), texel) < 0.005) {
        return plane_tent(uv, layer, reference, gradient);
    }
    var sum = 0.0;
    for (var y = -1; y <= 1; y++) {
        for (var x = -1; x <= 1; x++) {
            let offset = vec2<f32>(f32(x), f32(y)) * texel;
            let weight = (2.0 - abs(f32(x))) * (2.0 - abs(f32(y)));
            sum += weight * plane_compare(uv + offset, layer,
                reference + clamp(dot(offset, gradient), -0.02, 0.02), gradient);
        }
    }
    return sum / 16.0;
}

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
            if (frame.shadow.cam_fwd.w > 0.5) {
                let sy = length(vec3<f32>(vp[0][1], vp[1][1], vp[2][1]));
                let sz = length(vec3<f32>(vp[0][2], vp[1][2], vp[2][2]));
                lit = contact_filter(suv, c, ref_z, dz_duv, vec2<f32>(sx, sy) * 0.5, sz);
            } else {
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
            }
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
