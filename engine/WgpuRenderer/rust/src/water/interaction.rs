use crate::ffi::{MAX_WATER_INTERACTIONS, WgrWaterInteractionEvent, WgrWaterInteractionParams};

pub struct Interaction {
    params: wgpu::Buffer,
    events: wgpu::Buffer,
    views: [wgpu::TextureView; 2],
    sampler: wgpu::Sampler,
    bind_groups: [wgpu::BindGroup; 2],
    pipeline: wgpu::ComputePipeline,
    current: usize,
    active_events: Vec<WgrWaterInteractionEvent>,
    latest_params: WgrWaterInteractionParams,
}
impl Interaction {
    pub fn new(device: &wgpu::Device, composer: &mut naga_oil::compose::Composer) -> Self {
        let texture = |label| {
            device
                .create_texture(&wgpu::TextureDescriptor {
                    label: Some(label),
                    size: wgpu::Extent3d {
                        width: 256,
                        height: 256,
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
        let views = [
            texture("wgr_water_interaction_a"),
            texture("wgr_water_interaction_b"),
        ];
        let params = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("wgr_water_interaction_params"),
            size: std::mem::size_of::<WgrWaterInteractionParams>() as u64,
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        let events = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("wgr_water_interaction_events"),
            size: (MAX_WATER_INTERACTIONS * std::mem::size_of::<WgrWaterInteractionEvent>()) as u64,
            usage: wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        let sampler = device.create_sampler(&wgpu::SamplerDescriptor {
            mag_filter: wgpu::FilterMode::Linear,
            min_filter: wgpu::FilterMode::Linear,
            address_mode_u: wgpu::AddressMode::ClampToEdge,
            address_mode_v: wgpu::AddressMode::ClampToEdge,
            ..Default::default()
        });
        let layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("wgr_water_interaction_layout"),
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
                        ty: wgpu::BufferBindingType::Storage { read_only: true },
                        has_dynamic_offset: false,
                        min_binding_size: None,
                    },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 2,
                    visibility: wgpu::ShaderStages::COMPUTE,
                    ty: wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Float { filterable: true },
                        view_dimension: wgpu::TextureViewDimension::D2,
                        multisampled: false,
                    },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 3,
                    visibility: wgpu::ShaderStages::COMPUTE,
                    ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Filtering),
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 4,
                    visibility: wgpu::ShaderStages::COMPUTE,
                    ty: wgpu::BindingType::StorageTexture {
                        access: wgpu::StorageTextureAccess::WriteOnly,
                        format: wgpu::TextureFormat::Rgba16Float,
                        view_dimension: wgpu::TextureViewDimension::D2,
                    },
                    count: None,
                },
            ],
        });
        let bind = |previous: usize, next: usize| {
            device.create_bind_group(&wgpu::BindGroupDescriptor {
                label: Some("wgr_water_interaction_bind"),
                layout: &layout,
                entries: &[
                    wgpu::BindGroupEntry {
                        binding: 0,
                        resource: params.as_entire_binding(),
                    },
                    wgpu::BindGroupEntry {
                        binding: 1,
                        resource: events.as_entire_binding(),
                    },
                    wgpu::BindGroupEntry {
                        binding: 2,
                        resource: wgpu::BindingResource::TextureView(&views[previous]),
                    },
                    wgpu::BindGroupEntry {
                        binding: 3,
                        resource: wgpu::BindingResource::Sampler(&sampler),
                    },
                    wgpu::BindGroupEntry {
                        binding: 4,
                        resource: wgpu::BindingResource::TextureView(&views[next]),
                    },
                ],
            })
        };
        let shader = crate::shaders::make_module(
            device,
            composer,
            "wgr_water_interaction_shader",
            include_str!("interaction.wgsl"),
            "water/interaction.wgsl",
        );
        let pipeline = device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
            label: Some("wgr_water_interaction_pipeline"),
            layout: Some(
                &device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
                    label: Some("wgr_water_interaction_pipeline_layout"),
                    bind_group_layouts: &[Some(&layout)],
                    immediate_size: 0,
                }),
            ),
            module: &shader,
            entry_point: Some("interaction_update"),
            compilation_options: Default::default(),
            cache: crate::pipeline_cache::get(&device),
        });
        let bind_groups = [bind(0, 1), bind(1, 0)];
        Self {
            params,
            events,
            views,
            sampler,
            bind_groups,
            pipeline,
            current: 0,
            active_events: Vec::with_capacity(MAX_WATER_INTERACTIONS),
            latest_params: bytemuck::Zeroable::zeroed(),
        }
    }
    pub fn set_params(&mut self, queue: &wgpu::Queue, params: WgrWaterInteractionParams) {
        self.latest_params = params;
        self.expire_events();
        self.upload_params(queue);
    }
    pub fn submit(&mut self, queue: &wgpu::Queue, events: &[WgrWaterInteractionEvent]) {
        let now = self.latest_params.misc[1];
        self.expire_events();
        for event in events.iter().take(MAX_WATER_INTERACTIONS) {
            let mut event = *event;
            if event.time_life_foam_mass[0] <= 0.0 {
                event.time_life_foam_mass[0] = now;
            }
            // Continuous wakes with an explicit id are refreshed independently rather than
            // merging nearby swimmers or vehicles into one ring entry.
            if event.velocity_kind[3] == 5.0 {
                if let Some(existing) = self.active_events.iter_mut().find(|existing| {
                    existing.velocity_kind[3] == 5.0
                        && event.time_life_foam_mass[3] != 0.0
                        && existing.time_life_foam_mass[3] == event.time_life_foam_mass[3]
                }) {
                    *existing = event;
                    continue;
                }
            }
            if self.active_events.len() == MAX_WATER_INTERACTIONS {
                self.active_events.remove(0);
            }
            self.active_events.push(event);
        }
        if !self.active_events.is_empty() {
            queue.write_buffer(&self.events, 0, bytemuck::cast_slice(&self.active_events));
        }
        self.upload_params(queue);
    }
    fn expire_events(&mut self) {
        let now = self.latest_params.misc[1];
        self.active_events.retain(|event| {
            let kind = event.velocity_kind[3];
            let is_continuous = kind == 5.0; // WGR_WATER_INTERACTION_CONTINUOUS
            let lifetime = event.time_life_foam_mass[1];
            if is_continuous {
                lifetime > 0.0 && now - event.time_life_foam_mass[0] <= lifetime
            } else {
                // One-shot impulses (bullet, explosion, footstep, object) are processed on the single frame of submission
                now <= event.time_life_foam_mass[0]
            }
        });
    }
    fn upload_params(&mut self, queue: &wgpu::Queue) {
        self.latest_params.grid[2] = self.active_events.len() as f32;
        queue.write_buffer(&self.params, 0, bytemuck::bytes_of(&self.latest_params));
    }
    pub fn dispatch(&mut self, encoder: &mut wgpu::CommandEncoder) {
        let mut pass = encoder.begin_compute_pass(&wgpu::ComputePassDescriptor {
            label: Some("wgr_water_interactions"),
            timestamp_writes: None,
        });
        pass.set_pipeline(&self.pipeline);
        pass.set_bind_group(0, &self.bind_groups[self.current], &[]);
        pass.dispatch_workgroups(32, 32, 1);
        drop(pass);
        self.current = 1 - self.current;
    }
    pub fn view(&self) -> &wgpu::TextureView {
        &self.views[self.current]
    }
    pub fn views(&self) -> &[wgpu::TextureView; 2] {
        &self.views
    }
    pub fn current(&self) -> usize {
        self.current
    }
    pub fn sampler(&self) -> &wgpu::Sampler {
        &self.sampler
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn event(start: f32, lifetime: f32) -> WgrWaterInteractionEvent {
        WgrWaterInteractionEvent {
            position_radius: [0.0; 4],
            velocity_kind: [0.0; 4],
            time_life_foam_mass: [start, lifetime, 0.0, 0.0],
            direction_depth_flags: [0.0; 4],
        }
    }

    #[test]
    fn event_abi_keeps_the_fixed_ring_capacity() {
        assert_eq!(MAX_WATER_INTERACTIONS, 48);
        assert_eq!(std::mem::size_of::<WgrWaterInteractionEvent>(), 64);
    }

    #[test]
    fn event_lifetime_is_exclusive_after_expiry() {
        let active = [event(4.0, 0.5), event(4.0, 1.0)];
        let retained: Vec<_> = active
            .into_iter()
            .filter(|event| 5.0 - event.time_life_foam_mass[0] <= event.time_life_foam_mass[1])
            .collect();
        assert_eq!(retained.len(), 1);
    }

    #[test]
    fn kelvin_wake_is_vessel_only_and_trails_the_stern() {
        let shader = include_str!("interaction.wgsl");
        assert!(shader.contains("let wake_behind"));
        assert!(shader.contains("(flags & (1u << 12u)) != 0u"));
        assert!(shader.contains("continuous * large_body * kelvin_wedge"));
    }
}
