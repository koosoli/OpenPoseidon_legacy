use super::tests::headless;
use std::borrow::Cow;

const SHADER: &str = include_str!("grass.wgsl");

// Independent midpoint integration of the original point shading. GPU probes
// execute the production WGSL helpers, rather than a Rust copy of their formula.
fn integrate(taper: f64, root: f64, gust: f64, patch: f64, variation: f64) -> [[f64; 4]; 3] {
    let base = [0.055, 0.16, 0.025];
    let tip = [0.32, 0.43, 0.10];
    let straw = [0.74, 0.66, 0.32];
    let mut out = [[0.0; 4]; 3];
    let mut weight_sum = 0.0;
    for i in 0..65536 {
        let t = (i as f64 + 0.5) / 65536.0;
        let weight = (1.0 - t).powf(taper);
        let colour: [f64; 3] = std::array::from_fn(|c| {
            (base[c] + (tip[c] - base[c]) * t)
                * (1.0 - root + root * t * t)
                * (1.0 + gust * (0.035 + 0.075 * t))
                * variation
        });
        let luma = colour[0] * 0.2126 + colour[1] * 0.7152 + colour[2] * 0.0722;
        let dry = patch * (0.45 + 0.55 * t);
        for c in 0..3 {
            out[0][c] += (colour[c] * (1.0 - dry) + straw[c] * (0.55 + 1.35 * luma) * dry) * weight;
            out[1][c] += colour[c] * weight;
            out[2][c] += colour[c] * t * weight;
        }
        let ao_t = (t / 0.85).clamp(0.0, 1.0);
        out[0][3] += ao_t * ao_t * (3.0 - 2.0 * ao_t) * weight;
        out[1][3] += t * weight;
        weight_sum += weight;
    }
    for row in &mut out {
        for c in row {
            *c /= weight_sum;
        }
    }
    out
}

#[test]
fn production_footprint_moments_preserve_taper_weighted_colour_dry_tips_and_ao() {
    let Some((device, queue)) = headless() else {
        eprintln!("No headless grass adapter; footprint numeric GPU probe unavailable");
        return;
    };
    let start = SHADER.find("struct BladeRampMean {").unwrap();
    let end = SHADER.find("// End pure footprint moments.").unwrap();
    let mut source = SHADER[start..end].to_owned();
    source
        .push_str("\n@group(0) @binding(0) var<storage, read_write> results: array<vec4<f32>>;\n");
    source.push_str("@compute @workgroup_size(1) fn probe() {\n");
    let mut references = Vec::new();
    for taper in [0.05, 0.26, 0.65, 1.3] {
        for (root, gust, patch, variation) in [
            (0.0, 0.0, 0.0, 1.0),
            (0.7, 0.0, 0.0, 1.0),
            (0.7, 1.5, 0.75, 1.14),
            (0.95, 2.25, 1.0, 0.78),
        ] {
            let case = references.len();
            source.push_str(&format!(
                "{{ var mean = blade_ramp_mean(vec3<f32>(0.055, 0.16, 0.025), vec3<f32>(0.32, 0.43, 0.10), {root:.6}, {gust:.6}, {taper:.6});\n\
                 mean.colour *= {variation:.6}; mean.height_colour *= {variation:.6};\n\
                 results[{}] = vec4<f32>(blade_dry_mean(mean, {patch:.6}), blade_height_ao_mean({taper:.6}));\n\
                 results[{}] = vec4<f32>(mean.colour, mean.height);\n\
                 results[{}] = vec4<f32>(mean.height_colour, 0.0); }}\n",
                case * 3, case * 3 + 1, case * 3 + 2
            ));
            references.push(integrate(taper, root, gust, patch, variation));
        }
    }
    source.push_str("}\n");
    let actual = gpu_probe(&device, &queue, source, references.len() * 3);
    for (case, reference) in references.iter().enumerate() {
        for row in 0..3 {
            for channel in 0..4 {
                let observed = actual[case * 3 + row][channel] as f64;
                assert!(
                    (observed - reference[row][channel]).abs() < 0.00005,
                    "case={case} row={row} channel={channel}: GPU={observed}, integral={}",
                    reference[row][channel]
                );
            }
        }
    }
}

fn gpu_probe(
    device: &wgpu::Device,
    queue: &wgpu::Queue,
    source: String,
    rows: usize,
) -> Vec<[f32; 4]> {
    let shader = device.create_shader_module(wgpu::ShaderModuleDescriptor {
        label: Some("grass production footprint moment probe"),
        source: wgpu::ShaderSource::Wgsl(Cow::Owned(source)),
    });
    let pipeline = device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
        label: Some("grass footprint moment probe"),
        layout: None,
        module: &shader,
        entry_point: Some("probe"),
        compilation_options: Default::default(),
        cache: None,
    });
    let size = (rows * 16) as u64;
    let output = device.create_buffer(&wgpu::BufferDescriptor {
        label: Some("grass footprint output"),
        size,
        usage: wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_SRC,
        mapped_at_creation: false,
    });
    let readback = device.create_buffer(&wgpu::BufferDescriptor {
        label: Some("grass footprint readback"),
        size,
        usage: wgpu::BufferUsages::MAP_READ | wgpu::BufferUsages::COPY_DST,
        mapped_at_creation: false,
    });
    let bind = device.create_bind_group(&wgpu::BindGroupDescriptor {
        label: None,
        layout: &pipeline.get_bind_group_layout(0),
        entries: &[wgpu::BindGroupEntry {
            binding: 0,
            resource: output.as_entire_binding(),
        }],
    });
    let mut encoder = device.create_command_encoder(&Default::default());
    {
        let mut pass = encoder.begin_compute_pass(&Default::default());
        pass.set_pipeline(&pipeline);
        pass.set_bind_group(0, &bind, &[]);
        pass.dispatch_workgroups(1, 1, 1);
    }
    encoder.copy_buffer_to_buffer(&output, 0, &readback, 0, size);
    queue.submit([encoder.finish()]);
    let (tx, rx) = std::sync::mpsc::channel();
    readback.slice(..).map_async(wgpu::MapMode::Read, move |r| {
        tx.send(r).unwrap();
    });
    device.poll(wgpu::PollType::wait_indefinitely()).unwrap();
    rx.recv().unwrap().unwrap();
    let bytes = readback.slice(..).get_mapped_range();
    let actual = bytemuck::cast_slice::<u8, [f32; 4]>(&bytes).to_vec();
    drop(bytes);
    readback.unmap();
    actual
}

#[test]
fn native_filter_preserves_close_colour_and_empty_layers_and_converges_to_the_source_mean() {
    let Some((device, queue)) = headless() else {
        eprintln!("No headless grass adapter; native filtering GPU probe unavailable");
        return;
    };
    // Real CPU upload measurement: two soft/opaque coloured texels, with bright
    // hidden RGB in a transparent margin. Do not substitute the tuft_tex means.
    let mut rgba = vec![0u8; 3 * 4 * super::blade_atlas::LAYERS as usize];
    rgba[0..4].copy_from_slice(&[128, 64, 32, 255]);
    rgba[4..8].copy_from_slice(&[192, 128, 64, 128]);
    rgba[8..12].copy_from_slice(&[255, 255, 255, 0]);
    let mean = super::blade_atlas::blade_layer_means(3, 1, 8, &rgba)[0];
    let sample = [0.9f32, 0.6, 0.3];
    let start = SHADER.find("fn native_blade_colour(").unwrap();
    let end = start + SHADER[start..].find('}').unwrap() + 1;
    let mut source = SHADER[start..end].to_owned();
    source.push_str("\n@group(0) @binding(0) var<storage, read_write> results: array<vec4<f32>>;\n@compute @workgroup_size(1) fn probe() {\n");
    let mut expected = Vec::new();
    for valid in [true, false] {
        for detail in [0.0f32, 0.25, 1.0] {
            source.push_str(&format!("results[{}] = vec4<f32>(native_blade_colour(vec3<f32>(0.9, 0.6, 0.3), vec4<f32>({:.9}, {:.9}, {:.9}, {}.0), {detail:.9}), 1.0);\n",
                expected.len(), mean[0], mean[1], mean[2], u8::from(valid)));
            expected.push(std::array::from_fn::<_, 3, _>(|c| {
                if valid {
                    mean[c] * (1.0 - detail) + sample[c] * detail
                } else {
                    sample[c]
                }
            }));
        }
    }
    source.push_str("}\n");
    let actual = gpu_probe(&device, &queue, source, expected.len());
    for (observed, want) in actual.iter().zip(expected) {
        for c in 0..3 {
            assert!((observed[c] - want[c]).abs() < 1e-6);
        }
    }
    // The shader applies the same ground-luminance recolouring to this filtered
    // RGB as to the resolved atlas; alpha is still the original sampled alpha.
    assert!(SHADER.contains("dot(blade_colour, luma)"));
    assert!(SHADER.contains("mix(blade_colour, recoloured, clamp(grass.native2.x"));
    assert!(SHADER.contains("grass.blade_layer_means[min(u32(in.blade_uv.z), 7u)]"));
}

#[test]
fn unresolved_ramps_keep_the_native_and_resolved_paths_and_actual_taper() {
    assert!(SHADER.contains("if (ramp_detail < 1.0 && grass.native.x <= 0.0)"));
    assert!(SHADER.contains(
        "dry_colour = mix(blade_dry_mean(mean, patch_mask), dry_colour, ramp_detail);"
    ));
    assert!(SHADER.contains("@location(6) @interpolate(flat) blade_taper: f32"));
    assert!(SHADER.contains("out.blade_taper = max(shape.z * taper_jitter, 0.05);"));
    assert!(SHADER.contains("out.blade_taper = shape.z;"));
    assert!(SHADER.contains("albedo = textured;"));
}

#[test]
fn medium_lighting_resolves_height_but_keeps_aerial_means_and_bounds_broad_sheen() {
    let Some((device, queue)) = headless() else {
        eprintln!("No headless grass adapter; medium lighting GPU probe unavailable");
        return;
    };
    let start = SHADER.find("fn blade_height_resolution(").unwrap();
    let end = SHADER.find("// End pure medium grass lighting.").unwrap();
    let mut source = SHADER[start..end].to_owned();
    source.push_str("\n@group(0) @binding(0) var<storage, read_write> results: array<vec4<f32>>;\n@compute @workgroup_size(1) fn probe() {\n");
    let mut cases = Vec::new();
    for pixels in [0.25_f32, 1.0, 3.0, 6.0, 12.0, 40.0] {
        for vertical in [0.0_f32, 0.70, 0.80, 0.90, 0.9976] {
            for enabled in [false, true] {
                for alignment in [-1.0_f32, 0.0, 0.5, 0.70710677, 1.0] {
                    source.push_str(&format!("{{ let height = blade_height_resolution({:.9}); results[{}] = vec4<f32>(height, grass_ramp_detail(0.0, height, {vertical:.9}, {enabled}), grass_broad_sun_response({alignment:.9}, height, {vertical:.9}, {enabled}), grass_ramp_detail(1.0, height, {vertical:.9}, {enabled})); }}\n", 1.0 / pixels, cases.len()));
                    cases.push((pixels, vertical, enabled, alignment));
                }
            }
        }
    }
    source.push_str("}\n");
    let actual = gpu_probe(&device, &queue, source, cases.len());
    let smooth = |lo: f32, hi: f32, x: f32| {
        let t = ((x - lo) / (hi - lo)).clamp(0.0, 1.0);
        t * t * (3.0 - 2.0 * t)
    };
    for (row, (pixels, vertical, enabled, alignment)) in actual.iter().zip(cases) {
        let height = smooth(3.0, 12.0, pixels);
        let view = 1.0 - smooth(0.70, 0.90, vertical);
        assert!((row[0] - height).abs() < 0.000002);
        assert!((row[1] - if enabled { height * view } else { 0.0 }).abs() < 0.000002);
        let sheen = if enabled { 0.035 * alignment.clamp(0.0, 1.0).powi(4) * height * view } else { 0.0 };
        assert!((row[2] - sheen).abs() < 0.000002);
        assert!(row[2].is_finite() && row[2] >= 0.0 && row[2] <= 0.035001);
        assert_eq!(row[3], 1.0, "resolved width retains the original point ramp");
        if pixels <= 3.0 || vertical >= 0.90 || !enabled {
            assert_eq!(row[1], 0.0);
            assert_eq!(row[2], 0.0);
        }
        if pixels >= 12.0 && vertical <= 0.70 && enabled {
            assert_eq!(row[1], 1.0);
            if (alignment - 0.70710677).abs() < 0.000001 {
                assert!((row[2] - 0.00875).abs() < 0.000002, "45 degree half-vector still has 25% of the broad peak");
            }
        }
    }
}

#[test]
fn medium_sun_response_preserves_native_atlas_filter_and_all_coverage_consumers() {
    assert!(SHADER.contains("grass.blade_layer_means[min(u32(in.blade_uv.z), 7u)], shading_detail"));
    assert!(SHADER.contains("let n = normalize(mix(upright, resolved_normal, shading_detail));"));
    assert!(SHADER.contains("in.blade_taper, ramp_detail"));
    assert!(SHADER.contains("grass_medium_view_weight(view_vertical)"));
    assert!(SHADER.contains("(1.0 - max(terrain_shadow, self_shadow.x)) * cloud_lit"));
    assert!(SHADER.contains("(1.0 - max(terrain_shadow, clump_shadow)) * cloud_lit"));
    assert!(SHADER.contains("let cov = tuft_coverage(tex.a);"));
    assert!(SHADER.contains("let cov = tuft_coverage(tuft_sample(in).a);"));
    assert!(SHADER.contains("if (tuft_sample(in).a < tuft_alpha_cutoff()) { discard; }"));
    let helpers = &SHADER[SHADER.find("// Begin pure medium grass lighting.").unwrap()..SHADER.find("// End pure medium grass lighting.").unwrap()];
    let code = helpers.lines().filter(|line| !line.trim_start().starts_with("//")).collect::<Vec<_>>().join("\n");
    assert!(!code.contains("seed"));
    assert!(!code.contains("textureSample"));
    assert!(!code.contains("wind"));
}
