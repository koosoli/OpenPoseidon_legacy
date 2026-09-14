// REN-TEMP-001E/F/G — temporal camera state (per-frame jitter + previous-camera history)
// and the static-world velocity pass with its debug view.
//
// Everything here is renderer-owned history: nothing reaches back into gameplay state, and
// the C ABI is untouched. Jitter is applied to the MAIN SCENE camera's projection at the
// single point where cameras are ingested (lib.rs), so every scene pass — prepass, colour,
// water, clouds, GPU cull — rasterises with the same sub-pixel offset by construction,
// while the C++ side (gameplay, culling truth, picking, AI) never sees a jittered matrix.
// Gated by WGR_TEMPORAL=1 and OFF by default: jitter with no temporal accumulator behind
// it reads as shimmer, so the native path must not pay it.

use rustc_hash::FxHashMap;

const VELOCITY_FORMAT: wgpu::TextureFormat = wgpu::TextureFormat::Rg16Float;

// REN-TEMP-001H: uniform block for the rigid dynamic-object velocity pass
// (gfx3d/velocity_obj.wgsl `ObjVelMats`).
#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
pub struct ObjVelUniform {
    pub jittered_vp: [f32; 16],
    pub cur_vp: [f32; 16],
    pub prev_vp: [f32; 16],
    // x = relative depth tolerance for the fs visibility compare.
    pub misc: [f32; 4],
}
const _: () = assert!(std::mem::size_of::<ObjVelUniform>() == 208);

// REN-TEMP-001T: uniform for the vegetation-sway velocity variant
// (velocity_obj.wgsl `VegVelParams`). Sway lanes are (amp, time*speed, dir.x, dir.z)
// and (gust, leaf flutter, stiffness, 0) for the current and previous frame.
#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
pub struct VegVelUniform {
    pub sway_cur: [f32; 4],
    pub sway_cur2: [f32; 4],
    pub sway_prev: [f32; 4],
    pub sway_prev2: [f32; 4],
    pub cams: [f32; 4],
    pub cam_delta: [f32; 4],
}
const _: () = assert!(std::mem::size_of::<VegVelUniform>() == 96);

// REN-TEMP-001T: uniform for the RETAINED-path vegetation velocity twin
// (gpu_driven.wgsl `GpuVelParams`).
#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
pub struct GpuVegVelParams {
    pub cur_vp: [f32; 16],
    pub prev_vp: [f32; 16],
    pub sway_prev: [f32; 4],
    pub sway_prev2: [f32; 4],
    pub cam_delta: [f32; 4],
    pub vmisc: [f32; 4],
}
const _: () = assert!(std::mem::size_of::<GpuVegVelParams>() == 192);

pub fn gpu_veg_velocity_params(obj: &ObjVelUniform, veg: &VegVelUniform) -> GpuVegVelParams {
    GpuVegVelParams {
        cur_vp: obj.cur_vp,
        prev_vp: obj.prev_vp,
        sway_prev: veg.sway_prev,
        sway_prev2: veg.sway_prev2,
        cam_delta: veg.cam_delta,
        vmisc: obj.misc,
    }
}

// Snapshot of the UNJITTERED main scene camera. `view`/`proj` are the raw WgrCamera
// matrices (column-major, camera-relative view with zeroed translation, forward
// infinite-far projection whose rasterised depth is stored reversed).
#[derive(Clone, Copy)]
struct CameraSnap {
    view: [f32; 16],
    proj: [f32; 16],
    cam_pos: [f32; 3],
}

#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
struct TemporalParams {
    inv_view_proj: [f32; 16],
    prev_view_proj: [f32; 16],
    cam_delta: [f32; 4],
    misc: [f32; 4],
}
const _: () = assert!(std::mem::size_of::<TemporalParams>() == 160);

struct VelocityPass {
    target: wgpu::Texture,
    view: wgpu::TextureView,
    size: (u32, u32),
    uniform: wgpu::Buffer,
    layout: wgpu::BindGroupLayout,
    bind: Option<wgpu::BindGroup>,
    // Rebuilt when the depth view it reads is replaced (tracked by depth_gen).
    bound_depth_gen: u64,
    pipeline: wgpu::RenderPipeline,
    // Debug view (built lazily against the swapchain format on first use).
    debug_layout: wgpu::BindGroupLayout,
    debug_uniform: wgpu::Buffer,
    debug_bind: Option<wgpu::BindGroup>,
    debug_pipeline: Option<(wgpu::TextureFormat, wgpu::RenderPipeline)>,
}

// REN-TEMP-001K: the minimal temporal debug resolve (accumulator). Two HDR history
// textures ping-pong; the pass reprojects last frame's accumulator along velocity,
// neighbourhood-clamps it against the current frame and blends 90/10. Diagnostic only.
struct ResolvePass {
    history: [(wgpu::Texture, wgpu::TextureView); 2],
    // Which history slot the NEXT resolve writes into (the other is read).
    write_slot: usize,
    size: (u32, u32),
    sampler: wgpu::Sampler,
    uniform: wgpu::Buffer,
    layout: wgpu::BindGroupLayout,
    pipeline: wgpu::RenderPipeline,
    // History display (WGR_TEMPORAL_DEBUG=2).
    view_layout: wgpu::BindGroupLayout,
    view_uniform: wgpu::Buffer,
    view_pipeline: Option<(wgpu::TextureFormat, wgpu::RenderPipeline)>,
}

// REN-TEMP-001J: the history-control ("reactive") mask target + pass.
struct HistoryControlPass {
    target: wgpu::Texture,
    view: wgpu::TextureView,
    size: (u32, u32),
    uniform: wgpu::Buffer,
    layout: wgpu::BindGroupLayout,
    bind: Option<wgpu::BindGroup>,
    bound_depth_gen: u64,
    bound_cloud: Option<wgpu::TextureView>,
    clear_cloud: wgpu::TextureView,
    pipeline: wgpu::RenderPipeline,
}

pub struct Temporal {
    pub enabled: bool,
    // WGR_TEMPORAL_DEBUG: 0 = off, 1 = velocity overlay.
    pub debug_view: u32,
    frame_index: u64,
    // This frame's sub-pixel jitter in PIXELS of the render target, [-0.5, 0.5).
    pub jitter_px: [f32; 2],
    pub prev_jitter_px: [f32; 2],
    cur: Option<CameraSnap>,
    prev: Option<CameraSnap>,
    // REN-TEMP-001T: last frame's sway lanes (see veg_velocity_uniform).
    prev_sway: Option<[f32; 8]>,
    // True for exactly the frames whose history is invalid (first frame, render-size
    // change, enable toggle). The velocity pass writes zero and the debug view flashes
    // blue on these frames.
    reset: bool,
    // An explicit reset raised outside the frame (world load, upscaler change …),
    // consumed by the next begin_frame. REN-TEMP-001 plan Phase 5: resets are explicit
    // events wherever possible, not matrix-diff inference.
    pending_reset: Option<&'static str>,
    last_reset_reason: &'static str,
    render_size: (u32, u32),
    // Output width, for the jitter phase-count ratio (render_size alone cannot tell
    // 67% at 4K from 100% at 1440p). Set alongside begin_frame by the renderer.
    output_width_hint: u32,
    shader: Option<wgpu::ShaderModule>,
    velocity: Option<VelocityPass>,
    resolve: Option<ResolvePass>,
    history_control: Option<HistoryControlPass>,
    // WGR_REACTIVE_SKY / WGR_REACTIVE_WATER strengths (0 disables a term). Live-tunable.
    pub reactive_sky: f32,
    pub reactive_water: f32,
    // 0 = auto (~8 x (out/render)^2). Live-tunable (dev panel).
    pub jitter_phase_override: u64,
    // REN-TEMP-001H: ABSOLUTE world matrices of identified rigid draws, this frame and
    // last. Absolute on purpose — WgrDraw3D.world is camera-relative, which changes for
    // every object whenever the CAMERA moves; storing it directly would flag the whole
    // world as "moved" on any pan. Keyed by WgrDraw3D.object_id.
    cur_worlds: FxHashMap<u32, [f32; 16]>,
    prev_worlds: FxHashMap<u32, [f32; 16]>,
    // REN-TEMP-001I: (object id, mesh handle) -> baked base_vertex in that frame's skin
    // bake output. The association is the identity that palette_slot (a per-frame arena
    // index in submission order) cannot provide across frames.
    cur_skin_bases: FxHashMap<(u32, u64), u32>,
    prev_skin_bases: FxHashMap<(u32, u64), u32>,
}

// The stable object id rides bits 16-31 of WgrDraw3D.misc (see ffi.rs) — 0 = none.
pub fn draw_object_id(d: &crate::ffi::WgrDraw3D) -> u32 {
    d.misc >> 16
}

fn halton(mut index: u64, base: u64) -> f32 {
    let mut f = 1.0f32;
    let mut r = 0.0f32;
    while index > 0 {
        f /= base as f32;
        r += f * (index % base) as f32;
        index /= base;
    }
    r
}

impl Temporal {
    pub fn from_env() -> Self {
        let enabled = std::env::var("WGR_TEMPORAL").map(|v| v != "0").unwrap_or(false);
        let debug_view = std::env::var("WGR_TEMPORAL_DEBUG")
            .ok()
            .and_then(|v| v.parse::<u32>().ok())
            .unwrap_or(0);
        Temporal {
            enabled,
            debug_view,
            frame_index: 0,
            jitter_px: [0.0; 2],
            prev_jitter_px: [0.0; 2],
            cur: None,
            prev: None,
            prev_sway: None,
            reset: true,
            pending_reset: None,
            last_reset_reason: "first frame",
            render_size: (0, 0),
            output_width_hint: 0,
            shader: None,
            velocity: None,
            resolve: None,
            history_control: None,
            reactive_sky: std::env::var("WGR_REACTIVE_SKY")
                .ok()
                .and_then(|v| v.parse().ok())
                .unwrap_or(0.4f32)
                .clamp(0.0, 1.0),
            reactive_water: std::env::var("WGR_REACTIVE_WATER")
                .ok()
                .and_then(|v| v.parse().ok())
                .unwrap_or(0.3f32)
                .clamp(0.0, 1.0),
            jitter_phase_override: std::env::var("WGR_JITTER_PHASES")
                .ok()
                .and_then(|v| v.parse().ok())
                .unwrap_or(0),
            cur_worlds: FxHashMap::default(),
            prev_worlds: FxHashMap::default(),
            cur_skin_bases: FxHashMap::default(),
            prev_skin_bases: FxHashMap::default(),
        }
    }

    // Raise an explicit history reset, consumed by the next begin_frame. Callable from
    // any thread-confined renderer entry point (world load via the heightmap upload,
    // future mode/upscaler switches, an eventual C++ camera-cut event).
    pub fn set_output_width(&mut self, w: u32) {
        self.output_width_hint = w;
    }

    pub fn notify_reset(&mut self, reason: &'static str) {
        self.pending_reset = Some(reason);
    }

    // Ingest the UNJITTERED main scene camera and decide this frame's jitter. Call once
    // per frame, BEFORE apply_jitter and before any camera upload.
    pub fn begin_frame(&mut self, cam: &crate::ffi::WgrCamera, render_size: (u32, u32)) {
        if render_size != self.render_size {
            self.reset = true;
            self.last_reset_reason = if self.render_size == (0, 0) {
                "first frame"
            } else {
                "render-size change"
            };
            self.prev = None;
            self.render_size = render_size;
        } else if let Some(reason) = self.pending_reset.take() {
            self.reset = true;
            self.last_reset_reason = reason;
            self.prev = None;
        } else {
            // Teleport guard: >100 m of camera travel in ONE frame is not movement any
            // vehicle in this engine performs — it is a scripted cut/teleport that never
            // raised an event. This is a discontinuity detector, not the matrix-diff
            // inference the plan forbids relying on: explicit events stay the contract,
            // this only catches the ones nothing raises yet.
            let teleported = self.prev.as_ref().is_some_and(|p| {
                let dx = cam.cam_pos[0] - p.cam_pos[0];
                let dy = cam.cam_pos[1] - p.cam_pos[1];
                let dz = cam.cam_pos[2] - p.cam_pos[2];
                dx * dx + dy * dy + dz * dz > 100.0 * 100.0
            });
            if teleported {
                self.reset = true;
                self.last_reset_reason = "camera teleport";
                self.prev = None;
            } else {
                self.reset = self.prev.is_none();
            }
        }
        if self.reset {
            // Invalid history invalidates the object-world and skin-pose history with it.
            self.prev_worlds.clear();
            self.prev_skin_bases.clear();
        }
        self.cur = Some(CameraSnap {
            view: cam.view,
            proj: cam.proj,
            cam_pos: [cam.cam_pos[0], cam.cam_pos[1], cam.cam_pos[2]],
        });
        self.frame_index += 1;
        self.prev_jitter_px = self.jitter_px;
        self.jitter_px = if self.enabled {
            // Auto means: frozen offset at native scale (ratio <= 1, nothing to
            // reconstruct, cycling would only shimmer), cycling ~8 x ratio^2 when
            // upscaling so DLSS/FSR receive distinct samples to reconstruct detail
            // from. An explicit slider value / WGR_JITTER_PHASES always wins, which
            // is how a shimmer-sensitive upscaled setup pins the frozen offset.
            // The Halton (2,3) sequence itself stays replaceable per the plan.
            let override_phases =
                (self.jitter_phase_override > 0).then_some(self.jitter_phase_override);
            let phases = override_phases.unwrap_or_else(|| {
                let (rw, _) = self.render_size;
                let out_w = self.output_width_hint.max(rw).max(1);
                if out_w <= rw.max(1) {
                    1
                } else {
                    let ratio = out_w as f32 / rw.max(1) as f32;
                    ((8.0 * ratio * ratio).ceil() as u64).clamp(8, 64)
                }
            });
            let i = self.frame_index % phases + 1;
            [halton(i, 2) - 0.5, halton(i, 3) - 0.5]
        } else {
            [0.0; 2]
        };
    }

    // Add this frame's sub-pixel offset to a projection whose clip = P * v (column-major
    // cols array): a clip-space translation T(jx,jy)·P, i.e. row0 += jx·row3 and
    // row1 += jy·row3. No-op when disabled (jitter is zero).
    pub fn apply_jitter(&self, proj: &mut [f32; 16]) {
        let (w, h) = (self.render_size.0.max(1), self.render_size.1.max(1));
        let jx = self.jitter_px[0] * 2.0 / w as f32;
        let jy = self.jitter_px[1] * 2.0 / h as f32;
        if jx == 0.0 && jy == 0.0 {
            return;
        }
        for c in 0..4 {
            proj[c * 4] += jx * proj[c * 4 + 3];
            proj[c * 4 + 1] += jy * proj[c * 4 + 3];
        }
    }

    // Rotate current -> previous. Call once per frame after the frame is recorded.
    pub fn end_frame(&mut self) {
        if let Some(cur) = self.cur.take() {
            self.prev = Some(cur);
        }
        std::mem::swap(&mut self.prev_worlds, &mut self.cur_worlds);
        self.cur_worlds.clear();
        std::mem::swap(&mut self.prev_skin_bases, &mut self.cur_skin_bases);
        self.cur_skin_bases.clear();
    }

    // REN-TEMP-001I: this frame's (object id, mesh) -> baked base_vertex associations,
    // collected by Gfx3d::collect_skin_bases after the bake plan exists.
    pub fn ingest_skin_bases(&mut self, entries: Vec<(u32, u64, u32)>) {
        if !self.enabled {
            return;
        }
        self.cur_skin_bases.clear();
        for (id, mesh, base) in entries {
            self.cur_skin_bases.entry((id, mesh)).or_insert(base);
        }
    }

    // The skinned velocity subset: (draws3d index, PREVIOUS frame's baked base_vertex for
    // the same object+mesh). Every skinned identified draw with usable history is
    // included — animation moves essentially always, so there is no cheap "unchanged"
    // filter like the rigid path's.
    pub fn skinned_velocity_subset(
        &self,
        draws: &[crate::ffi::WgrDraw3D],
    ) -> Vec<(u32, u32)> {
        if !self.enabled || self.reset {
            return Vec::new();
        }
        let mut out = Vec::new();
        for (i, d) in draws.iter().enumerate() {
            let id = draw_object_id(d);
            if id == 0 || d.palette_slot == crate::ffi::NO_PALETTE {
                continue;
            }
            if let Some(prev_base) = self.prev_skin_bases.get(&(id, d.mesh)) {
                out.push((i as u32, *prev_base));
            }
        }
        out
    }

    // REN-TEMP-001H: record this frame's identified rigid draws as ABSOLUTE worlds
    // (camera-relative world + this frame's camera position on the translation column).
    // Skinned draws (palette_slot != NO_PALETTE, which includes the baked path) are
    // deliberately excluded — root motion presented as limb motion is exactly the false
    // vector the plan forbids; they stay on camera velocity until Phase 9.
    pub fn ingest_draws(&mut self, draws: &[crate::ffi::WgrDraw3D]) {
        if !self.enabled {
            return;
        }
        let Some(cur) = self.cur.as_ref() else {
            return;
        };
        let cam = cur.cam_pos;
        self.cur_worlds.clear();
        for d in draws {
            let id = draw_object_id(d);
            if id == 0 || d.palette_slot != crate::ffi::NO_PALETTE {
                continue;
            }
            self.cur_worlds.entry(id).or_insert_with(|| {
                let mut w = d.world;
                w[12] += cam[0];
                w[13] += cam[1];
                w[14] += cam[2];
                w
            });
        }
    }

    // The matrices the object-velocity pass needs, or None while history is invalid.
    pub fn object_velocity_uniform(&self) -> Option<ObjVelUniform> {
        if !self.enabled || self.reset {
            return None;
        }
        let (cur, prev) = (self.cur.as_ref()?, self.prev.as_ref()?);
        let view = glam::Mat4::from_cols_array(&cur.view);
        let proj = glam::Mat4::from_cols_array(&cur.proj);
        let mut proj_j = cur.proj;
        self.apply_jitter(&mut proj_j);
        let jittered_vp = (glam::Mat4::from_cols_array(&proj_j) * view).to_cols_array();
        let cur_vp = (proj * view).to_cols_array();
        let prev_vp = (glam::Mat4::from_cols_array(&prev.proj)
            * glam::Mat4::from_cols_array(&prev.view))
        .to_cols_array();
        Some(ObjVelUniform {
            jittered_vp,
            cur_vp,
            prev_vp,
            // 2% distance tolerance: stored depth is near/dist, so the fs ratio compare
            // tolerates the sub-pixel depth disagreement between the jittered scene
            // raster and this pass without letting occluded surfaces through.
            misc: [0.02, 0.0, 0.0, 0.0],
        })
    }

    // REN-TEMP-001T: sway parameters of the PREVIOUS frame, rotated by
    // veg_velocity_uniform once per frame. None until two frames have passed.
    // (Field lives here beside the camera history it pairs with.)

    // The vegetation-sway velocity uniform, rotating this frame's sway parameters
    // into the previous slot. Returns None (and still rotates) while history is
    // invalid or the wind is flat — the caller then skips the veg pass.
    pub fn veg_velocity_uniform(
        &mut self,
        foliage: &crate::ffi::WgrFoliage,
    ) -> Option<VegVelUniform> {
        let cur_sway = [
            foliage.sway_strength,
            foliage.sway_time * foliage.sway_speed,
            foliage.wind_dir_x,
            foliage.wind_dir_z,
            foliage.wind_gust,
            foliage.sway_leaf,
            foliage.sway_stiffness,
            0.0,
        ];
        let prev_sway = self.prev_sway.replace(cur_sway);
        if !self.enabled || self.reset || foliage.sway_strength <= 0.0 {
            return None;
        }
        let prev_sway = prev_sway?;
        let (cur, prev) = (self.cur.as_ref()?, self.prev.as_ref()?);
        Some(VegVelUniform {
            sway_cur: [cur_sway[0], cur_sway[1], cur_sway[2], cur_sway[3]],
            sway_cur2: [cur_sway[4], cur_sway[5], cur_sway[6], 0.0],
            sway_prev: [prev_sway[0], prev_sway[1], prev_sway[2], prev_sway[3]],
            sway_prev2: [prev_sway[4], prev_sway[5], prev_sway[6], 0.0],
            cams: [
                cur.cam_pos[0],
                cur.cam_pos[2],
                prev.cam_pos[0],
                prev.cam_pos[2],
            ],
            cam_delta: [
                cur.cam_pos[0] - prev.cam_pos[0],
                cur.cam_pos[1] - prev.cam_pos[1],
                cur.cam_pos[2] - prev.cam_pos[2],
                0.0,
            ],
        })
    }

    // The swaying-vegetation subset: (draws3d index, leaf flag). Selection mirrors the
    // scene VS gate (1 = bush, 2 = legacy tree, 3 = coherent native tree).
    // A negative section value selects coherent wind, including on solid bark.
    pub fn veg_velocity_subset(&self, draws: &[crate::ffi::WgrDraw3D]) -> Vec<(u32, f32)> {
        if !self.enabled || self.reset {
            return Vec::new();
        }
        let mut out = Vec::new();
        for (i, d) in draws.iter().enumerate() {
            if d.mat_sun_ambient[3] <= 0.5 || d.palette_slot != crate::ffi::NO_PALETTE {
                continue;
            }
            let wind_section = if d.mat_sun_ambient[3] > 2.5 { -1.0 }
                else if d.alpha_ref > 0.0 { 1.0 } else { 0.0 };
            out.push((i as u32, wind_section));
            if out.len() >= 16384 {
                break;
            }
        }
        out
    }

    // Build the moving-rigid subset for this frame: (draws3d index, previous world
    // RELATIVE TO THE PREVIOUS CAMERA — the space prev_vp consumes). Objects whose
    // absolute world is unchanged (within 1 mm / 1e-3 per element) are skipped: the
    // fullscreen camera reprojection is already exact for them.
    pub fn object_velocity_subset(
        &self,
        draws: &[crate::ffi::WgrDraw3D],
    ) -> Vec<(u32, [f32; 16])> {
        if !self.enabled || self.reset {
            return Vec::new();
        }
        let Some(prev_cam) = self.prev.as_ref() else {
            return Vec::new();
        };
        let Some(cur_cam) = self.cur.as_ref() else {
            return Vec::new();
        };
        let mut seen: FxHashMap<u32, bool> = FxHashMap::default();
        let mut out = Vec::new();
        for (i, d) in draws.iter().enumerate() {
            let id = draw_object_id(d);
            if id == 0 || d.palette_slot != crate::ffi::NO_PALETTE {
                continue;
            }
            let moved = *seen.entry(id).or_insert_with(|| {
                let Some(prev_abs) = self.prev_worlds.get(&id) else {
                    return false;
                };
                let mut cur_abs = d.world;
                cur_abs[12] += cur_cam.cam_pos[0];
                cur_abs[13] += cur_cam.cam_pos[1];
                cur_abs[14] += cur_cam.cam_pos[2];
                cur_abs
                    .iter()
                    .zip(prev_abs.iter())
                    .any(|(a, b)| (a - b).abs() > 1e-3)
            });
            if !moved {
                continue;
            }
            let prev_abs = self.prev_worlds.get(&id).expect("seen implies prev");
            let mut prev_rel = *prev_abs;
            prev_rel[12] -= prev_cam.cam_pos[0];
            prev_rel[13] -= prev_cam.cam_pos[1];
            prev_rel[14] -= prev_cam.cam_pos[2];
            out.push((i as u32, prev_rel));
        }
        out
    }

    pub fn last_reset_reason(&self) -> &'static str {
        self.last_reset_reason
    }

    // REN-TEMP-001K: blit the just-written accumulator slot to the swapchain (Reinhard
    // roll-off in the shader; the history is linear HDR).
    fn render_history_view(
        &mut self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        encoder: &mut wgpu::CommandEncoder,
        dst: &wgpu::TextureView,
        dst_format: wgpu::TextureFormat,
        output_size: (u32, u32),
    ) {
        let mut params = self.params();
        params.misc = [0.0, 0.0, output_size.0 as f32, output_size.1 as f32];
        self.ensure_shader(device);
        let shader = self.shader.clone().expect("temporal shader");
        let Some(rp) = self.resolve.as_mut() else {
            return; // resolve has not run yet this session
        };
        queue.write_buffer(&rp.view_uniform, 0, bytemuck::bytes_of(&params));
        if rp
            .view_pipeline
            .as_ref()
            .is_some_and(|(f, _)| *f != dst_format)
        {
            rp.view_pipeline = None;
        }
        if rp.view_pipeline.is_none() {
            let pipeline_layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
                label: Some("wgr_temporal_history_view_pl"),
                bind_group_layouts: &[Some(&rp.view_layout)],
                immediate_size: 0,
            });
            let pipeline = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
                label: Some("wgr_temporal_history_view"),
                layout: Some(&pipeline_layout),
                vertex: wgpu::VertexState {
                    module: &shader,
                    entry_point: Some("vs_fullscreen"),
                    buffers: &[],
                    compilation_options: Default::default(),
                },
                primitive: wgpu::PrimitiveState::default(),
                depth_stencil: None,
                multisample: wgpu::MultisampleState::default(),
                fragment: Some(wgpu::FragmentState {
                    module: &shader,
                    entry_point: Some("fs_history_debug"),
                    targets: &[Some(wgpu::ColorTargetState {
                        format: dst_format,
                        blend: None,
                        write_mask: wgpu::ColorWrites::ALL,
                    })],
                    compilation_options: Default::default(),
                }),
                multiview_mask: None,
                cache: None,
            });
            rp.view_pipeline = Some((dst_format, pipeline));
        }
        // write_slot was flipped after the resolve — the freshly-written slot is the
        // one the next frame reads, i.e. the CURRENT write_slot after the flip.
        let fresh = &rp.history[1 - rp.write_slot].1;
        let bind = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("wgr_temporal_history_view_bind"),
            layout: &rp.view_layout,
            entries: &[
                wgpu::BindGroupEntry {
                    binding: 0,
                    resource: wgpu::BindingResource::TextureView(fresh),
                },
                wgpu::BindGroupEntry {
                    binding: 4,
                    resource: rp.view_uniform.as_entire_binding(),
                },
            ],
        });
        encoder.push_debug_group("wgr_temporal_history_view");
        let mut pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
            label: Some("wgr_temporal_history_view"),
            color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                view: dst,
                depth_slice: None,
                resolve_target: None,
                ops: wgpu::Operations {
                    load: wgpu::LoadOp::Load,
                    store: wgpu::StoreOp::Store,
                },
            })],
            depth_stencil_attachment: None,
            timestamp_writes: None,
            occlusion_query_set: None,
            multiview_mask: None,
        });
        let (_, pipeline) = rp.view_pipeline.as_ref().expect("view pipeline");
        pass.set_pipeline(pipeline);
        pass.set_bind_group(0, &bind, &[]);
        pass.draw(0..3, 0..1);
        drop(pass);
        encoder.pop_debug_group();
    }

    fn params(&self) -> TemporalParams {
        let (cur, prev) = match (self.cur.as_ref(), self.prev.as_ref()) {
            (Some(c), Some(p)) => (c, p),
            _ => {
                return TemporalParams {
                    inv_view_proj: glam::Mat4::IDENTITY.to_cols_array(),
                    prev_view_proj: glam::Mat4::IDENTITY.to_cols_array(),
                    cam_delta: [0.0, 0.0, 0.0, 0.0],
                    misc: [0.0; 4],
                };
            }
        };
        // f64 and separate inversions, same reasoning as the camera UBO's inv_view_proj:
        // the infinite-far projection is ill-conditioned in f32.
        let view = glam::DMat4::from_cols_array(&cur.view.map(f64::from));
        let proj = glam::DMat4::from_cols_array(&cur.proj.map(f64::from));
        let inv_view_proj = (view.inverse() * proj.inverse()).as_mat4().to_cols_array();
        let prev_vp = (glam::DMat4::from_cols_array(&prev.proj.map(f64::from))
            * glam::DMat4::from_cols_array(&prev.view.map(f64::from)))
        .as_mat4()
        .to_cols_array();
        TemporalParams {
            inv_view_proj,
            prev_view_proj: prev_vp,
            cam_delta: [
                cur.cam_pos[0] - prev.cam_pos[0],
                cur.cam_pos[1] - prev.cam_pos[1],
                cur.cam_pos[2] - prev.cam_pos[2],
                if self.reset { 0.0 } else { 1.0 },
            ],
            // Depth was rasterized with apply_jitter; recover its unjittered ray.
            misc: [
                self.jitter_px[0] * 2.0 / self.render_size.0.max(1) as f32,
                self.jitter_px[1] * 2.0 / self.render_size.1.max(1) as f32,
                0.0,
                0.0,
            ],
        }
    }

    fn ensure_shader(&mut self, device: &wgpu::Device) -> &wgpu::ShaderModule {
        self.shader.get_or_insert_with(|| {
            device.create_shader_module(wgpu::ShaderModuleDescriptor {
                label: Some("wgr_temporal_shader"),
                source: wgpu::ShaderSource::Wgsl(include_str!("temporal.wgsl").into()),
            })
        })
    }

    pub fn velocity_view(&self) -> Option<&wgpu::TextureView> {
        self.velocity.as_ref().map(|v| &v.view)
    }

    // REN-TEMP-001M: the velocity TEXTURE, for native (NGX) interop.
    pub fn velocity_texture(&self) -> Option<&wgpu::Texture> {
        self.velocity.as_ref().map(|v| &v.target)
    }

    pub fn reset_this_frame(&self) -> bool {
        self.reset
    }

    // Record the static-world velocity pass. `depth` must be the frame's FINAL
    // single-sample depth (the far-reduce resolve; ensure_far_depth_resolve has run).
    // `depth_gen` tracks depth-target reallocation so the bind group follows it.
    pub fn render_velocity(
        &mut self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        encoder: &mut wgpu::CommandEncoder,
        depth: &wgpu::TextureView,
        depth_gen: u64,
        render_size: (u32, u32),
    ) {
        if !self.enabled {
            return;
        }
        self.ensure_shader(device);
        let shader = self.shader.as_ref().expect("temporal shader");
        if self
            .velocity
            .as_ref()
            .is_some_and(|v| v.size != render_size)
        {
            self.velocity = None;
        }
        if self.velocity.is_none() {
            let target = device.create_texture(&wgpu::TextureDescriptor {
                label: Some("wgr_velocity"),
                size: wgpu::Extent3d {
                    width: render_size.0.max(1),
                    height: render_size.1.max(1),
                    depth_or_array_layers: 1,
                },
                mip_level_count: 1,
                sample_count: 1,
                dimension: wgpu::TextureDimension::D2,
                format: VELOCITY_FORMAT,
                // COPY_SRC: readback for the headless device test and later debug dumps.
                usage: wgpu::TextureUsages::RENDER_ATTACHMENT
                    | wgpu::TextureUsages::TEXTURE_BINDING
                    | wgpu::TextureUsages::COPY_SRC,
                view_formats: &[],
            });
            let view = target.create_view(&wgpu::TextureViewDescriptor::default());
            let layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
                label: Some("wgr_velocity_layout"),
                entries: &[
                    wgpu::BindGroupLayoutEntry {
                        binding: 0,
                        visibility: wgpu::ShaderStages::FRAGMENT,
                        ty: wgpu::BindingType::Texture {
                            sample_type: wgpu::TextureSampleType::Depth,
                            view_dimension: wgpu::TextureViewDimension::D2,
                            multisampled: false,
                        },
                        count: None,
                    },
                    wgpu::BindGroupLayoutEntry {
                        binding: 1,
                        visibility: wgpu::ShaderStages::FRAGMENT,
                        ty: wgpu::BindingType::Buffer {
                            ty: wgpu::BufferBindingType::Uniform,
                            has_dynamic_offset: false,
                            min_binding_size: None,
                        },
                        count: None,
                    },
                ],
            });
            let pipeline_layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
                label: Some("wgr_velocity_pipeline_layout"),
                bind_group_layouts: &[Some(&layout)],
                immediate_size: 0,
            });
            let pipeline = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
                label: Some("wgr_velocity_pipeline"),
                layout: Some(&pipeline_layout),
                vertex: wgpu::VertexState {
                    module: shader,
                    entry_point: Some("vs_fullscreen"),
                    buffers: &[],
                    compilation_options: Default::default(),
                },
                primitive: wgpu::PrimitiveState::default(),
                depth_stencil: None,
                multisample: wgpu::MultisampleState::default(),
                fragment: Some(wgpu::FragmentState {
                    module: shader,
                    entry_point: Some("fs_velocity"),
                    targets: &[Some(wgpu::ColorTargetState {
                        format: VELOCITY_FORMAT,
                        blend: None,
                        write_mask: wgpu::ColorWrites::ALL,
                    })],
                    compilation_options: Default::default(),
                }),
                multiview_mask: None,
                cache: None,
            });
            let uniform = device.create_buffer(&wgpu::BufferDescriptor {
                label: Some("wgr_velocity_params"),
                size: std::mem::size_of::<TemporalParams>() as u64,
                usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
                mapped_at_creation: false,
            });
            let debug_layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
                label: Some("wgr_velocity_debug_layout"),
                entries: &[
                    wgpu::BindGroupLayoutEntry {
                        binding: 0,
                        visibility: wgpu::ShaderStages::FRAGMENT,
                        ty: wgpu::BindingType::Texture {
                            sample_type: wgpu::TextureSampleType::Float { filterable: false },
                            view_dimension: wgpu::TextureViewDimension::D2,
                            multisampled: false,
                        },
                        count: None,
                    },
                    wgpu::BindGroupLayoutEntry {
                        binding: 1,
                        visibility: wgpu::ShaderStages::FRAGMENT,
                        ty: wgpu::BindingType::Buffer {
                            ty: wgpu::BufferBindingType::Uniform,
                            has_dynamic_offset: false,
                            min_binding_size: None,
                        },
                        count: None,
                    },
                ],
            });
            let debug_uniform = device.create_buffer(&wgpu::BufferDescriptor {
                label: Some("wgr_velocity_debug_params"),
                size: std::mem::size_of::<TemporalParams>() as u64,
                usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
                mapped_at_creation: false,
            });
            self.velocity = Some(VelocityPass {
                target,
                view,
                size: render_size,
                uniform,
                layout,
                bind: None,
                bound_depth_gen: u64::MAX,
                pipeline,
                debug_layout,
                debug_uniform,
                debug_bind: None,
                debug_pipeline: None,
            });
        }
        let params = self.params();
        let vp = self.velocity.as_mut().expect("velocity pass");
        let _ = &vp.target;
        queue.write_buffer(&vp.uniform, 0, bytemuck::bytes_of(&params));
        if vp.bind.is_none() || vp.bound_depth_gen != depth_gen {
            vp.bind = Some(device.create_bind_group(&wgpu::BindGroupDescriptor {
                label: Some("wgr_velocity_bind"),
                layout: &vp.layout,
                entries: &[
                    wgpu::BindGroupEntry {
                        binding: 0,
                        resource: wgpu::BindingResource::TextureView(depth),
                    },
                    wgpu::BindGroupEntry {
                        binding: 1,
                        resource: vp.uniform.as_entire_binding(),
                    },
                ],
            }));
            vp.bound_depth_gen = depth_gen;
        }
        encoder.push_debug_group("wgr_velocity");
        let mut pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
            label: Some("wgr_velocity"),
            color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                view: &vp.view,
                depth_slice: None,
                resolve_target: None,
                ops: wgpu::Operations {
                    load: wgpu::LoadOp::Clear(wgpu::Color::TRANSPARENT),
                    store: wgpu::StoreOp::Store,
                },
            })],
            depth_stencil_attachment: None,
            timestamp_writes: None,
            occlusion_query_set: None,
            multiview_mask: None,
        });
        pass.set_pipeline(&vp.pipeline);
        pass.set_bind_group(0, vp.bind.as_ref().expect("velocity bind"), &[]);
        pass.draw(0..3, 0..1);
        drop(pass);
        encoder.pop_debug_group();
    }

    pub fn camera_y(&self) -> f32 {
        self.cur.as_ref().map(|c| c.cam_pos[1]).unwrap_or(0.0)
    }

    pub fn history_control_view(&self) -> Option<&wgpu::TextureView> {
        self.history_control.as_ref().map(|h| &h.view)
    }

    pub fn history_control_texture(&self) -> Option<&wgpu::Texture> {
        self.history_control.as_ref().map(|h| &h.target)
    }

    // REN-TEMP-001J: build this frame's history-control mask (R8Unorm, render res) from
    // the frame-final depth. `sea_level` = None disables the water term this frame.
    #[allow(clippy::too_many_arguments)]
    pub fn render_history_control(
        &mut self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        encoder: &mut wgpu::CommandEncoder,
        depth: &wgpu::TextureView,
        depth_gen: u64,
        render_size: (u32, u32),
        sea_level: Option<f32>,
        cloud: Option<&wgpu::TextureView>,
    ) {
        if !self.enabled {
            return;
        }
        self.ensure_shader(device);
        let shader = self.shader.clone().expect("temporal shader");
        if self
            .history_control
            .as_ref()
            .is_some_and(|h| h.size != render_size)
        {
            self.history_control = None;
        }
        if self.history_control.is_none() {
            let target = device.create_texture(&wgpu::TextureDescriptor {
                label: Some("wgr_history_control"),
                size: wgpu::Extent3d {
                    width: render_size.0.max(1),
                    height: render_size.1.max(1),
                    depth_or_array_layers: 1,
                },
                mip_level_count: 1,
                sample_count: 1,
                dimension: wgpu::TextureDimension::D2,
                format: wgpu::TextureFormat::R8Unorm,
                usage: wgpu::TextureUsages::RENDER_ATTACHMENT
                    | wgpu::TextureUsages::TEXTURE_BINDING
                    | wgpu::TextureUsages::COPY_SRC,
                view_formats: &[],
            });
            let view = target.create_view(&wgpu::TextureViewDescriptor::default());
            let layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
                label: Some("wgr_history_control_layout"),
                entries: &[
                    wgpu::BindGroupLayoutEntry {
                        binding: 2,
                        visibility: wgpu::ShaderStages::FRAGMENT,
                        ty: wgpu::BindingType::Texture {
                            sample_type: wgpu::TextureSampleType::Float { filterable: false },
                            view_dimension: wgpu::TextureViewDimension::D2,
                            multisampled: false,
                        },
                        count: None,
                    },
                    wgpu::BindGroupLayoutEntry {
                        binding: 0,
                        visibility: wgpu::ShaderStages::FRAGMENT,
                        ty: wgpu::BindingType::Texture {
                            sample_type: wgpu::TextureSampleType::Depth,
                            view_dimension: wgpu::TextureViewDimension::D2,
                            multisampled: false,
                        },
                        count: None,
                    },
                    wgpu::BindGroupLayoutEntry {
                        binding: 1,
                        visibility: wgpu::ShaderStages::FRAGMENT,
                        ty: wgpu::BindingType::Buffer {
                            ty: wgpu::BufferBindingType::Uniform,
                            has_dynamic_offset: false,
                            min_binding_size: None,
                        },
                        count: None,
                    },
                ],
            });
            let pipeline_layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
                label: Some("wgr_history_control_pl"),
                bind_group_layouts: &[Some(&layout)],
                immediate_size: 0,
            });
            let pipeline = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
                label: Some("wgr_history_control"),
                layout: Some(&pipeline_layout),
                vertex: wgpu::VertexState {
                    module: &shader,
                    entry_point: Some("vs_fullscreen"),
                    buffers: &[],
                    compilation_options: Default::default(),
                },
                primitive: wgpu::PrimitiveState::default(),
                depth_stencil: None,
                multisample: wgpu::MultisampleState::default(),
                fragment: Some(wgpu::FragmentState {
                    module: &shader,
                    entry_point: Some("fs_history_control"),
                    targets: &[Some(wgpu::ColorTargetState {
                        format: wgpu::TextureFormat::R8Unorm,
                        blend: None,
                        write_mask: wgpu::ColorWrites::ALL,
                    })],
                    compilation_options: Default::default(),
                }),
                multiview_mask: None,
                cache: None,
            });
            let uniform = device.create_buffer(&wgpu::BufferDescriptor {
                label: Some("wgr_history_control_params"),
                size: std::mem::size_of::<TemporalParams>() as u64,
                usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
                mapped_at_creation: false,
            });
            let clear_cloud = device.create_texture(&wgpu::TextureDescriptor {
                label: Some("wgr_history_clear_cloud"),
                size: wgpu::Extent3d { width: 1, height: 1, depth_or_array_layers: 1 },
                mip_level_count: 1,
                sample_count: 1,
                dimension: wgpu::TextureDimension::D2,
                format: wgpu::TextureFormat::Rgba8Unorm,
                usage: wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::COPY_DST,
                view_formats: &[],
            });
            queue.write_texture(
                clear_cloud.as_image_copy(), &[0, 0, 0, 255],
                wgpu::TexelCopyBufferLayout { offset: 0, bytes_per_row: Some(4), rows_per_image: Some(1) },
                clear_cloud.size(),
            );
            self.history_control = Some(HistoryControlPass {
                target,
                view,
                size: render_size,
                uniform,
                layout,
                bind: None,
                bound_depth_gen: u64::MAX,
                bound_cloud: None,
                clear_cloud: clear_cloud.create_view(&Default::default()),
                pipeline,
            });
        }
        let mut params = self.params();
        params.misc = [
            self.reactive_sky,
            if sea_level.is_some() {
                self.reactive_water
            } else {
                0.0
            },
            self.camera_y(),
            sea_level.unwrap_or(f32::MIN),
        ];
        let hc = self.history_control.as_mut().expect("history control");
        let cloud = cloud.unwrap_or(&hc.clear_cloud);
        queue.write_buffer(&hc.uniform, 0, bytemuck::bytes_of(&params));
        if hc.bind.is_none() || hc.bound_depth_gen != depth_gen || hc.bound_cloud.as_ref() != Some(cloud) {
            hc.bind = Some(device.create_bind_group(&wgpu::BindGroupDescriptor {
                label: Some("wgr_history_control_bind"),
                layout: &hc.layout,
                entries: &[
                    wgpu::BindGroupEntry {
                        binding: 2,
                        resource: wgpu::BindingResource::TextureView(cloud),
                    },
                    wgpu::BindGroupEntry {
                        binding: 0,
                        resource: wgpu::BindingResource::TextureView(depth),
                    },
                    wgpu::BindGroupEntry {
                        binding: 1,
                        resource: hc.uniform.as_entire_binding(),
                    },
                ],
            }));
            hc.bound_depth_gen = depth_gen;
            hc.bound_cloud = Some(cloud.clone());
        }
        encoder.push_debug_group("wgr_history_control");
        let mut pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
            label: Some("wgr_history_control"),
            color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                view: &hc.view,
                depth_slice: None,
                resolve_target: None,
                ops: wgpu::Operations {
                    load: wgpu::LoadOp::Clear(wgpu::Color::TRANSPARENT),
                    store: wgpu::StoreOp::Store,
                },
            })],
            depth_stencil_attachment: None,
            timestamp_writes: None,
            occlusion_query_set: None,
            multiview_mask: None,
        });
        pass.set_pipeline(&hc.pipeline);
        pass.set_bind_group(0, hc.bind.as_ref().expect("hc bind"), &[]);
        pass.draw(0..3, 0..1);
        drop(pass);
        encoder.pop_debug_group();
    }

    // REN-TEMP-001K: run the minimal temporal debug resolve. `cur_color` is this frame's
    // single-sample resolved HDR at render resolution. Only runs under
    // WGR_TEMPORAL_DEBUG=2 — the accumulator is a diagnostic, nothing consumes it.
    pub fn render_debug_resolve(
        &mut self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        encoder: &mut wgpu::CommandEncoder,
        cur_color: &wgpu::TextureView,
        render_size: (u32, u32),
    ) {
        if !self.enabled || self.debug_view != 2 {
            return;
        }
        let Some(velocity_view) = self.velocity.as_ref().map(|v| v.view.clone()) else {
            return;
        };
        let params = self.params();
        self.ensure_shader(device);
        let shader = self.shader.clone().expect("temporal shader");
        if self.resolve.as_ref().is_some_and(|r| r.size != render_size) {
            self.resolve = None;
        }
        if self.resolve.is_none() {
            let mk_hist = |slot: usize| {
                let t = device.create_texture(&wgpu::TextureDescriptor {
                    label: Some(if slot == 0 {
                        "wgr_temporal_history_a"
                    } else {
                        "wgr_temporal_history_b"
                    }),
                    size: wgpu::Extent3d {
                        width: render_size.0.max(1),
                        height: render_size.1.max(1),
                        depth_or_array_layers: 1,
                    },
                    mip_level_count: 1,
                    sample_count: 1,
                    dimension: wgpu::TextureDimension::D2,
                    // Matches the resolved HDR scene (lib.rs HDR_FORMAT).
                    format: wgpu::TextureFormat::Rgba16Float,
                    usage: wgpu::TextureUsages::RENDER_ATTACHMENT
                        | wgpu::TextureUsages::TEXTURE_BINDING,
                    view_formats: &[],
                });
                let v = t.create_view(&wgpu::TextureViewDescriptor::default());
                (t, v)
            };
            let sampler = device.create_sampler(&wgpu::SamplerDescriptor {
                label: Some("wgr_temporal_resolve_sampler"),
                address_mode_u: wgpu::AddressMode::ClampToEdge,
                address_mode_v: wgpu::AddressMode::ClampToEdge,
                mag_filter: wgpu::FilterMode::Linear,
                min_filter: wgpu::FilterMode::Linear,
                ..Default::default()
            });
            let tex_entry = |binding: u32| wgpu::BindGroupLayoutEntry {
                binding,
                visibility: wgpu::ShaderStages::FRAGMENT,
                ty: wgpu::BindingType::Texture {
                    sample_type: wgpu::TextureSampleType::Float { filterable: true },
                    view_dimension: wgpu::TextureViewDimension::D2,
                    multisampled: false,
                },
                count: None,
            };
            let layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
                label: Some("wgr_temporal_resolve_layout"),
                entries: &[
                    tex_entry(0),
                    // Velocity is Rg16Float and only ever textureLoad-ed, but declaring it
                    // filterable is harmless and keeps one entry constructor.
                    tex_entry(1),
                    tex_entry(2),
                    wgpu::BindGroupLayoutEntry {
                        binding: 3,
                        visibility: wgpu::ShaderStages::FRAGMENT,
                        ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Filtering),
                        count: None,
                    },
                    wgpu::BindGroupLayoutEntry {
                        binding: 4,
                        visibility: wgpu::ShaderStages::FRAGMENT,
                        ty: wgpu::BindingType::Buffer {
                            ty: wgpu::BufferBindingType::Uniform,
                            has_dynamic_offset: false,
                            min_binding_size: None,
                        },
                        count: None,
                    },
                ],
            });
            let pipeline_layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
                label: Some("wgr_temporal_resolve_pipeline_layout"),
                bind_group_layouts: &[Some(&layout)],
                immediate_size: 0,
            });
            let pipeline = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
                label: Some("wgr_temporal_resolve_pipeline"),
                layout: Some(&pipeline_layout),
                vertex: wgpu::VertexState {
                    module: &shader,
                    entry_point: Some("vs_fullscreen"),
                    buffers: &[],
                    compilation_options: Default::default(),
                },
                primitive: wgpu::PrimitiveState::default(),
                depth_stencil: None,
                multisample: wgpu::MultisampleState::default(),
                fragment: Some(wgpu::FragmentState {
                    module: &shader,
                    entry_point: Some("fs_temporal_resolve"),
                    targets: &[Some(wgpu::ColorTargetState {
                        format: wgpu::TextureFormat::Rgba16Float,
                        blend: None,
                        write_mask: wgpu::ColorWrites::ALL,
                    })],
                    compilation_options: Default::default(),
                }),
                multiview_mask: None,
                cache: None,
            });
            let view_layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
                label: Some("wgr_temporal_history_view_layout"),
                entries: &[
                    tex_entry(0),
                    wgpu::BindGroupLayoutEntry {
                        binding: 4,
                        visibility: wgpu::ShaderStages::FRAGMENT,
                        ty: wgpu::BindingType::Buffer {
                            ty: wgpu::BufferBindingType::Uniform,
                            has_dynamic_offset: false,
                            min_binding_size: None,
                        },
                        count: None,
                    },
                ],
            });
            let uniform = device.create_buffer(&wgpu::BufferDescriptor {
                label: Some("wgr_temporal_resolve_params"),
                size: std::mem::size_of::<TemporalParams>() as u64,
                usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
                mapped_at_creation: false,
            });
            let view_uniform = device.create_buffer(&wgpu::BufferDescriptor {
                label: Some("wgr_temporal_history_view_params"),
                size: std::mem::size_of::<TemporalParams>() as u64,
                usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
                mapped_at_creation: false,
            });
            self.resolve = Some(ResolvePass {
                history: [mk_hist(0), mk_hist(1)],
                write_slot: 0,
                size: render_size,
                sampler,
                uniform,
                layout,
                pipeline,
                view_layout,
                view_uniform,
                view_pipeline: None,
            });
        }
        let rp = self.resolve.as_mut().expect("resolve pass");
        queue.write_buffer(&rp.uniform, 0, bytemuck::bytes_of(&params));
        let read_slot = 1 - rp.write_slot;
        let bind = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("wgr_temporal_resolve_bind"),
            layout: &rp.layout,
            entries: &[
                wgpu::BindGroupEntry {
                    binding: 0,
                    resource: wgpu::BindingResource::TextureView(cur_color),
                },
                wgpu::BindGroupEntry {
                    binding: 1,
                    resource: wgpu::BindingResource::TextureView(&velocity_view),
                },
                wgpu::BindGroupEntry {
                    binding: 2,
                    resource: wgpu::BindingResource::TextureView(&rp.history[read_slot].1),
                },
                wgpu::BindGroupEntry {
                    binding: 3,
                    resource: wgpu::BindingResource::Sampler(&rp.sampler),
                },
                wgpu::BindGroupEntry {
                    binding: 4,
                    resource: rp.uniform.as_entire_binding(),
                },
            ],
        });
        encoder.push_debug_group("wgr_temporal_resolve");
        let mut pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
            label: Some("wgr_temporal_resolve"),
            color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                view: &rp.history[rp.write_slot].1,
                depth_slice: None,
                resolve_target: None,
                ops: wgpu::Operations {
                    load: wgpu::LoadOp::Clear(wgpu::Color::BLACK),
                    store: wgpu::StoreOp::Store,
                },
            })],
            depth_stencil_attachment: None,
            timestamp_writes: None,
            occlusion_query_set: None,
            multiview_mask: None,
        });
        pass.set_pipeline(&rp.pipeline);
        pass.set_bind_group(0, &bind, &[]);
        pass.draw(0..3, 0..1);
        drop(pass);
        encoder.pop_debug_group();
        rp.write_slot = read_slot;
    }

    // Draw the velocity debug overlay over the tonemapped frame (output resolution).
    pub fn render_debug(
        &mut self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        encoder: &mut wgpu::CommandEncoder,
        dst: &wgpu::TextureView,
        dst_format: wgpu::TextureFormat,
        output_size: (u32, u32),
    ) {
        if !self.enabled {
            return;
        }
        // WGR_TEMPORAL_DEBUG=2: show the debug-resolve accumulator instead of velocity.
        if self.debug_view == 2 {
            self.render_history_view(device, queue, encoder, dst, dst_format, output_size);
            return;
        }
        if self.debug_view != 1 {
            return;
        }
        let mut params = self.params();
        // Magnitude scale: 20 makes a half-percent-of-screen motion a full colour swing,
        // which is about the range a slow pan produces.
        params.misc = [20.0, 0.0, output_size.0 as f32, output_size.1 as f32];
        self.ensure_shader(device);
        let shader = self.shader.clone().expect("temporal shader");
        let Some(vp) = self.velocity.as_mut() else {
            return;
        };
        queue.write_buffer(&vp.debug_uniform, 0, bytemuck::bytes_of(&params));
        if vp
            .debug_pipeline
            .as_ref()
            .is_some_and(|(f, _)| *f != dst_format)
        {
            vp.debug_pipeline = None;
        }
        if vp.debug_pipeline.is_none() {
            let pipeline_layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
                label: Some("wgr_velocity_debug_pipeline_layout"),
                bind_group_layouts: &[Some(&vp.debug_layout)],
                immediate_size: 0,
            });
            let pipeline = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
                label: Some("wgr_velocity_debug_pipeline"),
                layout: Some(&pipeline_layout),
                vertex: wgpu::VertexState {
                    module: &shader,
                    entry_point: Some("vs_fullscreen"),
                    buffers: &[],
                    compilation_options: Default::default(),
                },
                primitive: wgpu::PrimitiveState::default(),
                depth_stencil: None,
                multisample: wgpu::MultisampleState::default(),
                fragment: Some(wgpu::FragmentState {
                    module: &shader,
                    entry_point: Some("fs_velocity_debug"),
                    targets: &[Some(wgpu::ColorTargetState {
                        format: dst_format,
                        blend: None,
                        write_mask: wgpu::ColorWrites::ALL,
                    })],
                    compilation_options: Default::default(),
                }),
                multiview_mask: None,
                cache: None,
            });
            vp.debug_pipeline = Some((dst_format, pipeline));
            vp.debug_bind = None;
        }
        if vp.debug_bind.is_none() {
            let velocity_view = vp.target.create_view(&wgpu::TextureViewDescriptor::default());
            vp.debug_bind = Some(device.create_bind_group(&wgpu::BindGroupDescriptor {
                label: Some("wgr_velocity_debug_bind"),
                layout: &vp.debug_layout,
                entries: &[
                    wgpu::BindGroupEntry {
                        binding: 0,
                        resource: wgpu::BindingResource::TextureView(&velocity_view),
                    },
                    wgpu::BindGroupEntry {
                        binding: 1,
                        resource: vp.debug_uniform.as_entire_binding(),
                    },
                ],
            }));
        }
        encoder.push_debug_group("wgr_velocity_debug");
        let mut pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
            label: Some("wgr_velocity_debug"),
            color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                view: dst,
                depth_slice: None,
                resolve_target: None,
                ops: wgpu::Operations {
                    load: wgpu::LoadOp::Load,
                    store: wgpu::StoreOp::Store,
                },
            })],
            depth_stencil_attachment: None,
            timestamp_writes: None,
            occlusion_query_set: None,
            multiview_mask: None,
        });
        let (_, pipeline) = vp.debug_pipeline.as_ref().expect("debug pipeline");
        pass.set_pipeline(pipeline);
        pass.set_bind_group(0, vp.debug_bind.as_ref().expect("debug bind"), &[]);
        pass.draw(0..3, 0..1);
        drop(pass);
        encoder.pop_debug_group();
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use bytemuck::Zeroable;

    #[test]
    fn halton_covers_the_pixel_without_repeats_inside_a_cycle() {
        let mut seen = Vec::new();
        for i in 1..=8u64 {
            let j = (halton(i, 2) - 0.5, halton(i, 3) - 0.5);
            assert!(j.0 >= -0.5 && j.0 < 0.5, "x jitter out of pixel: {}", j.0);
            assert!(j.1 >= -0.5 && j.1 < 0.5, "y jitter out of pixel: {}", j.1);
            for &(px, py) in &seen {
                let d2: f32 = (j.0 - px) * (j.0 - px) + (j.1 - py) * (j.1 - py);
                assert!(d2 > 1e-6, "repeated jitter phase inside one cycle");
            }
            seen.push(j);
        }
    }

    #[test]
    fn jitter_is_a_clip_space_translation_and_zero_when_disabled() {
        let mut t = Temporal::from_env();
        t.enabled = true;
        t.render_size = (1000, 500);
        t.jitter_px = [0.25, -0.5];
        // A perspective-like matrix with w = -z (column-major cols array).
        let base = [
            1.0, 0.0, 0.0, 0.0, //
            0.0, 2.0, 0.0, 0.0, //
            0.5, 0.1, 1.0, -1.0, //
            0.0, 0.0, -0.1, 0.0,
        ];
        let mut jittered = base;
        t.apply_jitter(&mut jittered);
        let p = glam::Mat4::from_cols_array(&base);
        let pj = glam::Mat4::from_cols_array(&jittered);
        let v = glam::Vec4::new(0.3, -0.2, -5.0, 1.0);
        let c = p * v;
        let cj = pj * v;
        // Same w, and NDC shifted by exactly 2*jitter_px/size.
        assert!((c.w - cj.w).abs() < 1e-6);
        let dx = cj.x / cj.w - c.x / c.w;
        let dy = cj.y / cj.w - c.y / c.w;
        assert!((dx - 2.0 * 0.25 / 1000.0).abs() < 1e-6, "dx {dx}");
        assert!((dy - 2.0 * -0.5 / 500.0).abs() < 1e-6, "dy {dy}");
        // Disabled -> exact no-op.
        let mut t2 = Temporal::from_env();
        t2.enabled = false;
        t2.render_size = (1000, 500);
        let mut untouched = base;
        t2.apply_jitter(&mut untouched);
        assert_eq!(untouched, base);
    }

    #[test]
    fn native_wind_velocity_uses_the_same_mode_for_bark_and_leaves() {
        let mut temporal = Temporal::from_env();
        temporal.enabled = true;
        temporal.reset = false;
        let mut draws: Vec<crate::ffi::WgrDraw3D> = (0..6)
            .map(|_| unsafe { std::mem::zeroed() }).collect();
        for draw in &mut draws { draw.palette_slot = crate::ffi::NO_PALETTE; }
        draws[0].mat_sun_ambient[3] = 3.0;
        draws[1].mat_sun_ambient[3] = 3.0;
        draws[1].alpha_ref = 0.5;
        draws[2].mat_sun_ambient[3] = 2.0;
        draws[3].mat_sun_ambient[3] = 2.0;
        draws[3].alpha_ref = 0.5;
        // Non-vegetation and skinned objects are still excluded.
        draws[5].mat_sun_ambient[3] = 3.0;
        draws[5].palette_slot = 0;
        assert_eq!(temporal.veg_velocity_subset(&draws),
                   vec![(0, -1.0), (1, -1.0), (2, 0.0), (3, 1.0)]);
    }

    #[test]
    fn reset_fires_on_first_frame_and_size_change_then_clears() {
        let cam = crate::ffi::WgrCamera::zeroed();
        let mut t = Temporal::from_env();
        t.enabled = true;
        t.begin_frame(&cam, (100, 100));
        assert!(t.reset, "first frame must reset");
        t.end_frame();
        t.begin_frame(&cam, (100, 100));
        assert!(!t.reset, "steady state must not reset");
        t.end_frame();
        t.begin_frame(&cam, (50, 100));
        assert!(t.reset, "render-size change must reset");
        assert_eq!(t.last_reset_reason(), "render-size change");
    }

    #[test]
    fn explicit_events_and_teleports_reset_history() {
        let mut cam = crate::ffi::WgrCamera::zeroed();
        let mut t = Temporal::from_env();
        t.enabled = true;
        t.begin_frame(&cam, (100, 100));
        t.end_frame();
        // Explicit event (world load) raised between frames.
        t.notify_reset("world load");
        t.begin_frame(&cam, (100, 100));
        assert!(t.reset && t.last_reset_reason() == "world load");
        t.end_frame();
        // Steady frame clears it.
        t.begin_frame(&cam, (100, 100));
        assert!(!t.reset);
        t.end_frame();
        // 200 m in one frame = scripted teleport, no event raised.
        cam.cam_pos = [200.0, 0.0, 0.0, 0.0];
        t.begin_frame(&cam, (100, 100));
        assert!(t.reset && t.last_reset_reason() == "camera teleport");
        t.end_frame();
        // Normal movement (2 m) must NOT reset.
        cam.cam_pos = [202.0, 0.0, 0.0, 0.0];
        t.begin_frame(&cam, (100, 100));
        assert!(!t.reset, "ordinary movement must keep history");
    }

    // Headless-device validation, because the pipelines here are built lazily at runtime:
    // a WGSL/bind mismatch would otherwise first appear in-game. The readback is asserted
    // non-vacuous both ways — a translated camera must produce NON-zero velocity and a
    // static one zero — so an all-zeros output cannot pass as success.
    #[test]
    fn cloud_history_mask_covers_terrain_and_clears_when_disabled() {
        let instance = wgpu::Instance::new(wgpu::InstanceDescriptor::new_without_display_handle());
        let adapter = pollster::block_on(instance.request_adapter(&Default::default()))
            .expect("GPU adapter required for history mask regression");
        let (device, queue) = pollster::block_on(adapter.request_device(&Default::default()))
            .expect("GPU device required for history mask regression");
        let size = wgpu::Extent3d { width: 1, height: 1, depth_or_array_layers: 1 };
        let depth = device.create_texture(&wgpu::TextureDescriptor {
            label: Some("mask_test_depth"), size, mip_level_count: 1, sample_count: 1,
            dimension: wgpu::TextureDimension::D2, format: wgpu::TextureFormat::Depth32Float,
            usage: wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::RENDER_ATTACHMENT,
            view_formats: &[],
        });
        let depth_view = depth.create_view(&Default::default());
        let cloud = device.create_texture(&wgpu::TextureDescriptor {
            label: Some("mask_test_cloud"), size, mip_level_count: 1, sample_count: 1,
            dimension: wgpu::TextureDimension::D2, format: wgpu::TextureFormat::Rgba8Unorm,
            usage: wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::COPY_DST,
            view_formats: &[],
        });
        queue.write_texture(cloud.as_image_copy(), &[0, 0, 0, 64],
            wgpu::TexelCopyBufferLayout { offset: 0, bytes_per_row: Some(4), rows_per_image: Some(1) }, size);
        let cloud_view = cloud.create_view(&Default::default());
        let readback = device.create_buffer(&wgpu::BufferDescriptor {
            label: None, size: 256, mapped_at_creation: false,
            usage: wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::MAP_READ,
        });
        let mut t = Temporal::from_env();
        t.enabled = true;
        t.reactive_water = 0.0;
        let mut cam = crate::ffi::WgrCamera::zeroed();
        cam.view = glam::Mat4::IDENTITY.to_cols_array();
        cam.proj = glam::Mat4::perspective_infinite_reverse_rh(1.0, 1.0, 0.5).to_cols_array();
        t.begin_frame(&cam, (1, 1));
        // Same depth generation across all runs: adding/removing the cloud view
        // must invalidate the bind independently, and zero tuning must clear it.
        for (cloud, strength, expected) in [
            (None, 1.0, 0u8), (Some(&cloud_view), 1.0, 191),
            (Some(&cloud_view), 0.5, 96), (Some(&cloud_view), 0.0, 0), (None, 1.0, 0),
        ] {
            t.reactive_sky = strength;
            let mut encoder = device.create_command_encoder(&Default::default());
            drop(encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
                label: None, color_attachments: &[],
                depth_stencil_attachment: Some(wgpu::RenderPassDepthStencilAttachment {
                    view: &depth_view,
                    depth_ops: Some(wgpu::Operations { load: wgpu::LoadOp::Clear(0.5), store: wgpu::StoreOp::Store }),
                    stencil_ops: None,
                }), timestamp_writes: None, occlusion_query_set: None, multiview_mask: None,
            }));
            t.render_history_control(&device, &queue, &mut encoder, &depth_view, 1, (1, 1), None, cloud);
            encoder.copy_texture_to_buffer(t.history_control_texture().unwrap().as_image_copy(),
                wgpu::TexelCopyBufferInfo { buffer: &readback,
                    layout: wgpu::TexelCopyBufferLayout { offset: 0, bytes_per_row: Some(256), rows_per_image: Some(1) } }, size);
            queue.submit([encoder.finish()]);
            let (tx, rx) = std::sync::mpsc::channel();
            readback.slice(..).map_async(wgpu::MapMode::Read, move |r| { tx.send(r).unwrap(); });
            device.poll(wgpu::PollType::wait_indefinitely()).unwrap();
            rx.recv().unwrap().unwrap();
            let actual = readback.slice(..).get_mapped_range()[0];
            readback.unmap();
            assert!(actual.abs_diff(expected) <= 1, "mask {actual}, expected {expected}");
        }
    }

    #[test]
    fn velocity_pass_runs_headlessly_and_reprojection_answers_both_ways() {
        let instance =
            wgpu::Instance::new(wgpu::InstanceDescriptor::new_without_display_handle_from_env());
        let Ok(adapter) = pollster::block_on(instance.request_adapter(
            &wgpu::RequestAdapterOptions {
                power_preference: wgpu::PowerPreference::default(),
                compatible_surface: None,
                force_fallback_adapter: false,
            },
        )) else {
            return; // no GPU on this runner
        };
        let Ok((device, queue)) =
            pollster::block_on(adapter.request_device(&wgpu::DeviceDescriptor::default()))
        else {
            return;
        };
        const W: u32 = 64;
        const H: u32 = 64;
        let depth_tex = device.create_texture(&wgpu::TextureDescriptor {
            label: Some("test_depth"),
            size: wgpu::Extent3d {
                width: W,
                height: H,
                depth_or_array_layers: 1,
            },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: wgpu::TextureFormat::Depth32Float,
            usage: wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::TEXTURE_BINDING,
            view_formats: &[],
        });
        let depth_view = depth_tex.create_view(&wgpu::TextureViewDescriptor::default());
        // A mid-range stored depth everywhere (reversed storage: 0.5 is a real surface).
        let mut encoder = device.create_command_encoder(&Default::default());
        let pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
            label: Some("test_depth_clear"),
            color_attachments: &[],
            depth_stencil_attachment: Some(wgpu::RenderPassDepthStencilAttachment {
                view: &depth_view,
                depth_ops: Some(wgpu::Operations {
                    load: wgpu::LoadOp::Clear(0.5),
                    store: wgpu::StoreOp::Store,
                }),
                stencil_ops: None,
            }),
            timestamp_writes: None,
            occlusion_query_set: None,
            multiview_mask: None,
        });
        drop(pass);
        queue.submit([encoder.finish()]);

        let run = |cam_prev: crate::ffi::WgrCamera, cam_cur: crate::ffi::WgrCamera| -> (f32, f32) {
            let mut t = Temporal::from_env();
            t.enabled = true;
            t.begin_frame(&cam_prev, (W, H));
            t.end_frame();
            t.begin_frame(&cam_cur, (W, H));
            let mut encoder = device.create_command_encoder(&Default::default());
            t.render_velocity(&device, &queue, &mut encoder, &depth_view, 1, (W, H));
            // Exercise the debug pipeline too (compilation + binding validation).
            let dbg_tex = device.create_texture(&wgpu::TextureDescriptor {
                label: Some("test_debug_target"),
                size: wgpu::Extent3d {
                    width: W,
                    height: H,
                    depth_or_array_layers: 1,
                },
                mip_level_count: 1,
                sample_count: 1,
                dimension: wgpu::TextureDimension::D2,
                format: wgpu::TextureFormat::Rgba8Unorm,
                usage: wgpu::TextureUsages::RENDER_ATTACHMENT,
                view_formats: &[],
            });
            let dbg_view = dbg_tex.create_view(&Default::default());
            t.debug_view = 1;
            t.render_debug(
                &device,
                &queue,
                &mut encoder,
                &dbg_view,
                wgpu::TextureFormat::Rgba8Unorm,
                (W, H),
            );
            // REN-TEMP-001K: exercise the debug resolve + history view too (their
            // pipelines are lazily built — this is their only pre-game validation).
            let color_tex = device.create_texture(&wgpu::TextureDescriptor {
                label: Some("test_hdr_color"),
                size: wgpu::Extent3d {
                    width: W,
                    height: H,
                    depth_or_array_layers: 1,
                },
                mip_level_count: 1,
                sample_count: 1,
                dimension: wgpu::TextureDimension::D2,
                format: wgpu::TextureFormat::Rgba16Float,
                usage: wgpu::TextureUsages::RENDER_ATTACHMENT
                    | wgpu::TextureUsages::TEXTURE_BINDING,
                view_formats: &[],
            });
            let color_view = color_tex.create_view(&Default::default());
            t.debug_view = 2;
            t.render_debug_resolve(&device, &queue, &mut encoder, &color_view, (W, H));
            t.render_debug_resolve(&device, &queue, &mut encoder, &color_view, (W, H));
            // REN-TEMP-001J: history-control pipeline (lazily built like the others).
            t.render_history_control(&device, &queue, &mut encoder, &depth_view, 1, (W, H), Some(5.0), None);
            t.render_debug(
                &device,
                &queue,
                &mut encoder,
                &dbg_view,
                wgpu::TextureFormat::Rgba8Unorm,
                (W, H),
            );
            t.debug_view = 1;
            // Read back the velocity centre texel (Rg16Float, 4 B/px; W=64 -> 256 B rows).
            let buf = device.create_buffer(&wgpu::BufferDescriptor {
                label: Some("test_readback"),
                size: (W * H * 4) as u64,
                usage: wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::MAP_READ,
                mapped_at_creation: false,
            });
            let vel_tex = &t.velocity.as_ref().expect("velocity pass exists").target;
            encoder.copy_texture_to_buffer(
                wgpu::TexelCopyTextureInfo {
                    texture: vel_tex,
                    mip_level: 0,
                    origin: wgpu::Origin3d::ZERO,
                    aspect: wgpu::TextureAspect::All,
                },
                wgpu::TexelCopyBufferInfo {
                    buffer: &buf,
                    layout: wgpu::TexelCopyBufferLayout {
                        offset: 0,
                        bytes_per_row: Some(W * 4),
                        rows_per_image: Some(H),
                    },
                },
                wgpu::Extent3d {
                    width: W,
                    height: H,
                    depth_or_array_layers: 1,
                },
            );
            queue.submit([encoder.finish()]);
            let slice = buf.slice(..);
            let (tx, rx) = std::sync::mpsc::channel();
            slice.map_async(wgpu::MapMode::Read, move |r| {
                tx.send(r).ok();
            });
            device.poll(wgpu::PollType::wait_indefinitely()).unwrap();
            rx.recv().expect("map callback").expect("map ok");
            let data = slice.get_mapped_range();
            let centre = ((H / 2) * W + W / 2) as usize * 4;
            let f16 = |lo: u8, hi: u8| -> f32 {
                let h = u16::from_le_bytes([lo, hi]);
                let sign = if h >> 15 == 1 { -1.0f32 } else { 1.0 };
                let exp = ((h >> 10) & 0x1f) as i32;
                let man = (h & 0x3ff) as f32;
                match exp {
                    0 => sign * man * (2.0f32).powi(-24),
                    31 => sign * f32::INFINITY,
                    _ => sign * (1.0 + man / 1024.0) * (2.0f32).powi(exp - 15),
                }
            };
            (
                f16(data[centre], data[centre + 1]),
                f16(data[centre + 2], data[centre + 3]),
            )
        };

        let mut cam = crate::ffi::WgrCamera::zeroed();
        cam.proj = glam::Mat4::perspective_infinite_reverse_rh(1.0, 1.0, 0.5).to_cols_array();
        cam.view = glam::Mat4::IDENTITY.to_cols_array();
        // Static camera: zero velocity everywhere.
        let (sx, sy) = run(cam, cam);
        assert!(
            sx.abs() < 1e-4 && sy.abs() < 1e-4,
            "static camera must give zero velocity, got ({sx}, {sy})"
        );
        // Rotated camera (yaw): the centre pixel must move horizontally.
        let mut cam_rot = cam;
        cam_rot.view = glam::Mat4::from_rotation_y(0.02f32).to_cols_array();
        let (rx_, ry_) = run(cam, cam_rot);
        assert!(
            rx_.abs() > 1e-3,
            "a yaw must produce horizontal velocity, got ({rx_}, {ry_})"
        );
        assert!(rx_.is_finite() && ry_.is_finite());
        // Translated camera (strafe): geometry is camera-relative, so translation reaches
        // the reprojection ONLY through cam_pos / cam_delta — this is the arm that catches
        // a delta that never propagates. Centre depth 0.5 stored = 1 m away at near 0.5;
        // a half-metre strafe at 1 m must move the pixel a lot.
        let mut cam_strafe = cam;
        cam_strafe.cam_pos = [0.5, 0.0, 0.0, 0.0];
        let (tx_, ty_) = run(cam, cam_strafe);
        assert!(
            tx_.abs() > 1e-3,
            "a strafe must produce horizontal velocity, got ({tx_}, {ty_})"
        );
    }

    #[test]
    fn static_camera_reprojects_to_zero_velocity() {
        // The exact maths the WGSL runs, in Rust: a static camera must produce ndc_prev ==
        // ndc_cur for any depth, and a translated camera must not (the shader's own logic
        // is pinned by the device tests; this pins the parameter construction).
        let cam = crate::ffi::WgrCamera {
            proj: glam::Mat4::perspective_infinite_reverse_rh(1.0, 1.6, 0.5).to_cols_array(),
            view: glam::Mat4::IDENTITY.to_cols_array(),
            ..crate::ffi::WgrCamera::zeroed()
        };
        let mut t = Temporal::from_env();
        t.enabled = true;
        t.begin_frame(&cam, (100, 100));
        t.end_frame();
        t.begin_frame(&cam, (100, 100));
        let p = t.params();
        assert_eq!(p.cam_delta, [0.0, 0.0, 0.0, 1.0]);
        let inv = glam::Mat4::from_cols_array(&p.inv_view_proj);
        let prev = glam::Mat4::from_cols_array(&p.prev_view_proj);
        let ndc = glam::Vec4::new(0.3, -0.4, 0.7, 1.0);
        let rel = inv * ndc;
        let rel = rel / rel.w;
        let back = prev * glam::Vec4::new(rel.x, rel.y, rel.z, 1.0);
        let back_ndc = glam::Vec2::new(back.x / back.w, back.y / back.w);
        assert!((back_ndc.x - 0.3).abs() < 1e-4 && (back_ndc.y + 0.4).abs() < 1e-4);
    }

    #[test]
    fn jittered_depth_recovers_static_world_motion_during_camera_travel() {
        let projection = glam::Mat4::perspective_rh(1.0, 1.6, 0.5, 50000.0);
        let previous = crate::ffi::WgrCamera {
            proj: projection.to_cols_array(),
            view: glam::Mat4::IDENTITY.to_cols_array(),
            ..crate::ffi::WgrCamera::zeroed()
        };
        let mut current = previous;
        current.cam_pos = [10.0, 2.0, 4.0, 0.0];
        current.view = glam::Mat4::from_rotation_y(0.25).to_cols_array();
        let world = glam::Vec3::new(3.0, 1.0, -40.0);
        for size in [(100, 100), (1280, 720)] {
            let mut t = Temporal::from_env();
            t.enabled = true;
            t.begin_frame(&previous, size);
            t.end_frame();
            t.begin_frame(&current, size);
            t.jitter_px = [0.49, -0.37];
            let mut raster_proj = current.proj;
            t.apply_jitter(&mut raster_proj);
            let rel = world - glam::Vec3::new(10.0, 2.0, 4.0);
            let view = glam::Mat4::from_cols_array(&current.view);
            let raster_clip = glam::Mat4::from_cols_array(&raster_proj) * view * rel.extend(1.0);
            let raster_ndc = raster_clip.truncate() / raster_clip.w;
            let p = t.params();
            let ndc = glam::Vec2::new(raster_ndc.x - p.misc[0], raster_ndc.y - p.misc[1]);
            let reconstructed = glam::Mat4::from_cols_array(&p.inv_view_proj)
                * glam::Vec4::new(ndc.x, ndc.y, raster_ndc.z, 1.0);
            let recovered = reconstructed.truncate() / reconstructed.w
                + glam::Vec3::new(p.cam_delta[0], p.cam_delta[1], p.cam_delta[2]);
            let back = glam::Mat4::from_cols_array(&p.prev_view_proj) * recovered.extend(1.0);
            let actual = ndc - glam::Vec2::new(back.x / back.w, back.y / back.w);
            let cur_clip = projection * view * rel.extend(1.0);
            let prev_clip = projection * world.extend(1.0);
            let expected = glam::Vec2::new(cur_clip.x / cur_clip.w, cur_clip.y / cur_clip.w)
                - glam::Vec2::new(prev_clip.x / prev_clip.w, prev_clip.y / prev_clip.w);
            assert!((actual - expected).length() < 0.0001, "{actual:?} != {expected:?}");
        }
        assert!(include_str!("temporal.wgsl").contains("1.0 - uv.y * 2.0) - tp.misc.xy"));
    }
}
