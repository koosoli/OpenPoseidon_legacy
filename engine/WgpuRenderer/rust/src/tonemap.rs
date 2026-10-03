// The HDR tonemap resolve pass: a fullscreen triangle that samples the offscreen
// HDR scene target and writes the swapchain (exposure -> curve -> optional sRGB
// encode). See docs/hdr-pipeline-plan.md. The pipeline is only constructed when the
// HDR path is enabled; GL33 and the LDR-direct wgpu path never touch it.

use wgpu::util::DeviceExt;

use crate::ffi::WgrTonemap;

pub struct Tonemap {
    pipeline: wgpu::RenderPipeline,
    layout: wgpu::BindGroupLayout,
    sampler: wgpu::Sampler,
    bloom_sampler: wgpu::Sampler,
    params_buf: wgpu::Buffer,
    // Rebuilt whenever the HDR source view is (re)created (allocation / resize).
    bind: Option<wgpu::BindGroup>,
    probe: Option<crate::post_optics_probe::Gpu>,
    probe_source: Option<wgpu::TextureView>,
    probe_scale: Option<wgpu::TextureView>,
    probe_epoch: u64,
}

impl Tonemap {
    pub fn new(device: &wgpu::Device, swapchain_format: wgpu::TextureFormat) -> Self {
        let shader = device.create_shader_module(wgpu::ShaderModuleDescriptor {
            label: Some("wgr_tonemap_shader"),
            source: wgpu::ShaderSource::Wgsl(include_str!("tonemap.wgsl").into()),
        });

        let layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("wgr_tonemap_layout"),
            entries: &[
                wgpu::BindGroupLayoutEntry {
                    binding: 0,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Float { filterable: false },
                        view_dimension: wgpu::TextureViewDimension::D2,
                        multisampled: false,
                    },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 1,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::NonFiltering),
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 2,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Buffer {
                        ty: wgpu::BufferBindingType::Uniform,
                        has_dynamic_offset: false,
                        min_binding_size: None,
                    },
                    count: None,
                },
                // Bloom pyramid mip0 + a linear sampler for the bilinear upscale.
                wgpu::BindGroupLayoutEntry {
                    binding: 3,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Float { filterable: true },
                        view_dimension: wgpu::TextureViewDimension::D2,
                        multisampled: false,
                    },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 4,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Filtering),
                    count: None,
                },
                // 1x1 auto-exposure scale (textureLoad, unfilterable R32Float).
                wgpu::BindGroupLayoutEntry {
                    binding: 5,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Float { filterable: false },
                        view_dimension: wgpu::TextureViewDimension::D2,
                        multisampled: false,
                    },
                    count: None,
                },
            ],
        });

        let pipeline_layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("wgr_tonemap_pipeline_layout"),
            bind_group_layouts: &[Some(&layout)],
            immediate_size: 0,
        });

        // Highlight roll-to-white knobs (see tonemap.wgsl). Pushed as pipeline constants so the
        // shader keeps a no-op default and the feature costs one compare when disabled.
        //   WGR_TONEMAP_DESAT="<strength>"           -> start 0.75
        //   WGR_TONEMAP_DESAT="<start>,<strength>"
        // Absent or unparseable = off, i.e. byte-identical output to today.
        let (desat_start, desat_strength) = match std::env::var("WGR_TONEMAP_DESAT") {
            Ok(v) => {
                let mut it = v.split(',').filter_map(|p| p.trim().parse::<f64>().ok());
                match (it.next(), it.next()) {
                    (Some(a), Some(b)) => (a, b),
                    (Some(a), None) => (0.75, a),
                    _ => (0.75, 0.0),
                }
            }
            Err(_) => (0.75, 0.0),
        };
        // Hable linear-white point (see `linear_white` in tonemap.wgsl). WGR_TONEMAP_WHITE=<w>.
        let linear_white = std::env::var("WGR_TONEMAP_WHITE")
            .ok()
            .and_then(|v| v.trim().parse::<f64>().ok())
            .filter(|w| *w >= 1.0)
            .unwrap_or(11.2);
        let constants = [
            ("desat_start", desat_start.clamp(0.0, 0.99)),
            ("desat_strength", desat_strength.clamp(0.0, 1.0)),
            ("linear_white", linear_white),
        ];

        let pipeline = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
            label: Some("wgr_tonemap_pipeline"),
            layout: Some(&pipeline_layout),
            vertex: wgpu::VertexState {
                module: &shader,
                entry_point: Some("vs_main"),
                buffers: &[],
                compilation_options: Default::default(),
            },
            primitive: wgpu::PrimitiveState::default(),
            depth_stencil: None,
            multisample: wgpu::MultisampleState::default(),
            fragment: Some(wgpu::FragmentState {
                module: &shader,
                entry_point: Some("fs_main"),
                targets: &[Some(wgpu::ColorTargetState {
                    format: swapchain_format,
                    blend: None,
                    write_mask: wgpu::ColorWrites::ALL,
                })],
                compilation_options: wgpu::PipelineCompilationOptions {
                    constants: &constants,
                    ..Default::default()
                },
            }),
            multiview_mask: None,
            cache: None,
        });

        // 1:1 point sample of the HDR target (same resolution as the swapchain).
        let sampler = device.create_sampler(&wgpu::SamplerDescriptor {
            label: Some("wgr_tonemap_sampler"),
            mag_filter: wgpu::FilterMode::Nearest,
            min_filter: wgpu::FilterMode::Nearest,
            ..Default::default()
        });
        // Bilinear for upsampling the half-res bloom pyramid.
        let bloom_sampler = device.create_sampler(&wgpu::SamplerDescriptor {
            label: Some("wgr_tonemap_bloom_sampler"),
            address_mode_u: wgpu::AddressMode::ClampToEdge,
            address_mode_v: wgpu::AddressMode::ClampToEdge,
            mag_filter: wgpu::FilterMode::Linear,
            min_filter: wgpu::FilterMode::Linear,
            ..Default::default()
        });

        // WgrTonemap followed by one renderer-side vec4 (`cave` in tonemap.wgsl).
        let mut init = bytemuck::bytes_of(&WgrTonemap::default()).to_vec();
        init.extend_from_slice(&[0u8; 16]);
        let params_buf = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
            label: Some("wgr_tonemap_params"),
            contents: &init,
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
        });

        let probe = crate::post_optics_probe::enabled()
            .then(|| crate::post_optics_probe::Gpu::new(device, &shader, &layout, &constants));
        Self {
            pipeline,
            layout,
            sampler,
            bloom_sampler,
            params_buf,
            bind: None,
            probe,
            probe_source: None,
            probe_scale: None,
            probe_epoch: 0,
        }
    }

    // Point the resolve at a (re)created HDR target view + the bloom pyramid mip0.
    // Called from ensure_hdr after the HDR target and bloom pyramid are (re)built.
    pub fn set_source(
        &mut self,
        device: &wgpu::Device,
        hdr_view: &wgpu::TextureView,
        bloom_view: &wgpu::TextureView,
        exposure_view: &wgpu::TextureView,
    ) {
        if self.probe.is_some() {
            self.probe_source = Some(hdr_view.clone());
            self.probe_scale = Some(exposure_view.clone());
            self.probe_epoch = self.probe_epoch.checked_add(1).unwrap_or(u64::MAX);
        }
        self.bind = Some(device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("wgr_tonemap_bind"),
            layout: &self.layout,
            entries: &[
                wgpu::BindGroupEntry {
                    binding: 0,
                    resource: wgpu::BindingResource::TextureView(hdr_view),
                },
                wgpu::BindGroupEntry {
                    binding: 1,
                    resource: wgpu::BindingResource::Sampler(&self.sampler),
                },
                wgpu::BindGroupEntry {
                    binding: 2,
                    resource: self.params_buf.as_entire_binding(),
                },
                wgpu::BindGroupEntry {
                    binding: 3,
                    resource: wgpu::BindingResource::TextureView(bloom_view),
                },
                wgpu::BindGroupEntry {
                    binding: 4,
                    resource: wgpu::BindingResource::Sampler(&self.bloom_sampler),
                },
                wgpu::BindGroupEntry {
                    binding: 5,
                    resource: wgpu::BindingResource::TextureView(exposure_view),
                },
            ],
        }));
    }

    pub fn probe(
        &self,
        device: &wgpu::Device,
        encoder: &mut wgpu::CommandEncoder,
        exposure: &crate::exposure::Exposure,
        state: &mut crate::post_optics_probe::State,
        size: (u32, u32),
    ) -> Result<(), &'static str> {
        if state.attempted {
            return Err("duplicate tone resolve");
        }
        state.attempted = true;
        if !state.valid {
            return Err("overlapping/exhausted request");
        }
        let cameras = state.cameras.ok_or("invalid main cameras")?;
        if self.probe_epoch == u64::MAX {
            return Err("source serial exhausted");
        }
        let (source, meter) = exposure
            .probe_source_meter()
            .ok_or("missing current meter")?;
        if self.probe_source.as_ref() != Some(source)
            || self.probe_scale.as_ref() != Some(exposure.scale_view())
        {
            return Err("stale exposure/tone source");
        }
        let input = crate::post_optics_probe::input(
            size,
            state.request,
            state.frame,
            self.probe_epoch,
            std::env::var("WGR_SCREENSHOT_POST_OPTICS_TILES")
                .ok()
                .as_deref(),
        )?;
        state.pending = Some(self.probe.as_ref().ok_or("probe disabled")?.encode(
            device,
            encoder,
            self.bind.as_ref().ok_or("missing tone bind")?,
            meter,
            exposure.probe_params(),
            input,
            cameras,
        ));
        Ok(())
    }

    pub fn upload_params(&self, queue: &wgpu::Queue, params: &WgrTonemap, underground: f32) {
        queue.write_buffer(&self.params_buf, 0, bytemuck::bytes_of(params));
        // Sinkhole W1b: despeckle with the camera underground at all (WGR_CAVE_DESPECKLE=0 off).
        static DESPECKLE: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
        let on = *DESPECKLE.get_or_init(|| std::env::var("WGR_CAVE_DESPECKLE").map(|v| v != "0").unwrap_or(true));
        let cave: [f32; 4] = [if on && underground > 0.05 { 1.0 } else { 0.0 }, 0.0, 0.0, 0.0];
        queue.write_buffer(&self.params_buf, std::mem::size_of::<WgrTonemap>() as u64, bytemuck::bytes_of(&cave));
    }

    // Draw the fullscreen resolve into an already-begun render pass targeting the
    // swapchain. No-op until a source view has been set.
    pub fn render(&self, pass: &mut wgpu::RenderPass<'_>) {
        let Some(bind) = self.bind.as_ref() else {
            return;
        };
        pass.set_pipeline(&self.pipeline);
        pass.set_bind_group(0, bind, &[]);
        pass.draw(0..3, 0..1);
    }
}
