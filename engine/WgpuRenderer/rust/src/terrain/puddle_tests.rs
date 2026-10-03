use crate::ffi::{WgrTerrainMaterial, WgrTerrainParams};
use naga_oil::compose::NagaModuleDescriptor;

#[test]
fn ground_puddles_compose_with_the_real_terrain_bindings() {
    let mut composer = crate::shaders::build_composer();
    if let Err(error) = composer.make_naga_module(NagaModuleDescriptor {
        source: include_str!("terrain.wgsl"),
        file_path: "terrain/terrain.wgsl",
        ..Default::default()
    }) {
        panic!("{}", error.emit_to_string(&composer));
    }
}

#[test]
fn ground_puddles_keep_existing_uniform_and_material_layouts() {
    assert_eq!(std::mem::size_of::<WgrTerrainParams>(), 88);
    assert_eq!(std::mem::offset_of!(WgrTerrainParams, _pad1), 72);
    assert_eq!(std::mem::offset_of!(WgrTerrainParams, _pad2), 76);
    assert_eq!(std::mem::size_of::<WgrTerrainMaterial>(), 384);
    assert_eq!(std::mem::offset_of!(WgrTerrainMaterial, puddle_flags), 380);
}

#[test]
fn ground_puddles_ignore_source_language_but_obey_physical_admission() {
    let src = include_str!("terrain.wgsl");
    let film = src.split("var rain_puddle = 0.0;").nth(1).unwrap()
        .split("if (snow.info.w >").next().unwrap();
    assert!(film.contains("tp.rain_wetness > 0.0"));
    assert!(film.contains("ground_puddle_mask(in.world_xz, puddle_footprint, tp.rain_wetness)"));
    assert!(film.contains("ground_puddle_land_factor(geometric_n.y, world_y, tp.sea_level,"));
    assert!(film.contains("snow_cover(in.world_xz, world_y)"));
    assert!(!film.contains("mud") && !film.contains("puddle_flags"));
    assert!(src.contains("ground_puddle_ripple_normal(in.world_xz, tp.time, tp.rain_strength, puddle_footprint)"));
    let shared = include_str!("../shaders/ground_puddles.wgsl");
    let mask = shared.split("fn ground_puddle_mask(").nth(1).unwrap()
        .split("fn ground_puddle_land_factor").next().unwrap();
    assert!(mask.contains("smoothstep(0.4, 0.9, footprint)"));
    assert!(!mask.contains("frame.") && !mask.contains("time") && !mask.contains("texture"));
    let land = shared.split("fn ground_puddle_land_factor(").nth(1).unwrap()
        .split("fn ground_puddle_ripple_normal").next().unwrap();
    assert!(land.contains("smoothstep(0.995, 0.9995, normal_up)"));
    assert!(land.contains("smoothstep(sea_level + 0.15, sea_level + 0.5, height)"));
    assert!(land.contains("smoothstep(0.002, 0.02, snow_depth)"));
}

#[test]
fn missing_rock_or_unknown_colour_does_not_promote_minor_source_mud() {
    // Independent soft-soil oracle: painted SOURCE coverage is the denominator;
    // resident image coverage belongs only to the existing color composition.
    let source_weights = [0.8_f32, 0.2, 0.0, 0.0, 0.0, 0.0];
    let mud_slots = [false, true, false, false, false, false];
    let expected = source_weights.iter().zip(mud_slots).filter(|(_, mud)| *mud)
        .map(|(weight, _)| weight).sum::<f32>() / source_weights.iter().sum::<f32>();
    assert!((expected - 0.2).abs() < 1e-6);
    for bound in [[true, true, false, false, false, false],
                  [false, true, false, false, false, false]] {
        let color_denominator = source_weights.iter().zip(bound).filter(|(_, bound)| *bound)
            .map(|(weight, _)| weight).sum::<f32>();
        assert!(color_denominator > 0.0); // normal color fallback is still available
        assert!(expected < 0.65); // neither state may promote this mixture to predominantly soft soil
        if !bound[0] {
            assert!((source_weights[1] / color_denominator - 1.0).abs() < 1e-6,
                    "fixture must expose the old residency-dependent normalization bug");
        }
    }
    // Tie the oracle to the real shader helper's complete source denominator,
    // independently of surface_count, bound texture indices or color denominator.
    let src = include_str!("terrain.wgsl");
    let helper = src.split("fn source_mud_fraction(").nth(1).unwrap().split("// `world_pos`").next().unwrap();
    assert!(helper.contains("i < TERRAIN_SURFACE_SLOTS"));
    assert!(helper.contains("selected += weights[i]"));
    assert!(helper.contains("mud / max(selected, 1e-6)"));
    assert!(!helper.contains("bound_weight") && !helper.contains("surface_count") && !helper.contains("texture"));
    assert!(src.contains("surface = surface / bound_weight")); // existing colour normalization retained
}

#[test]
fn puddle_roof_admission_uses_raw_geometry_not_artistic_ambient_controls() {
    // With strength zero or ambient floor one an enclosed roof is lit fully;
    // that artistic ambient choice must not make its raw 5% sky reach rainy.
    let raw_reach = 0.05_f32;
    for (strength, floor) in [(0.0_f32, 0.0_f32), (1.0, 1.0), (0.2, 0.9)] {
        let ambient_ao = (1.0 - strength * (1.0 - raw_reach)).max(floor);
        assert!(ambient_ao >= 0.9);
        assert!(raw_reach < 0.6);
    }
    let src = include_str!("terrain.wgsl");
    let admission = src.split("if (rain_puddle > 0.0 || soil_wet > 0.0) {")
        .nth(1).expect("must gate raw roof query on actual puddle work")
        .split('}').next().unwrap();
    assert!(admission.contains("interior_rain_reach(in.world_pos + frame.cam_pos.xyz)"));
    assert!(admission.contains("let rain_coverage = interior_rain_coverage("));
    assert!(admission.contains("ground_puddle_rain_factor(rain_reach, rain_coverage)"));
    assert!(admission.contains("interior_rain_coverage(in.world_pos + frame.cam_pos.xyz)"));
    assert!(admission.contains("rain_puddle *= rain_proof") && admission.contains("soil_wet *= rain_proof"));
    assert!(!admission.contains("interior_sky_reach("));
    assert!(!admission.contains("local_sky_visibility") && !admission.contains("frame.skyvis.z")
        && !admission.contains("frame.skyvis.w"));
    assert!(src.contains("let local_sky_visibility = interior_sky_ao(in.world_pos + frame.cam_pos.xyz, n)"),
        "existing ambient shading must retain its artistic AO controls");
}
