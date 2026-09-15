// GPU-driven cascade shadow depth pass (docs/gpu-culling-and-depth-plan.md §6, multi-view).
// The retained scene casts into a cascade's depth map straight from the cull compute's output:
// multi_draw_indexed_indirect over that cascade's out_args, one sub-draw per surviving
// (section, instance), first_instance = the record slot. The VS recovers the instance transform
// (absolute world -> camera-relative in-shader, then light_vp) and conforms terrain vegetation
// per vertex exactly like the colour path's vs_gpu, so the shadow silhouette matches the object.
// Depth-only, forward-Z (light_vp already yields NDC z in [0,1]; no reversed-Z). One pipeline
// serves both opaque variants: cutout foliage discards below the per-section alpha_ref, solids
// (alpha_ref = 0) never do.

// Group(4) terrain heightmap + surface_y/surface_grad, shared with shader3d / shadow_depth /
// gpu_driven, so the GPU shadow silhouette conforms ClipLand vegetation to the same ground.
#import conform::{surface_y}

// group(0): this cascade's light view-projection + the camera world position the casters are
// relative to (absolute world xz for surface_y = camera-relative xz + cam_pos.xz). Matches
// ShadowPassUbo (mod.rs), bound with a per-cascade dynamic offset.
struct PassData {
    light_vp: mat4x4<f32>,
    cam_pos: vec4<f32>,
};

// Mirrors InstanceGpu / RecordGpu / SectionMaterialGpu (cull.rs) — identical to gpu_driven.wgsl.
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
    specular: vec4<f32>,
    texture_slot: u32,
    sampler_idx: u32,
    alpha_ref: f32,
    // The shadow pass needs only the cutout alpha, but the layout must match
    // SectionMaterialGpu exactly or every field after this one reads the wrong bytes.
    specular_slot: u32,
    normal_slot: u32,
    // Multi's blend mask (0 = not layered) and its three further layer colours.
    mask_slot: u32,
    layer_slot: array<u32, 3>,
    // Explicit, because WGSL would insert this padding itself to 16-align layer_uv while
    // Rust would not, and the silent 12-byte disagreement corrupts every later field.
    // Per-section flags (DZ-003 reflective bit). Named, not padding, so this mirror of
    // SectionMaterialGpu keeps matching gpu_driven.wgsl field for field; the shadow pass
    // has no use for it.
    flags: u32,
    // MAT-048: the further layers' own normal maps. The shadow pass never shades, so it has
    // no use for them either -- they are here so this mirror stays field-for-field.
    layer_normal_slot: array<u32, 3>,
    _pad_layer0: u32,
    _pad_layer1: u32,
    _pad_layer2: u32,
    // Per layer, layer 0 at index 0: (scale_u, scale_v, offset_u, offset_v).
    layer_uv: array<vec4<f32>, 4>,
    // RFG-072: per layer, layer 0 at index 0: the LINEAR colour multiplier on the layer's
    // tile (Enfusion `Color_N`); identity for every other material.
    layer_colour: array<vec4<f32>, 4>,
    // NormalPower + pads: the shadow pass never shades, present only so this mirror
    // stays field-for-field with SectionMaterialGpu (272 bytes, 16 stride).
    normal_power: f32,
    _pad_np0: u32,
    _pad_np1: u32,
    _pad_np2: u32,
    // Crown AO lanes: the shadow pass never shades, present only so this mirror
    // stays field-for-field with SectionMaterialGpu (304 bytes, 16 stride).
    crown_ao_p0: vec4<f32>,
    crown_ao_p1: vec4<f32>,
};

@group(0) @binding(0) var<uniform> pass_data: PassData;
@group(1) @binding(0) var<storage, read> instances: array<Instance>;
@group(1) @binding(1) var<storage, read> records: array<Record>;
@group(1) @binding(2) var<storage, read> section_materials: array<SectionMaterial>;
@group(2) @binding(0) var textures: binding_array<texture_2d<f32>>;
@group(3) @binding(0) var samplers: binding_array<sampler, 8>;

struct VsOut {
    @builtin(position) clip: vec4<f32>,
    @location(0) uv: vec2<f32>,
    @location(1) @interpolate(flat) section: u32,
};

@vertex
fn vs_gpu_shadow(
    @builtin(instance_index) rec_slot: u32,
    @location(0) pos: vec3<f32>,
    @location(2) uv: vec2<f32>,
    @location(5) conform_sel: u32, // per-vertex conform selector (mode 2): 0 rigid / 1 keep / 2 on
) -> VsOut {
    let rec = records[rec_slot];
    let inst = instances[rec.instance];
    let world = inst.world;
    // Absolute -> camera-relative (light_vp is relative to pass_data.cam_pos). Conform runs in
    // the same camera-relative space + absolute world xz, mirroring gpu_driven::vs_gpu exactly,
    // so the depth silhouette matches the shaded object. Normals are irrelevant for depth.
    let world_pos_abs = world * vec4<f32>(pos, 1.0);
    var world_pos = world_pos_abs.xyz - pass_data.cam_pos.xyz;
    let mode = inst.conform2.z;
    if (mode > 1.5) {
        // Mode 2: individual ClipLand vegetation, conformed per vertex to SurfaceY.
        let sy = surface_y(world_pos_abs.xz);
        if (conform_sel == 1u) {
            world_pos.y = sy + world_pos.y - inst.conform0.x;
        } else if (conform_sel == 2u) {
            world_pos.y = sy - pass_data.cam_pos.y;
        }
    } else if (mode > 0.5) {
        // Mode 1: ForestPlain bilinear plane fit (ObjectClasses.cpp ComputeConformPlane).
        let s = inst.conform0.x;
        let xIn = world_pos_abs.x * s + inst.conform0.y;
        let zIn = world_pos_abs.z * s + inst.conform0.z;
        let y00 = inst.conform1.x; let y10 = inst.conform1.y;
        let d1000 = inst.conform1.z; let d0100 = inst.conform1.w;
        let d1011 = inst.conform2.x; let d0111 = inst.conform2.y;
        let triA = xIn <= 1.0 - zIn;
        let py = select(y10 + d0111 - d1011 * xIn - zIn * d0111,
                        y00 + d1000 * zIn + d0100 * xIn, triA);
        world_pos.y = py - pass_data.cam_pos.y + pos.y + inst.conform0.w;
    }
    var out: VsOut;
    out.clip = pass_data.light_vp * vec4<f32>(world_pos, 1.0);
    out.uv = uv;
    out.section = rec.section;
    return out;
}

@fragment
fn fs_gpu_shadow(in: VsOut) {
    // Cutout foliage: discard below the per-section threshold (solids carry alpha_ref = 0 and
    // never sample/discard). The section id is flat across the primitive, so the bindless index
    // is uniform for the quad.
    let sm = section_materials[in.section];
    if (sm.alpha_ref > 0.0) {
        if (textureSample(textures[sm.texture_slot], samplers[sm.sampler_idx], in.uv).a < sm.alpha_ref) {
            discard;
        }
    }
}

// REN-GI-002: the SUN PROXY (a reflective shadow map). Same records, same conform, same alpha
// test as the depth pass above; additionally writes the surface's albedo (texture x material
// diffuse) and its world normal, so the probe integrator can gather the first sun bounce with
// colour from every sunlit surface the sun sees -- walls and trees included, which the
// top-down dome maps never could.
struct RsmVsOut {
    @builtin(position) clip: vec4<f32>,
    @location(0) uv: vec2<f32>,
    @location(1) @interpolate(flat) section: u32,
    @location(2) normal: vec3<f32>,
};

struct RsmOut {
    @location(0) albedo: vec4<f32>,
    @location(1) normal: vec4<f32>,
};

@vertex
fn vs_gpu_rsm(
    @builtin(instance_index) rec_slot: u32,
    @location(0) pos: vec3<f32>,
    @location(1) nrm: vec3<f32>,
    @location(2) uv: vec2<f32>,
    @location(5) conform_sel: u32,
) -> RsmVsOut {
    let rec = records[rec_slot];
    let inst = instances[rec.instance];
    let world = inst.world;
    let world_pos_abs = world * vec4<f32>(pos, 1.0);
    var world_pos = world_pos_abs.xyz - pass_data.cam_pos.xyz;
    let mode = inst.conform2.z;
    if (mode > 1.5) {
        let sy = surface_y(world_pos_abs.xz);
        if (conform_sel == 1u) {
            world_pos.y = sy + world_pos.y - inst.conform0.x;
        } else if (conform_sel == 2u) {
            world_pos.y = sy - pass_data.cam_pos.y;
        }
    } else if (mode > 0.5) {
        let s = inst.conform0.x;
        let xIn = world_pos_abs.x * s + inst.conform0.y;
        let zIn = world_pos_abs.z * s + inst.conform0.z;
        let y00 = inst.conform1.x; let y10 = inst.conform1.y;
        let d1000 = inst.conform1.z; let d0100 = inst.conform1.w;
        let d1011 = inst.conform2.x; let d0111 = inst.conform2.y;
        let triA = xIn <= 1.0 - zIn;
        let py = select(y10 + d0111 - d1011 * xIn - zIn * d0111,
                        y00 + d1000 * zIn + d0100 * xIn, triA);
        world_pos.y = py - pass_data.cam_pos.y + pos.y + inst.conform0.w;
    }
    var out: RsmVsOut;
    out.clip = pass_data.light_vp * vec4<f32>(world_pos, 1.0);
    out.uv = uv;
    out.section = rec.section;
    out.normal = normalize((world * vec4<f32>(nrm, 0.0)).xyz);
    return out;
}

@fragment
fn fs_gpu_rsm(in: RsmVsOut, @builtin(front_facing) front: bool) -> RsmOut {
    let sm = section_materials[in.section];
    let tex = textureSample(textures[sm.texture_slot], samplers[sm.sampler_idx], in.uv);
    if (sm.alpha_ref > 0.0 && tex.a < sm.alpha_ref) {
        discard;
    }
    var n = normalize(in.normal);
    if (!front) {
        n = -n;
    }
    var out: RsmOut;
    out.albedo = vec4<f32>(tex.rgb * sm.diffuse.rgb, 1.0);
    out.normal = vec4<f32>(n * 0.5 + 0.5, 1.0);
    return out;
}
