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

// --- REN-TEMP-001T: vegetation-sway variant --------------------------------------
// The wind sway is a closed-form vertex deformation (frame.wgsl veg_sway_offset), so
// swaying vegetation gets REAL motion vectors: evaluate the same formula at the
// current and the previous frame's wind parameters and take the difference. Without
// this, DLSS reads sub-pixel sway as noise and freezes distant canopies (owner
// report 2026-08-30: trees stop moving at distance under the upscaler).
//
// The formula below is a copy of frame.wgsl `veg_sway_offset` with the frame-UBO
// lanes replaced by this pass's own uniform; keep the two in sync.

struct VegVelParams {
    sway_cur: vec4<f32>,   // amp, t (time*speed), wind_dir.x, wind_dir.z
    sway_cur2: vec4<f32>,  // gust fraction, leaf flutter, stiffness exponent, pad
    sway_prev: vec4<f32>,
    sway_prev2: vec4<f32>,
    cams: vec4<f32>,       // cam_cur.xz, cam_prev.xz (phase seed needs ABSOLUTE xz)
    cam_delta: vec4<f32>,  // cam_cur - cam_prev, xyz (prev-cam-relative rebase)
};

struct VegInst {
    world: mat4x4<f32>, // camera-relative current world (static objects: abs is shared)
    params: vec4<f32>,  // x = leaf (alpha-cutout section: extra flutter), yzw unused
};

@group(0) @binding(5) var<uniform> veg: VegVelParams;
@group(0) @binding(6) var<storage, read> veg_insts: array<VegInst>;

const VEG_SWAY_HEIGHT_REF: f32 = 12.0;

fn veg_sway_at(model_y: f32, phase_xz: vec2<f32>, leaf: f32, p: vec4<f32>, p2: vec4<f32>) -> vec3<f32> {
    let amp = p.x;
    if (amp <= 0.0) {
        return vec3<f32>(0.0, 0.0, 0.0);
    }
    let t = p.y;
    let dir = vec2<f32>(p.z, p.w);
    let hn = clamp(max(model_y, 0.0) / VEG_SWAY_HEIGHT_REF, 0.0, 3.0);
    let coherent_hs = model_y / VEG_SWAY_HEIGHT_REF * (1.6 / max(p2.z, 0.25));
    let hs = select(pow(hn, p2.z), coherent_hs, leaf < 0.0);
    let ph = dot(phase_xz, vec2<f32>(0.37, 0.29));
    let gust = 1.0 + 0.65 * p2.x;
    let s = sin(t * 0.9 + ph) * 0.65 + sin(t * 1.63 + ph * 1.7) * 0.35;
    var bend = dir * (amp * hs * s * gust);
    if (leaf > 0.0) {
        let f = p2.y * leaf;
        let perp = vec2<f32>(-dir.y, dir.x);
        bend = bend + (dir * sin(t * 5.7 + ph * 3.1) + perp * sin(t * 4.3 + ph * 2.3)) *
                          (min(amp * hs * 0.22 * gust, 0.025) * f);
    }
    return vec3<f32>(bend.x, 0.0, bend.y);
}

@vertex
fn vs_veg_velocity(@location(0) pos: vec3<f32>, @builtin(instance_index) inst: u32) -> VsOut {
    let vi = veg_insts[inst];
    let p = vec4<f32>(pos, 1.0);
    let rel = vi.world * p;
    // Same phase seed as the scene VS: object origin back to ABSOLUTE world xz.
    let phase = vi.world[3].xz + veg.cams.xy;
    let leaf = vi.params.x;
    let s_cur = veg_sway_at(pos.y, phase, leaf, veg.sway_cur, veg.sway_cur2);
    let s_prev = veg_sway_at(pos.y, phase, leaf, veg.sway_prev, veg.sway_prev2);
    let cur_pos = vec4<f32>(rel.xyz + s_cur, 1.0);
    // Previous-camera-relative: abs - prev_cam = rel + (cam_cur - cam_prev).
    let prev_pos = vec4<f32>(rel.xyz + veg.cam_delta.xyz + s_prev, 1.0);
    var raster = mats.jittered_vp * cur_pos;
    raster.z = raster.w - raster.z;
    var out: VsOut;
    out.pos = raster;
    out.cur_clip = mats.cur_vp * cur_pos;
    out.prev_clip = mats.prev_vp * prev_pos;
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
