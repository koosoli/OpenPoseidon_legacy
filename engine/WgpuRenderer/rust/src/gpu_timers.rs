// WTR-002 — GPU timestamp instrumentation. Encoder-level timestamp brackets around the
// named water-pipeline regions (spectrum/FFT phases, interaction, foam, planar reflection,
// water draw, underwater composite), resolved into a small round-robin readback ring and
// harvested non-blocking one-or-more frames later. The master plan gates any FFT rework on
// these timings existing ("Do not replace the FFT implementation before these timings exist").
//
// Design notes:
// * Uses TIMESTAMP_QUERY + TIMESTAMP_QUERY_INSIDE_ENCODERS so brackets are written on the
//   COMMAND ENCODER between passes — no pass descriptor changes, and one bracket can cover
//   multi-pass regions (planar clouds = march + composite, the planar mip chain, the split
//   FFT phases). Both features are adapter-gated like `partially_bound`: absent => every
//   method is a no-op and the FFI reports 0 regions.
// * `begin`/`end` take `&self` (the written mask is a Cell) so brackets can be dropped into
//   `render_frame` next to existing disjoint field borrows without threading `&mut self`.
// * Readback never blocks the frame: resolve+copy goes into the frame's encoder, map_async
//   is requested after submit, and results are drained with a non-blocking poll the next
//   time round. A saturated ring simply skips that frame's sample.
// * Regions the spec names but that have no standalone GPU pass yet (SSR, refraction,
//   underwater froxel, caustics) hold reserved indices reporting -1 ("n/a"), so the FFI
//   ABI and the Water-tab rows are already in place when those passes land. Whitewater is
//   rendered as part of WaterDraw, so its cost is intentionally included in that region.
//   SSR + refraction are likewise fragment-shader work inside the water draw; caustics
//   ride the underwater composite shader.

use std::cell::Cell;
use std::sync::mpsc;

/// One timed region. The discriminants are the FFI index contract mirrored by
/// `WgrGpuTimerRegion` in wgpu_renderer.hpp — append only, never reorder.
/// Reserved variants are never constructed yet (their passes don't exist), but they
/// hold their FFI slots — hence the allow.
#[allow(dead_code)]
#[derive(Clone, Copy)]
#[repr(u32)]
pub enum Region {
    SpectrumInit = 0,         // h0 spectrum generation (only on spectrum-dirty frames)
    SpectrumEvolve = 1,       // per-frame spectrum evolution
    FftHorizontal = 2,        // FFT butterfly stages, axis 0
    FftVertical = 3,          // FFT butterfly stages, axis 1
    FftCompose = 4,           // displacement/dynamics/auxiliary composition
    Interaction = 5,          // injection + propagation (one fused kernel today)
    Foam = 6,                 // persistent foam update
    Whitewater = 7,           // reserved — no whitewater pass exists yet
    PlanarSky = 8,            // planar reflection: sky
    PlanarTerrain = 9,        // planar reflection: terrain
    PlanarObjects = 10,       // planar reflection: reflected cull + GPU-driven objects
    PlanarClouds = 11,        // planar reflection: cloud march + composite
    PlanarMips = 12,          // planar reflection mip generation
    WaterSsr = 13,            // reserved — SSR is in-shader inside WaterDraw today
    WaterRefraction = 14,     // reserved — refraction is in-shader inside WaterDraw today
    WaterDraw = 15,           // the water surface pass (includes SSR + refraction cost)
    UnderwaterFroxel = 16,    // the froxel light-volume compute pass
    UnderwaterComposite = 17, // fullscreen underwater compositor
    Caustics = 18,            // the FFT-derived caustic compute pass
    // --- GRS-A: grass. The three placement dispatches are standalone compute
    // passes, so they bracket on the encoder like the water regions above. The
    // draw regions are ops inside the shared main/shadow render passes and can
    // only be bracketed with TIMESTAMP_QUERY_INSIDE_PASSES; without that
    // feature they stay at -1 ("n/a") while the compute rows still report.
    GrassPlaceNear = 19,
    GrassPlaceMid = 20,
    GrassPlaceFar = 21,
    GrassPrepass = 22, // depth/normal prepass (near + mid)
    GrassColor = 23,   // colour pass (far + mid + near)
    GrassShadow = 24,  // near blades into the cascade depth map
    // --- LIT-020: interior sky visibility. Both are encoder-level regions (a compute chain and
    // a set of depth passes), so they bracket like the water ones.
    InteriorSkyCull = 26, // one cull dispatch chain per sampled sky direction
    InteriorSkyDraw = 27, // the per-direction depth passes that fill the sky map
    // --- LIT-010: screen-space AO. Its cost had never been measured, which is a poor state in
    // which to make it default-on.
    GtaoPrep = 28,    // depth resolve + normal resolve + the linear-Z mip chain
    GtaoCompute = 29, // the horizon-march itself
    GtaoBlur = 30,    // bilateral denoise
    // --- PERF-004: main-view terrain. Appended so every established FFI index stays
    // stable. These are in-pass timestamps because terrain shares the world passes.
    TerrainPrepass = 31,
    TerrainColor = 32,
    // --- PERF-005: object rendering. Measured on Stratis @1280x720 with a ~20K-object working
    // set the GPU frame was 19.1 ms with only ~4.5 ms attributed (terrain + water + AO +
    // interior sky); everything below exists to name the other ~14.5 ms. Two of these are
    // CONTAINERS (PrepassSegment / ColorSegment) that deliberately overlap the leaves inside
    // them — never sum a container with its leaves, the C++ reporter keeps them apart.
    ObjCullMain = 33,      // main-view COUNT/EMIT/SCATTER (prepass + occluder set)
    ObjCullShadow = 34,    // every active cascade's cull dispatch chain
    ObjCullColor = 35,     // Hi-Z pyramid build + the colour-pass occlusion cull
    ObjPrepassSolid = 36,  // GPU-driven depth+normal prepass, variant 0 (solid)
    ObjPrepassAlpha = 37,  // GPU-driven depth+normal prepass, variant 1 (alpha-cutout foliage)
    ObjColorSolid = 38,    // GPU-driven colour, variant 0 (solid)
    ObjColorAlpha = 39,    // GPU-driven colour, variant 1 (alpha-cutout foliage)
    ObjPrepassSegment = 40, // CONTAINER: the whole depth/normal prepass render pass
    ObjColorSegment = 41,   // CONTAINER: the whole 3D colour sub-pass (incl. terrain/grass/direct)
    // Per-cascade shadow rendering. One region per cascade because the standing suspicion was
    // that shadows are among the frame's largest items and "shadows" as a single number cannot
    // tell you WHICH cascade to shrink. MAX_CASCADES is 4 (gfx3d/mod.rs).
    ShadowCascade0 = 42,
    ShadowCascade1 = 43,
    ShadowCascade2 = 44,
    ShadowCascade3 = 45,
    FrameTotal = 25,  // all submitted frame work; excludes acquire/present pacing
}

/// The four per-cascade regions, indexable by cascade number.
pub const SHADOW_CASCADE_REGIONS: [Region; 4] = [
    Region::ShadowCascade0,
    Region::ShadowCascade1,
    Region::ShadowCascade2,
    Region::ShadowCascade3,
];

/// Region count — the FFI getter's element contract (WGR_GPU_TIMER_REGION_COUNT).
pub const REGION_COUNT: usize = 46;
/// Water occupies [0, WATER_REGION_COUNT); grass occupies the remainder. The two
/// debug tabs slice the shared array with these so neither shows the other's rows.
/// Consumed on the C++ side (Engine::kWaterGpuRegionEnd) and by the layout test.
#[allow(dead_code)]
pub const WATER_REGION_COUNT: usize = 19;

const QUERY_COUNT: u32 = (REGION_COUNT * 2) as u32;
const BUFFER_SIZE: u64 = QUERY_COUNT as u64 * wgpu::QUERY_SIZE as u64;
// Three slots ride out double/triple-buffered presentation without ever blocking.
const RING_SLOTS: usize = 3;

enum SlotState {
    Idle,
    // Copied into by this frame's encoder; map_async is requested after submit.
    Pending {
        written: u64,
    },
    // map_async requested; the channel resolves once the GPU copy completes.
    InFlight {
        written: u64,
        rx: mpsc::Receiver<Result<(), wgpu::BufferAsyncError>>,
    },
}

struct Slot {
    buf: wgpu::Buffer,
    state: SlotState,
}

struct Inner {
    // TIMESTAMP_QUERY_INSIDE_PASSES: lets begin_pass/end_pass bracket draws that
    // live inside a shared render pass (grass is a Plan3dOp, not its own pass).
    inside_passes: bool,
    query_set: wgpu::QuerySet,
    resolve_buf: wgpu::Buffer,
    slots: Vec<Slot>,
    // Nanoseconds per timestamp tick (Queue::get_timestamp_period).
    period: f32,
    // Bitmask of regions bracketed since begin_frame (Cell: written from `&self`).
    written: Cell<u64>,
    // Latest harvested per-region duration in ms; -1 = never measured (pass absent).
    latest_ms: [f32; REGION_COUNT],
}

pub struct GpuTimers {
    inner: Option<Inner>,
    // PERF-005 — CPU ENCODE time per region, deliberately OUTSIDE `Inner` so it still reports on
    // an adapter without timestamp queries (where `inner` is None and every GPU row reads -1).
    // This measures how long the CPU spends RECORDING that region into the command encoder, not
    // how long the GPU takes to run it; the two answer different questions and on a GPU-driven
    // renderer they differ by orders of magnitude. Same -1 = "did not run this frame" convention
    // as the GPU rows, for the same reason: a stale repeated number reads as a live measurement.
    cpu_open: [Cell<Option<std::time::Instant>>; REGION_COUNT],
    cpu_ms: [Cell<f32>; REGION_COUNT],
}

impl GpuTimers {
    /// `enabled` = the device was created with TIMESTAMP_QUERY + TIMESTAMP_QUERY_INSIDE_ENCODERS.
    /// `inside_passes` = TIMESTAMP_QUERY_INSIDE_PASSES was also available.
    pub fn new(
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        enabled: bool,
        inside_passes: bool,
    ) -> Self {
        if !enabled {
            return Self {
                inner: None,
                cpu_open: std::array::from_fn(|_| Cell::new(None)),
                cpu_ms: std::array::from_fn(|_| Cell::new(-1.0)),
            };
        }
        let query_set = device.create_query_set(&wgpu::QuerySetDescriptor {
            label: Some("wgr_gpu_timers"),
            ty: wgpu::QueryType::Timestamp,
            count: QUERY_COUNT,
        });
        let resolve_buf = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("wgr_gpu_timers_resolve"),
            size: BUFFER_SIZE,
            usage: wgpu::BufferUsages::QUERY_RESOLVE | wgpu::BufferUsages::COPY_SRC,
            mapped_at_creation: false,
        });
        let slots = (0..RING_SLOTS)
            .map(|i| Slot {
                buf: device.create_buffer(&wgpu::BufferDescriptor {
                    label: Some(&format!("wgr_gpu_timers_readback_{i}")),
                    size: BUFFER_SIZE,
                    usage: wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::MAP_READ,
                    mapped_at_creation: false,
                }),
                state: SlotState::Idle,
            })
            .collect();
        Self {
            inner: Some(Inner {
                inside_passes,
                query_set,
                resolve_buf,
                slots,
                period: queue.get_timestamp_period(),
                written: Cell::new(0),
                latest_ms: [-1.0; REGION_COUNT],
            }),
            cpu_open: std::array::from_fn(|_| Cell::new(None)),
            cpu_ms: std::array::from_fn(|_| Cell::new(-1.0)),
        }
    }

    pub fn enabled(&self) -> bool {
        self.inner.is_some()
    }

    /// Reset the per-frame written mask. Call once after the frame encoder is created.
    pub fn begin_frame(&self) {
        if let Some(inner) = &self.inner {
            inner.written.set(0);
        }
        // CPU rows reset to -1 ("did not run this frame"), NOT to 0 and NOT left alone. Leaving
        // them alone is the documented trap this repo already paid for once: a one-shot pass keeps
        // advertising its old cost forever and reads as per-frame work.
        for i in 0..REGION_COUNT {
            self.cpu_open[i].set(None);
            self.cpu_ms[i].set(-1.0);
        }
    }

    /// Start the CPU encode clock for `region`. Pairs with `cpu_end`; unpaired opens are
    /// discarded by the next `begin_frame`.
    pub fn cpu_begin(&self, region: Region) {
        self.cpu_open[region as usize].set(Some(std::time::Instant::now()));
    }

    /// Stop the CPU encode clock, ACCUMULATING into this frame's total. Accumulating (rather
    /// than overwriting) is what lets a region that is recorded in several places — the colour
    /// variants, the per-cascade shadow draws — report one honest per-frame figure.
    pub fn cpu_end(&self, region: Region) {
        let i = region as usize;
        if let Some(t0) = self.cpu_open[i].replace(None) {
            let ms = t0.elapsed().as_secs_f32() * 1.0e3;
            let prev = self.cpu_ms[i].get();
            self.cpu_ms[i].set(if prev < 0.0 { ms } else { prev + ms });
        }
    }

    /// Copy the latest per-region CPU encode ms into `out` (-1 = not recorded this frame).
    /// Always available — unlike the GPU rows this needs no adapter feature.
    pub fn cpu_timings(&self, out: &mut [f32]) -> u32 {
        for (dst, src) in out.iter_mut().zip(self.cpu_ms.iter()) {
            *dst = src.get();
        }
        REGION_COUNT as u32
    }

    /// Open a region bracket. Must be called OUTSIDE any render/compute pass (the
    /// timestamp is written on the encoder itself).
    pub fn begin(&self, encoder: &mut wgpu::CommandEncoder, region: Region) {
        if let Some(inner) = &self.inner {
            encoder.write_timestamp(&inner.query_set, region as u32 * 2);
        }
    }

    /// Close a region bracket and mark it measured this frame.
    pub fn end(&self, encoder: &mut wgpu::CommandEncoder, region: Region) {
        if let Some(inner) = &self.inner {
            encoder.write_timestamp(&inner.query_set, region as u32 * 2 + 1);
            inner
                .written
                .set(inner.written.get() | (1u64 << region as u32));
        }
    }

    /// Open a region bracket from INSIDE a render pass. No-op unless the adapter
    /// offered TIMESTAMP_QUERY_INSIDE_PASSES. Use for draws that share a pass with
    /// other work (grass colour/prepass/shadow ops inside the 3D plan).
    pub fn begin_pass(&self, pass: &mut wgpu::RenderPass<'_>, region: Region) {
        if let Some(inner) = &self.inner {
            if inner.inside_passes {
                pass.write_timestamp(&inner.query_set, region as u32 * 2);
            }
        }
    }

    /// Close an in-pass bracket and mark it measured this frame.
    pub fn end_pass(&self, pass: &mut wgpu::RenderPass<'_>, region: Region) {
        if let Some(inner) = &self.inner {
            if inner.inside_passes {
                pass.write_timestamp(&inner.query_set, region as u32 * 2 + 1);
                inner
                    .written
                    .set(inner.written.get() | (1u64 << region as u32));
            }
        }
    }

    /// Record the query resolve + copy into a free ring slot. Call at the end of the
    /// frame encoder, before submit. Skips silently when nothing was bracketed or the
    /// ring is saturated (the sample for this frame is simply dropped).
    pub fn resolve(&mut self, encoder: &mut wgpu::CommandEncoder) {
        let Some(inner) = &mut self.inner else {
            return;
        };
        let written = inner.written.get();
        if written == 0 {
            return;
        }
        let Some(slot) = inner
            .slots
            .iter_mut()
            .find(|s| matches!(s.state, SlotState::Idle))
        else {
            return;
        };
        // Queries never bracketed this frame (reserved regions, frozen/absent passes) have
        // never been reset on the backend, and resolving an unavailable query can wedge the
        // device (observed: Vulkan device loss during --check — get_current_texture
        // validation errors + destroyed readback buffers). Stamp every unwritten pair at
        // "now" so the full-range resolve below only ever touches available queries;
        // harvest ignores these zero-duration dummies via the written mask.
        for region in 0..REGION_COUNT as u32 {
            if written & (1u64 << region) == 0 {
                encoder.write_timestamp(&inner.query_set, region * 2);
                encoder.write_timestamp(&inner.query_set, region * 2 + 1);
            }
        }
        encoder.resolve_query_set(&inner.query_set, 0..QUERY_COUNT, &inner.resolve_buf, 0);
        encoder.copy_buffer_to_buffer(&inner.resolve_buf, 0, &slot.buf, 0, BUFFER_SIZE);
        slot.state = SlotState::Pending { written };
    }

    /// Kick map_async on freshly copied slots and drain completed ones (non-blocking).
    /// Call once per frame after queue.submit.
    pub fn harvest(&mut self, device: &wgpu::Device) {
        let Some(inner) = &mut self.inner else {
            return;
        };
        for slot in &mut inner.slots {
            if let SlotState::Pending { written } = slot.state {
                let (tx, rx) = mpsc::channel();
                slot.buf.slice(..).map_async(wgpu::MapMode::Read, move |r| {
                    let _ = tx.send(r);
                });
                slot.state = SlotState::InFlight { written, rx };
            }
        }
        // Non-blocking maintenance so completed map_asyncs from previous frames fire.
        let _ = device.poll(wgpu::PollType::Poll);
        for slot in &mut inner.slots {
            let SlotState::InFlight { written, rx } = &slot.state else {
                continue;
            };
            match rx.try_recv() {
                Ok(Ok(())) => {
                    let written = *written;
                    {
                        let data = slot.buf.slice(..).get_mapped_range();
                        // Read ticks byte-wise: mapped data has no u64 alignment guarantee.
                        let tick = |i: usize| {
                            u64::from_le_bytes(data[i * 8..i * 8 + 8].try_into().unwrap())
                        };
                        for region in 0..REGION_COUNT {
                            if written & (1u64 << region) == 0 {
                                // Report the latest completed frame, not the last historical
                                // occurrence of each pass. Otherwise a skipped planar reflection
                                // keeps advertising its old multi-millisecond cost indefinitely.
                                inner.latest_ms[region] = -1.0;
                                continue;
                            }
                            let start = tick(region * 2);
                            let end = tick(region * 2 + 1);
                            let ns = end.saturating_sub(start) as f64 * inner.period as f64;
                            inner.latest_ms[region] = (ns / 1.0e6) as f32;
                        }
                    }
                    slot.buf.unmap();
                    slot.state = SlotState::Idle;
                }
                Ok(Err(_)) | Err(mpsc::TryRecvError::Disconnected) => {
                    // Mapping failed (device loss etc.) — recycle the slot, keep last values.
                    slot.state = SlotState::Idle;
                }
                Err(mpsc::TryRecvError::Empty) => {}
            }
        }
    }

    /// Copy the latest harvested per-region ms into `out` (-1 = never measured).
    /// Returns REGION_COUNT when timers exist, 0 when the adapter lacks the features.
    pub fn timings(&self, out: &mut [f32]) -> u32 {
        let Some(inner) = &self.inner else {
            return 0;
        };
        for (dst, src) in out.iter_mut().zip(inner.latest_ms.iter()) {
            *dst = *src;
        }
        REGION_COUNT as u32
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // The FFI index contract: WgrGpuTimerRegion in wgpu_renderer.hpp mirrors these
    // discriminants and DebugOverlay's name table is ordered by them. Locking the
    // count + a few pinned indices catches accidental reordering on either side.
    #[test]
    fn region_indices_stay_ffi_stable() {
        assert_eq!(REGION_COUNT, 46);
        assert_eq!(Region::SpectrumInit as u32, 0);
        assert_eq!(Region::FftCompose as u32, 4);
        assert_eq!(Region::Interaction as u32, 5);
        assert_eq!(Region::PlanarSky as u32, 8);
        assert_eq!(Region::WaterDraw as u32, 15);
        assert_eq!(Region::Caustics as u32, 18);
        // Water rows must stay contiguous at the front so the two tabs can slice
        // the shared array by range.
        assert_eq!(WATER_REGION_COUNT, 19);
        assert_eq!(Region::GrassPlaceNear as u32, WATER_REGION_COUNT as u32);
        assert_eq!(Region::GrassShadow as u32, 24);
        assert_eq!(Region::FrameTotal as u32, 25);
        // LIT-020 appended AFTER FrameTotal's index rather than renumbering it: these are FFI
        // slot numbers mirrored in wgpu_renderer.hpp, so an insert would silently relabel every
        // row in the debug tabs.
        assert_eq!(Region::InteriorSkyCull as u32, 26);
        assert_eq!(Region::InteriorSkyDraw as u32, 27);
        assert_eq!(Region::GtaoPrep as u32, 28);
        assert_eq!(Region::GtaoBlur as u32, 30);
        assert_eq!(Region::TerrainPrepass as u32, 31);
        assert_eq!(Region::TerrainColor as u32, 32);
        // PERF-005 object regions. The cascade block must stay contiguous and in order —
        // SHADOW_CASCADE_REGIONS is indexed by cascade number.
        assert_eq!(Region::ObjCullMain as u32, 33);
        assert_eq!(Region::ObjColorAlpha as u32, 39);
        assert_eq!(Region::ObjPrepassSegment as u32, 40);
        assert_eq!(Region::ObjColorSegment as u32, 41);
        for (c, r) in SHADOW_CASCADE_REGIONS.iter().enumerate() {
            assert_eq!(*r as u32, 42 + c as u32);
        }
        // Every region's begin/end pair fits the query set.
        assert!(QUERY_COUNT as usize == REGION_COUNT * 2);
        // A u64 written mask covers all regions and leaves append-only room.
        assert!(REGION_COUNT <= 64);
    }

    // A disabled timer (adapter without the features) must be a total no-op that
    // still satisfies the FFI contract by reporting 0 regions.
    #[test]
    fn disabled_timers_report_zero_regions() {
        let timers = GpuTimers {
            inner: None,
            cpu_open: std::array::from_fn(|_| Cell::new(None)),
            cpu_ms: std::array::from_fn(|_| Cell::new(-1.0)),
        };
        assert!(!timers.enabled());
        let mut out = [0.0f32; REGION_COUNT];
        assert_eq!(timers.timings(&mut out), 0);
    }

    // PERF-005 — the CPU rows must work with `inner: None` (no timestamp-query adapter), must
    // ACCUMULATE across repeated brackets, and must reset to -1 (not 0, not stale) each frame.
    #[test]
    fn cpu_rows_accumulate_and_reset_without_a_gpu() {
        let t = GpuTimers {
            inner: None,
            cpu_open: std::array::from_fn(|_| Cell::new(None)),
            cpu_ms: std::array::from_fn(|_| Cell::new(-1.0)),
        };
        let mut out = [0.0f32; REGION_COUNT];
        t.begin_frame();
        assert_eq!(t.cpu_timings(&mut out), REGION_COUNT as u32);
        assert_eq!(out[Region::ObjColorSolid as usize], -1.0);
        for _ in 0..2 {
            t.cpu_begin(Region::ObjColorSolid);
            t.cpu_end(Region::ObjColorSolid);
        }
        t.cpu_timings(&mut out);
        // Two closed brackets => a non-negative accumulated total, never still -1.
        assert!(out[Region::ObjColorSolid as usize] >= 0.0);
        // A region never bracketed stays "not run" rather than reading 0 ms of real work.
        assert_eq!(out[Region::ObjColorAlpha as usize], -1.0);
        // Next frame clears it back to "not run" even though it ran last frame.
        t.begin_frame();
        t.cpu_timings(&mut out);
        assert_eq!(out[Region::ObjColorSolid as usize], -1.0);
    }
}
