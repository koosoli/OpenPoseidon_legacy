#import water_fft_sampling::fft_aperiodic_uv
#import water_optics::whitecap_wind_factor

struct InteractionParams { domain: vec4<f32>, previous_domain: vec4<f32>, grid: vec4<f32>, physics: vec4<f32>, misc: vec4<f32>, weather: vec4<f32>, };
struct WaterParams { world_origin: vec2<f32>, terrain_grid: f32, sea_level: f32, hm_width: u32, hm_height: u32, time: f32, wave_amp: f32, wave_choppy: f32, wave_speed: f32, wave_scale: f32, fade_start: f32, fade_end: f32, warp_amp: f32, spec_power: f32, spec_intensity: f32, alpha: f32, shadow_dim: f32, color_ext: f32, coast_fade: f32, shallow_color: vec4<f32>, deep_color: vec4<f32>, foam_width: f32, foam_intensity: f32, swash_amp: f32, swash_speed: f32, fft_control: vec4<f32>, fft_wind_sea: vec4<f32>, fft_cascade_lengths: vec4<f32>, flow_direction_speed: vec4<f32>, debug_params: vec4<f32>, look_params: vec4<f32>, sea_params: vec4<f32>, underwater_params: vec4<f32>, underwater_gate: vec4<f32>, optics_params: vec4<f32>, };
@group(0) @binding(0) var<uniform> interaction_params: InteractionParams;
@group(0) @binding(1) var<uniform> water: WaterParams;
@group(0) @binding(2) var previous_foam: texture_2d<f32>;
@group(0) @binding(3) var fft_displacement: texture_2d_array<f32>;
@group(0) @binding(4) var fft_auxiliary: texture_2d_array<f32>;
@group(0) @binding(5) var interaction_field: texture_2d<f32>;
@group(0) @binding(6) var field_sampler: sampler;
@group(0) @binding(7) var next_foam: texture_storage_2d<rgba16float, write>;
struct CascadeConfig {
    enabled: u32, resolution: u32, tile_length_x: f32, tile_length_y: f32,
    displacement_scale: f32, horiz_displacement_scale: f32, normal_scale: f32, foam_scale: f32,
    wind_speed: f32, wind_direction_rad: f32, fetch_meters: f32, water_depth_meters: f32,
    swell: f32, directional_spread: f32, short_wave_detail: f32, whitecap_threshold: f32,
    spectrum_seed: u32, phase_offset_seconds: f32, update_rate_hz: f32, pad: f32,
};
struct CascadeConfigs { config: array<CascadeConfig, 4>, };
@group(0) @binding(8) var<uniform> cascades: CascadeConfigs;

fn sample_history(uv: vec2<f32>) -> vec4<f32> {
    let inside = step(0.0, uv.x) * step(0.0, uv.y) * step(uv.x, 1.0) * step(uv.y, 1.0);
    return textureSampleLevel(previous_foam, field_sampler, clamp(uv, vec2<f32>(0.001), vec2<f32>(0.999)), 0.0) * inside;
}
// WTR-080 — foam diffusion.
//
// Foam is a material, not a decal. Bubbles push each other apart, so a patch widens and its edge
// softens as it ages. Without this the field only ever advects and decays, keeping whatever shape
// it was injected with — which is why the foam read as sharp procedural noise sitting on the water
// rather than something spreading through it.
//
// The kernel is deliberately anisotropic: a raft of foam is drawn out ALONG the current carrying
// it and stays narrow across it. That directionality is what keeps this from looking like a
// uniform gaussian blur, which is the usual failure mode of foam diffusion.
fn diffuse_history(centre_uv: vec2<f32>, texel: vec2<f32>, flow: vec2<f32>, radius: f32) -> vec4<f32> {
    let along = normalize(flow + vec2<f32>(1e-4, 0.0));
    let across = vec2<f32>(-along.y, along.x);
    let ra = along * texel * radius * 2.1;
    let rc = across * texel * radius * 0.9;
    var sum = sample_history(centre_uv) * 4.0;
    sum = sum + sample_history(centre_uv + ra);
    sum = sum + sample_history(centre_uv - ra);
    sum = sum + sample_history(centre_uv + rc);
    sum = sum + sample_history(centre_uv - rc);
    sum = sum + sample_history(centre_uv + ra * 0.6 + rc * 0.6);
    sum = sum + sample_history(centre_uv - ra * 0.6 + rc * 0.6);
    sum = sum + sample_history(centre_uv + ra * 0.6 - rc * 0.6);
    sum = sum + sample_history(centre_uv - ra * 0.6 - rc * 0.6);
    return sum / 12.0;
}

fn fft_source(world: vec2<f32>) -> f32 {
    if (water.fft_control.x <= 0.5) { return 0.0; }
    // WRL-002: whitecaps need wind, not just crest geometry (Water tab "Whitecaps need wind").
    let wind_gate = select(1.0, whitecap_wind_factor(water.fft_wind_sea.z), water.optics_params.y > 0.5);
    var source = 0.0;
    for (var layer: i32 = 0; layer < 4; layer = layer + 1) {
        let cfg = cascades.config[u32(layer)];
        if (cfg.enabled == 0u) { continue; }
        let scale = max(water.fft_cascade_lengths[layer], 1.0);
        let uv = fft_aperiodic_uv(world, scale, layer, water.warp_amp);
        let auxiliary = textureSampleLevel(fft_auxiliary, field_sampler, uv, layer, 0.0);
        // Exact fft_unpack.glsl whitecap source:
        //   foam_factor = -min(0, jacobian - whitecap)
        //   grow_rate = dt * foam_amount * 7.5
        // Returning a per-second injection rate lets the history pass preserve the
        // reference threshold instead of whitening every mildly compressed texel.
        let foam_factor = max(cfg.whitecap_threshold - auxiliary.x, 0.0);
        source = max(source, foam_factor * max(cfg.foam_scale, 0.0) * 7.5 * wind_gate);
    }
    return source;
}

@compute @workgroup_size(8, 8, 1)
fn foam_update(@builtin(global_invocation_id) id: vec3<u32>) {
    let dimensions = textureDimensions(next_foam);
    if (any(id.xy >= dimensions)) { return; }
    if (interaction_params.grid.w > 0.5) { textureStore(next_foam, vec2<i32>(id.xy), vec4<f32>(0.0)); return; }
    let size = vec2<f32>(dimensions);
    let uv = (vec2<f32>(id.xy) + 0.5) / size;
    let world = interaction_params.domain.xy + uv * interaction_params.domain.z;
    let dt = clamp(interaction_params.grid.y, 0.0, 0.033);
    let texel = vec2<f32>(1.0) / size;

    // Sample current interaction velocity field and central differences for WTR-082 divergence & vorticity
    let interaction_uv = (world - interaction_params.domain.xy) * interaction_params.domain.w;
    let centre_inter = textureSampleLevel(interaction_field, field_sampler, clamp(interaction_uv, vec2<f32>(0.001), vec2<f32>(0.999)), 0.0);
    let inter_l = textureSampleLevel(interaction_field, field_sampler, clamp(interaction_uv - vec2<f32>(texel.x, 0.0), vec2<f32>(0.001), vec2<f32>(0.999)), 0.0);
    let inter_r = textureSampleLevel(interaction_field, field_sampler, clamp(interaction_uv + vec2<f32>(texel.x, 0.0), vec2<f32>(0.001), vec2<f32>(0.999)), 0.0);
    let inter_d = textureSampleLevel(interaction_field, field_sampler, clamp(interaction_uv - vec2<f32>(0.0, texel.y), vec2<f32>(0.001), vec2<f32>(0.999)), 0.0);
    let inter_u = textureSampleLevel(interaction_field, field_sampler, clamp(interaction_uv + vec2<f32>(0.0, texel.y), vec2<f32>(0.001), vec2<f32>(0.999)), 0.0);

    let cell_spacing = max(interaction_params.domain.z / size.x, 0.01);
    // The interaction field's .g channel is ONE SCALAR (vertical/impulse velocity). It was being
    // read as if it were the x component of the velocity in one difference and the y component in
    // the other, so "divergence" and "vorticity" were differences of the same scalar along two axes
    // — not the divergence and curl of any actual vector field.
    //
    // The physically meaningful 2D flow here is the down-slope flow of the ripple height (.r):
    // water accelerates away from a crest and toward a trough, so the horizontal velocity is
    // proportional to -grad(h). Its divergence is then -laplacian(h), which is exactly the
    // convergence signal foam wants (foam gathers where the surface flow piles up).
    let dh_dx = (inter_r.r - inter_l.r) / (2.0 * cell_spacing);
    let dh_dz = (inter_u.r - inter_d.r) / (2.0 * cell_spacing);
    let ripple_flow = -vec2<f32>(dh_dx, dh_dz) * 1.5;
    let laplacian_h = (inter_r.r + inter_l.r + inter_u.r + inter_d.r - 4.0 * centre_inter.r) /
        (cell_spacing * cell_spacing);
    let divergence = -laplacian_h * 1.5;
    let convergence = smoothstep(0.0, -0.6, divergence);
    // A height-derived flow is a gradient field, so its true curl is zero. Use the flow magnitude as
    // the "churn" signal the foam shader actually consumes from this channel instead of a curl that
    // cannot exist.
    let vorticity = length(ripple_flow);

    // WTR-085 — Composite physical foam advection velocity (interaction velocity + ambient current + wind drift)
    let flow_dir = normalize(water.flow_direction_speed.xy + vec2<f32>(1e-4, 0.0)) * max(water.flow_direction_speed.z, 0.0);
    let wind_dir = normalize(water.fft_wind_sea.xy + vec2<f32>(1e-4, 0.0));
    let wind_drift = wind_dir * (0.08 + max(water.fft_wind_sea.z, 0.0) * 0.010);
    // Was `vec2<f32>(centre_inter.g * 0.4)`, which fills BOTH components with the same scalar — so
    // every ripple advected foam along a perfect 45-degree diagonal regardless of the actual flow.
    // The ripple's real horizontal flow is the down-slope term computed above.
    let surface_velocity = ripple_flow * 0.4 + flow_dir + wind_drift;

    let previous_world = world - surface_velocity * dt;
    let previous_uv = (previous_world - interaction_params.previous_domain.xy) * interaction_params.previous_domain.w;
    let sharp_history = sample_history(previous_uv);
    let spread_history = diffuse_history(previous_uv, texel, surface_velocity, 1.6);
    // Per-channel diffusion rates, because the three foam types physically behave differently:
    // aerated breaker foam spreads fast, a vessel wake holds a defined line far longer (it is
    // continuously re-injected along the hull path, so smearing it destroys the shape that makes
    // it readable), and suspended air disperses fastest of all.
    let history = vec4<f32>(
        mix(sharp_history.r, spread_history.r, 0.35),
        mix(sharp_history.g, spread_history.g, 0.22),
        mix(sharp_history.b, spread_history.b, 0.70),
        sharp_history.a);

    // WTR-081 — Physical breaking energy and interaction aeration injection
    let fft_breaker = fft_source(world);
    let aeration = clamp(centre_inter.b, 0.0, 1.0);
    let wake_breaker = clamp(convergence * 0.35 + centre_inter.g * 0.15 + aeration, 0.0, 1.0);

    // Dissipation and decay rates. The rate now depends on coverage: a thin, unreplenished film
    // ruptures far faster than a thick fresh raft, so foam dissolves with an accelerating tail
    // instead of fading uniformly. This is the ageing behaviour, obtained without spending a
    // texture channel on an explicit age (all four are already in use).
    let breaker_decay = history.r * exp(-dt * mix(5.2, 2.6, clamp(history.r, 0.0, 1.0)));
    let wake_decay = history.g * exp(-dt * mix(4.0, 2.2, clamp(history.g, 0.0, 1.0)));
    let aeration_decay = history.b * exp(-dt * mix(6.0, 3.4, clamp(history.b, 0.0, 1.0)));

    let breaker_injection = 1.0 - exp(-fft_breaker * dt);
    let wake_injection = 1.0 - exp(-wake_breaker * dt * 1.50);

    let breaker_raw = 1.0 - (1.0 - breaker_decay) * (1.0 - breaker_injection);
    let wake_raw = 1.0 - (1.0 - wake_decay) * (1.0 - wake_injection);
    let air_entrainment = max(aeration_decay, aeration);
    // Diffusion on its own turns foam into a soft grey wash — the exact "blurred texture pasted on
    // the ocean" failure. Re-applying a mild S-curve restores a defined edge at the new, wider
    // extent, so the result reads as foam that has spread rather than foam that has been blurred.
    // smoothstep's own polynomial preserves 0 and 1 exactly, so downstream thresholds in the water
    // shader keep their meaning.
    let breaker_foam = breaker_raw * breaker_raw * (3.0 - 2.0 * breaker_raw);
    let wake_foam = wake_raw * wake_raw * (3.0 - 2.0 * wake_raw);

    let edge = min(min(uv.x, uv.y), min(1.0 - uv.x, 1.0 - uv.y));
    let edge_mask = smoothstep(0.002, 0.018, edge);

    textureStore(next_foam, vec2<i32>(id.xy), vec4<f32>(
        breaker_foam * edge_mask,
        wake_foam * edge_mask,
        air_entrainment * edge_mask,
        clamp(vorticity, 0.0, 1.0) * edge_mask
    ));
}
