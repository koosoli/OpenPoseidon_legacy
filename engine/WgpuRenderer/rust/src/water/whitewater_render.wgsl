#import water_shore_surf::{coastal_surf, add_surf_displacement}
// GodotOceanWaves-style sea spray.  The reference uses a GPUParticles3D emitter
// over a 10m grid and lets each particle follow the FFT displacement at its start
// point.  This port uses the same principle, but derives all particle state from the
// instance index and time so it stays entirely GPU resident and needs no readback.

#import frame::{frame, reverse_z, fog_factor}
#import color::srgb_to_linear
#import water_fft_sampling::fft_scaled_uv
#import water_optics::whitecap_wind_factor

const TAU: f32 = 6.28318530718;
const G: f32 = 9.81;
// The emitter is a world-anchored field, not a box stapled to the camera. The previous 10 m
// square meant spray existed only within 5 m of the player and jumped in 10 m steps as they
// walked, so a breaking wave 30 m out was silent and the field visibly teleported. 120 m at
// 128x128 gives ~0.94 m cells, which is fine enough to resolve individual crests.
const EMITTER_SIDE: u32 = 128u;
const EMITTER_SPAN: f32 = 120.0;
// Droplet drag time constant (seconds). Spray has a tiny ballistic coefficient, so it sheds its
// launch velocity and approaches the wind within a fraction of a second — which is why real
// spindrift tears downwind off a crest instead of arcing back into the water it came from.
const DRAG_TAU: f32 = 0.55;
override spray_world_anchor: f32 = 1.0;

struct WaterParams {
    world_origin: vec2<f32>, terrain_grid: f32, sea_level: f32,
    hm_width: u32, hm_height: u32, time: f32,
    wave_amp: f32, wave_choppy: f32, wave_speed: f32, wave_scale: f32,
    fade_start: f32, fade_end: f32, warp_amp: f32, spec_power: f32,
    spec_intensity: f32, alpha: f32, shadow_dim: f32, color_ext: f32,
    coast_fade: f32, shallow_color: vec4<f32>, deep_color: vec4<f32>,
    foam_width: f32, foam_intensity: f32, swash_amp: f32, swash_speed: f32,
    fft_control: vec4<f32>, fft_wind_sea: vec4<f32>,
    fft_cascade_lengths: vec4<f32>, flow_direction_speed: vec4<f32>,
    debug_params: vec4<f32>, look_params: vec4<f32>, sea_params: vec4<f32>,
    underwater_params: vec4<f32>, underwater_gate: vec4<f32>, optics_params: vec4<f32>,
    bodies: array<vec4<f32>,48>,
};

// This is deliberately the same group-1 interface as water.wgsl.  The unused
// bindings remain in the shared layout; spray only needs parameters and the FFT
// displacement/crest fields.
@group(1) @binding(0) var<uniform> wp: WaterParams;
@group(1) @binding(4) var interaction_field: texture_2d<f32>;
@group(1) @binding(5) var interaction_samp: sampler;
@group(1) @binding(6) var fft_displacement: texture_2d_array<f32>;
@group(1) @binding(8) var fft_auxiliary: texture_2d_array<f32>;
@group(1) @binding(9) var fft_samp: sampler;
@group(1) @binding(10) var foam_history: texture_2d<f32>;
@group(1) @binding(11) var foam_samp: sampler;

struct SprayOut {
    @builtin(position) clip: vec4<f32>,
    @location(0) uv: vec2<f32>,
    @location(1) alpha: f32,
    @location(2) fog: f32,
};

fn hash11(x: u32) -> f32 {
    var n = x;
    n = (n ^ (n >> 16u)) * 0x7feb352du;
    n = (n ^ (n >> 15u)) * 0x846ca68bu;
    n = n ^ (n >> 16u);
    return f32(n & 0x00ffffffu) / 16777216.0;
}

fn hash21(x: u32) -> vec2<f32> {
    return vec2<f32>(hash11(x * 1597334677u + 17u), hash11(x * 3812015801u + 53u));
}

fn spray_cell_seed(cell: vec2<i32>) -> u32 {
    let bits = bitcast<vec2<u32>>(cell);
    return bits.x * 1597334677u + bits.y * 3812015801u;
}

fn quad_corner(vertex: u32) -> vec2<f32> {
    // Two triangles: (-1,-1), (1,-1), (-1,1), (-1,1), (1,-1), (1,1).
    let corners = array<vec2<f32>, 6>(
        vec2<f32>(-1.0, -1.0), vec2<f32>(1.0, -1.0), vec2<f32>(-1.0, 1.0),
        vec2<f32>(-1.0, 1.0), vec2<f32>(1.0, -1.0), vec2<f32>(1.0, 1.0));
    return corners[vertex];
}

override surf_pilot: f32 = 0.0;
struct ConformParams { origin: vec2<f32>, terrain_grid: f32, enabled: f32, hm_width: u32, hm_height: u32, };
@group(1) @binding(17) var seabed: texture_2d<f32>;
@group(1) @binding(18) var<uniform> cf: ConformParams;
struct CrestSource {
    displacement: vec3<f32>,
    breaking: f32,
    // Orbital velocity of the water surface at this point (m/s). This is what a droplet actually
    // inherits when it is torn off a crest, and its absence is why spray read as an animated
    // shader layer: every particle left the surface with the same wind-plus-vertical-burst
    // velocity regardless of which way its wave was moving.
    velocity: vec3<f32>,
};

fn crest_source(world_xz: vec2<f32>) -> CrestSource {
    var displacement = vec3<f32>(0.0);
    var velocity = vec3<f32>(0.0);
    var crest = 0.0;
    var compression = 0.0;
    var slope = 0.0;
    for (var layer = 0; layer < 4; layer = layer + 1) {
        let tile = wp.fft_cascade_lengths[layer];
        if (tile > 0.5) {
            let uv = fft_scaled_uv(world_xz, tile, layer, wp.warp_amp, wp.wave_scale);
            let d = textureSampleLevel(fft_displacement, fft_samp, uv, layer, 0.0);
            let a = textureSampleLevel(fft_auxiliary, fft_samp, uv, layer, 0.0);
            displacement += d.xyz * max(wp.wave_scale, 0.01);
            crest = max(crest, d.w);
            compression = max(compression, a.y);
            slope = max(slope, sqrt(max(a.w, 0.0)));
            // Water particles in a linear deep-water wave travel in circles at angular rate
            // omega = sqrt(g*k), so the orbital velocity has the same shape as the displacement
            // scaled by that cascade's omega. Taking k from the cascade's own tile length gives a
            // per-cascade omega for free: short cascades contribute fast, small motion and long
            // swell contributes slow, large motion, which is the correct weighting.
            //
            // This is an approximation, not the exact time derivative (that would need a second
            // inverse transform of -i*omega*h). It gets the direction and the relative magnitude
            // right, which is what the spray needs.
            let k_peak = TAU / max(tile, 1.0);
            let omega = sqrt(G * k_peak);
            velocity += d.xyz * max(wp.wave_scale, 0.01) * omega;
        }
    }
    let domain_origin = floor((frame.cam_pos.xz - vec2<f32>(128.0)) / 4.0) * 4.0;
    let foam_uv = (world_xz - domain_origin) / 256.0;
    let foam_inside = step(0.0, foam_uv.x) * step(0.0, foam_uv.y) *
        step(foam_uv.x, 1.0) * step(foam_uv.y, 1.0);
    let foam = textureSampleLevel(foam_history, foam_samp,
        clamp(foam_uv, vec2<f32>(0.001), vec2<f32>(0.999)), 0.0).r * foam_inside;
    // A whitecap is where the surface folds over on itself, which the FFT reports directly as
    // horizontal compression (a collapsing Jacobian) on a rising crest. The gate used to be
    // `foam history > 0.9` alone, so spray only appeared well after foam had accumulated and
    // saturated — never on a wave breaking for the first time, and only in patches where the
    // history happened to be near full. Reading the break signal at its source fixes the timing
    // and the placement; the history term stays as a secondary source for lingering foam.
    // `compression` and `crest` were already being computed here and thrown away.
    let jacobian_break = smoothstep(0.030, 0.20, compression);
    let crest_break = smoothstep(0.018, 0.075, crest);
    let history_break = smoothstep(0.45, 0.85, foam);
    // A little slope sensitivity lets a steep wind-torn top shed spray before it fully folds.
    let breaking = clamp(max(history_break, jacobian_break * crest_break) + slope * 0.04, 0.0, 1.0);
    var out: CrestSource;
    out.displacement = displacement;
    out.breaking = breaking;
    out.velocity = velocity;
    if (surf_pilot > 0.5) {
        let surf = coastal_surf(seabed, world_xz, cf.origin, cf.terrain_grid, cf.enabled,
                                wp.sea_level, wp.time, 1.0);
        if (surf.weight > 0.0) {
            for (var body=0; body<16; body=body+1) {
                let e=wp.bodies[body*3];
                if (e.z<=0.0 || e.w<=0.0) {continue;}
                let angle=wp.bodies[body*3+2].w;
                let d=world_xz-e.xy;
                let q=vec2<f32>(cos(angle)*d.x+sin(angle)*d.y,-sin(angle)*d.x+cos(angle)*d.y)/e.zw;
                if (dot(q,q)<1.06) {return out;}
            }
        }
        out.displacement = add_surf_displacement(out.displacement, surf);
        out.breaking = max(out.breaking, surf.breaking * 0.35);
        out.velocity = out.velocity + vec3<f32>(surf.flow.x, 0.0, surf.flow.y);
    }
    return out;
}

// The interaction solver follows the camera in a snapped 256m domain.  Reading
// its aeration channel lets vessel wakes and impact splashes use the exact same
// billboard path as wind-torn wave crests, rather than being limited to surface foam.
fn interaction_source(world_xz: vec2<f32>) -> vec4<f32> {
    let domain_origin = floor((frame.cam_pos.xz - vec2<f32>(128.0)) / 4.0) * 4.0;
    let uv = (world_xz - domain_origin) / 256.0;
    let inside = step(0.0, uv.x) * step(0.0, uv.y) * step(uv.x, 1.0) * step(uv.y, 1.0);
    return textureSampleLevel(interaction_field, interaction_samp,
        clamp(uv, vec2<f32>(0.001), vec2<f32>(0.999)), 0.0) * inside;
}

@vertex
fn vs_whitewater(
    @builtin(vertex_index) vertex_index: u32,
    @builtin(instance_index) instance_index: u32,
) -> SprayOut {
    let cell = vec2<f32>(
        f32(instance_index % EMITTER_SIDE), f32(instance_index / EMITTER_SIDE));
    // Snap the anchor to the CELL size, not the whole span: a camera move then slides the field
    // by at most one 0.94 m cell instead of teleporting it a full tile, so a droplet stays
    // attached to the crest that threw it.
    let cell_size = EMITTER_SPAN / f32(EMITTER_SIDE);
    let anchor_cell = floor(frame.cam_pos.xz / cell_size);
    let anchor = anchor_cell * cell_size;
    // Moving the window reassigns instance indices. Jitter AND lifetime must
    // follow the absolute cell, or every spray particle changes at a cell crossing.
    let world_cell = vec2<i32>(anchor_cell) + vec2<i32>(cell) - vec2<i32>(i32(EMITTER_SIDE / 2u));
    let seed = select(instance_index, spray_cell_seed(world_cell), spray_world_anchor > 0.5);
    let random = hash21(seed);
    let local = ((cell + random) / f32(EMITTER_SIDE) - vec2<f32>(0.5)) * EMITTER_SPAN;
    let source_xz = anchor + local;
    // Fade the field out before its own boundary so the wrap is never a visible edge.
    let field_fade = 1.0 - smoothstep(EMITTER_SPAN * 0.34, EMITTER_SPAN * 0.5, length(local));

    // Shorter than the reference's 3 s: a wind wave breaks over about a second, and a droplet
    // that outlives its own breaking event is what made spray blink out mid-flight when the
    // crest moved on.
    let lifetime = mix(1.25, 2.10, hash11(seed * 747796405u + 2891336453u));
    let age = fract(wp.time / lifetime + hash11(seed * 277803737u + 1u));
    let t = age * lifetime; // seconds since this droplet left the surface
    let crest = crest_source(source_xz);
    let interaction = interaction_source(source_xz);
    // Interaction events write aeration into .b and radial impulse velocity into .g.
    // Their physical values are intentionally small, so the old 0.08..0.45 gate
    // discarded normal footsteps, wakes, and impact splashes before any billboard
    // was emitted.  Keep both sources independent: a local impact now makes a
    // visible splash even when the surrounding wind waves are not breaking.
    let interaction_splash = max(
        smoothstep(0.010, 0.090, interaction.b),
        smoothstep(0.060, 0.38, abs(interaction.g)));
    // Water-tab "Water splash particles" switch. Keep the draw allocation stable
    // but make all generated billboards fully transparent when disabled.
    // Low water quality drops spray entirely along with the screen-space reflections.
    let spray_enabled = step(0.5, wp.debug_params.y) * (1.0 - step(0.5, wp.sea_params.z));
    let spray_activity = clamp(wp.debug_params.z, 0.0, 1.0);
    // WRL-002: wind-torn spray needs wind. Impact splashes are local events and stay ungated.
    let wind_gate = select(1.0, whitecap_wind_factor(wp.fft_wind_sea.z), wp.optics_params.y > 0.5);
    let source_strength = max(crest.breaking * wind_gate, interaction_splash) * spray_enabled * spray_activity;

    // Ballistics with air drag, rather than a scripted parabola. Closed-form solution of
    // dv/dt = -g - v/tau:
    //     v(t) = (v0 + g*tau) * exp(-t/tau) - g*tau
    //     y(t) = (v0 + g*tau) * tau * (1 - exp(-t/tau)) - g*tau*t
    // and the same relaxation horizontally, toward the wind velocity instead of toward zero.
    let decay = 1.0 - exp(-t / DRAG_TAU);
    let wind = normalize(wp.fft_wind_sea.xy + vec2<f32>(1e-4, 0.0));
    let wind_vel = wind * max(wp.fft_wind_sea.z, 0.0);

    // The droplet inherits the motion of the water it came from. Previously every particle left
    // with the same upward burst plus wind drift, so spray had no relationship to the direction
    // its wave was travelling — which is what made it read as an effect layer painted over the
    // ocean rather than water torn off a moving crest.
    let surface_vel = crest.velocity;
    // Ejection along the crest's own motion, plus a normal-ish upward component from the break
    // itself and a random cone so a burst opens out instead of moving as a rigid clump.
    let scatter = (random * 2.0 - vec2<f32>(1.0)) * (0.6 + source_strength * 1.8);
    let v0_up = max(surface_vel.y, 0.0) * 0.85 + 0.9 + source_strength * 4.8;
    let v0_horiz = surface_vel.xz * 0.75 + scatter;

    let vert = (v0_up + G * DRAG_TAU) * DRAG_TAU * decay - G * DRAG_TAU * t;
    let horiz = wind_vel * t + (v0_horiz - wind_vel) * DRAG_TAU * decay;

    let crest_y = wp.sea_level + crest.displacement.y + interaction.r * 2.5;
    let world = vec3<f32>(
        source_xz.x + crest.displacement.x * 0.75 + horiz.x,
        crest_y + max(vert, 0.0),
        source_xz.y + crest.displacement.z * 0.75 + horiz.y,
    );
    let world_rel = world - frame.cam_pos.xyz;
    let corner = quad_corner(vertex_index);
    // Individual droplets, not puffs. The previous 0.09..0.49 m billboards (growing to ~0.9 m by
    // end of life) read as blobs of cotton wool sitting on the water; spray at this distance is
    // a fine mist of centimetre-scale specks. Sizes here are 1.5..7 cm, growing only slightly as
    // the cloud disperses.
    let size = (0.014 + source_strength * 0.052) * (0.70 + t * 0.30);
    // Form the quad in view space: this is exact camera billboarding without
    // depending on a CPU-side emitter transform.
    let view_pos = frame.view * vec4<f32>(world_rel, 1.0);
    let billboard = view_pos + vec4<f32>(corner * size, 0.0, 0.0);
    var out: SprayOut;
    out.clip = reverse_z(frame.proj * billboard);
    out.uv = corner * 0.5 + vec2<f32>(0.5);
    // Rises as it is thrown clear, thins as it disperses, and dies when it falls back into the
    // surface (vert going negative) rather than popping out at a fixed age.
    let rise = smoothstep(0.0, 0.10, t);
    let settle = smoothstep(-0.15, 0.20, vert);
    let disperse = 1.0 - smoothstep(0.55, 1.0, age);
    out.alpha = source_strength * rise * settle * disperse * field_fade;
    out.fog = fog_factor(length(world_rel));
    return out;
}

override linear: f32 = 0.0;

@fragment
fn fs_whitewater(in: SprayOut) -> @location(0) vec4<f32> {
    // A procedural soft droplet replaces the reference PNG atlas, avoiding an
    // additional asset/load path while retaining its rounded, dissolving silhouette.
    let q = in.uv * 2.0 - vec2<f32>(1.0);
    let radial = max(1.0 - dot(q, q), 0.0);
    // A tight falloff reads as a droplet; the old wide, animated wisp read as a soft puff, which
    // is what made the spray look like blobs however small the quad got.
    let droplet = pow(radial, 3.0);
    let alpha = in.alpha * droplet * 0.85;
    if (alpha < 0.004) { discard; }
    // Spray is suspended water droplets, not an emissive decal. Lighting it with the same sun
    // and ambient the surface uses keeps it in the scene at dawn, dusk and under overcast,
    // instead of staying a constant bright white that floats off the water at every hour.
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
    // Droplet clouds scatter forward strongly, so most of their brightness is sun transmitted
    // through the cloud rather than a surface reflection — hence a plain diffuse fraction with
    // no view dependence and no specular lobe.
    let albedo = vec3<f32>(0.90, 0.94, 0.97);
    var colour = (sun_ambient + sun_diffuse * 0.55) * albedo;
    if (linear <= 0.5) { colour = min(colour, vec3<f32>(1.0)); }
    // fog_factor above uses the same camera-relative distance as the water path.
    colour = mix(fog_color, colour, in.fog);
    return vec4<f32>(colour, alpha);
}
