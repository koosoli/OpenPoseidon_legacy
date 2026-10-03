//! TW-WATER W1 — the renderer's water behind one selectable backend.
//!
//! Before this module `lib.rs` owned `Water` and `Underwater` as two fields and reached into
//! both from about thirty call sites. Tidewater Water Plan v5 §4 asks for a second, complete
//! water implementation beside the current one, selectable in the Water tab, with these rules:
//!
//! * exactly ONE backend is alive; the inactive one holds no GPU resources and runs nothing;
//! * switching drops the old backend and builds the new one at a frame boundary;
//! * `Underwater` belongs to the backend (Current OP keeps today's compositor unchanged);
//! * the rest of the renderer reads the active surface through [`SurfaceProduct`], never
//!   through a backend's internals;
//! * Tidewater Native is the default; explicit Current OP remains a selectable legacy backend.
//!
//! The Current OP arm is a pure delegation to the untouched `Water` / `Underwater` types, so
//! its behaviour cannot change by construction; every method below that exists for it is a
//! one-line forward. Everything the C++ side pushes (params, the per-cascade spectrum config,
//! interaction params) is also retained here and replayed into a freshly built backend, because
//! the producer only resends cascade configs when they change and a rebuilt backend would
//! otherwise start from shader defaults.

use crate::ffi::{
    WgrWaterCascadeConfig, WgrWaterInteractionEvent, WgrWaterInteractionParams, WgrWaterNode,
    WgrWaterParams,
};
use crate::underwater::Underwater;
use crate::water::Water;
use crate::water_tw::Tidewater;

/// Which water implementation renders. The discriminants are the ABI values carried in
/// `WgrWaterParams::water_backend` and accepted by `WGR_WATER_BACKEND`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum WaterBackendKind {
    CurrentOp = 0,
    Tidewater = 1,
}

impl WaterBackendKind {
    /// Unknown values fall back to the default rather than failing: an old profile or a newer
    /// engine must never be able to select "no water".
    pub fn from_abi(v: u32) -> Self {
        match v {
            0 => WaterBackendKind::CurrentOp,
            _ => WaterBackendKind::Tidewater,
        }
    }

    pub fn label(self) -> &'static str {
        match self {
            WaterBackendKind::CurrentOp => "Current OP",
            WaterBackendKind::Tidewater => "Tidewater Native",
        }
    }
}

/// `WGR_WATER_BACKEND=0|1`, read once. When set it wins over the Water-tab selection for the
/// whole session, so a harness run is guaranteed to measure the backend it asked for.
pub fn env_override() -> Option<WaterBackendKind> {
    static V: std::sync::OnceLock<Option<WaterBackendKind>> = std::sync::OnceLock::new();
    *V.get_or_init(|| parse_env_override(std::env::var("WGR_WATER_BACKEND").ok().as_deref()))
}

fn parse_env_override(v: Option<&str>) -> Option<WaterBackendKind> {
    match v.map(str::trim) {
        Some("0") => Some(WaterBackendKind::CurrentOp),
        Some("1") => Some(WaterBackendKind::Tidewater),
        _ => None,
    }
}

fn startup_kind(override_kind: Option<WaterBackendKind>, storage_supported: bool) -> WaterBackendKind {
    match override_kind.unwrap_or(WaterBackendKind::Tidewater) {
        WaterBackendKind::Tidewater if storage_supported => WaterBackendKind::Tidewater,
        _ => WaterBackendKind::CurrentOp, // Explicit0 or unsupported storage: safe legacy fallback.
    }
}

/// The read-only, per-frame description of the active water surface (plan §4.2). Everything
/// outside the water code that needs to know about the surface asks this, so a consumer can
/// never depend on one backend's texture layout by accident.
#[derive(Clone, Copy, Debug, PartialEq)]
#[allow(dead_code)] // owns_terrain_wetness / writes_reactive_mask gain consumers in W3/W4
pub struct SurfaceProduct {
    pub kind: WaterBackendKind,
    /// Global ocean plane (not the level of an inland body the camera may be in).
    pub ocean_level: Option<f32>,
    /// `(local surface level under the camera, water clock, eye submersion depth)`.
    pub local: Option<(f32, f32, f32)>,
    /// The backend paints beach wetness itself, so terrain's static wet band must be off.
    pub owns_terrain_wetness: bool,
    /// The backend writes water/lip/spray pixels into the shared reactive history mask.
    pub writes_reactive_mask: bool,
    /// Whether the renderer should record OP's planar reflection pass for this backend.
    pub uses_planar_reflection: bool,
    /// The backend has an underwater compositor that may engage this frame.
    pub has_underwater: bool,
}

/// Today's water, exactly as it was: the FFT/CDLOD surface and the scene-wide underwater
/// compositor it feeds.
pub struct CurrentOp {
    pub water: Water,
    pub underwater: Underwater,
}

enum Active {
    CurrentOp(Box<CurrentOp>),
    Tidewater(Box<Tidewater>),
    /// Only between dropping the old backend and building the new one inside a switch.
    Empty,
}

/// Construction inputs kept so the backend can be rebuilt at a switch.
struct BuildInputs {
    camera_layout: wgpu::BindGroupLayout,
    surface_format: wgpu::TextureFormat,
    sample_count: u32,
    storage_supported: bool,
}

pub struct WaterBackend {
    active: Active,
    inputs: BuildInputs,
    // Replayed into a rebuilt backend (see the module note).
    last_params: Option<WgrWaterParams>,
    cascade_configs: [Option<WgrWaterCascadeConfig>; MAX_REPLAYED_CASCADES],
    interaction_params: Option<WgrWaterInteractionParams>,
    // TW-WATER W3: the CPU heightmap (the shore field is computed from it). Kept for replay: the
    // producer uploads it once per world load / terrain edit, not per frame.
    terrain_heights: Option<(std::sync::Arc<[f32]>, crate::ffi::WgrTerrainParams, u64)>,
    requested: WaterBackendKind,
    refused_logged: Option<WaterBackendKind>,
    switches: u64,
    /// W7h: the backend-switch soak's current pick (`WGR_WATER_SOAK`), ahead of everything else
    soak: Option<WaterBackendKind>,
}

const MAX_REPLAYED_CASCADES: usize = 8;

impl WaterBackend {
    // Construct only the selected startup backend. A saved UI profile may still
    // switch it at the existing frame boundary after the first producer params.
    pub fn new(
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        camera_layout: &wgpu::BindGroupLayout,
        surface_format: wgpu::TextureFormat,
        sample_count: u32,
        composer: &mut naga_oil::compose::Composer,
        storage_supported: bool,
    ) -> Self {
        let kind = startup_kind(env_override(), storage_supported);
        let active = match kind {
            WaterBackendKind::Tidewater => Active::Tidewater(Box::new(Tidewater::new(
                device, queue, camera_layout, surface_format, sample_count,
            ))),
            WaterBackendKind::CurrentOp => Active::CurrentOp(Box::new(CurrentOp {
                water: Water::new(device, queue, camera_layout, surface_format,
                    sample_count, composer, storage_supported),
                underwater: Underwater::new(device, surface_format, composer, storage_supported),
            })),
        };
        Self {
            active,
            inputs: BuildInputs {
                camera_layout: camera_layout.clone(),
                surface_format,
                sample_count,
                storage_supported,
            },
            last_params: None,
            cascade_configs: [None; MAX_REPLAYED_CASCADES],
            interaction_params: None,
            terrain_heights: None,
            requested: WaterBackendKind::Tidewater,
            refused_logged: None,
            switches: 0,
            soak: None,
        }
    }

    pub fn kind(&self) -> WaterBackendKind {
        match self.active {
            Active::Tidewater(_) => WaterBackendKind::Tidewater,
            Active::CurrentOp(_) | Active::Empty => WaterBackendKind::CurrentOp,
        }
    }

    /// Number of completed backend switches this session (the soak test reads it).
    #[allow(dead_code)]
    pub fn switch_count(&self) -> u64 {
        self.switches
    }

    fn op(&self) -> Option<&CurrentOp> {
        match &self.active {
            Active::CurrentOp(op) => Some(op),
            _ => None,
        }
    }

    fn op_mut(&mut self) -> Option<&mut CurrentOp> {
        match &mut self.active {
            Active::CurrentOp(op) => Some(op),
            _ => None,
        }
    }

    /// The Current OP surface, when it is the active backend. For code that is Current-OP
    /// specific by nature (the curling-breaker prototype, the froxel volume).
    #[allow(dead_code)]
    pub fn current_op(&self) -> Option<&CurrentOp> {
        self.op()
    }

    #[allow(dead_code)]
    pub fn tidewater(&self) -> Option<&Tidewater> {
        match &self.active {
            Active::Tidewater(tw) => Some(tw),
            _ => None,
        }
    }

    #[allow(dead_code)]
    pub fn tidewater_mut(&mut self) -> Option<&mut Tidewater> {
        match &mut self.active {
            Active::Tidewater(tw) => Some(tw),
            _ => None,
        }
    }

    // ------------------------------------------------------------------ selection / switching

    /// The selection the backend should converge to: the env override when set, else the
    /// Water tab (via the params lane).
    fn wanted(&self) -> WaterBackendKind {
        self.soak.or(env_override()).unwrap_or(self.requested)
    }

    /// W7h: the backend-switch soak (plan §8): the lib's frame loop flips the backend with this
    /// while `WGR_WATER_SOAK` runs; `None` hands the choice back.
    pub fn set_soak(&mut self, kind: Option<WaterBackendKind>) {
        self.soak = kind;
    }

    /// Called once per frame before anything reads the water: applies a pending selection
    /// change. A one-frame hitch is accepted (plan §4.2); nothing of the old backend survives
    /// the swap because it is dropped before the new one is created.
    pub fn apply_pending_switch(
        &mut self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        log: &crate::log::LogSink,
    ) {
        let want = self.wanted();
        if want == self.kind() {
            return;
        }
        if want == WaterBackendKind::Tidewater && !crate::water_tw::AVAILABLE {
            if self.refused_logged != Some(want) {
                self.refused_logged = Some(want);
                log.log(
                    crate::log::log_level::WARN,
                    "Water backend: Tidewater Native is not ported in this build (W1 infrastructure \
                     only); staying on Current OP",
                );
            }
            return;
        }
        if want == WaterBackendKind::Tidewater && !self.inputs.storage_supported {
            if self.refused_logged != Some(want) {
                self.refused_logged = Some(want);
                log.log(
                    crate::log::log_level::WARN,
                    "Water backend: Tidewater Native needs storage textures, which this adapter \
                     lacks; staying on Current OP",
                );
            }
            return;
        }
        let from = self.kind();
        let t0 = std::time::Instant::now();
        // Drop first: at no point do both backends hold GPU memory.
        crate::water_tw::probe::clear_drawn_grid();
        self.active = Active::Empty;
        self.active = match want {
            WaterBackendKind::CurrentOp => {
                let mut composer = crate::shaders::build_composer();
                let water = Water::new(
                    device,
                    queue,
                    &self.inputs.camera_layout,
                    self.inputs.surface_format,
                    self.inputs.sample_count,
                    &mut composer,
                    self.inputs.storage_supported,
                );
                let underwater = Underwater::new(
                    device,
                    self.inputs.surface_format,
                    &mut composer,
                    self.inputs.storage_supported,
                );
                Active::CurrentOp(Box::new(CurrentOp { water, underwater }))
            }
            WaterBackendKind::Tidewater => Active::Tidewater(Box::new(Tidewater::new(
                device,
                queue,
                &self.inputs.camera_layout,
                self.inputs.surface_format,
                self.inputs.sample_count,
            ))),
        };
        self.replay(device, queue);
        if want == WaterBackendKind::CurrentOp {
            self.terrain_heights = None;
        }
        self.switches += 1;
        self.refused_logged = None;
        log.log(
            crate::log::log_level::INFO,
            &format!(
                "Water backend: {} -> {} in {:.1} ms (switch #{}{})",
                from.label(),
                want.label(),
                t0.elapsed().as_secs_f64() * 1000.0,
                self.switches,
                if env_override().is_some() { ", WGR_WATER_BACKEND override" } else { "" },
            ),
        );
    }

    fn replay(&mut self, device: &wgpu::Device, queue: &wgpu::Queue) {
        let params = self.last_params;
        let cascades = self.cascade_configs;
        let interaction = self.interaction_params;
        match &mut self.active {
            Active::CurrentOp(op) => {
                // Params first, then the cascade configs: a config may rebuild the FFT at
                // another resolution, and that rebuild re-applies the params it has just seen.
                if let Some(p) = params {
                    op.water.set_params(queue, p);
                }
                for (i, c) in cascades.iter().enumerate() {
                    if let Some(c) = c {
                        op.water.set_cascade_config(device, queue, i as u32, *c);
                    }
                }
                if let Some(ip) = interaction {
                    op.water.set_interaction_params(queue, ip);
                }
            }
            Active::Tidewater(tw) => {
                if let Some(p) = params {
                    tw.set_params(queue, p);
                }
                if let Some(ip) = interaction {
                    tw.set_interaction_params(ip);
                }
                if let Some((h, tp, generation)) = &self.terrain_heights {
                    tw.set_terrain_heights(h.clone(), *tp, *generation);
                }
            }
            Active::Empty => {}
        }
    }

    /// Tidewater's wet sand for the terrain (None with Current OP or the shore simulation off).
    pub fn shore_wetness(&self) -> Option<crate::water_tw::ShoreWetness<'_>> {
        match &self.active {
            Active::Tidewater(tw) => tw.shore_wetness(),
            _ => None,
        }
    }

    /// The terrain heightmap as the producer uploaded it (world load, terrain edit). Current OP
    /// does not need the CPU copy; Tidewater computes its shore field from it.
    pub fn needs_terrain_heights(&self) -> bool {
        self.wanted() == WaterBackendKind::Tidewater
            && self.inputs.storage_supported && self.terrain_heights.is_none()
    }

    pub fn set_terrain_heights(&mut self, heights: &[f32], params: crate::ffi::WgrTerrainParams) {
        // No shore replay copy in normal Current OP sessions. A later explicit switch borrows
        // Terrain's existing sky-visibility source and captures it once at that frame boundary.
        if self.wanted() != WaterBackendKind::Tidewater || !self.inputs.storage_supported {
            self.terrain_heights = None;
            return;
        }
        let Some(n) = (params.hm_width as usize).checked_mul(params.hm_height as usize) else {
            return;
        };
        if n == 0 || heights.len() < n {
            return;
        }
        let generation = self.terrain_heights.as_ref().map_or(1, |t| t.2 + 1);
        let arc: std::sync::Arc<[f32]> = std::sync::Arc::from(&heights[..n]);
        if let Active::Tidewater(tw) = &mut self.active {
            tw.set_terrain_heights(arc.clone(), params, generation);
        }
        self.terrain_heights = Some((arc, params, generation));
    }

    // ------------------------------------------------------------------ producer inputs

    pub fn set_params(&mut self, queue: &wgpu::Queue, params: WgrWaterParams) {
        self.requested = WaterBackendKind::from_abi(params.water_backend);
        self.last_params = Some(params);
        match &mut self.active {
            Active::CurrentOp(op) => op.water.set_params(queue, params),
            Active::Tidewater(tw) => tw.set_params(queue, params),
            Active::Empty => {}
        }
    }

    pub fn set_cascade_config(
        &mut self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        index: u32,
        config: WgrWaterCascadeConfig,
    ) {
        if let Some(slot) = self.cascade_configs.get_mut(index as usize) {
            *slot = Some(config);
        }
        if let Some(op) = self.op_mut() {
            op.water.set_cascade_config(device, queue, index, config);
        }
    }

    pub fn set_interaction_params(&mut self, queue: &wgpu::Queue, params: WgrWaterInteractionParams) {
        self.interaction_params = Some(params);
        match &mut self.active {
            Active::CurrentOp(op) => op.water.set_interaction_params(queue, params),
            Active::Tidewater(tw) => tw.set_interaction_params(params),
            Active::Empty => {}
        }
    }

    pub fn submit_interactions(&mut self, queue: &wgpu::Queue, events: &[WgrWaterInteractionEvent]) {
        match &mut self.active {
            Active::CurrentOp(op) => op.water.submit_interactions(queue, events),
            // TW-WATER W8b: explosions and bullets on the water become Tidewater splashes
            Active::Tidewater(tw) => tw.submit_interactions(events),
            Active::Empty => {}
        }
    }

    // ------------------------------------------------------------------ the surface product

    pub fn surface_product(&self) -> SurfaceProduct {
        match &self.active {
            Active::CurrentOp(op) => SurfaceProduct {
                kind: WaterBackendKind::CurrentOp,
                ocean_level: op.water.ocean_level(),
                local: op.water.underwater_params(),
                owns_terrain_wetness: false,
                writes_reactive_mask: true,
                uses_planar_reflection: true,
                has_underwater: true,
            },
            Active::Tidewater(tw) => SurfaceProduct {
                kind: WaterBackendKind::Tidewater,
                ocean_level: tw.ocean_level(),
                local: tw.surface_params(),
                // No Eulerian shore state before W3: terrain keeps its static band.
                owns_terrain_wetness: false,
                writes_reactive_mask: true,
                uses_planar_reflection: false,
                // W5a: Tidewater's own underwater composite (water_tw/underwater.rs)
                has_underwater: true,
            },
            Active::Empty => SurfaceProduct {
                kind: WaterBackendKind::CurrentOp,
                ocean_level: None,
                local: None,
                owns_terrain_wetness: false,
                writes_reactive_mask: false,
                uses_planar_reflection: false,
                has_underwater: false,
            },
        }
    }

    #[allow(dead_code)]
    pub fn ocean_level(&self) -> Option<f32> {
        self.surface_product().ocean_level
    }

    /// `(local surface level, water clock, eye submersion depth)`, see [`SurfaceProduct::local`].
    pub fn underwater_params(&self) -> Option<(f32, f32, f32)> {
        self.surface_product().local
    }

    /// TW-WATER W9a: the drawn surface's height under the camera when the backend measures it
    /// (Tidewater: its probe grid, read back 1-3 frames late). Current OP: None (its eye
    /// submersion comes with [`Self::underwater_params`]).
    pub fn camera_surface_height(&self) -> Option<f32> {
        match &self.active {
            Active::Tidewater(tw) => tw.camera_surface_height(),
            _ => None,
        }
    }

    pub fn low_quality(&self) -> bool {
        match &self.active {
            Active::CurrentOp(op) => op.water.low_quality(),
            _ => false,
        }
    }

    pub fn uses_planar_reflection(&self) -> bool {
        self.surface_product().uses_planar_reflection
    }

    // ------------------------------------------------------------------ underwater (Current OP)

    pub fn underwater_enabled(&self) -> bool {
        match &self.active {
            Active::CurrentOp(op) => op.water.underwater_enabled(),
            // TW-WATER W5a: Tidewater's own underwater composite
            Active::Tidewater(tw) => tw.underwater_enabled(),
            Active::Empty => false,
        }
    }

    pub fn underwater_tuning(&self) -> (f32, f32, f32, f32) {
        self.op()
            .map(|op| op.water.underwater_tuning())
            .unwrap_or((1.5, 1.0, 1.0, 1.0))
    }

    pub fn underwater_body(&self) -> Option<([f32; 3], [f32; 3], f32)> {
        self.op().and_then(|op| op.water.underwater_body())
    }

    pub fn underwater_spectrum(&self) -> ([f32; 4], u32, f32, f32, f32, f32, f32) {
        self.op()
            .map(|op| op.water.underwater_spectrum())
            .unwrap_or(([1.0; 4], 0, 0.0, 0.0, 0.0, 1.0, 1.0))
    }

    pub fn camera_body_ellipse(&self, cam_xz: [f32; 2]) -> ([f32; 4], [f32; 2]) {
        self.op()
            .map(|op| op.water.camera_body_ellipse(cam_xz))
            .unwrap_or(([0.0; 4], [1.0, 0.0]))
    }

    /// The underwater compositor, when the active backend has one.
    pub fn underwater(&self) -> Option<&Underwater> {
        self.op().map(|op| &op.underwater)
    }

    pub fn underwater_fft_views(&self) -> Option<(wgpu::TextureView, wgpu::TextureView)> {
        self.op().map(|op| op.water.underwater_fft_views())
    }

    /// The scene-wide underwater composite. A no-op for a backend without one (the caller only
    /// reaches this when `underwater_time` engaged, which needs `underwater_enabled`).
    pub fn render_underwater(
        &self,
        device: &wgpu::Device,
        encoder: &mut wgpu::CommandEncoder,
        source: &wgpu::TextureView,
        depth: &wgpu::TextureView,
        destination: &wgpu::TextureView,
    ) {
        match &self.active {
            Active::CurrentOp(op) => {
                let displacement = op.water.underwater_displacement_view();
                op.underwater.render(device, encoder, source, depth, destination, &displacement);
            }
            Active::Tidewater(tw) => tw.render_underwater(device, encoder, source, depth, destination),
            Active::Empty => {}
        }
    }

    // ------------------------------------------------------------------ per-frame resources

    pub fn set_depth_view(&mut self, device: &wgpu::Device, depth: &wgpu::TextureView, view_gen: u64) {
        match &mut self.active {
            Active::CurrentOp(op) => op.water.set_depth_view(device, depth, view_gen),
            Active::Tidewater(tw) => tw.set_depth_view(device, depth, view_gen),
            Active::Empty => {}
        }
    }

    pub fn set_env_view(&mut self, device: &wgpu::Device, env: &wgpu::TextureView, view_gen: u64) {
        match &mut self.active {
            Active::CurrentOp(op) => op.water.set_env_view(device, env, view_gen),
            Active::Tidewater(tw) => tw.set_env_view(device, env, view_gen),
            Active::Empty => {}
        }
    }

    pub fn set_scene_view(&mut self, device: &wgpu::Device, scene: &wgpu::TextureView, view_gen: u64) {
        match &mut self.active {
            Active::CurrentOp(op) => op.water.set_scene_view(device, scene, view_gen),
            Active::Tidewater(tw) => tw.set_scene_view(device, scene, view_gen),
            Active::Empty => {}
        }
    }

    pub fn set_planar_view(
        &mut self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        planar: &wgpu::TextureView,
        view_gen: u64,
        full_vp: [f32; 16],
        fade: f32,
    ) {
        if let Some(op) = self.op_mut() {
            op.water
                .set_planar_view(device, queue, planar, view_gen, full_vp, fade);
        }
    }

    pub fn set_planar_fade(&self, queue: &wgpu::Queue, fade: f32) {
        if let Some(op) = self.op() {
            op.water.set_planar_fade(queue, fade);
        }
    }

    pub fn set_heightmap(
        &mut self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        heightmap: &wgpu::TextureView,
        view_gen: u64,
        params: &crate::terrain::TerrainConformParams,
    ) {
        match &mut self.active {
            Active::CurrentOp(op) => op.water.set_heightmap(device, queue, heightmap, view_gen, params),
            Active::Tidewater(tw) => tw.set_heightmap(device, queue, heightmap, view_gen, params),
            Active::Empty => {}
        }
    }

    pub fn prepare(
        &mut self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        nodes: &[WgrWaterNode],
        camera: Option<&crate::ffi::WgrCamera>,
    ) {
        match &mut self.active {
            Active::CurrentOp(op) => op.water.prepare(device, queue, nodes),
            Active::Tidewater(tw) => tw.prepare(device, queue, camera),
            Active::Empty => {}
        }
    }

    /// Per-frame compute that must precede the water draw (Current OP: the ripple field;
    /// Tidewater: the FFT, its mip chain and the sea-detail field).
    pub fn update_interactions(
        &mut self,
        device: &wgpu::Device,
        encoder: &mut wgpu::CommandEncoder,
        timers: &crate::gpu_timers::GpuTimers,
    ) {
        match &mut self.active {
            Active::CurrentOp(op) => op.water.update_interactions(device, encoder, timers),
            Active::Tidewater(tw) => tw.update(encoder, timers),
            Active::Empty => {}
        }
    }

    pub fn has_curling_breaker(&self) -> bool {
        self.op().is_some_and(|op| op.water.has_curling_breaker())
    }

    /// W4b: Tidewater's lips and spray into the upscalers' reactive mask (no-op otherwise).
    pub fn draw_reactive(&self, encoder: &mut wgpu::CommandEncoder, mask: &wgpu::TextureView, camera: &wgpu::BindGroup, offset: u32) {
        if let Some(tw) = self.tidewater() {
            tw.draw_reactive(encoder, mask, camera, offset);
        }
    }

    /// W12a: Tidewater's spray sprites, after the clouds (no-op otherwise; see TwSurface::draw_spray).
    pub fn draw_spray(&self, pass: &mut wgpu::RenderPass<'_>, camera: &wgpu::BindGroup, offset: u32) {
        if let Some(tw) = self.tidewater() {
            tw.draw_spray(pass, camera, offset);
        }
    }

    pub fn has_spray_pass(&self) -> bool {
        self.tidewater().is_some_and(|tw| tw.has_spray_pass())
    }

    pub fn draw_curling(&self, pass: &mut wgpu::RenderPass<'_>, camera: &wgpu::BindGroup, offset: u32) {
        if let Some(op) = self.op() {
            op.water.draw_curling(pass, camera, offset);
        }
    }

    /// One water batch. Current OP draws the producer's CDLOD node range; Tidewater draws its
    /// own camera-centred CDLOD selection once per batch camera (it ignores the OP nodes, whose
    /// 200 m leaf is the Current OP mesh density, plan §5 "keep gridSize 32 / leafSize 8").
    pub fn draw(
        &self,
        pass: &mut wgpu::RenderPass<'_>,
        camera_bind: &wgpu::BindGroup,
        camera_offset: u32,
        first_node: u32,
        node_count: u32,
    ) {
        match &self.active {
            Active::CurrentOp(op) => op
                .water
                .draw(pass, camera_bind, camera_offset, first_node, node_count),
            Active::Tidewater(tw) => {
                if first_node == 0 {
                    tw.draw(pass, camera_bind, camera_offset);
                }
            }
            Active::Empty => {}
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn abi_values_map_to_backends_and_unknown_is_the_default() {
        assert_eq!(WaterBackendKind::from_abi(0), WaterBackendKind::CurrentOp);
        assert_eq!(WaterBackendKind::from_abi(1), WaterBackendKind::Tidewater);
        assert_eq!(WaterBackendKind::from_abi(7), WaterBackendKind::Tidewater);
        assert_eq!(WaterBackendKind::from_abi(u32::MAX), WaterBackendKind::Tidewater);
        assert_eq!(WaterBackendKind::CurrentOp as u32, 0);
        assert_eq!(WaterBackendKind::Tidewater as u32, 1);
    }

    #[test]
    fn env_override_accepts_only_zero_and_one() {
        assert_eq!(parse_env_override(Some("0")), Some(WaterBackendKind::CurrentOp));
        assert_eq!(parse_env_override(Some(" 1 ")), Some(WaterBackendKind::Tidewater));
        assert_eq!(parse_env_override(Some("tidewater")), None);
        assert_eq!(parse_env_override(Some("")), None);
        assert_eq!(parse_env_override(None), None);
    }

    #[test]
    fn startup_defaults_to_tidewater_but_explicit_zero_and_unsupported_storage_use_legacy() {
        assert_eq!(startup_kind(None, true), WaterBackendKind::Tidewater);
        assert_eq!(startup_kind(parse_env_override(Some("1")), true), WaterBackendKind::Tidewater);
        assert_eq!(startup_kind(parse_env_override(Some("0")), true), WaterBackendKind::CurrentOp);
        assert_eq!(startup_kind(None, false), WaterBackendKind::CurrentOp);
        assert_eq!(startup_kind(Some(WaterBackendKind::Tidewater), false), WaterBackendKind::CurrentOp);
        assert_eq!(startup_kind(parse_env_override(Some("invalid")), true), WaterBackendKind::Tidewater);
    }

    #[test]
    fn backend_selector_is_appended_after_the_body_table() {
        // The selector must not shift any lane the WGSL mirrors of WaterParams read.
        let bodies = std::mem::offset_of!(WgrWaterParams, bodies);
        let sel = std::mem::offset_of!(WgrWaterParams, water_backend);
        assert_eq!(bodies, 288);
        assert_eq!(sel, 288 + 16 * 48);
        assert_eq!(std::mem::offset_of!(WgrWaterParams, tidewater), 288 + 16 * 48 + 16);
        assert_eq!(std::mem::offset_of!(WgrWaterParams, tidewater_shore), 288 + 16 * 48 + 16 + 64);
        assert_eq!(std::mem::offset_of!(WgrWaterParams, tidewater_sea), 288 + 16 * 48 + 16 + 64 + 48);
        assert_eq!(std::mem::offset_of!(WgrWaterParams, tidewater_fx), 288 + 16 * 48 + 16 + 64 + 48 + 16);
        assert_eq!(std::mem::offset_of!(WgrWaterParams, tidewater_wake), 288 + 16 * 48 + 16 + 64 + 48 + 16 + 16);
        assert_eq!(std::mem::size_of::<WgrWaterParams>(), 1472);
    }
}
