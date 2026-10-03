#define_import_path rain_water_body

// Body tint is a low reflectance, not already-lit radiance. On the physical
// sky path use the same irradiance/pi and sky scale as terrain; the legacy flat
// ambient is a separate lighting contract and can be tens of times brighter.
fn rain_water_body_radiance(physical_sky: f32, sky_irradiance: vec3<f32>, sky_scale: f32,
                            legacy_ambient: vec3<f32>, sun_diffuse: vec3<f32>,
                            sun_cosine: f32, sun_visibility: f32) -> vec3<f32> {
    // Only the direct component follows the actual shared shadow/cloud path.
    // Ambient and sky radiance remain available beneath a cast shadow.
    let direct = max(sun_diffuse,vec3<f32>(0.0))*clamp(sun_visibility,0.0,1.0);
    var illumination = max(legacy_ambient,vec3<f32>(0.0))
        + direct*0.2;
    if (physical_sky > 0.5) {
        illumination = max(sky_irradiance,vec3<f32>(0.0))*max(sky_scale,0.0)
            + direct*clamp(sun_cosine,0.0,1.0);
    }
    return vec3<f32>(0.045,0.060,0.052)*illumination;
}
