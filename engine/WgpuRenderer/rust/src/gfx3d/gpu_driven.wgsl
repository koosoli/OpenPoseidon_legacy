// GPU-driven object draw (docs/gpu-culling-and-depth-plan.md Stage 3). Consumes the cull
// compute's output: multi_draw_indexed_indirect over out_args, one sub-draw per surviving
// (section, instance) pair, first_instance = the record slot. The VS reads the record to
// find the instance transform (absolute world -> camera-relative in-shader) and the global
// section id; the FS folds that section's RAW material with the frame sun (the fold GL33 /
// the per-draw path do CPU-side) and hands it to the shared shade() so the lit look matches
// the per-draw path exactly.
//
// Rigid opaque only: terrain-conformed vegetation, skinned, and transparent draws stay on
// the CPU path (they never enter the retained instance set), so this VS has no conform.

#import frame::{frame, reverse_z, fog_factor}
#import shading::{shade, ShadeMaterial}
#import gbuffer::{oct_encode, a2c_coverage}
// Terrain conform (group 4 heightmap + surface_y/surface_grad), shared with shader3d /
// shadow_depth. Lets the GPU-driven VS conform ClipLand vegetation/fences to the ground per
// vertex, matching the per-draw path — so these objects no longer have to stay on the CPU.
#import conform::{surface_y, surface_grad}

// Pipeline-overridable constants (kept out of a per-draw binding), same as shader3d.
override linear: f32 = 0.0;
override foliage_shadow_ao: f32 = 0.35;
// 1 = alpha-to-coverage active on this (single) GPU-driven pipeline under MSAA. The set mixes
// opaque + cutout sections, so cutout-ness is decided per-fragment (sm.alpha_ref > 0, uniform
// per quad): cutout emits sharpened coverage, opaque emits 1.0 (full coverage -> unchanged).
override a2c: f32 = 0.0;

// InstanceGpu / RecordGpu / SectionMaterialGpu (cull.rs). conform0/1/2 is the terrain-conform
// plane (WgrDraw3D::conform* parity); conform2.z = mode (0 rigid, 1 ForestPlain plane, 2
// per-vertex ClipLand SurfaceY with conform0.x = bcSurfaceY).
struct Instance {
    world: mat4x4<f32>,
    center: vec4<f32>,
    model: u32,
    flags: u32,
    cull_radius: u32, // used by the cull compute only, not this VS
    _pad: u32,
    conform0: vec4<f32>,
    conform1: vec4<f32>,
    conform2: vec4<f32>,
};
struct Record {
    instance: u32,
    section: u32,
};
struct SectionMaterial {
    emissive: vec4<f32>,
    ambient: vec4<f32>,
    diffuse: vec4<f32>,
    specular: vec4<f32>, // w = specular power
    texture_slot: u32,
    sampler_idx: u32,
    alpha_ref: f32,
    // SMDI (or a family equivalent), 0 = none. Slot 0 is the white fallback, so this must be
    // tested rather than sampled blind: white would read as maximum specular everywhere,
    // which is the blowout the flat constant was scaled down to avoid.
    specular_slot: u32,
    // RVMAT Stage1, 0 = none. Same reason as specular_slot: slot 0 is the white fallback,
    // which would decode to a normal leaning hard in +X and light every surface wrongly.
    normal_slot: u32,
    // Multi's blend mask (0 = not layered) and its three further layer colours.
    mask_slot: u32,
    layer_slot: array<u32, 3>,
    // Explicit, because WGSL would insert this padding itself to 16-align layer_uv while
    // Rust would not, and the silent 12-byte disagreement corrupts every later field.
    _pad_layer0: u32,
    _pad_layer1: u32,
    _pad_layer2: u32,
    // Per layer, layer 0 at index 0: (scale_u, scale_v, offset_u, offset_v).
    layer_uv: array<vec4<f32>, 4>,
};

@group(1) @binding(0) var<storage, read> instances: array<Instance>;
@group(1) @binding(1) var<storage, read> records: array<Record>;
@group(1) @binding(2) var<storage, read> section_materials: array<SectionMaterial>;
// Per-tree crown centres (MODEL space, .xyz; .w unused), foliage-translucency-plan.md §9 Approach
// A. A merged forest mesh has one meaningless inst.center, so each forest vertex indexes this
// table (via its conform word) for its own tree's radial-normal centre. Register-once (cull.rs).
@group(1) @binding(3) var<storage, read> crown_centres: array<vec4<f32>>;
// Per-model BAKED sky visibility (LIT-020 Stage 2, docs/interior-sky-visibility-plan.md §3c).
// `sky_volume_meta` holds two vec4s per model — (bbox_min.xyz, first voxel index) and
// (bbox_max.xyz, 1 when a volume exists) — and `sky_volume_data` is every model's volume
// concatenated, x-major.
//
// A flat buffer with manual trilinear rather than a 3D texture: a 3D texture caps at 2048 on its
// largest axis (~128 models at 16 voxels deep) and R8Unorm 3D storage is not a core format. Eight
// fetches is a small price for a term that only modulates ambient.
@group(1) @binding(4) var<storage, read> sky_volume_meta: array<vec4<f32>>;
@group(1) @binding(5) var<storage, read> sky_volume_data: array<vec4<f32>>;

// Volume dimensions. Must match sky_bake::BakeSettings::default().dims — the one place a Rust
// value and its WGSL twin can silently disagree here, so a mismatch is a wrong-looking building
// rather than an error. Guarded by a field-order test on the Rust side.
const SKY_VOL_X: u32 = 32u;
const SKY_VOL_Y: u32 = 16u;
const SKY_VOL_Z: u32 = 32u;

// xyz = the direction the sky arrives from (model space), w = visibility.
fn sky_vol_fetch(base: u32, c: vec3<u32>) -> vec4<f32> {
    let i = base + c.x + c.y * SKY_VOL_X + c.z * SKY_VOL_X * SKY_VOL_Y;
    if (i >= arrayLength(&sky_volume_data)) {
        return vec4<f32>(0.0, 1.0, 0.0, 1.0);
    }
    return sky_volume_data[i];
}

// Trilinear tap of the volume, both channels at once.
fn sky_vol_sample(model: u32, model_pos: vec3<f32>) -> vec4<f32> {
    let m = model * 2u;
    if (m + 1u >= arrayLength(&sky_volume_meta) || sky_volume_meta[m + 1u].w < 0.5) {
        return vec4<f32>(0.0, 0.0, 0.0, 1.0);
    }
    let lo = sky_volume_meta[m].xyz;
    let hi = sky_volume_meta[m + 1u].xyz;
    let base = u32(sky_volume_meta[m].w);
    let dims = vec3<f32>(f32(SKY_VOL_X), f32(SKY_VOL_Y), f32(SKY_VOL_Z));
    // Half-voxel offset so samples sit on voxel CENTRES, where the bake evaluated them.
    let t = clamp((model_pos - lo) / max(hi - lo, vec3<f32>(1e-4)), vec3<f32>(0.0), vec3<f32>(1.0));
    let v = clamp(t * dims - vec3<f32>(0.5), vec3<f32>(0.0), dims - vec3<f32>(1.0));
    let i0 = vec3<u32>(floor(v));
    let i1 = min(i0 + vec3<u32>(1u), vec3<u32>(SKY_VOL_X - 1u, SKY_VOL_Y - 1u, SKY_VOL_Z - 1u));
    let f = v - floor(v);
    let c00 = mix(sky_vol_fetch(base, vec3<u32>(i0.x, i0.y, i0.z)), sky_vol_fetch(base, vec3<u32>(i1.x, i0.y, i0.z)), f.x);
    let c10 = mix(sky_vol_fetch(base, vec3<u32>(i0.x, i1.y, i0.z)), sky_vol_fetch(base, vec3<u32>(i1.x, i1.y, i0.z)), f.x);
    let c01 = mix(sky_vol_fetch(base, vec3<u32>(i0.x, i0.y, i1.z)), sky_vol_fetch(base, vec3<u32>(i1.x, i0.y, i1.z)), f.x);
    let c11 = mix(sky_vol_fetch(base, vec3<u32>(i0.x, i1.y, i1.z)), sky_vol_fetch(base, vec3<u32>(i1.x, i1.y, i1.z)), f.x);
    return mix(mix(c00, c10, f.y), mix(c01, c11, f.y), f.z);
}

// The baked incoming-sky direction at a model-space point, rotated to WORLD space, or a zero
// vector when the model has no volume. Unlike the five-direction per-frame steer, this is
// integrated over 41 directions and then trilinearly filtered, so it varies smoothly across a
// surface instead of jumping between a handful of discrete directions — which is what made the
// per-frame version produce hard shadow patches.
fn baked_sky_direction(model: u32, model_pos: vec3<f32>, rot: mat3x3<f32>) -> vec3<f32> {
    if (frame.skyvisc.x < 0.5) {
        return vec3<f32>(0.0);
    }
    let d = sky_vol_sample(model, model_pos).xyz;
    if (dot(d, d) < 1e-6) {
        return vec3<f32>(0.0);
    }
    return normalize(rot * d);
}

// Baked sky visibility at a MODEL-space position, trilinearly filtered. 1 (no darkening) when the
// model has no volume, which is also the correct answer while a bake is still pending.
fn baked_sky_visibility(model: u32, model_pos: vec3<f32>) -> f32 {
    if (frame.skyvisc.x < 0.5) {
        return 1.0;
    }
    let m = model * 2u;
    if (m + 1u >= arrayLength(&sky_volume_meta) || sky_volume_meta[m + 1u].w < 0.5) {
        return 1.0;
    }
    let vis = clamp(sky_vol_sample(model, model_pos).w, 0.0, 1.0);
    // strength blends toward the occluded result; floor keeps a sealed room playable, for the
    // same reason the per-frame path needs one — with the sun shadowed and no local lights, the
    // sky ambient is the only light in an OFP room.
    return max(1.0 - frame.skyvisc.y * (1.0 - vis), frame.skyvisc.z);
}

@group(2) @binding(0) var textures: binding_array<texture_2d<f32>>;
@group(3) @binding(0) var samplers: binding_array<sampler, 8>;

struct VsOut {
    @builtin(position) clip: vec4<f32>,
    @location(0) uv: vec2<f32>,
    @location(1) fog: f32,
    @location(2) world_pos: vec3<f32>, // camera-relative
    @location(3) normal: vec3<f32>,    // world space, outward
    @location(4) @interpolate(flat) section: u32,
    // 1 = this instance is vegetation (any canopy flag, i.e. MapType ∈ tree/bush/forest), so the
    // foliage lighting (leaf SSS + canopy AO) may apply; 0 = other cutouts (fences, grills, decals)
    // that must NOT pick up the leaf look. The alpha-test discard itself stays keyed on alpha_ref.
    @location(5) @interpolate(flat) is_veg: u32,
    // MODEL-space position + this instance's model id, for the baked sky-visibility volume.
    // Model space is the whole point: the volume is a property of the building, so the lookup
    // must happen in the building's own frame rather than the camera's — which is precisely what
    // the per-frame camera-space map could not do.
    @location(6) model_pos: vec3<f32>,
    @location(7) @interpolate(flat) model_id: u32,
    // The instance's model->world rotation, flat: the baked direction is stored in MODEL space
    // (that is what makes it reusable across every instance) so it has to be rotated to world
    // before it can steer a world-space irradiance lookup.
    @location(8) @interpolate(flat) model_rot0: vec3<f32>,
    @location(9) @interpolate(flat) model_rot1: vec3<f32>,
    @location(10) @interpolate(flat) model_rot2: vec3<f32>,
};

// WgrInstance::flags bits: vegetation canopy — bend cutout-section normals toward a radial crown
// normal. Bush and tree differ only in the bend + crown-Y knobs they pick (a tree's bounding-sphere
// centre sits mid-trunk, so it wants a larger lift). Mirror WgrInstanceFlags in wgpu_renderer.hpp.
const INST_CANOPY_BUSH: u32 = 1u;
const INST_CANOPY_TREE: u32 = 2u;
// A merged multi-tree forest mesh (§9 Approach A): per-vertex crown centre instead of inst.center.
const INST_CANOPY_FOREST: u32 = 4u;

// Absolute ForestPlain bilinear ground height at world xz (the mode-1 conform plane; identical to
// the per-vertex conform below and ObjectClasses.cpp ComputeConformPlane). Used to conform a
// forest tree's crown centre to the SAME ground its vertices sit on, so a forest on a slope
// doesn't skew every radial normal.
fn forest_plane_y(inst: Instance, xz: vec2<f32>) -> f32 {
    let s = inst.conform0.x;
    let xIn = xz.x * s + inst.conform0.y;
    let zIn = xz.y * s + inst.conform0.z;
    let y00 = inst.conform1.x; let y10 = inst.conform1.y;
    let d1000 = inst.conform1.z; let d0100 = inst.conform1.w;
    let d1011 = inst.conform2.x; let d0111 = inst.conform2.y;
    let triA = xIn <= 1.0 - zIn;
    return select(y10 + d0111 - d1011 * xIn - zIn * d0111,
                  y00 + d1000 * zIn + d0100 * xIn, triA);
}

@vertex
fn vs_gpu(
    @builtin(instance_index) rec_slot: u32,
    @location(0) pos: vec3<f32>,
    @location(1) norm: vec3<f32>,
    @location(2) uv: vec2<f32>,
    @location(5) conform_sel: u32, // per-vertex conform selector (mode 2): 0 rigid / 1 keep / 2 on
) -> VsOut {
    let rec = records[rec_slot];
    let inst = instances[rec.instance];
    let world = inst.world;
    // Absolute -> camera-relative (the per-draw path pre-offsets on the CPU; here the
    // transform is absolute, so subtract cam_pos). view has its translation zeroed.
    let world_pos_abs = world * vec4<f32>(pos, 1.0);
    var world_pos = world_pos_abs.xyz - frame.cam_pos.xyz;
    // Normals arrive already negated (MeshBuild stores -Norm); rotate to world and light
    // as-is, matching vs_main / GL33.
    let rot = mat3x3<f32>(world[0].xyz, world[1].xyz, world[2].xyz);
    var normal_ws = rot * norm;
    // Terrain conform: the shared base mesh is uploaded undeformed and conformed here per
    // instance, exactly like shader3d::vs_main. Heights are evaluated in ABSOLUTE world xz
    // (world_pos_abs.xz) and written back camera-relative. conform2.z = mode.
    let mode = inst.conform2.z;
    if (mode > 1.5) {
        // Mode 2: individual ClipLand vegetation, conformed per vertex to SurfaceY (matching
        // Object::Animate). conform_sel: 1 = ClipLandKeep (keep height above the surface),
        // 2 = ClipLandOn (pin onto it), 0 = rigid. conform0.x = bcSurfaceY.
        let sy = surface_y(world_pos_abs.xz);
        if (conform_sel == 1u) {
            // world.y_abs = SurfaceY + undeformedWorldY - bcSurfaceY; the cam.y offset cancels
            // between the two camera-relative terms.
            world_pos.y = sy + world_pos.y - inst.conform0.x;
        } else if (conform_sel == 2u) {
            world_pos.y = sy - frame.cam_pos.y;
        }
        // Tilt the conformed vertex's normal by the terrain slope so lighting follows the
        // ground (same shear as mode 1). Rigid verts (sel 0) keep theirs.
        if (conform_sel != 0u) {
            let g = surface_grad(world_pos_abs.xz);
            normal_ws = vec3<f32>(normal_ws.x - g.x * normal_ws.y, normal_ws.y,
                                  normal_ws.z - g.y * normal_ws.y);
        }
    } else if (mode > 0.5) {
        // Mode 1: ForestPlain bilinear plane fit (ObjectClasses.cpp ComputeConformPlane).
        let s = inst.conform0.x;                     // inv_land_grid
        let xIn = world_pos_abs.x * s + inst.conform0.y;  // *invLand - xf
        let zIn = world_pos_abs.z * s + inst.conform0.z;  // *invLand - zf
        let y00 = inst.conform1.x; let y10 = inst.conform1.y;
        let d1000 = inst.conform1.z; let d0100 = inst.conform1.w;
        let d1011 = inst.conform2.x; let d0111 = inst.conform2.y;
        let triA = xIn <= 1.0 - zIn;
        let py = select(y10 + d0111 - d1011 * xIn - zIn * d0111,
                        y00 + d1000 * zIn + d0100 * xIn, triA);
        // Camera-relative conformed height: absolute plane height + the vertex's own model
        // height above surface (conform0.w = BoundingCenter().y), minus cam.y.
        world_pos.y = py - frame.cam_pos.y + pos.y + inst.conform0.w;
        // Tilt the undeformed normal by the plane gradient (inverse-transpose of the affine
        // y-shear) so lighting matches the CPU's post-deform InvalidateNormals.
        let gx = select(-d1011, d0100, triA) * s;
        let gz = select(-d0111, d1000, triA) * s;
        normal_ws = vec3<f32>(normal_ws.x - gx * normal_ws.y, normal_ws.y, normal_ws.z - gz * normal_ws.y);
    }
    // Spherical / canopy normals (docs/foliage-translucency-plan.md Stage 3): bend a leaf's normal
    // toward a radial "crown" normal from the object centre, so the low-poly canopy shades as a
    // rounded volume instead of splitting hard per-card — this is what lets a card facing away from
    // the sun still light (fixing foliage that stays dark in full sun). Gated to canopy instances
    // (bush/tree flag) AND cutout (leaf) sections, so a tree's solid trunk keeps its real normal.
    // Bush and tree pick different bend + crown-Y knobs (a tree's bounding-sphere centre sits mid-
    // trunk, so it wants a larger lift). Applied at all distances (a normal is smoothing, not the
    // glowy SSS fill), so it also helps distant billboards. Blended here — after conform, in the
    // same world/outward space fs_gpu expects — so no fragment-shader change.
    let canopy = inst.flags & (INST_CANOPY_BUSH | INST_CANOPY_TREE | INST_CANOPY_FOREST);
    if (canopy != 0u && section_materials[rec.section].alpha_ref > 0.0) {
        // Forests share the tree bend/crown-Y knobs (both shade around a mid-crown centre, unlike a
        // bush whose centre is the whole blob).
        let tree_like = (inst.flags & (INST_CANOPY_TREE | INST_CANOPY_FOREST)) != 0u;
        var bend = frame.foliageb.y;    // bush bend
        var crown_y = frame.foliageb.z; // bush crown-Y lift
        if (tree_like) {
            bend = frame.foliagec.y;    // tree bend
            crown_y = frame.foliagec.z; // tree crown-Y lift
        }
        if (bend > 0.0) {
            var crown: vec3<f32>;
            if ((inst.flags & INST_CANOPY_FOREST) != 0u) {
                // §9 Approach A: a merged forest's inst.center spans many trees, so each vertex
                // carries its own tree's crown centre (model space) in the conform word, indexing
                // crown_centres. Transform to camera-relative world; conform its Y to the same
                // mode-1 ground plane the vertices use (mode 0 skewed forests are pre-placed rigid).
                let cm = crown_centres[conform_sel].xyz;
                let cw = world * vec4<f32>(cm, 1.0);
                crown = cw.xyz - frame.cam_pos.xyz;
                if (mode > 0.5 && mode < 1.5) {
                    crown.y = forest_plane_y(inst, cw.xz) - frame.cam_pos.y + cm.y + inst.conform0.w;
                }
            } else {
                crown = inst.center.xyz - frame.cam_pos.xyz;
            }
            crown.y = crown.y + crown_y;
            let d = world_pos - crown;
            let dl = length(d);
            if (dl > 1e-3) {
                normal_ws = normalize(mix(normal_ws, d / dl, bend));
            }
        }
    }
    var out: VsOut;
    out.clip = reverse_z(frame.proj * frame.view * vec4<f32>(world_pos, 1.0));
    out.uv = uv;
    out.world_pos = world_pos;
    out.normal = normal_ws;
    out.fog = fog_factor(length(world_pos));
    out.section = rec.section;
    // Vegetation = any canopy flag (bush/tree/forest cover the whole vegetation MapType set);
    // gates the foliage lighting in fs_gpu so non-plant cutouts don't get the leaf look.
    out.is_veg = select(0u, 1u, canopy != 0u);
    // Undeformed model-space position: the baked volume is a property of the AUTHORED model, so
    // the lookup must use the authored vertex, not a terrain-conformed one.
    out.model_pos = pos;
    out.model_id = inst.model;
    out.model_rot0 = world[0].xyz;
    out.model_rot1 = world[1].xyz;
    out.model_rot2 = world[2].xyz;
    return out;
}

@fragment
fn fs_gpu(in: VsOut) -> @location(0) vec4<f32> {
    let dwx = dpdx(in.world_pos);
    let dwy = dpdy(in.world_pos);
    // Keep the GPU-driven set on the same reflected-view waterline clip as terrain
    // and retained objects. Without this, below-water buildings/vehicles could be
    // reflected or write depth in front of valid above-water reflection geometry.
    let clip_len2 = dot(frame.clip_plane.xyz, frame.clip_plane.xyz);
    if (clip_len2 > 0.0 &&
        dot(frame.clip_plane.xyz, in.world_pos + frame.cam_pos.xyz) + frame.clip_plane.w < 0.0) {
        discard;
    }
    let sm = section_materials[in.section];
    // The section id is uniform across a derivative quad (one section per primitive), so the
    // bindless index stays uniform and implicit-mip sampling is legal.
    // Multi's four-layer masked blend. Each layer is authored at its own UV scale --
    // on Takistan's brick houses the brick tiles far more densely than the rock beside
    // it, and that difference is the detail the surface reads as, so the layer's own
    // transform is applied rather than the section's raw uv. The mask's RGB weights
    // layers 1-3 and whatever it leaves unpainted belongs to layer 0, which is the same
    // convention the terrain's LCA composition uses.
    //
    // mask_slot == 0 means "not a layered material" and takes layer 0 alone, which is
    // every non-Multi section and is byte-identical to the previous behaviour.
    var base = textureSample(textures[sm.texture_slot],
                             samplers[sm.sampler_idx],
                             in.uv * sm.layer_uv[0].xy + sm.layer_uv[0].zw);
    if (sm.mask_slot != 0u) {
        let mask = textureSample(textures[sm.mask_slot], samplers[sm.sampler_idx], in.uv).rgb;
        var weight = 0.0;
        var blended = vec3<f32>(0.0);
        for (var i = 0u; i < 3u; i = i + 1u) {
            // Slot 0 is the white fallback. A layer the material does not name must
            // contribute NOTHING and leave its mask weight to layer 0; sampling it blind
            // blends white into the surface, which blows the building out and drags the
            // whole frame's auto-exposure down with it.
            if (sm.layer_slot[i] == 0u) {
                continue;
            }
            let uv_i = in.uv * sm.layer_uv[i + 1u].xy + sm.layer_uv[i + 1u].zw;
            let layer = textureSample(textures[sm.layer_slot[i]], samplers[sm.sampler_idx], uv_i).rgb;
            blended = blended + layer * mask[i];
            weight = weight + mask[i];
        }
        // The unpainted remainder belongs to layer 0. Where the mask over-paints, the
        // layers are renormalised rather than allowed to sum past one.
        if (weight > 1.0) {
            blended = blended / weight;
            weight = 1.0;
        }
        base = vec4<f32>(base.rgb * (1.0 - weight) + blended, base.a);
    }
    // A2C (MSAA): cutout sections (sm.alpha_ref > 0, uniform per quad) emit a sharpened coverage
    // alpha; opaque sections keep full coverage (1.0). Without A2C, the classic hard discard.
    var out_a = base.a;
    if (a2c > 0.5) {
        if (sm.alpha_ref > 0.0) {
            out_a = a2c_coverage(base.a, sm.alpha_ref);
            if (out_a <= 0.0) {
                discard;
            }
        } else {
            out_a = 1.0;
        }
    } else if (base.a < sm.alpha_ref) {
        discard;
    }
    // Fold RAW material x the frame sun, reproducing GL33's UploadVSMaterialConstants
    // (EngineWgpu.cpp) in-shader: sun_* = sun x material (legacy path only), light_* = raw
    // material (local lights), specular = sun_diffuse x material specular. On the sky-lit HDR
    // path shade() ignores sun_*, so the emissive/light/specular terms are what carry.
    var m: ShadeMaterial;
    // Imported OFP/A3 materials use the legacy emissive lane for a mixture of
    // real signal lamps and fixed-function brightness compensation. The latter
    // was baked into plant materials to survive OFP's old lighting model; in
    // HDR it becomes source radiance and makes entire forests glow at night.
    // A real fixture, however, still needs a visible emissive core. Keep that
    // core bounded to a sensible scene-linear value, while vegetation receives
    // no legacy emissive compensation at all.
    let emissive_peak = max(sm.emissive.r, max(sm.emissive.g, sm.emissive.b));
    if (in.is_veg != 0u) {
        // Legacy vegetation emissive was a fixed-function visibility boost, not
        // source radiance. Preserve only a small starlight residual so foliage
        // is dark at night without collapsing to an entirely black silhouette.
        m.emissive = sm.emissive.rgb * 0.10;
    } else {
        m.emissive = sm.emissive.rgb * min(1.0, 0.50 / max(emissive_peak, 1e-4));
    }
    m.sun_ambient = frame.sun_ambient.rgb * sm.ambient.rgb;
    m.sun_diffuse = frame.sun_diffuse.rgb * sm.diffuse.rgb;
    m.light_diffuse = sm.diffuse.rgb;
    m.light_ambient = sm.ambient.rgb;
    m.specular = frame.sun_diffuse.rgb * sm.specular.rgb;
    m.spec_power = sm.specular.w;
    // Per-texel specular. SMDI's G is the specular LEVEL and B is gloss -- measured, not
    // folklore: across 40 sampled SMDI textures R and A are constant 255 while G and B vary
    // (MaterialChannels.hpp). Only G is consumed. Turning B into a Blinn-Phong exponent is a
    // conversion with its own evidence requirement, the same one that keeps
    // GlossToRoughnessUnvalidated out of the translation path; the map is bound and the
    // channel is there when that question is answered.
    m.spec_mask = 1.0;
    if (sm.specular_slot != 0u) {
        m.spec_mask = textureSample(textures[sm.specular_slot], samplers[sm.sampler_idx], in.uv).g;
    }
    // Foliage lighting (leaf SSS + canopy self-occlusion AO) applies only to real VEGETATION
    // cutouts (Stage 2 MapType gate, carried per-instance via the canopy flag) — other alpha-tested
    // cutouts (fences, grills, road/footprint decals) light normally. GPU-driven set is
    // opaque/cutout, never the glass path. The alpha discard above stays keyed on alpha_ref.
    let veg_cutout = in.is_veg != 0u && sm.alpha_ref > 0.0;
    // Debug: the baked volume as greyscale, before fog, matching the per-frame path's reach view.
    // The SHAPED value (strength + floor applied), not the raw one, because that is what the
    // lighting will actually use — a debug view of a number the renderer does not consume is how
    // you end up tuning against the wrong thing.
    if (frame.skyvisc.x > 0.5 && frame.skyvisc.w > 0.5) {
        return vec4<f32>(vec3<f32>(baked_sky_visibility(in.model_id, in.model_pos)), out_a);
    }
    // Stage1 normal mapping. The retained mesh carries no authored tangent stream, so the
    // frame comes from the interpolated UV0 / world-position derivatives -- the same
    // recovery the per-draw path uses, and the reason this needs no vertex-format change.
    // Decode matches it too: NOHQ stores X in A and Y in G, Z is reconstructed.
    let geometric_normal = normalize(in.normal);
    var shading_normal = geometric_normal;
    if (sm.normal_slot != 0u) {
        let normal_sample = textureSample(textures[sm.normal_slot], samplers[sm.sampler_idx], in.uv);
        let normal_ts_xy = vec2<f32>(normal_sample.a, normal_sample.g) * 2.0 - vec2<f32>(1.0);
        let normal_ts = vec3<f32>(normal_ts_xy, sqrt(max(0.0, 1.0 - dot(normal_ts_xy, normal_ts_xy))));
        let duv_dx = dpdx(in.uv);
        let duv_dy = dpdy(in.uv);
        // Scale-invariant cotangent frame. Building the tangent from a raw product of
        // derivatives does not survive close range: the world-space footprint and the
        // UV footprint both shrink as the surface approaches, so their product shrinks
        // quadratically and any absolute epsilon eventually rejects a perfectly good
        // frame -- the normal map visibly vanishing as you walk up to a wall.
        //
        // Crossing the position derivatives with the geometric normal first gives a
        // frame that is perpendicular to it by construction (so no orthonormalisation
        // step, and no oblique-frame facets on coarse LODs), and normalising both axes
        // by their shared maximum makes the result depend only on the frame's shape,
        // never on its magnitude. Handedness falls out of the construction, so mirrored
        // UV islands still light correctly.
        let dp_perp_y = cross(dwy, geometric_normal);
        let dp_perp_x = cross(geometric_normal, dwx);
        let tangent_raw = dp_perp_y * duv_dx.x + dp_perp_x * duv_dy.x;
        let bitangent_raw = dp_perp_y * duv_dx.y + dp_perp_x * duv_dy.y;
        let frame_scale2 = max(dot(tangent_raw, tangent_raw), dot(bitangent_raw, bitangent_raw));
        if (frame_scale2 > 0.0) {
            let inv_scale = inverseSqrt(frame_scale2);
            let tangent = tangent_raw * inv_scale;
            let bitangent = bitangent_raw * inv_scale;
            shading_normal =
                normalize(tangent * normal_ts.x + bitangent * normal_ts.y + geometric_normal * normal_ts.z);
        }
    }
    let rgb = shade(
        base.rgb, m, shading_normal, in.world_pos, in.fog, dwx, dwy, linear,
        // Per-model baked sky visibility (LIT-020 Stage 2), 1 when the model has no volume.
        baked_sky_visibility(in.model_id, in.model_pos),
        baked_sky_direction(in.model_id, in.model_pos,
            mat3x3<f32>(in.model_rot0, in.model_rot1, in.model_rot2)),
        foliage_shadow_ao,
        veg_cutout, false, veg_cutout, in.clip.xy,
    );
    return vec4<f32>(rgb, out_a);
}

// Depth+normal PREPASS fragment: no shading — writes ONLY the view-space octahedral normal
// into the Rg16Float G-buffer (depth is written by the fixed-function stage). Mirrors
// shader3d's fs_prepass, but reads the per-section cutout threshold + bindless texture from
// section_materials (the GPU-driven path is per-section, not per-instance). Cutout foliage
// applies the SAME discard as fs_gpu so prepass coverage matches the colour pass.
@fragment
fn fs_gpu_prepass(in: VsOut) -> @location(0) vec2<f32> {
    let sm = section_materials[in.section];
    if (sm.alpha_ref > 0.0) {
        if (textureSample(textures[sm.texture_slot], samplers[sm.sampler_idx], in.uv).a < sm.alpha_ref) {
            discard;
        }
    }
    // in.normal is world space; the view matrix's translation is zeroed, so transforming the
    // direction gives view space (matches shader3d::fs_prepass).
    let n_view = (frame.view * vec4<f32>(normalize(in.normal), 0.0)).xyz;
    return oct_encode(normalize(n_view));
}

// A2C prepass twin (MSAA), mirroring shader3d::fs_prepass_a2c for the GPU-driven set. Returns a
// vec4 so location(0)'s .a carries coverage for alpha-to-coverage; cutout emits the sharpened
// coverage (matching fs_gpu), opaque emits 1.0. Writes depth to exactly the covered samples so
// terrain fills the rest (no edge halo). The whole set goes through this pipeline, hence the
// per-fragment opaque/cutout split rather than a pipeline override.
@fragment
fn fs_gpu_prepass_a2c(in: VsOut) -> @location(0) vec4<f32> {
    let sm = section_materials[in.section];
    var cov = 1.0;
    if (sm.alpha_ref > 0.0) {
        let alpha = textureSample(textures[sm.texture_slot], samplers[sm.sampler_idx], in.uv).a;
        cov = a2c_coverage(alpha, sm.alpha_ref);
        if (cov <= 0.0) {
            discard;
        }
    }
    let n_view = (frame.view * vec4<f32>(normalize(in.normal), 0.0)).xyz;
    let oct = oct_encode(normalize(n_view));
    return vec4<f32>(oct.x, oct.y, 0.0, cov);
}
