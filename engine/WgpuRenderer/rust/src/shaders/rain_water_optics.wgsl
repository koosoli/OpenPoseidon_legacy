#define_import_path rain_water_optics

fn rain_water_impact_hash(cell: vec2<i32>, cycle: i32, salt: u32) -> f32 {
    let q = vec2<u32>(cell);
    var h = q.x * 1597334677u ^ q.y * 3812015801u ^ u32(cycle) * 2798796415u ^ salt;
    h = (h ^ (h >> 16u)) * 2246822519u;
    return f32(h ^ (h >> 13u)) / 4294967295.0;
}

// Independent world-space rain events, rather than one centred circle repeating
// at the same rate in every cell. The 3x3 neighbourhood lets packets cross cell
// boundaries continuously. Maximum radius .29m < .75m cell width bounds the
// neighbourhood; an event is fully faded before its next centre is selected.
fn rain_water_impact_normal(world_xz: vec2<f32>, time: f32, rain: f32, footprint: f32) -> vec3<f32> {
    if (rain <= 0.0 || footprint >= 0.16) { return vec3<f32>(0.0, 1.0, 0.0); }
    let cell = vec2<i32>(floor(world_xz / 0.75));
    let detail = 1.0 - smoothstep(0.035, 0.16, max(footprint, 0.0));
    let width = max(0.022, max(footprint, 0.0) * 1.5);
    var gradient = vec2<f32>(0.0);
    for (var y = -1; y <= 1; y += 1) {
        for (var x = -1; x <= 1; x += 1) {
            let source = cell + vec2<i32>(x, y);
            let period = 1.4 + rain_water_impact_hash(source, 0, 11u) * 1.3;
            let clock = time / period + rain_water_impact_hash(source, 0, 29u);
            let cycle = i32(floor(clock));
            let lifetime = 0.55 + rain_water_impact_hash(source, cycle, 47u) * 0.5;
            let age = fract(clock) * period / lifetime;
            if (age >= 1.0) { continue; }
            let jitter = vec2<f32>(rain_water_impact_hash(source, cycle, 71u),
                                  rain_water_impact_hash(source, cycle, 97u));
            let centre = (vec2<f32>(source) + vec2<f32>(0.1) + jitter * 0.8) * 0.75;
            let offset = world_xz - centre;
            let radius = length(offset);
            if (radius >= 0.34) { continue; }
            let delta = radius - mix(0.025, 0.29, age);
            let pulse = exp(-delta * delta / (width * width));
            let envelope = smoothstep(0.0, 0.10, age) * (1.0 - smoothstep(0.60, 1.0, age));
            let radial = 1.0 - smoothstep(0.30, 0.34, radius);
            let strength = 0.10 + rain_water_impact_hash(source, cycle, 131u) * 0.12;
            // Width-compensated slope: unresolved rings fade instead of gaining
            // energy when widened by the pixel footprint.
            let slope = delta / width * pulse * envelope * radial * strength
                * min(1.0, 0.022 / width) * detail * clamp(rain, 0.0, 1.0);
            gradient += offset / max(radius, 0.0001) * slope;
        }
    }
    return normalize(vec3<f32>(-gradient.x, 1.0, -gradient.y));
}

// The same dielectric Fresnel / GGX D / Smith V equations as Tidewater's
// sun lobe. The broad minimum slope width and derivative variance keep a
// filtered water surface from resolving into isolated sparkling pixels.
fn rain_water_sun_specular(ndv: f32, ndl: f32, ndh: f32, vdh: f32, normal_variation: f32) -> f32 {
    if (ndv <= 0.0 || ndl <= 0.0) { return 0.0; }
    let c = clamp(vdh, 0.0, 1.0);
    let eta = 1.333;
    let g = sqrt(eta * eta - 1.0 + c * c);
    let a = (g - c) / (g + c);
    let b = (c * (g + c) - 1.0) / (c * (g - c) + 1.0);
    let fresnel = 0.5 * a * a * (b * b + 1.0);
    let alpha2 = clamp(0.0064 + normal_variation * normal_variation, 0.0064, 0.5);
    let d = ndh * ndh * (alpha2 - 1.0) + 1.0;
    let distribution = alpha2 / (d * d * 3.14159265359);
    let gv = ndl * sqrt(ndv * ndv * (1.0 - alpha2) + alpha2);
    let gl = ndv * sqrt(ndl * ndl * (1.0 - alpha2) + alpha2);
    let visibility = 0.5 / max(gv + gl, 1e-5);
    return distribution * visibility * fresnel * ndl;
}

// Metres of ACTUAL supported local water depth. This is optical roughness,
// never a simulation/admission cutoff: a millimetre film still transmits the
// already-lit terrain and has its Beer-Lambert body. Below 8mm the substrate's
// grit/relief controls the wet response; by 50mm it is a standing-water mirror.
fn rain_water_standing_fraction(local_depth: f32) -> f32 {
    return smoothstep(0.008, 0.050, max(local_depth, 0.0));
}

fn rain_water_surface_optics(local_depth: f32, view_cosine: f32, reflected: vec3<f32>,
                             body: vec3<f32>, coverage: f32) -> vec4<f32> {
    return rain_water_surface_optics_extinction(local_depth,view_cosine,reflected,body,coverage,3.0);
}

fn rain_water_surface_optics_extinction(local_depth: f32, view_cosine: f32, reflected: vec3<f32>,
                             body: vec3<f32>, coverage: f32, extinction: f32) -> vec4<f32> {
    if (local_depth <= 0.0 || coverage <= 0.0) { return vec4<f32>(0.0); }
    let mu = clamp(view_cosine, 0.0, 1.0);
    // The bed has already received its broad rough wet-soil/puddle shading.
    // Do not paint a second smooth interface over a rough millimetre film.
    // Continuous depth handoff replaces that response with pooled reflection.
    let fresnel = rain_water_standing_fraction(local_depth)
        * (0.025 + 0.975 * pow(1.0 - mu, 5.0));
    let transmission = exp(-max(extinction,0.0) * min(local_depth, 64.0));
    let scatter_weight = (1.0 - fresnel) * (1.0 - transmission);
    let opacity = fresnel + scatter_weight;
    if (opacity <= 0.0) { return vec4<f32>(0.0); }
    // Straight alpha: reflection + body + transmitted destination sum to one.
    // No global grade, HDR clamp, alpha floor or instantaneous-rain dependence.
    let colour = (max(reflected, vec3<f32>(0.0)) * fresnel
        + max(body, vec3<f32>(0.0)) * scatter_weight) / opacity;
    return vec4<f32>(colour, opacity * clamp(coverage, 0.0, 1.0));
}
