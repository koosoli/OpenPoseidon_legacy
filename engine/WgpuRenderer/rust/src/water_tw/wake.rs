//! Tidewater's WakeSim.js — the interactive boat wake — ported from dgreenheck/tidewater @ 4811ba48
//! (MIT, © DRG Software Solutions LLC). W6.
//!
//! A linear free-surface wave simulation with exact dispersion on a 512² grid (0.4 m cells, 205 m)
//! in a window that follows one boat (`tw_wake_kernel.wgsl`), read by the water material
//! (`tw_wake.wgsl`: displacement in the vertex stage, slopes / foam / aeration per pixel).
//!
//! What OP changes:
//! * **Several boats.** Up to [`SLOTS`] wakes, one per boat, for the boats nearest the camera
//!   (the producer picks them, WaterWgpu.cpp). Each slot has its own state buffers and a layer of
//!   the shared display / template / aeration textures; the readers sum the slots. Other boats
//!   keep OP's own Kelvin-wedge emitter (the Current OP interaction field), which is not drawn in
//!   this backend, so they have no wake here.
//! * **No hull lines.** OP's boats have no lines plan: the hull immersion map is baked from an
//!   analytic hull of the boat's size (half beam, half length, draft from its bounding box: flat
//!   run aft to a transom, round bilge, a fine entry (W6i.2) and a keel that rises toward the stem), blurred to the grid
//!   scale exactly as `_bakeHull` blurs Tidewater's lines.
//! * **Size-scaled schedules.** The speed schedules (planing trim, hollow, wash width) are keyed to
//!   the boat's Froude number and beam instead of the constants of Tidewater's one 8 m boat.
//! * **No pier piles** (OP has no pile colliders to bake): land from OP's heightmap is the only
//!   obstacle.
//! * **Visual only.** Physics stays on the CPU water query: boats do not feel their own or each
//!   other's wakes (Tidewater's boat reads its wake through the GPU query, minus the near-field
//!   template; OP has no GPU query in the physics loop).
//! * **Paused water clock:** no step while the water clock stands still.

use crate::ffi::WgrWaterParams;

pub const SLOTS: usize = 2;
const N: u32 = 512;
const HALF: u32 = N / 2;
const CELL: f32 = 0.4;
const TW: u32 = 32;
const TH: u32 = 88;
const NEAR_GROUPS: u32 = (TW * TH).div_ceil(HALF);
const HULL_NX: u32 = 24;
const HULL_NZ: u32 = 112;
/// s of calm before a wake goes to sleep (WakeSim `settleTime`)
const SETTLE: f32 = 40.0;
const GRAVITY: f32 = 9.81;
pub const FORMAT: wgpu::TextureFormat = wgpu::TextureFormat::Rgba16Float;

/// `WakeKernel` in tw_wake_kernel.wgsl.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, bytemuck::Pod, bytemuck::Zeroable)]
struct WakeKernel {
    win: [f32; 4],
    boat: [f32; 4],
    trim: [f32; 4],
    m0: [f32; 4],
    m1: [f32; 4],
    m2: [f32; 4],
    m3: [f32; 4],
    hull: [f32; 4],
    near: [f32; 4],
    m4: [f32; 4],
}

/// One boat from the producer (`WgrWaterParams::tidewater_wake`, lanes 0-2 of the boat's 8).
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct WakeBoat {
    pub id: u32,
    pub pos: [f32; 2],
    pub fwd: [f32; 2],
    pub vel: [f32; 2],
    /// 0..1, the engine's thrust
    pub throttle: f32,
    pub driven: bool,
    pub half_beam: f32,
    pub half_len: f32,
    pub draft: f32,
}

impl WakeBoat {
    /// The boats in the params (flags bit 0 = present, bit 1 = driven).
    pub fn from_params(p: &WgrWaterParams) -> [Option<WakeBoat>; SLOTS] {
        let mut out = [None; SLOTS];
        for (i, o) in out.iter_mut().enumerate() {
            let a = p.tidewater_wake[i * 8];
            let b = p.tidewater_wake[i * 8 + 1];
            let c = p.tidewater_wake[i * 8 + 2];
            let flags = c[3] as u32;
            if flags & 1 == 0 {
                continue;
            }
            let fl = (a[2] * a[2] + a[3] * a[3]).sqrt();
            if !(fl > 1e-4) || ![a[0], a[1], b[0], b[1], c[0], c[1], c[2]].iter().all(|v| v.is_finite()) {
                continue;
            }
            let half_len = c[1].clamp(1.0, 45.0);
            *o = Some(WakeBoat {
                id: b[3] as u32,
                pos: [a[0], a[1]],
                fwd: [a[2] / fl, a[3] / fl],
                vel: [b[0], b[1]],
                throttle: b[2].clamp(-1.0, 1.0),
                driven: flags & 2 != 0,
                // OP's bounding boxes are wider than the hull (the PBR's reads 7.4 m for a ~3.5 m
                // beam: crew and gun proxies): no displacement hull is wider than ~0.42 x its length
                half_beam: c[0].clamp(0.4, 12.0).min(0.42 * half_len),
                half_len,
                draft: c[2].clamp(0.15, 5.0),
            });
        }
        out
    }

    fn dims(&self) -> [f32; 3] {
        [self.half_beam, self.half_len, (self.draft * tune().draft).max(0.05)]
    }
}

/// three.js MathUtils.smoothstep(x, min, max)
fn smooth(x: f32, lo: f32, hi: f32) -> f32 {
    if x <= lo {
        return 0.0;
    }
    if x >= hi {
        return 1.0;
    }
    let t = (x - lo) / (hi - lo);
    t * t * (3.0 - 2.0 * t)
}

/// The analytic hull's bottom height (m, negative below the design waterline) at boat-frame
/// (x = |port|, z = forward from amidships), or None outside the waterplane.
fn bottom_at(dims: [f32; 3], x: f32, z: f32) -> Option<f32> {
    let [b, l, d] = dims;
    let u = (z + l) / (2.0 * l); // 0 at the transom, 1 at the stem
    if !(0.0..=1.0).contains(&u) {
        return None;
    }
    // waterplane: parallel aft, a fine entry over the forward 45 %
    // (W6i.2: a fine entry, B (1 - e^1.3), ~30 degrees half-angle at the stem; was elliptic, which
    // made a blunt bow that threw its spray metres high)
    let w = if u < 0.55 { b } else { b * (1.0 - ((u - 0.55) / 0.45).powf(1.3)).max(0.0) };
    let ax = x.abs();
    if ax >= w || w <= 1e-3 {
        return None;
    }
    // keel: full depth aft, rising toward the stem
    let keel = if u < 0.6 { d } else { d * (1.0 - 0.7 * ((u - 0.6) / 0.4).powf(1.5)) };
    // section: round bilge
    Some(-keel * (1.0 - (ax / w).powi(2)).sqrt())
}

/// (X1, Z0, Z1) of the hull map for these dimensions (WakeSim `hullBox`, sized to the boat).
fn hull_box(dims: [f32; 3]) -> (f32, f32, f32) {
    (dims[0] + 0.7, -dims[1] - 0.8, dims[1] + 0.8)
}

/// Hull immersion at the design waterline over the boat frame, blurred to the grid scale so the
/// moving pressure field does not excite grid noise (WakeSim `_bakeHull`). R16Float texels.
fn bake_hull(dims: [f32; 3]) -> Vec<u16> {
    let (x1, z0, z1) = hull_box(dims);
    let r = 0.55f32;
    let mut data = vec![0u16; (HULL_NX * HULL_NZ) as usize];
    for iz in 0..HULL_NZ {
        for ix in 0..HULL_NX {
            let x = (ix as f32 + 0.5) / HULL_NX as f32 * x1;
            let z = z0 + (iz as f32 + 0.5) / HULL_NZ as f32 * (z1 - z0);
            let (mut sum, mut wsum) = (0.0f32, 0.0f32);
            for u in -3..=3 {
                for v in -3..=3 {
                    let dx = u as f32 / 3.0 * r;
                    let dz = v as f32 / 3.0 * r;
                    let wgt = (-(dx * dx + dz * dz) / (r * r * 0.4)).exp();
                    let y = bottom_at(dims, x + dx, z + dz);
                    sum += wgt * y.map_or(0.0, |y| (-y).max(0.0));
                    wsum += wgt;
                }
            }
            let edge = ix == HULL_NX - 1 || iz == 0 || iz == HULL_NZ - 1;
            data[(iz * HULL_NX + ix) as usize] = f32_to_f16(if edge { 0.0 } else { sum / wsum });
        }
    }
    data
}

/// IEEE half from f32 (round to nearest; the hull map is small, finite and non-negative).
fn f32_to_f16(v: f32) -> u16 {
    let bits = v.to_bits();
    let sign = ((bits >> 16) & 0x8000) as u16;
    let exp = ((bits >> 23) & 0xff) as i32 - 127 + 15;
    let man = bits & 0x7f_ffff;
    if exp <= 0 {
        if exp < -10 {
            return sign;
        }
        let m = (man | 0x80_0000) >> (1 - exp);
        return sign | ((m + 0x1000) >> 13) as u16;
    }
    if exp >= 31 {
        return sign | 0x7c00;
    }
    let h = sign as u32 | ((exp as u32) << 10) | (man >> 13);
    // round half up on the dropped bits (carry into the exponent is correct IEEE behaviour)
    (h + ((man >> 12) & 1)) as u16
}

struct Slot {
    boat: Option<u32>,
    last: Option<WakeBoat>,
    dims: [f32; 3],
    ubo: wgpu::Buffer,
    state: wgpu::Buffer,
    scratch: wgpu::Buffer,
    spec: wgpu::Buffer,
    aer_a: wgpu::Buffer,
    aer_b: wgpu::Buffer,
    near_buf: wgpu::Buffer,
    hull: wgpu::Texture,
    hull_view: wgpu::TextureView,
    display_layer: wgpu::TextureView,
    aer_layer: wgpu::TextureView,
    near_layer: wgpu::TextureView,
    binds: Option<[wgpu::BindGroup; 3]>,
    // WakeSim state
    center: [i32; 2],
    origin: [f32; 2],
    has_window: bool,
    sleeping: bool,
    idle: f32,
    reset: bool,
    amount: f32,
    near_box: [f32; 4],
    /// boat position xz, cos / sin yaw (kept after the boat leaves the list)
    pose: Option<[f32; 4]>,
    /// dispatch this frame
    step: bool,
}

pub struct WakeGpu {
    layouts: [wgpu::BindGroupLayout; 3],
    pipelines: [wgpu::ComputePipeline; 3],
    display_view: wgpu::TextureView,
    near_view: wgpu::TextureView,
    aer_view: wgpu::TextureView,
    smp_clamp: wgpu::Sampler,
    smp_repeat: wgpu::Sampler,
    slots: Vec<Slot>,
    /// `WGR_TW_WAKE=0` turns the wakes off (A/B and budget captures)
    enabled: bool,
}

/// `WGR_TW_WAKE_TUNE=foam=0.2,draft=0.7,source=1,wash=0.8,aer=1,amp=1`: look overrides for
/// captures (foam gain, draft scale, pressure gain, wash scale, aeration gain, height scale).
#[derive(Clone, Copy, Debug)]
struct Tune {
    foam: f32,
    draft: f32,
    source: f32,
    wash: f32,
    aer: f32,
    amp: f32,
}

fn tune() -> Tune {
    static V: std::sync::OnceLock<Tune> = std::sync::OnceLock::new();
    *V.get_or_init(|| {
        // OP defaults (W6h.2, from the tuning captures on twwake.eden): Tidewater's foam gain
        // (0.35) and wash saturate the track of a PBR at 13 m/s into one white slab 150 m long,
        // and the box bottom (props, rudder) reads deeper than the hull
        let mut t = Tune { foam: 0.2, draft: 0.7, source: 1.0, wash: 0.8, aer: 1.0, amp: 1.0 };
        if let Ok(v) = std::env::var("WGR_TW_WAKE_TUNE") {
            for kv in v.split(',') {
                let mut it = kv.splitn(2, '=');
                let (Some(k), Some(x)) = (it.next(), it.next().and_then(|x| x.trim().parse::<f32>().ok())) else { continue };
                match k.trim() {
                    "foam" => t.foam = x,
                    "draft" => t.draft = x,
                    "source" => t.source = x,
                    "wash" => t.wash = x,
                    "aer" => t.aer = x,
                    "amp" => t.amp = x,
                    _ => {}
                }
            }
        }
        t
    })
}

fn env_enabled() -> bool {
    static V: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *V.get_or_init(|| std::env::var("WGR_TW_WAKE").map_or(true, |v| v.trim() != "0"))
}

fn layered(device: &wgpu::Device, label: &str, (w, h): (u32, u32)) -> wgpu::Texture {
    device.create_texture(&wgpu::TextureDescriptor {
        label: Some(label),
        size: wgpu::Extent3d { width: w, height: h, depth_or_array_layers: SLOTS as u32 },
        mip_level_count: 1,
        sample_count: 1,
        dimension: wgpu::TextureDimension::D2,
        format: FORMAT,
        usage: wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::STORAGE_BINDING,
        view_formats: &[],
    })
}

fn layer_view(t: &wgpu::Texture, layer: u32) -> wgpu::TextureView {
    t.create_view(&wgpu::TextureViewDescriptor {
        dimension: Some(wgpu::TextureViewDimension::D2),
        base_array_layer: layer,
        array_layer_count: Some(1),
        ..Default::default()
    })
}

fn array_view(t: &wgpu::Texture) -> wgpu::TextureView {
    t.create_view(&wgpu::TextureViewDescriptor { dimension: Some(wgpu::TextureViewDimension::D2Array), ..Default::default() })
}

fn storage_buf(device: &wgpu::Device, label: &str, size: u64) -> wgpu::Buffer {
    device.create_buffer(&wgpu::BufferDescriptor {
        label: Some(label),
        size,
        usage: wgpu::BufferUsages::STORAGE,
        mapped_at_creation: false,
    })
}

impl WakeGpu {
    /// `shader_src`: tw_common.wgsl + tw_wake_kernel.wgsl (mod.rs).
    pub fn new(device: &wgpu::Device, shader_src: &str) -> Self {
        use wgpu::TextureSampleType as S;
        use wgpu::TextureViewDimension as D;
        let c = wgpu::ShaderStages::COMPUTE;
        let uniform = |binding| wgpu::BindGroupLayoutEntry {
            binding,
            visibility: c,
            ty: wgpu::BindingType::Buffer { ty: wgpu::BufferBindingType::Uniform, has_dynamic_offset: false, min_binding_size: None },
            count: None,
        };
        let storage = |binding| wgpu::BindGroupLayoutEntry {
            binding,
            visibility: c,
            ty: wgpu::BindingType::Buffer {
                ty: wgpu::BufferBindingType::Storage { read_only: false },
                has_dynamic_offset: false,
                min_binding_size: None,
            },
            count: None,
        };
        let tex = |binding, filterable| wgpu::BindGroupLayoutEntry {
            binding,
            visibility: c,
            ty: wgpu::BindingType::Texture { sample_type: S::Float { filterable }, view_dimension: D::D2, multisampled: false },
            count: None,
        };
        let smp = |binding| wgpu::BindGroupLayoutEntry {
            binding,
            visibility: c,
            ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Filtering),
            count: None,
        };
        let store = |binding| wgpu::BindGroupLayoutEntry {
            binding,
            visibility: c,
            ty: wgpu::BindingType::StorageTexture { access: wgpu::StorageTextureAccess::WriteOnly, format: FORMAT, view_dimension: D::D2 },
            count: None,
        };
        let layout = |label, entries: &[wgpu::BindGroupLayoutEntry]| {
            device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor { label: Some(label), entries })
        };
        // the bindings each entry point uses (tw_wake_kernel.wgsl)
        let rows = layout(
            "tw_wake_rows",
            &[
                uniform(0), storage(1), storage(2), storage(3), storage(4), storage(5), tex(6, true), smp(7), store(8), store(9),
                tex(14, false), uniform(15),
            ],
        );
        let cols = layout("tw_wake_cols", &[uniform(0), storage(3), tex(6, true), smp(7), tex(10, true), smp(11), storage(12), store(13)]);
        let inv = layout(
            "tw_wake_inverse",
            &[uniform(0), storage(1), storage(2), storage(3), storage(4), storage(5), tex(14, false), uniform(15)],
        );
        let module = device.create_shader_module(wgpu::ShaderModuleDescriptor {
            label: Some("tw_wake_kernel"),
            source: wgpu::ShaderSource::Wgsl(shader_src.into()),
        });
        let pipe = |label: &str, layout: &wgpu::BindGroupLayout, entry: &str| {
            let pl = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
                label: Some(label),
                bind_group_layouts: &[Some(layout)],
                immediate_size: 0,
            });
            device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
                label: Some(label),
                layout: Some(&pl),
                module: &module,
                entry_point: Some(entry),
                compilation_options: Default::default(),
                cache: None,
            })
        };
        let pipelines = [pipe("tw_wake_rows", &rows, "wake_rows"), pipe("tw_wake_cols", &cols, "wake_columns"), pipe("tw_wake_inverse", &inv, "wake_inverse")];

        let display = layered(device, "tw_wake_display", (N, N));
        let aer = layered(device, "tw_wake_aeration", (N, N));
        let near = layered(device, "tw_wake_near", (TW, TH));
        let cells = (N * N) as u64;
        let slots = (0..SLOTS as u32)
            .map(|i| {
                let hull = device.create_texture(&wgpu::TextureDescriptor {
                    label: Some("tw_wake_hull"),
                    size: wgpu::Extent3d { width: HULL_NX, height: HULL_NZ, depth_or_array_layers: 1 },
                    mip_level_count: 1,
                    sample_count: 1,
                    dimension: wgpu::TextureDimension::D2,
                    format: wgpu::TextureFormat::R16Float,
                    usage: wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::COPY_DST,
                    view_formats: &[],
                });
                Slot {
                    boat: None,
                    last: None,
                    dims: [0.0; 3],
                    ubo: device.create_buffer(&wgpu::BufferDescriptor {
                        label: Some("tw_wake_kernel_params"),
                        size: std::mem::size_of::<WakeKernel>() as u64,
                        usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
                        mapped_at_creation: false,
                    }),
                    state: storage_buf(device, "tw_wake_state", cells * 16),
                    scratch: storage_buf(device, "tw_wake_scratch", cells * 16),
                    spec: storage_buf(device, "tw_wake_spectrum", cells * 16),
                    aer_a: storage_buf(device, "tw_wake_aer", cells * 4),
                    aer_b: storage_buf(device, "tw_wake_aer_next", cells * 4),
                    near_buf: storage_buf(device, "tw_wake_near_mean", (TW * TH) as u64 * 4),
                    hull_view: hull.create_view(&Default::default()),
                    hull,
                    display_layer: layer_view(&display, i),
                    aer_layer: layer_view(&aer, i),
                    near_layer: layer_view(&near, i),
                    binds: None,
                    center: [0; 2],
                    origin: [0.0; 2],
                    has_window: false,
                    sleeping: true,
                    idle: 1e9,
                    reset: true,
                    amount: 0.0,
                    near_box: [0.0, 0.0, 1.0, 1.0],
                    pose: None,
                    step: false,
                }
            })
            .collect();
        let smp_clamp = device.create_sampler(&wgpu::SamplerDescriptor {
            label: Some("tw_wake_linear_clamp"),
            mag_filter: wgpu::FilterMode::Linear,
            min_filter: wgpu::FilterMode::Linear,
            ..Default::default()
        });
        let smp_repeat = device.create_sampler(&wgpu::SamplerDescriptor {
            label: Some("tw_wake_linear_repeat"),
            address_mode_u: wgpu::AddressMode::Repeat,
            address_mode_v: wgpu::AddressMode::Repeat,
            mag_filter: wgpu::FilterMode::Linear,
            min_filter: wgpu::FilterMode::Linear,
            ..Default::default()
        });
        Self {
            layouts: [rows, cols, inv],
            pipelines,
            display_view: array_view(&display),
            near_view: array_view(&near),
            aer_view: array_view(&aer),
            smp_clamp,
            smp_repeat,
            slots,
            enabled: env_enabled(),
        }
    }

    pub fn display_view(&self) -> &wgpu::TextureView {
        &self.display_view
    }
    pub fn near_view(&self) -> &wgpu::TextureView {
        &self.near_view
    }
    pub fn aer_view(&self) -> &wgpu::TextureView {
        &self.aer_view
    }

    /// (Re)build the kernels' bind groups (the terrain heightmap / conform block changed).
    pub fn rebuild_bind(&mut self, device: &wgpu::Device, heightmap: &wgpu::TextureView, conform_ubo: &wgpu::Buffer) {
        use wgpu::BindingResource as R;
        for s in &mut self.slots {
            let rows = device.create_bind_group(&wgpu::BindGroupDescriptor {
                label: Some("tw_wake_rows"),
                layout: &self.layouts[0],
                entries: &[
                    e(0, s.ubo.as_entire_binding()),
                    e(1, s.state.as_entire_binding()),
                    e(2, s.scratch.as_entire_binding()),
                    e(3, s.spec.as_entire_binding()),
                    e(4, s.aer_a.as_entire_binding()),
                    e(5, s.aer_b.as_entire_binding()),
                    e(6, R::TextureView(&s.hull_view)),
                    e(7, R::Sampler(&self.smp_clamp)),
                    e(8, R::TextureView(&s.display_layer)),
                    e(9, R::TextureView(&s.aer_layer)),
                    e(14, R::TextureView(heightmap)),
                    e(15, conform_ubo.as_entire_binding()),
                ],
            });
            let cols = device.create_bind_group(&wgpu::BindGroupDescriptor {
                label: Some("tw_wake_cols"),
                layout: &self.layouts[1],
                entries: &[
                    e(0, s.ubo.as_entire_binding()),
                    e(3, s.spec.as_entire_binding()),
                    e(6, R::TextureView(&s.hull_view)),
                    e(7, R::Sampler(&self.smp_clamp)),
                    e(10, R::TextureView(&s.display_layer)),
                    e(11, R::Sampler(&self.smp_repeat)),
                    e(12, s.near_buf.as_entire_binding()),
                    e(13, R::TextureView(&s.near_layer)),
                ],
            });
            let inv = device.create_bind_group(&wgpu::BindGroupDescriptor {
                label: Some("tw_wake_inverse"),
                layout: &self.layouts[2],
                entries: &[
                    e(0, s.ubo.as_entire_binding()),
                    e(1, s.state.as_entire_binding()),
                    e(2, s.scratch.as_entire_binding()),
                    e(3, s.spec.as_entire_binding()),
                    e(4, s.aer_a.as_entire_binding()),
                    e(5, s.aer_b.as_entire_binding()),
                    e(14, R::TextureView(heightmap)),
                    e(15, conform_ubo.as_entire_binding()),
                ],
            });
            s.binds = Some([rows, cols, inv]);
        }
    }

    /// Any wake awake (stepping or fading)?
    #[allow(dead_code)]
    pub fn any_active(&self) -> bool {
        self.slots.iter().any(|s| s.amount > 0.0)
    }

    /// Assign the producer's boats to slots and advance each slot's schedule (WakeSim.update).
    /// Returns the readers' lanes for the surface block (4 per slot). `dt`: the water-clock step
    /// (0 while paused: no step, the wakes hold).
    pub fn update(&mut self, queue: &wgpu::Queue, boats: [Option<WakeBoat>; SLOTS], dt: f32, time: f32, sea_level: f32) -> [[f32; 4]; SLOTS * 4] {
        let mut lanes = [[0.0f32; 4]; SLOTS * 4];
        if !self.enabled {
            for s in &mut self.slots {
                s.amount = 0.0;
                s.step = false;
            }
            return lanes;
        }
        // keep a boat in the slot it already has; give the rest free (or sleeping, or abandoned)
        // slots
        let mut placed = [false; SLOTS];
        for s in &mut self.slots {
            s.last = None;
        }
        for (bi, b) in boats.iter().enumerate() {
            let Some(b) = b else { continue };
            if let Some(s) = self.slots.iter_mut().find(|s| s.boat == Some(b.id)) {
                s.last = Some(*b);
                placed[bi] = true;
            }
        }
        for (bi, b) in boats.iter().enumerate() {
            let Some(b) = b else { continue };
            if placed[bi] {
                continue;
            }
            let ids: Vec<u32> = boats.iter().flatten().map(|b| b.id).collect();
            // a free slot, else one whose boat has left the list (asleep ones first)
            let pick = self
                .slots
                .iter()
                .position(|s| s.boat.is_none())
                .or_else(|| self.slots.iter().position(|s| s.last.is_none() && s.sleeping))
                .or_else(|| self.slots.iter().position(|s| s.last.is_none() && !s.boat.is_some_and(|id| ids.contains(&id))));
            if let Some(i) = pick {
                let s = &mut self.slots[i];
                s.boat = Some(b.id);
                s.last = Some(*b);
                s.has_window = false;
                s.sleeping = false;
                s.idle = 0.0;
                s.reset = true;
                if s.dims != b.dims() {
                    s.dims = b.dims();
                    upload_hull(queue, &s.hull, &bake_hull(s.dims));
                }
            }
        }
        for (i, s) in self.slots.iter_mut().enumerate() {
            s.step = false;
            if let Some(k) = s.advance(dt, time, sea_level) {
                queue.write_buffer(&s.ubo, 0, bytemuck::bytes_of(&k));
                s.step = true;
            }
            if s.amount > 0.0 {
                let bp = k_boat(s);
                lanes[i * 4] = [s.center[0] as f32 * CELL, s.center[1] as f32 * CELL, bp[0], bp[1]];
                lanes[i * 4 + 1] = [bp[2], bp[3], 0.03, 0.08];
                // amount, amplitude, aeration out
                lanes[i * 4 + 2] = [s.amount, tune().amp, 0.3, 0.0];
                lanes[i * 4 + 3] = s.near_box;
            }
        }
        lanes
    }

    /// The three passes for every slot that steps this frame (one compute pass).
    pub fn dispatch(&self, encoder: &mut wgpu::CommandEncoder) -> bool {
        if !self.slots.iter().any(|s| s.step && s.binds.is_some()) {
            return false;
        }
        let mut pass = encoder.begin_compute_pass(&wgpu::ComputePassDescriptor { label: Some("tw_wake"), timestamp_writes: None });
        for s in &self.slots {
            let (true, Some(b)) = (s.step, s.binds.as_ref()) else { continue };
            pass.set_pipeline(&self.pipelines[0]);
            pass.set_bind_group(0, &b[0], &[]);
            pass.dispatch_workgroups(N, 1, 1);
            pass.set_pipeline(&self.pipelines[1]);
            pass.set_bind_group(0, &b[1], &[]);
            pass.dispatch_workgroups(HALF + 1 + NEAR_GROUPS, 1, 1);
            pass.set_pipeline(&self.pipelines[2]);
            pass.set_bind_group(0, &b[2], &[]);
            pass.dispatch_workgroups(N, 1, 1);
        }
        true
    }

    /// For the log: each slot's boat, size (half beam, half length, draft as simulated), speed,
    /// thrust, amount and whether it stepped this frame.
    pub fn stats(&self) -> String {
        let mut out = format!("tune {:?}", tune());
        for (i, s) in self.slots.iter().enumerate() {
            let sp = s.last.map_or(0.0, |b| (b.vel[0] * b.vel[0] + b.vel[1] * b.vel[1]).sqrt());
            let th = s.last.map_or(0.0, |b| b.throttle);
            out += &format!(
                "; slot {i}: boat {:?} dims [{:.2}, {:.2}, {:.2}] speed {sp:.1} m/s thrust {th:.2} amount {:.2} step {}",
                s.boat, s.dims[0], s.dims[1], s.dims[2], s.amount, s.step
            );
        }
        out
    }
}

fn e(binding: u32, resource: wgpu::BindingResource<'_>) -> wgpu::BindGroupEntry<'_> {
    wgpu::BindGroupEntry { binding, resource }
}

fn upload_hull(queue: &wgpu::Queue, tex: &wgpu::Texture, data: &[u16]) {
    queue.write_texture(
        wgpu::TexelCopyTextureInfo { texture: tex, mip_level: 0, origin: wgpu::Origin3d::ZERO, aspect: wgpu::TextureAspect::All },
        bytemuck::cast_slice(data),
        wgpu::TexelCopyBufferLayout { offset: 0, bytes_per_row: Some(HULL_NX * 2), rows_per_image: Some(HULL_NZ) },
        wgpu::Extent3d { width: HULL_NX, height: HULL_NZ, depth_or_array_layers: 1 },
    );
}

/// The slot's boat position xz and (cos, sin) of its yaw (the last known pose once it left the list).
fn k_boat(s: &Slot) -> [f32; 4] {
    s.pose.unwrap_or([0.0, 0.0, 1.0, 0.0])
}

impl Slot {
    /// WakeSim.update for this slot: None when nothing is dispatched this frame.
    fn advance(&mut self, dt: f32, time: f32, sea_level: f32) -> Option<WakeKernel> {
        if self.boat.is_none() {
            self.amount = 0.0;
            return None;
        }
        let b = self.last;
        if let Some(b) = b {
            // Tidewater: yaw = atan2(fw.x, fw.z), boatRot = (cos yaw, sin yaw) = (fw.z, fw.x)
            self.pose = Some([b.pos[0], b.pos[1], b.fwd[1], b.fwd[0]]);
        }
        let speed = b.map_or(0.0, |b| (b.vel[0] * b.vel[0] + b.vel[1] * b.vel[1]).sqrt());
        let moving = b.is_some_and(|b| speed > 0.5 || (b.driven && b.throttle.abs() > 0.04));
        if dt > 0.0 {
            self.idle = if moving { 0.0 } else { self.idle + dt };
        }
        if self.idle > SETTLE {
            // asleep: nothing is dispatched; the (faded out) output is ignored by the shaders
            self.sleeping = true;
            self.amount = 0.0;
            if b.is_none() {
                // its boat has gone and the wake has settled: the slot is free
                self.boat = None;
                self.pose = None;
            }
            return None;
        }
        if self.sleeping {
            self.sleeping = false;
            self.has_window = false;
            self.reset = true;
        }
        self.amount = ((SETTLE - self.idle) / 4.0).min(1.0);
        if dt <= 0.0 || self.pose.is_none() {
            return None;
        }
        let h = dt.clamp(1.0 / 240.0, 1.0 / 30.0);
        let tn = tune();
        let pose = self.pose.unwrap_or_default();

        // ---- window: keep the boat inside a box around the centre (moves by whole cells)
        let bx = pose[0] / CELL;
        let bz = pose[1] / CELL;
        let bx_box = 170.0;
        let c = &mut self.center;
        if !self.has_window || (bx - c[0] as f32).abs() > N as f32 || (bz - c[1] as f32).abs() > N as f32 {
            *c = [bx.round() as i32, bz.round() as i32];
            self.has_window = true;
            self.reset = true;
        }
        if bx - c[0] as f32 > bx_box {
            c[0] = (bx - bx_box).ceil() as i32;
        }
        if c[0] as f32 - bx > bx_box {
            c[0] = (bx + bx_box).floor() as i32;
        }
        if bz - c[1] as f32 > bx_box {
            c[1] = (bz - bx_box).ceil() as i32;
        }
        if c[1] as f32 - bz > bx_box {
            c[1] = (bz + bx_box).floor() as i32;
        }
        let prev = self.origin;
        self.origin = [(c[0] - HALF as i32) as f32, (c[1] - HALF as i32) as f32];
        let prev = if self.reset { self.origin } else { prev };

        // ---- boat (Tidewater's schedules, keyed to the Froude number and beam: see the header)
        let [beam, half_len, draft] = self.dims;
        let froude = speed / (GRAVITY * 2.0 * half_len).sqrt();
        // Tidewater's 8 m boat: speed 3..10 m/s (trim), 2..9 m/s (hollow / wash width)
        let plane_trim = smooth(froude, 0.34, 1.13);
        let plane = smooth(froude, 0.23, 1.02);
        let size = (draft / 0.35).clamp(0.5, 3.0);
        let trim = [-0.08 * plane_trim * size, 0.0, -0.035 * plane_trim * (4.0 / half_len).min(1.0)];
        let throttle = b.map_or(0.0, |b| if b.driven { b.throttle.abs() } else { 0.0 });
        let present = if b.is_some() { 1.0 } else { 0.0 };
        let (x1, z0, z1) = hull_box(self.dims);
        let tx = beam + 0.4;
        let tz = half_len + 0.8;
        self.near_box = [-tx, -tz, 2.0 * tx, 2.0 * tz];
        let k = WakeKernel {
            win: [self.origin[0], self.origin[1], prev[0], prev[1]],
            boat: pose,
            trim: [trim[0], trim[1], trim[2], if self.reset { 1.0 } else { 0.0 }],
            // dt, amount, source gain (0 once the boat has gone), wash
            m0: [
                h,
                self.amount,
                0.72 * (1.0 + 0.2 * plane) * present * tn.source,
                (throttle + smooth(speed, 1.5, 7.0) * 0.5 * present) * tn.wash,
            ],
            // bow, speed, wash half width, boil
            m1: [smooth(speed, 3.5, 9.0), speed, (0.7 + 0.6 * plane) * (beam / 1.2).clamp(0.6, 4.0), 1.2],
            // visc, dynamic head, hollow length, hollow gain
            m2: [0.006, 0.35 * speed * speed / (2.0 * GRAVITY), 0.35 + 0.015 * speed * speed, 0.3 * plane],
            // foam gain, aeration gain, near-template rate, sea level
            m3: [tn.foam, tn.aer, if self.reset { 1.0 } else { 1.0 - (-h / 0.25).exp() }, sea_level],
            hull: [x1, z0, z1 - z0, -half_len],
            near: self.near_box,
            m4: [time, 0.0, 0.0, 0.0],
        };
        self.reset = false;
        Some(k)
    }
}
