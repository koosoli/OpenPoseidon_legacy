use glam::Vec2;

use crate::ffi::{WgrBlend, WgrDraw2DBatch, WgrOverlayDraw, WgrOverlayVertex, WgrVertex2D};
use crate::gfx3d::DEPTH_FORMAT;
use crate::textures::SharedTextures;

// Linear filter + clamp both axes (point<<2 | clampV<<1 | clampU).
const OVERLAY_SAMPLER: usize = 3;

pub struct Gfx2d {
    globals_buffer: wgpu::Buffer,
    globals_bind: wgpu::BindGroup,
    // [depth_mode][blend]: depth mode = WgrDepthMode
    // (none / test / test+write / test+write+alpha-cut).
    // `pipelines` -> scene colour target; `pipelines_display` -> swapchain (UI phase).
    pipelines: [[wgpu::RenderPipeline; 3]; 4],
    pipelines_display: [[wgpu::RenderPipeline; 3]; 4],

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
            // 32 bytes: vec2 screen size + vec2 padding + vec4 fog colour.
            size: 32,
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
        // `cut` picks the alpha-discarding fragment entry point; see fs_main_cut in
        // shader.wgsl and WGR_DEPTH_TEST_WRITE_CUT in the ABI header.
        let make_pipeline = |blend: Option<wgpu::BlendState>,
                             test: bool,
                             write: bool,
                             cut: bool,
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
                    format: DEPTH_FORMAT,
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
                    entry_point: Some(if cut { "fs_main_cut" } else { "fs_main" }),
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
                std::array::from_fn(|b| {
                    make_pipeline(blends[b], false, false, false, format, samples)
                }),
                std::array::from_fn(|b| {
                    make_pipeline(blends[b], true, false, false, format, samples)
                }),
                std::array::from_fn(|b| {
                    make_pipeline(blends[b], true, true, false, format, samples)
                }),
                std::array::from_fn(|b| {
                    make_pipeline(blends[b], true, true, true, format, samples)
                }),
            ]
        };
        let pipelines = make_set(surface_format, sample_count);
        let pipelines_display = make_set(overlay_format, 1);

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
        // 8 floats: screen.xy, pad.xy, fog.rgb, pad.
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
        let set = sets.get(b.depth as usize).unwrap_or(&sets[0]);
        let pipeline = set
            .get(b.blend as usize)
            .unwrap_or(&set[WgrBlend::Alpha as usize]);
        pass.set_pipeline(pipeline);
        pass.set_vertex_buffer(0, vbuf.slice(..));
        pass.set_bind_group(0, &self.globals_bind, &[]);
        pass.set_bind_group(1, textures.texture_bind(b.texture_id), &[]);
        pass.set_bind_group(2, textures.sampler_bind(b.sampler.index()), &[]);
        pass.draw(b.first_vertex..(b.first_vertex + b.vertex_count), 0..1);
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
            pass.set_bind_group(1, textures.texture_bind(d.texture_id), &[]);
            pass.draw_indexed(
                d.first_index..(d.first_index + d.index_count),
                d.base_vertex as i32,
                0..1,
            );
        }
        pass.set_scissor_rect(0, 0, width, height);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // A real wgpu device, not naga. The alpha-cut 2D pipeline (WGR_DEPTH_TEST_WRITE_CUT)
    // names a SECOND fragment entry point, `fs_main_cut`, and the pipeline array grew from
    // three depth modes to four. Both are the kind of mistake naga cannot see: a
    // misspelled entry point or an illegal depth state fails inside
    // `create_render_pipeline`, which the game only reaches at device init -- i.e. as a
    // black window on the owner's machine, not as a failing test. So build the whole set
    // inside a validation error scope and assert the scope pops empty.
    #[test]
    fn every_2d_pipeline_including_the_alpha_cut_one_is_valid() {
        // Own device rather than cull::tests::headless(): SharedTextures binds a texture
        // ARRAY, so the device must be created with TEXTURE_BINDING_ARRAY or the bind
        // group layout is rejected before any pipeline is reached.
        let instance =
            wgpu::Instance::new(wgpu::InstanceDescriptor::new_without_display_handle_from_env());
        let Some(adapter) =
            pollster::block_on(instance.request_adapter(&wgpu::RequestAdapterOptions {
                power_preference: wgpu::PowerPreference::default(),
                compatible_surface: None,
                force_fallback_adapter: false,
            }))
            .ok()
        else {
            // No adapter in this environment (headless CI without a GPU). Skipping is
            // correct; silently passing while pretending to have tested is not.
            eprintln!("no wgpu adapter available -- 2D pipeline validation skipped");
            return;
        };
        let wanted = wgpu::Features::TEXTURE_BINDING_ARRAY;
        if !adapter.features().contains(wanted) {
            eprintln!("adapter lacks TEXTURE_BINDING_ARRAY -- 2D pipeline validation skipped");
            return;
        }
        let Ok((device, queue)) =
            pollster::block_on(adapter.request_device(&wgpu::DeviceDescriptor {
                required_features: wanted,
                // The default limits put max_binding_array_elements_per_shader_stage at 0,
                // which rejects the texture array before any pipeline is built. The real
                // renderer asks the adapter for what it has; so does this.
                required_limits: adapter.limits(),
                ..Default::default()
            }))
        else {
            eprintln!("could not create a device -- 2D pipeline validation skipped");
            return;
        };
        let textures = crate::textures::SharedTextures::new(
            &device,
            &queue,
            /*bc_supported*/ false,
            /*object_cap*/ 4,
            /*partially_bound*/ false,
            &crate::log::LogSink::none(),
        );
        let scope = device.push_error_scope(wgpu::ErrorFilter::Validation);
        let gfx2d = Gfx2d::new(
            &device,
            &textures,
            wgpu::TextureFormat::Rgba16Float,
            wgpu::TextureFormat::Bgra8UnormSrgb,
            /*sample_count*/ 1,
        );
        let err = pollster::block_on(scope.pop());
        assert!(
            err.is_none(),
            "2D pipeline creation failed validation: {err:?}"
        );
        // Four depth modes, and the fourth is the alpha-cut one. Asserted rather than
        // assumed: `draw_one` falls back to `sets[0]` (no depth at all) for an index it
        // does not have, so a set that never grew would put the bug straight back and
        // every other test would stay green.
        assert_eq!(gfx2d.pipelines.len(), 4);
        assert_eq!(gfx2d.pipelines_display.len(), 4);
        assert_eq!(
            crate::ffi::WgrDepthMode::TestWriteCut as usize,
            gfx2d.pipelines.len() - 1
        );
    }
}
