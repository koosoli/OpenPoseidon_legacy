#define_import_path snow_material

// Snow is a diffuse powder. Filter every procedural band by the WORLD pixel
// footprint; no time/camera seed, specular grains or unresolved sparkle dots.
fn snow_band(footprint: f32, wavelength: f32) -> f32 {
    return 1.0 - smoothstep(wavelength * 0.18, wavelength * 0.5, footprint);
}

fn snow_hash(cell: vec2<i32>) -> f32 {
    var h = bitcast<u32>(cell.x) * 1597334677u ^ bitcast<u32>(cell.y) * 3812015801u;
    h = (h ^ (h >> 16u)) * 2246822519u;
    h = (h ^ (h >> 13u)) * 3266489917u;
    h = h ^ (h >> 16u);
    return f32(h & 0x00ffffffu) * (2.0 / 16777215.0) - 1.0;
}

// Continuous value noise and its analytic gradient. Integer hashing keeps
// absolute-world seeds stable; the rotated bands avoid parallel snow ridges.
fn snow_noise(p: vec2<f32>) -> vec3<f32> {
    let cell = vec2<i32>(floor(p));
    let f = fract(p);
    let u = f * f * (3.0 - 2.0 * f);
    let du = 6.0 * f * (1.0 - f);
    let a = snow_hash(cell);
    let b = snow_hash(cell + vec2<i32>(1, 0));
    let c = snow_hash(cell + vec2<i32>(0, 1));
    let d = snow_hash(cell + vec2<i32>(1, 1));
    let lower = mix(a, b, u.x);
    let upper = mix(c, d, u.x);
    return vec3<f32>(mix(lower, upper, u.y),
        mix(b - a, d - c, u.y) * du.x, (upper - lower) * du.y);
}

fn snow_powder_albedo(world: vec3<f32>, footprint: f32, compacted: f32) -> vec3<f32> {
#ifdef DISABLE_SNOW_POWDER_DETAIL
    return vec3<f32>(0.78, 0.80, 0.82) * (1.0 - clamp(compacted, 0.0, 1.0) * 0.28);
#else
    if (footprint >= 1.5) {
        return vec3<f32>(0.78, 0.80, 0.82) * (1.0 - clamp(compacted, 0.0, 1.0) * 0.28);
    }
    let p = world.xz + world.y * vec2<f32>(0.17, -0.11);
    let broad = snow_noise(vec2<f32>(dot(p, vec2<f32>(0.83, -0.56)), dot(p, vec2<f32>(0.56, 0.83))) / 3.0).x * snow_band(footprint, 3.0);
    let powder = snow_noise(vec2<f32>(dot(p, vec2<f32>(0.39, 0.92)), dot(p, vec2<f32>(-0.92, 0.39))) / 0.6 + vec2<f32>(17.3, -41.7)).x * snow_band(footprint, 0.6);
    let fine = snow_noise(vec2<f32>(dot(p, vec2<f32>(0.71, -0.71)), dot(p, vec2<f32>(0.71, 0.71))) / 0.12 + vec2<f32>(-11.8, 39.2)).x * snow_band(footprint, 0.12);
    let granular = snow_noise(vec2<f32>(dot(p, vec2<f32>(0.93, 0.37)), dot(p, vec2<f32>(-0.37, 0.93))) / 0.035 + vec2<f32>(53.1, 8.7)).x * snow_band(footprint, 0.035);
    // Nearly neutral fresh reflectance: sun/sky provide the warm light and blue
    // shade. Resolved powder has diffuse grain, never specular sparkle. Every
    // band converges to the exact mean, keeping distant snow stable.
    return vec3<f32>(0.78, 0.80, 0.82)
        * (1.0 + broad * 0.018 + powder * 0.025 + fine * 0.030 + granular * 0.012)
        * (1.0 - clamp(compacted, 0.0, 1.0) * 0.28);
#endif
}

fn snow_powder_normal(world: vec3<f32>, base: vec3<f32>, footprint: f32) -> vec3<f32> {
#ifdef DISABLE_SNOW_POWDER_DETAIL
    return base;
#else
    if (footprint >= 1.5) { return base; }
    let p = world.xz + world.y * vec2<f32>(0.17, -0.11);
    let broad = snow_noise(vec2<f32>(dot(p, vec2<f32>(0.83, -0.56)), dot(p, vec2<f32>(0.56, 0.83))) / 3.0);
    let powder = snow_noise(vec2<f32>(dot(p, vec2<f32>(0.39, 0.92)), dot(p, vec2<f32>(-0.92, 0.39))) / 0.6 + vec2<f32>(17.3, -41.7));
    let fine = snow_noise(vec2<f32>(dot(p, vec2<f32>(0.71, -0.71)), dot(p, vec2<f32>(0.71, 0.71))) / 0.12 + vec2<f32>(-11.8, 39.2));
    let granular = snow_noise(vec2<f32>(dot(p, vec2<f32>(0.93, 0.37)), dot(p, vec2<f32>(-0.37, 0.93))) / 0.035 + vec2<f32>(53.1, 8.7));
    let gradient = (broad.y * vec2<f32>(0.83, -0.56) + broad.z * vec2<f32>(0.56, 0.83)) * 0.018 * snow_band(footprint, 3.0)
        + (powder.y * vec2<f32>(0.39, 0.92) + powder.z * vec2<f32>(-0.92, 0.39)) * 0.040 * snow_band(footprint, 0.6)
        + (fine.y * vec2<f32>(0.71, -0.71) + fine.z * vec2<f32>(0.71, 0.71)) * 0.036 * snow_band(footprint, 0.12)
        + (granular.y * vec2<f32>(0.93, 0.37) + granular.z * vec2<f32>(-0.37, 0.93)) * 0.024 * snow_band(footprint, 0.035);
    let perturb = vec3<f32>(gradient.x, 0.0, gradient.y);
    let tangent = perturb - base * dot(perturb, base);
    // At most atan(.16) ~= 9.1 degrees: visual powder roughness must not make
    // flat terrain/roof normals look like sharp displaced spikes.
    let bounded = tangent / max(1.0, length(tangent) / 0.16);
    return normalize(base + bounded);
#endif
}

// Owner, 2026-10-19: "give the snow more definition (not just white)". Lying snow is shaped by
// the wind: drifts several metres long stretched across the wind, and in patches ripples about
// 0.85 m apart with a gentle windward face and a steeper lee. Returns (relief -1..1, slope x,
// slope z): relief for the albedo (troughs hold bluer, shaded snow), slope for the normal. Every
// band fades with the pixel footprint like the powder bands, so far snow stays calm.
fn snow_wind_relief(xz: vec2<f32>, footprint: f32) -> vec3<f32> {
    if (footprint >= 3.0) { return vec3<f32>(0.0); }
    let wind = vec2<f32>(0.80, 0.60);
    let across = vec2<f32>(-0.60, 0.80);
    // drifts: 5 m along the wind, 14 m across it, ~0.25 m high
    let dune = snow_noise(vec2<f32>(dot(xz, wind) / 5.0, dot(xz, across) / 14.0) + vec2<f32>(31.7, -12.9));
    let dune_band = snow_band(footprint, 5.0);
    var slope = (wind * (dune.y / 5.0) + across * (dune.z / 14.0)) * 0.25 * dune_band;
    var relief = dune.x * 0.6 * dune_band;
    let ripple_band = snow_band(footprint, 0.85);
    if (ripple_band > 0.0) {
        // ripples only in patches, crests that wander and break off
        let patch_n = smoothstep(-0.25, 0.45, snow_noise(xz / 9.0 + vec2<f32>(-21.3, 5.1)).x);
        let crest = 0.55 + 0.45 * snow_noise(vec2<f32>(dot(xz, across) / 1.7, dot(xz, wind) / 4.0) + vec2<f32>(7.3, 2.9)).x;
        let warp = snow_noise(xz / 7.0 + vec2<f32>(3.1, -7.7)).x * 1.2 + snow_noise(xz / 2.3 + vec2<f32>(-9.4, 4.4)).x * 0.25;
        let ph = (dot(xz, wind) + warp) * (6.2831853 / 0.85);
        let h = sin(ph) + 0.3 * sin(2.0 * ph);
        let dh = (cos(ph) + 0.6 * cos(2.0 * ph)) * (6.2831853 / 0.85);
        let amp = 0.010 * patch_n * crest * ripple_band;
        slope += wind * dh * amp;
        relief += h * 0.4 * patch_n * crest * ripple_band;
    }
    return vec3<f32>(clamp(relief, -1.0, 1.0), slope);
}

fn snow_surface_coverage(depth: f32, normal_up: f32, rain_reach: f32) -> f32 {
    return smoothstep(0.002, 0.04, depth)
        * smoothstep(0.35, 0.7, normal_up)
        * smoothstep(0.6, 0.95, rain_reach);
}

// BOOM-003 (owner, 2026-10-19: "little ice glitter in the snow"): ice crystals in the surface,
// as glints, not as dots. Each crystal is a world-anchored cell with its own facet tilt and
// flashes only where that facet mirrors the sun into the eye (half-vector lobe), so the
// sparkle moves as the viewer moves, like real snow, and sits still on a still camera. Two
// crystal sizes; each layer fades out once a crystal is smaller than the pixel (footprint),
// so distant snow never shimmers -- the rule the powder bands above follow.
fn snow_glint_layer(p: vec2<f32>, cell: f32, density: f32, view: vec3<f32>, to_sun: vec3<f32>,
                    footprint: f32) -> vec3<f32> {
    let resolve = 1.0 - smoothstep(cell * 0.6, cell * 2.0, footprint);
    if (resolve <= 0.0) { return vec3<f32>(0.0); }
    let c = vec2<i32>(floor(p / cell));
    if (snow_hash(c) * 0.5 + 0.5 > density) { return vec3<f32>(0.0); }
    let h1 = snow_hash(c + vec2<i32>(37, 11));
    let h2 = snow_hash(c + vec2<i32>(-23, 71));
    // facets tilted up to ~35 degrees: a spread of crystals catches the sun from many angles
    let facet = normalize(vec3<f32>(h1 * 0.7, 1.0, h2 * 0.7));
    let half_vector = normalize(view + to_sun);
    // a slightly wider lobe: each crystal catches the sun over a few degrees of head movement
    let spec = pow(max(dot(facet, half_vector), 0.0), 600.0);
    let f = fract(p / cell) - vec2<f32>(0.5) - vec2<f32>(h1, h2) * 0.2;
    let spot = 1.0 - smoothstep(0.18, 0.42, length(f));
    // ice splits the sun a little: most glints are white, some lean blue, gold or rose
    let h3 = snow_hash(c + vec2<i32>(91, -53));
    let tint = mix(vec3<f32>(1.0), vec3<f32>(0.75 + 0.25 * h1, 0.85 + 0.15 * h3, 1.0 - 0.25 * h2), 0.6);
    return tint * (spec * spot * resolve);
}

// Sun-relative glint strength (multiply by the sun's irradiance). `world_xz` absolute.
fn snow_glitter(world_xz: vec2<f32>, view: vec3<f32>, to_sun: vec3<f32>, footprint: f32) -> vec3<f32> {
    if (to_sun.y <= 0.0) { return vec3<f32>(0.0); }
    // local coordinates: f32 keeps no fraction at a 2 cm cell ten kilometres out
    let p = world_xz - floor(world_xz / 512.0) * 512.0;
    // (owner, 2026-10-19: "make the snow glitter stronger") brighter, denser, and a coarse third
    // layer so the sparkle carries to ~30-40 m; each layer still fades once sub-pixel
    return 150.0 * snow_glint_layer(p, 0.02, 0.16, view, to_sun, footprint)
         + 80.0 * snow_glint_layer(p + vec2<f32>(3.1, 7.7), 0.06, 0.08, view, to_sun, footprint)
         + 45.0 * snow_glint_layer(p + vec2<f32>(-5.3, 2.9), 0.15, 0.035, view, to_sun, footprint);
}
