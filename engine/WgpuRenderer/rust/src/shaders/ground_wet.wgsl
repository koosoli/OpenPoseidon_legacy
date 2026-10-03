#define_import_path ground_wet
#import rain_water_optics::rain_water_sun_specular

// Exact cultivated Noe tiles omit CfgSurfaces. Source identity is carried by
// bit2; only their unjittered central world-cell region can be soft soil.
fn cultivated_soil_fraction(flags: u32, world_cell_uv: vec2<f32>) -> f32 {
    if ((flags & 4u) == 0u) { return 0.0; }
    let edge = min(min(world_cell_uv.x, world_cell_uv.y),
                   min(1.0 - world_cell_uv.x, 1.0 - world_cell_uv.y));
    return smoothstep(0.12, 0.17, edge);
}

// Retained rainfall wets the entire proven soil surface, not just the random
// puddle patches. Snow and missing physical shelter proof leave it dry.
fn soil_wet_amount(soil: f32, wetness: f32, snow_depth: f32) -> f32 {
    return clamp(soil, 0.0, 1.0) * smoothstep(0.03, 0.65, clamp(wetness, 0.0, 1.0))
        * (1.0 - smoothstep(0.002, 0.02, snow_depth));
}

// x: diffuse substrate multiplier, y: remaining cosmetic smooth-film share.
// Call only after the existing source, snow and physical rain-proof gates.
// The ground's procedural mask has no water depth: on proven soft soil it is
// a moist rough substrate, not another smooth water interface. Actual retained
// pools are drawn separately from their supported depth and keep their optics.
fn soil_wet_surface(soil: f32, wet_amount: f32, film: f32) -> vec2<f32> {
    let wet = clamp(wet_amount, 0.0, 1.0);
    let source = select(0.0, clamp(soil, 0.0, 1.0), wet > 0.0);
    let smooth_film = clamp(film, 0.0, 1.0) * (1.0 - source);
    // Tidewater's accepted damp substrate darkens albedo by 0.58. Reuse that
    // bounded, channel-preserving response for the admitted wet-soil share;
    // never exempt a wet substrate because the procedural film mask is high.
    // Preserve the existing 0.38 film darkening on the remaining hard share.
    let diffuse = mix(1.0, 0.58, wet) * (1.0 - 0.38 * smooth_film);
    return vec2<f32>(diffuse, smooth_film);
}

// Broad, low-energy moist-soil response. Use the geometric normal: authored
// grit cannot turn sunlight into individual sparkles. No time/position noise.
fn soil_wet_fresnel(view_cosine: f32) -> f32 {
    let grazing = 1.0 - clamp(view_cosine, 0.0, 1.0);
    return 0.02 + 0.10 * pow(grazing, 5.0);
}
fn soil_wet_sun_lobe(half_cosine: f32) -> f32 {
    return 0.06 * pow(clamp(half_cosine, 0.0, 1.0), 8.0);
}

// Direct-sun BRDF times N.L, in inverse steradians. The shared Tidewater water
// interface includes dielectric Fresnel exactly once and normalized GGX/Smith
// masking. Rain smooths the moist coat continuously from slope width alpha=.4
// to .22; actual geometric-normal derivatives widen that still-broad minimum.
// No grain/time noise, artificial gloss gain or added unshadowed light.
// Input wet_amount is the caller's source/snow/physical-rain-admitted fraction;
// visibility is its actual sun shadow/cloud/sky visibility, never a gloss knob.
fn soil_wet_sun_specular(ndv: f32, ndl: f32, ndh: f32, vdh: f32,
                         normal_variation: f32, wet_amount: f32, visibility: f32) -> f32 {
    if (wet_amount <= 0.0 || visibility <= 0.0) { return 0.0; }
    let alpha = soil_wet_sun_alpha(wet_amount);
    // Remove the shared helper's .0064 floor before adding the real normal
    // variance, so its final GGX alpha² is alpha² + derivative variance.
    let variance = sqrt(alpha * alpha - 0.0064
        + max(normal_variation, 0.0) * max(normal_variation, 0.0));
    return rain_water_sun_specular(ndv, ndl, ndh, vdh, variance)
        * clamp(wet_amount, 0.0, 1.0) * clamp(visibility, 0.0, 1.0);
}

fn soil_wet_sun_alpha(wet_amount: f32) -> f32 {
    return mix(0.4, 0.22, clamp(wet_amount, 0.0, 1.0));
}

// Local tangent-plane sky visibility, filtered by the actual pixel derivative.
// This is not a terrain ray hit: the existing broad terrain normal defines the
// approximate horizon. Below it, atmosphere background padding is not sky light.
fn soil_wet_sky_fraction(horizon_cosine: f32, pixel_width: f32) -> f32 {
    let width = max(abs(pixel_width), 0.000001);
    return clamp(0.5 + 0.5 * horizon_cosine / width, 0.0, 1.0);
}
fn soil_wet_ground_reflection(sky: vec3<f32>, substrate: vec3<f32>,
                              horizon_cosine: f32, pixel_width: f32) -> vec3<f32> {
    // The already-lit/darkened substrate approximates nearby diffuse-ground
    // radiance for locally blocked rays. No invented hit, tint or extra light.
    // A convex replacement retains HDR energy and the caller's Fresnel split.
    return mix(substrate, sky, soil_wet_sky_fraction(horizon_cosine, pixel_width));
}

// Eight deterministic importance samples of the same GGX slope distribution
// as the direct-sun coat. Stratify its radial CDF in four antipodal pairs;
// normal-space samples have no world-position/time noise or view-axis rotation.
// The NDF sampling PDF is D*N.H/(4*V.H) in reflected-direction space. Cancelling
// it against BRDF*N.L gives 4*N.L*V_GGX*V.H/N.H. Fresnel stays in the caller's
// existing energy partition, so only this nonnegative directional kernel is
// normalised: this is a bounded radiance convolution, not a new BRDF amplitude.
fn soil_wet_environment_samples(view: vec3<f32>, n: vec3<f32>, wet_amount: f32,
                                normal_variation: f32) -> array<vec4<f32>, 8> {
    var samples: array<vec4<f32>, 8>;
    let nv = dot(n, view);
    if (nv <= 0.0) { return samples; }
    let alpha = soil_wet_sun_alpha(wet_amount);
    let variance = max(normal_variation, 0.0);
    let alpha2 = clamp(alpha * alpha + variance * variance, 0.0064, 0.5);
    // Orthonormal y-up Frisvad basis, continuous on the upper terrain hemisphere.
    let sign_y = select(-1.0, 1.0, n.y >= 0.0);
    let a = -1.0 / (sign_y + n.y);
    let b = n.x * n.z * a;
    let tangent = vec3<f32>(1.0 + sign_y*n.x*n.x*a, -sign_y*n.x, sign_y*b);
    let bitangent = vec3<f32>(b, -n.z, sign_y + n.z*n.z*a);
    var total = 0.0;
    for (var i=0u; i<8u; i+=1u) {
        let ring = f32(i/2u);
        let u = (ring + 0.5) / 4.0;
        let slope = sqrt(alpha2 * u / (1.0-u));
        let phi = (ring*0.25 + f32(i%2u)) * 3.14159265359;
        let h = normalize(n + slope*(tangent*cos(phi) + bitangent*sin(phi)));
        let vh = dot(view, h);
        let direction = reflect(-view, h);
        let nl = dot(n, direction);
        var weight = 0.0;
        if (vh > 0.0 && nl > 0.0) {
            let gv = nl * sqrt(nv*nv*(1.0-alpha2) + alpha2);
            let gl = nv * sqrt(nl*nl*(1.0-alpha2) + alpha2);
            let visibility = 0.5 / max(gv+gl, 1e-5);
            weight = 4.0*nl*visibility*vh / max(dot(n,h), 1e-5);
        }
        samples[i] = vec4<f32>(direction, weight);
        total += weight;
    }
    if (total > 0.0) {
        for (var i=0u; i<8u; i+=1u) { samples[i].w /= total; }
    }
    return samples;
}
