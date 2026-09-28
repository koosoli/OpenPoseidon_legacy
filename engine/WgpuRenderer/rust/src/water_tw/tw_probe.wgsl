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
// tw.probe: the grid's first lattice point x, z (m), grid step (m), grid size (texels; 0 = off)
//
// (W12d, OP) The grid holds the drawn mesh's own vertices, not heights at fixed points: texel
// (i, j) is the displaced position of the finest CDLOD lattice point base + (i, j) x 0.25 m -- the
// vertex the water vertex shader places there (the same twDisplace, the same spacing; near the
// camera the mesh is the finest level, unmorphed). The underwater composite interpolates the
// mesh's own triangles from them, so its waterline on the lens is where the drawn surface crosses
// the near plane. With heights from the analytic surface (W9a) the two differed by millimetres,
// and a camera a few centimetres from the water turned that into a band tens of pixels tall
// between them where neither drew: the unfogged sea bed, a strip of sand across the lens (owner's
// third-person shot, W12c).

const PROBE_MESH: f32 = 0.25; // the finest CDLOD spacing (cdlod::LEAF / cdlod::GRID)

@vertex
fn vs_tw_probe(@builtin(vertex_index) vi: u32) -> @builtin(position) vec4<f32> {
    let p = vec2<f32>(f32((vi << 1u) & 2u), f32(vi & 2u));
    return vec4<f32>(p * 2.0 - 1.0, 0.0, 1.0);
}

@fragment
fn fs_tw_probe(@builtin(position) pos: vec4<f32>) -> @location(0) vec4<f32> {
    let xz = tw.probe.xy + floor(pos.xy) * tw.probe.z;
    let d = twDisplace(xz, PROBE_MESH);
    return vec4<f32>(d.world, 1.0);
}
