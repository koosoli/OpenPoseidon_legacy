//! Shader composition via naga_oil.
//!
//! The 3D pipelines share a lot of WGSL — the group(0) frame/shadow bindings,
//! the cascaded-shadow kernel, GPU skinning, sun lighting. Rather than
//! copy-paste, the shared pieces live in the sibling `*.wgsl` files here as
//! naga_oil composable modules (`#define_import_path`), and the entry-point
//! shaders `#import` them. This module builds the composer and turns a composed
//! source into a `wgpu::ShaderModule`.
//!
//! Pipeline-overridable `override` constants stay in the entry shaders, never in
//! a shared module: naga_oil's own `override` keyword means virtual-function
//! override, so keeping them separate sidesteps the collision, and they survive
//! composition as ordinary naga `Override` entries (wgpu still applies them via
//! `PipelineCompilationOptions`).

use std::borrow::Cow;

#[cfg(test)]
mod shadow_tent_tests;

#[cfg(test)]
mod rain_cover_tests;
#[cfg(test)]
mod weather_cover_tests;
#[cfg(test)]
mod snow_receiver_tests;

#[cfg(test)]
mod snow_material_tests;
#[cfg(test)]
mod tree_snow_tests;

#[cfg(test)]
mod ground_puddles_tests;

#[cfg(test)]
mod mud_geometry_tests;
#[cfg(test)]
mod ground_road_tests;
#[cfg(test)]
mod ground_mesh_tests;
#[cfg(test)]
mod ground_wet_tests;
#[cfg(test)]
mod sand_geometry_tests;

use naga_oil::compose::{
    ComposableModuleDescriptor, Composer, NagaModuleDescriptor, ShaderLanguage,
};

/// Build a composer with every shared module registered. Registration order
/// matters: a module must be added before anything that `#import`s it — `shadow`
/// and `lighting` import `frame`.
// REN-OBJ-004: whether the device was created with SHADER_EARLY_DEPTH_TEST. Set once by the
// renderer before any shader is composed. When true the composer validates the
// EARLY_DEPTH_TEST capability and every module is composed with the EARLY_DEPTH define, which
// is what lets a shader carry an `@early_depth_test(force)` entry at all -- naga rejects the
// whole module otherwise, switch or no switch (which is how the first attempt took the game
// down at startup).
static EARLY_DEPTH_SUPPORTED: std::sync::OnceLock<bool> = std::sync::OnceLock::new();

pub fn set_early_depth_supported(on: bool) {
    let _ = EARLY_DEPTH_SUPPORTED.set(on);
}

pub fn early_depth_supported() -> bool {
    *EARLY_DEPTH_SUPPORTED.get().unwrap_or(&false)
}

pub(crate) fn shader_defs() -> std::collections::HashMap<String, naga_oil::compose::ShaderDefValue> {
    let mut defs = std::collections::HashMap::new();
    if std::env::var("WGR_TREE_SNOW").as_deref() == Ok("0") {
        defs.insert("DISABLE_TREE_SNOW".to_string(), naga_oil::compose::ShaderDefValue::Bool(true));
    }
    if std::env::var("WGR_FOREST_SNOW_COVER_DEBUG").as_deref() == Ok("1") {
        defs.insert("FOREST_SNOW_COVER_DEBUG".to_string(), naga_oil::compose::ShaderDefValue::Bool(true));
        static REPORTED: std::sync::OnceLock<()> = std::sync::OnceLock::new();
        if REPORTED.set(()).is_ok() {
            eprintln!("[wgr] forest snow coverage diagnostic: enabled=1 scope=admitted-retained-forest-leaves-only-preserve-alpha");
        }
    }
    if std::env::var("WGR_SNOW_POWDER").as_deref() == Ok("0") {
        defs.insert("DISABLE_SNOW_POWDER_DETAIL".to_string(), naga_oil::compose::ShaderDefValue::Bool(true));
    }
    // Native (.xob) trees keep their authored leaf normals: the spherical crown
    // bend flattens dense native crowns into a smooth wash (Reference-spot A/B:
    // authored = modeled light/shade with depth, bent = flat bright). Default ON;
    // WGR_NATIVE_AUTHORED_NORMALS=0 restores the bend. Legacy trees can never take
    // this path (kind 3 / the coherent-wind bit require a .xob model name), so
    // legacy output is identical either way.
    if std::env::var("WGR_NATIVE_AUTHORED_NORMALS").as_deref() != Ok("0") {
        defs.insert("NATIVE_AUTHORED_NORMALS".to_string(), naga_oil::compose::ShaderDefValue::Bool(true));
    }
    if std::env::var("WGR_NATIVE_LEAF_CAVITY").as_deref() == Ok("1") {
        defs.insert("NATIVE_LEAF_CAVITY".to_string(), naga_oil::compose::ShaderDefValue::Bool(true));
    }
    if std::env::var("WGR_REFORGER_PBR_PREVIEW").as_deref() == Ok("1") {
        defs.insert("NATIVE_PBR_PREVIEW".to_string(), naga_oil::compose::ShaderDefValue::Bool(true));
    }
    if std::env::var("WGR_NATIVE_GROUND_AO").as_deref() == Ok("0") {
        defs.insert("DISABLE_NATIVE_GROUND_AO".to_string(), naga_oil::compose::ShaderDefValue::Bool(true));
    }
    if std::env::var("WGR_COALESCED_PCF").as_deref() == Ok("1") {
        defs.insert("COALESCED_PCF".to_string(), naga_oil::compose::ShaderDefValue::Bool(true));
    }
    if early_depth_supported() {
        defs.insert("EARLY_DEPTH".to_string(), naga_oil::compose::ShaderDefValue::Bool(true));
    }
    defs
}

pub fn build_composer() -> Composer {
    // naga_oil validates the composed module itself, defaulting to zero
    // capabilities — so it rejects our shaders' use of texture binding arrays
    // (terrain's bindless ground layers, indexed non-uniformly). Grant the
    // capabilities matching the device features the backend requests
    // (TEXTURE_BINDING_ARRAY + SAMPLED_TEXTURE_..._NON_UNIFORM_INDEXING). wgpu
    // still re-validates against the real device caps at pipeline creation.
    let mut capabilities = naga::valid::Capabilities::TEXTURE_AND_SAMPLER_BINDING_ARRAY
        | naga::valid::Capabilities::TEXTURE_AND_SAMPLER_BINDING_ARRAY_NON_UNIFORM_INDEXING;
    if early_depth_supported() {
        capabilities |= naga::valid::Capabilities::EARLY_DEPTH_TEST;
    }
    let mut composer = Composer::default().with_capabilities(capabilities);
    for (source, file_path) in [
        (include_str!("color.wgsl"), "color.wgsl"),
        (include_str!("boot_relief.wgsl"), "boot_relief.wgsl"),
        (
            include_str!("../water/fft_sampling.wgsl"),
            "water/fft_sampling.wgsl",
        ),
        (include_str!("../water/shore_surf.wgsl"), "water/shore_surf.wgsl"),
        (include_str!("../water/curling_breaker.wgsl"), "water/curling_breaker.wgsl"),
        // WRL-002: the one optical model shared by the water surface and the underwater
        // compositor (absorption / scattering / transmittance / in-scatter).
        (
            include_str!("../water/water_optics.wgsl"),
            "water/water_optics.wgsl",
        ),
        (include_str!("gbuffer.wgsl"), "gbuffer.wgsl"),
        (include_str!("layered_fog_optics.wgsl"), "layered_fog_optics.wgsl"),
        (include_str!("frame.wgsl"), "frame.wgsl"),
        (include_str!("rain_water_body.wgsl"), "rain_water_body.wgsl"),
        (include_str!("rain_water_optics.wgsl"), "rain_water_optics.wgsl"),
        (include_str!("terrain_material.wgsl"), "terrain_material.wgsl"),
        (include_str!("../rain_water_reflections.wgsl"), "rain_water_reflections.wgsl"),
        (include_str!("snow_material.wgsl"), "snow_material.wgsl"),
        (include_str!("ground_puddles.wgsl"), "ground_puddles.wgsl"),
        (include_str!("ground_wet.wgsl"), "ground_wet.wgsl"),
        (include_str!("rain_water_medium.wgsl"), "rain_water_medium.wgsl"),
        (include_str!("tree_snow.wgsl"), "tree_snow.wgsl"),
        (include_str!("skin.wgsl"), "skin.wgsl"),
        (include_str!("conform.wgsl"), "conform.wgsl"),
        (include_str!("lighting.wgsl"), "lighting.wgsl"),
        (include_str!("shadow.wgsl"), "shadow.wgsl"),
        // Shared object fragment shading, imported by both the per-draw (shader3d) and the
        // GPU-driven (gpu_driven) paths; depends on frame/shadow/lighting/color above.
        (include_str!("shading.wgsl"), "shading.wgsl"),
    ] {
        if let Err(e) = composer.add_composable_module(ComposableModuleDescriptor {
            source,
            file_path,
            language: ShaderLanguage::Wgsl,
            ..Default::default()
        }) {
            // Static shaders: a compose failure is a build-time bug, not a
            // runtime condition. emit_to_string points at the offending line.
            panic!(
                "shared shader module {file_path}: {}",
                e.emit_to_string(&composer)
            );
        }
    }
    composer
}

/// Compose an entry-point shader source (which may `#import` the shared modules)
/// into a wgpu shader module.
pub fn make_module(
    device: &wgpu::Device,
    composer: &mut Composer,
    label: &str,
    source: &str,
    file_path: &str,
) -> wgpu::ShaderModule {
    let module = match composer.make_naga_module(NagaModuleDescriptor {
        source,
        file_path,
        shader_defs: shader_defs(),
        ..Default::default()
    }) {
        Ok(module) => module,
        Err(e) => panic!(
            "compose {label} ({file_path}): {}",
            e.emit_to_string(composer)
        ),
    };
    device.create_shader_module(wgpu::ShaderModuleDescriptor {
        label: Some(label),
        source: wgpu::ShaderSource::Naga(Cow::Owned(module)),
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    // Compose every entry-point shader through the shared modules. This runs
    // naga_oil's full import resolution + naga validation without needing a GPU
    // device, so it catches broken imports, duplicate-binding rejections, and
    // capability gaps that would otherwise only surface at device init.
    fn compose(source: &str, file_path: &str) {
        let mut composer = build_composer();
        if let Err(e) = composer.make_naga_module(NagaModuleDescriptor {
            source,
            file_path,
            ..Default::default()
        }) {
            panic!("{file_path}: {}", e.emit_to_string(&composer));
        }
    }

    #[test]
    fn contact_shadow_orthographic_radius_is_cascade_invariant() {
        let slope = 0.266_f64.to_radians().tan();
        for separation in [0.0, 0.1, 3.0, 20.0, 80.0] {
            let expected = separation * slope;
            for depth_range in [64.0, 256.0, 1024.0] {
                for width in [32.0, 128.0, 512.0] {
                    let depth_delta = separation / depth_range;
                    let radius_uv = depth_delta / (1.0 / depth_range) * slope / width;
                    assert!((radius_uv * width - expected).abs() < 1e-10);
                }
            }
        }
        let source = include_str!("shadow.wgsl");
        let helper = source.split("fn contact_filter").nth(1).unwrap()
            .split("// Cascaded shadow strength").next().unwrap();
        assert!(!helper.contains("eye_depth"));
        assert!(!helper.contains("splits["));
        assert!(helper.contains("textureLoad(shadow_map"));
        assert!(helper.contains("point * length(point) * search_uv"));
        assert!(helper.contains("budget && lit > 0.0 && lit < f32(taps)"));
        // Refinement completes, rather than duplicates, the reference disk.
        let mut visited = [0; 16];
        for i in 0..8 { visited[i * 2] += 1; visited[i * 2 + 1] += 1; }
        assert!(visited.iter().all(|&count| count == 1));
        for i in 0..16 {
            let r = ((i as f64 + 0.5) / 16.0).sqrt();
            assert!(r * r > 0.0 && r * r < 1.0);
            assert!(r * r <= r);
        }
        assert!(!helper.contains("return plane_compare("));
        assert!(helper.contains("smoothstep(0.5, 1.5, radius_texels)"));
        // Continuous reconstruction-to-penumbra handoff, including both endpoints.
        let blend = |radius: f64| {
            let t = (radius - 0.5).clamp(0.0, 1.0);
            t * t * (3.0 - 2.0 * t)
        };
        assert_eq!(blend(0.0), 0.0);
        assert_eq!(blend(0.5), 0.0);
        assert_eq!(blend(1.5), 1.0);
        assert!(blend(0.50001) < 1e-8);
        assert!(1.0 - blend(1.49999) < 1e-8);
        assert!(!helper.contains("centre_separation"));
        assert!(helper.contains("min(separation / blockers, 100.0)"));
        // A finite occluder's umbra diminishes as its penumbra grows, without
        // an arbitrary distance multiplier on shadow strength. Infinite/wide
        // blockers must still be allowed to cast fully dark direct shadows.
        let blocked_fraction = |half_width: f64, gap: f64, angle: f64| {
            let radius = gap * angle.to_radians().tan();
            if radius == 0.0 { 1.0 } else { (half_width / radius).min(1.0) }
        };
        assert_eq!(blocked_fraction(0.05, 0.0, 1.0), 1.0);
        assert!(blocked_fraction(0.05, 12.0, 1.0) < blocked_fraction(0.05, 3.0, 1.0));
        assert_eq!(blocked_fraction(100.0, 12.0, 1.0), 1.0);
        for (source, file_path) in [
            (include_str!("../gfx3d/shader3d.wgsl"), "gfx3d/shader3d.wgsl"),
            (include_str!("../gfx3d/gpu_driven.wgsl"), "gfx3d/gpu_driven.wgsl"),
            (include_str!("../terrain/terrain.wgsl"), "terrain/terrain.wgsl"),
        ] {
            compose(source, file_path);
        }
    }

    #[test]
    fn native_authored_normals_compose_without_changing_legacy_canopy_gate() {
        for (source, file_path, gate) in [
            (include_str!("../gfx3d/shader3d.wgsl"), "gfx3d/shader3d.wgsl",
             "bend_canopy = bend_canopy && foliage_kind < 2.5;"),
            (include_str!("../gfx3d/gpu_driven.wgsl"), "gfx3d/gpu_driven.wgsl",
             "bend_canopy = bend_canopy && (inst.flags & INST_COHERENT_TREE_WIND) == 0u;"),
        ] {
            assert!(source.contains(gate));
            for enabled in [false, true] {
                let mut composer = build_composer();
                let mut defs = std::collections::HashMap::new();
                if enabled {
                    defs.insert("NATIVE_AUTHORED_NORMALS".to_string(),
                                naga_oil::compose::ShaderDefValue::Bool(true));
                }
                if let Err(e) = composer.make_naga_module(NagaModuleDescriptor {
                    source, file_path, shader_defs: defs, ..Default::default()
                }) {
                    panic!("{file_path}: {}", e.emit_to_string(&composer));
                }
            }
        }
    }

    #[test]
    fn legacy_zero_normals_reach_the_geometric_fallback_in_colour_and_prepass() {
        for shader in [include_str!("../gfx3d/shader3d.wgsl"), include_str!("../gfx3d/gpu_driven.wgsl")] {
            assert!(!shader.contains("normalize(in.normal)"),
                    "normalizing first destroys the zero-normal sentinel on legacy signs");
            assert!(shader.contains("let geometric_normal = surface_normal(in.normal"));
            assert_eq!(shader.matches("let surface_n = surface_normal(in.normal").count(), 2,
                       "both the 1x and A2C normal prepasses must use the same fallback");
        }
    }

    #[test]
    fn receiver_plane_filter_does_not_bias_through_thin_occluders() {
        let source = include_str!("shadow.wgsl");
        assert!(source.contains("textureLoad(shadow_map, coordinate, layer, 0)"));
        assert!(!source.contains("let plane_bias"));
        for slope in [0.0_f32, 0.001, 0.01] {
            for separation in [-0.0005_f32, 0.0005] {
                let mut lit = 0.0;
                for x in [-0.5_f32, 0.5] {
                    for y in [-0.5_f32, 0.5] {
                        let reference = 0.5 + slope * (x + y);
                        let depth = reference + separation;
                        lit += if reference <= depth { 0.25 } else { 0.0 };
                    }
                }
                assert_eq!(lit, if separation > 0.0 { 1.0 } else { 0.0 });
            }
        }
    }

    #[test]
    fn entry_shaders_compose() {
        compose(include_str!("../gfx3d/velocity_obj.wgsl"), "gfx3d/velocity_obj.wgsl");
        compose(
            include_str!("../gfx3d/shader3d.wgsl"),
            "gfx3d/shader3d.wgsl",
        );
        compose(
            include_str!("../gfx3d/shadow_depth.wgsl"),
            "gfx3d/shadow_depth.wgsl",
        );
        compose(
            include_str!("../gfx3d/skin_bake.wgsl"),
            "gfx3d/skin_bake.wgsl",
        );
        compose(
            include_str!("../gfx3d/gpu_driven.wgsl"),
            "gfx3d/gpu_driven.wgsl",
        );
        compose(
            include_str!("../gfx3d/gpu_driven_shadow.wgsl"),
            "gfx3d/gpu_driven_shadow.wgsl",
        );
        compose(
            include_str!("../gfx3d/cull_debug.wgsl"),
            "gfx3d/cull_debug.wgsl",
        );
        compose(
            include_str!("../terrain/terrain.wgsl"),
            "terrain/terrain.wgsl",
        );
        compose(
            include_str!("../terrain/terrain_shadow.wgsl"),
            "terrain/terrain_shadow.wgsl",
        );
        compose(include_str!("../far/far.wgsl"), "far/far.wgsl");
        compose(include_str!("../grass/grass.wgsl"), "grass/grass.wgsl");
        compose(
            include_str!("../grass/grass_shadow.wgsl"),
            "grass/grass_shadow.wgsl",
        );
        compose(include_str!("../water/water.wgsl"), "water/water.wgsl");
        compose(
            include_str!("../water/whitewater_render.wgsl"),
            "water/whitewater_render.wgsl",
        );
        compose(
            include_str!("../water/interaction.wgsl"),
            "water/interaction.wgsl",
        );
        compose(include_str!("../water/foam.wgsl"), "water/foam.wgsl");
        compose(
            include_str!("../water/fft_spectrum.wgsl"),
            "water/fft_spectrum.wgsl",
        );
        compose(include_str!("../water/fft_row.wgsl"), "water/fft_row.wgsl");
        compose(
            include_str!("../water/fft_compose.wgsl"),
            "water/fft_compose.wgsl",
        );
        compose(
            include_str!("../water/whitewater.wgsl"),
            "water/whitewater.wgsl",
        );
    }

    #[test]
    fn point_shadow_penumbra_attenuates_backface_ambient() {
        let shader = include_str!("lighting.wgsl");
        assert!(shader.contains("let occl = select(point_occl, cone, shadow_slot >= 0);"));
        assert!(shader.contains("var point_occl = 1.0;"));
        // Spotlight shaping remains independent; no cube means unchanged ambient.
        let visibility = |spot: bool, cone: f32, point: f32| if spot { cone } else { point };
        assert_eq!(visibility(false, 1.0, 1.0), 1.0);
        assert_eq!(visibility(false, 0.25, 0.25), 0.25);
        assert_eq!(visibility(false, 0.0, 0.0), 0.0);
        assert_eq!(visibility(true, 0.1, 1.0), 0.1);
    }

    #[test]
    fn native_wind_preserves_branch_attachment_across_mesh_density() {
        // A branch may attach inside a long trunk edge, rather than at its vertex.
        // Nonlinear per-vertex bending cannot commute with that interpolation.
        let old = |y: f64| (y.max(0.0) / 12.0).powf(1.6);
        assert!(((old(4.0) + old(20.0)) * 0.5 - old(12.0)).abs() > 0.2);
        for phase in [-2.0, -0.5, 0.0, 0.7, 2.0] {
            for (a, b) in [(-1.0, 24.0), (0.0, 12.0), (4.0, 20.0), (20.0, 48.0)] {
                for fraction in [0.0, 0.1, 0.5, 0.8, 1.0] {
                    let bend = |y: f64| y / 12.0 * phase;
                    let y = a + (b - a) * fraction;
                    let trunk = bend(a) + (bend(b) - bend(a)) * fraction;
                    assert!((trunk - bend(y)).abs() < 1e-12);
                }
            }
        }
        for source in [include_str!("frame.wgsl"),
                       include_str!("../gfx3d/velocity_obj.wgsl"),
                       include_str!("../gfx3d/gpu_driven.wgsl")] {
            assert!(source.contains("leaf < 0.0"));
            assert!(source.contains("if (leaf > 0.0)"));
        }
        assert!(include_str!("../gfx3d/shader3d.wgsl").contains("-1.0, foliage_kind > 2.5"));
        // Native stem/crown coherence shares one world-space phase anchor; the
        // gpu_driven call stays on the per-object phase until its sway behavior
        // is measured (deferred with the shared-path sway change).
        let frame = include_str!("frame.wgsl");
        assert!(frame.contains("fn veg_sway_phase(phase_xz: vec2<f32>, leaf: f32)"));
        assert!(frame.contains("veg_sway_phase(phase_xz, leaf)"));
        assert_eq!(include_str!("../gfx3d/gpu_driven.wgsl")
            .matches("-1.0, (inst.flags & INST_COHERENT_TREE_WIND) != 0u)").count(), 2);
    }

    #[test]
    fn road_and_terrain_share_snow_depth_evaluation() {
        fn body(source: &str) -> String {
            let start = source.find("fn snow_depth(").unwrap();
            source[start..].split("\n}").next().unwrap().replace('\r', "")
        }
        assert_eq!(body(include_str!("conform.wgsl")),
                   body(include_str!("../terrain/terrain.wgsl")));
        let shader = include_str!("../gfx3d/shader3d.wgsl");
        assert!(shader.contains("in.conform_w > 0.99 && snow_depth(in.world_pos.xz + frame.cam_pos.xz) >= 0.04"));
        // Shading computes derivatives before the conditional discard.
        let start = shader.find("fn fs_surface(").unwrap();
        let surface = &shader[start..shader[start..].find("fn surface_pixel_depth(").unwrap() + start];
        assert!(surface.find("out.color = shade_fragment(in, in.conform_w > 0.99);").unwrap() < surface.find("snow_depth(").unwrap());
    }

    #[test]
    fn retained_draws_do_not_consume_a_ninth_fragment_storage_slot() {
        let gfx = include_str!("../gfx3d/mod.rs");
        let start = gfx.find("label: Some(\"wgr_gpu_conform_layout\")").unwrap();
        let layout = gfx[start..].split("let gpu_bind =").next().unwrap();
        assert_eq!(layout.matches("BufferBindingType::Storage").count(), 1);
        assert!(layout.contains("visibility: wgpu::ShaderStages::VERTEX | wgpu::ShaderStages::COMPUTE"));
        let start = gfx.find("let gpu_velocity_pipeline = cull::build_gpu_velocity_pipeline(").unwrap();
        let pipelines = gfx[start..].split("let cull_debug_layout").next().unwrap();
        // The far weather-depth pipeline also uses the vertex/compute-only
        // conform layout; adding it must not expose storage to fragments.
        assert_eq!(pipelines.matches("&conform.gpu_layout").count(), 8);
        assert!(!pipelines.contains("&conform.layout"));
        assert_eq!(gfx.matches("self.conform.gpu_bind.as_ref()").count(), 4);
    }
}
