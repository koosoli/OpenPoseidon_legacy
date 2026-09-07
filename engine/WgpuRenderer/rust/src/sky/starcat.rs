// Bright-star catalogue for the night sky.
//
// WHY A CATALOGUE AND NOT A TEXTURE OR A HASH
// -------------------------------------------
// The field this replaces hashed a cube grid and dropped one star per cell at a random offset
// with a random magnitude. That gives an even dot screen with no constellations, no relative
// brightness anyone can recognise, and colours drawn from a hash rather than from spectral class.
// A catalogue of a few thousand real stars fixes all three at once, costs ~300 KB of GPU memory,
// and stays SHARP at any resolution because each star is still evaluated analytically per pixel
// rather than sampled out of a bitmap.
//
// The diffuse Milky Way glow is the one thing points cannot do, and it is handled separately in
// sky.wgsl (see `milky_way`) — a band on the galactic great circle, placed at the real galactic
// pole so a real equirectangular map drops into the same place later.
//
// ON-DISK FORMAT (`.pstar`) — deliberately trivial, so swapping the placeholder for a real
// catalogue is a DATA change and not a code change:
//
//     offset  type        meaning
//     0       u8[4]       magic "PSTR"
//     4       u32 LE      version (1)
//     8       u32 LE      record count
//     12      u32 LE      flags (reserved, 0)
//     16      record[]    16 bytes each, all f32 LE:
//                           ra_deg   right ascension,  degrees, J2000, [0,360)
//                           dec_deg  declination,      degrees, J2000, [-90,90]
//                           vmag     visual magnitude (lower = brighter; Sirius = -1.46)
//                           bv       B-V colour index (blue ~ -0.3, red ~ +1.8)
//
// `tools/sky/make_star_catalogue.py` writes this from a public-domain catalogue, and can also
// emit the same generated placeholder this module falls back to.
//
// SEARCH ORDER for the file (first hit wins), so a deployed game folder and a dev checkout both
// work without configuration:
//     1. $WGR_STAR_CATALOG                       (absolute path, for experiments)
//     2. <exe dir>/sky/bright_stars.pstar        (deployed)
//     3. <exe dir>/../resources/sky/bright_stars.pstar
//     4. resources/sky/bright_stars.pstar        (relative to cwd — repo checkout)
// If none exists the module GENERATES a placeholder so the whole path is testable end to end.
// The log line says which of the two you are looking at; do not report on constellations
// without reading it.

use std::path::PathBuf;

/// Cells per cube-face edge. 64 gives cells of ~1.4 deg, i.e. ~0.2 stars per cell for a
/// naked-eye catalogue — so the per-pixel loop reads a handful of stars at most.
pub const GRID_N: u32 = 64;
pub const CELL_COUNT: usize = 6 * (GRID_N as usize) * (GRID_N as usize);

/// Faintest magnitude the brightness curve is normalised against. A star at exactly this
/// magnitude gets amplitude 1.0; everything brighter scales up from there.
const MAG_LIMIT: f32 = 6.5;

/// Flux is a 1000:1 range over the naked-eye catalogue and a linear mapping is unusable — the
/// faint majority vanishes and Sirius blows out to a white disc. The eye does not work that way
/// either. Compressing with this exponent lands the range at about 16:1, which is what a
/// photograph of a real sky shows, and it is the single number to turn if bright stars look
/// too shouty or the field looks flat.
const AMP_GAMMA: f32 = 0.40;

/// Angular radius of the faintest star's disc, radians. ~0.055 deg, about one pixel at 1600x900
/// with a 70 deg field — the shader floors this at the actual pixel size so it never scintillates
/// out of existence when the camera moves.
const BASE_RADIUS: f32 = 0.00096;

/// How much brighter stars swell. A first-magnitude star reads bigger than a fifth-magnitude one
/// to the naked eye (seeing plus glare in the eye), and reproducing that is most of what makes a
/// constellation pop out of the field rather than hide in it.
const RADIUS_PER_DEX: f32 = 0.30;

/// Naked-eye star colour is washed out — only the brightest few show any tint at all. Full
/// blackbody saturation looks like a Christmas tree. 0 = white, 1 = full blackbody chroma.
const SATURATION: f32 = 0.55;

#[repr(C)]
#[derive(Copy, Clone, Debug, bytemuck::Pod, bytemuck::Zeroable)]
pub struct StarGpu {
    /// Unit vector in the EQUATORIAL frame: +z = north celestial pole, +x = RA 0h.
    pub dir: [f32; 3],
    /// Linear brightness multiplier (see AMP_GAMMA).
    pub amp: f32,
    /// Linear RGB, normalised to luminance 1 so `amp` alone controls brightness.
    pub color: [f32; 3],
    /// Angular radius of the disc, radians.
    pub radius: f32,
}

#[derive(Copy, Clone, Debug)]
pub struct StarRecord {
    pub ra_deg: f32,
    pub dec_deg: f32,
    pub vmag: f32,
    pub bv: f32,
}

pub struct StarCatalogue {
    /// Stars, reordered so that every cell's members are contiguous.
    pub stars: Vec<StarGpu>,
    /// One entry per cube-grid cell: (count << 24) | offset into `stars`.
    pub cells: Vec<u32>,
    /// Where the data came from, for the log line.
    pub source: String,
}

// ---------------------------------------------------------------------------------------------
// Cube-grid cell addressing. MUST match `star_cell_index` in sky.wgsl exactly — a divergence
// here does not error, it just makes stars disappear in bands, which reads as a shader bug.
// ---------------------------------------------------------------------------------------------
fn cell_index(dir: [f32; 3]) -> u32 {
    let (x, y, z) = (dir[0], dir[1], dir[2]);
    let (ax, ay, az) = (x.abs(), y.abs(), z.abs());
    let (face, sc, tc, ma): (u32, f32, f32, f32);
    if ax >= ay && ax >= az {
        if x > 0.0 {
            face = 0;
            sc = -z;
            tc = -y;
        } else {
            face = 1;
            sc = z;
            tc = -y;
        }
        ma = ax;
    } else if ay >= az {
        if y > 0.0 {
            face = 2;
            sc = x;
            tc = z;
        } else {
            face = 3;
            sc = x;
            tc = -z;
        }
        ma = ay;
    } else {
        if z > 0.0 {
            face = 4;
            sc = x;
            tc = -y;
        } else {
            face = 5;
            sc = -x;
            tc = -y;
        }
        ma = az;
    }
    let inv = 1.0 / ma.max(1e-8);
    let u = 0.5 * (sc * inv + 1.0);
    let v = 0.5 * (tc * inv + 1.0);
    let n = GRID_N as f32;
    let i = ((u * n).floor() as i32).clamp(0, GRID_N as i32 - 1) as u32;
    let j = ((v * n).floor() as i32).clamp(0, GRID_N as i32 - 1) as u32;
    face * GRID_N * GRID_N + j * GRID_N + i
}

fn normalize3(v: [f32; 3]) -> [f32; 3] {
    let l = (v[0] * v[0] + v[1] * v[1] + v[2] * v[2]).sqrt().max(1e-8);
    [v[0] / l, v[1] / l, v[2] / l]
}

// ---------------------------------------------------------------------------------------------
// B-V colour index -> linear RGB.
//
// Ballesteros' formula gives an effective temperature from B-V; a standard blackbody fit turns
// that into RGB. Both are published approximations, not measurements, and that is fine here:
// the owner's bar is "looks real", and what matters is that Betelgeuse is orange, Rigel is blue
// and the rest are near-white in roughly the right proportion.
// ---------------------------------------------------------------------------------------------
fn bv_to_temperature(bv: f32) -> f32 {
    let b = bv.clamp(-0.4, 2.0);
    (4600.0 * (1.0 / (0.92 * b + 1.7) + 1.0 / (0.92 * b + 0.62))).clamp(1500.0, 40000.0)
}

fn temperature_to_rgb(kelvin: f32) -> [f32; 3] {
    // Helland's piecewise fit, in sRGB-ish 0..255, then de-gamma'd to linear below.
    let t = (kelvin / 100.0).clamp(10.0, 400.0);
    let r = if t <= 66.0 {
        255.0
    } else {
        329.698_73 * (t - 60.0).powf(-0.133_204_76)
    };
    let g = if t <= 66.0 {
        99.470_802 * t.ln() - 161.119_57
    } else {
        288.122_17 * (t - 60.0).powf(-0.075_514_846)
    };
    let b = if t >= 66.0 {
        255.0
    } else if t <= 19.0 {
        0.0
    } else {
        138.517_73 * (t - 10.0).ln() - 305.044_79
    };
    let srgb = [
        (r / 255.0).clamp(0.0, 1.0),
        (g / 255.0).clamp(0.0, 1.0),
        (b / 255.0).clamp(0.0, 1.0),
    ];
    // sRGB -> linear. The sky pass outputs linear radiance, so a gamma-space colour here would
    // make every star too saturated AND too bright in the mid tones.
    let mut lin = [0.0f32; 3];
    for k in 0..3 {
        let c = srgb[k];
        lin[k] = if c <= 0.04045 {
            c / 12.92
        } else {
            ((c + 0.055) / 1.055).powf(2.4)
        };
    }
    lin
}

fn star_color(bv: f32) -> [f32; 3] {
    let lin = temperature_to_rgb(bv_to_temperature(bv));
    // Desaturate toward white, then normalise to luminance 1 so `amp` is the only brightness
    // control. Without the normalise, a red star of the same magnitude as a white one renders
    // dimmer, because its RGB sums to less.
    let mut c = [0.0f32; 3];
    for k in 0..3 {
        c[k] = 1.0 + (lin[k] - 1.0) * SATURATION;
    }
    let luma = (0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2]).max(1e-4);
    [c[0] / luma, c[1] / luma, c[2] / luma]
}

// ---------------------------------------------------------------------------------------------
// Record -> GPU star.
// ---------------------------------------------------------------------------------------------
fn to_gpu(rec: &StarRecord) -> StarGpu {
    let ra = rec.ra_deg.to_radians();
    let dec = rec.dec_deg.to_radians();
    let cd = dec.cos();
    let dir = normalize3([cd * ra.cos(), cd * ra.sin(), dec.sin()]);

    // Magnitude is logarithmic: five magnitudes is a factor of 100 in flux.
    let flux = 10.0f32.powf(-0.4 * (rec.vmag - MAG_LIMIT));
    let amp = flux.powf(AMP_GAMMA);
    let radius = BASE_RADIUS * (1.0 + RADIUS_PER_DEX * flux.max(1.0).log10());

    StarGpu {
        dir,
        amp,
        color: star_color(rec.bv),
        radius,
    }
}

// ---------------------------------------------------------------------------------------------
// Placeholder catalogue.
//
// Real positions are the whole point of the feature, so this is NOT trying to be a stand-in for
// them — it exists so the buffers, the bind group, the cell index and the shader can all be
// exercised before the real data lands. It does reproduce the magnitude DISTRIBUTION (star counts
// go as 10^(0.6 m)), because a uniform one makes the brightness curve impossible to judge.
// ---------------------------------------------------------------------------------------------
fn generate_placeholder(count: usize) -> Vec<StarRecord> {
    let mut state: u64 = 0x9E37_79B9_7F4A_7C15;
    let mut next = || -> f32 {
        state = state
            .wrapping_mul(6364136223846793005)
            .wrapping_add(1442695040888963407);
        ((state >> 33) as u32 as f32) / (u32::MAX >> 1) as f32
    };
    let mut out = Vec::with_capacity(count);
    for _ in 0..count {
        // Uniform on the sphere: uniform in cos(dec), not in dec (which crowds the poles).
        let ra = next() * 360.0;
        let dec = (2.0 * next() - 1.0).clamp(-1.0, 1.0).asin().to_degrees();
        // Invert N(<m) = 10^(0.6 m) so the drawn magnitudes have the real cumulative shape.
        let u = next().max(1e-6);
        let vmag = (MAG_LIMIT + (u.log10() / 0.6)).clamp(-1.5, MAG_LIMIT);
        // B-V skewed toward the middle of the main sequence, with tails both ways.
        let bv = -0.3 + 2.1 * (0.5 * (next() + next()));
        out.push(StarRecord {
            ra_deg: ra,
            dec_deg: dec,
            vmag,
            bv,
        });
    }
    out
}

// ---------------------------------------------------------------------------------------------
// File loading.
// ---------------------------------------------------------------------------------------------
fn candidate_paths() -> Vec<PathBuf> {
    let mut v = Vec::new();
    if let Ok(p) = std::env::var("WGR_STAR_CATALOG") {
        if !p.is_empty() {
            v.push(PathBuf::from(p));
        }
    }
    if let Ok(exe) = std::env::current_exe() {
        if let Some(dir) = exe.parent() {
            v.push(dir.join("sky").join("bright_stars.pstar"));
            v.push(
                dir.join("..")
                    .join("resources")
                    .join("sky")
                    .join("bright_stars.pstar"),
            );
        }
    }
    v.push(PathBuf::from("resources/sky/bright_stars.pstar"));
    v
}

fn parse(bytes: &[u8]) -> Option<Vec<StarRecord>> {
    if bytes.len() < 16 || &bytes[0..4] != b"PSTR" {
        return None;
    }
    let u32_at = |o: usize| u32::from_le_bytes([bytes[o], bytes[o + 1], bytes[o + 2], bytes[o + 3]]);
    if u32_at(4) != 1 {
        return None;
    }
    let count = u32_at(8) as usize;
    if bytes.len() < 16 + count * 16 {
        return None;
    }
    let f32_at = |o: usize| {
        f32::from_le_bytes([bytes[o], bytes[o + 1], bytes[o + 2], bytes[o + 3]])
    };
    let mut out = Vec::with_capacity(count);
    for k in 0..count {
        let o = 16 + k * 16;
        out.push(StarRecord {
            ra_deg: f32_at(o),
            dec_deg: f32_at(o + 4),
            vmag: f32_at(o + 8),
            bv: f32_at(o + 12),
        });
    }
    Some(out)
}

/// The shipped catalogue, compiled into the binary. Yale Bright Star Catalogue, 5th edition
/// (Hoffleit & Warren), as distributed by CDS/ADC as V/50 -- PUBLIC DOMAIN, which is why it is
/// this one and not HYG (CC BY-SA, which would need attribution and a redistribution story).
/// Built by tools/sky/make_star_catalogue.py --bsc; 8404 stars to visual magnitude 6.50, the
/// naked-eye limit, brightest Sirius at -1.46.
const EMBEDDED_CATALOGUE: &[u8] = include_bytes!("../../../../../resources/sky/bright_stars.pstar");

// ---------------------------------------------------------------------------------------------
// Build.
// ---------------------------------------------------------------------------------------------
impl StarCatalogue {
    pub fn load() -> Self {
        for p in candidate_paths() {
            let Ok(bytes) = std::fs::read(&p) else {
                continue;
            };
            match parse(&bytes) {
                Some(recs) if !recs.is_empty() => {
                    let n = recs.len();
                    return Self::build(recs, format!("{} ({} stars)", p.display(), n));
                }
                _ => {
                    // A file that exists but does not parse is a deployment error worth saying
                    // out loud rather than silently falling through to the placeholder.
                    eprintln!(
                        "[wgr] sky: star catalogue at {} is not a valid .pstar file - ignoring",
                        p.display()
                    );
                }
            }
        }
        // SKY-002: the real catalogue is COMPILED IN, so it cannot be lost by a deploy, a
        // partial copy, or someone moving just the executable. At 131 KiB that is a trivial
        // price for a file that must never be missing -- and it makes the external paths above
        // an OVERRIDE rather than a requirement, which is what you want for a data file a
        // curious owner might swap.
        //
        // The generated placeholder stays as the last resort so the module still compiles and
        // runs in a tree that has not built the .pstar yet, and the name it reports says
        // plainly which of the three you are looking at.
        match parse(EMBEDDED_CATALOGUE) {
            Some(recs) if !recs.is_empty() => {
                let n = recs.len();
                return Self::build(recs, format!("embedded Yale BSC5 ({n} stars)"));
            }
            _ => eprintln!("[wgr] sky: the EMBEDDED star catalogue failed to parse - this is a build error"),
        }
        let recs = generate_placeholder(5000);
        let n = recs.len();
        Self::build(
            recs,
            format!("GENERATED PLACEHOLDER ({n} stars, positions are not real)"),
        )
    }

    fn build(recs: Vec<StarRecord>, source: String) -> Self {
        let gpu: Vec<StarGpu> = recs.iter().map(to_gpu).collect();

        // Which cells does each star need to appear in? Its own, plus any cell its disc spills
        // into. Probing the cell of eight points on the disc's rim catches face crossings for
        // free — deriving the neighbours arithmetically does not, because a cube face's
        // neighbours are not its (i +/- 1, j +/- 1).
        let mut membership: Vec<(u32, u32)> = Vec::with_capacity(gpu.len() * 2);
        for (idx, s) in gpu.iter().enumerate() {
            let d = s.dir;
            // Any two vectors orthogonal to d.
            let up = if d[2].abs() < 0.9 {
                [0.0, 0.0, 1.0]
            } else {
                [1.0, 0.0, 0.0]
            };
            let t1 = normalize3([
                up[1] * d[2] - up[2] * d[1],
                up[2] * d[0] - up[0] * d[2],
                up[0] * d[1] - up[1] * d[0],
            ]);
            let t2 = [
                d[1] * t1[2] - d[2] * t1[1],
                d[2] * t1[0] - d[0] * t1[2],
                d[0] * t1[1] - d[1] * t1[0],
            ];
            let r = s.radius * 1.5;
            let mut cells: [u32; 9] = [u32::MAX; 9];
            let mut used = 0usize;
            let mut push = |c: u32, cells: &mut [u32; 9], used: &mut usize| {
                if !cells[..*used].contains(&c) {
                    cells[*used] = c;
                    *used += 1;
                }
            };
            push(cell_index(d), &mut cells, &mut used);
            for k in 0..8 {
                let a = (k as f32) * std::f32::consts::FRAC_PI_4;
                let (sa, ca) = (a.sin(), a.cos());
                let p = normalize3([
                    d[0] + r * (ca * t1[0] + sa * t2[0]),
                    d[1] + r * (ca * t1[1] + sa * t2[1]),
                    d[2] + r * (ca * t1[2] + sa * t2[2]),
                ]);
                push(cell_index(p), &mut cells, &mut used);
            }
            for c in &cells[..used] {
                membership.push((*c, idx as u32));
            }
        }

        membership.sort_unstable();

        let mut stars = Vec::with_capacity(membership.len());
        let mut cells = vec![0u32; CELL_COUNT];
        let mut i = 0usize;
        while i < membership.len() {
            let cell = membership[i].0;
            let start = stars.len();
            let mut j = i;
            while j < membership.len() && membership[j].0 == cell {
                stars.push(gpu[membership[j].1 as usize]);
                j += 1;
            }
            let count = (stars.len() - start).min(255) as u32;
            stars.truncate(start + count as usize);
            cells[cell as usize] = (count << 24) | (start as u32 & 0x00FF_FFFF);
            i = j;
        }

        // A star buffer must never be empty: an empty storage buffer is a validation error, and
        // the branch that would avoid binding it does not exist in the sky pipeline.
        if stars.is_empty() {
            stars.push(StarGpu {
                dir: [0.0, 0.0, 1.0],
                amp: 0.0,
                color: [1.0, 1.0, 1.0],
                radius: BASE_RADIUS,
            });
        }

        StarCatalogue {
            stars,
            cells,
            source,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn cell_index_is_in_range_over_the_whole_sphere() {
        // The shader indexes `star_cells` with this number and WGSL clamps nothing for us.
        let mut n = 0u32;
        for a in 0..64 {
            for b in 0..64 {
                let theta = (a as f32 + 0.5) / 64.0 * std::f32::consts::PI;
                let phi = (b as f32 + 0.5) / 64.0 * std::f32::consts::TAU;
                let d = [
                    theta.sin() * phi.cos(),
                    theta.sin() * phi.sin(),
                    theta.cos(),
                ];
                assert!((cell_index(d) as usize) < CELL_COUNT);
                n += 1;
            }
        }
        assert_eq!(n, 4096);
    }

    #[test]
    fn brighter_stars_are_brighter_and_bigger() {
        let sirius = to_gpu(&StarRecord {
            ra_deg: 101.3,
            dec_deg: -16.7,
            vmag: -1.46,
            bv: 0.0,
        });
        let faint = to_gpu(&StarRecord {
            ra_deg: 0.0,
            dec_deg: 0.0,
            vmag: 6.0,
            bv: 0.0,
        });
        assert!(sirius.amp > faint.amp * 8.0, "amp {} vs {}", sirius.amp, faint.amp);
        // ...but not by the raw 1000:1 flux ratio, which is unrenderable.
        assert!(sirius.amp < faint.amp * 40.0);
        assert!(sirius.radius > faint.radius);
    }

    #[test]
    fn colour_is_normalised_and_ordered_by_temperature() {
        let blue = star_color(-0.30);
        let red = star_color(1.60);
        for c in [blue, red] {
            let luma = 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2];
            assert!((luma - 1.0).abs() < 1e-3, "luma {luma}");
        }
        assert!(blue[2] > blue[0], "a hot star must be blue-heavy");
        assert!(red[0] > red[2], "a cool star must be red-heavy");
    }

    #[test]
    fn placeholder_builds_a_consistent_index() {
        let cat = StarCatalogue::build(generate_placeholder(1200), "test".into());
        assert_eq!(cat.cells.len(), CELL_COUNT);
        let mut seen = 0usize;
        for c in &cat.cells {
            let count = (c >> 24) as usize;
            let off = (c & 0x00FF_FFFF) as usize;
            assert!(off + count <= cat.stars.len());
            seen += count;
        }
        // Every star lands in at least its own cell.
        assert!(seen >= 1200, "only {seen} memberships for 1200 stars");
    }

    #[test]
    fn a_star_is_found_in_the_cell_its_own_direction_maps_to() {
        // This is the invariant the shader depends on. If cell_index here and star_cell_index in
        // sky.wgsl ever drift apart, this test still passes and the sky goes empty in bands —
        // so read it as necessary, not sufficient.
        let recs = generate_placeholder(400);
        let cat = StarCatalogue::build(recs.clone(), "test".into());
        for r in recs.iter().take(50) {
            let g = to_gpu(r);
            let c = cat.cells[cell_index(g.dir) as usize];
            let count = (c >> 24) as usize;
            let off = (c & 0x00FF_FFFF) as usize;
            let found = cat.stars[off..off + count]
                .iter()
                .any(|s| (s.dir[0] - g.dir[0]).abs() < 1e-6 && (s.dir[1] - g.dir[1]).abs() < 1e-6);
            assert!(found, "star at {:?} not in its own cell", g.dir);
        }
    }
}
