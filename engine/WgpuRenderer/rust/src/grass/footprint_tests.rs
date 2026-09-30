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
    let size = (references.len() * 3 * 16) as u64;
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
    let actual: &[[f32; 4]] = bytemuck::cast_slice(&bytes);
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
    drop(bytes);
    readback.unmap();
}

#[test]
fn unresolved_ramps_keep_the_native_and_resolved_paths_and_actual_taper() {
    assert!(SHADER.contains("if (shading_detail < 1.0 && grass.native.x <= 0.0)"));
    assert!(SHADER.contains(
        "dry_colour = mix(blade_dry_mean(mean, patch_mask), dry_colour, shading_detail);"
    ));
    assert!(SHADER.contains("@location(6) @interpolate(flat) blade_taper: f32"));
    assert!(SHADER.contains("out.blade_taper = max(shape.z * taper_jitter, 0.05);"));
    assert!(SHADER.contains("out.blade_taper = shape.z;"));
    assert!(SHADER.contains("albedo = textured;"));
}
