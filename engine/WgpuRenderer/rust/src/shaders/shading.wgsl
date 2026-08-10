// Shared object fragment shading — extracted verbatim from shader3d.wgsl's fs_main so the
// GPU-driven indirect path (docs/gpu-culling-and-depth-plan.md Stage 3) reuses the EXACT
// same lit look. Given a sampled albedo + the six resolved material colour terms, produces
// the final fogged rgb. The per-draw path folds the sun into the material CPU-side and
// passes the folded terms; the GPU-driven path folds raw material × the frame sun in the
// shader — both then call shade(), so the lighting / shadow / specular / fog logic lives in
// ONE place and can't drift between the two paths.

#define_import_path shading

#import frame::{frame, terrain_sun_shadow, apply_fog, cloud_sun_shadow, sky_irradiance, sky_vis_ao, gtao_ao, gtao_debug_on, gtao_bent_normal_world, gtao_debug_colour, interior_sky_ao, interior_sky_reach, interior_sky_debug_on, interior_sky_ambient_normal}
#import shadow::shadow_strength
#import lighting::lights_contrib
#import color::srgb_to_linear

// The six material colour terms shade() consumes (raw gamma-space; shade srgb-decodes on
// the HDR/linear path). sun_ambient/sun_diffuse are the SUN-FOLDED terms (material × sun),
// used only on the legacy path; light_diffuse/light_ambient are the raw material terms for
// the frame-global local lights; specular is the sun-folded highlight colour and spec_power
// its exponent.
struct ShadeMaterial {
    emissive: vec3<f32>,
    sun_ambient: vec3<f32>,
    sun_diffuse: vec3<f32>,
    light_diffuse: vec3<f32>,
    light_ambient: vec3<f32>,
    specular: vec3<f32>,
    spec_power: f32,
    // Per-texel specular level, 1.0 when the material has no specular map. Kept apart from
    // `specular` because it is a LINEAR mask and `specular` is a gamma-space colour: folding
    // it in before the srgb decode below would put the mask through the sRGB curve, and at
    // SMDI's measured mean (G ~ 47/255) that is 0.028 where 0.184 was meant -- a specular
    // highlight roughly a seventh of its intended strength.
    spec_mask: f32,
};

// world_pos is camera-relative; dwx/dwy = dpdx/dpdy(world_pos), computed in the entry point
// under uniform control flow (before any discard). `linear` selects the HDR path,
// `is_cutout` the alpha-tested-foliage canopy-AO branch, `foliage_shadow_ao` its constant.
fn shade(
    albedo_in: vec3<f32>,
    mat: ShadeMaterial,
    normal: vec3<f32>,
    world_pos: vec3<f32>,
    fog: f32,
    dwx: vec3<f32>,
    dwy: vec3<f32>,
    linear: f32,
    // Per-model BAKED sky visibility at this point [0,1] (LIT-020 Stage 2), or 1 when the model
    // has no volume / the feature is off. Multiplies the ambient exactly like the other two
    // occluders; it is separate from them because it answers a question neither can — "does the
    // building I am standing in let sky in here" — from geometry resolved in MODEL space, so its
    // boundaries land on the walls instead of on a camera-relative grid.
    baked_sky_vis: f32,
    // The BAKED incoming-sky direction at this point, world space, or zero when the model has no
    // volume. Steers the sky-irradiance lookup toward where light actually enters — the window —
    // instead of only scaling how much arrives. Integrated over 41 directions at bake time and
    // trilinearly filtered, so it varies smoothly; the five-direction per-frame equivalent jumped
    // between discrete directions across a surface and that quantisation was the shadow patches.
    baked_sky_dir: vec3<f32>,
    foliage_shadow_ao: f32,
    // Vegetation canopy cutout (leaf/needle section of a plant): enables the dense-canopy
    // self-occlusion darkening in terrain shadow (foliage_shadow_ao). Since Stage 2 (the MapType
    // gate) this is vegetation-only — callers pass `is_vegetation && alpha_ref > 0`, NOT every
    // cutout — so fences/road decals/characters don't get the foliage treatment.
    is_cutout: bool,
    // Alpha-blended (glass) surface: damp the diffuse sky-irradiance ambient (a transparent
    // canopy is not a diffuse reflector; a full sky wash blows it out + spikes auto-exposure).
    is_translucent: bool,
    // Vegetation canopy cutout (as is_cutout): emulate leaf subsurface scattering so the low-poly
    // cards don't split into a lit/near-black pair at harsh sun angles. Knobs ride in
    // frame.foliage / frame.foliageb / frame.foliagec. See docs/foliage-translucency-plan.md.
    is_foliage: bool,
    // @builtin(position).xy of the calling fragment — the screen pixel this shade() is for.
    // Only the screen-space AO needs it; it is passed rather than derived because shade() is
    // shared by the per-draw and GPU-driven fragment shaders and neither has a global to read.
    frag_coord: vec2<f32>,
) -> vec3<f32> {
    var albedo = albedo_in;
    var m_emissive = mat.emissive;
    var m_sun_ambient = mat.sun_ambient;
    var m_sun_diffuse = mat.sun_diffuse;
    var m_light_diffuse = mat.light_diffuse;
    var m_light_ambient = mat.light_ambient;
    var m_specular = mat.specular;
    if (linear > 0.5) {
        albedo = srgb_to_linear(albedo);
        m_emissive = srgb_to_linear(m_emissive);
        m_sun_ambient = srgb_to_linear(m_sun_ambient);
        m_sun_diffuse = srgb_to_linear(m_sun_diffuse);
        m_light_diffuse = srgb_to_linear(m_light_diffuse);
        m_light_ambient = srgb_to_linear(m_light_ambient);
        m_specular = srgb_to_linear(m_specular);
    }
    // After the decode, for the reason recorded on the field.
    m_specular = m_specular * mat.spec_mask;
    // Decal / overlay sections (traffic-sign text, insignia) can carry ZERO vertex normals:
    // the legacy flat ambient (m_sun_ambient) never used the normal, so nobody noticed. The
    // sky-lit path's DIRECTIONAL sky_irradiance(nrm) does — and normalize((0,0,0)) is NaN, which
    // renders the whole face black regardless of sun/ambient level. When the vertex normal is
    // degenerate, reconstruct the GEOMETRIC face normal from the world-position derivatives
    // (oriented toward the camera) so the overlay lights IDENTICALLY to the base face it sits on
    // — a flat DC-only fallback would leave it noticeably darker than that face (no directional
    // sky, no sun N.L). For every real (unit-ish) normal this is bit-identical to normalize().
    let n_len = length(normal);
    var nrm = normal / max(n_len, 1e-4);
    if (n_len < 1e-4) {
        var n_geo = normalize(cross(dwx, dwy));
        // world_pos is camera-relative (camera at the origin), so the front hemisphere faces it.
        nrm = select(n_geo, -n_geo, dot(n_geo, world_pos) > 0.0);
    }
    // Two-sided foliage. A leaf card is a thin surface authored with ONE normal, and the canopy
    // is built from cards facing every direction, so roughly half of any tree is seen from its
    // back face. Those keep a normal pointing away from the viewer: N.L collapses to zero and the
    // DIRECTIONAL sky-irradiance ambient below is sampled along a normal aimed at the ground, so
    // the card renders near-black regardless of how lit the tree is. That is the dark wedge on a
    // canopy, and it moves with the camera because which cards are back-facing moves with it.
    //
    // Flipping the normal toward the viewer is what "two-sided" means for a surface with no
    // inside. Same idiom and same justification as the degenerate-normal fallback directly above,
    // including its use of world_pos as the camera-relative view vector.
    //
    // Gated on is_foliage so it reaches only vegetation canopy cutouts (MapType-gated, alpha-
    // tested) -- a fence, a road decal or a character must keep its authored facing.
    if (is_foliage && dot(nrm, world_pos) > 0.0) {
        nrm = -nrm;
    }
    let ndotl = max(dot(nrm, -frame.sun_dir_world.xyz), 0.0);
    let sky_lit = frame.sun_diffuse.w > 0.5;
    // CSM shadow (near contact). Folded into the sun removal on the sky-lit path, kept as a
    // final multiply on the legacy path — exactly as the original fs_main did.
    let csm_s = shadow_strength(world_pos, nrm, fog, dwx, dwy);
    // Long-range terrain sun-shadow: removes direct sun (diffuse+specular), keeps ambient/
    // emissive/local, sampled by absolute world position (world_pos is camera-relative).
    let world_abs = world_pos + frame.cam_pos.xyz;
    let terrain_s = terrain_sun_shadow(world_abs.xz, world_abs.y);
    let sun_occ = select(terrain_s, max(terrain_s, csm_s), sky_lit);
    // CLD-020: cloud transmittance MULTIPLIES the remaining direct sun rather than joining the
    // max() above. The others are binary occluders answering "is something between me and the
    // sun"; cloud shadow is a partial transmittance, and a thin deck under a ridge should darken
    // what the ridge already left, not compete with it for the same slot.
    let sun_vis = (1.0 - sun_occ) * cloud_sun_shadow(world_abs.xz);
    // Ambient occlusion on the ambient term (both paths), orthogonal to sun_occ (direct sun).
    // Two independent occluders, so they MULTIPLY (plan §6): sky-visibility is the baked far /
    // km-scale term keyed on the object's terrain column, GTAO the screen-space near/mid term
    // that actually sits objects on the ground. Each returns 1 when its feature is off.
    // Three independent occluders of the sky ambient, so they MULTIPLY: sky-visibility is the
    // baked km-scale terrain term, GTAO the screen-space near/mid term, and interior sky
    // visibility the "is there a roof over me" term that neither of the other two can see (one
    // knows only the heightfield, the other reaches ~2 m and cannot see off-screen geometry).
    // Each returns 1 when its own feature is off.
    let amb_ao = sky_vis_ao(world_abs.xz) * gtao_ao(frag_coord)
        * interior_sky_ao(world_abs, nrm) * clamp(baked_sky_vis, 0.0, 1.0);
    var sun: vec3<f32>;
    if (sky_lit) {
        // Sky-based lighting: frame-global atmosphere sun + DIRECTIONAL sky-irradiance ambient
        // (SH-9 projection of the env map, evaluated per normal), scaled by the skyAmbient knob in
        // sun_ambient.w. albedo is the reflectance via `rgb = albedo * lit`. The per-material folded
        // sun (m_sun_*) is deliberately unused here — see the original fs_main note.
        // Directional ambient (Stage 2): sample the sky along the direction light actually
        // reaches this pixel from, not along the surface normal. Near an occluder those differ,
        // and that difference is what stops a shaded surface reading as a flat wash. Falls back
        // to the geometric normal when GTAO or the bent-normal path is off.
        // Steered TWICE, by two occluders at different scales, and the order matters: GTAO's
        // bent normal is the local (~2 m) open direction from the depth buffer, and the interior
        // steer then bends that toward the direction the SKY reaches this point from — the
        // window or doorway. Indoors GTAO has nothing useful to say (the room is bigger than its
        // radius and the roof is off-screen), so the interior term is what carries the result.
        var amb_n = interior_sky_ambient_normal(world_abs, gtao_bent_normal_world(frag_coord, nrm));
        // The baked steer, when this model has a volume. frame.skyvisb.z is the shared
        // "directional" knob: 0 leaves the ambient purely scaled (the safe default), 1 samples
        // the sky fully along the direction it enters from.
        if (dot(baked_sky_dir, baked_sky_dir) > 1e-6 && frame.skyvisb.z > 0.0) {
            amb_n = normalize(mix(amb_n, baked_sky_dir, frame.skyvisb.z));
        }
        var ambient = sky_irradiance(amb_n) * frame.sun_ambient.w * amb_ao;
        // Glass canopies: keep only a fraction of the sky wash so they read as glazing, not a lit
        // diffuse dome (the direct sun sheen + any glint still sit on top).
        if (is_translucent) {
            ambient *= 0.2;
        }
        if (is_foliage) {
            // Emulated leaf subsurface scattering: even out the hard lit/dark split on low-poly
            // alpha-tested canopy at harsh sun angles. The fill is tinted by the leaf albedo via the
            // shared `rgb = albedo * lit` and gated by sun_vis (a leaf in cast shadow neither
            // transmits nor glows). Knobs: frame.foliage = (trans_scale, distortion, trans_power,
            // wrap); frame.foliageb = (ambient_boost, normal_bend, crown_y_offset, fill_fade_end);
            // frame.foliagec = (gi_strength, _, _, _).
            let k = frame.foliage;
            let kb = frame.foliageb;
            let kc = frame.foliagec;
            let sl = -frame.sun_dir_world.xyz; // surface -> light
            let ndl = dot(nrm, sl);
            let vdir = normalize(-world_pos);  // camera at the origin in camera-relative space
            // Near-field fade (1 near, 0 far), shared by the ambient boost and the SSS fill so both
            // are close-up enhancements and distant billboards revert to plain sky-ambient + Lambert.
            var fade = 1.0;
            if (kb.w > 0.0) {
                fade = 1.0 - smoothstep(kb.w * 0.5, kb.w, length(world_pos));
            }
            // Cheap GI: bounce light tracks local sun exposure, so scale the sky-ambient by the
            // terrain's light level (1 - terrain sun-shadow). Lit areas keep full ambient; foliage in
            // a mountain's shadow settles toward the shadowed terrain instead of glowing in the dark.
            // gi_strength (kc.x) 0 = off; the residual at full shadow is (1 - gi_strength).
            ambient *= mix(1.0, 1.0 - terrain_s, kc.x);
            // Ambient boost — a NEAR-FIELD evening-out of lit foliage; fades to the base ambient with
            // distance so far billboards aren't over-lit.
            ambient *= 1.0 + (kb.x - 1.0) * fade;
            // Base Lambert reflectance — IDENTICAL to terrain's response, so a leaf's sunlit side
            // never over-brightens and a distant leaf shades like the ground it sits on.
            let front = max(ndl, 0.0);
            // Terminator-wrap fill: extra lift toward the dark side only (0 on the lit side, where
            // the wrapped value equals `front`).
            let wrap_fill = max((ndl + k.w) / (1.0 + k.w), 0.0) - front;
            // Unified transmission (DICE fast-SSS): light through the thin leaf, its direction bent
            // by the normal (distortion), seen when the view looks toward that bent light — strong on
            // the backlit / shadow side, ~0 on the sunlit side, so it lifts the dark side without
            // doubling the lit side or painting a flat view-only sheet across a billboard.
            let lt = normalize(sl + nrm * k.y);
            let trans = pow(clamp(dot(vdir, -lt), 0.0, 1.0), max(k.z, 1.0)) * k.x;
            // The SSS fill is a NEAR-FIELD effect (per-leaf translucency shouldn't read as a glow at
            // distance, and low-LOD billboards otherwise flatten into a bright sheet), so it shares
            // the distance fade above; the base Lambert stays so far foliage matches terrain.
            let fill = (wrap_fill + trans) * fade;
            sun = m_emissive + ambient + frame.sun_diffuse.rgb * (front + fill) * sun_vis;
        } else {
            sun = m_emissive + ambient + frame.sun_diffuse.rgb * ndotl * sun_vis;
        }
    } else {
        sun = m_emissive + m_sun_ambient * amb_ao + m_sun_diffuse * ndotl * sun_vis;
    }
    let local = lights_contrib(world_pos, nrm, m_light_diffuse, m_light_ambient, linear);
    let raw = sun + local;
    let lit = select(clamp(raw, vec3<f32>(0.0), vec3<f32>(1.0)), max(raw, vec3<f32>(0.0)), linear > 0.5);
    var rgb = albedo * lit;
    // Sun-only Blinn-Phong specular (untextured, additive, before the shadow multiply). The
    // camera is at the origin in camera-relative space, so view_dir = normalize(-world_pos).
    if (mat.spec_power > 0.0) {
        let view_dir = normalize(-world_pos);
        let half_vec = normalize(-frame.sun_dir_world.xyz + view_dir);
        let n_dot_h = max(dot(nrm, half_vec), 0.0);
        let spec = m_specular * pow(n_dot_h, max(mat.spec_power, 1.0));
        let spec_vis = spec * sun_vis;
        rgb += select(clamp(spec_vis, vec3<f32>(0.0), vec3<f32>(1.0)), max(spec_vis, vec3<f32>(0.0)), linear > 0.5);
    }
    // Canopy self-occlusion for alpha-tested foliage in terrain shadow (terrain-shadow only;
    // CSM already darkens near foliage).
    if (is_cutout) {
        rgb *= mix(1.0, foliage_shadow_ao, terrain_s);
    }
    // Legacy path keeps CSM as a final colour multiply; the sky-lit path already removed the
    // direct sun in shadow above, so it must not double-darken here.
    if (!sky_lit) {
        rgb *= mix(1.0, frame.shadow.ctlb.y, csm_s);
    }
    // Debug: the raw AO buffer as greyscale, BEFORE fog — shipped alongside the effect because
    // judging AO through sun + SH ambient + fog + tonemap is far harder than looking at the
    // buffer. Terrain does the same, so the whole opaque scene switches together.
    if (gtao_debug_on() > 0.5) {
        return gtao_debug_colour(frag_coord, nrm);
    }
    // Same, for the interior sky-reach factor: white = open sky above, black = fully roofed.
    if (interior_sky_debug_on() > 0.5) {
        return vec3<f32>(interior_sky_reach(world_abs));
    }
    var fog_color = frame.fog_color.rgb;
    if (linear > 0.5) {
        fog_color = srgb_to_linear(fog_color);
    }
    // fog_enabled: >=2 = aerial-perspective froxel (per-fragment); 1 = legacy flat fog; 0 =
    // off (fog == 1, mix is a no-op).
    if (frame.params.fog_enabled >= 1.5) {
        rgb = apply_fog(rgb, world_pos);
    } else {
        rgb = mix(fog_color, rgb, fog);
    }
    return rgb;
}
