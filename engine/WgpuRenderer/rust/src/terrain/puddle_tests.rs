use crate::ffi::{WgrTerrainMaterial, WgrTerrainParams};
use naga_oil::compose::NagaModuleDescriptor;

#[test]
fn mud_puddles_compose_with_the_real_terrain_bindings() {
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
fn mud_puddles_keep_existing_uniform_and_material_layouts() {
    assert_eq!(std::mem::size_of::<WgrTerrainParams>(), 88);
    assert_eq!(std::mem::offset_of!(WgrTerrainParams, _pad1), 72);
    assert_eq!(std::mem::size_of::<WgrTerrainMaterial>(), 384);
    assert_eq!(std::mem::offset_of!(WgrTerrainMaterial, puddle_flags), 380);
}

#[test]
fn mud_puddles_obey_source_mask_weather_slope_coast_snow_and_footprint() {
    let src = include_str!("terrain.wgsl");
    assert!(src.contains("source_mud_fraction(weights, material.puddle_flags)"));
    assert!(!src.contains("mud_weight / bound_weight"));
    assert!(src.contains("tp.rain_wetness > 0.0 && mud > 0.0"));
    assert!(src.contains("smoothstep(0.995, 0.9995, geometric_n.y)"));
    assert!(src.contains("smoothstep(tp.sea_level + 3.5, tp.sea_level + 4.5, world_y)"));
    assert!(src.contains("snow_cover(in.world_xz, world_y)"));
    assert!(src.contains("smoothstep(0.4, 0.9, footprint)"));
    let mask = src.split("fn mud_puddle_mask(").nth(1).unwrap().split("// Tangent-space").next().unwrap();
    assert!(!mask.contains("frame.") && !mask.contains("tp.time"));
    // No terrain/water geometry or additional texture allocation is introduced.
    assert!(!mask.contains("texture"));
}

#[test]
fn missing_rock_or_unknown_colour_does_not_promote_minor_source_mud() {
    // Independent admission oracle: painted SOURCE coverage is the denominator;
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
        assert!(expected < 0.65); // both fully bound and missing rock/unknown must reject puddles
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
    let admission = src.split("if (rain_puddle > 0.0 && frame.skyvis.x >= 0.5) {")
        .nth(1).expect("must gate raw roof query on map availability and actual puddle work")
        .split('}').next().unwrap();
    assert!(admission.contains("interior_sky_reach(in.world_pos + frame.cam_pos.xyz)"));
    assert!(admission.contains("smoothstep(0.6, 0.95, rain_sky_reach)"));
    assert!(!admission.contains("local_sky_visibility") && !admission.contains("frame.skyvis.z")
        && !admission.contains("frame.skyvis.w"));
    assert!(src.contains("let local_sky_visibility = interior_sky_ao(in.world_pos + frame.cam_pos.xyz, n)"),
        "existing ambient shading must retain its artistic AO controls");
}
