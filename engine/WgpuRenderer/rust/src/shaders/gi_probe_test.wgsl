// Test-only: a fragment entry that actually CALLS frame::gi_irradiance, so the device test
// runs naga's backend over the probe gather. The real caller is shading::shade, whose pipeline
// needs the whole renderer to build; this one needs nothing but the camera group, and it is
// what would have caught "Expression [148] is not cached" before a deploy (2026-09-03).
#import frame::{frame, gi_irradiance}

@vertex
fn vs_gi_probe_test(@builtin(vertex_index) i: u32) -> @builtin(position) vec4<f32> {
    let x = f32(i32(i % 2u) * 2 - 1);
    let y = f32(i32(i / 2u) * 2 - 1);
    return vec4<f32>(x, y, 0.0, 1.0);
}

@fragment
fn fs_gi_probe_test(@builtin(position) pos: vec4<f32>) -> @location(0) vec4<f32> {
    let world = vec3<f32>(pos.x, pos.y, pos.z) + frame.cam_pos.xyz;
    let n = normalize(vec3<f32>(0.3, 1.0, 0.2));
    let gi = gi_irradiance(world, n);
    return vec4<f32>(gi.rgb * gi.a, 1.0);
}
