// The whitecap cover the sea actually SHOWS, measured on a real device: the FFT is run for a minute
// of sea time per weather preset, the persistent foam field is read back, and the water shader's
// whitecap path is replayed on the CPU (the four cascades' foam, weighted and box-filtered like its
// mip-1.5 sample, then the WHITECAP_PATCH ramp). Bias and decay alone say nothing about cover: on
// 2026-10-13 a storm with the "capped" bias of a rough sea still turned most of the ocean white,
// because its steeper waves foam far more crests at the same threshold and the trail of each lasts
// ~9 s above the patch ramp.

use super::*;

struct Preset {
    name: &'static str,
    wind: f32,
    fetch: f32,
    chop: f32,
    swell: f32,
    whitecaps: f32,
}

// WaterWgpu.cpp kTwSea: the presets the weather wind interpolates between, two seas halfway between
// them (as Weather mode blends them), and a gale past the table (an env or scripted 30 m/s wind)
const PRESETS: [Preset; 7] = [
    Preset { name: "calm", wind: 3.5, fetch: 40.0, chop: 0.75, swell: 0.28, whitecaps: 0.2 },
    Preset { name: "breezy", wind: 7.0, fetch: 120.0, chop: 0.9, swell: 0.48, whitecaps: 0.5 },
    Preset { name: "fresh", wind: 9.5, fetch: 210.0, chop: 0.975, swell: 0.58, whitecaps: 0.625 },
    Preset { name: "choppy", wind: 12.0, fetch: 300.0, chop: 1.05, swell: 0.68, whitecaps: 0.75 },
    Preset { name: "strong", wind: 16.0, fetch: 600.0, chop: 1.125, swell: 0.84, whitecaps: 0.875 },
    Preset { name: "storm", wind: 20.0, fetch: 900.0, chop: 1.2, swell: 1.0, whitecaps: 1.0 },
    Preset { name: "gale", wind: 30.0, fetch: 600.0, chop: 1.2, swell: 1.0, whitecaps: 1.0 },
];

fn device() -> Option<(wgpu::Device, wgpu::Queue)> {
    let instance = wgpu::Instance::new(wgpu::InstanceDescriptor::new_without_display_handle_from_env());
    let adapter = pollster::block_on(instance.request_adapter(&wgpu::RequestAdapterOptions::default())).ok()?;
    pollster::block_on(adapter.request_device(&wgpu::DeviceDescriptor::default())).ok()
}

fn spectrum(p: &Preset) -> SpectrumInputs {
    let mut params = <WgrWaterParams as bytemuck::Zeroable>::zeroed();
    params.tidewater = crate::water_tw::DEFAULT_PARAMS;
    params.tidewater[0][1] = p.chop;
    params.tidewater[0][2] = p.swell;
    params.tidewater_sea = [p.wind, p.fetch, p.whitecaps, 0.0];
    SpectrumInputs::from_params(&params)
}

fn read_foam(device: &wgpu::Device, queue: &wgpu::Queue, fft: &OceanFft) -> Vec<f32> {
    let bytes = (N * N * CASCADES) as u64 * 4;
    let staging = device.create_buffer(&wgpu::BufferDescriptor {
        label: Some("tw_foam_readback"),
        size: bytes,
        usage: wgpu::BufferUsages::MAP_READ | wgpu::BufferUsages::COPY_DST,
        mapped_at_creation: false,
    });
    let mut enc = device.create_command_encoder(&Default::default());
    enc.copy_buffer_to_buffer(fft.foam_buffer(), 0, &staging, 0, bytes);
    queue.submit([enc.finish()]);
    staging.slice(..).map_async(wgpu::MapMode::Read, |_| {});
    device.poll(wgpu::PollType::wait_indefinitely()).unwrap();
    let out = bytemuck::cast_slice::<u8, f32>(&staging.slice(..).get_mapped_range()).to_vec();
    out
}

/// The share of open deep water the whitecap path draws as a cap (`capCover`, area mean) and as a
/// solid one (`capCover > 0.5`), from the foam field as tw_water.wgsl reads it (gust at its mean).
/// What the sea shows as caps: the area mean of the cap ramp, the solid share, and the solid caps as
/// patches (4-connected on the 0.7 m sample grid): how many per hectare, their mean and 90th
/// percentile area (m^2). Cover alone cannot tell a few large floes from many small breaking crests.
struct Cover {
    mean: f32,
    solid: f32,
    per_ha: f32,
    mean_area: f32,
    p90_area: f32,
}

fn cover(foam: &[f32], sizes: [f32; 4], patch: [f32; 2]) -> Cover {
    let n = N as usize;
    // the shader reads the foam per vertex at mip 1.5 (~3 texels wide); TW_COVER_BLUR=0: unblurred
    let blur = !std::env::var("TW_COVER_BLUR").is_ok_and(|v| v.trim() == "0");
    let blurred: Vec<Vec<f32>> = (0..CASCADES as usize)
        .map(|c| {
            let f = &foam[c * n * n..(c + 1) * n * n];
            if !blur {
                return f.to_vec();
            }
            let mut b = vec![0.0f32; n * n];
            for y in 0..n {
                for x in 0..n {
                    let mut s = 0.0;
                    for dy in [n - 1, 0, 1] {
                        for dx in [n - 1, 0, 1] {
                            s += f[((y + dy) % n) * n + (x + dx) % n];
                        }
                    }
                    b[y * n + x] = s / 9.0;
                }
            }
            b
        })
        .collect();
    let sample = |c: usize, wx: f32, wz: f32| {
        let u = (wx / sizes[c]).rem_euclid(1.0) * n as f32 - 0.5;
        let v = (wz / sizes[c]).rem_euclid(1.0) * n as f32 - 0.5;
        let (x0, y0) = (u.floor(), v.floor());
        let (fx, fy) = (u - x0, v - y0);
        let at = |x: f32, y: f32| blurred[c][((y as i64).rem_euclid(n as i64) as usize) * n + (x as i64).rem_euclid(n as i64) as usize];
        let a = at(x0, y0) + (at(x0 + 1.0, y0) - at(x0, y0)) * fx;
        let b = at(x0, y0 + 1.0) + (at(x0 + 1.0, y0 + 1.0) - at(x0, y0 + 1.0)) * fx;
        a + (b - a) * fy
    };
    // Tidewater's per-cascade foam weights (WaterSurface.js foamWeights)
    let weights = [0.35f32, 0.45, 0.5, 0.25];
    let smooth = |a: f32, b: f32, x: f32| {
        let t = ((x - a) / (b - a)).clamp(0.0, 1.0);
        t * t * (3.0 - 2.0 * t)
    };
    let (mut mean, mut solid, mut count) = (0.0f64, 0.0f64, 0.0f64);
    let step = 0.7f32;
    const G: usize = 720;
    let mut mask = vec![false; G * G];
    for j in 0..G {
        for i in 0..G {
            let (wx, wz) = (i as f32 * step + 0.13, j as f32 * step + 0.37);
            let f: f32 = (0..4).map(|c| sample(c, wx, wz) * weights[c]).sum();
            let cap = smooth(patch[0], patch[1], f);
            mean += cap as f64;
            mask[j * G + i] = cap > 0.5;
            solid += mask[j * G + i] as u32 as f64;
            count += 1.0;
        }
    }
    let mut areas = Vec::new();
    let mut seen = vec![false; G * G];
    let mut stack = Vec::new();
    for start in 0..G * G {
        if !mask[start] || seen[start] {
            continue;
        }
        seen[start] = true;
        stack.push(start);
        let mut cells = 0usize;
        while let Some(k) = stack.pop() {
            cells += 1;
            let (i, j) = (k % G, k / G);
            let mut visit = |n: usize| {
                if mask[n] && !seen[n] {
                    seen[n] = true;
                    stack.push(n);
                }
            };
            if i > 0 { visit(k - 1); }
            if i + 1 < G { visit(k + 1); }
            if j > 0 { visit(k - G); }
            if j + 1 < G { visit(k + G); }
        }
        areas.push(cells as f32 * step * step);
    }
    areas.sort_by(|a, b| a.partial_cmp(b).unwrap());
    let ha = (G as f32 * step).powi(2) / 10_000.0;
    Cover {
        mean: (mean / count) as f32,
        solid: (solid / count) as f32,
        per_ha: areas.len() as f32 / ha,
        mean_area: if areas.is_empty() { 0.0 } else { areas.iter().sum::<f32>() / areas.len() as f32 },
        p90_area: areas.get(areas.len() * 9 / 10).copied().unwrap_or(0.0),
    }
}

/// Runs the sea for `seconds` at 60 Hz and returns the cover after it.
fn run(device: &wgpu::Device, queue: &wgpu::Queue, spec: SpectrumInputs, chop: f32, seconds: f32) -> Cover {
    // the coverage island-expansion thresholds its lace pattern by (foam x foamCoverage 1, smoothed)
    // TW_COVER_AMP: the drawn amplitude (choppiness held as the game holds it, x amplitude <= 1.3)
    let amp = std::env::var("TW_COVER_AMP").ok().and_then(|v| v.parse::<f32>().ok()).unwrap_or(1.0);
    cover(&run_foam(device, queue, spec, chop, seconds, amp), spec.sizes, [0.0, 1.0])
}

/// Runs the sea for `seconds` at 60 Hz and returns the persistent foam field after it.
fn run_foam(device: &wgpu::Device, queue: &wgpu::Queue, spec: SpectrumInputs, chop: f32, seconds: f32, amp: f32) -> Vec<f32> {
    let timers = crate::gpu_timers::GpuTimers::new(device, queue, false, false);
    let mut fft = OceanFft::new(device);
    let dt = 1.0 / 60.0;
    let frames = (seconds / dt) as u32;
    for f in 0..frames {
        // TW_COVER_FOAMLANE: the Jacobian lane (choppiness factor the foam sees) instead of the amplitude
        let lane = std::env::var("TW_COVER_FOAMLANE").ok().and_then(|v| v.parse::<f32>().ok()).unwrap_or(amp);
        fft.set_frame(queue, spec, chop.min(1.3 / amp.max(1.0)), lane, if std::env::var("TW_COVER_FOAMLANE").is_ok() { 0.0 } else { chop }, 100.0 + f as f32 * dt, dt);
        let mut enc = device.create_command_encoder(&Default::default());
        fft.dispatch(&mut enc, &timers);
        queue.submit([enc.finish()]);
        if f % 120 == 0 {
            device.poll(wgpu::PollType::wait_indefinitely()).unwrap();
        }
    }
    read_foam(device, queue, &fft)
}

/// Prints the cover per preset (cargo test ... whitecap_cover -- --ignored --nocapture).
/// TW_COVER_BIASES=a,b,.. sweeps the bias, TW_COVER_DECAY overrides the decay, TW_COVER_ONLY=storm,..
/// picks presets.
#[test]
#[ignore]
fn whitecap_cover_by_preset() {
    let Some((device, queue)) = device() else {
        eprintln!("whitecap cover: no adapter");
        return;
    };
    let env = |k: &str| std::env::var(k).ok().and_then(|v| v.parse::<f32>().ok());
    let biases: Vec<Option<f32>> = match std::env::var("TW_COVER_BIASES") {
        Ok(v) => v.split(',').filter_map(|s| s.trim().parse::<f32>().ok()).map(Some).collect(),
        Err(_) => vec![None],
    };
    let only = std::env::var("TW_COVER_ONLY").unwrap_or_default();
    for p in PRESETS.iter().filter(|p| only.is_empty() || only.contains(p.name)) {
        for b in &biases {
            let mut spec = spectrum(p);
            if let Some(b) = b {
                spec.foam_bias = *b;
            }
            if let Some(d) = env("TW_COVER_DECAY") {
                spec.foam_decay = d;
            }
            let c = run(&device, &queue, spec, p.chop, 40.0);
            eprintln!(
                "{:7} wind {:4.1} bias {:.3} decay {:.3}: cap cover {:5.1}%  solid {:5.1}%  patches {:6.1}/ha mean {:6.1} m2 p90 {:6.1} m2",
                p.name, p.wind, spec.foam_bias, spec.foam_decay, c.mean * 100.0, c.solid * 100.0, c.per_ha, c.mean_area, c.p90_area
            );
        }
    }
}

/// The guard: the whitecap coverage rises with the wind across the weather presets.
#[test]
fn whitecap_coverage_rises_with_the_wind() {
    let Some((device, queue)) = device() else {
        eprintln!("whitecap cover: no adapter");
        return;
    };
    let mean = |name: &str| {
        let p = PRESETS.iter().find(|p| p.name == name).unwrap();
        run(&device, &queue, spectrum(p), p.chop, 40.0).mean
    };
    let (breezy, choppy, storm) = (mean("breezy"), mean("choppy"), mean("storm"));
    assert!(breezy < choppy && choppy < storm, "{breezy} {choppy} {storm}");
}

/// The foam pattern (FoamTexture.js) at full resolution, read back: (foam, bubbles) per texel.
fn bake_pattern(device: &wgpu::Device, queue: &wgpu::Queue) -> Vec<[f32; 2]> {
    let size = crate::water_tw::textures::FOAM_SIZE;
    let tex = device.create_texture(&wgpu::TextureDescriptor {
        label: Some("tw_foam_pattern_readback"),
        size: wgpu::Extent3d { width: size, height: size, depth_or_array_layers: 1 },
        mip_level_count: 1,
        sample_count: 1,
        dimension: wgpu::TextureDimension::D2,
        format: wgpu::TextureFormat::Rgba16Float,
        usage: wgpu::TextureUsages::STORAGE_BINDING | wgpu::TextureUsages::COPY_SRC,
        view_formats: &[],
    });
    let module = device.create_shader_module(wgpu::ShaderModuleDescriptor {
        label: Some("tw_foam_pattern"),
        source: wgpu::ShaderSource::Wgsl(crate::water_tw::textures::wgsl_foam_pattern().into()),
    });
    let pipe = device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
        label: Some("tw_foam_pattern"),
        layout: None,
        module: &module,
        entry_point: Some("main"),
        compilation_options: Default::default(),
        cache: None,
    });
    let view = tex.create_view(&Default::default());
    let bind = device.create_bind_group(&wgpu::BindGroupDescriptor {
        label: None,
        layout: &pipe.get_bind_group_layout(0),
        entries: &[wgpu::BindGroupEntry { binding: 0, resource: wgpu::BindingResource::TextureView(&view) }],
    });
    let row = size * 8;
    let staging = device.create_buffer(&wgpu::BufferDescriptor {
        label: None,
        size: (row * size) as u64,
        usage: wgpu::BufferUsages::MAP_READ | wgpu::BufferUsages::COPY_DST,
        mapped_at_creation: false,
    });
    let mut enc = device.create_command_encoder(&Default::default());
    {
        let mut pass = enc.begin_compute_pass(&Default::default());
        pass.set_pipeline(&pipe);
        pass.set_bind_group(0, &bind, &[]);
        pass.dispatch_workgroups(size / 8, size / 8, 1);
    }
    enc.copy_texture_to_buffer(
        tex.as_image_copy(),
        wgpu::TexelCopyBufferInfo { buffer: &staging, layout: wgpu::TexelCopyBufferLayout { offset: 0, bytes_per_row: Some(row), rows_per_image: None } },
        wgpu::Extent3d { width: size, height: size, depth_or_array_layers: 1 },
    );
    queue.submit([enc.finish()]);
    staging.slice(..).map_async(wgpu::MapMode::Read, |_| {});
    device.poll(wgpu::PollType::wait_indefinitely()).unwrap();
    let half = |h: u16| -> f32 {
        let s = if h & 0x8000 != 0 { -1.0 } else { 1.0 };
        let e = ((h >> 10) & 0x1f) as i32;
        let m = (h & 0x3ff) as f32;
        s * if e == 0 { m * 2f32.powi(-24) } else { (1.0 + m / 1024.0) * 2f32.powi(e - 15) }
    };
    let raw = staging.slice(..).get_mapped_range();
    let h: &[u16] = bytemuck::cast_slice(&raw);
    (0..(size * size) as usize).map(|i| [half(h[i * 4]), half(h[i * 4 + 1])]).collect()
}

/// Box mips of a square single-channel image, level 0 first.
fn box_mips(base: Vec<f32>, size: usize) -> Vec<(usize, Vec<f32>)> {
    let mut out = vec![(size, base)];
    while out.last().unwrap().0 > 1 {
        let (n, prev) = out.last().unwrap();
        let (n, h) = (*n, *n / 2);
        let mut next = vec![0.0f32; h * h];
        for y in 0..h {
            for x in 0..h {
                next[y * h + x] = 0.25 * (prev[2 * y * n + 2 * x] + prev[2 * y * n + 2 * x + 1] + prev[(2 * y + 1) * n + 2 * x] + prev[(2 * y + 1) * n + 2 * x + 1]);
            }
        }
        out.push((h, next));
    }
    out
}

/// The lace's area mean (tw_water.wgsl `detail` at footprint 0) at coverages 0..1 in steps of 0.05,
/// from the full-resolution pattern: what `laceExpected` must return far off.
fn lace_table(px: &[(usize, Vec<f32>)], py: &[(usize, Vec<f32>)]) -> Vec<f32> {
    let (ps, x0) = (px[0].0, &px[0].1);
    let y0 = &py[0].1;
    let at = |img: &Vec<f32>, u: f32, v: f32| {
        let x = (u.rem_euclid(1.0) * ps as f32) as usize % ps;
        let y = (v.rem_euclid(1.0) * ps as f32) as usize % ps;
        img[y * ps + x]
    };
    let smooth = |a: f32, b: f32, x: f32| {
        let t = ((x - a) / (b - a)).clamp(0.0, 1.0);
        t * t * (3.0 - 2.0 * t)
    };
    (0..=20)
        .map(|k| {
            let c = k as f32 / 20.0;
            let thresh = 1.05 - c * 1.1;
            let (mut s, mut cnt) = (0.0f64, 0.0f64);
            for j in (0..ps).step_by(3) {
                for i in (0..ps).step_by(3) {
                    let (fu, fv) = (i as f32 / ps as f32, j as f32 / ps as f32);
                    let r2 = (fu * 0.8 - fv * 0.6, fu * 0.6 + fv * 0.8);
                    let pattern = at(x0, fu, fv) * 0.62 + at(x0, r2.0 * 2.37 + 0.31, r2.1 * 2.37 + 0.77) * 0.38;
                    s += (smooth(thresh - 0.06, thresh + 0.06, pattern) * (at(y0, fu, fv) * 0.25 + 0.8)) as f64;
                    cnt += 1.0;
                }
            }
            (s / cnt) as f32
        })
        .collect()
}

/// The guard: the far lace table in tw_water.wgsl is what the foam pattern really draws. If the
/// pattern (textures.rs) or the lace threshold changes, the table must be measured again (the
/// `far_foam_matches_near_lace` print shows the new one) or a distant sea turns milky or dark.
#[test]
fn lace_expected_table_matches_the_pattern() {
    let Some((device, queue)) = device() else {
        eprintln!("lace table: no adapter");
        return;
    };
    let src = include_str!("tw_water.wgsl");
    let start = src.find("var g = array<f32, 21>(").expect("laceExpected table") + "var g = array<f32, 21>(".len();
    let end = start + src[start..].find(')').unwrap();
    let table: Vec<f32> = src[start..end].split(',').map(|v| v.trim().parse::<f32>().unwrap()).collect();
    let pat = bake_pattern(&device, &queue);
    let ps = crate::water_tw::textures::FOAM_SIZE as usize;
    let px = box_mips(pat.iter().map(|p| p[0]).collect(), ps);
    let py = box_mips(pat.iter().map(|p| p[1]).collect(), ps);
    let measured = lace_table(&px, &py);
    for (k, (t, m)) in table.iter().zip(&measured).enumerate() {
        assert!((t - m).abs() < 0.01, "coverage {:.2}: table {t} measured {m}", k as f32 / 20.0);
    }
}

/// What a whitecapping sea looks like near the camera and far from it, from the same foam field:
/// the lace detail (tw_water.wgsl `detail`) with the pattern read at the mip a pixel of the given
/// footprint reads, averaged over the area, against the full-resolution (near) average. A value
/// well above the near one is a sea that turns milky with distance.
/// (cargo test ... far_foam -- --ignored --nocapture)
#[test]
#[ignore]
fn far_foam_matches_near_lace() {
    let Some((device, queue)) = device() else {
        eprintln!("far foam: no adapter");
        return;
    };
    let pat = bake_pattern(&device, &queue);
    let ps = crate::water_tw::textures::FOAM_SIZE as usize;
    let px = box_mips(pat.iter().map(|p| p[0]).collect(), ps);
    let py = box_mips(pat.iter().map(|p| p[1]).collect(), ps);
    let stats = |v: &Vec<f32>| {
        let m = v.iter().map(|&x| x as f64).sum::<f64>() / v.len() as f64;
        let var = v.iter().map(|&x| (x as f64 - m).powi(2)).sum::<f64>() / v.len() as f64;
        (m as f32, var.sqrt() as f32)
    };
    for (l, (n, v)) in px.iter().enumerate() {
        let (m, sd) = stats(v);
        eprintln!("pattern mip {l:2} ({n:4}): mean {m:.4} sd {sd:.4}");
    }
    let at = |mips: &Vec<(usize, Vec<f32>)>, lod: f32, u: f32, v: f32| {
        let l = (lod.max(0.0).round() as usize).min(mips.len() - 1);
        let (n, img) = &mips[l];
        let x = (u.rem_euclid(1.0) * *n as f32) as usize % n;
        let y = (v.rem_euclid(1.0) * *n as f32) as usize % n;
        img[y * n + x]
    };
    let sd0 = stats(&px[0].1).1;
    let sd_at: Vec<f32> = px.iter().map(|(_, v)| stats(v).1).collect();
    let mean0 = stats(&px[0].1).0;
    let scale = 0.09f32; // tw.surface.w
    let n = N as usize;
    let weights = [0.35f32, 0.45, 0.5, 0.25];
    let smooth = |a: f32, b: f32, x: f32| {
        let t = ((x - a) / (b - a)).clamp(0.0, 1.0);
        t * t * (3.0 - 2.0 * t)
    };
    let g_table = lace_table(&px, &py);
    eprintln!("G(c) c=0..1 step 0.05: {:?}", g_table.iter().map(|g| (g * 1000.0).round() / 1000.0).collect::<Vec<_>>());
    let amp_list: Vec<f32> = std::env::var("TW_FAR_AMPS").ok().map(|v| v.split(',').filter_map(|s| s.trim().parse().ok()).collect()).unwrap_or(vec![1.0, 2.0]);
    let only = std::env::var("TW_COVER_ONLY").unwrap_or_else(|_| "choppy,storm,gale".into());
    // TW_FAR_FIX: 1 = expected-lace far value + contrast-restored pattern mips, 2 = far value only
    let fix_mode = std::env::var("TW_FAR_FIX").ok().and_then(|v| v.trim().parse::<u32>().ok()).unwrap_or(0);
    for p in PRESETS.iter().filter(|p| only.contains(p.name)) {
        for &amp in &amp_list {
            let foam = run_foam(&device, &queue, spectrum(p), p.chop, 40.0, amp);
            let sizes = spectrum(p).sizes;
            let sample = |c: usize, wx: f32, wz: f32| {
                let x = ((wx / sizes[c]).rem_euclid(1.0) * n as f32) as usize % n;
                let y = ((wz / sizes[c]).rem_euclid(1.0) * n as f32) as usize % n;
                foam[c * n * n + y * n + x]
            };
            let mut line = format!("{:6} amp {:.1}:", p.name, amp);
            let mut cbar = 0.0;
            for &fp in &[0.0f32, 0.05, 0.1, 0.2, 0.4, 0.8, 1.6, 4.0] {
                let lod1 = if fp > 0.0 { (fp * scale * ps as f32).log2() } else { 0.0 };
                let lod2 = lod1 + 2.37f32.log2();
                let far = smooth(0.15, 1.2, fp);
                let soft = 0.06 + fp * 0.1;
                let (mut acc, mut cov_sum, mut cnt) = (0.0f64, 0.0f64, 0.0f64);
                let (step, g) = (0.5f32, 240usize);
                for j in 0..g {
                    for i in 0..g {
                        let (wx, wz) = (i as f32 * step + 0.11, j as f32 * step + 0.29);
                        let f: f32 = (0..4).map(|c| sample(c, wx, wz) * weights[c]).sum();
                        let cov = f.clamp(0.0, 1.0);
                        let (fu, fv) = (wx * scale, wz * scale);
                        let mut p1 = at(&px, lod1, fu, fv);
                        let r2 = (fu * 0.8 - fv * 0.6, fu * 0.6 + fv * 0.8);
                        let mut p2 = at(&px, lod2, r2.0 * 2.37 + 0.31, r2.1 * 2.37 + 0.77);
                        if fix_mode == 1 {
                            // the contrast a mip loses, restored around the pattern's mean
                            let k = |lod: f32| sd0 / sd_at[(lod.max(0.0).round() as usize).min(sd_at.len() - 1)].max(1e-4);
                            p1 = mean0 + (p1 - mean0) * k(lod1);
                            p2 = mean0 + (p2 - mean0) * k(lod2);
                        }
                        let pattern = p1 * 0.62 + p2 * 0.38;
                        let thresh = 1.05 - cov * 1.1;
                        let detail = smooth(thresh - soft, thresh + soft, pattern) * (at(&py, lod1, fu, fv) * 0.25 + 0.8);
                        let far_v = if fix_mode > 0 {
                            let x = cov * 20.0;
                            let k = (x.floor() as usize).min(19);
                            g_table[k] + (g_table[k + 1] - g_table[k]) * (x - k as f32)
                        } else {
                            cov * 0.85
                        };
                        acc += (detail + (far_v - detail) * far) as f64;
                        cov_sum += cov as f64;
                        cnt += 1.0;
                    }
                }
                cbar = cov_sum / cnt;
                line += &format!(" {fp:.2}m {:5.1}%", acc / cnt * 100.0);
            }
            eprintln!("{line}   (mean coverage {cbar:.3})");
        }
    }
}
