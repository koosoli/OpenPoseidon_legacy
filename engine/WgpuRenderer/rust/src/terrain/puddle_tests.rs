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
    assert!(src.contains("mud_weight += weights[i]"));
    assert!(src.contains("mud_weight / bound_weight"));
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
