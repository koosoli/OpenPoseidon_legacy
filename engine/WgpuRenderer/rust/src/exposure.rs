// Eye adaptation / auto-exposure (see exposure.wgsl). A luminance pyramid reduces the
// HDR scene to a 1x1 average log-luminance; the adapt pass eases a persistent exposure
// scale toward key/avgLuminance and the tonemap multiplies its exposure by it. Built
// only on the HDR path. The pyramid resizes with the scene; the 1x1 scale textures are
// stable so the tonemap binds a fixed view. Disabled by default (eases to 1.0).

use wgpu::util::DeviceExt;

use crate::ffi::WgrExposure;

// Rg16Float: R = weighted log-luminance, G = metering weight (see exposure.wgsl).
const LUM_FORMAT: wgpu::TextureFormat = wgpu::TextureFormat::Rg16Float;
const SCALE_FORMAT: wgpu::TextureFormat = wgpu::TextureFormat::R32Float;

#[test]
fn exposure_wgsl_validates() {
    let module =
        naga::front::wgsl::parse_str(include_str!("exposure.wgsl")).expect("exposure.wgsl parse");
    naga::valid::Validator::new(
        naga::valid::ValidationFlags::all(),
        naga::valid::Capabilities::all(),
    )
    .validate(&module)
    .expect("exposure.wgsl validate");
}

pub struct Exposure {
    lum_first: wgpu::RenderPipeline,
    lum_down: wgpu::RenderPipeline,
    adapt: wgpu::RenderPipeline,
    reduce_layout: wgpu::BindGroupLayout,
    adapt_layout: wgpu::BindGroupLayout,
    sampler: wgpu::Sampler,
    params_buf: wgpu::Buffer,
    // 1x1 R32Float ping: adapt writes `scratch`, then we copy it into the stable
    // `current` (which the tonemap binds and the next adapt reads as history).
    scratch: wgpu::Texture,
    scratch_view: wgpu::TextureView,
    current: wgpu::Texture,
    current_view: wgpu::TextureView,
    // Per-resize luminance pyramid.
    mip_views: Vec<wgpu::TextureView>,
    reduce_binds: Vec<wgpu::BindGroup>,
    adapt_bind: Option<wgpu::BindGroup>,
    mip_count: usize,
    probe_source: Option<wgpu::TextureView>,
}

impl Exposure {
    pub fn new(device: &wgpu::Device, queue: &wgpu::Queue) -> Self {
        let shader = device.create_shader_module(wgpu::ShaderModuleDescriptor {
            label: Some("wgr_exposure_shader"),
            source: wgpu::ShaderSource::Wgsl(include_str!("exposure.wgsl").into()),
        });

        let tex_entry = |binding: u32| wgpu::BindGroupLayoutEntry {
            binding,
            visibility: wgpu::ShaderStages::FRAGMENT,
            ty: wgpu::BindingType::Texture {
                sample_type: wgpu::TextureSampleType::Float { filterable: true },
                view_dimension: wgpu::TextureViewDimension::D2,
                multisampled: false,
            },
            count: None,
        };
        // Reduction: source texture + linear sampler + params (the first step reads
        // sky_weight for spatial metering; fs_lum_down ignores it but shares the layout).
        let reduce_layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("wgr_exposure_reduce_layout"),
            entries: &[
                tex_entry(0),
                wgpu::BindGroupLayoutEntry {
                    binding: 1,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Filtering),
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
            ],
        });
        // Adapt: 1x1 avg-luminance + 1x1 previous scale (both textureLoad, unfilterable
        // R32Float for the prev) + params.
        let adapt_layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("wgr_exposure_adapt_layout"),
            entries: &[
                tex_entry(0),
                wgpu::BindGroupLayoutEntry {
                    binding: 1,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Float { filterable: false },
                        view_dimension: wgpu::TextureViewDimension::D2,
                        multisampled: false,
                    },
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
            ],
        });

        let make =
            |label: &str, layout: &wgpu::BindGroupLayout, fs: &str, format: wgpu::TextureFormat| {
                let pl = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
                    label: Some(label),
                    bind_group_layouts: &[Some(layout)],
                    immediate_size: 0,
                });
                device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
                    label: Some(label),
                    layout: Some(&pl),
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
                        entry_point: Some(fs),
                        targets: &[Some(wgpu::ColorTargetState {
                            format,
                            blend: None,
                            write_mask: wgpu::ColorWrites::ALL,
                        })],
                        compilation_options: Default::default(),
                    }),
                    multiview_mask: None,
                    cache: None,
                })
            };

        let lum_first = make(
            "wgr_exposure_first",
            &reduce_layout,
            "fs_lum_first",
            LUM_FORMAT,
        );
        let lum_down = make(
            "wgr_exposure_down",
            &reduce_layout,
            "fs_lum_down",
            LUM_FORMAT,
        );
        let adapt = make(
            "wgr_exposure_adapt",
            &adapt_layout,
            "fs_adapt",
            SCALE_FORMAT,
        );

        let sampler = device.create_sampler(&wgpu::SamplerDescriptor {
            label: Some("wgr_exposure_sampler"),
            address_mode_u: wgpu::AddressMode::ClampToEdge,
            address_mode_v: wgpu::AddressMode::ClampToEdge,
            mag_filter: wgpu::FilterMode::Linear,
            min_filter: wgpu::FilterMode::Linear,
            ..Default::default()
        });

        let params_buf = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
            label: Some("wgr_exposure_params"),
            contents: bytemuck::bytes_of(&WgrExposure::default()),
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
        });

        let make_scale = |label: &str, usage: wgpu::TextureUsages| {
            let tex = device.create_texture(&wgpu::TextureDescriptor {
                label: Some(label),
                size: wgpu::Extent3d {
                    width: 1,
                    height: 1,
                    depth_or_array_layers: 1,
                },
                mip_level_count: 1,
                sample_count: 1,
                dimension: wgpu::TextureDimension::D2,
                format: SCALE_FORMAT,
                usage,
                view_formats: &[],
            });
            let view = tex.create_view(&wgpu::TextureViewDescriptor::default());
            (tex, view)
        };
        let (scratch, scratch_view) = make_scale(
            "wgr_exposure_scratch",
            wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::COPY_SRC,
        );
        let (current, current_view) = make_scale(
            "wgr_exposure_current",
            wgpu::TextureUsages::TEXTURE_BINDING
                | wgpu::TextureUsages::COPY_DST
                | wgpu::TextureUsages::COPY_SRC,
        );
        // Seed the persistent scale to 1.0 (neutral) so the first frames are stable.
        queue.write_texture(
            current.as_image_copy(),
            bytemuck::bytes_of(&1.0f32),
            wgpu::TexelCopyBufferLayout {
                offset: 0,
                bytes_per_row: Some(4),
                rows_per_image: Some(1),
            },
            wgpu::Extent3d {
                width: 1,
                height: 1,
                depth_or_array_layers: 1,
            },
        );

        Self {
            lum_first,
            lum_down,
            adapt,
            reduce_layout,
            adapt_layout,
            sampler,
            params_buf,
            scratch,
            scratch_view,
            current,
            current_view,
            mip_views: Vec::new(),
            reduce_binds: Vec::new(),
            adapt_bind: None,
            mip_count: 0,
            probe_source: None,
        }
    }

    // Build the luminance pyramid (full chain to 1x1) for the scene size, and the
    // reduction + adapt bind groups. `scene_view` is the linear HDR target.
    pub fn resize(
        &mut self,
        device: &wgpu::Device,
        width: u32,
        height: u32,
        scene_view: &wgpu::TextureView,
    ) {
        let base_w = (width / 2).max(1);
        let base_h = (height / 2).max(1);
        // Full mip chain so the last mip is exactly 1x1: floor(log2(max))+1.
        let mip_count = (32 - base_w.max(base_h).leading_zeros()).max(1) as usize;

        let texture = device.create_texture(&wgpu::TextureDescriptor {
            label: Some("wgr_exposure_lum"),
            size: wgpu::Extent3d {
                width: base_w,
                height: base_h,
                depth_or_array_layers: 1,
            },
            mip_level_count: mip_count as u32,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: LUM_FORMAT,
            usage: wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::TEXTURE_BINDING,
            view_formats: &[],
        });

        let mip_views: Vec<wgpu::TextureView> = (0..mip_count)
            .map(|i| {
                texture.create_view(&wgpu::TextureViewDescriptor {
                    label: Some("wgr_exposure_lum_mip"),
                    base_mip_level: i as u32,
                    mip_level_count: Some(1),
                    dimension: Some(wgpu::TextureViewDimension::D2),
                    ..Default::default()
                })
            })
            .collect();

        let reduce_binds: Vec<wgpu::BindGroup> = (0..mip_count)
            .map(|i| {
                let src = if i == 0 {
                    scene_view
                } else {
                    &mip_views[i - 1]
                };
                device.create_bind_group(&wgpu::BindGroupDescriptor {
                    label: Some("wgr_exposure_reduce_bind"),
                    layout: &self.reduce_layout,
                    entries: &[
                        wgpu::BindGroupEntry {
                            binding: 0,
                            resource: wgpu::BindingResource::TextureView(src),
                        },
                        wgpu::BindGroupEntry {
                            binding: 1,
                            resource: wgpu::BindingResource::Sampler(&self.sampler),
                        },
                        wgpu::BindGroupEntry {
                            binding: 2,
                            resource: self.params_buf.as_entire_binding(),
                        },
                    ],
                })
            })
            .collect();

        let adapt_bind = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("wgr_exposure_adapt_bind"),
            layout: &self.adapt_layout,
            entries: &[
                wgpu::BindGroupEntry {
                    binding: 0,
                    resource: wgpu::BindingResource::TextureView(&mip_views[mip_count - 1]),
                },
                wgpu::BindGroupEntry {
                    binding: 1,
                    resource: wgpu::BindingResource::TextureView(&self.current_view),
                },
                wgpu::BindGroupEntry {
                    binding: 2,
                    resource: self.params_buf.as_entire_binding(),
                },
            ],
        });

        self.mip_views = mip_views;
        self.reduce_binds = reduce_binds;
        self.adapt_bind = Some(adapt_bind);
        self.mip_count = mip_count;
        if crate::post_optics_probe::enabled() {
            self.probe_source = Some(scene_view.clone());
        }
    }

    pub fn upload_params(&self, queue: &wgpu::Queue, params: &WgrExposure) {
        queue.write_buffer(&self.params_buf, 0, bytemuck::bytes_of(params));
    }

    // The underwater compositor substitutes its scratch target for the finished scene.
    pub fn set_source(&mut self, device: &wgpu::Device, scene_view: &wgpu::TextureView) {
        if self.reduce_binds.is_empty() {
            return;
        }
        if crate::post_optics_probe::enabled() {
            self.probe_source = Some(scene_view.clone());
        }
        self.reduce_binds[0] = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("wgr_exposure_scene_bind"),
            layout: &self.reduce_layout,
            entries: &[
                wgpu::BindGroupEntry {
                    binding: 0,
                    resource: wgpu::BindingResource::TextureView(scene_view),
                },
                wgpu::BindGroupEntry {
                    binding: 1,
                    resource: wgpu::BindingResource::Sampler(&self.sampler),
                },
                wgpu::BindGroupEntry {
                    binding: 2,
                    resource: self.params_buf.as_entire_binding(),
                },
            ],
        });
    }

    // The stable 1x1 exposure-scale view the tonemap samples.
    // REN-TEMP-001M: the 1x1 R32Float exposure-scale TEXTURE, for native (NGX) interop.
    pub fn scale_texture(&self) -> &wgpu::Texture {
        &self.current
    }

    pub fn scale_view(&self) -> &wgpu::TextureView {
        &self.current_view
    }

    // Debug: read back the current 1x1 exposure scale (blocking GPU sync — call only
    // from the dev panel). Returns the last published scale, or 1.0 on failure.
    pub fn read_scale(&self, device: &wgpu::Device, queue: &wgpu::Queue) -> f32 {
        let buf = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("wgr_exposure_readback"),
            size: 256,
            usage: wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::MAP_READ,
            mapped_at_creation: false,
        });
        let mut encoder = device.create_command_encoder(&wgpu::CommandEncoderDescriptor {
            label: Some("wgr_exposure_readback"),
        });
        encoder.copy_texture_to_buffer(
            self.current.as_image_copy(),
            wgpu::TexelCopyBufferInfo {
                buffer: &buf,
                layout: wgpu::TexelCopyBufferLayout {
                    offset: 0,
                    bytes_per_row: Some(256),
                    rows_per_image: Some(1),
                },
            },
            wgpu::Extent3d {
                width: 1,
                height: 1,
                depth_or_array_layers: 1,
            },
        );
        queue.submit(std::iter::once(encoder.finish()));
        let slice = buf.slice(0..4);
        let (tx, rx) = std::sync::mpsc::channel();
        slice.map_async(wgpu::MapMode::Read, move |r| {
            let _ = tx.send(r);
        });
        if device.poll(wgpu::PollType::wait_indefinitely()).is_err()
            || !matches!(rx.recv(), Ok(Ok(())))
        {
            return 1.0;
        }
        let data = slice.get_mapped_range();
        let v = f32::from_le_bytes([data[0], data[1], data[2], data[3]]);
        drop(data);
        buf.unmap();
        v
    }

    // Reduce the scene to average luminance, ease the exposure scale, and publish it.
    // `upload_params` must have run this frame. No-op until `resize`.
    pub fn probe_params(&self) -> &wgpu::Buffer {
        &self.params_buf
    }

    pub fn probe_source_meter(&self) -> Option<(&wgpu::TextureView, &wgpu::TextureView)> {
        if self.adapt_bind.is_none()
            || self.mip_count == 0
            || self.reduce_binds.len() != self.mip_count
        {
            return None;
        }
        Some((self.probe_source.as_ref()?, self.mip_views.last()?))
    }

    pub fn render(&self, encoder: &mut wgpu::CommandEncoder) -> bool {
        let Some(adapt_bind) = self.adapt_bind.as_ref() else {
            return false;
        };
        encoder.push_debug_group("wgr_exposure");
        for i in 0..self.mip_count {
            let mut pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
                label: Some("wgr_exposure_reduce"),
                color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                    view: &self.mip_views[i],
                    depth_slice: None,
                    resolve_target: None,
                    ops: wgpu::Operations {
                        load: wgpu::LoadOp::Clear(wgpu::Color::BLACK),
                        store: wgpu::StoreOp::Store,
                    },
                })],
                depth_stencil_attachment: None,
                timestamp_writes: None,
                occlusion_query_set: None,
                multiview_mask: None,
            });
            pass.set_pipeline(if i == 0 {
                &self.lum_first
            } else {
                &self.lum_down
            });
            pass.set_bind_group(0, &self.reduce_binds[i], &[]);
            pass.draw(0..3, 0..1);
        }
        // Adapt into scratch, then copy into the stable `current`.
        {
            let mut pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
                label: Some("wgr_exposure_adapt"),
                color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                    view: &self.scratch_view,
                    depth_slice: None,
                    resolve_target: None,
                    ops: wgpu::Operations {
                        load: wgpu::LoadOp::Clear(wgpu::Color::BLACK),
                        store: wgpu::StoreOp::Store,
                    },
                })],
                depth_stencil_attachment: None,
                timestamp_writes: None,
                occlusion_query_set: None,
                multiview_mask: None,
            });
            pass.set_pipeline(&self.adapt);
            pass.set_bind_group(0, adapt_bind, &[]);
            pass.draw(0..3, 0..1);
        }
        encoder.copy_texture_to_texture(
            self.scratch.as_image_copy(),
            self.current.as_image_copy(),
            wgpu::Extent3d {
                width: 1,
                height: 1,
                depth_or_array_layers: 1,
            },
        );
        encoder.pop_debug_group();
        true
    }
}
