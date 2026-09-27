// Tidewater Native — the caustic light factor (W5b): Caustics.js's lookup module `_causticsSample`
// (dgreenheck/tidewater @ 4811ba48, MIT, © DRG Software Solutions LLC). Shared by the water
// material (the sea bed seen from above) and the underwater composite; the textures and the
// sampler are passed in (they are bound differently in each).
//
// OP: flat surface above the point (no long-wave slope / foam inputs), a blur level instead of
// gradients (the callers pass one that covers the pixel footprint), no sea-detail gust factor.

const TW_CAUSTICS_STRENGTH: f32 = 0.75;
const TW_CAUSTICS_FINE_RES: f32 = 512.0;
const TW_CAUSTICS_BROAD_RES: f32 = 256.0;

// Caustic light factor (mean ~1) at world point P, `depth` metres below the surface. L: toward the
// sun. tiles: (fine tile, broad tile) in metres. minLevel: the least blur (pixel footprint).
// mono: one fine lookup (no chromatic dispersion).
fn twCausticsSample(fineTex: texture_2d<f32>, broadTex: texture_2d<f32>, smp: sampler, tiles: vec2<f32>,
                    P: vec3<f32>, depth: f32, L: vec3<f32>, minLevel: f32, mono: bool) -> vec3<f32> {
    if (tiles.x <= 0.0 || tiles.y <= 0.0) { return vec3<f32>(1.0); }
    // the light reaching this point entered the water up-sun along the refracted sun ray
    let Ls = refract(-L, vec3<f32>(0.0, 1.0, 0.0), 1.0 / 1.333);
    let tDown = max(-Ls.y, 0.15);
    let entry = P.xz - Ls.xz * (depth / tDown);
    // deeper -> softer (finite sun disk + forward scattering)
    let blur = max(clamp(depth * 0.4 - 0.2, 0.0, 3.0), minLevel);
    let wD = clamp((depth - 1.2) / 2.8, 0.0, 1.0); // between the two focal planes
    let uvF = entry / tiles.x;
    let tg = textureSampleLevel(fineTex, smp, uvF, blur);
    let g = mix(tg.x, tg.y, wD);
    var r = g;
    var b = g;
    if (!mono) {
        // slight chromatic dispersion: each colour lands a little apart along the sun direction
        let disp = normalize(Ls.xz + vec2<f32>(1e-4, 0.0)) * (depth * 0.0035);
        let tr = textureSampleLevel(fineTex, smp, uvF + disp / tiles.x, blur);
        let tb = textureSampleLevel(fineTex, smp, uvF - disp / tiles.x, blur);
        r = mix(tr.x, tr.y, wD);
        b = mix(tb.x, tb.y, wD);
    }
    let broad = textureSampleLevel(broadTex, smp, entry / tiles.y, max(1.5, minLevel - 1.0));
    let br = mix(broad.x, broad.y, clamp(depth / 9.0, 0.0, 1.0));
    let c = vec3<f32>(r, g, b) * mix(1.0, br, 0.6);
    // no caustics right at the surface, strongest in the first metres, fading with depth
    let k = smoothstep(0.03, 0.5, depth) * exp(depth * -0.06) * TW_CAUSTICS_STRENGTH;
    return mix(vec3<f32>(1.0), c, k);
}

// the fine-texture blur level whose texel covers `footprint` metres
fn twCausticsLevel(footprint: f32, tile: f32) -> f32 {
    return max(log2(max(footprint * TW_CAUSTICS_FINE_RES / max(tile, 1e-3), 1.0)), 0.0);
}
