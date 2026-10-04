//! Presentation of simulation-owned rainfall runoff; independent of either ocean backend.
use crate::{
    ffi::{
        WGR_RAIN_WATER_FINE_BACKEND, WGR_RAIN_WATER_FINE_READY, WGR_RAIN_WATER_SOURCE_READY,
        WgrCamera, WgrRainWaterFineCell, WgrRainWaterParams, WgrRainWaterPublication,
        WgrRainWaterSourceKey, WgrVec4,
    },
    terrain::TerrainConformParams,
};
use wgpu::util::DeviceExt;
#[path = "rain_water_fine.rs"]
mod fine;
#[cfg(test)]
#[path = "rain_water_body_tests.rs"]
mod body_tests;
#[cfg(test)]
#[path = "rain_water_optics_tests.rs"]
mod optics_tests;
#[cfg(test)]
#[path = "rain_water_medium_tests.rs"]
mod medium_tests;
// Physical compute probes exercise the actual production grid/fine/support
// functions without the separate terrain-material resource interface.
#[cfg(test)]
fn physical_helpers_source() -> String {
    let source = include_str!("rain_water.wgsl");
    let begin = source.find("struct RainWaterParams").unwrap();
    let medium_begin = source.find("// Prefix of the EXISTING terrain uniform").unwrap();
    let medium_end = source.find("fn fine_record").unwrap();
    let end = source.find("struct VsOut").unwrap();
    assert!(begin < medium_begin && medium_begin < medium_end && medium_end < end);
    format!("{}{}", &source[begin..medium_begin], &source[medium_end..end])
}

#[cfg(test)]
#[path = "rain_water_edge_optics_tests.rs"]
mod edge_optics_tests;
type PublicationIdentity = (fine::SourceIdentity, u64, [u32; 6], u32);
fn publication_identity(p: &WgrRainWaterPublication) -> Option<PublicationIdentity> {
    Some((
        fine::source_identity(p.source)?,
        p.revision,
        [
            p.coarse.domain[0].to_bits(),
            p.coarse.domain[1].to_bits(),
            p.coarse.domain[2].to_bits(),
            p.coarse.control[0].to_bits(),
            p.coarse.control[1].to_bits(),
            p.coarse.control[3].to_bits(),
        ],
        p.flags,
    ))
}

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
    medium_layout: wgpu::BindGroupLayout,
    medium_control: wgpu::Buffer,
    medium_bind: Option<wgpu::BindGroup>,
    medium_generation: Option<u64>,
    medium_uploaded_control: [u32;4],
    params: wgpu::Buffer,
    terrain_params: wgpu::Buffer,
    pipeline: wgpu::RenderPipeline,
    fine_pipeline: wgpu::RenderPipeline,
    fine_texture: wgpu::Texture,
    fine_mask_buffer: wgpu::Buffer,
    fine_mask: fine::TileMask,
    fine_grid: Vec<WgrRainWaterFineCell>,
    fine_instance: wgpu::Buffer,
    fine_instance_capacity: u64,
    fine_count: u32,
    fine_enabled: bool,
    source: Option<fine::SourceIdentity>,
    publication: Option<PublicationIdentity>,
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
    reflections: crate::rain_water_reflections::Reflections,
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
            entries: &[
                texture_entry(0),
                uniform_entry(1),
                texture_entry(2),
                uniform_entry(3),
            ],
        });
        let terrain_layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("rain_water_bare_terrain_layout"),
            entries: &[texture_entry(0), uniform_entry(1)],
        });
        let medium_layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("rain_water_actual_medium_sources"),
            entries: &[
                uniform_entry(0),
                wgpu::BindGroupLayoutEntry {binding:1,visibility:wgpu::ShaderStages::FRAGMENT,
                    ty:wgpu::BindingType::Texture{sample_type:wgpu::TextureSampleType::Uint,
                        view_dimension:wgpu::TextureViewDimension::D2,multisampled:false},count:None},
                wgpu::BindGroupLayoutEntry {binding:2,visibility:wgpu::ShaderStages::FRAGMENT,
                    ty:wgpu::BindingType::Buffer{ty:wgpu::BufferBindingType::Storage{read_only:true},
                        has_dynamic_offset:false,min_binding_size:None},count:None},
                uniform_entry(3),
            ],
        });
        let medium_control=device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
            label:Some("rain_water_medium_readiness"),contents:bytemuck::cast_slice(&[0u32;4]),
            usage:wgpu::BufferUsages::UNIFORM|wgpu::BufferUsages::COPY_DST,
        });
        let reflections = crate::rain_water_reflections::Reflections::new(device, format);
        let pipeline_layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("rain_water_pipeline_layout"),
            bind_group_layouts: &[
                Some(camera),
                Some(&layout),
                Some(&reflections.layout),
                Some(&medium_layout),
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
        let make_pipeline = |entry| {
            device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
                label: Some("rain_water_pipeline"),
                layout: Some(&pipeline_layout),
                vertex: wgpu::VertexState {
                    module: &shader,
                    entry_point: Some(entry),
                    compilation_options: Default::default(),
                    buffers: &[wgpu::VertexBufferLayout {
                        array_stride: 4,
                        step_mode: wgpu::VertexStepMode::Instance,
                        attributes: &attributes,
                    }],
                },
                primitive: wgpu::PrimitiveState {
                    // World-up cells are clockwise after the engine's LH projection.
                    // fs_main still rejects undersides; cull_mode=None alone cannot
                    // compensate for the wrong front_facing convention.
                    front_face: wgpu::FrontFace::Cw,
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
            })
        };
        let pipeline = make_pipeline("vs_main");
        let fine_pipeline = make_pipeline("vs_fine");
        let fine_texture = device.create_texture(&wgpu::TextureDescriptor {
            label: Some("rain_water_fine_records"),
            size: wgpu::Extent3d {
                width: fine::TEXTURE_WIDTH,
                height: fine::TEXTURE_HEIGHT,
                depth_or_array_layers: 1,
            },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: wgpu::TextureFormat::Rgba32Float,
            usage: wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::COPY_DST,
            view_formats: &[],
        });
        let fine_mask: fine::TileMask = bytemuck::Zeroable::zeroed();
        let fine_mask_buffer = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
            label: Some("rain_water_fine_tile_mask"),
            contents: bytemuck::bytes_of(&fine_mask),
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
        });
        let fine_instance = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
            label: Some("rain_water_fine_instances"),
            contents: bytemuck::cast_slice(&[0u32]),
            usage: wgpu::BufferUsages::VERTEX | wgpu::BufferUsages::COPY_DST,
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
            medium_layout, medium_control, medium_bind:None, medium_generation:None, medium_uploaded_control:[0;4],
            params,
            terrain_params,
            pipeline,
            fine_pipeline,
            fine_texture,
            fine_mask_buffer,
            fine_mask,
            fine_grid: vec![],
            fine_instance,
            fine_instance_capacity: 4,
            fine_count: 0,
            fine_enabled: std::env::var("WGR_RAIN_WATER_FINE").as_deref() == Ok("1"),
            source: None,
            publication: None,
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
            reflections,
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
        // Legacy receipts can never keep a prior fine/source ownership lifetime.
        if self.source.is_some() || self.publication.is_some() || !self.fine_grid.is_empty() {
            self.clear_pair();
        }
        self.source = None;
        self.fine_count = 0;
        queue.write_buffer(
            &self.fine_mask_buffer,
            0,
            bytemuck::bytes_of(&self.fine_mask),
        );
        self.set_grid_data(device, queue, p, cells, revision)
    }
    fn clear_pair(&mut self) {
        self.count = 0;
        self.fine_count = 0;
        self.grid.clear();
        self.fine_grid.clear();
        self.uploaded = None;
        self.publication = None;
        self.fine_mask = bytemuck::Zeroable::zeroed();
    }
    pub fn invalidate_source(&mut self) {
        self.source = None;
        self.clear_pair();
    }
    pub fn set_source(&mut self, key: WgrRainWaterSourceKey) {
        let source = fine::source_identity(key);
        if source != self.source || source.is_none() {
            self.clear_pair();
        }
        self.source = source;
    }
    /// Actual retained paired bytes for the root's opt-in publication trace.
    pub fn publication_counts(&self)->(usize,usize,u32){
        (self.grid.len(),self.fine_grid.len(),self.fine_mask.meta[0])
    }
    pub fn set_publication(
        &mut self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        p: WgrRainWaterPublication,
        coarse: &[WgrVec4],
        cells: &[WgrRainWaterFineCell],
    ) -> bool {
        let identity = publication_identity(&p);
        let acceptable = self.enabled
            && self.fine_enabled
            && p.flags & WGR_RAIN_WATER_FINE_BACKEND != 0
            && p.flags & WGR_RAIN_WATER_SOURCE_READY != 0
            && p.flags & !7 == 0
            && p.reserved == 0
            && p.coarse.reserved == 0
            && p.coarse.control[3] >= 0.5
            && dimensions(&p.coarse).is_some()
            && p.coarse.generation == p.source.generation
            && self.source.is_some()
            && self.source == fine::source_identity(p.source);
        if !acceptable {
            self.clear_pair();
            return false;
        }
        if coarse.is_empty() && cells.is_empty() && identity == self.publication {
            self.current = p.coarse;
            queue.write_buffer(&self.params, 0, bytemuck::bytes_of(&p.coarse));
            return true;
        }
        let Some((w, h)) = dimensions(&p.coarse) else {
            self.clear_pair();
            return false;
        };
        let Some(mask) = fine::validate_tiles(
            &p.coarse,
            p.source,
            cells,
            p.flags & WGR_RAIN_WATER_FINE_READY != 0,
        ) else {
            self.clear_pair();
            return false;
        };
        if !valid_cells(coarse, w, h) {
            self.clear_pair();
            return false;
        }
        // Validate the complete pair before updating any reusable GPU/CPU state.
        self.fine_mask = mask;
        self.fine_grid.clear();
        self.fine_grid.extend_from_slice(cells);
        queue.write_buffer(&self.fine_mask_buffer, 0, bytemuck::bytes_of(&mask));
        let packed = fine::packed_cells(cells);
        queue.write_texture(
            wgpu::TexelCopyTextureInfo {
                texture: &self.fine_texture,
                mip_level: 0,
                origin: wgpu::Origin3d::ZERO,
                aspect: wgpu::TextureAspect::All,
            },
            bytemuck::cast_slice(&packed),
            wgpu::TexelCopyBufferLayout {
                offset: 0,
                bytes_per_row: Some(fine::TEXTURE_WIDTH * 16),
                rows_per_image: Some(fine::TEXTURE_HEIGHT),
            },
            self.fine_texture.size(),
        );
        if !self.set_grid_data(device, queue, p.coarse, coarse, p.revision) {
            self.clear_pair();
            return false;
        }
        self.publication = identity;
        true
    }
    fn set_grid_data(
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
                let fine_view = self.fine_texture.create_view(&Default::default());
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
                        wgpu::BindGroupEntry {
                            binding: 2,
                            resource: wgpu::BindingResource::TextureView(&fine_view),
                        },
                        wgpu::BindGroupEntry {
                            binding: 3,
                            resource: self.fine_mask_buffer.as_entire_binding(),
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

    pub fn prepare_medium(&mut self,device:&wgpu::Device,queue:&wgpu::Queue,
        resources:crate::rain_water_medium::Resources<'_>) {
        if self.medium_generation != Some(resources.revision) || self.medium_bind.is_none() {
            self.medium_bind=Some(device.create_bind_group(&wgpu::BindGroupDescriptor {
                label:Some("rain_water_current_material_sources"),layout:&self.medium_layout,
                entries:&[
                    wgpu::BindGroupEntry{binding:0,resource:resources.params.as_entire_binding()},
                    wgpu::BindGroupEntry{binding:1,resource:wgpu::BindingResource::TextureView(
                        &resources.indices.create_view(&Default::default()))},
                    wgpu::BindGroupEntry{binding:2,resource:resources.materials.as_entire_binding()},
                    wgpu::BindGroupEntry{binding:3,resource:self.medium_control.as_entire_binding()},
                ],
            }));
            self.medium_generation=Some(resources.revision);
        }
        if self.medium_uploaded_control != resources.control {
            queue.write_buffer(&self.medium_control,0,bytemuck::cast_slice(&resources.control));
            self.medium_uploaded_control=resources.control;
        }
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
        self.invalidate_frame(queue);
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
        let mut selected = visible_cells(
            &self.grid,
            &self.current,
            [camera.cam_pos[0], camera.cam_pos[1], camera.cam_pos[2]],
        );
        // Remove only wholly owned coarse quads. Boundary quads retain their
        // geometry and the exact fragment rectangle mask; parent indices alone
        // would erase valid outside portions of the old node-centred grid.
        if self.fine_mask.meta[0]>0 {
            let width=self.current.control[0] as u32;
            let spacing=self.current.domain[2];
            selected.retain(|&cell|{
                let x=self.current.domain[0]+(cell%width) as f32*spacing;
                let z=self.current.domain[1]+(cell/width) as f32*spacing;
                !self.fine_mask.rects[..self.fine_mask.meta[0] as usize].iter()
                    .any(|r|x>=r[0] && z>=r[1] && x+spacing<=r[2] && z+spacing<=r[3])
            });
        }
        let mut selected_fine = Vec::new();
        for (i, cell) in self.fine_grid.iter().enumerate() {
            let [x, z, size, head] = cell.rect;
            let dx = camera.cam_pos[0] - camera.cam_pos[0].clamp(x, x + size);
            let dz = camera.cam_pos[2] - camera.cam_pos[2].clamp(z, z + size);
            let minimum = cell.corners.iter().copied().fold(f32::INFINITY, f32::min);
            if dx * dx + dz * dz <= 600.0 * 600.0 && head - minimum > 0.001 {
                selected_fine.push(i as u32);
                if self.reflections.enabled() {
                    self.reflections.offer_box(
                        camera,
                        glam::Vec3::new(x, head, z),
                        glam::Vec3::new(x + size, head, z + size),
                    );
                }
            }
        }
        if !selected_fine.is_empty() {
            let size = selected_fine.len() as u64 * 4;
            if size > self.fine_instance_capacity {
                self.fine_instance_capacity = size.next_power_of_two();
                self.fine_instance = device.create_buffer(&wgpu::BufferDescriptor {
                    label: Some("rain_water_fine_instances"),
                    size: self.fine_instance_capacity,
                    usage: wgpu::BufferUsages::VERTEX | wgpu::BufferUsages::COPY_DST,
                    mapped_at_creation: false,
                });
            }
            queue.write_buffer(&self.fine_instance, 0, bytemuck::cast_slice(&selected_fine));
            self.fine_count = selected_fine.len() as u32;
        }
        if selected.is_empty() {
            return;
        }
        // Reuse actual selected simulation cells; no extra global wet-grid scan.
        let width = self.current.control[0] as usize;
        let spacing = self.current.domain[2];
        if self.reflections.enabled() {
            for &cell in &selected {
                if self.reflections.needed() {
                    break;
                }
                let i = cell as usize;
                let x = self.current.domain[0] + (i % width) as f32 * spacing;
                let z = self.current.domain[1] + (i / width) as f32 * spacing;
                let dx = (camera.cam_pos[0] - camera.cam_pos[0].clamp(x, x + spacing)).abs();
                let dz = (camera.cam_pos[2] - camera.cam_pos[2].clamp(z, z + spacing)).abs();
                if dx * dx + dz * dz > crate::rain_water_reflections::NEAR_RANGE.powi(2) {
                    continue;
                }
                let corners = [i, i + 1, i + width, i + width + 1];
                let mut minimum_bed = f32::INFINITY;
                let mut maximum_bed = f32::NEG_INFINITY;
                let mut minimum_depth = f32::INFINITY;
                let mut maximum_depth = 0.0_f32;
                for index in corners {
                    let c = self.grid[index];
                    minimum_bed = minimum_bed.min(c[0] - c[1]);
                    maximum_bed = maximum_bed.max(c[0] - c[1]);
                    minimum_depth = minimum_depth.min(c[1]);
                    maximum_depth = maximum_depth.max(c[1]);
                }
                self.reflections.offer_box(
                    camera,
                    glam::Vec3::new(x, minimum_bed + minimum_depth, z),
                    glam::Vec3::new(x + spacing, maximum_bed + maximum_depth, z + spacing),
                );
            }
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
        (self.count > 0 || self.fine_count > 0)
            && self.bind.is_some()
            && self.terrain_bind.is_some()
            && self.medium_bind.is_some()
    }
    pub fn reflections_needed(&self) -> bool {
        self.drawable() && self.reflections.needed()
    }
    pub fn invalidate_frame(&mut self, queue: &wgpu::Queue) {
        self.count = 0;
        self.fine_count = 0;
        self.reflections.begin_frame(queue);
    }
    pub fn record_reflections(
        &mut self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        encoder: &mut wgpu::CommandEncoder,
        source: &wgpu::Texture,
        view: &wgpu::TextureView,
        depth: &wgpu::TextureView,
        generation: u64,
    ) {
        self.reflections
            .record(device, queue, encoder, source, view, depth, generation);
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
        pass.set_bind_group(2, &self.reflections.bind, &[]);
        pass.set_bind_group(3, self.medium_bind.as_ref().unwrap(), &[]);
        pass.set_bind_group(4, self.terrain_bind.as_ref().unwrap(), &[]);
        pass.set_vertex_buffer(0, self.instance.slice(..));
        pass.draw(0..24, 0..self.count);
        if self.fine_count > 0 {
            pass.set_pipeline(&self.fine_pipeline);
            pass.set_vertex_buffer(0, self.fine_instance.slice(..));
            pass.draw(0..6, 0..self.fine_count);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn actual_gpu_triangular_bed_bilinear_depth_and_support_policy() {
        let (device, queue) = crate::gfx3d::cull::tests::headless()
            .expect("Rainwater sampling falsifier requires an actual device");
        let probe = format!(
            "{}\n{}\n@group(0) @binding(0) var<storage,read_write> result: array<vec4<f32>>;\n\
            @compute @workgroup_size(1) fn cs_probe() {{\n\
            let water=water_at(vec2<f32>(105.0,205.0)); result[0]=water;\n\
            result[1]=vec4<f32>(rain_water_support_depth(water,2.15,0.0),\n\
                rain_water_support_depth(water,0.0,0.0),\n\
                rain_water_support_depth(water,2.15,3.0),\n\
                rain_water_support_depth(vec4<f32>(2.0,0.0,0.0,0.0),2.0,0.0));\n\
            result[2]=water_at(vec2<f32>(-100.0,-100.0));\n\
            result[3]=water_at(vec2<f32>(102.5,202.5));\n\
            result[4]=water_at(vec2<f32>(107.5,207.5));\n\
            let supported=vec4<f32>(2.3,0.3,0.0,0.0);\n\
            result[5]=vec4<f32>(rain_water_raster_support_depth(supported,2.0,0.0,2.33),\n\
                rain_water_raster_support_depth(supported,2.0,0.0,2.27),\n\
                rain_water_raster_support_depth(supported,2.0,0.0,2.55),\n\
                rain_water_raster_support_depth(supported,2.0,0.0,2.05));\n\
            result[6]=vec4<f32>(rain_water_support_depth(supported,2.0,0.0),\n\
                rain_water_raster_support_depth(supported,2.0,0.0,2.3),\n\
                rain_water_raster_support_depth(vec4<f32>(2.02,0.02,0.0,0.0),2.0,0.0,1.995),\n\
                rain_water_raster_support_depth(supported,2.0,3.0,2.3));\n}}",
            include_str!("shaders/rain_water_optics.wgsl")
                .lines().skip(1).collect::<Vec<_>>().join("\n"),
            physical_helpers_source()
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
            // Deliberately nonplanar bed and varying depth: bilinear surface
            // height or triangular depth interpolation must both fail.
            [5.7, 0.4, 2., -3.],
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
            size: 112,
            usage: wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_SRC,
            mapped_at_creation: false,
        });
        let staging = device.create_buffer(&wgpu::BufferDescriptor {
            label: None,
            size: 112,
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
        encoder.copy_buffer_to_buffer(&output, 0, &staging, 0, 112);
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
        for (actual, expected) in values[..4].iter().zip([2.325, 0.175, 1.0, -2.0]) {
            assert!((actual - expected).abs() < 1e-5);
        }
        assert!((values[4] - 0.175).abs() < 1e-5);
        assert_eq!(&values[5..8], &[0., 0., 0.]);
        for index in [20,21,24,25] {assert!((values[index]-0.3).abs()<0.00001,
            "3cm boundary/analytic/fine-horizontal support must remain valid: {index}");}
        assert_eq!(&values[22..24], &[0.,0.],"Analytically supported +/-25cm raster mismatch must be rejected");
        assert_eq!(&values[26..28], &[0.,0.],"Raster under bed and sea are independent negatives");
        for (actual, expected) in values[8..12].iter().zip(cells[0]) {
            assert!((actual - expected).abs() < 1e-5);
        }
        for (actual, expected) in values[12..16].iter().zip([2.19375, 0.11875, 0.5, -1.5]) {
            assert!((actual - expected).abs() < 1e-5);
        }
        for (actual, expected) in values[16..20].iter().zip([3.99375, 0.26875, 1.5, -2.5]) {
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
        let selected = visible_cells(&cells, &p, [64., 3., 64.]);
        assert!(selected.len() <= MAX_DRAW_CELLS && !selected.is_empty());
        let source = include_str!("rain_water.wgsl");
        for proof in [
            "rain_water_raster_support_depth(water,bed,frame.ground_weather.z,in.world.y)",
            "bare_surface_valid",
            "interior_rain_coverage",
            "ground_sky_reflection",
            "rain_water_impact_normal",
            "rain_water_sun_specular",
            "shadow_strength",
            "terrain_sun_shadow",
            "cloud_sun_shadow",
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

    fn optics_source() -> String {
        let source = include_str!("rain_water.wgsl");
        let begin = source.find("fn rain_water_optics(").unwrap();
        let end = source[begin..].find("struct VsOut").unwrap() + begin;
        format!(
            "{}\n{}",
            include_str!("shaders/rain_water_optics.wgsl")
                .lines().skip(1).collect::<Vec<_>>().join("\n"),
            &source[begin..end]
        )
    }

    #[test]
    fn actual_depth_optics_validates_without_resources_and_is_used_by_fragment() {
        // Validate the actual production helper without GPU/resources, then
        // verify that the composed real fragment calls it, not a test-only copy.
        let module = naga::front::wgsl::parse_str(&optics_source()).unwrap();
        naga::valid::Validator::new(
            naga::valid::ValidationFlags::all(),
            naga::valid::Capabilities::all(),
        )
        .validate(&module)
        .unwrap();
        assert!(module.global_variables.is_empty());
        let (_, helper) = module
            .functions
            .iter()
            .find(|(_, f)| f.name.as_deref() == Some("rain_water_optics"))
            .unwrap();
        assert_eq!(
            helper
                .arguments
                .iter()
                .map(|arg| arg.name.as_deref().unwrap())
                .collect::<Vec<_>>(),
            [
                "local_depth",
                "view_cosine",
                "reflected",
                "body",
                "coverage"
            ]
        );
        // No instantaneous rain parameter: retained water keeps its optical
        // body when drops stop. Rain continues to affect the existing ripples.
        let mut composer = crate::shaders::build_composer();
        let composed = composer
            .make_naga_module(naga_oil::compose::NagaModuleDescriptor {
                source: include_str!("rain_water.wgsl"),
                file_path: "rain_water.wgsl",
                ..Default::default()
            })
            .unwrap_or_else(|error| panic!("{}", error.emit_to_string(&composer)));
        let fragment = composed
            .entry_points
            .iter()
            .find(|entry| entry.stage == naga::ShaderStage::Fragment && entry.name == "fs_main")
            .unwrap();
        assert!(
            fragment.function.body.iter().any(|statement| {
                if let naga::Statement::Call { function, .. } = statement {
                    composed.functions[*function]
                        .name
                        .as_deref()
                        .is_some_and(|name| name.ends_with("rain_water_optics"))
                } else {
                    false
                }
            }),
            "the actual fragment must use the tested depth material"
        );
    }

    #[test]
    fn actual_gpu_depth_optics_transmits_bed_conserves_energy_and_is_not_milk() {
        let (device, queue) = crate::gfx3d::cull::tests::headless()
            .expect("Depth water optics require an actual device");
        let probe = format!(
            r#"{}
@group(0) @binding(0) var<storage,read_write> result: array<vec4<f32>>;
@compute @workgroup_size(64)
fn cs_probe(@builtin(global_invocation_id) id: vec3<u32>) {{
    let i=id.x;
    let depth=f32(i)/63.0*2.0;
    let sky=vec3<f32>(4.0,8.0,16.0);
    let body=vec3<f32>(0.045,0.060,0.052);
    result[i*4u]=rain_water_optics(depth,1.0,sky,body,1.0);
    result[i*4u+1u]=rain_water_optics(0.1,f32(i)/63.0,sky,body,1.0);
    result[i*4u+2u]=rain_water_optics(depth,1.0,sky,body,0.25);
    result[i*4u+3u]=vec4<f32>(
        rain_water_optics(0.0,1.0,sky,body,1.0).a,
        rain_water_optics(depth,1.0,sky,body,0.0).a,
        rain_water_optics(-0.1,1.0,sky,body,1.0).a,
        rain_water_optics(depth,1.0,sky,body,-1.0).a);
    if(i==0u) {{
        result[256]=rain_water_optics(1e30,1.0,sky,body,1.0);
        result[257]=rain_water_optics(0.1,-10.0,sky,body,1.0);
        result[258]=rain_water_optics(0.1,10.0,sky,body,1.0);
        result[259]=rain_water_optics(0.1,1.0,sky,body,2.0);
        result[260]=rain_water_optics(0.1,1.0,vec3<f32>(-1.0),vec3<f32>(-2.0),1.0);
        result[261]=rain_water_optics(0.002,1.0,sky,body,1.0);
    }}
}}
"#,
            optics_source()
        );
        let shader = device.create_shader_module(wgpu::ShaderModuleDescriptor {
            label: Some("actual_depth_water_optics"),
            source: wgpu::ShaderSource::Wgsl(probe.into()),
        });
        let pipeline = device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
            label: Some("actual_depth_water_optics"),
            layout: None,
            module: &shader,
            entry_point: Some("cs_probe"),
            compilation_options: Default::default(),
            cache: None,
        });
        let size = 262 * 16;
        let output = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("water_optics_result"),
            size,
            usage: wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_SRC,
            mapped_at_creation: false,
        });
        let staging = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("water_optics_readback"),
            size,
            usage: wgpu::BufferUsages::MAP_READ | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        let bind = device.create_bind_group(&wgpu::BindGroupDescriptor {
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
            pass.set_bind_group(0, &bind, &[]);
            pass.dispatch_workgroups(1, 1, 1);
        }
        encoder.copy_buffer_to_buffer(&output, 0, &staging, 0, size);
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
        assert!(
            values
                .iter()
                .all(|value| value.is_finite() && *value >= 0.0)
        );
        let mut previous_depth_alpha = 0.0;
        let mut previous_view_alpha = 1.0;
        let bed = [0.4, 0.3, 0.2];
        let sky = [4.0, 8.0, 16.0];
        let body = [0.045, 0.060, 0.052];
        for i in 0..64 {
            let s = &values[i * 16..i * 16 + 16];
            let depth = i as f64 / 63.0 * 2.0;
            let transmitted = (-3.0 * depth).exp();
            assert!(s[3] >= previous_depth_alpha && s[3] <= 1.0);
            previous_depth_alpha = s[3];
            assert!(s[7] <= previous_view_alpha + 1e-6 && s[7] <= 1.0);
            previous_view_alpha = s[7];
            assert_eq!(
                &s[12..16],
                &[0.0; 4],
                "dry/covered/negative inputs must be inert"
            );
            assert!((s[11] - s[3] * 0.25).abs() < 1e-6);
            if i == 0 {
                assert_eq!(&s[..4], &[0.0; 4]);
            } else {
                for channel in 0..3 {
                    assert!(
                        (s[channel] - s[channel + 8]).abs() < 1e-5,
                        "coverage changes opacity without recolouring the pool"
                    );
                    assert!(
                        s[channel] as f64 >= body[channel] - 1e-5
                            && s[channel] as f64 <= sky[channel] + 1e-5
                    );
                    // Independent compositing oracle includes the destination
                    // bed. This catches double premultiplication, fixed opacity,
                    // sky lift, and an HDR clamp even if alpha alone looks valid.
                    let actual =
                        s[channel] as f64 * s[3] as f64 + bed[channel] * (1.0 - s[3] as f64);
                    let t = ((depth - 0.008) / 0.042).clamp(0.0, 1.0);
                    let reflected_weight = 0.025 * t * t * (3.0 - 2.0 * t);
                    let expected = reflected_weight * sky[channel]
                        + (1.0 - reflected_weight)
                            * ((1.0 - transmitted) * body[channel] + transmitted * bed[channel]);
                    assert!(
                        (actual - expected).abs() < 1e-5,
                        "depth={depth} channel={channel}: {actual} vs {expected}"
                    );
                }
            }
        }
        let special = &values[256 * 4..];
        assert!(
            (special[3] - 1.0).abs() < 1e-6,
            "deep pool must obscure the bed"
        );
        assert_eq!(
            &special[4..8],
            &[4.0, 8.0, 16.0, 1.0],
            "grazing reflection retains HDR energy"
        );
        for channel in 0..4 {
            assert!(
                (special[8 + channel] - special[12 + channel]).abs() < 1e-6,
                "view/coverage finite bounds must clamp consistently"
            );
        }
        assert_eq!(
            &special[16..19],
            &[0.0; 3],
            "negative radiance cannot create negative light"
        );
        assert!(
            (0.005..0.007).contains(&special[23]),
            "2mm water must not restore the old .3 opacity milk floor"
        );
        drop(bytes);
        staging.unmap();
    }
}

#[cfg(test)]
mod full_draw_tests {
    use super::*;

    #[test]
    fn fine_actual_composed_uniform_offsets_and_both_vertex_entries(){
        let mut composer=crate::shaders::build_composer();
        let module=composer.make_naga_module(naga_oil::compose::NagaModuleDescriptor{
            source:include_str!("rain_water.wgsl"),file_path:"rain_water.wgsl",..Default::default()}).unwrap();
        let (members,span)=module.types.iter().find_map(|(_,ty)|{
            if let naga::TypeInner::Struct{members,span}=&ty.inner {
                if members.len()==2 && members[0].name.as_deref()==Some("counts") && members[1].name.as_deref()==Some("rects") {return Some((members,*span));}
            }None
        }).expect("Actual fine tile uniform is missing");
        assert_eq!(span as usize,std::mem::size_of::<fine::TileMask>());
        assert_eq!(members[0].offset as usize,std::mem::offset_of!(fine::TileMask,meta));
        assert_eq!(members[1].offset as usize,std::mem::offset_of!(fine::TileMask,rects));
        for name in ["vs_main","vs_fine"] {assert!(module.entry_points.iter().any(|e|e.name==name && e.stage==naga::ShaderStage::Vertex));}
    }

    // This fixture uses the actual composed Frame/Conform and BOTH production
    // entry points. It deliberately does not replace fs_main with a debug colour
    // or bypass the front-facing, native-bed or physical-cover discards.
    #[test]
    fn actual_gpu_complete_pool_draw_visibility_and_admission() {
        let instance =
            wgpu::Instance::new(wgpu::InstanceDescriptor::new_without_display_handle_from_env());
        let adapter = pollster::block_on(instance.request_adapter(&wgpu::RequestAdapterOptions {
            force_fallback_adapter: false,
            ..Default::default()
        }))
        .expect("Complete rainwater draw falsifier requires an actual GPU adapter");
        let limits = wgpu::Limits {
            max_bind_groups: 5,
            ..Default::default()
        };
        let (device, queue) = pollster::block_on(adapter.request_device(&wgpu::DeviceDescriptor {
            required_limits: limits,
            ..Default::default()
        }))
        .expect("Complete rainwater draw requires the production five bind groups");
        let mut composer = crate::shaders::build_composer();
        let module = composer
            .make_naga_module(naga_oil::compose::NagaModuleDescriptor {
                source: include_str!("rain_water.wgsl"),
                file_path: "rain_water.wgsl",
                ..Default::default()
            })
            .unwrap_or_else(|error| panic!("{}", error.emit_to_string(&composer)));
        let (members, span) = module
            .types
            .iter()
            .find_map(|(_, ty)| {
                if let naga::TypeInner::Struct { members, span } = &ty.inner {
                    if members
                        .iter()
                        .any(|m| m.name.as_deref() == Some("weather_vp"))
                        && members.iter().any(|m| m.name.as_deref() == Some("proj"))
                    {
                        return Some((members.clone(), *span));
                    }
                }
                None
            })
            .expect("Actual Frame layout missing");
        let offset = |name: &str| -> usize {
            members
                .iter()
                .find(|m| m.name.as_deref() == Some(name))
                .unwrap_or_else(|| panic!("Actual Frame has no {name}"))
                .offset as usize
        };
        let mut frame = vec![0u8; span as usize];
        let put = |bytes: &mut [u8], at: usize, value: &[f32]| {
            bytes[at..at + value.len() * 4].copy_from_slice(bytemuck::cast_slice(value));
        };
        let camera_pos = glam::Vec3::new(5., 15., -15.);
        let direction = (glam::Vec3::new(5., 2.3, 5.) - camera_pos).normalize();
        // Engine convention: camera-relative left-handed forward projection.
        // Existing mesh front faces are Cw; default Ccw plus fragment !front
        // must not silently erase this genuinely upward world-space surface.
        let projection = glam::Mat4::perspective_lh(60_f32.to_radians(), 1., 0.1, 100.);
        let view = glam::Mat4::look_to_lh(glam::Vec3::ZERO, direction, glam::Vec3::Y);
        put(&mut frame, offset("proj"), &projection.to_cols_array());
        put(&mut frame, offset("view"), &view.to_cols_array());
        put(
            &mut frame,
            offset("cam_pos"),
            &[camera_pos.x, camera_pos.y, camera_pos.z, 1.],
        );
        put(&mut frame, offset("sun_ambient"), &[1., 1., 1., 0.]);
        put(&mut frame, offset("sun_diffuse"), &[1., 1., 1., 0.]);
        put(&mut frame, offset("sun_dir_world"), &[0., -1., 0., 0.]);
        put(&mut frame, offset("ground_weather"), &[0.8, 0., 0., 10.]);
        // Artistic AO disabled. Independent physical map must still certify
        // the entire bounded native cell, then reject it under actual cover.
        let weather_vp = glam::Mat4::from_cols_array(&[
            0.1, 0., 0., 0., 0., 0., 0., 0., 0., 0.1, 0., 0., -0.5, -0.5, 0.5, 1.,
        ]);
        put(
            &mut frame,
            offset("weather_vp"),
            &weather_vp.to_cols_array(),
        );
        put(&mut frame, offset("weather_cover"), &[1., 0.001, 0.05, 1.]);
        let frame_buffer = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
            label: Some("actual_pool_frame"),
            contents: &frame,
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
        });
        let stages = wgpu::ShaderStages::VERTEX_FRAGMENT;
        let tex_entry = |binding, dimension, sample_type| wgpu::BindGroupLayoutEntry {
            binding,
            visibility: stages,
            ty: wgpu::BindingType::Texture {
                sample_type,
                view_dimension: dimension,
                multisampled: false,
            },
            count: None,
        };
        let samp_entry = |binding, kind| wgpu::BindGroupLayoutEntry {
            binding,
            visibility: stages,
            ty: wgpu::BindingType::Sampler(kind),
            count: None,
        };
        let camera_layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("actual_pool_camera_layout"),
            entries: &[
                wgpu::BindGroupLayoutEntry {
                    binding: 0,
                    visibility: stages,
                    ty: wgpu::BindingType::Buffer {
                        ty: wgpu::BufferBindingType::Uniform,
                        has_dynamic_offset: true,
                        min_binding_size: wgpu::BufferSize::new(span as u64),
                    },
                    count: None,
                },
                tex_entry(1, wgpu::TextureViewDimension::D2Array, wgpu::TextureSampleType::Depth),
                samp_entry(2, wgpu::SamplerBindingType::Comparison),
                tex_entry(4, wgpu::TextureViewDimension::D2, wgpu::TextureSampleType::Float { filterable: true }),
                samp_entry(5, wgpu::SamplerBindingType::Filtering),
                wgpu::BindGroupLayoutEntry {
                    binding: 6, visibility: stages,
                    ty: wgpu::BindingType::Buffer {ty: wgpu::BufferBindingType::Uniform,
                        has_dynamic_offset: false, min_binding_size: wgpu::BufferSize::new(64)},
                    count: None,
                },
                tex_entry(
                    7,
                    wgpu::TextureViewDimension::D3,
                    wgpu::TextureSampleType::Float { filterable: true },
                ),
                samp_entry(8, wgpu::SamplerBindingType::Filtering),
                wgpu::BindGroupLayoutEntry {
                    binding: 9,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Buffer {
                        ty: wgpu::BufferBindingType::Uniform,
                        has_dynamic_offset: false,
                        min_binding_size: wgpu::BufferSize::new(144),
                    },
                    count: None,
                },
                tex_entry(
                    12,
                    wgpu::TextureViewDimension::D2Array,
                    wgpu::TextureSampleType::Depth,
                ),
                tex_entry(13, wgpu::TextureViewDimension::D2, wgpu::TextureSampleType::Float { filterable: true }),
                tex_entry(
                    18,
                    wgpu::TextureViewDimension::D2,
                    wgpu::TextureSampleType::Depth,
                ),
                tex_entry(
                    19,
                    wgpu::TextureViewDimension::D2,
                    wgpu::TextureSampleType::Float { filterable: true },
                ),
                tex_entry(
                    20,
                    wgpu::TextureViewDimension::D2,
                    wgpu::TextureSampleType::Depth,
                ),
                tex_entry(21,wgpu::TextureViewDimension::D3,wgpu::TextureSampleType::Float {filterable:true}),
                wgpu::BindGroupLayoutEntry {binding:22,visibility:wgpu::ShaderStages::FRAGMENT,
                    ty:wgpu::BindingType::Buffer {ty:wgpu::BufferBindingType::Uniform,has_dynamic_offset:false,
                        min_binding_size:wgpu::BufferSize::new(std::mem::size_of::<crate::layered_fog::Consumer>() as u64)},count:None},
            ],
        });
        let texture = |label, format, width, height, layers, dimension, usage| {
            device.create_texture(&wgpu::TextureDescriptor {
                label: Some(label),
                size: wgpu::Extent3d {
                    width,
                    height,
                    depth_or_array_layers: layers,
                },
                mip_level_count: 1,
                sample_count: 1,
                dimension,
                format,
                usage,
                view_formats: &[],
            })
        };
        let weather = texture(
            "actual_pool_weather",
            wgpu::TextureFormat::Depth32Float,
            4,
            4,
            1,
            wgpu::TextureDimension::D2,
            wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::RENDER_ATTACHMENT,
        );
        let weather_view = weather.create_view(&Default::default());
        let near = texture(
            "actual_pool_near",
            wgpu::TextureFormat::Depth32Float,
            4,
            4,
            5,
            wgpu::TextureDimension::D2,
            wgpu::TextureUsages::TEXTURE_BINDING,
        );
        let near_view = near.create_view(&wgpu::TextureViewDescriptor {
            dimension: Some(wgpu::TextureViewDimension::D2Array),
            ..Default::default()
        });
        let sky = texture(
            "actual_pool_directional_sky",
            wgpu::TextureFormat::Rgba8Unorm,
            1,
            1,
            1,
            wgpu::TextureDimension::D2,
            wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::COPY_DST,
        );
        queue.write_texture(
            wgpu::TexelCopyTextureInfo {
                texture: &sky,
                mip_level: 0,
                origin: wgpu::Origin3d::ZERO,
                aspect: wgpu::TextureAspect::All,
            },
            &[80, 140, 220, 255],
            wgpu::TexelCopyBufferLayout {
                offset: 0,
                bytes_per_row: Some(4),
                rows_per_image: Some(1),
            },
            sky.size(),
        );
        let sky_view = sky.create_view(&Default::default());
        let cloud = texture("actual_pool_cloud_transmittance", wgpu::TextureFormat::Rgba8Unorm,
            1,1,1,wgpu::TextureDimension::D2,wgpu::TextureUsages::TEXTURE_BINDING|wgpu::TextureUsages::COPY_DST);
        let cloud_view=cloud.create_view(&Default::default());
        let write_cloud=|bytes:&[u8]|queue.write_texture(wgpu::TexelCopyTextureInfo{texture:&cloud,mip_level:0,
            origin:wgpu::Origin3d::ZERO,aspect:wgpu::TextureAspect::All},bytes,
            wgpu::TexelCopyBufferLayout{offset:0,bytes_per_row:Some(4),rows_per_image:Some(1)},cloud.size());
        write_cloud(&[255,255,255,255]);
        // Bind the actual shared terrain/cloud mapping ABI, even when the
        // existing support cases disable terrain and cascade shadow sampling.
        let mut shadow_map_params=[0.0f32;16];
        let shadow_map_buffer=device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
            label:Some("actual_pool_terrain_cloud_mapping"), contents:bytemuck::cast_slice(&shadow_map_params),
            usage:wgpu::BufferUsages::UNIFORM|wgpu::BufferUsages::COPY_DST,
        });
        let froxel = texture(
            "actual_pool_unused_fog",
            wgpu::TextureFormat::Rgba8Unorm,
            1,
            1,
            1,
            wgpu::TextureDimension::D3,
            wgpu::TextureUsages::TEXTURE_BINDING,
        );
        let froxel_view = froxel.create_view(&Default::default());
        let linear = device.create_sampler(&wgpu::SamplerDescriptor {
            mag_filter: wgpu::FilterMode::Linear,
            min_filter: wgpu::FilterMode::Linear,
            ..Default::default()
        });
        let comparison = device.create_sampler(&wgpu::SamplerDescriptor {
            compare: Some(wgpu::CompareFunction::LessEqual),
            ..Default::default()
        });
        // A required real production binding, even while legacy-lighting cases
        // do not evaluate SH. Isotropic unit radiance has L00=sqrt(4*pi).
        let mut sh = [[0.0_f32;4];9];
        sh[0] = [(4.0*std::f32::consts::PI).sqrt();4];
        let sky_sh = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
            label: Some("actual_pool_physical_sky_sh"),
            contents: bytemuck::cast_slice(&sh),
            usage: wgpu::BufferUsages::UNIFORM,
        });
        let layer_fog_packet=device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
            label:Some("actual_pool_disabled_layer_fog"),contents:bytemuck::bytes_of(&<crate::layered_fog::Consumer as bytemuck::Zeroable>::zeroed()),
            usage:wgpu::BufferUsages::UNIFORM});
        let camera_bind = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("actual_pool_camera"),
            layout: &camera_layout,
            entries: &[
                wgpu::BindGroupEntry {
                    binding: 0,
                    resource: frame_buffer.as_entire_binding(),
                },
                wgpu::BindGroupEntry {binding:1,resource:wgpu::BindingResource::TextureView(&near_view)},
                wgpu::BindGroupEntry {
                    binding: 2,
                    resource: wgpu::BindingResource::Sampler(&comparison),
                },
                wgpu::BindGroupEntry {
                    binding: 5,
                    resource: wgpu::BindingResource::Sampler(&linear),
                },
                wgpu::BindGroupEntry {binding:4,resource:wgpu::BindingResource::TextureView(&sky_view)},
                wgpu::BindGroupEntry {binding:6,resource:shadow_map_buffer.as_entire_binding()},
                wgpu::BindGroupEntry {
                    binding: 7,
                    resource: wgpu::BindingResource::TextureView(&froxel_view),
                },
                wgpu::BindGroupEntry {
                    binding: 8,
                    resource: wgpu::BindingResource::Sampler(&linear),
                },
                wgpu::BindGroupEntry {
                    binding: 9,
                    resource: sky_sh.as_entire_binding(),
                },
                wgpu::BindGroupEntry {
                    binding: 12,
                    resource: wgpu::BindingResource::TextureView(&near_view),
                },
                wgpu::BindGroupEntry {binding:13,resource:wgpu::BindingResource::TextureView(&cloud_view)},
                wgpu::BindGroupEntry {
                    binding: 18,
                    resource: wgpu::BindingResource::TextureView(&weather_view),
                },
                wgpu::BindGroupEntry {
                    binding: 19,
                    resource: wgpu::BindingResource::TextureView(&sky_view),
                },
                wgpu::BindGroupEntry {
                    binding: 20,
                    resource: wgpu::BindingResource::TextureView(&weather_view),
                },
                wgpu::BindGroupEntry {binding:21,resource:wgpu::BindingResource::TextureView(&froxel_view)},
                wgpu::BindGroupEntry {binding:22,resource:layer_fog_packet.as_entire_binding()},
            ],
        });
        let mut rain = RainWater::new(
            &device,
            &camera_layout,
            wgpu::TextureFormat::Rgba8Unorm,
            1,
            &mut composer,
        );
        // The production medium layout is populated with actual resources even
        // when the fixture has no authored terrain material/source receipt.
        // Readiness zero leaves its existing clear-water raster tests unchanged.
        let medium_indices=texture("actual_pool_source_indices",wgpu::TextureFormat::R16Uint,1,1,1,
            wgpu::TextureDimension::D2,wgpu::TextureUsages::TEXTURE_BINDING);
        let medium_materials=device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
            label:None,contents:bytemuck::bytes_of(&<crate::ffi::WgrTerrainMaterial as bytemuck::Zeroable>::zeroed()),usage:wgpu::BufferUsages::STORAGE});
        let medium_params=device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
            label:None,contents:bytemuck::bytes_of(&<crate::ffi::WgrTerrainParams as bytemuck::Zeroable>::zeroed()),usage:wgpu::BufferUsages::UNIFORM});
        rain.prepare_medium(&device,&queue,crate::rain_water_medium::Resources {
            params:&medium_params,indices:&medium_indices,materials:&medium_materials,revision:1,control:[0;4]});
        rain.enabled = true; // independent of ambient process environment in this required-device test
                             // Counterfactual raster-only control: same production shader, resources,
                             // geometry, blend and depth policy, changing solely front-face convention.
        let diagnostic_layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("pool_clockwise_falsifier"),
            bind_group_layouts: &[
                Some(&camera_layout),
                Some(&rain.layout),
                Some(&rain.reflections.layout),
                Some(&rain.medium_layout),
                Some(&rain.terrain_layout),
            ],
            immediate_size: 0,
        });
        let shader = crate::shaders::make_module(
            &device,
            &mut composer,
            "rain_water_draw_control",
            include_str!("rain_water.wgsl"),
            "rain_water.wgsl",
        );
        let attributes = [wgpu::VertexAttribute {
            format: wgpu::VertexFormat::Uint32,
            offset: 0,
            shader_location: 0,
        }];
        let clockwise = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
            label: Some("pool_clockwise_falsifier"),
            layout: Some(&diagnostic_layout),
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
                front_face: wgpu::FrontFace::Cw,
                cull_mode: None,
                ..Default::default()
            },
            depth_stencil: Some(wgpu::DepthStencilState {
                format: crate::gfx3d::depth_format(&device),
                depth_write_enabled: Some(false),
                depth_compare: Some(wgpu::CompareFunction::GreaterEqual),
                stencil: Default::default(),
                bias: Default::default(),
            }),
            multisample: Default::default(),
            fragment: Some(wgpu::FragmentState {
                module: &shader,
                entry_point: Some("fs_main"),
                compilation_options: Default::default(),
                targets: &[Some(wgpu::ColorTargetState {
                    format: wgpu::TextureFormat::Rgba8Unorm,
                    blend: Some(wgpu::BlendState::ALPHA_BLENDING),
                    write_mask: wgpu::ColorWrites::ALL,
                })],
            }),
            multiview_mask: None,
            cache: None,
        });
        let mut clockwise = Some(clockwise);
        let hm = texture(
            "actual_pool_native_bed",
            wgpu::TextureFormat::R32Float,
            2,
            2,
            1,
            wgpu::TextureDimension::D2,
            wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::COPY_DST,
        );
        let hm_view = hm.create_view(&Default::default());
        let output = texture(
            "actual_pool_pixels",
            wgpu::TextureFormat::Rgba8Unorm,
            64,
            64,
            1,
            wgpu::TextureDimension::D2,
            wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::COPY_SRC,
        );
        let output_view = output.create_view(&Default::default());
        let depth = texture(
            "actual_pool_depth",
            crate::gfx3d::depth_format(&device),
            64,
            64,
            1,
            wgpu::TextureDimension::D2,
            wgpu::TextureUsages::RENDER_ATTACHMENT,
        );
        let depth_view = depth.create_view(&Default::default());
        let readback = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("actual_pool_readback"),
            size: 64 * 64 * 4,
            usage: wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::MAP_READ,
            mapped_at_creation: false,
        });
        let mut camera: WgrCamera = bytemuck::Zeroable::zeroed();
        camera.cam_pos = [camera_pos.x, camera_pos.y, camera_pos.z, 1.];
        camera.proj = projection.to_cols_array();
        camera.view = view.to_cols_array();
        let mut native: TerrainConformParams = bytemuck::Zeroable::zeroed();
        native.enabled = 1.;
        native.terrain_grid = 10.;
        native.hm_width = 2;
        native.hm_height = 2;
        let cases = [
            ("flat positive", [2., 2., 2., 2.], 0., 1., 1., 0.),
            (
                "triangular sloped positive",
                [2., 2.2, 2.4, 3.],
                0.,
                1.,
                1.,
                0.,
            ),
            ("physical roof negative", [2., 2., 2., 2.], 0., 0., 1., 0.),
            (
                "stale native support negative",
                [2., 2., 2., 2.],
                1.,
                1.,
                1.,
                0.,
            ),
            ("explicit master off", [2., 2., 2., 2.], 0., 1., 0., 0.),
            ("snow hides pool", [2., 2., 2., 2.], 0., 1., 1., 0.18),
            (
                "clockwise flat diagnostic",
                [2., 2., 2., 2.],
                0.,
                1.,
                1.,
                0.,
            ),
        ];
        let mut counts = Vec::new();
        for (case_index, (name, bed, stale, cover, enabled, snow)) in cases.iter().enumerate() {
            if case_index == 6 {
                rain.pipeline = clockwise.take().unwrap();
            }
            put(&mut frame, offset("snow_surface"), &[*snow, 10000., 1., 0.]);
            queue.write_buffer(&frame_buffer, 0, &frame);
            let native_bed = bed.map(|b| b + stale);
            queue.write_texture(
                wgpu::TexelCopyTextureInfo {
                    texture: &hm,
                    mip_level: 0,
                    origin: wgpu::Origin3d::ZERO,
                    aspect: wgpu::TextureAspect::All,
                },
                bytemuck::cast_slice(&native_bed),
                wgpu::TexelCopyBufferLayout {
                    offset: 0,
                    bytes_per_row: Some(8),
                    rows_per_image: Some(2),
                },
                hm.size(),
            );
            let cells = bed.map(|b| [b + 0.3, 0.3, 0., 0.]);
            let params = WgrRainWaterParams {
                domain: [0., 0., 10., 10.],
                control: [2., 2., 0., *enabled],
                generation: 1,
                reserved: 0,
            };
            assert!(rain.set_grid(&device, &queue, params, &cells, case_index as u64));
            rain.prepare(&device, &queue, &camera, &hm_view, 1, native);
            assert_eq!(
                rain.drawable(),
                *enabled > 0.5,
                "actual instance selection: {name}"
            );
            let mut encoder = device.create_command_encoder(&Default::default());
            {
                let _pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
                    label: Some("actual_pool_weather_depth"),
                    color_attachments: &[],
                    depth_stencil_attachment: Some(wgpu::RenderPassDepthStencilAttachment {
                        view: &weather_view,
                        depth_ops: Some(wgpu::Operations {
                            load: wgpu::LoadOp::Clear(*cover),
                            store: wgpu::StoreOp::Store,
                        }),
                        stencil_ops: None,
                    }),
                    ..Default::default()
                });
            }
            {
                let mut pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
                    label: Some(name),
                    color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                        view: &output_view,
                        resolve_target: None,
                        depth_slice: None,
                        ops: wgpu::Operations {
                            load: wgpu::LoadOp::Clear(wgpu::Color {
                                r: 0.,
                                g: 0.,
                                b: 0.,
                                a: 0.,
                            }),
                            store: wgpu::StoreOp::Store,
                        },
                    })],
                    depth_stencil_attachment: Some(wgpu::RenderPassDepthStencilAttachment {
                        view: &depth_view,
                        depth_ops: Some(wgpu::Operations {
                            load: wgpu::LoadOp::Clear(0.),
                            store: wgpu::StoreOp::Store,
                        }),
                        stencil_ops: None,
                    }),
                    ..Default::default()
                });
                rain.draw(&mut pass, &camera_bind, 0);
            }
            encoder.copy_texture_to_buffer(
                wgpu::TexelCopyTextureInfo {
                    texture: &output,
                    mip_level: 0,
                    origin: wgpu::Origin3d::ZERO,
                    aspect: wgpu::TextureAspect::All,
                },
                wgpu::TexelCopyBufferInfo {
                    buffer: &readback,
                    layout: wgpu::TexelCopyBufferLayout {
                        offset: 0,
                        bytes_per_row: Some(256),
                        rows_per_image: Some(64),
                    },
                },
                output.size(),
            );
            queue.submit(Some(encoder.finish()));
            let slice = readback.slice(..);
            let (tx, rx) = std::sync::mpsc::channel();
            slice.map_async(wgpu::MapMode::Read, move |result| {
                let _ = tx.send(result);
            });
            device.poll(wgpu::PollType::wait_indefinitely()).unwrap();
            rx.recv().unwrap().unwrap();
            let bytes = slice.get_mapped_range();
            let count = bytes.chunks_exact(4).filter(|p| p[3] > 0).count();
            let energy: usize = bytes
                .chunks_exact(4)
                .map(|p| p[0] as usize + p[1] as usize + p[2] as usize)
                .sum();
            eprintln!("actual rainwater vs_main/fs_main {name}: pixels={count}, RGB energy={energy}, drawable={}",rain.drawable());
            counts.push((count, energy));
            drop(bytes);
            readback.unmap();
        }
        assert!(
            counts[0].0 > 100 && counts[0].1 > 1000,
            "Actual flat physical pool produced no visible pixels: {counts:?}"
        );
        assert!(
            counts[1].0 > 100 && counts[1].1 > 1000,
            "Actual triangular physical pool produced no visible pixels: {counts:?}"
        );
        assert!(
            counts[6].0 > 100 && counts[6].1 > 1000,
            "Clockwise control failed: resources/projection need investigation: {counts:?}"
        );
        for (index, (name, ..)) in cases.iter().enumerate().take(6).skip(2) {
            assert_eq!(
                counts[index],
                (0, 0),
                "Negative pool admission painted pixels: {name}"
            );
        }

        // The same actual production fragment now draws complete native fine
        // ownership, not a shader-only debug mask. Keep the original coarse
        // flat/sloped/Cw controls above as independent regressions.
        rain.fine_enabled=true;
        let (coarse_params,mut source,fine_template)=fine::tests::fixture(1,1);
        let coarse=vec![[2.3,0.3,0.,0.];129*129];
        let fine_hm=texture("actual_fine_native",wgpu::TextureFormat::R32Float,512,512,1,
            wgpu::TextureDimension::D2,wgpu::TextureUsages::TEXTURE_BINDING|wgpu::TextureUsages::COPY_DST);
        let fine_hm_view=fine_hm.create_view(&Default::default());
        native.terrain_grid=6.25;native.hm_width=512;native.hm_height=512;
        let eye=glam::Vec3::new(300.,25.,280.);
        let aim=glam::Vec3::new(300.,2.3,300.);
        let fine_projection=glam::Mat4::perspective_lh(45_f32.to_radians(),1.,0.1,80.);
        let weather_vp=glam::Mat4::from_cols_array(&[
            0.005,0.,0.,0., 0.,0.,0.,0., 0.,0.005,0.,0., -1.5,-1.5,0.5,1.]);
        put(&mut frame,offset("weather_vp"),&weather_vp.to_cols_array());
        put(&mut frame,offset("proj"),&fine_projection.to_cols_array());
        // Fine cases exercise the real physical-sky fragment and SH binding,
        // replaying captured rainy ambient/direct inputs with known isotropic
        // SH. They are not a reconstruction of the mission's unavailable SH.
        put(&mut frame,offset("sun_ambient"),&[22.1267,24.138218,32.184288,1.35]);
        put(&mut frame,offset("sun_diffuse"),&[1.0355144,0.9062767,0.60597956,1.]);
        put(&mut frame,offset("sun_dir_world"),&[0.8081431,-0.58894414,-0.0070375875,0.]);
        let raster=|rain:&RainWater,cover:f32|->Vec<u8>{
            let mut encoder=device.create_command_encoder(&Default::default());
            {let _pass=encoder.begin_render_pass(&wgpu::RenderPassDescriptor{
                label:Some("actual_fine_weather"),color_attachments:&[],depth_stencil_attachment:Some(wgpu::RenderPassDepthStencilAttachment{
                    view:&weather_view,depth_ops:Some(wgpu::Operations{load:wgpu::LoadOp::Clear(cover),store:wgpu::StoreOp::Store}),stencil_ops:None}),..Default::default()});}
            {let mut pass=encoder.begin_render_pass(&wgpu::RenderPassDescriptor{
                label:Some("actual_fine_vs_fs"),color_attachments:&[Some(wgpu::RenderPassColorAttachment{
                    view:&output_view,resolve_target:None,depth_slice:None,ops:wgpu::Operations{load:wgpu::LoadOp::Clear(wgpu::Color::TRANSPARENT),store:wgpu::StoreOp::Store}})],
                depth_stencil_attachment:Some(wgpu::RenderPassDepthStencilAttachment{view:&depth_view,
                    depth_ops:Some(wgpu::Operations{load:wgpu::LoadOp::Clear(0.),store:wgpu::StoreOp::Store}),stencil_ops:None}),..Default::default()});
                rain.draw(&mut pass,&camera_bind,0);}
            encoder.copy_texture_to_buffer(wgpu::TexelCopyTextureInfo{texture:&output,mip_level:0,origin:wgpu::Origin3d::ZERO,aspect:wgpu::TextureAspect::All},
                wgpu::TexelCopyBufferInfo{buffer:&readback,layout:wgpu::TexelCopyBufferLayout{offset:0,bytes_per_row:Some(256),rows_per_image:Some(64)}},output.size());
            queue.submit(Some(encoder.finish()));let slice=readback.slice(..);let(tx,rx)=std::sync::mpsc::channel();
            slice.map_async(wgpu::MapMode::Read,move|r|{let _=tx.send(r);});device.poll(wgpu::PollType::wait_indefinitely()).unwrap();rx.recv().unwrap().unwrap();
            let bytes=slice.get_mapped_range().to_vec();readback.unmap();bytes
        };
        let fine_cases=["flat","crater","slope","dry owned coarse exclusion","physical roof","snow","underside","sea"];
        let mut fine_counts=Vec::new();
        for (index,name) in fine_cases.iter().enumerate(){
            let bed_at=|x:f32,z:f32|->f32{match index{
                1=>{let r=((x-300.).powi(2)+(z-300.).powi(2)).sqrt();4.-2.*(1.-r/12.5).max(0.)},
                2=>2.+(x-300.)*0.03,
                _=>2.,
            }};
            let native_values:Vec<f32>=(0..512*512).map(|i|bed_at((i%512) as f32*6.25,(i/512) as f32*6.25)).collect();
            queue.write_texture(wgpu::TexelCopyTextureInfo{texture:&fine_hm,mip_level:0,origin:wgpu::Origin3d::ZERO,aspect:wgpu::TextureAspect::All},
                bytemuck::cast_slice(&native_values),wgpu::TexelCopyBufferLayout{offset:0,bytes_per_row:Some(512*4),rows_per_image:Some(512)},fine_hm.size());
            source.height_revision=index as u64+1;
            let mut cells=fine_template.clone();
            for c in &mut cells{
                let [x,z,size,_]=c.rect;
                c.corners=[bed_at(x,z),bed_at(x+size,z),bed_at(x,z+size),bed_at(x+size,z+size)];
                if index==3 {c.rect[3]=1.7;}
                c.flow_depth[2]=(c.rect[3]-c.corners.iter().sum::<f32>()*0.25).max(0.);
            }
            let position=if index==6 {glam::Vec3::new(300.,1.,280.)}else{eye};
            let fine_view=glam::Mat4::look_to_lh(glam::Vec3::ZERO,(aim-position).normalize(),glam::Vec3::Y);
            camera.cam_pos=[position.x,position.y,position.z,1.];camera.proj=fine_projection.to_cols_array();camera.view=fine_view.to_cols_array();
            put(&mut frame,offset("cam_pos"),&camera.cam_pos);put(&mut frame,offset("view"),&fine_view.to_cols_array());
            put(&mut frame,offset("snow_surface"),&[if index==5 {0.18}else{0.},10000.,1.,0.]);
            put(&mut frame,offset("ground_weather"),&[0.,0.,if index==7 {3.}else{0.},10.]);
            queue.write_buffer(&frame_buffer,0,&frame);
            rain.set_source(source);
            let publication=WgrRainWaterPublication{coarse:coarse_params,source,revision:index as u64+1,flags:7,reserved:0};
            assert!(rain.set_publication(&device,&queue,publication,&coarse,&cells),"complete source/ownership: {name}");
            rain.prepare(&device,&queue,&camera,&fine_hm_view,index as u64+10,native);
            let pixels=raster(&rain,if index==4 {0.}else{1.});
            let count=pixels.chunks_exact(4).filter(|p|p[3]>0).count();
            fine_counts.push(count);eprintln!("actual vs_fine/fs_main {name}: pixels={count}");
            if index==0{
                let count_coarse=rain.count;rain.count=0;
                assert_eq!(raster(&rain,1.),pixels,"Owned tile must not blend coarse and fine water twice");rain.count=count_coarse;
                // Actual production solar lobe under the same wet geometry:
                // cloud transmittance may remove sun energy, never pool coverage.
                // Align the real sun to the flat interface's reflected view.
                let v=(eye-aim).normalize();
                put(&mut frame,offset("sun_dir_world"),&[v.x,-v.y,v.z,0.]);
                queue.write_buffer(&frame_buffer,0,&frame);
                shadow_map_params[12..16].copy_from_slice(&[200.,200.,0.005,1.]);
                queue.write_buffer(&shadow_map_buffer,0,bytemuck::cast_slice(&shadow_map_params));
                let sun_pixels=raster(&rain,1.);
                write_cloud(&[0,255,255,255]);let shadow_pixels=raster(&rain,1.);
                assert_eq!(sun_pixels.chunks_exact(4).map(|p|p[3]).collect::<Vec<_>>(),
                    shadow_pixels.chunks_exact(4).map(|p|p[3]).collect::<Vec<_>>(),
                    "Actual cloud shadow must not alter water admission/opacity");
                let energy=|bytes:&[u8]|->usize{bytes.chunks_exact(4).map(|p|p[0] as usize+p[1] as usize+p[2] as usize).sum()};
                assert!(energy(&sun_pixels)>energy(&shadow_pixels)+100,
                    "Actual cloud transmittance did not remove direct solar energy: {} vs {}",
                    energy(&sun_pixels),energy(&shadow_pixels));
                assert!(energy(&shadow_pixels)>0,"Cloud shadow removed non-solar water reflection/body");
                write_cloud(&[255,255,255,255]);shadow_map_params=[0.;16];
                queue.write_buffer(&shadow_map_buffer,0,bytemuck::cast_slice(&shadow_map_params));
                put(&mut frame,offset("sun_dir_world"),&[0.8081431,-0.58894414,-0.0070375875,0.]);
                queue.write_buffer(&frame_buffer,0,&frame);
                // Parameter-only camera publication retains BOTH complete
                // arrays only under the same source/layout/lifetime identity.
                assert!(rain.set_publication(&device,&queue,publication,&[],&[]));
                let mut wrong=publication;wrong.source.height_revision+=1;
                assert!(!rain.set_publication(&device,&queue,wrong,&[],&[]));
                rain.prepare(&device,&queue,&camera,&fine_hm_view,10,native);
                assert_eq!(raster(&rain,1.).chunks_exact(4).filter(|p|p[3]>0).count(),0,"Mismatched source cannot retain previous pair");
                assert!(rain.set_publication(&device,&queue,publication,&coarse,&cells));
                rain.invalidate_source();assert!(!rain.set_publication(&device,&queue,publication,&[],&[]));
                rain.set_source(source);rain.fine_enabled=false;
                assert!(!rain.set_publication(&device,&queue,publication,&coarse,&cells),"Fine opt-out may not draw unmasked coarse proxy");rain.fine_enabled=true;
            }
        }
        assert!(fine_counts[0]>100,"Native horizontal pool must be visible: {fine_counts:?}");
        assert!(fine_counts[1]>0 && fine_counts[1]<fine_counts[0],"Native crater clips to its actual wet bed: {fine_counts:?}");
        assert!(fine_counts[2]>100 && fine_counts[2]<fine_counts[0],"Native sloping bed clips shoreline: {fine_counts:?}");
        assert_eq!(&fine_counts[3..],&[0,0,0,0,0],"Dry ownership, roof, snow, underside and sea remain closed");

        // Execute the exact production fine helpers at physical tile edges and
        // across a native saddle. No averaged depth or coarse interpolation is
        // substituted for the local triangle/constant-head result.
        let mut helper_cells=fine_template;
        helper_cells[0].rect[3]=3.;helper_cells[0].corners=[2.,2.5,3.,7.];
        let packed=fine::packed_cells(&helper_cells);
        queue.write_texture(wgpu::TexelCopyTextureInfo{texture:&rain.fine_texture,mip_level:0,origin:wgpu::Origin3d::ZERO,aspect:wgpu::TextureAspect::All},
            bytemuck::cast_slice(&packed),wgpu::TexelCopyBufferLayout{offset:0,bytes_per_row:Some(fine::TEXTURE_WIDTH*16),rows_per_image:Some(fine::TEXTURE_HEIGHT)},rain.fine_texture.size());
        let probe=format!("{}\n{}\n@group(0) @binding(0) var<storage,read_write> result:array<vec4<f32>>;\n\
            @compute @workgroup_size(1) fn cs_probe() {{\n\
            result[0]=vec4<f32>(select(0.,1.,fine_owned(vec2<f32>(187.5,187.5))),select(0.,1.,fine_owned(vec2<f32>(387.5,300.))),\n\
                select(0.,1.,fine_owned(vec2<f32>(187.499,300.))),select(0.,1.,fine_owned(vec2<f32>(387.499,300.))));\n\
            result[1]=fine_water_at(0u,vec2<f32>(189.0625,189.0625));\n\
            result[2]=fine_water_at(0u,vec2<f32>(192.1875,192.1875)); }}",
            include_str!("shaders/rain_water_optics.wgsl").lines().skip(1).collect::<Vec<_>>().join("\n"),
            super::physical_helpers_source());
        let shader=device.create_shader_module(wgpu::ShaderModuleDescriptor{label:Some("actual_fine_helpers"),source:wgpu::ShaderSource::Wgsl(probe.into())});
        let result_buffer=device.create_buffer(&wgpu::BufferDescriptor{label:None,size:48,usage:wgpu::BufferUsages::STORAGE|wgpu::BufferUsages::COPY_SRC,mapped_at_creation:false});
        let result_layout=device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor{label:None,entries:&[wgpu::BindGroupLayoutEntry{binding:0,
            visibility:wgpu::ShaderStages::COMPUTE,ty:wgpu::BindingType::Buffer{ty:wgpu::BufferBindingType::Storage{read_only:false},has_dynamic_offset:false,min_binding_size:None},count:None}]});
        let helper_layout=device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor{label:None,entries:&[
            wgpu::BindGroupLayoutEntry{binding:2,visibility:wgpu::ShaderStages::COMPUTE,ty:wgpu::BindingType::Texture{sample_type:wgpu::TextureSampleType::Float{filterable:false},view_dimension:wgpu::TextureViewDimension::D2,multisampled:false},count:None},
            wgpu::BindGroupLayoutEntry{binding:3,visibility:wgpu::ShaderStages::COMPUTE,ty:wgpu::BindingType::Buffer{ty:wgpu::BufferBindingType::Uniform,has_dynamic_offset:false,min_binding_size:None},count:None}]});
        let result_bind=device.create_bind_group(&wgpu::BindGroupDescriptor{label:None,layout:&result_layout,entries:&[wgpu::BindGroupEntry{binding:0,resource:result_buffer.as_entire_binding()}]});
        let fine_view=rain.fine_texture.create_view(&Default::default());
        let helper_bind=device.create_bind_group(&wgpu::BindGroupDescriptor{label:None,layout:&helper_layout,entries:&[
            wgpu::BindGroupEntry{binding:2,resource:wgpu::BindingResource::TextureView(&fine_view)},wgpu::BindGroupEntry{binding:3,resource:rain.fine_mask_buffer.as_entire_binding()}]});
        let pipeline_layout=device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor{label:None,bind_group_layouts:&[Some(&result_layout),Some(&helper_layout)],immediate_size:0});
        let pipeline=device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor{label:None,layout:Some(&pipeline_layout),module:&shader,entry_point:Some("cs_probe"),compilation_options:Default::default(),cache:None});
        let mut encoder=device.create_command_encoder(&Default::default());
        {let mut pass=encoder.begin_compute_pass(&Default::default());pass.set_pipeline(&pipeline);pass.set_bind_group(0,&result_bind,&[]);pass.set_bind_group(1,&helper_bind,&[]);pass.dispatch_workgroups(1,1,1);}
        encoder.copy_buffer_to_buffer(&result_buffer,0,&readback,0,48);queue.submit(Some(encoder.finish()));
        let slice=readback.slice(..48);let(tx,rx)=std::sync::mpsc::channel();slice.map_async(wgpu::MapMode::Read,move|r|{let _=tx.send(r);});
        device.poll(wgpu::PollType::wait_indefinitely()).unwrap();rx.recv().unwrap().unwrap();let data=slice.get_mapped_range();
        let actual:&[f32]=bytemuck::cast_slice(&data);assert_eq!(&actual[..4],&[1.,0.,0.,1.]);
        assert!((actual[4]-3.).abs()<1e-6 && (actual[5]-0.625).abs()<1e-6,"Actual native lower triangle: {actual:?}");
        assert_eq!(actual[8],3.);assert_eq!(actual[9],0.,"Native upper triangle is above horizontal head");drop(data);readback.unmap();
    }
}
