#define_import_path rain_water_medium
#import rain_water_optics::rain_water_surface_optics_extinction
#import ground_wet::cultivated_soil_fraction

// Material admission is supplied from the actual source table. Multi-surface
// authored masks need exact coverage; any soft slot cannot promote the cell.
fn rain_water_soil_fraction(surface_count: u32, flags: u32, cell_uv: vec2<f32>) -> f32 {
    if (surface_count != 0u || (flags & 1u) == 0u) { return 0.0; }
    return max(f32((flags & 2u) >> 1u),cultivated_soil_fraction(flags,cell_uv));
}

// A bounded appearance profile over genuinely admitted soil. These are medium
// calibration constants, not simulated sediment concentration or erosion.
// Scalar 18/m extinction (5.56 cm e-fold) versus current clear-medium 3/m;
// low LINEAR body reflectance prevents luminous milk-like suspended material.
// Existing direct/ambient illumination is preserved through this reflectance
// ratio, including actual shadow/cloud/night controls.
fn rain_water_soil_body(clear_body: vec3<f32>, soil: f32) -> vec3<f32> {
    let ratio = vec3<f32>(0.035,0.018,0.007)/vec3<f32>(0.045,0.060,0.052);
    return clear_body*mix(vec3<f32>(1.0),ratio,clamp(soil,0.0,1.0));
}
fn rain_water_medium_optics(depth: f32, cosine: f32, reflected: vec3<f32>,
                            clear_body: vec3<f32>, coverage: f32, soil: f32) -> vec4<f32> {
    let share=clamp(soil,0.0,1.0);
    return rain_water_surface_optics_extinction(depth,cosine,reflected,
        rain_water_soil_body(clear_body,share),coverage,mix(3.0,18.0,share));
}
