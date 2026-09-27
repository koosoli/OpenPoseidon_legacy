// Tidewater Native — reading the shore simulation (W3b). Port of ShoreSim.js's material module
// (dgreenheck/tidewater @ 4811ba48, MIT, © DRG Software Solutions LLC): shoreSimUvOf,
// shoreSimInside, shoreSimStateAt, shoreSimState, shoreSimSample, shoreSimLaceLoad,
// shoreSimSandFoam.
//
// OP adaptation: Tidewater simulates one fixed 380 m region over its beach. OP keeps up to two
// such regions on the coast nearest the camera (plan §6 W3, K = 2), stored as the two layers of
// one texture array. A point reads the region it is deepest inside; each region's weight
// (tw.sim[r].w) fades it in over its warm-up and is 0 when the region is unused.
// Consumers declare `tw`, `shoreSimStateTex` (texture_2d_array, the state read by materials) and
// `shoreSimLace` (the lace texture; mip 0 is Tidewater's "nearest" copy, read with loads).

const SHORE_SIM_RES: f32 = 768.0;
const SHORE_SIM_LACE_TILE: f32 = 3.5;
const SHORE_SIM_LACE_N: i32 = 512;

fn shoreSimUvOfR(xz: vec2<f32>, r: i32) -> vec2<f32> {
    return (xz - tw.sim[r].xy) / max(tw.sim[r].z, 1e-3);
}

fn shoreSimInside(uv: vec2<f32>) -> f32 {
    // (W3k: 15% of the region, ~57 m, instead of Tidewater's 2%. Tidewater's one region covers its
    // whole bay, so its border never crosses the surf; OP's regions follow the camera along the
    // coast, and a 7.6 m fade drew the region's square outline across the whitewater.)
    return smoothstep(0.0, 0.15, uv.x) * smoothstep(1.0, 0.85, uv.x) * smoothstep(0.0, 0.15, uv.y) * smoothstep(1.0, 0.85, uv.y);
}

// bilinear state of region layer r at texture coordinate uv, from 4 loads
fn shoreSimStateAt(uv: vec2<f32>, r: i32) -> vec4<f32> {
    let fp = clamp(uv * SHORE_SIM_RES - 0.5, vec2<f32>(0.0), vec2<f32>(SHORE_SIM_RES - 1.001));
    let i = vec2<i32>(floor(fp));
    let t = fract(fp);
    let a = textureLoad(shoreSimStateTex, i, r, 0);
    let b = textureLoad(shoreSimStateTex, i + vec2<i32>(1, 0), r, 0);
    let c = textureLoad(shoreSimStateTex, i + vec2<i32>(0, 1), r, 0);
    let d = textureLoad(shoreSimStateTex, i + vec2<i32>(1, 1), r, 0);
    return mix(mix(a, b, t.x), mix(c, d, t.x), t.y);
}

// the region a point belongs to (the one it is deepest inside, weighted): (layer, inside x weight).
// A tie keeps the lower layer only if it is clearly deeper: a region that has just faded in does
// not take over the overlap from the one already there (its drying history would be lost).
fn shoreSimPick(xz: vec2<f32>) -> vec2<f32> {
    var best = vec2<f32>(0.0, 0.0);
    for (var r = 0; r < 2; r++) {
        let uv = shoreSimUvOfR(xz, r);
        var k = 0.0;
        if (tw.sim[r].w > 0.0 && uv.x > 0.0 && uv.x < 1.0 && uv.y > 0.0 && uv.y < 1.0) {
            k = shoreSimInside(uv) * tw.sim[r].w;
        }
        if (k > best.y + 1e-4) { best = vec2<f32>(f32(r), k); }
    }
    return best;
}

// raw state (foam, wetness, residue amount, flow speed), faded out at the region border, for a
// pick from shoreSimPick (so a caller that also needs the coverage picks once)
fn shoreSimStateFor(xz: vec2<f32>, pick: vec2<f32>) -> vec4<f32> {
    var out = vec4<f32>(0.0);
    if (tw.simP1.w > 0.5 && pick.y > 0.0) {
        let r = i32(pick.x);
        out = shoreSimStateAt(shoreSimUvOfR(xz, r), r) * pick.y;
    }
    return out;
}

// how much of the point is covered by a simulated region (0 outside every region)
fn shoreSimInsideAt(xz: vec2<f32>) -> f32 {
    return shoreSimPick(xz).y;
}

// raw state (foam, wetness, residue amount, flow speed), faded out at the region border
fn shoreSimState(xz: vec2<f32>) -> vec4<f32> {
    return shoreSimStateFor(xz, shoreSimPick(xz));
}

// vec4(foam amount on the water, sand wetness, foam amount left on the sand, flow speed)
fn shoreSimSample(xz: vec2<f32>) -> vec4<f32> {
    return shoreSimState(xz);
}

// bilinear lace lookup from 4 loads of the nearest-filtered copy (no sampler binding needed)
fn shoreSimLaceLoad(q: vec2<f32>) -> vec4<f32> {
    let fp = q / SHORE_SIM_LACE_TILE * f32(SHORE_SIM_LACE_N) - 0.5;
    let i = vec2<i32>(floor(fp));
    let t = fract(fp);
    let m = vec2<i32>(SHORE_SIM_LACE_N - 1);
    let a = textureLoad(shoreSimLace, i & m, 0);
    let b = textureLoad(shoreSimLace, (i + vec2<i32>(1, 0)) & m, 0);
    let c = textureLoad(shoreSimLace, (i + vec2<i32>(0, 1)) & m, 0);
    let d = textureLoad(shoreSimLace, (i + vec2<i32>(1, 1)) & m, 0);
    return mix(mix(a, b, t.x), mix(c, d, t.x), t.y);
}

// Foam left on the sand (0..1): thin bubble lines and single bubbles where the draining water
// left its foam, popping patch by patch as it dries. Static on the sand (world space).
// s: shoreSimSample(xz). fp: the pixel footprint in lace texels,
// length(fwidth(xz)) / SHORE_SIM_LACE_TILE * SHORE_SIM_LACE_N, taken by the caller in uniform
// control flow (Tidewater takes it here, before the branch).
fn shoreSimSandFoam(xz: vec2<f32>, s: vec4<f32>, fp: f32) -> f32 {
    let r = s.z;
    var out = 0.0;
    if (r > 0.01) {
        let lace = shoreSimLaceLoad(xz + 11.3);
        // fade to the average where a pixel covers several strands (no mipmaps on the load path)
        let near = smoothstep(3.0, 1.2, fp);
        let keep = smoothstep(lace.w * 0.55, lace.w * 0.55 + 0.08, r); // staggered popping
        // thin bubble lines where the strands were, a little wider where more foam was left
        let lw = r * 0.1 + 0.06;
        let strand = (1.0 - smoothstep(lw, lw + 0.07, lace.x)) * (lace.z * 0.5 + 0.6);
        let lines = max(strand * smoothstep(0.02, 0.25, r) * keep, lace.y * keep * 0.8);
        out = mix(smoothstep(0.08, 0.6, r) * 0.12, lines, near) * 0.85;
    }
    return out;
}
