// Far-field / large-world terrain rendering experiments.
//
// Deliberately isolated from the engine: this crate is its own cargo workspace,
// links nothing from Poseidon, and renders a procedurally generated world. Its
// only job is to produce defensible numbers for the architecture decision.
//
//   cargo run --release -- far        EXP-FAR-001..004  raymarch cost + step counts
//   cargo run --release -- raster     EXP-TERRAIN-002/3 CDLOD raster cost, same world
//   cargo run --release -- compare    both, same camera, same target, one process
//   cargo run --release -- scale      world-size sweep: memory + cost vs extent
//   cargo run --release -- veg        EXP-VEG-001 tree-instance clustering arithmetic

mod camera;
mod cdlod;
mod gpu;
mod raster;
mod raymarch;
mod veg;
mod world;

use gpu::{write_bmp, Gpu};
use raymarch::{Mode, Raymarcher};
use world::{World, WorldGpu, WorldSpec};

struct Args {
    cmd: String,
    n: u32,
    grid: f32,
    width: u32,
    height: u32,
    frames: usize,
    far: f32,
    max_steps: u32,
    fixed_step: f32,
    seed: u32,
    out: String,
    shots: bool,
}

fn parse() -> Args {
    let mut a = Args {
        cmd: "far".into(),
        n: 4096,
        grid: 12.5,
        width: 1920,
        height: 1080,
        frames: 20,
        far: 100_000.0,
        max_steps: 512,
        fixed_step: 8.0,
        seed: 1337,
        out: "out".into(),
        shots: true,
    };
    let mut it = std::env::args().skip(1);
    if let Some(c) = it.next() {
        if !c.starts_with("--") {
            a.cmd = c;
        }
    }
    let rest: Vec<String> = std::env::args().skip(1).collect();
    let mut i = 0;
    while i < rest.len() {
        let k = rest[i].as_str();
        let mut val = || {
            i += 1;
            rest.get(i).cloned().unwrap_or_default()
        };
        match k {
            "--n" => a.n = val().parse().unwrap(),
            "--grid" => a.grid = val().parse().unwrap(),
            "--width" => a.width = val().parse().unwrap(),
            "--height" => a.height = val().parse().unwrap(),
            "--frames" => a.frames = val().parse().unwrap(),
            "--far" => a.far = val().parse().unwrap(),
            "--max-steps" => a.max_steps = val().parse().unwrap(),
            "--fixed-step" => a.fixed_step = val().parse().unwrap(),
            "--seed" => a.seed = val().parse().unwrap(),
            "--out" => a.out = val(),
            "--no-shots" => a.shots = false,
            _ => {}
        }
        i += 1;
    }
    a
}

fn mb(bytes: u64) -> f32 {
    bytes as f32 / (1024.0 * 1024.0)
}

fn steps_heatmap(steps: &[u32], w: u32, h: u32, cap: u32) -> Vec<u8> {
    let mut out = vec![0u8; (w * h * 4) as usize];
    for (i, &s) in steps.iter().enumerate() {
        let t = (s as f32 / cap as f32).clamp(0.0, 1.0);
        // black -> blue -> yellow -> red
        let (r, g, b) = if t < 0.5 {
            let u = t * 2.0;
            (0.0, u * 0.7, 0.35 + u * 0.65)
        } else {
            let u = (t - 0.5) * 2.0;
            (u, 0.7 - u * 0.7, 1.0 - u)
        };
        out[i * 4] = (r * 255.0) as u8;
        out[i * 4 + 1] = (g * 255.0) as u8;
        out[i * 4 + 2] = (b * 255.0) as u8;
        out[i * 4 + 3] = 255;
    }
    out
}

fn make_world(a: &Args, with_cover: bool) -> World {
    let spec = WorldSpec { n: a.n, grid: a.grid, seed: a.seed, with_cover, veg_cell: 0.0 };
    let w = World::generate(spec);
    eprintln!(
        "world {}x{} @ {} m = {:.1} km, h {:.0}..{:.0} m, {} pyramid levels, {:.0} MB cpu, gen {:.0} ms",
        w.spec.n,
        w.spec.n,
        w.spec.grid,
        w.spec.extent_m() / 1000.0,
        w.h_min,
        w.h_max,
        w.levels(),
        mb(w.cpu_bytes),
        w.gen_ms
    );
    w
}

fn cmd_far(gpu: &Gpu, a: &Args) {
    std::fs::create_dir_all(&a.out).ok();
    let rm = Raymarcher::new(gpu, a.width, a.height);

    for &with_cover in &[false, true] {
        let w = make_world(a, with_cover);
        let wg = WorldGpu::upload(gpu, &w);
        eprintln!("  vram (height + max-mip pyramid + cover) = {:.0} MB", mb(wg.vram_bytes));

        let modes: &[(&str, Mode)] = if with_cover {
            &[("EXP-FAR-004 hier+cover", Mode::HierCoverHard), ("EXP-FAR-003 hier+canopy", Mode::HierCoverSoft)]
        } else {
            &[("EXP-FAR-001 fixed", Mode::Fixed), ("EXP-FAR-002 hier", Mode::Hier)]
        };

        println!();
        println!(
            "{:<26} {:<11} {:>9} {:>9} {:>9} {:>8} {:>8} {:>8}",
            "experiment", "view", "ms(min)", "ms(med)", "ms(max)", "steps~", "p99", "max"
        );
        for (label, mode) in modes {
            for (vname, view) in camera::standard_views(w.spec.extent_m(), w.h_max * 0.5) {
                let r = rm.run(
                    gpu, &w, &wg, &view, *mode, a.max_steps, a.far, a.fixed_step, a.frames, false,
                );
                println!(
                    "{:<26} {:<11} {:>9.3} {:>9.3} {:>9.3} {:>8.1} {:>8} {:>8}",
                    label, vname, r.ms.min, r.ms.median, r.ms.max, r.steps_mean, r.steps_p99, r.steps_max
                );
                if a.shots {
                    let tag = format!("{}-{}", label.split(' ').next().unwrap(), vname);
                    write_bmp(&format!("{}/{}.bmp", a.out, tag), a.width, a.height, &r.rgba).ok();
                    let hm = steps_heatmap(&r.steps_img, a.width, a.height, a.max_steps.min(256));
                    write_bmp(&format!("{}/{}-steps.bmp", a.out, tag), a.width, a.height, &hm).ok();
                }
            }
        }
    }
}

fn cmd_scale(gpu: &Gpu, a: &Args) {
    // Same sample spacing, growing extent: what does "100 km of terrain" cost?
    println!();
    println!(
        "{:>7} {:>8} {:>9} {:>10} {:>10} {:>9} {:>9} {:>9}",
        "n", "grid", "extent km", "cpu MB", "vram MB", "gen ms", "ground ms", "air ms"
    );
    let rm = Raymarcher::new(gpu, a.width, a.height);
    for &(n, grid) in &[
        (1024u32, 12.5f32),
        (2048, 12.5),
        (4096, 12.5),
        (8192, 12.5),
        (2048, 50.0),
        (4096, 50.0),
        (8192, 25.0),
    ] {
        let spec = WorldSpec { n, grid, seed: a.seed, with_cover: true, veg_cell: 0.0 };
        let w = World::generate(spec);
        let wg = WorldGpu::upload(gpu, &w);
        let views = camera::standard_views(w.spec.extent_m(), w.h_max * 0.5);
        let g = rm.run(gpu, &w, &wg, &views[0].1, Mode::HierCoverHard, a.max_steps, a.far, a.fixed_step, a.frames, false);
        let air = rm.run(gpu, &w, &wg, &views[3].1, Mode::HierCoverHard, a.max_steps, a.far, a.fixed_step, a.frames, false);
        println!(
            "{:>7} {:>8.1} {:>9.1} {:>10.0} {:>10.0} {:>9.0} {:>9.3} {:>9.3}",
            n,
            grid,
            w.spec.extent_m() / 1000.0,
            mb(w.cpu_bytes),
            mb(wg.vram_bytes),
            w.gen_ms,
            g.ms.min,
            air.ms.min
        );
    }
}


/// EXP-COL-001. Bakes explicit tree instances into vertical occupancy columns, then
/// renders the SAME baked data twice: once reconstructed bilinearly (what a far view
/// wants) and once nearest (one flat top per texel with vertical walls). The question
/// it answers is narrow and falsifiable: does a 2.5D column field, viewed from inside
/// its intended range, fail in the way the reference screenshots fail?
fn cmd_columns(gpu: &Gpu, a: &Args, veg_cell: f32) {
    std::fs::create_dir_all(&a.out).ok();
    let spec = WorldSpec { n: a.n, grid: a.grid, seed: a.seed, with_cover: true, veg_cell };
    let w = World::generate(spec);
    eprintln!(
        "world {:.1} km @ {} m heightfield, {} trees baked into {} x {} columns of {} m, {:.0} MB, gen {:.0} ms",
        w.spec.extent_m() / 1000.0, w.spec.grid, w.baked_trees, w.column_cells, w.column_cells,
        veg_cell, mb(w.cpu_bytes), w.gen_ms
    );
    let wg = WorldGpu::upload(gpu, &w);
    let rm = Raymarcher::new(gpu, a.width, a.height);

    // Eye-level and low-air views inside a forested area, i.e. deliberately much
    // closer than any sane transition distance.
    let c = w.spec.extent_m() * 0.5;
    let ground = w.h_max * 0.35;
    let views: Vec<(&str, camera::View)> = vec![
        ("close-eye", camera::View {
            eye: glam::Vec3::new(c, ground + 30.0, c - 900.0),
            yaw: 0.0, pitch: 6.0f32.to_radians(), fov_y: 55.0f32.to_radians() }),
        ("close-low", camera::View {
            eye: glam::Vec3::new(c, ground + 220.0, c - 2200.0),
            yaw: 0.0, pitch: -3.0f32.to_radians(), fov_y: 55.0f32.to_radians() }),
        ("far-2km", camera::View {
            eye: glam::Vec3::new(c, ground + 400.0, c - 6000.0),
            yaw: 0.0, pitch: -2.0f32.to_radians(), fov_y: 55.0f32.to_radians() }),
    ];

    println!();
    println!("EXP-COL-001  2.5D vertical occupancy columns, one interval per XY cell");
    println!(
        "{:<12} {:<10} {:>9} {:>9} {:>8}",
        "view", "filter", "ms(min)", "ms(med)", "steps~"
    );
    for (vname, view) in &views {
        for &(fname, nearest) in &[("bilinear", false), ("nearest", true)] {
            let r = rm.run(gpu, &w, &wg, view, Mode::HierCoverHard,
                           a.max_steps, a.far, a.fixed_step, a.frames, nearest);
            println!(
                "{:<12} {:<10} {:>9.3} {:>9.3} {:>8.1}",
                vname, fname, r.ms.min, r.ms.median, r.steps_mean
            );
            write_bmp(
                &format!("{}/EXP-COL-001-{}-{}.bmp", a.out, vname, fname),
                a.width, a.height, &r.rgba,
            ).ok();
        }
    }
}

fn main() {
    let a = parse();
    let gpu = Gpu::new();
    eprintln!(
        "adapter: {} ({}), timestamps {}",
        gpu.adapter_name,
        gpu.backend,
        if gpu.timestamps { "on" } else { "OFF" }
    );
    eprintln!("target: {}x{}, {} frames/measurement, quoting the cheapest", a.width, a.height, a.frames);

    match a.cmd.as_str() {
        "far" => cmd_far(&gpu, &a),
        "raster" => raster::cmd_raster(&gpu, &a_to_raster(&a)),
        "compare" => {
            cmd_far(&gpu, &a);
            raster::cmd_raster(&gpu, &a_to_raster(&a));
        }
        "scale" => cmd_scale(&gpu, &a),
        "columns" => cmd_columns(&gpu, &a, veg_cell_arg()),
        "veg" => cdlod::cmd_veg(&a_to_raster(&a)),
        "vegcards" => veg::cmd_veg_cards(gpu_ref(&gpu), &a_to_raster(&a)),
        other => eprintln!("unknown command {other}"),
    }
}

/// Small shared view of the CLI for the other modules.
pub struct Common {
    pub n: u32,
    pub grid: f32,
    pub width: u32,
    pub height: u32,
    pub frames: usize,
    pub far: f32,
    pub seed: u32,
    pub out: String,
    pub shots: bool,
}

fn a_to_raster(a: &Args) -> Common {
    Common {
        n: a.n,
        grid: a.grid,
        width: a.width,
        height: a.height,
        frames: a.frames,
        far: a.far,
        seed: a.seed,
        out: a.out.clone(),
        shots: a.shots,
    }
}

fn gpu_ref(g: &Gpu) -> &Gpu { g }

/// Column cell size in metres for the `columns` command (--veg-cell), default 8 m.
fn veg_cell_arg() -> f32 {
    let v: Vec<String> = std::env::args().collect();
    for (i, k) in v.iter().enumerate() {
        if k == "--veg-cell" {
            return v.get(i + 1).and_then(|s| s.parse().ok()).unwrap_or(8.0);
        }
    }
    8.0
}
