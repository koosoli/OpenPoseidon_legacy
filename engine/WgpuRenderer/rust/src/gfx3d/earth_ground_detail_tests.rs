use naga_oil::compose::NagaModuleDescriptor;

const SOURCE: &str = include_str!("shader3d.wgsl");

#[test]
fn earth_ground_detail_production_shader_composes_cpu_only() {
    let mut composer = crate::shaders::build_composer();
    composer
        .make_naga_module(NagaModuleDescriptor {
            source: SOURCE,
            file_path: "gfx3d/shader3d.wgsl",
            shader_defs: crate::shaders::shader_defs(),
            ..Default::default()
        })
        .unwrap_or_else(|e| panic!("{}", e.emit_to_string(&composer)));
    let start = SOURCE.find("// BEGIN_EARTH_GROUND_DETAIL_HELPERS").unwrap();
    let end = SOURCE.find("// END_EARTH_GROUND_DETAIL_HELPERS").unwrap();
    let helpers = &SOURCE[start..end];
    // Validate the actual independent helper as well as its real scene callsite.
    let module = naga::front::wgsl::parse_str(helpers).unwrap();
    naga::valid::Validator::new(
        naga::valid::ValidationFlags::all(),
        naga::valid::Capabilities::empty(),
    )
    .validate(&module)
    .unwrap();
    assert!(!helpers.contains("texture") && !helpers.contains("frame.cam_pos"));
    assert!(SOURCE.contains("let ground_detail = material.light_ambient.w > 0.5;"));
    assert!(SOURCE.contains("ground_detail && distance < 400.0"));
}

fn hash(c: [i32; 3]) -> f32 {
    let mut h = (c[0] as u32).wrapping_mul(1597334677)
        ^ (c[1] as u32).wrapping_mul(3812015801)
        ^ (c[2] as u32).wrapping_mul(2798796415);
    h = (h ^ (h >> 16)).wrapping_mul(2246822519);
    h = (h ^ (h >> 13)).wrapping_mul(3266489917);
    ((h ^ (h >> 16)) & 65535) as f32 / 65535.0
}
// Numerical mirror of the shader's bounded interpolation/analytic gradient.
fn noise(high: [f32; 3], low: [f32; 3], frequency: f32) -> [f32; 4] {
    let p = low.map(|v| v * frequency);
    let cell =
        std::array::from_fn::<_, 3, _>(|i| (high[i] * frequency) as i32 + p[i].floor() as i32);
    let f = p.map(|v| v - v.floor());
    let s = f.map(|v| v * v * (3.0 - 2.0 * v));
    let ds = f.map(|v| 6.0 * v * (1.0 - v));
    let mut out = [0.0; 4];
    for z in 0..2 {
        for y in 0..2 {
            for x in 0..2 {
                let bits = [x, y, z];
                let corner = hash(std::array::from_fn(|i| cell[i] + bits[i]));
                let w =
                    std::array::from_fn::<_, 3, _>(
                        |i| if bits[i] == 1 { s[i] } else { 1.0 - s[i] },
                    );
                let dw =
                    std::array::from_fn::<_, 3, _>(|i| if bits[i] == 1 { ds[i] } else { -ds[i] });
                out[0] += corner * w[0] * w[1] * w[2];
                for i in 0..3 {
                    out[i + 1] += corner * dw[i] * w[(i + 1) % 3] * w[(i + 2) % 3] * frequency;
                }
            }
        }
    }
    out
}

#[test]
fn earth_ground_detail_noise_is_bounded_continuous_and_origin_split_invariant() {
    for frequency in [0.125, 1.0, 8.0] {
        let high = [4000256.0, -2000384.0, 1000448.0];
        for k in 0..40 {
            let p = [k as f32 * 0.123 - 3.2, k as f32 * 0.371 + 0.17, 0.339];
            let n = noise(high, p, frequency);
            assert!((0.0..=1.000001).contains(&n[0]));
            assert!(n.iter().all(|v| v.is_finite()));
            for axis in 0..3 {
                let mut a = p;
                let mut b = p;
                a[axis] -= 0.0005;
                b[axis] += 0.0005;
                let derivative =
                    (noise(high, b, frequency)[0] - noise(high, a, frequency)[0]) / 0.001;
                assert!(
                    (derivative - n[axis + 1]).abs() < 0.02,
                    "f={frequency} axis={axis}"
                );
            }
        }
        let p = [16.25, 32.5, 64.75];
        let moved_high = [high[0] + 512.0, high[1] - 512.0, high[2] + 512.0];
        let moved_low = [p[0] - 512.0, p[1] + 512.0, p[2] - 512.0];
        assert_eq!(
            noise(high, p, frequency),
            noise(moved_high, moved_low, frequency)
        );
    }
}

#[test]
fn earth_ground_detail_has_exact_distant_and_minified_neutral_endpoints() {
    fn smooth(a: f32, b: f32, x: f32) -> f32 {
        let t = ((x - a) / (b - a)).clamp(0.0, 1.0);
        t * t * (3.0 - 2.0 * t)
    }
    for frequency in [0.125, 1.0, 8.0] {
        let band = |footprint: f32, distance: f32| {
            (1.0 - smooth(0.12, 0.5, footprint * frequency)) * (1.0 - smooth(25.0, 180.0, distance))
        };
        assert_eq!(band(0.0, 15.0), 1.0);
        assert_eq!(band(0.0, 180.0), 0.0);
        assert_eq!(band(0.5 / frequency, 1.0), 0.0);
    }
    // Reflectance coefficients sum to .25, preserving a finite [.75,1.25]
    // multiplier without inventing radiance when sun/ambient are zero.
    assert!((0.12f32 + 0.09 + 0.04 - 0.25).abs() < 1e-6);
}

#[test]
fn earth_ground_detail_near_chart_gradient_is_finite_monotone_and_edge_exact() {
    fn chart(u: f32) -> (f32, f32) {
        let q = 2.0 * u - 1.0;
        let d = 0.25 + 0.75 * q.abs();
        (0.5 + 0.5 * q / d, 0.25 / (d * d))
    }
    assert_eq!(chart(0.0), (0.0, 0.25));
    assert_eq!(chart(0.5), (0.5, 4.0));
    assert_eq!(chart(1.0), (1.0, 0.25));
    for u in [
        -0.001, 0.0, 0.0001, 0.2, 0.499, 0.5, 0.501, 0.8, 0.9999, 1.0, 1.001,
    ] {
        let (value, derivative) = chart(u);
        assert!(value.is_finite() && derivative.is_finite() && derivative > 0.0);
        assert_eq!(value < 0.5, u < 0.5);
        assert_eq!(value > 0.5, u > 0.5);
        let numeric = (chart(u + 0.0001).0 - chart(u - 0.0001).0) / 0.0002;
        assert!((numeric - derivative).abs() < 0.005, "u={u}");
    }
    assert!(SOURCE.contains("sample_dx *= chart.zw;"));
    assert!(SOURCE.contains("sample_dy *= chart.zw;"));
    assert!(SOURCE.contains("if (material.specular.w > 0.5)"));
}
