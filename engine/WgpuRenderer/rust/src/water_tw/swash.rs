//! (OP) The swash field around the camera, for the shorebirds (BirdFlocks.cpp's sanderlings).
//!
//! Tidewater's `SwashProbe.js` evaluates the shore waves' run-up at the birds' own points on the GPU
//! with the very code the water draws (ShoreWaves) and reads it back (1-3 frames old, like its
//! WaterQuery). OP evaluates the same `shoreEvaluateNoNormal` on a fixed lattice instead
//! (`fs_tw_swash` in tw_probe.wgsl: `SW_N` x `SW_N` texels `SW_STEP` m apart around the camera), so
//! the birds need no query upload: the CPU interpolates the field wherever a bird stands. Per texel:
//! run-up of the current wave and the point's own distance up the beach (both m, along the beach),
//! the leading edge's speed (> 0 while the water runs up) and the swash cycle's phase; w = -1 where
//! no shore waves run. Rendering only; nothing here feeds gameplay.

use std::sync::atomic::{AtomicU8, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Instant;

/// Lattice size and step (m), mirrored by `SW_N` / `SW_STEP` in tw_probe.wgsl: 192 m around the camera.
pub const N: u32 = 64;
pub const STEP: f32 = 3.0;
const RING: usize = 3;
const ROW: u64 = N as u64 * 16;
/// The swash moves on a wave period (~9 s); a field every few frames is plenty (Tidewater: 6).
const INTERVAL: u32 = 6;

/// The latest field read back: lanes (first lattice x, z, step, size) and (runup, inland, dRdt, tau).
pub struct SwashField {
    pub lanes: [f32; 4],
    pub texels: Vec<[f32; 4]>,
    pub at: Instant,
}
static FIELD: Mutex<Option<SwashField>> = Mutex::new(None);

pub(crate) fn clear_field() {
    if let Ok(mut g) = FIELD.lock() {
        *g = None;
    }
}

/// Copies the latest field out (any thread): the texel count, or 0 when there is none.
pub fn field(out: &mut [[f32; 4]], lanes: &mut [f32; 4], age_ms: &mut f32) -> usize {
    let Ok(g) = FIELD.lock() else { return 0 };
    let Some(g) = g.as_ref() else { return 0 };
    if out.len() < g.texels.len() {
        return 0;
    }
    out[..g.texels.len()].copy_from_slice(&g.texels);
    *lanes = g.lanes;
    *age_ms = g.at.elapsed().as_secs_f32() * 1000.0;
    g.texels.len()
}

/// The field's lanes for the probe's lanes (the lattice `swashOrigin` derives in the shader).
pub fn lanes(probe: [f32; 4]) -> [f32; 4] {
    let cam = |b: f32| b + probe[3] * 0.5 * probe[2];
    let o = |c: f32| ((c / STEP).floor() - (N / 2) as f32) * STEP;
    [o(cam(probe[0])), o(cam(probe[1])), STEP, N as f32]
}

enum Rb {
    Idle,
    Copied,
    Mapping(Arc<AtomicU8>),
}

struct Slot {
    buf: wgpu::Buffer,
    state: Rb,
    lanes: [f32; 4],
}

pub struct Swash {
    tex: wgpu::Texture,
    view: wgpu::TextureView,
    pipeline: wgpu::RenderPipeline,
    empty: wgpu::BindGroup,
    slots: Vec<Slot>,
    next: usize,
    frame: u32,
}

impl Swash {
    pub fn new(device: &wgpu::Device, group1: &wgpu::BindGroupLayout, module: &wgpu::ShaderModule) -> Self {
        clear_field();
        let tex = device.create_texture(&wgpu::TextureDescriptor {
            label: Some("tw_swash_field"),
            size: wgpu::Extent3d { width: N, height: N, depth_or_array_layers: 1 },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: wgpu::TextureFormat::Rgba32Float,
            usage: wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::COPY_SRC,
            view_formats: &[],
        });
        let view = tex.create_view(&Default::default());
        let empty_layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor { label: Some("tw_swash_group0"), entries: &[] });
        let empty = device.create_bind_group(&wgpu::BindGroupDescriptor { label: Some("tw_swash_group0"), layout: &empty_layout, entries: &[] });
        let pl = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("tw_swash"),
            bind_group_layouts: &[Some(&empty_layout), Some(group1)],
            immediate_size: 0,
        });
        let pipeline = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
            label: Some("tw_swash"),
            layout: Some(&pl),
            vertex: wgpu::VertexState { module, entry_point: Some("vs_tw_probe"), compilation_options: Default::default(), buffers: &[] },
            primitive: wgpu::PrimitiveState::default(),
            depth_stencil: None,
            multisample: wgpu::MultisampleState::default(),
            fragment: Some(wgpu::FragmentState {
                module,
                entry_point: Some("fs_tw_swash"),
                compilation_options: Default::default(),
                targets: &[Some(wgpu::ColorTargetState { format: wgpu::TextureFormat::Rgba32Float, blend: None, write_mask: wgpu::ColorWrites::ALL })],
            }),
            multiview_mask: None,
            cache: None,
        });
        let slots = (0..RING)
            .map(|_| Slot {
                buf: device.create_buffer(&wgpu::BufferDescriptor {
                    label: Some("tw_swash_readback"),
                    size: ROW * N as u64,
                    usage: wgpu::BufferUsages::MAP_READ | wgpu::BufferUsages::COPY_DST,
                    mapped_at_creation: false,
                }),
                state: Rb::Idle,
                lanes: [0.0; 4],
            })
            .collect();
        Self { tex, view, pipeline, empty, slots, next: 0, frame: 0 }
    }

    /// Encoder side, with the probe (after everything the shore waves read): every `INTERVAL`
    /// frames the field and a copy of it for the CPU. `probe`: this frame's probe lanes.
    pub fn record(&mut self, encoder: &mut wgpu::CommandEncoder, group1: &wgpu::BindGroup, probe: [f32; 4]) {
        self.frame = self.frame.wrapping_add(1);
        if self.frame % INTERVAL != 0 || probe[3] < 2.0 || !matches!(self.slots[self.next].state, Rb::Idle) {
            return;
        }
        {
            let mut pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
                label: Some("tw_swash"),
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
        let slot = &mut self.slots[self.next];
        encoder.copy_texture_to_buffer(
            wgpu::TexelCopyTextureInfo { texture: &self.tex, mip_level: 0, origin: wgpu::Origin3d { x: 0, y: 0, z: 0 }, aspect: wgpu::TextureAspect::All },
            wgpu::TexelCopyBufferInfo {
                buffer: &slot.buf,
                layout: wgpu::TexelCopyBufferLayout { offset: 0, bytes_per_row: Some(ROW as u32), rows_per_image: Some(N) },
            },
            wgpu::Extent3d { width: N, height: N, depth_or_array_layers: 1 },
        );
        slot.state = Rb::Copied;
        slot.lanes = lanes(probe);
        self.next = (self.next + 1) % RING;
    }

    /// CPU side, once per frame before recording: request maps, publish the newest completed field.
    pub fn poll(&mut self) {
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
                        let texels: Vec<[f32; 4]> = {
                            let data = slot.buf.slice(..).get_mapped_range().expect("wgpu: mapped range of a mapped buffer");
                            data.chunks_exact(16)
                                .map(|c| {
                                    let v = |k: usize| f32::from_le_bytes([c[k], c[k + 1], c[k + 2], c[k + 3]]);
                                    [v(0), v(4), v(8), v(12)]
                                })
                                .collect()
                        };
                        slot.buf.unmap();
                        if texels.iter().flatten().all(|v| v.is_finite()) {
                            if let Ok(mut g) = FIELD.lock() {
                                *g = Some(SwashField { lanes: slot.lanes, texels, at: Instant::now() });
                            }
                        }
                        slot.state = Rb::Idle;
                    }
                    _ => slot.state = Rb::Idle,
                },
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn field_lattice_is_centred_on_the_probe_camera() {
        for cam in [[1.1f32, 2.0, 3.3], [-7.6, 0.0, 12345.3], [9950.0, 4.0, 1440.0]] {
            let p = super::super::probe::Probe::lanes(cam);
            let l = lanes(p);
            for (o, c) in [(l[0], cam[0]), (l[1], cam[2])] {
                assert!(((o / STEP) - (o / STEP).round()).abs() < 1e-4, "on the lattice");
                let lo = o + (N / 2) as f32 * STEP;
                assert!(lo - STEP <= c && c < lo + STEP + 1e-3, "{o} {c}");
            }
        }
    }
}
