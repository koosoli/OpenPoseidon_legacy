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
const LOCAL_ATLAS_GRID: f32 = 8.0;

// LGT-015: which face of a point light's cube a fragment falls in, in the same order the CPU
// builds the six matrices: +X, -X, +Y, -Y, +Z, -Z. `from_light` points light -> fragment.
fn cube_face_of(from_light: vec3<f32>) -> i32 {
    let a = abs(from_light);
    if (a.x >= a.y && a.x >= a.z) {
        return select(1, 0, from_light.x > 0.0);
    }
    if (a.y >= a.z) {
        return select(3, 2, from_light.y > 0.0);
    }
    return select(5, 4, from_light.z > 0.0);
}

fn local_shadow_lit(world_rel: vec3<f32>, normal_ws: vec3<f32>, to_light_dir: vec3<f32>,
                    to_light_dist: f32, tan_outer: f32, slot: i32) -> f32 {
    let n_local = i32(frame.shadow.local_ctl.x);
    if (slot < 0 || slot >= n_local) {
        return 1.0;
    }
    // One texel of this light's map, in world metres, at this receiver's distance.
    // local_ctl.z is one ATLAS texel; a tile is 1/GRID of the atlas, so a TILE texel covers
    // GRID times as much world. Getting this wrong under-offsets by 4x and the surface
    // shadows itself in stripes.
    let texel_world =
        2.0 * tan_outer * 1.15 * max(to_light_dist, 0.1) * frame.shadow.local_ctl.z * LOCAL_ATLAS_GRID;
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
    // LGT-015: every local view is a TILE in one layer, not a layer of its own. The tile's
    // own [0,1] uv is mapped into its cell, and the PCF taps are clamped a half-texel inside
    // it -- without that clamp a tap at the rim reads the neighbouring light's depth, which
    // shows up as a hard rectangle of false shadow rather than as noise.
    let grid = LOCAL_ATLAS_GRID;
    let tile = 1.0 / grid;
    let cell = vec2<f32>(f32(slot % i32(grid)), f32(slot / i32(grid))) * tile;
    let uv_tile = vec2<f32>(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    let layer = i32(frame.shadow.local_ctl.w);
    // Constant depth bias. A perspective map's texel footprint grows with distance from the
    // light, so a slope-scaled bias would want the derivative of a value this function does
    // not have; the constant is what the cascades use as their floor and it is enough at the
    // ranges a headlight covers.
    // Small now, and only there to absorb depth quantisation: the normal offset above is
    // what keeps a surface from shadowing itself.
    let bias = 0.0004;
    let texel = frame.shadow.local_ctl.z;
    let lo = cell + vec2<f32>(texel * 0.5);
    let hi = cell + vec2<f32>(tile - texel * 0.5);
    var lit = 0.0;
    for (var y = -1; y <= 1; y = y + 1) {
        for (var x = -1; x <= 1; x = x + 1) {
            let off = vec2<f32>(f32(x), f32(y)) * texel;
            let uv = clamp(cell + uv_tile * tile + off, lo, hi);
            lit = lit + textureSampleCompareLevel(shadow_map, shadow_samp, uv, layer, ndc.z - bias);
        }
    }
    lit = lit * (1.0 / 9.0);
    // `darkness` scales how much occlusion is applied, so 0 restores the unshadowed look
    // exactly for an A/B without a rebuild.
    return mix(1.0, lit, frame.shadow.local_ctl.y);
}

struct LocalLighting {
    diffuse: vec3<f32>,
    specular: vec3<f32>,
};

fn lights_material_contrib(world_rel: vec3<f32>, normal_ws: vec3<f32>, mat_diffuse: vec3<f32>, mat_ambient: vec3<f32>, mat_specular: vec3<f32>, spec_power: f32, linear: f32) -> LocalLighting {
    var acc: LocalLighting;
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
        // LGT-014: reach used to be hardwired at 10x the start attenuation, which welded a
        // light's CORE SIZE to its RANGE -- shrink one to stop a street lamp reading as a
        // floodlight and the lamp also stops existing 90 m away. A point light carries its
        // own multiplier in dir.x (its dir.xyz is otherwise unused here); anything that does
        // not, spots included, keeps the 10.
        var end_mult = 10.0;
        if (light.dir.w < 0.5 && light.dir.x > 0.0) {
            end_mult = light.dir.x;
        }
        let end_atten2 = start_atten2 * end_mult * end_mult;
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
        // LGT-015: a POINT light that owns a shadow cube. It cannot use the same encoding as
        // a spot -- `dir.w > 0.5` is what makes a light a spot, and a lamp that became a spot
        // to get a shadow is the regression this fixes -- so it goes negative instead:
        // -(1 + first slot), with the light's six faces in slots first..first+5.
        var cube_first = -1;
        if (light.dir.w < -0.5) {
            cube_first = i32(-light.dir.w) - 1;
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
            if (light.diffuse.w > 0.0) {
                min_inside2 = light.diffuse.w;
            }
            if (cos2 < min_inside2) {
                continue;
            }
            // LGT-012: ramp in the ANGLE, not in cos^2, and smoothstep it. cos^2 compresses
            // near the axis and stretches at the rim, so a linear ramp in it puts almost all
            // of the falloff in the last degree -- which is what made the beam edge read as a
            // cut-out disc rather than a light. The angle domain spreads it evenly and the
            // smoothstep removes the two hard derivative breaks at the ends.
            let a_out = acos(sqrt(clamp(min_inside2, 0.0, 1.0)));
            let a_here = acos(sqrt(clamp(cos2, 0.0, 1.0)));
            // LGT-017: ramp from the AXIS, not from the authored inner angle.
            //
            // LGT-012 already spread the falloff evenly in the angle and smoothstepped it,
            // and the owner still called the result an ugly cone on a lit wall. The reason is
            // that the ramp only occupied the gap between the inner and outer angles: inside
            // the inner cone the beam is perfectly FLAT, so what lands on a wall is a disc of
            // uniform brightness with a rim, and the eye reads the rim as an edge no matter
            // how softly it is drawn. A real reflector has a hot centre that falls off the
            // whole way out. So the ramp now runs axis -> rim, smoothstepped, then pushed
            // back up by `shape` so the middle keeps its brightness while the rim still
            // arrives at zero with a zero derivative. `light.ambient.w`, the authored inner
            // angle, no longer gates the shape -- it is what the beam WAS, and it is what
            // made the disc.
            let t = clamp((a_out - a_here) / max(a_out, 1e-4), 0.0, 1.0);
            let s = t * t * (3.0 - 2.0 * t);
            // ambient.w is the beam's shaping exponent (LGT-017); below 1 keeps a hot
            // centre. Zero means an old light that never said: fall back to a plain ramp.
            cone = pow(s, select(1.0, light.ambient.w, light.ambient.w > 0.0));
        }
        // GL33's flat-core / inverse-square attenuation, but with the tail smoothly
        // windowed to zero at end_atten2. GL33 cuts the tail hard — invisible in its
        // clamped LDR output, but the HDR exposure turns that discontinuity into a
        // visible ring. The window is ~1 across the near field (it only bites as the
        // fragment approaches the cutoff), so it doesn't change GL33's near look.
        // LGT-019: inverse square, and this is where it was NOT being respected.
        //
        // `base_atten` is honest: inside the bulb radius the light is flat (you are inside the
        // emitter), and outside it is exactly r^2/d^2. The offender was the window below. It
        // used to be (1 - (d/end)^2)^2, which does not switch on near the cutoff at all -- it
        // is already taking 44% of the light at HALF the range and 75% at 0.7 of it. Every
        // local light in the game was therefore far dimmer at distance than inverse square
        // says, on top of the small core, which is most of why a lit village read as black
        // from the air.
        //
        // The window's only job is to stop the hard cut at end_atten showing as a ring in HDR,
        // so it now bites over the last 15% of the range and nothing before it. Inverse square
        // holds across the whole useful field.
        let base_atten = select(1.0, start_atten2 / size2, size2 >= start_atten2);
        let d_frac = sqrt(size2 / max(end_atten2, 1e-6));
        let fade = 1.0 - smoothstep(0.85, 1.0, d_frac);
        let atten = base_atten * fade;
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
        // LGT-015: the point light's cube. Six 90-degree faces, so the face's half-angle
        // tangent is exactly 1; which face is chosen from the major axis of light -> fragment,
        // the same rule the CPU used to order the six matrices.
        var point_occl = 1.0;
        if (cube_first >= 0) {
            let dist_p = sqrt(size2);
            let face = cube_face_of(-to_light);
            point_occl = local_shadow_lit(world_rel, normal_ws, to_light / max(dist_p, 1e-4), dist_p,
                                          1.0, cube_first + face);
            if (point_occl <= 0.0) {
                continue;
            }
            cone = cone * point_occl;
        }
        let cos_fi = dot(to_light, normal_ws);
        if (cos_fi > 0.0) {
            let ndotl = cos_fi * inverseSqrt(size2);
            acc.diffuse += (l_diffuse * mat_diffuse * ndotl + l_ambient * mat_ambient) * (atten * cone);
            if (spec_power > 0.0 && any(mat_specular > vec3<f32>(0.0))) {
                let view_dir = -world_rel / max(length(world_rel), 1e-4);
                let half_sum = to_light / max(sqrt(size2), 1e-4) + view_dir;
                let half_dir = half_sum / max(length(half_sum), 1e-4);
                let gloss = pow(max(dot(normal_ws, half_dir), 0.0), max(spec_power, 1.0));
                // Reuse the same spot/cube visibility, no second shadow lookup.
                acc.specular += l_diffuse * mat_specular * (gloss * atten * cone);
            }
        } else {
            // Back-facing: ambient only, no cone gate (GL33 parity) -- except for the
            // shadow, which must apply here too. A surface facing away from the light still
            // received its ambient through a wall before this.
            // A cube-shadow penumbra must attenuate back-face ambient as well.
            // point_occl is 1 for lights without a cube; reuse the existing lookup.
            let occl = select(point_occl, cone, shadow_slot >= 0);
            acc.diffuse += l_ambient * mat_ambient * (atten * occl);
        }
    }
    return acc;
}

fn lights_contrib(world_rel: vec3<f32>, normal_ws: vec3<f32>, mat_diffuse: vec3<f32>, mat_ambient: vec3<f32>, linear: f32) -> vec3<f32> {
    return lights_material_contrib(world_rel, normal_ws, mat_diffuse, mat_ambient,
                                  vec3<f32>(0.0), 0.0, linear).diffuse;
}
