#import water_optics::{water_extinction, optics_srgb_to_linear}

struct Params {
    time_height_range_ext: vec4<f32>,
    camera_pos_layers: vec4<f32>,
    sun_dir_debug: vec4<f32>,
    sun_radiance: vec4<f32>,
    inv_view_proj: mat4x4<f32>,
    shallow_color: vec4<f32>,
    deep_color: vec4<f32>,
    cascade_lengths: vec4<f32>,
    water_controls: vec4<f32>,
    underwater_tuning: vec4<f32>,
    body_ellipse: vec4<f32>,
    body_frame: vec4<f32>,
};

struct TerrainShadowMap {
    origin: vec2<f32>,
    inv_span: vec2<f32>,
    half_texel: vec2<f32>,
    enabled: f32,
    sky_vis_strength: f32,
    sky_vis_floor: f32,
    sky_vis_debug: f32,
    sky_vis_contrast: f32,
    pad_c: f32,
};

struct CameraShadow {
    cascade_vp: array<mat4x4<f32>, 4>,
    splits: vec4<f32>,
    omni_radius: vec4<f32>,
    ctl: vec4<f32>,
    ctlb: vec4<f32>,
    cam_fwd: vec4<f32>,
    sun_dir: vec4<f32>,
};

@group(0) @binding(0) var<uniform> params: Params;
@group(0) @binding(1) var froxel_out: texture_storage_3d<rgba16float, write>;
@group(0) @binding(2) var shadow_mask: texture_2d<f32>;
@group(0) @binding(3) var shadow_samp: sampler;
@group(0) @binding(4) var<uniform> shadow_map: TerrainShadowMap;
@group(0) @binding(5) var csm_tex: texture_depth_2d_array;
@group(0) @binding(6) var csm_cmp: sampler_comparison;
@group(0) @binding(7) var<uniform> csm: CameraShadow;


fn water_path_length(ray_dir: vec3<f32>, target_distance: f32, cam_above: f32) -> f32 {
    if (cam_above >= 0.0) {
        if (ray_dir.y >= -1e-4) {
            return 0.0;
        }
        let entry_distance = cam_above / max(-ray_dir.y, 1e-4);
        return max(target_distance - entry_distance, 0.0);
    }
    if (ray_dir.y > 1e-4) {
        let exit_distance = -cam_above / ray_dir.y;
        return min(target_distance, max(exit_distance, 0.0));
    }
    return target_distance;
}

fn terrain_occlusion(world_xz: vec2<f32>, world_y: f32) -> f32 {
    if (shadow_map.enabled < 0.5) {
        return 0.0;
    }
    let uv = (world_xz - shadow_map.origin) * shadow_map.inv_span + shadow_map.half_texel;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
        return 0.0;
    }
    let sm = textureSampleLevel(shadow_mask, shadow_samp, uv, 0.0);
    let lit = smoothstep(sm.r - sm.g, sm.r + sm.g + 1e-3, world_y);
    return clamp(sm.b * (1.0 - lit), 0.0, 1.0);
}

fn csm_occlusion(pos: vec3<f32>) -> f32 {
    let n = i32(csm.ctl.x);
    if (n <= 0) {
        return 0.0;
    }
    let omni_n = i32(csm.ctl.y);
    let eye_depth = dot(pos, csm.cam_fwd.xyz);
    let dist3d = length(pos);
    var ci = n;
    for (var i = 0; i < 4; i = i + 1) {
        if (i >= n) {
            break;
        }
        let metric = select(eye_depth, dist3d, i < omni_n);
        if (metric <= csm.splits[i]) {
            ci = i;
            break;
        }
    }
    if (ci >= n) {
        return 0.0;
    }
    let cp = csm.cascade_vp[ci] * vec4<f32>(pos, 1.0);
    let sc = cp.xyz / cp.w;
    let suv = vec2<f32>(sc.x * 0.5 + 0.5, 0.5 - sc.y * 0.5);
    if (suv.x <= 0.0 || suv.x >= 1.0 || suv.y <= 0.0 || suv.y >= 1.0 ||
        sc.z <= 0.0 || sc.z >= 1.0) {
        return 0.0;
    }
    let bias = csm.ctl.w * f32(ci + 1) * f32(ci + 1);
    let lit = textureSampleCompareLevel(csm_tex, csm_cmp, suv, ci, sc.z - bias);
    let fade = clamp((csm.splits[n - 1] - eye_depth) / max(csm.ctl.z, 0.001), 0.0, 1.0);
    return (1.0 - lit) * fade;
}

fn hg_phase(cos_theta: f32, g: f32) -> f32 {
    let g2 = g * g;
    return (1.0 - g2) /
        max(12.5663706 * pow(max(1.0 + g2 - 2.0 * g * cos_theta, 1e-3), 1.5), 1e-4);
}

@compute @workgroup_size(8, 8, 1)
fn cs_underwater_froxel(@builtin(global_invocation_id) gid: vec3<u32>) {
    let dims = textureDimensions(froxel_out);
    if (gid.x >= dims.x || gid.y >= dims.y) {
        return;
    }

    let uv = (vec2<f32>(gid.xy) + 0.5) / vec2<f32>(dims.xy);
    let ndc = uv * vec2<f32>(2.0, -2.0) + vec2<f32>(-1.0, 1.0);
    let ray_h = params.inv_view_proj * vec4<f32>(ndc, 0.5, 1.0);
    let ray = normalize(ray_h.xyz / max(abs(ray_h.w), 1e-5) * sign(ray_h.w));
    let max_dist = params.time_height_range_ext.z;
    let cam_above = params.time_height_range_ext.y;
    let ext = max(params.time_height_range_ext.w, 1e-3);
    let shallow = optics_srgb_to_linear(clamp(params.shallow_color.xyz, vec3<f32>(1e-4), vec3<f32>(1.0)));
    let deep = optics_srgb_to_linear(clamp(params.deep_color.xyz, vec3<f32>(1e-4), vec3<f32>(1.0)));
    // WRL-002: the same water_extinction the compositor and the surface use, so the
    // in-scattering balances the extinction it sits under by construction.
    let bias = clamp(params.underwater_tuning.y, 0.0, 1.0);
    let extinction_rgb = water_extinction(params.deep_color.xyz, ext, bias) * max(params.underwater_tuning.x, 0.0);
    let shallow_peak = max(max(shallow.r, shallow.g), max(shallow.b, 1e-4));
    let deep_peak = max(max(deep.r, deep.g), max(deep.b, 1e-4));
    let shallow_unit = shallow / shallow_peak;
    let deep_unit = deep / deep_peak;
    let sun = normalize(params.sun_dir_debug.xyz);
    let phase = hg_phase(dot(ray, sun), 0.72);

    var lum = vec3<f32>(0.0);
    var trans = vec3<f32>(1.0);
    var previous_dist = 0.0;
    var previous_water_path = 0.0;
    var last_visibility = 1.0;
    // WRL-006: the in-scattering stops where the ray leaves the camera's body through its
    // side, the same ellipse-exit rule the compositor applies to extinction.
    var body_exit = 1.0e9;
    if (params.body_ellipse.z > 0.0 && params.body_ellipse.w > 0.0) {
        let e = params.body_ellipse;
        let fc = params.body_frame.x;
        let fs = params.body_frame.y;
        let dxz = vec2<f32>(params.camera_pos_layers.x - e.x, params.camera_pos_layers.z - e.y);
        let u0 = vec2<f32>((fc * dxz.x + fs * dxz.y) / e.z, (-fs * dxz.x + fc * dxz.y) / e.w);
        let d = vec2<f32>((fc * ray.x + fs * ray.z) / e.z, (-fs * ray.x + fc * ray.z) / e.w);
        let a = dot(d, d);
        let b = 2.0 * dot(u0, d);
        let c = dot(u0, u0) - 1.0;
        if (a > 1e-8) {
            let disc = b * b - 4.0 * a * c;
            if (disc >= 0.0) {
                body_exit = max((-b + sqrt(disc)) / (2.0 * a), 0.0);
            }
        }
    }
    for (var z = 0u; z < dims.z; z = z + 1u) {
        let w = (f32(z) + 0.5) / f32(dims.z);
        let target_dist = max_dist * w * w;
        let water_path = min(water_path_length(ray, target_dist, cam_above), body_exit);
        let segment_m = max(water_path - previous_water_path, 0.0);
        if (segment_m > 1e-4) {
            let midpoint = mix(previous_dist, target_dist, 0.5);
            let world_rel = ray * midpoint;
            let world_abs = params.camera_pos_layers.xyz + world_rel;
            let terrain_occ = terrain_occlusion(world_abs.xz, world_abs.y);
            let cascade_occ = csm_occlusion(world_rel);
            let visibility = 1.0 - max(terrain_occ, cascade_occ);
            last_visibility = visibility;

            let depth_below_surface = max(-(cam_above + world_rel.y), 0.0);
            let surface_transmission = exp(-depth_below_surface * 0.095);
            let depth_mix = smoothstep(8.0, 70.0, water_path + max(-cam_above, 0.0));
            let tint = mix(shallow_unit, deep_unit, depth_mix);
            let ambient_source = tint * mix(0.018, 0.032, clamp(ray.y * 0.5 + 0.5, 0.0, 1.0));
            let direct_source = params.sun_radiance.xyz * phase * visibility *
                surface_transmission * 0.075;
            let segment_trans = exp(-extinction_rgb * segment_m);
            let scatter_source = ambient_source + direct_source * tint;
            let integrated = scatter_source * (vec3<f32>(1.0) - segment_trans) /
                max(extinction_rgb, vec3<f32>(1e-3));
            lum = lum + trans * integrated;
            trans = trans * segment_trans;
        }
        textureStore(
            froxel_out,
            vec3<i32>(i32(gid.x), i32(gid.y), i32(z)),
            vec4<f32>(lum, last_visibility),
        );
        previous_dist = target_dist;
        previous_water_path = water_path;
    }
}
