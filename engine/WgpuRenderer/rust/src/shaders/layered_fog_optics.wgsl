#define_import_path layered_fog_optics

// Finite ASL slab with linear boundary feathers. Extinction is in 1/metre.
// The primitive integrates thin layers even when neither step endpoint lies in them.
fn fog_height_density(y: f32, layer: vec4<f32>) -> f32 {
    let feather = max(layer.z, 0.001);
    return clamp((y - layer.x + feather) / feather, 0.0, 1.0)
        * clamp((layer.y + feather - y) / feather, 0.0, 1.0);
}
fn fog_height_primitive(y: f32, layer: vec4<f32>) -> f32 {
    let f = max(layer.z, 0.001);
    let rise = clamp(y - (layer.x - f), 0.0, f);
    let plateau = clamp(y - layer.x, 0.0, layer.y - layer.x);
    let fall = clamp(y - layer.y, 0.0, f);
    return rise * rise / (2.0 * f) + plateau + fall - fall * fall / (2.0 * f);
}
fn fog_segment_density(y: f32, ray_y: f32, distance: f32, layer: vec4<f32>) -> f32 {
    if (distance <= 0.0 || layer.w <= 0.0) { return 0.0; }
    let delta = ray_y * distance;
    if (abs(delta) < 0.001) { return fog_height_density(y + 0.5 * delta, layer) * distance; }
    return max((fog_height_primitive(y + delta, layer) - fog_height_primitive(y, layer)) / ray_y, 0.0);
}
// Unit-solid-angle normalized Henyey-Greenstein, not an unnormalized highlight.
fn fog_phase(cosine: f32, g: f32) -> f32 {
    let gg = clamp(g, -0.95, 0.95);
    return (1.0 - gg * gg) / (12.56637061436 * pow(max(1.0 + gg * gg - 2.0 * gg * cosine, 0.0025), 1.5));
}
fn fog_transmittance(tau: f32) -> f32 { return exp(-max(tau, 0.0)); }
fn fog_transport(background: vec3<f32>, transport: vec4<f32>) -> vec3<f32> {
    return background * transport.a + transport.rgb;
}

// Shared packet layout. Camera matching prevents a cockpit/reflection camera from
// sampling a volume encoded for the main scene. OFF/invalid reads no volume texture.
struct FogConsumer {
    view: mat4x4<f32>,
    proj: mat4x4<f32>,
    inv_vp: mat4x4<f32>,
    camera: vec4<f32>,
    control: vec4<f32>, // ready, far distance, physical weather fog, reserved
};
fn fog_camera_matches(packet: FogConsumer, view: mat4x4<f32>, proj: mat4x4<f32>, camera: vec3<f32>) -> bool {
    if (packet.control.x < 0.5 || packet.control.y <= 0.0) { return false; }
    if (any(packet.camera.xyz != camera)) { return false; }
    for (var i = 0u; i < 4u; i = i + 1u) {
        if (any(packet.view[i] != view[i]) || any(packet.proj[i] != proj[i])) { return false; }
    }
    return true;
}
