use glam::Vec2;

use crate::ffi::{WgrBlend, WgrDraw2DBatch, WgrOverlayDraw, WgrOverlayVertex, WgrVertex2D};
use crate::gfx3d::depth_format;
use crate::textures::SharedTextures;

// Linear filter + clamp both axes (point<<2 | clampV<<1 | clampU).
const OVERLAY_SAMPLER: usize = 3;

#[cfg(test)]
mod shader_tests {
    #[test]
    fn analytic_glow_and_legacy_sprite_shader_validate() {
        let module = naga::front::wgsl::parse_str(include_str!("shader.wgsl")).unwrap();
        naga::valid::Validator::new(naga::valid::ValidationFlags::all(), naga::valid::Capabilities::all())
            .validate(&module).unwrap();
    }
}

/// SMK-037: the per-frame camera + tuning the soft-particle fragment stage needs.
/// `active` is the only thing the shader branches on, and it is false unless the frame
/// actually recorded a depth snapshot against this camera.
#[derive(Clone, Copy, Default)]
pub struct SoftGlobals {
    pub active: bool,
    pub fade: f32,
    /// Render-target size in pixels (NOT the swapchain size -- see shader.wgsl).
    pub render: [f32; 2],
    pub inv_view_proj: [f32; 16],
}

pub struct Gfx2d {
    globals_buffer: wgpu::Buffer,
    globals_bind: wgpu::BindGroup,
    // [depth_mode][blend]: depth mode = WgrDepthMode (none / test / test+write).
    // `pipelines` -> scene colour target; `pipelines_display` -> swapchain (UI phase).
    pipelines: [[wgpu::RenderPipeline; 3]; 3],
    pipelines_display: [[wgpu::RenderPipeline; 3]; 3],
    // SMK-037: depth mode 3 (WGR_DEPTH_TEST_SOFT). Same depth state as mode 1 -- test,
    // no write -- but a fourth bind group carrying the scene-depth snapshot and the
    // fs_soft entry point that fades against it. A separate pipeline LAYOUT, so the three
    // sets above and the dev overlay keep their three-group layout untouched.
    pipelines_soft: [wgpu::RenderPipeline; 3],
    pipelines_soft_display: [wgpu::RenderPipeline; 3],
    soft_layout: wgpu::BindGroupLayout,
    soft_bind: Option<wgpu::BindGroup>,
    soft_gen: u64,

    vbuf: Option<wgpu::Buffer>,
    vbuf_cap: u64,

    // Dev-panel overlay: own pipeline (no depth attachment) + buffers.
    overlay_pipeline: wgpu::RenderPipeline,
    overlay_vbuf: Option<wgpu::Buffer>,
    overlay_vbuf_cap: u64,
    overlay_ibuf: Option<wgpu::Buffer>,
    overlay_ibuf_cap: u64,
}

impl Gfx2d {
    pub fn new(
        device: &wgpu::Device,
        textures: &SharedTextures,
        // Scene color target the interleaved 2D draws render into (the HDR format
        // when the HDR path is on, else the swapchain format).
        surface_format: wgpu::TextureFormat,
        // Swapchain format for the dev overlay, which always composites post-tonemap
        // straight to the surface.
        overlay_format: wgpu::TextureFormat,
        // MSAA sample count of the scene target (the HDR path). The scene-phase 2D set
        // matches it; the display set + overlay always target the single-sample swapchain.
        sample_count: u32,
    ) -> Self {
        let shader = device.create_shader_module(wgpu::ShaderModuleDescriptor {
            label: Some("wgr_2d_shader"),
            source: wgpu::ShaderSource::Wgsl(include_str!("shader.wgsl").into()),
        });

        let globals_layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("wgr_2d_globals_layout"),
            entries: &[wgpu::BindGroupLayoutEntry {
                binding: 0,
                visibility: wgpu::ShaderStages::VERTEX_FRAGMENT,
                ty: wgpu::BindingType::Buffer {
                    ty: wgpu::BufferBindingType::Uniform,
                    has_dynamic_offset: false,
                    min_binding_size: None,
                },
                count: None,
            }],
        });

        let globals_buffer = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("wgr_2d_globals"),
            // 128 bytes: vec2 screen + vec2 pad + vec4 fog + vec4 soft params + vec4 camera
            // forward + mat4 inverse view-projection (SMK-037; zero and unread when the
            // soft path is off).
            size: 128,
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        let globals_bind = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("wgr_2d_globals_bind"),
            layout: &globals_layout,
            entries: &[wgpu::BindGroupEntry {
                binding: 0,
                resource: globals_buffer.as_entire_binding(),
            }],
        });

        let pipeline_layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("wgr_2d_pipeline_layout"),
            bind_group_layouts: &[
                Some(&globals_layout),
                Some(&textures.texture_layout),
                Some(&textures.sampler_layout),
            ],
            immediate_size: 0,
        });

        // pos(x,y,z), (rhw, fog), uv, color.
        let attrs =
            wgpu::vertex_attr_array![0 => Float32x3, 1 => Float32x2, 2 => Float32x2, 3 => Unorm8x4];
        let vbuf_layout = wgpu::VertexBufferLayout {
            array_stride: std::mem::size_of::<WgrVertex2D>() as wgpu::BufferAddress,
            step_mode: wgpu::VertexStepMode::Vertex,
            attributes: &attrs,
        };

        // (test, write): plain 2D / sky use (false,false); transparent meshes
        // (false… ) — see callers below. test gates GreaterEqual (reversed-Z) vs Always.
        let make_pipeline = |blend: Option<wgpu::BlendState>,
                             test: bool,
                             write: bool,
                             format: wgpu::TextureFormat,
                             samples: u32| {
            device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
                label: Some("wgr_2d_pipeline"),
                layout: Some(&pipeline_layout),
                vertex: wgpu::VertexState {
                    module: &shader,
                    entry_point: Some("vs_main"),
                    compilation_options: Default::default(),
                    buffers: std::slice::from_ref(&vbuf_layout),
                },
                primitive: wgpu::PrimitiveState {
                    topology: wgpu::PrimitiveTopology::TriangleList,
                    cull_mode: None,
                    ..Default::default()
                },
                depth_stencil: Some(wgpu::DepthStencilState {
                    format: depth_format(device),
                    depth_write_enabled: Some(write),
                    depth_compare: Some(if test {
                        // Reverse Z
                        wgpu::CompareFunction::GreaterEqual
                    } else {
                        wgpu::CompareFunction::Always
                    }),
                    stencil: wgpu::StencilState::default(),
                    bias: wgpu::DepthBiasState::default(),
                }),
                multisample: wgpu::MultisampleState {
                    count: samples,
                    ..Default::default()
                },
                fragment: Some(wgpu::FragmentState {
                    module: &shader,
                    entry_point: Some("fs_main"),
                    compilation_options: Default::default(),
                    targets: &[Some(wgpu::ColorTargetState {
                        format,
                        blend,
                        write_mask: wgpu::ColorWrites::ALL,
                    })],
                }),
                multiview_mask: None,
                cache: None,
            })
        };

        let alpha = wgpu::BlendState {
            color: wgpu::BlendComponent {
                src_factor: wgpu::BlendFactor::SrcAlpha,
                dst_factor: wgpu::BlendFactor::OneMinusSrcAlpha,
                operation: wgpu::BlendOperation::Add,
            },
            alpha: wgpu::BlendComponent {
                src_factor: wgpu::BlendFactor::One,
                dst_factor: wgpu::BlendFactor::OneMinusSrcAlpha,
                operation: wgpu::BlendOperation::Add,
            },
        };
        let additive = wgpu::BlendState {
            color: wgpu::BlendComponent {
                src_factor: wgpu::BlendFactor::SrcAlpha,
                dst_factor: wgpu::BlendFactor::One,
                operation: wgpu::BlendOperation::Add,
            },
            alpha: wgpu::BlendComponent::OVER,
        };
        // Indexed by WgrDepthMode: 0 none, 1 test (no write), 2 test+write.
        let blends = [None, Some(alpha), Some(additive)];
        // Two pipeline sets: `pipelines` targets the scene colour format (the HDR
        // target when HDR is on) for scene-phase 2D (sky, horizon, rain); `pipelines_display`
        // targets the swapchain for the display-referred UI phase drawn after the tonemap
        // resolve. Identical when HDR is off (surface_format == overlay_format).
        let make_set = |format: wgpu::TextureFormat, samples: u32| {
            [
                std::array::from_fn(|b| make_pipeline(blends[b], false, false, format, samples)),
                std::array::from_fn(|b| make_pipeline(blends[b], true, false, format, samples)),
                std::array::from_fn(|b| make_pipeline(blends[b], true, true, format, samples)),
            ]
        };
        let pipelines = make_set(surface_format, sample_count);
        let pipelines_display = make_set(overlay_format, 1);

        // SMK-037: the soft set. Depth state is mode 1 exactly (test, never write): a
        // cloudlet must still be hidden by a wall in front of it and must still not punch
        // a hole in the alpha pass behind it. The only difference is the fragment stage.
        let soft_layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("wgr_2d_soft_layout"),
            entries: &[wgpu::BindGroupLayoutEntry {
                binding: 0,
                visibility: wgpu::ShaderStages::FRAGMENT,
                ty: wgpu::BindingType::Texture {
                    // The snapshot is a plain R32Float colour target, not a depth texture:
                    // an unfilterable float sample, read with textureLoad.
                    sample_type: wgpu::TextureSampleType::Float { filterable: false },
                    view_dimension: wgpu::TextureViewDimension::D2,
                    multisampled: false,
                },
                count: None,
            }],
        });
        let soft_pipeline_layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("wgr_2d_soft_pipeline_layout"),
            bind_group_layouts: &[
                Some(&globals_layout),
                Some(&textures.texture_layout),
                Some(&textures.sampler_layout),
                Some(&soft_layout),
            ],
            immediate_size: 0,
        });
        let make_soft = |blend: Option<wgpu::BlendState>,
                         format: wgpu::TextureFormat,
                         samples: u32| {
            device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
                label: Some("wgr_2d_soft_pipeline"),
                layout: Some(&soft_pipeline_layout),
                vertex: wgpu::VertexState {
                    module: &shader,
                    entry_point: Some("vs_main"),
                    compilation_options: Default::default(),
                    buffers: std::slice::from_ref(&vbuf_layout),
                },
                primitive: wgpu::PrimitiveState {
                    topology: wgpu::PrimitiveTopology::TriangleList,
                    cull_mode: None,
                    ..Default::default()
                },
                depth_stencil: Some(wgpu::DepthStencilState {
                    format: depth_format(device),
                    depth_write_enabled: Some(false),
                    depth_compare: Some(wgpu::CompareFunction::GreaterEqual),
                    stencil: wgpu::StencilState::default(),
                    bias: wgpu::DepthBiasState::default(),
                }),
                multisample: wgpu::MultisampleState {
                    count: samples,
                    ..Default::default()
                },
                fragment: Some(wgpu::FragmentState {
                    module: &shader,
                    entry_point: Some("fs_soft"),
                    compilation_options: Default::default(),
                    targets: &[Some(wgpu::ColorTargetState {
                        format,
                        blend,
                        write_mask: wgpu::ColorWrites::ALL,
                    })],
                }),
                multiview_mask: None,
                cache: None,
            })
        };
        let pipelines_soft: [wgpu::RenderPipeline; 3] =
            std::array::from_fn(|b| make_soft(blends[b], surface_format, sample_count));
        let pipelines_soft_display: [wgpu::RenderPipeline; 3] =
            std::array::from_fn(|b| make_soft(blends[b], overlay_format, 1));

        let overlay_shader = device.create_shader_module(wgpu::ShaderModuleDescriptor {
            label: Some("wgr_overlay_shader"),
            source: wgpu::ShaderSource::Wgsl(include_str!("overlay.wgsl").into()),
        });
        let overlay_attrs = wgpu::vertex_attr_array![0 => Float32x2, 1 => Float32x2, 2 => Unorm8x4];
        let overlay_pipeline = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
            label: Some("wgr_overlay_pipeline"),
            layout: Some(&pipeline_layout),
            vertex: wgpu::VertexState {
                module: &overlay_shader,
                entry_point: Some("vs_main"),
                compilation_options: Default::default(),
                buffers: &[wgpu::VertexBufferLayout {
                    array_stride: std::mem::size_of::<WgrOverlayVertex>() as wgpu::BufferAddress,
                    step_mode: wgpu::VertexStepMode::Vertex,
                    attributes: &overlay_attrs,
                }],
            },
            primitive: wgpu::PrimitiveState {
                topology: wgpu::PrimitiveTopology::TriangleList,
                cull_mode: None,
                ..Default::default()
            },
            depth_stencil: None,
            multisample: wgpu::MultisampleState::default(),
            fragment: Some(wgpu::FragmentState {
                module: &overlay_shader,
                entry_point: Some("fs_main"),
                compilation_options: Default::default(),
                targets: &[Some(wgpu::ColorTargetState {
                    format: overlay_format,
                    blend: Some(wgpu::BlendState {
                        color: wgpu::BlendComponent {
                            src_factor: wgpu::BlendFactor::SrcAlpha,
                            dst_factor: wgpu::BlendFactor::OneMinusSrcAlpha,
                            operation: wgpu::BlendOperation::Add,
                        },
                        alpha: wgpu::BlendComponent {
                            src_factor: wgpu::BlendFactor::One,
                            dst_factor: wgpu::BlendFactor::OneMinusSrcAlpha,
                            operation: wgpu::BlendOperation::Add,
                        },
                    }),
                    write_mask: wgpu::ColorWrites::ALL,
                })],
            }),
            multiview_mask: None,
            cache: None,
        });

        Gfx2d {
            globals_buffer,
            globals_bind,
            pipelines,
            pipelines_display,
            pipelines_soft,
            pipelines_soft_display,
            soft_layout,
            soft_bind: None,
            soft_gen: u64::MAX,
            vbuf: None,
            vbuf_cap: 0,
            overlay_pipeline,
            overlay_vbuf: None,
            overlay_vbuf_cap: 0,
            overlay_ibuf: None,
            overlay_ibuf_cap: 0,
        }
    }

    pub fn prepare(
        &mut self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        screen: Vec2,
        fog: [f32; 3],
        verts: &[WgrVertex2D],
    ) {
        // 8 floats: screen.xy, pad.xy, fog.rgb, pad. The SMK-037 lanes that follow are
        // written separately by `update_soft_globals`, because the camera they carry is
        // only derived later in the frame (with the sky) and a stale one would fade
        // sprites against the previous frame's view.
        let globals = [screen.x, screen.y, 0.0, 0.0, fog[0], fog[1], fog[2], 0.0];
        queue.write_buffer(&self.globals_buffer, 0, bytemuck::bytes_of(&globals));

        if verts.is_empty() {
            return;
        }
        let bytes: &[u8] = bytemuck::cast_slice(verts);
        let needed = bytes.len() as u64;
        if self.vbuf_cap < needed {
            let cap = needed.next_power_of_two().max(64 * 1024);
            self.vbuf = Some(device.create_buffer(&wgpu::BufferDescriptor {
                label: Some("wgr_2d_vertices"),
                size: cap,
                usage: wgpu::BufferUsages::VERTEX | wgpu::BufferUsages::COPY_DST,
                mapped_at_creation: false,
            }));
            self.vbuf_cap = cap;
        }
        queue.write_buffer(self.vbuf.as_ref().unwrap(), 0, bytes);
    }

    // Draw one batch. Re-binds the vertex buffer + globals every call because 3D
    // draws interleaved in the same pass clobber vertex buffer slot 0. The batch's
    // depth mode picks the pipeline set (plain 2D = none; pre-projected meshes test
    // and, when opaque, write).
    pub fn draw_one(
        &self,
        device: &wgpu::Device,
        pass: &mut wgpu::RenderPass<'_>,
        textures: &SharedTextures,
        b: &WgrDraw2DBatch,
        // true = the display-referred UI phase (after the tonemap resolve), so use
        // the swapchain-format pipeline set instead of the scene (HDR) one.
        display: bool,
    ) {
        if b.vertex_count == 0 {
            return;
        }
        let Some(vbuf) = self.vbuf.as_ref() else {
            return;
        };
        let sets = if display {
            &self.pipelines_display
        } else {
            &self.pipelines
        };
        // SMK-037: depth mode 3 = WGR_DEPTH_TEST_SOFT. It needs a snapshot bind group; if
        // this frame has none (the soft lever is off, or the camera drew no world), fall
        // back to mode 1 -- the depth state is identical, so the sprite draws exactly as it
        // did before the feature existed rather than not at all.
        let soft = (b.depth == 3).then_some(self.soft_bind.as_ref()).flatten();
        let pipeline = match soft {
            Some(_) => {
                let set = if display {
                    &self.pipelines_soft_display
                } else {
                    &self.pipelines_soft
                };
                set.get(b.blend as usize)
                    .unwrap_or(&set[WgrBlend::Alpha as usize])
            }
            None => {
                let idx = if b.depth == 3 { 1 } else { b.depth as usize };
                let set = sets.get(idx).unwrap_or(&sets[0]);
                set.get(b.blend as usize)
                    .unwrap_or(&set[WgrBlend::Alpha as usize])
            }
        };
        pass.set_pipeline(pipeline);
        pass.set_vertex_buffer(0, vbuf.slice(..));
        pass.set_bind_group(0, &self.globals_bind, &[]);
        pass.set_bind_group(1, textures.texture_bind(device, b.texture_id), &[]);
        pass.set_bind_group(2, textures.sampler_bind(b.sampler.index()), &[]);
        if let Some(bind) = soft {
            pass.set_bind_group(3, bind, &[]);
        }
        pass.draw(b.first_vertex..(b.first_vertex + b.vertex_count), 0..1);
    }

    /// SMK-037: the tail of the globals block -- everything the soft fragment stage reads.
    /// Written after the frame's camera is known; a no-op cost when the path is off, and
    /// zeroes leave `soft.y` at 0 so even a soft pipeline would fade nothing.
    pub fn update_soft_globals(&self, queue: &wgpu::Queue, soft: &SoftGlobals) {
        let mut tail = [0.0f32; 24];
        tail[0] = soft.fade;
        tail[1] = if soft.active { 1.0 } else { 0.0 };
        tail[2] = soft.render[0];
        tail[3] = soft.render[1];
        tail[8..24].copy_from_slice(&soft.inv_view_proj);
        queue.write_buffer(&self.globals_buffer, 32, bytemuck::bytes_of(&tail));
    }

    /// SMK-037: point the soft pipelines at this frame's depth snapshot. `depth_gen` changes
    /// when the view does (render resize), which is the only time the bind group is
    /// rebuilt. Passing None un-binds it, so every soft batch falls back to plain mode 1.
    pub fn set_soft_depth(
        &mut self,
        device: &wgpu::Device,
        view: Option<&wgpu::TextureView>,
        depth_gen: u64,
    ) {
        match view {
            None => {
                self.soft_bind = None;
                self.soft_gen = u64::MAX;
            }
            Some(view) => {
                if self.soft_bind.is_some() && self.soft_gen == depth_gen {
                    return;
                }
                self.soft_bind = Some(device.create_bind_group(&wgpu::BindGroupDescriptor {
                    label: Some("wgr_2d_soft_bind"),
                    layout: &self.soft_layout,
                    entries: &[wgpu::BindGroupEntry {
                        binding: 0,
                        resource: wgpu::BindingResource::TextureView(view),
                    }],
                }));
                self.soft_gen = depth_gen;
            }
        }
    }

    pub fn prepare_overlay(
        &mut self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        verts: &[WgrOverlayVertex],
        indices: &[u16],
    ) {
        if verts.is_empty() || indices.is_empty() {
            return;
        }

        let vbytes: &[u8] = bytemuck::cast_slice(verts);
        if self.overlay_vbuf_cap < vbytes.len() as u64 {
            let cap = (vbytes.len() as u64).next_power_of_two().max(16 * 1024);
            self.overlay_vbuf = Some(device.create_buffer(&wgpu::BufferDescriptor {
                label: Some("wgr_overlay_vertices"),
                size: cap,
                usage: wgpu::BufferUsages::VERTEX | wgpu::BufferUsages::COPY_DST,
                mapped_at_creation: false,
            }));
            self.overlay_vbuf_cap = cap;
        }
        queue.write_buffer(self.overlay_vbuf.as_ref().unwrap(), 0, vbytes);

        // write_buffer needs 4-byte-aligned sizes; pad an odd u16 count.
        let ibytes: &[u8] = bytemuck::cast_slice(indices);
        let padded = (ibytes.len() as u64 + 3) & !3;
        if self.overlay_ibuf_cap < padded {
            let cap = padded.next_power_of_two().max(16 * 1024);
            self.overlay_ibuf = Some(device.create_buffer(&wgpu::BufferDescriptor {
                label: Some("wgr_overlay_indices"),
                size: cap,
                usage: wgpu::BufferUsages::INDEX | wgpu::BufferUsages::COPY_DST,
                mapped_at_creation: false,
            }));
            self.overlay_ibuf_cap = cap;
        }
        let ibuf = self.overlay_ibuf.as_ref().unwrap();
        if padded == ibytes.len() as u64 {
            queue.write_buffer(ibuf, 0, ibytes);
        } else {
            let mut scratch = ibytes.to_vec();
            scratch.resize(padded as usize, 0);
            queue.write_buffer(ibuf, 0, &scratch);
        }
    }

    pub fn render_overlay(
        &self,
        device: &wgpu::Device,
        pass: &mut wgpu::RenderPass<'_>,
        textures: &SharedTextures,
        draws: &[WgrOverlayDraw],
        width: u32,
        height: u32,
    ) {
        let (Some(vbuf), Some(ibuf)) = (self.overlay_vbuf.as_ref(), self.overlay_ibuf.as_ref())
        else {
            return;
        };
        pass.set_pipeline(&self.overlay_pipeline);
        pass.set_vertex_buffer(0, vbuf.slice(..));
        pass.set_index_buffer(ibuf.slice(..), wgpu::IndexFormat::Uint16);
        pass.set_bind_group(0, &self.globals_bind, &[]);
        pass.set_bind_group(2, textures.sampler_bind(OVERLAY_SAMPLER), &[]);
        for d in draws {
            let x0 = (d.clip[0].max(0.0) as u32).min(width);
            let y0 = (d.clip[1].max(0.0) as u32).min(height);
            let x1 = (d.clip[2].max(0.0).ceil() as u32).min(width);
            let y1 = (d.clip[3].max(0.0).ceil() as u32).min(height);
            if x1 <= x0 || y1 <= y0 || d.index_count == 0 {
                continue;
            }
            pass.set_scissor_rect(x0, y0, x1 - x0, y1 - y0);
            pass.set_bind_group(1, textures.texture_bind(device, d.texture_id), &[]);
            pass.draw_indexed(
                d.first_index..(d.first_index + d.index_count),
                d.base_vertex as i32,
                0..1,
            );
        }
        pass.set_scissor_rect(0, 0, width, height);
    }
}
