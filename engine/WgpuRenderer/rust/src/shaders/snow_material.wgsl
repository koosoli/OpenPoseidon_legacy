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

fn snow_surface_coverage(depth: f32, normal_up: f32, rain_reach: f32) -> f32 {
    return smoothstep(0.002, 0.04, depth)
        * smoothstep(0.35, 0.7, normal_up)
        * smoothstep(0.6, 0.95, rain_reach);
}
