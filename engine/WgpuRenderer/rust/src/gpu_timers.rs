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
    ObjCullMain = 33,       // main-view COUNT/EMIT/SCATTER (prepass + occluder set)
    ObjCullShadow = 34,     // every active cascade's cull dispatch chain
    ObjCullColor = 35,      // Hi-Z pyramid build + the colour-pass occlusion cull
    ObjPrepassSolid = 36,   // GPU-driven depth+normal prepass, variant 0 (solid)
    ObjPrepassAlpha = 37,   // GPU-driven depth+normal prepass, variant 1 (alpha-cutout foliage)
    ObjColorSolid = 38,     // GPU-driven colour, variant 0 (solid)
    ObjColorAlpha = 39,     // GPU-driven colour, variant 1 (alpha-cutout foliage)
    ObjPrepassSegment = 40, // CONTAINER: the whole depth/normal prepass render pass
    ObjColorSegment = 41,   // CONTAINER: the whole 3D colour sub-pass (incl. terrain/grass/direct)
    // Per-cascade shadow rendering. One region per cascade because the standing suspicion was
    // that shadows are among the frame's largest items and "shadows" as a single number cannot
    // tell you WHICH cascade to shrink. MAX_CASCADES is 4 (gfx3d/mod.rs).
    ShadowCascade0 = 42,
    ShadowCascade1 = 43,
    ShadowCascade2 = 44,
    ShadowCascade3 = 45,
    // --- FAR INSTANCE TIER. The cull is a standalone compute pass (encoder-level bracket,
    // like the water regions); the draw is an op inside the shared 3D colour sub-pass, so it
    // needs TIMESTAMP_QUERY_INSIDE_PASSES and reads "n/a" without it. Two rows because they
    // scale with different things: the sweep with the world's total placement count, the draw
    // with whatever survived sub-pixel rejection.
    FarCull = 46,
    FarDraw = 47,
    // --- Attributing the 3D colour sub-pass. It measured 12.824 ms on perf_abel @1920x1080
    // while every leaf inside it summed to 3.777 (grass 3.400, terrain 0.242, objects solid
    // 0.090, objects alpha 0.045): 9.05 ms, 70% of the pass, belonged to no region at all.
    // What was unbracketed in there was the cloud march + its composite, and the two
    // CPU-replayed op replays. Hence these four.
    CloudMarch = 48,     // depth-aware over-scene cloud march (low-res)
    CloudComposite = 49, // upsample + composite of the cloud buffer
    // CONTAINERS, like the segment pair above: the pre-water replay also encloses terrain and
    // grass, which have their own regions. The CPU-replayed direct draws have no region of
    // their own and cannot get one -- Plan3dOp::Draw3D is per-draw and a region is one
    // begin/end per frame -- so their cost is DERIVED: OpsPreWater - TerrainColor - GrassColor.
    OpsPreWater = 50,  // CONTAINER: opaque replay (terrain + grass + CPU-replayed direct)
    OpsPostWater = 51, // CONTAINER: the depth-sorted transparent replay
    // --- The post chain and the UI. After the colour sub-pass was attributed, 2.85 ms of a
    // 23.08 ms frame at 1920x1080 -- 12.3% -- still belonged to no region: everything between
    // the last world draw and present. These six name it. Every one is OUTERMOST: they run
    // after the colour sub-pass closes, not inside it.
    HdrResolve = 52,     // MSAA -> single-sample resolve of the HDR scene target
    GodRays = 53,        // volumetric sun shafts, marched before bloom so they bloom
    Bloom = 54,          // bright-pass + the blur pyramid
    ExposureAdapt = 55,  // luminance reduction + the eased 1x1 exposure scale
    TonemapResolve = 56, // the tonemap curve into the display-referred target
    Ui2d = 57,           // the 2D/UI segment drawn over the tonemapped frame
    // --- The atmosphere block. PERF-010 spent a night narrowing a ~3 ms per-frame residual
    // that is independent of scene cost, of pass count and of RESOLUTION (7.7x the output
    // pixels moved it 12%). These five were the resolution-independent per-frame GPU work
    // with no region at all: every one is a FIXED-SIZE target -- LUT, froxel grid, equirect
    // env map, nine SH coefficients -- which is exactly why the residual did not care how
    // big the window was. Appended after Ui2d rather than inserted, because these are FFI
    // slot numbers mirrored in wgpu_renderer.hpp and an insert silently relabels every row.
    SkyLuts = 58,   // transmittance + multiscatter LUTs (rebuilt only when the atmosphere changes)
    SkyFroxel = 59, // the aerial-perspective froxel volume, with terrain + cascade occlusion
    SkyEnv = 60,    // the disc-free equirect sky reflection env map
    SkySh = 61,     // env map -> SH-9 diffuse sky irradiance
    SkyDraw = 62,   // the fullscreen sky pass itself
    // --- Worker pacing (PERF-021). CPU-only rows: the GPU column stays n/a. FrameTotal
    // deliberately excludes these; they are where the worker's period goes when the GPU is
    // the bound. Acquire is the swapchain wait BEFORE this frame's encoder exists (recorded
    // into the frame after begin_frame), present the call at the end.
    WorkerAcquire = 63, // surface.get_current_texture(): blocked until an image is free
    WorkerPresent = 64, // frame.present()
    WorkerSetup = 65,   // render_frame entry -> begin_frame (uploads, drains, acquire included)
    WorkerSubmit = 66,  // queue.submit of the frame encoder
    WorkerHarvest = 67, // the non-blocking readback harvests after present
    GiProbes = 68,      // REN-GI-001: the irradiance probe volume update (compute)
    GiRsm = 69,         // REN-GI-002: the sun proxy (cull + draw), when it re-renders
    // PERF-024: the worker's ENCODE, split four ways. CPU-only rows: they measure how long the
    // recording takes, not the GPU work it records (the regions above do that). The four are
    // disjoint and cover render_frame from begin_frame to submit.
    WorkerEncEarly = 70,  // grass/water updates, skin bake, culls, interior sky, GI probes
    WorkerEncShadow = 71, // the shadow cascade passes
    WorkerEncMain = 72,   // sky, prepass, the 3D colour sub-pass, water, the post chain, UI
    WorkerEncTail = 73,   // the stats/mip/visibility resolves before submit
    // ...and `main` split again, since it holds most of it.
    WorkerEncSky = 74,    // the atmosphere LUTs, froxel, env bake, cloud march and the sky pass
    WorkerEncDraw = 75,   // the depth prepass and the 3D colour sub-pass replay, water included
    WorkerEncPost = 76,   // the post chain (tonemap, bloom, DoF, god rays), upscale and UI
    // LGT-026: the local-light shadow views as one region. SHADOW_CASCADE_REGIONS is indexed
    // by cascade number and has exactly MAX_CASCADES entries, so `SHADOW_CASCADE_REGIONS.get(c)`
    // returned None for every local view and the whole tiled atlas was unmeasured.
    LocalShadow = 77,
    // TW-WATER W2 — the Tidewater Native ocean's own compute, so a capture shows WHICH backend
    // ran: in Tidewater mode the Current OP water rows (0-6, 8-12, 16-18) read n/a and these
    // two carry the FFT; in Current OP mode it is the other way round. The draw itself stays in
    // WaterDraw (it is the same pass for either backend).
    TwOceanSpectrum = 78, // h0 spectrum regeneration (spectrum-dirty frames only)
    TwOceanFft = 79,      // the two fused FFT dispatches + the compute mip chain
    TwShoreSim = 80,      // TW-WATER W3b: the shore simulation (both regions) + its state copy
    TwWake = 81,          // TW-WATER W6: the boat wake simulations (every slot that stepped)
    TwCaustics = 82,      // TW-WATER W7h: the caustic photon splats + their mip chains (W5b)
    TwBreakers = 83,      // TW-WATER W7h: crest finder, breaker spray emitters, spray update (W3d/W4a/W6i)
    WeatherCoverNearCull = 84,
    WeatherCoverFarCull = 85,
    WeatherCoverNearDraw = 86,
    WeatherCoverFarDraw = 87,
    LayeredFog = 88, // opt-in finite ASL transport compute, separate from atmospheric froxels
    FrameTotal = 25, // all submitted frame work; excludes acquire/present pacing
}

/// The four per-cascade regions, indexable by cascade number.
pub const SHADOW_CASCADE_REGIONS: [Region; 4] = [
    Region::ShadowCascade0,
    Region::ShadowCascade1,
    Region::ShadowCascade2,
    Region::ShadowCascade3,
];

/// Region count — the FFI getter's element contract (WGR_GPU_TIMER_REGION_COUNT).
pub const REGION_COUNT: usize = 89;
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
        written: u128,
    },
    // map_async requested; the channel resolves once the GPU copy completes.
    InFlight {
        written: u128,
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
    written: Cell<u128>,
    // Latest harvested per-region duration in ms; -1 = never measured (pass absent).
    latest_ms: [f32; REGION_COUNT],
}

/// Copied CPU recording facts for one explicitly observed render call. No GPU completion proof.
/// Kept on the caller stack; ordinary render calls do not allocate or retain this object.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct RenderCallCpuTimings {
    pub struct_size: u32,
    pub version: u32,
    /// 0 unobserved, 1 completed real frame, 2 acquire skipped, 3 failed/panicked.
    pub state: u32,
    pub count: u32,
    pub call_token: u64,
    /// acquire,present,setup,submit,harvest,early,shadow,main,tail,sky,draw,post.
    /// Hierarchical CPU buckets overlap; -1 means not recorded. Not GPU elapsed time.
    pub milliseconds: [f32; 12],
}
impl RenderCallCpuTimings {
    pub const VERSION: u32 = 1;
    pub fn new(call_token: u64) -> Self {
        Self { struct_size: std::mem::size_of::<Self>() as u32, version: Self::VERSION,
            state: 0, count: 0, call_token, milliseconds: [-1.0; 12] }
    }
    pub fn skip(&mut self) { self.clear(2); }
    pub fn fail(&mut self) { self.clear(3); }
    fn clear(&mut self, state: u32) {
        self.state = state; self.count = 0; self.milliseconds.fill(-1.0);
    }
    /// Only called at the successful real render-frame return, after present and harvest.
    pub fn complete(&mut self, timers: &GpuTimers) {
        let regions = [Region::WorkerAcquire, Region::WorkerPresent, Region::WorkerSetup,
            Region::WorkerSubmit, Region::WorkerHarvest, Region::WorkerEncEarly,
            Region::WorkerEncShadow, Region::WorkerEncMain, Region::WorkerEncTail,
            Region::WorkerEncSky, Region::WorkerEncDraw, Region::WorkerEncPost];
        for (dst, region) in self.milliseconds.iter_mut().zip(regions) {
            *dst = timers.cpu_ms[region as usize].get();
        }
        self.count = self.milliseconds.len() as u32; self.state = 1;
    }
}
const _: () = assert!(std::mem::size_of::<RenderCallCpuTimings>() == 72);
const _: () = assert!(std::mem::offset_of!(RenderCallCpuTimings, call_token) == 16);
const _: () = assert!(std::mem::offset_of!(RenderCallCpuTimings, milliseconds) == 24);

pub struct GpuTimers {
    inner: Option<Inner>,
    // PERF-005 — CPU ENCODE time per region, deliberately OUTSIDE `Inner` so it still reports on
    // an adapter without timestamp queries (where `inner` is None and every GPU row reads -1).
    // This measures how long the CPU spends RECORDING that region into the command encoder, not
    // how long the GPU takes to run it; the two answer different questions and on a GPU-driven
    // renderer they differ by orders of magnitude. Same -1 = "did not run this frame" convention
    // as the GPU rows, for the same reason: a stale repeated number reads as a live measurement.
    cpu_open: [Cell<Option<std::time::Instant>>; REGION_COUNT],
    // The frame being RECORDED. Cleared to -1 by begin_frame and filled by cpu_end as the
    // encoder is built, so at any instant it is a half-written frame.
    cpu_ms: [Cell<f32>; REGION_COUNT],
    // The last COMPLETE frame -- what readers see. This split is not tidiness, it is a bug
    // fix: --capture-metrics reported cpu_milliseconds = -1 for all 58 regions, including
    // ones whose GPU row showed real time, because the capture read after begin_frame had
    // cleared the accumulator and before the frame's cpu_end calls had refilled it. The GPU
    // rows were immune only by accident -- they live in an async readback that no per-frame
    // reset touches. Publishing on begin_frame gives the CPU rows the same property: a reader
    // at ANY point in the frame sees the previous frame whole, never a partial one and never
    // a freshly cleared one.
    cpu_ms_latest: [Cell<f32>; REGION_COUNT],
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
                cpu_ms_latest: std::array::from_fn(|_| Cell::new(-1.0)),
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
            cpu_ms_latest: std::array::from_fn(|_| Cell::new(-1.0)),
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
        // PUBLISH the frame that just ended, then clear the accumulator for the new one.
        //
        // Reset to -1 ("did not run this frame"), NOT to 0 and NOT left alone. Leaving them
        // alone is the documented trap this repo already paid for once: a one-shot pass keeps
        // advertising its old cost forever and reads as per-frame work. But clearing alone was
        // its own bug -- readers landing between the clear and the frame's cpu_end calls saw
        // -1 everywhere, which is how --capture-metrics came to report a dead CPU column for
        // every region. Publish first, then clear, and both properties hold at once.
        for i in 0..REGION_COUNT {
            self.cpu_ms_latest[i].set(self.cpu_ms[i].get());
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

    /// Record an already-measured CPU duration for `region`, accumulating like `cpu_end`. For
    /// spans measured before this frame's `begin_frame` (the swapchain acquire).
    pub fn cpu_record(&self, region: Region, ms: f32) {
        let i = region as usize;
        let prev = self.cpu_ms[i].get();
        self.cpu_ms[i].set(if prev < 0.0 { ms } else { prev + ms });
    }

    /// Copy the latest per-region CPU encode ms into `out` (-1 = not recorded this frame).
    /// Always available — unlike the GPU rows this needs no adapter feature.
    pub fn cpu_timings(&self, out: &mut [f32]) -> u32 {
        for (dst, src) in out.iter_mut().zip(self.cpu_ms_latest.iter()) {
            *dst = src.get();
        }
        REGION_COUNT as u32
    }

    /// Open a region bracket. Must be called OUTSIDE any render/compute pass (the
    /// timestamp is written on the encoder itself).
    ///
    /// A REGION OPENED SEVERAL TIMES IN ONE FRAME REPORTS THE LAST ONE, NOT THE SUM.
    /// Each region owns one query pair, so a second bracket overwrites the first: the
    /// harvest reads that pair and cannot know it was written more than once. Contrast
    /// `cpu_end`, which deliberately ACCUMULATES so a region recorded in several places
    /// reports one honest per-frame figure -- the two halves of this file do not agree,
    /// and only the CPU half says so at the call site.
    ///
    /// `GrassShadow` is the live example: `draw_shadow` is called once per shadow cascade,
    /// so the row reports one cascade's grass draw and not the four. Reading it as a total
    /// understates grass shadowing by the cascade count, which is a trap worth knowing
    /// before quoting the number (2026-08-31: nearly quoted it here).
    pub fn begin(&self, encoder: &mut wgpu::CommandEncoder, region: Region) {
        // PERF-022: the CPU clock rides every bracket, so the worker's encode time is
        // attributed by the same rows as the GPU time. cpu_end accumulates, so a region
        // bracketed several times reports its per-frame sum on the CPU side (the GPU side
        // keeps the last bracket -- see the note above).
        self.cpu_begin(region);
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
                .set(inner.written.get() | (1u128 << region as u32));
        }
        self.cpu_end(region);
    }

    /// Open a region bracket from INSIDE a render pass. No-op unless the adapter
    /// offered TIMESTAMP_QUERY_INSIDE_PASSES. Use for draws that share a pass with
    /// other work (grass colour/prepass/shadow ops inside the 3D plan).
    pub fn begin_pass(&self, pass: &mut wgpu::RenderPass<'_>, region: Region) {
        self.cpu_begin(region); // PERF-022, as in `begin`
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
                    .set(inner.written.get() | (1u128 << region as u32));
            }
        }
        self.cpu_end(region);
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
            if written & (1u128 << region) == 0 {
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
                            if written & (1u128 << region) == 0 {
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

    // A GpuTimers with no device: `inner` is None, so every GPU row reads -1 and only the
    // CPU rows are live. That is exactly the surface this test is about, and it needs no
    // adapter -- which matters, because the bug it pins was invisible to every existing
    // test precisely because nothing exercised the read/write interleaving.

    #[test]
    fn copied_render_call_cpu_facts_distinguish_current_latest_and_refused_calls() {
        let t = cpu_only_timers();
        t.begin_frame();
        t.cpu_record(Region::WorkerAcquire, 17.0);
        t.cpu_record(Region::WorkerEncMain, 31.0);
        t.begin_frame(); // Existing getter now publishes the preceding frame.
        t.cpu_record(Region::WorkerAcquire, 2.0);
        t.cpu_record(Region::WorkerEncMain, 5.0);
        let mut latest = [-1.0; REGION_COUNT];
        t.cpu_timings(&mut latest);
        assert_eq!(latest[Region::WorkerAcquire as usize], 17.0);
        let mut facts = RenderCallCpuTimings::new(41);
        assert_eq!((facts.state, facts.count), (0, 0));
        facts.complete(&t);
        assert_eq!((facts.state, facts.count, facts.call_token), (1, 12, 41));
        assert_eq!(facts.milliseconds[0], 2.0);
        assert_eq!(facts.milliseconds[7], 5.0);
        assert_eq!(facts.milliseconds[1], -1.0);
        t.cpu_timings(&mut latest); // New copied API does not republish/change old API.
        assert_eq!(latest[Region::WorkerEncMain as usize], 31.0);
        facts.skip();
        assert_eq!((facts.state, facts.count, facts.call_token), (2, 0, 41));
        assert_eq!(facts.milliseconds, [-1.0; 12]);
        facts.complete(&t); facts.fail();
        assert_eq!((facts.state, facts.count), (3, 0));
        assert_eq!(facts.milliseconds, [-1.0; 12]);
        let next = RenderCallCpuTimings::new(42);
        assert_eq!(next.call_token, 42);
        assert_eq!(next.milliseconds, [-1.0; 12]);
    }

    fn cpu_only_timers() -> GpuTimers {
        GpuTimers {
            inner: None,
            cpu_open: std::array::from_fn(|_| Cell::new(None)),
            cpu_ms: std::array::from_fn(|_| Cell::new(-1.0)),
            cpu_ms_latest: std::array::from_fn(|_| Cell::new(-1.0)),
        }
    }

    // THE BUG: a reader between begin_frame and the frame's cpu_end calls used to see -1
    // for every region, because begin_frame cleared the very array the reader read. In a
    // capture that is a whole dead column -- 58 regions reporting "did not run" while their
    // GPU rows showed real milliseconds.
    #[test]
    fn cpu_rows_report_the_previous_complete_frame_not_a_half_built_one() {
        let t = cpu_only_timers();
        let mut out = [0.0f32; REGION_COUNT];

        // Frame 1: record something into two regions.
        t.begin_frame();
        t.cpu_begin(Region::FftCompose);
        t.cpu_end(Region::FftCompose);
        t.cpu_begin(Region::WaterDraw);
        t.cpu_end(Region::WaterDraw);

        // Read DURING frame 1, before it has been published. The previous frame is empty,
        // so -1 is correct here -- this is the honest "nothing complete yet" answer, not
        // the bug.
        t.cpu_timings(&mut out);
        assert_eq!(out[Region::FftCompose as usize], -1.0);

        // Frame 2 begins: frame 1 is now complete and must be visible IMMEDIATELY, before
        // any of frame 2's regions have been recorded. This is the read that used to fail.
        t.begin_frame();
        t.cpu_timings(&mut out);
        assert!(
            out[Region::FftCompose as usize] >= 0.0,
            "a region recorded in the previous frame must still be readable at the very              start of the next one; got {}",
            out[Region::FftCompose as usize]
        );
        assert!(out[Region::WaterDraw as usize] >= 0.0);

        // ...and a region that did NOT run in frame 1 still reads -1, so publishing the
        // previous frame has not resurrected the stale-value trap it replaced.
        assert_eq!(out[Region::Caustics as usize], -1.0);
    }

    // The other half of the same contract: a region that stops running must stop reporting.
    // This is the trap the GPU rows were fixed for (a skipped planar reflection advertising
    // its old multi-millisecond cost forever) and the fix above must not reintroduce it.
    #[test]
    fn a_region_that_stops_running_stops_reporting_after_one_frame() {
        let t = cpu_only_timers();
        let mut out = [0.0f32; REGION_COUNT];

        t.begin_frame();
        t.cpu_begin(Region::Caustics);
        t.cpu_end(Region::Caustics);

        t.begin_frame(); // publishes the frame that ran Caustics
        t.cpu_timings(&mut out);
        assert!(out[Region::Caustics as usize] >= 0.0);

        t.begin_frame(); // publishes a frame in which it did NOT run
        t.cpu_timings(&mut out);
        assert_eq!(
            out[Region::Caustics as usize], -1.0,
            "a pass that stopped running must read -1, not keep advertising its last cost"
        );
    }

    // The FFI index contract: WgrGpuTimerRegion in wgpu_renderer.hpp mirrors these
    // discriminants and DebugOverlay's name table is ordered by them. Locking the
    // count + a few pinned indices catches accidental reordering on either side.
    #[test]
    fn region_indices_stay_ffi_stable() {
        assert_eq!(REGION_COUNT, 89);
        assert_eq!(Region::WeatherCoverNearCull as u32, 84);
        assert_eq!(Region::WeatherCoverFarCull as u32, 85);
        assert_eq!(Region::WeatherCoverNearDraw as u32, 86);
        assert_eq!(Region::WeatherCoverFarDraw as u32, 87);
        assert_eq!(Region::TwCaustics as u32, 82);
        assert_eq!(Region::TwBreakers as u32, 83);
        assert_eq!(Region::TwOceanFft as u32, 79);
        assert_eq!(Region::TwShoreSim as u32, 80);
        assert_eq!(Region::TwWake as u32, 81);
        assert_eq!(Region::LocalShadow as u32, 77);
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
        assert_eq!(Region::CloudMarch as u32, 48);
        assert_eq!(Region::Ui2d as u32, 57);
        // The atmosphere block, appended for PERF-010.
        assert_eq!(Region::SkyLuts as u32, 58);
        assert_eq!(Region::SkyDraw as u32, 62);
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
        // FAR INSTANCE TIER, appended past the cascade block.
        assert_eq!(Region::FarCull as u32, 46);
        assert_eq!(Region::FarDraw as u32, 47);
        // Every region's begin/end pair fits the query set.
        assert!(QUERY_COUNT as usize == REGION_COUNT * 2);
        // A u64 written mask covers all regions and leaves append-only room.
        assert!(REGION_COUNT <= 128);
    }

    // A disabled timer (adapter without the features) must be a total no-op that
    // still satisfies the FFI contract by reporting 0 regions.
    #[test]
    fn disabled_timers_report_zero_regions() {
        let timers = cpu_only_timers();
        assert!(!timers.enabled());
        let mut out = [0.0f32; REGION_COUNT];
        assert_eq!(timers.timings(&mut out), 0);
    }

    // PERF-005 — the CPU rows must work with `inner: None` (no timestamp-query adapter) and
    // must ACCUMULATE across repeated brackets within one frame.
    //
    // REWRITTEN 2026-08-31, and the rewrite is the point. This test used to assert that the
    // accumulated value is readable mid-frame and reads -1 again after the next begin_frame.
    // Both assertions described the bug: readers see the PREVIOUS COMPLETE frame, so a value
    // is not visible until the frame that recorded it has ended, and it stays visible for the
    // whole of the next frame rather than vanishing at its start. The old contract is why
    // --capture-metrics reported a dead cpu_milliseconds column -- a test agreeing with a bug
    // does not make it a feature.
    #[test]
    fn cpu_rows_accumulate_and_reset_without_a_gpu() {
        let t = cpu_only_timers();
        let mut out = [0.0f32; REGION_COUNT];
        t.begin_frame();
        assert_eq!(t.cpu_timings(&mut out), REGION_COUNT as u32);
        assert_eq!(out[Region::ObjColorSolid as usize], -1.0);
        for _ in 0..2 {
            t.cpu_begin(Region::ObjColorSolid);
            t.cpu_end(Region::ObjColorSolid);
        }
        // Ending the frame publishes it; two closed brackets accumulate into one total.
        t.begin_frame();
        t.cpu_timings(&mut out);
        assert!(out[Region::ObjColorSolid as usize] >= 0.0);
        // A region never bracketed stays "not run" rather than reading 0 ms of real work.
        assert_eq!(out[Region::ObjColorAlpha as usize], -1.0);
        // And the frame AFTER that -- in which it did not run -- clears it back to "not run",
        // so accumulation never turns into a value that outlives the work it measured.
        t.begin_frame();
        t.cpu_timings(&mut out);
        assert_eq!(out[Region::ObjColorSolid as usize], -1.0);
    }
}
