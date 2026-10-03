//! TW-WATER W9a (OP, not ported): the water surface around the camera, as it is drawn.
//!
//! One small render pass (`tw_probe.wgsl`) evaluates the water vertex shader's own displacement
//! (FFT, shore waves and swash sheet, wakes, bursts) on an `N` x `N` grid of `STEP` m centred on
//! the camera, into an r32float texture of absolute surface heights. Two readers:
//! * the underwater composite (`tw_underwater.wgsl`) samples it for the waterline across the lens
//!   and the camera's depth, in the same frame -- so the split between the view above and below
//!   the water follows the surface actually drawn, wherever the camera is (in the surf, under a
//!   breaking crest, bobbing at the waterline);
//! * the CPU reads the centre texel back (1-3 frames late, never blocking) for the compositor's
//!   engage gate and the water material's view side, which Current OP's reference waves decided
//!   before (they are not the surface drawn in this mode).
//! Rendering only: nothing here feeds physics or gameplay -- with one exception (Sinkhole W3): the
//! whole grid is published ([`drawn_grid`], `wgr_water_drawn_grid`) so the engine can hold a swimmer
//! near the camera, and the camera following him, on the surface as drawn instead of the CPU wave
//! predictor, which matches it in scale but not crest for crest. Presentation of the local view only.

use std::sync::atomic::{AtomicU8, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Instant;

/// Grid size (texels per side) and step (m): 16 m square, far wider than the lens (the near plane
/// is centimetres from the eye) and the waterline's neighbourhood; (Sinkhole W3) wide enough to hold
/// the man a 3rd person camera follows.
pub const N: u32 = 64;
pub const STEP: f32 = 0.25;
const RING: usize = 3;
/// The whole grid is copied back, one rgba32float row of N texels per row.
const ROW: u64 = N as u64 * 16;

/// Sinkhole W3: the latest grid read back -- (lanes, displaced vertex x, y, z per texel, when).
pub struct DrawnGrid {
    pub lanes: [f32; 4],
    pub xyz: Vec<[f32; 3]>,
    pub at: Instant,
}
static DRAWN: Mutex<Option<DrawnGrid>> = Mutex::new(None);

/// No completed surface survives construction or a backend swap.
pub(crate) fn clear_drawn_grid() {
    if let Ok(mut g) = DRAWN.lock() {
        *g = None;
    }
}

/// Copies the latest drawn grid out (any thread): the texel count, or 0 when there is none.
pub fn drawn_grid(out: &mut [[f32; 3]], lanes: &mut [f32; 4], age_ms: &mut f32) -> usize {
    let Ok(g) = DRAWN.lock() else { return 0 };
    let Some(g) = g.as_ref() else { return 0 };
    if out.len() < g.xyz.len() {
        return 0;
    }
    out[..g.xyz.len()].copy_from_slice(&g.xyz);
    *lanes = g.lanes;
    *age_ms = g.at.elapsed().as_secs_f32() * 1000.0;
    g.xyz.len()
}

enum Rb {
    Idle,
    Copied,
    Mapping(Arc<AtomicU8>),
}

struct Slot {
    buf: wgpu::Buffer,
    state: Rb,
    /// the grid's lanes when it was copied
    lanes: [f32; 4],
}

pub struct Probe {
    pub view: wgpu::TextureView,
    tex: wgpu::Texture,
    pipeline: wgpu::RenderPipeline,
    empty: wgpu::BindGroup,
    slots: Vec<Slot>,
    next: usize,
    /// the surface height under the camera from the latest completed readback (m, absolute)
    pub height: Option<f32>,
    /// frames since `height` was read
    pub age: u32,
    /// whether the grid has been drawn at least once (until then the composite uses the FFT)
    pub ready: bool,
}

impl Probe {
    pub fn new(device: &wgpu::Device, group1: &wgpu::BindGroupLayout, module: &wgpu::ShaderModule) -> Self {
        clear_drawn_grid();
        let tex = device.create_texture(&wgpu::TextureDescriptor {
            label: Some("tw_probe_heights"),
            size: wgpu::Extent3d { width: N, height: N, depth_or_array_layers: 1 },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: wgpu::TextureFormat::Rgba32Float,
            usage: wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::COPY_SRC,
            view_formats: &[],
        });
        let view = tex.create_view(&Default::default());
        let empty_layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor { label: Some("tw_probe_group0"), entries: &[] });
        let empty = device.create_bind_group(&wgpu::BindGroupDescriptor { label: Some("tw_probe_group0"), layout: &empty_layout, entries: &[] });
        let pl = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("tw_probe"),
            bind_group_layouts: &[Some(&empty_layout), Some(group1)],
            immediate_size: 0,
        });
        let pipeline = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
            label: Some("tw_probe"),
            layout: Some(&pl),
            vertex: wgpu::VertexState { module, entry_point: Some("vs_tw_probe"), compilation_options: Default::default(), buffers: &[] },
            primitive: wgpu::PrimitiveState::default(),
            depth_stencil: None,
            multisample: wgpu::MultisampleState::default(),
            fragment: Some(wgpu::FragmentState {
                module,
                entry_point: Some("fs_tw_probe"),
                compilation_options: Default::default(),
                targets: &[Some(wgpu::ColorTargetState { format: wgpu::TextureFormat::Rgba32Float, blend: None, write_mask: wgpu::ColorWrites::ALL })],
            }),
            multiview_mask: None,
            cache: None,
        });
        let slots = (0..RING)
            .map(|i| Slot {
                buf: device.create_buffer(&wgpu::BufferDescriptor {
                    label: Some(if i == 0 { "tw_probe_readback_0" } else { "tw_probe_readback" }),
                    size: ROW * N as u64,
                    usage: wgpu::BufferUsages::MAP_READ | wgpu::BufferUsages::COPY_DST,
                    mapped_at_creation: false,
                }),
                state: Rb::Idle,
                lanes: [0.0; 4],
            })
            .collect();
        Self { view, tex, pipeline, empty, slots, next: 0, height: None, age: u32::MAX, ready: false }
    }

    /// The `tw.probe` lanes for this frame's camera: the grid's first lattice point (W12d: the
    /// finest CDLOD lattice, `STEP`-aligned, the camera between texels N/2 and N/2 + 1), step, size.
    /// W12d: the texels hold the displaced mesh vertices (rgba32float), which the composite
    /// interpolates as the mesh's own triangles.
    pub fn lanes(cam: [f32; 3]) -> [f32; 4] {
        let base = |c: f32| ((c as f64 / STEP as f64).floor() - (N / 2) as f64) * STEP as f64;
        [base(cam[0]) as f32, base(cam[2]) as f32, STEP, N as f32]
    }

    /// Encoder side, after the FFT, wakes and shore simulation of the frame: the grid, and a copy of
    /// it for the CPU. `lanes`: this frame's [`Self::lanes`].
    pub fn record(&mut self, encoder: &mut wgpu::CommandEncoder, group1: &wgpu::BindGroup, lanes: [f32; 4]) {
        {
            let mut pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
                label: Some("tw_probe"),
                color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                    view: &self.view,
                    depth_slice: None,
                    resolve_target: None,
                    ops: wgpu::Operations { load: wgpu::LoadOp::Clear(wgpu::Color::BLACK), store: wgpu::StoreOp::Store },
                })],
                depth_stencil_attachment: None,
                timestamp_writes: None,
                occlusion_query_set: None,
                multiview_mask: None,
            });
            pass.set_pipeline(&self.pipeline);
            pass.set_bind_group(0, &self.empty, &[]);
            pass.set_bind_group(1, group1, &[]);
            pass.draw(0..3, 0..1);
        }
        self.ready = true;
        let slot = &mut self.slots[self.next];
        if matches!(slot.state, Rb::Idle) {
            encoder.copy_texture_to_buffer(
                wgpu::TexelCopyTextureInfo {
                    texture: &self.tex,
                    mip_level: 0,
                    origin: wgpu::Origin3d { x: 0, y: 0, z: 0 },
                    aspect: wgpu::TextureAspect::All,
                },
                wgpu::TexelCopyBufferInfo {
                    buffer: &slot.buf,
                    layout: wgpu::TexelCopyBufferLayout { offset: 0, bytes_per_row: Some(ROW as u32), rows_per_image: Some(N) },
                },
                wgpu::Extent3d { width: N, height: N, depth_or_array_layers: 1 },
            );
            slot.state = Rb::Copied;
            slot.lanes = lanes;
            self.next = (self.next + 1) % RING;
        }
    }

    /// CPU side, once per frame before recording (the renderer's per-frame device poll completes
    /// the maps): request maps for copied slots and take the newest completed value.
    pub fn poll(&mut self) {
        self.age = self.age.saturating_add(1);
        for slot in &mut self.slots {
            match &slot.state {
                Rb::Idle => {}
                Rb::Copied => {
                    let flag = Arc::new(AtomicU8::new(0));
                    let f = flag.clone();
                    slot.buf.slice(..).map_async(wgpu::MapMode::Read, move |r| f.store(if r.is_ok() { 1 } else { 2 }, Ordering::Release));
                    slot.state = Rb::Mapping(flag);
                }
                Rb::Mapping(flag) => match flag.load(Ordering::Acquire) {
                    0 => {}
                    1 => {
                        let (h, xyz) = {
                            let data = slot.buf.slice(..).get_mapped_range();
                            let lane = |row: usize, col: usize, l: usize| {
                                // (W12d) rgba32float vertices: x, height, z
                                let o = row * ROW as usize + col * 16 + l * 4;
                                f32::from_le_bytes([data[o], data[o + 1], data[o + 2], data[o + 3]])
                            };
                            let c = N as usize / 2;
                            // the camera sits between the four centre texels
                            let h = (lane(c, c, 1) + lane(c, c + 1, 1) + lane(c + 1, c, 1) + lane(c + 1, c + 1, 1)) * 0.25;
                            let mut xyz = Vec::with_capacity((N * N) as usize);
                            for row in 0..N as usize {
                                for col in 0..N as usize {
                                    xyz.push([lane(row, col, 0), lane(row, col, 1), lane(row, col, 2)]);
                                }
                            }
                            (h, xyz)
                        };
                        slot.buf.unmap();
                        if h.is_finite()
                            && slot.lanes.iter().all(|v| v.is_finite())
                            && xyz.iter().flatten().all(|v| v.is_finite())
                        {
                            self.height = Some(h);
                            self.age = 0;
                            if let Ok(mut g) = DRAWN.lock() {
                                *g = Some(DrawnGrid { lanes: slot.lanes, xyz, at: Instant::now() });
                            }
                        }
                        slot.state = Rb::Idle;
                    }
                    _ => slot.state = Rb::Idle,
                },
            }
        }
    }

    /// The surface height under the camera when a recent readback exists (at most ~10 frames old).
    pub fn camera_height(&self) -> Option<f32> {
        self.height.filter(|_| self.age <= 10)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn grid_is_the_lattice_around_the_camera() {
        // texel i: lattice point base + i * STEP; the camera between texels N/2 and N/2 + 1
        for cam in [[1.1f32, 2.0, 3.3], [-7.6, 0.0, 12345.3], [0.0, 0.0, 0.0]] {
            let l = Probe::lanes(cam);
            for (b, c) in [(l[0], cam[0]), (l[1], cam[2])] {
                let k = b / STEP;
                assert!((k - k.round()).abs() < 1e-4, "on the lattice");
                let lo = b + (N / 2) as f32 * STEP;
                assert!(lo <= c + 1e-3 && c < lo + STEP + 1e-3);
            }
            assert_eq!(l[2], STEP);
        }
        assert!(STEP * N as f32 >= 8.0);
    }
}
