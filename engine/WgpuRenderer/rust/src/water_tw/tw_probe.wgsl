// Tidewater Native W9a (OP) — the water surface around the camera, exactly as it is drawn.
//
// Tidewater decides the medium at the lens from the side of the rendered surface each pixel sees
// (a WaterQuery slot and a mask the water material writes). OP's water pass has one colour
// target, so W5a decided it from the sea level plus the FFT's height, while the surface itself
// also carries the shore waves, the swash sheet, the wakes and the bursts; and the engine's
// submersion test used Current OP's own reference waves. Near a beach the three disagreed by
// metres (TW-WATER W9: the flat blue sheet at the waterline).
//
// This pass evaluates the vertex shader's own displacement (twDisplace in tw_water.wgsl) on a
// small grid centred on the camera, inverting the horizontal displacement (three fixed-point
// steps), into an r32float target that the underwater composite reads for the waterline and the
// camera's depth. The centre texel is also read back for the CPU side (the compositor gate, the
// water material's view side), 1-3 frames late.
// tw.probe: camera x, camera z, grid step (m), grid size (texels; 0 = off)

const PROBE_MESH: f32 = 0.25; // the finest CDLOD spacing (cdlod::LEAF / cdlod::GRID)

fn twHeightAt(xz: vec2<f32>) -> f32 {
    var p = xz;
    var d = twDisplace(p, PROBE_MESH);
    for (var i = 0; i < 3; i++) {
        p += xz - d.world.xz;
        d = twDisplace(p, PROBE_MESH);
    }
    return d.world.y;
}

@vertex
fn vs_tw_probe(@builtin(vertex_index) vi: u32) -> @builtin(position) vec4<f32> {
    let p = vec2<f32>(f32((vi << 1u) & 2u), f32(vi & 2u));
    return vec4<f32>(p * 2.0 - 1.0, 0.0, 1.0);
}

@fragment
fn fs_tw_probe(@builtin(position) pos: vec4<f32>) -> @location(0) vec4<f32> {
    let n = tw.probe.w;
    let xz = tw.probe.xy + (floor(pos.xy) - (n - 1.0) * 0.5) * tw.probe.z;
    return vec4<f32>(twHeightAt(xz), 0.0, 0.0, 1.0);
}
