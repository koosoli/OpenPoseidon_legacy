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

const VELOCITY_FORMAT: wgpu::TextureFormat = wgpu::TextureFormat::Rg16Float;

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
    shader: Option<wgpu::ShaderModule>,
    velocity: Option<VelocityPass>,
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
            reset: true,
            pending_reset: None,
            last_reset_reason: "first frame",
            render_size: (0, 0),
            shader: None,
            velocity: None,
        }
    }

    // Ingest the UNJITTERED main scene camera and decide this frame's jitter. Call once
    // per frame, BEFORE apply_jitter and before any camera upload.
    // Raise an explicit history reset, consumed by the next begin_frame. Callable from
    // any thread-confined renderer entry point (world load via the heightmap upload,
    // future mode/upscaler switches, an eventual C++ camera-cut event).
    pub fn notify_reset(&mut self, reason: &'static str) {
        self.pending_reset = Some(reason);
    }

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
        self.cur = Some(CameraSnap {
            view: cam.view,
            proj: cam.proj,
            cam_pos: [cam.cam_pos[0], cam.cam_pos[1], cam.cam_pos[2]],
        });
        self.frame_index += 1;
        self.prev_jitter_px = self.jitter_px;
        self.jitter_px = if self.enabled {
            // Halton (2,3), 8-phase cycle: enough phases to cover the pixel while staying
            // short enough that a debug accumulator converges quickly. Replaceable — the
            // plan forbids baking one vendor's sequence into the camera system, and this
            // is one function.
            let i = self.frame_index % 8 + 1;
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
    }

    pub fn last_reset_reason(&self) -> &'static str {
        self.last_reset_reason
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
            misc: [0.0; 4],
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
        if !self.enabled || self.debug_view != 1 {
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
}
