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
    let timers = crate::gpu_timers::GpuTimers::new(device, queue, false, false);
    let mut fft = OceanFft::new(device);
    let dt = 1.0 / 60.0;
    let frames = (seconds / dt) as u32;
    // TW_COVER_AMP: the drawn amplitude (choppiness held as the game holds it, x amplitude <= 1.3)
    let amp = std::env::var("TW_COVER_AMP").ok().and_then(|v| v.parse::<f32>().ok()).unwrap_or(1.0);
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
    // the coverage island-expansion thresholds its lace pattern by (foam x foamCoverage 1, smoothed)
    cover(&read_foam(device, queue, &fft), spec.sizes, [0.0, 1.0])
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
