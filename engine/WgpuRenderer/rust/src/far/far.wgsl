// FAR INSTANCE TIER — one cheap proxy for EVERY authored placement, culled and drawn
// entirely on the GPU from the placement rows. Nothing here ever becomes an Object.
//
// The problem it solves: the object residency window ends at ~900 m on Everon and
// ~550-711 m on Chernarus. Past it a placement is not a coarse LOD, it is ABSENT — the
// world is bare ground and every asset pops into being as you close on it. This tier
// carries the island's silhouette from that edge out to the fog wall.
//
// Ported from experiments/farfield (real_cull.wgsl + veg_draw.wgsl), whose numbers are the
// reason the design looks like this:
//   * sweeping 1.2M placements with one compute costs 0.05-0.14 ms; 3.65M costs 0.15-0.23 ms
//   * the whole island at 12 km as cards is 693,525 visible, 0.123 ms cull + 0.627 ms draw
//   * SUB-PIXEL rejection at 2 px is the decisive test: an aircraft view goes 421,323 ->
//     22,135 instances and 0.314 -> 0.025 ms of draw. Most of a million placements are
//     smaller than a pixel long before they leave the frustum, and dropping them is free
//     correctness rather than a quality trade
//   * cards (2 triangles) cost ~4x less to draw than prisms (12), which is why the class
//     bit exists at all

#import frame::{frame, reverse_z, apply_fog, sky_irradiance, cloud_sun_shadow}

struct FarParams {
    // x = near cutoff (m) — a HARD FLOOR, not the start rule (see cs_cull), y = far distance (m),
    // z = sub-pixel limit in PIXELS OF PROJECTED HEIGHT, w = source instance count
    cfg: vec4<f32>,
    // x = pixels per radian ((viewport_height/2) * proj[1][1]), y = enabled,
    // z = dispatch row stride in INVOCATIONS (workgroups_x * 64),
    // w = start rule: 0 = complement of the real cull (default), 1 = legacy pure distance
    //     (WGR_FAR_MODE=distance)
    tune: vec4<f32>,
    // THE REAL OBJECT CULL'S OWN COEFFICIENTS, pushed per frame from Gfx3d::cull_inputs — the
    // same four numbers wgr_set_cull_params hands the object cull.
    //   x = k = pixel_limit * lod_scale * lod_inv_width   (the sub-pixel coefficient, folded)
    //   y = objects_z (m)                                 (the object cull's radial edge)
    //   z, w spare
    // These MOVE AT RUNTIME: the framerate governor drives lod_inv_width, so k is not a
    // constant and a start rule expressed as a distance cannot track it.
    cull: vec4<f32>,
};

// One authored placement's proxy. Mirrors WgrFarInstance (32 B) exactly.
// `pos` is ABSOLUTE world space; every consumer below subtracts the camera first.
struct FarSource {
    pos: vec3<f32>,
    height: f32,
    radius: f32,
    colour: u32,
    flags: u32,
    padding: u32,
};

// One survivor, compacted by the cull for the indirect draws.
struct FarVisible {
    pos: vec3<f32>,
    height: f32,
    radius: f32,
    colour: u32,
    seed: f32,
    padding: f32,
};

// The single compacted buffer is PARTITIONED, not shared: cards occupy [0, CARD_CAP) and
// prisms [CARD_CAP, CARD_CAP + PRISM_CAP). A partition needs no INDIRECT_FIRST_INSTANCE
// (which is an optional wgpu capability here — see WGR_RUNTIME_CAP_INDIRECT_FIRST_INSTANCE):
// the prism vertex shader simply adds CARD_CAP to its instance index. Two separate buffers
// would cost the same memory and one more binding.
const CARD_CAP: u32 = 1048576u;
const PRISM_CAP: u32 = 262144u;

// A zero horizontal extent is a legal ModelInfo (a flagpole, a wire), so the proxy that is
// actually emitted is widened to this. The complement test measures the SAME clamped value,
// because the question it asks is about the box this tier would draw, not about a degenerate
// number in the source row.
const MIN_PROXY_RADIUS: f32 = 0.25;

@group(1) @binding(0) var<uniform> far: FarParams;
@group(1) @binding(1) var<storage, read> src: array<FarSource>;
@group(1) @binding(2) var<storage, read_write> visible: array<FarVisible>;
// [0] = cards, [1] = prisms. Copied into the indirect args after the pass (a buffer bound
// read_write cannot also be the indirect source in the same encoder).
@group(1) @binding(3) var<storage, read_write> counts: array<atomic<u32>>;

fn hash01(v: u32) -> f32 {
    var h = v;
    h ^= h >> 16u;
    h = h * 0x7feb352du;
    h ^= h >> 15u;
    h = h * 0x846ca68bu;
    h ^= h >> 16u;
    return f32(h & 0xffffffu) / 16777216.0;
}

fn unpack_argb(c: u32) -> vec3<f32> {
    // The engine's PackedColor is (a << 24) | (r << 16) | (g << 8) | b — see
    // PackedColorRGB in Graphics/Rendering/ColorsFloat.hpp. The alpha byte is ignored:
    // a proxy is opaque by construction.
    let r = f32((c >> 16u) & 0xffu) * (1.0 / 255.0);
    let g = f32((c >> 8u) & 0xffu) * (1.0 / 255.0);
    let b = f32(c & 0xffu) * (1.0 / 255.0);
    return vec3<f32>(r, g, b);
}

@compute @workgroup_size(64, 1, 1)
fn cs_cull(@builtin(global_invocation_id) gid: vec3<u32>) {
    if (far.tune.y < 0.5) {
        return;
    }
    // A world can hold more placements than one dispatch dimension addresses (65,535
    // workgroups x 64 = 4,194,240 invocations), so the sweep is dispatched as a 2D grid and
    // the row stride comes down in the uniform. Chernarus alone is 2.97M rows; the limit is
    // not hypothetical headroom.
    let i = gid.y * u32(far.tune.z) + gid.x;
    if (i >= u32(far.cfg.w)) {
        return;
    }

    let s = src[i];
    // CAMERA-RELATIVE, always. At kilometre-scale world coordinates an absolute frustum
    // test shifts the frustum by cam_pos, which passes a spot check near the origin and
    // fails everywhere else on the map.
    let rel = s.pos - frame.cam_pos.xyz;
    let d = length(rel);
    // The far distance is the fog wall. The near cutoff is a HARD FLOOR and nothing more:
    // whatever the start rule below concludes, never put a crude box this close to the eye,
    // where it would be compared against the real model standing beside it.
    if (d > far.cfg.y || d < far.cfg.x) {
        return;
    }

    // START RULE — THE COMPLEMENT OF THE REAL OBJECT CULL, not a distance.
    //
    // No distance can be right, and this was measured rather than argued (see
    // experiments/farfield/cutoff_analysis.py against the real 1.2M Everon placement set with
    // exact per-model bounds). At the governor's coarse rail, a cutoff at the fog edge left
    // 36.2% of the placements inside the fog radius drawn by NOTHING — that is the reported
    // pop-in — while a cutoff tuned to close that gap put a proxy on top of a real model for
    // 46.8% of them. The trade exists because the real cull is SIZE-dependent and its
    // coefficient moves with the framerate governor, so any fixed metre value is wrong in
    // both directions at once and wrong by a different amount each frame.
    //
    // So this tier accepts EXACTLY what the object cull drops. gfx3d/cull.wgsl drops an
    // instance when either the radial test fails (dist2 > objects_z2) or the sub-pixel test
    // does (diameter^2 < (pixel_limit*lod_scale)^2 * dist2 * lod_inv_width^2); with k folding
    // pixel_limit * lod_scale * lod_inv_width, that second one is diameter < k * d. Accept is
    // the negation of both, which is 0% overlap and 0% gap by construction, at every rail.
    //
    // Deliberately NOT replicated: cull.wgsl's legacy near-clamp (a sphere large relative to
    // its distance is measured from its near surface). It only ever SHRINKS the distance, so
    // omitting it can only make this tier think the real object was dropped when it was
    // drawn — i.e. it errs toward a redundant proxy, never toward a hole — and it can only
    // fire within a few radii of the camera, which the hard floor above already owns.
    //
    // The proxy's size term is its own drawn width, 2 * radius (radius is
    // ModelProxyBounds::horizontalExtent * 0.5, so this is the model's horizontal extent) —
    // the same quantity the offline analysis swept, and the analogue of cull.wgsl's
    // `model.bounding_sphere * 2.0 * scale`: a stored half-size doubled into a diameter.
    if (far.tune.w < 0.5) {
        let k = far.cull.x;
        let objects_z = far.cull.y;
        let proxy_diameter = max(s.radius, MIN_PROXY_RADIUS) * 2.0;
        let real_drawn = (d <= objects_z) && (proxy_diameter >= k * d);
        if (real_drawn) {
            return;
        }
    }

    let h = max(s.height, 0.01);

    // Sub-pixel rejection — the test that decides whether the tier is affordable at all.
    // px_limit <= 0 disables it, so the A/B is one uniform apart.
    if (far.cfg.z > 0.0 && far.tune.x > 0.0) {
        let px = h / max(d, 1.0) * far.tune.x;
        if (px < far.cfg.z) {
            return;
        }
    }

    // Frustum reject on the bounding sphere, still camera-relative.
    let centre = rel + vec3<f32>(0.0, h * 0.5, 0.0);
    let clip = frame.proj * frame.view * vec4<f32>(centre, 1.0);
    let r = max(h * 0.9, s.radius);
    if (clip.w < -r) {
        return;
    }
    let m = max(abs(clip.w), 1.0) + r * 2.0;
    if (abs(clip.x) > m || abs(clip.y) > m) {
        return;
    }

    var slot: u32 = 0u;
    if ((s.flags & 1u) != 0u) {
        let n = atomicAdd(&counts[1], 1u);
        if (n >= PRISM_CAP) {
            // Pin the counter at the cap so the indirect draw cannot ask for instances the
            // buffer does not hold. Every overflowing invocation does this AFTER its own
            // add, so whichever atomic runs last, a min follows it in that thread's program
            // order and the settled value is exactly PRISM_CAP.
            atomicMin(&counts[1], PRISM_CAP);
            return;
        }
        slot = CARD_CAP + n;
    } else {
        let n = atomicAdd(&counts[0], 1u);
        if (n >= CARD_CAP) {
            atomicMin(&counts[0], CARD_CAP);
            return;
        }
        slot = n;
    }

    visible[slot].pos = s.pos;
    visible[slot].height = h;
    visible[slot].radius = max(s.radius, MIN_PROXY_RADIUS);
    visible[slot].colour = s.colour;
    visible[slot].seed = hash01(i ^ 0x9e3779b9u);
    visible[slot].padding = 0.0;
}

struct VsOut {
    @builtin(position) clip: vec4<f32>,
    @location(0) uv: vec2<f32>,
    @location(1) albedo: vec3<f32>,
    @location(2) world_rel: vec3<f32>,
    @location(3) normal: vec3<f32>,
    @location(4) seed: f32,
};

// Lit exactly like the rest of the world (sun + sky irradiance + cloud shadow, linear
// output, scene fog) rather than with its own constants. A proxy that is merely PLAUSIBLE
// in isolation reads as a different material painted onto the distance — the mistake the
// far grass ring made for as long as it carried two hardcoded greens.
fn far_shade(albedo: vec3<f32>, normal: vec3<f32>, world_rel: vec3<f32>) -> vec4<f32> {
    let world = world_rel + frame.cam_pos.xyz;
    let light_dir = normalize(frame.sun_dir_world.xyz);
    let diffuse = max((dot(normal, light_dir) + 0.35) / 1.35, 0.0);
    let direct = frame.sun_diffuse.rgb * diffuse * cloud_sun_shadow(world.xz);
    let ambient = sky_irradiance(normal);
    return vec4<f32>(apply_fog(albedo * (ambient + direct), world_rel), 1.0);
}

// --- Cards: the vegetation / default proxy, 2 triangles -----------------------

@vertex
fn vs_card(@builtin(vertex_index) vi: u32, @builtin(instance_index) ii: u32) -> VsOut {
    var corner = array<vec2<f32>, 6>(
        vec2<f32>(-0.5, 0.0), vec2<f32>(0.5, 0.0), vec2<f32>(-0.5, 1.0),
        vec2<f32>(0.5, 0.0), vec2<f32>(0.5, 1.0), vec2<f32>(-0.5, 1.0));
    let c = corner[vi];
    let inst = visible[ii];

    // Camera-facing about the WORLD UP axis: a cylindrical billboard, which is what a
    // distant tree wants. A spherical one leans as the camera pitches, and from an
    // aircraft — the view this tier exists for — that is the whole forest tilting.
    let to_cam = normalize(vec3<f32>(frame.cam_pos.x - inst.pos.x, 0.0, frame.cam_pos.z - inst.pos.z));
    let side = normalize(cross(vec3<f32>(0.0, 1.0, 0.0), to_cam));
    let w = max(inst.radius * 2.0, inst.height * 0.25);
    let world = inst.pos + side * (c.x * w) + vec3<f32>(0.0, c.y * inst.height, 0.0);
    let rel = world - frame.cam_pos.xyz;

    var o: VsOut;
    o.clip = reverse_z(frame.proj * frame.view * vec4<f32>(rel, 1.0));
    o.uv = vec2<f32>(c.x + 0.5, c.y);
    o.world_rel = rel;
    // Billboards have no real normal; face the camera, tilted up, so the sun still reads
    // across a hillside of them instead of flattening the whole stand to one tone.
    o.normal = normalize(to_cam + vec3<f32>(0.0, 0.6, 0.0));
    o.albedo = unpack_argb(inst.colour) * (0.85 + 0.3 * inst.seed);
    o.seed = inst.seed;
    return o;
}

@fragment
fn fs_card(in: VsOut) -> @location(0) vec4<f32> {
    // Analytic canopy silhouette. A far card's job is to break the rectangle: an
    // untrimmed quad reads as a fence panel, which is more wrong at distance than a
    // slightly generic tree shape is.
    let x = (in.uv.x - 0.5) * 2.0;
    let y = in.uv.y;
    let width = mix(1.0, 0.12, pow(y, 0.75));
    let n = fract(sin(in.uv.x * 37.0 + y * 91.0 + in.seed * 53.0) * 43758.5453) * 0.30;
    if (abs(x) > width * (1.0 - n) || y > 0.985) {
        discard;
    }
    let lit = 0.55 + 0.45 * (1.0 - abs(x)) * (0.4 + 0.6 * y);
    return far_shade(in.albedo * lit, in.normal, in.world_rel);
}

// --- Prisms: the structure proxy, 12 triangles --------------------------------

// Unit box, 36 vertices, origin at the base centre. Extruding a footprint is the cheapest
// far representation that keeps a building's silhouette AND its exact position — which,
// unlike a tree's, is authored and not negotiable.
fn box_corner(vi: u32) -> vec3<f32> {
    var idx = array<u32, 36>(
        0u, 2u, 1u, 0u, 3u, 2u, 4u, 5u, 6u, 4u, 6u, 7u,
        0u, 1u, 5u, 0u, 5u, 4u, 1u, 2u, 6u, 1u, 6u, 5u,
        2u, 3u, 7u, 2u, 7u, 6u, 3u, 0u, 4u, 3u, 4u, 7u);
    var pos = array<vec3<f32>, 8>(
        vec3<f32>(-0.5, 0.0, -0.5), vec3<f32>(0.5, 0.0, -0.5),
        vec3<f32>(0.5, 0.0, 0.5), vec3<f32>(-0.5, 0.0, 0.5),
        vec3<f32>(-0.5, 1.0, -0.5), vec3<f32>(0.5, 1.0, -0.5),
        vec3<f32>(0.5, 1.0, 0.5), vec3<f32>(-0.5, 1.0, 0.5));
    return pos[idx[vi]];
}

@vertex
fn vs_prism(@builtin(vertex_index) vi: u32, @builtin(instance_index) ii: u32) -> VsOut {
    // Prisms live in the upper partition of the same compacted buffer.
    let inst = visible[CARD_CAP + ii];
    let c = box_corner(vi);
    // Axis-aligned footprint from the model's own horizontal extent. The authored yaw is
    // not carried: a proxy this far out is a few pixels wide, and a rotation would cost a
    // matrix per instance in the source rows for no visible difference.
    let d = max(inst.radius * 2.0, 1.0);
    let world = inst.pos + vec3<f32>(c.x * d, c.y * inst.height, c.z * d);
    let rel = world - frame.cam_pos.xyz;

    var o: VsOut;
    o.clip = reverse_z(frame.proj * frame.view * vec4<f32>(rel, 1.0));
    o.uv = vec2<f32>(c.x + 0.5, c.y);
    o.world_rel = rel;
    // Face normal from which side of the unit box the corner sits on: the roof reads
    // separately from the walls, which is most of what makes a box read as a building.
    if (c.y > 0.99) {
        o.normal = vec3<f32>(0.0, 1.0, 0.0);
    } else {
        o.normal = normalize(vec3<f32>(c.x, 0.35, c.z));
    }
    o.albedo = unpack_argb(inst.colour) * (0.9 + 0.2 * inst.seed);
    o.seed = inst.seed;
    return o;
}

@fragment
fn fs_prism(in: VsOut) -> @location(0) vec4<f32> {
    return far_shade(in.albedo, in.normal, in.world_rel);
}
