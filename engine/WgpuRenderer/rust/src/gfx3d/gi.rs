// REN-GI-001 — the irradiance probe volume (see gi_probes.wgsl for the model).
//
// Owns the probe state (a storage buffer), the sampled 3D texture the fragment shaders read
// (six y-slabs of ambient-cube faces), the parameters uniform, and the compute pipeline that
// re-integrates one eighth of the probes per frame. The volume is a world-anchored grid that
// follows the camera in whole probe spacings; when it moves, the batch integrated that frame
// starts from scratch and the rest converge over the following seven frames.
//
// Bind groups at dispatch: 0 = the camera group (frame UBO + shadow maps + sky SH + dome maps,
// the same one every shader reads, with a dynamic offset for the main camera), 1 = the
// terrain conform group (heightmap + its params), 2 = this module's own.

pub const NX: u32 = 32;
pub const NY: u32 = 8;
pub const NZ: u32 = 32;
pub const FACES: u32 = 6;
pub const BATCHES: u32 = 8;

#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
struct GiParamsGpu {
    origin: [f32; 4],
    dims: [f32; 4],
    knobs: [f32; 4],
    ground: [f32; 4],
    reset: [f32; 4],
}

#[derive(Clone, Copy, Debug)]
pub struct GiSettings {
    pub enabled: bool,
    pub debug: bool,
    pub rays: u32,
    pub weight: f32,
    pub interior_mix: f32,
    pub spacing: f32,
    pub hysteresis: f32,
    pub ground_albedo: [f32; 3],
    pub ground_gain: f32,
    pub wall_albedo: f32,
    pub ray_length: f32,
}

impl Default for GiSettings {
    fn default() -> Self {
        Self {
            enabled: true,
            debug: false,
            rays: 24,
            weight: 1.0,
            interior_mix: 0.5,
            spacing: 4.0,
            hysteresis: 0.3,
            ground_albedo: [0.22, 0.25, 0.16],
            ground_gain: 1.0,
            wall_albedo: 0.3,
            ray_length: 48.0,
        }
    }
}

impl GiSettings {
    pub fn apply(&mut self, p: &crate::ffi::WgrGi) {
        self.enabled = p.enabled != 0;
        self.debug = p.debug != 0;
        self.rays = p.rays.clamp(8, 64);
        self.weight = p.weight.clamp(0.0, 1.0);
        self.interior_mix = p.interior_mix.clamp(0.0, 1.0);
        self.spacing = p.spacing.clamp(1.0, 16.0);
        self.hysteresis = p.hysteresis.clamp(0.05, 1.0);
        self.ground_albedo = p.ground_albedo;
        self.ground_gain = p.ground_gain.max(0.0);
        self.wall_albedo = p.wall_albedo.clamp(0.0, 1.0);
        self.ray_length = p.ray_length.clamp(8.0, 256.0);
    }
}

pub struct GiProbes {
    pub settings: GiSettings,
    pub view: wgpu::TextureView,
    tex: wgpu::Texture,
    write_tex: wgpu::Texture,
    storage_view: wgpu::TextureView,
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
                height: NY * FACES,
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
                height: NY * FACES,
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
        let state_buf = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("wgr_gi_state"),
            size: (NX * NY * NZ * FACES) as u64 * 16,
            usage: wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        let params_buf = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("wgr_gi_params"),
            size: std::mem::size_of::<GiParamsGpu>() as u64,
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
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
            state_buf,
            params_buf,
            bind,
            pipeline,
            origin: glam::Vec3::ZERO,
            have_origin: false,
            moved: false,
            frame: 0,
            generation: 1,
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

    /// Per frame, before the camera UBO is written: settle the origin and upload the params.
    /// Returns (origin, spacing) for the frame lanes.
    pub fn prepare(&mut self, queue: &wgpu::Queue, cam_pos: glam::Vec3) -> (glam::Vec3, f32) {
        let s = self.settings;
        let origin = Self::origin_for(cam_pos, s.spacing);
        self.moved = !self.have_origin || origin != self.origin;
        self.origin = origin;
        self.have_origin = true;
        let params = GiParamsGpu {
            origin: [origin.x, origin.y, origin.z, s.spacing],
            dims: [NX as f32, NY as f32, NZ as f32, (self.frame % BATCHES) as f32],
            knobs: [s.hysteresis, s.ground_gain, s.wall_albedo, s.rays as f32],
            ground: [s.ground_albedo[0], s.ground_albedo[1], s.ground_albedo[2], s.ray_length],
            reset: [if self.moved { 1.0 } else { 0.0 }, 0.0, 0.0, 0.0],
        };
        queue.write_buffer(&self.params_buf, 0, bytemuck::bytes_of(&params));
        (origin, s.spacing)
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
        encoder.copy_texture_to_texture(
            self.write_tex.as_image_copy(),
            self.tex.as_image_copy(),
            wgpu::Extent3d {
                width: NX,
                height: NY * FACES,
                depth_or_array_layers: NZ,
            },
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
}
