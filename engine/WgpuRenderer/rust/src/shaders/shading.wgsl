// Shared object fragment shading — extracted verbatim from shader3d.wgsl's fs_main so the
// GPU-driven indirect path (docs/gpu-culling-and-depth-plan.md Stage 3) reuses the EXACT
// same lit look. Given a sampled albedo + the six resolved material colour terms, produces
// the final fogged rgb. The per-draw path folds the sun into the material CPU-side and
// passes the folded terms; the GPU-driven path folds raw material × the frame sun in the
// shader — both then call shade(), so the lighting / shadow / specular / fog logic lives in
// ONE place and can't drift between the two paths.

#define_import_path shading

#import frame::{frame, gi_irradiance, terrain_sun_shadow, apply_fog_receiver, fog_sun_reach, cloud_sun_shadow, sky_irradiance, sky_vis_ao, canopy_sky_ao, canopy_sun_shadow, gtao_ao, gtao_debug_on, gtao_bent_normal_world, gtao_debug_colour, interior_sky_ao, interior_sky_reach, interior_sky_debug_on, interior_sky_ambient_normal, normal_map_cavity, sky_specular_split, spec_power_to_gloss}
#import shadow::{shadow_strength, shadow_strength_lod}
#import lighting::lights_material_contrib
#import color::srgb_to_linear
#import frame::{interior_rain_reach, interior_rain_coverage, snow_receiver_reach}
#import ground_puddles::{ground_puddle_mask, ground_puddle_land_factor, ground_puddle_rain_factor, ground_puddle_ripple_normal}
#import tree_snow::tree_snow_albedo
#import frame::ground_sky_reflection
#import snow_material::{snow_powder_albedo, snow_powder_normal, snow_surface_coverage, snow_noise}

// Cosmetic coating uses the actual raised surface, never terrain snow_depth / ground shelter
// deficits. Baked exposure is raw known-volume proof (0 if missing); active zenith wins nearby.
fn object_snow_baked_exposure(raw_visibility: f32, known_volume: bool) -> f32 {
    if (!known_volume) { return 0.0; }
    return smoothstep(0.98, 0.995, clamp(raw_visibility, 0.0, 1.0));
}

fn object_snow_coverage(world_pos: vec3<f32>, geo_normal: vec3<f32>, eligible: bool,
                        baked_exposure: f32) -> f32 {
    if (!eligible || (frame.snow_surface.x <= 0.0 && frame.snow_surface.w <= 0.0)) { return 0.0; }
    if (!all(abs(geo_normal) < vec3<f32>(1e20))) { return 0.0; }
    let n_len = length(geo_normal);
    if (n_len < 1e-4 || dot(geo_normal, world_pos) > 0.0) { return 0.0; }
    let n_up = geo_normal.y / n_len;
    if (n_up <= 0.25) { return 0.0; }
    let world_abs = world_pos + frame.cam_pos.xyz;
    var depth = frame.snow_surface.x;
    if (frame.snow_surface.y >= 0.0 && frame.snow_surface.z > 0.0) {
        depth = max(depth, smoothstep(frame.snow_surface.y,
            frame.snow_surface.y + frame.snow_surface.z, world_abs.y) * frame.snow_surface.w);
    }
    let validity = interior_rain_coverage(world_abs);
    var exposed = baked_exposure;
    if (validity > 0.0) {
        // Dedicated surface-plane reach never contains the ambient OPEN feather.
        // Blend only actual coverage with independently admitted baked proof.
        let reach = snow_receiver_reach(world_abs, geo_normal);
        exposed = validity * reach + (1.0 - validity) * baked_exposure;
    }
    return snow_surface_coverage(depth, n_up, exposed);
}

fn object_snow_normal(world_pos: vec3<f32>, geo_normal: vec3<f32>, base_n: vec3<f32>,
                      dwx: vec3<f32>, dwy: vec3<f32>, coverage: f32) -> vec3<f32> {
    if (coverage <= 0.0) { return base_n; }
    let footprint = max(length(dwx), length(dwy));
    let powder = snow_powder_normal(world_pos + frame.cam_pos.xyz, normalize(geo_normal), footprint);
    return normalize(mix(base_n, powder, coverage));
}
// Sinkhole W1: the terrain height at a fragment, for the underground sun test below. Every pipeline that
// imports `shading` (shader3d, gpu_driven) already binds the conform heightmap group.
#import conform::{cave_daylight2, bare_surface_y}

// The six material colour terms shade() consumes (raw gamma-space; shade srgb-decodes on
// the HDR/linear path). sun_ambient/sun_diffuse are the SUN-FOLDED terms (material × sun),
// used only on the legacy path; light_diffuse/light_ambient are the raw material terms for
// the frame-global local lights; specular is the sun-folded highlight colour and spec_power
// its exponent.
// BEGIN_WET_TEXTILE_HELPERS (pure production helpers exercised by probes)
fn wet_textile_response(wetness: f32, view_cosine: f32, footprint: f32) -> vec4<f32> {
    if (!(wetness > 0.0)) { return vec4<f32>(0.0); }
    let wet = clamp(wetness, 0.0, 1.0);
    // Saturation darkens the whole fibre substrate, but pore-held water is not
    // a continuous plastic sheet. Only one eighth forms an exterior film here.
    // The water reflection remains neutral and independent of camouflage dye.
    let coating = wet * (2.0 - wet) * 0.125;
    let grazing = 1.0 - clamp(view_cosine, 0.0, 1.0);
    // Water on rough fibres: restrained Fresnel, never a grazing mirror.
    let fresnel = 0.02 + 0.10 * pow(grazing, 5.0);
    // Broaden before creases shrink below a pixel. No fine normal-map grain,
    // time seed, spatial noise or sharp microfacet lobe can glitter here.
    let power = mix(16.0, 8.0, smoothstep(0.015, 0.08, max(footprint, 0.0)));
    let normalized_f0 = 0.02 * (power + 2.0) / 25.13274123;
    return vec4<f32>(coating, fresnel, power, normalized_f0);
}
fn wet_textile_sun_lobe(normal_half_cosine: f32, normal_light_cosine: f32, response: vec4<f32>) -> f32 {
    if (!(response.x > 0.0)) { return 0.0; }
    return response.w * pow(clamp(normal_half_cosine, 0.0, 1.0), response.z)
        * clamp(normal_light_cosine, 0.0, 1.0);
}
fn wet_textile_composite(substrate: vec3<f32>, sky_radiance: vec3<f32>, sun_glare: vec3<f32>,
                         local_glare: vec3<f32>, response: vec4<f32>) -> vec3<f32> {
    if (!(response.x > 0.0)) { return substrate; }
    let reflected = max(sky_radiance, vec3<f32>(0.0)) * response.y
        + max(sun_glare + local_glare, vec3<f32>(0.0));
    return substrate * (1.0 - response.x * response.y) + response.x * reflected;
}
// END_WET_TEXTILE_HELPERS

struct ShadeMaterial {
    emissive: vec3<f32>,
    sun_ambient: vec3<f32>,
    sun_diffuse: vec3<f32>,
    light_diffuse: vec3<f32>,
    light_ambient: vec3<f32>,
    specular: vec3<f32>,
    local_specular: vec3<f32>,
    spec_power: f32,
    // Per-texel specular level, 1.0 when the material has no specular map. Kept apart from
    // `specular` because it is a LINEAR mask and `specular` is a gamma-space colour: folding
    // it in before the srgb decode below would put the mask through the sRGB curve, and at
    // SMDI's measured mean (G ~ 47/255) that is 0.028 where 0.184 was meant -- a specular
    // highlight roughly a seventh of its intended strength.
    spec_mask: f32,
    native_cavity: f32,
    native_ao: f32,
    // RFG-185: per-material scale on the leaf back-light (Enfusion SSSBackScattering / its 0.15
    // class default); 1.0 for everything that is not a native crown.
    foliage_trans: f32,
    // RFG-189: how much of the sun a leaf in an object's shadow still receives THROUGH the leaves
    // in front of it (diffuse, forward-scattered). 0 = an opaque shadow, as every caster was; native
    // crowns set it (gpu_driven.wgsl).
    leaf_transmission: f32,
    // Shader-local material value, never a public buffer/ABI field.
    // Nonzero only after exact primary cloth admission and filtered UV masking.
    wet_cloth: f32,
    // REFLECT-001 phase C (ReflectMaterial.hpp): roughness, F0, metal, clear coat / class,
    // coat roughness, source, debug view. Zero (every caller's `var m: ShadeMaterial` default,
    // and the retained path never sets it) = not classified: shade() takes its old branches.
    // (Not reflect0/reflect1: naga_oil forbids module identifiers that end in a digit.)
    refl_a: vec4<f32>,
    refl_b: vec4<f32>,
};

// REFLECT-001 phase C -- physically based sun response for classified surfaces.
const REFLECT_PI: f32 = 3.14159265;
const REFLECT_CLASS_PAINT_METALLIC: u32 = 4u;
const REFLECT_CLASS_CHROME: u32 = 5u;
const REFLECT_CLASS_MIRROR: u32 = 7u;
const REFLECT_CLASS_LAMP_GLASS: u32 = 8u;
const REFLECT_CLASS_LAMP_PLASTIC: u32 = 9u;

fn reflect_classified(mat: ShadeMaterial, linear: f32) -> bool {
    return mat.refl_b.x > 0.5 && linear > 0.5;
}

// Normal-incidence reflectance of the base layer. Polished metal parts (chrome, mirror glass,
// the chromed bowl behind a lamp cover) are silver whatever their often dark or atlas-shared
// texture says, so they take a fixed silver tinted lightly by the albedo; other metals take
// the albedo, dielectrics the scalar F0.
fn reflect_base_reflectance(mat: ShadeMaterial, albedo_lin: vec3<f32>) -> vec3<f32> {
    let cls = u32(mat.refl_b.x + 0.5);
    let polished = cls == REFLECT_CLASS_CHROME || cls == REFLECT_CLASS_MIRROR ||
                   cls == REFLECT_CLASS_LAMP_GLASS || cls == REFLECT_CLASS_LAMP_PLASTIC;
    let metal_f0 = select(albedo_lin, mix(vec3<f32>(0.62), albedo_lin, 0.25), polished);
    // Metallic paint: aluminium flakes in a tinted binder, so the base layer's reflection takes
    // the paint's colour. A white F0 there washed Arma 3's red C_SUV_01_F to grey-pink once the
    // sky and the ground reached it (E.2/E.3 runs, 2026-10-23). Half the F0 stays neutral, the
    // other half is the albedo, scaled with F0 so an SMDI-dulled texel dulls its tint too.
    if (cls == REFLECT_CLASS_PAINT_METALLIC) {
        let f0 = mat.refl_a.y;
        return vec3<f32>(0.5 * f0) + albedo_lin * min(f0 * 6.0, 0.5);
    }
    return mix(vec3<f32>(mat.refl_a.y), metal_f0, clamp(mat.refl_a.z, 0.0, 1.0));
}

fn reflect_schlick(f0: vec3<f32>, cos_t: f32) -> vec3<f32> {
    let c = clamp(1.0 - cos_t, 0.0, 1.0);
    let c2 = c * c;
    return f0 + (vec3<f32>(1.0) - f0) * (c2 * c2 * c);
}

// GGX distribution x height-correlated Smith visibility (Filament's form), for one lobe.
// The sun is a disc of finite size, so the lobe is never sharper than roughness 0.1: a perfect
// mirror would otherwise turn the sun into a single-pixel spike of tens of thousands.
fn reflect_ggx(ndl: f32, ndv: f32, ndh: f32, roughness: f32) -> f32 {
    let r = max(roughness, 0.1);
    let a2 = r * r * r * r;
    let d_den = ndh * ndh * (a2 - 1.0) + 1.0;
    let d = a2 / (REFLECT_PI * d_den * d_den);
    let gv = ndl * sqrt(ndv * ndv * (1.0 - a2) + a2);
    let gl = ndv * sqrt(ndl * ndl * (1.0 - a2) + a2);
    let v = 0.5 / max(gv + gl, 1e-5);
    return d * v;
}

// Sun specular of base + clear coat, times N.L (radiance scale: multiply by the sun colour).
fn reflect_sun_specular(mat: ShadeMaterial, albedo_lin: vec3<f32>, n: vec3<f32>, view_dir: vec3<f32>,
                        to_sun: vec3<f32>) -> vec3<f32> {
    let ndl = dot(n, to_sun);
    let ndv = max(dot(n, view_dir), 1e-4);
    if (ndl <= 0.0) {
        return vec3<f32>(0.0);
    }
    let h = normalize(to_sun + view_dir);
    let ndh = clamp(dot(n, h), 0.0, 1.0);
    let vdh = clamp(dot(view_dir, h), 0.0, 1.0);
    let base = reflect_schlick(reflect_base_reflectance(mat, albedo_lin), vdh) * reflect_ggx(ndl, ndv, ndh, mat.refl_a.x);
    let coat = clamp(mat.refl_a.w, 0.0, 1.0);
    let fc = reflect_schlick(vec3<f32>(0.04), vdh).x * coat;
    let coat_lobe = fc * reflect_ggx(ndl, ndv, ndh, mat.refl_b.y);
    return (base * (1.0 - fc) + vec3<f32>(coat_lobe)) * ndl;
}

// Rotate `v` by the rotation that carries unit vector `src_n` onto unit vector `dst_n`.
//
// Rodrigues' formula, with cos/sin read straight off dot()/length(cross()) of the two inputs --
// no trig call, ~12 ALU. Returns `v` untouched when the two are (anti)parallel: there the
// rotation is either the identity or undefined about an arbitrary axis, and inventing an axis is
// how a "subtle" shading term acquires a seam.
//
// Used to carry a normal map's ANGULAR DEVIATION (geometric normal -> shading normal) over onto a
// direction that was computed without it. That is a rotation and not a blend on purpose: a blend
// would dilute the direction it is applied to, and the whole point here is to leave that direction
// exactly where it was on a flat surface.
fn rotate_between(src_n: vec3<f32>, dst_n: vec3<f32>, v: vec3<f32>) -> vec3<f32> {
    let axis = cross(src_n, dst_n);
    let s = length(axis);
    if (s < 1e-5) {
        return v;
    }
    let k = axis / s;
    let c = dot(src_n, dst_n);
    return v * c + cross(k, v) * s + k * (dot(k, v) * (1.0 - c));
}

// world_pos is camera-relative; dwx/dwy = dpdx/dpdy(world_pos), computed in the entry point
// under uniform control flow (before any discard).
fn surface_normal(normal: vec3<f32>, world_pos: vec3<f32>, dwx: vec3<f32>, dwy: vec3<f32>) -> vec3<f32> {
    let len2 = dot(normal, normal);
    if (len2 > 1e-8) {
        return normal * inverseSqrt(len2);
    }
    // Legacy sign/overlay vertices can carry zero normals. Resolve these BEFORE
    // normal mapping and cavity shading, not after normalize(0) has produced NaNs.
    let face = cross(dwx, dwy);
    let face_len2 = dot(face, face);
    if (face_len2 > 1e-20) {
        let n = face * inverseSqrt(face_len2);
        return select(n, -n, dot(n, world_pos) > 0.0);
    }
    return vec3<f32>(0.0, 1.0, 0.0);
}

// `linear` selects HDR; `is_cutout` and `foliage_shadow_ao` select canopy shading.
fn shade(
    albedo_in: vec3<f32>,
    mat: ShadeMaterial,
    normal: vec3<f32>,
    // The INTERPOLATED VERTEX normal of this fragment, before any normal map -- i.e. the basis
    // the tangent-space normal was resolved against in the caller. Only the directional sky
    // ambient reads it, to recover "how far did the normal map turn this pixel", which is
    // otherwise unrecoverable inside shade() (it receives only the already-mapped result).
    //
    // Deliberately the vertex normal and NOT cross(dwx, dwy): the derivative cross is the FACE
    // normal, so using it as the reference would inject the mesh's faceting into the ambient on
    // every curved surface -- a new artifact introduced by a fix for flatness.
    // May be zero/degenerate (decal sections authored with no normals); the reader checks.
    geo_normal: vec3<f32>,
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
    // Actual MapType vegetation, including opaque trunks. Used only by fog,
    // separately from the cutout-only leaf lighting and alpha coverage gates.
    is_vegetation: bool,
    // First-person cockpit draw (MAT-052 second half): take the LEGACY flat scene ambient
    // instead of the sky-dome irradiance. A cockpit interior is enclosed by the vehicle
    // body, which none of the ambient occluders can see (sky-vis is terrain-keyed, GTAO
    // reaches ~2 m and the roof is off-screen, the interior volumes are baked for
    // buildings only), so the outdoor sky term washed every interior plate to near-sky
    // brightness -- which against the sky reads as a TRANSPARENT cockpit. GL33's
    // fixed-function look for the same surfaces IS the flat scene ambient.
    is_cockpit: bool,
    // @builtin(position).xy of the calling fragment — the screen pixel this shade() is for.
    // Only the screen-space AO needs it; it is passed rather than derived because shade() is
    // shared by the per-draw and GPU-driven fragment shaders and neither has a global to read.
    frag_coord: vec2<f32>,
    // REN-OBJ-001: take the two SCREEN-SPACE ambient occluders -- the GTAO texture and the
    // interior-sky volume -- for this fragment. False for vegetation cutouts by default: on
    // Everon's perf_combat the alpha-tested object pass is 2.4 ms of which 0.24 is albedo,
    // and ablating GTAO and the interior sky took 0.7 + 0.56 ms of the rest (the four largest
    // terms together left 0.7 ms of base shading). A leaf card has its own canopy occlusion
    // term (foliage_shadow_ao), is never inside a building volume, and the GTAO it received
    // was already mixed down to 35%; what it lost is two texture loads and a volume walk per
    // fragment over ~40% of the screen. WGR_FOLIAGE_SCREEN_AO=1 restores both.
    screen_ao: bool,
    snow_eligible: bool,
    snow_baked_exposure: f32,
    tree_snow_cover: f32,
    ground_surface: bool,
    ground_snow_depth: f32,
    // PERF-FAR-CROWN: (budget-filter from m, tent-filter from m) for leaf cards; x 0 = off.
    foliage_shadow_lite: vec2<f32>,
) -> vec3<f32> {
    var albedo = albedo_in;
    var m_emissive = mat.emissive;
    var m_sun_ambient = mat.sun_ambient;
    var m_sun_diffuse = mat.sun_diffuse;
    var m_light_diffuse = mat.light_diffuse;
    var m_light_ambient = mat.light_ambient;
    var m_specular = mat.specular;
    var m_local_specular = mat.local_specular;
    var m_spec_power = mat.spec_power;
    var m_spec_mask = mat.spec_mask;
    if (linear > 0.5) {
        albedo = srgb_to_linear(albedo);
        m_emissive = srgb_to_linear(m_emissive);
        m_sun_ambient = srgb_to_linear(m_sun_ambient);
        m_sun_diffuse = srgb_to_linear(m_sun_diffuse);
        m_light_diffuse = srgb_to_linear(m_light_diffuse);
        m_light_ambient = srgb_to_linear(m_light_ambient);
        m_specular = srgb_to_linear(m_specular);
        m_local_specular = srgb_to_linear(m_local_specular);
    }
    // After the decode, for the reason recorded on the field.
    m_specular = m_specular * mat.spec_mask;
    m_local_specular = m_local_specular * mat.spec_mask;
    // Decal / overlay sections (traffic-sign text, insignia) can carry ZERO vertex normals:
    // the legacy flat ambient (m_sun_ambient) never used the normal, so nobody noticed. The
    // sky-lit path's DIRECTIONAL sky_irradiance(nrm) does — and normalize((0,0,0)) is NaN, which
    // renders the whole face black regardless of sun/ambient level. When the vertex normal is
    // degenerate, reconstruct the GEOMETRIC face normal from the world-position derivatives
    // (oriented toward the camera) so the overlay lights IDENTICALLY to the base face it sits on
    // — a flat DC-only fallback would leave it noticeably darker than that face (no directional
    // sky, no sun N.L). For every real (unit-ish) normal this is bit-identical to normalize().
    let n_len = length(normal);
    var nrm = surface_normal(normal, world_pos, dwx, dwy);
    // Powder is linear reflectance and belongs to the HDR path. Dry/disabled coverage is
    // an exact early-out. Authored roof paint/gloss/cavity must not remain on top of snow.
    let object_cover = object_snow_coverage(world_pos, geo_normal,
        snow_eligible && linear > 0.5 && !is_foliage && !is_cockpit && !is_translucent,
        snow_baked_exposure);
    let snow_cover = max(object_cover, tree_snow_cover);
    if (snow_cover > 0.0) {
        let footprint = max(length(dwx), length(dwy));
        // Leaf cards use the powder MEAN only: no spatial/time seed or fine
        // normal detail that could sparkle when the cards move or shrink.
        var powder = snow_powder_albedo(world_pos + frame.cam_pos.xyz, footprint, 0.0);
        if (tree_snow_cover > 0.0) {
            powder = tree_snow_albedo(albedo, world_pos + frame.cam_pos.xyz, footprint);
        }
        albedo = mix(albedo, powder, snow_cover);
        nrm = object_snow_normal(world_pos, geo_normal, nrm, dwx, dwy, object_cover);
        m_emissive *= 1.0 - snow_cover;
        m_light_diffuse = mix(m_light_diffuse, vec3<f32>(1.0), snow_cover);
        m_light_ambient = mix(m_light_ambient, vec3<f32>(1.0), snow_cover);
        m_specular *= 1.0 - snow_cover;
        m_local_specular *= 1.0 - snow_cover;
        m_spec_power = mix(m_spec_power, 8.0, snow_cover);
        m_spec_mask = mix(m_spec_mask, 0.02, snow_cover);
        m_spec_mask *= 1.0 - tree_snow_cover;
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
    // Cockpit interiors are single-sided geometry authored for the OUTSIDE: the roof and
    // canopy frames carry exterior (up-facing) normals, so seen from the pilot seat the
    // sun's N.L lights their BACK side -- full noon sun through the roof, the second half
    // of the washed-out interior (the first half was the sky-dome ambient, handled by
    // is_cockpit above). The seen side of a cockpit surface is by definition the interior
    // side, so face the normal at the viewer, same test as the foliage flip: world_pos is
    // camera-relative, so dot(nrm, world_pos) > 0 means the normal points away.
    if (is_cockpit && dot(nrm, world_pos) > 0.0) {
        nrm = -nrm;
    }
    let ndotl = max(dot(nrm, -frame.sun_dir_world.xyz), 0.0);
    let sky_lit = frame.sun_diffuse.w > 0.5;

    // CSM shadow (near contact). Folded into the sun removal on the sky-lit path, kept as a
    // final multiply on the legacy path — exactly as the original fs_main did.
    // PERF-FAR-CROWN: distant leaf cards take a cheaper receiver filter (see shadow_strength_lod).
    // A crown fragment is the single most numerous shaded pixel in a forest view, and the
    // contact filter's 17-tap blocker search + 16-24 compares is a large share of its lighting;
    // past 40 m the 8-tap budget disk resolves the same penumbra, past 120 m a 3x3 tent shows
    // what the far cascades can hold. Measured (owner profile): alpha-test pass -0.2..0.35 ms at
    // Durras / forestOwner, crops equal within wind-sway noise. (budget-from m, tent-from m);
    // x 0 = off (WGR_FAR_CROWN_SHADOW=0, or =<budget>,<tent>).
    // PERF-FAR-OBJECT-SHADOW: a NEGATIVE y extends the same distance steps to every surface
    // (walls, roofs, rocks, vehicles), as the terrain has them (PERF-TERRAIN-FAR-SHADOW); |y| is
    // the tent distance. WGR_FAR_OBJECT_SHADOW=0 = leaf cards only.
    var csm_lod = 0u;
    if ((is_foliage || foliage_shadow_lite.y < 0.0) && foliage_shadow_lite.x > 0.0) {
        let crown_dist = length(world_pos);
        csm_lod = select(select(0u, 1u, crown_dist > foliage_shadow_lite.x), 2u,
                         crown_dist > abs(foliage_shadow_lite.y));
    }
    let csm_s = shadow_strength_lod(world_pos, nrm, fog, dwx, dwy, csm_lod);
    // Long-range terrain sun-shadow: removes direct sun (diffuse+specular), keeps ambient/
    // emissive/local, sampled by absolute world position (world_pos is camera-relative).
    let world_abs = world_pos + frame.cam_pos.xyz;
    let ground_footprint = max(length(dwx.xz), length(dwy.xz));
    var rain_puddle = 0.0;
    if (ground_surface && frame.ground_weather.x > 0.18 && linear > 0.5 && !is_foliage && !is_cockpit && snow_cover == 0.0) {
        rain_puddle = ground_puddle_mask(world_abs.xz, ground_footprint, frame.ground_weather.x)
            * ground_puddle_land_factor(surface_normal(geo_normal, world_pos, dwx, dwy).y,
                world_abs.y, frame.ground_weather.z, ground_snow_depth);
        if (rain_puddle > 0.0) {
            rain_puddle *= ground_puddle_rain_factor(interior_rain_reach(world_abs), interior_rain_coverage(world_abs));
        }
        albedo *= 1.0 - rain_puddle * 0.38;
    }
    // BOOM-003 (owner, 2026-10-19: "icy roads"): a road (a ground surface) in a snowy world is
    // packed, dirty snow in patches and smooth black ice between them, not dry asphalt. Ice is
    // darker and mirror-glossy (a tight sun highlight); packed snow is grey-white and matt.
    // World-anchored noise at 0.7-3.7 m, local coordinates; nothing at all outside winter.
    if (ground_surface && linear > 0.5 && !is_foliage && !is_cockpit && snow_cover == 0.0) {
        var winter = smoothstep(0.0, 0.01, frame.snow_surface.x);
        if (frame.snow_surface.y >= 0.0 && frame.snow_surface.z > 0.0 && frame.snow_surface.w > 0.001) {
            winter = max(winter, smoothstep(frame.snow_surface.y - frame.snow_surface.z, frame.snow_surface.y, world_abs.y));
        }
        if (winter > 0.0) {
            let lp = world_abs.xz - floor(world_abs.xz / 512.0) * 512.0;
            let packed_n = snow_noise(lp / 2.3).x * 0.6 + snow_noise(lp / 0.7 + vec2<f32>(5.0, 9.0)).x * 0.4;
            let ice_n = snow_noise(lp / 3.7 + vec2<f32>(-13.0, 4.0)).x;
            let packed = smoothstep(-0.1, 0.45, packed_n) * winter;
            let ice = smoothstep(0.1, 0.5, ice_n) * winter * (1.0 - 0.7 * packed);
            albedo = mix(albedo, vec3<f32>(0.52, 0.54, 0.57) * (0.85 + 0.15 * packed_n), packed);
            albedo = mix(albedo, albedo * vec3<f32>(0.5, 0.54, 0.6), ice);
            m_specular = mix(m_specular, mat.sun_diffuse * 0.9, ice);
            m_spec_power = mix(m_spec_power, 240.0, ice);
            m_spec_mask = mix(m_spec_mask, 1.0, ice);
            m_specular *= 1.0 - 0.8 * packed;
        }
    }
    // Sinkhole W1/W1b: a fragment under the terrain surface (a cave, a cellar, a tunnel) is lit only by what
    // comes in through the terrain's openings. The heightfield is never a shadow caster for objects, and the
    // long-range mask marches OUTWARD from each column and never counts the column's own ground, so a cave
    // roof 0.4 m down read as open sky (owner report: "the roof of the cave is bleeding light"). W1 removed
    // the sun from everything under the ground, which also blacked out stairwells open to the sky; W1b fades
    // it with the reach from the nearest opening (conform::cave_daylight), DayZ-style.
    // OnSurface roads have terrain-seated fragment depth, but their interpolated
    // vertex chord can still cross below the fine heightfield. Classify the
    // seated receiver, otherwise entire road tiles become dark, unfogged caves.
    // Only proven ground receivers qualify; cave meshes and bridges keep their
    // actual elevation and their underground lighting.
    var cave_pos = world_abs;
    if (ground_surface) {
        cave_pos.y = max(cave_pos.y, bare_surface_y(cave_pos.xz));
    }
    let cave = cave_daylight2(cave_pos);
    let underground_s = 1.0 - cave.x; // direct sun: only through an opening's footprint
    let underground_amb_s = 1.0 - cave.y; // sky / bounce: fades with the reach from an opening
    let terrain_s = max(max(terrain_sun_shadow(world_abs.xz, world_abs.y), underground_s), canopy_sun_shadow(world_abs));
    let sun_occ = select(terrain_s, max(terrain_s, csm_s), sky_lit);
    // CLD-020: cloud transmittance MULTIPLIES the remaining direct sun rather than joining the
    // max() above. The others are binary occluders answering "is something between me and the
    // sun"; cloud shadow is a partial transmittance, and a thin deck under a ridge should darken
    // what the ridge already left, not compete with it for the same slot.
    let sun_open = cloud_sun_shadow(world_abs.xz) * fog_sun_reach(world_pos);
    let sun_vis = (1.0 - sun_occ) * sun_open;
    // RFG-189: the share of the sun that ONLY the cascade casters (objects: other leaves, the
    // crown's own branches) take away. Terrain and cloud shadow stay opaque.
    let sun_csm_blocked = max((1.0 - terrain_s) * sun_open - sun_vis, 0.0);
    // Ambient occlusion on the ambient term (both paths), orthogonal to sun_occ (direct sun).
    // Two independent occluders, so they MULTIPLY (plan §6): sky-visibility is the baked far /
    // km-scale term keyed on the object's terrain column, GTAO the screen-space near/mid term
    // that actually sits objects on the ground. Each returns 1 when its feature is off.
    // Three independent occluders of the sky ambient, so they MULTIPLY: sky-visibility is the
    // baked km-scale terrain term, GTAO the screen-space near/mid term, and interior sky
    // visibility the "is there a roof over me" term that neither of the other two can see (one
    // knows only the heightfield, the other reaches ~2 m and cannot see off-screen geometry).
    // Each returns 1 when its own feature is off.
    // GTAO reads a dense canopy as a cave: leaf cards hand the horizon march an
    // occluder in every direction, so the interior of every tree collapsed to
    // near-black (owner report 2026-08-25; A/B on the Abel oak: WGR_GTAO=0
    // restores the GL33 reference's canopy, so GTAO is the whole mechanism).
    // A leaf card is not a wall -- most of what the march counts as occlusion
    // around a leaf is other leaves, which transmit and scatter light; the
    // foliage translucency path directly below this exists for exactly that
    // reason. So on foliage the term is applied at reduced weight rather than
    // exempted: contact darkening where canopy meets trunk and ground
    // survives, the cave reading does not. 0.35 leaves the darkest canopy
    // interior at ~0.65 ambient, which matches the GL33 capture's character at
    // the same pose. Same is_foliage gate as the cavity exemption below.
    var gtao_t = 1.0;
    var interior_ao = 1.0;
    if (screen_ao) {
        gtao_t = gtao_ao(frag_coord);
        if (is_foliage) {
            gtao_t = mix(1.0, gtao_t, 0.35);
        }
        interior_ao = interior_sky_ao(world_abs, nrm);
    }
    // Native NTC cavity attenuates ambient AND diffuse (Reforger texture contract),
    // unlike NMO ambient occlusion. It does not darken specular or emissive light.
    let leaf_cavity = mix(select(1.0, mat.native_cavity, is_foliage), 1.0, snow_cover);
    m_light_diffuse *= leaf_cavity;
    m_light_ambient *= leaf_cavity * mix(mat.native_ao, 1.0, snow_cover);
    // Sinkhole W1: under the terrain the sky ambient goes too, down to a dim floor. The interior-sky term darkens
    // static buildings it has rooms for; a soldier walking in a cave got the open-sky ambient (owner report:
    // "soldiers also illuminated and not dark"). The same ramp as the sun test, so a cave is one consistent dark.
    let underground_amb = mix(1.0, 0.03, underground_amb_s);
    let amb_ao = sky_vis_ao(world_abs.xz) * canopy_sky_ao(world_abs) * gtao_t * interior_ao * clamp(baked_sky_vis, 0.0, 1.0)
        * leaf_cavity * mix(mat.native_ao, 1.0, snow_cover) * underground_amb;
    // ...and a FOURTH, at a scale none of the three can reach: the normal map's own micro-
    // occlusion. Sky-vis is keyed on the terrain column (km), GTAO marches the depth buffer
    // (~2 m), the interior term answers "is there a roof" (room-sized) — the smallest thing any
    // of them can resolve is a screen pixel of DEPTH, and a crevice in a normal map has no depth
    // at all. So a bump's dark side was lit exactly like its bright side under ambient, which is
    // most of what "the normals do not work at dawn" is. Derivation, the never-brightens and the
    // exactly-1-on-a-flat-map guarantees: frame::normal_map_cavity.
    //
    // It joins the other three by MULTIPLYING for the same reason they multiply each other: it
    // occludes independently and at a disjoint scale. It stays out of `amb_ao` itself because
    // amb_ao is also the term the legacy path applies to its flat ambient, and keeping the two
    // separate is what lets the foliage exemption below be one condition instead of two.
    //
    // Foliage is exempt. A leaf card's normal is AUTHORED (a spherical fake, not relief), and
    // the two-sided flip 30 lines above may have replaced `nrm` with its own negation — against
    // which the vertex normal is a 180-degree reference, so a cavity term would read every
    // back-facing card as a maximally deep crevice and black out half of every tree.
    let cavity = select(normal_map_cavity(geo_normal, nrm), 1.0, is_foliage);
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
        var amb_steer = nrm;
        if (screen_ao) {
            amb_steer = interior_sky_ambient_normal(world_abs, gtao_bent_normal_world(frag_coord, nrm));
        }
        // The baked steer, when this model has a volume. frame.skyvisb.z is the shared
        // "directional" knob: 0 leaves the ambient purely scaled (the safe default), 1 samples
        // the sky fully along the direction it enters from.
        if (dot(baked_sky_dir, baked_sky_dir) > 1e-6 && frame.skyvisb.z > 0.0) {
            amb_steer = normalize(mix(amb_steer, baked_sky_dir, frame.skyvisb.z));
        }
        var amb_n = amb_steer;
        // LIT: put the NORMAL MAP back into the ambient direction.
        //
        // Everything above resolves a direction that contains NO per-pixel detail at all. The
        // GTAO bent normal comes out of the DEPTH buffer at roughly a 2 m march radius, and when
        // it is on it REPLACES the shading normal outright; the interior term then steers that
        // further toward the window. So on any surface where ambient dominates the frame -- dawn,
        // dusk, overcast, shadow, interiors -- the sky term was a direction-per-2-metres, and the
        // normal map contributed to the picture only through the sun's N.L. Measured on Stratis
        // (stone wall, freefly 3094.20 2138.07 170.45): at hour 10 toggling the normal map moves
        // 42.6% of the wall's pixels (mean |d| 11.18 at wall mean level 65); at hour 6 it moves
        // 6.5% (mean |d| 2.85 at level 39). Relief per unit brightness falls 2.4x exactly when
        // the sun stops carrying it. That is the "the normals do not really work" report.
        //
        // (a) sky_irradiance is an SH-9 projection and therefore ALREADY low-frequency in the
        //     direction argument -- it cannot resolve a bump. That is not an argument against
        //     doing this. A dawn sky is strongly directional (bright horizon band, dark zenith,
        //     warm one way and cold the other), so even a smooth L2 field has a large gradient
        //     across the ~30-60 degrees a normal map turns a pixel through, and it is that
        //     gradient, sampled per pixel, that makes brick read as brick instead of as a wash.
        //     The SH-9 bandwidth caps the CONTRAST of the relief; it does not remove it.
        //
        // (b) This must NOT double-count occlusion. Nothing here touches magnitude: `amb_ao`
        //     (sky-vis x GTAO x interior x baked) is applied on the line below, unchanged, and
        //     stays the only place ambient is attenuated. This term changes only WHICH direction
        //     the SH is evaluated in.
        //
        // (c) Which is also why the steer is applied as a ROTATION rather than a blend. The
        //     bent/interior direction is a ~2 m-scale visibility hint, not a surface orientation,
        //     so it must bend the mapped normal, not replace it -- but mixing the two would pull
        //     the result back toward `nrm` and quietly undo LIT-020's "the room is lit through
        //     its window". Carrying the map's own deviation (geo_normal -> nrm) onto the steer
        //     keeps BOTH at full strength, and on a flat surface the rotation is the identity, so
        //     amb_n is bit-identical to what shipped. That is the guarantee that mean ambient
        //     LEVEL is unchanged and this is a directionality change only -- no new blow-out on a
        //     sunlit wall, no extra light in an interior. Over a bumpy surface the perturbations
        //     are zero-mean about the vertex normal, so the mean only moves at second order.
        //
        // No cap on the deviation angle: a steep normal can turn the sample below the horizon,
        // where sky_irradiance's max(e, 0) simply returns less light -- which is the right answer
        // for a crevice, and cannot brighten anything.
        //
        // frame.gtao.w is the weight (WGR_AMBIENT_NORMAL_MAPPED, default 1). 0 restores today's
        // look exactly by skipping the branch.
        //
        // (c, foliage) The foliage branch deliberately does NOT get this. A leaf card's normal is
        // authored, not relief, and the two-sided flip 30 lines above already replaced `nrm` with
        // its negation -- against which the vertex normal is a 180 degree reference and the
        // rotation axis is arbitrary. Canopy shading is carried by the SSS/wrap terms below, not
        // by per-pixel ambient relief. Glass (is_translucent) DOES take the treatment: it is a
        // real surface with a real normal map, and its ambient is damped to 0.2 immediately
        // below, so a more directional sky wash there is 20% of a term that was already tuned
        // low rather than a new source of blow-out.
        let amb_map_w = clamp(frame.gtao.w, 0.0, 1.0);
        let geo_len = length(geo_normal);
        if (amb_map_w > 0.0 && !is_foliage && geo_len > 1e-4 && n_len >= 1e-4) {
            let n_geo_amb = geo_normal / geo_len;
            // Same hemisphere only. A back-facing double-sided draw (or a broken tangent frame)
            // can leave the mapped normal opposite the vertex normal, and rotating by ~180
            // degrees about an arbitrary axis would fling the sample somewhere unrelated.
            if (dot(n_geo_amb, nrm) > 0.1) {
                let mapped_steer = rotate_between(n_geo_amb, nrm, amb_steer);
                amb_n = normalize(mix(amb_steer, mapped_steer, amb_map_w));
            }
        }
        // SKY SPECULAR, as a REALLOCATION of the ambient rather than an addition to it — the
        // second per-pixel mechanism, and the one that carries GRAZING surfaces and anything with
        // authored gloss (wet-ish stone, paint, glass, metal trim) where the cavity term is
        // weakest. See frame::sky_specular_split for the derivation; the two facts that matter
        // here are that the split conserves energy by construction (diffuse is scaled by exactly
        // the weight the specular took, so a sunlit wall cannot gain a single level of brightness
        // from it) and that it is roughness-aware, so it does not paint a Fresnel rim on every
        // silhouette in the scene.
        //
        // It rides INSIDE the same amb_ao * cavity multiply as the diffuse sky, not beside it: it
        // is the same sky arriving at the same point, so anything that occludes one occludes the
        // other. Letting it out of the occlusion is how crevices acquire glints.
        let view_dir_sky = normalize(-world_pos);
        // REFLECT-001: a classified surface reflects the sky through the env map in the caller
        // (shader3d.wgsl, reflect_environment), so its share here is zero -- not counted twice.
        var sky_spec = sky_specular_split(
            nrm, view_dir_sky, spec_power_to_gloss(m_spec_power), m_spec_mask);
        if (reflect_classified(mat, linear)) {
            sky_spec = vec4<f32>(0.0, 0.0, 0.0, 1.0);
        }
        // REN-GI-001: the probe volume's irradiance replaces the analytic sky-dome ambient
        // where the volume covers the point (weight fades at its edge; 0 with the feature
        // off, which makes this line the pre-GI expression exactly). The probes already saw
        // the roof and the terrain, so of the three sky occluders only the screen-space one
        // (GTAO) and the long-range terrain mask stay; the per-pixel interior-sky AO keeps a
        // tunable share, because a 4 m probe grid cannot resolve a small room.
        var ambient =
            (sky_irradiance(amb_n) * sky_spec.w + sky_spec.rgb) * frame.sun_ambient.w * amb_ao * cavity;
        let gi_s = gi_irradiance(world_abs, amb_n);
        if (gi_s.a > 0.0) {
            // Sinkhole W1b: the probes are lit by the sky too (a probe under the terrain reads the ground it
            // hits as sunlit), so GI ambient fades underground exactly like the sky ambient above.
            let gi_amb = gi_s.rgb * gtao_t * sky_vis_ao(world_abs.xz)
                * mix(1.0, interior_ao, frame.gi.z) * clamp(baked_sky_vis, 0.0, 1.0) * underground_amb;
            let gi_ambient = (gi_amb * sky_spec.w + sky_spec.rgb * amb_ao) * frame.sun_ambient.w * cavity;
            ambient = mix(ambient, gi_ambient, gi_s.a * frame.gi.y);
        }
        // Glass canopies: keep only a fraction of the sky wash so they read as glazing, not a lit
        // diffuse dome (the direct sun sheen + any glint still sit on top).
        // `is_translucent` now means "this draw carries MATFLAG_GLASS" -- the MAT-051 alpha
        // histogram (pctClear < 5, pctPartial > 80, aMean < 128, EngineWgpu.cpp:4956) -- and no
        // longer "this draw went through the alpha-blend pipeline". The old wording of this note
        // recorded the reason: the alpha classifier calls a texture BLEND from a soft border
        // alone, so a road sign's plate and, measured 2026-09-04, every original OFP/CWA road
        // surface (asf_new.paa: 11.5% clear, 10.4% partial, aMean 212) landed here and lost 80%
        // of its sky ambient as if it were a cockpit window. See shader3d.wgsl's call site.
        //
        // Kept from that note, because it is still true and still saves an afternoon: this term
        // is NOT what makes the Nogova road SIGNS dark. Measured 2026-08-29, setting the factor
        // to 1.0 left the sign exactly as dark. The signs are a separate fault.
        if (is_translucent) {
            ambient *= 0.2;
        }
        // MAT-052 second half -- cockpit interiors: keep only a fraction of the sky wash.
        // The interior is enclosed by the vehicle body, which no ambient occluder can see
        // (sky-vis is terrain-keyed, GTAO reaches ~2 m with the roof off-screen, interior
        // volumes are baked for buildings), so the full outdoor sky washed every interior
        // plate to near-sky brightness -- which against the sky reads as a TRANSPARENT
        // cockpit (owner: T72 and UH-60 alike, "fine looking down, transparent against
        // sky"). A DAMPED sky term rather than a branch switch: the first attempt routed
        // cockpit draws to the legacy flat ambient and turned every instrument face black
        // within the hour (dial materials carry no ambient of their own and live off this
        // term). 0.18 puts the UH-60 roof plate near the GL33 reference at noon while the
        // gauge faces keep their sky share.
        if (is_cockpit && !is_translucent) {
            ambient *= 0.18;
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
            // Daylight-only foliage fill: the daytime tuning must not survive
            // after the atmospheric sun has gone down. SUN-only, published by the CPU in
            // sun_dir_world.w -- sun_diffuse carries the moon too, and a full moon must not
            // read as 40% daylight here.
            //
            // FOLIAGE-DUSK. `daylight` is published from the SUN RADIANCE through a hard
            // (0.002 -> 0.04) ramp (EngineWgpu.cpp:2833-2837), and at a low sun the air mass
            // eats the transmittance -- so it collapses well BEFORE the geometric sunset. The
            // two lines below are the only foliage-only ambient terms in the renderer, so at
            // golden hour near-field leaves lost up to ~7x ambient while the terrain, houses
            // and rocks beside them kept theirs: trees went black and stood out. The night
            // purpose (a forest must not glow after dark) is kept EXACTLY, because pow(0, k)
            // is 0 for any k > 0 and pow(1, k) is 1 -- the curve only lifts the middle. k is
            // frame.foliagec.w (dev panel: Foliage > "Dusk curve"); 1.0 restores the old look
            // bit for bit, and a 0 lane (a renderer that predates this) also reads as 1.0.
            let daylight_raw = frame.sun_dir_world.w;
            var dusk_k = 1.0;
            if (kc.w > 0.0) { dusk_k = clamp(kc.w, 0.05, 1.0); }
            var daylight = 0.0;
            if (daylight_raw > 0.0) { daylight = pow(min(daylight_raw, 1.0), dusk_k); }
            ambient *= 1.0 + (kb.x - 1.0) * fade * daylight * (1.0 - tree_snow_cover);
            // The sky irradiance is tuned for daylight readability. Leaving its full
            // strength on a leaf/card at night makes entire forests appear self-lit,
            // even where no local source reaches them. Keep a small moon/sky floor
            // and blend continuously to the unmodified daylight response.
            ambient *= mix(0.35, 1.0, daylight);
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
            let trans = pow(clamp(dot(vdir, -lt), 0.0, 1.0), max(k.z, 1.0)) * k.x * mat.foliage_trans;
            // The SSS fill is a NEAR-FIELD effect (per-leaf translucency shouldn't read as a glow at
            // distance, and low-LOD billboards otherwise flatten into a bright sheet), so it shares
            // the distance fade above; the base Lambert stays so far foliage matches terrain.
            let fill = (wrap_fill + trans) * fade * (1.0 - tree_snow_cover);
            // RFG-189: a crown is not a solid. A leaf in its neighbours' shadow is still lit by the
            // sun that came through them -- diffusely, so it does not depend on this leaf's own
            // facing. Without it every shadowed leaf held sky light only, and a crown seen against
            // the sun read black where Reforger's glows green.
            // Transmitted light leaves a leaf on its FAR side: it is what you see looking toward the
            // sun, little of it from the sun's own side, where the leaves show reflected light. A
            // quarter stays everywhere -- light already scattered around inside the crown.
            let toward_sun = smoothstep(-0.3, 0.8, dot(vdir, -sl));
            let through = sun_csm_blocked * mat.leaf_transmission * mix(0.25, 1.0, toward_sun) *
                (1.0 - tree_snow_cover);
            sun = m_emissive + ambient + frame.sun_diffuse.rgb * ((front + fill) * sun_vis + through) * leaf_cavity;
        } else {
            sun = m_emissive + ambient + frame.sun_diffuse.rgb * ndotl * sun_vis;
        }
    } else {
        // The legacy (non-sky) path takes the cavity too — its ambient is a FLAT constant, so it
        // is the flattest ambient in the renderer and the one with the most to gain. It gets no
        // sky specular: there is no atmosphere on this path to reflect, and sky_irradiance would
        // be reading an SH set the legacy sun never wrote.
        sun = m_emissive + m_sun_ambient * amb_ao * cavity + m_sun_diffuse * ndotl * sun_vis * leaf_cavity;
    }
    let local = lights_material_contrib(world_pos, nrm, m_light_diffuse, m_light_ambient,
                                       m_local_specular, m_spec_power, linear);
    let raw = sun + local.diffuse;
    let lit = select(clamp(raw, vec3<f32>(0.0), vec3<f32>(1.0)), max(raw, vec3<f32>(0.0)), linear > 0.5);
    var rgb = albedo * lit;
    // REFLECT-001: a metal has no diffuse body; its colour is all in the reflection.
    if (reflect_classified(mat, linear)) {
        rgb = rgb * (1.0 - clamp(mat.refl_a.z, 0.0, 1.0)) + m_emissive * albedo * clamp(mat.refl_a.z, 0.0, 1.0);
    }
    // Object lighting diagnostics (WGR_OBJECT_DEBUG_VIEW=albedo|light): the albedo alone, or the
    // light it receives (sun + sky + local, before albedo) at 1/8 -- to tell a dark texture from a
    // dark light before reasoning about either.
#ifdef OBJECT_DEBUG_ALBEDO
    rgb = albedo;
#endif
#ifdef OBJECT_DEBUG_LIGHT
    rgb = lit * 0.125;
#endif
    rgb += select(clamp(local.specular, vec3<f32>(0.0), vec3<f32>(1.0)),
                  max(local.specular, vec3<f32>(0.0)), linear > 0.5);
    // Sun-only Blinn-Phong specular (untextured, additive, before the shadow multiply). The
    // camera is at the origin in camera-relative space, so view_dir = normalize(-world_pos).
    if (reflect_classified(mat, linear)) {
        // REFLECT-001 phase C: GGX base + clear coat instead of the legacy Blinn lobe (never both:
        // that is the double highlight). Raw sun colour, as the sky-lit diffuse above uses.
        let spec_r = reflect_sun_specular(mat, albedo, nrm, normalize(-world_pos), -frame.sun_dir_world.xyz)
            * frame.sun_diffuse.rgb * sun_vis;
        rgb += max(spec_r, vec3<f32>(0.0));
    } else if (m_spec_power > 0.0) {
        let view_dir = normalize(-world_pos);
        let half_vec = normalize(-frame.sun_dir_world.xyz + view_dir);
        let n_dot_h = max(dot(nrm, half_vec), 0.0);
        let spec = m_specular * pow(n_dot_h, max(m_spec_power, 1.0));
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
    if (mat.wet_cloth > 0.0 && linear > 0.5) {
        // Wet cloth holds water after rain/under a roof: retained person history
        // controls this layer; the actual lighting controls its visibility.
        let cloth_n = surface_normal(geo_normal, world_pos, dwx, dwy);
        let view = -world_pos / max(length(world_pos), 0.0001);
        let response = wet_textile_response(mat.wet_cloth, dot(cloth_n, view),
            max(length(dwx), length(dwy)));
        var coat_sky = vec3<f32>(0.0);
        if (sky_lit && !is_cockpit) {
            // Reflect the same actual linear sky/cloud radiance as water, with
            // a broad rough-fibre kernel. SH irradiance/PI is a diffuse lobe,
            // not this directional reflection; do not divide radiance by PI.
            let mirror = reflect(-view, cloth_n);
            let guide = select(vec3<f32>(0.0,1.0,0.0), vec3<f32>(1.0,0.0,0.0), abs(mirror.y) > 0.95);
            let tangent = normalize(cross(mirror, guide));
            let bitangent = cross(mirror, tangent);
            let spread = sqrt(8.0 / (response.z + 2.0));
            let directions = array<vec3<f32>,5>(mirror,
                normalize(mirror + tangent * spread), normalize(mirror - tangent * spread),
                normalize(mirror + bitangent * spread), normalize(mirror - bitangent * spread));
            var sky_weight = 0.0;
            for (var i = 0u; i < 5u; i += 1u) {
                let weight = select(1.0, 2.0, i == 0u) * max(dot(cloth_n, directions[i]), 0.0);
                coat_sky += ground_sky_reflection(directions[i]) * weight;
                sky_weight += weight;
            }
            coat_sky = coat_sky / max(sky_weight, 0.0001) * frame.sun_ambient.w * amb_ao * cavity;
        }
        let half_sum = view - frame.sun_dir_world.xyz;
        let half_vector = half_sum / max(length(half_sum), 0.0001);
        let sun_radiance = select(m_sun_diffuse, frame.sun_diffuse.rgb, sky_lit);
        let coat_sun = sun_radiance * wet_textile_sun_lobe(dot(cloth_n, half_vector),
            dot(cloth_n, -frame.sun_dir_world.xyz), response) * sun_vis;
        // Reuse the existing bounded point/spot attenuation and physical
        // spot/cube shadow lookup. Only actual wet-cloth pixels pay this pass.
        let coat_local = lights_material_contrib(world_pos, cloth_n, vec3<f32>(0.0),
            vec3<f32>(0.0), vec3<f32>(response.w), response.z, linear);
        // Reallocate the outgoing reflected share from the underlying layer.
        // Neutral water highlights do not inherit camouflage dye/texture grain.
        rgb = wet_textile_composite(rgb, coat_sky, coat_sun, coat_local.specular, response);
    }
    if (rain_puddle > 0.0) {
        let view = normalize(-world_pos);
        let water_n = ground_puddle_ripple_normal(world_abs.xz, frame.ground_weather.w,
            frame.ground_weather.y, ground_footprint);
        let grazing = 1.0 - clamp(dot(water_n, view), 0.0, 1.0);
        let fresnel = 0.02 + 0.98 * pow(grazing, 5.0);
        var reflected = m_sun_ambient;
        if (sky_lit) {
            reflected = ground_sky_reflection(reflect(-view, water_n)) * frame.sun_ambient.w
                * sky_vis_ao(world_abs.xz) * interior_sky_reach(world_abs) * cave.y;
        }
        let half_vector = normalize(view - frame.sun_dir_world.xyz + vec3<f32>(0.0, 1e-4, 0.0));
        reflected += frame.sun_diffuse.rgb * pow(max(dot(water_n, half_vector), 0.0), 96.0) * sun_vis;
        rgb = mix(rgb, rgb * (1.0 - fresnel) + reflected * fresnel, rain_puddle);
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
        rgb = apply_fog_receiver(rgb, world_pos, !is_cockpit && cave.y >= 0.999);
    } else {
        rgb = mix(fog_color, rgb, fog);
    }
    return rgb;
}
