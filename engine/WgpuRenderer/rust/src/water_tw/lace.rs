//! Tileable surf-foam lace texture, generated on the CPU.
//!
//! Port of `laceData(size)` and the CPU mip chain of `makeLaceTexture(size = 512)` from
//! Tidewater `src/ocean/SurfFoam.js` @ 4811ba48 (MIT, © DRG Software Solutions LLC).
//!
//! The output is bit-exact with the JavaScript: all arithmetic is done in f64 in the same order
//! as the JS, `Float32Array` storage is reproduced with `as f32` round trips, and the JS integer
//! semantics (`| 0`, `Math.imul`, `>>>`, `Math.round`, `Math.hypot`, `%`) are emulated exactly.
//!
//! Channels (RGBA8, row-major):
//!   R: distance to the nearest bubble strand (0 on a strand, 1 in the middle of a hole)
//!   G: small bubbles clustered along the strands
//!   B: soft mottling (foam density variation)
//!   A: per-cell random value (staggers when stranded foam pops)

/// Metres per tile of the lace texture.
pub const LACE_TILE: f32 = 3.5;
/// Default texture size (texels per side).
pub const LACE_SIZE: u32 = 512;

/// JS `Math.round` (V8: ceil, then step down if that rounded up by more than a half).
#[inline]
fn js_round(x: f64) -> f64 {
    let c = x.ceil();
    if c - 0.5 > x { c - 1.0 } else { c }
}

/// JS `Math.hypot(x, y)` as implemented by V8 (normalise by the max, Kahan-summed squares).
/// Differs from libm `hypot` in the last ulp, so it is reproduced exactly.
#[inline]
fn js_hypot(x: f64, y: f64) -> f64 {
    let ax = x.abs();
    let ay = y.abs();
    if x.is_infinite() || y.is_infinite() {
        return f64::INFINITY;
    }
    if x.is_nan() || y.is_nan() {
        return f64::NAN;
    }
    let mut max = 0.0f64;
    if ax > max {
        max = ax;
    }
    if ay > max {
        max = ay;
    }
    if max == 0.0 {
        return 0.0;
    }
    let mut sum = 0.0f64;
    let mut comp = 0.0f64;
    for v in [ax, ay] {
        let n = v / max;
        let summand = n * n - comp;
        let prelim = sum + summand;
        comp = (prelim - sum) - summand;
        sum = prelim;
    }
    sum.sqrt() * max
}

/// Assignment to a `Uint8Array` element (ToUint8: truncate, modulo 256; NaN -> 0).
#[inline]
fn to_u8(x: f64) -> u8 {
    if !x.is_finite() {
        return 0;
    }
    (x.trunc() as i64).rem_euclid(256) as u8
}

/// `( ( a % n ) + n ) % n` on integer-valued doubles.
#[inline]
fn wrap(a: i64, n: i64) -> i64 {
    ((a % n) + n) % n
}

/// `hash( x, y, s )`: integer arguments; `( ... ) | 0` of an exact double is ToInt32.
#[inline]
fn hash(x: i64, y: i64, s: i64) -> f64 {
    // all products and the sum are < 2^53, so the JS double is exact and equals this i64
    let mut h = (x * 374761393 + y * 668265263 + s * 2246822519) as i32;
    h = (h ^ ((h as u32) >> 13) as i32).wrapping_mul(1274126177);
    h ^= ((h as u32) >> 16) as i32;
    (h as u32) as f64 / 4294967296.0
}

/// Periodic value noise (period `n` cells).
fn vnoise(x: f64, y: f64, n: i64, s: i64) -> f64 {
    let i = x.floor();
    let j = y.floor();
    let fx = x - i;
    let fy = y - j;
    let ux = fx * fx * (3.0 - 2.0 * fx);
    let uy = fy * fy * (3.0 - 2.0 * fy);
    let (i, j) = (i as i64, j as i64);
    let a = hash(wrap(i, n), wrap(j, n), s);
    let b = hash(wrap(i + 1, n), wrap(j, n), s);
    let c = hash(wrap(i, n), wrap(j + 1, n), s);
    let d = hash(wrap(i + 1, n), wrap(j + 1, n), s);
    (a * (1.0 - ux) + b * ux) * (1.0 - uy) + (c * (1.0 - ux) + d * ux) * uy
}

/// Periodic fractal value noise (`oct` octaves, default 3 in the JS).
fn fbm(x: f64, y: f64, n: i64, s: i64, oct: i32) -> f64 {
    let mut v = 0.0f64;
    let mut a = 0.5f64;
    let mut t = 0.0f64;
    for o in 0..oct {
        let m = (1i64 << o) as f64;
        v += vnoise(x * m, y * m, n * (1i64 << o), s + o as i64 * 17) * a;
        t += a;
        a *= 0.5;
    }
    v / t
}

/// Voronoi F1, F2 and nearest cell id on an n x n periodic jittered grid (coordinates in cells).
fn voronoi(x: f64, y: f64, n: i64, s: i64) -> (f64, f64, f64) {
    let i = x.floor() as i64;
    let j = y.floor() as i64;
    let mut f1 = 9.0f64;
    let mut f2 = 9.0f64;
    let mut id = 0.0f64;
    for dj in -1..=1i64 {
        for di in -1..=1i64 {
            let ci = i + di;
            let cj = j + dj;
            let wi = wrap(ci, n);
            let wj = wrap(cj, n);
            let px = ci as f64 + 0.15 + 0.7 * hash(wi, wj, s);
            let py = cj as f64 + 0.15 + 0.7 * hash(wi, wj, s + 1);
            let d = js_hypot(px - x, py - y);
            if d < f1 {
                f2 = f1;
                f1 = d;
                id = hash(wi, wj, s + 2);
            } else if d < f2 {
                f2 = d;
            }
        }
    }
    (f1, f2, id)
}

#[inline]
fn sstep(a: f64, b: f64, x: f64) -> f64 {
    let t = f64::min(1.0, f64::max(0.0, (x - a) / (b - a)));
    t * t * (3.0 - 2.0 * t)
}

/// RGBA8 lace texture, `size` x `size`, row-major; identical bytes to JS `laceData(size)`.
pub fn lace_data(size: u32) -> Vec<u8> {
    let sz = size as usize;
    let sf = size as f64;
    let n = sz * sz;
    let mut data = vec![0u8; n * 4];

    // pass 1: warped coordinates and the contour noise at every texel (Float32Array storage)
    let mut uu_a = vec![0f32; n];
    let mut vv_a = vec![0f32; n];
    let mut nc = vec![0f32; n];
    for py in 0..sz {
        for px in 0..sz {
            let u = (px as f64 + 0.5) / sf;
            let v = (py as f64 + 0.5) / sf;
            // two-level domain warp: organic, curvy strands
            let w1x = (fbm(u * 3.0, v * 3.0, 3, 3, 3) - 0.5) * 0.16;
            let w1y = (fbm(u * 3.0 + 5.2, v * 3.0 + 1.3, 3, 7, 3) - 0.5) * 0.16;
            let w2x = (fbm((u + w1x) * 9.0, (v + w1y) * 9.0, 9, 13, 3) - 0.5) * 0.07;
            let w2y = (fbm((u + w1x) * 9.0 + 2.7, (v + w1y) * 9.0, 9, 17, 3) - 0.5) * 0.07;
            let k = py * sz + px;
            uu_a[k] = (u + w1x + w2x) as f32;
            vv_a[k] = (v + w1y + w2y) as f32;
            nc[k] = fbm(uu_a[k] as f64 * 6.0, vv_a[k] as f64 * 6.0, 6, 91, 3) as f32;
        }
    }

    // pass 2: distance to the nearest strand
    const N1: i64 = 16;
    let half = 0.5 / N1 as f64;
    let mut dd = vec![0f32; n];
    for py in 0..sz {
        for px in 0..sz {
            let u = (px as f64 + 0.5) / sf;
            let v = (py as f64 + 0.5) / sf;
            let k = py * sz + px;
            let uu = uu_a[k] as f64;
            let vv = vv_a[k] as f64;
            // strands 1: edges of a warped cell network
            let (a1, b1, id1) = voronoi(uu * N1 as f64, vv * N1 as f64, N1, 11);
            let d_cells = (b1 - a1) * 0.5 / N1 as f64;
            // strands 2: contour loops of the warped noise
            let n1 = nc[k] as f64;
            let xl = (px + sz - 1) % sz;
            let xr = (px + 1) % sz;
            let yd = (py + sz - 1) % sz;
            let yu = (py + 1) % sz;
            let gx = (nc[py * sz + xr] as f64 - nc[py * sz + xl] as f64) * sf * 0.5;
            let gy = (nc[yu * sz + px] as f64 - nc[yd * sz + px] as f64) * sf * 0.5;
            let g = f64::max(js_hypot(gx, gy), 0.5);
            let d_loops = f64::min(
                f64::min((n1 - 0.5).abs(), (n1 - 0.36).abs()),
                (n1 - 0.64).abs(),
            ) / g;
            let hi = fbm(u * 24.0, v * 24.0, 24, 5, 2);
            let gap = sstep(0.52, 0.36, fbm(u * 10.0 + 3.1, v * 10.0, 10, 31, 3));
            let d = f64::min(d_cells * 1.35 + 0.004, d_loops) / half * (0.75 + 0.5 * hi) + gap * 0.22;
            let mott = fbm(u * 3.0, v * 3.0, 3, 71, 4);
            // bubbles: small dots, clustered near the strands
            const N3: i64 = 120;
            let (a3, _, id3) = voronoi(u * N3 as f64, v * N3 as f64, N3, 61);
            let rad = 0.1 + 0.22 * id3;
            let dot = sstep(rad, rad * 0.35, a3) * (if id3 > 0.45 { 1.0 } else { 0.0 });
            let bub = dot * (0.25 + 0.75 * sstep(0.5, 0.1, d));
            dd[k] = f64::min(1.0, d) as f32;
            data[k * 4 + 1] = to_u8(js_round(f64::min(1.0, bub) * 255.0));
            data[k * 4 + 2] = to_u8(js_round(mott * 255.0));
            data[k * 4 + 3] = to_u8(js_round(id1 * 255.0));
        }
    }

    // pass 3: rounded holes: blurred distance inside the holes, the exact one near the strands
    let d0 = dd.clone();
    let mut t = vec![0f32; n];
    for _ in 0..2 {
        for py in 0..sz {
            for px in 0..sz {
                let mut s = 0.0f64;
                for o in -2i64..=2 {
                    let x = ((px as i64 + o + sz as i64) as usize) % sz;
                    s += dd[py * sz + x] as f64;
                }
                t[py * sz + px] = (s / 5.0) as f32;
            }
        }
        for py in 0..sz {
            for px in 0..sz {
                let mut s = 0.0f64;
                for o in -2i64..=2 {
                    let y = ((py as i64 + o + sz as i64) as usize) % sz;
                    s += t[y * sz + px] as f64;
                }
                dd[py * sz + px] = (s / 5.0) as f32;
            }
        }
    }

    for k in 0..n {
        let a = d0[k] as f64;
        let b = dd[k] as f64;
        data[k * 4] = to_u8(js_round((a + (b - a) * sstep(0.12, 0.45, a)) * 255.0));
    }
    data
}

/// Full mip chain: level 0 is `lace_data(size)`, then 2x2 box filter `(a + b + d + e + 2) >> 2`
/// down to 1x1 (as in `makeLaceTexture`). `size` must be a power of two.
pub fn lace_mips(size: u32) -> Vec<Vec<u8>> {
    let data = lace_data(size);
    let mut mips = vec![data];
    let mut s = size as usize;
    while s > 1 {
        let h = s >> 1;
        let src = mips.last().unwrap();
        let mut dst = vec![0u8; h * h * 4];
        for y in 0..h {
            for x in 0..h {
                for c in 0..4 {
                    let a = src[((2 * y) * s + 2 * x) * 4 + c] as u32;
                    let b = src[((2 * y) * s + 2 * x + 1) * 4 + c] as u32;
                    let d = src[((2 * y + 1) * s + 2 * x) * 4 + c] as u32;
                    let e = src[((2 * y + 1) * s + 2 * x + 1) * 4 + c] as u32;
                    dst[(y * h + x) * 4 + c] = ((a + b + d + e + 2) >> 2) as u8;
                }
            }
        }
        mips.push(dst);
        s = h;
    }
    mips
}

#[cfg(test)]
mod tests {
    use super::*;

    fn fnv1a64(bytes: &[u8]) -> u64 {
        let mut h: u64 = 0xcbf29ce484222325;
        for &b in bytes {
            h ^= b as u64;
            h = h.wrapping_mul(0x100000001b3);
        }
        h
    }

    // Reference hashes from the JS (node v22) running SurfFoam.js laceData @ 4811ba48.
    #[test]
    fn lace_64_matches_js() {
        let d = lace_data(64);
        assert_eq!(d.len(), 64 * 64 * 4);
        assert_eq!(fnv1a64(&d), 0xc79ae70b3364f1eb);
    }

    #[test]
    fn lace_mips_64_match_js() {
        let m = lace_mips(64);
        assert_eq!(m.len(), 7);
        for (i, lvl) in m.iter().enumerate() {
            let s = 64usize >> i;
            assert_eq!(lvl.len(), s * s * 4);
        }
        assert_eq!(m.last().unwrap().as_slice(), &[98, 7, 141, 126]);
        assert_eq!(fnv1a64(&m.concat()), 0x9e92a3234dbb2f3e);
    }

    #[test]
    #[ignore = "slow in debug builds"]
    fn lace_512_matches_js() {
        let m = lace_mips(LACE_SIZE);
        assert_eq!(m.len(), 10);
        assert_eq!(fnv1a64(&m[0]), 0x3746804a816346d3);
        assert_eq!(m.last().unwrap().as_slice(), &[108, 8, 141, 126]);
        assert_eq!(fnv1a64(&m.concat()), 0x30327943a7cb72d4);
    }
}
