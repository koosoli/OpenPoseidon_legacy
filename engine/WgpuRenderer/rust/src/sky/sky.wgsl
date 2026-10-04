#import layered_fog_optics::{fog_neutral_radiance, fog_neutral_horizon}
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
    cam_pos: vec4<f32>,       // xyz = absolute camera; w = height above ocean, 0 disables ocean clipping
    // CLD-020 cloud sun-transmittance map. xy = world-xz of the map's min corner (SNAPPED to the
    // texel grid on the CPU, so the field does not crawl as the camera moves), z = 1/span in
    // metres, w = strength (0 = the pass is skipped entirely and every surface reads fully lit).
    cloud_shadow: vec4<f32>,
    // Moon disc. x = angular RADIUS (rad), y = illuminated fraction, z = disc radiance
    // scale (irradiance; divided by the solid angle in moon_disc), w = draw (0 = skip).
    moon_params: vec4<f32>,
    // xyz = unit dir TO the sun as seen from the MOON; w = earthshine reflectance floor.
    moon_sun: vec4<f32>,
    // Second cloud layer LOOK. Written by the RENDERER, not by C++ — same arrangement as
    // cloud4.w's puffiness, and for the same reason (WgrSkyLook's size is part of the ABI
    // handshake), except that this one needed a lane of its own because every spare `w` was
    // already spoken for. Both are eased on the Rust side, so a slider drag cannot step the sky.
    //   x = AMOUNT (0..1, 0.5 = the shipped look) — how MUCH cirrus there is: coverage first,
    //       optical depth second. This is the "more or less of it" control that on/off was not.
    //   y = MATCH  (0..1, 0   = the shipped look) — how far the layer takes on the CUMULUS
    //       DECK's character instead of high ice cloud's: altitude, feature size, isotropy,
    //       depth, opacity and phase all move together toward the deck. 1 = a second cumulus
    //       deck above the first rather than a cirrus veil.
    //   z = EDGE SOFTNESS (0..1, 0 = the layer exactly as it rendered before this existed).
    //   w = the Milky Way band's gain.
    cirrus: vec4<f32>,
    // SKY-002. x = STAR CLOUD-OCCLUSION strength [0,1]: how much of the star field is withheld
    // from the sky radiance that the cloud march uses as its AMBIENT SOURCE. The deck is lit by
    // `bg` (see march_clouds), and `bg` was the FULL sky radiance at that same pixel — stars
    // included — so every star re-lit the cloud standing in front of it and came back through an
    // opaque overcast at roughly the cloud's ambient fraction. 1 = the cloud is lit by the
    // STAR-FREE sky, so a deck hides the stars behind it exactly as it hides the sky behind them;
    // 0 = the pre-SKY-002 behaviour, bit for bit. Renderer-written (`WGR_STAR_OCCLUSION`).
    //
    // SKY-004. y = MOONLIGHT ON THE CLOUD DECKS, as a gain on the physical lunar irradiance the
    // moon disc already carries (1 = default, 0 = the pre-SKY-004 behaviour bit for bit, i.e. the
    // decks are lit by the sun alone and an overcast night is black). Renderer-written
    // (`WGR_MOON_CLOUDS`). z = STYLISTIC BLUE SHIFT [0,1] on that light only (`WGR_MOON_CLOUD_BLUE`,
    // default 0 = physical). Moonlight is REFLECTED SUNLIGHT and is therefore very close to the
    // sun's own colour -- the blue "moonlight" of film is an artefact of human scotopic vision, not
    // of the light -- so the blue is off unless somebody asks for it, and it is a look knob that
    // says so rather than a number quietly baked into the tint.
    // w = actual admitted weather-fog neutral chroma strength, reset per camera.
    night_sky: vec4<f32>,
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
// .r = CLOUD sun transmittance, .g = SMOKE sun transmittance. Two channels, not one
// product, because the consumers weigh them differently: the ground shadow wants r*g,
// while the god-ray march has to undo the cloud STRENGTH slider on r (which was never
// applied to smoke) and would otherwise distort the smoke term or, with cloud shadows
// off, lose it entirely. .b/.a unused.
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
// Star radiance alone: the catalogue field + the Milky Way band, gated to night by sun altitude
// and scaled by the master gain (Dev Tools -> Sky -> Stars -> "Star brightness"). Split out of
// sky_radiance so a caller can ask for a STAR-FREE sky; see sky_radiance_weighted.
fn star_radiance(dir: vec3<f32>, sun: vec3<f32>) -> vec3<f32> {
    let night_blend_stars = 1.0 - smoothstep(sky.night_params.y, sky.night_params.x, sun.y);
    let star_amount = max(sky.output.y, 0.0);
    if (star_amount > 0.001 && night_blend_stars > 0.0) {
        return star_field(dir, sun) * star_amount * night_blend_stars;
    }
    return vec3<f32>(0.0);
}

// `star_weight` scales the star term ONLY. 1 = the sky as the eye sees it. Anything less is the
// sky handed to the CLOUD march as its ambient source: the deck must not be lit by the stars it
// is standing in front of, or an opaque overcast returns them at its ambient fraction (~0.65 of
// the star's own radiance, measured). Weight 0 skips star_field entirely, so the cloud pass gets
// CHEAPER as well as correct — and it never had a valid `g_px_angle` to feed it anyway (the
// derivative is taken in fs_sky, not in fs_cloud).
struct SkyRadianceLanes { original: vec3<f32>, atmosphere: vec3<f32>, };
fn sky_radiance_lanes(dir: vec3<f32>, star_weight: f32) -> SkyRadianceLanes {
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

    let atmosphere=color;
    // Stars. Added with the night floor and BEFORE the cloud composite below, so a cloud deck
    // covers them exactly as it covers the sky behind it -- stars punching through overcast is the
    // giveaway that they were pasted on at the end. Being ahead of the composite is NECESSARY but
    // not SUFFICIENT: see star_weight on this function.
    if (star_weight > 0.001) {
        color = color + star_radiance(dir, sun) * star_weight;
    }

    // Night-sky floor: an authored deep-blue that fills in as the sun drops below the horizon
    // (blended by sun altitude), so twilight/night settle into a believable blue instead of the
    // physical model's near-black. See docs/procedural-sky-plan.md Stage 6.
    let night_blend = 1.0 - smoothstep(sky.night_params.y, sky.night_params.x, sun.y);
    if (night_blend > 0.0) {
        let night = mix(sky.night_horizon.rgb, sky.night_zenith.rgb, clamp(dir.y, 0.0, 1.0));
        color = color + night * sky.night_params.z * night_blend;
    }

    return SkyRadianceLanes(max(color,vec3<f32>(0.0)),atmosphere);
}
fn sky_radiance_weighted(dir: vec3<f32>, star_weight: f32) -> vec3<f32> {
    // Reflection/SH/cloud illumination retain their original sky radiance.
    return sky_radiance_lanes(dir,star_weight).original;
}

// The sky as the EYE sees it (stars at full weight).
fn sky_radiance(dir: vec3<f32>) -> vec3<f32> {
    return sky_radiance_weighted(dir, 1.0);
}

// The sky the CLOUD march is allowed to be lit BY. Identical to sky_radiance wherever there are no
// stars -- daytime is bit-for-bit unchanged, because star_radiance is already zero above the
// night threshold -- and star-free at night in proportion to night_sky.x.
fn sky_radiance_cloud_bg(dir: vec3<f32>) -> vec3<f32> {
    return sky_radiance_weighted(dir, clamp(1.0 - sky.night_sky.x, 0.0, 1.0));
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
// `override`, not `const`: these are the two knobs that decide what the cloud march costs,
// and it is the single most expensive thing in the frame (7.97 ms of a 34.17 ms GPU frame on
// perf_abel @1920x1080, plus 0.93 for the composite). Pipeline-overridable so the cost can be
// measured and traded without a shader edit; both are only ever passed as values, never used
// in a const-expression, so an override is legal here.
override CLOUD_STEPS: f32 = 128.0;
override CLOUD_LIGHT_STEPS: i32 = 4;
// REN-SKY-002: 1 = the light march samples cloud_density_light (two taps), 0 = the full density.
override CLOUD_LIGHT_COARSE: f32 = 1.0;
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
// camera-relative ray xz MINUS the CPU-wrapped wind WORLD offset (cloud1.xy, bounded so the scaled
// texture coord stays precise), with .y = altitude above the cloud base (bounded, unlike raw
// planet-scale p.y). Returns (worldX, altitude, worldZ) — the 3 noise axes.
// MINUS, not plus: the offset is the AIR's displacement (it grows along the wind), so the lookup
// coordinate must move against it for a fixed noise feature to translate WITH the wind. Sampling
// at +offset made every bank crawl upwind -- the field was steady and the sky moved backwards.
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
    let world_xz = sky.cam_pos.xz + p.xz - sky.cloud1.xy;
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

// REN-SKY-002: the density the LIGHT march samples. Same weather and shape taps as cloud_density,
// without the domain warp and the detail erosion: the sun march only needs the optical depth
// towards the sun, and the two high-frequency taps change that depth by a few percent while
// costing half the march (4 light samples per primary sample, 4 taps each). Two taps instead of
// four. The primary march keeps the full density, so silhouettes and edges are unchanged.
// CLOUD_LIGHT_COARSE=0 (WGR_CLOUD_LIGHT_COARSE=0) is the A/B back to the full density.
fn cloud_density_light(p: vec3<f32>) -> f32 {
    let planet_r = sky.params.z;
    let r_bot = planet_r + sky.cloud0.z;
    let thick = cloud_thickness();
    let half_vary = thick * CLOUD_BASE_VARY * 0.5;
    let alt = length(p) - r_bot;
    if (alt <= -half_vary || alt >= thick + half_vary) {
        return 0.0;
    }
    var w = cloud_world(p);
    let weather = cloud_weather(w.xz);
    let cov = weather.x;
    if (cov <= 0.001) {
        return 0.0;
    }
    let base_h = (weather.y - 0.5) * CLOUD_BASE_VARY * thick;
    let hl = (alt - base_h) / thick;
    if (hl <= 0.0 || hl >= 1.0) {
        return 0.0;
    }
    let grad = smoothstep(0.0, 0.12, hl) * (1.0 - smoothstep(0.55, 1.0, hl));
    if (grad <= 0.0) {
        return 0.0;
    }
    w.y -= base_h;
    var ws = w * sky.cloud1.z;
    ws.y += sky.cloud4.x * sky.cloud1.z + (w.x * 0.31830989 + w.z * 0.53190563) * sky.cloud1.z * 0.31;
    let shape = textureSampleLevel(cloud_noise, cloud_samp, ws, 0.0).r;
    let n = max(shape, smoothstep(0.55, 1.0, cov));
    let base = clamp((n - (1.0 - cov)) / max(cov, 1e-3), 0.0, 1.0);
    return base * grad;
}

// ================================================================================================
// NIGHT SKY: a bright-star CATALOGUE plus a Milky Way band.
//
// What this replaces: a hashed cube grid that dropped one random star per cell with a random
// magnitude and a hashed colour. Even density, no constellations, no recognisable relative
// brightness. It read as a dot screen because that is what it was.
//
// What it is now:
//   * Stars come from `star_list` -- real right ascension / declination / visual magnitude /
//     B-V colour index, converted on the CPU (see starcat.rs) into a unit vector in the
//     EQUATORIAL frame, a linear amplitude, a linear RGB colour and an angular radius.
//     Evaluated analytically per pixel, so they stay sharp at any resolution.
//   * `star_cells` is a cube-grid acceleration structure: one u32 per cell holding
//     (count << 24) | offset into star_list. A pixel reads ONE cell, so the inner loop sees a
//     couple of stars, not a couple of thousand. Stars whose disc spills over a cell boundary
//     were duplicated into the neighbouring cell at build time.
//   * The Milky Way is a separate diffuse term. Points cannot make that glow -- it is the one
//     part of a real night sky that is genuinely a continuous field. See `milky_way`.
//
// The rotation is the other half of "looks real". The old field wheeled the sky about the
// ZENITH, which spins the whole bowl flat like a plate and is the thing people notice is wrong
// even when they cannot name it. Stars turn about the CELESTIAL POLE, which at mid latitudes
// sits ~45 deg off the zenith, so they rise in the east, arc over, and set in the west. That
// tilt is one constant (STAR_LATITUDE) and it is most of the effect.
// ================================================================================================

const STAR_GRID_N: u32 = 64u;

// Observer latitude in radians -- the tilt of the celestial pole away from the zenith. NOT an
// ephemeris input: the owner's bar is "looks right and turns plausibly", and a fixed mid-northern
// latitude gives the familiar arc. Everon/Malden sit in a vaguely northern-European climate, so
// 45 deg N is the honest guess rather than a computed one.
const STAR_LATITUDE: f32 = 0.7854;

// Overall radiance scale for a magnitude-6.5 star (the faintest in the catalogue). Everything
// brighter scales up from here through the magnitude curve baked into `amp`. This is the knob to
// turn if the field is too dim or too shouty; the dev panel's "Star brightness" multiplies it.
const STAR_GAIN: f32 = 0.26;

// Peak surface radiance of the Milky Way band, in the same units. It is a DIFFUSE glow, so it has
// to sit well below a star's peak or it reads as fog.
const MILKY_WAY_GAIN: f32 = 0.075;
// The texture carries real relative radiance, so this is one exposure number rather than the
// stack of shape constants the procedural band needs. Set so the band reads at roughly the
// brightness the procedural one did at the same star gain, which is what the owner has already
// seen and accepted; it is a taste knob and WGR_MILKYWAY scales it.
const MILKY_WAY_TEX_GAIN: f32 = 0.85;

// North galactic pole and galactic centre as unit vectors in equatorial J2000 coordinates
// (RA 12h51.4m / Dec +27.13, and RA 17h45.6m / Dec -28.94). These place the band where it really
// is on the celestial sphere, so the procedural placeholder below and a real equirectangular
// Milky Way map land in the SAME place -- swapping one for the other does not move the band.
const NGP_EQ: vec3<f32> = vec3<f32>(-0.86777, -0.19754, 0.45598);
const GC_EQ: vec3<f32> = vec3<f32>(-0.05489, -0.87345, -0.48389);

struct StarGpu {
    dir: vec3<f32>,   // unit vector, equatorial frame (+z = north celestial pole, +x = RA 0h)
    amp: f32,         // linear brightness (magnitude curve already applied)
    color: vec3<f32>, // linear RGB, normalised to luminance 1
    radius: f32,      // angular radius of the disc, radians
};

@group(0) @binding(10) var<storage, read> star_cells: array<u32>;
@group(0) @binding(11) var<storage, read> star_list: array<StarGpu>;
// SKY-005: the Milky Way band. Equirectangular in the EQUATORIAL frame -- the same frame
// star_list uses -- so it needs no separate rotation: whatever to_celestial() does to the stars
// happens to the band too, and the two can never drift apart. Sampled with cloud_samp because
// that one REPEATS in u, and an equirectangular map wraps at RA 0h; a clamping sampler would
// leave a visible seam down the sky there.
@group(0) @binding(12) var milkyway_tex: texture_2d<f32>;

// Angular size of one pixel, radians. Written at the top of each fragment entry point -- i.e. in
// UNIFORM control flow, which is where a derivative is allowed. star_field is called from inside
// a branch, so it cannot take the derivative itself. Zero (the default, for any path that does
// not set it) simply means "no pixel floor", which is safe.
var<private> g_px_angle: f32 = 0.0;

// Integer bit-mixing hash on the noise lattice. NOT the fract(sin(dot(...))) idiom the old star
// field used: that one is not white noise, it is a smooth function sampled coarsely, and on a
// low-frequency lattice like the Milky Way's its correlations show up as hard-edged diamonds
// across the whole sky. Those diamonds were visible in the first capture of this feature and read
// convincingly as a cloud-composite artefact -- they were the hash. Same lesson as the cloud
// march's dithering note further up this file.
fn mw_hash(p: vec3<i32>) -> f32 {
    var n: u32 = bitcast<u32>(p.x) * 1597334677u;
    n = n ^ (bitcast<u32>(p.y) * 3812015801u);
    n = n ^ (bitcast<u32>(p.z) * 2654435761u);
    n = n ^ (n >> 15u);
    n = n * 2246822519u;
    n = n ^ (n >> 13u);
    n = n * 3266489917u;
    n = n ^ (n >> 16u);
    return f32(n) * (1.0 / 4294967296.0);
}

// Cube-face cell address for a unit direction. MUST match `cell_index` in starcat.rs exactly.
// A divergence does not error: stars simply stop appearing in bands, which reads as a shader bug
// and is not one.
fn star_cell_index(d: vec3<f32>) -> u32 {
    let a = abs(d);
    var face: u32 = 0u;
    var sc: f32 = 0.0;
    var tc: f32 = 0.0;
    var ma: f32 = 1.0;
    if (a.x >= a.y && a.x >= a.z) {
        ma = a.x;
        if (d.x > 0.0) {
            face = 0u;
            sc = -d.z;
            tc = -d.y;
        } else {
            face = 1u;
            sc = d.z;
            tc = -d.y;
        }
    } else if (a.y >= a.z) {
        ma = a.y;
        if (d.y > 0.0) {
            face = 2u;
            sc = d.x;
            tc = d.z;
        } else {
            face = 3u;
            sc = d.x;
            tc = -d.z;
        }
    } else {
        ma = a.z;
        if (d.z > 0.0) {
            face = 4u;
            sc = d.x;
            tc = -d.y;
        } else {
            face = 5u;
            sc = -d.x;
            tc = -d.y;
        }
    }
    let inv = 1.0 / max(ma, 1e-8);
    let n = f32(STAR_GRID_N);
    let uu = 0.5 * (sc * inv + 1.0);
    let vv = 0.5 * (tc * inv + 1.0);
    let i = u32(clamp(floor(uu * n), 0.0, n - 1.0));
    let j = u32(clamp(floor(vv * n), 0.0, n - 1.0));
    return face * STAR_GRID_N * STAR_GRID_N + j * STAR_GRID_N + i;
}

// World direction -> equatorial (catalogue) direction, given the hour angle the sky has turned
// through. The basis is built from the celestial pole, so the whole field pivots about that axis
// rather than about the zenith.
fn to_celestial(dir: vec3<f32>, hour_angle: f32) -> vec3<f32> {
    let clat = cos(STAR_LATITUDE);
    let slat = sin(STAR_LATITUDE);
    // World frame here is +Y up, +Z the horizontal bearing the pole leans toward (north),
    // +X the one 90 deg clockwise from it (east).
    let pole = vec3<f32>(0.0, slat, clat);
    let east = vec3<f32>(1.0, 0.0, 0.0);
    let nrth = cross(pole, east); // completes a right-handed basis with `pole` as +z
    let c = vec3<f32>(dot(dir, east), dot(dir, nrth), dot(dir, pole));
    let ca = cos(hour_angle);
    let sa = sin(hour_angle);
    return vec3<f32>(c.x * ca - c.y * sa, c.x * sa + c.y * ca, c.z);
}

// Value noise on the sphere, for the Milky Way's clumping and its dust lanes. Cheap and only ever
// evaluated once per pixel.
fn mw_noise(p: vec3<f32>) -> f32 {
    let i = floor(p);
    let ii = vec3<i32>(i);
    let f = fract(p);
    let w = f * f * (3.0 - 2.0 * f);
    let c000 = mw_hash(ii + vec3<i32>(0, 0, 0));
    let c100 = mw_hash(ii + vec3<i32>(1, 0, 0));
    let c010 = mw_hash(ii + vec3<i32>(0, 1, 0));
    let c110 = mw_hash(ii + vec3<i32>(1, 1, 0));
    let c001 = mw_hash(ii + vec3<i32>(0, 0, 1));
    let c101 = mw_hash(ii + vec3<i32>(1, 0, 1));
    let c011 = mw_hash(ii + vec3<i32>(0, 1, 1));
    let c111 = mw_hash(ii + vec3<i32>(1, 1, 1));
    let x00 = mix(c000, c100, w.x);
    let x10 = mix(c010, c110, w.x);
    let x01 = mix(c001, c101, w.x);
    let x11 = mix(c011, c111, w.x);
    return mix(mix(x00, x10, w.y), mix(x01, x11, w.y), w.z);
}

// The Milky Way, as a diffuse band on the galactic great circle.
//
// PLACEHOLDER. The real article is NASA SVS "Deep Star Maps 2020" (public domain, derived from
// Gaia DR2), which is an equirectangular image and would be sampled here in galactic coordinates.
// Everything around that sample is already correct: the galactic pole and centre above are the
// real ones, so the band sits where it belongs and a texture drops straight in. What is fake is
// only the SHAPE of the glow -- a Gaussian across galactic latitude, brightened toward the centre,
// broken up with noise, and cut by a dark rift along the mid-plane where the dust really is.
//
// `dir_eq` is already in the equatorial frame, i.e. it has been through to_celestial.
// SKY-005. The band is a PHOTOGRAPH now (NASA SVS Deep Star Maps 2020, Gaia DR2) rather than
// the procedural arc below, which stays as the fallback for a build whose texture failed to
// load -- the loader reports which one you have.
//
// Its point sources were stripped offline (tools/sky/make_milkyway.py): our bright stars come
// from the catalogue as real discs, and keeping the map's would draw every one of them twice,
// once sharp and once as a blurry blob underneath in slightly the wrong place. What is left is
// the diffuse glow, which is the one thing a point catalogue cannot produce.
fn milky_way(dir_eq: vec3<f32>) -> vec3<f32> {
    let gain = max(sky.cirrus.w, 0.0);
    if (gain <= 0.0) {
        return vec3<f32>(0.0);
    }
    let dims = textureDimensions(milkyway_tex);
    if (dims.x > 4u) {
        // Equatorial spherical coordinates in the frame StarGpu documents: +z is the north
        // celestial pole, +x is RA 0h. Row 0 of the map is declination +90, matching the
        // converter's header.
        let ra = atan2(dir_eq.y, dir_eq.x);
        let dec = asin(clamp(dir_eq.z, -1.0, 1.0));
        let uv = vec2<f32>(fract(ra / (2.0 * PI)), 0.5 - dec / PI);
        // Level 0 explicitly: this is called from inside a branch, where a derivative-taking
        // sample is undefined, and there is only one level anyway.
        let band = textureSampleLevel(milkyway_tex, cloud_samp, uv, 0.0).rgb;
        return band * (MILKY_WAY_TEX_GAIN * gain);
    }
    // Galactic latitude (0 = the plane) and longitude (0 = the centre).
    let sb = clamp(dot(dir_eq, NGP_EQ), -1.0, 1.0);
    let b = asin(sb);
    let gy = cross(NGP_EQ, GC_EQ);
    let l = atan2(dot(dir_eq, gy), dot(dir_eq, GC_EQ));

    // The band. Sigma ~7.5 deg with a 16 deg skirt so it does not end abruptly.
    //
    // These were three times wider on the first attempt (10 deg core, 26 deg skirt) and the result
    // was not a band at all: the skirt lifted the WHOLE sky by a couple of percent, which reads as
    // overcast haze and hid the stars. The measurement that caught it was an ablation --
    // WGR_MILKYWAY=0 turned the "cloud" off. If this ever looks like weather again, ablate before
    // theorising: a diffuse term that covers the sky is indistinguishable from one that is too wide.
    let core_band = exp(-(b * b) / (2.0 * 0.13 * 0.13));
    let skirt = 0.14 * exp(-(b * b) / (2.0 * 0.24 * 0.24));
    // The bulge toward Sagittarius: the brightest part of the sky by a wide margin.
    let bulge = 1.0 + 1.9 * exp(-(l * l) / (2.0 * 0.55 * 0.55));

    // Clumping: two octaves, so it is not a smooth airbrushed arc.
    let n1 = mw_noise(dir_eq * 7.0);
    let n2 = mw_noise(dir_eq * 19.0);
    let clump = 0.55 + 0.75 * n1 + 0.30 * n2;

    // The Great Rift: a dust lane straight down the middle, darker than the band around it.
    // Without it the band reads as an airbrush stroke rather than as a galaxy seen edge-on.
    let rift_wobble = 0.045 * (mw_noise(dir_eq * 4.0) - 0.5);
    let bd = b - rift_wobble;
    let rift = 1.0 - 0.62 * exp(-(bd * bd) / (2.0 * 0.045 * 0.045)) * (0.5 + 0.9 * mw_noise(dir_eq * 11.0));

    let amount = max((core_band + skirt) * bulge * clump * rift, 0.0);
    // Slightly warm in the bulge, slightly cool off-plane -- integrated starlight, not a nebula.
    let tint = mix(vec3<f32>(0.80, 0.85, 1.00), vec3<f32>(1.00, 0.93, 0.80),
                   clamp(bulge - 1.0, 0.0, 1.0));
    return tint * (amount * MILKY_WAY_GAIN * gain);
}

fn star_field(dir_in: vec3<f32>, sun: vec3<f32>) -> vec3<f32> {
    // Wheel the field with the SUN'S AZIMUTH rather than a clock. This UBO carries no time value,
    // and the sun's bearing is the celestial clock anyway -- it advances at the same 15 deg an
    // hour the sky really turns at, so the stars drift through the night for free and stay
    // consistent with wherever the sun comes up.
    let ang = atan2(sun.z, sun.x);
    let dir = to_celestial(dir_in, ang);

    // Below the horizon there is no sky to put stars in, and near it atmospheric extinction takes
    // them out well before the geometric horizon -- so fade rather than cut. Measured on the WORLD
    // up direction, not the celestial one: the horizon does not tilt with the pole.
    let horizon = smoothstep(-0.02, 0.16, dir_in.y);
    if (horizon <= 0.0) {
        return vec3<f32>(0.0);
    }

    var col = milky_way(dir);

    // One cell lookup. The build side duplicated any star whose disc crosses a cell boundary into
    // the neighbouring cell, so a single cell is enough -- no 3x3 neighbourhood, and no special
    // case at a cube-face seam (which is exactly where an arithmetic neighbourhood would be wrong).
    let packed = star_cells[star_cell_index(dir)];
    let count = packed >> 24u;
    let offset = packed & 0x00FFFFFFu;
    // Floor the disc at roughly a pixel. A star smaller than a pixel does not get dimmer as you
    // raise the resolution, it FLICKERS -- it drops in and out as the sample point moves across
    // it. The area term below takes the brightness back out, so widening the disc conserves the
    // star's total light instead of making the sky brighter at low resolution.
    let px = g_px_angle * 0.75;
    for (var k: u32 = 0u; k < count; k = k + 1u) {
        let s = star_list[offset + k];
        let cd = clamp(dot(dir, s.dir), -1.0, 1.0);
        // Chord length; for the sub-degree angles involved it equals the arc to well inside a
        // part in 10^6, and it costs one sqrt instead of an acos.
        let d = sqrt(max(2.0 - 2.0 * cd, 0.0));
        let r = max(s.radius, px);
        if (d < r) {
            let t = 1.0 - d / r;
            let area = (s.radius * s.radius) / (r * r);
            col = col + s.color * (s.amp * t * t * area);
        }
    }

    // No twinkle. Scintillation needs a per-frame clock and this UBO has none; faking it from the
    // sun's bearing would change on the scale of hours, which is not twinkling. Left out rather
    // than approximated into something that only looks like a bug.
    return col * (STAR_GAIN * horizon);
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
// Transmittance at which the deck is called opaque: the march stops, and the accumulated
// transmittance is rescaled so this value maps to exactly 0 (see the end of march_clouds).
// Both halves must use the same number or the rescale would clip a range the march still visits.
const CLOUD_TRANS_CUT: f32 = 0.02;

// ---- SKY-004: the lunar light source for the cloud decks ----------------------
//
// Before this, `sun_col = sample_transmittance(pos, sun) * radiance` was the ONLY light the cloud
// march had. At night that samples the transmittance LUT toward a sun below the horizon, which is
// zero, so a night deck's entire output was its ambient term -- and once SKY-003 took the stars out
// of that ambient, an overcast night was genuinely black. It should not be: a full moon over an
// overcast is a soft grey dome you can walk by, and a broken deck under one has lit tops and dark
// bases exactly as it does by day.
//
// PHYSICALLY this is not a new light model, it is the SAME light model with the other body in it.
// Everything needed was already in the UBO because the moon DISC needs it: a real lunar ephemeris
// (position accurate to ~0.015 deg), the illuminated fraction, and `moon_params.z` -- the disc's
// radiance scale, which C++ builds as
//
//     sunIntensity * exposure * moonIntensity * (moonBrightness / max(illum, 0.02)) * scale
//
// i.e. the sun's own irradiance units, times the moon-to-sun ratio, times the empirical lunar phase
// function, divided by the lit fraction to turn total brightness into per-area radiance. Multiply
// that back by the illuminated fraction and you have the lunar IRRADIANCE in exactly the units
// `radiance` is in, phase already folded in. So the phase here is not faked and not a cosine: it is
// whatever the ephemeris says the moon is doing tonight, and a quarter moon lights the deck about
// an order of magnitude less than a full one, which is the real behaviour.
//
// The moon-to-sun ratio itself is `moonIntensity`, default 0.06 -- NOT the true 1e-6. That is a
// deliberate pre-existing choice of this renderer (see the Moon panel's tooltip): the frame is not
// tone-mapped through a scotopic response, so the honest ratio renders as black. Riding the same
// number means the deck is lit at precisely the fraction the GROUND is lit at, so cloud and terrain
// agree about how bright the night is instead of drifting apart under one slider.
//
// COLOUR is the sun's, near enough (moonlight IS sunlight, off a slightly reddish regolith), which
// is why the tint below is the moon disc's own 1.0/0.97/0.92 rather than anything blue. The blue
// moonlight of film is scotopic vision, not photometry. `night_sky.z` will lerp toward a cold
// blue-grey for anyone who wants the cinematic look, and it is 0 by default so nobody gets it by
// accident.
struct LunarLight {
    dir: vec3<f32>,  // unit direction TO the moon
    col: vec3<f32>,  // irradiance at the deck: atmosphere-attenuated, tinted, gated, gained
    on: f32,         // 0 = no lunar term at all -- skip the light march entirely
};

fn lunar_cloud_light(pos: vec3<f32>, sun: vec3<f32>) -> LunarLight {
    var l: LunarLight;
    l.dir = vec3<f32>(0.0, 1.0, 0.0);
    l.col = vec3<f32>(0.0);
    l.on = 0.0;
    let gain = sky.night_sky.y;
    if (gain <= 0.0) {
        return l; // WGR_MOON_CLOUDS=0 -- the pre-SKY-004 behaviour, bit for bit
    }
    // Total lunar irradiance: per-area disc radiance scale x the lit fraction. Zero whenever the
    // Moon panel has turned the body off (moonIntensity or moonBrightnessScale at 0 drive
    // moon_params.z to 0), so one knob still kills the moon everywhere it appears.
    let irr = max(sky.moon_params.z, 0.0) * clamp(sky.moon_params.y, 0.0, 1.0);
    if (irr <= 0.0) {
        return l;
    }
    let moon = normalize(sky.moon_dir.xyz);
    // TWO GATES, both smooth, both one-sided.
    //
    // 1. The moon must be UP. A moonless night must stay black -- that is the whole point of a
    //    moonless night, and a deck glowing with a moon below the horizon would be worse than the
    //    black it replaced. Faded over ~3 deg so moonrise is a gradual filling-in rather than a
    //    frame where the sky changes.
    // 2. The SUN must be down. Not because the moon stops existing by day (it does not, and the
    //    disc is still drawn), but because daylight is a control that must not move: below -2 deg
    //    of solar elevation this is 1 and above the horizon it is exactly 0, so every daytime
    //    frame takes the early-out above and is bit-identical to the pre-SKY-004 build. -2 deg is
    //    the same threshold the scene's own moonlight hand-off already uses, so the deck and the
    //    ground start being moonlit at the same moment.
    let up = smoothstep(-0.05, 0.02, moon.y);
    let night = 1.0 - smoothstep(-0.035, 0.0, sun.y);
    let k = up * night * gain;
    if (k <= 0.0) {
        return l;
    }
    // Reddened toward the horizon by the same transmittance LUT that reddens the sun, so a low moon
    // lights the deck with the low moon's own colour instead of a constant grey.
    let blue = clamp(sky.night_sky.z, 0.0, 1.0);
    let tint = mix(vec3<f32>(1.0, 0.97, 0.92), vec3<f32>(0.60, 0.76, 1.05), blue);
    l.dir = moon;
    l.col = sample_transmittance(pos, moon) * (irr * k) * tint;
    l.on = 1.0;
    return l;
}

// How much of the lunar irradiance comes back out of a THICK deck as diffuse glow, per unit of
// isotropic radiance. This is the term that makes a moonlit overcast a grey dome rather than a
// black one, and it is the lunar stand-in for something the solar path gets for free: by day the
// deck's fill light is `amb_sky`, the atmosphere's own radiance at that pixel, and the atmosphere
// march has no lunar source in it, so at night `amb_sky` collapses to the authored night floor.
//
// It matters most exactly where the DIRECT term is useless. Under 0.95 coverage the optical depth
// from the deck's base to the moon is large, so `exp(-od)` is ~0 for every sample the eye can see,
// and a direct-only lunar term would have changed nothing about the reported symptom. Real
// overcast is dominated by multiple scattering; this is the same cheap stand-in the phase function
// already makes for it (mixing toward 1/4pi with coverage), applied to magnitude instead of shape.
//
// Weighted by coverage so it does NOT paint a broken deck flat: at low coverage the look should be
// the direct term's lit tops and shadowed bases, which is what a few moonlit cumulus actually look
// like, and the fill only takes over as the deck closes.
//
// THE VALUE IS CALIBRATED AGAINST THE DAYLIGHT CONTROL, not chosen by eye. The physical statement
// is that an overcast dome's brightness is linear in the irradiance falling on it, whatever body
// supplies it -- so the lunar fill should be the same fraction of the LUNAR irradiance that the
// solar fill is of the SOLAR irradiance. The solar fill is `amb_sky`, i.e. the sky's own radiance
// `bg`, and the day control measures that ratio directly: an 0.95-coverage overcast at 64 deg solar
// elevation renders at 0.196 display-linear, which through `ambient * ao_floor` with
// ao_floor = 0.69 and cloud2.z = 1 puts amb_sky at ~0.284, against an effective solar irradiance of
// sunIntensity*exposure*T = 0.564*0.9 = 0.51. That ratio is 0.56, so 0.5 here.
//
// It is deliberately the LOW end of what that measurement supports: the 0.196 is read after the
// tonemapper, which compresses, so the true scene-space ratio is higher and 0.5 errs toward a
// night that is too dark rather than one that glows.
//
// MEASURED on the shipped build, mean sky luma over the top 55% of frame. The two rows differ
// only in where the moon was put, which is the point -- the term follows the ephemeris rather
// than a constant, so the number is supposed to move with elevation and phase:
//   full moon forced to 45 deg (WGR_MOON_AZEL=180,45, WGR_MOON_PHASE=1)   31.36
//   near-full moon at 52 deg from the mission date                        24.15  (predicted 24.37)
//   same frame, WGR_MOON_CLOUDS=0                                          0.22
//   moon below the horizon                                                 0.29
//   clear night, same moon                                                 9.00
// The 24.15 landing within 1% of the 24.37 this calibration predicts is the check that the
// linear-in-irradiance argument above is doing real work and is not a fitted constant.
// Daylight control at this value: overcast day 126.6965 with the term on against 126.6666 with
// it off, max per-pixel 6/255 -- the same spread two identical-path runs show.
const MOON_CLOUD_MS: f32 = 0.50;

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
    // Is the solar light march worth running at all? Below the horizon the transmittance LUT
    // returns ~0 toward the sun, so `direct` is a light march whose result is multiplied by zero --
    // paid on every cloudy sample of every night frame. Skipping it is exactly neutral (the term
    // IS zero, not merely small) and it is what pays for the lunar march below: at night the deck
    // now does one light march toward the moon instead of one toward the sun, so the night cost of
    // this feature is about nothing rather than double.
    let sun_lit = dot(sun_col, vec3<f32>(0.2126, 0.7152, 0.0722)) > 1.0e-6;
    // SKY-004: the moon as a second light source. `on` is 0 in daylight (and with no moon up, and
    // with the feature off), and every lunar term below is gated on it, so a daytime frame runs
    // precisely the code it ran before.
    let ml = lunar_cloud_light(pos, sun);
    let cos_m = dot(dir, ml.dir);
    // Same phase construction as the sun's -- a cloud does not know which body lit it, and the
    // forward silver lining round a moon behind a thin deck is a real and rather good-looking
    // thing. Built unconditionally (it is arithmetic, no taps) and multiplied by `on`.
    let phase_m_hg = mix(hg(cos_m, sky.cloud2.x), hg(cos_m, -0.15), 0.5);
    let phase_m = mix(phase_m_hg, 1.0 / (4.0 * PI), cov * 0.7);
    // The multiple-scattering fill. Height-graded with the ambient below rather than added flat, so
    // even a fully overcast moonlit deck keeps a top-to-bottom gradient instead of reading as a
    // uniform grey card.
    let amb_moon = ml.col * (MOON_CLOUD_MS * mix(0.25, 1.0, cov) * ml.on);

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
        if (trans < CLOUD_TRANS_CUT) {
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
            // Beer-Powder dark-edge term (the silver-lining / fluffy look).
            let powder = 1.0 - exp(-2.0 * sigma * dt);
            let beer = mix(1.0, powder, sky.cloud2.y);
            // Light march toward the sun with exponentially growing steps -> optical depth.
            var direct = vec3<f32>(0.0);
            if (sun_lit) {
                var od = 0.0;
                var lt = 0.0;
                var ls = ls_base * 0.3;
                for (var j = 0; j < light_steps; j = j + 1) {
                    lt += ls;
                    let lp = p + sun * lt;
                    let ld = select(cloud_density(lp), cloud_density_light(lp), CLOUD_LIGHT_COARSE > 0.5);
                    od += ld * ext * ls;
                    ls *= 1.6;
                }
                direct = sun_col * exp(-od) * phase * beer;
            }
            // SKY-004: the identical march toward the MOON. Same steps, same growth, same
            // Beer-Powder -- it is the same physics with a different body, and self-shadowing is
            // what gives a moonlit broken deck its lit tops and dark bases. This is the loop the
            // solar skip above pays for.
            if (ml.on > 0.5) {
                var od_m = 0.0;
                var lt_m = 0.0;
                var ls_m = ls_base * 0.3;
                for (var j = 0; j < light_steps; j = j + 1) {
                    lt_m += ls_m;
                    let lp = p + ml.dir * lt_m;
                    let ld = select(cloud_density(lp), cloud_density_light(lp), CLOUD_LIGHT_COARSE > 0.5);
                    od_m += ld * ext * ls_m;
                    ls_m *= 1.6;
                }
                direct = direct + ml.col * exp(-od_m) * phase_m * beer;
            }
            let hfp = clamp((length(p) - (sky.params.z + sky.cloud0.z)) / thick, 0.0, 1.0);
            // darker undersides (cheap AO). The lunar multiple-scattering fill rides the SAME
            // height gradient as the sky ambient, so it cannot flatten the deck.
            let ambient = (amb_sky + amb_moon) * mix(ao_floor, 1.0, hfp);
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
    // The early-out above LEFT transmittance at the cut instead of driving it to zero, so a deck
    // that is opaque to the eye still passed ~1% of whatever was behind it. That is harmless for a
    // background of ordinary radiance and catastrophic for the sun disc, whose radiance is the solar
    // irradiance divided by the solar solid angle — about 1.5e4x the surrounding sky. One per cent
    // of that is still ~150x the sky, i.e. a crisp white disc through solid overcast, which is
    // exactly what the owner reported. Rescale so the cut IS zero: continuous, monotonic, and
    // unchanged anywhere near trans = 1.
    res.trans = max((trans - CLOUD_TRANS_CUT) / (1.0 - CLOUD_TRANS_CUT), 0.0);
    return res;
}

// ---- Second cloud layer: high cirrus -----------------------------------------
// Cloud at a completely different altitude, so the sky has depth instead of one populated slab.
// Two modes, selected by sky.output.w (0 = off, 1 = flat sheet, 2 = thin-shell march):
//
//   FLAT       one ray/sphere intersection against a sheet at CIRRUS_ALT, one field evaluation.
//   VOLUMETRIC an adaptive number of samples (CIRRUS_STEPS_MIN..CIRRUS_STEPS_MAX, derived from
//              the ray's path) through the shell (500-1400 m, thickening with puffiness).
//              Cirrus is optically thin, so a handful of steps is enough to give it real vertical
//              extent: the field walks the noise volume's third axis with height, so top and
//              bottom of the shell are different cloud, and a grazing ray travels kilometres
//              THROUGH it instead of piercing a plane.
//
// Either way this is sky pixels only, occluded by terrain via `max_dist`.
const CIRRUS_ALT: f32 = 7000.0;          // m ASL — shell CENTRE; well clear of the cumulus deck
// MAX march samples through the shell. This is a CAP, not the count: the count is derived from the
// ray's own path length (see cirrus_layer), exactly as the deck's march does it.
//
// It was a fixed 6, and that is what produced the reported PARALLEL STRIPS. With a fixed count, dt
// scales with the angle-dependent path length — a grazing ray travels thickness/0.15 through the
// shell, i.e. 6.7x the vertical crossing, so its steps were 6.7x coarser. The step PHASE then
// varied smoothly across the screen, so the aliasing organised itself into coherent bands running
// parallel to the horizon rather than into noise. It is the same defect the deck's march already
// documents and already fixed; the cirrus shell simply never got the fix, because a 900 m veil has
// little vertical structure to alias and it only became visible once MATCH could deepen the shell.
const CIRRUS_STEPS_MAX: i32 = 24;
// Target distance between samples, as a fraction of the shell's own thickness. 1/10 of a 900 m
// shell is a 90 m step; the field walks CIRRUS_SLICE_H of the noise volume over the full
// thickness, so ~10 samples resolve its vertical structure. Grazing rays get proportionally MORE
// samples because their path is longer, which is the entire point.
const CIRRUS_STEP_FRAC: f32 = 0.10;
// Floor on the derived count. Straight up through a thin shell the path is short and the formula
// would ask for 2-3 samples; the vertical profile alone needs more than that to integrate smoothly.
const CIRRUS_STEPS_MIN: f32 = 8.0;
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
// ---- AMOUNT ------------------------------------------------------------------
// sky.cirrus.x, one 0..1 slider mapped through the shipped value at its midpoint (the same
// three-knot arrangement PUFFINESS uses below, for the same reason: 0.5 must be bit-for-bit the
// look the layer shipped with, and the two halves need different reach).
//
// It is a QUANTITY control, and quantity aloft is mostly COVERAGE — what fraction of the sheet
// holds cloud at all — so that is what moves furthest. 0.18 is a handful of separated wisps in an
// otherwise empty upper sky; 0.90 is a continuous cirrostratus veil with only thin breaks in it.
//
// Optical depth follows, but only slightly (0.80 .. 1.00 .. 1.25). That coupling is deliberate and
// is the opposite of PUFFINESS's, which cancels its own density change to stay a pure shape
// control: a sky filling with cirrus really does thicken as it fills — that is a warm front — so
// leaving density flat would make the top of the slider read as "the same thin veil, everywhere",
// which is not what "more of it" looks like. It stays small so the slider is not a brightness
// control wearing a coverage hat.
const CIRRUS_COVER_SPARSE: f32 = 0.18;
const CIRRUS_COVER_FULL: f32 = 0.90;
const CIRRUS_AMOUNT_DENSITY_SPARSE: f32 = 0.80;
const CIRRUS_AMOUNT_DENSITY_FULL: f32 = 1.25;

// ---- MATCH (make the second layer more like the first) -----------------------
// sky.cirrus.y. 0 = high ice cloud, which is what this layer has always been. 1 = as close to the
// CUMULUS DECK below as a six-to-fourteen-step shell march can get without becoming a second copy
// of the deck's own renderer.
//
// Six things move together, all of them read from the deck's own live parameters rather than from
// constants, so "similar" tracks whatever the deck is actually doing this frame:
//   ALTITUDE  7 km -> just above the deck's top (cloud0.w). Clamped so the layer can never sink
//             INTO or below the deck: it stays a second layer, which is the whole point of it.
//   SCALE     ~7 km features -> the deck's own shape scale (cloud1.z), so the two layers agree on
//             how big a cloud is. Clamped to a sane band because cloud1.z is authored.
//   ASPECT    the 4:1 shear stretch relaxes to 1:1. Cumulus is not sheared, so it is not striped.
//   ERODE     fibre striation goes away for the same reason.
//   DEPTH     the shell thickens toward a fraction of the deck's own bottom-to-top span.
//   OPACITY   optical depth rises toward CIRRUS_DENSITY_MATCHED — enough to read as solid-ish
//             cloud rather than a veil, and capped there rather than tracking the deck's own
//             value, which is tens of optical depths and would just be a white ceiling.
// The phase function and a cheap vertical self-shading term move with it too, in cirrus_layer.
const CIRRUS_ALT_MATCH_GAP: f32 = 1200.0;   // m above the deck's top at full match
const CIRRUS_ALT_MATCH_MIN: f32 = 2500.0;   // m ASL floor, whatever the deck is doing
const CIRRUS_SCALE_MATCH_MIN: f32 = 1.0 / 20000.0;
const CIRRUS_SCALE_MATCH_MAX: f32 = 1.0 / 1500.0;
const CIRRUS_ASPECT_MATCHED: f32 = 1.0;     // isotropic — cells, not streaks
const CIRRUS_ERODE_MATCHED: f32 = 0.05;
const CIRRUS_DENSITY_MATCHED: f32 = 8.0;    // optical depth straight up through full density
const CIRRUS_THICK_MATCH_FRAC: f32 = 0.45;  // of the deck's bottom-to-top span
const CIRRUS_THICK_MATCH_MIN: f32 = 600.0;
const CIRRUS_THICK_MATCH_MAX: f32 = 2200.0;
// Extra sampling density at full match, as a multiplier on the derived count. A matched shell is
// deeper AND carries far more optical depth, so each step covers more extinction and quantises the
// transmittance curve more coarsely; the path-derived count already grows with the depth, this
// tightens the target step on top of it. 1.0 at the default, so the default pays nothing.
const CIRRUS_STEPS_MATCH_GAIN: f32 = 1.6;
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
// shell also lengthens the worst-case march. That used to cap the thickness, because the step
// count was fixed; the count is now derived from the path, so a longer march buys more samples
// rather than coarser ones.
const CIRRUS_THICK_PUFF: f32 = 1400.0;

// ---- EDGE SOFTNESS ------------------------------------------------------------
// sky.cirrus.z. What it changes is the WIDTH of the band over which a cloud in this layer goes
// from nothing to something.
//
// `d0` in cirrus_field is a LINEAR remap of the sheet noise about the coverage threshold, hard-
// clamped at 0. A linear ramp into a hard clamp is a fairly definite boundary: density reaches
// useful values within a short distance of the threshold, the march builds opacity quickly, and
// the cloud gets a crisp silhouette. High cloud does not look like that. Cirrus is ice, its edges
// sublimate rather than condense, and the bigger the mass the deeper the fringe of thinning
// crystals around it — which is exactly the "bigger ones should be fuzzier" note.
//
// pow(d0, k>1) holds the density down through a much wider band either side of the threshold, so
// the transmittance falls off over a longer path and the silhouette gains a soft fringe instead of
// an edge. Applied BEFORE the fibre erosion, so erosion carves the fringe softening just created —
// which is what keeps "soft" from becoming "blurry".
//
// 2.6 rather than more: past roughly 3 the fringe stops reading as cloud and starts reading as
// haze, and the mean-restoring gain has to work hard enough that the core brightens visibly.
const CIRRUS_SOFT_POW: f32 = 2.6;
// The coverage band the effect fades in over. This is what makes it reach the BIG masses and not
// the isolated wisps: a large cloud in this layer is a place where the coverage field is high,
// because that is what merges separate streaks into a bank.
const CIRRUS_SOFT_COV_LO: f32 = 0.20;
const CIRRUS_SOFT_COV_HI: f32 = 0.65;

// Map the 0..1 slider through the shipped value at its midpoint.
fn cirrus_mix3(fibre: f32, shipped: f32, puffy: f32, puff: f32) -> f32 {
    let t = puff * 2.0;
    return select(mix(fibre, shipped, t), mix(shipped, puffy, t - 1.0), puff > 0.5);
}

// The puffiness-derived shape of the layer. Built ONCE per pixel in cirrus_layer and handed to
// every field evaluation in the march, so the mixes are not repeated once per march step.
struct CirrusShape {
    aspect: f32,
    fine: f32,
    erode: f32,
    density: f32,
    thick: f32,
    // AMOUNT/MATCH-derived, and therefore no longer constants: the coverage threshold the field
    // remaps against, the horizontal noise frequency, the shell's centre altitude, how many
    // samples the march takes, and the raw match value the phase/shading terms still need.
    cover: f32,
    scale: f32,
    alt: f32,
    // Sampling-density multiplier from MATCH; the COUNT itself is derived per ray in cirrus_layer
    // from the path length, so it cannot live here (t0/t1 are not known yet).
    step_gain: f32,
    match_deck: f32,
    // EDGE SOFTNESS, already clamped. The coverage weighting is per-sample, so it stays in
    // cirrus_field rather than being folded in here.
    soft: f32,
}

// The deck's bottom-to-top span, guarded: cloud0.z/.w are authored and an inverted or degenerate
// pair would otherwise propagate a negative thickness into the march bounds.
fn cirrus_deck_span() -> f32 {
    return max(sky.cloud0.w - sky.cloud0.z, 0.0);
}

fn cirrus_shape() -> CirrusShape {
    // cloud4.w is the evolution vec4's pad lane, written by the RENDERER (the C++ side sends 0
    // there) — the same "ride a spare lane" trick the cirrus mode uses for output.w, and for the
    // same reason: WgrSkyLook's size is part of the ABI handshake and must not grow.
    let puff = clamp(sky.cloud4.w, 0.0, 1.0);
    let amount = clamp(sky.cirrus.x, 0.0, 1.0);
    let m = clamp(sky.cirrus.y, 0.0, 1.0);
    var sh: CirrusShape;
    sh.aspect = cirrus_mix3(CIRRUS_ASPECT_FIBRE, CIRRUS_ASPECT_SHIPPED, CIRRUS_ASPECT_PUFF, puff);
    sh.fine = cirrus_mix3(CIRRUS_FINE_FIBRE, CIRRUS_FINE_SHIPPED, CIRRUS_FINE_PUFF, puff);
    sh.erode = cirrus_mix3(CIRRUS_ERODE_FIBRE, CIRRUS_ERODE_SHIPPED, CIRRUS_ERODE_PUFF, puff);
    sh.density = cirrus_mix3(CIRRUS_DENSITY_FIBRE, CIRRUS_DENSITY_SHIPPED,
                             CIRRUS_DENSITY_PUFF, puff);
    sh.thick = cirrus_mix3(CIRRUS_THICK_FIBRE, CIRRUS_THICK_SHIPPED, CIRRUS_THICK_PUFF, puff);

    // AMOUNT. Coverage is the primary axis; density follows a little (see the AMOUNT block).
    sh.cover = cirrus_mix3(CIRRUS_COVER_SPARSE, CIRRUS_COVER, CIRRUS_COVER_FULL, amount);
    sh.density = sh.density * cirrus_mix3(CIRRUS_AMOUNT_DENSITY_SPARSE, 1.0,
                                          CIRRUS_AMOUNT_DENSITY_FULL, amount);

    // MATCH. Every target below is derived from the DECK's live parameters where the deck has an
    // opinion, so the layer follows the weather rather than a second set of constants.
    sh.match_deck = m;
    sh.scale = mix(CIRRUS_SCALE,
                   clamp(sky.cloud1.z, CIRRUS_SCALE_MATCH_MIN, CIRRUS_SCALE_MATCH_MAX), m);
    // Never below the deck's top + a gap: a "second layer" that sinks into the first is just a
    // denser first layer, and the march bounds below assume the shell is clear of the deck.
    let matched_alt = clamp(sky.cloud0.w + CIRRUS_ALT_MATCH_GAP, CIRRUS_ALT_MATCH_MIN, CIRRUS_ALT);
    sh.alt = mix(CIRRUS_ALT, matched_alt, m);
    sh.aspect = mix(sh.aspect, CIRRUS_ASPECT_MATCHED, m);
    sh.erode = mix(sh.erode, CIRRUS_ERODE_MATCHED, m);
    sh.density = mix(sh.density, CIRRUS_DENSITY_MATCHED, m);
    let matched_thick = clamp(cirrus_deck_span() * CIRRUS_THICK_MATCH_FRAC,
                              CIRRUS_THICK_MATCH_MIN, CIRRUS_THICK_MATCH_MAX);
    sh.thick = mix(sh.thick, matched_thick, m);
    sh.soft = clamp(sky.cirrus.z, 0.0, 1.0);
    // Deeper shell + far more optical depth needs a tighter step; the path-derived count in
    // cirrus_layer scales by this.
    sh.step_gain = mix(1.0, CIRRUS_STEPS_MATCH_GAIN, m);
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
fn cirrus_warp(world_xz: vec2<f32>, scale: f32) -> vec2<f32> {
    let q = cirrus_rot(world_xz * scale * 0.11, 0.87630, 0.48178);
    let n = textureSampleLevel(cloud_noise, cloud_samp,
        vec3<f32>(q.x, 0.17 + sky.cloud4.z * scale * 0.5, q.y), 0.0).rg;
    // Warp displacement scales with the feature size, so MATCH (which shrinks features toward the
    // deck's) does not leave a 6 km warp bending a 2 km cloud into a smear.
    return (n - vec2<f32>(0.5, 0.5)) * CIRRUS_WARP * (CIRRUS_SCALE / scale);
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
    let s = sh.scale;
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
    var d0 = clamp((sheet - (1.0 - cover)) / max(cover, 1e-3), 0.0, 1.0);
    if (d0 <= 0.0) {
        return 0.0; // in a gap -> skip the fibre tap (the empty-space fast path, as the deck does)
    }
    // EDGE SOFTNESS (see the block above the constants). Weighted by the LOCAL coverage this
    // sample was remapped against, so the big merged banks soften and isolated wisps stay defined.
    //
    // The gain restores the mean. pow() on a roughly uniform [0,1] takes the mean from 1/2 to
    // 1/(k+1), so without (k+1)/2 back the slider would visibly thin the layer and read as a
    // brightness control with a shape side-effect — the same trap PUFFINESS documents avoiding.
    // It is an approximation (the true distribution is not uniform) but it is the right order, and
    // it keeps the layer's overall opacity roughly still across the range.
    let soft = sh.soft * smoothstep(CIRRUS_SOFT_COV_LO, CIRRUS_SOFT_COV_HI, cover);
    if (soft > 0.001) {
        let k = mix(1.0, CIRRUS_SOFT_POW, soft);
        d0 = clamp(pow(d0, k) * ((k + 1.0) * 0.5), 0.0, 1.0);
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

// `jitter` is a per-PIXEL start offset in [0,1), same contract as march_clouds: it breaks the
// step phase between neighbouring pixels so whatever undersampling survives reads as fine noise
// instead of coherent bands. Pass 0 from the env bake (see fs_sky_env for why a jittered march is
// wrong at 256x128).
fn cirrus_layer(pos: vec3<f32>, dir: vec3<f32>, sun: vec3<f32>, radiance: f32, bg: vec3<f32>,
                jitter: f32, max_dist: f32) -> CloudResult {
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
    // Puffiness/amount/match resolved ONCE for this pixel — the march below reuses it for every
    // step. Hoisted above `cover` because the coverage threshold now comes out of it (AMOUNT).
    let sh = cirrus_shape();
    let cover = sh.cover * smoothstep(0.02, 0.35, sky.cloud0.x);
    let cos_t = dot(dir, sun);
    // Ice: a strong forward lobe (the halo/silver look) over an isotropic floor. Thin enough that
    // self-shadowing is not worth a light march, so no sun-transmittance term through the sheet.
    // MATCH slides it toward the DECK's own anisotropy (cloud2.x) with far less isotropic floor,
    // because the halo/silver look is what ice does and water droplets do not.
    let ice_phase = mix(hg(cos_t, 0.75), 1.0 / (4.0 * PI), 0.55);
    let deck_phase = mix(hg(cos_t, clamp(sky.cloud2.x, 0.0, 0.95)), 1.0 / (4.0 * PI), 0.25);
    let phase = mix(ice_phase, deck_phase, sh.match_deck);
    let sun_col = sample_transmittance(pos, sun) * radiance;
    let luma = dot(bg, vec3<f32>(0.2126, 0.7152, 0.0722));
    let ambient = mix(bg, vec3<f32>(luma), 0.3) * sky.cloud2.z * 0.8;
    // Kept SPLIT, not summed, because MATCH's self-shading term below may only attenuate the
    // SUN. Dimming the sky-ambient fill with it as well darkens the shaded sides toward whatever
    // hue the direct sun happens to be, which at a low sun turned them olive-brown; a real
    // cumulus base is blue-grey precisely BECAUSE the sun is what the cloud above has removed and
    // sky ambient is what is left.
    // SKY-004: the moon lights the high layer too. A cirrostratus veil under a full moon is one of
    // the more recognisable night skies there is -- a pale halo-ringed sheet with the stars going
    // soft behind it -- and the layer is optically thin, so this needs no light march: it is the
    // same `lit_sun` lane with the other body's irradiance and phase in it, which also means it
    // inherits the MATCH self-shading term at line ~1913 for free.
    let ml = lunar_cloud_light(pos, sun);
    let cos_m = dot(dir, ml.dir);
    let ice_phase_m = mix(hg(cos_m, 0.75), 1.0 / (4.0 * PI), 0.55);
    let deck_phase_m = mix(hg(cos_m, clamp(sky.cloud2.x, 0.0, 0.95)), 1.0 / (4.0 * PI), 0.25);
    let phase_m = mix(ice_phase_m, deck_phase_m, sh.match_deck);
    let lit_sun = sun_col * phase + ml.col * (phase_m * ml.on);
    let lit = lit_sun + ambient;
    // Fade in off the horizon, where the sheet is hundreds of km away and haze owns the sky.
    let horizon_fade = smoothstep(0.015, 0.07, dir.y);

    if (mode < 1.5) {
        // ---- FLAT: one intersection with the sheet at the shell centre, one field evaluation.
        let hit = ray_sphere(pos, dir, planet_r + sh.alt);
        let t = hit.y;
        if (t <= 0.0 || t >= max_dist) {
            return res; // no intersection, or the opaque scene is nearer — terrain occludes it
        }
        let p = pos + dir * t;
        // Same world anchoring and MINUS wind offset as the deck (cloud1.xy is CPU-wrapped for
        // precision, so it must be used as given, not rescaled to fake a different wind speed
        // aloft; subtracted so the sheet drifts downwind like the deck does).
        let world_xz = sky.cam_pos.xz + p.xz - sky.cloud1.xy;
        var d = cirrus_field(world_xz, 0.5, cover, cirrus_warp(world_xz, sh.scale), sh);
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
    let r_bot = planet_r + sh.alt - sh.thick * 0.5;
    let r_top = planet_r + sh.alt + sh.thick * 0.5;
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
    // Adaptive step COUNT, derived from the ray's own path exactly as the deck's march does it, so
    // sampling density is roughly constant regardless of shell thickness or view angle. A grazing
    // ray now gets more samples instead of coarser ones -- which is the whole cure for the bands.
    let path = t1 - t0;
    let target_step = max(sh.thick * CIRRUS_STEP_FRAC / sh.step_gain, 1.0);
    let n = clamp(ceil(path / target_step), CIRRUS_STEPS_MIN, f32(CIRRUS_STEPS_MAX));
    let steps = i32(n);
    let dt = path / n;
    // ONE warp tap for the whole column (see cirrus_warp) — evaluated at the shell midpoint.
    let mid = pos + dir * (t0 + (t1 - t0) * 0.5);
    let warp = cirrus_warp(sky.cam_pos.xz + mid.xz - sky.cloud1.xy, sh.scale);
    let sigma = cirrus_sigma(sh);
    var t = 0.0;
    var col = vec3<f32>(0.0);
    var trans = 1.0;
    // Jittered start (see the parameter's note). 0.5 for jitter = 0 keeps the env bake's old
    // midpoint sampling exactly.
    t = t0 + dt * mix(0.5, jitter, step(0.0001, jitter));
    for (var i: i32 = 0; i < steps; i = i + 1) {
        // A matched layer reaches 8 optical depths, so most rays are opaque long before the last
        // sample. The deck's march has always bailed on transmittance; the cirrus shell could not
        // afford to care at 6 steps and now can.
        if (trans < 0.01) {
            break;
        }
        let p = pos + dir * t;
        let h = (length(p) - r_bot) / sh.thick;
        let prof = cirrus_profile(h);
        if (prof > 0.0) {
            let world_xz = sky.cam_pos.xz + p.xz - sky.cloud1.xy;
            var d = cirrus_field(world_xz, h, cover, warp, sh) * prof;
            d = d * horizon_fade * (1.0 - smoothstep(CIRRUS_FADE * 0.4, CIRRUS_FADE, t));
            if (d > 0.0005) {
                let seg = exp(-d * sigma * dt);
                // Cheap vertical self-shading, MATCH only. A veil has no meaningful top-to-bottom
                // gradient and the layer has never paid for a light march; a cumuliform layer that
                // is lit uniformly through 8 optical depths reads as glowing fog. Approximating
                // the overhead column by the density above `h` costs one exp and gives the shape
                // the eye actually reads as "solid cloud": bright tops, shaded bases.
                let shade = mix(1.0, exp(-sh.density * (1.0 - h) * 0.35), sh.match_deck);
                col += trans * (lit_sun * shade + ambient) * (1.0 - seg);
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

// ---- Cloud cover over a celestial disc ---------------------------------------
// The cloud composite that runs after geometry attenuates everything the sky pass drew by the view
// ray's transmittance, ONCE. For a background of ordinary radiance that is the whole story. The sun
// and moon discs are not ordinary: the sun's radiance is its irradiance divided by its solid angle,
// ~1.5e4x the surrounding sky, so even a deck that passes a few per cent leaves a disc that still
// dominates the frame -- and it stays a SHARP disc, which no amount of cloud in front of it should
// ever allow. Cloud destroys the coherent disc long before it stops passing light.
//
// So the discs are attenuated by the SQUARE of their own ray's cloud transmittance here, on top of
// the composite's linear pass: cubic overall. A thin veil still lets a bright, hazy sun through; a
// rain deck does not. Marched at full resolution for the disc pixels only -- the caller gates on
// being inside a disc, which is a few hundred pixels, so the short march costs nothing measurable.
fn disc_cloud_cover(pos: vec3<f32>, dir: vec3<f32>, sun: vec3<f32>, radiance: f32, bg: vec3<f32>) -> f32 {
    let deck = march_clouds(pos, dir, sun, radiance, bg, 24.0, 2, 0.5, 1e12);
    // Fixed midpoint sampling, matching the deck march's own 0.5 above and NOT a per-pixel
    // hash: this runs only for the few hundred pixels inside a celestial disc, and a
    // per-pixel jitter there would scatter the disc's attenuation into visible speckle on
    // the one object in the frame whose smoothness the eye is most sensitive to.
    let both = cloud_over_cirrus(deck, cirrus_layer(pos, dir, sun, radiance, bg, 0.5, 1e12));
    return clamp(both.trans, 0.0, 1.0);
}

@fragment
fn fs_sky(in: VsOut) -> @location(0) vec4<f32> {
    let dir = normalize(in.ray_dir);
    // Pixel angular size, for the star discs' anti-aliasing floor. Taken HERE, at the top of the
    // entry point, because a derivative is only defined in uniform control flow and star_field
    // runs inside a branch. See g_px_angle.
    g_px_angle = length(fwidth(dir));
    let sun = normalize(sky.sun_dir.xyz);
    let planet_r = sky.params.z;
    let cam_alt = max(sky.night_zenith.w, 0.0);
    let pos = vec3<f32>(0.0, planet_r + cam_alt, 0.0);

    // Two lanes on purpose: `bg` is what a CLOUD standing on this ray may be lit by (star-free at
    // night), `color` is what the eye sees. With the default occlusion of 1 exactly one of the two
    // star_field lookups below actually runs, so this costs nothing over the single call it
    // replaces -- and the disc's short cloud march below stops being lit by the star behind it.
    let lanes=sky_radiance_lanes(dir,clamp(1.0-sky.night_sky.x,0.0,1.0));
    let bg = lanes.original;
    let star_occl = clamp(sky.night_sky.x, 0.0, 1.0);
    var color = bg;
    let fog_chroma=fog_neutral_horizon(dir.y,sky.night_sky.w);
    if(fog_chroma>0.0) {
        // Modify only fogged horizon atmosphere, before adding visible stars,
        // sun/moon discs and clouds. bg remains the original cloud light source.
        color+=fog_neutral_radiance(lanes.atmosphere,fog_chroma)-lanes.atmosphere;
    }
    if (star_occl > 0.001) {
        color = color + star_radiance(dir, sun) * star_occl;
    }

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
        // Is this pixel inside EITHER disc? Only then is the short cloud march below worth firing,
        // and only then does anything in this block cost more than a dot product.
        let moon_dir = normalize(sky.moon_dir.xyz);
        let in_moon = sky.moon_params.w >= 0.5 &&
                      dot(dir, moon_dir) > cos(max(sky.moon_params.x, 1.0e-4));
        var disc_cover = 1.0;
        if (cos_sun > cos_radius || in_moon) {
            let t = disc_cloud_cover(pos, dir, sun, sky.sun_dir.w * sky.params.y, bg);
            disc_cover = t * t;
        }
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
            color = color + disc * sun_t * limb * disc_cover;
        }
        // Moon disc, same gate: only where the view ray misses the planet, and drawn here in
        // fs_sky (not in sky_radiance) so the depth-aware cloud composite that runs after
        // geometry occludes it exactly as it occludes the sun.
        color = color + moon_disc(dir, pos) * disc_cover;
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
    // Same two lanes as fs_sky: the reflected sky keeps its stars, the cloud march that runs over
    // it does not get to be lit by them.
    let bg = sky_radiance_cloud_bg(dir);
    let star_occl = clamp(sky.night_sky.x, 0.0, 1.0);
    var color = bg;
    if (star_occl > 0.001) {
        color = color + star_radiance(dir, normalize(sky.sun_dir.xyz)) * star_occl;
    }
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
    let deck = march_clouds(pos, dir, sun, radiance, bg, 12.0, 2, 0.0, 1e12);
    // The cirrus layer goes into the env bake too, so the water reflects — and the SH ambient sees —
    // the same two-layer sky the camera does. It runs in whatever mode the Sky tab selected, and
    // the env map is only 256x128, so even the volumetric path is a rounding error here; the
    // measured cost of the layer as a whole is dominated by the full-resolution cloud pass.
    let cloud = cloud_over_cirrus(deck, cirrus_layer(pos, dir, sun, radiance, bg, 0.0, 1e12));
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

    // Water is depth-read-only. A ray missing the spherical ground can still hit
    // the flat ocean. Stop both decks there, without removing foreground clouds.
    // Mirrored and underwater views opt out via cam_pos.w = 0.
    if (sky.cam_pos.w > 0.0 && dir.y < -0.000001) {
        scene_dist = min(scene_dist, sky.cam_pos.w / -dir.y);
    }

    // Local sky colour for the cloud AMBIENT -- star-free (see sky_radiance_cloud_bg). This is the
    // pixel where the bug lived: with the full sky radiance here, a star's radiance became the
    // ambient lighting the cloud in front of it, so the composite handed it straight back.
    let bg = sky_radiance_cloud_bg(dir);
    let radiance = sky.sun_dir.w * sky.params.y;
    let deck = march_clouds(pos, dir, sun, radiance, bg,
                            CLOUD_STEPS, CLOUD_LIGHT_STEPS, hash21(in.clip.xy), scene_dist);
    // Second layer, behind the deck. Bounded by the same scene distance, so terrain occludes it.
    let cloud = cloud_over_cirrus(deck, cirrus_layer(pos, dir, sun, radiance, bg,
                                                     hash21(in.clip.xy), scene_dist));

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
        textureStore(cloud_shadow_out, vec2<i32>(gid.xy), vec4<f32>(1.0, smoke0, 0.0, 1.0));
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
        textureStore(cloud_shadow_out, vec2<i32>(gid.xy), vec4<f32>(1.0, smoke, 0.0, 1.0));
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
        textureStore(cloud_shadow_out, vec2<i32>(gid.xy), vec4<f32>(1.0, smoke, 0.0, 1.0));
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
    textureStore(cloud_shadow_out, vec2<i32>(gid.xy), vec4<f32>(shadow, smoke, 0.0, 1.0));
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
        let airlight=fog_neutral_radiance(lum*radiance,sky.night_sky.w);
        let closure=fog_neutral_radiance(sky_full,fog_neutral_horizon(ray.y,sky.night_sky.w));
        let col = mix(airlight,closure,farfade);
        let t_mono = dot(trans, vec3<f32>(0.2126, 0.7152, 0.0722)) * (1.0 - farfade);
        textureStore(froxel_out, vec3<i32>(i32(gid.x), i32(gid.y), i32(z)), vec4<f32>(col, t_mono));
    }
}
