// SPDX-License-Identifier: GPL-3.0-or-later
// Developer-only raw world-depth snapshot and bounded numeric witness.
@group(0) @binding(0) var source_depth: texture_depth_2d;

@vertex
fn vs(@builtin(vertex_index) id: u32) -> @builtin(position) vec4<f32> {
    let positions = array<vec2<f32>, 3>(vec2<f32>(-1.0, -1.0),
        vec2<f32>(3.0, -1.0), vec2<f32>(-1.0, 3.0));
    return vec4<f32>(positions[id], 0.0, 1.0);
}
@fragment
fn snapshot(@builtin(position) p: vec4<f32>) -> @location(0) f32 {
    return textureLoad(source_depth, vec2<i32>(p.xy), 0);
}
@fragment
fn grid(@builtin(position) p: vec4<f32>) -> @location(0) f32 {
    let size = textureDimensions(source_depth);
    let bin = vec2<u32>(p.xy);
    let pixel = min(((bin * 2u + vec2<u32>(1u)) * size) / 32u, size - vec2<u32>(1u));
    return textureLoad(source_depth, vec2<i32>(pixel), 0);
}
