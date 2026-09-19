#define_import_path water_curling_breaker

// Authored local prototype, not a fluid solver. Coordinates are metres relative
// to one fixed Nogova beach. No camera-following phase or authoritative state.
struct CurlPoint {
    position: vec3<f32>,
    collapse: f32,
    coverage: f32,
    age: f32,
};
fn curl_point(uv: vec2<f32>, time: f32) -> CurlPoint {
    let along = (uv.y - 0.5) * 36.0;
    let age = fract((time + along * 0.035) / 10.0) * 10.0;
    let grow = smoothstep(0.0, 1.6, age);
    let crash = smoothstep(5.5 + (1.0 - uv.x) * 0.65, 7.2, age);
    let fade = 1.0 - smoothstep(8.1, 9.8, age);
    let tip = mix(2.6, 5.85, smoothstep(2.0, 5.7, age));
    let radius = 1.35 * grow * (1.0 + 0.055 * sin(along * 0.53) + 0.025 * sin(along * 1.37));
    let q = clamp((uv.x - 0.38) / 0.62, 0.0, 1.0);
    let theta = mix(1.57079633, tip, q);
    var forward = -radius * 1.25 * sin(theta);
    var height = radius * (1.0 - cos(theta));
    if (uv.x < 0.38) {
        let t = uv.x / 0.38;
        forward = mix(-8.0, -radius * 1.25, sin(t * 1.57079633));
        height = radius * (1.0 - cos(t * 1.57079633));
    }
    // The lip passes beyond vertical (theta > 3*pi/2), then drops and spreads.
    // The wave sheet has multiple heights at the same horizontal coordinate.
    height -= 0.14 * pow(q, 8.0) * smoothstep(3.5, 5.4, age);
    let wash_forward = mix(-8.0, 5.0, uv.x);
    forward = mix(forward, wash_forward, crash);
    let turbulence = (sin(uv.x * 39.0 + along * 2.2 - age * 9.0) +
                      sin(uv.x * 63.0 - along * 1.7 + age * 7.0)) * 0.055;
    height = mix(height, 0.12 + turbulence, crash);
    let ends = smoothstep(0.0, 0.12, uv.y) * (1.0 - smoothstep(0.88, 1.0, uv.y));
    // Fade coverage at the lateral ends, not the curl height: collapsing the
    // cross-section there sealed the barrel opening with a sloping curtain.
    let leading = smoothstep(0.0, 0.06, uv.x);
    var s: CurlPoint;
    // Westward arrival: x decreases. Arrival at roughly the 2m contour.
    let crest_x = 7861.0 - min(age, 7.2) * 2.35;
    s.position = vec3<f32>(crest_x - forward, height * fade, 4582.0 + along);
    s.collapse = crash;
    s.coverage = grow * fade * ends * leading;
    s.age = age;
    return s;
}
