struct WaterParams { world_origin: vec2<f32>, terrain_grid: f32, sea_level: f32, hm_width: u32, hm_height: u32, time: f32, wave_amp: f32, wave_choppy: f32, wave_speed: f32, wave_scale: f32, fade_start: f32, fade_end: f32, warp_amp: f32, spec_power: f32, spec_intensity: f32, alpha: f32, shadow_dim: f32, color_ext: f32, coast_fade: f32, shallow_color: vec4<f32>, deep_color: vec4<f32>, foam_width: f32, foam_intensity: f32, swash_amp: f32, swash_speed: f32, fft_control: vec4<f32>, fft_wind_sea: vec4<f32>, fft_cascade_lengths: vec4<f32>, flow_direction_speed: vec4<f32> };
@group(0) @binding(0) var<uniform> water: WaterParams;
@group(0) @binding(1) var pack0: texture_2d_array<f32>;
@group(0) @binding(2) var pack1: texture_2d_array<f32>;
@group(0) @binding(3) var pack2: texture_2d_array<f32>;
@group(0) @binding(4) var displacement: texture_storage_2d_array<rgba16float, write>;
@group(0) @binding(5) var dynamics: texture_storage_2d_array<rgba16float, write>;
@group(0) @binding(6) var auxiliary: texture_storage_2d_array<rgba16float, write>;
struct CascadeConfig {
    enabled: u32, resolution: u32, tile_length_x: f32, tile_length_y: f32,
    displacement_scale: f32, horiz_displacement_scale: f32, normal_scale: f32, foam_scale: f32,
    wind_speed: f32, wind_direction_rad: f32, fetch_meters: f32, water_depth_meters: f32,
    swell: f32, directional_spread: f32, short_wave_detail: f32, whitecap_threshold: f32,
    spectrum_seed: u32, phase_offset_seconds: f32, update_rate_hz: f32, pad: f32,
};
struct CascadeConfigs { config: array<CascadeConfig, 4>, };
@group(0) @binding(7) var<uniform> cascades: CascadeConfigs;
fn wrap(v: i32, n: i32) -> i32 { return (v % n + n) % n; }
// spectrum_init stores frequencies with the zero-frequency component in the
// centre of the texture, exactly like GodotOceanWaves' spectrum_compute.glsl.
// Its fft_unpack applies (-1)^(x+y) after the inverse FFT to undo that
// centring.  Do the same before taking derivatives; omitting it produces a
// checkerboard-modulated displacement field that reads as faceted spikes.
fn centred_inverse_sign(c: vec2<i32>) -> f32 {
    let parity = (u32(c.x) + u32(c.y)) & 1u;
    return select(-1.0, 1.0, parity == 0u);
}
@compute @workgroup_size(8, 8, 1)
fn fft_compose(@builtin(global_invocation_id) id: vec3<u32>) {
    let dims = textureDimensions(displacement);
    if (id.x >= dims.x || id.y >= dims.y || id.z >= 4u) { return; }
    let c = vec2<i32>(id.xy);
    let layer = i32(id.z);
    let cfg = cascades.config[id.z];
    let sign_shift = centred_inverse_sign(c);
    let p0 = textureLoad(pack0, c, layer, 0) * sign_shift;
    let p1 = textureLoad(pack1, c, layer, 0) * sign_shift;
    let p2 = textureLoad(pack2, c, layer, 0) * sign_shift;
    let cell = water.fft_cascade_lengths[id.z] / f32(dims.x);
    let c_l = vec2<i32>(wrap(c.x - 1, i32(dims.x)), c.y);
    let c_r = vec2<i32>(wrap(c.x + 1, i32(dims.x)), c.y);
    let c_d = vec2<i32>(c.x, wrap(c.y - 1, i32(dims.y)));
    let c_u = vec2<i32>(c.x, wrap(c.y + 1, i32(dims.y)));
    let h_l = textureLoad(pack0, c_l, layer, 0).y * centred_inverse_sign(c_l);
    let h_r = textureLoad(pack0, c_r, layer, 0).y * centred_inverse_sign(c_r);
    let h_d = textureLoad(pack0, c_d, layer, 0).y * centred_inverse_sign(c_d);
    let h_u = textureLoad(pack0, c_u, layer, 0).y * centred_inverse_sign(c_u);
    // Auxiliary derivatives must describe the displacement actually written below.
    // Per-cascade amplitude/choppiness are consumer scales, absent from pack1.
    let vertical_scale = cfg.displacement_scale;
    let horizontal_scale = cfg.displacement_scale * water.wave_choppy;
    // Independent normal strength remains an artistic control.
    let slope_x = p0.w * cfg.normal_scale / (1.0 + abs(p1.y * horizontal_scale));
    let slope_z = p1.x * cfg.normal_scale / (1.0 + abs(p1.z * horizontal_scale));
    let curvature = -(h_l + h_r + h_d + h_u - 4.0 * p0.y) * vertical_scale / max(cell * cell, 0.001);
    let crest = clamp(max(p0.y * vertical_scale, 0.0) + max(curvature, 0.0) * 0.1, 0.0, 1.0);
    let d_dxdx = p1.y * horizontal_scale;
    let d_dxdz = p1.w * horizontal_scale;
    let d_dzdx = p1.w * horizontal_scale;
    let d_dzdz = p1.z * horizontal_scale;
    let jacobian = (1.0 + d_dxdx) * (1.0 + d_dzdz) - d_dxdz * d_dzdx;
    let compression = max(1.0 - jacobian, 0.0);
    let slope_variance = slope_x * slope_x + slope_z * slope_z;
    // displacement = (Dx, height, Dz, crest); dynamics.xy = height slope.
    // auxiliary = (signed horizontal Jacobian J, compression max(1-J,0),
    //              positive crest curvature, local height-slope magnitude squared).
    textureStore(displacement, c, layer, vec4<f32>(
        p0.x * horizontal_scale, p0.y * vertical_scale, p0.z * horizontal_scale, crest));
    textureStore(dynamics, c, layer, vec4<f32>(slope_x, slope_z, 0.0, 0.0));
    textureStore(auxiliary, c, layer, vec4<f32>(jacobian, compression, max(curvature, 0.0), slope_variance));
}
