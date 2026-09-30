//! Presentation of simulation-owned rainfall runoff; independent of either ocean backend.
use crate::{
    ffi::{WgrCamera, WgrRainWaterParams, WgrVec4},
    terrain::TerrainConformParams,
};
use wgpu::util::DeviceExt;

const MAX_CELLS: usize = 1024 * 1024;
const MAX_DRAW_CELLS: usize = 65_536;

fn grid_identity(
    p: &WgrRainWaterParams,
    revision: u64,
    width: u32,
    height: u32,
) -> (u64, u64, [f32; 3], u32, u32) {
    (
        p.generation,
        revision,
        [p.domain[0], p.domain[1], p.domain[2]],
        width,
        height,
    )
}

fn dimensions(p: &WgrRainWaterParams) -> Option<(u32, u32)> {
    if !p
        .domain
        .iter()
        .chain(p.control.iter())
        .all(|v| v.is_finite())
        || p.domain[2] <= 0.0
        || p.control[0] < 2.0
        || p.control[1] < 2.0
        || p.control[0].fract() != 0.0
        || p.control[1].fract() != 0.0
        || p.control[0] > 1024.0
        || p.control[1] > 1024.0
    {
        return None;
    }
    let (w, h) = (p.control[0] as u32, p.control[1] as u32);
    ((w as usize) * (h as usize) <= MAX_CELLS).then_some((w, h))
}

fn valid_cells(cells: &[WgrVec4], width: u32, height: u32) -> bool {
    cells.len() == width as usize * height as usize
        && cells.iter().all(|c| {
            c.iter().all(|v| v.is_finite())
                && c[1] >= 0.0
                && c[1] <= 100.0
                && c[2].abs() <= 100.0
                && c[3].abs() <= 100.0
        })
}

// Camera selection never alters the grid or its lifetime. The rectangle is
// bounded even for an unusually fine mod grid; no camera-owned hydrology.
fn visible_cells(cells: &[WgrVec4], p: &WgrRainWaterParams, camera: [f32; 3]) -> Vec<u32> {
    let Some((w, h)) = dimensions(p) else {
        return vec![];
    };
    if cells.len() != w as usize * h as usize || !camera.iter().all(|v| v.is_finite()) {
        return vec![];
    }
    let spacing = p.domain[2];
    let relative = [
        (camera[0] - p.domain[0]) / spacing,
        (camera[2] - p.domain[1]) / spacing,
    ];
    if relative
        .iter()
        .any(|v| !v.is_finite() || v.abs() > 1_000_000.0)
    {
        return vec![];
    }
    let radius = (600.0 / spacing).ceil().clamp(1.0, 127.0) as i32;
    let cx = relative[0].floor() as i32;
    let cz = relative[1].floor() as i32;
    let mut selected = Vec::new();
    for z in (cz - radius).max(0)..=(cz + radius).min(h as i32 - 2) {
        for x in (cx - radius).max(0)..=(cx + radius).min(w as i32 - 2) {
            let i = z as usize * w as usize + x as usize;
            if [i, i + 1, i + w as usize, i + w as usize + 1]
                .iter()
                .any(|&i| cells[i][1] > 0.001)
            {
                selected.push(i as u32);
                if selected.len() == MAX_DRAW_CELLS {
                    return selected;
                }
            }
        }
    }
    selected
}

pub struct RainWater {
    layout: wgpu::BindGroupLayout,
    terrain_layout: wgpu::BindGroupLayout,
    empty_bind: wgpu::BindGroup,
    params: wgpu::Buffer,
    terrain_params: wgpu::Buffer,
    pipeline: wgpu::RenderPipeline,
    texture: Option<wgpu::Texture>,
    bind: Option<wgpu::BindGroup>,
    terrain_bind: Option<wgpu::BindGroup>,
    terrain_generation: u64,
    instance: wgpu::Buffer,
    instance_capacity: u64,
    count: u32,
    grid: Vec<WgrVec4>,
    uploaded: Option<(u64, u64, [f32; 3], u32, u32)>,
    current: WgrRainWaterParams,
    enabled: bool,
}

impl RainWater {
    pub fn new(
        device: &wgpu::Device,
        camera: &wgpu::BindGroupLayout,
        format: wgpu::TextureFormat,
        samples: u32,
        composer: &mut naga_oil::compose::Composer,
    ) -> Self {
        let texture_entry = |binding| wgpu::BindGroupLayoutEntry {
            binding,
            visibility: wgpu::ShaderStages::VERTEX_FRAGMENT,
            ty: wgpu::BindingType::Texture {
                sample_type: wgpu::TextureSampleType::Float { filterable: false },
                view_dimension: wgpu::TextureViewDimension::D2,
                multisampled: false,
            },
            count: None,
        };
        let uniform_entry = |binding| wgpu::BindGroupLayoutEntry {
            binding,
            visibility: wgpu::ShaderStages::VERTEX_FRAGMENT,
            ty: wgpu::BindingType::Buffer {
                ty: wgpu::BufferBindingType::Uniform,
                has_dynamic_offset: false,
                min_binding_size: None,
            },
            count: None,
        };
        let layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("rain_water_grid_layout"),
            entries: &[texture_entry(0), uniform_entry(1)],
        });
        let terrain_layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("rain_water_bare_terrain_layout"),
            entries: &[texture_entry(0), uniform_entry(1)],
        });
        let empty = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("rain_water_empty"),
            entries: &[],
        });
        let empty_bind = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("rain_water_empty"),
            layout: &empty,
            entries: &[],
        });
        let pipeline_layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("rain_water_pipeline_layout"),
            bind_group_layouts: &[
                Some(camera),
                Some(&layout),
                Some(&empty),
                Some(&empty),
                Some(&terrain_layout),
            ],
            immediate_size: 0,
        });
        let shader = crate::shaders::make_module(
            device,
            composer,
            "rain_water",
            include_str!("rain_water.wgsl"),
            "rain_water.wgsl",
        );
        let attributes = [wgpu::VertexAttribute {
            format: wgpu::VertexFormat::Uint32,
            offset: 0,
            shader_location: 0,
        }];
        let pipeline = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
            label: Some("rain_water_pipeline"),
            layout: Some(&pipeline_layout),
            vertex: wgpu::VertexState {
                module: &shader,
                entry_point: Some("vs_main"),
                compilation_options: Default::default(),
                buffers: &[wgpu::VertexBufferLayout {
                    array_stride: 4,
                    step_mode: wgpu::VertexStepMode::Instance,
                    attributes: &attributes,
                }],
            },
            primitive: wgpu::PrimitiveState {
                cull_mode: None,
                ..Default::default()
            },
            depth_stencil: Some(wgpu::DepthStencilState {
                format: crate::gfx3d::depth_format(device),
                depth_write_enabled: Some(false),
                depth_compare: Some(wgpu::CompareFunction::GreaterEqual),
                stencil: Default::default(),
                bias: Default::default(),
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
                    blend: Some(wgpu::BlendState::ALPHA_BLENDING),
                    write_mask: wgpu::ColorWrites::ALL,
                })],
            }),
            multiview_mask: None,
            cache: None,
        });
        let params = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("rain_water_params"),
            size: 48,
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        let terrain_params = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("rain_water_terrain_params"),
            size: 48,
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        let instance = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
            label: Some("rain_water_instances"),
            contents: bytemuck::cast_slice(&[0u32]),
            usage: wgpu::BufferUsages::VERTEX | wgpu::BufferUsages::COPY_DST,
        });
        Self {
            layout,
            terrain_layout,
            empty_bind,
            params,
            terrain_params,
            pipeline,
            texture: None,
            bind: None,
            terrain_bind: None,
            terrain_generation: u64::MAX,
            instance,
            instance_capacity: 4,
            count: 0,
            grid: vec![],
            uploaded: None,
            current: bytemuck::Zeroable::zeroed(),
            enabled: std::env::var("WGR_RAIN_WATER").as_deref() != Ok("0"),
        }
    }

    pub fn set_grid(
        &mut self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        p: WgrRainWaterParams,
        cells: &[WgrVec4],
        revision: u64,
    ) -> bool {
        self.current = p;
        if !self.enabled || p.control[3] < 0.5 {
            self.count = 0;
            self.grid.clear();
            self.uploaded = None;
            return true;
        }
        let Some((w, h)) = dimensions(&p) else {
            self.count = 0;
            self.grid.clear();
            self.uploaded = None;
            return false;
        };
        let key = grid_identity(&p, revision, w, h);
        if self.uploaded != Some(key) || !cells.is_empty() {
            if !valid_cells(cells, w, h) {
                self.count = 0;
                self.grid.clear();
                self.uploaded = None;
                return false;
            }
            let resize = self
                .texture
                .as_ref()
                .is_none_or(|t| t.width() != w || t.height() != h);
            if resize {
                let texture = device.create_texture(&wgpu::TextureDescriptor {
                    label: Some("rain_water_simulation_grid"),
                    size: wgpu::Extent3d {
                        width: w,
                        height: h,
                        depth_or_array_layers: 1,
                    },
                    mip_level_count: 1,
                    sample_count: 1,
                    dimension: wgpu::TextureDimension::D2,
                    format: wgpu::TextureFormat::Rgba32Float,
                    usage: wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::COPY_DST,
                    view_formats: &[],
                });
                let view = texture.create_view(&Default::default());
                self.bind = Some(device.create_bind_group(&wgpu::BindGroupDescriptor {
                    label: Some("rain_water_grid_bind"),
                    layout: &self.layout,
                    entries: &[
                        wgpu::BindGroupEntry {
                            binding: 0,
                            resource: wgpu::BindingResource::TextureView(&view),
                        },
                        wgpu::BindGroupEntry {
                            binding: 1,
                            resource: self.params.as_entire_binding(),
                        },
                    ],
                }));
                self.texture = Some(texture);
            }
            queue.write_texture(
                wgpu::TexelCopyTextureInfo {
                    texture: self.texture.as_ref().unwrap(),
                    mip_level: 0,
                    origin: wgpu::Origin3d::ZERO,
                    aspect: wgpu::TextureAspect::All,
                },
                bytemuck::cast_slice(cells),
                wgpu::TexelCopyBufferLayout {
                    offset: 0,
                    bytes_per_row: Some(w * 16),
                    rows_per_image: Some(h),
                },
                wgpu::Extent3d {
                    width: w,
                    height: h,
                    depth_or_array_layers: 1,
                },
            );
            self.grid.clear();
            self.grid.extend_from_slice(cells);
            self.uploaded = Some(key);
        }
        queue.write_buffer(&self.params, 0, bytemuck::bytes_of(&p));
        true
    }

    pub fn prepare(
        &mut self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        camera: &WgrCamera,
        terrain_view: &wgpu::TextureView,
        generation: u64,
        terrain: TerrainConformParams,
    ) {
        self.count = 0;
        if !self.enabled || self.current.control[3] < 0.5 || terrain.enabled < 0.5 {
            return;
        }
        if self.terrain_bind.is_none() || self.terrain_generation != generation {
            self.terrain_bind = Some(device.create_bind_group(&wgpu::BindGroupDescriptor {
                label: Some("rain_water_terrain_bind"),
                layout: &self.terrain_layout,
                entries: &[
                    wgpu::BindGroupEntry {
                        binding: 0,
                        resource: wgpu::BindingResource::TextureView(terrain_view),
                    },
                    wgpu::BindGroupEntry {
                        binding: 1,
                        resource: self.terrain_params.as_entire_binding(),
                    },
                ],
            }));
            self.terrain_generation = generation;
        }
        queue.write_buffer(&self.terrain_params, 0, bytemuck::bytes_of(&terrain));
        let selected = visible_cells(
            &self.grid,
            &self.current,
            [camera.cam_pos[0], camera.cam_pos[1], camera.cam_pos[2]],
        );
        if selected.is_empty() {
            return;
        }
        let size = (selected.len() * 4) as u64;
        if size > self.instance_capacity {
            self.instance_capacity = size.next_power_of_two();
            self.instance = device.create_buffer(&wgpu::BufferDescriptor {
                label: Some("rain_water_instances"),
                size: self.instance_capacity,
                usage: wgpu::BufferUsages::VERTEX | wgpu::BufferUsages::COPY_DST,
                mapped_at_creation: false,
            });
        }
        queue.write_buffer(&self.instance, 0, bytemuck::cast_slice(&selected));
        self.count = selected.len() as u32;
    }

    pub fn drawable(&self) -> bool {
        self.count > 0 && self.bind.is_some() && self.terrain_bind.is_some()
    }
    pub fn draw<'a>(
        &'a self,
        pass: &mut wgpu::RenderPass<'a>,
        camera: &'a wgpu::BindGroup,
        offset: u32,
    ) {
        if !self.drawable() {
            return;
        }
        pass.set_pipeline(&self.pipeline);
        pass.set_bind_group(0, camera, &[offset]);
        pass.set_bind_group(1, self.bind.as_ref().unwrap(), &[]);
        pass.set_bind_group(2, &self.empty_bind, &[]);
        pass.set_bind_group(3, &self.empty_bind, &[]);
        pass.set_bind_group(4, self.terrain_bind.as_ref().unwrap(), &[]);
        pass.set_vertex_buffer(0, self.instance.slice(..));
        pass.draw(0..24, 0..self.count);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn actual_gpu_bilinear_surface_and_support_policy() {
        let (device, queue) = crate::gfx3d::cull::tests::headless()
            .expect("Rainwater sampling falsifier requires an actual device");
        let source = include_str!("rain_water.wgsl");
        let begin = source.find("struct RainWaterParams").unwrap();
        let end = source.find("struct VsOut").unwrap();
        let probe = format!(
            "{}\n@group(0) @binding(0) var<storage,read_write> result: array<vec4<f32>>;\n\
            @compute @workgroup_size(1) fn cs_probe() {{\n\
            let water=water_at(vec2<f32>(105.0,205.0)); result[0]=water;\n\
            result[1]=vec4<f32>(rain_water_support_depth(water,2.15,0.0),\n\
                rain_water_support_depth(water,0.0,0.0),\n\
                rain_water_support_depth(water,2.15,3.0),\n\
                rain_water_support_depth(vec4<f32>(2.0,0.0,0.0,0.0),2.0,0.0));\n\
            result[2]=water_at(vec2<f32>(-100.0,-100.0));\n}}",
            &source[begin..end]
        );
        let shader = device.create_shader_module(wgpu::ShaderModuleDescriptor {
            label: Some("actual_rainwater_sampling_probe"),
            source: wgpu::ShaderSource::Wgsl(probe.into()),
        });
        let result_layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: None,
            entries: &[wgpu::BindGroupLayoutEntry {
                binding: 0,
                visibility: wgpu::ShaderStages::COMPUTE,
                ty: wgpu::BindingType::Buffer {
                    ty: wgpu::BufferBindingType::Storage { read_only: false },
                    has_dynamic_offset: false,
                    min_binding_size: None,
                },
                count: None,
            }],
        });
        let grid_layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: None,
            entries: &[
                wgpu::BindGroupLayoutEntry {
                    binding: 0,
                    visibility: wgpu::ShaderStages::COMPUTE,
                    ty: wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Float { filterable: false },
                        view_dimension: wgpu::TextureViewDimension::D2,
                        multisampled: false,
                    },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 1,
                    visibility: wgpu::ShaderStages::COMPUTE,
                    ty: wgpu::BindingType::Buffer {
                        ty: wgpu::BufferBindingType::Uniform,
                        has_dynamic_offset: false,
                        min_binding_size: None,
                    },
                    count: None,
                },
            ],
        });
        let probe_layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: None,
            bind_group_layouts: &[Some(&result_layout), Some(&grid_layout)],
            immediate_size: 0,
        });
        let pipeline = device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
            label: None,
            layout: Some(&probe_layout),
            module: &shader,
            entry_point: Some("cs_probe"),
            compilation_options: Default::default(),
            cache: None,
        });
        let texture = device.create_texture(&wgpu::TextureDescriptor {
            label: None,
            size: wgpu::Extent3d {
                width: 2,
                height: 2,
                depth_or_array_layers: 1,
            },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: wgpu::TextureFormat::Rgba32Float,
            usage: wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::COPY_DST,
            view_formats: &[],
        });
        let cells: [[f32; 4]; 4] = [
            [2.1, 0.1, 0., -1.],
            [2.2, 0.1, 1., -2.],
            [2.3, 0.1, 1., -2.],
            [2.4, 0.1, 2., -3.],
        ];
        queue.write_texture(
            wgpu::TexelCopyTextureInfo {
                texture: &texture,
                mip_level: 0,
                origin: wgpu::Origin3d::ZERO,
                aspect: wgpu::TextureAspect::All,
            },
            bytemuck::cast_slice(&cells),
            wgpu::TexelCopyBufferLayout {
                offset: 0,
                bytes_per_row: Some(32),
                rows_per_image: Some(2),
            },
            wgpu::Extent3d {
                width: 2,
                height: 2,
                depth_or_array_layers: 1,
            },
        );
        let params = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
            label: None,
            contents: bytemuck::bytes_of(&WgrRainWaterParams {
                domain: [100., 200., 10., 7.],
                control: [2., 2., 1., 1.],
                generation: 1,
                reserved: 0,
            }),
            usage: wgpu::BufferUsages::UNIFORM,
        });
        let output = device.create_buffer(&wgpu::BufferDescriptor {
            label: None,
            size: 48,
            usage: wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_SRC,
            mapped_at_creation: false,
        });
        let staging = device.create_buffer(&wgpu::BufferDescriptor {
            label: None,
            size: 48,
            usage: wgpu::BufferUsages::MAP_READ | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        let view = texture.create_view(&Default::default());
        let resources = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: None,
            layout: &pipeline.get_bind_group_layout(1),
            entries: &[
                wgpu::BindGroupEntry {
                    binding: 0,
                    resource: wgpu::BindingResource::TextureView(&view),
                },
                wgpu::BindGroupEntry {
                    binding: 1,
                    resource: params.as_entire_binding(),
                },
            ],
        });
        let result = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: None,
            layout: &pipeline.get_bind_group_layout(0),
            entries: &[wgpu::BindGroupEntry {
                binding: 0,
                resource: output.as_entire_binding(),
            }],
        });
        let mut encoder = device.create_command_encoder(&Default::default());
        {
            let mut pass = encoder.begin_compute_pass(&Default::default());
            pass.set_pipeline(&pipeline);
            pass.set_bind_group(0, &result, &[]);
            pass.set_bind_group(1, &resources, &[]);
            pass.dispatch_workgroups(1, 1, 1);
        }
        encoder.copy_buffer_to_buffer(&output, 0, &staging, 0, 48);
        queue.submit(Some(encoder.finish()));
        let slice = staging.slice(..);
        let (send, receive) = std::sync::mpsc::channel();
        slice.map_async(wgpu::MapMode::Read, move |result| {
            let _ = send.send(result);
        });
        device.poll(wgpu::PollType::wait_indefinitely()).unwrap();
        receive.recv().unwrap().unwrap();
        let bytes = slice.get_mapped_range();
        let values = bytemuck::cast_slice::<u8, f32>(&bytes);
        assert!(values.iter().all(|v| v.is_finite()));
        for (actual, expected) in values[..4].iter().zip([2.25, 0.1, 1.0, -2.0]) {
            assert!((actual - expected).abs() < 1e-5);
        }
        assert!((values[4] - 0.1).abs() < 1e-5);
        assert_eq!(&values[5..8], &[0., 0., 0.]);
        for (actual, expected) in values[8..].iter().zip(cells[0]) {
            assert!((actual - expected).abs() < 1e-5);
        }
        drop(bytes);
        staging.unmap();
    }
    fn p() -> WgrRainWaterParams {
        WgrRainWaterParams {
            domain: [100., 200., 10., 7.],
            control: [3., 3., 1., 1.],
            generation: 1,
            reserved: 0,
        }
    }
    #[test]
    fn physical_grid_validation_and_camera_selection() {
        let mut cells = vec![[2., 0., 0., 0.]; 9];
        cells[4] = [2.1, 0.1, 1., 0.];
        assert!(valid_cells(&cells, 3, 3));
        assert_eq!(
            visible_cells(&cells, &p(), [110., 3., 210.]),
            vec![0, 1, 3, 4]
        );
        assert!(visible_cells(&cells, &p(), [5000., 3., 5000.]).is_empty());
        let snapshot = cells.clone();
        let _ = visible_cells(&cells, &p(), [120., 3., 220.]);
        assert_eq!(cells, snapshot);
        cells[4][1] = -0.1;
        assert!(!valid_cells(&cells, 3, 3));
        cells[4][1] = f32::NAN;
        assert!(!valid_cells(&cells, 3, 3));
        assert!(!valid_cells(&cells[..8], 3, 3));
        let mut invalid = p();
        invalid.control[0] = 3.5;
        assert!(dimensions(&invalid).is_none());
        invalid = p();
        invalid.domain[2] = 0.;
        assert!(dimensions(&invalid).is_none());
    }
    #[test]
    fn camera_draw_bound_and_shader_uses_actual_grid() {
        let mut p = p();
        p.domain = [0., 0., 0.125, 10.];
        p.control = [1024., 1024., 1., 1.];
        let cells = vec![[2., 0.02, 0., 0.]; MAX_CELLS];
        let selected = visible_cells(&cells, &p(), [64., 3., 64.]);
        assert!(selected.len() <= MAX_DRAW_CELLS && !selected.is_empty());
        let source = include_str!("rain_water.wgsl");
        for proof in [
            "rain_water_support_depth(water,bed",
            "bare_surface_valid",
            "interior_rain_coverage",
            "ground_sky_reflection",
            "ground_puddle_ripple_normal",
            "flow * rainwater.domain.w",
        ] {
            assert!(source.contains(proof));
        }
        assert!(!source.contains("ground_puddle_mask"));
        let mut composer = crate::shaders::build_composer();
        composer
            .make_naga_module(naga_oil::compose::NagaModuleDescriptor {
                source,
                file_path: "rain_water.wgsl",
                ..Default::default()
            })
            .unwrap();
    }
}
