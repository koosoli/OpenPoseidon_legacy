//! Explicit developer opt-in, raw world snapshot before UI clears. No GI claim.
use crate::ffi::WgrCamera;
use std::sync::mpsc;

pub const GRID: u32 = 16;
const ROW: u32 = 256;
const BYTES: u64 = ROW as u64 * GRID as u64;
const SHADER: &str = include_str!("scene_depth.wgsl");

#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
pub struct Publication {
    pub bytes: u32,
    pub version: u32,
    pub generation: u64,
    pub frame: u64,
    pub device: u64,
    pub queue: u64,
    pub image: u64,
    pub view: u64,
    pub sample_frame: u64,
    pub sample_generation: u64,
    pub width: u32,
    pub height: u32,
    pub samples: u32,
    pub flags: u32,
    pub finite_samples: u32,
    pub nonzero_samples: u32,
    pub grid_width: u32,
    pub grid_height: u32,
    pub near_plane: f32,
    pub minimum: f32,
    pub maximum: f32,
    pub reserved: f32,
    pub projection: [f32; 16],
    pub grid: [f32; 256],
}
impl Publication {
    fn empty() -> Self {
        use bytemuck::Zeroable;
        let mut p = Self::zeroed();
        p.bytes = std::mem::size_of::<Self>() as u32;
        p.version = 1;
        p.grid_width = GRID;
        p.grid_height = GRID;
        p
    }
    fn numeric(&mut self, values: [f32; 256], frame: u64, generation: u64) {
        self.grid = values;
        self.sample_frame = frame;
        self.sample_generation = generation;
        self.finite_samples = values
            .iter()
            .filter(|v| v.is_finite() && **v >= 0.0 && **v <= 1.0)
            .count() as u32;
        self.nonzero_samples = values
            .iter()
            .filter(|v| v.is_finite() && **v > 0.0 && **v <= 1.0)
            .count() as u32;
        self.minimum = values
            .iter()
            .copied()
            .filter(|v| v.is_finite())
            .fold(f32::INFINITY, f32::min);
        self.maximum = values
            .iter()
            .copied()
            .filter(|v| v.is_finite())
            .fold(f32::NEG_INFINITY, f32::max);
        self.flags &= !2;
        if self.finite_samples == 256
            && self.nonzero_samples > 0
            && self.maximum > self.minimum
            && generation == self.generation
        {
            self.flags |= 2;
        }
        // No NaN/infinity is allowed into JSON or the cross-module diagnostic.
        if !self.minimum.is_finite() {
            self.minimum = 0.0;
        }
        if !self.maximum.is_finite() {
            self.maximum = 0.0;
        }
        for v in &mut self.grid {
            if !v.is_finite() {
                *v = 0.0;
            }
        }
    }
}

struct Gpu {
    texture: wgpu::Texture,
    view: wgpu::TextureView,
    grid_texture: wgpu::Texture,
    grid_view: wgpu::TextureView,
    layout: wgpu::BindGroupLayout,
    snapshot: wgpu::RenderPipeline,
    grid: wgpu::RenderPipeline,
}
struct Pending {
    buffer: wgpu::Buffer,
    receiver: Option<mpsc::Receiver<Result<(), wgpu::BufferAsyncError>>>,
    frame: u64,
    generation: u64,
}
pub struct State {
    gpu: Option<Gpu>,
    pending: Option<Pending>,
    pub publication: Publication,
    frame: u64,
    captured: bool,
    rejected_logged: bool,
}
impl State {
    pub fn enabled() -> bool {
        cfg!(windows)
            && matches!(
                std::env::var("WGR_RESHADE_SCENE_DEPTH").ok().as_deref(),
                Some("1" | "2")
            )
    }
    pub fn requested(&self) -> bool {
        std::env::var("WGR_RESHADE_SCENE_DEPTH").ok().as_deref() == Some("1") || native::requested()
    }
    pub fn new() -> Self {
        Self {
            gpu: None,
            pending: None,
            publication: Publication::empty(),
            frame: 0,
            captured: false,
            rejected_logged: false,
        }
    }
    pub fn invalidate(&mut self) {
        // The add-on unbinds borrowed views and waits its matching native queue.
        // No Vulkan resource may be dropped/reused while external effects use it.
        native::invalidate();
        self.gpu = None;
        self.pending = None;
        let generation = self.publication.generation;
        self.publication = Publication::empty();
        self.publication.generation = generation;
        self.captured = false;
    }
    pub fn begin(&mut self) {
        if !self.requested() && self.gpu.is_some() {
            self.invalidate();
        }
        self.frame = self.frame.checked_add(1).unwrap_or(u64::MAX);
        self.captured = false;
        self.publication.flags &= !1;
    }
    pub fn capture(
        &mut self,
        device: &wgpu::Device,
        encoder: &mut wgpu::CommandEncoder,
        source: &wgpu::TextureView,
        size: (u32, u32),
        output: (u32, u32),
        samples: u32,
        camera: &WgrCamera,
    ) {
        if !supported(size, output, samples, &camera.proj) || self.frame == u64::MAX {
            if self.gpu.is_some() {
                self.invalidate();
            }
            if !self.rejected_logged {
                eprintln!(
                    "[wgr] scene-depth snapshot rejected: requires Vulkan, 1x/4x samples, output-sized scene and finite infinite-far projection"
                );
                self.rejected_logged = true;
            }
            return;
        }
        let source_msaa_flag = if samples == 4 { 4 } else { 0 };
        if self.gpu.is_none()
            || (self.publication.width, self.publication.height) != size
            || self.publication.flags & 4 != source_msaa_flag
        {
            self.invalidate();
            let Some(generation) = self.publication.generation.checked_add(1) else {
                return;
            };
            let gpu = Gpu::new(device, size);
            let Some((native_device, queue, image, view)) =
                native::handles(device, &gpu.texture, &gpu.view)
            else {
                if !self.rejected_logged {
                    eprintln!(
                        "[wgr] scene-depth snapshot unavailable: native Vulkan handles missing"
                    );
                    self.rejected_logged = true;
                }
                return;
            };
            self.publication.generation = generation;
            self.publication.device = native_device;
            self.publication.queue = queue;
            self.publication.image = image;
            self.publication.view = view;
            self.publication.width = size.0;
            self.publication.height = size.1;
            self.publication.samples = 1;
            self.publication.flags |= source_msaa_flag;
            self.gpu = Some(gpu);
        }
        let gpu = self.gpu.as_ref().unwrap();
        let bind = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("op_scene_depth_source"),
            layout: &gpu.layout,
            entries: &[wgpu::BindGroupEntry {
                binding: 0,
                resource: wgpu::BindingResource::TextureView(source),
            }],
        });
        draw(
            encoder,
            &gpu.view,
            &gpu.snapshot,
            &bind,
            "op_world_depth_snapshot",
        );
        // A 4KiB asynchronous grid witness at most once per 60 frames; never a
        // frame-wide CPU download/reduction, and never blocks presentation.
        if self.pending.is_none() && (self.publication.sample_frame == 0 || self.frame % 60 == 0) {
            draw(
                encoder,
                &gpu.grid_view,
                &gpu.grid,
                &bind,
                "op_world_depth_grid",
            );
            let buffer = device.create_buffer(&wgpu::BufferDescriptor {
                label: Some("op_world_depth_readback"),
                size: BYTES,
                usage: wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::MAP_READ,
                mapped_at_creation: false,
            });
            encoder.copy_texture_to_buffer(
                gpu.grid_texture.as_image_copy(),
                wgpu::TexelCopyBufferInfo {
                    buffer: &buffer,
                    layout: wgpu::TexelCopyBufferLayout {
                        offset: 0,
                        bytes_per_row: Some(ROW),
                        rows_per_image: Some(GRID),
                    },
                },
                wgpu::Extent3d {
                    width: GRID,
                    height: GRID,
                    depth_or_array_layers: 1,
                },
            );
            self.pending = Some(Pending {
                buffer,
                receiver: None,
                frame: self.frame,
                generation: self.publication.generation,
            });
        }
        // wgpu explicitly documents transition_resources for native interop.
        // R32Float is a color resource: RESOURCE maps to SHADER_READ_ONLY_OPTIMAL.
        encoder.transition_resources(
            std::iter::empty(),
            std::iter::once(wgpu::TextureTransition {
                texture: &gpu.texture,
                selector: None,
                state: wgpu::TextureUses::RESOURCE,
            }),
        );
        self.publication.frame = self.frame;
        self.publication.projection = camera.proj;
        self.publication.near_plane = -camera.proj[14];
        self.publication.flags &= !8;
        if surface_fog_disabled(camera.params.fog_enabled) {
            self.publication.flags |= 8;
        }
        self.publication.flags |= 1;
        self.captured = true;
    }
    pub fn submitted(&mut self, device: &wgpu::Device) {
        if let Some(pending) = self.pending.as_mut() {
            if pending.receiver.is_none() {
                let (tx, rx) = mpsc::channel();
                pending
                    .buffer
                    .slice(..)
                    .map_async(wgpu::MapMode::Read, move |r| {
                        let _ = tx.send(r);
                    });
                pending.receiver = Some(rx);
            }
        }
        let _ = device.poll(wgpu::PollType::Poll);
        let result = self
            .pending
            .as_ref()
            .and_then(|p| p.receiver.as_ref())
            .and_then(|rx| rx.try_recv().ok());
        if let Some(result) = result {
            let pending = self.pending.take().unwrap();
            if result.is_ok() && pending.generation == self.publication.generation {
                let mapped = pending.buffer.slice(..).get_mapped_range();
                let values = std::array::from_fn(|i| {
                    let offset = (i / GRID as usize) * ROW as usize + (i % GRID as usize) * 4;
                    f32::from_le_bytes(mapped[offset..offset + 4].try_into().unwrap())
                });
                self.publication
                    .numeric(values, pending.frame, pending.generation);
                eprintln!(
                    "[wgr] scene-depth grid generation={} frame={} finite={}/256 nonzero={} min={:.9} max={:.9} near={:.6} scope=16x16-bin-centres",
                    pending.generation,
                    pending.frame,
                    self.publication.finite_samples,
                    self.publication.nonzero_samples,
                    self.publication.minimum,
                    self.publication.maximum,
                    self.publication.near_plane
                );
                drop(mapped);
                pending.buffer.unmap();
            }
        }
        if self.captured {
            native::publish(&self.publication);
        } else {
            native::publish(&Publication::empty());
        }
    }
}
fn supported(size: (u32, u32), output: (u32, u32), samples: u32, p: &[f32; 16]) -> bool {
    matches!(samples, 1 | 4)
        && size == output
        && size.0 > 0
        && size.1 > 0
        && size.0 <= 8192
        && size.1 <= 8192
        && p.iter().all(|x| x.is_finite())
        && p[0] > 0.0
        && p[5] > 0.0
        && [1, 2, 3, 4, 6, 7, 12, 13]
            .iter()
            .all(|&i| p[i].abs() < 1e-4)
        && -p[14] > 0.0
        && (p[10] - 1.0).abs() < 1e-4
        && (p[11] - 1.0).abs() < 1e-4
        && p[15].abs() < 1e-4
}
fn draw(
    encoder: &mut wgpu::CommandEncoder,
    view: &wgpu::TextureView,
    pipeline: &wgpu::RenderPipeline,
    bind: &wgpu::BindGroup,
    label: &str,
) {
    let mut pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
        label: Some(label),
        color_attachments: &[Some(wgpu::RenderPassColorAttachment {
            view,
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
    pass.set_pipeline(pipeline);
    pass.set_bind_group(0, bind, &[]);
    pass.draw(0..3, 0..1);
}
impl Gpu {
    fn new(device: &wgpu::Device, size: (u32, u32)) -> Self {
        let texture = |label: &'static str, size: (u32, u32), usage| {
            device.create_texture(&wgpu::TextureDescriptor {
                label: Some(label),
                size: wgpu::Extent3d {
                    width: size.0,
                    height: size.1,
                    depth_or_array_layers: 1,
                },
                mip_level_count: 1,
                sample_count: 1,
                dimension: wgpu::TextureDimension::D2,
                format: wgpu::TextureFormat::R32Float,
                usage: wgpu::TextureUsages::RENDER_ATTACHMENT | usage,
                view_formats: &[],
            })
        };
        let texture = texture(
            "op_world_depth_snapshot",
            size,
            wgpu::TextureUsages::TEXTURE_BINDING,
        );
        let view = texture.create_view(&Default::default());
        let grid_texture = texture_for_grid(device);
        let grid_view = grid_texture.create_view(&Default::default());
        let layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("op_scene_depth"),
            entries: &[wgpu::BindGroupLayoutEntry {
                binding: 0,
                visibility: wgpu::ShaderStages::FRAGMENT,
                ty: wgpu::BindingType::Texture {
                    sample_type: wgpu::TextureSampleType::Depth,
                    view_dimension: wgpu::TextureViewDimension::D2,
                    multisampled: false,
                },
                count: None,
            }],
        });
        let pipeline_layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("op_scene_depth"),
            bind_group_layouts: &[Some(&layout)],
            immediate_size: 0,
        });
        let shader = device.create_shader_module(wgpu::ShaderModuleDescriptor {
            label: Some("op_scene_depth"),
            source: wgpu::ShaderSource::Wgsl(SHADER.into()),
        });
        let pipeline = |entry| {
            device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
                label: Some(entry),
                layout: Some(&pipeline_layout),
                vertex: wgpu::VertexState {
                    module: &shader,
                    entry_point: Some("vs"),
                    compilation_options: Default::default(),
                    buffers: &[],
                },
                primitive: Default::default(),
                depth_stencil: None,
                multisample: Default::default(),
                fragment: Some(wgpu::FragmentState {
                    module: &shader,
                    entry_point: Some(entry),
                    compilation_options: Default::default(),
                    targets: &[Some(wgpu::ColorTargetState {
                        format: wgpu::TextureFormat::R32Float,
                        blend: None,
                        write_mask: wgpu::ColorWrites::ALL,
                    })],
                }),
                multiview_mask: None,
                cache: None,
            })
        };
        Self {
            texture,
            view,
            grid_texture,
            grid_view,
            snapshot: pipeline("snapshot"),
            grid: pipeline("grid"),
            layout,
        }
    }
}
fn texture_for_grid(device: &wgpu::Device) -> wgpu::Texture {
    device.create_texture(&wgpu::TextureDescriptor {
        label: Some("op_scene_depth_grid"),
        size: wgpu::Extent3d {
            width: GRID,
            height: GRID,
            depth_or_array_layers: 1,
        },
        mip_level_count: 1,
        sample_count: 1,
        dimension: wgpu::TextureDimension::D2,
        format: wgpu::TextureFormat::R32Float,
        usage: wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::COPY_SRC,
        view_formats: &[],
    })
}

#[cfg(windows)]
mod native {
    use super::Publication;
    use ash::vk::Handle;
    use std::ffi::c_void;
    #[link(name = "kernel32")]
    unsafe extern "system" {
        fn GetModuleHandleExW(flags: u32, name: *const u16, module: *mut *mut c_void) -> i32;
        fn GetProcAddress(module: *mut c_void, name: *const u8) -> *mut c_void;
    }
    fn symbol(name: &'static [u8]) -> *mut c_void {
        let wide: Vec<u16> = "OpenPoseidonReShade.addon64\0".encode_utf16().collect();
        let mut module = std::ptr::null_mut();
        // PIN only an already loaded add-on, never load an optional dependency.
        unsafe {
            if GetModuleHandleExW(1, wide.as_ptr(), &mut module) == 0 {
                return std::ptr::null_mut();
            }
            GetProcAddress(module, name.as_ptr())
        }
    }
    pub fn publish(p: &Publication) {
        let address = symbol(b"OpReShadePublishSceneDepthV1\0");
        if !address.is_null() {
            let call: unsafe extern "C" fn(*const Publication, u32, u32) -> u32 =
                unsafe { std::mem::transmute(address) };
            unsafe {
                call(p, std::mem::size_of::<Publication>() as u32, 1);
            }
        }
    }
    pub fn invalidate() {
        let address = symbol(b"OpReShadeInvalidateSceneDepthV1\0");
        if !address.is_null() {
            let call: unsafe extern "C" fn() = unsafe { std::mem::transmute(address) };
            unsafe {
                call();
            }
        }
    }
    pub fn requested() -> bool {
        let address = symbol(b"OpReShadeWantsSceneDepthV1\0");
        if address.is_null() {
            return false;
        }
        let call: unsafe extern "C" fn() -> u32 = unsafe { std::mem::transmute(address) };
        unsafe { call() != 0 }
    }
    pub fn handles(
        device: &wgpu::Device,
        texture: &wgpu::Texture,
        view: &wgpu::TextureView,
    ) -> Option<(u64, u64, u64, u64)> {
        // Guards only expose handles; owned wgpu objects remain in State until
        // explicit invalidation unbinds and waits the matching native queue.
        unsafe {
            let d = device.as_hal::<wgpu::hal::api::Vulkan>()?;
            let t = texture.as_hal::<wgpu::hal::api::Vulkan>()?;
            let v = view.as_hal::<wgpu::hal::api::Vulkan>()?;
            Some((
                d.raw_device().handle().as_raw(),
                d.raw_queue().as_raw(),
                t.raw_handle().as_raw(),
                v.raw_handle().as_raw(),
            ))
        }
    }
}
#[cfg(not(windows))]
mod native {
    pub fn publish(_: &super::Publication) {}
    pub fn invalidate() {}
    pub fn requested() -> bool {
        false
    }
    pub fn handles(
        _: &wgpu::Device,
        _: &wgpu::Texture,
        _: &wgpu::TextureView,
    ) -> Option<(u64, u64, u64, u64)> {
        None
    }
}

// Native frame::apply_fog_receiver and shading::shade bypass surface fog in
// this mode. Do not infer compatibility from colors, a distance threshold, or
// an absent/zero-filled inverse range; a non-finite mode fails closed.
fn surface_fog_disabled(enabled: f32) -> bool {
    enabled.is_finite() && (0.0..=0.5).contains(&enabled)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn fog_compatibility_requires_explicit_finite_disabled_native_mode() {
        for mode in [0.0, 0.5] { assert!(surface_fog_disabled(mode)); }
        for mode in [-1.0, 0.5001, 1.0, 2.0, f32::NAN, f32::INFINITY] {
            assert!(!surface_fog_disabled(mode));
        }
    }
    #[test]
    fn contract_layout_matches_cpp_and_grid_is_bounded() {
        assert_eq!(std::mem::size_of::<Publication>(), 1208);
        assert_eq!(std::mem::offset_of!(Publication, near_plane), 104);
        assert_eq!(std::mem::offset_of!(Publication, grid), 184);
        assert_eq!(BYTES, 4096);
    }
    #[test]
    fn empty_invalid_stale_and_uniform_grids_never_prove_depth() {
        let mut p = Publication::empty();
        p.generation = 3;
        for values in [[0.0; 256], [0.2; 256], [f32::NAN; 256]] {
            p.numeric(values, 7, 3);
            assert_eq!(p.flags & 2, 0);
        }
        let values = std::array::from_fn(|i| if i < 100 { 0.05 } else { 0.0 });
        p.numeric(values, 7, 2);
        assert_eq!(p.flags & 2, 0);
        p.numeric(values, 8, 3);
        assert_eq!(p.flags & 2, 2);
        assert_eq!(p.finite_samples, 256);
        assert_eq!(p.nonzero_samples, 100);
        assert_eq!(p.minimum, 0.0);
        assert_eq!(p.maximum, 0.05);
    }
    #[test]
    fn gate_rejects_msaa_scaling_and_nonmatching_projection() {
        let mut p = [0.0; 16];
        p[0] = 1.0;
        p[5] = 1.0;
        p[10] = 1.0;
        p[11] = 1.0;
        p[14] = -0.1;
        assert!(supported((1600, 900), (1600, 900), 1, &p));
        assert!(supported((1600, 900), (1600, 900), 4, &p));
        assert!(!supported((1600, 900), (1600, 900), 2, &p));
        assert!(!supported((800, 450), (1600, 900), 1, &p));
        p[8] = 0.025;
        p[9] = -0.04;
        assert!(supported((1600, 900), (1600, 900), 1, &p));
        p[1] = 0.01;
        assert!(!supported((1600, 900), (1600, 900), 1, &p));
        p[1] = 0.0;
        p[0] = 0.0;
        assert!(!supported((1600, 900), (1600, 900), 1, &p));
        p[0] = 1.0;
        p[14] = 0.1;
        assert!(!supported((1600, 900), (1600, 900), 1, &p));
    }
    #[test]
    fn snapshot_and_grid_wgsl_validate_without_gpu() {
        let module = naga::front::wgsl::parse_str(SHADER).expect("parse scene depth");
        naga::valid::Validator::new(
            naga::valid::ValidationFlags::all(),
            naga::valid::Capabilities::empty(),
        )
        .validate(&module)
        .expect("validate scene depth");
        assert_eq!(module.entry_points.len(), 3);
    }
}
