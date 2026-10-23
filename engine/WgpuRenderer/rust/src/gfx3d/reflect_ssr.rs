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
    /// Tunables for an in-game A/B without a rebuild: x = share of the normal map's tilt the
    /// reflected direction keeps (WGR_REFLECT_NMAP, default 0.35), yzw unused.
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

pub(crate) struct ReflectSsr {
    tex: Option<wgpu::Texture>,
    view: Option<wgpu::TextureView>,
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
            ..Default::default()
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
        let tex = device.create_texture(&wgpu::TextureDescriptor {
            label: Some("wgr_reflect_ssr_prev_color"),
            size: wgpu::Extent3d { width: size.0, height: size.1, depth_or_array_layers: 1 },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: HDR_FORMAT,
            usage: wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::COPY_DST,
            view_formats: &[],
        });
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
            tune: [normal_map_share(), 0.0, 0.0, 0.0],
        };
        queue.write_buffer(&self.params, 0, bytemuck::bytes_of(&params));
    }
}

const HDR_FORMAT: wgpu::TextureFormat = wgpu::TextureFormat::Rgba16Float;

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
