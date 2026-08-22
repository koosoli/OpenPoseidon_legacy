// Volumetric sun shafts (crepuscular rays / god rays) — MARCH pass.
//
// This is a WORLD-SPACE march, not a screen-space radial blur, and that choice is the whole
// design. A radial blur streaks whatever is bright around the sun's screen position outward; it
// therefore (a) has nothing to say when the sun is off-screen except to invent a halo at the
// frame edge, and (b) cannot be occluded by anything the camera cannot see. Marching the view ray
// and asking "is the sun visible from HERE" at each step costs about the same at low resolution
// and answers both: the sun's screen position never appears in this shader at all, so there is no
// halo to fix, and the occluders are the ones the world actually has.
//
// Three occluders, all of them maps that already exist and are rebuilt every frame:
//   * CLOUD — the CLD-020 sun-transmittance map (sky/mod.rs, cs_cloud_shadow). A world-anchored
//     2D map of "how much sun reaches this ground point through the deck". This is what makes the
//     shafts respond to the clouds: when a bank crosses the sun the map darkens along the bank's
//     shape and the shafts take that shape.
//   * TERRAIN — the long-range terrain shadow-ceiling mask (the same one frame::terrain_sun_shadow
//     and cs_froxel read), so ridges cast shafts kilometres long.
//   * OBJECTS — the cascade shadow map, so buildings and tree crowns carve the near field, which
//     the smooth terrain ceiling cannot resolve.
//
// The structs and the two occlusion helpers are DUPLICATED from sky.wgsl rather than imported, for
// the reason stated there: these are standalone-validated shaders, and sky/ is a separate module.
// If FroxelShadow / FroxelCsm / TerrainShadowMap / WgrCameraShadow change, change them here too.

struct GodRays {
    // Camera-relative (translation-free) inverse view-projection, exactly as the sky pass uses:
    // forward NDC z = 0 is the near plane, z = 1 - stored_reversed_z reconstructs a depth sample.
    inv_view_proj: mat4x4<f32>,
    // xyz = unit direction TO the sun. w = elevation fade [0,1], computed on the CPU: 0 once the
    // sun is below the horizon, so a night frame costs one early-out per pixel and adds nothing.
    sun: vec4<f32>,
    // rgb = scene-referred sun radiance already reddened by the sun's own air mass (CPU side, from
    // the same Rayleigh coefficients the sky uses), so a low sun throws orange shafts, not white.
    sun_color: vec4<f32>,
    // xyz = ABSOLUTE world camera position. The march is camera-relative; the occlusion maps are
    // world-anchored, so every lookup adds this back.
    // w = AEROSOL SCALE HEIGHT (m), the engine's own mie.z (~1200). Aerosol is concentrated in the
    // lowest kilometre or so — an order of magnitude below the molecular 8 km — which is why shafts
    // thin out as the camera climbs, and why this is a per-sample profile and not a constant.
    cam_pos: vec4<f32>,
    // x = intensity, y = SEA-LEVEL aerosol scattering coefficient (1/m) derived on the CPU from the
    // atmosphere's own state (see aerosol_sigma in godrays.rs), z = max march distance (m),
    // w = step count.
    tune0: vec4<f32>,
    // x = Henyey-Greenstein g, y = cloud influence [0,1], z = terrain-occlusion strength,
    // w = cascade-occlusion strength.
    tune1: vec4<f32>,
    // CLD-020 cloud map: xy = the map's snapped world-xz min corner, z = 1/span (m), w = strength.
    cloud_map: vec4<f32>,
    // xy = low-res target size in texels, zw = 1/size. Only the composite reads this.
    lo_res: vec4<f32>,
    // x = SMOKE influence [0,1] on the shafts. Its own lane rather than a tune1 slot
    // because smoke is a separate occluder from the cloud deck: it must survive cloud
    // shadows being switched off. yzw reserved.
    tune2: vec4<f32>,
};

// Mirrors the first 48 bytes of terrain::TerrainShadowMap (the buffer is larger; WGSL only needs
// the prefix it reads). Same declaration as sky.wgsl's FroxelShadow.
struct TerrainShadow {
    origin: vec2<f32>,     // world xz of the mask's (0,0)
    inv_span: vec2<f32>,   // world-xz -> [0,1] over the map
    half_texel: vec2<f32>, // 0.5 / mask_dims
    enabled: f32,          // 0 until a heightmap is loaded
    pad: f32,
};

// Mirrors ffi::WgrCameraShadow. Same declaration as sky.wgsl's FroxelCsm.
struct GodRayCsm {
    cascade_vp: array<mat4x4<f32>, 4>,
    splits: vec4<f32>,      // frustum tiers: far eye-depth per tier
    omni_radius: vec4<f32>, // omni tiers: camera-distance radius
    ctl: vec4<f32>,         // {count, omni_count, fade_range, bias_const}
    ctlb: vec4<f32>,        // {texel_size, darkness, normal_offset_scale, pcf}
    cam_fwd: vec4<f32>,     // xyz = camera forward (eye-depth cascade select)
    sun_dir: vec4<f32>,
};

@group(0) @binding(0) var<uniform> gr: GodRays;
@group(0) @binding(1) var scene_depth: texture_depth_2d;
@group(0) @binding(2) var cloud_shadow_tex: texture_2d<f32>;
@group(0) @binding(3) var lin_samp: sampler;
@group(0) @binding(4) var shadow_mask: texture_2d<f32>;
@group(0) @binding(5) var<uniform> shadow_map: TerrainShadow;
@group(0) @binding(6) var csm_tex: texture_depth_2d_array;
@group(0) @binding(7) var csm_cmp: sampler_comparison;
@group(0) @binding(8) var<uniform> csm: GodRayCsm;

// Henyey-Greenstein, normalised over the sphere (integrates to 1), so multiplying it by the sun
// radiance and the marched in-scatter fraction is the physical single-scatter estimate rather than
// an arbitrary "add some white". Forward-peaked g is also what fades the effect out when the sun
// is behind the camera: at g = 0.6 the back lobe is ~2% of the forward one.
fn hg_phase(cos_t: f32, g: f32) -> f32 {
    let g2 = g * g;
    let denom = 1.0 + g2 - 2.0 * g * cos_t;
    return (1.0 - g2) / (12.566370614 * pow(max(denom, 1.0e-4), 1.5));
}

// Interleaved gradient noise. Used to offset each ray's sample inside its segment so the
// undersampled far segments turn into fine noise (which the depth-aware upsample then averages)
// instead of the concentric banding a fixed step start produces. Deliberately NOT animated: there
// is no TAA behind this pass, so a per-frame offset would read as a crawling grain.
fn ign(p: vec2<f32>) -> f32 {
    return fract(52.9829189 * fract(dot(p, vec2<f32>(0.06711056, 0.00583715))));
}

// Occlusion [0,1] of the sun by terrain at an ABSOLUTE world position (0 = lit, 1 = fully
// shadowed). Mirror of frame::terrain_sun_shadow / sky::terrain_occlusion. Off-map reads LIT:
// absence of data must never invent shadow.
fn terrain_occlusion(world_xz: vec2<f32>, world_y: f32) -> f32 {
    if (shadow_map.enabled < 0.5) {
        return 0.0;
    }
    let uv = (world_xz - shadow_map.origin) * shadow_map.inv_span + shadow_map.half_texel;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
        return 0.0;
    }
    let sm = textureSampleLevel(shadow_mask, lin_samp, uv, 0.0);
    let lit = smoothstep(sm.r - sm.g, sm.r + sm.g + 1.0e-3, world_y);
    return clamp(sm.b * (1.0 - lit), 0.0, 1.0);
}

// Occlusion [0,1] of the sun by the cascade shadow map at a CAMERA-RELATIVE position. Single
// compare tap, constant bias, no PCF — the volume is soft and dithered, so the extra taps would
// buy nothing. Mirror of sky::csm_occlusion.
fn csm_occlusion(pos: vec3<f32>) -> f32 {
    let n = i32(csm.ctl.x);
    if (n <= 0) {
        return 0.0;
    }
    let omni_n = i32(csm.ctl.y);
    let eye_depth = dot(pos, csm.cam_fwd.xyz);
    let dist3d = length(pos);
    var ci = n;
    for (var i = 0; i < 4; i++) {
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
    if (suv.x <= 0.0 || suv.x >= 1.0 || suv.y <= 0.0 || suv.y >= 1.0 || sc.z <= 0.0 || sc.z >= 1.0) {
        return 0.0;
    }
    let bias = csm.ctl.w * f32(ci + 1) * f32(ci + 1);
    let lit = textureSampleCompareLevel(csm_tex, csm_cmp, suv, ci, sc.z - bias);
    let fade = clamp((csm.splits[n - 1] - eye_depth) / max(csm.ctl.z, 0.001), 0.0, 1.0);
    return (1.0 - lit) * fade;
}

// Fraction of the map's half-extent at which a lookup starts easing back to fully lit. Kept
// numerically equal to frame.wgsl's CLOUD_SHADOW_EDGE_FADE; a separate name because these two
// shaders are separate naga_oil modules and a shared const would have to be plumbed as an import.
const CLOUD_MAP_EDGE_FADE: f32 = 0.85;

// Sun visibility [0,1] through the CLOUD DECK for a world-space sample.
//
// The CLD-020 map stores transmittance for a ray leaving SEA LEVEL toward the sun. A sample at
// altitude h has its own ray, but that ray pierces the deck at the same place as the sea-level ray
// launched from h's projection down the sun direction — so shift the lookup by that parallax
// rather than reading straight down. At eye height the shift is metres; from an aircraft it is
// hundreds, and reading straight down there would paste the shadow pattern onto the wrong ground.
//
// The stored value has the ground-shadow STRENGTH slider already folded in as
// mix(1, T, strength). Undo it, so dialling ground cloud shadows down does not silently flatten
// the shafts too — the two are different questions. The inversion amplifies the map's 8-bit
// quantisation by 1/strength, so below 0.25 it is not worth doing and the stored value is used
// directly (at strength 0 the map is uniformly lit and there is no cloud shape to recover at all;
// see the note in godrays.rs).
fn cloud_visibility(world: vec3<f32>, sun: vec3<f32>) -> f32 {
    let strength = clamp(gr.cloud_map.w, 0.0, 1.0);
    let influence = clamp(gr.tune1.y, 0.0, 1.0);
    let smoke_influence = clamp(gr.tune2.x, 0.0, 1.0);
    // NOTE the smoke term below is deliberately NOT gated on the cloud strength or
    // influence: smoke is a separate occluder that happens to ride the same map, and a
    // plume must throw shafts on a clear day with cloud shadows dialled to zero.
    if ((strength <= 0.001 || influence <= 0.001) && smoke_influence <= 0.001) {
        return 1.0;
    }
    let drop = world.y / max(sun.y, 0.05);
    let ground_xz = world.xz - sun.xz * drop;
    let uv = (ground_xz - gr.cloud_map.xy) * gr.cloud_map.z;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
        return 1.0;
    }
    let texel = textureSampleLevel(cloud_shadow_tex, lin_samp, uv, 0.0);

    // -- cloud (.r): undo the ground-shadow strength, as before ------------------
    let stored = clamp(texel.r, 0.0, 1.0);
    var transmit = stored;
    if (strength > 0.25) {
        transmit = clamp(1.0 - (1.0 - stored) / strength, 0.0, 1.0);
    }
    var vis = mix(1.0, transmit, influence);
    if (strength <= 0.001 || influence <= 0.001) {
        vis = 1.0;
    }

    // -- smoke (.g): its own occluder, its own influence -------------------------
    // Stored with no strength lerp folded in, so there is nothing to undo. A plume
    // between this sample and the sun darkens the shaft here, which is what makes a
    // burning wreck throw real god rays through its own column.
    if (smoke_influence > 0.001) {
        let smoke = clamp(texel.g, 0.0, 1.0);
        vis = vis * mix(1.0, smoke, smoke_influence);
    }
    // Ease to fully lit at the map's boundary, matching frame.wgsl's cloud_sun_shadow so the
    // shafts and the ground under them stop at the same place and in the same way. This lookup
    // leaves the map SOONER than the ground one does -- the parallax shift above is hundreds of
    // metres for a high sample under a low sun -- so without the fade the shafts carried a hard
    // edge of their own even when the ground's boundary was safely past the far plane.
    //
    // Applied to the COMBINED visibility, cloud and smoke together, rather than to the cloud term
    // alone: both are channels of the same texel of the same map, so both leave it at the same
    // boundary and both must ease out there.
    let edge = max(abs(uv.x - 0.5), abs(uv.y - 0.5)) * 2.0;
    let keep = 1.0 - smoothstep(CLOUD_MAP_EDGE_FADE, 1.0, edge);
    return mix(1.0, vis, keep);
}

struct VsOut {
    @builtin(position) clip: vec4<f32>,
    @location(0) uv: vec2<f32>,
};

@vertex
fn vs_main(@builtin(vertex_index) vi: u32) -> VsOut {
    let uv = vec2<f32>(f32((vi << 1u) & 2u), f32(vi & 2u));
    var out: VsOut;
    out.uv = uv;
    out.clip = vec4<f32>(uv * vec2<f32>(2.0, -2.0) + vec2<f32>(-1.0, 1.0), 0.0, 1.0);
    return out;
}

@fragment
fn fs_march(in: VsOut) -> @location(0) vec4<f32> {
    // Point-sample the FULL-RES depth under this low-res pixel's centre, and carry it in alpha:
    // the composite needs it to decide which low-res neighbours belong to the same surface.
    let full_dims = vec2<f32>(textureDimensions(scene_depth));
    let texel = vec2<i32>(clamp(in.uv * full_dims, vec2<f32>(0.0), full_dims - vec2<f32>(1.0)));
    let depth = textureLoad(scene_depth, texel, 0);

    let horizon = clamp(gr.sun.w, 0.0, 1.0);
    if (horizon <= 0.001 || gr.tune0.x <= 0.0) {
        return vec4<f32>(0.0, 0.0, 0.0, depth);
    }

    let ndc = vec2<f32>(in.uv.x * 2.0 - 1.0, 1.0 - in.uv.y * 2.0);
    let near_h = gr.inv_view_proj * vec4<f32>(ndc, 0.0, 1.0);
    let ray = normalize(near_h.xyz / near_h.w);

    // How far to march: to the first opaque surface, or to the cap for a sky pixel (depth ~ 0 is
    // the cleared reversed-Z far plane). Without this the shafts would be painted in front of a
    // near wall as though the air behind it were still in view.
    let max_dist = max(gr.tune0.z, 1.0);
    var end = max_dist;
    if (depth > 1.0e-6) {
        let hit = gr.inv_view_proj * vec4<f32>(ndc, 1.0 - depth, 1.0);
        end = min(length(hit.xyz / hit.w), max_dist);
    }
    if (end <= 1.0) {
        return vec4<f32>(0.0, 0.0, 0.0, depth);
    }

    let sun = normalize(gr.sun.xyz);
    let sigma_sea = max(gr.tune0.y, 0.0);
    let aerosol_h = max(gr.cam_pos.w, 1.0);
    let steps = i32(clamp(gr.tune0.w, 4.0, 48.0));
    let phase = hg_phase(dot(ray, sun), clamp(gr.tune1.x, 0.0, 0.95));
    let jitter = ign(in.clip.xy);

    // Front-to-back with a SQUARED step distribution (the same one the aerial froxel uses): the
    // near field gets metres-long steps where a shaft edge occupies many pixels, the far field
    // gets hundreds, where it occupies few. A uniform march would spend most of its samples where
    // they cannot be seen.
    var inscatter = 0.0;
    var transmit = 1.0;
    var t_prev = 0.0;
    for (var i = 0; i < steps; i = i + 1) {
        let w = (f32(i) + 1.0) / f32(steps);
        let t_next = end * w * w;
        let dt = t_next - t_prev;
        if (dt > 0.0) {
            let t = t_prev + dt * jitter;
            let rel = ray * t;
            let world = gr.cam_pos.xyz + rel;
            let cloud = cloud_visibility(world, sun);
            let occ_terrain = terrain_occlusion(world.xz, world.y) * gr.tune1.z;
            let occ_object = csm_occlusion(rel) * gr.tune1.w;
            let vis = cloud * clamp(1.0 - max(occ_terrain, occ_object), 0.0, 1.0);
            // The scatterers at THIS sample. Aerosol density falls off exponentially with height,
            // so a shaft is dense in the valley haze and thin over a ridge, and a camera at 3 km is
            // largely above the medium that draws shafts at all. Clamped at sea level rather than
            // extrapolated below it: there is no air denser than sea-level air to march through,
            // and letting the exponent run positive would blow up on a steep downward ray.
            let altitude = max(world.y, 0.0);
            let sigma = sigma_sea * exp(-altitude / aerosol_h);
            // Analytic segment integral: the fraction of THIS segment's extinction that scatters
            // toward the eye, attenuated by everything already crossed. Summed over a fully lit
            // infinite path this converges to 1, which is what keeps the result energy-bounded
            // instead of growing with the step count.
            let seg = exp(-sigma * dt);
            inscatter = inscatter + transmit * vis * (1.0 - seg);
            transmit = transmit * seg;
        }
        t_prev = t_next;
    }

    let rgb = gr.sun_color.rgb * (inscatter * phase * gr.tune0.x * horizon);
    return vec4<f32>(rgb, depth);
}
