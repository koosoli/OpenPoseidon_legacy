//! (OP) Whole World terrain sun shadow: the elevation pyramid's mountains shade the Earth ground.
//!
//! Two cascades around the camera (512 x 512 each: 16 m texels over 8 km, 128 m over 65 km) hold, per texel, the
//! height a point there must be above to see the sun (`earth_shadow.wgsl`). A new map is computed into the back
//! texture a few rows a frame when the sun or the camera has moved enough, and only a finished one is shown: the
//! front texture and its placement (`shader_params`) go to shader3d's conform group (bindings 6, 7).

pub const N: u32 = 512;
const TEXELS: [f32; 2] = [16.0, 128.0];
const ROWS_PER_FRAME: u32 = 32;

#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
struct Params {
    origin: [f32; 4],
    sun: [f32; 4],
    c0: [f32; 4],
    c1: [f32; 4],
    rows: [u32; 4],
}

/// What the producer gives each frame (wgr_earth_shadow_input).
#[derive(Clone, Copy, PartialEq)]
pub struct Input {
    pub cam: [f32; 2],
    pub lat0: f32, // radians
    pub lon0: f32,
}

struct Job {
    params: Params,
    next_row: u32,
}

pub struct EarthShadow {
    pipeline: wgpu::ComputePipeline,
    layout: wgpu::BindGroupLayout,
    params_buf: wgpu::Buffer,
    views: [wgpu::TextureView; 2],
    front: usize,
    /// the front map's placement for shader3d: c0, c1, (tan sun elevation, valid, 0, 0)
    shown: [[f32; 4]; 3],
    /// bumps when the front texture changes (the conform group rebinds)
    pub generation: u64,
    job: Option<Job>,
    input: Option<Input>,
    bound: Option<(wgpu::BindGroup, [u64; 2], usize)>,
}

impl EarthShadow {
    pub fn new(device: &wgpu::Device) -> Self {
        let module = device.create_shader_module(wgpu::ShaderModuleDescriptor {
            label: Some("wgr_earth_shadow"),
            source: wgpu::ShaderSource::Wgsl(include_str!("earth_shadow.wgsl").into()),
        });
        let tex = |binding, sample_type| wgpu::BindGroupLayoutEntry {
            binding,
            visibility: wgpu::ShaderStages::COMPUTE,
            ty: wgpu::BindingType::Texture { sample_type, view_dimension: wgpu::TextureViewDimension::D2, multisampled: false },
            count: None,
        };
        let layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("wgr_earth_shadow"),
            entries: &[
                wgpu::BindGroupLayoutEntry {
                    binding: 0,
                    visibility: wgpu::ShaderStages::COMPUTE,
                    ty: wgpu::BindingType::Buffer {
                        ty: wgpu::BufferBindingType::Uniform,
                        has_dynamic_offset: false,
                        min_binding_size: wgpu::BufferSize::new(std::mem::size_of::<Params>() as u64),
                    },
                    count: None,
                },
                tex(1, wgpu::TextureSampleType::Float { filterable: false }),
                tex(2, wgpu::TextureSampleType::Float { filterable: false }),
                wgpu::BindGroupLayoutEntry {
                    binding: 3,
                    visibility: wgpu::ShaderStages::COMPUTE,
                    ty: wgpu::BindingType::StorageTexture {
                        access: wgpu::StorageTextureAccess::WriteOnly,
                        format: wgpu::TextureFormat::R32Float,
                        view_dimension: wgpu::TextureViewDimension::D2,
                    },
                    count: None,
                },
            ],
        });
        let pl = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("wgr_earth_shadow"),
            bind_group_layouts: &[Some(&layout)],
            immediate_size: 0,
        });
        let pipeline = device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
            label: Some("wgr_earth_shadow"),
            layout: Some(&pl),
            module: &module,
            entry_point: Some("cs_earth_shadow"),
            compilation_options: Default::default(),
            cache: None,
        });
        let params_buf = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("wgr_earth_shadow_params"),
            size: std::mem::size_of::<Params>() as u64,
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        let make = |label| {
            device
                .create_texture(&wgpu::TextureDescriptor {
                    label: Some(label),
                    size: wgpu::Extent3d { width: 2 * N, height: N, depth_or_array_layers: 1 },
                    mip_level_count: 1,
                    sample_count: 1,
                    dimension: wgpu::TextureDimension::D2,
                    format: wgpu::TextureFormat::R32Float,
                    usage: wgpu::TextureUsages::STORAGE_BINDING | wgpu::TextureUsages::TEXTURE_BINDING,
                    view_formats: &[],
                })
                .create_view(&wgpu::TextureViewDescriptor::default())
        };
        Self {
            pipeline,
            layout,
            params_buf,
            views: [make("wgr_earth_shadow_a"), make("wgr_earth_shadow_b")],
            front: 0,
            shown: [[0.0; 4]; 3],
            generation: 1,
            job: None,
            input: None,
            bound: None,
        }
    }

    pub fn set_input(&mut self, input: Option<Input>) {
        self.input = input;
    }

    /// The map shader3d reads and its placement (c0, c1, (tan sun elevation, valid, 0, 0)).
    pub fn front(&self) -> (&wgpu::TextureView, [[f32; 4]; 3]) {
        let valid = self.input.is_some() && self.shown[2][1] > 0.5;
        let mut shown = self.shown;
        shown[2][1] = if valid { 1.0 } else { 0.0 };
        (&self.views[self.front], shown)
    }

    /// Per frame: start a new map when the shown one is stale, and record the next rows of the one under way.
    /// `heights`: the pyramid's atlas and lookup (ids and views); `to_sun`: unit world direction to the sun.
    pub fn update(&mut self, device: &wgpu::Device, queue: &wgpu::Queue, encoder: &mut wgpu::CommandEncoder,
                  heights: Option<([u64; 2], &wgpu::TextureView, &wgpu::TextureView)>, to_sun: glam::Vec3) {
        let (Some(input), Some((ids, atlas, lookup))) = (self.input, heights) else {
            return;
        };
        let horizontal = (to_sun.x * to_sun.x + to_sun.z * to_sun.z).sqrt();
        let tan_e = if horizontal > 1e-4 { to_sun.y / horizontal } else { 1000.0 };
        if self.job.is_none() {
            let stale = self.shown[2][1] < 0.5
                || (self.shown[2][0] - tan_e).abs() > 0.004 // ~0.25 degrees of sun elevation
                || {
                    let half = TEXELS[0] * N as f32 * 0.5;
                    let centre = [self.shown[0][0] + half, self.shown[0][1] + half];
                    (input.cam[0] - centre[0]).abs() > half * 0.25 || (input.cam[1] - centre[1]).abs() > half * 0.25
                }
                || self.sun_azimuth_moved(to_sun);
            if stale {
                let cascade = |texel: f32| {
                    let span = texel * N as f32;
                    [((input.cam[0] - span * 0.5) / texel).floor() * texel, ((input.cam[1] - span * 0.5) / texel).floor() * texel,
                     texel, N as f32]
                };
                self.job = Some(Job {
                    params: Params {
                        origin: [input.lat0.sin(), input.lat0.cos(), input.lon0, 0.0],
                        sun: [to_sun.x, to_sun.y, to_sun.z, tan_e],
                        c0: cascade(TEXELS[0]),
                        c1: cascade(TEXELS[1]),
                        rows: [0, 0, 0, 0],
                    },
                    next_row: 0,
                });
            }
        }
        let back = 1 - self.front;
        let Some(job) = self.job.as_mut() else { return };
        if self.bound.as_ref().is_none_or(|(_, b_ids, b_back)| *b_ids != ids || *b_back != back) {
            let bind = device.create_bind_group(&wgpu::BindGroupDescriptor {
                label: Some("wgr_earth_shadow"),
                layout: &self.layout,
                entries: &[
                    wgpu::BindGroupEntry { binding: 0, resource: self.params_buf.as_entire_binding() },
                    wgpu::BindGroupEntry { binding: 1, resource: wgpu::BindingResource::TextureView(atlas) },
                    wgpu::BindGroupEntry { binding: 2, resource: wgpu::BindingResource::TextureView(lookup) },
                    wgpu::BindGroupEntry { binding: 3, resource: wgpu::BindingResource::TextureView(&self.views[back]) },
                ],
            });
            self.bound = Some((bind, ids, back));
        }
        let rows = ROWS_PER_FRAME.min(N - job.next_row);
        job.params.rows = [job.next_row, rows, 0, 0];
        queue.write_buffer(&self.params_buf, 0, bytemuck::bytes_of(&job.params));
        {
            let mut pass = encoder.begin_compute_pass(&wgpu::ComputePassDescriptor { label: Some("wgr_earth_shadow"), timestamp_writes: None });
            pass.set_pipeline(&self.pipeline);
            pass.set_bind_group(0, &self.bound.as_ref().unwrap().0, &[]);
            pass.dispatch_workgroups((2 * N).div_ceil(8), rows.div_ceil(8), 1);
        }
        job.next_row += rows;
        if job.next_row >= N {
            let done = self.job.take().unwrap();
            self.front = back;
            self.shown = [done.params.c0, done.params.c1, [done.params.sun[3], 1.0, done.params.sun[0], done.params.sun[2]]];
            self.generation += 1;
        }
    }

    // Has the sun swung round (azimuth) by more than ~0.5 degrees since the shown map?
    fn sun_azimuth_moved(&self, to_sun: glam::Vec3) -> bool {
        let a = glam::Vec2::new(self.shown[2][2], self.shown[2][3]);
        let b = glam::Vec2::new(to_sun.x, to_sun.z);
        if a.length_squared() < 1e-8 || b.length_squared() < 1e-8 {
            return a.length_squared() >= 1e-8 || b.length_squared() >= 1e-8;
        }
        a.normalize().dot(b.normalize()) < 0.99996
    }
}

#[cfg(test)]
mod tests {
    #[test]
    fn earth_shadow_shader_validates() {
        let module = naga::front::wgsl::parse_str(include_str!("earth_shadow.wgsl")).expect("earth_shadow.wgsl parses");
        naga::valid::Validator::new(naga::valid::ValidationFlags::all(), naga::valid::Capabilities::all())
            .validate(&module)
            .expect("earth_shadow.wgsl validates");
    }
}
