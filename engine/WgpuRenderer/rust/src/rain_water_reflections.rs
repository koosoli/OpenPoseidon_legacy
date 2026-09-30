//! Current-frame, near-water opaque-scene reflection resources. No ocean state.
use crate::ffi::WgrCamera;
use glam::{Mat4, Vec3};
use wgpu::util::DeviceExt;

pub const NEAR_RANGE: f32 = 60.0;
pub const TRACE_RANGE: f32 = 25.0;

/// Conservative actual wet-cell box interest. Behind-camera/far water does not
/// pay a scene snapshot. A box crossing the frustum edge remains admitted.
pub fn near_visible_box(camera: &WgrCamera, minimum: Vec3, maximum: Vec3) -> bool {
    let eye = Vec3::from_array([camera.cam_pos[0], camera.cam_pos[1], camera.cam_pos[2]]);
    if !minimum.is_finite()
        || !maximum.is_finite()
        || !eye.is_finite()
        || minimum.cmpgt(maximum).any()
        || eye.distance(minimum.max(eye.min(maximum))) > NEAR_RANGE
    {
        return false;
    }
    let vp = Mat4::from_cols_array(&camera.proj) * Mat4::from_cols_array(&camera.view);
    if !vp.is_finite() {
        return false;
    }
    let corners: [glam::Vec4; 8] = std::array::from_fn(|i| {
        let p = Vec3::new(
            if i & 1 == 0 { minimum.x } else { maximum.x },
            if i & 2 == 0 { minimum.y } else { maximum.y },
            if i & 4 == 0 { minimum.z } else { maximum.z },
        );
        vp * (p - eye).extend(1.0)
    });
    if corners.iter().any(|p| !p.is_finite()) {
        return false;
    }
    !(0..7).any(|plane| {
        corners.iter().all(|p| match plane {
            0 => p.w <= 1e-5,
            1 => p.x < -p.w,
            2 => p.x > p.w,
            3 => p.y < -p.w,
            4 => p.y > p.w,
            5 => p.z < 0.0,
            _ => p.z > p.w,
        })
    })
}

struct Target {
    texture: wgpu::Texture,
    view: wgpu::TextureView,
    size: (u32, u32),
}

pub struct Reflections {
    pub layout: wgpu::BindGroupLayout,
    pub bind: wgpu::BindGroup,
    params: wgpu::Buffer,
    sampler: wgpu::Sampler,
    target: Option<Target>,
    depth_generation: u64,
    format: wgpu::TextureFormat,
    enabled: bool,
    interested: bool,
    trace: bool,
    trace_rows: u32,
}

impl Reflections {
    pub fn new(device: &wgpu::Device, format: wgpu::TextureFormat) -> Self {
        let enabled = std::env::var("WGR_RAIN_WATER_SSR").as_deref() != Ok("0");
        let trace = std::env::var("WGR_RAIN_WATER_SSR_TRACE").as_deref() == Ok("1");
        if trace {
            eprintln!(
                "WGPU_RAIN_WATER_SSR_POLICY enabled={} receiverRange=60 rayRange=25 steps=24 refine=4 scope=current-screen-world",
                u8::from(enabled)
            );
        }
        let texture = |binding, sample_type| wgpu::BindGroupLayoutEntry {
            binding,
            visibility: wgpu::ShaderStages::FRAGMENT,
            ty: wgpu::BindingType::Texture {
                sample_type,
                view_dimension: wgpu::TextureViewDimension::D2,
                multisampled: false,
            },
            count: None,
        };
        let layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("rain_water_reflections_layout"),
            entries: &[
                texture(0, wgpu::TextureSampleType::Float { filterable: true }),
                wgpu::BindGroupLayoutEntry {
                    binding: 1,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Filtering),
                    count: None,
                },
                texture(2, wgpu::TextureSampleType::Depth),
                wgpu::BindGroupLayoutEntry {
                    binding: 3,
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
        let params = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
            label: Some("rain_water_reflections_validity"),
            contents: bytemuck::cast_slice(&[0.0_f32, NEAR_RANGE, TRACE_RANGE, 0.0]),
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
        });
        let sampler = device.create_sampler(&wgpu::SamplerDescriptor {
            label: Some("rain_water_reflections_sampler"),
            mag_filter: wgpu::FilterMode::Linear,
            min_filter: wgpu::FilterMode::Linear,
            ..Default::default()
        });
        let dummy = |label, format, usage| {
            device.create_texture(&wgpu::TextureDescriptor {
                label: Some(label),
                size: wgpu::Extent3d {
                    width: 1,
                    height: 1,
                    depth_or_array_layers: 1,
                },
                mip_level_count: 1,
                sample_count: 1,
                dimension: wgpu::TextureDimension::D2,
                format,
                usage,
                view_formats: &[],
            })
        };
        let color = dummy(
            "rain_water_reflections_closed_color",
            format,
            wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::RENDER_ATTACHMENT,
        );
        let depth = dummy(
            "rain_water_reflections_closed_depth",
            wgpu::TextureFormat::Depth32Float,
            wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::RENDER_ATTACHMENT,
        );
        let bind = Self::binding(
            device,
            &layout,
            &color.create_view(&Default::default()),
            &depth.create_view(&Default::default()),
            &sampler,
            &params,
        );
        Self {
            layout,
            bind,
            params,
            sampler,
            target: None,
            depth_generation: u64::MAX,
            format,
            enabled,
            interested: false,
            trace,
            trace_rows: 0,
        }
    }
    fn binding(
        device: &wgpu::Device,
        layout: &wgpu::BindGroupLayout,
        color: &wgpu::TextureView,
        depth: &wgpu::TextureView,
        sampler: &wgpu::Sampler,
        params: &wgpu::Buffer,
    ) -> wgpu::BindGroup {
        device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("rain_water_reflections_bind"),
            layout,
            entries: &[
                wgpu::BindGroupEntry {
                    binding: 0,
                    resource: wgpu::BindingResource::TextureView(color),
                },
                wgpu::BindGroupEntry {
                    binding: 1,
                    resource: wgpu::BindingResource::Sampler(sampler),
                },
                wgpu::BindGroupEntry {
                    binding: 2,
                    resource: wgpu::BindingResource::TextureView(depth),
                },
                wgpu::BindGroupEntry {
                    binding: 3,
                    resource: params.as_entire_binding(),
                },
            ],
        })
    }
    pub fn begin_frame(&mut self, queue: &wgpu::Queue) {
        self.interested = false;
        // Previous textures may remain allocated, but never supply stale radiance.
        queue.write_buffer(
            &self.params,
            0,
            bytemuck::cast_slice(&[0.0_f32, NEAR_RANGE, TRACE_RANGE, 0.0]),
        );
    }
    pub fn offer_box(&mut self, camera: &WgrCamera, minimum: Vec3, maximum: Vec3) {
        if self.enabled && !self.interested {
            self.interested = near_visible_box(camera, minimum, maximum);
        }
    }
    pub fn needed(&self) -> bool {
        self.enabled && self.interested
    }
    pub fn enabled(&self) -> bool {
        self.enabled
    }

    /// Record the snapshot before the only draw which consumes it, in this same
    /// encoder. Ready is never asserted for a previous-frame or requested image.
    pub fn record(
        &mut self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        encoder: &mut wgpu::CommandEncoder,
        source: &wgpu::Texture,
        source_view: &wgpu::TextureView,
        depth: &wgpu::TextureView,
        depth_generation: u64,
    ) -> bool {
        if !self.needed() {
            return false;
        }
        let size = (source.width(), source.height());
        if size.0 == 0 || size.1 == 0 || source.format() != self.format {
            return false;
        }
        let resize = self.target.as_ref().is_none_or(|t| t.size != size);
        if resize {
            let texture = device.create_texture(&wgpu::TextureDescriptor {
                label: Some("rain_water_opaque_snapshot"),
                size: wgpu::Extent3d {
                    width: size.0,
                    height: size.1,
                    depth_or_array_layers: 1,
                },
                mip_level_count: 1,
                sample_count: 1,
                dimension: wgpu::TextureDimension::D2,
                format: self.format,
                usage: wgpu::TextureUsages::RENDER_ATTACHMENT
                    | wgpu::TextureUsages::TEXTURE_BINDING
                    | wgpu::TextureUsages::COPY_DST,
                view_formats: &[],
            });
            let view = texture.create_view(&Default::default());
            self.target = Some(Target {
                texture,
                view,
                size,
            });
        }
        let target = self.target.as_ref().unwrap();
        if resize || self.depth_generation != depth_generation {
            self.bind = Self::binding(
                device,
                &self.layout,
                &target.view,
                depth,
                &self.sampler,
                &self.params,
            );
            self.depth_generation = depth_generation;
        }
        if source.sample_count() > 1 {
            let pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
                label: Some("rain_water_opaque_snapshot_resolve"),
                color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                    view: source_view,
                    depth_slice: None,
                    resolve_target: Some(&target.view),
                    ops: wgpu::Operations {
                        load: wgpu::LoadOp::Load,
                        store: wgpu::StoreOp::Store,
                    },
                })],
                depth_stencil_attachment: None,
                timestamp_writes: None,
                occlusion_query_set: None,
                multiview_mask: None,
            });
            drop(pass);
        } else {
            encoder.copy_texture_to_texture(
                wgpu::TexelCopyTextureInfo {
                    texture: source,
                    mip_level: 0,
                    origin: wgpu::Origin3d::ZERO,
                    aspect: wgpu::TextureAspect::All,
                },
                wgpu::TexelCopyTextureInfo {
                    texture: &target.texture,
                    mip_level: 0,
                    origin: wgpu::Origin3d::ZERO,
                    aspect: wgpu::TextureAspect::All,
                },
                wgpu::Extent3d {
                    width: size.0,
                    height: size.1,
                    depth_or_array_layers: 1,
                },
            );
        }
        queue.write_buffer(
            &self.params,
            0,
            bytemuck::cast_slice(&[1.0_f32, NEAR_RANGE, TRACE_RANGE, 0.0]),
        );
        if self.trace && self.trace_rows < 32 {
            eprintln!(
                "WGPU_RAIN_WATER_SSR enabled=1 interest=1 ready=1 size={}x{} samples={} range=25 steps=24 refine=4 source=pre-rain-world selfFeedback=false",
                size.0,
                size.1,
                source.sample_count()
            );
            self.trace_rows += 1;
        }
        true
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn camera() -> WgrCamera {
        let mut c: WgrCamera = bytemuck::Zeroable::zeroed();
        c.proj = Mat4::perspective_rh(1.0, 1.0, 0.1, 500.0).to_cols_array();
        c.view = Mat4::IDENTITY.to_cols_array();
        c
    }
    #[test]
    fn actual_near_water_interest_rejects_far_behind_nonfinite_and_accepts_frustum_edges() {
        let c = camera();
        assert!(near_visible_box(
            &c,
            Vec3::new(-1.0, -1.0, -8.0),
            Vec3::new(1.0, 1.0, -6.0)
        ));
        assert!(!near_visible_box(
            &c,
            Vec3::new(-1.0, -1.0, 6.0),
            Vec3::new(1.0, 1.0, 8.0)
        ));
        assert!(!near_visible_box(
            &c,
            Vec3::new(-1.0, -1.0, -100.0),
            Vec3::new(1.0, 1.0, -90.0)
        ));
        assert!(!near_visible_box(
            &c,
            Vec3::new(40.0, -1.0, -8.0),
            Vec3::new(42.0, 1.0, -6.0)
        ));
        assert!(near_visible_box(
            &c,
            Vec3::new(3.0, -1.0, -8.0),
            Vec3::new(8.0, 1.0, -6.0)
        ));
        assert!(!near_visible_box(&c, Vec3::splat(f32::NAN), Vec3::ONE));
        assert!(!near_visible_box(&c, Vec3::ONE, Vec3::ZERO));
    }
    #[test]
    fn actual_reflection_helper_composes_with_frame_without_ocean_modules() {
        let mut composer = crate::shaders::build_composer();
        let source = "#import rain_water_reflections::rain_water_scene_reflection\n@fragment fn fs_probe()->@location(0) vec4<f32>{return rain_water_scene_reflection(vec3<f32>(0.0,0.0,-5.0),vec3<f32>(0.0,1.0,0.0),0.0,0.01);}";
        composer
            .make_naga_module(naga_oil::compose::NagaModuleDescriptor {
                source,
                file_path: "rain_water_ssr_probe.wgsl",
                ..Default::default()
            })
            .unwrap_or_else(|e| panic!("{}", e.emit_to_string(&composer)));
    }
    fn gpu_probe_source() -> String {
        // Only the camera declaration is reduced for this numeric probe. Every
        // production ray/projection/depth/crossing/refinement/hit function remains.
        let source=include_str!("rain_water_reflections.wgsl")
            .replace("#define_import_path rain_water_reflections","")
            .replace("#import frame::frame","struct ProbeFrame {proj:mat4x4<f32>,view:mat4x4<f32>,inv_view_proj:mat4x4<f32>}; @group(0) @binding(0) var<uniform> frame:ProbeFrame;");
        format!(
            "{source}\n@group(1) @binding(0) var<storage,read_write> result:array<vec4<f32>>;\n\
            @compute @workgroup_size(1) fn cs_probe(@builtin(global_invocation_id) id:vec3<u32>) {{\n\
            let s=vec3<f32>(0.0,-2.0,-5.0);let n=vec3<f32>(0.0,1.0,0.0);\n\
            result[0]=rain_water_scene_reflection(s,n,0.0,0.01);\n\
            result[1]=rain_water_scene_reflection(s,n,1.0,0.01);\n\
            result[2]=rain_water_scene_reflection(vec3<f32>(0.0,-2.0,-80.0),n,0.0,0.01);\n\
            result[3]=rain_water_scene_reflection(s,-n,0.0,0.01);\n\
            result[4]=rain_water_scene_reflection(vec3<f32>(50.0,-2.0,-5.0),n,0.0,0.01);\n\
            result[5]=rain_water_scene_reflection(s,n,0.0,1.0);\n\
            result[6]=rain_water_scene_reflection(vec3<f32>(bitcast<f32>(0x7fc00000u+id.x),-2.0,-5.0),n,0.0,0.01);\n}} "
        )
    }
    #[test]
    fn actual_gpu_probe_source_validates_without_device() {
        let module = naga::front::wgsl::parse_str(&gpu_probe_source()).unwrap();
        naga::valid::Validator::new(
            naga::valid::ValidationFlags::all(),
            naga::valid::Capabilities::all(),
        )
        .validate(&module)
        .unwrap();
    }
    #[test]
    fn gpu_actual_snapshot_msaa_resize_black_hits_expiry_and_bounded_reflection_guards() {
        let (device, queue) = crate::gfx3d::cull::tests::headless()
            .expect("puddle SSR acceptance requires a real device");
        let probe = gpu_probe_source();
        let shader = device.create_shader_module(wgpu::ShaderModuleDescriptor {
            label: Some("actual_puddle_ssr_functions"),
            source: wgpu::ShaderSource::Wgsl(probe.into()),
        });
        let pipeline = device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
            label: None,
            layout: None,
            module: &shader,
            entry_point: Some("cs_probe"),
            compilation_options: Default::default(),
            cache: None,
        });
        let proj = Mat4::perspective_rh(1.0, 1.0, 0.1, 500.0);
        let camera_bytes: [f32; 48] = [
            proj.to_cols_array(),
            Mat4::IDENTITY.to_cols_array(),
            proj.inverse().to_cols_array(),
        ]
        .concat()
        .try_into()
        .unwrap();
        let camera_buffer = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
            label: None,
            contents: bytemuck::cast_slice(&camera_bytes),
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
        });
        let camera_bind = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: None,
            layout: &pipeline.get_bind_group_layout(0),
            entries: &[wgpu::BindGroupEntry {
                binding: 0,
                resource: camera_buffer.as_entire_binding(),
            }],
        });
        let output = device.create_buffer(&wgpu::BufferDescriptor {
            label: None,
            size: 7 * 16,
            usage: wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_SRC,
            mapped_at_creation: false,
        });
        let staging = device.create_buffer(&wgpu::BufferDescriptor {
            label: None,
            size: 7 * 16,
            usage: wgpu::BufferUsages::MAP_READ | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        let output_bind = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: None,
            layout: &pipeline.get_bind_group_layout(1),
            entries: &[wgpu::BindGroupEntry {
                binding: 0,
                resource: output.as_entire_binding(),
            }],
        });
        let mut reflection = Reflections::new(&device, wgpu::TextureFormat::Rgba16Float);
        reflection.enabled = true;
        // Repeated resource recreation covers 1x copy and 4x resolve, resizing,
        // valid black radiance, absent depth, foreground and beyond-range walls.
        for (size, samples, wall, black) in [
            (128, 1, -8.0, false),
            (256, 4, -8.0, true),
            (128, 1, 0.0, false),
            (256, 4, -1.0, false),
            (128, 1, -40.0, false),
        ] {
            reflection.begin_frame(&queue);
            reflection.offer_box(
                &camera(),
                Vec3::new(-1.0, -2.0, -6.0),
                Vec3::new(1.0, -1.0, -4.0),
            );
            let color = device.create_texture(&wgpu::TextureDescriptor {
                label: None,
                size: wgpu::Extent3d {
                    width: size,
                    height: size,
                    depth_or_array_layers: 1,
                },
                mip_level_count: 1,
                sample_count: samples,
                dimension: wgpu::TextureDimension::D2,
                format: wgpu::TextureFormat::Rgba16Float,
                usage: wgpu::TextureUsages::RENDER_ATTACHMENT
                    | if samples == 1 {
                        wgpu::TextureUsages::COPY_SRC
                    } else {
                        wgpu::TextureUsages::empty()
                    },
                view_formats: &[],
            });
            let color_view = color.create_view(&Default::default());
            let depth = device.create_texture(&wgpu::TextureDescriptor {
                label: None,
                size: wgpu::Extent3d {
                    width: size,
                    height: size,
                    depth_or_array_layers: 1,
                },
                mip_level_count: 1,
                sample_count: 1,
                dimension: wgpu::TextureDimension::D2,
                format: wgpu::TextureFormat::Depth32Float,
                usage: wgpu::TextureUsages::RENDER_ATTACHMENT
                    | wgpu::TextureUsages::TEXTURE_BINDING,
                view_formats: &[],
            });
            let depth_view = depth.create_view(&Default::default());
            let z = proj * Vec3::new(0.0, 0.0, wall).extend(1.0);
            let stored = if wall == 0.0 { 0.0 } else { 1.0 - z.z / z.w };
            let mut encoder = device.create_command_encoder(&Default::default());
            {
                let pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
                    label: None,
                    color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                        view: &color_view,
                        depth_slice: None,
                        resolve_target: None,
                        ops: wgpu::Operations {
                            load: wgpu::LoadOp::Clear(if black {
                                wgpu::Color::BLACK
                            } else {
                                wgpu::Color {
                                    r: 0.25,
                                    g: 0.5,
                                    b: 0.75,
                                    a: 1.0,
                                }
                            }),
                            store: wgpu::StoreOp::Store,
                        },
                    })],
                    depth_stencil_attachment: None,
                    timestamp_writes: None,
                    occlusion_query_set: None,
                    multiview_mask: None,
                });
                drop(pass);
            }
            {
                let pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
                    label: None,
                    color_attachments: &[],
                    depth_stencil_attachment: Some(wgpu::RenderPassDepthStencilAttachment {
                        view: &depth_view,
                        depth_ops: Some(wgpu::Operations {
                            load: wgpu::LoadOp::Clear(stored),
                            store: wgpu::StoreOp::Store,
                        }),
                        stencil_ops: None,
                    }),
                    timestamp_writes: None,
                    occlusion_query_set: None,
                    multiview_mask: None,
                });
                drop(pass);
            }
            assert!(reflection.record(
                &device,
                &queue,
                &mut encoder,
                &color,
                &color_view,
                &depth_view,
                size as u64
            ));
            let scene_bind = device.create_bind_group(&wgpu::BindGroupDescriptor {
                label: None,
                layout: &pipeline.get_bind_group_layout(2),
                entries: &[
                    wgpu::BindGroupEntry {
                        binding: 0,
                        resource: wgpu::BindingResource::TextureView(
                            &reflection.target.as_ref().unwrap().view,
                        ),
                    },
                    wgpu::BindGroupEntry {
                        binding: 1,
                        resource: wgpu::BindingResource::Sampler(&reflection.sampler),
                    },
                    wgpu::BindGroupEntry {
                        binding: 2,
                        resource: wgpu::BindingResource::TextureView(&depth_view),
                    },
                    wgpu::BindGroupEntry {
                        binding: 3,
                        resource: reflection.params.as_entire_binding(),
                    },
                ],
            });
            {
                let mut pass = encoder.begin_compute_pass(&Default::default());
                pass.set_pipeline(&pipeline);
                pass.set_bind_group(0, &camera_bind, &[]);
                pass.set_bind_group(1, &output_bind, &[]);
                pass.set_bind_group(2, &scene_bind, &[]);
                pass.dispatch_workgroups(1, 1, 1);
            }
            encoder.copy_buffer_to_buffer(&output, 0, &staging, 0, 7 * 16);
            queue.submit(Some(encoder.finish()));
            let read = || {
                let (send, recv) = std::sync::mpsc::channel();
                staging.slice(..).map_async(wgpu::MapMode::Read, move |r| {
                    let _ = send.send(r);
                });
                device.poll(wgpu::PollType::wait_indefinitely()).unwrap();
                recv.recv().unwrap().unwrap();
                let bytes = staging.slice(..).get_mapped_range();
                let result = bytemuck::cast_slice::<u8, [f32; 4]>(&bytes).to_vec();
                drop(bytes);
                staging.unmap();
                result
            };
            let values = read();
            assert!(values.iter().flatten().all(|v| v.is_finite()));
            if wall == -8.0 {
                assert!(
                    values[0][3] > 0.9,
                    "actual wall reflection missing: {values:?}"
                );
                let expected = if black { [0.0; 3] } else { [0.25, 0.5, 0.75] };
                for i in 0..3 {
                    assert!((values[0][i] - expected[i]).abs() < 0.001);
                }
            } else {
                assert_eq!(
                    values[0], [0.0; 4],
                    "clear/foreground/beyond-range reflection must fail closed"
                );
            }
            for value in &values[1..] {
                assert_eq!(
                    *value, [0.0; 4],
                    "rough/far/underside/offscreen/unresolved/nonfinite reflection"
                );
            }
            if wall == -8.0 && !black {
                // A depth crossing with reconstructed geometry two metres off
                // the ray is NOT a hit. Exercise the actual world-distance gate,
                // rather than accepting a projected depth discontinuity alone.
                let displaced_inverse = Mat4::from_translation(Vec3::X * 2.0) * proj.inverse();
                let bad_camera: [f32; 48] = [
                    proj.to_cols_array(),
                    Mat4::IDENTITY.to_cols_array(),
                    displaced_inverse.to_cols_array(),
                ]
                .concat()
                .try_into()
                .unwrap();
                queue.write_buffer(&camera_buffer, 0, bytemuck::cast_slice(&bad_camera));
                let mut encoder = device.create_command_encoder(&Default::default());
                {
                    let mut pass = encoder.begin_compute_pass(&Default::default());
                    pass.set_pipeline(&pipeline);
                    pass.set_bind_group(0, &camera_bind, &[]);
                    pass.set_bind_group(1, &output_bind, &[]);
                    pass.set_bind_group(2, &scene_bind, &[]);
                    pass.dispatch_workgroups(1, 1, 1);
                }
                encoder.copy_buffer_to_buffer(&output, 0, &staging, 0, 7 * 16);
                queue.submit(Some(encoder.finish()));
                assert!(
                    read().iter().all(|v| *v == [0.0; 4]),
                    "depth-only false hit leaked into puddle reflection"
                );
                queue.write_buffer(&camera_buffer, 0, bytemuck::cast_slice(&camera_bytes));
                let mut disabled = Reflections::new(&device, wgpu::TextureFormat::Rgba16Float);
                disabled.enabled = false;
                disabled.begin_frame(&queue);
                disabled.offer_box(
                    &camera(),
                    Vec3::new(-1.0, -2.0, -6.0),
                    Vec3::new(1.0, -1.0, -4.0),
                );
                assert!(!disabled.needed());
                let mut unused = device.create_command_encoder(&Default::default());
                assert!(!disabled.record(
                    &device,
                    &queue,
                    &mut unused,
                    &color,
                    &color_view,
                    &depth_view,
                    1
                ));
                assert!(
                    disabled.target.is_none(),
                    "ablation allocated a full scene snapshot"
                );
            }
            // Same allocated textures, new frame without a recorded snapshot:
            // the ACTUAL production readiness buffer must refuse stale radiance.
            reflection.begin_frame(&queue);
            assert!(!reflection.needed());
            let mut encoder = device.create_command_encoder(&Default::default());
            {
                let mut pass = encoder.begin_compute_pass(&Default::default());
                pass.set_pipeline(&pipeline);
                pass.set_bind_group(0, &camera_bind, &[]);
                pass.set_bind_group(1, &output_bind, &[]);
                pass.set_bind_group(2, &scene_bind, &[]);
                pass.dispatch_workgroups(1, 1, 1);
            }
            encoder.copy_buffer_to_buffer(&output, 0, &staging, 0, 7 * 16);
            queue.submit(Some(encoder.finish()));
            assert!(read().iter().all(|v| *v == [0.0; 4]));
        }
    }
}
