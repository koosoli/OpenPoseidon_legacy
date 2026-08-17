// Procedural atmospheric sky — Hillaire-style LUT model (plan Stage 2a). One shader
// module with a shared atmosphere and three fragment entry points:
//   fs_transmittance : builds the transmittance LUT   T(r, mu_sun)     (params-only)
//   fs_multiscatter  : builds the multi-scattering LUT Psi(r, mu_sun)  (params-only)
//   fs_sky           : the fullscreen sky — marches the view ray, samples both LUTs
//                      for sun transmittance + multiscatter, adds a transmittance-
//                      attenuated sun disc, night-faded horizon haze, and outputs
//                      linear radiance (HDR) or self-tonemaps (LDR-direct).
// The LUTs depend only on atmosphere params (the sun is a LUT axis), so they rebuild
// only when those change. See docs/procedural-sky-plan.md §3 (Stage 2) and §8.

struct Sky {
    inv_view_proj: mat4x4<f32>,
    sun_dir: vec4<f32>,       // xyz = dir TO sun, w = sun radiance scale
    moon_dir: vec4<f32>,      // xyz = dir TO moon, w = illuminated fraction (0 new .. 1 full)
    rayleigh: vec4<f32>,      // xyz = Rayleigh beta (1/m), w = Rayleigh scale height (m)
    mie: vec4<f32>,           // x = Mie beta, y = Mie g, z = Mie scale height (m), w = turbidity
    ground_albedo: vec4<f32>, // xyz = ground albedo, w = night factor
    params: vec4<f32>,        // x = sun angular radius (rad), y = exposure, z = planet radius (m), w = atmosphere (m)
    control: vec4<f32>,       // x = enabled, y = view samples, z = light samples, w = ozone strength
    fog_color: vec4<f32>,     // xyz = scene fog colour, w = horizon-haze strength
    night_zenith: vec4<f32>,  // xyz = night radiance at zenith, w = camera altitude ASL (m)
    night_horizon: vec4<f32>, // xyz = night radiance at horizon
    night_params: vec4<f32>,  // x = full-day sun.y, y = full-night sun.y, z = intensity, w = far-fade range (m)
    cloud0: vec4<f32>,        // x = coverage [0,1], y = extinction (1/m), z = cloud bottom (m ASL), w = cloud top (m ASL)
    cloud1: vec4<f32>,        // x/y = wind WORLD offset (m, CPU-wrapped, runtime), z = shape scale (1/m), w = detail scale (1/m)
    cloud2: vec4<f32>,        // x = HG forward g, y = powder strength, z = ambient scale, w = max march distance (m)
    cloud3: vec4<f32>,        // x = weather scale (1/m), y = weather amount, z = warp scale (1/m), w = warp amount (m)
    // Evolution offsets (m, runtime, CPU-wrapped): x = shape, y = detail, z = weather drift.
    // Applied on the noise volume's Y axis, which the horizontal lookups otherwise hold fixed —
    // so the field MORPHS in place instead of sliding, i.e. clouds form and dissolve rather than
    // merely translating with the wind. w = the cirrus layer's live PUFFINESS (0..1) — its base
    // setting already combined with the time variation and eased; unlike xyz that lane is written
    // by the renderer, not by C++ (which sends 0 there). See cirrus_shape().
    cloud4: vec4<f32>,
    // x = linear output (1) vs self-tonemap (0), y = star intensity, z = lens-flare gain,
    // w = second cloud layer (high cirrus): 0 = off, 1 = flat sheet, 2 = volumetric shell march.
    output: vec4<f32>,
    cam_pos: vec4<f32>,       // xyz = ABSOLUTE world camera position (froxel -> terrain-mask lookup)
    // CLD-020 cloud sun-transmittance map. xy = world-xz of the map's min corner (SNAPPED to the
    // texel grid on the CPU, so the field does not crawl as the camera moves), z = 1/span in
    // metres, w = strength (0 = the pass is skipped entirely and every surface reads fully lit).
    cloud_shadow: vec4<f32>,
    // Moon disc. x = angular RADIUS (rad), y = illuminated fraction, z = disc radiance
    // scale (irradiance; divided by the solid angle in moon_disc), w = draw (0 = skip).
    moon_params: vec4<f32>,
    // xyz = unit dir TO the sun as seen from the MOON; w = earthshine reflectance floor.
    moon_sun: vec4<f32>,
};

@group(0) @binding(0) var<uniform> sky: Sky;
@group(0) @binding(1) var lut_sampler: sampler;
@group(0) @binding(2) var transmittance_lut: texture_2d<f32>;
@group(0) @binding(3) var multiscatter_lut: texture_2d<f32>;
// Tileable 3D cloud noise (Repeat sampler): R = low-freq shape fBm, G = higher-freq detail. Only
// fs_sky / fs_sky_env sample it. (binding 5 is the froxel storage image, declared further down.)
@group(0) @binding(4) var cloud_noise: texture_3d<f32>;
@group(0) @binding(6) var cloud_samp: sampler;
// CLD-020 output: sun transmittance through the cloud deck, one texel per world square.
@group(0) @binding(7) var cloud_shadow_out: texture_storage_2d<rgba8unorm, write>;

// Smoke -> ground shadow. Each blob is one smoke particle; cs_cloud_shadow
// projects it to the ground ALONG THE SUN RAY and darkens the texel it lands
// on. Riding the cloud map means every lit surface -- terrain, buildings,
// vehicles -- picks up smoke shadow through the sample it already does, with
// no per-shader change. Texel is ~8 m, so this is a soft ground shadow for a
// plume, not a crisp one for a puff.
struct SmokeBlob {
    pos_radius: vec4<f32>, // xyz world position, w radius (m)
    density: vec4<f32>,    // x optical density; y = height above ground (m)
};
struct SmokeShadowHeader {
    count: u32,
    strength: f32,
    _pad0: f32,
    _pad1: f32,
};
@group(0) @binding(8) var<storage, read> smoke_header: SmokeShadowHeader;
@group(0) @binding(9) var<storage, read> smoke_blobs: array<SmokeBlob>;

// Transmittance through the smoke above ground texel `world_xz`, along `sun`
// (the direction light TRAVELS, i.e. pointing down). 1 = clear.
fn smoke_ground_shadow(world_xz: vec2<f32>, sun: vec3<f32>, texel: f32) -> f32 {
    let count = smoke_header.count;
    if (count == 0u || smoke_header.strength <= 0.0 || sun.y >= -0.02) {
        return 1.0;
    }
    var tau = 0.0;
    for (var i = 0u; i < count; i = i + 1u) {
        let b = smoke_blobs[i];
        let p = b.pos_radius.xyz;
        let r = b.pos_radius.w;
        // Slide the particle down the sun ray by its height above ground.
        let h = max(b.density.y, 0.0);
        let t = h / max(-sun.y, 1e-3);
        let foot = p.xz + sun.xz * t;
        let d = length(world_xz - foot);
        // Soft disc, widened to at least most of a texel so a small puff still
        // lands rather than falling between samples.
        // The shadow of a soft puff reads wider than its dense core: 1.3x the
        // visual radius, and never narrower than most of a texel.
        let rr = max(r * 1.3, texel * 0.6);
        if (d < rr) {
            let w = 0.5 + 0.5 * cos(3.14159265 * d / rr);
            tau = tau + b.density.x * w;
        }
    }
    return exp(-tau * smoke_header.strength);
}

const PI: f32 = 3.14159265359;
const TRANSMITTANCE_STEPS: f32 = 40.0;
const MS_SQRT_SAMPLES: i32 = 8;        // 8x8 = 64 sphere directions
const MS_STEPS: f32 = 20.0;
// Mie extinction ~ 1.11x its scattering (a little absorption in the aerosol layer).
const MIE_EXT: f32 = 1.11;
// Ozone absorption coefficients (1/m at peak density; Earth values). Absorbs green
// and red far more than blue, which is what keeps twilight/zenith blue.
const OZONE_ABSORPTION: vec3<f32> = vec3<f32>(0.650e-6, 1.881e-6, 0.085e-6);

// Ozone density: absorption-only tent peaking mid-atmosphere, scaled to the
// atmosphere thickness (Earth: ~25 km peak, ~15 km half-width in a 60 km shell).
fn ozone_density(alt: f32) -> f32 {
    let atmos_h = sky.params.w;
    return max(0.0, 1.0 - abs(alt - atmos_h * 0.417) / (atmos_h * 0.25));
}

struct VsOut {
    @builtin(position) clip: vec4<f32>,
    @location(0) uv: vec2<f32>,
    @location(1) ray_dir: vec3<f32>,
};

@vertex
fn vs_main(@builtin(vertex_index) vi: u32) -> VsOut {
    let uv = vec2<f32>(f32((vi << 1u) & 2u), f32(vi & 2u));
    let ndc = uv * vec2<f32>(2.0, -2.0) + vec2<f32>(-1.0, 1.0);
    var out: VsOut;
    out.clip = vec4<f32>(ndc, 0.0, 1.0);
    out.uv = uv;
    // World view ray (camera at origin — the view matrix has no translation).
    // Unproject at the NEAR plane (forward NDC z = 0), not the far plane: with an
    // infinite-far projection NDC z = 1 is the point at infinity, where w -> 0 and the
    // divide explodes into NaNs (black sky). Any finite depth on the ray gives the same
    // direction since the camera sits at the origin, so the near plane is the safe pick.
    let world = sky.inv_view_proj * vec4<f32>(ndc, 0.0, 1.0);
    out.ray_dir = world.xyz / world.w;
    return out;
}

// Ray/sphere intersection (sphere centred at the planet origin, radius `radius`).
// Returns near/far parametric distances; x > y means a miss.
fn ray_sphere(origin: vec3<f32>, dir: vec3<f32>, radius: f32) -> vec2<f32> {
    let b = dot(origin, dir);
    let c = dot(origin, origin) - radius * radius;
    let d = b * b - c;
    if (d < 0.0) {
        return vec2<f32>(1.0, -1.0);
    }
    let s = sqrt(d);
    return vec2<f32>(-b - s, -b + s);
}

struct Medium {
    rayleigh: vec3<f32>,   // Rayleigh scattering coeff at this altitude
    mie: f32,              // Mie scattering coeff
    extinction: vec3<f32>, // total extinction (scattering + Mie absorption)
};

fn scattering_values(pos: vec3<f32>) -> Medium {
    let planet_r = sky.params.z;
    let alt = length(pos) - planet_r;
    let rho_r = exp(-alt / sky.rayleigh.w);
    let rho_m = exp(-alt / max(sky.mie.z, 1.0));
    var m: Medium;
    m.rayleigh = sky.rayleigh.xyz * rho_r;
    m.mie = sky.mie.x * rho_m;
    // Ozone (absorption only) keeps twilight/zenith blue — without it the reddened
    // low-sun light re-scatters sickly green. control.w scales it (blue-hour knob).
    m.extinction = m.rayleigh + vec3<f32>(m.mie * MIE_EXT)
        + OZONE_ABSORPTION * ozone_density(alt) * sky.control.w;
    return m;
}

// Non-linear cos-angle <-> texture-U mapping that concentrates LUT resolution near
// the horizon (mu = 0), where transmittance changes fastest and the long grazing
// ozone path that makes the blue hour lives. A linear axis under-samples it and the
// twilight blue falls between texels. Inverse pair.
fn mu_to_u(mu: f32) -> f32 {
    return sign(mu) * sqrt(abs(mu)) * 0.5 + 0.5;
}
fn u_to_mu(u: f32) -> f32 {
    let x = u * 2.0 - 1.0;
    return sign(x) * x * x;
}

fn tlut_uv(pos: vec3<f32>, dir: vec3<f32>) -> vec2<f32> {
    let planet_r = sky.params.z;
    let top_r = planet_r + sky.params.w;
    let h = length(pos);
    let up = pos / h;
    let mu = dot(dir, up);
    return vec2<f32>(mu_to_u(mu), clamp((h - planet_r) / (top_r - planet_r), 0.0, 1.0));
}

fn sample_transmittance(pos: vec3<f32>, dir: vec3<f32>) -> vec3<f32> {
    return textureSampleLevel(transmittance_lut, lut_sampler, tlut_uv(pos, dir), 0.0).rgb;
}

fn sample_multiscatter(pos: vec3<f32>, dir: vec3<f32>) -> vec3<f32> {
    return textureSampleLevel(multiscatter_lut, lut_sampler, tlut_uv(pos, dir), 0.0).rgb;
}

fn rayleigh_phase(cos_t: f32) -> f32 {
    return 3.0 / (16.0 * PI) * (1.0 + cos_t * cos_t);
}

fn mie_phase(cos_t: f32) -> f32 {
    let g = sky.mie.y;
    let num = (1.0 - g * g) * (1.0 + cos_t * cos_t);
    let den = (2.0 + g * g) * pow(1.0 + g * g - 2.0 * g * cos_t, 1.5);
    return 3.0 / (8.0 * PI) * num / den;
}

// ---- Transmittance LUT -------------------------------------------------------

@fragment
fn fs_transmittance(in: VsOut) -> @location(0) vec4<f32> {
    let planet_r = sky.params.z;
    let top_r = planet_r + sky.params.w;
    let r = mix(planet_r, top_r, in.uv.y);
    let mu = u_to_mu(in.uv.x);
    let pos = vec3<f32>(0.0, r, 0.0);
    let dir = vec3<f32>(sqrt(max(1.0 - mu * mu, 0.0)), mu, 0.0);

    // A ray that hits the planet reaches no light: transmittance is zero (the sun
    // is below this point's local horizon — planet shadow).
    if (ray_sphere(pos, dir, planet_r).x > 0.0) {
        return vec4<f32>(0.0, 0.0, 0.0, 1.0);
    }
    let t_max = ray_sphere(pos, dir, top_r).y;

    var od_r = 0.0;
    var od_m = 0.0;
    var od_o = 0.0;
    var t = 0.0;
    for (var i = 0.0; i < TRANSMITTANCE_STEPS; i += 1.0) {
        let new_t = ((i + 0.5) / TRANSMITTANCE_STEPS) * t_max;
        let dt = new_t - t;
        t = new_t;
        let p = pos + dir * t;
        let alt = length(p) - planet_r;
        od_r += exp(-alt / sky.rayleigh.w) * dt;
        od_m += exp(-alt / max(sky.mie.z, 1.0)) * dt;
        od_o += ozone_density(alt) * dt;
    }
    // Ozone MUST be in the transmittance LUT too: it colours the sunlight reaching
    // every scattering point, which is the dominant tint of the twilight sky.
    let tau = sky.rayleigh.xyz * od_r + vec3<f32>(sky.mie.x * MIE_EXT) * od_m
        + OZONE_ABSORPTION * od_o * sky.control.w;
    return vec4<f32>(exp(-tau), 1.0);
}

// ---- Multiple-scattering LUT (Hillaire isotropic approximation) --------------

@fragment
fn fs_multiscatter(in: VsOut) -> @location(0) vec4<f32> {
    let planet_r = sky.params.z;
    let top_r = planet_r + sky.params.w;
    let r = mix(planet_r, top_r, in.uv.y);
    let mu_s = u_to_mu(in.uv.x);
    let pos = vec3<f32>(0.0, r, 0.0);
    let sun = vec3<f32>(sqrt(max(1.0 - mu_s * mu_s, 0.0)), mu_s, 0.0);

    let inv_samples = 1.0 / f32(MS_SQRT_SAMPLES * MS_SQRT_SAMPLES);
    var lum_total = vec3<f32>(0.0);
    var fms_total = vec3<f32>(0.0);

    for (var i = 0; i < MS_SQRT_SAMPLES; i = i + 1) {
        for (var j = 0; j < MS_SQRT_SAMPLES; j = j + 1) {
            // Uniform sphere sample.
            let u = (f32(i) + 0.5) / f32(MS_SQRT_SAMPLES);
            let v = (f32(j) + 0.5) / f32(MS_SQRT_SAMPLES);
            let cos_th = 1.0 - 2.0 * u;
            let sin_th = sqrt(max(0.0, 1.0 - cos_th * cos_th));
            let phi = 2.0 * PI * v;
            let ray = vec3<f32>(sin_th * cos(phi), cos_th, sin_th * sin(phi));

            var t_max = ray_sphere(pos, ray, top_r).y;
            let ground = ray_sphere(pos, ray, planet_r);
            let hit_ground = ground.x > 0.0;
            if (hit_ground) {
                t_max = ground.x;
            }

            let cos_t = dot(ray, sun);
            let rp = rayleigh_phase(cos_t);
            let mp = mie_phase(cos_t);

            var lum = vec3<f32>(0.0);
            var lum_factor = vec3<f32>(0.0);
            var trans = vec3<f32>(1.0);
            var t = 0.0;
            for (var s = 0.0; s < MS_STEPS; s += 1.0) {
                let new_t = ((s + 0.5) / MS_STEPS) * t_max;
                let dt = new_t - t;
                t = new_t;
                let p = pos + ray * t;
                let m = scattering_values(p);
                let safe_ext = max(m.extinction, vec3<f32>(1e-9));
                let sample_trans = exp(-dt * m.extinction);

                // Isotropic transfer (no phase) — feeds the multiscatter feedback.
                let scat_no_phase = m.rayleigh + vec3<f32>(m.mie);
                let scat_f = (scat_no_phase - scat_no_phase * sample_trans) / safe_ext;
                lum_factor += trans * scat_f;

                // Second-order single scattering toward the sun (real phase).
                let sun_t = sample_transmittance(p, sun);
                let in_scat = (m.rayleigh * rp + vec3<f32>(m.mie) * mp) * sun_t;
                let scat_int = (in_scat - in_scat * sample_trans) / safe_ext;
                lum += scat_int * trans;
                trans = trans * sample_trans;
            }

            if (hit_ground) {
                let hit_p = pos + ray * t_max;
                let up = normalize(hit_p);
                let ndl = max(dot(up, sun), 0.0);
                lum += trans * sky.ground_albedo.xyz * ndl * sample_transmittance(hit_p, sun) / PI;
            }

            lum_total += lum * inv_samples;
            fms_total += lum_factor * inv_samples;
        }
    }

    // Closed-form infinite-scattering sum: L2 / (1 - f).
    let psi = lum_total / max(vec3<f32>(1.0) - fms_total, vec3<f32>(1e-3));
    return vec4<f32>(psi, 1.0);
}

// ---- Main sky pass -----------------------------------------------------------

fn raymarch_sky(pos: vec3<f32>, ray: vec3<f32>, sun: vec3<f32>, t_max: f32) -> vec3<f32> {
    let cos_t = dot(ray, sun);
    let rp = rayleigh_phase(cos_t);
    let mp = mie_phase(cos_t);
    let steps = max(sky.control.y, 4.0);

    var lum = vec3<f32>(0.0);
    var trans = vec3<f32>(1.0);
    var t = 0.0;
    for (var i = 0.0; i < steps; i += 1.0) {
        let new_t = ((i + 0.3) / steps) * t_max;
        let dt = new_t - t;
        t = new_t;
        let p = pos + ray * t;
        let m = scattering_values(p);
        let safe_ext = max(m.extinction, vec3<f32>(1e-9));
        let sample_trans = exp(-dt * m.extinction);

        let sun_t = sample_transmittance(p, sun);
        let psi = sample_multiscatter(p, sun);
        // Single scattering (phase * sun transmittance) + multiscatter (phase-less).
        let rayleigh_in = m.rayleigh * (rp * sun_t + psi);
        let mie_in = vec3<f32>(m.mie) * (mp * sun_t + psi);
        let in_scat = rayleigh_in + mie_in;
        let scat_int = (in_scat - in_scat * sample_trans) / safe_ext;
        lum += scat_int * trans;
        trans = trans * sample_trans;
    }
    return lum;
}

fn hable_partial(x: vec3<f32>) -> vec3<f32> {
    let a = 0.15; let b = 0.50; let c = 0.10; let d = 0.20; let e = 0.02; let f = 0.30;
    return ((x * (a * x + c * b) + d * e) / (x * (a * x + b) + d * f)) - e / f;
}

fn hable(color: vec3<f32>) -> vec3<f32> {
    let w = vec3<f32>(11.2);
    return hable_partial(color) / hable_partial(w);
}

fn linear_to_srgb(c: vec3<f32>) -> vec3<f32> {
    let lo = c * 12.92;
    let hi = 1.055 * pow(max(c, vec3<f32>(0.0)), vec3<f32>(1.0 / 2.4)) - 0.055;
    return select(hi, lo, c <= vec3<f32>(0.0031308));
}

// Interleaved gradient noise — ~1 LSB dither for the LDR-direct path's 8-bit write.
fn ign(p: vec2<f32>) -> f32 {
    return fract(52.9829189 * fract(dot(p, vec2<f32>(0.06711056, 0.00583715))));
}

// Unstructured per-pixel hash (Hoskins hash11-style). IGN is a fine dither for a 1-LSB write, but it
// is a STRUCTURED diagonal pattern, and using it as the cloud march's start offset makes that
// structure visible: residual undersampling error is modulated by the offset, so the diagonal stripes
// print straight onto cloud silhouettes and are then magnified by the low-res march's upsample. This
// file already records the same failure for the env map at line ~788 ("reads as a coarse
// checkerboard"). White noise has the same mean and no orientation, so the identical amount of
// residual error reads as fine grain instead of a herringbone.
fn hash21(p: vec2<f32>) -> f32 {
    var p3 = fract(vec3<f32>(p.x, p.y, p.x) * 0.1031);
    p3 = p3 + dot(p3, vec3<f32>(p3.y, p3.z, p3.x) + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

// Linear sky radiance along a world direction: the atmosphere march + night-faded horizon
// haze + the authored night-sky floor, WITHOUT the sun disc (callers that want the disc add it
// against the true, unfloored `dir`). Shared by the fullscreen sky (fs_sky) and the reflection
// environment map (fs_sky_env), so the water surface reflects the exact same sky.
fn sky_radiance(dir: vec3<f32>) -> vec3<f32> {
    let sun = normalize(sky.sun_dir.xyz);
    let planet_r = sky.params.z;
    let top_r = planet_r + sky.params.w;
    let cam_alt = max(sky.night_zenith.w, 0.0);
    let pos = vec3<f32>(0.0, planet_r + cam_alt, 0.0);

    // Sun radiance scale x the authored exposure (radiance -> scene-referred).
    let radiance = sky.sun_dir.w * sky.params.y;

    // March a horizon-floored direction so the sky BELOW the horizon smoothly continues the
    // horizon colour instead of darkening into an ugly band (the terrain / water draws over it).
    let march_dir = normalize(vec3<f32>(dir.x, max(dir.y, 0.0), dir.z));
    let atmo = ray_sphere(pos, march_dir, top_r);
    var color = vec3<f32>(0.0);
    if (atmo.y > 0.0) {
        color = raymarch_sky(pos, march_dir, sun, atmo.y) * radiance;
    }

    // Night-faded horizon haze: blend toward the scene fog colour near the horizon so the fogged
    // distant terrain and the sky meet without a seam. Fades out at night (dark sky).
    let haze_strength = sky.fog_color.w * (1.0 - sky.ground_albedo.w);
    if (haze_strength > 0.0) {
        var fog = sky.fog_color.rgb;
        if (sky.output.x > 0.5) {
            fog = pow(max(fog, vec3<f32>(0.0)), vec3<f32>(2.2));
        }
        let th = (1.0 - smoothstep(0.0, 0.15, dir.y)) * haze_strength;
        color = mix(color, fog, clamp(th, 0.0, 1.0));
    }

    let night_blend_stars = 1.0 - smoothstep(sky.night_params.y, sky.night_params.x, sun.y);
    // Stars. Added with the night floor and BEFORE the cloud composite below, so a cloud deck
    // covers them exactly as it covers the sky behind it -- stars punching through overcast is the
    // giveaway that they were pasted on at the end.
    let star_amount = max(sky.output.y, 0.0);
    if (star_amount > 0.001 && night_blend_stars > 0.0) {
        color = color + star_field(dir, sun) * star_amount * night_blend_stars;
    }

    // Night-sky floor: an authored deep-blue that fills in as the sun drops below the horizon
    // (blended by sun altitude), so twilight/night settle into a believable blue instead of the
    // physical model's near-black. See docs/procedural-sky-plan.md Stage 6.
    let night_blend = 1.0 - smoothstep(sky.night_params.y, sky.night_params.x, sun.y);
    if (night_blend > 0.0) {
        let night = mix(sky.night_horizon.rgb, sky.night_zenith.rgb, clamp(dir.y, 0.0, 1.0));
        color = color + night * sky.night_params.z * night_blend;
    }

    return max(color, vec3<f32>(0.0));
}

// ---- Volumetric clouds (plan Stage 5) ----------------------------------------
// A raymarched cloud shell between cloud0.z and cloud0.w (altitudes ASL) composited
// OVER the atmosphere background (sky_radiance) by fs_sky (full march) and fs_sky_env
// (a cheap low-step path so the wind-scrolled noise doesn't alias into the SH-ambient
// bake). Lit on the same linear radiance scale as the sky, so clouds sit correctly in
// HDR and show up in water reflections + ambient. See docs/procedural-sky-plan.md §5.

// Max primary steps (the actual count is adaptive: derived from the ray's path length through the
// shell so sampling density stays constant with thickness/angle, then clamped to this). Each step is
// one cheap texture tap now, so a higher cap is affordable and needed for thick/grazing views.
const CLOUD_STEPS: f32 = 128.0;
const CLOUD_LIGHT_STEPS: i32 = 4;
// Volumetric march distance cap (m) along the view ray, measured from the shell entry. Grazing / in-
// deck rays otherwise traverse tens of km of shell, which at the fixed step cap undersamples the
// vertical structure into a bright band across the horizon (worst where cloud masses overlap). Bounding
// the path keeps the step size honest; the density dissolves over the last stretch so the bound itself
// doesn't read as an edge. The true far field is the (later) cheap 2D deck, not this volumetric layer.
const CLOUD_MARCH_DIST: f32 = 16000.0;
// Total spread of the per-column cloud BASE height, as a fraction of the deck thickness. 0 restores
// the old single-plane deck. At the default 1200..3500 m deck this is ~690 m of spread — a column's
// base sits anywhere in +/-345 m of the authored sky.cloud0.z. See cloud_density.
//
// CENTRED on the authored base rather than only rising, so the deck's mean altitude — and therefore
// the whole sky's composition — is what it was; only the flatness goes away.
//
// A shader constant rather than a uniform field: it is a look constant, not a per-frame one, and
// WgrSky's size is checked by an ABI handshake on both the C++ and Rust sides — a new field there
// costs a padding-matched change in three places to expose a number nothing animates.
//
// The whole vertical profile is TRANSLATED by this, not squashed into the authored deck, so a
// higher-based cloud is the same cloud higher up rather than a thinner one. That means the marched
// shell has to reach half this beyond the authored base and top, which cloud_shell_bot/top do.
const CLOUD_BASE_VARY: f32 = 0.30;

// Henyey-Greenstein phase (g > 0 forward, g < 0 back).
fn hg(cos_t: f32, g: f32) -> f32 {
    let g2 = g * g;
    return (1.0 - g2) / (4.0 * PI * pow(max(1.0 + g2 - 2.0 * g * cos_t, 1e-4), 1.5));
}

// World-anchored sample position (metres) for a planet-centred `p`: camera world xz + the
// camera-relative ray xz + the CPU-wrapped wind WORLD offset (cloud1.xy, bounded so the scaled
// texture coord stays precise), with .y = altitude above the cloud base (bounded, unlike raw
// planet-scale p.y). Returns (worldX, altitude, worldZ) — the 3 noise axes.
// Deck thickness (m) — the vertical extent of ONE column's cloud profile, which is constant.
fn cloud_thickness() -> f32 {
    return max(sky.cloud0.w - sky.cloud0.z, 1.0);
}

// Planet radii of the MARCHED shell — the authored deck grown by half the base-height spread at
// each end, because a column whose base is displaced carries its whole profile with it. A shell
// that stopped at the authored bottom/top would slice those columns off flat and trade one flat
// plane for another. Everything that bounds the march (cloud_interval, and through it the cloud
// shadow pass) must use these, not sky.cloud0.z/w.
fn cloud_shell_bot() -> f32 {
    return sky.params.z + sky.cloud0.z - cloud_thickness() * CLOUD_BASE_VARY * 0.5;
}

fn cloud_shell_top() -> f32 {
    return sky.params.z + sky.cloud0.w + cloud_thickness() * CLOUD_BASE_VARY * 0.5;
}

fn cloud_world(p: vec3<f32>) -> vec3<f32> {
    let world_xz = sky.cam_pos.xz + p.xz + sky.cloud1.xy;
    let alt = length(p) - (sky.params.z + sky.cloud0.z);
    return vec3<f32>(world_xz.x, alt, world_xz.y);
}

// Weather fields at a WORLD xz (metres), from ONE tap of the noise volume:
//   .x = LOCAL coverage — the authored base coverage drifts across the world by a very large-scale
//        2D field (a huge-scale slice of the noise volume), so the sky isn't uniformly cloudy —
//        some regions read cloudier, some clearer.
//   .y = LOCAL cloud-base height [0,1] — see CLOUD_BASE_VARY / cloud_density. FREE: the same tap.
// World-anchored, so both are stable as the camera moves; sampled PER cloud sample (not once per ray
// at the view-dependent shell entry, which made the whole cloud field shift/morph with camera
// altitude and view angle).
// The coverage drift amount is tied to the AVAILABLE HEADROOM (max at coverage 0.5, ZERO at 0 and 1):
// authored full overcast stays genuinely solid (so the sun is actually blocked) and authored clear
// stays clear.
fn cloud_weather(world_xz: vec2<f32>) -> vec2<f32> {
    // The 0.5 slice is now drifted: where the weather field says "more cloud here" moves slowly,
    // so banks build in one place and clear in another instead of the pattern being fixed forever.
    // The slice this samples DRIFTS with world position, and that is what stops the coverage field
    // repeating. The noise volume tiles in all three axes, so sampling a FIXED y slice means the
    // moment world xz wraps the tile you get the identical pattern back -- a domain warp only
    // displaces within that repeat, it cannot remove it. Advancing y with xz means a wrap in x
    // lands on a DIFFERENT slice, so the true period becomes the lowest common multiple of the xz
    // and y periods instead of the xz period. The drift rates are irrational multiples of the xz
    // scale and of each other, so that multiple is effectively never reached in view.
    //
    // Costs nothing: same single tap, two extra multiply-adds on the coordinate.
    let drift = (world_xz.x * 0.61803399 + world_xz.y * 0.41421356) * sky.cloud3.x * 0.27;
    let wc = vec3<f32>(
        world_xz.x * sky.cloud3.x,
        0.5 + sky.cloud4.z * sky.cloud3.x + drift,
        world_xz.y * sky.cloud3.x,
    );
    let weather = textureSampleLevel(cloud_noise, cloud_samp, wc, 0.0);
    let vary = sky.cloud3.y * 2.0 * min(sky.cloud0.x, 1.0 - sky.cloud0.x);
    let cov = clamp(sky.cloud0.x + (weather.r - 0.5) * 2.0 * vary, 0.0, 1.0);
    // Cloud-BASE field, out of the SAME tap's G channel (the volume's detail fBm: base frequency 8
    // against the shape channel's 4, so at the weather scale it varies over roughly a kilometre —
    // one bank of cloud sits lower than the next, which is what a real sky does; the condensation
    // level is regional, not global).
    //
    // R and G share their upper octaves (R = fBm 4/8/16, G = fBm 8/16/32), so raw G correlates
    // ~0.5 with R: every cloudier region would also be a higher-based region, and that systematic
    // reads as the whole deck tilting where it thickens. Subtracting R's deviation once cancels
    // exactly the shared part (the octave weights work out to a coefficient of ~1), leaving a base
    // field independent of coverage. Pure ALU — still one tap.
    //
    // smoothstep rather than a linear remap so the field spends most of its area near 0 or near 1:
    // that gives distinct base LEVELS with soft transitions between them, instead of a continuous
    // ramp that just reads as a tilted plane.
    let bfield = 0.5 + (weather.g - 0.5) * 2.0 - (weather.r - 0.5);
    return vec2<f32>(cov, smoothstep(0.30, 0.70, bfield));
}

// Cloud density fraction [0,1] at a planet-centred position, given the ray's local coverage. Anti-
// repetition: a low-frequency DOMAIN WARP of the horizontal position (kills the grid regularity that
// makes tiling legible), then SHAPE (large tile, R) and DETAIL (small tile, G) sampled at
// INCOMMENSURATE world scales so the visual product period is far longer than either tile. Structured
// so empty space bails before the detail erosion (grad + coverage remap only reduce density).
fn cloud_density(p: vec3<f32>) -> f32 {
    let planet_r = sky.params.z;
    let r_bot = planet_r + sky.cloud0.z;
    let thick = cloud_thickness();
    // Altitude above the AUTHORED cloud base. A column's own base sits within +/-half the spread of
    // that and carries a full `thick` of profile with it, so this band is the union of every
    // column's possible profile. Testing it here keeps the empty-space bail ZERO-TAP; the exact
    // per-column band needs the weather tap, which the cloudy path was fetching anyway.
    let half_vary = thick * CLOUD_BASE_VARY * 0.5;
    let alt = length(p) - r_bot;
    if (alt <= -half_vary || alt >= thick + half_vary) {
        return 0.0;
    }
    var w = cloud_world(p);
    // Domain warp FIRST, so the warped position feeds coverage as well as shape.
    //
    // Coverage used to be sampled at the unwarped position, and coverage is the field that carries
    // the largest, most legible structure — where a bank is and where a gap is. Sampling one
    // tileable volume on an unwarped grid means that structure repeats EXACTLY every weather tile,
    // and the march runs to 60 km against a 16 km weather tile and a 9.3 km shape tile, so several
    // whole periods are on screen at once and they compress into bands toward the horizon. Warping
    // shape but not coverage broke the small structure's grid while leaving the big one's intact,
    // which is the wrong way round: the eye finds the large repeat first.
    //
    // This costs no extra taps in cloudy regions — the warp tap was already being fetched here,
    // just after the coverage test instead of before it. Clear regions pay one tap more before
    // bailing, which the density-gradient early-out above already keeps off most of the sky.
    let warp_n = textureSampleLevel(cloud_noise, cloud_samp, w * sky.cloud3.z, 0.0);
    w.x += (warp_n.r - 0.5) * 2.0 * sky.cloud3.w;
    w.z += (warp_n.g - 0.5) * 2.0 * sky.cloud3.w;
    // World-anchored coverage sampled at THIS sample (not once per ray) so the cloud field stays
    // locked in the world as the camera flies through it, instead of morphing with altitude/angle.
    let weather = cloud_weather(w.xz);
    let cov = weather.x;
    if (cov <= 0.001) {
        return 0.0; // clear patch -> skip the shape/detail taps (per-sample empty-space fast path)
    }
    // Per-column cloud BASE (metres above the authored base). Every cloud used to start at exactly
    // sky.cloud0.z, because the height gradient was a function of altitude alone — so the whole
    // sky's undersides lay on one plane, which is the most artificial thing about a procedural deck.
    // A real sky's condensation level varies by hundreds of metres from one air mass to the next,
    // and it is the bottoms, not the tops, that the eye reads as a floor.
    let base_h = (weather.y - 0.5) * CLOUD_BASE_VARY * thick;
    let hl = (alt - base_h) / thick;
    if (hl <= 0.0 || hl >= 1.0) {
        return 0.0; // outside THIS column's cloud band
    }
    // Height gradient: flat-ish bottom, rounded fading top — unchanged, but now in the column's own
    // frame rather than the deck's, so the profile is TRANSLATED and keeps its full thickness.
    let grad = smoothstep(0.0, 0.12, hl) * (1.0 - smoothstep(0.55, 1.0, hl));
    if (grad <= 0.0) {
        return 0.0;
    }
    // Carry the noise column up with the base as well, so a raised cloud is the SAME cloud higher
    // up rather than the upper slice of a fixed one. (Vertical domain warp; base_h varies smoothly
    // with world xz, so this is a smooth shear, not a seam.)
    w.y -= base_h;
    // Shape (large, incommensurate tile) drives the blobs; overcast floor + coverage remap.
    // Shape/detail sampled at a Y drifted by the evolution offset: the volume is 3D and tileable,
    // so walking the third axis walks through uncorrelated slices — the cheapest honest way to get
    // formation and dissolution without a second noise fetch or a simulation.
    var ws = w * sky.cloud1.z;
    // Same slice drift as the coverage field, for the same reason: a fixed y slice makes the shape
    // repeat exactly every shape tile, and at 9.3 km with a 60 km march that repeat is on screen
    // several times over. Drifting y with world xz turns the period into the lcm of the xz and y
    // periods. Rates differ from the coverage field's so the two do not come back into phase.
    ws.y += sky.cloud4.x * sky.cloud1.z + (w.x * 0.31830989 + w.z * 0.53190563) * sky.cloud1.z * 0.31;
    let shape = textureSampleLevel(cloud_noise, cloud_samp, ws, 0.0).r;
    let n = max(shape, smoothstep(0.55, 1.0, cov));
    let base = clamp((n - (1.0 - cov)) / max(cov, 1e-3), 0.0, 1.0);
    var d = base * grad;
    if (d <= 0.0) {
        return 0.0; // in a gap -> skip the detail tap + erosion (the empty-space fast path)
    }
    // Detail erosion (small, incommensurate tile), FADED with horizontal distance so far clouds lose
    // high-frequency detail — cuts both the residual step-alias and the exaggerated tiling on the
    // horizon (many tiles visible at once). The true far-field 2D layer is a later phase.
    var wd = w * sky.cloud1.w;
    wd.y += sky.cloud4.y * sky.cloud1.w;
    let detail = textureSampleLevel(cloud_noise, cloud_samp, wd, 0.0).g;
    let dist_fade = 1.0 - smoothstep(sky.cloud2.w * 0.3, sky.cloud2.w * 0.8, length(p.xz));
    d = clamp(d - (1.0 - d) * detail * 0.4 * dist_fade, 0.0, 1.0);
    return d;
}

// A procedural star field on the celestial sphere.
//
// Cube-grid rather than spherical coordinates: lat/long bunches stars at the poles, and the
// bunching is obvious the moment you look up. Quantising the unit direction itself gives cells of
// roughly equal solid angle, so density is even across the sky.
//
// One star per cell at a hashed offset. The sharp falloff is what makes a star read as a POINT
// rather than a blob -- the width is a fraction of a cell, so at typical resolutions a star lands
// on one or two pixels, which is what it should be.
fn star_hash3(c: vec3<f32>) -> vec3<f32> {
    var p = vec3<f32>(dot(c, vec3<f32>(127.1, 311.7, 74.7)),
                      dot(c, vec3<f32>(269.5, 183.3, 246.1)),
                      dot(c, vec3<f32>(113.5, 271.9, 124.6)));
    return fract(sin(p) * 43758.5453123);
}

fn star_field(dir_in: vec3<f32>, sun: vec3<f32>) -> vec3<f32> {
    // Wheel the field with the SUN'S AZIMUTH rather than a clock. This UBO carries no time value,
    // and the sun's bearing is the celestial clock anyway -- it advances at the same 15 deg an
    // hour the sky really turns at, so the stars drift through the night for free and stay
    // consistent with wherever the sun comes up.
    let ang = atan2(sun.z, sun.x);
    let ca = cos(ang);
    let sa = sin(ang);
    let dir = vec3<f32>(dir_in.x * ca - dir_in.z * sa, dir_in.y, dir_in.x * sa + dir_in.z * ca);

    // Below the horizon there is no sky to put stars in, and near it atmospheric extinction takes
    // them out well before the geometric horizon -- so fade rather than cut.
    let horizon = smoothstep(-0.02, 0.22, dir.y);
    if (horizon <= 0.0) {
        return vec3<f32>(0.0);
    }

    let scale = 190.0;
    let p = dir * scale;
    let cell = floor(p);
    let h = star_hash3(cell);
    // Only a fraction of cells hold a star; without this the sky is a uniform dot screen.
    if (h.x > 0.10) {
        return vec3<f32>(0.0);
    }
    let centre = cell + vec3<f32>(0.25) + h * 0.5;
    let d = length(p - centre);
    let core = 1.0 - smoothstep(0.0, 0.42, d);
    if (core <= 0.0) {
        return vec3<f32>(0.0);
    }
    // Magnitude: mostly faint with a few bright ones, which is what the real distribution looks
    // like. A uniform brightness reads as a texture rather than a sky.
    let mag = pow(h.y, 3.0);
    // No twinkle. Scintillation needs a per-frame clock and this UBO has none; faking it from the
    // sun's bearing would change on the scale of hours, which is not twinkling. Left out rather
    // than approximated into something that only looks like a bug.
    //
    // Colour by spectral class, kept subtle -- most stars read white to the naked eye.
    let tint = mix(vec3<f32>(1.0, 0.86, 0.72), vec3<f32>(0.78, 0.86, 1.0), h.z);
    return tint * (core * core * mag * horizon);
}

// View-ray parametric interval [t0, t1] inside the cloud shell, robust for camera below /
// inside / above the deck, and clamped so nothing renders below the horizon (behind the
// planet). t1 <= t0 means the ray never crosses the shell (caller skips).
fn cloud_interval(pos: vec3<f32>, dir: vec3<f32>) -> vec2<f32> {
    let planet_r = sky.params.z;
    // The SHELL, not the authored deck: columns whose base is displaced carry their profile past
    // sky.cloud0.z/w, and a ray that stopped at the authored bounds would cut those off flat.
    let r_bot = cloud_shell_bot();
    let r_top = cloud_shell_top();
    let h = length(pos);
    let hb = ray_sphere(pos, dir, r_bot);
    let ht = ray_sphere(pos, dir, r_top);
    let hb_hit = hb.x <= hb.y; // false = miss (ray_sphere returns x>y on a miss)
    var t0 = 0.0;
    var t1 = -1.0;
    if (h < r_bot) {
        // Below the deck (normal ground case): enter at the bottom sphere far hit, exit at top.
        t0 = hb.y;
        t1 = ht.y;
    } else if (h > r_top) {
        // Above the deck (high flight): must hit the top shell.
        if (ht.x > ht.y) {
            return vec2<f32>(1.0, -1.0);
        }
        t0 = ht.x;
        t1 = select(ht.y, hb.x, hb_hit && hb.x > ht.x);
    } else {
        // Inside the deck.
        t0 = 0.0;
        t1 = select(ht.y, hb.x, hb_hit && hb.x > 0.0);
    }
    let ground = ray_sphere(pos, dir, planet_r);
    if (ground.x > 0.0 && ground.x <= ground.y) {
        t1 = min(t1, ground.x);
    }
    return vec2<f32>(max(t0, 0.0), t1);
}

struct CloudResult {
    inscatter: vec3<f32>, // premultiplied cloud in-scatter (already faded); composite = bg*trans + inscatter
    trans: f32,           // view-ray transmittance through the cloud
};

// March the cloud shell and return raw (inscatter, transmittance) — NOT composited over the
// background. `bg` is used only as the local sky colour for the cloud AMBIENT; the caller composites
// `bg*trans + inscatter` if it wants a colour. `max_dist` bounds the march at the scene surface (for
// the depth-aware over-scene composite) — a cloud beyond the terrain along this ray is not marched, a
// cloud in front of it accumulates and occludes it. `steps`/`light_steps` trade quality for cost.
fn march_clouds(
    pos: vec3<f32>,
    dir: vec3<f32>,
    sun: vec3<f32>,
    radiance: f32,
    bg: vec3<f32>,
    steps: f32,      // MAX step count (adaptive n is clamped to this)
    light_steps: i32,
    jitter: f32,     // per-PIXEL start offset in [0,1) — breaks step coherence (see below)
    max_dist: f32,   // far bound along the ray (scene distance, or a large value for sky-only)
) -> CloudResult {
    var res: CloudResult;
    res.inscatter = vec3<f32>(0.0);
    res.trans = 1.0;
    if (sky.cloud0.x <= 0.001 || sky.cloud0.y <= 0.0) {
        return res;
    }
    let iv = cloud_interval(pos, dir);
    let t0 = max(iv.x, 0.0);
    // Bound the march at BOTH the scene surface (occlusion) and the volumetric distance cap (measured
    // from the shell entry, so it caps the marched PATH regardless of where the shell starts). The
    // scene bound keeps clouds solid right up to an occluder; the distance cap only ever kicks in when
    // nothing nearer stops the ray, and the density dissolve below hides it.
    let t1 = min(iv.y, min(max_dist, t0 + CLOUD_MARCH_DIST));
    if (t1 <= t0) {
        return res;
    }
    // Coverage is now world-anchored and sampled PER cloud sample (in cloud_density). Here we only need
    // a representative value at the shell entry to tint the broad cloud AMBIENT / underside AO (a slow-
    // varying look term, so one large-scale tap per ray is plenty).
    let cov = cloud_weather(cloud_world(pos + dir * t0).xz).x;
    // Adaptive step COUNT + per-pixel jittered start. The radial-ring artifact (worse with cloud
    // thickness / grazing angle) was primary-ray step aliasing of the vertical density structure: a
    // FIXED step count made dt scale with the angle-dependent path length, and the step phase varied
    // smoothly across the screen, so the bands organised into concentric rings. Fix: derive the step
    // count from the path so sampling density is ~constant regardless of thickness/angle (capped by
    // `steps`), and offset the start by a per-PIXEL hash so residual undersampling reads as fine
    // noise instead of coherent rings. (Thin shells were clean because they have little vertical
    // structure to alias.)
    let thick = max(sky.cloud0.w - sky.cloud0.z, 1.0);
    let path = t1 - t0;
    // Sampling density. thick/24 put the step at ~96 m through a 2300 m deck while the DETAIL tile
    // is 1700 m, so each step straddled several density features and the integral aliased -- that
    // residual is the grain, and redistributing it with a different jitter cannot remove it. Only
    // more samples can. The early-out on transmittance and the empty-space bails in cloud_density
    // mean the extra steps are mostly paid inside cloud, not across the whole sky.
    let target_step = max(thick / 56.0, 1.0);
    let n = clamp(ceil(path / target_step), 8.0, steps);
    let dt = path / n;
    let ext = sky.cloud0.y;
    let cos_t = dot(dir, sun);
    // Forward silver-lining lobe + a soft back lobe for wrap light, blended toward ISOTROPIC as
    // coverage rises. Single scattering peaks sharply toward the sun, which under thick overcast reads
    // as a hard bright blob at the sun's screen position (there is no disc left to see through solid
    // cloud, so a peaked glow looks wrong). Real overcast is dominated by multiple scattering and glows
    // ~uniformly; mixing the phase toward 1/4pi with coverage is a cheap stand-in until MS (Phase 4).
    let phase_hg = mix(hg(cos_t, sky.cloud2.x), hg(cos_t, -0.15), 0.5);
    let phase = mix(phase_hg, 1.0 / (4.0 * PI), cov * 0.7);
    let sun_col = sample_transmittance(pos, sun) * radiance;

    // Cloud ambient = the local sky radiance already computed for the background (bg): right
    // magnitude + colour, and FREE (vs the old per-pixel zenith raymarch_sky). BUT desaturate it
    // toward grey as coverage -> 1: under a thick deck the blue sky is blocked, so the underside is
    // lit by grey multiscattered light, not clear-sky blue. Without this, full overcast just reads
    // as a darker BLUE sky instead of grey (the cloud practically disappears).
    let luma = dot(bg, vec3<f32>(0.2126, 0.7152, 0.0722));
    let amb_sky = mix(bg, vec3<f32>(luma), cov) * sky.cloud2.z;
    // Undersides get less dark at high coverage (a bright overcast dome fills them in more).
    let ao_floor = mix(0.35, 0.7, cov);

    let ls_base = thick / f32(light_steps);

    var col = vec3<f32>(0.0);
    var trans = 1.0;
    var t = t0 + dt * jitter;
    for (var i = 0.0; i < n; i = i + 1.0) {
        if (trans < 0.01) {
            break;
        }
        let p = pos + dir * t;
        // Dissolve density over the last stretch of the distance cap so the cap is a soft fade into
        // sky/haze, not a hard shell edge. Keyed off the marched distance (t - t0) against the fixed
        // cap — NOT against t1, which may be a nearby occluder (a cloud in front of a hill must stay
        // solid). When t1 is the scene occluder we never reach the fade zone, so occlusion is crisp.
        let far_fade = 1.0 - smoothstep(0.6, 1.0, (t - t0) / CLOUD_MARCH_DIST);
        let dens = cloud_density(p) * far_fade;
        if (dens > 0.001) {
            let sigma = dens * ext;
            // Light march toward the sun with exponentially growing steps -> optical depth.
            var od = 0.0;
            var lt = 0.0;
            var ls = ls_base * 0.3;
            for (var j = 0; j < light_steps; j = j + 1) {
                lt += ls;
                od += cloud_density(p + sun * lt) * ext * ls;
                ls *= 1.6;
            }
            let sun_t = exp(-od);
            // Beer-Powder dark-edge term (the silver-lining / fluffy look).
            let powder = 1.0 - exp(-2.0 * sigma * dt);
            let direct = sun_col * sun_t * phase * mix(1.0, powder, sky.cloud2.y);
            let hfp = clamp((length(p) - (sky.params.z + sky.cloud0.z)) / thick, 0.0, 1.0);
            let ambient = amb_sky * mix(ao_floor, 1.0, hfp); // darker undersides (cheap AO)
            let scatter = direct + ambient;
            let seg = exp(-sigma * dt);
            // In-scattered radiance over the segment: T * albedo * J * (1 - exp(-sigma*dt)), with
            // J = scatter (phase*sun + ambient). Cloud albedo ~= 1 (scattering >> absorption), so
            // there is NO 1/sigma factor here. (The earlier /sigma copied the sky march's form, but
            // the sky's source already bakes in the scattering coeff while `scatter` does not — that
            // made optically-thin clouds ~1/sigma, i.e. ~15-30x, too bright and bloom-blown.)
            col += trans * scatter * (1.0 - seg);
            trans *= seg;
        }
        t += dt;
    }
    // The far dissolve is folded into the march itself (far_fade on density), so the raw accumulated
    // (inscatter, transmittance) already dissolves toward the distance cap — no post fade needed.
    res.inscatter = col;
    res.trans = trans;
    return res;
}

// ---- Second cloud layer: high cirrus -----------------------------------------
// Cloud at a completely different altitude, so the sky has depth instead of one populated slab.
// Two modes, selected by sky.output.w (0 = off, 1 = flat sheet, 2 = thin-shell march):
//
//   FLAT       one ray/sphere intersection against a sheet at CIRRUS_ALT, one field evaluation.
//   VOLUMETRIC CIRRUS_STEPS samples through the shell (500-1400 m, thickening with puffiness).
//              Cirrus is optically thin, so a handful of steps is enough to give it real vertical
//              extent: the field walks the noise volume's third axis with height, so top and
//              bottom of the shell are different cloud, and a grazing ray travels kilometres
//              THROUGH it instead of piercing a plane.
//
// Either way this is sky pixels only, occluded by terrain via `max_dist`.
const CIRRUS_ALT: f32 = 7000.0;          // m ASL — shell CENTRE; well clear of the cumulus deck
const CIRRUS_STEPS: i32 = 6;             // march samples through the shell (thin layer, few steps)
// Grazing paths are clamped to shell_thickness/0.15, which matches exactly the
// clamp(dir.y, 0.15, 1) the flat path uses, so the two modes agree on the horizon rather than
// disagreeing at the worst angle. At grazing elevations the slant path through a thin shell
// diverges as 1/sin, so without this the far sheet becomes a solid white band AND dt grows to tens
// of kilometres. It is computed from the live thickness in cirrus_layer (see CIRRUS_THICK_*).
const CIRRUS_PATH_SIN_MIN: f32 = 0.15;
// Horizontal noise scale (1/m). This is the number that decides whether the layer reads as cirrus
// or as one enormous smear: the sheet tap below stretches it 4x along world X, so at 1/7000 a
// streak is roughly 7 km across and 28 km long — a few degrees wide from 8 km below, which is what
// a cirrus field subtends. The first attempt used 1/26000 and produced a single blob filling the
// upper sky, because one "feature" was 100 km long.
const CIRRUS_SCALE: f32 = 1.0 / 7000.0;
const CIRRUS_COVER: f32 = 0.52;          // fraction of the sheet that holds cloud
const CIRRUS_FADE: f32 = 150000.0;       // m — dissolve into haze beyond this slant distance
// Domain-warp displacement (m, peak-to-peak). See cirrus_warp.
const CIRRUS_WARP: f32 = 6000.0;
// How far the field walks the noise volume's THIRD axis between the bottom and the top of the
// shell. The R channel's coarsest octave has 4 cells across the volume, so 0.12 is roughly half a
// coarse cell: the big shapes survive the crossing (it still reads as ONE cloud with depth) while
// everything finer changes, which is what stops the marched shell looking like an extruded plane.
const CIRRUS_SLICE_H: f32 = 0.12;

// ---- PUFFINESS -----------------------------------------------------------------
// One 0..1 slider (Sky tab; arrives in sky.cloud4.w already combined with its time variation, see
// Sky::cirrus_puff_now on the Rust side). It is a SHAPE control, not a brightness control.
//
// What "puffy" means here is the meteorological difference between the two things this layer can
// be. Both are ice cloud at the same altitude; the difference is whether the air up there is being
// SHEARED or is convecting:
//
//   puff = 0   cirrus fibratus/uncinus — wind shear draws the ice out into long parallel fibres.
//              Very elongated, heavily striated, optically thin, almost no vertical extent.
//   puff = 1   cirrocumulus — shallow convection breaks the sheet into a raft of discrete, nearly
//              round cells. Short, lumpy, little striation, real (if small) vertical depth.
//
// Four things move together, because in the sky they are consequences of the same cause; splitting
// them into four knobs would just make it possible to author combinations that do not exist:
//   1. ASPECT   how far the sheet octaves are stretched along world X (shear stretches, convection
//               does not) — the single strongest cue for which of the two you are looking at.
//               7.7:1 at the fibrous end, 1.9:1 at the puffy one;
//   2. FINE     the balance between the coarse sheet octave and the finer one. Puffy = the mass
//               lives in big cells, so the fine octave is dialled DOWN and the lumps stay whole;
//   3. ERODE    fibre-erosion strength — the striations themselves;
//   4. THICK    shell thickness in the volumetric mode. "Puffy" is literally vertical development,
//               so this is the one that is not a texture trick: the layer gets deeper and a
//               grazing ray travels through more of it.
// DENSITY moves the other way (thinner as it gets puffier) purely to keep the layer's total
// opacity roughly constant across the slider: erosion subtracts mass, so without this the puffy
// end would arrive ~40% denser and the slider would read as a brightness control with a shape
// side-effect. Optical depth is preserved through the shell too — cirrus_sigma normalises by the
// profile integral AND by the live thickness, so a deeper shell is not automatically whiter.
//
// Every parameter is given THREE values, not two, and the slider is mapped through the middle one
// at 0.5. That is not decoration: it is what lets the default be EXACTLY the value the layer
// shipped with (it is literally the same constant, not the average of two others), and it lets the
// two halves have different reach. They need different reach — the puffy end has plenty of room to
// run, while the fibrous end is hard-limited by the erosion below, which deletes the layer rather
// than texturing it if pushed much past 0.66.
// (One deliberate exception to "0.5 changes nothing": the contrast re-expansion in cirrus_field is
// now computed from the octave weights instead of hard-coded, and at the default it evaluates to
// 1.3746 where the constant read 1.37 — a 0.3% difference, and the constant was that expression's
// rounded value in the first place.)
const CIRRUS_ASPECT_FIBRE: f32 = 0.13;   // world-X compression of the sheet octaves: 7.7:1 streaks
const CIRRUS_ASPECT_SHIPPED: f32 = 0.25; // 4:1 — ~7 km across, ~28 km long
const CIRRUS_ASPECT_PUFF: f32 = 0.52;    // ~1.9:1 — cells, not streaks
const CIRRUS_FINE_FIBRE: f32 = 0.46;     // weight of the x1.618 octave
const CIRRUS_FINE_SHIPPED: f32 = 0.38;
const CIRRUS_FINE_PUFF: f32 = 0.24;
// Fibre erosion strength. Careful with the fibrous end: the eroding channel is an fBm with mean
// 0.5, so 0.66 already subtracts ~0.33 from a field whose surviving values are themselves only
// ~0.3. It TEXTURES the layer here and would DELETE it not far above.
const CIRRUS_ERODE_FIBRE: f32 = 0.66;
const CIRRUS_ERODE_SHIPPED: f32 = 0.45;
const CIRRUS_ERODE_PUFF: f32 = 0.10;
const CIRRUS_DENSITY_FIBRE: f32 = 3.00;  // optical depth straight up through full density
const CIRRUS_DENSITY_SHIPPED: f32 = 2.80;
const CIRRUS_DENSITY_PUFF: f32 = 2.35;
const CIRRUS_THICK_FIBRE: f32 = 500.0;   // m — volumetric shell thickness
const CIRRUS_THICK_SHIPPED: f32 = 900.0;
// 1400 rather than more: the grazing-path clamp is thickness/CIRRUS_PATH_SIN_MIN, so a deeper
// shell also lengthens the worst-case march — and CIRRUS_STEPS does not grow with it, so past
// this the horizon steps start to show as bands instead of cloud.
const CIRRUS_THICK_PUFF: f32 = 1400.0;

// Map the 0..1 slider through the shipped value at its midpoint.
fn cirrus_mix3(fibre: f32, shipped: f32, puffy: f32, puff: f32) -> f32 {
    let t = puff * 2.0;
    return select(mix(fibre, shipped, t), mix(shipped, puffy, t - 1.0), puff > 0.5);
}

// The puffiness-derived shape of the layer. Built ONCE per pixel in cirrus_layer and handed to
// every field evaluation in the march, so the mixes are not repeated CIRRUS_STEPS times.
struct CirrusShape {
    aspect: f32,
    fine: f32,
    erode: f32,
    density: f32,
    thick: f32,
}

fn cirrus_shape() -> CirrusShape {
    // cloud4.w is the evolution vec4's pad lane, written by the RENDERER (the C++ side sends 0
    // there) — the same "ride a spare lane" trick the cirrus mode uses for output.w, and for the
    // same reason: WgrSkyLook's size is part of the ABI handshake and must not grow.
    let puff = clamp(sky.cloud4.w, 0.0, 1.0);
    var sh: CirrusShape;
    sh.aspect = cirrus_mix3(CIRRUS_ASPECT_FIBRE, CIRRUS_ASPECT_SHIPPED, CIRRUS_ASPECT_PUFF, puff);
    sh.fine = cirrus_mix3(CIRRUS_FINE_FIBRE, CIRRUS_FINE_SHIPPED, CIRRUS_FINE_PUFF, puff);
    sh.erode = cirrus_mix3(CIRRUS_ERODE_FIBRE, CIRRUS_ERODE_SHIPPED, CIRRUS_ERODE_PUFF, puff);
    sh.density = cirrus_mix3(CIRRUS_DENSITY_FIBRE, CIRRUS_DENSITY_SHIPPED,
                             CIRRUS_DENSITY_PUFF, puff);
    sh.thick = cirrus_mix3(CIRRUS_THICK_FIBRE, CIRRUS_THICK_SHIPPED, CIRRUS_THICK_PUFF, puff);
    return sh;
}

// Rotate a 2D lookup coordinate.
//
// The anisotropic stretch that makes cirrus read as fibres is applied in WORLD space BEFORE this,
// so rotating here changes only WHICH part of the tiling volume is read — the streaks still run
// along world X. That is exactly the point: it takes the tile lattice off the streak axis, so when
// a repeat does occur it lands on a diagonal instead of stacking into the horizontal rows the
// owner photographed. Costs two multiply-adds.
fn cirrus_rot(p: vec2<f32>, c: f32, s: f32) -> vec2<f32> {
    return vec2<f32>(p.x * c - p.y * s, p.x * s + p.y * c);
}

// Low-frequency DOMAIN WARP for the cirrus field: one tap at ~1/9 the sheet frequency (a ~64 km
// period), rotated off-axis, displacing the sheet lookup by up to +-CIRRUS_WARP/2 metres.
//
// This is the single strongest of the four anti-repetition measures, because it is the one that
// attacks what the EYE actually locks onto. A tiling texture's repeat is legible not because the
// values recur but because the recurrence is on a straight, axis-aligned lattice: the same wisp
// appears at the same screen height, over and over, in a row. Bending the lookup by a large
// fraction of a tile means the tile boundary is no longer a straight line anywhere in the sky, so
// there is no repeating landmark left to find.
//
// Hoisted OUT of the volumetric march (evaluated once, at the shell midpoint) — the field varies
// over ~64 km and the marched column spans at most thick/CIRRUS_PATH_SIN_MIN horizontally (3-9 km
// depending on puffiness), so reusing it across the shell is a slow shear, not a seam, and it
// saves five taps per pixel.
fn cirrus_warp(world_xz: vec2<f32>) -> vec2<f32> {
    let q = cirrus_rot(world_xz * CIRRUS_SCALE * 0.11, 0.87630, 0.48178);
    let n = textureSampleLevel(cloud_noise, cloud_samp,
        vec3<f32>(q.x, 0.17 + sky.cloud4.z * CIRRUS_SCALE * 0.5, q.y), 0.0).rg;
    return (n - vec2<f32>(0.5, 0.5)) * CIRRUS_WARP;
}

// Cirrus density fraction [0,1] at a world XZ, at normalised height `h` through the shell.
//
// WHY THIS IS NOT ONE TAP ANY MORE. The previous version took the sheet from a single lookup at
// (x*s*0.25, const, z*s) and the fibres from a single lookup at (x*s*0.9, const, z*s*3.5). The
// noise volume tiles with period 1 on every axis, so that sheet repeated EXACTLY every 28 km in
// world X and every 7 km in world Z, the fibres every 7.8 km and 2 km — and 7000/2000 = 7/2 is
// rational, so the two agreed on a common period of 14 km in Z. Seen from 6.5 km below, 14 km of
// sheet subtends a few degrees, which put four or five identical copies of the same wisp in one
// frame, stacked in rows. That is the reported artefact, and it was arithmetic, not bad luck.
//
// Four measures, none of which is "make the tile bigger" (that only pushes the repeat further out
// and costs resolution):
//   1. domain warp, above — bends the lattice so it is not straight;
//   2. TWO sheet octaves whose scales are in the golden ratio. An irrational ratio has NO common
//      period, so their sum never recurs, at any distance;
//   3. rotation, different per octave — the lattices are not even parallel to each other;
//   4. slice drift on the volume's third axis, proportional to world position. Two points a whole
//      tile apart in X/Z now read DIFFERENT slices, so the repeat would have to align in three
//      axes at once. Free: it is an add on a coordinate the tap already carries.
fn cirrus_field(world_xz: vec2<f32>, h: f32, cover: f32, warp: vec2<f32>, sh: CirrusShape) -> f32 {
    let s = CIRRUS_SCALE;
    let drift = sky.cloud4.z * s;
    let p = world_xz + warp;
    // Slice drift (4). The two weights are irrational multiples of each other so the x and z
    // contributions do not come back into phase either. `h` walks the same axis, which is what
    // gives the marched shell genuine vertical structure.
    let slice = (p.x * 0.61803399 + p.y * 0.41421356) * s * 0.043 + h * CIRRUS_SLICE_H;
    // Octave A: the sheet itself. Compressed along world X by sh.aspect (PUFFINESS 1), so at the
    // default 0.25 a streak is ~7 km across and ~28 km long — the aspect a real cirrus field has —
    // while the puffy end shortens it toward round cirrocumulus cells.
    let qa = cirrus_rot(vec2<f32>(p.x * sh.aspect, p.y) * s, 0.95534, 0.29552);
    let a = textureSampleLevel(cloud_noise, cloud_samp,
        vec3<f32>(qa.x, 0.5 + slice + drift, qa.y), 0.0).r;
    // Octave B (2): same channel, same stretch, scale x1.618 and a different rotation.
    let qb = cirrus_rot(vec2<f32>(p.x * sh.aspect, p.y) * s * 1.61803399, -0.54030, 0.84147);
    let b = textureSampleLevel(cloud_noise, cloud_samp,
        vec3<f32>(qb.x, 0.29 - slice * 1.7 + drift, qb.y), 0.0).r;
    // Octave balance (PUFFINESS 2): puffier pushes weight onto the COARSE octave, so the mass
    // stays in whole cells instead of being broken up by the finer one.
    // Summing two independent fields shrinks the variance (at the default 0.62^2 + 0.38^2 = 0.53
    // of one tap), and the coverage remap below is a CONTRAST operation — left alone, the layer
    // would come out uniformly thin and hazy instead of having gaps. Re-expand about the mean by
    // 1/sqrt of that sum, which is where the old hard-coded 1.37 came from; computing it keeps the
    // gaps behaving the same at every octave balance instead of only at the default.
    let wb = sh.fine;
    let wa = 1.0 - wb;
    let mixed = a * wa + b * wb;
    let sheet = clamp(0.5 + (mixed - 0.5) * inverseSqrt(wa * wa + wb * wb), 0.0, 1.0);
    let d0 = clamp((sheet - (1.0 - cover)) / max(cover, 1e-3), 0.0, 1.0);
    if (d0 <= 0.0) {
        return 0.0; // in a gap -> skip the fibre tap (the empty-space fast path, as the deck does)
    }
    // Fibre erosion. z ratio 3.301 rather than the old 3.5: 3.5 = 7/2 shares a period with the
    // sheet, 3.301 does not.
    let qf = cirrus_rot(vec2<f32>(p.x * 0.9, p.y * 3.301) * s, 0.26236, -0.96496);
    let fibre = textureSampleLevel(cloud_noise, cloud_samp,
        vec3<f32>(qf.x, 0.25 + drift * 2.0 - slice * 0.9, qf.y), 0.0).g;
    // PUFFINESS 3: the striations themselves. The fibre lookup's own geometry is left alone —
    // only how hard it bites. Puffy cloud is not striated cloud with the striations moved, it is
    // cloud that was never sheared.
    return clamp(d0 - (1.0 - d0) * fibre * sh.erode, 0.0, 1.0);
}

// Vertical profile through the shell: soft on both faces so the marched layer has no hard lid or
// floor. Its integral over h is 1 - 0.3/2 - 0.35/2 = 0.675, which is what the extinction divides
// by so a full-density column has the SAME total optical depth as the flat sheet.
fn cirrus_profile(h: f32) -> f32 {
    return smoothstep(0.0, 0.30, h) * (1.0 - smoothstep(0.65, 1.0, h));
}
const CIRRUS_PROFILE_INTEGRAL: f32 = 0.675;
// Extinction per metre for a shell of `thick` metres carrying `density` total optical depth. Both
// arguments move with puffiness, and dividing by the LIVE thickness is what stops a deeper shell
// from also being a whiter one — puffiness changes the layer's shape, not its exposure.
fn cirrus_sigma(sh: CirrusShape) -> f32 {
    return sh.density / (sh.thick * CIRRUS_PROFILE_INTEGRAL);
}

fn cirrus_layer(pos: vec3<f32>, dir: vec3<f32>, sun: vec3<f32>, radiance: f32, bg: vec3<f32>,
                max_dist: f32) -> CloudResult {
    var res: CloudResult;
    res.inscatter = vec3<f32>(0.0);
    res.trans = 1.0;
    // sky.output.w: 0 = layer off entirely (Sky tab), 1 = flat, 2 = volumetric.
    let mode = sky.output.w;
    // Follows the deck's on/off so "clear weather" stays clear, and skips downward rays outright:
    // from inside the shell every direction has a far intersection with the sheet, including ones
    // pointing at the ground, and drawing those would paint cirrus under the horizon.
    if (mode < 0.5 || sky.cloud0.x <= 0.001 || dir.y <= 0.01) {
        return res;
    }
    let planet_r = sky.params.z;
    let cover = CIRRUS_COVER * smoothstep(0.02, 0.35, sky.cloud0.x);
    let cos_t = dot(dir, sun);
    // Ice: a strong forward lobe (the halo/silver look) over an isotropic floor. Thin enough that
    // self-shadowing is not worth a light march, so no sun-transmittance term through the sheet.
    let phase = mix(hg(cos_t, 0.75), 1.0 / (4.0 * PI), 0.55);
    let sun_col = sample_transmittance(pos, sun) * radiance;
    let luma = dot(bg, vec3<f32>(0.2126, 0.7152, 0.0722));
    let ambient = mix(bg, vec3<f32>(luma), 0.3) * sky.cloud2.z * 0.8;
    let lit = sun_col * phase + ambient;
    // Fade in off the horizon, where the sheet is hundreds of km away and haze owns the sky.
    let horizon_fade = smoothstep(0.015, 0.07, dir.y);
    // Puffiness resolved ONCE for this pixel — the march below reuses it for every step.
    let sh = cirrus_shape();

    if (mode < 1.5) {
        // ---- FLAT: one intersection with the sheet at the shell centre, one field evaluation.
        let hit = ray_sphere(pos, dir, planet_r + CIRRUS_ALT);
        let t = hit.y;
        if (t <= 0.0 || t >= max_dist) {
            return res; // no intersection, or the opaque scene is nearer — terrain occludes it
        }
        let p = pos + dir * t;
        // Same world anchoring and wind offset as the deck (cloud1.xy is CPU-wrapped for precision,
        // so it must be used as given, not rescaled to fake a different wind speed aloft).
        let world_xz = sky.cam_pos.xz + p.xz + sky.cloud1.xy;
        var d = cirrus_field(world_xz, 0.5, cover, cirrus_warp(world_xz), sh);
        d = d * horizon_fade * (1.0 - smoothstep(CIRRUS_FADE * 0.4, CIRRUS_FADE, t));
        if (d <= 0.001) {
            return res;
        }
        // Slant path through a thin sheet goes as 1/sin(elevation); clamped because the horizon
        // limit is unbounded and would turn the far sheet into a solid white band.
        let trans = exp(-d * sh.density / clamp(dir.y, CIRRUS_PATH_SIN_MIN, 1.0));
        res.inscatter = lit * (1.0 - trans);
        res.trans = trans;
        return res;
    }

    // ---- VOLUMETRIC: march the shell between its bottom and top spheres. The shell GROWS with
    // puffiness (PUFFINESS 4) — vertical development is what the word means — but the step COUNT
    // does not, so a deeper layer costs the same as a shallow one.
    let r_bot = planet_r + CIRRUS_ALT - sh.thick * 0.5;
    let r_top = planet_r + CIRRUS_ALT + sh.thick * 0.5;
    let hb = ray_sphere(pos, dir, r_bot);
    let ht = ray_sphere(pos, dir, r_top);
    if (ht.y <= 0.0) {
        return res; // above the shell, or no intersection at all
    }
    // Below the shell: hb.y is the entry. Inside it (or above r_bot): hb misses on an upward ray
    // and returns the (1,-1) sentinel, so max(.,0) correctly starts the march at the camera.
    let t0 = max(hb.y, 0.0);
    let t1 = min(min(ht.y, max_dist), t0 + sh.thick / CIRRUS_PATH_SIN_MIN);
    if (t1 <= t0) {
        return res;
    }
    let dt = (t1 - t0) / f32(CIRRUS_STEPS);
    // ONE warp tap for the whole column (see cirrus_warp) — evaluated at the shell midpoint.
    let mid = pos + dir * (t0 + (t1 - t0) * 0.5);
    let warp = cirrus_warp(sky.cam_pos.xz + mid.xz + sky.cloud1.xy);
    let sigma = cirrus_sigma(sh);
    var col = vec3<f32>(0.0);
    var trans = 1.0;
    var t = t0 + dt * 0.5;
    for (var i: i32 = 0; i < CIRRUS_STEPS; i = i + 1) {
        let p = pos + dir * t;
        let h = (length(p) - r_bot) / sh.thick;
        let prof = cirrus_profile(h);
        if (prof > 0.0) {
            let world_xz = sky.cam_pos.xz + p.xz + sky.cloud1.xy;
            var d = cirrus_field(world_xz, h, cover, warp, sh) * prof;
            d = d * horizon_fade * (1.0 - smoothstep(CIRRUS_FADE * 0.4, CIRRUS_FADE, t));
            if (d > 0.0005) {
                let seg = exp(-d * sigma * dt);
                col += trans * lit * (1.0 - seg);
                trans *= seg;
            }
        }
        t += dt;
    }
    res.inscatter = col;
    res.trans = trans;
    return res;
}

// Composite the cumulus deck OVER the cirrus sheet: the deck is nearer, so its transmittance
// attenuates everything behind it. Returns the pair the caller hands to the over-scene blend
// (out = inscatter + scene * trans), where `scene` for a sky pixel is fs_sky's atmosphere.
fn cloud_over_cirrus(deck: CloudResult, cirrus: CloudResult) -> CloudResult {
    var res: CloudResult;
    res.inscatter = deck.inscatter + deck.trans * cirrus.inscatter;
    res.trans = deck.trans * cirrus.trans;
    return res;
}

// ---- Moon disc ---------------------------------------------------------------
// A SHADED SPHERE, not a phase billboard. The lunar surface normal is reconstructed per
// pixel inside the disc and lit with the real sub-solar direction, so the terminator gets
// both its correct CURVATURE (an ellipse, not a straight cut) and its correct TILT
// relative to the horizon for free. A rotated phase texture can match one of those or the
// other, and the tilt is the one people notice.
//
// The reflectance is Lommel-Seeliger (I = ndl / (ndl + ndv)) rather than Lambert. That is
// what makes the real moon read as a flat disc instead of a shaded ball: the regolith is a
// deep, dark, porous scattering layer, and a Lambert full moon has obviously wrong dark
// limbs.
fn moon_disc(dir: vec3<f32>, pos: vec3<f32>) -> vec3<f32> {
    if (sky.moon_params.w < 0.5) {
        return vec3<f32>(0.0);
    }
    let moon = normalize(sky.moon_dir.xyz);
    let cos_a = dot(dir, moon);
    let radius = max(sky.moon_params.x, 1.0e-4);
    let cos_r = cos(radius);
    if (cos_a <= cos_r) {
        return vec3<f32>(0.0);
    }
    // Tangent frame on the disc. World up is degenerate at the zenith, so seed elsewhere.
    var seed = vec3<f32>(0.0, 1.0, 0.0);
    if (abs(moon.y) > 0.99) {
        seed = vec3<f32>(1.0, 0.0, 0.0);
    }
    let tx = normalize(cross(seed, moon));
    let ty = cross(moon, tx);
    // Offset of this ray from the disc centre, in units of the disc radius. The small-angle
    // form is exact to a part in 1e5 at a quarter of a degree.
    let off = dir - moon * cos_a;
    let u = dot(off, tx) / radius;
    let v = dot(off, ty) / radius;
    let r2 = u * u + v * v;
    if (r2 >= 1.0) {
        return vec3<f32>(0.0);
    }
    let nz = sqrt(max(0.0, 1.0 - r2));
    // Outward normal of the VISIBLE hemisphere: -moon at the disc centre, perpendicular to
    // the view at the limb.
    let n = tx * u + ty * v - moon * nz;
    let l = normalize(sky.moon_sun.xyz);
    let ndl = dot(n, l);
    let ndv = nz;
    let lit = max(ndl, 0.0);
    let ls = lit / max(lit + ndv, 1.0e-3);
    let brdf = mix(ls, lit, 0.15);
    // Earthshine: the dark side of a crescent is genuinely visible, lit by a nearly full
    // Earth. It is the detail that makes a young moon read as a sphere rather than a
    // cut-out. Artistically scaled — the true ratio is ~1e-4 and would vanish here.
    let earthshine = max(sky.moon_sun.w, 0.0);
    // Soften the limb over the outer 3% of the radius so the edge does not alias.
    let edge = 1.0 - smoothstep(0.97, 1.0, sqrt(r2));
    // Radiance from irradiance / solid angle, exactly as the sun disc does below, so
    // inflating the disc for visibility lowers its radiance and total power holds.
    let solid_angle = 2.0 * PI * (1.0 - cos_r);
    let scale = sky.moon_params.z / solid_angle;
    let tint = vec3<f32>(1.0, 0.97, 0.92);
    let moon_t = sample_transmittance(pos, moon);
    return tint * (brdf + earthshine) * edge * scale * moon_t;
}

@fragment
fn fs_sky(in: VsOut) -> @location(0) vec4<f32> {
    let dir = normalize(in.ray_dir);
    let sun = normalize(sky.sun_dir.xyz);
    let planet_r = sky.params.z;
    let cam_alt = max(sky.night_zenith.w, 0.0);
    let pos = vec3<f32>(0.0, planet_r + cam_alt, 0.0);

    var color = sky_radiance(dir);

    // Clouds are NO LONGER drawn here — they are a depth-aware over-scene composite (fs_cloud +
    // fs_cloud_composite) so they occlude terrain and envelop the camera when flown through. The sky
    // pass draws only the atmosphere + sun disc; the cloud composite runs after geometry and correctly
    // occludes this disc where a cloud crosses it.

    // Procedural sun disc, attenuated by atmospheric transmittance (reddens at low sun). Only when
    // the view ray misses the planet. Added here (not in sky_radiance) so the water reflection —
    // which uses its own analytic glint — doesn't double up on the sun.
    let ground = ray_sphere(pos, dir, planet_r);
    if (ground.x <= 0.0) {
        let cos_sun = dot(dir, sun);
        let cos_radius = cos(sky.params.x);
        if (cos_sun > cos_radius) {
            let sun_t = sample_transmittance(pos, sun);
            let edge = clamp((cos_sun - cos_radius) / (1.0 - cos_radius), 0.0, 1.0);
            let limb = 0.4 + 0.6 * sqrt(edge);
            // sun_dir.w * params.y is the solar IRRADIANCE (the scale that drives the sky
            // scattering integral). The disc's RADIANCE is that divided by the sun's solid
            // angle Omega = 2*pi*(1 - cos(radius)) — ~1e4x larger, which is what makes the
            // disc eye-searingly bright (HDR + bloom) instead of matching the surrounding
            // haze. Dividing by the ACTUAL drawn radius keeps it energy-conserving: inflating
            // the disc for visibility lowers its radiance so total power stays fixed.
            let solid_angle = 2.0 * PI * (1.0 - cos_radius);
            let disc = (sky.sun_dir.w * sky.params.y) / solid_angle;
            color = color + disc * sun_t * limb;
        }
        // Moon disc, same gate: only where the view ray misses the planet, and drawn here in
        // fs_sky (not in sky_radiance) so the depth-aware cloud composite that runs after
        // geometry occludes it exactly as it occludes the sun.
        color = color + moon_disc(dir, pos);
    }

    // Linear HDR path: hand linear radiance to the tonemap resolve. LDR-direct: no resolve runs,
    // so self-tonemap (exposure already baked into the radiance scale).
    if (sky.output.x < 0.5) {
        color = hable(color);
        color = linear_to_srgb(color);
        color += (ign(in.clip.xy) - 0.5) / 255.0;
    }
    return vec4<f32>(color, 1.0);
}

// Reflection environment map: bakes sky_radiance into an equirectangular (lat-long) texture the
// water surface samples in its reflected view direction. Always LINEAR radiance (disc-free), so
// water Fresnel-mixes it directly on the HDR path; the fullscreen sky/tonemap are unaffected.
// UV convention (must match water.wgsl's dir_to_equirect): u = azimuth, v = 0 at zenith .. 1 nadir.
@fragment
fn fs_sky_env(in: VsOut) -> @location(0) vec4<f32> {
    let azimuth = (in.uv.x - 0.5) * (2.0 * PI);
    let polar = in.uv.y * PI;              // 0 = up, PI = down
    let sp = sin(polar);
    let dir = vec3<f32>(sp * cos(azimuth), cos(polar), sp * sin(azimuth));
    var color = sky_radiance(dir);
    // Cheap low-step cloud path (few primary + light steps): the 256x128 env map is re-baked
    // every frame and projected to SH-9, so the full detailed march would both cost a lot AND
    // alias the wind-scrolled noise into temporal SH flicker. Low frequencies are all SH keeps.
    let cam_alt = max(sky.night_zenith.w, 0.0);
    let pos = vec3<f32>(0.0, sky.params.z + cam_alt, 0.0);
    let sun = normalize(sky.sun_dir.xyz);
    // Sky-only (no scene depth) -> a large far bound; composite the clouds over the sky ourselves.
    // NO per-pixel jitter here: at the 256x128 env resolution the interleaved-gradient offset reads as
    // a coarse checkerboard once reflected in the water (which magnifies each env texel). A jitter-free
    // march bands slightly instead, but the low step count + huge texels blur that away; the smooth
    // result is what the SH projection + reflection want.
    let radiance = sky.sun_dir.w * sky.params.y;
    let deck = march_clouds(pos, dir, sun, radiance, color, 12.0, 2, 0.0, 1e12);
    // The cirrus layer goes into the env bake too, so the water reflects — and the SH ambient sees —
    // the same two-layer sky the camera does. It runs in whatever mode the Sky tab selected, and
    // the env map is only 256x128, so even the volumetric path is a rounding error here; the
    // measured cost of the layer as a whole is dominated by the full-resolution cloud pass.
    let cloud = cloud_over_cirrus(deck, cirrus_layer(pos, dir, sun, radiance, color, 1e12));
    return vec4<f32>(color * cloud.trans + cloud.inscatter, 1.0);
}

// ---- Depth-aware over-scene cloud march (plan Phase 1) -----------------------
// Marches the clouds at LOW RES into a single-sample (inscatter.rgb, transmittance.a) buffer,
// bounding each ray at the opaque scene surface so clouds occlude terrain and envelop the camera
// when flown through. A separate composite shader (cloud_composite.wgsl) upsamples this buffer and
// blends it over the lit scene as out = inscatter + scene*transmittance (fixed-function blend).
// The resolved single-sample prepass depth (reversed-Z: 0 = far/sky) rides group(1) binding 6 —
// binding 6 avoids the froxel's group(1) bindings 0-5.
@group(1) @binding(6) var scene_depth: texture_depth_2d;

@fragment
fn fs_cloud(in: VsOut) -> @location(0) vec4<f32> {
    let dir = normalize(in.ray_dir);
    let sun = normalize(sky.sun_dir.xyz);
    let cam_alt = max(sky.night_zenith.w, 0.0);
    let pos = vec3<f32>(0.0, sky.params.z + cam_alt, 0.0);

    // Opaque scene distance along this ray: reconstruct the camera-relative scene position from the
    // resolved depth (forward ndc.z = 1 - stored, like water's seabed_depth). d == 0 => sky => no
    // occluder, march to the shell far exit. The march origin is at the camera, so length = distance.
    let dd = vec2<f32>(textureDimensions(scene_depth));
    let px = vec2<i32>(clamp(in.uv, vec2<f32>(0.0), vec2<f32>(0.9999)) * dd);
    let d = textureLoad(scene_depth, px, 0);
    let ndc = in.uv * vec2<f32>(2.0, -2.0) + vec2<f32>(-1.0, 1.0);
    var scene_dist = 1e12;
    if (d > 0.0) {
        let wh = sky.inv_view_proj * vec4<f32>(ndc, 1.0 - d, 1.0);
        scene_dist = length(wh.xyz / wh.w);
    }

    let bg = sky_radiance(dir); // local sky colour for the cloud ambient
    let radiance = sky.sun_dir.w * sky.params.y;
    let deck = march_clouds(pos, dir, sun, radiance, bg,
                            CLOUD_STEPS, CLOUD_LIGHT_STEPS, hash21(in.clip.xy), scene_dist);
    // Second layer, behind the deck. Bounded by the same scene distance, so terrain occludes it.
    let cloud = cloud_over_cirrus(deck, cirrus_layer(pos, dir, sun, radiance, bg, scene_dist));

    // ---- Lens flare -----------------------------------------------------------------------
    // Written here rather than as its own pass because this shader already holds everything a
    // correctly-occluded flare needs: the sun direction, this pixel's view ray, the cloud
    // transmittance along it, and scene_dist -- the distance to the opaque surface, which IS the
    // terrain-and-buildings occlusion test. A separate full-screen pass would have had to
    // re-derive all four.
    //
    // Occlusion is therefore not approximated:
    //   * geometry -- a pixel whose ray hits something before the sky has scene_dist < 1e11, and
    //     the flare is cut there, so it cannot leak through a mountain or a wall;
    //   * cloud -- cloud.trans is the transmittance this ray already measured through BOTH layers,
    //     so a thick cloud (or a cirrus veil) crossing the sun dims the flare by exactly what it
    //     dims the sun by, and it does so continuously, with no pop at the cloud edge;
    //   * horizon -- faded out as the sun sets rather than cut, so it does not blink at the moment
    //     the disc touches the ground.
    //
    // Restrained on purpose: a tight core plus one wide halo, no ghost chain. The intensity rides
    // sky.output.z so Dev Tools can turn it off entirely.
    let flare_gain = sky.output.z;
    if (flare_gain > 0.0 && scene_dist > 1e11) {
        let cos_sun = dot(dir, sun);
        if (cos_sun > 0.9) {
            // Angular falloff, not a screen-space sprite: this stays correct at any FOV and needs
            // no projection of the sun into screen space.
            let core = pow(clamp((cos_sun - 0.9) / 0.1, 0.0, 1.0), 12.0);
            let halo = pow(clamp((cos_sun - 0.9) / 0.1, 0.0, 1.0), 2.5) * 0.28;
            // Below the horizon the sun is gone; fade over a few degrees so nothing snaps.
            let horizon = smoothstep(-0.05, 0.05, sun.y);
            let sun_col = sample_transmittance(pos, sun) * (sky.sun_dir.w * sky.params.y);
            let strength = (core + halo) * horizon * cloud.trans * flare_gain;
            // Added to the inscatter lane, which the composite already adds over the scene, so the
            // flare needs no blend state of its own.
            return vec4<f32>(cloud.inscatter + sun_col * strength * 0.02, cloud.trans);
        }
    }
    return vec4<f32>(cloud.inscatter, cloud.trans);
}

// ---- Aerial-perspective froxel volume (fill) ---------------------------------
// The Forward+-native replacement for the deferred fs_aerial pass: a frustum-aligned
// 3D volume where each froxel stores the atmosphere in-scattered toward the camera and
// the transmittance from the camera up to that froxel's distance. The forward shaders
// then apply fog with ONE trilinear tap at the fragment's froxel coordinate, so every
// fragment (terrain, object, foliage, transparent) fogs by its own distance and 2D is
// simply never sampled — no deferred depth readback, no render-order split.
//
// XY = screen; the Z slice maps to distance with a SQUARED distribution (dense near the
// camera): texel-centre w in [0,1] -> dist = max_dist * w^2, so sampling is
// w = sqrt(dist / max_dist). Filled once per frame by marching each column front-to-back
// and storing the running (inscatter, transmittance) at each slice — O(depth) per column.
// Reuses the exact sky atmosphere (LUTs + phase), so the froxel fog matches the sky.
// (Stage 1: fill only. Forward-shader sampling + sun-shadowing land in later stages.)
@group(0) @binding(5) var froxel_out: texture_storage_3d<rgba16float, write>;

// Group(1): the long-range terrain sun-shadow mask (same world-space "shadow ceiling"
// map the forward shaders sample via frame::terrain_sun_shadow), lent to the froxel fill
// so the fog is occluded by terrain — the sun stops shining THROUGH hills into the haze,
// and gaps between ridges become god-ray shafts. Standalone-validated shader, so the
// struct + sampling are duplicated here rather than imported. Matches TerrainShadowMap.
struct FroxelShadow {
    origin: vec2<f32>,     // world xz of the mask's (0,0)
    inv_span: vec2<f32>,   // world-xz -> [0,1] over the map
    half_texel: vec2<f32>, // 0.5 / mask_dims
    enabled: f32,          // 0 until a heightmap is loaded
    pad: f32,
};
@group(1) @binding(0) var shadow_mask: texture_2d<f32>;
@group(1) @binding(1) var shadow_samp: sampler;
@group(1) @binding(2) var<uniform> shadow_map: FroxelShadow;

// Occlusion [0,1] of the sun by terrain at an ABSOLUTE world position: 0 = lit, 1 = fully
// terrain-shadowed. Mirror of frame::terrain_sun_shadow (a point at (xz, y) is occluded by
// how far y sits below the column's shadow ceiling, softened by the penumbra half-width).
fn terrain_occlusion(world_xz: vec2<f32>, world_y: f32) -> f32 {
    if (shadow_map.enabled < 0.5) {
        return 0.0;
    }
    let uv = (world_xz - shadow_map.origin) * shadow_map.inv_span + shadow_map.half_texel;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
        return 0.0;
    }
    let sm = textureSampleLevel(shadow_mask, shadow_samp, uv, 0.0);
    let lit = smoothstep(sm.r - sm.g, sm.r + sm.g + 1e-3, world_y);
    return clamp(sm.b * (1.0 - lit), 0.0, 1.0);
}

// Cascade shadow map (near field, objects + terrain), so the fog is occluded by SHARP
// casters — tree trunks, buildings, ridgelines — which the smooth terrain-ceiling mask
// can't resolve. That's what carves crisp god-ray shafts. Mirrors WgrCameraShadow; the
// fog has no surface normal / screen derivatives, so this is a simplified single-tap
// version of frame::shadow_strength (no PCF, no normal/plane bias — just a constant depth
// bias), which is plenty for the soft 32^3 volume.
struct FroxelCsm {
    cascade_vp: array<mat4x4<f32>, 4>,
    splits: vec4<f32>,      // frustum tiers: far eye-depth per tier
    omni_radius: vec4<f32>, // omni tiers: camera-distance radius
    ctl: vec4<f32>,         // {count, omni_count, fade_range, bias_const}
    ctlb: vec4<f32>,        // {texel_size, darkness, normal_offset_scale, pcf}
    cam_fwd: vec4<f32>,     // xyz = camera forward (eye-depth cascade select)
    sun_dir: vec4<f32>,
};
@group(1) @binding(3) var csm_tex: texture_depth_2d_array;
@group(1) @binding(4) var csm_cmp: sampler_comparison;
@group(1) @binding(5) var<uniform> csm: FroxelCsm;

// Occlusion [0,1] of the sun by the cascade shadow map at a camera-relative position
// (1 = shadowed). Same cascade select + far-fade as shadow_strength, single compare tap.
fn csm_occlusion(pos: vec3<f32>) -> f32 {
    let n = i32(csm.ctl.x);
    if (n <= 0) {
        return 0.0;
    }
    let omni_n = i32(csm.ctl.y);
    let eye_depth = dot(pos, csm.cam_fwd.xyz);
    let dist3d = length(pos);
    var ci = n;
    for (var i = 0; i < 4; i++) {
        if (i >= n) {
            break;
        }
        let metric = select(eye_depth, dist3d, i < omni_n);
        if (metric <= csm.splits[i]) {
            ci = i;
            break;
        }
    }
    if (ci >= n) {
        return 0.0;
    }
    let cp = csm.cascade_vp[ci] * vec4<f32>(pos, 1.0);
    let sc = cp.xyz / cp.w;
    let suv = vec2<f32>(sc.x * 0.5 + 0.5, 0.5 - sc.y * 0.5);
    if (suv.x <= 0.0 || suv.x >= 1.0 || suv.y <= 0.0 || suv.y >= 1.0 || sc.z <= 0.0 || sc.z >= 1.0) {
        return 0.0;
    }
    let bias = csm.ctl.w * f32(ci + 1) * f32(ci + 1);
    let lit = textureSampleCompareLevel(csm_tex, csm_cmp, suv, ci, sc.z - bias);
    // Fade out over the last cascade's tail exactly like shadow_strength, so the near-field
    // CSM occlusion hands off to the long-range terrain mask without a hard edge.
    let fade = clamp((csm.splits[n - 1] - eye_depth) / max(csm.ctl.z, 0.001), 0.0, 1.0);
    return (1.0 - lit) * fade;
}

// CLD-020 -- cloud shadows.
//
// Marches the SAME cloud_density field the sky raymarch uses, from a ground point toward the sun,
// and stores the resulting transmittance in a world-anchored 2D map. Every lit surface then costs
// one texture tap instead of a raymarch, which is the whole point: per-pixel marching would be
// correct and unaffordable, while this is one dispatch shared by terrain, objects and grass.
//
// Deliberate limitations, so nobody has to rediscover them:
//   * Flat map. Transmittance is evaluated at ONE altitude, so a hillside and the valley under it
//     get the same shadow. Cloud shadows are enormous and soft, so this reads correctly; it would
//     not for a sharp caster.
//   * Camera-centred and finite. Outside the span, surfaces read fully lit rather than dark --
//     absence of data must never invent shadow.
//   * The map is snapped to its own texel grid, NOT to the camera. An unsnapped origin makes the
//     whole pattern crawl with every camera movement, which reads as the clouds sliding.
@compute @workgroup_size(8, 8, 1)
fn cs_cloud_shadow(@builtin(global_invocation_id) gid: vec3<u32>) {
    let dims = textureDimensions(cloud_shadow_out);
    if (gid.x >= dims.x || gid.y >= dims.y) {
        return;
    }
    // Fully lit is the safe default: a disabled or degenerate pass must not darken the world --
    // except for smoke, which is not the cloud deck and shadows the ground whether or not the
    // cloud shadow feature is on.
    if (sky.cloud_shadow.w <= 0.0 || sky.cloud0.x <= 0.001) {
        let span0 = 1.0 / max(sky.cloud_shadow.z, 1e-6);
        let texel0 = span0 / f32(dims.x);
        let world_xz0 = sky.cloud_shadow.xy + (vec2<f32>(f32(gid.x), f32(gid.y)) + 0.5) * texel0;
        let smoke0 = smoke_ground_shadow(world_xz0, -normalize(sky.sun_dir.xyz), texel0);
        textureStore(cloud_shadow_out, vec2<i32>(gid.xy), vec4<f32>(smoke0, 0.0, 0.0, 1.0));
        return;
    }
    let span = 1.0 / max(sky.cloud_shadow.z, 1e-6);
    let texel = span / f32(dims.x);
    let world_xz = sky.cloud_shadow.xy + (vec2<f32>(f32(gid.x), f32(gid.y)) + 0.5) * texel;

    let sun = normalize(sky.sun_dir.xyz);
    // Smoke shadow is independent of the cloud deck: computed once here and
    // folded into EVERY store below, including the early "fully lit" returns,
    // so a plume shadows the ground on a clear day. sky.sun_dir points TOWARD
    // the sun; the light travels the other way.
    let smoke = smoke_ground_shadow(world_xz, -sun, texel);
    // Below the horizon there is no sun to occlude. Returning lit here also stops the march from
    // running against a ray that never reaches the deck.
    if (sun.y <= 0.02) {
        textureStore(cloud_shadow_out, vec2<i32>(gid.xy), vec4<f32>(smoke, 0.0, 0.0, 1.0));
        return;
    }

    // Planet-centred position of this ground texel. cloud_world() reconstructs the world xz as
    // cam_pos.xz + p.xz, so p.xz must be CAMERA-RELATIVE for the noise to stay world-anchored.
    let rel = world_xz - sky.cam_pos.xz;
    // Evaluated at SEA LEVEL, not at the terrain height under the texel: the deck sits ~1.5 km up,
    // so a few hundred metres of terrain moves the sun ray by a fraction of a cloud, and sea level
    // is a value this pass can trust without a heightmap lookup per texel.
    let ground = vec3<f32>(rel.x, sky.params.z, rel.y);

    let interval = cloud_interval(ground, sun);
    if (interval.y <= interval.x) {
        textureStore(cloud_shadow_out, vec2<i32>(gid.xy), vec4<f32>(smoke, 0.0, 0.0, 1.0));
        return;
    }

    let steps = 12;
    let dt = (interval.y - interval.x) / f32(steps);
    var tau = 0.0;
    for (var i = 0; i < steps; i = i + 1) {
        // Mid-point sampling: with only twelve steps through a slab, sampling at the segment start
        // biases the whole integral toward the deck's underside.
        let t = interval.x + (f32(i) + 0.5) * dt;
        tau = tau + cloud_density(ground + sun * t);
    }
    let transmittance = exp(-tau * dt * sky.cloud0.y);
    // Strength lerps toward fully lit rather than scaling the transmittance, so turning it down
    // lightens the shadows instead of shifting what counts as cloud.
    let shadow = mix(1.0, clamp(transmittance, 0.0, 1.0), clamp(sky.cloud_shadow.w, 0.0, 1.0));
    textureStore(cloud_shadow_out, vec2<i32>(gid.xy), vec4<f32>(shadow * smoke, 0.0, 0.0, 1.0));
}

@compute @workgroup_size(8, 8, 1)
fn cs_froxel(@builtin(global_invocation_id) gid: vec3<u32>) {
    let dims = textureDimensions(froxel_out);
    if (gid.x >= dims.x || gid.y >= dims.y) {
        return;
    }

    // View ray for this column: unproject the pixel centre at the NEAR plane (NDC z = 0,
    // safe under the infinite-far projection), translation-free so the length is metric.
    let uv = (vec2<f32>(f32(gid.x), f32(gid.y)) + 0.5) / vec2<f32>(f32(dims.x), f32(dims.y));
    let ndc = uv * vec2<f32>(2.0, -2.0) + vec2<f32>(-1.0, 1.0);
    let world = sky.inv_view_proj * vec4<f32>(ndc, 0.0, 1.0);
    let ray = normalize(world.xyz / world.w);

    let sun = normalize(sky.sun_dir.xyz);
    let cam_alt = max(sky.night_zenith.w, 0.0);
    let origin = vec3<f32>(0.0, sky.params.z + cam_alt, 0.0);
    let radiance = sky.sun_dir.w * sky.params.y;
    let max_dist = max(sky.night_params.w, 1.0);
    let depth = f32(dims.z);

    let cos_t = dot(ray, sun);
    let rp = rayleigh_phase(cos_t);
    let mp = mie_phase(cos_t);

    // Full sky radiance along this column (horizon-floored exactly like fs_sky, so a
    // downward terrain ray still resolves the HORIZON sky it should dissolve into). The
    // far froxels blend toward this so the terrain edge and newly-streamed tiles fade into
    // the real sky instead of the dim airlight-to-fog-range (which read as a grey band and
    // let geometry pop against the sky). This is the froxel-native far-fade.
    let march_dir = normalize(vec3<f32>(ray.x, max(ray.y, 0.0), ray.z));
    let atmo = ray_sphere(origin, march_dir, sky.params.z + sky.params.w);
    var sky_full = vec3<f32>(0.0);
    if (atmo.y > 0.0) {
        sky_full = raymarch_sky(origin, march_dir, sun, atmo.y) * radiance;
    }

    // March front-to-back, accumulating into each slice. Two analytic sub-steps per slice
    // keep the thick far froxels honest; the near ones are thin so it's cheap.
    let sub = 2u;
    var lum = vec3<f32>(0.0);
    var trans = vec3<f32>(1.0);
    var t_prev = 0.0;
    for (var z = 0u; z < dims.z; z = z + 1u) {
        let w_center = (f32(z) + 0.5) / depth;
        let t_target = max_dist * w_center * w_center;
        let seg = (t_target - t_prev) / f32(sub);
        for (var s = 0u; s < sub; s = s + 1u) {
            let t0 = t_prev + seg * f32(s);
            let dt = seg;
            let march = t0 + dt * 0.5;
            let p = origin + ray * march;
            let m = scattering_values(p);
            let safe_ext = max(m.extinction, vec3<f32>(1e-9));
            let sample_trans = exp(-dt * m.extinction);
            let sun_t = sample_transmittance(p, sun);
            let psi = sample_multiscatter(p, sun);
            // Occlude the DIRECT sun single-scatter by terrain (the froxel sample's absolute
            // world position = camera + the marched camera-relative offset). Multiscatter
            // (psi) stays as ambient fill, so shadowed fog is dim-but-not-black. This is what
            // stops the low sun bleeding through ridges and carves god-ray shafts.
            let world_off = ray * march;
            // Fog occlusion by the long-range terrain shadow-ceiling mask (absolute world pos).
            // CSM froxel occlusion is DISABLED: the cascade range is far shorter than where the
            // fog is dense, so it never overlaps foggy regions and had zero visible effect. The
            // path is kept fully wired (csm_occlusion + group(1) bindings 3-5, csm_ubo upload) so
            // re-enabling is a one-line change once the CSM range is extended:
            //     let occ = max(csm_occlusion(world_off), occ);
            // See the wgpu-renderer-project memory, "Stage 3b".
            let occ = terrain_occlusion(sky.cam_pos.xz + world_off.xz, sky.cam_pos.y + world_off.y);
            // night_horizon.w = user occlusion strength (0 = off for A/B; 1 = physical; >1 exaggerated).
            let sun_vis = clamp(1.0 - occ * sky.night_horizon.w, 0.0, 1.0);
            let in_scat = m.rayleigh * (rp * sun_t * sun_vis + psi) + vec3<f32>(m.mie) * (mp * sun_t * sun_vis + psi);
            let scat_int = (in_scat - in_scat * sample_trans) / safe_ext;
            lum += scat_int * trans;
            trans = trans * sample_trans;
        }
        t_prev = t_target;
        // Blend the physical airlight toward the full sky over the far half of the volume,
        // so w -> 1 (the draw edge) is the sky and geometry there dissolves seamlessly.
        let farfade = smoothstep(0.5, 1.0, w_center);
        let col = mix(lum * radiance, sky_full, farfade);
        let t_mono = dot(trans, vec3<f32>(0.2126, 0.7152, 0.0722)) * (1.0 - farfade);
        textureStore(froxel_out, vec3<i32>(i32(gid.x), i32(gid.y), i32(z)), vec4<f32>(col, t_mono));
    }
}
