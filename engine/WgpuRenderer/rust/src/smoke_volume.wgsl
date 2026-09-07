// SMK-038: the volumetric smoke field, and the march that turns it into pixels.
//
// FIVE ENTRY POINTS, in the order the frame runs them:
//   cs_clear   -- zero the atomic density accumulator
//   cs_inject  -- SCATTER the frame's smoke particles into it, one workgroup per particle
//   cs_resolve -- accumulator -> a filterable r16float 3D texture
//   fs_march   -- one ray per (low-res) pixel: front-to-back integration, stopping at the
//                 scene depth, with a secondary march toward the sun per sample
//   fs_composite -- nearest-depth upsample of the march over the lit scene
//
// WHY SCATTER AND NOT GATHER. The obvious injection is per-froxel: for each of the
// 128x64x128 = 1,048,576 cells, sum the particles covering it. With the 512-2048 particles
// a burning village produces that is half a billion sphere tests per frame, and it scales
// with the PRODUCT of the two counts. Scattering costs one workgroup per particle and
// touches only the cells that particle actually covers -- a few thousand -- so it scales
// with the particles alone. The price is that the accumulator has to be an atomic buffer
// (WGSL storage textures have no atomics), hence cs_clear and cs_resolve either side of it.

struct Params {
    // xyz = world position of the grid's MIN corner, snapped to the cell size so the field
    // does not crawl as the camera moves; w = cell size in metres.
    origin_cell: vec4<f32>,
    // xyz = grid dimensions in cells; w = march steps along the view ray.
    dims_steps: vec4<f32>,
    // xyz = camera world position; w = scattering albedo.
    cam_albedo: vec4<f32>,
    // xyz = direction TOWARD the sun; w = sun-march step count.
    sun_dir_steps: vec4<f32>,
    // rgb = sun radiance; a = sun-march step length in metres.
    sun_radiance: vec4<f32>,
    // rgb = ambient radiance the smoke picks up from the sky; a = extinction scale.
    ambient_ext: vec4<f32>,
    // x = density scale, y = Henyey-Greenstein g, z = per-ray jitter amount,
    // w = self-shadow strength (0 = flat, 1 = the full sun march).
    tune: vec4<f32>,
    // xy = march target size in pixels, zw = full render target size in pixels.
    sizes: vec4<f32>,
    // Camera-relative (translation-free) inverse view-projection.
    inv_view_proj: mat4x4<f32>,
    // x = particle count this frame; yzw spare.
    counts: vec4<u32>,
};

@group(0) @binding(0) var<uniform> P: Params;
// Fixed-point density accumulator, one u32 per cell. See DENSITY_FIXED below.
@group(0) @binding(1) var<storage, read_write> accum: array<atomic<u32>>;
// The frame's particles, two vec4 each: (xyz world, w radius m) and (x optical density,
// y height above ground, zw unused) -- the layout WgrSmokeShadowBlob already uses.
@group(0) @binding(2) var<storage, read> blobs: array<vec4<f32>>;
// Rgba16Float, not the r16float the density alone would need: rgba16float is the widest
// format that is BOTH storage-writable and filterable in core WebGPU, and the march samples
// this trilinearly. R32Float is storage-writable but not filterable without an optional
// feature, and r16float is not a core storage format at all.
@group(0) @binding(3) var field_out: texture_storage_3d<rgba16float, write>;

// The accumulator is integer, so density is carried as fixed point. 1024 steps per unit of
// extinction is far finer than anything visible and leaves room for ~4 million units of
// overlap before a u32 could wrap, which a 2048-particle plume cannot reach.
const DENSITY_FIXED: f32 = 1024.0;

fn cell_index(c: vec3<i32>) -> u32 {
    let d = vec3<i32>(P.dims_steps.xyz);
    return u32(c.x) + u32(c.y) * u32(d.x) + u32(c.z) * u32(d.x) * u32(d.y);
}

@compute @workgroup_size(64)
fn cs_clear(@builtin(global_invocation_id) gid: vec3<u32>) {
    let d = vec3<u32>(P.dims_steps.xyz);
    let total = d.x * d.y * d.z;
    if (gid.x >= total) {
        return;
    }
    atomicStore(&accum[gid.x], 0u);
}

// One workgroup per particle; its 64 threads stride the particle's cell bounding box 4 at a
// time on each axis.
@compute @workgroup_size(4, 4, 4)
fn cs_inject(
    @builtin(workgroup_id) wid: vec3<u32>,
    @builtin(local_invocation_id) lid: vec3<u32>,
) {
    if (wid.x >= P.counts.x) {
        return;
    }
    let pr = blobs[wid.x * 2u];
    let dn = blobs[wid.x * 2u + 1u];
    let centre = pr.xyz;
    let radius = max(pr.w, 0.05);
    // Optical density PER METRE. The particle carries the alpha it is being drawn with,
    // which is a whole-puff opacity, not an extinction coefficient -- dividing by the radius
    // is what stops a 20 m artillery puff being twenty times as opaque as a 1 m one for the
    // same authored alpha.
    let sigma = dn.x / radius * P.tune.x;
    if (sigma <= 0.0) {
        return;
    }

    let cell = P.origin_cell.w;
    let inv_cell = 1.0 / cell;
    let dims = vec3<i32>(P.dims_steps.xyz);
    let lo = vec3<i32>(floor((centre - vec3<f32>(radius) - P.origin_cell.xyz) * inv_cell));
    let hi = vec3<i32>(ceil((centre + vec3<f32>(radius) - P.origin_cell.xyz) * inv_cell));
    let lo_c = max(lo, vec3<i32>(0, 0, 0));
    let hi_c = min(hi, dims - vec3<i32>(1, 1, 1));

    var z: i32 = lo_c.z + i32(lid.z);
    loop {
        if (z > hi_c.z) { break; }
        var y: i32 = lo_c.y + i32(lid.y);
        loop {
            if (y > hi_c.y) { break; }
            var x: i32 = lo_c.x + i32(lid.x);
            loop {
                if (x > hi_c.x) { break; }
                let world = P.origin_cell.xyz + (vec3<f32>(f32(x), f32(y), f32(z)) + 0.5) * cell;
                let t = length(world - centre) / radius;
                if (t < 1.0) {
                    // Smooth, compactly supported kernel: 1 at the centre, 0 with zero slope
                    // at the rim, so a puff does not show its own bounding sphere as an edge.
                    let f = 1.0 - t * t;
                    let w = f * f;
                    atomicAdd(&accum[cell_index(vec3<i32>(x, y, z))], u32(sigma * w * DENSITY_FIXED));
                }
                x = x + 4;
            }
            y = y + 4;
        }
        z = z + 4;
    }
}

@compute @workgroup_size(4, 4, 4)
fn cs_resolve(@builtin(global_invocation_id) gid: vec3<u32>) {
    let d = vec3<u32>(P.dims_steps.xyz);
    if (gid.x >= d.x || gid.y >= d.y || gid.z >= d.z) {
        return;
    }
    let v = f32(atomicLoad(&accum[cell_index(vec3<i32>(gid))])) / DENSITY_FIXED;
    textureStore(field_out, vec3<i32>(gid), vec4<f32>(v, 0.0, 0.0, 0.0));
}

// ---------------------------------------------------------------- march + composite

@group(1) @binding(0) var field: texture_3d<f32>;
@group(1) @binding(1) var field_samp: sampler;
@group(1) @binding(2) var scene_depth: texture_depth_2d;
@group(1) @binding(3) var march_result: texture_2d<f32>;

fn fullscreen(vi: u32) -> vec4<f32> {
    let x = f32((vi << 1u) & 2u) * 2.0 - 1.0;
    let y = f32(vi & 2u) * 2.0 - 1.0;
    return vec4<f32>(x, y, 0.0, 1.0);
}

@vertex
fn vs_fullscreen(@builtin(vertex_index) vi: u32) -> @builtin(position) vec4<f32> {
    return fullscreen(vi);
}

// Camera-relative world position of the surface at this NDC and reversed-Z depth.
fn unproject(ndc: vec2<f32>, depth_rev: f32) -> vec3<f32> {
    let p = P.inv_view_proj * vec4<f32>(ndc, 1.0 - depth_rev, 1.0);
    return p.xyz / p.w;
}

fn density_at(world: vec3<f32>) -> f32 {
    let dims = P.dims_steps.xyz;
    let uvw = (world - P.origin_cell.xyz) / (dims * P.origin_cell.w);
    if (uvw.x < 0.0 || uvw.x > 1.0 || uvw.y < 0.0 || uvw.y > 1.0 || uvw.z < 0.0 || uvw.z > 1.0) {
        return 0.0;
    }
    // FADE OUT AT THE BOX WALLS. Without this the grid announces itself: a dense plume that
    // reaches a wall is cut off along a dead-straight line in mid-air, which is exactly what
    // the first capture showed -- two horizontal edges across the sky where the box's top and
    // far faces are. Six percent of the box on each side is enough to hide the seam and is
    // cheap (three smoothsteps, no extra taps).
    let edge = min(min(uvw.x, uvw.y), uvw.z);
    let edge2 = min(min(1.0 - uvw.x, 1.0 - uvw.y), 1.0 - uvw.z);
    let fade = smoothstep(0.0, 0.06, min(edge, edge2));
    return textureSampleLevel(field, field_samp, uvw, 0.0).r * fade;
}

// Slab test against the grid box, in camera-relative space.
fn box_span(origin: vec3<f32>, dir: vec3<f32>) -> vec2<f32> {
    let dims = P.dims_steps.xyz;
    let lo = P.origin_cell.xyz - P.cam_albedo.xyz;
    let hi = lo + dims * P.origin_cell.w;
    let inv = 1.0 / dir;
    let t0 = (lo - origin) * inv;
    let t1 = (hi - origin) * inv;
    let tmin = min(t0, t1);
    let tmax = max(t0, t1);
    let near = max(max(tmin.x, tmin.y), tmin.z);
    let far = min(min(tmax.x, tmax.y), tmax.z);
    return vec2<f32>(max(near, 0.0), far);
}

// Henyey-Greenstein. Smoke forward-scatters, which is why a plume between you and the sun
// glows at its rim instead of being a uniform grey lump.
fn phase_hg(cos_t: f32, g: f32) -> f32 {
    let g2 = g * g;
    let denom = 1.0 + g2 - 2.0 * g * cos_t;
    return (1.0 - g2) / (4.0 * 3.14159265 * max(denom * sqrt(max(denom, 1e-4)), 1e-4));
}

// Transmittance from a sample toward the sun, by a short second march through the same
// field. This is the term a billboard cannot have at all: the CPU approximation SMK-035
// added guesses optical depth from a particle's AGE, because a sprite has no field to look
// through. Here there is one.
fn sun_transmittance(world_start: vec3<f32>) -> f32 {
    let steps = i32(P.sun_dir_steps.w);
    if (steps <= 0) {
        return 1.0;
    }
    let step_len = P.sun_radiance.a;
    let dir = P.sun_dir_steps.xyz;
    var tau: f32 = 0.0;
    var i: i32 = 0;
    loop {
        if (i >= steps) { break; }
        // Widening steps: near the sample the field matters most, far away a coarse read
        // is enough, and this reaches several times further for the same tap count.
        let t = step_len * (f32(i) + 0.5) * (1.0 + f32(i) * 0.5);
        tau = tau + density_at(world_start + dir * t) * step_len * (1.0 + f32(i) * 0.5);
        i = i + 1;
    }
    return exp(-tau * P.ambient_ext.a);
}

@fragment
fn fs_march(@builtin(position) frag: vec4<f32>) -> @location(0) vec4<f32> {
    let size = max(P.sizes.xy, vec2<f32>(1.0, 1.0));
    let ndc = vec2<f32>(frag.x / size.x * 2.0 - 1.0, 1.0 - frag.y / size.y * 2.0);

    // Scene depth at this low-res pixel's centre, point-sampled from the FULL-res buffer.
    let full = vec2<i32>(vec2<f32>(P.sizes.zw) * (frag.xy / size));
    let depth = textureLoad(scene_depth, full, 0);

    // The ray direction is the NEAR-plane point in camera-relative space (forward NDC z 0),
    // exactly as the god-ray march builds it. Unprojecting the FAR plane instead looks
    // equivalent and is not: this projection has an infinite far plane, so w goes to zero
    // there and the divide blows up.
    let dir = normalize(unproject(ndc, 1.0));
    // How far to march: to the first opaque surface, or to the grid's far wall for a sky
    // pixel. Stopping at the scene depth is what makes the smoke intersect the world
    // correctly with no per-sprite trick at all.
    var limit: f32 = 1.0e9;
    if (depth > 1.0e-6) {
        limit = length(unproject(ndc, depth));
    }
    let span = box_span(vec3<f32>(0.0, 0.0, 0.0), dir);
    let t_start = span.x;
    let t_end = min(span.y, limit);
    if (t_end <= t_start) {
        return vec4<f32>(0.0, 0.0, 0.0, 1.0);
    }

    let steps = i32(P.dims_steps.w);
    let dt = (t_end - t_start) / f32(steps);
    // Per-pixel jitter breaks the step planes into noise, which the upsample and the eye
    // both tolerate far better than banding.
    let jitter = fract(sin(dot(frag.xy, vec2<f32>(12.9898, 78.233))) * 43758.5453) * P.tune.z;

    let cos_t = dot(dir, P.sun_dir_steps.xyz);
    let phase = phase_hg(cos_t, P.tune.y);
    let ext = P.ambient_ext.a;
    let albedo = P.cam_albedo.w;

    var transmittance: f32 = 1.0;
    var inscatter = vec3<f32>(0.0, 0.0, 0.0);
    var i: i32 = 0;
    loop {
        if (i >= steps) { break; }
        let t = t_start + (f32(i) + jitter) * dt;
        let rel = dir * t;
        let world = rel + P.cam_albedo.xyz;
        let d = density_at(world);
        if (d > 1.0e-4) {
            let sigma_t = d * ext;
            let sun_t = mix(1.0, sun_transmittance(world), P.tune.w);
            let lit = P.sun_radiance.rgb * (sun_t * phase) + P.ambient_ext.rgb;
            // Energy-conserving front-to-back: integrate the step analytically rather than
            // multiplying by dt, so the result does not change when the step count does.
            let step_t = exp(-sigma_t * dt);
            let integrated = (1.0 - step_t) * albedo;
            inscatter = inscatter + transmittance * integrated * lit;
            transmittance = transmittance * step_t;
            if (transmittance < 0.003) {
                break;
            }
        }
        i = i + 1;
    }
    return vec4<f32>(inscatter, transmittance);
}

@fragment
fn fs_composite(@builtin(position) frag: vec4<f32>) -> @location(0) vec4<f32> {
    let low = vec2<i32>(P.sizes.xy);
    let full_size = max(P.sizes.zw, vec2<f32>(1.0, 1.0));
    let ratio = P.sizes.xy / full_size;

    // BILATERAL upsample, not a nearest-depth PICK. The pick was tried first and dithers
    // visibly on a flat wall: neighbouring low-res texels differ because the march is
    // jittered per pixel, and a hard winner-takes-all choice between them turns that noise
    // into a 2x2 checkerboard across the whole surface. Weighting the four instead averages
    // the jitter away, which is what the jitter is for, while the depth term still stops
    // smoke that marched past a foreground silhouette bleeding back over it.
    let my_depth = textureLoad(scene_depth, vec2<i32>(frag.xy), 0);
    let f = frag.xy * ratio - 0.5;
    let base = floor(f);
    let frac = f - base;
    let bi = vec2<i32>(base);

    var acc = vec4<f32>(0.0, 0.0, 0.0, 0.0);
    var wsum: f32 = 0.0;
    var j: i32 = 0;
    loop {
        if (j >= 4) { break; }
        let o = vec2<i32>(j & 1, j >> 1);
        let c = clamp(bi + o, vec2<i32>(0, 0), low - vec2<i32>(1, 1));
        // Bilinear weight of this corner.
        let wx = select(1.0 - frac.x, frac.x, o.x == 1);
        let wy = select(1.0 - frac.y, frac.y, o.y == 1);
        // Depth similarity. Reversed-Z device depth is wildly non-linear, so compare the
        // RELATIVE difference -- which for this projection is proportional to the relative
        // difference in eye depth and needs no matrix.
        let cf = clamp(vec2<i32>((vec2<f32>(c) + 0.5) / ratio),
                       vec2<i32>(0, 0), vec2<i32>(full_size) - vec2<i32>(1, 1));
        let d = textureLoad(scene_depth, cf, 0);
        let rel = abs(d - my_depth) / max(max(d, my_depth), 1.0e-6);
        let w = wx * wy * exp(-rel * 24.0) + 1.0e-4;
        acc = acc + textureLoad(march_result, c, 0) * w;
        wsum = wsum + w;
        j = j + 1;
    }
    // Premultiplied: the pipeline blends src * 1 + dst * srcAlpha, so alpha carries the
    // ray's surviving transmittance and rgb the light the volume added.
    return acc / wsum;
}
