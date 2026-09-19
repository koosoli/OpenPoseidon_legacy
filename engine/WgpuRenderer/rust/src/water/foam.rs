use crate::ffi::WgrWaterInteractionParams;

// Persistent foam covers the 256 m interaction domain at 0.25 m/texel. The interaction
// injection field remains 256² (1 m/texel), avoiding a fourfold event-raster cost.
const FOAM_RESOLUTION: u32 = 1024;

pub struct Foam {
    params: wgpu::Buffer,
    views: [wgpu::TextureView; 2],
    sampler: wgpu::Sampler,
    bind_groups: [[wgpu::BindGroup; 2]; 2],
    pipeline: wgpu::ComputePipeline,
    terrain_layout: wgpu::BindGroupLayout,
    terrain_group: wgpu::BindGroup,
    current: usize,
}

#[cfg(test)]
mod tests {
    use super::FOAM_RESOLUTION;

    #[test]
    fn persistent_history_is_quarter_meter_over_the_interaction_domain() {
        const INTERACTION_DOMAIN_METERS: u32 = 256;
        assert_eq!(FOAM_RESOLUTION, 1024);
        assert_eq!(FOAM_RESOLUTION / INTERACTION_DOMAIN_METERS, 4);
        assert_eq!(FOAM_RESOLUTION / 8, 128);
    }

    #[test]
    fn breaker_coverage_survives_pause_and_small_continuous_injection() {
        fn advance(history: f64, dt: f64, source: f64, legacy: bool) -> f64 {
            let decay = history * (-dt * (5.2 - 2.6 * history.clamp(0.0, 1.0))).exp();
            let raw = 1.0 - (1.0 - decay) * (-source * dt).exp();
            if legacy { raw * raw * (3.0 - 2.0 * raw) } else { raw.clamp(0.0, 1.0) }
        }
        assert!((advance(0.2, 0.0, 1.0, false) - 0.2).abs() < 1e-12);
        assert!(advance(0.2, 0.0, 1.0, true) < 0.11);
        let mut finals = Vec::new();
        for hz in [60, 120, 240] {
            let mut coverage = 0.0;
            let mut legacy = 0.0;
            for _ in 0..hz * 4 {
                coverage = advance(coverage, 1.0 / hz as f64, 1.0, false);
                legacy = advance(legacy, 1.0 / hz as f64, 1.0, true);
            }
            assert!(coverage > 0.15 && coverage < 0.22);
            assert!(legacy < 0.002);
            finals.push(coverage);
            for _ in 0..hz * 4 { coverage = advance(coverage, 1.0 / hz as f64, 0.0, false); }
            assert!(coverage < 1e-6);
        }
        assert!((finals[0] - finals[2]).abs() < 0.01);
        let shader = include_str!("foam.wgsl");
        assert!(shader.contains("select(clamp(breaker_raw, 0.0, 1.0)"));
        assert!(shader.contains("select(1.0 - exp(-25.847 * dt), 0.35"));
    }

    #[test]
    fn breaker_history_shader_composes_with_its_ab_override() {
        let mut composer = crate::shaders::build_composer();
        let module = composer.make_naga_module(naga_oil::compose::NagaModuleDescriptor {
            source: include_str!("foam.wgsl"),
            file_path: "water/foam.wgsl",
            ..Default::default()
        }).expect("foam shader must compose");
        assert!(module.overrides.iter().any(|(_, value)| value.name.as_deref() == Some("breaker_history_legacy")));
    }
}

impl Foam {
    pub fn new(
        device: &wgpu::Device,
        composer: &mut naga_oil::compose::Composer,
        water_params: &wgpu::Buffer,
        interaction_views: &[wgpu::TextureView; 2],
        displacement: &wgpu::TextureView,
        auxiliary: &wgpu::TextureView,
        cascade_configs: &wgpu::Buffer,
        heightmap: &wgpu::TextureView,
        conform: &wgpu::Buffer,
    ) -> Self {
        let texture = |label| {
            device
                .create_texture(&wgpu::TextureDescriptor {
                    label: Some(label),
                    size: wgpu::Extent3d {
                        width: FOAM_RESOLUTION,
                        height: FOAM_RESOLUTION,
                        depth_or_array_layers: 1,
                    },
                    mip_level_count: 1,
                    sample_count: 1,
                    dimension: wgpu::TextureDimension::D2,
                    format: wgpu::TextureFormat::Rgba16Float,
                    usage: wgpu::TextureUsages::TEXTURE_BINDING
                        | wgpu::TextureUsages::STORAGE_BINDING,
                    view_formats: &[],
                })
                .create_view(&Default::default())
        };
        let views = [texture("wgr_water_foam_a"), texture("wgr_water_foam_b")];
        let params = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("wgr_water_foam_params"),
            size: std::mem::size_of::<WgrWaterInteractionParams>() as u64,
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        let sampler = device.create_sampler(&wgpu::SamplerDescriptor {
            label: Some("wgr_water_foam_sampler"),
            mag_filter: wgpu::FilterMode::Linear,
            min_filter: wgpu::FilterMode::Linear,
            address_mode_u: wgpu::AddressMode::ClampToEdge,
            address_mode_v: wgpu::AddressMode::ClampToEdge,
            ..Default::default()
        });
        let layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("wgr_water_foam_layout"),
            entries: &[
                entry(
                    0,
                    wgpu::BindingType::Buffer {
                        ty: wgpu::BufferBindingType::Uniform,
                        has_dynamic_offset: false,
                        min_binding_size: None,
                    },
                ),
                entry(
                    1,
                    wgpu::BindingType::Buffer {
                        ty: wgpu::BufferBindingType::Uniform,
                        has_dynamic_offset: false,
                        min_binding_size: None,
                    },
                ),
                entry(
                    2,
                    wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Float { filterable: true },
                        view_dimension: wgpu::TextureViewDimension::D2,
                        multisampled: false,
                    },
                ),
                entry(
                    3,
                    wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Float { filterable: true },
                        view_dimension: wgpu::TextureViewDimension::D2Array,
                        multisampled: false,
                    },
                ),
                entry(
                    4,
                    wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Float { filterable: true },
                        view_dimension: wgpu::TextureViewDimension::D2Array,
                        multisampled: false,
                    },
                ),
                entry(
                    5,
                    wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Float { filterable: true },
                        view_dimension: wgpu::TextureViewDimension::D2,
                        multisampled: false,
                    },
                ),
                entry(
                    6,
                    wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Filtering),
                ),
                entry(
                    7,
                    wgpu::BindingType::StorageTexture {
                        access: wgpu::StorageTextureAccess::WriteOnly,
                        format: wgpu::TextureFormat::Rgba16Float,
                        view_dimension: wgpu::TextureViewDimension::D2,
                    },
                ),
                entry(
                    8,
                    wgpu::BindingType::Buffer {
                        ty: wgpu::BufferBindingType::Uniform,
                        has_dynamic_offset: false,
                        min_binding_size: None,
                    },
                ),
            ],
        });
        let bind = |previous: usize, next: usize, interaction: usize| {
            device.create_bind_group(&wgpu::BindGroupDescriptor {
                label: Some("wgr_water_foam_bind"),
                layout: &layout,
                entries: &[
                    wgpu::BindGroupEntry {
                        binding: 0,
                        resource: params.as_entire_binding(),
                    },
                    wgpu::BindGroupEntry {
                        binding: 1,
                        resource: water_params.as_entire_binding(),
                    },
                    wgpu::BindGroupEntry {
                        binding: 2,
                        resource: wgpu::BindingResource::TextureView(&views[previous]),
                    },
                    wgpu::BindGroupEntry {
                        binding: 3,
                        resource: wgpu::BindingResource::TextureView(displacement),
                    },
                    wgpu::BindGroupEntry {
                        binding: 4,
                        resource: wgpu::BindingResource::TextureView(auxiliary),
                    },
                    wgpu::BindGroupEntry {
                        binding: 5,
                        resource: wgpu::BindingResource::TextureView(
                            &interaction_views[interaction],
                        ),
                    },
                    wgpu::BindGroupEntry {
                        binding: 6,
                        resource: wgpu::BindingResource::Sampler(&sampler),
                    },
                    wgpu::BindGroupEntry {
                        binding: 7,
                        resource: wgpu::BindingResource::TextureView(&views[next]),
                    },
                    wgpu::BindGroupEntry {
                        binding: 8,
                        resource: cascade_configs.as_entire_binding(),
                    },
                ],
            })
        };
        let bind_groups = [
            [bind(0, 1, 0), bind(0, 1, 1)],
            [bind(1, 0, 0), bind(1, 0, 1)],
        ];
        let shader = crate::shaders::make_module(
            device,
            composer,
            "wgr_water_foam_shader",
            include_str!("foam.wgsl"),
            "water/foam.wgsl",
        );
        let legacy = std::env::var("WGR_BREAKER_HISTORY_LEGACY").ok().as_deref() == Some("1");
        let constants = [("breaker_history_legacy", if legacy { 1.0 } else { 0.0 })];
        let terrain_layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("wgr_foam_coastal_layout"),
            entries: &[
                entry(0, wgpu::BindingType::Texture { sample_type: wgpu::TextureSampleType::Float {filterable: false}, view_dimension: wgpu::TextureViewDimension::D2, multisampled: false }),
                entry(1, wgpu::BindingType::Buffer {ty: wgpu::BufferBindingType::Uniform,has_dynamic_offset:false,min_binding_size:None}),
            ],
        });
        let terrain_group = coastal_group(device, &terrain_layout, heightmap, conform);
        let constants = [constants[0], ("surf_pilot", if std::env::var("WGR_WATER_SURF_PILOT").ok().as_deref() == Some("1") {1.0} else {0.0})];
        let pipeline = device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
            label: Some("wgr_water_foam_pipeline"),
            layout: Some(
                &device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
                    label: Some("wgr_water_foam_pipeline_layout"),
                    bind_group_layouts: &[Some(&layout), Some(&terrain_layout)],
                    immediate_size: 0,
                }),
            ),
            module: &shader,
            entry_point: Some("foam_update"),
            compilation_options: wgpu::PipelineCompilationOptions {
                constants: &constants,
                ..Default::default()
            },
            cache: None,
        });
        Self {
            params,
            views,
            sampler,
            bind_groups,
            pipeline,
            terrain_layout,
            terrain_group,
            current: 0,
        }
    }

    pub fn set_heightmap(&mut self, device: &wgpu::Device, heightmap: &wgpu::TextureView, conform: &wgpu::Buffer) {
        self.terrain_group = coastal_group(device, &self.terrain_layout, heightmap, conform);
    }

    pub fn set_params(&self, queue: &wgpu::Queue, params: WgrWaterInteractionParams) {
        queue.write_buffer(&self.params, 0, bytemuck::bytes_of(&params));
    }

    pub fn dispatch(&mut self, encoder: &mut wgpu::CommandEncoder, interaction_current: usize) {
        let mut pass = encoder.begin_compute_pass(&wgpu::ComputePassDescriptor {
            label: Some("wgr_water_foam"),
            timestamp_writes: None,
        });
        pass.set_pipeline(&self.pipeline);
        pass.set_bind_group(0, &self.bind_groups[self.current][interaction_current], &[]);
        pass.set_bind_group(1, &self.terrain_group, &[]);
        pass.dispatch_workgroups(FOAM_RESOLUTION / 8, FOAM_RESOLUTION / 8, 1);
        drop(pass);
        self.current = 1 - self.current;
    }

    pub fn view(&self) -> &wgpu::TextureView {
        &self.views[self.current]
    }
    pub fn sampler(&self) -> &wgpu::Sampler {
        &self.sampler
    }
}

fn entry(binding: u32, ty: wgpu::BindingType) -> wgpu::BindGroupLayoutEntry {
    wgpu::BindGroupLayoutEntry {
        binding,
        visibility: wgpu::ShaderStages::COMPUTE,
        ty,
        count: None,
    }
}

fn coastal_group(device: &wgpu::Device, layout: &wgpu::BindGroupLayout, heightmap: &wgpu::TextureView, conform: &wgpu::Buffer) -> wgpu::BindGroup {
    device.create_bind_group(&wgpu::BindGroupDescriptor {label:Some("wgr_foam_coastal_group"),layout,entries:&[
        wgpu::BindGroupEntry {binding:0,resource:wgpu::BindingResource::TextureView(heightmap)},
        wgpu::BindGroupEntry {binding:1,resource:conform.as_entire_binding()},
    ]})
}
