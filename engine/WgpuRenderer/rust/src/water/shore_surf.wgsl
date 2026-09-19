#define_import_path water_shore_surf

// Opt-in bounded render-only spilling experiment. Constant incoming phase is differentiated
// in a fixed world-relative frame, never dot(world, varying shore direction).
struct SurfState {
    height: f32,
    slope: vec2<f32>,
    breaking: f32,
    wash: f32,
    wetness: f32,
    weight: f32,
    flow: vec2<f32>,
};
fn surf_cycle(p: vec2<f32>, time: f32, depth: f32, shoreward: vec2<f32>,
              slope: f32, exposure: f32, gain: f32) -> SurfState {
    var s: SurfState;
    let direction = vec2<f32>(-1.0, 0.0);
    let k = 0.18;
    let omega = sqrt(9.81 * k);
    let phase = dot(p, direction) * k - omega * time;
    let beach = smoothstep(0.005, 0.025, slope) * (1.0 - smoothstep(0.25, 0.65, slope));
    let approach = smoothstep(0.15, 0.65, dot(shoreward, direction));
    let shallow = (1.0 - smoothstep(2.0, 6.0, depth)) * smoothstep(-0.12, 0.3, depth);
    s.weight = beach * approach * clamp(exposure, 0.0, 1.0) * shallow * clamp(gain, 0.0, 1.0);
    // Bounded 0.30m local contribution; preserve the existing ocean displacement.
    // Weight the contribution once, never crossfade away the underlying swell.
    let amplitude = 0.30 * s.weight;
    s.height = amplitude * (sin(phase) + 0.18 * sin(2.0 * phase));
    s.slope = direction * amplitude * k * (cos(phase) + 0.36 * cos(2.0 * phase));
    let crest = smoothstep(0.55, 0.95, sin(phase));
    let depth_break = 1.0 - smoothstep(0.45, 1.5, depth);
    s.breaking = crest * depth_break * s.weight;
    // Elapsed phase since a crest passed this point; wash trails that arrival.
    let age = fract((1.57079632679 - phase) / 6.28318530718) * 6.28318530718 / omega;
    let arrival = (1.0 - exp(-age * 5.0)) * exp(-age * 1.2);
    s.wash = 0.12 * arrival * s.weight;
    s.wetness = exp(-age * 0.35) * s.weight;
    s.flow = shoreward * (arrival * 0.65 - (1.0 - exp(-age)) * 0.10) * s.weight;
    return s;
}

fn surf_bed(hm: texture_2d<f32>, p: vec2<f32>, origin: vec2<f32>, grid: f32) -> f32 {
    let q = (p - origin) / max(grid, 0.01);
    let b = vec2<i32>(floor(q));
    let f = fract(q);
    let hi = vec2<i32>(textureDimensions(hm)) - vec2<i32>(1);
    let h00 = textureLoad(hm, clamp(b, vec2<i32>(0), hi), 0).x;
    let h10 = textureLoad(hm, clamp(b + vec2<i32>(1,0), vec2<i32>(0), hi), 0).x;
    let h01 = textureLoad(hm, clamp(b + vec2<i32>(0,1), vec2<i32>(0), hi), 0).x;
    let h11 = textureLoad(hm, clamp(b + vec2<i32>(1,1), vec2<i32>(0), hi), 0).x;
    return mix(mix(h00,h10,f.x),mix(h01,h11,f.x),f.y);
}

fn coastal_surf(hm: texture_2d<f32>, p: vec2<f32>, origin: vec2<f32>, grid: f32,
                enabled: f32, level: f32, time: f32, gain: f32) -> SurfState {
    var empty: SurfState;
    if (enabled < 0.5 || gain <= 0.0) { return empty; }
    let extent = vec2<f32>(textureDimensions(hm) - vec2<u32>(1)) * grid;
    let local = p - origin;
    if (any(local < vec2<f32>(grid)) || any(local > extent - vec2<f32>(grid))) { return empty; }
    let depth = level - surf_bed(hm,p,origin,grid);
    if (depth < -0.12 || depth > 6.0) { return empty; }
    let d = max(grid, 1.0);
    let grad = vec2<f32>(surf_bed(hm,p+vec2<f32>(d,0.0),origin,grid)-surf_bed(hm,p-vec2<f32>(d,0.0),origin,grid),
                         surf_bed(hm,p+vec2<f32>(0.0,d),origin,grid)-surf_bed(hm,p-vec2<f32>(0.0,d),origin,grid))/(2.0*d);
    // Conservative up-wave visibility: block a headland, do not bend swell around it.
    var exposure = 1.0;
    for (var i=1; i<=4; i=i+1) {
        let upstream = p + vec2<f32>(f32(i)*32.0, 0.0);
        if (upstream.x <= origin.x + extent.x) {
            exposure = exposure * smoothstep(-0.1, 0.5, level-surf_bed(hm,upstream,origin,grid));
        }
    }
    return surf_cycle(local,time,depth,normalize(grad+vec2<f32>(1e-6,0.0)),length(grad),exposure,gain);
}

// Keep the resolved ocean field intact. A full-strength coastal mask must not
// replace metre-scale swell with the bounded local surf contribution.
fn add_surf_displacement(ocean: vec3<f32>, surf: SurfState) -> vec3<f32> {
    return ocean + vec3<f32>(0.0, surf.height, 0.0);
}
