const MAX_EVENTS: u32 = 48u;
const BULLET: f32 = 0.0;
const OBJECT: f32 = 1.0;
const PLAYER: f32 = 2.0;
const EXPLOSION: f32 = 3.0;
const FOOTSTEP: f32 = 4.0;
const CONTINUOUS: f32 = 5.0;

struct InteractionEvent { position_radius: vec4<f32>, velocity_kind: vec4<f32>, time_life_foam_mass: vec4<f32>, direction_depth_flags: vec4<f32>, };
struct InteractionParams { domain: vec4<f32>, previous_domain: vec4<f32>, grid: vec4<f32>, physics: vec4<f32>, misc: vec4<f32>, weather: vec4<f32>, };
@group(0) @binding(0) var<uniform> params: InteractionParams;
@group(0) @binding(1) var<storage, read> events: array<InteractionEvent, 48>;
@group(0) @binding(2) var previous_field: texture_2d<f32>;
@group(0) @binding(3) var field_sampler: sampler;
@group(0) @binding(4) var next_field: texture_storage_2d<rgba16float, write>;

fn sample_previous(uv: vec2<f32>) -> vec4<f32> {
    let inside = step(0.0, uv.x) * step(0.0, uv.y) * step(uv.x, 1.0) * step(uv.y, 1.0);
    return textureSampleLevel(previous_field, field_sampler, clamp(uv, vec2<f32>(0.001), vec2<f32>(0.999)), 0.0) * inside;
}
fn hash2(p: vec2<f32>) -> f32 { return fract(sin(dot(p, vec2<f32>(127.1, 311.7))) * 43758.5453); }
fn safe_normalize_vec2(v: vec2<f32>) -> vec2<f32> {
    let len_sq = dot(v, v);
    return select(vec2<f32>(0.0, 1.0), v * inverseSqrt(max(len_sq, 1e-8)), len_sq > 1e-8);
}
fn type_mask(value: f32, expected: f32) -> f32 { return 1.0 - step(0.45, abs(value - expected)); }

@compute @workgroup_size(8, 8, 1)
fn interaction_update(@builtin(global_invocation_id) id: vec3<u32>) {
    let dimensions = textureDimensions(next_field);
    if (any(id.xy >= dimensions)) { return; }
    if (params.grid.w > 0.5) { textureStore(next_field, vec2<i32>(id.xy), vec4<f32>(0.0)); return; }
    let size = vec2<f32>(dimensions);
    let texel = 1.0 / size;
    let uv = (vec2<f32>(id.xy) + 0.5) / size;
    let world = params.domain.xy + uv * params.domain.z;
    let back_uv = (world - params.previous_domain.xy) * params.previous_domain.w;
    let centre = sample_previous(back_uv);
    let h_l = sample_previous(back_uv - vec2<f32>(texel.x, 0.0)).r;
    let h_r = sample_previous(back_uv + vec2<f32>(texel.x, 0.0)).r;
    let h_d = sample_previous(back_uv - vec2<f32>(0.0, texel.y)).r;
    let h_u = sample_previous(back_uv + vec2<f32>(0.0, texel.y)).r;
    let dt = clamp(params.grid.y, 0.0, 0.033);
    let calmness = clamp(params.weather.y, 0.0, 1.0);
    let cell = params.domain.z / max(size.x, 1.0);
    let laplacian = (h_l + h_r + h_d + h_u - 4.0 * centre.r) / max(cell * cell, 0.001);
    var velocity = centre.g + laplacian * params.physics.x * params.physics.x * dt * calmness;
    velocity = velocity * exp(-(params.physics.y + (1.0 - calmness) * 0.72) * dt);
    var height = centre.r + velocity * dt;
    var foam = centre.b * exp(-params.physics.w * dt);
    let event_count = min(u32(params.grid.z + 0.5), MAX_EVENTS);
    for (var index: u32 = 0u; index < event_count; index = index + 1u) {
        let event = events[index];
        let flags = u32(max(event.direction_depth_flags.w, 0.0) + 0.5);
        if ((flags & 1u) == 0u) { continue; }
        let delta = world - event.position_radius.xy;
        let radius = max(event.position_radius.z, cell * 1.25);
        // WTR-062 — Bounded event dispatch optimization:
        // Skip texel calculations for events whose radius box does not overlap the current position.
        if (abs(delta.x) > radius * 2.5 || abs(delta.y) > radius * 2.5) { continue; }
        let direction = normalize(event.direction_depth_flags.xy + event.velocity_kind.xy + vec2<f32>(0.001, 0.0));
        let capsule = (flags & (1u << 8u)) != 0u;
        let along = clamp(dot(delta, direction), -radius, radius);
        let capsule_distance = length(delta - direction * along);
        let distance = select(length(delta), capsule_distance, capsule);
        let q = distance / radius;
        if (q > 2.45) { continue; }
        let core = exp(-q * q * 5.0);
        let ring = exp(-((q - 0.88) * (q - 0.88)) * 20.0);
        let kind = event.velocity_kind.w;
        let bullet = type_mask(kind, BULLET);
        let object = type_mask(kind, OBJECT);
        let player = type_mask(kind, PLAYER);
        let explosion = type_mask(kind, EXPLOSION);
        let footstep = type_mask(kind, FOOTSTEP);
        let continuous = type_mask(kind, CONTINUOUS);
        let entry = clamp(abs(event.velocity_kind.z) / 8.0, 0.22, 2.0);

        // WTR-072 — Directional footstep & wading bow wave bias
        let move_dot = dot(safe_normalize_vec2(delta), direction);
        let footstep_bias = 1.0 + move_dot * 0.45;

        // WTR-074 — Vessel Kelvin wake V-shaped wedge (19.47 degrees, sin ~ 0.333)
        let perp_dir = vec2<f32>(-direction.y, direction.x);
        let lateral_dist = abs(dot(delta, perp_dir));
        let kelvin_angle_sin = lateral_dist / max(distance, 0.01);
        let wake_behind = smoothstep(0.0, radius * 0.18, -dot(delta, direction));
        let large_body = select(0.0, 1.0, (flags & (1u << 12u)) != 0u);
        let kelvin_wedge = smoothstep(0.38, 0.28, kelvin_angle_sin) * wake_behind;
        let kelvin_wake = continuous * large_body * kelvin_wedge *
            (sin(q * 14.0) * 0.75 - core * 1.1);

        let capillary_ring1 = exp(-((q - 0.55) * (q - 0.55)) * 30.0);
        let capillary_ring2 = exp(-((q - 1.25) * (q - 1.25)) * 18.0);
        let capillary_ring3 = exp(-((q - 1.70) * (q - 1.70)) * 12.0);
        let bullet_pulse = bullet * (-core * 4.50 + ring * 4.00 - capillary_ring1 * 2.80 + capillary_ring2 * 2.20 - capillary_ring3 * 1.50);
        let object_pulse = object * (-core * 1.65 + ring * 2.05) + kelvin_wake;
        let player_pulse = player * (-core * 1.10 + ring * 1.38) * footstep_bias;
        let explosion_pulse = explosion * (-core * 4.50 + ring * 6.50); // Deep cavity + central rebound column
        let footstep_pulse = footstep * (-core * 0.32 + ring * 0.42) * footstep_bias;
        let continuous_pulse = continuous * (core * 0.16 + ring * 0.12) + kelvin_wake;

        let pulse = bullet_pulse + object_pulse + player_pulse + explosion_pulse + footstep_pulse + continuous_pulse;
        velocity = velocity + pulse * event.position_radius.w * entry * mix(0.25, 1.0, calmness * calmness);
        height = height + (bullet_pulse + explosion_pulse * 0.6 + object_pulse * 0.3) * event.position_radius.w * 0.35;
        foam = max(foam, clamp(bullet * (core * 3.5 + ring * 2.0) + explosion * (core * 5.0 + ring * 3.0) + (core + ring) * event.time_life_foam_mass.z * event.position_radius.w * 0.35, 0.0, 1.0));
    }
    // Rain strikes: a hashed subset of cells takes a small crater + outward
    // kick each sim tick, scaled by the live rain density. This is the bullet
    // splash's shape (a depression the wave equation blooms into a ring) at a
    // fraction of the scale.
    //
    // Two things kept this invisible in practice. First, the density fed here
    // is the weather's, which random-walks up to a ceiling of overcast * 1.5 - 1
    // and in a normal storm sits around 0.2-0.4 -- so a term scaled LINEARLY by
    // it ran at a fifth of what it was tuned for while the drops you could see
    // fell at full strength. sqrt() is the response curve that fixes that: 0.25
    // density now reads as half intensity rather than a quarter, and full rain
    // is unchanged. Second, the tick index is derived from absolute scene time,
    // which after an hour of play is large enough that sin()-based hashing in
    // f32 starts losing the low bits that make consecutive ticks differ; the
    // wrap keeps it in a range where the hash stays a hash.
    //
    // weather.z is the Weather tab's gain. The whole term is a hashed branch in
    // a compute pass that already visits every texel of the field every frame,
    // so none of this costs measurable time.
    let rain_density = clamp(params.weather.x, 0.0, 1.0);
    if (rain_density > 0.0005 && calmness > 0.025) {
        let rain = sqrt(rain_density) * max(params.weather.z, 0.0);
        let rain_cell = floor(world * 1.6);
        let rain_tick = floor(params.misc.y * 30.0 % 8192.0);
        let strike = hash2(rain_cell + vec2<f32>(rain_tick, rain_tick * 0.773));
        if (strike > 1.0 - rain * 0.08) {
            // velocity here is the field's SCALAR state channel, not a vector:
            // an impulsive kick plus a small depression is exactly what the
            // bullet splash injects, and the wave equation blooms both into
            // expanding rings. Magnitudes stay a fraction of a bullet's (its
            // pulse peaks near 4.5).
            //
            // AMPLITUDE IS BOUNDED BY THE FOAM PASS, not by taste. foam.wgsl
            // derives its own foam from this field's height laplacian
            // (`convergence`) and slope flow, so a deep crater on a 1 m cell
            // injects foam whether or not the strike asks for any -- measured
            // on Everon at density 1.0, a -0.08 crater dressed the whole sea in
            // tan mottling that read as grit, and cutting the strike's OWN foam
            // term did nothing because that was never the source. Halving the
            // crater and kick is what actually quiets it. The Weather tab's
            // gain (weather.z, default 1) scales all of this: dial it up for a
            // storm sea and the foam comes back with it, which is correct.
            //
            // Note the field is 256 texels over a 256 m domain, so one "drop"
            // here is a metre across. This is rain-driven surface texture at
            // the resolution the field can represent, not raindrop rings.
            let kick = (hash2(rain_cell * 1.71 + vec2<f32>(4.7, 9.3)) - 0.5) * 0.7 * rain;
            velocity = velocity + kick;
            height = height - 0.035 * rain;
            foam = max(foam, 0.04 * rain);
        }
    }
    let edge = min(min(uv.x, uv.y), min(1.0 - uv.x, 1.0 - uv.y));
    let edge_damp = smoothstep(0.0, 0.045, edge);
    height = clamp(height * edge_damp, -params.physics.z, params.physics.z);
    textureStore(next_field, vec2<i32>(id.xy), vec4<f32>(height, velocity * edge_damp, foam, 0.0));
}
