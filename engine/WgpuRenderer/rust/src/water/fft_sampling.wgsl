// Shared world-to-FFT lookup.  A finite FFT texture is necessarily periodic, but
// sampling it through a low-gradient, world-space value-noise warp prevents a tile
// edge or the common cascade period from reading as a repeating ocean pattern.  The
// integer hash repeats only after tens of thousands of kilometres at the chosen
// frequencies, far beyond the playable world.
#define_import_path water_fft_sampling

// Artistic uniform dilation of a fixed spectrum: D_S(x,t)=S*D(x/S,t).
// Displacement scales by S, slopes/Jacobian do not, curvature scales by 1/S.
// Period is unchanged, so crest speed scales by S; this is not a new physical
// sea state. Every field associated with that spectrum must use this coordinate.
fn fft_scaled_uv(world_xz: vec2<f32>, tile_length: f32, layer: i32, warp: f32, wave_scale: f32) -> vec2<f32> {
    return fft_aperiodic_uv(world_xz / max(wave_scale, 0.01), tile_length, layer, warp);
}

fn fft_hash(cell: vec2<i32>) -> f32 {
    let c = vec2<u32>(cell) & vec2<u32>(0xffffu);
    var n = c.x * 1597334677u + c.y * 3812015801u;
    n = (n ^ (n >> 15u)) * 2246822519u;
    n = n ^ (n >> 13u);
    return f32(n & 0x00ffffffu) / 16777216.0;
}

fn fft_value_noise(p: vec2<f32>) -> f32 {
    let cell = vec2<i32>(floor(p));
    let f = fract(p);
    let s = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    let a = fft_hash(cell);
    let b = fft_hash(cell + vec2<i32>(1, 0));
    let c = fft_hash(cell + vec2<i32>(0, 1));
    let d = fft_hash(cell + vec2<i32>(1, 1));
    return mix(mix(a, b, s.x), mix(c, d, s.x), s.y);
}

// The rotation and phase are unique to a cascade.  The warp itself is not tied to
// a cascade tile, so a crest does not return to the same world position when its
// source texture wraps. `amplitude` is the existing Water-tab de-tile control.
fn fft_aperiodic_uv(world_xz: vec2<f32>, tile_length: f32, layer: i32, amplitude: f32) -> vec2<f32> {
    // The reference project samples each cascade directly in world space.  Preserve
    // that path exactly when de-tiling is disabled: the former unconditional
    // per-layer rotation made crossing crests appear to spin even at warp = 0.
    if (amplitude <= 0.0) {
        return fract(world_xz / max(tile_length, 1.0));
    }
    let l = f32(layer);
    let broad_p = world_xz * 0.00173 + vec2<f32>(l * 13.7, l * -19.1);
    let fine_p = world_xz * 0.00891 + vec2<f32>(l * -7.3, l * 11.9);
    let broad = vec2<f32>(fft_value_noise(broad_p), fft_value_noise(broad_p + vec2<f32>(41.3, 17.9))) * 2.0 - vec2<f32>(1.0);
    let fine = vec2<f32>(fft_value_noise(fine_p), fft_value_noise(fine_p + vec2<f32>(23.1, 37.7))) * 2.0 - vec2<f32>(1.0);
    let warped = world_xz + (broad * 0.72 + fine * 0.28) * max(amplitude, 0.0) * (1.0 + l * 0.17);

    let angle = 0.173 * l + 0.071;
    let c = cos(angle);
    let s = sin(angle);
    let rotated = vec2<f32>(c * warped.x - s * warped.y, s * warped.x + c * warped.y);
    let phase = vec2<f32>(0.173 * l, 0.347 * l);
    return fract(rotated / max(tile_length, 1.0) + phase);
}
