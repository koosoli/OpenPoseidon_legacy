// GRS-D — procedural blade/flower albedo for the near LOD.
//
// Eight species in a 64x256 RGBA8 texture ARRAY (one layer each), not a column
// atlas: with an atlas the lower mips average neighbouring columns together, so
// a distant fine blade would fade into the flower beside it. Layers mip
// independently.
//
// Generated on the CPU at init rather than shipped as image files: the crate has
// no image decoder, the repo is GPL while the game's own grass textures are
// APL-SA, and keeping it as code means the palette stays tweakable here.
//
// NOTE ON REALISM: this is procedural, so it is detailed but not photographic.
// Real grass albedo carries irregular chlorophyll mottling, insect damage and
// dirt that no closed-form function reproduces convincingly. What is modelled
// here is the structure that actually reads at gameplay distance: the midrib
// crease, lengthwise vein bundles, translucent thinning toward the edges, tip
// dieback, and for flowers a radial petal head.
//
// The blade geometry supplies the silhouette, so this stays opaque — no alpha
// cutout, no discard, and the near colour pass keeps early-Z.

pub const LAYER_W: u32 = 64;
pub const LAYER_H: u32 = 256;
pub const LAYERS: u32 = 8;

/// Which species a layer is. The compute placement pass picks one per clump and
/// stores the index in `packed.w`; the vertex shader also reads it to vary the
/// blade's width profile, so the ordering here is an ABI shared with grass.wgsl.
///
/// Grouped so the shader can classify by range: 0..4 grass, 4..6 weed, 6..8 flower.
/// These mirror the same-named constants in grass.wgsl (which cannot import Rust);
/// the tests below are what keep the two definitions honest.
#[allow(dead_code)]
pub const SPECIES_GRASS_END: u32 = 4;
#[allow(dead_code)]
pub const SPECIES_WEED_END: u32 = 6;

struct Species {
    root: [f32; 3],
    tip: [f32; 3],
    /// Midrib crease darkness.
    rib: f32,
    /// Lengthwise vein contrast.
    vein: f32,
    /// Dry/dieback amount, biased toward the tip.
    dryness: f32,
    /// Petal head colour; `None` for foliage.
    flower: Option<[f32; 3]>,
    /// Fraction of the layer (from the tip) the flower head occupies.
    head: f32,
}

const fn foliage(root: [f32; 3], tip: [f32; 3], rib: f32, vein: f32, dryness: f32) -> Species {
    Species {
        root,
        tip,
        rib,
        vein,
        dryness,
        flower: None,
        head: 0.0,
    }
}

// Midrib/vein contrast is deliberately strong: the layer is sampled across a
// ribbon only a few pixels wide, so subtle structure reads as flat colour.
const SPECIES: [Species; LAYERS as usize] = [
    // --- grass (0..4) ---
    foliage(
        [0.055, 0.135, 0.030],
        [0.300, 0.430, 0.110],
        0.50,
        0.26,
        0.05,
    ), // fine blade
    foliage(
        [0.070, 0.160, 0.040],
        [0.255, 0.400, 0.120],
        0.40,
        0.18,
        0.08,
    ), // broad meadow
    foliage(
        [0.105, 0.115, 0.045],
        [0.470, 0.400, 0.150],
        0.45,
        0.24,
        0.55,
    ), // dry stem
    foliage(
        [0.045, 0.110, 0.028],
        [0.190, 0.300, 0.080],
        0.34,
        0.20,
        0.03,
    ), // dense low
    // --- weed (4..6): broader, flatter leaves with strong veins ---
    foliage(
        [0.050, 0.140, 0.045],
        [0.150, 0.320, 0.090],
        0.55,
        0.38,
        0.06,
    ), // clover/broadleaf
    foliage(
        [0.080, 0.120, 0.038],
        [0.300, 0.330, 0.095],
        0.46,
        0.42,
        0.30,
    ), // ragged weed
    // --- flower (6..8): green stem with a coloured head at the tip ---
    Species {
        root: [0.050, 0.120, 0.030],
        tip: [0.130, 0.230, 0.070],
        rib: 0.22,
        vein: 0.10,
        dryness: 0.04,
        flower: Some([0.880, 0.870, 0.760]), // white/cream daisy
        head: 0.26,
    },
    Species {
        root: [0.050, 0.115, 0.030],
        tip: [0.140, 0.220, 0.070],
        rib: 0.22,
        vein: 0.10,
        dryness: 0.05,
        flower: Some([0.720, 0.230, 0.260]), // red/pink poppy
        head: 0.22,
    },
];

// Small deterministic PRNG — the crate has no `rand`, and a fixed sequence keeps
// the generated texture identical across runs and machines.
struct Rng(u32);

impl Rng {
    fn next_f32(&mut self) -> f32 {
        self.0 ^= self.0 << 13;
        self.0 ^= self.0 >> 17;
        self.0 ^= self.0 << 5;
        (self.0 >> 8) as f32 * (1.0 / 16_777_216.0)
    }
}

/// Value noise on a coarse lattice, smoothed — used for the irregular chlorophyll
/// mottling that a pure sine pattern cannot give.
fn mottle(u: f32, v: f32, freq: f32, seed: u32) -> f32 {
    let hash = |xi: i32, yi: i32| -> f32 {
        let mut h =
            (xi as u32).wrapping_mul(0x9e3779b9) ^ (yi as u32).wrapping_mul(0x85ebca6b) ^ seed;
        h ^= h >> 16;
        h = h.wrapping_mul(0x7feb352d);
        h ^= h >> 15;
        (h >> 8) as f32 * (1.0 / 16_777_216.0)
    };
    let (px, py) = (u * freq, v * freq);
    let (xi, yi) = (px.floor() as i32, py.floor() as i32);
    let (fx, fy) = (px - px.floor(), py - py.floor());
    let (sx, sy) = (fx * fx * (3.0 - 2.0 * fx), fy * fy * (3.0 - 2.0 * fy));
    let a = hash(xi, yi);
    let b = hash(xi + 1, yi);
    let c = hash(xi, yi + 1);
    let d = hash(xi + 1, yi + 1);
    (a + (b - a) * sx) + ((c + (d - c) * sx) - (a + (b - a) * sx)) * sy
}

/// One layer's mip 0, RGBA8 (alpha always 255 — the geometry is the silhouette).
fn generate_layer(index: usize) -> Vec<u8> {
    let sp = &SPECIES[index];
    let mut rng = Rng(0x9e3779b9 ^ (index as u32 + 1).wrapping_mul(0x85ebca6b));
    let mut out = vec![0u8; (LAYER_W * LAYER_H * 4) as usize];
    for y in 0..LAYER_H {
        // v = 0 at the tip (row 0), 1 at the root — matches `v = 1 - height_t`.
        let v = y as f32 / (LAYER_H - 1) as f32;
        let t = 1.0 - v;
        for x in 0..LAYER_W {
            let u = x as f32 / (LAYER_W - 1) as f32;
            let centred = u - 0.5;

            // Central midrib: the fold down a real blade, darkest at the crease.
            let crease = 1.0 - sp.rib * (-(centred * centred) / 0.006).exp();
            // Thin translucent margins catch light at the rolled edges.
            let edge_d = centred.abs() - 0.42;
            let edge = 1.0 + 0.20 * (-(edge_d * edge_d) / 0.004).exp();
            // Vein bundles: two octaves so they are not a single clean ripple.
            let drift = (v * 9.0 + index as f32).sin() * 0.03;
            let vein_a = ((u + drift) * std::f32::consts::PI * 26.0 + index as f32 * 2.1).sin();
            let vein_b = ((u + drift) * std::f32::consts::PI * 61.0).sin() * 0.4;
            let veins = 1.0 + sp.vein * (vein_a + vein_b) * (0.35 + 0.65 * t);
            // Irregular chlorophyll mottling — the part a sine pattern misses.
            let blotch = 0.90 + 0.20 * mottle(u, v, 7.0, 0x51ed270b ^ index as u32);
            // Self-occlusion right at the base of the blade.
            let base_ao = 0.55 + 0.45 * (t / 0.18).clamp(0.0, 1.0);
            let grain = 1.0 + (rng.next_f32() - 0.5) * 0.05;
            // Dry/blighted streaks, biased toward the tip.
            let blight = (((rng.next_f32() - 0.62) / 0.38).clamp(0.0, 1.0))
                * sp.dryness
                * ((t - 0.35) / 0.65).clamp(0.0, 1.0);

            let shade = crease * edge * veins * blotch * base_ao * grain;
            let px = ((y * LAYER_W + x) * 4) as usize;

            // Flower head: radial petals over the top `head` fraction, fading
            // into the stem so there is no hard seam.
            let (petal_mix, petal_shade) = match sp.flower {
                Some(_) if v < sp.head => {
                    // Local polar coordinates about the head's centre.
                    let hy = (v / sp.head - 0.5) * 2.0; // -1 (tip) .. 1 (stem side)
                    let hx = centred * 2.4;
                    let r = (hx * hx + hy * hy).sqrt();
                    let angle = hy.atan2(hx);
                    // Eight petals; the lobe function dips between them.
                    let lobe = 0.55 + 0.45 * (angle * 8.0).cos().abs();
                    let inside = 1.0 - ((r / lobe - 0.85) / 0.35).clamp(0.0, 1.0);
                    // Bright disc floret at the very centre.
                    let disc = 1.0 - (r / 0.25).clamp(0.0, 1.0);
                    let radial = 0.82 + 0.18 * (1.0 - r.min(1.0));
                    (inside, radial + disc * 0.25)
                }
                _ => (0.0, 1.0),
            };

            for c in 0..3 {
                let base = sp.root[c] * (1.0 - t) + sp.tip[c] * t;
                let dry = [0.42, 0.34, 0.13][c];
                let foliage_lin = (base * shade) * (1.0 - blight) + dry * blight;
                let lin = match sp.flower {
                    Some(petal) if petal_mix > 0.0 => {
                        // Blue channel is damped so the disc florets read warm.
                        let warm: f32 = if c == 2 { 0.72 } else { 1.0 };
                        let petal_lin = petal[c] * petal_shade * warm;
                        foliage_lin * (1.0 - petal_mix) + petal_lin * petal_mix
                    }
                    _ => foliage_lin,
                };
                // sRGB encode; the texture is Rgba8UnormSrgb so the sampler
                // linearises it back before it reaches the lighting maths.
                let enc = lin.clamp(0.0, 1.0).powf(1.0 / 2.2);
                out[px + c] = (enc * 255.0 + 0.5) as u8;
            }
            out[px + 3] = 255;
        }
    }
    out
}

/// Upload a photographed tuft (the game's own decoded PAA) as a mipped 2D
/// texture. Cutout alpha is preserved, so the mid fragment shader can alpha-test
/// it; mips are generated with the same box filter as the procedural layers.
///
/// Mips use an alpha-weighted filter (see `downsample_cutout`) and are then
/// coverage-corrected, the standard pair for alpha-tested foliage: the first
/// stops transparent black bleeding into the colour, the second stops the tuft
/// thinning out with distance.
pub fn create_tuft(
    device: &wgpu::Device,
    queue: &wgpu::Queue,
    width: u32,
    height: u32,
    rgba: &[u8],
) -> Option<wgpu::TextureView> {
    create_tufts(device, queue, width, height, 1, rgba)
}

/// Creates a texture-array of same-sized, alpha-tested photographed clumps.
/// Every layer gets independent coverage-corrected mips so mixing never makes
/// the distant cards turn into an opaque dark sheet.
pub fn create_tufts(
    device: &wgpu::Device,
    queue: &wgpu::Queue,
    width: u32,
    height: u32,
    layers: u32,
    rgba: &[u8],
) -> Option<wgpu::TextureView> {
    let layer_bytes = width as usize * height as usize * 4;
    if width == 0 || height == 0 || layers == 0 || rgba.len() < layer_bytes * layers as usize {
        return None;
    }
    let mip_levels = 32 - width.max(height).leading_zeros();
    let texture = device.create_texture(&wgpu::TextureDescriptor {
        label: Some("wgr_grass_tuft"),
        size: wgpu::Extent3d {
            width,
            height,
            depth_or_array_layers: layers,
        },
        mip_level_count: mip_levels,
        sample_count: 1,
        // Photo-tuft inputs are ordinary colour images. Decode their stored sRGB
        // albedo before lighting; treating A3's colour data as legacy linear
        // values washes the clumps into the grey-white regression seen in GRS-030.
        dimension: wgpu::TextureDimension::D2,
        format: wgpu::TextureFormat::Rgba8UnormSrgb,
        usage: wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::COPY_DST,
        view_formats: &[],
    });
    // Must match TUFT_ALPHA_CUTOFF in grass.wgsl (0.5 * 255).
    const CUTOFF: u8 = 128;
    for layer in 0..layers {
        let start = layer as usize * layer_bytes;
        let mut data = rgba[start..start + layer_bytes].to_vec();
        let base_coverage = coverage(&data, CUTOFF);
        let (mut w, mut h) = (width, height);
        for mip in 0..mip_levels {
            queue.write_texture(
                wgpu::TexelCopyTextureInfo {
                    texture: &texture,
                    mip_level: mip,
                    origin: wgpu::Origin3d {
                        x: 0,
                        y: 0,
                        z: layer,
                    },
                    aspect: wgpu::TextureAspect::All,
                },
                &data,
                wgpu::TexelCopyBufferLayout {
                    offset: 0,
                    bytes_per_row: Some(w * 4),
                    rows_per_image: Some(h),
                },
                wgpu::Extent3d {
                    width: w,
                    height: h,
                    depth_or_array_layers: 1,
                },
            );
            if mip + 1 < mip_levels {
                let (mut next, nw, nh) = downsample_cutout(&data, w, h);
                preserve_coverage(&mut next, base_coverage, CUTOFF);
                data = next;
                w = nw;
                h = nh;
            }
        }
    }
    Some(texture.create_view(&wgpu::TextureViewDescriptor {
        dimension: Some(wgpu::TextureViewDimension::D2Array),
        ..Default::default()
    }))
}

/// Number of photo-clump layers the shader indexes. Layer 0 is the primary
/// clump; 1..8 are the local families.
// 32, not 9: the atlas is now built from the MAP's clutter classes, and a
// surface's classes must occupy a CONTIGUOUS run so the shader can select one
// with `first + hash*count` instead of walking a mask. Contiguity means the
// budget is the SUM of the per-surface class lists, not their distinct union --
// Stratis is 28 slots over 21 distinct classes, Takistan 25 over 10. 32 is also
// exactly what the 5-bit `first` field in the geography word can address.
//
// The layers are 512 rather than 1024 square, so this is 42.7 MB against the
// 48 MB nine 1024-square layers cost before.
pub const TUFT_LAYERS: usize = 32;

/// The alpha threshold the trim is computed against. Deliberately far below the
/// 0.5 alpha test: the card is shrunk to this box, so anything the runtime
/// cutoff could ever keep must be inside it. Measured across the shipped plates,
/// the box is within 0.5% of the same size at alpha 8, 64 and 128, so the low
/// threshold costs nothing.
const TRIM_ALPHA: u8 = 8;

/// Mean LINEAR colour of each layer's covered texels, as (r, g, b, 1).
///
/// This is the pivot the card contrast turns about. A fixed pivot is wrong here:
/// the plates' means span 0.05 to 0.45 luma, so one constant made contrast a
/// brightness change for every plate but one. Worse, it was a DISTANCE-dependent
/// brightness change -- up close, stems above the pivot brighten and gaps below
/// it darken and roughly cancel, but as mips average each texel toward the plate
/// mean, that cancellation disappears and the whole card shifts. Pivoting on the
/// layer's own mean makes contrast brightness-neutral at every mip, so a clump
/// keeps its tone as it recedes.
///
/// Sampled on a stride rather than every texel: this runs on 4K plates at load,
/// and a mean does not need every sample to be accurate to three decimals.
pub fn tuft_layer_means(
    width: u32,
    height: u32,
    layers: u32,
    rgba: &[u8],
) -> [[f32; 4]; TUFT_LAYERS] {
    // Linear-space, matching the Rgba8UnormSrgb view the shader samples through.
    fn srgb_to_linear(v: u8) -> f32 {
        let c = v as f32 / 255.0;
        if c <= 0.04045 {
            c / 12.92
        } else {
            ((c + 0.055) / 1.055).powf(2.4)
        }
    }
    let mut table = [[0.0f32; 256]; 1];
    for (i, slot) in table[0].iter_mut().enumerate() {
        *slot = srgb_to_linear(i as u8);
    }
    let lut = table[0];
    let mut out = [[0.34, 0.34, 0.34, 1.0]; TUFT_LAYERS];
    let layer_bytes = width as usize * height as usize * 4;
    if width == 0 || height == 0 {
        return out;
    }
    let stride = 4usize;
    for layer in 0..(layers as usize).min(TUFT_LAYERS) {
        let start = layer * layer_bytes;
        let Some(data) = rgba.get(start..start + layer_bytes) else {
            break;
        };
        let (mut sum, mut count) = ([0.0f64; 3], 0u64);
        for texel in (0..data.len() / 4).step_by(stride) {
            let i = texel * 4;
            if data[i + 3] < 128 {
                continue;
            }
            sum[0] += lut[data[i] as usize] as f64;
            sum[1] += lut[data[i + 1] as usize] as f64;
            sum[2] += lut[data[i + 2] as usize] as f64;
            count += 1;
        }
        if count == 0 {
            continue;
        }
        let n = count as f64;
        out[layer] = [
            (sum[0] / n) as f32,
            (sum[1] / n) as f32,
            (sum[2] / n) as f32,
            1.0,
        ];
    }
    out
}

/// Opaque bounding box of every layer, as (u0, u1, v0, v1) in texture space.
///
/// The card quads are trimmed to these, which is pure fill saved: the plates
/// carry transparent margins the alpha test was discarding anyway, but only
/// AFTER rasterising them and paying a texture sample. This path is dominated by
/// overdraw, so removing geometry that can never produce a texel is free
/// performance with an identical image. Layers with no margin get the full
/// (0,1,0,1) box and are unchanged.
pub fn tuft_bounds(width: u32, height: u32, layers: u32, rgba: &[u8]) -> [[f32; 4]; TUFT_LAYERS] {
    let mut out = [[0.0, 1.0, 0.0, 1.0]; TUFT_LAYERS];
    let layer_bytes = width as usize * height as usize * 4;
    if width == 0 || height == 0 {
        return out;
    }
    for layer in 0..(layers as usize).min(TUFT_LAYERS) {
        let start = layer * layer_bytes;
        let Some(data) = rgba.get(start..start + layer_bytes) else {
            break;
        };
        let (mut x0, mut x1, mut y0, mut y1) = (u32::MAX, 0u32, u32::MAX, 0u32);
        for y in 0..height {
            let row = (y * width * 4) as usize;
            for x in 0..width {
                if data[row + (x * 4) as usize + 3] >= TRIM_ALPHA {
                    x0 = x0.min(x);
                    x1 = x1.max(x + 1);
                    y0 = y0.min(y);
                    y1 = y1.max(y + 1);
                }
            }
        }
        // A fully transparent layer has no box; leave it at the full quad rather
        // than collapsing the card to a degenerate triangle.
        if x0 == u32::MAX {
            continue;
        }
        out[layer] = [
            x0 as f32 / width as f32,
            x1 as f32 / width as f32,
            y0 as f32 / height as f32,
            y1 as f32 / height as f32,
        ];
    }
    out
}

/// Per-layer alpha-edge health of a photo atlas: the numbers that say whether a
/// slice can flicker under the alpha test BEFORE anyone has to step through the
/// "Force one family" combo to find it by eye.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct TuftAlphaEdgeReport {
    /// Texels with any coverage at all (alpha >= TRIM_ALPHA).
    pub covered: u32,
    /// Fraction of covered texels whose alpha is PARTIAL (between TRIM_ALPHA and
    /// 248). A hard-authored cutout is ~0; a JPEG-derived opacity map is high,
    /// and every partial texel near the 0.5 cutoff is one the mip chain and the
    /// wind can push across it from one frame to the next.
    pub partial_fraction: f32,
    /// Fraction of texels ABOVE the cutoff whose four neighbours are all below
    /// it: isolated speckles. Each is a stem fragment one texel wide, which no
    /// minification keeps stable -- the direct predictor of "this variety
    /// flickers, that one does not".
    pub speckle_fraction: f32,
    /// Fraction of covered texels lying on a cutoff edge (above with a neighbour
    /// below, or the reverse). Higher = more silhouette per unit of plant, i.e.
    /// thinner stems and more edge to shimmer.
    pub edge_fraction: f32,
}

/// Which slices have bad alpha edges. Runs once per atlas upload, so it walks
/// every texel; the cost is a few ms on a 32 x 512^2 atlas.
pub fn tuft_alpha_edge_report(
    width: u32,
    height: u32,
    layers: u32,
    rgba: &[u8],
) -> Vec<TuftAlphaEdgeReport> {
    const CUTOFF: u8 = 128;
    let mut out = Vec::new();
    if width == 0 || height == 0 {
        return out;
    }
    let (w, h) = (width as usize, height as usize);
    let layer_bytes = w * h * 4;
    let alpha_at = |data: &[u8], x: usize, y: usize| -> u8 { data[(y * w + x) * 4 + 3] };
    for layer in 0..(layers as usize).min(TUFT_LAYERS) {
        let Some(data) = rgba.get(layer * layer_bytes..(layer + 1) * layer_bytes) else {
            break;
        };
        let (mut covered, mut partial, mut above, mut speckle, mut edge) =
            (0u32, 0u32, 0u32, 0u32, 0u32);
        for y in 0..h {
            for x in 0..w {
                let a = alpha_at(data, x, y);
                if a < TRIM_ALPHA {
                    continue;
                }
                covered += 1;
                if a < 248 {
                    partial += 1;
                }
                let over = a >= CUTOFF;
                // Four-neighbourhood against the cutoff; the border counts as
                // "below", which is what the sampler's clamp-to-edge sees.
                let n = [
                    if x > 0 { alpha_at(data, x - 1, y) } else { 0 },
                    if x + 1 < w {
                        alpha_at(data, x + 1, y)
                    } else {
                        0
                    },
                    if y > 0 { alpha_at(data, x, y - 1) } else { 0 },
                    if y + 1 < h {
                        alpha_at(data, x, y + 1)
                    } else {
                        0
                    },
                ];
                let neighbours_over = n.iter().filter(|&&v| v >= CUTOFF).count();
                if over {
                    above += 1;
                    if neighbours_over == 0 {
                        speckle += 1;
                    }
                    if neighbours_over < 4 {
                        edge += 1;
                    }
                } else if neighbours_over > 0 {
                    edge += 1;
                }
            }
        }
        let frac = |n: u32, d: u32| if d == 0 { 0.0 } else { n as f32 / d as f32 };
        out.push(TuftAlphaEdgeReport {
            covered,
            partial_fraction: frac(partial, covered),
            speckle_fraction: frac(speckle, above),
            edge_fraction: frac(edge, covered),
        });
    }
    out
}

/// A 1x1 opaque stand-in so the bind group is always complete before the game
/// has loaded a tuft (or if the PAA is missing). The shader keys off a params
/// flag, not this texture's contents.
pub fn create_tuft_placeholder(device: &wgpu::Device, queue: &wgpu::Queue) -> wgpu::TextureView {
    create_tuft(device, queue, 1, 1, &[255, 255, 255, 255]).expect("1x1 tuft placeholder")
}

/// Alpha-WEIGHTED box filter for cutout textures.
///
/// A plain box filter is wrong for a cutout: `trava1_pmp2` is DXT1 1-bit alpha
/// and 79.5% of it is fully transparent, and transparent DXT1 texels decode to
/// BLACK. Averaging that black into the visible texels drags the colour down
/// hard -- by mip 5-7 (which is what a 25-64 m card samples) the tuft resolves
/// to roughly a fifth of its true colour and reads as a near-black band.
///
/// Weighting RGB by alpha and normalising by the summed alpha keeps the colour
/// of the *covered* texels; alpha itself still averages, since that is the
/// coverage ratio the alpha test wants.
fn downsample_cutout(src: &[u8], w: u32, h: u32) -> (Vec<u8>, u32, u32) {
    let dw = (w / 2).max(1);
    let dh = (h / 2).max(1);
    let mut dst = vec![0u8; (dw * dh * 4) as usize];
    for y in 0..dh {
        for x in 0..dw {
            let x0 = (x * 2).min(w - 1);
            let x1 = (x * 2 + 1).min(w - 1);
            let y0 = (y * 2).min(h - 1);
            let y1 = (y * 2 + 1).min(h - 1);
            let taps = [(x0, y0), (x1, y0), (x0, y1), (x1, y1)];
            let texel = |px: u32, py: u32| -> [u32; 4] {
                let i = ((py * w + px) * 4) as usize;
                [
                    src[i] as u32,
                    src[i + 1] as u32,
                    src[i + 2] as u32,
                    src[i + 3] as u32,
                ]
            };
            let mut rgb = [0u32; 3];
            let mut alpha_sum = 0u32;
            for (px, py) in taps {
                let t = texel(px, py);
                for c in 0..3 {
                    rgb[c] += t[c] * t[3];
                }
                alpha_sum += t[3];
            }
            let o = ((y * dw + x) * 4) as usize;
            if alpha_sum > 0 {
                for c in 0..3 {
                    dst[o + c] = (rgb[c] / alpha_sum) as u8;
                }
            }
            dst[o + 3] = ((alpha_sum + 2) / 4) as u8;
        }
    }
    (dst, dw, dh)
}

/// Fraction of texels that survive the alpha test.
fn coverage(data: &[u8], cutoff: u8) -> f32 {
    let total = data.len() / 4;
    if total == 0 {
        return 0.0;
    }
    let kept = data.chunks(4).filter(|p| p[3] >= cutoff).count();
    kept as f32 / total as f32
}

/// Rescale a mip's alpha so the same fraction of texels survives the alpha test
/// as at mip 0. Without this a cutout thins out as it drops down the chain --
/// averaging pushes alpha below the cutoff and the tuft dissolves into gaps.
fn preserve_coverage(data: &mut [u8], target: f32, cutoff: u8) {
    let (mut lo, mut hi) = (0.0f32, 8.0f32);
    // Identity unless a better scale is found: only scales that actually MEET
    // the target are recorded. Taking the last midpoint unconditionally lands on
    // a failing scale half the time, which collapses coverage to zero.
    let mut best = 1.0f32;
    for _ in 0..16 {
        let scale = 0.5 * (lo + hi);
        let hit = data
            .chunks(4)
            .filter(|p| (p[3] as f32 * scale).min(255.0) >= cutoff as f32)
            .count() as f32
            / (data.len() / 4).max(1) as f32;
        if hit >= target {
            best = scale; // smallest passing scale so far
            hi = scale;
        } else {
            lo = scale;
        }
    }
    for p in data.chunks_mut(4) {
        p[3] = (p[3] as f32 * best).min(255.0) as u8;
    }
}

/// Box-filter `src` (w x h, RGBA8) down to half size, clamping at 1.
fn downsample(src: &[u8], w: u32, h: u32) -> (Vec<u8>, u32, u32) {
    let dw = (w / 2).max(1);
    let dh = (h / 2).max(1);
    let mut dst = vec![0u8; (dw * dh * 4) as usize];
    for y in 0..dh {
        for x in 0..dw {
            // When an axis has already collapsed to 1 the two taps coincide.
            let x0 = (x * 2).min(w - 1);
            let x1 = (x * 2 + 1).min(w - 1);
            let y0 = (y * 2).min(h - 1);
            let y1 = (y * 2 + 1).min(h - 1);
            for c in 0..4 {
                let s = |px: u32, py: u32| src[((py * w + px) * 4) as usize + c] as u32;
                let sum = s(x0, y0) + s(x1, y0) + s(x0, y1) + s(x1, y1);
                dst[((y * dw + x) * 4) as usize + c] = ((sum + 2) / 4) as u8;
            }
        }
    }
    (dst, dw, dh)
}

/// Build the species texture array with a full mip chain. Distant blades cover
/// well under a pixel, so the chain is what keeps them from sparkling.
pub fn create(device: &wgpu::Device, queue: &wgpu::Queue) -> wgpu::TextureView {
    let mip_levels = 32 - LAYER_H.leading_zeros(); // floor(log2(256)) + 1 = 9
    let texture = device.create_texture(&wgpu::TextureDescriptor {
        label: Some("wgr_grass_blade_atlas"),
        size: wgpu::Extent3d {
            width: LAYER_W,
            height: LAYER_H,
            depth_or_array_layers: LAYERS,
        },
        mip_level_count: mip_levels,
        sample_count: 1,
        dimension: wgpu::TextureDimension::D2,
        format: wgpu::TextureFormat::Rgba8UnormSrgb,
        usage: wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::COPY_DST,
        view_formats: &[],
    });

    for layer in 0..LAYERS {
        let mut data = generate_layer(layer as usize);
        let (mut w, mut h) = (LAYER_W, LAYER_H);
        for mip in 0..mip_levels {
            queue.write_texture(
                wgpu::TexelCopyTextureInfo {
                    texture: &texture,
                    mip_level: mip,
                    origin: wgpu::Origin3d {
                        x: 0,
                        y: 0,
                        z: layer,
                    },
                    aspect: wgpu::TextureAspect::All,
                },
                &data,
                wgpu::TexelCopyBufferLayout {
                    offset: 0,
                    bytes_per_row: Some(w * 4),
                    rows_per_image: Some(h),
                },
                wgpu::Extent3d {
                    width: w,
                    height: h,
                    depth_or_array_layers: 1,
                },
            );
            if mip + 1 < mip_levels {
                let (next, nw, nh) = downsample(&data, w, h);
                data = next;
                w = nw;
                h = nh;
            }
        }
    }

    texture.create_view(&wgpu::TextureViewDescriptor {
        dimension: Some(wgpu::TextureViewDimension::D2Array),
        ..Default::default()
    })
}

/// Mean linear colour of each uploaded blade layer, weighted by source opacity.
/// Native PlantMat uploads contain cropped BCR RGB and OpacityMap alpha; zero
/// alpha margins must not pull an unresolved leaf towards black (or its hidden
/// RGB). These are NOT the separate photographed tuft atlas's measured means.
/// Scan once on upload, using a 256-entry sRGB table and every source texel so
/// thin stems cannot fall between a sampling stride. W=0 means no valid colour.
pub fn blade_layer_means(width: u32, height: u32, layers: u32, rgba: &[u8]) -> [[f32; 4]; LAYERS as usize] {
    let mut out = [[0.0; 4]; LAYERS as usize];
    let Some(layer_bytes) = (width as usize).checked_mul(height as usize).and_then(|n| n.checked_mul(4)) else {
        return out;
    };
    if layer_bytes == 0 || layers != LAYERS || layer_bytes.checked_mul(layers as usize) != Some(rgba.len()) {
        return out;
    }
    let lut: [f64; 256] = std::array::from_fn(|i| {
        let c = i as f64 / 255.0;
        if c <= 0.04045 { c / 12.92 } else { ((c + 0.055) / 1.055).powf(2.4) }
    });
    for (layer, mean) in rgba.chunks_exact(layer_bytes).zip(out.iter_mut()) {
        let mut colour = [0.0f64; 3];
        let mut opacity = 0.0f64;
        for pixel in layer.chunks_exact(4) {
            let weight = pixel[3] as f64;
            if weight == 0.0 { continue; }
            for c in 0..3 { colour[c] += lut[pixel[c] as usize] * weight; }
            opacity += weight;
        }
        if opacity > 0.0 {
            *mean = [(colour[0] / opacity) as f32, (colour[1] / opacity) as f32, (colour[2] / opacity) as f32, 1.0];
        }
    }
    out
}

/// Upload authored blade-surface images as the species array used by
/// the near LOD. The geometry remains responsible for each blade's silhouette,
/// so this deliberately uses plain colour mips and never enables alpha tests.
pub fn create_from_images(
    device: &wgpu::Device,
    queue: &wgpu::Queue,
    width: u32,
    height: u32,
    layers: u32,
    rgba: &[u8],
) -> Option<wgpu::TextureView> {
    let layer_bytes = width as usize * height as usize * 4;
    if width == 0 || height == 0 || layers != LAYERS || rgba.len() != layer_bytes * layers as usize
    {
        return None;
    }
    let mip_levels = 32 - width.max(height).leading_zeros();
    let texture = device.create_texture(&wgpu::TextureDescriptor {
        label: Some("wgr_grass_blade_photo_atlas"),
        size: wgpu::Extent3d {
            width,
            height,
            depth_or_array_layers: layers,
        },
        mip_level_count: mip_levels,
        sample_count: 1,
        dimension: wgpu::TextureDimension::D2,
        // These files are modern authored PNGs, so sampling must linearise them
        // before the grass lighting calculations.
        format: wgpu::TextureFormat::Rgba8UnormSrgb,
        usage: wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::COPY_DST,
        view_formats: &[],
    });

    for layer in 0..layers {
        let start = layer as usize * layer_bytes;
        let mut data = rgba[start..start + layer_bytes].to_vec();
        let (mut w, mut h) = (width, height);
        for mip in 0..mip_levels {
            queue.write_texture(
                wgpu::TexelCopyTextureInfo {
                    texture: &texture,
                    mip_level: mip,
                    origin: wgpu::Origin3d {
                        x: 0,
                        y: 0,
                        z: layer,
                    },
                    aspect: wgpu::TextureAspect::All,
                },
                &data,
                wgpu::TexelCopyBufferLayout {
                    offset: 0,
                    bytes_per_row: Some(w * 4),
                    rows_per_image: Some(h),
                },
                wgpu::Extent3d {
                    width: w,
                    height: h,
                    depth_or_array_layers: 1,
                },
            );
            if mip + 1 < mip_levels {
                let (next, nw, nh) = downsample(&data, w, h);
                data = next;
                w = nw;
                h = nh;
            }
        }
    }

    Some(texture.create_view(&wgpu::TextureViewDescriptor {
        dimension: Some(wgpu::TextureViewDimension::D2Array),
        ..Default::default()
    }))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn blade_source_means_use_all_texels_linear_colour_and_soft_opacity() {
        let mut rgba = vec![0u8; 2 * 2 * 4 * LAYERS as usize];
        // Thin opaque red falls between the tuft mean's stride-4 samples.
        // Hidden green is deliberately bright but transparent; soft blue counts.
        rgba[4..8].copy_from_slice(&[255, 0, 0, 255]);
        rgba[8..12].copy_from_slice(&[0, 0, 255, 128]);
        rgba[12..16].copy_from_slice(&[0, 255, 0, 0]);
        rgba[16..20].copy_from_slice(&[0, 128, 0, 255]);
        rgba[32..36].copy_from_slice(&[0, 0, 0, 64]); // covered black remains valid
        let means = blade_layer_means(2, 2, LAYERS, &rgba);
        assert!((means[0][0] - 255.0 / 383.0).abs() < 1e-6);
        assert!((means[0][2] - 128.0 / 383.0).abs() < 1e-6);
        assert_eq!(means[0][1], 0.0);
        assert_eq!(means[0][3], 1.0);
        let mid_linear = ((128.0f64 / 255.0 + 0.055) / 1.055).powf(2.4) as f32;
        assert_eq!(means[1], [0.0, mid_linear, 0.0, 1.0]);
        assert_eq!(means[2], [0.0, 0.0, 0.0, 1.0]);
        assert_eq!(means[3], [0.0; 4]);
        // Changing only fully transparent RGB cannot change a layer's colour.
        rgba[0..4].copy_from_slice(&[255, 255, 255, 0]);
        assert_eq!(means, blade_layer_means(2, 2, LAYERS, &rgba));
    }

    #[test]
    fn blade_source_means_reject_empty_or_incomplete_atlases() {
        let invalid = [[0.0; 4]; LAYERS as usize];
        assert_eq!(blade_layer_means(0, 2, LAYERS, &[]), invalid);
        assert_eq!(blade_layer_means(2, 2, LAYERS, &[255; 15]), invalid);
        assert_eq!(blade_layer_means(1, 1, 1, &[255; 4]), invalid);
        assert_eq!(blade_layer_means(1, 1, LAYERS, &[0; 32]), invalid);
    }

    // The shader indexes layers by the species byte in packed.w; a mismatch
    // here would sample a nonexistent layer.
    #[test]
    fn species_table_matches_layer_count() {
        assert_eq!(SPECIES.len(), LAYERS as usize);
    }

    // grass.wgsl classifies species by these ranges when applying the dev-tool
    // weed/flower percentages, so the boundaries must stay ordered and in range.
    #[test]
    fn species_group_boundaries_are_ordered() {
        assert!(SPECIES_GRASS_END < SPECIES_WEED_END);
        assert!(SPECIES_WEED_END < LAYERS);
        // Only the flower group carries petal colour.
        for (i, sp) in SPECIES.iter().enumerate() {
            assert_eq!(sp.flower.is_some(), i as u32 >= SPECIES_WEED_END);
        }
    }

    // Full chain down to 1x1: the last mip is what a sub-pixel blade samples.
    #[test]
    fn mip_chain_reaches_one_by_one() {
        let (mut w, mut h) = (LAYER_W, LAYER_H);
        let mut data = vec![255u8; (w * h * 4) as usize];
        let levels = 32 - LAYER_H.leading_zeros();
        for _ in 1..levels {
            let (next, nw, nh) = downsample(&data, w, h);
            data = next;
            w = nw;
            h = nh;
        }
        assert_eq!((w, h), (1, 1));
        assert_eq!(data.len(), 4);
    }

    // Opaque: an alpha hole would mean discard in the fragment shader, which
    // is exactly what this design avoids.
    #[test]
    fn layers_are_fully_opaque() {
        for layer in 0..LAYERS as usize {
            assert!(generate_layer(layer).chunks(4).all(|p| p[3] == 255));
        }
    }

    // The bug that made the mid tuft render as a near-black band: a plain box
    // filter averages a cutout's transparent BLACK texels into the visible ones,
    // so the colour collapses as the chain descends. The alpha-weighted filter
    // must keep the covered texels' colour intact.
    #[test]
    fn cutout_mips_do_not_darken_toward_transparent_black() {
        // Half covered (opaque mid-grey), half transparent black — the shape of
        // the real tuft, which is ~80% clear.
        let (w, h) = (8u32, 8u32);
        let mut data = vec![0u8; (w * h * 4) as usize];
        for y in 0..h {
            for x in 0..w {
                let i = ((y * w + x) * 4) as usize;
                if x < w / 2 {
                    data[i] = 200;
                    data[i + 1] = 200;
                    data[i + 2] = 200;
                    data[i + 3] = 255;
                } // else stays 0,0,0,0
            }
        }
        let (mip, _, _) = downsample_cutout(&data, w, h);
        // Every texel with any coverage must retain the source colour, not a
        // blend toward black. A plain box filter would give 100 on the seam.
        for p in mip.chunks(4).filter(|p| p[3] > 0) {
            assert!(
                p[0] >= 199,
                "cutout mip darkened to {} (transparent black bled in)",
                p[0]
            );
        }
    }

    // Averaging pushes a cutout's alpha under the test threshold, thinning the
    // tuft with distance; the rescale must hold coverage roughly steady.
    #[test]
    fn coverage_is_preserved_across_a_mip() {
        let (w, h) = (16u32, 16u32);
        let mut data = vec![0u8; (w * h * 4) as usize];
        for (n, p) in data.chunks_mut(4).enumerate() {
            // ~40% coverage in a scattered pattern, like blade tips.
            p[3] = if n % 5 < 2 { 255 } else { 0 };
        }
        let base = coverage(&data, 128);
        let (mut mip, _, _) = downsample_cutout(&data, w, h);
        preserve_coverage(&mut mip, base, 128);
        let after = coverage(&mip, 128);
        assert!(
            (after - base).abs() < 0.15,
            "coverage drifted {base} -> {after}"
        );
    }

    // The per-slice flicker predictor: a hard-authored cutout reads ~0 partial
    // and 0 speckle; a JPEG-style noisy opacity map with one-texel stem
    // fragments reads high on both. This is what the "which variety flickers"
    // log line ranks by, so the two must separate cleanly.
    #[test]
    fn alpha_edge_report_separates_hard_cutouts_from_speckled_opacity_maps() {
        let (w, h) = (16u32, 16u32);
        // Layer 0: a solid 8x8 opaque block, hard alpha. Layer 1: the same block
        // with soft (partial) alpha throughout, plus isolated one-texel speckles
        // in the transparent margin.
        let mut rgba = vec![0u8; (w * h * 4 * 2) as usize];
        let layer_bytes = (w * h * 4) as usize;
        for y in 0..h {
            for x in 0..w {
                let i = ((y * w + x) * 4) as usize;
                let inside = (4..12).contains(&x) && (4..12).contains(&y);
                if inside {
                    rgba[i + 3] = 255;
                    rgba[layer_bytes + i + 3] = 160; // partial but above the cutoff
                }
            }
        }
        for (x, y) in [(1u32, 1u32), (14, 1), (1, 14), (14, 14)] {
            rgba[layer_bytes + ((y * w + x) * 4) as usize + 3] = 200; // speckles
        }
        let report = tuft_alpha_edge_report(w, h, 2, &rgba);
        assert_eq!(report.len(), 2);
        let hard = report[0];
        let soft = report[1];
        assert_eq!(hard.covered, 64);
        assert_eq!(hard.partial_fraction, 0.0);
        assert_eq!(hard.speckle_fraction, 0.0);
        // 8x8 block: the 28 perimeter texels are edges.
        assert!(
            (hard.edge_fraction - 28.0 / 64.0).abs() < 1e-6,
            "{}",
            hard.edge_fraction
        );
        assert_eq!(soft.covered, 68);
        assert!(soft.partial_fraction > 0.99, "{}", soft.partial_fraction);
        assert!(
            (soft.speckle_fraction - 4.0 / 68.0).abs() < 1e-6,
            "{}",
            soft.speckle_fraction
        );
        // Fully transparent input: no coverage, no division by zero.
        let empty = tuft_alpha_edge_report(w, h, 1, &vec![0u8; layer_bytes]);
        assert_eq!(empty[0], TuftAlphaEdgeReport::default());
    }

    // A flower layer must actually be brighter near the tip than a grass layer,
    // otherwise the petal head silently failed to composite.
    #[test]
    fn flower_head_is_brighter_than_its_stem() {
        let flower = generate_layer(LAYERS as usize - 1);
        let luma = |data: &[u8], row: u32| -> u32 {
            (0..LAYER_W)
                .map(|x| data[((row * LAYER_W + x) * 4) as usize] as u32)
                .sum()
        };
        // Row 8 sits inside the head; row 200 is well down the stem.
        assert!(luma(&flower, 8) > luma(&flower, 200));
    }
}
