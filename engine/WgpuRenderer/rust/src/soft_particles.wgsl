// SMK-037: the scene-depth snapshot the soft-particle fade reads.
//
// WHY A COPY AT ALL. The cloudlet billboards are drawn in the transparent replay, whose
// render pass has the scene depth attached read-write. A texture cannot be a writable
// attachment and a sampled binding of the same pass, so the fragment shader that wants to
// compare against that depth cannot have it. This pass runs BEFORE that one, attaches no
// depth of its own, and copies the depth aspect into a plain R32Float colour target which
// the 2D pipeline is then free to sample.
//
// Stored value is the RAW reversed-Z device depth, untouched: 1 at the near plane, 0 at
// the far plane / sky. Linearising here would need the projection, and the consumer has it
// anyway.

@group(0) @binding(0) var src_depth: texture_depth_2d;

@vertex
fn vs_main(@builtin(vertex_index) vi: u32) -> @builtin(position) vec4<f32> {
    // Fullscreen triangle. Three vertices, no buffer.
    let x = f32((vi << 1u) & 2u) * 2.0 - 1.0;
    let y = f32(vi & 2u) * 2.0 - 1.0;
    return vec4<f32>(x, y, 0.0, 1.0);
}

@fragment
fn fs_main(@builtin(position) frag: vec4<f32>) -> @location(0) f32 {
    let texel = vec2<i32>(i32(frag.x), i32(frag.y));
    return textureLoad(src_depth, texel, 0);
}
