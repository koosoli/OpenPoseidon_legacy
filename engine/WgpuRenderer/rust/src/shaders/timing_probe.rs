// LOAD-002 diagnostic: CPU cost of compose + validate + SPIR-V emission per entry shader, the part of
// renderer start-up that runs on every launch regardless of the driver cache. Run with
// `cargo test --profile rwdi -p wgpu_renderer --lib shader_cpu_timing -- --ignored --nocapture`.
#[cfg(test)]
mod t {
    use crate::shaders::build_composer;
    use naga_oil::compose::NagaModuleDescriptor;
    #[test]
    #[ignore]
    fn shader_cpu_timing() {
        let files: &[(&str, &str)] = &[
            ("gfx3d/shader3d.wgsl", include_str!("../gfx3d/shader3d.wgsl")),
            ("gfx3d/shadow_depth.wgsl", include_str!("../gfx3d/shadow_depth.wgsl")),
            ("gfx3d/gpu_driven.wgsl", include_str!("../gfx3d/gpu_driven.wgsl")),
            ("gfx3d/gpu_driven_shadow.wgsl", include_str!("../gfx3d/gpu_driven_shadow.wgsl")),
            ("gfx3d/velocity_obj.wgsl", include_str!("../gfx3d/velocity_obj.wgsl")),
            ("terrain/terrain.wgsl", include_str!("../terrain/terrain.wgsl")),
            ("terrain/terrain_shadow.wgsl", include_str!("../terrain/terrain_shadow.wgsl")),
            ("grass/grass.wgsl", include_str!("../grass/grass.wgsl")),
            ("water/water.wgsl", include_str!("../water/water.wgsl")),
            ("far/far.wgsl", include_str!("../far/far.wgsl")),
        ];
        let t_c = std::time::Instant::now();
        let mut composer = build_composer();
        eprintln!("build_composer {:.1} ms", t_c.elapsed().as_secs_f64() * 1e3);
        for (path, src) in files {
            let t0 = std::time::Instant::now();
            let m = composer.make_naga_module(NagaModuleDescriptor { source: src, file_path: path, ..Default::default() }).unwrap();
            let t1 = std::time::Instant::now();
            let info = naga::valid::Validator::new(naga::valid::ValidationFlags::all(), naga::valid::Capabilities::all()).validate(&m).unwrap();
            let t2 = std::time::Instant::now();
            let mut spv_ms = 0.0; let mut n = 0;
            for ep in &m.entry_points {
                let ts = std::time::Instant::now();
                let po = naga::back::spv::PipelineOptions { shader_stage: ep.stage, entry_point: ep.name.clone() };
                let _ = naga::back::spv::write_vec(&m, &info, &naga::back::spv::Options::default(), Some(&po));
                spv_ms += ts.elapsed().as_secs_f64() * 1e3; n += 1;
            }
            eprintln!("{path:34} compose {:7.1} ms  validate {:7.1} ms  spv {:7.1} ms ({n} entry points, {} fns)",
                (t1 - t0).as_secs_f64() * 1e3, (t2 - t1).as_secs_f64() * 1e3, spv_ms, m.functions.len());
        }
    }
}
