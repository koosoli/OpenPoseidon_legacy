//! REFLECT-001 phase E: screen-space reflections for classified object surfaces (paint, chrome,
//! glass, lamps) in the per-draw object pass.
//!
//! Phase D reflects a prefiltered SKY: nothing standing in the world -- trees, houses, the next
//! car, the ground a door is parked on -- can appear in a bonnet that way. Vehicles are drawn
//! INSIDE the opaque pass, so unlike water (which draws after an opaque snapshot) they cannot
//! read this frame's lit image. The standard answer, and the one used here:
//!
//! * the ray is marched against THIS frame's geometry -- GTAO's linear view-Z chain
//!   (gtao_depth_mips.rs, mip 0), built from the depth prepass before the colour pass;
//! * the colour at the hit is read from the PREVIOUS frame's lit HDR image (`prev_color`, a
//!   copy taken at the end of every frame), reprojected through the previous camera.
//!
//! Both inputs belong to the main scene camera only; the shader gates on `frame.gtao.x`, which
//! is 1 exactly for that camera on frames GTAO ran (the planar reflection camera, the weapon
//! segment and cockpit views keep the phase-D environment).
//!
//! Off unless WGR_REFLECT / WGR_REFLECT_DEBUG (sky::env_prefilter::enabled): no copy, no
//! march, params.misc.x = 0.

#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
pub(crate) struct SsrParams {
    /// Previous frame's clip space from THIS frame's camera-relative position:
    /// prev_proj * prev_view * translate(cur_cam - prev_cam). Column-major.
    pub reproj: [f32; 16],
    /// x = enabled (1/0), y = width, z = height, w = max march distance (m).
    pub misc: [f32; 4],
    /// x = share of the normal map's tilt the reflected direction keeps (WGR_REFLECT_NMAP,
    /// default 0.35, for an in-game A/B without a rebuild); y = highest mip of `prev_color`
    /// (the blur a rough lobe may ask for); zw unused.
    pub tune: [f32; 4],
}
const _: () = assert!(std::mem::size_of::<SsrParams>() == 96);

fn env_f32(name: &str, default: f32) -> f32 {
    std::env::var(name).ok().and_then(|v| v.trim().parse::<f32>().ok()).filter(|v| v.is_finite()).unwrap_or(default)
}

/// WGR_REFLECT_SSR=0 keeps phase D only (A/B of the screen-space part).
fn ssr_switch() -> bool {
    static ON: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *ON.get_or_init(|| env_f32("WGR_REFLECT_SSR", 1.0) != 0.0)
}

fn normal_map_share() -> f32 {
    static V: std::sync::OnceLock<f32> = std::sync::OnceLock::new();
    *V.get_or_init(|| env_f32("WGR_REFLECT_NMAP", 0.35).clamp(0.0, 1.0))
}

pub(crate) const SSR_MAX_DISTANCE: f32 = 80.0;
/// E.3: `prev_color` carries a box-filtered mip chain so a rough lobe reads the world blurred
/// by its own width -- a clear coat sharp, the paint underneath soft, as in the reference
/// renders -- instead of the full-resolution image at every roughness.
pub(crate) const PREV_MIPS_MAX: u32 = 6;

fn mip_count(width: u32, height: u32) -> u32 {
    (32 - width.max(height).max(1).leading_zeros()).min(PREV_MIPS_MAX)
}

struct MipLevel {
    target: wgpu::TextureView,
    bind: wgpu::BindGroup,
}

pub(crate) struct ReflectSsr {
    tex: Option<wgpu::Texture>,
    view: Option<wgpu::TextureView>,
    mips: u32,
    levels: Vec<MipLevel>,
    down_pipeline: wgpu::RenderPipeline,
    down_layout: wgpu::BindGroupLayout,
    down_sampler: wgpu::Sampler,
    size: (u32, u32),
    /// The copy target holds a real frame (false after create/resize until the first copy).
    valid: bool,
    /// Bumped whenever `view` changes, so the group-1 binds know to rebuild.
    pub(crate) generation: u64,
    pub(crate) dummy_color: wgpu::TextureView,
    pub(crate) dummy_depth: wgpu::TextureView,
    pub(crate) sampler: wgpu::Sampler,
    pub(crate) params: wgpu::Buffer,
}

impl ReflectSsr {
    pub(crate) fn new(device: &wgpu::Device) -> Self {
        let one = |label: &str, format: wgpu::TextureFormat| {
            device
                .create_texture(&wgpu::TextureDescriptor {
                    label: Some(label),
                    size: wgpu::Extent3d { width: 1, height: 1, depth_or_array_layers: 1 },
                    mip_level_count: 1,
                    sample_count: 1,
                    dimension: wgpu::TextureDimension::D2,
                    format,
                    usage: wgpu::TextureUsages::TEXTURE_BINDING,
                    view_formats: &[],
                })
                .create_view(&wgpu::TextureViewDescriptor::default())
        };
        let sampler = device.create_sampler(&wgpu::SamplerDescriptor {
            label: Some("wgr_reflect_ssr_sampler"),
            address_mode_u: wgpu::AddressMode::ClampToEdge,
            address_mode_v: wgpu::AddressMode::ClampToEdge,
            address_mode_w: wgpu::AddressMode::ClampToEdge,
            mag_filter: wgpu::FilterMode::Linear,
            min_filter: wgpu::FilterMode::Linear,
            mipmap_filter: wgpu::MipmapFilterMode::Linear,
            ..Default::default()
        });
        let down_sampler = device.create_sampler(&wgpu::SamplerDescriptor {
            label: Some("wgr_reflect_ssr_down_sampler"),
            address_mode_u: wgpu::AddressMode::ClampToEdge,
            address_mode_v: wgpu::AddressMode::ClampToEdge,
            mag_filter: wgpu::FilterMode::Linear,
            min_filter: wgpu::FilterMode::Linear,
            ..Default::default()
        });
        let down_layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("wgr_reflect_ssr_down_layout"),
            entries: &[
                wgpu::BindGroupLayoutEntry {
                    binding: 0,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Float { filterable: true },
                        view_dimension: wgpu::TextureViewDimension::D2,
                        multisampled: false,
                    },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 1,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Filtering),
                    count: None,
                },
            ],
        });
        let module = device.create_shader_module(wgpu::ShaderModuleDescriptor {
            label: Some("wgr_reflect_ssr_down"),
            source: wgpu::ShaderSource::Wgsl(DOWN_WGSL.into()),
        });
        let down_pl = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("wgr_reflect_ssr_down"),
            bind_group_layouts: &[Some(&down_layout)],
            immediate_size: 0,
        });
        let down_pipeline = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
            label: Some("wgr_reflect_ssr_down"),
            layout: Some(&down_pl),
            vertex: wgpu::VertexState {
                module: &module,
                entry_point: Some("vs_main"),
                buffers: &[],
                compilation_options: Default::default(),
            },
            primitive: wgpu::PrimitiveState::default(),
            depth_stencil: None,
            multisample: wgpu::MultisampleState::default(),
            fragment: Some(wgpu::FragmentState {
                module: &module,
                entry_point: Some("fs_down"),
                targets: &[Some(wgpu::ColorTargetState {
                    format: HDR_FORMAT,
                    blend: None,
                    write_mask: wgpu::ColorWrites::ALL,
                })],
                compilation_options: Default::default(),
            }),
            multiview_mask: None,
            cache: None,
        });
        let params = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("wgr_reflect_ssr_params"),
            size: std::mem::size_of::<SsrParams>() as u64,
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        Self {
            tex: None,
            view: None,
            mips: 1,
            levels: Vec::new(),
            down_pipeline,
            down_layout,
            down_sampler,
            size: (0, 0),
            valid: false,
            generation: 0,
            dummy_color: one("wgr_reflect_ssr_dummy_color", HDR_FORMAT),
            dummy_depth: one("wgr_reflect_ssr_dummy_depth", wgpu::TextureFormat::R32Float),
            sampler,
            params,
        }
    }

    /// The previous-frame colour to bind, or the 1x1 dummy.
    pub(crate) fn color_view(&self) -> &wgpu::TextureView {
        self.view.as_ref().unwrap_or(&self.dummy_color)
    }

    /// (Re)allocate the copy target for the HDR scene size. Invalidates its contents.
    pub(crate) fn ensure(&mut self, device: &wgpu::Device, width: u32, height: u32) {
        let size = (width.max(1), height.max(1));
        if self.tex.is_some() && self.size == size {
            return;
        }
        let mips = mip_count(size.0, size.1);
        let tex = device.create_texture(&wgpu::TextureDescriptor {
            label: Some("wgr_reflect_ssr_prev_color"),
            size: wgpu::Extent3d { width: size.0, height: size.1, depth_or_array_layers: 1 },
            mip_level_count: mips,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: HDR_FORMAT,
            usage: wgpu::TextureUsages::TEXTURE_BINDING
                | wgpu::TextureUsages::COPY_DST
                | wgpu::TextureUsages::RENDER_ATTACHMENT,
            view_formats: &[],
        });
        let mip_view = |level: u32| {
            tex.create_view(&wgpu::TextureViewDescriptor {
                label: Some("wgr_reflect_ssr_prev_mip"),
                base_mip_level: level,
                mip_level_count: Some(1),
                ..Default::default()
            })
        };
        self.levels = (1..mips)
            .map(|level| {
                let src = mip_view(level - 1);
                let bind = device.create_bind_group(&wgpu::BindGroupDescriptor {
                    label: Some("wgr_reflect_ssr_down_bind"),
                    layout: &self.down_layout,
                    entries: &[
                        wgpu::BindGroupEntry { binding: 0, resource: wgpu::BindingResource::TextureView(&src) },
                        wgpu::BindGroupEntry { binding: 1, resource: wgpu::BindingResource::Sampler(&self.down_sampler) },
                    ],
                });
                MipLevel { target: mip_view(level), bind }
            })
            .collect();
        self.mips = mips;
        self.view = Some(tex.create_view(&wgpu::TextureViewDescriptor::default()));
        self.tex = Some(tex);
        self.size = size;
        self.valid = false;
        self.generation += 1;
    }

    /// Keep this frame's lit HDR image for the next frame's reflections. `src` must be the
    /// single-sample HDR scene (the target itself at 1x, the resolve under MSAA) at `size`.
    pub(crate) fn copy_from(&mut self, encoder: &mut wgpu::CommandEncoder, src: &wgpu::Texture) {
        let Some(dst) = self.tex.as_ref() else {
            return;
        };
        if (src.width(), src.height()) != self.size || src.sample_count() != 1 || src.format() != HDR_FORMAT {
            self.valid = false;
            return;
        }
        encoder.copy_texture_to_texture(
            src.as_image_copy(),
            dst.as_image_copy(),
            wgpu::Extent3d { width: self.size.0, height: self.size.1, depth_or_array_layers: 1 },
        );
        // Each level from the one above: the destination texel centre sits on the corner of
        // four source texels, so one bilinear tap is their box average.
        for level in &self.levels {
            let mut pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
                label: Some("wgr_reflect_ssr_down"),
                color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                    view: &level.target,
                    depth_slice: None,
                    resolve_target: None,
                    ops: wgpu::Operations { load: wgpu::LoadOp::Clear(wgpu::Color::BLACK), store: wgpu::StoreOp::Store },
                })],
                depth_stencil_attachment: None,
                timestamp_writes: None,
                occlusion_query_set: None,
                multiview_mask: None,
            });
            pass.set_pipeline(&self.down_pipeline);
            pass.set_bind_group(0, &level.bind, &[]);
            pass.draw(0..3, 0..1);
        }
        self.valid = true;
    }

    /// Write this frame's parameters. `reproj` is None while the camera history is invalid
    /// (first frame, resize, teleport) -- the march is then off for the frame.
    pub(crate) fn upload(&self, queue: &wgpu::Queue, reproj: Option<[f32; 16]>) {
        let on = crate::sky::env_prefilter::enabled() && ssr_switch() && self.valid && reproj.is_some();
        let params = SsrParams {
            reproj: reproj.unwrap_or([0.0; 16]),
            misc: [
                if on { 1.0 } else { 0.0 },
                self.size.0 as f32,
                self.size.1 as f32,
                SSR_MAX_DISTANCE,
            ],
            tune: [normal_map_share(), self.mips.saturating_sub(1) as f32, 0.0, 0.0],
        };
        queue.write_buffer(&self.params, 0, bytemuck::bytes_of(&params));
    }
}

const HDR_FORMAT: wgpu::TextureFormat = wgpu::TextureFormat::Rgba16Float;

const DOWN_WGSL: &str = r#"
@group(0) @binding(0) var src: texture_2d<f32>;
@group(0) @binding(1) var samp: sampler;

struct VsOut {
    @builtin(position) pos: vec4<f32>,
    @location(0) uv: vec2<f32>,
};

@vertex
fn vs_main(@builtin(vertex_index) i: u32) -> VsOut {
    let xy = vec2<f32>(f32((i << 1u) & 2u), f32(i & 2u));
    var o: VsOut;
    o.pos = vec4<f32>(xy * 2.0 - vec2<f32>(1.0), 0.0, 1.0);
    o.uv = vec2<f32>(xy.x, 1.0 - xy.y);
    return o;
}

@fragment
fn fs_down(in: VsOut) -> @location(0) vec4<f32> {
    return vec4<f32>(textureSampleLevel(src, samp, in.uv, 0.0).rgb, 1.0);
}
"#;

/// prev_proj * prev_view * translate(cur_cam - prev_cam): a point given relative to THIS
/// frame's camera, into the previous frame's clip space. Views are camera-relative with their
/// translation zeroed, so the camera's own travel is the translation.
pub(crate) fn reprojection(
    prev_view: &[f32; 16],
    prev_proj: &[f32; 16],
    cur_cam: [f32; 3],
    prev_cam: [f32; 3],
) -> [f32; 16] {
    let t = glam::Mat4::from_translation(glam::Vec3::new(
        cur_cam[0] - prev_cam[0],
        cur_cam[1] - prev_cam[1],
        cur_cam[2] - prev_cam[2],
    ));
    (glam::Mat4::from_cols_array(prev_proj) * glam::Mat4::from_cols_array(prev_view) * t).to_cols_array()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn reprojection_moves_a_point_with_the_camera() {
        // Identity view/proj, camera moved +2 m in x: a point 1 m ahead of the CURRENT camera
        // was 3 m from the previous one along x.
        let id = glam::Mat4::IDENTITY.to_cols_array();
        let m = glam::Mat4::from_cols_array(&reprojection(&id, &id, [2.0, 0.0, 0.0], [0.0, 0.0, 0.0]));
        let p = m * glam::Vec4::new(1.0, 0.0, 0.0, 1.0);
        assert!((p.x - 3.0).abs() < 1e-6 && p.w == 1.0);
    }

    #[test]
    fn downsample_shader_validates_and_mips_are_capped() {
        let module = naga::front::wgsl::parse_str(DOWN_WGSL).expect("parse");
        naga::valid::Validator::new(naga::valid::ValidationFlags::all(), naga::valid::Capabilities::all())
            .validate(&module)
            .expect("validate");
        assert_eq!(mip_count(1600, 900), PREV_MIPS_MAX);
        assert_eq!(mip_count(1, 1), 1);
        assert_eq!(mip_count(8, 4), 4);
    }

    #[test]
    fn object_shader_declares_the_ssr_bindings_and_gate() {
        let src = include_str!("shader3d.wgsl");
        for b in [
            "@group(1) @binding(5) var ssr_prev_color: texture_2d<f32>;",
            "@group(1) @binding(6) var ssr_depth: texture_2d<f32>;",
            "@group(1) @binding(7) var ssr_samp: sampler;",
            "@group(1) @binding(8) var<uniform> ssr: SsrParams;",
        ] {
            assert!(src.contains(b), "missing {b}");
        }
        // Main scene camera only (GTAO's per-camera gate), and only with the switch on.
        assert!(src.contains("if (ssr.misc.x < 0.5 || frame.gtao.x < 0.5"));
    }
}
