//! Tidewater's Spray.js — GPU spray particles (drops, ligaments, dense spray, mist) — and the
//! breakers' spray emitters, ported from dgreenheck/tidewater @ 4811ba48 (MIT, © DRG Software
//! Solutions LLC). W4a.
//!
//! One storage ring of `RING` particles (pos: xyz + age, vel: xyz + radius, info: kind, life,
//! water height, tag; OP keeps the three arrays in one buffer and Tidewater's two sprite
//! textures in one, to stay inside the per-stage binding limits the water layout is near). The breaker kernel (`tw_breakers_kernel.wgsl`) emits into it for every
//! crest it finds (`tw_spray_emit.wgsl`: Breakers.js `_emitCode`, rates ~ the energy the breaker
//! releases), a second entry point of the same module integrates the particles (gravity + drag
//! toward the wind, killed when they fall back into the water: `tw_spray_update.wgsl`), and the
//! water module draws them as soft camera-facing sprites after the lips (`tw_spray.wgsl`).
//!
//! What OP changes:
//!
//! * **The CPU tail** (W6i): Tidewater's `cpuCapacity` slots after the GPU ring, spawned by the
//!   update kernel from this frame's emit requests (the boats' bow spray, `boat_spray.rs`); up to
//!   64 requests a frame (Tidewater: 32, one boat); collisions with up to two bodies (the boats'
//!   analytic hulls; no wheelhouse box).
//! * **Budget on the GPU.** Tidewater reads the ring head back a few frames late and steers the
//!   emission scale on the CPU so the ring holds ~2 s of spray; here the update kernel's first
//!   thread does the same step each frame from the head it can read directly.
//! * **Water height** for the particles is sea level + the FFT's and the shore waves' vertical
//!   displacement at the particle (Tidewater: its full water query).
//! * **No foam deposit** into the shore simulation when drops fall back in (yet).
//! * **Light** in Tidewater's units from OP's sun and ambient, as for the lip; OP's fog.

/// Ring size (Tidewater's `gpuCapacity`); a power of two (the ring index wraps with the head).
pub const RING: u32 = 32768;
/// W6i: the CPU-owned tail after the ring (Tidewater's `cpuCapacity`).
pub const TAIL: u32 = 8192;
/// All particle slots (the arrays' stride and the sprite instance count).
pub const SLOTS: u32 = RING + TAIL;
/// CPU emit requests per frame (Tidewater: 32 for its one boat; OP: two boats).
pub const MAX_REQUESTS: usize = 64;
/// Collision bodies (the boats with a simulated wake).
pub const MAX_BODIES: usize = 2;
/// Emission gain (Breakers `spray`), sprite intensity and fade distance (Spray `maxDistance`),
/// camera range of the emitters (Breakers `emitRange`): Tidewater's defaults.
pub const GAIN: f32 = 1.0;
pub const INTENSITY: f32 = 1.0;
pub const MAX_DISTANCE: f32 = 320.0;
pub const EMIT_RANGE: f32 = 260.0;
/// Initial emission budget (Breakers `budget`).
pub const BUDGET0: f32 = 0.3;

/// The per-frame spray block (`SprayParams` in tw_spray_common.wgsl), 336 bytes.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, bytemuck::Pod, bytemuck::Zeroable)]
pub struct SprayParams {
    pub seed: [u32; 4],
    pub a: [f32; 4],
    pub b: [f32; 4],
    pub cam: [f32; 4],
    /// W6i: first tail slot, particles requested, requests, bodies
    pub req: [u32; 4],
    /// W6i: 8 lanes per body (see tw_spray_common.wgsl)
    pub bodies: [[f32; 4]; MAX_BODIES * 8],
}

impl SprayParams {
    /// `on`: the breakers emit; `draw`: the sprites are drawn (breakers or boats).
    #[allow(clippy::too_many_arguments)]
    pub fn new(frame: u32, dt: f32, time: f32, cam: [f32; 3], gain: f32, intensity: f32, on: bool, draw: bool) -> Self {
        Self {
            seed: [frame, SLOTS, RING, 0],
            a: [dt, time, 0.0, gain],
            b: [EMIT_RANGE, intensity, MAX_DISTANCE, if on { 1.0 } else { 0.0 }],
            cam: [cam[0], cam[1], cam[2], if draw { 1.0 } else { 0.0 }],
            req: [0; 4],
            bodies: [[0.0; 4]; MAX_BODIES * 8],
        }
    }
}

/// `WGR_TW_SPRAY_DEBUG=<n>` (OP diagnostics): every live sprite as a disc showing one opacity
/// factor (1 vertex opacity, 2 shape, 3 soft scene fade, 4 water fade; tw_spray.wgsl).
fn sprite_debug() -> u32 {
    static V: std::sync::OnceLock<u32> = std::sync::OnceLock::new();
    *V.get_or_init(|| std::env::var("WGR_TW_SPRAY_DEBUG").ok().and_then(|v| v.trim().parse().ok()).unwrap_or(0))
}

/// W6i: a CPU emit request (Spray.js `emit`): `count` particles spread along a -> b, base
/// velocity `vel` with `spread` (m/s) and `jitter` (m) of randomness, radius `size` (m) +-
/// `size_jitter`, lifetime `life` (s).
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct EmitRequest {
    pub a: [f32; 3],
    pub b: [f32; 3],
    pub vel: [f32; 3],
    pub count: u32,
    pub size: f32,
    pub kind: f32,
    pub spread: f32,
    pub jitter: f32,
    pub life: f32,
    pub size_jitter: f32,
}

/// W6i: a collision body (the boats' hulls, Spray.js setBody / setBodyShape) as its 8 lanes.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Body {
    pub origin: [f32; 3],
    pub x: [f32; 3],
    pub y: [f32; 3],
    pub z: [f32; 3],
    pub vel: [f32; 3],
    /// z aft, z shoulder, z stem, half beam (at the sheer)
    pub hull: [f32; 4],
    /// z shoulder, z stem, half beam (at the waterline)
    pub hull_wl: [f32; 3],
    /// y sheer aft, y sheer stem, y bottom
    pub sheer: [f32; 3],
}

impl Body {
    fn lanes(&self) -> [[f32; 4]; 8] {
        let v = |a: [f32; 3], w: f32| [a[0], a[1], a[2], w];
        [
            v(self.origin, 1.0),
            v(self.x, 0.0),
            v(self.y, 0.0),
            v(self.z, 0.0),
            v(self.vel, 0.0),
            self.hull,
            v(self.hull_wl, 0.0),
            v(self.sheer, 0.0),
        ]
    }
}

/// The head buffer's initial contents: head, head at the last budget step, rate, budget, live
/// particles (even / odd frames), frames updated, unused.
pub fn head_init() -> [u32; 8] {
    [0, 0, 0f32.to_bits(), BUDGET0.to_bits(), 0, 0, 0, 0]
}

/// What the spray diagnostics read back (every ~5 s): particles emitted since the start, the
/// emission rate (particles / s) and budget, live particles, update frames.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct SprayStats {
    pub emitted: u32,
    pub rate: f32,
    pub budget: f32,
    pub live: u32,
    pub frames: u32,
}

impl SprayStats {
    pub fn from_head(h: &[u32; 8]) -> Self {
        Self { emitted: h[0], rate: f32::from_bits(h[2]), budget: f32::from_bits(h[3]), live: h[4].max(h[5]), frames: h[6] }
    }
}

enum Readback {
    Idle,
    Copied,
    Mapping(std::sync::Arc<std::sync::atomic::AtomicU8>),
}

fn mip_chain(data: Vec<u8>, size: u32) -> Vec<(u32, Vec<u8>)> {
    // box-filtered mips of a tileable rgba8 image (the drops average out through them)
    let mut out = vec![(size, data)];
    loop {
        let (s, prev) = out.last().unwrap();
        let s = *s;
        if s == 1 {
            break;
        }
        let h = s / 2;
        let mut next = vec![0u8; (h * h * 4) as usize];
        for y in 0..h {
            for x in 0..h {
                for c in 0..4 {
                    let at = |xx: u32, yy: u32| prev[((yy * s + xx) * 4 + c) as usize] as u32;
                    let v = at(2 * x, 2 * y) + at(2 * x + 1, 2 * y) + at(2 * x, 2 * y + 1) + at(2 * x + 1, 2 * y + 1);
                    next[((y * h + x) * 4 + c) as usize] = ((v + 2) / 4) as u8;
                }
            }
        }
        out.push((h, next));
    }
    out
}

/// Small tileable value-noise texture for the sheets and mist (Spray.js makePuffTexture).
pub fn puff_texture(size: u32) -> Vec<u8> {
    let rnd = |i: i64, j: i64, o: i64| -> f64 {
        let s = ((i.rem_euclid(o)) as f64 * 127.1 + (j.rem_euclid(o)) as f64 * 311.7 + o as f64 * 17.3).sin() * 43758.5453;
        s - s.floor()
    };
    let vnoise = |x: f64, y: f64, o: i64| -> f64 {
        let (i, j) = (x.floor() as i64, y.floor() as i64);
        let (fx, fy) = (x - i as f64, y - j as f64);
        let (ux, uy) = (fx * fx * (3.0 - 2.0 * fx), fy * fy * (3.0 - 2.0 * fy));
        let (a, b, c, d) = (rnd(i, j, o), rnd(i + 1, j, o), rnd(i, j + 1, o), rnd(i + 1, j + 1, o));
        (a * (1.0 - ux) + b * ux) * (1.0 - uy) + (c * (1.0 - ux) + d * ux) * uy
    };
    let mut data = vec![0u8; (size * size * 4) as usize];
    for j in 0..size {
        for i in 0..size {
            let (mut s, mut a, mut n) = (0.0, 0.5, 0.0);
            let mut o = 4i64;
            while o <= 32 {
                s += vnoise(i as f64 / size as f64 * o as f64, j as f64 / size as f64 * o as f64, o) * a;
                n += a;
                a *= 0.55;
                o *= 2;
            }
            let v = ((s / n - 0.2) * 1.6).clamp(0.0, 1.0);
            let k = ((j * size + i) * 4) as usize;
            let b = (v * 255.0).round() as u8;
            data[k] = b;
            data[k + 1] = b;
            data[k + 2] = b;
            data[k + 3] = 255;
        }
    }
    data
}

/// Tileable texture of scattered drops, heavy-tailed radii (Spray.js makeDotsTexture).
pub fn dots_texture(size: u32, count: u32) -> Vec<u8> {
    let mut seed: u32 = 12345;
    let mut rnd = || {
        seed = seed.wrapping_mul(1664525).wrapping_add(1013904223);
        seed as f64 / 4294967296.0
    };
    let n = size as i64;
    let mut cov = vec![0f64; (size * size) as usize];
    for _ in 0..count {
        let (cx, cy) = (rnd() * size as f64, rnd() * size as f64);
        let r = (0.9 * (1.0 - rnd() * 0.97).powf(-0.55)).min(6.0);
        let rr = (r + 1.5).ceil() as i64;
        for dy in -rr..=rr {
            for dx in -rr..=rr {
                let (fx, fy) = (cx.floor() as i64 + dx, cy.floor() as i64 + dy);
                let (x, y) = (fx.rem_euclid(n), fy.rem_euclid(n));
                let d = ((fx as f64 + 0.5 - cx).powi(2) + (fy as f64 + 0.5 - cy).powi(2)).sqrt();
                let c = (r + 0.5 - d).clamp(0.0, 1.0);
                let k = (y * n + x) as usize;
                cov[k] = cov[k].max(c);
            }
        }
    }
    let mut data = vec![0u8; (size * size * 4) as usize];
    for (i, c) in cov.iter().enumerate() {
        let v = (c * 255.0).round() as u8;
        data[i * 4] = v;
        data[i * 4 + 1] = v;
        data[i * 4 + 2] = v;
        data[i * 4 + 3] = 255;
    }
    data
}

/// The upscalers' history-control ("reactive") mask target (temporal.rs: R8Unorm, render size),
/// max-blended so the spray and lips only ever raise the engine's own sky / water values (W4b).
pub fn mask_target() -> wgpu::ColorTargetState {
    let max = wgpu::BlendComponent { src_factor: wgpu::BlendFactor::One, dst_factor: wgpu::BlendFactor::One, operation: wgpu::BlendOperation::Max };
    wgpu::ColorTargetState {
        format: wgpu::TextureFormat::R8Unorm,
        blend: Some(wgpu::BlendState { color: max, alpha: max }),
        write_mask: wgpu::ColorWrites::RED,
    }
}

/// Size of the combined sprite texture.
pub const TEX_SIZE: u32 = 128;

/// OP: Tidewater's two sprite textures in one rgba8 image (r: the puff noise, generated at 128
/// instead of 64 — the same tileable function, sampled finer; g: the drops).
pub fn sprite_texture() -> Vec<u8> {
    let puff = puff_texture(TEX_SIZE);
    let mut out = dots_texture(TEX_SIZE, 150);
    for (o, p) in out.chunks_mut(4).zip(puff.chunks(4)) {
        o[1] = o[0];
        o[0] = p[0];
        o[2] = 0;
    }
    out
}

fn upload_mipped(device: &wgpu::Device, queue: &wgpu::Queue, label: &str, data: Vec<u8>, size: u32) -> wgpu::TextureView {
    let mips = mip_chain(data, size);
    let tex = device.create_texture(&wgpu::TextureDescriptor {
        label: Some(label),
        size: wgpu::Extent3d { width: size, height: size, depth_or_array_layers: 1 },
        mip_level_count: mips.len() as u32,
        sample_count: 1,
        dimension: wgpu::TextureDimension::D2,
        format: wgpu::TextureFormat::Rgba8Unorm,
        usage: wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::COPY_DST,
        view_formats: &[],
    });
    for (level, (s, bytes)) in mips.iter().enumerate() {
        queue.write_texture(
            wgpu::TexelCopyTextureInfo { texture: &tex, mip_level: level as u32, origin: wgpu::Origin3d::ZERO, aspect: wgpu::TextureAspect::All },
            bytes,
            wgpu::TexelCopyBufferLayout { offset: 0, bytes_per_row: Some(s * 4), rows_per_image: Some(*s) },
            wgpu::Extent3d { width: *s, height: *s, depth_or_array_layers: 1 },
        );
    }
    tex.create_view(&wgpu::TextureViewDescriptor::default())
}

/// The ring, the spray block, the sprite textures and the sprite pipeline. The emitters and the
/// update kernel live in the breaker kernel's module (breakers.rs binds these buffers).
pub struct SprayGpu {
    /// pos | vel | info, `SLOTS` vec4 each
    pub ring: wgpu::Buffer,
    /// W6i: this frame's CPU emit requests, 4 vec4 each
    pub requests: wgpu::Buffer,
    /// W6i: the next free tail slot (Spray.js cpuHead)
    cpu_head: u32,
    /// W6i: boat spray particles emitted in the last few seconds (keeps the update and sprites on)
    boat_live: f32,
    pub head: wgpu::Buffer,
    pub params: wgpu::Buffer,
    /// r: puff noise, g: drops
    pub tex_view: wgpu::TextureView,
    pipeline: wgpu::RenderPipeline,
    /// the sprites' coverage into the reactive mask (W4b)
    mask_pipeline: wgpu::RenderPipeline,
    frame: u32,
    /// spray on this frame (the update and the sprites: breakers or boats)
    pub on: bool,
    /// the breakers emit this frame
    pub breakers_on: bool,
    readback: wgpu::Buffer,
    rb_state: Readback,
    /// the latest diagnostics read back (None until the first one arrives)
    pub stats: Option<SprayStats>,
}

impl SprayGpu {
    #[allow(clippy::too_many_arguments)]
    pub fn new(
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        camera_layout: &wgpu::BindGroupLayout,
        render_layout: &wgpu::BindGroupLayout,
        shader: &wgpu::ShaderModule,
        surface_format: wgpu::TextureFormat,
        sample_count: u32,
        linear: f64,
    ) -> Self {
        use wgpu::util::DeviceExt;
        // pos | vel | info, SLOTS vec4 each; starts zeroed: every slot dead (life 0)
        let ring = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("tw_spray_ring"),
            size: SLOTS as u64 * 3 * 16,
            usage: wgpu::BufferUsages::STORAGE,
            mapped_at_creation: false,
        });
        let requests = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("tw_spray_requests"),
            size: (MAX_REQUESTS * 64) as u64,
            usage: wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        let head = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
            label: Some("tw_spray_head"),
            contents: bytemuck::cast_slice(&head_init()),
            usage: wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_SRC,
        });
        let readback = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("tw_spray_head_readback"),
            size: 32,
            usage: wgpu::BufferUsages::MAP_READ | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        let params = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
            label: Some("tw_spray_params"),
            contents: bytemuck::bytes_of(&SprayParams::new(0, 0.0, 0.0, [0.0; 3], GAIN, INTENSITY, false, false)),
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
        });
        let tex_view = upload_mipped(device, queue, "tw_spray_tex", sprite_texture(), TEX_SIZE);

        let layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("tw_spray"),
            bind_group_layouts: &[Some(camera_layout), Some(render_layout)],
            immediate_size: 0,
        });
        let pipeline = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
            label: Some("tw_spray"),
            layout: Some(&layout),
            vertex: wgpu::VertexState {
                module: shader,
                entry_point: Some("vs_spray"),
                compilation_options: wgpu::PipelineCompilationOptions { constants: &[("linear", linear)], ..Default::default() },
                buffers: &[],
            },
            // side: 'double'
            primitive: wgpu::PrimitiveState { topology: wgpu::PrimitiveTopology::TriangleList, cull_mode: None, ..Default::default() },
            // transparent, depthWrite false, depthTest true
            depth_stencil: Some(wgpu::DepthStencilState {
                format: crate::gfx3d::depth_format(device),
                depth_write_enabled: Some(false),
                depth_compare: Some(wgpu::CompareFunction::GreaterEqual),
                stencil: wgpu::StencilState::default(),
                bias: wgpu::DepthBiasState::default(),
            }),
            multisample: wgpu::MultisampleState { count: sample_count, ..Default::default() },
            fragment: Some(wgpu::FragmentState {
                module: shader,
                entry_point: Some("fs_spray"),
                compilation_options: wgpu::PipelineCompilationOptions { constants: &[("linear", linear)], ..Default::default() },
                // blending: 'premultiplied'
                targets: &[Some(wgpu::ColorTargetState {
                    format: surface_format,
                    blend: Some(wgpu::BlendState::PREMULTIPLIED_ALPHA_BLENDING),
                    write_mask: wgpu::ColorWrites::ALL,
                })],
            }),
            multiview_mask: None,
            cache: None,
        });
        let mask_pipeline = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
            label: Some("tw_spray_mask"),
            layout: Some(&layout),
            vertex: wgpu::VertexState {
                module: shader,
                entry_point: Some("vs_spray"),
                compilation_options: wgpu::PipelineCompilationOptions { constants: &[("linear", linear)], ..Default::default() },
                buffers: &[],
            },
            primitive: wgpu::PrimitiveState { topology: wgpu::PrimitiveTopology::TriangleList, cull_mode: None, ..Default::default() },
            depth_stencil: None,
            multisample: wgpu::MultisampleState::default(),
            fragment: Some(wgpu::FragmentState {
                module: shader,
                entry_point: Some("fs_spray_mask"),
                compilation_options: wgpu::PipelineCompilationOptions { constants: &[("linear", linear)], ..Default::default() },
                targets: &[Some(mask_target())],
            }),
            multiview_mask: None,
            cache: None,
        });
        Self {
            ring,
            requests,
            cpu_head: 0,
            boat_live: 0.0,
            head,
            params,
            tex_view,
            pipeline,
            mask_pipeline,
            frame: 0,
            on: false,
            breakers_on: false,
            readback,
            rb_state: Readback::Idle,
            stats: None,
        }
    }

    /// The spray block for this frame (Spray.update: the frame seed advances every frame).
    /// `on`: the breakers emit. W6i: `requests` (the boats' bow spray, spawned into the tail by
    /// this frame's update) and `bodies` (the hulls the particles collide with); the update and
    /// the sprites run while either the breakers or recent boat spray are live.
    #[allow(clippy::too_many_arguments)]
    pub fn set_frame(
        &mut self,
        queue: &wgpu::Queue,
        dt: f32,
        time: f32,
        cam: [f32; 3],
        gain: f32,
        intensity: f32,
        on: bool,
        requests: &[EmitRequest],
        bodies: &[Body],
    ) {
        self.frame = self.frame.wrapping_add(1);
        // requests -> tail slots (Spray.js update: prefix ends, cpuStart / cpuCount)
        let mut data = [[0.0f32; 4]; MAX_REQUESTS * 4];
        let mut n = 0u32;
        let mut nreq = 0usize;
        for r in requests.iter().take(MAX_REQUESTS) {
            let count = r.count.min(TAIL - n);
            if count == 0 {
                continue;
            }
            n += count;
            let o = nreq * 4;
            data[o] = [r.a[0], r.a[1], r.a[2], n as f32];
            data[o + 1] = [r.b[0], r.b[1], r.b[2], r.size];
            data[o + 2] = [r.vel[0], r.vel[1], r.vel[2], r.kind];
            data[o + 3] = [r.spread, r.jitter, r.life, r.size_jitter];
            nreq += 1;
        }
        if nreq > 0 {
            queue.write_buffer(&self.requests, 0, bytemuck::cast_slice(&data[..nreq * 4]));
            self.boat_live = 3.0;
        } else {
            self.boat_live = (self.boat_live - dt.max(0.0)).max(0.0);
        }
        self.breakers_on = on;
        self.on = on || self.boat_live > 0.0;
        let mut p = SprayParams::new(self.frame, dt, time, cam, gain.clamp(0.0, 3.0), intensity.clamp(0.0, 50.0), on, self.on);
        p.seed[3] = sprite_debug();
        p.req = [self.cpu_head, n, nreq as u32, bodies.len().min(MAX_BODIES) as u32];
        for (i, b) in bodies.iter().take(MAX_BODIES).enumerate() {
            p.bodies[i * 8..i * 8 + 8].copy_from_slice(&b.lanes());
        }
        self.cpu_head = (self.cpu_head + n) % TAIL;
        queue.write_buffer(&self.params, 0, bytemuck::bytes_of(&p));
    }

    /// Diagnostics, encoder side: every 300 frames copy the head block for readback.
    pub fn copy_stats(&mut self, encoder: &mut wgpu::CommandEncoder) {
        if matches!(self.rb_state, Readback::Idle) && self.on && self.frame % 300 == 0 {
            encoder.copy_buffer_to_buffer(&self.head, 0, &self.readback, 0, 32);
            self.rb_state = Readback::Copied;
        }
    }

    /// Diagnostics, next frame (the copy has been submitted): map it, and read it once mapped
    /// (the renderer's per-frame device poll completes the map).
    pub fn poll_stats(&mut self) {
        use std::sync::atomic::{AtomicU8, Ordering};
        match &self.rb_state {
            Readback::Idle => {}
            Readback::Copied => {
                let flag = std::sync::Arc::new(AtomicU8::new(0));
                let f = flag.clone();
                self.readback.slice(..).map_async(wgpu::MapMode::Read, move |r| f.store(if r.is_ok() { 1 } else { 2 }, Ordering::Release));
                self.rb_state = Readback::Mapping(flag);
            }
            Readback::Mapping(flag) => match flag.load(Ordering::Acquire) {
                0 => {}
                1 => {
                    let mut h = [0u32; 8];
                    h.copy_from_slice(bytemuck::cast_slice(&self.readback.slice(..).get_mapped_range()[..32]));
                    self.readback.unmap();
                    self.stats = Some(SprayStats::from_head(&h));
                    self.rb_state = Readback::Idle;
                }
                _ => self.rb_state = Readback::Idle,
            },
        }
    }

    /// The sprites' coverage into the reactive mask (W4b).
    pub fn draw_mask(&self, pass: &mut wgpu::RenderPass<'_>, camera: &wgpu::BindGroup, offset: u32, render_bind: &wgpu::BindGroup) {
        if !self.on {
            return;
        }
        pass.set_pipeline(&self.mask_pipeline);
        pass.set_bind_group(0, camera, &[offset]);
        pass.set_bind_group(1, render_bind, &[]);
        pass.draw(0..6, 0..SLOTS);
    }

    /// The sprites (after the lips, same pass).
    pub fn draw(&self, pass: &mut wgpu::RenderPass<'_>, camera: &wgpu::BindGroup, offset: u32, render_bind: &wgpu::BindGroup) {
        if !self.on {
            return;
        }
        pass.set_pipeline(&self.pipeline);
        pass.set_bind_group(0, camera, &[offset]);
        pass.set_bind_group(1, render_bind, &[]);
        pass.draw(0..6, 0..SLOTS);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn params_block_matches_the_wgsl_layout() {
        assert_eq!(std::mem::size_of::<SprayParams>(), 64 + 16 + 16 * MAX_BODIES * 8);
        assert_eq!(std::mem::offset_of!(SprayParams, req), 64);
        assert_eq!(std::mem::offset_of!(SprayParams, bodies), 80);
        let p = SprayParams::new(7, 0.016, 3.0, [1.0, 2.0, 3.0], GAIN, INTENSITY, true, true);
        assert_eq!(p.seed, [7, SLOTS, RING, 0]);
        assert_eq!(p.b[3], 1.0);
        assert_eq!(p.cam[3], 1.0);
        assert!(RING.is_power_of_two());
        assert_eq!(f32::from_bits(head_init()[3]), BUDGET0);
        let s = SprayStats::from_head(&[100, 0, 5.0f32.to_bits(), 0.5f32.to_bits(), 7, 9, 3, 0]);
        assert_eq!((s.emitted, s.rate, s.budget, s.live, s.frames), (100, 5.0, 0.5, 9, 3));
    }

    #[test]
    fn textures_match_tidewaters_generators() {
        let puff = puff_texture(64);
        assert_eq!(puff.len(), 64 * 64 * 4);
        let mean = puff.chunks(4).map(|p| p[0] as f64).sum::<f64>() / (64.0 * 64.0);
        assert!(mean > 40.0 && mean < 220.0, "{mean}");
        // tileable: the value noise wraps with its octave period
        let dots = dots_texture(128, 150);
        let covered = dots.chunks(4).filter(|p| p[0] > 127).count();
        assert!(covered > 100 && covered < 128 * 128 / 2, "{covered}");
        let mips = mip_chain(dots, 128);
        assert_eq!(mips.len(), 8);
        assert_eq!(mips.last().unwrap().0, 1);
        let t = sprite_texture();
        assert_eq!(t.len(), (TEX_SIZE * TEX_SIZE * 4) as usize);
        assert_eq!(t[1], dots_texture(TEX_SIZE, 150)[0]);
    }
}
