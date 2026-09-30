// GPU cull + LOD + indirect-arg compaction (docs/gpu-culling-and-depth-plan.md Stage 3 + §3.6).
//
// Frustum + distance + sub-pixel cull, pick a LOD, and emit one INSTANCED DrawIndexedIndirect
// per surviving GLOBAL section (instance_count = # instances that chose a LOD containing it),
// their per-draw records laid out contiguously. This replaces the CPU per-object walk + LODShape
// draw (Scene::ObjectForDrawing / AdjustComplexity / Object::Draw) — the whole point being that
// with the CPU out of the per-object loop we can push draw distance + LOD detail well past what
// the CPU triangle budget allowed.
//
// Instancing collapse (§3.6) is a THREE-PASS dispatch (COUNT -> EMIT -> SCATTER, see the entries
// at the bottom). The old scheme emitted one instance_count=1 sub-draw per (instance, section);
// this collapses N identical models into one instanced draw, so the arg count drops from
// (# surviving pairs) to (# surviving sections). No prefix sum — a single atomic bump per section
// carves the contiguous record runs.
//
// LOD selection is seeded from the legacy FindSqrtLevel / LevelFromDistance2 SHAPE
// (dist² -> detail² -> resol² vs each level's resolution²), but EXACT parity is a non-goal:
// `lod_scale` is a tunable detail knob and the adaptive `_lodInvWidth` feedback loop is
// dropped for a fixed generous bias. Frustum + distance culling, by contrast, must be
// correct — dropping visible geometry or drawing everything are the real hazards.

struct CullParams {
    // World-space frustum planes (nx, ny, nz, d), normalized + oriented so a point is
    // INSIDE when dot(plane.xyz, p) + plane.w >= 0. Six planes; the near plane is the
    // reversed-Z-aware one (see frustum_planes_from_view_proj on the Rust side).
    frustum: array<vec4<f32>, 6>,
    cam_pos: vec4<f32>,      // world camera position (xyz)
    objects_z2: f32,         // draw distance² (distance cull)
    lod_scale: f32,          // Camera::Left() * detail_bias (LevelFromDistance2's `scale`, tunable)
    lod_inv_width: f32,      // detail multiplier (legacy _lodInvWidth; fixed, not fed back)
    pixel_limit: f32,        // sub-pixel cull threshold (legacy 0.125)
    instance_count: u32,
    variant_capacity: u32,   // max args per pipeline variant (partition stride into out_args)
    variant_count: u32,
    debug_flags: u32,        // bit 0 = no frustum; bit 1 = positive main-camera authority
    // Occlusion (main_occlude only; ignored by main). view_proj is the CAMERA-RELATIVE
    // proj*view (translation zeroed) that projects a camera-relative point to clip; the
    // Hi-Z is sampled with the resulting screen rect + reversed-Z depth (see occluded()).
    view_proj: mat4x4<f32>,
    viewport: vec2<f32>,     // Hi-Z mip0 dimensions in texels (= render target size)
    hiz_mips: u32,           // Hi-Z mip count (clamp the selected mip)
    occlusion: u32,          // 1 = run the Hi-Z occlusion test (main_occlude)
    // Private diagnostic target; Rust CullParamsGpu carries the same aligned tail.
    target_model_id: u32,
    target_count_enabled: u32,
    _target_pad: vec2<u32>,
};

struct Instance {
    world: mat4x4<f32>,      // absolute model->world (read by the GPU-driven VS, not here)
    center: vec4<f32>,       // world bounding-sphere center (xyz), w = uniform scale
    model: u32,              // index into models[]
    flags: u32,
    cull_radius: u32,        // inflated frustum-cull radius (f32 bits); 0 = rigid (use model sphere)
    _pad: u32,
    // Terrain-conform plane — read only by the GPU-driven VS, not the cull; present here so
    // the storage-buffer stride matches InstanceGpu (cull.rs).
    conform0: vec4<f32>,
    conform1: vec4<f32>,
    conform2: vec4<f32>,
};

const INST_MAIN_CAMERA_ONLY: u32 = 32u; // WGR_INSTANCE_MAIN_CAMERA_ONLY
const INST_OTHER_VIEWS_ONLY: u32 = 64u; // WGR_INSTANCE_OTHER_VIEWS_ONLY
const CULL_VIEW_MAIN_CAMERA: u32 = 2u;  // only main prepass/color setters grant this

struct Model {
    lod_base: u32,           // first entry in lods[]
    lod_count: u32,          // drawable LOD levels (graphical only; special >=900 excluded)
    bounding_sphere: f32,    // model radius at scale 1
    _pad: u32,
};

struct Lod {
    resolution: f32,         // _resolutions[i]
    section_base: u32,       // first entry in sections[]
    section_count: u32,
    is_decal: u32,           // 1 = decal LOD (stepped past by the noDecal rule)
};

struct Section {
    first_index: u32,        // pool ibase + section start (into the shared index buffer)
    index_count: u32,
    base_vertex: u32,        // pool vbase
    variant: u32,            // pipeline-variant bucket (0..variant_count)
};

// The DrawIndexedIndirectArgs layout every backend's multi_draw_indexed_indirect consumes.
struct DrawArgs {
    index_count: u32,
    instance_count: u32,
    first_index: u32,
    base_vertex: i32,
    first_instance: u32,
};

@group(0) @binding(0) var<uniform> cull: CullParams;
@group(0) @binding(1) var<storage, read> instances: array<Instance>;
@group(0) @binding(2) var<storage, read> models: array<Model>;
@group(0) @binding(3) var<storage, read> lods: array<Lod>;
@group(0) @binding(4) var<storage, read> sections: array<Section>;
@group(0) @binding(5) var<storage, read_write> out_args: array<DrawArgs>;
// Append cursors: one per pipeline variant (0..variant_count) for the arg compaction, PLUS
// one extra word at index variant_count = the global out_records bump allocator (the "records
// cursor" the instancing-collapse passes carve per-section runs from). Sized variant_count + 1.
@group(0) @binding(6) var<storage, read_write> counters: array<atomic<u32>>;
// Per-draw record parallel to out_records slots: which instance + which section this sub-draw is.
// A multi_draw sub-draw's first_instance indexes THIS (not the instance buffer directly),
// so the VS/FS can recover both the instance transform AND the per-section material —
// material is per-section, and the shader can't derive the section from the instance alone.
struct Record {
    instance: u32,
    section: u32,
};
@group(0) @binding(7) var<storage, read_write> out_records: array<Record>;
// Hi-Z depth pyramid (main_occlude only — main's layout omits this binding). Full mip
// chain; the occlusion test textureLoads the mip whose texels cover the instance's screen rect.
@group(0) @binding(8) var hiz: texture_2d<f32>;
// Per-global-section scratch for the instancing-collapse three-pass (docs §3.6). Sized
// sections.len(); reused across the passes: pass COUNT atomicAdds the surviving-instance count
// into it, pass EMIT reads that count and overwrites the slot with the section's out_records run
// base, pass SCATTER bumps it to fill the run. Cleared to 0 each frame before COUNT.
@group(0) @binding(9) var<storage, read_write> sec_count: array<atomic<u32>>;
// PERF-005 — per-view accounting, living in the TAIL of the existing `counters` buffer rather
// than a binding of its own. A tenth binding is not available: this layout already holds 8
// storage buffers and `max_storage_buffers_per_shader_stage` is 8 on the baseline adapter (the
// headless test device rejects a 9th outright), so a separate stats buffer would have cost
// compatibility with every 8-storage-buffer GPU to save one indexing offset.
// `counters` words 0..variant_count are the arg cursors + the records cursor (see above);
// word 3 is spare padding and STAT_BASE onwards is this accounting. Word map — keep in lockstep
// with STAT_* in cull.rs:
//   +0 surviving instances       +1 instance-section records    +2 triangles
//   +3 draw args emitted         +4/+5 draw args by variant     +6/+7 triangles by variant
//   +8..+15 LOD histogram (level 0..6, 7 = "7 or coarser")
//   +16..+19 WHAT-IF triangles: the main view's triangle total if lod_inv_width were scaled by
//            WHATIF_SCALE[i] (0.5, 0.71, 1.41, 2.0), same LOD rule, same survivors. This is
//            what lets the CPU's LOD governor see the cost of a step BEFORE taking it
//            (REN-VEG-004): the retained set's cost is a 700x ladder on a vegetation world and
//            last frame's total says nothing about the next level.
//   +20 exact-model COUNT (private opt-in); +21..+23 spare
// The histogram is the point of this block: the standing hypothesis is that detailed building
// and vegetation LODs stay active too far out, and only a per-level population count can test it.
const STAT_BASE: u32 = 4u;

fn outside_frustum(center: vec3<f32>, radius: f32) -> bool {
    for (var i = 0u; i < 6u; i++) {
        let p = cull.frustum[i];
        if (dot(p.xyz, center) + p.w < -radius) {
            return true;
        }
    }
    return false;
}

// Per-instance cull + LOD result. `culled` short-circuits the caller; the geometry fields
// feed the (optional) occlusion test. No Hi-Z reference here, so the plain `main` entry (whose
// layout omits binding 8) can call this — only main_occlude touches `hiz`.
struct CullResult {
    culled: bool,
    level: u32,
    dist2: f32,        // the near-clamped distance^2 the LOD was picked from (what-if reuses it)
    center: vec3<f32>, // world bounding-sphere center
    radius: f32,       // model radius * scale (for the near-clamp + occlusion projection)
};

// Frustum + distance + sub-pixel cull and LOD select (replicating LevelFromDistance2 /
// FindSqrtLevel). Occlusion is applied separately by the caller (Hi-Z, main_occlude only).
fn classify(idx: u32) -> CullResult {
    var res: CullResult;
    res.culled = true;
    res.level = 0u;
    let inst = instances[idx];
    // A private page has no authority in shadow, reflection, sky/GI or a
    // future view that has not explicitly opted into main-camera visibility.
    // COUNT and SCATTER both pass through classify, so they exclude it alike.
    if ((inst.flags & INST_MAIN_CAMERA_ONLY) != 0u &&
        (cull.debug_flags & CULL_VIEW_MAIN_CAMERA) == 0u) {
        return res;
    }
    // The original remains in every view without positive main-camera
    // authority, including future/unknown views. Conflicting 32|64 bits
    // fail closed because one of these two predicates rejects every view.
    if ((inst.flags & INST_OTHER_VIEWS_ONLY) != 0u &&
        (cull.debug_flags & CULL_VIEW_MAIN_CAMERA) != 0u) {
        return res;
    }
    // Removed static slots are marked model = 0xFFFFFFFF (a free-list hole); this also
    // guards any out-of-range model id.
    if (inst.model >= arrayLength(&models)) {
        return res;
    }
    let center = inst.center.xyz;
    let scale = inst.center.w;
    let model = models[inst.model];
    let radius = model.bounding_sphere * scale;
    // Terrain-conformed instances draw their geometry displaced onto the terrain, so the flat,
    // surface-relative model sphere no longer contains it (it spreads with the slope across the
    // footprint). C++ stores an inflated FRUSTUM-cull radius in cull_radius for these; use it only
    // for the frustum test, keeping the model radius for the distance near-clamp + sub-pixel size.
    // 0 = rigid instance -> use the model radius.
    let cull_radius = select(radius, bitcast<f32>(inst.cull_radius), inst.cull_radius != 0u);

    // Distance (camera-relative) with the legacy near-clamp: a sphere large relative to its
    // distance is measured from its near surface so big near objects don't drop to a coarse
    // LOD (Scene.cpp:903).
    let rel = center - cull.cam_pos.xyz;
    var dist2 = dot(rel, rel);
    if (radius * radius > dist2 * 0.0625) {
        let dn = max(sqrt(dist2) - radius, 0.0);
        dist2 = dn * dn;
    }
    if (dist2 > cull.objects_z2) {
        return res;
    }

    // Frustum: the planes are CAMERA-RELATIVE (extracted from proj * a translation-zeroed
    // view — the engine's geometry is camera-relative), so test the camera-relative center
    // `rel`, not the absolute `center`. Testing the absolute center here shifts the frustum
    // by cam_pos (kilometres on a real map) and culls erratically.
    // debug_flags bit 0 = WGR_CULL_NO_FRUSTUM: skip the frustum test entirely. A debug
    // discriminator — if objects that vanish at certain pitches STOP vanishing with this set, the
    // frustum cull is the cause; if they still vanish, the cull is innocent (chase the draw/LOD).
    if ((cull.debug_flags & 1u) == 0u && outside_frustum(rel, cull_radius)) {
        return res;
    }

    // Sub-pixel: diameter² < (pixel_limit * lod_scale)² * detail²  -> too small to see.
    let detail2 = dist2 * cull.lod_inv_width * cull.lod_inv_width;
    let diameter = model.bounding_sphere * 2.0 * scale;
    let px = cull.pixel_limit * cull.lod_scale;
    if (diameter * diameter < px * px * detail2) {
        return res;
    }

    // LOD select (FindSqrtLevel-style; ShapeLOD.cpp:1592). Walk resolutions ascending,
    // stop at the first level too rough for resol2, then step back past decal LODs.
    var level = 0u;
    if (model.lod_count >= 2u) {
        let resol2 = detail2 * cull.lod_scale * cull.lod_scale;
        var i = 1u;
        loop {
            if (i >= model.lod_count) { break; }
            let r = lods[model.lod_base + i].resolution;
            if (r * r > resol2) { break; }
            i++;
        }
        i -= 1u;
        loop {
            if (i == 0u || lods[model.lod_base + i].is_decal == 0u) { break; }
            i--;
        }
        level = i;
    }

    // Visual-only authored far tier, shared by colour and shadow culls. Never
    // admit a special/decal level as its replacement for the resident object.
    if ((inst.flags & 16u) != 0u && model.lod_count > 0u) {
        var far_level = model.lod_count - 1u;
        loop {
            if (far_level == 0u || lods[model.lod_base + far_level].is_decal == 0u) { break; }
            far_level--;
        }
        level = far_level;
    }

    res.culled = false;
    res.level = level;
    res.dist2 = dist2;
    res.center = center;
    res.radius = radius;
    return res;
}

// Instancing collapse (docs §3.6): three passes replace the old one-sub-draw-per-(instance,
// section) emit. Instead we emit ONE instanced DrawArgs per surviving GLOBAL section, with
// instance_count = the number of instances that selected a LOD containing it and their records
// laid out contiguously. The section id already encodes (model, LOD, section), so grouping by it
// is automatic: instances at different distances pick different LODs -> different sections ->
// separate draws. No prefix sum — a single atomic bump per section carves the contiguous runs.

// Pass COUNT (1 thread / surviving instance): tally this instance into each section of its LOD.
// Also the only place that sees (surviving instance, chosen LOD) together, so the PERF-005
// survivor count + LOD histogram are recorded here. SCATTER re-derives the same survivors but
// must NOT record — it would double every count.
fn count_sections(idx: u32, level: u32) {
    let lod_base = models[instances[idx].model].lod_base;
    let lod = lods[lod_base + level];
    for (var s = 0u; s < lod.section_count; s++) {
        atomicAdd(&sec_count[lod.section_base + s], 1u);
    }
    atomicAdd(&counters[STAT_BASE + 0u], 1u);
    atomicAdd(&counters[STAT_BASE + 8u + min(level, 7u)], 1u);
    // COUNT sees the surviving instance and its exact model together. A zero-section
    // LOD still counts as present, conservatively, for any future retirement.
    if (cull.target_count_enabled != 0u &&
        instances[idx].model == cull.target_model_id) {
        atomicAdd(&counters[STAT_BASE + 20u], 1u);
    }
}

// Triangles of one LOD level of one instance (sum of its sections' index counts / 3).
fn lod_triangles(lod_base: u32, level: u32) -> u32 {
    let lod = lods[lod_base + level];
    var tris = 0u;
    for (var s = 0u; s < lod.section_count; s++) {
        tris += sections[lod.section_base + s].index_count / 3u;
    }
    return tris;
}

// The LOD this instance would pick under a scaled detail multiplier (same rule as classify:
// the coarsest level whose resolution^2 <= resol2, stepped back over decal levels).
fn level_for(lod_base: u32, lod_count: u32, resol2: f32) -> u32 {
    if (lod_count < 2u) {
        return 0u;
    }
    var i = 1u;
    loop {
        if (i >= lod_count) { break; }
        let r = lods[lod_base + i].resolution;
        if (r * r > resol2) { break; }
        i++;
    }
    i -= 1u;
    loop {
        if (i == 0u || lods[lod_base + i].is_decal == 0u) { break; }
        i--;
    }
    return i;
}

// What-if accounting (word map +16..+19): the survivors' triangle total under four alternative
// detail multipliers. Sub-pixel and distance culls are not re-evaluated -- the question is
// what the SAME survivors would cost one step finer or coarser, which is the governor's
// question. Costs four LOD walks and four section sums per surviving instance.
fn whatif_triangles(idx: u32, dist2: f32) {
    let inst = instances[idx];
    let model = models[inst.model];
    if ((inst.flags & 16u) != 0u) {
        let level = level_for(model.lod_base, model.lod_count, 1.0e30);
        let tris = lod_triangles(model.lod_base, level);
        for (var i = 16u; i < 20u; i++) {
            atomicAdd(&counters[STAT_BASE + i], tris);
        }
        return;
    }
    let base_detail2 = dist2 * cull.lod_inv_width * cull.lod_inv_width;
    let scale2 = cull.lod_scale * cull.lod_scale;
    let f0 = 0.5;
    let f1 = 0.70710678;
    let f2 = 1.41421356;
    let f3 = 2.0;
    let t0 = lod_triangles(model.lod_base, level_for(model.lod_base, model.lod_count, base_detail2 * f0 * f0 * scale2));
    let t1 = lod_triangles(model.lod_base, level_for(model.lod_base, model.lod_count, base_detail2 * f1 * f1 * scale2));
    let t2 = lod_triangles(model.lod_base, level_for(model.lod_base, model.lod_count, base_detail2 * f2 * f2 * scale2));
    let t3 = lod_triangles(model.lod_base, level_for(model.lod_base, model.lod_count, base_detail2 * f3 * f3 * scale2));
    atomicAdd(&counters[STAT_BASE + 16u], t0);
    atomicAdd(&counters[STAT_BASE + 17u], t1);
    atomicAdd(&counters[STAT_BASE + 18u], t2);
    atomicAdd(&counters[STAT_BASE + 19u], t3);
}

// Pass SCATTER (1 thread / surviving instance): append this instance's record into each of its
// sections' runs. sec_count[s] holds the run's fill cursor (seeded to the run base by EMIT).
fn scatter_sections(idx: u32, level: u32) {
    let lod_base = models[instances[idx].model].lod_base;
    let lod = lods[lod_base + level];
    let n_rec = arrayLength(&out_records);
    for (var s = 0u; s < lod.section_count; s++) {
        let gsec = lod.section_base + s;
        let slot = atomicAdd(&sec_count[gsec], 1u);
        // In-bounds only: a section whose run overflowed out_records (EMIT emitted no arg for it)
        // still bumps here, but its writes past the end are dropped — nothing draws them anyway.
        if (slot < n_rec) {
            out_records[slot] = Record(idx, gsec);
        }
    }
}

// Hi-Z occlusion test (main_occlude only). Projects the instance's world-space bounding-sphere
// AABB to the screen, picks the mip whose texels cover the rect, samples the reversed-Z Hi-Z
// (min over the region = the FARTHEST occluder), and reports occluded when even the sphere's
// NEAREST point is behind that occluder — the conservative Hi-Z test. Bails (returns false =
// visible) whenever it can't test safely: a bound crossing the near plane, or a screen rect that
// leaves the viewport (edge texels carry no coverage info for the off-screen part).
fn occluded(center: vec3<f32>, radius: f32) -> bool {
    let rel = center - cull.cam_pos.xyz;
    var uv_min = vec2<f32>(1.0e30, 1.0e30);
    var uv_max = vec2<f32>(-1.0e30, -1.0e30);
    var depth_near = 0.0; // max reversed-Z over the corners = the sphere's nearest point
    for (var i = 0u; i < 8u; i++) {
        let sx = select(-1.0, 1.0, (i & 1u) != 0u);
        let sy = select(-1.0, 1.0, (i & 2u) != 0u);
        let sz = select(-1.0, 1.0, (i & 4u) != 0u);
        let corner = rel + vec3<f32>(sx, sy, sz) * radius;
        let clip = cull.view_proj * vec4<f32>(corner, 1.0);
        // clip.w = view-space forward distance; <= 0 means the bound straddles/behind the near
        // plane, where the perspective projection is unreliable -> don't cull.
        if (clip.w <= 1.0e-4) {
            return false;
        }
        // view_proj is the FORWARD projection (near->0, far->1); the pipelines apply
        // frame.wgsl reverse_z (z = w - z) before rasterizing, so the depth buffer + Hi-Z store
        // reversed-Z. Match that exactly here (z/w would compare forward depth against a reversed
        // Hi-Z — the whole scene reads "not occluded"). This is NOT visible to the frustum cull
        // (which only uses x/y/w), so it must be applied specifically here.
        let uv = vec2<f32>(clip.x / clip.w * 0.5 + 0.5, clip.y / clip.w * -0.5 + 0.5);
        let depth = (clip.w - clip.z) / clip.w; // reverse_z, matching the depth buffer
        uv_min = min(uv_min, uv);
        uv_max = max(uv_max, uv);
        depth_near = max(depth_near, depth);
    }
    // Clamp the screen rect to the viewport rather than bailing on any overhang: the off-screen
    // part of the bound is frustum-clipped and never rasterized, so testing only the ON-screen
    // extent is still correct — and it recovers the (large) set of edge-poking objects that a
    // full-viewport view otherwise leaves undrawn-but-uncalled. depth_near stays computed from ALL
    // 8 corners (incl. off-screen ones), which only makes culling MORE conservative (the nearest
    // corner is the hardest to be behind an occluder), never over-culls. A clamped-edge texel that
    // is a hole (sky) just keeps the object. (The near-plane bail above is still mandatory — a
    // bound crossing the near plane can't be projected at all.)
    uv_min = clamp(uv_min, vec2<f32>(0.0), vec2<f32>(1.0));
    uv_max = clamp(uv_max, vec2<f32>(0.0), vec2<f32>(1.0));
    // Mip whose texel spans the rect (rect <= ~1 texel wide at that level, so the 2x2 below
    // covers it). ceil(log2(max screen extent in texels)).
    let extent = (uv_max - uv_min) * cull.viewport;
    let max_px = max(extent.x, extent.y);
    var mip = 0;
    if (max_px > 1.0) {
        mip = i32(ceil(log2(max_px)));
    }
    mip = clamp(mip, 0, i32(cull.hiz_mips) - 1);
    let mip_dims = vec2<f32>(max(cull.viewport / exp2(f32(mip)), vec2<f32>(1.0, 1.0)));
    let maxc = vec2<i32>(mip_dims) - vec2<i32>(1, 1);
    // Sample the four rect corners at this mip (up to a 2x2 block); min = farthest occluder.
    let c0 = clamp(vec2<i32>(uv_min * mip_dims), vec2<i32>(0), maxc);
    let c1 = clamp(vec2<i32>(vec2<f32>(uv_max.x, uv_min.y) * mip_dims), vec2<i32>(0), maxc);
    let c2 = clamp(vec2<i32>(vec2<f32>(uv_min.x, uv_max.y) * mip_dims), vec2<i32>(0), maxc);
    let c3 = clamp(vec2<i32>(uv_max * mip_dims), vec2<i32>(0), maxc);
    var far = textureLoad(hiz, c0, mip).r;
    far = min(far, textureLoad(hiz, c1, mip).r);
    far = min(far, textureLoad(hiz, c2, mip).r);
    far = min(far, textureLoad(hiz, c3, mip).r);
    // Reversed-Z: occluded when the sphere's nearest point is still behind (smaller reversed-Z
    // than) the farthest occluder covering the whole rect.
    return depth_near < far;
}

// --- Three-pass instancing-collapse entries (docs §3.6) ---
// The single-dispatch cull is split into COUNT -> EMIT -> SCATTER, one dispatch each (wgpu
// auto-barriers the storage writes between passes). COUNT/SCATTER run 1 thread / instance; EMIT
// runs 1 thread / global section. classify() (and occluded()) are pure, so COUNT and SCATTER
// see the SAME survivors — an instance counted in a section is scattered into that same run.

// Frustum + distance + LOD only (prepass / occluder set, and every shadow cascade). No Hi-Z.
@compute @workgroup_size(64)
fn count(@builtin(global_invocation_id) gid: vec3<u32>) {
    let idx = gid.x;
    if (idx >= cull.instance_count) {
        return;
    }
    let res = classify(idx);
    if (res.culled) {
        return;
    }
    count_sections(idx, res.level);
    whatif_triangles(idx, res.dist2);
}

// Frustum + distance + LOD + Hi-Z occlusion (color pass). COUNT half; must agree with the
// occlusion-view SCATTER (classify + occluded are pure, so both passes see the same survivors).
@compute @workgroup_size(64)
fn count_occlude(@builtin(global_invocation_id) gid: vec3<u32>) {
    let idx = gid.x;
    if (idx >= cull.instance_count) {
        return;
    }
    let res = classify(idx);
    if (res.culled) {
        return;
    }
    if (cull.occlusion != 0u && occluded(res.center, res.radius)) {
        return;
    }
    count_sections(idx, res.level);
    whatif_triangles(idx, res.dist2);
}

// EMIT (1 thread / GLOBAL section): allocate the section's contiguous out_records run and emit
// its one instanced DrawArgs. Layout-agnostic (no Hi-Z), so this single entry serves both the
// main and the occlusion pipelines.
@compute @workgroup_size(64)
fn emit_args(@builtin(global_invocation_id) gid: vec3<u32>) {
    let s = gid.x;
    if (s >= arrayLength(&sections)) {
        return;
    }
    let c = atomicLoad(&sec_count[s]);
    if (c == 0u) {
        return;
    }
    // Reserve the run BEFORE any cap check, so an un-emitted section still gets a UNIQUE base and
    // SCATTER never collides two sections at base 0. Repurpose sec_count[s] as the fill cursor.
    let base = atomicAdd(&counters[cull.variant_count], c);
    atomicStore(&sec_count[s], base);
    // Records overflow: run reserved past out_records -> emit no arg (SCATTER's bound check drops
    // the out-of-range writes). Never silently wrap; the counter readback surfaces it.
    if (base + c > arrayLength(&out_records)) {
        return;
    }
    let sec = sections[s];
    let v = sec.variant;
    if (v >= cull.variant_count) {
        return;
    }
    // Arg compaction unchanged — but the arg count is now (# surviving sections) not (# pairs),
    // so this per-variant cap is effectively unreachable (bounded by the registered section
    // count). Overflow is still dropped, never wrapped.
    let slot = atomicAdd(&counters[v], 1u);
    if (slot >= cull.variant_capacity) {
        return;
    }
    let out_i = v * cull.variant_capacity + slot;
    out_args[out_i] = DrawArgs(sec.index_count, c, sec.first_index, i32(sec.base_vertex), base);
    // PERF-005: recorded only for args that SURVIVED every cap above, so these are the counts
    // the GPU will really execute, not the counts the cull wanted to execute. Triangles =
    // index_count/3 per instance x the collapsed instance count. u32 holds ~4.3e9 — a frame
    // measured at tens of millions of triangles has three orders of magnitude of headroom.
    let tris = (sec.index_count / 3u) * c;
    atomicAdd(&counters[STAT_BASE + 1u], c);
    atomicAdd(&counters[STAT_BASE + 2u], tris);
    atomicAdd(&counters[STAT_BASE + 3u], 1u);
    atomicAdd(&counters[STAT_BASE + 4u + min(v, 1u)], 1u);
    atomicAdd(&counters[STAT_BASE + 6u + min(v, 1u)], tris);
}

// SCATTER half (main / prepass / shadow views). No Hi-Z.
@compute @workgroup_size(64)
fn scatter(@builtin(global_invocation_id) gid: vec3<u32>) {
    let idx = gid.x;
    if (idx >= cull.instance_count) {
        return;
    }
    let res = classify(idx);
    if (res.culled) {
        return;
    }
    scatter_sections(idx, res.level);
}

// SCATTER half (color pass, Hi-Z occlusion). Must apply the SAME tests as count_occlude.
@compute @workgroup_size(64)
fn scatter_occlude(@builtin(global_invocation_id) gid: vec3<u32>) {
    let idx = gid.x;
    if (idx >= cull.instance_count) {
        return;
    }
    let res = classify(idx);
    if (res.culled) {
        return;
    }
    if (cull.occlusion != 0u && occluded(res.center, res.radius)) {
        return;
    }
    scatter_sections(idx, res.level);
}
