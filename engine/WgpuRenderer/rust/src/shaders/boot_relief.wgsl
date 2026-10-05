#define_import_path boot_relief

// The authored sole texture is a visual mask, never a physical height source.
// 3 mm of relief decorates the real mud/sand macro-depression without changing
// collision, silhouette or neighbouring terrain cells.
fn boot_relief_height(texel: vec4<f32>) -> f32 {
    let darkness = 1.0 - clamp(dot(texel.rgb, vec3<f32>(0.2126, 0.7152, 0.0722)), 0.0, 1.0);
    return -0.003 * clamp(texel.a, 0.0, 1.0) * darkness;
}

fn boot_relief_normal(n: vec3<f32>, tangent: vec3<f32>, bitangent: vec3<f32>, gradient: vec2<f32>) -> vec3<f32> {
    let tl = length(tangent);
    let bl = length(bitangent);
    if (tl <= 0.00001 || bl <= 0.00001) { return n; }
    let slope = clamp(gradient / vec2<f32>(tl, bl), vec2<f32>(-0.65), vec2<f32>(0.65));
    return normalize(n - tangent / tl * slope.x - bitangent / bl * slope.y);
}

fn boot_relief_parallax(view: vec3<f32>, n: vec3<f32>, tangent: vec3<f32>, bitangent: vec3<f32>, height: f32) -> vec2<f32> {
    let tl = length(tangent);
    let bl = length(bitangent);
    if (tl <= 0.00001 || bl <= 0.00001) { return vec2<f32>(0.0); }
    let eye = vec2<f32>(dot(view, tangent / tl), dot(view, bitangent / bl));
    let offset = height * eye / (max(abs(dot(view, n)), 0.25) * vec2<f32>(tl, bl));
    return clamp(offset, vec2<f32>(-0.02), vec2<f32>(0.02));
}

fn boot_relief_multiplier(n: vec3<f32>, relieved: vec3<f32>, light: vec3<f32>, height: f32,
                          opacity: f32, visibility: f32, direct: f32) -> f32 {
    let delta = max(dot(relieved, light), 0.0) - max(dot(n, light), 0.0);
    let cavity = clamp(-height / 0.003, 0.0, 1.0) * 0.055;
    let detail = clamp(delta * 0.65 * clamp(direct, 0.0, 1.0) - cavity, -0.22, 0.14);
    // A scalar multiplies the already lit receiver, preserving terrain hue and
    // fogged colour. A transparent texel, expired mark or far view is identity.
    return min(1.0, 1.0 + detail * clamp(opacity * 2.0, 0.0, 1.0) * clamp(visibility, 0.0, 1.0));
}

fn boot_relief_composite(multiplier: f32, airlight: vec3<f32>) -> vec4<f32> {
    // Receiver = airlight + transmitted ground. SrcOne/DstSrcAlpha applies M
    // to ground only: airlight*(1-M) + M*receiver. Never darken foreground mist.
    let m = clamp(multiplier, 0.0, 1.0);
    return vec4<f32>(airlight * (1.0 - m), m);
}
