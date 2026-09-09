// CPU sky-view-factor (sky-visibility) scan — the ambient-occlusion analogue of the sun-shadow
// sweep (see docs/sky-visibility-ambient-plan.md). For each output column it marches the heightfield
// in K azimuths, tracks the steepest horizon slope in each, and reduces to the cosine-weighted
// fraction of visible sky:
//
//     V = mean_k( 1 / (1 + t_k^2) )   where   t_k = max ( h(sample) - h0 ) / horizontal_distance
//
// (the closed form of the Lambertian hemisphere integral with a per-azimuth horizon angle; see the
// plan §3). Purely a function of the heightfield, so it is computed once per map (and later disk-
// cached). Output is a COARSE grid (sky-view is low-frequency) sampled bilinearly by the shaders.
//
// Runs on the CPU rather than as a GPU dispatch because the result is disk-cacheable without any
// texture readback, and its cost (sub-second even for a 2048^2 map, on a coarse grid) is paid once.

/// Tunable scan options. `downsample` sets the output grid coarseness relative to the heightmap
/// (2 = half-res per axis, 4 = quarter). `k_azimuths` directions, marched out to `radius_m`.
#[derive(Clone, Copy)]
pub struct SkyvisOptions {
    pub downsample: u32,
    pub k_azimuths: u32,
    pub radius_m: f32,
    // Separable box-blur radius (output texels) applied to the finished map. Softens the high-contrast
    // neighbour jumps on steep cliffs so the shader's bilinear tap doesn't Mach-band into facets, and
    // removes the fine noise from the per-texel fan rotation. 0 = no blur.
    pub blur_radius: u32,
}

impl Default for SkyvisOptions {
    fn default() -> Self {
        SkyvisOptions {
            // downsample 2 (not 4): a /4 mask bilinear-blurs exactly the steep, one-texel-wide cliffs
            // the AO is meant to catch. 2 keeps them sharp at a modest cost; tunable live.
            downsample: 2,
            k_azimuths: 8,
            radius_m: 600.0,
            blur_radius: 1,
        }
    }
}

// Deterministic integer hash -> [0,1) (no RNG, so the bake stays reproducible/cacheable). Bit-mixed
// so it stays well-distributed at large world coordinates (the sin(dot)*large one-liner collapses).
fn hash2(x: u32, y: u32) -> f32 {
    let mut n = x
        .wrapping_mul(1597334677)
        .wrapping_add(y.wrapping_mul(3812015801));
    n = (n ^ (n >> 15)).wrapping_mul(2246822519);
    n ^= n >> 13;
    (n & 0x00ff_ffff) as f32 / (0x0100_0000 as f32)
}

// Separable box blur over a row-major grid, edge-clamped. Cheap (it runs once per bake).
fn blur_separable(data: &[f32], w: usize, h: usize, radius: u32) -> Vec<f32> {
    if radius == 0 || w == 0 || h == 0 {
        return data.to_vec();
    }
    let r = radius as isize;
    let n = (2 * r + 1) as f32;
    let mut tmp = vec![0.0f32; data.len()];
    for y in 0..h {
        for x in 0..w {
            let mut sum = 0.0;
            for d in -r..=r {
                let sx = (x as isize + d).clamp(0, w as isize - 1) as usize;
                sum += data[y * w + sx];
            }
            tmp[y * w + x] = sum / n;
        }
    }
    let mut out = vec![0.0f32; data.len()];
    for y in 0..h {
        for x in 0..w {
            let mut sum = 0.0;
            for d in -r..=r {
                let sy = (y as isize + d).clamp(0, h as isize - 1) as usize;
                sum += tmp[sy * w + x];
            }
            out[y * w + x] = sum / n;
        }
    }
    out
}

// Ratio the march step grows by each iteration: fine near the column (a thin near ridge must not be
// stepped over), coarsening with distance (far ridges subtend little solid angle). Keeps the sample
// count ~logarithmic in the radius.
const STEP_GROWTH: f32 = 1.35;

/// Bilinear heightfield sample at fractional texel coordinates, edges clamped (a column near the map
/// edge sees the border height repeated — matches the sun sweep's `hm_load` clamp).
fn sample_bilinear(heights: &[f32], w: u32, h: u32, x: f32, z: f32) -> f32 {
    let fx = x.floor();
    let fz = z.floor();
    let tx = x - fx;
    let tz = z - fz;
    let ix = fx as i32;
    let iz = fz as i32;
    let at = |cx: i32, cz: i32| -> f32 {
        let ccx = cx.clamp(0, w as i32 - 1) as usize;
        let ccz = cz.clamp(0, h as i32 - 1) as usize;
        heights[ccz * w as usize + ccx]
    };
    let h00 = at(ix, iz);
    let h10 = at(ix + 1, iz);
    let h01 = at(ix, iz + 1);
    let h11 = at(ix + 1, iz + 1);
    let a = h00 + (h10 - h00) * tx;
    let b = h01 + (h11 - h01) * tx;
    a + (b - a) * tz
}

/// Output dimensions for a heightmap of the given size at `downsample` (>= 1 per axis).
pub fn skyvis_dims(w: u32, h: u32, downsample: u32) -> (u32, u32) {
    let ds = downsample.max(1);
    (w.div_ceil(ds).max(1), h.div_ceil(ds).max(1))
}

/// Compute the sky-view-factor grid. Returns (out_w, out_h, values in [0,1], row-major). `heights`
/// is the row-major heightfield (`w*h` samples), `terrain_grid` its world-metre spacing per texel.
pub fn compute(
    heights: &[f32],
    w: u32,
    h: u32,
    terrain_grid: f32,
    opts: SkyvisOptions,
) -> (u32, u32, Vec<f32>) {
    let (w, h, _, blurred) = compute_cached(heights, w, h, terrain_grid, opts, None);
    (w, h, blurred)
}

/// Reuse raw horizon values outside the changed samples' finite influence radius.
/// The caller must only supply previous data from identical dimensions/options.
pub fn compute_cached(
    heights: &[f32],
    w: u32,
    h: u32,
    terrain_grid: f32,
    opts: SkyvisOptions,
    previous: Option<(&[f32], &[f32])>,
) -> (u32, u32, Vec<f32>, Vec<f32>) {
    let (out_w, out_h) = skyvis_dims(w, h, opts.downsample);
    let ds = opts.downsample.max(1) as f32;
    let k = opts.k_azimuths.max(1);
    let grid = terrain_grid.max(1e-3);
    let sector = std::f32::consts::TAU / (k as f32);

    let count = (out_w as usize) * (out_h as usize);
    let previous = previous.filter(|(old, raw)| old.len() == heights.len() && raw.len() == count);
    let mut out = previous.map_or_else(|| vec![0.0f32; count], |(_, raw)| raw.to_vec());
    let mut changed = (w, h, 0u32, 0u32);
    if let Some((old, _)) = previous {
        for (i, (&a, &b)) in old.iter().zip(heights).enumerate() {
            if a != b {
                let x = i as u32 % w;
                let z = i as u32 / w;
                changed.0 = changed.0.min(x);
                changed.1 = changed.1.min(z);
                changed.2 = changed.2.max(x);
                changed.3 = changed.3.max(z);
            }
        }
    }
    // Include the bilinear sampling footprint as well as the ray's full radius.
    let reach = opts.radius_m.max(0.0) / grid + 1.0;
    for oy in 0..out_h {
        for ox in 0..out_w {
            // Heightfield-texel coordinate at the centre of this output block.
            let hx = (ox as f32 + 0.5) * ds;
            let hz = (oy as f32 + 0.5) * ds;
            if previous.is_some() {
                let cx = hx.clamp(0.0, w.saturating_sub(1) as f32);
                let cz = hz.clamp(0.0, h.saturating_sub(1) as f32);
                if changed.0 == w
                    || cx < changed.0 as f32 - reach
                    || cx > changed.2 as f32 + reach
                    || cz < changed.1 as f32 - reach
                    || cz > changed.3 as f32 + reach
                {
                    continue;
                }
            }
            let h0 = sample_bilinear(heights, w, h, hx, hz);

            // Per-texel rotation of the whole even fan by up to one sector: decorrelates the
            // fixed-direction error (which, shared by every texel, reads as a coherent starburst)
            // into fine noise the blur removes. Even spacing keeps the per-texel variance low.
            let rot = hash2(ox, oy) * sector;

            let mut vis_sum = 0.0f32;
            for i in 0..k {
                let phi = sector * (i as f32) + rot;
                let (dz, dx) = phi.sin_cos(); // dx = cos, dz = sin (unit xz azimuth)
                // Per-(texel, direction) radial jitter: shift the geometric march's start so the
                // discrete sample distances differ between neighbours, breaking the concentric
                // banding a fixed step sequence would stamp everywhere.
                let j = hash2(
                    ox.wrapping_add(i.wrapping_mul(7919)),
                    oy.wrapping_add(i.wrapping_mul(104729)),
                );
                // March outward, tracking the steepest horizon slope this azimuth raises.
                let mut t_max = 0.0f32;
                let mut d = grid * (0.5 + j); // jittered start (~0.5..1.5 texels)
                let mut step = grid;
                while d <= opts.radius_m {
                    let sx = hx + dx * (d / grid);
                    let sz = hz + dz * (d / grid);
                    let hs = sample_bilinear(heights, w, h, sx, sz);
                    let t = (hs - h0) / d; // rise over run (metres/metres)
                    if t > t_max {
                        t_max = t;
                    }
                    step *= STEP_GROWTH;
                    d += step;
                }
                // Cosine-weighted visible-sky fraction of this azimuth slice: cos^2(alpha) = 1/(1+t^2).
                vis_sum += 1.0 / (1.0 + t_max * t_max);
            }
            out[(oy as usize) * (out_w as usize) + ox as usize] = vis_sum / (k as f32);
        }
    }
    // Smooth the coarse map so the shader's bilinear tap doesn't Mach-band into facets on
    // high-contrast cliffs, and to erase the per-texel jitter noise. Runs once per bake.
    let blurred = blur_separable(&out, out_w as usize, out_h as usize, opts.blur_radius);
    (out_w, out_h, out, blurred)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn cached_edits_match_full_scan_including_clamped_edges_and_blur() {
        let (w, h) = (65, 49);
        for ds in [1, 2, 4, 8] {
            let opts = SkyvisOptions {
                downsample: ds,
                radius_m: 35.0,
                ..Default::default()
            };
            let mut heights: Vec<f32> = (0..w * h).map(|i| ((i * 37) % 61) as f32).collect();
            let (_, _, mut raw, _) = compute_cached(&heights, w, h, 10.0, opts, None);
            for (x, z) in [(32, 24), (0, 0), (64, 48), (0, 30), (64, 2), (17, 48)] {
                let old = heights.clone();
                heights[(z * w + x) as usize] += 70.0;
                let (_, _, next, filtered) =
                    compute_cached(&heights, w, h, 10.0, opts, Some((&old, &raw)));
                let (_, _, reference) = compute(&heights, w, h, 10.0, opts);
                assert_eq!(filtered, reference, "edge edit ({x},{z}) ds={ds}");
                raw = next;
            }
            let (_, _, unchanged, filtered) =
                compute_cached(&heights, w, h, 10.0, opts, Some((&heights, &raw)));
            assert_eq!(unchanged, raw);
            assert_eq!(filtered, compute(&heights, w, h, 10.0, opts).2);
        }
    }

    #[test]
    fn cached_local_edit_timing() {
        let (w, h) = (513, 513);
        let opts = SkyvisOptions::default();
        let mut heights = vec![0.0; (w * h) as usize];
        let start = std::time::Instant::now();
        let (_, _, raw, _) = compute_cached(&heights, w, h, 25.0, opts, None);
        let full = start.elapsed();
        let old = heights.clone();
        heights[(256 * w + 256) as usize] = 50.0;
        let start = std::time::Instant::now();
        let (_, _, _, filtered) = compute_cached(&heights, w, h, 25.0, opts, Some((&old, &raw)));
        let patch = start.elapsed();
        assert_eq!(filtered, compute(&heights, w, h, 25.0, opts).2);
        eprintln!("Sky visibility 513-square: full={full:?}, local={patch:?}");
    }

    #[test]
    fn flat_terrain_is_fully_visible() {
        let (w, h) = (32u32, 32u32);
        let heights = vec![10.0f32; (w * h) as usize];
        let (_, _, v) = compute(&heights, w, h, 10.0, SkyvisOptions::default());
        // A flat field occludes nothing: V == 1 everywhere.
        for val in v {
            assert!((val - 1.0).abs() < 1e-4, "flat V should be 1, got {val}");
        }
    }

    #[test]
    fn tall_wall_occludes_nearby_columns() {
        // A single tall ridge along the x=0 column; a point right beside it sees less sky than a
        // point far away on the flat plain.
        let (w, h) = (64u32, 8u32);
        let mut heights = vec![0.0f32; (w * h) as usize];
        for z in 0..h {
            heights[(z * w) as usize] = 500.0; // wall at column 0
        }
        let opts = SkyvisOptions {
            downsample: 1,
            k_azimuths: 16,
            radius_m: 400.0,
            blur_radius: 0, // test the raw scan, not the blur
        };
        let (out_w, _, v) = compute(&heights, w, h, 10.0, opts);
        let near = v[(4 * out_w + 1) as usize]; // one texel from the wall
        let far = v[(4 * out_w + 60) as usize]; // far out on the plain
        assert!(near < far, "near-wall V ({near}) should be < far V ({far})");
        assert!(near < 1.0, "near-wall V ({near}) should be occluded");
        assert!(far > 0.95, "far V ({far}) should be near-open");
    }
}
