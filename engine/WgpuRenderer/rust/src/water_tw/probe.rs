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
    /// the camera's x, z when it was copied
    cam: [f32; 2],
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
    /// the camera's x, z for the next recorded grid ([`Self::set_camera`])
    cam: [f32; 2],
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
                cam: [0.0; 2],
            })
            .collect();
        Self { view, tex, pipeline, empty, slots, next: 0, height: None, age: u32::MAX, ready: false, cam: [0.0; 2] }
    }

    /// The `tw.probe` lanes for this frame's camera: the grid's first lattice point (W12d: the
    /// finest CDLOD lattice, `STEP`-aligned, the camera between texels N/2 and N/2 + 1), step, size.
    /// W12d: the texels hold the displaced mesh vertices (rgba32float), which the composite
    /// interpolates as the mesh's own triangles.
    pub fn lanes(cam: [f32; 3]) -> [f32; 4] {
        let base = |c: f32| ((c as f64 / STEP as f64).floor() - (N / 2) as f64) * STEP as f64;
        [base(cam[0]) as f32, base(cam[2]) as f32, STEP, N as f32]
    }

    /// The camera the next recorded grid is centred on (its x, z), for the height read back under it.
    pub fn set_camera(&mut self, cam: [f32; 3]) {
        self.cam = [cam[0], cam[2]];
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
            slot.cam = self.cam;
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
                            let mut xyz = Vec::with_capacity((N * N) as usize);
                            for row in 0..N as usize {
                                for col in 0..N as usize {
                                    xyz.push([lane(row, col, 0), lane(row, col, 1), lane(row, col, 2)]);
                                }
                            }
                            // the surface drawn OVER the camera, not the vertex that started under it:
                            // in a choppy sea the vertices move metres sideways, and the centre texels'
                            // height (used until 2026-10-13) could put a camera half a metre under a
                            // crest above the water, so the gate switched the underwater look off
                            // (owner: undistorted clouds and clear water just under the surface)
                            let c = N as usize / 2;
                            let centre = (lane(c, c, 1) + lane(c, c + 1, 1) + lane(c + 1, c, 1) + lane(c + 1, c + 1, 1)) * 0.25;
                            (surface_height(&xyz, slot.lanes, slot.cam).unwrap_or(centre), xyz)
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

/// The drawn surface's height at `xz` from a grid of displaced vertices (`xyz`, row-major, row = z),
/// as `uwProbeHeight` in tw_underwater.wgsl finds it: the lattice point displaced to `xz` (two
/// fixed-point steps), then the mesh triangle over `xz` among that cell and its 8 neighbours, CDLOD's
/// diagonal parity (cdlod.rs grid_mesh). None outside the grid; a folded patch averages its corners.
pub fn surface_height(xyz: &[[f32; 3]], lanes: [f32; 4], xz: [f32; 2]) -> Option<f32> {
    let nf = lanes[3];
    let n = nf as i64;
    if nf < 2.0 || xyz.len() < (n * n) as usize {
        return None;
    }
    let (h, base) = (lanes[2], [lanes[0], lanes[1]]);
    let v = |i: i64, j: i64| xyz[(j * n + i) as usize];
    // Newton on p + D(p) = xz with the cell's bilinear displacement D (exact for that interpolant).
    // The plain fixed-point step p = xz - D(p) used before contracts by only 1 - k Q per step: on a
    // choppy crest (k Q ~ 0.9) two steps left the point metres off, outside the 3 x 3 cells searched.
    let mut p = xz;
    for _ in 0..6 {
        let f = [((p[0] - base[0]) / h).clamp(0.0, nf - 1.001), ((p[1] - base[1]) / h).clamp(0.0, nf - 1.001)];
        let (i, j) = (f[0].floor() as i64, f[1].floor() as i64);
        let (tx, tz) = (f[0] - i as f32, f[1] - j as f32);
        let d = |a: i64, b: i64| {
            let q = v(a, b);
            [q[0] - (base[0] + a as f32 * h), q[2] - (base[1] + b as f32 * h)]
        };
        let (d00, d10, d01, d11) = (d(i, j), d(i + 1, j), d(i, j + 1), d(i + 1, j + 1));
        let mut m = [0.0f32; 2];
        let mut jac = [[1.0f32, 0.0], [0.0, 1.0]]; // I + dD/dp
        for k in 0..2 {
            let a = d00[k] + (d10[k] - d00[k]) * tx;
            let b = d01[k] + (d11[k] - d01[k]) * tx;
            m[k] = a + (b - a) * tz;
            jac[k][0] += ((d10[k] - d00[k]) * (1.0 - tz) + (d11[k] - d01[k]) * tz) / h;
            jac[k][1] += (b - a) / h;
        }
        let r = [p[0] + m[0] - xz[0], p[1] + m[1] - xz[1]];
        let det = jac[0][0] * jac[1][1] - jac[0][1] * jac[1][0];
        let mut step = if det > 0.05 {
            [(jac[1][1] * r[0] - jac[0][1] * r[1]) / det, (jac[0][0] * r[1] - jac[1][0] * r[0]) / det]
        } else {
            r // folded here: the fixed-point step
        };
        let len = (step[0] * step[0] + step[1] * step[1]).sqrt();
        if len > 1.0 {
            step = [step[0] / len, step[1] / len];
        }
        p = [p[0] - step[0], p[1] - step[1]];
    }
    let f = [(p[0] - base[0]) / h, (p[1] - base[1]) / h];
    if f.iter().any(|&x| x < 1.0 || x > nf - 2.0) {
        return None;
    }
    let tri = |a: [f32; 3], b: [f32; 3], c: [f32; 3]| -> Option<f32> {
        let v0 = [b[0] - a[0], b[2] - a[2]];
        let v1 = [c[0] - a[0], c[2] - a[2]];
        let v2 = [xz[0] - a[0], xz[1] - a[2]];
        let den = v0[0] * v1[1] - v1[0] * v0[1];
        if den.abs() < 1e-10 {
            return None;
        }
        let u = (v2[0] * v1[1] - v1[0] * v2[1]) / den;
        let w = (v0[0] * v2[1] - v2[0] * v0[1]) / den;
        if u < -1e-4 || w < -1e-4 || u + w > 1.0 + 1e-4 {
            return None;
        }
        Some(a[1] + (b[1] - a[1]) * u + (c[1] - a[1]) * w)
    };
    let c0 = (f[0].floor() as i64, f[1].floor() as i64);
    let g0 = ((base[0] / h).round() as i64, (base[1] / h).round() as i64);
    for k in 0..9i64 {
        let (ci, cj) = (c0.0 + k % 3 - 1, c0.1 + k / 3 - 1);
        if ci < 0 || cj < 0 || ci > n - 2 || cj > n - 2 {
            continue;
        }
        let (a, b, cc, d) = (v(ci, cj), v(ci + 1, cj), v(ci, cj + 1), v(ci + 1, cj + 1));
        let even = (g0.0 + ci + g0.1 + cj).rem_euclid(2) == 0;
        let r = if even { tri(a, cc, b).or_else(|| tri(b, cc, d)) } else { tri(a, cc, d).or_else(|| tri(a, d, b)) };
        if r.is_some() {
            return r;
        }
    }
    let (ci, cj) = (c0.0.clamp(0, n - 2), c0.1.clamp(0, n - 2));
    Some((v(ci, cj)[1] + v(ci + 1, cj)[1] + v(ci, cj + 1)[1] + v(ci + 1, cj + 1)[1]) * 0.25)
}

#[cfg(test)]
mod tests {
    use super::*;

    // a trochoidal (Gerstner) wave on the probe lattice: vertex (x0, z0) moves to
    // (x0 - Q sin(k x0), A cos(k x0), z0) -- crests sharpened and moved sideways, like a choppy FFT
    fn gerstner_grid(lanes: [f32; 4], amp: f32, k: f32, q: f32) -> Vec<[f32; 3]> {
        let n = lanes[3] as usize;
        let mut xyz = Vec::new();
        for j in 0..n {
            for i in 0..n {
                let (x0, z0) = (lanes[0] + i as f32 * lanes[2], lanes[1] + j as f32 * lanes[2]);
                xyz.push([x0 - q * (k * x0).sin(), amp * (k * x0).cos(), z0]);
            }
        }
        xyz
    }

    #[test]
    fn height_is_the_surface_over_the_camera_not_the_vertex_under_it() {
        // a steep choppy wave: 1.5 m amplitude, 8 m long, 1.2 m of sideways motion
        let (amp, k, q) = (1.5f32, std::f32::consts::TAU / 8.0, 1.2f32);
        let mut worst_old = 0.0f32;
        for s in 0..40 {
            let cam = [3.0 + s as f32 * 0.21, 0.0, 5.0];
            let lanes = Probe::lanes(cam);
            let xyz = gerstner_grid(lanes, amp, k, q);
            // the true surface over cam.x: solve x0 - q sin(k x0) = x (monotonic for k q < 1)
            let (mut lo, mut hi) = (cam[0] - 2.0, cam[0] + 2.0);
            for _ in 0..60 {
                let mid = 0.5 * (lo + hi);
                if mid - q * (k * mid).sin() < cam[0] {
                    lo = mid
                } else {
                    hi = mid
                }
            }
            let truth = amp * (k * lo).cos();
            let h = surface_height(&xyz, lanes, [cam[0], cam[2]]).expect("inside the grid");
            assert!((h - truth).abs() < 0.08, "cam x {}: {h} vs {truth}", cam[0]);
            let (c, n) = (N as usize / 2, N as usize);
            let old = (xyz[c * n + c][1] + xyz[c * n + c + 1][1] + xyz[(c + 1) * n + c][1] + xyz[(c + 1) * n + c + 1][1]) * 0.25;
            worst_old = worst_old.max((old - truth).abs());
        }
        // the centre-texel height it replaces was off by most of a metre on this sea
        assert!(worst_old > 0.5, "old error {worst_old}");
    }

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
