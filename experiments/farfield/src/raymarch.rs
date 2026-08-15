use crate::camera::View;
use crate::gpu::{Gpu, Stat, Timer};
use crate::world::{World, WorldGpu, BUILDING_SCALE, CANOPY_SCALE, COVER_SHIFT};
use bytemuck::{Pod, Zeroable};

#[repr(C)]
#[derive(Clone, Copy, Pod, Zeroable, Default)]
struct Params {
    cam: [f32; 4],
    fwd: [f32; 4],
    right: [f32; 4],
    up: [f32; 4],
    world: [f32; 4],
    cfg: [f32; 4],
    sun: [f32; 4],
    cover: [f32; 4],
    bounds: [f32; 4],
    screen: [f32; 4],
}

#[derive(Clone, Copy, PartialEq)]
pub enum Mode {
    Fixed = 0,
    Hier = 1,
    HierCoverHard = 2,
    HierCoverSoft = 3,
}

pub struct RayResult {
    pub ms: Stat,
    pub steps_mean: f32,
    pub steps_p99: u32,
    pub steps_max: u32,
    pub hit_frac: f32,
    pub rgba: Vec<u8>,
    pub steps_img: Vec<u32>,
}

pub struct Raymarcher {
    pipeline: wgpu::ComputePipeline,
    layout: wgpu::BindGroupLayout,
    ubo: wgpu::Buffer,
    color: wgpu::Texture,
    color_view: wgpu::TextureView,
    steps: wgpu::Texture,
    steps_view: wgpu::TextureView,
    pub width: u32,
    pub height: u32,
}

impl Raymarcher {
    pub fn new(gpu: &Gpu, width: u32, height: u32) -> Self {
        let module = gpu.shader("raymarch", include_str!("raymarch.wgsl"));
        let layout = gpu
            .device
            .create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
                label: Some("raymarch-bgl"),
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
                    tex(1),
                    tex(2),
                    tex(3),
                    storage_tex(4, wgpu::TextureFormat::Rgba8Unorm),
                    storage_tex(5, wgpu::TextureFormat::R32Uint),
                ],
            });
        let pl = gpu
            .device
            .create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
                label: Some("raymarch-pl"),
                bind_group_layouts: &[Some(&layout)],
                immediate_size: 0,
            });
        let pipeline = gpu
            .device
            .create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
                label: Some("raymarch"),
                layout: Some(&pl),
                module: &module,
                entry_point: Some("main"),
                compilation_options: Default::default(),
                cache: None,
            });
        let ubo = gpu.device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("raymarch-ubo"),
            size: std::mem::size_of::<Params>() as u64,
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        let (color, color_view) = target(gpu, width, height, wgpu::TextureFormat::Rgba8Unorm);
        let (steps, steps_view) = target(gpu, width, height, wgpu::TextureFormat::R32Uint);
        Self { pipeline, layout, ubo, color, color_view, steps, steps_view, width, height }
    }

    pub fn run(
        &self,
        gpu: &Gpu,
        world: &World,
        wg: &WorldGpu,
        view: &View,
        mode: Mode,
        max_steps: u32,
        far: f32,
        fixed_step: f32,
        frames: usize,
        nearest: bool,
    ) -> RayResult {
        let b = view.basis();
        let cn = world.spec.n >> COVER_SHIFT;
        let p = Params {
            cam: [view.eye.x, view.eye.y, view.eye.z, view.tan_half_fov()],
            fwd: [b.fwd.x, b.fwd.y, b.fwd.z, 0.0],
            right: [b.right.x, b.right.y, b.right.z, 0.0],
            up: [b.up.x, b.up.y, b.up.z, 0.0],
            world: [
                world.spec.grid,
                world.spec.n as f32,
                world.levels() as f32,
                world.spec.extent_m(),
            ],
            cfg: [max_steps as f32, far, mode as i32 as f32, fixed_step],
            sun: [0.42, 0.72, 0.55, 0.0],
            cover: [
                cn as f32,
                world.spec.grid * (1 << COVER_SHIFT) as f32,
                CANOPY_SCALE,
                BUILDING_SCALE,
            ],
            bounds: [world.h_min, world.h_max, 0.0, if nearest { 1.0 } else { 0.0 }],
            screen: [self.width as f32, self.height as f32, 0.0, 0.0],
        };
        gpu.queue.write_buffer(&self.ubo, 0, bytemuck::bytes_of(&p));

        let bg = gpu.device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("raymarch-bg"),
            layout: &self.layout,
            entries: &[
                wgpu::BindGroupEntry { binding: 0, resource: self.ubo.as_entire_binding() },
                wgpu::BindGroupEntry {
                    binding: 1,
                    resource: wgpu::BindingResource::TextureView(&wg.height_view),
                },
                wgpu::BindGroupEntry {
                    binding: 2,
                    resource: wgpu::BindingResource::TextureView(&wg.top_view),
                },
                wgpu::BindGroupEntry {
                    binding: 3,
                    resource: wgpu::BindingResource::TextureView(&wg.cover_view),
                },
                wgpu::BindGroupEntry {
                    binding: 4,
                    resource: wgpu::BindingResource::TextureView(&self.color_view),
                },
                wgpu::BindGroupEntry {
                    binding: 5,
                    resource: wgpu::BindingResource::TextureView(&self.steps_view),
                },
            ],
        });

        let timer = Timer::new(gpu);
        let mut samples = Vec::new();
        for _ in 0..frames {
            let mut enc = gpu
                .device
                .create_command_encoder(&wgpu::CommandEncoderDescriptor { label: None });
            {
                let mut cp = enc.begin_compute_pass(&wgpu::ComputePassDescriptor {
                    label: Some("raymarch"),
                    timestamp_writes: timer.writes().map(|w| wgpu::ComputePassTimestampWrites {
                        query_set: w.query_set,
                        beginning_of_pass_write_index: w.beginning_of_pass_write_index,
                        end_of_pass_write_index: w.end_of_pass_write_index,
                    }),
                });
                cp.set_pipeline(&self.pipeline);
                cp.set_bind_group(0, &bg, &[]);
                cp.dispatch_workgroups(self.width.div_ceil(8), self.height.div_ceil(8), 1);
            }
            timer.resolve_into(&mut enc);
            gpu.queue.submit([enc.finish()]);
            gpu.device.poll(wgpu::PollType::wait_indefinitely()).unwrap();
            if let Some(ms) = timer.read_ms(gpu) {
                samples.push(ms);
            }
        }

        let rgba = read_texture(gpu, &self.color, self.width, self.height, 4);
        let steps_bytes = read_texture(gpu, &self.steps, self.width, self.height, 4);
        let steps_img: Vec<u32> = bytemuck::cast_slice(&steps_bytes).to_vec();

        let mut sorted = steps_img.clone();
        sorted.sort_unstable();
        let n = sorted.len();
        let sum: u64 = sorted.iter().map(|&v| v as u64).sum();
        let hits = steps_img.iter().filter(|&&v| v > 0).count();

        RayResult {
            ms: Stat::of(samples),
            steps_mean: sum as f32 / n as f32,
            steps_p99: sorted[(n as f32 * 0.99) as usize],
            steps_max: *sorted.last().unwrap(),
            hit_frac: hits as f32 / n as f32,
            rgba,
            steps_img,
        }
    }
}

fn tex(binding: u32) -> wgpu::BindGroupLayoutEntry {
    wgpu::BindGroupLayoutEntry {
        binding,
        visibility: wgpu::ShaderStages::COMPUTE,
        ty: wgpu::BindingType::Texture {
            sample_type: if binding == 3 {
                wgpu::TextureSampleType::Float { filterable: true }
            } else {
                wgpu::TextureSampleType::Float { filterable: false }
            },
            view_dimension: wgpu::TextureViewDimension::D2,
            multisampled: false,
        },
        count: None,
    }
}

fn storage_tex(binding: u32, format: wgpu::TextureFormat) -> wgpu::BindGroupLayoutEntry {
    wgpu::BindGroupLayoutEntry {
        binding,
        visibility: wgpu::ShaderStages::COMPUTE,
        ty: wgpu::BindingType::StorageTexture {
            access: wgpu::StorageTextureAccess::WriteOnly,
            format,
            view_dimension: wgpu::TextureViewDimension::D2,
        },
        count: None,
    }
}

pub fn target(
    gpu: &Gpu,
    w: u32,
    h: u32,
    format: wgpu::TextureFormat,
) -> (wgpu::Texture, wgpu::TextureView) {
    let t = gpu.device.create_texture(&wgpu::TextureDescriptor {
        label: Some("target"),
        size: wgpu::Extent3d { width: w, height: h, depth_or_array_layers: 1 },
        mip_level_count: 1,
        sample_count: 1,
        dimension: wgpu::TextureDimension::D2,
        format,
        usage: wgpu::TextureUsages::STORAGE_BINDING
            | wgpu::TextureUsages::COPY_SRC
            | wgpu::TextureUsages::RENDER_ATTACHMENT
            | wgpu::TextureUsages::TEXTURE_BINDING,
        view_formats: &[],
    });
    let v = t.create_view(&wgpu::TextureViewDescriptor::default());
    (t, v)
}

pub fn read_texture(gpu: &Gpu, tex: &wgpu::Texture, w: u32, h: u32, bpp: u32) -> Vec<u8> {
    let unpadded = w * bpp;
    let padded = unpadded.div_ceil(256) * 256;
    let buf = gpu.device.create_buffer(&wgpu::BufferDescriptor {
        label: Some("tex-readback"),
        size: (padded * h) as u64,
        usage: wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::COPY_SRC,
        mapped_at_creation: false,
    });
    let mut enc = gpu
        .device
        .create_command_encoder(&wgpu::CommandEncoderDescriptor { label: None });
    enc.copy_texture_to_buffer(
        wgpu::TexelCopyTextureInfo {
            texture: tex,
            mip_level: 0,
            origin: wgpu::Origin3d::ZERO,
            aspect: wgpu::TextureAspect::All,
        },
        wgpu::TexelCopyBufferInfo {
            buffer: &buf,
            layout: wgpu::TexelCopyBufferLayout {
                offset: 0,
                bytes_per_row: Some(padded),
                rows_per_image: Some(h),
            },
        },
        wgpu::Extent3d { width: w, height: h, depth_or_array_layers: 1 },
    );
    gpu.queue.submit([enc.finish()]);
    let padded_bytes = gpu.read_buffer(&buf, (padded * h) as u64);
    let mut out = Vec::with_capacity((unpadded * h) as usize);
    for y in 0..h {
        let s = (y * padded) as usize;
        out.extend_from_slice(&padded_bytes[s..s + unpadded as usize]);
    }
    out
}
