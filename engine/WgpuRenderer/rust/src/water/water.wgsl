#import water_curling_breaker::{curl_point}
#import water_shore_surf::{coastal_surf, SurfState, add_surf_displacement}
// GPU water: a CDLOD surface at the global sea level, displaced by a sum of Gerstner
// waves in the vertex shader and shaded per-fragment with an analytic wave normal, a
// sharp HDR sun specular, and a Fresnel mix toward the horizon tint. The shared grid
// mesh is instanced per node (mirroring terrain) and camera-relative, reversed-Z.
// Shares group(0) (camera UBO + aerial fog) so distant water dissolves into the sky.
//
// Waves are purely cosmetic vertex displacement — gameplay reads the flat sea plane.
// Depth-based colour, refraction and planar reflection are later stages of the water
// look plan; this stage adds shape + glint + reduced transparency, no new render targets.
// The look params are UBO fields (not pipeline overrides) so the Water ImGui tab tunes
// them live.

#import frame::{frame, reverse_z, fog_factor, apply_fog, terrain_sun_shadow, sky_vis_ao, cloud_sun_shadow}
#import shadow::shadow_strength
#import color::srgb_to_linear
#import water_fft_sampling::fft_scaled_uv
#import water_optics::{water_extinction, water_transmittance, water_inscatter, water_albedo_along_path, whitecap_wind_factor}

const PI: f32 = 3.14159265359;

struct WaterParams {
    world_origin: vec2<f32>,
    terrain_grid: f32,
    sea_level: f32,
    hm_width: u32,
    hm_height: u32,
    time: f32,
    wave_amp: f32,       // overall amplitude scale
    wave_choppy: f32,    // horizontal steepness
    wave_speed: f32,     // animation speed
    wave_scale: f32,     // wavelength scale (>1 = larger, farther-apart waves)
    fade_start: f32,     // distance (m) where wave detail starts flattening
    fade_end: f32,       // distance (m) where water is fully flat (kills far moiré/repetition)
    warp_amp: f32,       // de-tiling domain-warp amplitude (m)
    spec_power: f32,     // sun-specular sharpness
    spec_intensity: f32, // sun-specular brightness (HDR, blooms)
    alpha: f32,          // base opacity (Fresnel raises it toward 1 at grazing angles)
    shadow_dim: f32,     // extra darkening of shadowed water (0 = sun-only removal)
    color_ext: f32,      // 1/m: body tint saturates shallow->deep over ~1/ext metres of depth
    coast_fade: f32,     // m of column depth over which the shore ramps transparent->opaque
    shallow_color: vec4<f32>, // rgb = shallow body tint (gamma-space; decoded to linear on HDR)
    deep_color: vec4<f32>,    // rgb = deep body tint
    foam_width: f32,     // m of column depth over which shoreline foam fades out
    foam_intensity: f32, // foam brightness/coverage scale
    swash_amp: f32,      // m the near-shore waterline oscillates in/out (cosmetic)
    swash_speed: f32,    // swash cycles per second
    fft_control: vec4<f32>, // enabled, seed, minimum geometry wavelength, pad
    fft_wind_sea: vec4<f32>, // wind x/z, speed, sea state
    fft_cascade_lengths: vec4<f32>, // stable world-space cascade lengths
    flow_direction_speed: vec4<f32>, // x/z direction, m/s, water kind (0 ocean, 1 river)
    debug_params: vec4<f32>, // WTR-003: x = debug view (0 = normal), y = spray gate, z = spray activity, w = viewport height px
    look_params: vec4<f32>,  // WTR-LOOK: x = energy model (0 legacy, 1 physical), y = glitter gain, z = SSS gain, w = reflection gain
    sea_params: vec4<f32>,   // WTR-LOOK: x = sea-state coupling, y = residual spectrum amp, z = low quality, w = shore breaker gain
    // Declared so the two lanes below are reachable; the underwater COMPOSITOR is a separate
    // shader that owns underwater_params in full.
    underwater_params: vec4<f32>,
    // x = underwater effect on/off (read by the compositor, not here).
    // y = WAVE foam intensity: whitecaps and persistent breaker foam only, independent of the
    //     shoreline band that foam_intensity drives.
    // z = deep-water falloff for that wave foam, 0 = break the same everywhere, 1 = open ocean
    //     stays nearly smooth. w reserved.
    underwater_gate: vec4<f32>,
    // WRL-002: x = shared optical model (1) / previous physical composite (0), y = whitecap
    // wind gate, z = local surface level under the camera, w = its wave scale.
    optics_params: vec4<f32>,
    // WRL-003: body table, two lanes per body: [2i] = ellipse (cx, cz, rx, rz),
    // [2i+1] = (level, wave scale, kind, profile). See wgpu_renderer.hpp.
    bodies: array<vec4<f32>, 48>,
};

// Must match GRID_N in water/mod.rs -- this drives the CDLOD morph target, so a
// mismatch cracks every LOD boundary. Intentionally denser than WaterWgpu's CDLOD
// leaf span so near-field FFT displacement stays smooth; see the sizing note in
// water/mod.rs for why 96 is above the shader's own cascade cutoff.
const GRID_N: f32 = 96.0;

@group(1) @binding(0) var<uniform> wp: WaterParams;
// Opaque scene depth from the prepass, farthest-sample resolved (single-sample: 1x aspect or the
// MSAA far-resolve). Used to reconstruct the seabed and hence the water column depth for depth-based
// colour + soft shore + foam. The FARTHEST sample matters: a nearest resolve would read A2C foliage
// / rotor edges as the "seabed", collapsing water_depth to ~0 and ringing them with foam.
@group(1) @binding(1) var scene_depth: texture_depth_2d;
// Sky reflection environment map (equirect, linear radiance) + its sampler (U wraps, V clamps),
// for the Stage-4a real sky reflection: sampled in the reflected view direction (HDR path only).
@group(1) @binding(2) var sky_env: texture_2d<f32>;
@group(1) @binding(3) var sky_env_samp: sampler;
@group(1) @binding(4) var interaction_field: texture_2d<f32>;
@group(1) @binding(5) var interaction_samp: sampler;
@group(1) @binding(6) var fft_displacement: texture_2d_array<f32>;
@group(1) @binding(7) var fft_dynamics: texture_2d_array<f32>;
@group(1) @binding(8) var fft_auxiliary: texture_2d_array<f32>;
@group(1) @binding(9) var fft_samp: sampler;
@group(1) @binding(10) var foam_history: texture_2d<f32>;
@group(1) @binding(11) var foam_samp: sampler;
// Completed opaque HDR scene snapshot. It is resolved/copied before water starts, never
// sampled from the colour target currently being blended into.
@group(1) @binding(12) var scene_color: texture_2d<f32>;
@group(1) @binding(13) var scene_samp: sampler;
@group(1) @binding(14) var planar_color: texture_2d<f32>;
@group(1) @binding(15) var planar_samp: sampler;
struct PlanarParams { full_vp: mat4x4<f32>, valid: vec4<f32> };
@group(1) @binding(16) var<uniform> planar: PlanarParams;

// Multi-band open-ocean carrier. The long swells establish the readable sea state,
// while successively shorter cross-waves break up the regularity around the camera.
// The forthcoming Hydro FFT backend replaces this analytic spectrum but samples through
// the same CDLOD surface path, so the engine integration remains stable in the interim.
const NUM_WAVES: i32 = 8;
const WAVES = array<vec4<f32>, 8>(
    vec4<f32>( 0.92,  0.38, 70.0, 1.100),
    vec4<f32>( 0.42,  0.91, 39.0, 0.650),
    vec4<f32>(-0.61,  0.79, 24.0, 0.380),
    vec4<f32>( 0.97, -0.22, 15.0, 0.200),
    vec4<f32>(-0.20, -0.98,  9.0, 0.120),
    vec4<f32>( 0.70,  0.72,  5.0, 0.065),
    vec4<f32>(-0.94,  0.34,  3.0, 0.035),
    vec4<f32>( 0.20, -0.98,  1.8, 0.015),
);
const G: f32 = 9.81;      // gravity, for the deep-water dispersion omega = sqrt(g*k)
const TWO_PI: f32 = 6.2831853;

// Per-wave steepness, normalised by 1/(k*A*N) so the summed horizontal displacement
// stays below the loop-forming limit regardless of the chosen choppiness.
fn wave_steepness(k: f32, a: f32) -> f32 {
    return wp.wave_choppy / max(k * a * f32(NUM_WAVES), 1e-4);
}

// 1 near the camera, ramping to 0 by fade_end so ALL wave detail (short and long)
// flattens with distance — this is what removes the airplane-view moiré and the far
// repetition: past fade_end the water is a smooth mirror of the horizon tint, which is
// also how distant water genuinely reads.
fn wave_fade(dist: f32) -> f32 {
    return 1.0 - smoothstep(wp.fade_start, wp.fade_end, dist);
}

// Slowly-varying position offset (two incommensurate octaves) that bends the wave field
// off the regular grid. Low-frequency so its gradient is small and the analytic normal
// (evaluated at the warped position) stays accurate to first order.
fn domain_warp(p: vec2<f32>) -> vec2<f32> {
    let w1 = vec2<f32>(sin(p.y * 0.024 + 1.7), sin(p.x * 0.024 + 4.2));
    let w2 = vec2<f32>(sin(p.y * 0.057 + 3.1), sin(p.x * 0.057 + 0.6));
    return wp.warp_amp * (w1 + 0.5 * w2);
}

// Sum of Gerstner horizontal + vertical displacement at base world-xz `p_in`, time
// `wp.time`, faded by distance so far water flattens (no geometric aliasing).
fn gerstner_disp(p_in: vec2<f32>, dist: f32) -> vec3<f32> {
    let fade = wave_fade(dist);
    let p = p_in + domain_warp(p_in);
    let scale = max(wp.wave_scale, 0.01);
    var disp = vec3<f32>(0.0);
    for (var i = 0; i < NUM_WAVES; i = i + 1) {
        let w = WAVES[i];
        let d = normalize(w.xy);
        let a = w.w * wp.wave_amp * fade;
        let k = TWO_PI / (w.z * scale);
        let omega = sqrt(G * k) * wp.wave_speed;
        let phase = k * dot(d, p) - omega * wp.time;
        let q = wave_steepness(k, a);
        let c = cos(phase);
        disp.x = disp.x + q * a * d.x * c;
        disp.z = disp.z + q * a * d.y * c;
        disp.y = disp.y + a * sin(phase);
    }
    return disp;
}

// Analytic surface normal of the same Gerstner sum, evaluated per-fragment (like
// terrain's per-fragment normal) so specular stays crisp independent of tessellation.
// The same distance fade flattens the far-field normal, removing the specular shimmer.
fn gerstner_normal(p_in: vec2<f32>, dist: f32) -> vec3<f32> {
    let fade = wave_fade(dist);
    let p = p_in + domain_warp(p_in);
    let scale = max(wp.wave_scale, 0.01);
    var nx = 0.0;
    var ny = 0.0;
    var nz = 0.0;
    for (var i = 0; i < NUM_WAVES; i = i + 1) {
        let w = WAVES[i];
        let d = normalize(w.xy);
        let a = w.w * wp.wave_amp * fade;
        let k = TWO_PI / (w.z * scale);
        let omega = sqrt(G * k) * wp.wave_speed;
        let phase = k * dot(d, p) - omega * wp.time;
        let q = wave_steepness(k, a);
        let wa = k * a;
        let c = cos(phase);
        let s = sin(phase);
        nx = nx - d.x * wa * c;
        nz = nz - d.y * wa * c;
        ny = ny - q * wa * s;
    }
    return normalize(vec3<f32>(nx, 1.0 + ny, nz));
}

// Near a coastline, shallow-water waves refract and their crests run toward land.
// This is a local breaker train layered over the unchanged offshore FFT, avoiding
// seams from attempting to rotate a global FFT lookup.
// Shoaling. As a swell runs into shallowing water its group speed drops, so its energy piles up:
// Green's law gives height ~ depth^(-1/4) while the wavelength shortens. `shore_factor` already
// ramps 0 (deep water) -> 1 (the beach), so it stands in for that depth coordinate. The train also
// tightens as it shoals, which is what turns a long low swell into a steep short breaker.
struct ShoreWave {
    amp_a: f32,
    amp_b: f32,
    k_a: f32,
    k_b: f32,
    phase_a: f32,
    phase_b: f32,
    dir: vec2<f32>,
};

fn shore_wave_setup(p: vec2<f32>, shore_dir: vec2<f32>, factor: f32) -> ShoreWave {
    let d = normalize(shore_dir + vec2<f32>(1e-4, 0.0));
    let lateral = vec2<f32>(-d.y, d.x);
    let along = dot(p, d);
    let across = dot(p, lateral);
    // Wavenumbers: ~57 m offshore swell compressing to ~24 m at the break (k = 2*pi/lambda).
    let k_a = mix(0.11, 0.26, factor);
    let k_b = mix(0.21, 0.47, factor);
    // Green's law height gain, ~3.2x from the outer band to the break.
    let shoal = 1.0 + factor * factor * 2.2;
    let gain = wp.wave_amp * factor * shoal * max(wp.sea_params.w, 0.0);
    var w: ShoreWave;
    w.dir = d;
    w.k_a = k_a;
    w.k_b = k_b;
    // Crests bend to follow the depth contours, so the lateral wobble stays gentle.
    w.phase_a = along * k_a - wp.time * 2.05 + sin(across * 0.045) * 0.50;
    w.phase_b = along * k_b - wp.time * 2.70 + sin(across * 0.085 + 1.7) * 0.32;
    w.amp_a = 0.62 * gain;
    w.amp_b = 0.19 * gain;
    return w;
}

fn shore_breaker_disp(p: vec2<f32>, shore_dir: vec2<f32>, shore_factor: f32) -> vec3<f32> {
    let factor = clamp(shore_factor, 0.0, 1.0);
    if (factor <= 0.001) { return vec3<f32>(0.0); }
    let w = shore_wave_setup(p, shore_dir, factor);
    // A shoaling wave is not a sine: the crest sharpens and the trough flattens. The second
    // harmonic (a Stokes-style skew, growing with the shoaling factor) buys that peaked profile
    // for one extra sin() rather than a solver.
    let skew = 0.34 * factor;
    let vertical = w.amp_a * (sin(w.phase_a) + skew * sin(2.0 * w.phase_a)) +
        w.amp_b * sin(w.phase_b);
    // Horizontal throw pitches the crest forward toward the beach as it steepens.
    let horizontal = w.amp_a * (0.34 + 0.30 * factor) * cos(w.phase_a) + w.amp_b * 0.18 * cos(w.phase_b);
    return vec3<f32>(w.dir.x * horizontal, vertical, w.dir.y * horizontal);
}

fn shore_breaker_normal(p: vec2<f32>, shore_dir: vec2<f32>, shore_factor: f32) -> vec3<f32> {
    let factor = clamp(shore_factor, 0.0, 1.0);
    if (factor <= 0.001) { return vec3<f32>(0.0, 1.0, 0.0); }
    let w = shore_wave_setup(p, shore_dir, factor);
    let skew = 0.34 * factor;
    // d/d(along) of the displacement above.
    let slope = w.amp_a * w.k_a * (cos(w.phase_a) + 2.0 * skew * cos(2.0 * w.phase_a)) +
        w.amp_b * w.k_b * cos(w.phase_b);
    return normalize(vec3<f32>(-w.dir.x * slope, 1.0, -w.dir.y * slope));
}

fn texture_bicubic_displacement_at(uv: vec2<f32>, layer: i32, magnified: bool) -> vec4<f32> {
    // Low quality: one hardware bilinear tap instead of the four-tap B-spline reconstruction.
    // The bicubic filters run per cascade in both the vertex and fragment stages, so they are the
    // bulk of the water draw — this is where a performance mode has to cut, not in the planar
    // reflection (measured at 0.34 ms of a 30 ms frame).
    //
    // `magnified` is false once this cascade's texels are smaller than a pixel, where the four-tap
    // reconstruction is indistinguishable from the hardware bilinear tap it is built from.
    if (wp.sea_params.z > 0.5 || !magnified) {
        return textureSampleLevel(fft_displacement, fft_samp, uv, layer, 0.0);
    }
    let dims = vec2<f32>(textureDimensions(fft_displacement));
    let inv_x = 1.0 / dims.x;
    let inv_y = 1.0 / dims.y;
    let uv_grid = uv * dims + 0.5;
    let fuv = fract(uv_grid);
    let wx = cubic_weights(fuv.x);
    let wy = cubic_weights(fuv.y);

    // Collapse adjacent B-spline taps into hardware bilinear fetches. Each
    // axis needs its own pair sums; non-adjacent sums create texel-edge jumps.
    let g = vec4<f32>(wx.x + wx.y, wx.z + wx.w, wy.x + wy.y, wy.z + wy.w);
    let floor_uv = floor(uv_grid);
    let hx0 = (wx.y / g.x + (-1.5 + floor_uv.x)) * inv_x;
    let hx1 = (wx.w / g.y + ( 0.5 + floor_uv.x)) * inv_x;
    let hy0 = (wy.y / g.z + (-1.5 + floor_uv.y)) * inv_y;
    let hy1 = (wy.w / g.w + ( 0.5 + floor_uv.y)) * inv_y;
    let w = g.yw / (g.xz + g.yw);

    let s00 = textureSampleLevel(fft_displacement, fft_samp, vec2<f32>(hx0, hy0), layer, 0.0);
    let s10 = textureSampleLevel(fft_displacement, fft_samp, vec2<f32>(hx1, hy0), layer, 0.0);
    let s01 = textureSampleLevel(fft_displacement, fft_samp, vec2<f32>(hx0, hy1), layer, 0.0);
    let s11 = textureSampleLevel(fft_displacement, fft_samp, vec2<f32>(hx1, hy1), layer, 0.0);

    return mix(mix(s00, s10, w.x), mix(s01, s11, w.x), w.y);
}

// Sample absolute xz so camera-relative rendering never changes FFT phase.
fn fft_sample(xz: vec2<f32>, layer: i32, dist: f32) -> vec4<f32> {
    let length_m = max(wp.fft_cascade_lengths[layer], 1.0);
    let dims = f32(textureDimensions(fft_displacement).x);
    let texel_m = length_m * max(wp.wave_scale, 0.01) / max(dims, 1.0);
    let pixel_m = dist * 2.0 / (max(wp.debug_params.w, 1.0) * max(frame.proj[1][1], 1e-3));
    let magnified = texel_m > pixel_m;
    // Preserve the Water-tab convention: scale > 1 means longer waves.  The
    // spectrum stays stable; only the world-space lookup is dilated.
    let scale = max(wp.wave_scale, 0.01);
    let uv = fft_scaled_uv(xz, length_m, layer, wp.warp_amp, wp.wave_scale);
    let s = texture_bicubic_displacement_at(uv, layer, magnified);
    // Dilating the lookup changes the wavelength but NOT the stored displacement, so the control
    // used to alter steepness instead of scale: at 0.25 the waves came out four times shorter at
    // unchanged height, which is why the surface went spiky. Scaling the displacement by the same
    // factor keeps H/lambda — the actual wave steepness — constant, so the control does what its
    // name says and only resizes the sea.
    //
    // The slope field needs no such correction: h'(x) = S*h(x/S) differentiates to h'(x/S), so
    // sampling fft_dynamics at the already-dilated coordinate is correct as it stands.
    return vec4<f32>(s.xyz * scale, s.w);
}
// WTR-031 / WTR-032 — Projected footprint cascade visibility weights.
// Calculates separate weights for geometry, normal detail, and foam based on projected pixel size.
struct CascadeWeights {
    geometry_weight: f32,
    normal_weight: f32,
    foam_weight: f32,
};

fn compute_cascade_weights(layer: i32, dist: f32, view_dir: vec3<f32>) -> CascadeWeights {
    var w: CascadeWeights;
    let raw_length = wp.fft_cascade_lengths[layer];
    if (raw_length <= 0.0) {
        w.geometry_weight = 0.0;
        w.normal_weight = 0.0;
        w.foam_weight = 0.0;
        return w;
    }
    let length_m = max(raw_length, 1.0);
    let view_angle_cos = max(abs(view_dir.y), 0.1);
    // Real viewport height (debug_params.w, written per frame by WaterWgpu) and the actual
    // vertical projection scale (proj[1][1] = 1/tan(fovY/2)) replace the earlier hardcoded
    // 1080 px / tan(30 deg) constants, so the filtering threshold is correct at every
    // resolution, FOV and camera pitch.
    let screen_h = max(wp.debug_params.w, 1.0);
    let tan_half_fov_y = 1.0 / max(frame.proj[1][1], 1e-4);
    let proj_pixels = (length_m * max(wp.wave_scale, 0.01) * 0.5 * screen_h) / (max(dist, 0.1) * tan_half_fov_y * view_angle_cos);

    w.geometry_weight = smoothstep(1.5, 4.0, proj_pixels);
    w.normal_weight = smoothstep(0.5, 2.0, proj_pixels);
    w.foam_weight = smoothstep(1.0, 3.0, proj_pixels);

    // GodotOceanWaves reference parity tuning:
    // Cascade 2 (16m) is a normal/foam-only detail cascade with 0 displacement.
    if (raw_length < 20.0) {
        w.geometry_weight = 0.0;
    }
    return w;
}

// WTR-038 — GodotOceanWaves-style Pixels-Per-Meter (PPM) bicubic B-spline normal filtering
fn cubic_weights(a: f32) -> vec4<f32> {
    let a2 = a * a;
    let a3 = a2 * a;
    let w0 = -a3 + a2 * 3.0 - a * 3.0 + 1.0;
    let w1 = a3 * 3.0 - a2 * 6.0 + 4.0;
    let w2 = -a3 * 3.0 + a2 * 3.0 + a * 3.0 + 1.0;
    let w3 = a3;
    return vec4<f32>(w0, w1, w2, w3) / 6.0;
}

fn texture_bicubic_dynamics(uv: vec2<f32>, layer: i32) -> vec4<f32> {
    let dims = vec2<f32>(textureDimensions(fft_dynamics));
    let inv_x = 1.0 / dims.x;
    let inv_y = 1.0 / dims.y;
    let uv_grid = uv * dims + 0.5;
    let fuv = fract(uv_grid);
    let wx = cubic_weights(fuv.x);
    let wy = cubic_weights(fuv.y);

    // Match displacement's adjacent B-spline pairs. Mixing non-adjacent taps
    // or the other axis's weights makes specular normals jump at texel edges.
    let g = vec4<f32>(wx.x + wx.y, wx.z + wx.w, wy.x + wy.y, wy.z + wy.w);
    let floor_uv = floor(uv_grid);
    // Offset x coords
    let hx0 = (wx.y / g.x + (-1.5 + floor_uv.x)) * inv_x;
    let hx1 = (wx.w / g.y + ( 0.5 + floor_uv.x)) * inv_x;
    // Offset y coords
    let hy0 = (wy.y / g.z + (-1.5 + floor_uv.y)) * inv_y;
    let hy1 = (wy.w / g.w + ( 0.5 + floor_uv.y)) * inv_y;
    let w = g.yw / (g.xz + g.yw);

    let s00 = textureSampleLevel(fft_dynamics, fft_samp, vec2<f32>(hx0, hy0), layer, 0.0);
    let s10 = textureSampleLevel(fft_dynamics, fft_samp, vec2<f32>(hx1, hy0), layer, 0.0);
    let s01 = textureSampleLevel(fft_dynamics, fft_samp, vec2<f32>(hx0, hy1), layer, 0.0);
    let s11 = textureSampleLevel(fft_dynamics, fft_samp, vec2<f32>(hx1, hy1), layer, 0.0);

    return mix(mix(s00, s10, w.x), mix(s01, s11, w.x), w.y);
}

fn sample_fft_dynamics_filtered(xz: vec2<f32>, layer: i32, length_m: f32, dist: f32) -> vec4<f32> {
    let uv = fft_scaled_uv(xz, length_m, layer, wp.warp_amp, wp.wave_scale);
    if (wp.sea_params.z > 0.5) {
        return textureSampleLevel(fft_dynamics, fft_samp, uv, layer, 0.0);
    }
    // Bicubic reconstruction only matters while a cascade's texels are MAGNIFIED — i.e. while one
    // texel still covers more than a pixel. Past that point the hardware bilinear tap is already
    // averaging sub-pixel detail and the four-tap B-spline is indistinguishable from it, so it is
    // three extra fetches for nothing.
    //
    // GodotOceanWaves does the same thing (it blends bicubic -> bilinear on a pixels-per-metre
    // term); this switches outright at the crossover so the cost is actually saved rather than
    // paid twice. The threshold is per cascade, since a 16 m cascade's texels shrink below a pixel
    // far sooner than an 88 m one's.
    let dims = f32(textureDimensions(fft_dynamics).x);
    let texel_m = length_m * max(wp.wave_scale, 0.01) / max(dims, 1.0);
    // Approximate world size of one pixel at this distance, from the live viewport height and the
    // projection's vertical FOV term — the same inputs compute_cascade_weights uses.
    let pixel_m = dist * 2.0 / (max(wp.debug_params.w, 1.0) * max(frame.proj[1][1], 1e-3));
    if (texel_m <= pixel_m) {
        return textureSampleLevel(fft_dynamics, fft_samp, uv, layer, 0.0);
    }
    return texture_bicubic_dynamics(uv, layer);
}

fn fft_geometry_disp(xz: vec2<f32>, dist: f32, shore_factor: f32) -> vec3<f32> {
    var disp = vec3<f32>(0.0);
    // Approximate per-vertex view direction from the undisplaced base position — good
    // enough for LOD weighting (the displaced position differs by metres at most).
    let approx_rel = vec3<f32>(xz.x, wp.sea_level, xz.y) - frame.cam_pos.xyz;
    let view_dir = approx_rel / max(length(approx_rel), 1e-3);
    // Shoaling redistributes the spectrum. A long swell entering shallow water slows, shortens and
    // steepens, so energy moves from the long cascades into the short ones; conversely open ocean
    // should be dominated by long swell rather than the same chop that appears at the beach.
    // shore_factor is the CPU-side shoaling coordinate (0 = deep, 1 = at the beach) and is now
    // continuous across CDLOD nodes, so using it here introduces no seam.
    let shoal = clamp(shore_factor, 0.0, 1.0);
    // The redistribution must be ENERGY PRESERVING. Applying the per-cascade bias directly dropped
    // near-shore displacement by about a third, because a disabled cascade (preset 1 runs 88/57/16
    // and leaves layer 3 off) received the boost while every live cascade received the cut — so the
    // surface sank near the beach and left a visible gap at the waterline. Accumulating both the
    // biased and unbiased weights and rescaling by their ratio moves energy BETWEEN cascades
    // without removing any, whatever subset of cascades a preset happens to enable.
    var weight_biased = 0.0;
    var weight_plain = 0.0;
    for (var layer = 0; layer < 4; layer = layer + 1) {
        let w = compute_cascade_weights(layer, dist, view_dir);
        // Cascade 0 is the longest domain and 3 the shortest (see fft_cascade_lengths), so bias
        // toward low layers offshore and high layers inshore.
        let long_wave = 1.0 - f32(layer) / 3.0;
        let shoal_weight = mix(1.0, mix(0.55, 1.45, 1.0 - long_wave), shoal);
        let combined = w.geometry_weight * shoal_weight;
        // Skip cascades that contribute nothing. compute_cascade_weights already returns a zero
        // weight for a DISABLED cascade (the reference preset leaves layer 3 off) and for one whose
        // wavelength has fallen below the projected pixel footprint at this distance — yet the
        // sample was taken anyway, and each one is a four-tap bicubic fetch per vertex. On a
        // 192x192 node that is a lot of texture traffic multiplied by a contribution of zero.
        //
        // This cannot change the image: the threshold is 0.2% of a cascade's weight, and the term
        // being skipped is multiplied by that same near-zero weight. It is skipped in the
        // normalisation totals too, so the energy-preserving rescale stays consistent.
        if (combined > 0.002) {
            disp = disp + fft_sample(xz, layer, dist).xyz * combined;
            weight_biased = weight_biased + combined;
            weight_plain = weight_plain + w.geometry_weight;
        }
    }
    if (weight_biased > 1e-4) {
        disp = disp * (weight_plain / weight_biased);
    }
    return disp;
}

fn world_to_material_pos(world_xz: vec2<f32>, dist: f32) -> vec2<f32> {
    var q = world_xz;
    if (wp.fft_control.x > 0.5) {
        for (var i = 0; i < 3; i = i + 1) {
            // Shoaling bias is intentionally omitted from the inversion iteration: it only needs
            // to converge on the undisplaced material coordinate, and feeding a depth-dependent
            // weight into a fixed-point iteration would make convergence depth-dependent too.
            let disp_xz = fft_geometry_disp(q, dist, 0.0).xz;
            q = world_xz - disp_xz;
        }
    }
    return q;
}

fn whitewater_surface_transition(world_xz: vec2<f32>, dist: f32) -> f32 {
    let mat_q = world_to_material_pos(world_xz, dist);
    let disp = fft_geometry_disp(mat_q, dist, 0.0);
    return disp.y;
}

fn fft_normal_with_weights(xz: vec2<f32>, dist: f32, view_dir: vec3<f32>) -> vec3<f32> {
    var slope = vec2<f32>(0.0);
    for (var layer = 0; layer < 4; layer = layer + 1) {
        let length_m = max(wp.fft_cascade_lengths[layer], 1.0);
        let w = compute_cascade_weights(layer, dist, view_dir);
        // As in fft_geometry_disp: a cascade with a zero normal weight (disabled, or filtered out
        // because its wavelength is below the projected pixel footprint) was still paying for a
        // four-tap bicubic fetch before being multiplied by zero. This is the FRAGMENT path, so it
        // is the more expensive of the two. Explicit-LOD sampling is used throughout, so taking it
        // inside non-uniform control flow is well-defined.
        if (w.normal_weight > 0.002) {
            // Both position and displacement are dilated: d[S*h(x/S)]/dx = h'(x/S).
            // Keep the producer's independent artistic normal strength unchanged.
            let layer_slope = sample_fft_dynamics_filtered(xz, layer, length_m, dist).xy;
            slope = slope + layer_slope * w.normal_weight;
        }
    }
    return normalize(vec3<f32>(-slope.x, 1.0, -slope.y));
}

// The interaction field follows the camera in a 256m domain. Outside it samples zero;
// zero events therefore leave the established Gerstner-only path bit-for-bit unchanged.
fn interaction_sample(xz: vec2<f32>) -> vec4<f32> {
    let domain_origin = vec2<f32>(floor((frame.cam_pos.x - 128.0) / 4.0) * 4.0, floor((frame.cam_pos.z - 128.0) / 4.0) * 4.0);
    let uv = (xz - domain_origin) / 256.0;
    let inside = step(0.0, uv.x) * step(0.0, uv.y) * step(uv.x, 1.0) * step(uv.y, 1.0);
    return textureSampleLevel(interaction_field, interaction_samp, clamp(uv, vec2<f32>(0.001), vec2<f32>(0.999)), 0.0) * inside;
}
// Foam shares the interaction's snapped 256 m camera-relative domain, but its history is
// reprojected by the compute pass so this stable world lookup does not swim with the camera.
fn persistent_foam_sample(xz: vec2<f32>) -> vec4<f32> {
    let domain_origin = vec2<f32>(floor((frame.cam_pos.x - 128.0) / 4.0) * 4.0, floor((frame.cam_pos.z - 128.0) / 4.0) * 4.0);
    let uv = (xz - domain_origin) / 256.0;
    let inside = step(0.0, uv.x) * step(0.0, uv.y) * step(uv.x, 1.0) * step(uv.y, 1.0);
    return textureSampleLevel(foam_history, foam_samp, clamp(uv, vec2<f32>(0.001), vec2<f32>(0.999)), 0.0) * inside;
}

struct VsOut {
    @builtin(position) clip: vec4<f32>,
    @location(0) world_pos: vec3<f32>, // camera-relative displaced position
    @location(1) base_xz: vec2<f32>,   // undisplaced world-xz (for per-fragment normal)
    @location(2) fog: f32,             // 1 = keep colour, 0 = full fog
    @location(3) shore_dir: vec2<f32>,
    @location(4) shore_factor: f32,
    @location(5) body_level: f32,  // WRL-003: this node's mean surface level
    @location(6) body_id: f32,     // WRL-003: 0 = ocean, i + 1 = wp.bodies entry i
};

// Terrain heightmap for the vertex-stage seabed clamp (see clamp_to_seabed below).
struct ConformParams {
    origin: vec2<f32>,     // world xz of heightmap texel (0,0)
    terrain_grid: f32,     // world metres per texel
    enabled: f32,          // 0 until a world is loaded — clamp is a no-op then
    hm_width: u32,
    hm_height: u32,
};
@group(1) @binding(17) var seabed_heightmap: texture_2d<f32>;
@group(1) @binding(18) var<uniform> cf: ConformParams;

fn pilot_surf(p: vec2<f32>) -> SurfState {
    return coastal_surf(seabed_heightmap, p, cf.origin, cf.terrain_grid,
                        cf.enabled * surf_pilot, wp.sea_level, wp.time, 1.0);
}

fn seabed_load(ix: i32, iz: i32) -> f32 {
    let cx = clamp(ix, 0, i32(cf.hm_width) - 1);
    let cz = clamp(iz, 0, i32(cf.hm_height) - 1);
    return textureLoad(seabed_heightmap, vec2<i32>(cx, cz), 0).x;
}

fn seabed_contains(world_xz: vec2<f32>) -> bool {
    if (cf.enabled <= 0.5 || cf.hm_width < 2u || cf.hm_height < 2u) {
        return false;
    }
    let t = (world_xz - cf.origin) / max(cf.terrain_grid, 1e-4);
    return t.x >= 0.0 && t.y >= 0.0 &&
        t.x <= f32(cf.hm_width - 1u) && t.y <= f32(cf.hm_height - 1u);
}

// Ground height under a world-xz. Must reproduce terrain.wgsl's `sample_height` exactly,
// including its diagonal split: a bilinear approximation would sit above the true surface
// on one triangle of every cell and reintroduce the tear it exists to prevent.
fn seabed_height(world_xz: vec2<f32>) -> f32 {
    let t = (world_xz - cf.origin) / max(cf.terrain_grid, 1e-4);
    let base = floor(t);
    let ix = i32(base.x);
    let iz = i32(base.y);
    let f = t - base;
    let y00 = seabed_load(ix, iz);
    let y01 = seabed_load(ix + 1, iz);
    let y10 = seabed_load(ix, iz + 1);
    let y11 = seabed_load(ix + 1, iz + 1);
    if (f.x <= 1.0 - f.y) {
        return y00 + (y10 - y00) * f.y + (y01 - y00) * f.x;
    }
    return y10 + (y01 - y11) - (y10 - y11) * f.x - (y01 - y11) * f.y;
}

// Direction of rising terrain, hence toward land. This is only evaluated when the optional
// analytic shore-breaker train is enabled. Unlike the old per-CDLOD-node value, every
// instance sharing a world-space edge obtains the same result at that edge.
fn seabed_shore_direction(world_xz: vec2<f32>) -> vec2<f32> {
    let t = (world_xz - cf.origin) / max(cf.terrain_grid, 1e-4);
    let ix = i32(round(t.x));
    let iz = i32(round(t.y));
    let radius = 4;
    let gx = seabed_load(ix + radius, iz) - seabed_load(ix - radius, iz);
    let gz = seabed_load(ix, iz + radius) - seabed_load(ix, iz - radius);
    let gradient = vec2<f32>(gx, gz);
    if (length(gradient) <= 1e-5) {
        return vec2<f32>(1.0, 0.0);
    }
    return normalize(gradient);
}

// THE coast-gap guarantee.
//
// Water is a plane at sea level, cut against the land by the depth test — terrain draws
// first and occludes it. That cut is only correct while the water surface stays ABOVE the
// ground. FFT displacement moves it vertically by up to wave_amp, and on a shallow beach
// the seabed sits barely below sea level, so a trough pushed the surface under the sand;
// the depth test then hid it and tore a hole along the waterline that grew with amplitude.
// Four previous fixes tuned opacity (shoal weight, swash, shore fades) and could not work,
// because the hole is geometry, not alpha.
//
// Clamping the displaced height to the ground makes the failure impossible by construction
// rather than unlikely: if y >= seabed + margin everywhere, no wave height, sea state or
// camera angle can put the surface behind the terrain.
//
// It is also the physically right constraint. Wave height is depth-limited (H ~ 0.78 * d):
// real water cannot hold a trough deeper than its own depth either. Waves therefore flatten
// into the beach as they shoal instead of being clipped by it.
fn clamp_to_seabed_height(seabed_y: f32, y: f32, surface_level: f32) -> f32 {
    // Sits just above the ground so the surface never coincides with terrain depth exactly,
    // which would z-fight along the waterline instead of tearing.
    //
    // Capped at sea level, and that cap is load-bearing: the water grid is drawn across the
    // whole CDLOD tree, INCLUDING over dry land, where it is meant to stay at sea level and
    // be occluded by the terrain above it. An uncapped max() lifted those vertices up onto
    // the ground, painting the sea over beaches and hillsides. Seaward of the waterline the
    // seabed is below sea level, so the cap is inactive and the clamp does its job; landward
    // the floor collapses to sea level and the surface is occluded exactly as before.
    let floor_y = min(seabed_y + 0.02, surface_level);
    return max(y, floor_y);
}

override skirt_k: f32 = 0.0;

@vertex
fn vs_water(
    @location(0) grid_in: vec3<f32>, // xy = unit grid position in [0,1]^2, z = skirt flag
    @location(1) origin: vec2<f32>,  // node world-xz origin
    @location(2) size: f32,          // node world size
    @location(3) lod: u32,
    @location(4) morph: vec2<f32>,   // (morph_start, morph_end) camera-distance band
    @location(5) shore_dir: vec2<f32>,
    @location(6) shore_factor: f32,
    @location(7) body: f32,
) -> VsOut {
    let grid = grid_in.xy;
    // WRL-003: a node's body lane selects its surface. 0 = the ocean plane; i + 1 = wp.bodies[i],
    // which carries its own mean level and wave scale. Bodies get no shore-breaker train (that
    // is an ocean swell shoaling on a beach) and no tide.
    let body_index = i32(body + 0.5);
    var surface_level = wp.sea_level;
    var body_wave = 1.0;
    if (body_index > 0) {
        let b = wp.bodies[3 * (body_index - 1) + 1];
        surface_level = b.x;
        body_wave = max(b.y, 0.0);
        // Bed-following reach: the surface is the terrain along the centreline plus a depth,
        // sampled from the same heightmap the CPU reads, so a river lies in its valley.
        if (b.w > 0.0) {
            let e = wp.bodies[3 * (body_index - 1)];
            let f = wp.bodies[3 * (body_index - 1) + 2];
            let fc = cos(f.w);
            let fs = sin(f.w);
            let world_v = origin + grid * size;
            let dxz = world_v - e.xy;
            let along = fc * dxz.x + fs * dxz.y;
            let centre = e.xy + vec2<f32>(fc, fs) * along;
            if (seabed_contains(centre)) {
                surface_level = seabed_height(centre) + b.w;
            }
        }
    }
    let world_xz_fine = origin + grid * size;
    let dist = length(vec3<f32>(world_xz_fine.x, surface_level, world_xz_fine.y) - frame.cam_pos.xyz);

    // Morph toward the coarser even lattice near the LOD boundary; both the wave sample
    // and the drawn geometry use this morphed base so adjacent levels agree (crack-free).
    var morph_k = 0.0;
    if (morph.y > morph.x)
    {
        morph_k = clamp((dist - morph.x) / (morph.y - morph.x), 0.0, 1.0);
    }
    let gidx = grid * GRID_N;
    let grid_coarse = (round(gidx * 0.5) * 2.0) / GRID_N;
    let base_xz = origin + mix(grid, grid_coarse, morph_k) * size;

    // The old shoaling coordinate and direction were evaluated once at each CDLOD node's
    // centre, then used to deform every vertex in that node. Adjacent nodes consequently
    // displaced their shared edge by different amounts, creating long triangular shoreline
    // wedges. Derive geometry-affecting values from the terrain height at the vertex instead:
    // a shared world position now produces identical output regardless of its owning node.
    var vertex_shore_factor = shore_factor;
    var vertex_shore_dir = shore_dir;
    var base_seabed = surface_level - 1000.0;
    var horizontal_keep = 1.0;
    let has_seabed = seabed_contains(base_xz);
    if (has_seabed) {
        base_seabed = seabed_height(base_xz);
        let local_depth = max(surface_level - base_seabed, 0.0);
        vertex_shore_factor = 1.0 - smoothstep(2.0, 30.0, local_depth);
        // Bottom friction constrains horizontal orbital motion at the beach. This also keeps
        // the base-position seabed sample valid where a tear can occur, avoiding a second
        // heightmap interpolation and preserving the previous vertex texture-fetch cost.
        horizontal_keep = smoothstep(0.08, 2.0, local_depth);
        if (wp.sea_params.w > 0.001) {
            vertex_shore_dir = seabed_shore_direction(base_xz);
        }
    }

    var disp = gerstner_disp(base_xz, dist);
    if (wp.fft_control.x > 0.5) {
        disp = fft_geometry_disp(base_xz, dist, vertex_shore_factor);
    }
    if (body_index == 0) {
        if (surf_pilot > 0.5) {
            let surf = pilot_surf(base_xz);
            disp = add_surf_displacement(disp, surf);
        } else {
            disp = disp + shore_breaker_disp(base_xz, vertex_shore_dir, vertex_shore_factor);
        }
    }
    disp = vec3<f32>(disp.x * horizontal_keep, disp.y, disp.z * horizontal_keep) * body_wave;
    let interaction = interaction_sample(base_xz);
    let y = surface_level + disp.y + interaction.r * 2.5 - grid_in.z * (size / GRID_N) * skirt_k;
    // Horizontal movement has faded to zero where the clamp is load-bearing, so the single
    // base-position seabed sample remains conservative without another four texture loads.
    let displaced_xz = vec2<f32>(base_xz.x + disp.x, base_xz.y + disp.z);
    let clamped_y = select(y, clamp_to_seabed_height(base_seabed, y, surface_level), has_seabed);
    let world_rel = vec3<f32>(displaced_xz.x, clamped_y, displaced_xz.y) - frame.cam_pos.xyz;

    var out: VsOut;
    out.clip = reverse_z(frame.proj * frame.view * vec4<f32>(world_rel, 1.0));
    out.world_pos = world_rel;
    out.base_xz = base_xz;
    out.fog = fog_factor(length(world_rel));
    out.shore_dir = vertex_shore_dir;
    out.shore_factor = select(vertex_shore_factor, 0.0, body_index > 0);
    out.body_level = surface_level;
    out.body_id = body;
    return out;
}

// HDR path: 1 = decode tint/fog to linear, keep the glint un-clamped so it blooms.
override linear: f32 = 0.0;

// 1 = restore the pre-WTR shoreline foam exactly: unfiltered noise, the single-octave streak
// lattice, and the old (much stronger) band gains. Driven from WGR_WATER_SHORE_FOAM_LEGACY so both
// looks are reachable from one binary for an A/B capture. See the shoreline foam block in fs_water.
override shore_foam_legacy: f32 = 0.0;
override surf_pilot: f32 = 0.0;

// Deep-side extent of the shoreline foam band, in units of `ft` (column depth over
// foam_width * 2.2). The owner's report is that the wash still reaches too far out to sea, so
// this is the knob that answers it directly and it is an override rather than a literal because
// "how far out is too far" is a judgement about a picture, and settling it should not need a
// rebuild. WGR_WATER_FOAM_BAND_OUTER retunes it live; see the band in fs_water for what the
// number means in metres of depth.
//
// The deep-side fade START is derived from this (band_outer * 0.32) rather than being a second
// literal, so ONE knob moves the whole deep edge instead of only its outermost tail. At the
// previous 1.10 that expression reproduces the old 0.35 exactly, which is what makes it a
// refactor of the old shape rather than a new one.
override foam_band_outer: f32 = 0.80;

// Strength of the large-scale patchiness applied to the shoreline band (0 = the previous
// stationary field, 1 = full). WGR_WATER_FOAM_PATCH. See the patch block in fs_water.
override foam_patch_gain: f32 = 1.0;

// How far the deep edge of the shoreline band meanders in and out along the coast, as a fraction
// of `ft`. 0 = the previous ruler-straight ribbon. WGR_WATER_FOAM_MEANDER. See the meander block
// in fs_water for why this is a stronger anti-repetition lever than another noise octave.
override foam_band_meander: f32 = 0.34;
override deep_breaker_recovery: f32 = 1.0;
override breaker_cells: f32 = 0.0;

// Water column depth (m) at this fragment: reconstruct the seabed's camera-relative position
// from the opaque prepass depth and take the vertical gap to the water surface. In the fragment
// shader `in.clip` is the framebuffer position (window pixels + depth), so its xy indexes the
// depth texel directly (framebuffer-resolution). The seabed shares this fragment's view ray, so
// its ndc.xy is the surface's ndc.xy (reproject world_pos); combine with the stored reversed-Z
// depth (forward ndc.z = 1 - stored) and inv_view_proj to unproject. Clamp >= 0.
const DEEP: f32 = 1000.0; // "no seabed behind" fallback column depth (metres)

fn seabed_depth(frag_xy: vec2<f32>, surface_rel: vec3<f32>) -> f32 {
    let d = textureLoad(scene_depth, vec2<i32>(frag_xy), 0);
    // d ~ 0 is the reversed-Z far/cleared value: no opaque seabed was drawn behind this pixel
    // (beyond the terrain/fog extent). Treat as maximally deep and skip the unproject, which
    // would divide by a ~0 w there.
    if (d <= 1e-6) {
        return DEEP;
    }
    let clip_s = frame.proj * frame.view * vec4<f32>(surface_rel, 1.0);
    let ndc_xy = clip_s.xy / clip_s.w;
    // Keep reversed depth small: forming 1-d first quantizes kilometre-distant
    // seabeds into horizontal bands. Distribute the inverse transform instead.
    let seabed_h = frame.inv_view_proj * vec4<f32>(ndc_xy, 1.0, 1.0)
        - frame.inv_view_proj[2] * d;
    let seabed_rel = seabed_h.xyz / seabed_h.w;
    return max(surface_rel.y - seabed_rel.y, 0.0);
}

// Recover the world-xz gradient of the reconstructed column depth from screen derivatives.
// A cleared/far depth has already become DEEP, so it naturally disables this shallow-only
// approximation instead of creating flow where terrain/depth data is unavailable.
fn shallow_flow(depth: f32, xz: vec2<f32>) -> vec2<f32> {
    let shallow = smoothstep(0.0, max(wp.coast_fade, 1e-4), depth) *
        (1.0 - smoothstep(max(wp.foam_width, 0.1), max(wp.foam_width * 3.0, 0.3), depth));
    let dxz = dpdx(xz);
    let dyz = dpdy(xz);
    let determinant = dxz.x * dyz.y - dxz.y * dyz.x;
    if (depth >= DEEP * 0.5 || abs(determinant) < 1e-5 || shallow <= 0.0) {
        return vec2<f32>(0.0);
    }
    let ddx = dpdx(depth);
    let ddy = dpdy(depth);
    // Depth increases offshore; carry foam shoreward along the opposite gradient.
    let gradient = vec2<f32>((ddx * dyz.y - ddy * dxz.y) / determinant,
                             (dxz.x * ddy - dyz.x * ddx) / determinant);
    let gradient_length = length(gradient);
    return select(vec2<f32>(0.0), -gradient / gradient_length * shallow * 0.75, gradient_length > 1e-4);
}

// Cheap value noise for the shoreline foam (churning band, no texture).
// Integer-cell bit hash → white-noise value in [0,1). A sin(dot())*large hash (the usual WGSL
// one-liner) loses all precision once world coords reach the thousands — OFP islands are ~12 km —
// and collapses into big axis-aligned blocks that read as a badly-tiled repeating texture. Hashing
// the integer cell index with bit ops is exact at any magnitude. `cell` is already integer-valued
// (vnoise floors before calling); the & 0xffff wrap only repeats every 65536 cells (far off-map).
fn hash2(cell: vec2<f32>) -> f32 {
    let c = vec2<u32>(vec2<i32>(cell) & vec2<i32>(0xffff));
    var n = c.x * 1597334677u + c.y * 3812015801u;
    n = (n ^ (n >> 15u)) * 2246822519u;
    n = n ^ (n >> 13u);
    return f32(n & 0xffffffu) / f32(0x1000000u);
}
fn vnoise(p: vec2<f32>) -> f32 {
    let i = floor(p);
    let f = fract(p);
    // Quintic C2 continuous interpolation curve (6f^5 - 15f^4 + 10f^3) eliminates grid lines
    let u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    let a = hash2(i);
    let b = hash2(i + vec2<f32>(1.0, 0.0));
    let c = hash2(i + vec2<f32>(0.0, 1.0));
    let d = hash2(i + vec2<f32>(1.0, 1.0));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}
// Organic multi-octave domain-warped foam noise (shared by shore swash, persistent breakers, and
// flecks). The unfiltered `foam_noise(p, t)` this replaced is now `foam_noise_lod(p, t, 0.0)`:
// at a zero footprint every octave returns the raw `vnoise` and the threshold collapses back to
// the original smoothstep(0.32, 0.64). See the band-limiting note below.
const FOAM_FREQ: f32 = 0.55; // spatial frequency (per metre)

// ---------------------------------------------------------------------------------------------
// WTR — band-limited foam noise.
//
// `vnoise` is ANALYTIC: unlike a texture it has no mip chain, so every octave keeps full contrast
// no matter how many of its cells land inside one pixel. FOAM_FREQ's top octave is 8.37 * 0.55 =
// 4.6 cells/m (22 cm cells) and the spray term multiplies its input by another 4.6, reaching ~5 cm
// cells; past a few hundred metres thousands of those cells fall in one pixel. Because every foam
// term then THRESHOLDS the field with a smoothstep, the aliasing does not average out into grey —
// it resolves into hard-edged speckle that beats against the pixel grid at a fixed world-space
// frequency, which is the repeating "pattern" a distant coastline shows.
//
// The fix is the mip a procedural field never had: fade an octave toward its own mean (0.5, which
// is exactly what a correctly filtered mip would return) as its cell size approaches the sampling
// footprint. `w` is that footprint expressed in CELLS OF THIS OCTAVE, so callers must scale it by
// the same factor they scale the position by.
fn vnoise_lod(p: vec2<f32>, w: f32) -> f32 {
    let detail = 1.0 - smoothstep(0.30, 0.85, w);
    // Sub-pixel: the octave carries no signal, only aliasing. Skip the hash entirely.
    if (detail <= 0.001) {
        return 0.5;
    }
    // Above-Nyquist near field (and the whole legacy A/B path, which passes w = 0): return the
    // unfiltered value bit-for-bit rather than routing it through a mix by 1.0.
    if (detail >= 0.999) {
        return vnoise(p);
    }
    return mix(0.5, vnoise(p), detail);
}

// A resolved foam patch covers water; it is not a translucent metallic film.
// Reuse the already-filtered structure rather than adding bubble texture fetches.
fn resolved_breaker_coverage(amount: f32, structure: f32, footprint: f32) -> f32 {
    let threshold = mix(0.15, 0.85, clamp(structure, 0.0, 1.0));
    let edge = 0.08 + 0.25 * clamp(footprint, 0.0, 1.0);
    return smoothstep(max(0.001, threshold - edge), min(0.999, threshold + edge),
                      clamp(amount, 0.0, 1.0));
}

// Resolved bubble clusters, not a repeated image or another simulation grid.
// Fade to the average before the cells become subpixel; distant water does no hash work.
fn breaker_bubble_rim(world: vec2<f32>, footprint: f32) -> f32 {
    let frequency = 17.23;
    let detail = 1.0 - smoothstep(0.18, 0.65, footprint * frequency);
    if (detail <= 0.001) { return 0.65; }
    let p = world * frequency;
    let cell = floor(p);
    let within = fract(p);
    var nearest = 4.0;
    for (var y: i32 = -1; y <= 1; y = y + 1) {
        for (var x: i32 = -1; x <= 1; x = x + 1) {
            let offset = vec2<f32>(f32(x), f32(y));
            let seed = hash2(cell + offset) * 4096.0;
            let jitter = vec2<f32>(fract(seed), floor(seed) / 4096.0);
            let delta = offset + vec2<f32>(0.2) + 0.6 * jitter - within;
            let radius = 0.18 + 0.26 * fract(seed * 0.6180339);
            nearest = min(nearest, dot(delta, delta) / (radius * radius));
        }
    }
    let rim = smoothstep(0.45, 1.0, sqrt(nearest));
    return mix(0.65, rim, detail);
}

// Band-limited twin of `foam_noise`. `w_world` is how far `p_world` moves across one pixel, in the
// same units as `p_world` — a caller that pre-scales its position by k must pass k * footprint.
//
// The final threshold has to widen along with the filtering. Leaving it at the sharp
// smoothstep(0.32, 0.64) would take the converged 0.5 mean and re-sharpen it into a hard on/off
// decision, putting the aliasing straight back. Widening to a window centred on 0.5 makes the
// fully-filtered result the field's true mean COVERAGE (0.5) instead of a binary edge, so
// filtering removes variance only — it is not doing the strength reduction's job.
fn foam_noise_lod(p_world: vec2<f32>, t: f32, w_world: f32) -> f32 {
    let p0 = p_world * FOAM_FREQ;
    let w0 = w_world * FOAM_FREQ;
    let warp = vec2<f32>(vnoise_lod(p0 * 0.45 + vec2<f32>(t * 0.20, -t * 0.15), w0 * 0.45),
                         vnoise_lod(p0 * 0.45 + vec2<f32>(-t * 0.15, t * 0.25), w0 * 0.45)) * 1.8 - vec2<f32>(0.9);
    let p = p0 + warp;
    // Each rotated octave's frequency is the length of its matrix rows: 1.97 and 3.93.
    var v = 0.42 * vnoise_lod(p + vec2<f32>(t * 0.60, t * 0.20), w0);
    v = v + 0.28 * vnoise_lod(vec2<f32>(p.x * 1.83 - p.y * 0.72, p.x * 0.72 + p.y * 1.83) - vec2<f32>(t * 0.30, t * 0.50), w0 * 1.97);
    v = v + 0.18 * vnoise_lod(vec2<f32>(p.x * 3.91 + p.y * 0.41, -p.x * 0.41 + p.y * 3.91) + vec2<f32>(t * 0.44, -t * 0.17), w0 * 3.93);
    v = v + 0.12 * vnoise_lod(p * 8.37 + vec2<f32>(17.3, 41.7) + vec2<f32>(-t * 0.18, t * 0.37), w0 * 8.37);
    let sharp = 1.0 - smoothstep(0.30, 0.85, w0);
    return smoothstep(mix(0.44, 0.32, sharp), mix(0.56, 0.64, sharp), v);
}

// Elongated wash streaks that follow the shoreline contour.
//
// The version this replaces was ONE octave of grid value noise evaluated in the
// (alongshore, shoreward) frame at a fixed 0.74 / 0.19 cells per metre, then hard-thresholded.
// Because that frame is near-constant along a stretch of beach, its cells formed a literal
// 1.35 m x 5.3 m rectangular lattice locked to the coast — a repeating tile at a fixed world
// frequency, which is precisely what becomes visible once a whole beach fits in a few pixels.
//
// Two things kill it. Incommensurate octaves (0.74 and 0.317 alongshore) remove the single
// dominant spacing, and a low-frequency warp of the alongshore coordinate bends the rulings so
// they are not parallel straight lines. Band limiting then removes whatever survives once the
// cells go sub-pixel: the threshold window slides UP with the footprint so a filtered streak
// field converges to zero rather than to a flat wash — streaks are a near-field detail and a
// distant beach should read as an even band, not as a lattice.
//
// A third octave was the obvious next move here and is deliberately NOT what this does. The
// streak field is read through a threshold (the smoothstep at the end), so its look depends on the
// VARIANCE of `combined`, not just its mean: averaging in another independent octave narrows the
// distribution and quietly thins every streak on the coast, which then has to be paid back by
// re-tuning the threshold by eye. A domain WARP is free of that — warping a stationary field
// leaves its one-point statistics exactly as they were — so the extra incommensurate octaves go
// into the warp instead, where they buy the same decorrelation at no calibration cost.
fn shoreline_streaks(xz: vec2<f32>, tangent: vec2<f32>, shoreward: vec2<f32>,
                     t: f32, drift: f32, w_world: f32) -> f32 {
    let s = dot(xz, tangent);
    let d = dot(xz, shoreward);
    let bend = vnoise_lod(vec2<f32>(s * 0.031, d * 0.047 + t * 0.03), w_world * 0.047) - 0.5;
    // Second bend, an octave lower and stronger. The single 0.031 bend (32 m) curves the rulings
    // locally, but over the 100-200 m of coastline that fits on screen at once its own period is
    // what you end up seeing; a 0.0117 (85 m) companion at ~2.4x the amplitude carries the rulings
    // through a longer arc so no single bending period is legible. 0.031 / 0.0117 = 2.65, not a
    // simple fraction, so the pair cannot re-align into a beat.
    //
    // Both new warps are skipped under shore_foam_legacy so the A/B stays a clean comparison
    // rather than differing only in the band gains. `shore_foam_legacy` is a pipeline override, so
    // this branch is constant-folded at pipeline creation and costs nothing at runtime.
    var s_warped = s + bend * 9.0;
    var d_warped = d;
    if (shore_foam_legacy <= 0.5) {
        let bend_long = vnoise_lod(vec2<f32>(s * 0.0117 + 41.9, d * 0.0163 - t * 0.011),
                                   w_world * 0.0163) - 0.5;
        s_warped = s_warped + bend_long * 22.0;
        // Warp the SHOREWARD coordinate too. Without this every streak sits at the same offshore
        // offset within the band, so the field is ragged alongshore but perfectly stratified
        // across — rows of dashes at fixed distances from the waterline. A 24 m-cell perturbation
        // of a few metres breaks the stratification, and because it is a warp it cannot change how
        // much foam there is.
        let d_warp = vnoise_lod(vec2<f32>(s * 0.0413 - t * 0.02, d * 0.0271 + 17.3),
                                w_world * 0.0413) - 0.5;
        d_warped = d + d_warp * 5.5;
    }
    let a = vnoise_lod(vec2<f32>(s_warped * 0.74 + t * 0.10, d_warped * 0.19 - t * drift), w_world * 0.74);
    let b = vnoise_lod(vec2<f32>(s_warped * 0.317 - t * 0.07, d_warped * 0.083 + t * drift * 0.55) +
                       vec2<f32>(23.7, 11.3), w_world * 0.317);
    let combined = a * 0.58 + b * 0.42;
    let sharp = 1.0 - smoothstep(0.30, 0.85, w_world * 0.74);
    return smoothstep(mix(0.60, 0.52, sharp), mix(0.80, 0.72, sharp), combined);
}

// A small shading-only ripple field. Rotating each octave avoids aligned fBm cells;
// exponential shaping preserves fine crest definition without changing displacement.
fn micro_fbm(p: vec2<f32>, t: f32) -> f32 {
    let p0 = p + vec2<f32>(0.11, -0.07) * t;
    let p1 = vec2<f32>(p0.x * 0.81 - p0.y * 0.59, p0.x * 0.59 + p0.y * 0.81);
    let p2 = vec2<f32>(p0.x * 0.36 + p0.y * 0.93, -p0.x * 0.93 + p0.y * 0.36);
    let p3 = vec2<f32>(p0.x * 0.97 - p0.y * 0.24, p0.x * 0.24 + p0.y * 0.97);
    var value = 0.52 * vnoise(p0);
    value = value + 0.27 * vnoise(p1 * 2.07 + vec2<f32>(19.1, 7.3));
    value = value + 0.14 * vnoise(p2 * 4.19 + vec2<f32>(3.7, 31.9));
    value = value + 0.07 * vnoise(p3 * 8.47 + vec2<f32>(43.3, 13.7));
    let signed_value = value * 2.0 - 1.0;
    return 0.5 + 0.5 * sign(signed_value) * pow(abs(signed_value), 1.35);
}

fn micro_normal(xz: vec2<f32>, dist: f32, water_depth: f32, base_normal: vec3<f32>, fft_slope_variance: f32) -> vec3<f32> {
    let distance_fade = wave_fade(dist);
    let coast_fade = smoothstep(max(wp.coast_fade, 0.1), max(wp.coast_fade * 3.0, 0.3), water_depth);
    let steep_fade = 1.0 - smoothstep(0.10, 0.38, 1.0 - base_normal.y);
    let fft_roughness_fade = 1.0 - smoothstep(0.035, 0.20, fft_slope_variance);
    let strength = 0.050 * distance_fade * coast_fade * steep_fade * fft_roughness_fade;
    // Most helicopter/horizon pixels have already faded this shading-only detail to
    // zero. Avoid eighteen procedural-noise evaluations when their final multiplier
    // is zero (or far below a visible normal change).
    if (strength <= 1e-5) {
        return base_normal;
    }
    let p0 = xz * 0.48;
    // Domain warp breaks up the otherwise regular fBm cells without introducing a texture.
    let warp = vec2<f32>(vnoise(p0 * 0.23 + vec2<f32>(11.0, 5.0)),
                         vnoise(p0 * 0.23 + vec2<f32>(37.0, 23.0))) - vec2<f32>(0.5);
    let p = p0 + warp * 0.45;
    let e = 0.035;
    let slope = vec2<f32>(
        micro_fbm(p + vec2<f32>(e, 0.0), wp.time) - micro_fbm(p - vec2<f32>(e, 0.0), wp.time),
        micro_fbm(p + vec2<f32>(0.0, e), wp.time) - micro_fbm(p - vec2<f32>(0.0, e), wp.time)
    ) / (2.0 * e);
    return normalize(vec3<f32>(base_normal.x - slope.x * strength, base_normal.y, base_normal.z - slope.y * strength));
}

// WTR-033 — Corrected slope-variance roughness compensation.
// Adds ONLY the slope variance removed by filtering: lostVariance = sum(cascadeSlopeVariance[i] * (1 - normalWeight[i])).
fn water_roughness(spec_power: f32, lost_variance: f32, base_normal: vec3<f32>, shading_normal: vec3<f32>) -> f32 {
    let legacy_floor = sqrt(2.0 / max(spec_power + 2.0, 2.0));
    let micro_slope = length(shading_normal.xz - base_normal.xz);
    let lost_roughness = sqrt(clamp(lost_variance, 0.0, 0.25));
    return clamp(legacy_floor + micro_slope * 0.35 + lost_roughness * 0.45, 0.075, 0.45);
}

fn safe_normalize3(x: vec3<f32>, fallback: vec3<f32>) -> vec3<f32> {
    let length_sq = dot(x, x);
    return select(fallback, x * inverseSqrt(max(length_sq, 1e-8)), length_sq > 1e-8);
}

// WTR-052 — Physical Fresnel foundation.
// For water IOR eta = 1.333, physical F0 at normal incidence is ((1.333 - 1)/(1.333 + 1))^2 = 0.02037.
fn water_fresnel_f0() -> f32 {
    return 0.02037;
}

fn schlick_fresnel(f0: f32, cosine: f32) -> f32 {
    return f0 + (1.0 - f0) * pow(max(1.0 - cosine, 0.0), 5.0);
}

// Roughness-aware Fresnel. Plain Schlick drives F to 1.0 at grazing incidence, which is correct
// for a MIRROR-FLAT surface and wrong for a rough one: on a wind-roughened sea the microfacets face
// many directions, so a grazing view is not looking along a single specular lobe and the effective
// reflectance stays well below unity.
//
// This matters enormously here for a reason that is easy to miss. A standing player's eye is ~1.7 m
// up, so water 50 m away is viewed only ~2 degrees off the surface — flat Schlick returns ~84%
// reflection there, and at 15 m still ~56%. Almost every water pixel on screen is therefore mostly
// sky, and the sky is bright grey. That is why the sea washes out to grey no matter what the body
// colour is set to, even black.
//
// Capping the reflection with an arbitrary constant is what made the water read as blue plastic
// before. This instead lowers the grazing limit by the amount the surface is actually rough
// (the standard F90 = 1 - roughness construction), so calm water still goes properly mirror-like
// while a choppy sea keeps its own colour.
fn schlick_fresnel_rough(f0: f32, roughness: f32, cosine: f32) -> f32 {
    let f90 = max(1.0 - clamp(roughness, 0.0, 1.0), f0);
    return f0 + (f90 - f0) * pow(max(1.0 - cosine, 0.0), 5.0);
}

// WTR-053 — Optical refraction direction via Snell's Law (eta = 1.0 / 1.333 = 0.7502)
fn optical_refract(view_dir: vec3<f32>, normal: vec3<f32>, eta: f32) -> vec3<f32> {
    let cos_i = dot(-view_dir, normal);
    let k = 1.0 - eta * eta * (1.0 - cos_i * cos_i);
    if (k < 0.0) {
        return reflect(view_dir, normal); // Total internal reflection fallback
    }
    return eta * view_dir + (eta * cos_i - sqrt(k)) * normal;
}

// WRL-002: the surface's extinction is water_optics::water_extinction — the same function,
// the same clarity slider, the same deep swatch and the same hue bias the underwater
// compositor uses. Legacy mode keeps the old neutral curve by passing bias 0.
fn surface_extinction(hue_bias: f32) -> vec3<f32> {
    return water_extinction(wp.deep_color.rgb, wp.color_ext, hue_bias);
}

fn beer_lambert_attenuation(water_path_length: f32) -> vec3<f32> {
    return water_transmittance(surface_extinction(0.0), water_path_length);
}

fn smith_schlick_g1(ndx: f32, roughness: f32) -> f32 {
    let k = (roughness + 1.0) * (roughness + 1.0) * 0.125;
    return ndx / max(ndx * (1.0 - k) + k, 1e-4);
}

// Direct ports of GodotOceanWaves' water.gdshader light() helpers.  Keep these
// separate from the engine's physically based reflection/refraction helpers above:
// this pair defines the distinctive broad sunlight catch and turquoise crest glow
// of the reference project.
fn godot_smith_masking_shadowing(cos_theta: f32, alpha: f32) -> f32 {
    let a = cos_theta / (alpha * sqrt(max(1.0 - cos_theta * cos_theta, 1e-6)));
    let a_sq = a * a;
    if (a < 1.6) {
        return (1.0 - 1.259 * a + 0.396 * a_sq) / max(3.535 * a + 2.181 * a_sq, 1e-6);
    }
    return 0.0;
}

fn godot_ggx_distribution(cos_theta: f32, alpha: f32) -> f32 {
    let a_sq = alpha * alpha;
    let d = 1.0 + (a_sq - 1.0) * cos_theta * cos_theta;
    return a_sq / (PI * d * d);
}

fn godot_water_fresnel(cos_view_normal: f32, roughness: f32) -> f32 {
    // Exact reference expression: mix(custom grazing term, 1, REFLECTANCE=0.02).
    let grazing = pow(max(1.0 - cos_view_normal, 0.0), 5.0 * exp(-2.69 * roughness)) /
        (1.0 + 22.7 * pow(roughness, 1.5));
    return mix(grazing, 1.0, 0.02);
}

// Equirect lookup into the sky reflection env map. Matches fs_sky_env's convention in sky.wgsl:
// u = azimuth (atan2(z, x)/2pi + 0.5, U-wrapped), v = 0 at zenith .. 1 at nadir (acos(y)/pi).
// `dir` is a world-space direction. Returns linear sky radiance.
fn sky_env_sample(dir: vec3<f32>) -> vec3<f32> {
    let u = 0.5 + atan2(dir.z, dir.x) / TWO_PI;
    let v = acos(clamp(dir.y, -1.0, 1.0)) / (TWO_PI * 0.5);
    return textureSampleLevel(sky_env, sky_env_samp, vec2<f32>(u, v), 0.0).rgb;
}

fn scene_uv(frag_xy: vec2<f32>) -> vec2<f32> {
    return frag_xy / vec2<f32>(textureDimensions(scene_color));
}

struct SceneSample {
    color: vec3<f32>,
    valid: f32,
};

// WTR-053 & WTR-055 — Refracted scene lookup with strict foreground depth rejection guard.
// The distorted lookup is accepted only when opaque geometry is farther from the
// camera than the water surface. Compare reconstructed camera-relative distance,
// not nonlinear reversed-Z values, and keep validity separate from scene colour.
fn refracted_scene(uv: vec2<f32>, surface_rel: vec3<f32>) -> SceneSample {
    let dims = vec2<i32>(textureDimensions(scene_depth));
    let texel = clamp(vec2<i32>(uv * vec2<f32>(dims)), vec2<i32>(0), dims - vec2<i32>(1));
    let opaque_depth = textureLoad(scene_depth, texel, 0);
    if (opaque_depth <= 1e-6) {
        return SceneSample(vec3<f32>(0.0), 0.0);
    }
    let ndc_xy = uv * vec2<f32>(2.0, -2.0) + vec2<f32>(-1.0, 1.0);
    let opaque_h = frame.inv_view_proj * vec4<f32>(ndc_xy, 1.0 - opaque_depth, 1.0);
    let opaque_rel = opaque_h.xyz / opaque_h.w;
    let behind_water = length(opaque_rel) > length(surface_rel) + 0.05;
    return SceneSample(textureSampleLevel(scene_color, scene_samp, uv, 0.0).rgb, select(0.0, 1.0, behind_water));
}

// Trace the physically reflected view ray against the opaque depth snapshot. This deliberately
// projects every ray position instead of mirroring screen UVs: the latter is not a reflection and
// produces the old angle/distance artifact. A hit must be in front of the ray in reversed-Z and
// reconstruct close to it in camera-relative world space, rejecting foreground and depth-gap leaks.
// Off-screen/missing data falls through to the environment reflection.
fn reflected_scene(surface_rel: vec3<f32>, reflect_dir: vec3<f32>, normal_variation: f32) -> vec4<f32> {
    let dims = vec2<i32>(textureDimensions(scene_depth));
    var hit_color = vec3<f32>(0.0);
    var hit_weight = 0.0;
    var hit_distance = 0.0;

    // Twenty coarse samples keep this viable in the forward water pass. Terrain and large opaque
    // objects are the intended targets; thin geometry remains an environment-reflection fallback.
    for (var i = 0; i < 20; i = i + 1) {
        if (hit_weight == 0.0) {
            let ray_distance = 2.0 + f32(i) * 10.0;
            let ray_pos = surface_rel + reflect_dir * ray_distance;
            let clip = frame.proj * frame.view * vec4<f32>(ray_pos, 1.0);
            if (clip.w > 1e-5) {
                let ndc = clip.xyz / clip.w;
                let uv = ndc.xy * vec2<f32>(0.5, -0.5) + vec2<f32>(0.5, 0.5);
                if (all(uv > vec2<f32>(0.001)) && all(uv < vec2<f32>(0.999)) && ndc.z > 0.0 && ndc.z < 1.0) {
                    let texel = clamp(vec2<i32>(uv * vec2<f32>(dims)), vec2<i32>(0), dims - vec2<i32>(1));
                    let opaque_depth = textureLoad(scene_depth, texel, 0);
                    let ray_depth = 1.0 - ndc.z;
                    // Larger reversed depth is closer to the camera. A cleared texel has no scene
                    // intersection, and geometry behind the traced ray must not be reflected.
                    if (opaque_depth > 1e-6 && opaque_depth >= ray_depth) {
                        let opaque_h = frame.inv_view_proj * vec4<f32>(ndc.xy, 1.0 - opaque_depth, 1.0);
                        let opaque_rel = opaque_h.xyz / opaque_h.w;
                        // The opaque snapshot contains first-person hands/weapons on some
                        // draw paths.  They are foreground presentation geometry, never
                        // world objects that can plausibly appear in an ocean reflection.
                        // Reject that near-camera band before accepting an SSR hit.
                        if (length(opaque_rel) < 12.0) { continue; }
                        let thickness = 1.5 + ray_distance * 0.025;
                        if (length(opaque_rel - ray_pos) <= thickness) {
                            let edge = min(min(uv.x, uv.y), min(1.0 - uv.x, 1.0 - uv.y));
                            hit_color = textureSampleLevel(scene_color, scene_samp, uv, 0.0).rgb;
                            hit_weight = smoothstep(0.01, 0.07, edge);
                            hit_distance = ray_distance;
                        }
                    }
                }
            }
        }
    }

    // Rapid normal changes are a cheap water-roughness proxy: blur-free SSR is unstable on crests,
    // so leave those pixels to the filtered sky environment instead.
    let roughness_fade = 1.0 - smoothstep(0.08, 0.30, normal_variation);
    let distance_fade = 1.0 - smoothstep(120.0, 192.0, hit_distance);
    return vec4<f32>(hit_color, hit_weight * roughness_fade * distance_fade);
}

// Project a point through the reflected camera. This is intentionally not a flipped
// main-screen UV: parallax follows the reflected camera.
fn planar_project(mirrored: vec3<f32>) -> vec3<f32> {
    let clip = planar.full_vp * vec4<f32>(mirrored, 1.0);
    if (clip.w <= 1e-5) { return vec3<f32>(0.0); }
    return vec3<f32>(clip.xy / clip.w * vec2<f32>(0.5, -0.5) + vec2<f32>(0.5), 1.0);
}

// Project the stable mean-water plane through the mirrored camera. The reflected
// camera already mirrors the world; wave slope perturbs this plane lookup below.
// Mirroring the already displaced surface point makes cloud reflections slide as the
// camera pitches.
fn planar_reflection(surface_rel: vec3<f32>, surface_normal: vec3<f32>, roughness: f32) -> vec4<f32> {
    // valid.x is a 0..1 FADE, not a flag: the coverage gate in lib.rs dissolves the planar
    // contribution to zero before it stops rendering the target, so the handover to the
    // environment reflection completes while the target is still being updated. Testing it as a
    // boolean would put a hard switch exactly where a shoreline crosses the horizon.
    if (planar.valid.x <= 0.0) { return vec4<f32>(0.0); }
    let absolute = surface_rel + frame.cam_pos.xyz;
    let plane_point = vec3<f32>(absolute.x, wp.sea_level, absolute.z);
    let projection = planar_project(plane_point);
    if (projection.z < 0.5) { return vec4<f32>(0.0); }
    let uv = projection.xy;
    let texel = 1.0 / vec2<f32>(textureDimensions(planar_color));
    // The reflected camera already accounts for the world-space planar parallax.
    // Do not add a normal-projected UV warp here: it makes cloud features crawl over
    // a fixed water point as the player pitches. Surface roughness is handled by the
    // filtered mip chain below instead.
    let distorted_uv = clamp(uv, texel, vec2<f32>(1.0) - texel);
    // How far OUTSIDE the target this lookup wanted to go. Fading on the INSIDE distance was the
    // mistake: it threw away good reflection over a band of water that had perfectly valid data,
    // and still ended in a boundary because the two sources differ. Measuring the overshoot
    // instead means every pixel the planar target actually covers keeps its full reflection, and
    // only the pixels beyond it are treated.
    let over = max(vec2<f32>(0.0), max(texel - uv, uv - (vec2<f32>(1.0) - texel)));
    let outside = length(over);
    // Hand over to the sky/environment reflection across a WIDE band, not a hard rim.
    //
    // Some water simply cannot be covered by a planar reflection: tilt down and the water directly
    // beneath you maps to a point directly ABOVE the mirrored camera, which is looking up and
    // forward -- outside its frustum at any sane field of view. Widening the reflected FOV does not
    // reach that case, so the fade is what has to carry it.
    //
    // At the old 3% the two sources swapped over almost instantly, and because planar carries
    // parallax-correct clouds while the environment sample is a distant approximation, the swap
    // read as a line drawn across the sea. Spreading it over a large fraction of the target makes
    // the same handover invisible -- the reflection loses parallax gradually instead of ending.
    let fade_band = clamp(wp.underwater_gate.w, 0.02, 0.49);
    // Beyond the target, keep sampling the CLAMPED edge texels and blur them progressively into
    // an average instead of dropping the reflection. Clamping alone would smear recognisable
    // detail into streaks; blurring toward the top of the mip chain turns it into a plausible
    // continuation of the sky above it, which for a cloud reflection is what the eye accepts.
    let outside_t = clamp(outside / fade_band, 0.0, 1.0);
    // Still fade, but over twice the band and starting only once we are outside -- so the handover
    // is a long dissolve at the far edge rather than a boundary drawn across usable water.
    let valid = 1.0 - smoothstep(0.0, fade_band * 2.0, outside);
    // The planar target is a real filtered mip pyramid. Keep a small minimum
    // footprint even on calm water: otherwise the cloud layer reads like a second,
    // unnaturally sharp sky painted on the sea. Surface roughness still broadens it
    // strongly for foam and windier conditions.
    let max_mip = f32(textureNumLevels(planar_color) - 1u);
    let reflection_lod = (0.14 + 0.86 * roughness * roughness) * max_mip;
    // Escalate toward the coarsest mip as the lookup leaves the target, so the smear dissolves.
    let lod = min(max_mip, mix(reflection_lod, max_mip, outside_t));
    let color = textureSampleLevel(planar_color, planar_samp, distorted_uv, lod).rgb;
    // Two independent fades multiply: `valid` handles the lookup running off the edge of the
    // target, planar.valid.x handles the coverage gate dissolving the whole source out.
    return vec4<f32>(color, valid * planar.valid.x);
}

// WTR-003 — water debug views. When wp.debug_params.x is non-zero the fragment shader
// replaces its lit output with the selected diagnostic (see WgrWaterDebugView). Views are
// aggregated over the four FFT cascades; the interaction/foam fields read zero outside the
// 256 m camera domain. Views whose backing pass does not exist yet (underwater froxel /
// in-scattering, god rays, caustics, whitewater) fall through to black.
fn dbg_heat(v: f32, scale: f32) -> vec3<f32> {
    let t = clamp(v / max(scale, 1e-5), 0.0, 1.0);
    return vec3<f32>(t, 0.25 + 0.5 * t, 1.0 - t);
}
fn dbg_signed(v: f32, scale: f32) -> vec3<f32> {
    let t = clamp(abs(v) / max(scale, 1e-5), 0.0, 1.0);
    return select(vec3<f32>(0.25, 0.55, 1.0) * t, vec3<f32>(1.0, 0.45, 0.20) * t, v >= 0.0);
}
fn debug_view(view: i32, base_xz: vec2<f32>, world_rel: vec3<f32>, water_depth: f32,
    fft_disp: vec3<f32>, fft_horiz: f32, fft_vert: f32, fft_slope: f32, fft_j: f32,
    fft_comp: f32, fft_curv: f32, fft_crest: f32, fft_var: f32, interaction: vec4<f32>,
    foam_hist: vec4<f32>, foam_src: f32, sky_refl: vec3<f32>, ssr: vec4<f32>,
    planar_refl: vec4<f32>, refl: vec3<f32>, refract_uv: vec2<f32>, base_uv: vec2<f32>,
    refracted: SceneSample, transmission: f32) -> vec4<f32> {
    let dist = length(world_rel);
    var c = vec3<f32>(0.0);
    switch view {
        case 1: { c = dbg_heat(length(fft_disp), 1.5); }
        case 2: { c = dbg_heat(fft_horiz, 1.0); }
        case 3: { c = dbg_signed(fft_vert, 1.0); }
        case 4: { c = dbg_heat(fft_slope, 0.5); }
        case 5: { c = dbg_heat(fft_j, 1.0); }
        case 6: { c = dbg_heat(fft_comp, 0.5); }
        case 7: { c = dbg_heat(fft_curv, 0.1); }
        case 8: { c = dbg_heat(fft_crest, 0.2); }
        case 9: { c = dbg_heat(fft_var, 0.2); }
        case 10: { c = vec3<f32>(fract(base_xz * 0.01), 0.35); }
        case 11: { let w = base_xz + world_rel.xz; c = vec3<f32>(fract(w * 0.01), 0.35); }
        case 12: { c = dbg_signed(interaction.r, 0.5); }
        case 13: { c = dbg_signed(interaction.g, 1.0); }
        case 14: { c = dbg_heat(interaction.b, 1.0); }
        case 15: { c = dbg_heat(foam_src, 1.0); }
        case 16: { c = dbg_heat(foam_hist.r, 1.0); }
        case 17: { let vel = interaction.g * 0.5 + 0.5; c = vec3<f32>(vel, vel, 0.3); }
        case 18: { c = dbg_heat(min(water_depth, 60.0), 60.0); }
        case 19: { c = dbg_heat(dist, 1000.0); }
        case 20: { c = ssr.rgb; }
        case 21: { c = vec3<f32>(ssr.a); }
        case 22: { c = planar_refl.rgb; }
        case 23: { c = vec3<f32>(planar_refl.a); }
        case 24: { c = sky_refl; }
        case 25: {
            c = vec3<f32>(0.10);
            if (ssr.a > 0.02) { c = vec3<f32>(1.0, 0.30, 0.20); }
            else if (planar_refl.a > 0.02) { c = vec3<f32>(0.25, 0.75, 1.0); }
            else { c = vec3<f32>(0.30, 1.0, 0.40); }
        }
        case 26: {
            let dims = vec2<f32>(textureDimensions(scene_color));
            c = vec3<f32>((refract_uv - base_uv) * dims / 32.0 + 0.5, 0.4);
        }
        case 27: { c = vec3<f32>(refracted.valid); }
        case 28: { c = dbg_heat(min(water_depth, 40.0), 40.0); }
        case 29: { c = transmission * vec3<f32>(1.4); }
        // The interaction field is (height, velocity, foam, unused) — see the textureStore in
        // interaction.wgsl. `.x` is HEIGHT, not a velocity component, which is what both of
        // these views used to get wrong.
        case 37: { // WTR-012 Interaction velocity
            c = dbg_heat(abs(interaction.g), 2.0);
        }
        case 38: { // WTR-012 Interaction height
            // Was |velocity * 0.0333| under the label "previous displacement delta". No
            // previous displacement is stored anywhere — the field's fourth channel is
            // written as 0 — so that view was velocity again on a different scale, and the
            // height channel that drives the actual surface offset had no view at all.
            c = dbg_heat(abs(interaction.r), 0.5);
        }
        case 39: { // WTR-040 Directional sky
            c = sky_refl;
        }
        case 40: { // WTR-040 Directional clouds
            c = sky_refl * vec3<f32>(1.2, 1.1, 0.9);
        }
        case 41: { // WTR-040 Planar sky
            c = select(vec3<f32>(0.0), planar_refl.rgb, planar_refl.a > 0.02);
        }
        case 42: { // WTR-040 Planar clouds
            c = select(vec3<f32>(0.0), planar_refl.rgb * vec3<f32>(1.1, 1.1, 1.2), planar_refl.a > 0.02);
        }
        case 43: { // WTR-040 Planar terrain/objects
            c = select(vec3<f32>(0.0), planar_refl.rgb * vec3<f32>(0.8, 0.9, 0.7), planar_refl.a > 0.02);
        }
        case 44: { // WTR-040 Planar geometry validity
            c = vec3<f32>(planar_refl.a);
        }
        case 45: { // WTR-040 SSR
            c = ssr.rgb;
        }
        case 46: { // WTR-040 Final reflection owner badge
            if (ssr.a > 0.02) { c = vec3<f32>(1.0, 0.20, 0.20); } // SSR (Red)
            else if (planar_refl.a > 0.02) { c = vec3<f32>(0.20, 0.60, 1.0); } // Planar (Blue)
            else { c = vec3<f32>(0.20, 0.90, 0.30); } // Directional Sky (Green)
        }
        default: { c = vec3<f32>(0.0); }
    }
    return vec4<f32>(c, 1.0);
}

// WTR-011 / WTR-012 — Shared water-surface state representation & evaluation
struct WaterSurfaceState {
    surf: SurfState,
    material_position: vec2<f32>,
    world_pos: vec3<f32>,
    displaced_pos: vec3<f32>,
    previous_displaced_pos: vec3<f32>,
    displacement: vec3<f32>,
    velocity: vec3<f32>,
    geometric_normal: vec3<f32>,
    shading_normal: vec3<f32>,
    jacobian: f32,
    compression: f32,
    curvature: f32,
    slope_variance: f32,
    // Portion of slope_variance the shading normal does not represent.
    // water_roughness consumes this, not the total -- see the accumulator
    // in evaluate_water_surface.
    slope_variance_lost: f32,
    crest_energy: f32,
    breaking_energy: f32,
    interaction_height: f32,
    interaction_velocity: f32,
    aeration: f32,
    foam_density: f32,
    foam_history_sample: vec4<f32>,
};

fn evaluate_water_surface(in: VsOut) -> WaterSurfaceState {
    var state: WaterSurfaceState;
    if (surf_pilot > 0.5 && in.body_id < 0.5) { state.surf = pilot_surf(in.base_xz); }
    state.material_position = in.base_xz;
    state.world_pos = in.world_pos;
    state.displaced_pos = in.world_pos;
    
    let dist = length(in.world_pos);
    var n = gerstner_normal(in.base_xz, dist);
    var fft_slope_var = 0.0;
    // Slope variance the shading normal does NOT represent.
    //
    // fft_normal_with_weights accumulates each cascade's slope scaled by
    // normal_weight, so (1 - normal_weight) is exactly the fraction left out of
    // the normal. water_roughness wants that residual: it already adds the
    // represented slope separately via micro_slope, and feeding it the total
    // counted the resolved part twice and made water read rougher than the
    // spectrum implies.
    //
    // Kept as a second accumulator rather than redefining fft_slope_var, which
    // three other consumers use as a measure of total wave activity
    // (micro_normal's detail fade, open_ocean_activity, and debug view 4).
    var fft_slope_var_lost = 0.0;
    var fft_crest = 0.0;
    var fft_comp = 0.0;
    var fft_curv = 0.0;
    
    if (wp.fft_control.x > 0.5) {
        let view_dir = normalize(in.world_pos);
        n = fft_normal_with_weights(in.base_xz, dist, view_dir);
        for (var layer = 0; layer < 4; layer = layer + 1) {
            let raw_length = wp.fft_cascade_lengths[layer];
            // The reference preset deliberately leaves layer 3 disabled. It used to
            // pay two texture fetches plus projected-weight math here before multiplying
            // nothing by nothing. Disabled layers contain no surface diagnostics.
            if (raw_length > 0.0) {
                let uv = fft_scaled_uv(in.base_xz, raw_length, layer, wp.warp_amp, wp.wave_scale);
                let aux = textureSampleLevel(fft_auxiliary, fft_samp, uv, layer, 0.0);
                fft_slope_var = fft_slope_var + aux.w;
                let lost_w = 1.0 - clamp(compute_cascade_weights(layer, dist, view_dir).normal_weight, 0.0, 1.0);
                fft_slope_var_lost = fft_slope_var_lost + aux.w * lost_w;
                fft_crest = max(fft_crest, textureSampleLevel(fft_displacement, fft_samp, uv, layer, 0.0).w);
                fft_comp = max(fft_comp, aux.y);
                fft_curv = max(fft_curv, aux.z / max(wp.wave_scale, 0.01));
            }
        }
    }
    
    // FFT normals replace the offshore Gerstner normal above; add the same local
    // shoreward component after that replacement so geometry and lighting agree.
    var shore_n = shore_breaker_normal(in.base_xz, in.shore_dir, in.shore_factor);
    if (surf_pilot > 0.5 && in.body_id < 0.5) {
        let surf = state.surf;
        n = normalize(n + vec3<f32>(-surf.slope.x, 0.0, -surf.slope.y) * n.y);
        shore_n = vec3<f32>(0.0, 1.0, 0.0);
    }
    n = normalize(n + shore_n - vec3<f32>(0.0, 1.0, 0.0));
    let interaction_texel = interaction_sample(in.base_xz);
    state.interaction_height = interaction_texel.r;
    state.interaction_velocity = interaction_texel.g;
    state.aeration = interaction_texel.b;
    
    // Multi-scale central differences for crisp, physical capillary-gravity wave train normals
    let cell_fine = 0.20;
    let cell_wide = 0.80;
    let h_l_f = interaction_sample(in.base_xz - vec2<f32>(cell_fine, 0.0)).r;
    let h_r_f = interaction_sample(in.base_xz + vec2<f32>(cell_fine, 0.0)).r;
    let h_d_f = interaction_sample(in.base_xz - vec2<f32>(0.0, cell_fine)).r;
    let h_u_f = interaction_sample(in.base_xz + vec2<f32>(0.0, cell_fine)).r;

    let h_l_w = interaction_sample(in.base_xz - vec2<f32>(cell_wide, 0.0)).r;
    let h_r_w = interaction_sample(in.base_xz + vec2<f32>(cell_wide, 0.0)).r;
    let h_d_w = interaction_sample(in.base_xz - vec2<f32>(0.0, cell_wide)).r;
    let h_u_w = interaction_sample(in.base_xz + vec2<f32>(0.0, cell_wide)).r;

    let slope_fine = vec2<f32>(h_l_f - h_r_f, h_d_f - h_u_f) * 8.5;
    let slope_wide = vec2<f32>(h_l_w - h_r_w, h_d_w - h_u_w) * 3.2;
    let combined_slope = slope_fine + slope_wide;
    // Add height gradients, not normals: residual interaction height must never
    // replace the FFT/shore waves with an almost-flat ripple-only surface.
    // Multiplying by n.y adds slopes without dividing by a near-horizontal normal.
    n = normalize(n + vec3<f32>(combined_slope.x, 0.0, combined_slope.y) * n.y * 0.95);
    
    let flow_speed = max(wp.flow_direction_speed.z, 0.0);
    let flow_dir = normalize(wp.flow_direction_speed.xy + vec2<f32>(1e-4, 0.0)) * flow_speed;
    state.velocity = vec3<f32>(flow_dir.x, state.interaction_velocity, flow_dir.y);
    state.previous_displaced_pos = in.world_pos - state.velocity * 0.0333;
    state.displacement = in.world_pos - vec3<f32>(in.base_xz.x - frame.cam_pos.x, 0.0, in.base_xz.y - frame.cam_pos.z);
    state.geometric_normal = n;
    
    let water_depth = seabed_depth(in.clip.xy, in.world_pos);
    let shading_n = micro_normal(in.base_xz, dist, water_depth, n, fft_slope_var);
    state.shading_normal = shading_n;
    
    state.jacobian = 1.0;
    state.compression = fft_comp;
    state.curvature = fft_curv;
    state.slope_variance = fft_slope_var;
    state.slope_variance_lost = fft_slope_var_lost;
    state.crest_energy = fft_crest;
    // Foam history is camera-domain anchored, not world-origin anchored.  Sampling it
    // through the same mapping used by the material path keeps the state/debug data
    // aligned with visible wake and whitecap foam after the camera has moved.
    let foam_sample = persistent_foam_sample(in.base_xz);
    state.foam_history_sample = foam_sample;
    state.foam_density = clamp(foam_sample.r + foam_sample.g * 1.25 + foam_sample.b * 0.75, 0.0, 1.0);
    state.aeration = max(state.aeration, foam_sample.b);
    
    return state;
}

@fragment
fn fs_water(in: VsOut) -> @location(0) vec4<f32> {
    // Receiver-plane derivatives for the CSM bias must be taken in uniform control flow.
    let dwx = dpdx(in.world_pos);
    let dwy = dpdy(in.world_pos);
    // World metres covered by one pixel on the water plane — the Nyquist limit for every
    // procedural foam octave below. Taken HERE, in uniform control flow, because each foam term is
    // behind a coverage branch and a derivative inside non-uniform flow is undefined. It grows
    // without bound toward the horizon, which is exactly the behaviour the band limiting wants:
    // grazing pixels integrate a huge patch of sea and must return that patch's average.
    let foam_footprint = max(length(dpdx(in.base_xz)), length(dpdy(in.base_xz)));
    // Legacy A/B: a zero footprint disables every band limit, so vnoise_lod is the identity and
    // foam_noise_lod reduces to foam_noise's original sharp smoothstep(0.32, 0.64).
    let foam_fp = select(foam_footprint, 0.0, shore_foam_legacy > 0.5);

    // WRL-003: a bounded body ends at its containment ellipse, with a short soft rim; the ocean
    // plane never does. Discarding here (after the derivatives above) keeps everything below
    // uniform for the fragments that survive.
    let body_index = i32(in.body_id + 0.5);
    var body_coverage = 1.0;
    var body_profile = -1;
    var body_edge = 0.0; // normalised ellipse radius^2 at this fragment (1 = the bank)
    var body_kind = 0;
    var body_flow = vec2<f32>(0.0);
    var body_flow_speed = 0.0;
    if (body_index > 0) {
        let e = wp.bodies[3 * (body_index - 1)];
        let k = wp.bodies[3 * (body_index - 1) + 1];
        let f = wp.bodies[3 * (body_index - 1) + 2];
        let world_xz = in.world_pos.xz + frame.cam_pos.xz;
        // The ellipse frame follows the body's heading (a river reach is a rotated ellipse).
        let fc = cos(f.w);
        let fs = sin(f.w);
        let dxz = world_xz - e.xy;
        let u = (fc * dxz.x + fs * dxz.y) / max(e.z, 1e-3);
        let v = (-fs * dxz.x + fc * dxz.y) / max(e.w, 1e-3);
        let d = u * u + v * v;
        body_kind = i32(k.z + 0.5) / 8;
        body_flow = normalize(f.xy + vec2<f32>(1e-4, 0.0));
        body_flow_speed = max(f.z, 0.0);
        if (d > 1.06) {
            discard;
        }
        body_coverage = 1.0 - smoothstep(0.94, 1.03, d);
        body_edge = d;
        // Connected mouth: where a reach's surface comes down to the sea, the sea owns the
        // pixels. The river fades out over its last 0.6 m of fall so the two surfaces never
        // composite twice and the junction has no step.
        if (body_kind == 2) {
            // Below the sea datum there is no river surface at all: the sea is drawn there and
            // two surfaces a few centimetres apart z-fight. Discard, not fade.
            if (in.body_level <= wp.sea_level) {
                discard;
            }
            body_coverage = body_coverage * smoothstep(0.0, 0.6, in.body_level - wp.sea_level);
        }
        body_profile = i32(k.z + 0.5) % 8;
    }

    let interaction_cell = 1.0;
    let h_l = interaction_sample(in.base_xz - vec2<f32>(interaction_cell, 0.0)).r;
    let h_r = interaction_sample(in.base_xz + vec2<f32>(interaction_cell, 0.0)).r;
    let h_d = interaction_sample(in.base_xz - vec2<f32>(0.0, interaction_cell)).r;
    let h_u = interaction_sample(in.base_xz + vec2<f32>(0.0, interaction_cell)).r;
    let state = evaluate_water_surface(in);
    var n = state.shading_normal;
    // WRL-004: a river's surface detail is carried by its flow. Two advection phases half a
    // period apart, cross-faded, so the pattern travels without the reset pulse a single
    // scrolling phase produces; ripples are stretched along the flow (streaky), and the
    // amplitude follows the flow speed and eases at the bank with the flow itself.
    if (body_kind == 2 && body_flow_speed > 0.01) {
        let along = body_flow;
        let across = vec2<f32>(-along.y, along.x);
        let speed = body_flow_speed * mix(1.0, 0.15, smoothstep(0.55, 1.0, body_edge));
        let period = 4.0;
        let t1 = fract(wp.time / period);
        let t2 = fract(wp.time / period + 0.5);
        let w1 = 1.0 - abs(2.0 * t1 - 1.0);
        let w2 = 1.0 - abs(2.0 * t2 - 1.0);
        let local = vec2<f32>(dot(in.base_xz, along) * 0.35, dot(in.base_xz, across) * 1.6);
        let p1 = local - vec2<f32>(speed * t1 * period * 0.35, 0.0);
        let p2 = local - vec2<f32>(speed * t2 * period * 0.35, 0.0) + vec2<f32>(17.3, 9.1);
        let e = 0.05;
        let s1 = vec2<f32>(micro_fbm(p1 + vec2<f32>(e, 0.0), 0.0) - micro_fbm(p1 - vec2<f32>(e, 0.0), 0.0),
                           micro_fbm(p1 + vec2<f32>(0.0, e), 0.0) - micro_fbm(p1 - vec2<f32>(0.0, e), 0.0)) / (2.0 * e);
        let s2 = vec2<f32>(micro_fbm(p2 + vec2<f32>(e, 0.0), 0.0) - micro_fbm(p2 - vec2<f32>(e, 0.0), 0.0),
                           micro_fbm(p2 + vec2<f32>(0.0, e), 0.0) - micro_fbm(p2 - vec2<f32>(0.0, e), 0.0)) / (2.0 * e);
        let slope_local = (s1 * w1 + s2 * w2) / max(w1 + w2, 1e-3);
        let slope_world = along * slope_local.x * 0.35 + across * slope_local.y * 1.6;
        let strength = 0.03 * clamp(speed / 1.5, 0.2, 1.5) * wave_fade(length(in.world_pos));
        n = normalize(vec3<f32>(n.x - slope_world.x * strength, n.y, n.z - slope_world.y * strength));
    }
    let base_normal = state.geometric_normal;
    let roughness = water_roughness(wp.spec_power, state.slope_variance_lost, base_normal, n);
    // Filter sub-pixel glitter analytically, without extra texture samples.
    // Cascade filtering alone misses normal variation still present within a pixel.
    let normal_dx = dpdx(n);
    let normal_dy = dpdy(n);
    let pixel_normal_variance = 0.5 * (dot(normal_dx, normal_dx) + dot(normal_dy, normal_dy));
    let v = normalize(-in.world_pos);              // surface -> camera
    let l = normalize(-frame.sun_dir_world.xyz);   // surface -> sun
    let world_y = in.world_pos.y + frame.cam_pos.y;
    let csm_s = shadow_strength(in.world_pos, n, in.fog, dwx, dwy);
    let ter_raw = terrain_sun_shadow(in.base_xz, world_y);
    let ter_s = ter_raw * in.fog;
    let sun_shadow = max(csm_s, ter_s);
    let sun_up = smoothstep(0.0, 0.06, l.y);
    // CLD-020: clouds dim the water''"'"'s direct sun too, or the sea stays brilliant under an
    // overcast that visibly darkens the shore beside it.
    let sun_vis = (1.0 - sun_shadow) * sun_up * cloud_sun_shadow(in.base_xz);
    var sun_diffuse = frame.sun_diffuse.rgb;
    var sun_ambient = frame.sun_ambient.rgb;
    var fog_color = frame.fog_color.rgb;
    let sky_lit = frame.sun_diffuse.w > 0.5;
    if (linear > 0.5) {
        if (!sky_lit) {
            sun_diffuse = srgb_to_linear(sun_diffuse);
            sun_ambient = srgb_to_linear(sun_ambient);
        }
        fog_color = srgb_to_linear(fog_color);
    }
    let water_depth = seabed_depth(in.clip.xy, in.world_pos);
    var shallow = wp.shallow_color.rgb;
    var deep = wp.deep_color.rgb;
    if (linear > 0.5) {
        shallow = srgb_to_linear(shallow);
        deep = srgb_to_linear(deep);
    }
    // Coastal Turquoise Cyan vs Deep Ocean Navy Blue color palette:
    // Shallow: Vibrant Turquoise Cyan (Linear = [0.005, 0.28, 0.38])
    // Deep: Rich Navy Blue (Linear = [0.001, 0.022, 0.14])
    let shallow_preset = vec3<f32>(0.005, 0.28, 0.38);
    let deep_preset = vec3<f32>(0.001, 0.022, 0.14);
    // The "was a colour authored?" test used a 0.01 length threshold. In LINEAR space that is not a
    // near-zero epsilon at all — it is brighter than any plausible deep-ocean body colour. A deep
    // blue of (0.002, 0.011, 0.036) in gamma decodes to about (0.0002, 0.0009, 0.0028) linear, whose
    // length is ~0.003, so the guard declared it "unset" and silently substituted the much brighter
    // hardcoded preset below. That is why every attempt to darken the deep water did nothing: the
    // authored value was being thrown away precisely because it was dark. 1e-6 is an actual epsilon.
    let shallow_col = select(shallow, shallow_preset, length(shallow) <= 1e-6);
    let deep_col = select(deep, deep_preset, length(deep) <= 1e-6);

    // The old ramp floored the extinction at 0.15/m, which saturated to the deep colour by ~20 m
    // of column depth no matter where the clarity slider sat — so a bay and the open ocean were
    // the same navy and the turquoise only survived in the last few metres at the waterline.
    // Letting color_ext actually control the ramp gives a gradient that reads out to ~60 m, which
    // is the coast-turquoise -> offshore-blue transition you actually see over a real shelf.
    let depth_tint = 1.0 - exp(-water_depth * max(wp.color_ext, 0.004));
    // Deep water also darkens, not just shifts hue: absorption removes total radiance, so the far
    // body is a deeper, denser blue rather than a lighter navy tint of the same brightness. The
    // falloff is deliberately front-loaded (sqrt rather than squared) so the surface goes dark
    // quickly as the bottom drops away, instead of staying bright until it is already far out.
    // Mild. Stacked on top of an already-dark deep colour this was crushing the body to black,
    // leaving only the reflection visible — the deep colour itself now carries the darkening.
    let deep_darkening = mix(1.0, 0.80, sqrt(depth_tint));
    let ocean_body = mix(shallow_col, deep_col, depth_tint) * deep_darkening;

    let amb_ao = sky_vis_ao(in.base_xz);
    let ndl = max(dot(n, l), 0.0);
    // Direct sun is added below with the GodotOceanWaves light() port.  Leave the
    // volume/body term ambient-lit here so it is not double-counted.
    var rgb = ocean_body * (sun_ambient * amb_ao);

    // Fresnel toward the horizon/sky tint: near-grazing water lightens and reads reflective.
    let ndv = max(dot(n, v), 0.0);
    let f0 = 0.035;
    let fresnel = schlick_fresnel(f0, ndv);

    var refl: vec3<f32>;
    if (linear > 0.5) {
        let refl_dir = reflect(normalize(in.world_pos), n);
        var sky_refl = sky_env_sample(refl_dir);
        let toward_sun = smoothstep(0.0, 0.4, dot(refl_dir, l));
        sky_refl = mix(sky_refl, sun_ambient, ter_raw * toward_sun);
        refl = sky_refl;
    } else {
        refl = mix(sun_ambient, fog_color, sun_up);
    }
    let refl_dir = reflect(normalize(in.world_pos), n);
    let normal_variation = length(dpdx(n)) + length(dpdy(n));
    // WTR-LOOK — low water quality drops the two screen-space reflection sources, which are the
    // dominant fragment cost here (SSR marches the depth buffer, planar samples a second render
    // of the scene). The sky/environment reflection below is a single texture fetch and stays, so
    // the water is still reflective — it just loses parallax-correct scene reflections.
    let low_quality = wp.sea_params.z > 0.5;
    var ssr = vec4<f32>(0.0);
    var planar_refl = vec4<f32>(0.0);
    if (!low_quality) {
        // SSR is the expensive 20-step depth march. It is valuable around the camera,
        // where nearby terrain/boats need parallax, but distant ocean already has the
        // reflected-camera result and previously paid this march for almost no accepted
        // hits. Fade ownership smoothly to planar before skipping the march entirely.
        let ssr_distance_weight = 1.0 - smoothstep(180.0, 320.0, length(in.world_pos));
        if (ssr_distance_weight > 0.002) {
            ssr = reflected_scene(in.world_pos, refl_dir, normal_variation);
            ssr.a = ssr.a * ssr_distance_weight;
        }
        // A bounded body at another elevation must never sample the SEA's planar transform as
        // if it were its own (plan section 5, WRL-005); it keeps the sky/environment sample.
        if (body_index == 0) {
            planar_refl = planar_reflection(in.world_pos, base_normal, roughness);
        }
    }
    refl = mix(refl, ssr.rgb, ssr.a);
    // Retain stable planar parallax, but keep its cloud layer deliberately softer
    // and less dominant than the sky/environment reflection.
    refl = mix(refl, planar_refl.rgb, planar_refl.a * 0.68 * (1.0 - ssr.a * 0.80));
    let uv = scene_uv(in.clip.xy);

    // WTR-051 / WTR-052 / WTR-053 / WTR-056 — Physical Fresnel, Snell's law refraction & RGB extinction
    const WATER_IOR: f32 = 1.333;
    let physical_f0 = water_fresnel_f0();
    let view_dir = normalize(in.world_pos); // camera -> surface
    let refracted_dir = optical_refract(view_dir, n, 1.0 / WATER_IOR);
    let path_length = water_depth / max(abs(refracted_dir.y), 0.1);
    let refract_offset = (refracted_dir.xz - view_dir.xz) * clamp(water_depth * 0.12, 0.005, 0.45);
    let refract_uv = clamp(uv + refract_offset, vec2<f32>(0.001), vec2<f32>(0.999));
    let refracted = refracted_scene(refract_uv, in.world_pos);

    // RGB Beer-Lambert transmission (red light absorbed fastest in water)
    let rgb_transmittance = beer_lambert_attenuation(path_length);
    // Roughness-aware: see schlick_fresnel_rough. `roughness` here is the variance-filtered value,
    // so it already grows with distance as cascade filtering removes slope detail — which means the
    // far sea (viewed at the most grazing angles) is exactly where the correction is strongest.
    let physical_fresnel = schlick_fresnel_rough(physical_f0, roughness, ndv);
    let transmission = (1.0 - physical_fresnel) * 0.85;
    let legacy_transmitted = mix(ocean_body * (1.0 - rgb_transmittance * 0.4) + rgb * rgb_transmittance, refracted.color * rgb_transmittance, refracted.valid * transmission);

    // WTR-LOOK / WRL-002 — physical body radiance through the shared optical model.
    //
    // Three things used to attenuate the seabed here, each with its own constant: a
    // hard-coded red-absorbing Beer-Lambert curve, a scalar "seabed visibility" falloff
    // (a scattering term that was never called one), and an authored shallow->deep lerp
    // with a sqrt darkening on top. The underwater compositor derived a fourth from the
    // deep swatch. water_optics now owns all of it:
    //   sigma_t       = absorption(deep swatch hue, clarity) + scattering(clarity)
    //   T             = exp(-sigma_t * path)               transmitted seabed image
    //   L_body        = albedo * E / PI * (1 - T)           single-scatter glow, 0 at zero path
    //   result        = background * T + L_body
    // and the SAME sigma_t is what the compositor fogs with when the eye goes under, so the
    // water seen from above and from inside is one liquid.
    //
    // `E` is the irradiance at the surface (ambient with AO, plus the sun's cosine term
    // less what Fresnel reflects). That is the single-scatter closed form for a slab lit
    // from above; light is not re-integrated along the path, which is the documented
    // approximation for this stage (section 3.1 of the plan).
    //
    // The 1/PI is inside water_inscatter: outgoing radiance from a Lambertian scatterer is
    // albedo * irradiance / PI, the same normalisation the foam material below uses.
    let hue_bias = clamp(wp.underwater_params.z, 0.0, 1.0);
    var sigma_t = surface_extinction(hue_bias);
    // WRL-003 / WRL-004: per-body profiles as conservative multipliers on the shared model
    // (the plan's clear lake, turbid lake, slow river, fast shallow river). Authoring data is
    // absent, so these are modest, and the ocean (profile -1) is untouched.
    var body_shallow = shallow_col;
    var body_deep = deep_col;
    if (body_profile == 0) {
        sigma_t = sigma_t * 0.6; // clear lake: longer visibility, the bed shows
    } else if (body_profile == 1 || body_profile == 3) {
        sigma_t = sigma_t * 3.0; // turbid lake / fast shallow river: short, brown-green
        let murk = vec3<f32>(0.16, 0.17, 0.10);
        body_deep = select(murk, srgb_to_linear(murk), linear > 0.5);
        body_shallow = mix(body_shallow, body_deep, 0.5);
    } else if (body_profile == 2) {
        sigma_t = sigma_t * 1.6; // slow river: a little greener and shorter than the sea
        let river = vec3<f32>(0.05, 0.14, 0.13);
        body_deep = select(river, srgb_to_linear(river), linear > 0.5);
    }
    // Opacity, physical meaning: 1.0 is the model itself; lowering it lets MORE of the
    // background through (T lifted toward 1). It used to be a framebuffer alpha that
    // blended the background a second time over a colour that already contained it.
    let transmittance = mix(water_transmittance(sigma_t, path_length), vec3<f32>(1.0),
        1.0 - clamp(wp.alpha, 0.0, 1.0));
    let body_light = sun_ambient * amb_ao + sun_diffuse * ndl * sun_vis * (1.0 - physical_fresnel);
    let body_albedo = water_albedo_along_path(body_shallow, body_deep, sigma_t, path_length);
    let body_radiance = water_inscatter(body_albedo, body_light, transmittance);
    // The background the water transmits: the refracted sample when its depth test says
    // it is genuinely behind the surface, otherwise this pixel's own opaque sample, which
    // the depth test already guarantees is behind the water. Either way the shader now
    // ALWAYS composites the background itself, so the physical path returns full coverage
    // below and the framebuffer blend cannot add the background a second time.
    let own_background = textureSampleLevel(scene_color, scene_samp, uv, 0.0).rgb;
    let background = select(own_background, refracted.color, refracted.valid > 0.5);
    let shared_optics_transmitted = background * transmittance + body_radiance;
    // The composite this replaced, kept selectable (Water tab "Shared optical model" off) so
    // the two can be A/B'd on the same frame. Note it composites the background only where
    // the refracted sample was valid; that is why its alpha below still had to blend.
    let prev_body_radiance = ocean_body * (sun_ambient * amb_ao + sun_diffuse * ndl * sun_vis * (1.0 - physical_fresnel)) / PI;
    let prev_seabed_visibility = exp(-water_depth * max(wp.color_ext * 1.6, 0.05));
    let prev_seabed_share = rgb_transmittance * prev_seabed_visibility;
    let prev_seabed_through = refracted.color * prev_seabed_share + prev_body_radiance * (1.0 - prev_seabed_share);
    let prev_physical_transmitted = mix(prev_body_radiance, prev_seabed_through, refracted.valid);
    let shared_optics = wp.optics_params.x > 0.5;
    let physical_transmitted = select(prev_physical_transmitted, shared_optics_transmitted, shared_optics);

    let physical_look = wp.look_params.x > 0.5;
    let transmitted = select(legacy_transmitted, physical_transmitted, physical_look);

    // Legacy: the reflection weight was capped at 0.43..0.72 and scaled DOWN as the sea got
    // rougher. That cap is what made the surface read as blue plastic — real water approaches a
    // mirror at grazing incidence. Physical mode lets Fresnel run uncapped and fixes the look by
    // fixing what is reflected, not by hiding the reflection.
    let open_ocean_activity = clamp(max(state.slope_variance * 2.8, wp.wave_amp * 0.70), 0.0, 1.0);
    let reflection_scale = mix(0.72, 0.43, open_ocean_activity);
    let reflection_cap = mix(0.72, 0.52, open_ocean_activity);
    let legacy_reflection_weight = clamp(physical_fresnel * reflection_scale + 0.012, 0.012, reflection_cap);
    // Geometric self-occlusion of the reflected sky — the piece that was actually missing.
    //
    // We were weighting the environment reflection by Fresnel ALONE. A full specular BRDF has two
    // parts: Fresnel and a geometry/masking term. For the sun lobe we compute masking (Smith) and
    // use it; for the environment reflection we were silently treating it as 1.0. That is only valid
    // on a flat surface. On a rough sea at a grazing angle most reflected rays do not reach the sky
    // at all — they hit the back of the next wave. Ignoring that made every distant water pixel
    // ~70-85% bright sky, which is why the ocean stayed light blue-grey however dark the body colour
    // was set, and why neither the colour ramp nor the roughness-aware Fresnel moved it.
    //
    // Smith G1 on its own over-darkens (the split-sum environment BRDF is not simply F*G1), so this
    // blends 70% of the way toward it. That 0.7 is an artistic constant, not physics — it is the one
    // fudge in this term, and "Reflection gain" still scales the result if you want it further either
    // way.
    let env_visibility = mix(1.0, smith_schlick_g1(ndv, roughness), 0.7);
    let physical_reflection_weight =
        clamp(physical_fresnel * env_visibility * max(wp.look_params.w, 0.0), 0.0, 1.0);
    let reflection_weight = select(legacy_reflection_weight, physical_reflection_weight, physical_look);
    rgb = mix(transmitted, refl, reflection_weight);

    // fft_control.w carries submersion depth in metres now, not a 0/1 flag.
    // WRL-002: in physical mode the compositor owns the eye->surface segment (it marches to
    // the crossing and attenuates that path), so fogging the surface here as well counted
    // the same water twice. Legacy keeps its own tint for A/B.
    let is_underwater = wp.fft_control.w > 0.0;
    if (is_underwater && !(physical_look && shared_optics)) {
        let uw_dist = length(in.world_pos);
        let uw_extinction = 1.0 - exp(-uw_dist * vec3<f32>(0.20, 0.08, 0.03));
        let uw_fog_color = deep_col * 0.90 + vec3<f32>(0.0, 0.04, 0.08);
        rgb = mix(rgb, uw_fog_color, uw_extinction);
    }

    // WTR-003 — debug views replace the lit output (view 0 = normal shading). Aggregated
    // FFT diagnostics + the interaction/foam/reflection/refraction intermediates computed
    // above; compositor-owned underwater/god-ray/caustic views are handled after this pass,
    // while the still-reserved whitewater pool/overflow diagnostics fall through to black.
    let dbg_view = i32(wp.debug_params.x + 0.5);
    if (dbg_view > 0) {
        var fft_disp = vec3<f32>(0.0);
        var fft_slope_v = vec2<f32>(0.0);
        var fft_j = 1.0;
        let fft_comp = state.compression;
        let fft_curv = state.curvature;
        let fft_crest = state.crest_energy;
        let fft_slope_variance = state.slope_variance;
        if (wp.fft_control.x > 0.5) {
            for (var layer = 0; layer < 4; layer = layer + 1) {
                let length_m = max(wp.fft_cascade_lengths[layer], 1.0);
                let duv = fft_scaled_uv(in.base_xz, length_m, layer, wp.warp_amp, wp.wave_scale);
                fft_disp = fft_disp + textureSampleLevel(fft_displacement, fft_samp, duv, layer, 0.0).xyz * max(wp.wave_scale, 0.01);
                fft_slope_v = fft_slope_v + textureSampleLevel(fft_dynamics, fft_samp, duv, layer, 0.0).xy;
            }
        }
        let dbg_interaction = interaction_sample(in.base_xz);
        let dbg_foam_hist = persistent_foam_sample(in.base_xz);
        var dbg_foam_src = clamp(dbg_interaction.b, 0.0, 1.0);
        if (wp.fft_control.x > 0.5) {
            let breaker = smoothstep(0.014, 0.050, fft_crest);
            dbg_foam_src = max(dbg_foam_src, breaker);
        }
        var dbg_sky = vec3<f32>(0.0);
        if (linear > 0.5) {
            dbg_sky = sky_env_sample(reflect(normalize(in.world_pos), n));
        } else {
            dbg_sky = mix(sun_ambient, fog_color, sun_up);
        }
        let dbg_base_uv = scene_uv(in.clip.xy);
        return debug_view(dbg_view, in.base_xz, in.world_pos, water_depth,
            fft_disp, length(fft_disp.xz), fft_disp.y, length(fft_slope_v), fft_j,
            fft_comp, fft_curv, fft_crest, fft_slope_variance, dbg_interaction,
            dbg_foam_hist, dbg_foam_src, dbg_sky, ssr, planar_refl, refl, refract_uv,
            dbg_base_uv, refracted, transmission);
    }

    // Exact GodotOceanWaves light() model.  Its author uses a fixed 0.4 light
    // roughness (independent of the material ROUGHNESS output), custom Fresnel,
    // empirical Smith masking and a height-driven turquoise SSS term.  The current
    // FFT displacement supplies the reference shader's `wave_height`.
    const GODOT_LIGHT_ROUGHNESS: f32 = 0.4;
    let halfway = safe_normalize3(l + v, n);
    let godot_nl = max(dot(n, l), 2e-5);
    let godot_nv = max(dot(n, v), 2e-5);
    let godot_fresnel = godot_water_fresnel(godot_nv, GODOT_LIGHT_ROUGHNESS);
    let light_mask = godot_smith_masking_shadowing(GODOT_LIGHT_ROUGHNESS, godot_nv);
    let view_mask = godot_smith_masking_shadowing(GODOT_LIGHT_ROUGHNESS, godot_nl);
    let microfacet_distribution = godot_ggx_distribution(dot(n, halfway), GODOT_LIGHT_ROUGHNESS);
    let geometric_attenuation = 1.0 / (1.0 + light_mask + view_mask);
    let godot_specular = godot_fresnel * microfacet_distribution * geometric_attenuation /
        (4.0 * godot_nv + 0.1) * sun_vis;

    const GODOT_SSS_MODIFIER = vec3<f32>(0.9, 1.15, 0.85);
    let wave_height = state.displacement.y;
    // WTR-LOOK — crest-thickness translucency.
    //
    // The reference gates its subsurface term on `wave_height + 2.5`: a crude stand-in for "how
    // much water is there to glow", which is really just elevation. It cannot tell a thin, sharp,
    // about-to-break crest (which genuinely transmits light, because there are only centimetres of
    // water between the sun and the eye) from the broad shoulder of a swell at the same height
    // (which is metres thick and should stay opaque).
    //
    // Everything needed for a real thinness estimate is already computed for the foam path:
    //   crest_energy — how sharply the surface is peaked
    //   curvature    — positive curvature means a convex, thinning ridge
    //   compression  — the collapsing Jacobian of a crest folding over, i.e. the thinnest state
    // Foam works against transmission: an aerated, broken crest scatters light diffusely instead
    // of transmitting it, so it must reduce the effect rather than add to it.
    let crest_sharpness = smoothstep(0.010, 0.075, state.crest_energy);
    let ridge_convexity = smoothstep(0.004, 0.045, state.curvature);
    let fold_thinning = smoothstep(0.020, 0.16, state.compression);
    // Elevation still matters — a trough is thick by definition — but it is now one term among
    // four rather than the whole estimate.
    let elevation_term = smoothstep(0.0, 0.55, max(wave_height, 0.0));
    let thinness = clamp(
        (crest_sharpness * 0.42 + ridge_convexity * 0.22 + fold_thinning * 0.26 + elevation_term * 0.10) *
        (1.0 - state.foam_density * 0.75), 0.0, 1.0);
    // The reference's backlighting geometry is kept verbatim: transmission peaks when the sun is
    // behind the wave (dot(l, -v)) and grazing the surface (the 0.5 - 0.5*dot(l, n) term).
    let sss_height = thinness * 2.5 * pow(max(dot(l, -v), 0.0), 4.0) *
        pow(0.5 - 0.5 * dot(l, n), 3.0);
    let sss_near = 0.5 * pow(godot_nv, 2.0);
    let lambertian = 0.5 * godot_nl;
    let godot_foam_factor = state.foam_density;
    let godot_foam_color = vec3<f32>(0.88, 0.92, 0.94);
    let godot_diffuse = mix((sss_height + sss_near) * GODOT_SSS_MODIFIER /
        (1.0 + light_mask) + vec3<f32>(lambertian), godot_foam_color, godot_foam_factor) *
        (1.0 - godot_fresnel) * sun_vis;
    if (physical_look) {
        // --- WTR-LOOK sun glitter: GGX evaluated at the variance-filtered roughness, full radiance.
        // water_roughness() already folds in the slope variance that cascade filtering removed, so a
        // near crest gets a tight sparkle while the horizon settles into a broad stable sheen. The
        // reference's fixed 0.4 roughness can do neither, and aliases badly with distance — this is
        // the term where we go past it rather than merely matching it.
        let roughness4 = roughness * roughness * roughness * roughness;
        let spec_alpha = clamp(sqrt(sqrt(roughness4 + min(pixel_normal_variance, 0.04))), 0.075, 0.45);
        let ndh = max(dot(n, halfway), 0.0);
        let vdh = max(dot(v, halfway), 0.0);
        let d_term = godot_ggx_distribution(ndh, spec_alpha);
        let vis_term = smith_schlick_g1(godot_nl, spec_alpha) * smith_schlick_g1(godot_nv, spec_alpha) /
            max(4.0 * godot_nl * godot_nv, 1e-4);
        let f_term = schlick_fresnel(physical_f0, vdh);
        // Clamped so one sub-pixel crest cannot fire a runaway speck into the bloom.
        let glitter = min(d_term * vis_term * f_term * godot_nl, 64.0);
        rgb = rgb + sun_diffuse * glitter * sun_vis * max(wp.look_params.y, 0.0);

        // --- Subsurface scattering on its own light path. The reference scatters near-white,
        // green-biased light out of the wave volume and never multiplies it by the body colour.
        // Multiplying it by a navy deep tint (linear red ~0.001) is what erased the backlit crest
        // glow. The 0.35 normaliser maps the reference's Godot-side energy onto our HDR sun
        // radiance so a gain of 1.0 is the intended look.
        // These two terms are physically different things and must NOT share a near-white tint.
        //
        // sss_height is genuine transmission: sunlight passing THROUGH a thin backlit crest. It
        // travels only centimetres of water, so it emerges bright and turquoise — tint it with the
        // shallow (short-path) colour.
        //
        // sss_near is 0.5 * ndv^2 — a view-dependent wrap-lighting fudge with no backlighting
        // geometry in it at all. The reference gets away with a near-white value there because
        // Godot's DIFFUSE_LIGHT is scaled differently; multiplied by our real HDR sun radiance it
        // became a constant whitish wash over every water pixel facing the camera. THAT is what made
        // the sea read as a desaturated translucent silver mirror rather than coloured water. It is
        // light scattered from just below the surface, so it must carry the water's own colour.
        let transmit_tint = shallow_col * 6.0 * GODOT_SSS_MODIFIER;
        let sss_gain = max(wp.look_params.z, 0.0);
        rgb = rgb + transmit_tint * sun_diffuse * (sss_height / (1.0 + light_mask)) *
            (1.0 - physical_fresnel) * sun_vis * sss_gain * 0.35;
        rgb = rgb + ocean_body * 2.5 * sun_diffuse * (sss_near / (1.0 + light_mask)) *
            (1.0 - physical_fresnel) * sun_vis * sss_gain * 0.35;
        // Foam direct light is deliberately NOT added here: the foam block below replaces rgb
        // wherever combined_foam is non-zero, so adding it twice would only brighten the seam.
    } else {
        // Legacy composite. Godot applies these light accumulators inside its material pipeline.
        // Our HDR renderer stores physical sun radiance directly, so adding the reference output
        // raw would bypass the water-body albedo and turn the surface white. This path preserved
        // the reference lobe/crest response but placed diffuse through the ocean body and kept a
        // restrained 0.12 glint — which is precisely what suppressed the sun glitter.
        rgb = rgb + ocean_body * sun_diffuse * godot_diffuse;
        rgb = rgb + sun_diffuse * (godot_specular * 0.12);
    }

    // Artistic darkening of shadowed water for readability (dims the ambient/sky term
    // too, which the pure sun-removal above does not). 0 = physical (sun-only).
    rgb = rgb * (1.0 - sun_shadow * wp.shadow_dim);

    // LDR path saturates like the rest of the LDR pipeline; HDR keeps radiance uncapped.
    if (linear <= 0.5) {
        rgb = min(rgb, vec3<f32>(1.0));
    }

    if (frame.params.fog_enabled >= 1.5) {
        rgb = apply_fog(rgb, in.world_pos);
    } else {
        rgb = mix(fog_color, rgb, in.fog);
    }

    // Swash: a gentle oscillation of the near-shore waterline (cosmetic — buoyancy stays flat).
    // It raises/lowers the EFFECTIVE column depth so the transparent edge + foam breathe in and
    // out over the wet beach. The body colour keeps the true depth so it doesn't flicker.
    // WRL-004: a body's own flow (table lane [3i+2]) drives its advection; the ocean keeps the
    // global lane. A lake has no swash and a much weaker shoreline band ("little surf").
    var river_flow = normalize(wp.flow_direction_speed.xy + vec2<f32>(1e-4, 0.0)) *
        max(wp.flow_direction_speed.z, 0.0) * select(0.0, 1.0, wp.flow_direction_speed.w > 0.5);
    var body_surf = 1.0;
    if (body_index > 0) {
        let f = wp.bodies[3 * (body_index - 1) + 2];
        // WRL-004 bank slowdown: the flow eases to ~15% at the bank, from the body's own
        // boundary distance (authored bank data would replace this; screen depth never does).
        river_flow = normalize(f.xy + vec2<f32>(1e-4, 0.0)) * max(f.z, 0.0) *
            mix(1.0, 0.15, smoothstep(0.55, 1.0, body_edge));
        body_surf = 0.15;
    }
    let swash = sin(TWO_PI * wp.time * wp.swash_speed);
    let surf = state.surf;
    let wetness = select(0.0, state.foam_history_sample.a, surf_pilot > 0.5 && body_index == 0);
    let wet_band = (1.0 - smoothstep(0.03, 0.25, water_depth)) * surf.weight;
    // One surface owns the thin wet film; no extra translucent terrain overlay.
    rgb = mix(rgb, own_background * 0.78, wetness * wet_band * 0.55);
    let eff_depth = water_depth + mix(swash * wp.swash_amp * body_surf, surf.wash, surf.weight) + wetness * wet_band * 0.02;
    let coast_flow = shallow_flow(water_depth, in.base_xz);
    let spray_wind = normalize(wp.fft_wind_sea.xy + vec2<f32>(1e-4, 0.0)) * max(wp.fft_wind_sea.z, 0.5);

    let surface_wave = in.world_pos.y + frame.cam_pos.y - in.body_level;
    let shore_break = mix(smoothstep(0.015, 0.12, surface_wave), surf.breaking, surf.weight);

    // Shoreline foam: churning procedural noise in a band along the (swash-moved) edge. The band
    // fades IN from the waterline and OUT into deeper water (peak ~1/4 of foam_width in), so the
    // LAND side dissolves softly instead of ending on a hard bright line where the water geometry
    // is clipped by the beach — the deep side already faded. Foam also fades to 0 right at the
    // edge, so the water there is transparent (soft wash over wet sand) rather than opaque white.
    let ft = eff_depth / max(wp.foam_width * 2.2, 0.4);
    // Deep-side extent of the band. 1.95 -> 1.55 -> 1.10 -> 0.80 over three passes, every time
    // because the owner reported the wash reaching too far out to sea. At the default foam_width
    // of 0.4 the divisor above is 0.88 m, so `ft` IS column depth in units of 0.88 m.
    //
    // The previous pass moved only the outermost tail (the point where the fade REACHES zero) and
    // left the fade's start pinned at 0.35, which is why cutting 1.55 -> 1.10 read as a smaller
    // change than the number suggests: the band's bright part never moved, only its faint outer
    // skirt got shorter. The fade start is now band_outer * 0.32, so both ends of the deep edge
    // travel together. At the old 1.10 that expression IS 0.35, so this is a re-parametrisation of
    // the existing shape and not a new one; the visible change is entirely the 1.10 -> 0.80 default.
    //
    // In metres of column depth, deep edge: full band out to 0.23 m (was 0.31 m), gone by 0.70 m
    // (was 0.97 m). The half-brightness point, which is what the eye actually calls "the outer edge
    // of the surf", moves from 0.63 m to 0.46 m — a 27% pull-in, and the first one that moves the
    // bright part of the band rather than its tail. The meander below then pulls the average in
    // further, to ~0.40 m, because it is one-sided.
    //
    // This is the DEEP edge, which had already faded to nothing. The LAND side (the
    // smoothstep(0.0, 0.06, ft) below) carries the no-hard-line guarantee and is untouched — the
    // whole point of splitting the band into two independent ends is that this knob cannot
    // reintroduce the bright clipped line at the beach. Note that the meander below likewise feeds
    // `ft_deep` to the deep term ONLY: the land term keeps the raw `ft`.
    //
    // `band_outer` is forced to stay above `band_fade_start`: smoothstep(e, e, x) has no defined
    // value, and WGR_WATER_FOAM_BAND_OUTER is a live env knob someone will eventually set to 0 to
    // see what happens. Doing it here rather than only in the Rust parse keeps the shader correct
    // on its own terms.
    let band_fade_start = select(foam_band_outer * 0.32, 0.35, shore_foam_legacy > 0.5);
    let band_outer = max(select(foam_band_outer, 1.95, shore_foam_legacy > 0.5),
        band_fade_start + 0.02);

    // MEANDERING DEEP EDGE — the other half of "it still looks tiled".
    //
    // Every anti-repetition pass so far has worked on what FILLS the band (more octaves, warps,
    // band limiting, then large-scale patchiness of its brightness). None of them touched the
    // band's SHAPE, and the shape is the part that actually reads as machine-made: `ft` is a
    // function of column depth alone, so along any stretch of coast the wash is a ribbon of
    // exactly constant width following a depth contour. No amount of noise inside a perfectly
    // parallel-sided ribbon stops it looking stamped, which is why "add another octave" has
    // diminishing returns here and this does not.
    //
    // Real surf has a ragged outer edge: bathymetry and refraction push the break line in and out
    // by metres over tens of metres of coast (the same process that cuts beach cusps). So perturb
    // the depth coordinate the DEEP term sees, with two low-frequency octaves at 12.2 m and 29.3 m
    // cells whose ratio (2.40) is not a simple fraction, and which are also incommensurate with
    // the 21.7 m / 34.5 m patchiness pair above — two independent fields at similar scales would
    // otherwise beat into a single visible rhythm, which is the lattice bug one octave down. All
    // four cross-ratios are checked by the test, because picking round numbers here is exactly how
    // that bug gets rebuilt by an edit that looks like a tidy-up.
    //
    // ONE-SIDED ON PURPOSE: the noise pair is mapped to [0,1] and only ever SCALES ft UP, i.e. the
    // edge only ever pulls further IN. That keeps the meander from fighting the owner's actual
    // request — no part of the coast ends up with foam further out than the nominal band_outer.
    //
    // vnoise_lod converges to 0.5 once the cells go sub-pixel, so a distant coast gets a constant
    // 1.17 scale rather than aliasing: the scalloping fades out by itself at range, exactly like
    // the patchiness term, and a far beach still reads as one even band.
    var ft_deep = ft;
    if (shore_foam_legacy <= 0.5 && ft < band_outer * 2.0) {
        let meander_a = vnoise_lod(in.base_xz * 0.0817 + vec2<f32>(wp.time * 0.005, wp.time * 0.003),
            foam_fp * 0.0817);
        let meander_b = vnoise_lod(in.base_xz * 0.0341 + vec2<f32>(-wp.time * 0.003, wp.time * 0.004) +
            vec2<f32>(88.3, 61.7), foam_fp * 0.0341);
        let meander = clamp(meander_a * 0.6 + meander_b * 0.4, 0.0, 1.0);
        ft_deep = ft * (1.0 + max(foam_band_meander, 0.0) * meander);
    }
    let foam_band = smoothstep(0.0, 0.06, ft) * (1.0 - smoothstep(band_fade_start, band_outer, ft_deep)) * body_surf;
    // coast_flow points toward land. Sampling x + v*t moves a pattern in -v, so
    // the old wash visibly travelled offshore. x - v*t makes each band advance landward.
    var foam = 0.0;
    if (foam_band > 0.001 && wp.foam_intensity > 0.001) {
        let shore_speed = 0.38 + shore_break * 0.82;
        let coast_noise = foam_noise_lod(in.base_xz - coast_flow * wp.time * shore_speed + river_flow * wp.time,
            wp.time, foam_fp);

        // `coast_flow` is the reconstructed water-depth gradient toward land. Build elongated
        // streaks perpendicular to that direction so wash follows the actual shoreline contour.
        let shoreward = normalize(coast_flow + spray_wind * 0.001);
        let shoreline_tangent = vec2<f32>(-shoreward.y, shoreward.x);
        let shoreline_streak = shoreline_streaks(in.base_xz, shoreline_tangent, shoreward,
            wp.time, shore_speed, foam_fp);
        let coast_pattern = max(coast_noise, shoreline_streak);
        // STRENGTH (independent of the band limiting above, which only removes variance): the old
        // gains ran to 1.30 * 1.65 = 2.14 before the clamp, so every pixel with coast_pattern above
        // ~0.32 was pure white and the wash read as a solid painted rim rather than as foam with
        // structure in it. 0.86 * 1.45 = 1.25 keeps a saturating peak on the strongest breaks while
        // leaving the rest of the band graded.
        let base_gain = select(0.20, 0.35, shore_foam_legacy > 0.5);
        let pattern_gain = select(0.66, 0.95, shore_foam_legacy > 0.5);
        let break_gain = select(0.45, 0.65, shore_foam_legacy > 0.5);
        // A distant coastline should read as a thin bright line, not as a band of the same
        // brightness as the surf twenty metres away. Independent of the band limiting: that fixes
        // WHAT the far foam looks like, this fixes HOW MUCH of it there is.
        let far = 1.0 - 0.45 * smoothstep(150.0, 1100.0, length(in.world_pos));
        let far_fade = select(far, 1.0, shore_foam_legacy > 0.5);
        // LARGE-SCALE PATCHINESS — the residual "it still tiles".
        //
        // The 1.35 x 5.3 m streak lattice is gone, but what replaced it is still a STATIONARY
        // field: statistically identical at every point of every beach. Its largest feature is one
        // base-octave cell (FOAM_FREQ 0.55, so 1.8 m), which means that along 200 m of coastline
        // the eye sees the same texture a hundred times with nothing varying at the scale it is
        // actually looking at. That reads as a repeat even though no two cells are equal — the
        // tell here is not a visible seam, it is uniformity.
        //
        // Real surf is not uniform: bathymetry, refraction and headlands make it heavy in some
        // places and nearly absent in others, over tens of metres. So modulate the band with a
        // very low-frequency field. Two octaves at 21.7 m and 34.5 m cells, whose ratio (1.59) is
        // deliberately not a simple fraction so the pair cannot re-align into a beat of its own at
        // some third frequency — that re-alignment is exactly the mistake that produced the old
        // lattice, and repeating it two octaves lower would be a slower, larger version of the
        // same bug.
        //
        // It runs through vnoise_lod on the same footprint as every other octave, so it cannot
        // reintroduce the aliasing the band-limiting work removed; note that a fully filtered
        // vnoise_lod returns 0.5, which maps to a patch of exactly 1.0, so this term switches
        // itself off at distance instead of having to be faded out by hand.
        //
        // Drift is near-static (0.004-0.006 m/s). Where surf is heavy is a property of the seabed
        // and should not visibly slide along the coast, but a dead-static field looks painted on.
        let patch_a = vnoise_lod(in.base_xz * 0.0461 + vec2<f32>(wp.time * 0.006, -wp.time * 0.004),
            foam_fp * 0.0461);
        let patch_b = vnoise_lod(in.base_xz * 0.0290 + vec2<f32>(-wp.time * 0.004, wp.time * 0.005) +
            vec2<f32>(53.1, 27.9), foam_fp * 0.0290);
        // Centred on 1.0 so mean coverage is unchanged: this REDISTRIBUTES foam along the shore,
        // it does not add or remove any. 0.52..1.48 is a wide enough spread to read as "heavier
        // here, lighter there" without emptying a stretch of coast completely.
        let foam_patch = mix(1.0, 0.52 + 0.96 * (patch_a * 0.62 + patch_b * 0.38),
            select(foam_patch_gain, 0.0, shore_foam_legacy > 0.5));
        foam = clamp(foam_band * (base_gain + coast_pattern * pattern_gain) *
            max(wp.foam_intensity, 0.0) * (1.0 + shore_break * break_gain) * far_fade * foam_patch,
            0.0, 1.0);
    }
    // evaluate_water_surface already fetched this texel for crest translucency.
    let foam_history_sample = state.foam_history_sample;
    let persistent_foam = clamp((foam_history_sample.r + foam_history_sample.g * 1.5) * (0.65 + foam_history_sample.b * 0.45), 0.0, 1.0);
    // Whitecaps belong on the *top* of a breaking crest. Godot stores the
    // negative-Jacobian accumulation in normal-map alpha, then thresholds that
    // field in the material. Retain our persistent equivalent, but add a direct
    // crest-shaped term so a tall crest reads as a white cap instead of leaving
    // foam scattered around the troughs. The noise only breaks the edge up; it
    // does not decide where a crest is.
    let crest_top = smoothstep(0.035, 0.16, state.crest_energy) *
        smoothstep(0.008, 0.085, max(surface_wave, 0.0));
    let jacobian_break = smoothstep(0.035, 0.22, state.compression);
    var breaker_foam = 0.0;
    if (crest_top > 0.001) {
        // Pre-scaling the position by 1.5 scales its per-pixel footprint by 1.5 too.
        let crest_shape = smoothstep(0.42, 0.68,
            foam_noise_lod(in.base_xz * 1.5 - spray_wind * wp.time * 0.18, wp.time, foam_fp * 1.5));
        // Ocean vs coast. A wave breaks when it can no longer support its own steepness, and shoaling
        // water forces that much sooner: the bottom compresses the orbital motion, the crest steepens
        // and spills. Over deep water the same sea state carries long swell that mostly does not break.
        let deep_water = smoothstep(6.0, 45.0, water_depth); // 0 = shoaling, 1 = open ocean
        // A wave breaks when it can no longer support its own steepness, and shoaling water forces
        // that much sooner. The deep-water end is now tunable: the fixed 0.55 still left open
        // ocean noticeably capped, and whitecaps out there should be occasional, not general.
        let deep_falloff = clamp(wp.underwater_gate.z, 0.0, 1.0);
        // WRL-002: and a crest only whitecaps when there is wind to make it (the same gate the
        // foam injection and the spray emitter use, so all three sources agree).
        let wind_gate = select(1.0, whitecap_wind_factor(wp.fft_wind_sea.z), wp.optics_params.y > 0.5);
        // Depth suppresses ordinary swell, not a crest already strongly compressed.
        // Reuse the geometric breaking signal; no extra samples or shoreline gain.
        let depth_readiness = mix(1.05, mix(1.05, 0.12, deep_falloff), deep_water);
        let break_readiness = mix(depth_readiness, 1.05,
            jacobian_break * clamp(deep_breaker_recovery, 0.0, 1.0)) * wind_gate;
        breaker_foam = clamp(crest_top * mix(0.28, 0.72, jacobian_break) * crest_shape * break_readiness,
            0.0, 1.0);
    }
    // Incoming-swell breaking is independent of local wind. The pilot owns this
    // source only where it also owns the displaced surface.
    breaker_foam = breaker_foam * (1.0 - surf.weight) + surf.breaking * 0.65;
    // Sparse short-lived flecks sell wind-torn shore break and whitecap spindrift
    // without a separate particle system or a broad white surface layer.
    let spray_source = foam_band * shore_break + breaker_foam;
    var spray_flecks = 0.0;
    if (spray_source > 0.001) {
        // The worst aliaser in the shader: 4.6x pre-scale puts the top octave at ~5 cm cells, and
        // the 0.78..0.93 threshold is the narrowest, so unfiltered it turned into crawling speckle
        // the moment the surf left the near field. Filtered, the flecks fade out with distance
        // instead — which is also what sub-pixel spray genuinely does.
        spray_flecks = spray_source * smoothstep(0.78, 0.93,
            foam_noise_lod(in.base_xz * 4.6 + spray_wind * wp.time * 1.8, wp.time, foam_fp * 4.6)) * 0.26;
    }
    // Shore/river foam and wave whitecaps have independent UI controls. Do not
    // multiply whitecaps by the shore gain: setting shore foam to zero otherwise
    // makes the entire wave-foam slider inert.
    let foam_scale = max(wp.foam_intensity, 0.0);
    // Wave foam gets its own multiplier. The shoreline band and the
    // whitecaps are different phenomena -- one is water breaking on land, the other is a crest
    // collapsing under its own steepness -- and wanting more of one while wanting less of the
    // other is the normal case, not an exotic one.
    let wave_scale = max(wp.underwater_gate.y, 0.0);
    // WRL-004 foam streaks: a fast reach carries thin foam lines along its flow, densest at
    // the banks where the water is slowest and the bubbles collect. Two phases again, so the
    // streaks travel without a pulse.
    var river_streak = 0.0;
    if (body_kind == 2 && body_flow_speed > 0.3) {
        let along = body_flow;
        let across = vec2<f32>(-along.y, along.x);
        let period = 6.0;
        let t1 = fract(wp.time / period);
        let t2 = fract(wp.time / period + 0.5);
        let w1 = 1.0 - abs(2.0 * t1 - 1.0);
        let w2 = 1.0 - abs(2.0 * t2 - 1.0);
        let local = vec2<f32>(dot(in.base_xz, along) * 0.12, dot(in.base_xz, across) * 1.1);
        let q1 = local - vec2<f32>(body_flow_speed * t1 * period * 0.12, 0.0);
        let q2 = local - vec2<f32>(body_flow_speed * t2 * period * 0.12, 0.0) + vec2<f32>(5.7, 3.3);
        let streak = (foam_noise_lod(q1, wp.time, foam_fp * 0.5) * w1 + foam_noise_lod(q2, wp.time, foam_fp * 0.5) * w2) /
            max(w1 + w2, 1e-3);
        let bank = smoothstep(0.35, 0.95, body_edge);
        river_streak = smoothstep(0.58, 0.82, streak) * clamp(body_flow_speed / 2.0, 0.0, 1.0) * (0.25 + 0.75 * bank) * 0.55;
    }
    // Break the periodic FFT crest outlines into irregular wind-stretched rafts.
    // World-space, incommensurate scales avoid repeating the simulation tile;
    // filtering preserves average coverage once these rafts become sub-pixel.
    let foam_drift = in.base_xz - spray_wind * wp.time * 0.15;
    let foam_wind_dir = normalize(wp.fft_wind_sea.xy + vec2<f32>(1e-4, 0.0));
    let foam_crosswind = vec2<f32>(-foam_wind_dir.y, foam_wind_dir.x);
    let raft_uv = vec2<f32>(dot(foam_drift, foam_wind_dir) * 0.035,
                            dot(foam_drift, foam_crosswind) * 0.12);
    let raft_coarse = vnoise_lod(raft_uv, foam_fp * 0.12);
    let raft_detail = vnoise_lod(vec2<f32>(raft_uv.x * 1.73 - raft_uv.y * 0.61,
                                          raft_uv.x * 0.61 + raft_uv.y * 1.73) + vec2<f32>(19.7, 43.1),
                                foam_fp * 0.221);
    let raft_coverage = mix(0.18, 1.0, smoothstep(0.25, 0.75, raft_coarse * 0.65 + raft_detail * 0.35));
    let unstructured_foam = max(max(foam, river_streak),
        max(persistent_foam, breaker_foam) * wave_scale * raft_coverage) +
        spray_flecks * wave_scale * raft_coverage;
    var combined_foam = 0.0;
    var breaker_material_weight = 0.0;
    var breaker_material_detail = 1.0;
    if (unstructured_foam > 0.001) {
        // This one multiplies EVERY foam source, so its aliasing was modulating the shoreline
        // band as well as the whitecaps — a second, finer pattern laid over the first.
        let foam_structure = foam_noise_lod(in.base_xz * 2.7 + spray_wind * wp.time * 0.45,
            wp.time, foam_fp * 2.7);
        combined_foam = clamp(unstructured_foam * mix(0.46, 1.0, foam_structure), 0.0, 1.0);
        if (breaker_cells > 0.5) {
            let wave_amount = (max(persistent_foam, breaker_foam) + spray_flecks) * wave_scale * raft_coverage;
            let cell_footprint = foam_fp * 2.7 * FOAM_FREQ;
            var resolved = resolved_breaker_coverage(wave_amount, foam_structure, cell_footprint);
            let shore = clamp(max(foam, river_streak) * mix(0.46, 1.0, foam_structure), 0.0, 1.0);
            // Never re-threshold subpixel noise: converge to the original filtered
            // coverage, not all-white/all-clear distant water.
            let detail = 1.0 - smoothstep(0.15, 0.55, cell_footprint);
            if (detail > 0.001 && resolved > 0.02) {
                // One additional band-limited octave only on resolved near foam,
                // not another full fractal field over every water pixel.
                let bubbles = vnoise_lod(foam_drift * 7.31, foam_fp * 7.31);
                resolved = resolved * mix(0.55, 1.0, smoothstep(0.20, 0.65, bubbles));
                breaker_material_detail = 0.80 + 0.20 * bubbles;
                breaker_material_weight = detail * resolved / max(resolved + shore, 1e-4);
                if (breaker_cells > 1.5) {
                    let rim = breaker_bubble_rim(foam_drift, foam_fp);
                    let fresh = smoothstep(0.25, 0.80, wave_amount);
                    resolved = resolved * mix(0.38 + 0.62 * rim, 0.78 + 0.22 * rim, fresh);
                    breaker_material_detail = 0.82 + 0.18 * rim;
                    breaker_material_weight = detail * resolved / max(resolved + shore, 1e-4);
                }
            }
            combined_foam = mix(combined_foam, max(shore, resolved), detail);
        }
    }
    // Bubbles form a broad, rough dielectric layer: mostly sky/sun-lit diffuse scattering with
    // only a subdued dielectric highlight. This is material response, never an emissive overlay.
    var foam_color = vec3<f32>(0.0);
    if (combined_foam > 0.0) {
        let foam_normal = normalize(mix(n, vec3<f32>(0.0, 1.0, 0.0), mix(0.72, 0.35, breaker_material_weight)));
        let foam_ndl = max(dot(foam_normal, l), 0.0);
        let foam_ndv = max(dot(foam_normal, v), 0.0);
        let foam_h = safe_normalize3(l + v, foam_normal);
        let foam_ndh = max(dot(foam_normal, foam_h), 0.0);
        let foam_vdh = max(dot(v, foam_h), 0.0);
        let foam_roughness = 0.72;
        let foam_alpha_sq = pow(foam_roughness, 4.0);
        let foam_d_base = max(foam_ndh * foam_ndh * (foam_alpha_sq - 1.0) + 1.0, 1e-6);
        let foam_distribution = foam_alpha_sq / (PI * foam_d_base * foam_d_base);
        let foam_visibility = smith_schlick_g1(foam_ndl, foam_roughness) * smith_schlick_g1(foam_ndv, foam_roughness);
        let foam_specular = foam_distribution * foam_visibility * schlick_fresnel(0.02, foam_vdh) /
            max(4.0 * foam_ndl * foam_ndv, 1e-4) * foam_ndl * sun_vis;
        let foam_albedo = vec3<f32>(0.88, 0.92, 0.94) * mix(1.0, breaker_material_detail, breaker_material_weight);
        let foam_diffuse = (sun_ambient * amb_ao + sun_diffuse * foam_ndl * sun_vis) * (foam_albedo / PI);
        foam_color = foam_diffuse + sun_diffuse * foam_specular * 0.08;
    }
    rgb = mix(rgb, foam_color, combined_foam);

    // Base opacity, Fresnel-opaque at grazing angles (where real water is a mirror) so
    // the seabed only shows through looking down. A soft shoreline fade dissolves the water
    // to transparent as the (swash-moved) column depth -> 0, so the hard coast cut becomes a
    // gentle wash over the visible wet beach; foam forces opacity where it sits.
    let shore = smoothstep(0.0, wp.coast_fade, eff_depth);
    // Physical: the colour above already contains the transmitted background, so the only
    // thing the framebuffer blend may still do is the geometric shore fade (and foam forces
    // coverage). Legacy keeps its Fresnel-raised opacity, which double-blends by design.
    let legacy_alpha = max(mix(wp.alpha, 1.0, fresnel) * shore, combined_foam) * body_coverage;
    let physical_alpha = max(shore, combined_foam) * body_coverage;
    let alpha = select(legacy_alpha, physical_alpha, physical_look && shared_optics);
    return vec4<f32>(rgb, alpha);
}

// Local curling-sheet prototype. No grid displacement or physics is replaced.
struct CurlOut {
    @builtin(position) clip: vec4<f32>,
    @location(0) world_rel: vec3<f32>,
    @location(1) normal: vec3<f32>,
    @location(2) data: vec4<f32>, // coverage, collapse, across coordinate, particle flag
    @location(3) uv: vec2<f32>,
};
// Bounded local contour search, using the existing terrain texture. This is
// only called by the opt-in fixture; normal ocean vertices pay no cost.
fn curl_shore_offset(z: f32) -> f32 {
    var dry = 7790.0;
    var wet = 7900.0;
    for (var i = 0u; i < 8u; i += 1u) {
        let x = (dry + wet) * 0.5;
        if (seabed_height(vec2<f32>(x,z)) > wp.sea_level) {dry=x;} else {wet=x;}
    }
    let hd = seabed_height(vec2<f32>(dry,z)) - wp.sea_level;
    let hw = seabed_height(vec2<f32>(wet,z)) - wp.sea_level;
    return mix(dry,wet,clamp(hd/max(hd-hw,1e-5),0.0,1.0)) - 7836.6085;
}
fn curl_placed(uv: vec2<f32>, time: f32) -> vec3<f32> {
    let p = curl_point(uv,time).position;
    return p + vec3<f32>(curl_shore_offset(p.z),0.0,0.0);
}
fn curl_world(uv: vec2<f32>) -> vec3<f32> {
    let position = curl_placed(uv, wp.time);
    // Attach the tail to the existing ocean, leaving the barrel's underside open.
    let dist = length(vec3<f32>(position.x, wp.sea_level, position.z) - frame.cam_pos.xyz);
    var ocean = gerstner_disp(position.xz, dist);
    if (wp.fft_control.x > 0.5) { ocean = fft_geometry_disp(position.xz, dist, 0.0); }
    let tail = 1.0 - smoothstep(0.0, 0.40, uv.x);
    return position + vec3<f32>(0.0, wp.sea_level + 0.04 + ocean.y * max(tail,curl_point(uv,wp.time).collapse), 0.0);
}
@vertex
fn vs_curling_breaker(@builtin(vertex_index) vertex: u32) -> CurlOut {
    var out: CurlOut;
    let corners = array<vec2<f32>,6>(vec2<f32>(0,0),vec2<f32>(1,0),vec2<f32>(0,1),
                                      vec2<f32>(0,1),vec2<f32>(1,0),vec2<f32>(1,1));
    let cells_x = 64u;
    let cells_z = 48u;
    let sheet_vertices = cells_x * cells_z * 6u;
    var world: vec3<f32>;
    if (vertex < sheet_vertices) {
        let cell = vertex / 6u;
        let uv = (vec2<f32>(f32(cell % cells_x), f32(cell / cells_x)) + corners[vertex%6u]) /
                 vec2<f32>(f32(cells_x), f32(cells_z));
        let s = curl_point(uv, wp.time);
        world = curl_world(uv);
        // Derivatives of the actual folded surface, not a heightfield normal.
        let du = curl_point(uv + vec2<f32>(0.001,0.0), wp.time).position -
                 curl_point(uv - vec2<f32>(0.001,0.0), wp.time).position;
        let dv = curl_placed(uv + vec2<f32>(0.0,0.01), wp.time) -
                 curl_placed(uv - vec2<f32>(0.0,0.01), wp.time);
        out.normal = normalize(cross(du,dv) + vec3<f32>(0.0,1e-8,0.0));
        out.data = vec4<f32>(s.coverage, s.collapse, uv.x, 0.0);
        out.uv = uv;
    } else {
        let particle = (vertex-sheet_vertices)/6u;
        let seed = f32(particle);
        let random = fract(sin(vec3<f32>(seed*12.9898,seed*78.233,seed*31.713)+1.0)*43758.5453);
        let uv = vec2<f32>(1.0,random.x);
        let s = curl_point(uv,wp.time);
        let t = s.age - 5.6 - random.y * 0.65;
        let life = smoothstep(0.0,0.12,t) * (1.0-smoothstep(0.8,1.6,t));
        let start = curl_placed(uv,wp.time-max(t,0.0));
        world = start + vec3<f32>(-max(t,0.0)*(1.0+random.z*3.0),
                wp.sea_level + max(t,0.0)*(2.5+random.y*3.0)-4.905*t*t,
                (random.z-0.5)*max(t,0.0)*3.0);
        let corner = corners[(vertex-sheet_vertices)%6u]*2.0-vec2<f32>(1.0);
        let view = frame.view*vec4<f32>(world-frame.cam_pos.xyz,1.0);
        let size = 0.004 + random.y*0.018;
        out.clip = reverse_z(frame.proj*(view+vec4<f32>(corner*size,0.0,0.0)));
        out.normal = vec3<f32>(0,1,0);
        out.data = vec4<f32>(life*s.coverage,1.0,1.0,1.0);
        out.uv = corner;
    }
    out.world_rel = world-frame.cam_pos.xyz;
    if (vertex < sheet_vertices) { out.clip = reverse_z(frame.proj*frame.view*vec4<f32>(out.world_rel,1.0)); }
    // This fixture requires the expected coastal terrain and stays off in other maps.
    if (!seabed_contains(vec2<f32>(7852.0,4582.0)) ||
        abs(seabed_height(vec2<f32>(7852.0,4582.0)) + 3.59631) > 0.35) {
        out.clip = vec4<f32>(2.0,2.0,2.0,1.0);
    }
    return out;
}
@fragment
fn fs_curling_breaker(in: CurlOut, @builtin(front_facing) front: bool) -> @location(0) vec4<f32> {
    let world = in.world_rel + frame.cam_pos.xyz;
    if (in.data.x < 0.005 || world.y < wp.sea_level - 0.05 ||
        seabed_height(world.xz) > world.y - 0.02) { discard; }
    var n = select(-in.normal,in.normal,front);
    let v = normalize(-in.world_rel);
    if (dot(n,v)<0.0) { n = -n; }
    let l = normalize(-frame.sun_dir_world.xyz);
    var sun = frame.sun_diffuse.rgb;
    var ambient = frame.sun_ambient.rgb;
    var fog = frame.fog_color.rgb;
    if (linear > 0.5) {
        if (frame.sun_diffuse.w < 0.5) {sun=srgb_to_linear(sun);ambient=srgb_to_linear(ambient);}
        fog=srgb_to_linear(fog);
    }
    // Material coordinates follow the sheet, rather than vertically projecting
    // world xz onto its nearly vertical face (which made foam into long stripes).
    let material = mix(in.uv * vec2<f32>(14.0,36.0),world.xz-vec2<f32>(7836.6085,4582.0),in.data.y);
    let footprint = max(length(dpdx(material)),length(dpdy(material)));
    let warp = vec2<f32>(vnoise_lod(material*0.53,footprint*0.53),
                         vnoise_lod(material*0.53+vec2<f32>(7.3,2.1),footprint*0.53));
    let flow = material + warp*1.4 + vec2<f32>(wp.time*0.12,0.0);
    let noise = 0.55 * vnoise_lod(flow*1.7,footprint*1.7) +
                0.30 * vnoise_lod(flow*4.3+vec2<f32>(0.37,0.81),footprint*4.3) +
                0.15 * vnoise_lod(flow*11.1,footprint*11.1);
    let lip = smoothstep(0.92,1.0,in.data.z) * 0.55;
    // Aeration starts at impact, not across the entire still-standing wall.
    let impact = in.data.y * (1.0-smoothstep(0.25,1.2,world.y-wp.sea_level));
    let foam = clamp(impact * smoothstep(0.30,0.62,noise) +
                     lip * smoothstep(0.25,0.6,noise),0.0,1.0);
    // The ocean helper deliberately suppresses detail on steep heightfields.
    // Evaluate it in the sheet's tangent frame so the vertical lip keeps ripples.
    let detail = micro_normal(material, length(in.world_rel), 2.0, vec3<f32>(0,1,0), 0.0);
    let axis = select(vec3<f32>(0,0,1),vec3<f32>(1,0,0),abs(n.z)>0.95);
    let tangent = normalize(cross(axis,n));
    let bitangent = cross(n,tangent);
    n = normalize(n*detail.y + tangent*detail.x + bitangent*detail.z);
    let fresnel = 0.02+0.98*pow(1.0-max(dot(n,v),0.0),5.0);
    let reflection = sky_env_sample(reflect(-v,n));
    let sun_visibility = smoothstep(0.0,0.06,l.y) * cloud_sun_shadow(world.xz);
    let inside = select(0.0,1.0,dot(in.normal,v)<0.0);
    let irradiance = ambient * sky_vis_ao(world.xz) * mix(1.0,0.45,inside) +
                     sun * max(dot(n,l),0.0) * sun_visibility * mix(1.0,0.2,inside);
    var shallow = wp.shallow_color.rgb;
    var deep = wp.deep_color.rgb;
    if (linear>0.5) {shallow=srgb_to_linear(shallow);deep=srgb_to_linear(deep);}
    // Same authored swatches and irradiance/pi convention as the existing sea.
    // The refracted ray also traverses the rear water wall; the opaque
    // scene input contains no curling geometry, so include that path here.
    let thickness = 3.0 + mix(2.0,0.35,smoothstep(0.4,1.0,in.data.z));
    let sigma = water_extinction(wp.deep_color.rgb,wp.color_ext,clamp(wp.underwater_params.z,0.0,1.0));
    let path = thickness/max(abs(dot(n,v)),0.25);
    let transmittance = water_transmittance(sigma,path);
    let tint = water_albedo_along_path(shallow,deep,sigma,path);
    let view_n = (frame.view*vec4<f32>(n,0.0)).xy;
    let offset = view_n*clamp(thickness/max(length(in.world_rel),1.0)*0.10,0.002,0.025);
    let uv = clamp(scene_uv(in.clip.xy)+offset,vec2<f32>(0.001),vec2<f32>(0.999));
    let background = textureSampleLevel(scene_color,scene_samp,uv,0.0).rgb;
    // The opaque snapshot cannot represent refraction through a second water
    // boundary. Use the rear water body's radiance behind the folded face;
    // only the very thin lip admits a little of the refracted scene. Otherwise
    // distant hills read as a sharp image pasted through a sheet of glass.
    let rear_body = water_inscatter(deep,ambient*sky_vis_ao(world.xz)+sun*sun_visibility*0.2,vec3<f32>(0.0));
    let thin_lip = smoothstep(0.94,1.0,in.data.z) * (1.0-in.data.y) * 0.12;
    let transmitted = mix(rear_body,background,thin_lip);
    var rgb = mix(transmitted*transmittance+water_inscatter(tint,irradiance,transmittance),
                  reflection,clamp(fresnel*wp.look_params.w,0.0,1.0));
    let foam_light = (ambient + sun * sun_visibility * max(l.y,0.0)) / PI;
    rgb = mix(rgb,vec3<f32>(0.82,0.88,0.86)*foam_light,foam);
    // Transmitted background is already composited above, as in the old ocean.
    // Framebuffer alpha only fades the geometric ends; do not double-blend it.
    var alpha = in.data.x * mix(1.0,smoothstep(0.30,0.48,noise),impact);
    if (in.data.w>0.5) {
        let radial = max(1.0-dot(in.uv,in.uv),0.0);
        alpha = alpha*radial*radial*0.55;
        rgb = vec3<f32>(0.85,0.9,0.88)*foam_light;
        if (alpha<0.02) {discard;}
    }
    rgb = mix(fog,rgb,fog_factor(length(in.world_rel)));
    return vec4<f32>(rgb,alpha);
}
