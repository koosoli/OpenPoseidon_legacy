// REN-TEMP-001H — rigid dynamic-object velocity. Re-rasterises the frame's MOVING rigid
// draws (object_id != 0, unskinned, world changed since last frame) into the velocity
// target, over the camera-only reprojection the fullscreen pass already wrote.
//
// Standalone module on purpose: it needs BOTH frames' matrices and the frame UBO holds
// only the current jittered camera. Conventions match temporal.wgsl:
//   * raster position goes through the same reversed-Z flip as the scene
//     (frame.wgsl reverse_z: c.z = c.w - c.z), and uses the JITTERED current VP so the
//     fragment depth compares against the scene depth the jittered raster produced;
//   * the velocity itself is computed from UNJITTERED current/previous matrices;
//   * clip positions ride varyings and are divided per-fragment — perspective-correct
//     interpolation of clip coords is the standard motion-vector construction;
//   * output is UV-space motion, current -> previous.

struct ObjVelMats {
    jittered_vp: mat4x4<f32>,
    cur_vp: mat4x4<f32>,
    prev_vp: mat4x4<f32>,
    // x = relative depth tolerance for the visibility test (stored depth = near/dist,
    // so a RATIO compare is a distance-ratio compare), yzw unused.
    misc: vec4<f32>,
};

// One moving object: its camera-relative world NOW, and last frame's world relative to
// LAST frame's camera (which is exactly the space prev_vp consumes).
struct VelObj {
    cur_world: mat4x4<f32>,
    prev_world: mat4x4<f32>,
};

@group(0) @binding(0) var<uniform> mats: ObjVelMats;
@group(0) @binding(1) var<storage, read> objects: array<VelObj>;
@group(0) @binding(2) var scene_depth: texture_depth_2d;

struct VsOut {
    @builtin(position) pos: vec4<f32>,
    @location(0) cur_clip: vec4<f32>,
    @location(1) prev_clip: vec4<f32>,
};

@vertex
fn vs_velocity(@location(0) pos: vec3<f32>, @builtin(instance_index) inst: u32) -> VsOut {
    let o = objects[inst];
    let p = vec4<f32>(pos, 1.0);
    let world_cur = o.cur_world * p;
    let world_prev = o.prev_world * p;
    var raster = mats.jittered_vp * world_cur;
    raster.z = raster.w - raster.z; // reversed-Z, exactly like frame::reverse_z
    var out: VsOut;
    out.pos = raster;
    out.cur_clip = mats.cur_vp * world_cur;
    out.prev_clip = mats.prev_vp * world_prev;
    return out;
}

// --- REN-TEMP-001I: skinned variant ---------------------------------------------
// The current pose arrives as the vertex stream (this frame's bake output, sliced to
// the draw's baked base exactly like draw_one); the PREVIOUS pose is read from last
// frame's ping-ponged bake buffer by mesh-local vertex index. Baked vertices are
// camera-relative world space with their own frame's camera folded in, so no world
// matrices appear: current positions go through the current VPs, previous positions
// through the previous VP, by construction. Per-limb motion falls out of the bake —
// this is real skinned motion, not root motion in disguise.

struct SkinInst {
    prev_base: u32,
    pada: u32,
    padb: u32,
    padc: u32,
};
@group(0) @binding(3) var<storage, read> skin_insts: array<SkinInst>;
// Last frame's bake output as raw floats: WgrMeshVertex is 68 B = 17 floats, pos at 0.
@group(0) @binding(4) var<storage, read> prev_baked: array<f32>;

@vertex
fn vs_skinned_velocity(@location(0) pos: vec3<f32>,
                       @builtin(vertex_index) vi: u32,
                       @builtin(instance_index) inst: u32) -> VsOut {
    let si = skin_insts[inst];
    let base = (si.prev_base + vi) * 17u;
    let prev_pos = vec3<f32>(prev_baked[base], prev_baked[base + 1u], prev_baked[base + 2u]);
    var raster = mats.jittered_vp * vec4<f32>(pos, 1.0);
    raster.z = raster.w - raster.z;
    var out: VsOut;
    out.pos = raster;
    out.cur_clip = mats.cur_vp * vec4<f32>(pos, 1.0);
    out.prev_clip = mats.prev_vp * vec4<f32>(prev_pos, 1.0);
    return out;
}

@fragment
fn fs_velocity(in: VsOut) -> @location(0) vec2<f32> {
    // Visibility: this pass has no depth attachment (the scene depth is multisampled,
    // this target is 1x), so occlusion is a manual compare against the single-sample
    // far-depth resolve. Keep the fragment only where this object IS the visible
    // surface: at least (nearly) as near as what the scene stored there.
    let frag = vec2<i32>(in.pos.xy);
    let scene_d = textureLoad(scene_depth, frag, 0);
    let tol = mats.misc.x;
    if (in.pos.z < scene_d * (1.0 - tol)) {
        discard;
    }
    let cur_ndc = in.cur_clip.xy / max(in.cur_clip.w, 1e-6);
    if (in.prev_clip.w <= 1e-6) {
        // Behind the previous camera: no valid history for this surface. Zero is the
        // deliberate "no motion known" answer (the reactive mask is the eventual home
        // for this case).
        return vec2<f32>(0.0, 0.0);
    }
    let prev_ndc = in.prev_clip.xy / in.prev_clip.w;
    let d = cur_ndc - prev_ndc;
    return vec2<f32>(d.x * 0.5, d.y * -0.5);
}
