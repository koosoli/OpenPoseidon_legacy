#define_import_path lighting

#import frame::{frame, lights, shadow_map, shadow_samp}
#import color::srgb_to_linear

// Sun lighting matching GL33's lit path: diffuse * N.L + ambient (eye
// accommodation folded in on the CPU), saturated like the vertex-colour pack it
// replaces. sun_dir_world is the light's travel direction (GL33's sunDir
// constant, negated against the true up normal exactly as GL33's vertex shader
// does); at night/dawn it points at or up through the horizon, so level ground
// falls back to ambient. Not the shadow block's sun_dir, which is only valid
// while the cascade pass runs.
//
// `linear` (the HDR path's pipeline override, 0/1): decode the CPU-supplied
// gamma-space sun colours to linear and drop the [0,1] saturate so radiance can
// exceed 1.0 into the HDR target. 0 = the exact gamma-naive GL33 behaviour.
fn sun_light(normal_ws: vec3<f32>, linear: f32) -> vec3<f32> {
    let cos_fi = max(dot(normal_ws, -frame.sun_dir_world.xyz), 0.0);
    var diffuse = frame.sun_diffuse.rgb;
    var ambient = frame.sun_ambient.rgb;
    if (linear > 0.5) {
        diffuse = srgb_to_linear(diffuse);
        ambient = srgb_to_linear(ambient);
        return diffuse * cos_fi + ambient;
    }
    return min(diffuse * cos_fi + ambient, vec3<f32>(1.0));
}

// Cone falloff band for spotlights: full inside cos(8deg), zero outside cos(12deg)
// (GL33's LightReflector cone), interpolated in cos^2 of the angle from the axis.
const LOCAL_MIN_INSIDE2: f32 = 0.95677279; // (cos 12deg)^2
const LOCAL_MAX_INSIDE2: f32 = 0.98063081; // (cos 8deg)^2

// Accumulated point/spot light contribution at a fragment, matching GL33's
// VSNormal local-light loop but per-fragment. `world_rel` is the camera-relative
// fragment position both entry shaders already carry; the light's camera-relative
// position is reconstructed from its absolute position via frame.cam_pos.
// mat_diffuse/mat_ambient modulate the per-light colour (GL33's matDif/matAmb;
// pass white for terrain). Quadratic falloff past the light's start distance, cut
// off at 100x; the light colours already fold in NightEffect, so day = no-op.
// LGT-010: how much of local light `slot` reaches this fragment. 1 = lit, 0 = fully
// shadowed. `world_rel` is camera-relative and local_vp is built camera-relative to the same
// camera, so no rebasing is needed here -- getting that wrong is the classic way this comes
// out either uniformly lit or uniformly black.
//
// The 3x3 PCF is the same shape the cascades use. A single tap on a perspective map gives a
// hard, crawling edge, and a spot's map is small enough that one extra kernel is cheap.
// `normal_ws`, `to_light_dist` and `tan_outer` are what turn a constant depth bias into a
// NORMAL-OFFSET one -- the same cure the cascades use, and the reason the first working
// version produced grain instead of silhouettes.
//
// A perspective shadow map's texel covers more world the further it is from the light, so a
// single constant bias is either useless up close or a peter-panning gap far away. The world
// size of one texel here is 2 * tan(halfFov) * distance / resolution, and every term of that
// is already available: the resolution in local_ctl.z and the cone's tangent recovered from
// the cos^2 the light already carries for its cone test. Pushing the receiver along its own
// normal by a texel or two, scaled by how obliquely the light strikes it, moves the sample
// off the surface that is shadowing itself without moving the shadow's edge.
fn local_shadow_lit(world_rel: vec3<f32>, normal_ws: vec3<f32>, to_light_dir: vec3<f32>,
                    to_light_dist: f32, tan_outer: f32, slot: i32) -> f32 {
    let n_local = i32(frame.shadow.local_ctl.x);
    if (slot < 0 || slot >= n_local) {
        return 1.0;
    }
    // One texel of this light's map, in world metres, at this receiver's distance.
    let texel_world = 2.0 * tan_outer * 1.15 * max(to_light_dist, 0.1) * frame.shadow.local_ctl.z;
    // Obliquely lit surfaces need more of it: across one texel their depth changes by
    // texel * tan(angle to the light), so the offset is scaled by sin over cos of that
    // angle -- the same shape the cascades' ShadowBias uses, clamped so a surface seen
    // edge-on cannot demand an unbounded push.
    let ndl = clamp(dot(normal_ws, to_light_dir), 0.0, 1.0);
    let sin_t = sqrt(max(0.0, 1.0 - ndl * ndl));
    let oblique = clamp(sin_t / max(ndl, 0.15), 0.0, 4.0);
    let offset_pos = world_rel + normal_ws * (texel_world * (1.0 + 1.5 * oblique));
    let clip = frame.shadow.local_vp[slot] * vec4<f32>(offset_pos, 1.0);
    if (clip.w <= 0.0) {
        return 1.0; // behind the light: its cone term has already rejected this fragment
    }
    let ndc = clip.xyz / clip.w;
    if (ndc.x < -1.0 || ndc.x > 1.0 || ndc.y < -1.0 || ndc.y > 1.0 || ndc.z < 0.0 || ndc.z > 1.0) {
        return 1.0; // outside the light's own map: nothing was rendered to occlude with
    }
    let uv = vec2<f32>(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    let layer = i32(frame.shadow.local_ctl.w) + slot;
    // Constant depth bias. A perspective map's texel footprint grows with distance from the
    // light, so a slope-scaled bias would want the derivative of a value this function does
    // not have; the constant is what the cascades use as their floor and it is enough at the
    // ranges a headlight covers.
    // Small now, and only there to absorb depth quantisation: the normal offset above is
    // what keeps a surface from shadowing itself.
    let bias = 0.0004;
    let texel = frame.shadow.local_ctl.z;
    var lit = 0.0;
    for (var y = -1; y <= 1; y = y + 1) {
        for (var x = -1; x <= 1; x = x + 1) {
            let off = vec2<f32>(f32(x), f32(y)) * texel;
            lit = lit + textureSampleCompareLevel(shadow_map, shadow_samp, uv + off, layer, ndc.z - bias);
        }
    }
    lit = lit * (1.0 / 9.0);
    // `darkness` scales how much occlusion is applied, so 0 restores the unshadowed look
    // exactly for an A/B without a rebuild.
    return mix(1.0, lit, frame.shadow.local_ctl.y);
}

fn lights_contrib(world_rel: vec3<f32>, normal_ws: vec3<f32>, mat_diffuse: vec3<f32>, mat_ambient: vec3<f32>, linear: f32) -> vec3<f32> {
    var acc = vec3<f32>(0.0);
    let count = u32(max(frame.cam_pos.w, 0.0));
    for (var i = 0u; i < count; i = i + 1u) {
        let light = lights[i];
        // Per-light gamma-space colours decoded to linear for the HDR path (the
        // caller passes already-linearized mat_diffuse/mat_ambient). The night
        // dimming factor folded into these on the CPU stays as a linear scalar.
        var l_diffuse = light.diffuse.rgb;
        var l_ambient = light.ambient.rgb;
        if (linear > 0.5) {
            l_diffuse = srgb_to_linear(l_diffuse);
            l_ambient = srgb_to_linear(l_ambient);
        }
        // Camera-relative light position, then the fragment-to-light vector.
        let to_light = (light.pos.xyz - frame.cam_pos.xyz) - world_rel;
        let size2 = dot(to_light, to_light);
        let start_atten2 = light.pos.w * light.pos.w;
        let end_atten2 = start_atten2 * 100.0;
        if (size2 >= end_atten2) {
            continue;
        }
        var cone = 1.0;
        // LGT-010: dir.w is the light kind. 0 = point, 1 = spot, 2+n = spot shadowed by
        // local view n. The spot test is unchanged (`> 0.5`), which is why the slot could be
        // carried here without growing the 64-byte light struct.
        var shadow_slot = -1;
        if (light.dir.w > 1.5) {
            shadow_slot = i32(light.dir.w) - 2;
        }
        if (light.dir.w > 0.5) {
            // inside = (light -> fragment) . beam axis; cos^2 = inside^2 / size2.
            let inside = -dot(to_light, light.dir.xyz);
            if (inside <= 0.0) {
                continue;
            }
            let cos2 = (inside * inside) / size2;
            // LGT-011: the light's own cone when it supplies one (cos^2 of the outer and
            // inner half-angles, in the two alpha channels), and the old hardcoded 12/8
            // degrees when it does not -- so a light that says nothing behaves exactly as
            // before and this cannot regress anything that was working.
            var min_inside2 = LOCAL_MIN_INSIDE2;
            var max_inside2 = LOCAL_MAX_INSIDE2;
            if (light.diffuse.w > 0.0) {
                min_inside2 = light.diffuse.w;
                max_inside2 = select(min(light.diffuse.w + 0.02, 0.9999), light.ambient.w,
                                     light.ambient.w > light.diffuse.w);
            }
            if (cos2 < min_inside2) {
                continue;
            }
            cone = clamp((cos2 - min_inside2) / max(max_inside2 - min_inside2, 1e-4), 0.0, 1.0);
        }
        // GL33's flat-core / inverse-square attenuation, but with the tail smoothly
        // windowed to zero at end_atten2. GL33 cuts the tail hard — invisible in its
        // clamped LDR output, but the HDR exposure turns that discontinuity into a
        // visible ring. The window is ~1 across the near field (it only bites as the
        // fragment approaches the cutoff), so it doesn't change GL33's near look.
        let base_atten = select(1.0, start_atten2 / size2, size2 >= start_atten2);
        let fade = clamp(1.0 - (size2 / end_atten2) * (size2 / end_atten2), 0.0, 1.0);
        let atten = base_atten * fade * fade;
        // The occlusion term local lights never had. Applied to the cone factor so it scales
        // the diffuse AND the ambient this light adds -- an unshadowed ambient term is what
        // made a wall's far side glow in the first place.
        if (shadow_slot >= 0) {
            // The cone's tangent, recovered from the cos^2 the light already carries: the
            // shadow map's field of view is that cone, so this is the map's own geometry
            // rather than a guess.
            let c2 = clamp(light.diffuse.w, 0.02, 0.9999);
            let tan_outer = sqrt(max(1.0 - c2, 0.0) / c2);
            let dist_l = sqrt(size2);
            cone = cone * local_shadow_lit(world_rel, normal_ws, to_light / max(dist_l, 1e-4), dist_l,
                                           tan_outer, shadow_slot);
            if (cone <= 0.0) {
                continue;
            }
        }
        let cos_fi = dot(to_light, normal_ws);
        if (cos_fi > 0.0) {
            let ndotl = cos_fi * inverseSqrt(size2);
            acc += (l_diffuse * mat_diffuse * ndotl + l_ambient * mat_ambient) * (atten * cone);
        } else {
            // Back-facing: ambient only, no cone gate (GL33 parity) -- except for the
            // shadow, which must apply here too. A surface facing away from the light still
            // received its ambient through a wall before this.
            let occl = select(1.0, cone, shadow_slot >= 0);
            acc += l_ambient * mat_ambient * (atten * occl);
        }
    }
    return acc;
}
