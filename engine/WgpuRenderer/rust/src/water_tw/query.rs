//! (OP) Water height queries at arbitrary points, as the water draws them -- Tidewater's
//! `WaterQuery.js` (island-expansion: "gameplay points (boat hull samples, swimmer, particles...)").
//!
//! The probe (probe.rs) covers 16 m around the camera; a boat 15 m away floated on the CPU wave
//! predictor instead, which matches the drawn sea in scale but not crest for crest, so in a storm it
//! hung over the drawn troughs and sank into the crests (owner, 2026-10-13: "the boat jumps around
//! like crazy in the air"). Here the engine hands up to `MAX_POINTS` world points per frame; one tiny
//! render pass (`fs_tw_query`, tw_probe.wgsl) evaluates the vertex shader's own displacement there,
//! inverting the horizontal motion, and writes height and slope; the CPU reads them back 1-3 frames
//! late (never blocking), with the points they were asked at. Like the probe this serves the local
//! view: the engine falls back to the predictor when there is no fresh result (servers, other modes).

use std::sync::atomic::{AtomicU8, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Instant;

/// Points per frame (the shader's `TW_QUERY_N`): Tidewater's 128, six boats of 21 hull samples.
pub const MAX_POINTS: usize = 128;
const RING: usize = 3;
const ROW: u64 = MAX_POINTS as u64 * 16;

/// The latest results: the points asked (x, z), and per point (height, dh/dx, dh/dz, valid).
pub struct QueryResults {
    pub points: Vec<[f32; 2]>,
    pub values: Vec<[f32; 4]>,
    pub at: Instant,
}
static RESULTS: Mutex<Option<QueryResults>> = Mutex::new(None);
static PENDING: Mutex<Vec<[f32; 2]>> = Mutex::new(Vec::new());

pub(crate) fn clear() {
    if let Ok(mut g) = RESULTS.lock() {
        *g = None;
    }
    if let Ok(mut p) = PENDING.lock() {
        p.clear();
    }
}

/// The points to evaluate from the next frame on (any thread); at most `MAX_POINTS` are kept.
pub fn set_points(points: &[[f32; 2]]) {
    if let Ok(mut p) = PENDING.lock() {
        p.clear();
        p.extend(points.iter().filter(|q| q[0].is_finite() && q[1].is_finite()).take(MAX_POINTS));
    }
}

/// Copies the latest results out (any thread): the point count, or 0 when there are none.
pub fn results(points: &mut [[f32; 2]], values: &mut [[f32; 4]], age_ms: &mut f32) -> usize {
    let Ok(g) = RESULTS.lock() else { return 0 };
    let Some(g) = g.as_ref() else { return 0 };
    let n = g.points.len().min(points.len()).min(values.len());
    points[..n].copy_from_slice(&g.points[..n]);
    values[..n].copy_from_slice(&g.values[..n]);
    *age_ms = g.at.elapsed().as_secs_f32() * 1000.0;
    n
}

enum Rb {
    Idle,
    Copied,
    Mapping(Arc<AtomicU8>),
}

struct Slot {
    buf: wgpu::Buffer,
    state: Rb,
    points: Vec<[f32; 2]>,
}

pub struct Query {
    tex: wgpu::Texture,
    view: wgpu::TextureView,
    pipeline: wgpu::RenderPipeline,
    empty: wgpu::BindGroup,
    uniform: wgpu::Buffer,
    group2: wgpu::BindGroup,
    slots: Vec<Slot>,
    next: usize,
    /// the points uploaded for this frame's pass (empty: no pass)
    current: Vec<[f32; 2]>,
}

impl Query {
    pub fn new(device: &wgpu::Device, group1: &wgpu::BindGroupLayout, module: &wgpu::ShaderModule) -> Self {
        clear();
        let tex = device.create_texture(&wgpu::TextureDescriptor {
            label: Some("tw_query"),
            size: wgpu::Extent3d { width: MAX_POINTS as u32, height: 1, depth_or_array_layers: 1 },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: wgpu::TextureFormat::Rgba32Float,
            usage: wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::COPY_SRC,
            view_formats: &[],
        });
        let view = tex.create_view(&Default::default());
        let empty_layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor { label: Some("tw_query_group0"), entries: &[] });
        let empty = device.create_bind_group(&wgpu::BindGroupDescriptor { label: Some("tw_query_group0"), layout: &empty_layout, entries: &[] });
        let uniform = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("tw_query_points"),
            size: (MAX_POINTS * 16) as u64,
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        let g2_layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("tw_query_group2"),
            entries: &[wgpu::BindGroupLayoutEntry {
                binding: 0,
                visibility: wgpu::ShaderStages::FRAGMENT,
                ty: wgpu::BindingType::Buffer { ty: wgpu::BufferBindingType::Uniform, has_dynamic_offset: false, min_binding_size: None },
                count: None,
            }],
        });
        let group2 = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("tw_query_group2"),
            layout: &g2_layout,
            entries: &[wgpu::BindGroupEntry { binding: 0, resource: uniform.as_entire_binding() }],
        });
        let pl = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("tw_query"),
            bind_group_layouts: &[Some(&empty_layout), Some(group1), Some(&g2_layout)],
            immediate_size: 0,
        });
        let pipeline = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
            label: Some("tw_query"),
            layout: Some(&pl),
            vertex: wgpu::VertexState { module, entry_point: Some("vs_tw_probe"), compilation_options: Default::default(), buffers: &[] },
            primitive: wgpu::PrimitiveState::default(),
            depth_stencil: None,
            multisample: wgpu::MultisampleState::default(),
            fragment: Some(wgpu::FragmentState {
                module,
                entry_point: Some("fs_tw_query"),
                compilation_options: Default::default(),
                targets: &[Some(wgpu::ColorTargetState { format: wgpu::TextureFormat::Rgba32Float, blend: None, write_mask: wgpu::ColorWrites::ALL })],
            }),
            multiview_mask: None,
            cache: None,
        });
        let slots = (0..RING)
            .map(|_| Slot {
                buf: device.create_buffer(&wgpu::BufferDescriptor {
                    label: Some("tw_query_readback"),
                    size: ROW,
                    usage: wgpu::BufferUsages::MAP_READ | wgpu::BufferUsages::COPY_DST,
                    mapped_at_creation: false,
                }),
                state: Rb::Idle,
                points: Vec::new(),
            })
            .collect();
        Self { tex, view, pipeline, empty, uniform, group2, slots, next: 0, current: Vec::new() }
    }

    /// Frame setup: uploads the pending points (w = 1 for a live point, 0 for the unused rest).
    pub fn prepare(&mut self, queue: &wgpu::Queue) {
        self.current = PENDING.lock().map(|p| p.clone()).unwrap_or_default();
        if self.current.is_empty() {
            return;
        }
        let mut data = vec![[0.0f32; 4]; MAX_POINTS];
        for (d, p) in data.iter_mut().zip(&self.current) {
            *d = [p[0], p[1], 0.0, 1.0];
        }
        queue.write_buffer(&self.uniform, 0, bytemuck::cast_slice(&data));
    }

    /// Encoder side, with the probe: the pass and a copy of its results for the CPU.
    pub fn record(&mut self, encoder: &mut wgpu::CommandEncoder, group1: &wgpu::BindGroup) {
        if self.current.is_empty() || !matches!(self.slots[self.next].state, Rb::Idle) {
            return;
        }
        {
            let mut pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
                label: Some("tw_query"),
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
            pass.set_bind_group(2, &self.group2, &[]);
            pass.draw(0..3, 0..1);
        }
        let slot = &mut self.slots[self.next];
        encoder.copy_texture_to_buffer(
            wgpu::TexelCopyTextureInfo { texture: &self.tex, mip_level: 0, origin: wgpu::Origin3d { x: 0, y: 0, z: 0 }, aspect: wgpu::TextureAspect::All },
            wgpu::TexelCopyBufferInfo {
                buffer: &slot.buf,
                layout: wgpu::TexelCopyBufferLayout { offset: 0, bytes_per_row: Some(ROW as u32), rows_per_image: Some(1) },
            },
            wgpu::Extent3d { width: MAX_POINTS as u32, height: 1, depth_or_array_layers: 1 },
        );
        slot.state = Rb::Copied;
        slot.points = self.current.clone();
        self.next = (self.next + 1) % RING;
    }

    /// CPU side, once per frame before recording: request maps, publish the newest completed results.
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
                        let n = slot.points.len();
                        let values: Vec<[f32; 4]> = {
                            let data = slot.buf.slice(..).get_mapped_range();
                            data.chunks_exact(16)
                                .take(n)
                                .map(|c| {
                                    let v = |k: usize| f32::from_le_bytes([c[k], c[k + 1], c[k + 2], c[k + 3]]);
                                    let r = [v(0), v(4), v(8), v(12)];
                                    // a non-finite texel is reported invalid, never passed on
                                    if r.iter().all(|x| x.is_finite()) { r } else { [0.0, 0.0, 0.0, 0.0] }
                                })
                                .collect()
                        };
                        slot.buf.unmap();
                        if let Ok(mut g) = RESULTS.lock() {
                            *g = Some(QueryResults { points: std::mem::take(&mut slot.points), values, at: Instant::now() });
                        }
                        slot.state = Rb::Idle;
                    }
                    _ => slot.state = Rb::Idle,
                },
            }
        }
    }
}
