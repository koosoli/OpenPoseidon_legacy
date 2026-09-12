use naga_oil::compose::{NagaModuleDescriptor, ShaderDefValue};

#[test]
fn coalesced_filter_composes_in_every_receiver() {
    for (source, path) in [
        (include_str!("../gfx3d/shader3d.wgsl"), "gfx3d/shader3d.wgsl"),
        (include_str!("../gfx3d/gpu_driven.wgsl"), "gfx3d/gpu_driven.wgsl"),
        (include_str!("../terrain/terrain.wgsl"), "terrain/terrain.wgsl"),
        (include_str!("../grass/grass.wgsl"), "grass/grass.wgsl"),
    ] {
        let mut composer = super::build_composer();
        let result = composer.make_naga_module(NagaModuleDescriptor {
            source,
            file_path: path,
            shader_defs: [
                ("COALESCED_PCF".to_owned(), ShaderDefValue::Bool(true)),
                ("NATIVE_LEAF_CAVITY".to_owned(), ShaderDefValue::Bool(true)),
            ].into(),
            ..Default::default()
        });
        if let Err(error) = result {
            panic!("{path}: {}", error.emit_to_string(&composer));
        }
    }
}

// Independent scalar comparison against the old nine bilinear taps. Includes
// texture-edge clamping, fractional UVs and sloped receivers. Not a GPU timing.
#[test]
fn sixteen_fetches_match_thirty_six_in_the_unclamped_gradient_domain() {
    const SIZE: f64 = 16.0;
    let tap = |x: i32, y: i32, uv: [f64; 2], slope: [f64; 2], reference: f64| {
        let x = x.clamp(0, 15);
        let y = y.clamp(0, 15);
        let depth = ((x * 37 + y * 19 + x * y * 7) % 101) as f64 / 100.0;
        let adjusted = reference + ((x as f64 + 0.5) / SIZE - uv[0]) * slope[0]
            + ((y as f64 + 0.5) / SIZE - uv[1]) * slope[1];
        if adjusted <= depth { 1.0 } else { 0.0 }
    };
    for u in [0.0001, 0.03, 0.17, 0.49, 0.5, 0.87, 0.9999] {
        for v in [0.0001, 0.05, 0.23, 0.5, 0.78, 0.9999] {
            for slope in [[0.0, 0.0], [0.031, -0.022], [-0.028, 0.017]] {
                for reference in [0.101, 0.313, 0.517, 0.799] {
                    let uv = [u, v];
                    let mut old = 0.0;
                    for dy in -1_i32..=1 {
                        for dx in -1_i32..=1 {
                            let shifted = [u + dx as f64 / SIZE, v + dy as f64 / SIZE];
                            let pixel = [shifted[0] * SIZE - 0.5, shifted[1] * SIZE - 0.5];
                            let base = [pixel[0].floor() as i32, pixel[1].floor() as i32];
                            let f = [pixel[0] - pixel[0].floor(), pixel[1] - pixel[1].floor()];
                            let r = reference + dx as f64 / SIZE * slope[0] + dy as f64 / SIZE * slope[1];
                            for y in 0..2 {
                                for x in 0..2 {
                                    let wx = if x == 0 { 1.0 - f[0] } else { f[0] };
                                    let wy = if y == 0 { 1.0 - f[1] } else { f[1] };
                                    old += (2 - dx.abs()) as f64 * (2 - dy.abs()) as f64 * wx * wy
                                        * tap(base[0] + x, base[1] + y, shifted, slope, r);
                                }
                            }
                        }
                    }
                    let p = [u * SIZE - 0.5, v * SIZE - 0.5];
                    let b = [p[0].floor() as i32, p[1].floor() as i32];
                    let f = [p[0] - p[0].floor(), p[1] - p[1].floor()];
                    let w = |f: f64| [1.0 - f, 2.0 - f, 1.0 + f, f];
                    let mut new = 0.0;
                    for y in 0..4 {
                        for x in 0..4 {
                            new += w(f[0])[x] * w(f[1])[y]
                                * tap(b[0] + x as i32 - 1, b[1] + y as i32 - 1, uv, slope, reference);
                        }
                    }
                    assert!((old - new).abs() < 1e-10, "uv={uv:?} slope={slope:?} old={old} new={new}");
                }
            }
        }
    }
}
