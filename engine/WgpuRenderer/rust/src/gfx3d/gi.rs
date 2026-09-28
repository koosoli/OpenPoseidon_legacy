// REN-GI-001/002 — the irradiance probe volume (see gi_probes.wgsl for the model).
//
// Owns the probe state (a storage buffer), the sampled 3D texture the fragment shaders read
// (six y-slabs of ambient-cube faces), the parameters uniform, the compute pipeline that
// re-integrates one eighth of the probes per frame, and (stage 2) the SUN PROXY: a reflective
// shadow map -- depth, albedo, normal -- rendered from the sun over the volume's box by the
// GPU-driven depth path, from which the probes gather the first sun bounce with colour.
//
// The volume is a world-anchored grid that follows the camera in whole probe spacings; when it
// moves, the batch integrated that frame starts from scratch and the rest converge over the
// following seven frames. The sun proxy is re-rendered when its view changes (camera moved a
// probe spacing, the sun moved a quarter degree) or the retained set changed.
//
// Bind groups at dispatch: 0 = the camera group (frame UBO + shadow maps + sky SH + dome maps,
// the same one every shader reads, with a dynamic offset for the main camera), 1 = the
// terrain conform group (heightmap + its params), 2 = this module's own.

use super::sky_vis::{self, SkyVisSettings, SkyVisView};

pub const NX: u32 = 32;
pub const NY: u32 = 8;
pub const NZ: u32 = 32;
pub const FACES: u32 = 6;
/// REN-GI-009: how many slabs each probe texture carries. Nine, because second-order SH needs
/// nine coefficients; the ambient cube uses the first six and the distance moments the first
/// six of their own texture. Both textures are cut to the same height so one copy extent covers
/// them, which costs about half a megabyte and removes a class of mistake.
pub const SLABS: u32 = 9;
pub const BATCHES: u32 = 8;
/// Sun proxy resolution (square) and half-extent: 1024 over 96 m is 9.4 cm per texel.
pub const RSM_RES: u32 = 1024;
pub const RSM_EXTENT: f32 = 48.0;

fn rsm_refresh_requested(eligible: bool, view_moved: bool, budget_stale: bool,
                         retry_unpublished: bool) -> bool {
    eligible && (view_moved || budget_stale || retry_unpublished)
}

fn note_closed_rsm_sample(valid: &std::cell::Cell<bool>) {
    valid.set(true);
}

#[test]
fn gi_rsm_keeps_budgeted_refresh_except_after_failed_publication() {
    assert!(!rsm_refresh_requested(true, false, false, false));
    assert!(rsm_refresh_requested(true, false, true, false));
    assert!(rsm_refresh_requested(true, true, false, false));
    assert!(rsm_refresh_requested(true, false, false, true));
    assert!(!rsm_refresh_requested(false, true, true, true));
}

#[test]
fn closed_rsm_stays_sampleable_while_failed_publication_retries() {
    use super::gi_rsm_publication::{GiRsmPublication, GI_CULL_VIEW};
    let sample_valid = std::cell::Cell::new(false);
    let mut publication = GiRsmPublication::default();
    assert!(publication.plan(7));
    note_closed_rsm_sample(&sample_valid);
    publication.cull_recorded(GI_CULL_VIEW);
    publication.final_submit_accepted();
    assert_eq!(publication.commit(7), None); // no indirect GPU depth draw
    assert!(sample_valid.get());
    assert!(publication.needs_retry());
    assert!(rsm_refresh_requested(true, false, false, publication.needs_retry()));
}

#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
struct GiParamsGpu {
    origin: [f32; 4],
    dims: [f32; 4],
    knobs: [f32; 4],
    ground: [f32; 4],
    reset: [f32; 4],
    rsm_vp: [f32; 16],
    rsm_inv_vp: [f32; 16],
    rsm: [f32; 4],
    rsm_meta: [f32; 4],
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct GiSettings {
    pub enabled: bool,
    /// 0 = six-face ambient cube (shipped), 1 = first-order SH. See `basis_from_env`.
    pub basis: u32,
    pub rays: u32,
    pub weight: f32,
    pub interior_mix: f32,
    /// REN-GI-010: how much of a probe's own sky occlusion applies to the LATERAL sky rays.
    /// 0 restores the pre-fix integration exactly (walls occluded nothing sideways, so an
    /// indoor probe was nearly as bright as one outdoors); 1 attenuates a probe's sky by the
    /// dome maps' reach at its own position.
    pub indoor_sky: f32,
    pub spacing: f32,
    pub hysteresis: f32,
    pub ground_albedo: [f32; 3],
    pub ground_gain: f32,
    pub wall_albedo: f32,
    pub ray_length: f32,
    pub rsm_samples: u32,
    pub rsm_radius: f32,
    pub rsm_gain: f32,
}

impl Default for GiSettings {
    fn default() -> Self {
        Self {
            enabled: true,
            basis: 2,
            rays: 24,
            weight: 1.0,
            interior_mix: 0.25,
            indoor_sky: 1.0,
            spacing: 2.5,
            hysteresis: 0.3,
            ground_albedo: [0.22, 0.25, 0.16],
            ground_gain: 1.0,
            wall_albedo: 0.3,
            ray_length: 48.0,
            rsm_samples: 16,
            rsm_radius: 12.0,
            rsm_gain: 1.0,
        }
    }
}

impl GiSettings {
    pub fn apply(&mut self, p: &crate::ffi::WgrGi) {
        self.enabled = p.enabled != 0;
        self.basis = p.basis.min(2);
        self.rays = p.rays.clamp(8, 64);
        self.weight = p.weight.clamp(0.0, 1.0);
        self.interior_mix = p.interior_mix.clamp(0.0, 1.0);
        self.indoor_sky = p.indoor_sky.clamp(0.0, 1.0);
        self.spacing = p.spacing.clamp(1.0, 16.0);
        self.hysteresis = p.hysteresis.clamp(0.05, 1.0);
        self.ground_albedo = p.ground_albedo;
        self.ground_gain = p.ground_gain.max(0.0);
        self.wall_albedo = p.wall_albedo.clamp(0.0, 1.0);
        self.ray_length = p.ray_length.clamp(8.0, 256.0);
        self.rsm_samples = p.rsm_samples.clamp(0, 64);
        self.rsm_radius = p.rsm_radius.clamp(1.0, 40.0);
        self.rsm_gain = p.rsm_gain.max(0.0);
    }
}

/// `WGR_GI_RSM_ISO=1` — see the `rsm_meta` comment. Diagnostic, not a shipping path.
fn rsm_iso() -> bool {
    static ISO: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *ISO.get_or_init(|| std::env::var("WGR_GI_RSM_ISO").map(|v| v == "1").unwrap_or(false))
}

pub struct GiProbes {
    pub settings: GiSettings,
    pub view: wgpu::TextureView,
    tex: wgpu::Texture,
    write_tex: wgpu::Texture,
    storage_view: wgpu::TextureView,
    /// REN-GI-003: the distance moments (mean r, mean r^2) per face, sampled by the shading
    /// for the Chebyshev visibility test. Same sampled/written twin dance as the radiance.
    pub dist_view: wgpu::TextureView,
    dist_tex: wgpu::Texture,
    dist_write_tex: wgpu::Texture,
    dist_storage_view: wgpu::TextureView,
    state_buf: wgpu::Buffer,
    params_buf: wgpu::Buffer,
    bind: wgpu::BindGroup,
    pipeline: wgpu::ComputePipeline,
    origin: glam::Vec3,
    have_origin: bool,
    moved: bool,
    frame: u32,
    /// Bumped when the sampled texture is (re)created, so the camera bind group follows it.
    pub generation: u64,
    // --- the sun proxy (REN-GI-002)
    _rsm_depth: wgpu::Texture,
    pub rsm_depth_view: wgpu::TextureView,
    /// Identity of the depth/albedo/normal RSM allocation. A future resize
    /// must assign a fresh identity when replacing those textures.
    pub rsm_image_generation: u64,
    _rsm_albedo: wgpu::Texture,
    pub rsm_albedo_view: wgpu::TextureView,
    _rsm_normal: wgpu::Texture,
    pub rsm_normal_view: wgpu::TextureView,
    /// This frame's sun-proxy view (the one the params carry), and whether it needs drawing.
    pub rsm_view: SkyVisView,
    /// Cells, not plain fields: the draw site holds `&self` (it is inside the encode phase),
    /// and the proxy must only count as rendered once the draw ACTUALLY ran. Marking it in
    /// `prepare` meant the single draw landed on frame 0, whose world was still empty, and
    /// the proxy then stayed empty for the rest of the run (2026-09-03).
    pub rsm_refresh: std::cell::Cell<bool>,
    pub rsm_eligible: std::cell::Cell<bool>,
    rsm_rendered: std::cell::Cell<Option<(SkyVisView, u64)>>,
    rsm_epoch: std::cell::Cell<u64>,
    rsm_valid: std::cell::Cell<bool>,
}

impl GiProbes {
    pub fn new(
        device: &wgpu::Device,
        composer: &mut naga_oil::compose::Composer,
        camera_layout: &wgpu::BindGroupLayout,
        conform_layout: &wgpu::BindGroupLayout,
    ) -> Self {
        let tex = device.create_texture(&wgpu::TextureDescriptor {
            label: Some("wgr_gi_probes"),
            size: wgpu::Extent3d {
                width: NX,
                height: NY * SLABS,
                depth_or_array_layers: NZ,
            },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D3,
            format: wgpu::TextureFormat::Rgba16Float,
            usage: wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::COPY_DST,
            view_formats: &[],
        });
        let view = tex.create_view(&wgpu::TextureViewDescriptor {
            label: Some("wgr_gi_probes_view"),
            dimension: Some(wgpu::TextureViewDimension::D3),
            ..Default::default()
        });
        // The compute pass writes a twin and the result is copied over: the sampled texture
        // sits in the camera bind group the compute pass itself binds, and wgpu forbids one
        // texture being sampled and storage-written inside the same dispatch. The twin is
        // persistent, so after the first eight frames it holds every probe.
        let write_tex = device.create_texture(&wgpu::TextureDescriptor {
            label: Some("wgr_gi_probes_write"),
            size: wgpu::Extent3d {
                width: NX,
                height: NY * SLABS,
                depth_or_array_layers: NZ,
            },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D3,
            format: wgpu::TextureFormat::Rgba16Float,
            usage: wgpu::TextureUsages::STORAGE_BINDING | wgpu::TextureUsages::COPY_SRC,
            view_formats: &[],
        });
        let storage_view = write_tex.create_view(&wgpu::TextureViewDescriptor {
            label: Some("wgr_gi_probes_storage"),
            dimension: Some(wgpu::TextureViewDimension::D3),
            ..Default::default()
        });
        // REN-GI-003: the moments live in the same 3D shape as the radiance.
        let dist_tex = device.create_texture(&wgpu::TextureDescriptor {
            label: Some("wgr_gi_dist"),
            size: wgpu::Extent3d {
                width: NX,
                height: NY * SLABS,
                depth_or_array_layers: NZ,
            },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D3,
            // Rgba16Float, not Rg16Float: WebGPU core does not allow the two-channel form as a
            // storage texture, and the write twin must be one.
            format: wgpu::TextureFormat::Rgba16Float,
            usage: wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::COPY_DST,
            view_formats: &[],
        });
        let dist_view = dist_tex.create_view(&wgpu::TextureViewDescriptor {
            label: Some("wgr_gi_dist_view"),
            dimension: Some(wgpu::TextureViewDimension::D3),
            ..Default::default()
        });
        let dist_write_tex = device.create_texture(&wgpu::TextureDescriptor {
            label: Some("wgr_gi_dist_write"),
            size: wgpu::Extent3d {
                width: NX,
                height: NY * SLABS,
                depth_or_array_layers: NZ,
            },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D3,
            format: wgpu::TextureFormat::Rgba16Float,
            usage: wgpu::TextureUsages::STORAGE_BINDING | wgpu::TextureUsages::COPY_SRC,
            view_formats: &[],
        });
        let dist_storage_view = dist_write_tex.create_view(&wgpu::TextureViewDescriptor {
            label: Some("wgr_gi_dist_storage"),
            dimension: Some(wgpu::TextureViewDimension::D3),
            ..Default::default()
        });
        let state_buf = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("wgr_gi_state"),
            // 12 vec4 per probe: 6 radiance + 6 moments (REN-GI-003).
            // 15 vec4 per probe: 9 radiance (SH-2, or 6 cube faces) + 6 distance moments.
            size: (NX * NY * NZ * (SLABS + FACES)) as u64 * 16,
            usage: wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        let params_buf = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("wgr_gi_params"),
            size: std::mem::size_of::<GiParamsGpu>() as u64,
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        // The sun proxy targets.
        let rsm_tex = |label: &str, format: wgpu::TextureFormat| {
            device.create_texture(&wgpu::TextureDescriptor {
                label: Some(label),
                size: wgpu::Extent3d {
                    width: RSM_RES,
                    height: RSM_RES,
                    depth_or_array_layers: 1,
                },
                mip_level_count: 1,
                sample_count: 1,
                dimension: wgpu::TextureDimension::D2,
                format,
                usage: wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::TEXTURE_BINDING,
                view_formats: &[],
            })
        };
        static NEXT_RSM_IMAGE: std::sync::atomic::AtomicU64 =
            std::sync::atomic::AtomicU64::new(1);
        let rsm_image_generation = NEXT_RSM_IMAGE.fetch_update(
            std::sync::atomic::Ordering::Relaxed,
            std::sync::atomic::Ordering::Relaxed,
            |next| next.checked_add(1),
        ).unwrap_or(0); // Exhaustion fails closed in the cached COUNT key.
        let rsm_depth = rsm_tex("wgr_gi_rsm_depth", super::SHADOW_FORMAT);
        let rsm_albedo = rsm_tex("wgr_gi_rsm_albedo", wgpu::TextureFormat::Rgba8Unorm);
        let rsm_normal = rsm_tex("wgr_gi_rsm_normal", wgpu::TextureFormat::Rgba8Unorm);
        let rsm_depth_view = rsm_depth.create_view(&Default::default());
        let rsm_albedo_view = rsm_albedo.create_view(&Default::default());
        let rsm_normal_view = rsm_normal.create_view(&Default::default());

        let tex_entry = |binding: u32, sample_type: wgpu::TextureSampleType| wgpu::BindGroupLayoutEntry {
            binding,
            visibility: wgpu::ShaderStages::COMPUTE,
            ty: wgpu::BindingType::Texture {
                sample_type,
                view_dimension: wgpu::TextureViewDimension::D2,
                multisampled: false,
            },
            count: None,
        };
        let layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("wgr_gi_layout"),
            entries: &[
                wgpu::BindGroupLayoutEntry {
                    binding: 0,
                    visibility: wgpu::ShaderStages::COMPUTE,
                    ty: wgpu::BindingType::Buffer {
                        ty: wgpu::BufferBindingType::Uniform,
                        has_dynamic_offset: false,
                        min_binding_size: None,
                    },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 1,
                    visibility: wgpu::ShaderStages::COMPUTE,
                    ty: wgpu::BindingType::Buffer {
                        ty: wgpu::BufferBindingType::Storage { read_only: false },
                        has_dynamic_offset: false,
                        min_binding_size: None,
                    },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 2,
                    visibility: wgpu::ShaderStages::COMPUTE,
                    ty: wgpu::BindingType::StorageTexture {
                        access: wgpu::StorageTextureAccess::WriteOnly,
                        format: wgpu::TextureFormat::Rgba16Float,
                        view_dimension: wgpu::TextureViewDimension::D3,
                    },
                    count: None,
                },
                tex_entry(3, wgpu::TextureSampleType::Depth),
                tex_entry(4, wgpu::TextureSampleType::Float { filterable: true }),
                tex_entry(5, wgpu::TextureSampleType::Float { filterable: true }),
                wgpu::BindGroupLayoutEntry {
                    binding: 6,
                    visibility: wgpu::ShaderStages::COMPUTE,
                    ty: wgpu::BindingType::StorageTexture {
                        access: wgpu::StorageTextureAccess::WriteOnly,
                        format: wgpu::TextureFormat::Rgba16Float,
                        view_dimension: wgpu::TextureViewDimension::D3,
                    },
                    count: None,
                },
            ],
        });
        let bind = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("wgr_gi_bind"),
            layout: &layout,
            entries: &[
                wgpu::BindGroupEntry {
                    binding: 0,
                    resource: params_buf.as_entire_binding(),
                },
                wgpu::BindGroupEntry {
                    binding: 1,
                    resource: state_buf.as_entire_binding(),
                },
                wgpu::BindGroupEntry {
                    binding: 2,
                    resource: wgpu::BindingResource::TextureView(&storage_view),
                },
                wgpu::BindGroupEntry {
                    binding: 3,
                    resource: wgpu::BindingResource::TextureView(&rsm_depth_view),
                },
                wgpu::BindGroupEntry {
                    binding: 4,
                    resource: wgpu::BindingResource::TextureView(&rsm_albedo_view),
                },
                wgpu::BindGroupEntry {
                    binding: 5,
                    resource: wgpu::BindingResource::TextureView(&rsm_normal_view),
                },
                wgpu::BindGroupEntry {
                    binding: 6,
                    resource: wgpu::BindingResource::TextureView(&dist_storage_view),
                },
            ],
        });
        let module = crate::shaders::make_module(
            device,
            composer,
            "wgr_gi_probes",
            include_str!("gi_probes.wgsl"),
            "gfx3d/gi_probes.wgsl",
        );
        let pl = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("wgr_gi_probes"),
            bind_group_layouts: &[Some(camera_layout), Some(conform_layout), Some(&layout)],
            immediate_size: 0,
        });
        let pipeline = device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
            label: Some("wgr_gi_probes"),
            layout: Some(&pl),
            module: &module,
            entry_point: Some("cs_gi_probes"),
            compilation_options: Default::default(),
            cache: None,
        });
        Self {
            settings: GiSettings::default(),
            view,
            tex,
            write_tex,
            storage_view,
            dist_view,
            dist_tex,
            dist_write_tex,
            dist_storage_view,
            state_buf,
            params_buf,
            bind,
            pipeline,
            origin: glam::Vec3::ZERO,
            have_origin: false,
            moved: false,
            frame: 0,
            generation: 1,
            _rsm_depth: rsm_depth,
            rsm_depth_view,
            rsm_image_generation,
            _rsm_albedo: rsm_albedo,
            rsm_albedo_view,
            _rsm_normal: rsm_normal,
            rsm_normal_view,
            rsm_view: sky_vis::build_view(glam::Vec3::ZERO, &SkyVisSettings::default()),
            rsm_refresh: std::cell::Cell::new(false),
            rsm_eligible: std::cell::Cell::new(false),
            rsm_rendered: std::cell::Cell::new(None),
            rsm_epoch: std::cell::Cell::new(0),
            rsm_valid: std::cell::Cell::new(false),
        }
    }

    /// The volume's world origin for a camera position: the camera sits at probe column
    /// (NX/2, 2, NZ/2), snapped to whole spacings so the grid is world-anchored.
    fn origin_for(cam: glam::Vec3, spacing: f32) -> glam::Vec3 {
        let snap = |v: f32| (v / spacing).floor() * spacing;
        glam::Vec3::new(
            snap(cam.x) - (NX / 2) as f32 * spacing,
            snap(cam.y) - 2.0 * spacing,
            snap(cam.z) - (NZ / 2) as f32 * spacing,
        )
    }

    /// Per frame, before the camera UBO is written: settle the origin, decide whether the sun
    /// proxy needs re-rendering, upload the params. `to_sun` is the unit direction toward the
    /// sun; `epoch` the retained instance set's change counter. Returns (origin, spacing).
    pub fn prepare(
        &mut self,
        queue: &wgpu::Queue,
        cam_pos: glam::Vec3,
        to_sun: glam::Vec3,
        epoch: u64,
        retry_unpublished: bool,
    ) -> (glam::Vec3, f32) {
        let s = self.settings;
        let origin = Self::origin_for(cam_pos, s.spacing);
        self.moved = !self.have_origin || origin != self.origin;
        self.origin = origin;
        self.have_origin = true;

        // The sun proxy's view: over the volume's centre, snapped to the probe spacing so it
        // holds still while the camera walks inside a cell; the sun direction quantised so a
        // slowly moving sun re-renders about once a quarter degree.
        let centre = origin + glam::Vec3::new((NX / 2) as f32, 2.0, (NZ / 2) as f32) * s.spacing;
        let q = 256.0;
        let sun_q = glam::Vec3::new(
            (to_sun.x * q).round() / q,
            (to_sun.y * q).round() / q,
            (to_sun.z * q).round() / q,
        )
        .normalize_or_zero();
        let rsm_ok = s.rsm_samples > 0 && sun_q.y > 0.02 && s.enabled;
        self.rsm_eligible.set(rsm_ok);
        let mut rsm_settings = SkyVisSettings::default();
        rsm_settings.resolution = RSM_RES;
        rsm_settings.extent = RSM_EXTENT;
        rsm_settings.height = 300.0;
        let dir = if rsm_ok { sun_q } else { glam::Vec3::Y };
        let view = sky_vis::build_view_for(centre, dir, &rsm_settings);
        self.rsm_view = view;
        self.rsm_epoch.set(epoch);
        // BUDGETED, not event-driven. A moving instance bumps the epoch, so on a scene with
        // walking soldiers "the retained set changed" is true every frame and the proxy would
        // re-render a 1024^2 depth+albedo+normal pass every frame. It re-renders when its VIEW
        // moved (the camera left the probe cell, or the sun turned a quarter degree), and
        // otherwise at most once every RSM_PERIOD frames to pick the world's changes up.
        const RSM_PERIOD: u32 = 8;
        let view_moved = match self.rsm_rendered.get() {
            Some((v, _)) => v.view_proj != view.view_proj,
            None => true,
        };
        let stale = match self.rsm_rendered.get() {
            Some((_, e)) => e != epoch && self.frame % RSM_PERIOD == 0,
            None => true,
        };
        self.rsm_refresh.set(rsm_refresh_requested(rsm_ok, view_moved, stale, retry_unpublished));
        if !rsm_ok {
            self.rsm_valid.set(false);
        }

        let inv = view.view_proj.inverse();
        let params = GiParamsGpu {
            origin: [origin.x, origin.y, origin.z, s.spacing],
            dims: [NX as f32, NY as f32, NZ as f32, (self.frame % BATCHES) as f32],
            knobs: [s.hysteresis, s.ground_gain, s.wall_albedo, s.rays as f32],
            ground: [s.ground_albedo[0], s.ground_albedo[1], s.ground_albedo[2], s.ray_length],
            reset: [
                if self.moved { 1.0 } else { 0.0 },
                // REN-GI-010: the indoor sky occlusion knob, read once per probe in cs_gi_probes.
                s.indoor_sky,
                0.0,
                0.0,
            ],
            rsm_vp: view.view_proj.to_cols_array(),
            rsm_inv_vp: inv.to_cols_array(),
            rsm: [
                s.rsm_radius,
                s.rsm_samples as f32,
                s.rsm_gain,
                if self.rsm_valid.get() { 1.0 } else { 0.0 },
            ],
            // .z is REN-GI-007's ablation: 1 spreads the sun bounce isotropically over the
            // ambient cube instead of by the direction it arrived from. It exists to answer
            // whether the bounce is ABSENT on a vertical surface or merely stored in a face a
            // vertical surface cannot read. Diagnostic only, off unless WGR_GI_RSM_ISO=1.
            rsm_meta: [
                2.0 * RSM_EXTENT / RSM_RES as f32,
                RSM_RES as f32,
                if rsm_iso() { 1.0 } else { 0.0 },
                self.settings.basis as f32,
            ],
        };
        queue.write_buffer(&self.params_buf, 0, bytemuck::bytes_of(&params));
        (origin, s.spacing)
    }

    /// Called by the draw site once the sun-proxy pass has actually been recorded.
    pub fn mark_rsm_drawn(&self) {
        self.rsm_rendered
            .set(Some((self.rsm_view, self.rsm_epoch.get())));
        self.rsm_valid.set(true);
        self.rsm_refresh.set(false);
    }

    /// Shader sample availability is separate from the optional cache publication
    /// proof. A closed clear-only pass remains sampleable as in the ordinary path,
    /// while the opt-in proof may still refuse to certify its cached contents.
    pub fn mark_rsm_sample_ready(&self) {
        note_closed_rsm_sample(&self.rsm_valid);
    }

    pub fn dispatch(
        &mut self,
        encoder: &mut wgpu::CommandEncoder,
        camera_bind: &wgpu::BindGroup,
        cam_off: u32,
        conform_bind: &wgpu::BindGroup,
        timers: &crate::gpu_timers::GpuTimers,
    ) {
        if !self.settings.enabled {
            return;
        }
        timers.begin(encoder, crate::gpu_timers::Region::GiProbes);
        encoder.push_debug_group("wgr_gi_probes");
        {
            let mut pass = encoder.begin_compute_pass(&wgpu::ComputePassDescriptor {
                label: Some("wgr_gi_probes"),
                timestamp_writes: None,
            });
            pass.set_pipeline(&self.pipeline);
            pass.set_bind_group(0, camera_bind, &[cam_off]);
            pass.set_bind_group(1, conform_bind, &[]);
            pass.set_bind_group(2, &self.bind, &[]);
            let per_batch = (NX * NY * NZ).div_ceil(BATCHES);
            pass.dispatch_workgroups(per_batch.div_ceil(64), 1, 1);
        }
        let whole = wgpu::Extent3d {
            width: NX,
            height: NY * SLABS,
            depth_or_array_layers: NZ,
        };
        encoder.copy_texture_to_texture(
            self.write_tex.as_image_copy(),
            self.tex.as_image_copy(),
            whole,
        );
        encoder.copy_texture_to_texture(
            self.dist_write_tex.as_image_copy(),
            self.dist_tex.as_image_copy(),
            whole,
        );
        encoder.pop_debug_group();
        timers.end(encoder, crate::gpu_timers::Region::GiProbes);
        self.frame = self.frame.wrapping_add(1);
    }
}

#[cfg(test)]
mod tests {
    // The probe compute shader through the real composer and a real device: naga's backend
    // runs at pipeline creation, which is where "Expression [n] is not cached" and its kin
    // surface -- a deploy used to be the first place that happened (2026-09-03).
    #[test]
    fn gi_probe_pipeline_builds_on_a_real_device() {
        let Some((device, _queue)) = crate::gfx3d::cull::tests::headless() else {
            return;
        };
        let mut composer = crate::shaders::build_composer();
        let cameras = super::super::CameraGroup::new(&device);
        let conform = super::super::ConformGroup::new(&device);
        let gi = super::GiProbes::new(&device, &mut composer, &cameras.layout, &conform.layout);
        assert_eq!(gi.generation, 1);
        assert!(gi.settings.enabled);
    }

    // REN-GI-003, the test that actually bites: the OBJECT shader, whose shade() calls the
    // probe gather. A toy fragment entry over frame.wgsl is not enough -- the same gather
    // compiled there and still brought the renderer down inside shader3d.
    #[test]
    fn gi_gather_survives_the_object_shader_backend() {
        let Some((device, _queue)) = headless_five_groups() else {
            return;
        };
        let mut composer = crate::shaders::build_composer();
        let module = crate::shaders::make_module(
            &device,
            &mut composer,
            "wgr_shader3d_gi_test",
            include_str!("shader3d.wgsl"),
            "gfx3d/shader3d.wgsl",
        );
        let attrs = wgpu::vertex_attr_array![
            0 => Float32x3, 1 => Float32x3, 2 => Float32x2, 5 => Uint32, 6 => Float32x3,
            7 => Float32x3
        ];
        let vbuffers = [wgpu::VertexBufferLayout {
            array_stride: super::super::BAKED_VERT_SIZE,
            step_mode: wgpu::VertexStepMode::Vertex,
            attributes: &attrs,
        }];
        let _ = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
            label: Some("wgr_shader3d_gi_test"),
            layout: None,
            vertex: wgpu::VertexState {
                module: &module,
                entry_point: Some("vs_main"),
                compilation_options: Default::default(),
                buffers: &vbuffers,
            },
            primitive: wgpu::PrimitiveState::default(),
            depth_stencil: None,
            multisample: wgpu::MultisampleState::default(),
            fragment: Some(wgpu::FragmentState {
                module: &module,
                entry_point: Some("fs_main"),
                compilation_options: Default::default(),
                targets: &[Some(wgpu::ColorTargetState {
                    format: wgpu::TextureFormat::Rgba16Float,
                    blend: None,
                    write_mask: wgpu::ColorWrites::ALL,
                })],
            }),
            multiview_mask: None,
            cache: None,
        });
    }

    // REN-GI-003: the probe GATHER through naga's backend. The compute test above imports
    // frame.wgsl but never calls gi_irradiance, so its code is stripped and a backend fault in
    // it escapes to the deploy -- which is exactly what happened.
    #[test]
    fn gi_gather_survives_the_naga_backend() {
        let Some((device, _queue)) = crate::gfx3d::cull::tests::headless() else {
            return;
        };
        let mut composer = crate::shaders::build_composer();
        let module = crate::shaders::make_module(
            &device,
            &mut composer,
            "wgr_gi_probe_test",
            include_str!("../shaders/gi_probe_test.wgsl"),
            "shaders/gi_probe_test.wgsl",
        );
        let _ = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
            label: Some("wgr_gi_probe_test"),
            layout: None,
            vertex: wgpu::VertexState {
                module: &module,
                entry_point: Some("vs_gi_probe_test"),
                compilation_options: Default::default(),
                buffers: &[],
            },
            primitive: wgpu::PrimitiveState::default(),
            depth_stencil: None,
            multisample: wgpu::MultisampleState::default(),
            fragment: Some(wgpu::FragmentState {
                module: &module,
                entry_point: Some("fs_gi_probe_test"),
                compilation_options: Default::default(),
                targets: &[Some(wgpu::ColorTargetState {
                    format: wgpu::TextureFormat::Rgba8Unorm,
                    blend: None,
                    write_mask: wgpu::ColorWrites::ALL,
                })],
            }),
            multiview_mask: None,
            cache: None,
        });
    }

    // REN-GI-002: the sun proxy's fragment/vertex pair through naga's BACKEND. The layouts the
    // real pipeline uses are the renderer's (textures need a C++ LogSink, so they cannot be
    // built here); `layout: None` derives one from the shader instead, which is enough to run
    // the codegen where "Expression [n] is not cached" and its kin appear.
    // The shadow module reaches bind group 4 (the terrain conform group), so the shared
    // headless device (default limits, 4 groups) refuses its shader outright. This one asks
    // for the 5 the renderer asks for.
    fn headless_five_groups() -> Option<(wgpu::Device, wgpu::Queue)> {
        let instance =
            wgpu::Instance::new(wgpu::InstanceDescriptor::new_without_display_handle_from_env());
        let adapter = pollster::block_on(instance.request_adapter(&wgpu::RequestAdapterOptions {
            power_preference: wgpu::PowerPreference::default(),
            compatible_surface: None,
            force_fallback_adapter: false,
        }))
        .ok()?;
        // ...and the binding-array features the module's bindless textures/samplers need.
        let bindless = wgpu::Features::TEXTURE_BINDING_ARRAY
            | wgpu::Features::SAMPLED_TEXTURE_AND_STORAGE_BUFFER_ARRAY_NON_UNIFORM_INDEXING;
        if !adapter.features().contains(bindless) {
            return None;
        }
        pollster::block_on(adapter.request_device(&wgpu::DeviceDescriptor {
            required_features: bindless,
            required_limits: wgpu::Limits {
                max_bind_groups: 5,
                max_binding_array_elements_per_shader_stage: 128,
                max_binding_array_sampler_elements_per_shader_stage: 8,
                ..Default::default()
            },
            ..Default::default()
        }))
        .ok()
    }

    #[test]
    fn gi_sun_proxy_shader_survives_the_naga_backend() {
        let Some((device, _queue)) = headless_five_groups() else {
            return;
        };
        let mut composer = crate::shaders::build_composer();
        let module = crate::shaders::make_module(
            &device,
            &mut composer,
            "wgr_gpu_driven_rsm_test",
            include_str!("gpu_driven_shadow.wgsl"),
            "gfx3d/gpu_driven_shadow.wgsl",
        );
        let attrs =
            wgpu::vertex_attr_array![0 => Float32x3, 1 => Float32x3, 2 => Float32x2, 5 => Uint32];
        let vbuffers = [wgpu::VertexBufferLayout {
            array_stride: super::super::BAKED_VERT_SIZE,
            step_mode: wgpu::VertexStepMode::Vertex,
            attributes: &attrs,
        }];
        let target = |format: wgpu::TextureFormat| {
            Some(wgpu::ColorTargetState {
                format,
                blend: None,
                write_mask: wgpu::ColorWrites::ALL,
            })
        };
        let _ = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
            label: Some("wgr_gi_rsm_test"),
            layout: None,
            vertex: wgpu::VertexState {
                module: &module,
                entry_point: Some("vs_gpu_rsm"),
                compilation_options: Default::default(),
                buffers: &vbuffers,
            },
            primitive: wgpu::PrimitiveState::default(),
            depth_stencil: Some(wgpu::DepthStencilState {
                format: super::super::SHADOW_FORMAT,
                depth_write_enabled: Some(true),
                depth_compare: Some(wgpu::CompareFunction::LessEqual),
                stencil: wgpu::StencilState::default(),
                bias: wgpu::DepthBiasState::default(),
            }),
            multisample: wgpu::MultisampleState::default(),
            fragment: Some(wgpu::FragmentState {
                module: &module,
                entry_point: Some("fs_gpu_rsm"),
                compilation_options: Default::default(),
                targets: &[
                    target(wgpu::TextureFormat::Rgba8Unorm),
                    target(wgpu::TextureFormat::Rgba8Unorm),
                ],
            }),
            multiview_mask: None,
            cache: None,
        });
    }
}
