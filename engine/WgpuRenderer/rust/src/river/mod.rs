//! RFG-162: native Reforger rivers drawn as water (see river.wgsl).
//!
//! The loader hands over one mesh for all of a world's rivers (`wgr_river_set_mesh`): their
//! ribbons, level across their width at the authored water level, with flow coordinates per
//! vertex. They are drawn in the water pass, after the sea, sampling the same frozen scene
//! snapshot and resolved depth the sea refracts through, and the sky's reflection env map.
//! Nothing is allocated until a world sends rivers; an empty mesh releases them.

use crate::gfx3d::depth_format;

/// One river vertex, 32 bytes. Mirrors `VsIn` in river.wgsl and the 8 floats per vertex the
/// C++ loader writes.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, bytemuck::Pod, bytemuck::Zeroable)]
pub struct RiverVertex {
    pub pos: [f32; 3],
    pub along: f32,
    pub flow: [f32; 2],
    pub across: f32,
    pub half_width: f32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, bytemuck::Pod, bytemuck::Zeroable)]
struct RiverParams {
    colour: [f32; 4],
    flow: [f32; 4],
    optics: [f32; 4],
    misc: [f32; 4],
    bed: [f32; 4],
}

/// The look the loader read from the river's MatRiver (see `RiverLook::from_floats`).
#[derive(Clone, Copy, Debug)]
pub struct RiverLook {
    pub colour: [f32; 3],
    pub half_light: f32,
    pub flow_speed: f32,
    pub fresnel: f32,
    pub sun_power: f32,
    pub sun_influence: f32,
    pub wet_floor: f32,
    /// RFG-190: `WaterVisibility` (m) -- how far one sees through the water (5% contrast left);
    /// Reforger's pools and rivers author 1. Still water takes its view extinction from it.
    pub visibility: f32,
}

impl Default for RiverLook {
    fn default() -> Self {
        // RiverSurface_01.emat (Everon's rivers).
        Self {
            colour: [0.035, 0.04, 0.019],
            half_light: 30.0,
            flow_speed: 0.2,
            fresnel: 0.02,
            sun_power: 1024.0,
            sun_influence: 0.025,
            wet_floor: 0.3,
            visibility: 1.0,
        }
    }
}

impl RiverLook {
    /// `[r, g, b, HalfLightDistance, FlowSpeed, Fresnel, SunPower, SunInfluence, WetFloor]`;
    /// missing or non-finite entries keep the RiverSurface_01 values.
    pub fn from_floats(v: &[f32]) -> Self {
        let mut look = Self::default();
        let get = |i: usize| v.get(i).copied().filter(|x| x.is_finite());
        for c in 0..3 {
            if let Some(x) = get(c) {
                look.colour[c] = x.clamp(0.0, 1.0);
            }
        }
        if let Some(x) = get(3) { look.half_light = x.clamp(0.5, 500.0); }
        if let Some(x) = get(4) { look.flow_speed = x.clamp(0.0, 5.0); }
        if let Some(x) = get(5) { look.fresnel = x.clamp(0.0, 1.0); }
        if let Some(x) = get(6) { look.sun_power = x.clamp(8.0, 8192.0); }
        if let Some(x) = get(7) { look.sun_influence = x.clamp(0.0, 4.0); }
        if let Some(x) = get(8) { look.wet_floor = x.clamp(0.0, 0.9); }
        if let Some(x) = get(9) { look.visibility = x.clamp(0.05, 100.0); }
        look
    }
}

/// LAKE-UW-001: minimum underwater visibility (m) in still water; see `River::lake_water`.
fn lake_underwater_visibility() -> f32 {
    static V: std::sync::OnceLock<f32> = std::sync::OnceLock::new();
    *V.get_or_init(|| env_f32("WGR_LAKE_UNDERWATER_VIS", 6.0, 0.0, 50.0))
}

fn level_in(tris: &[[f32; 7]], x: f32, z: f32) -> Option<f32> {
    let edge = |ax: f32, az: f32, bx: f32, bz: f32| (bx - ax) * (z - az) - (bz - az) * (x - ax);
    tris.iter().find_map(|t| {
        if x < t[0].min(t[2]).min(t[4]) || x > t[0].max(t[2]).max(t[4]) ||
            z < t[1].min(t[3]).min(t[5]) || z > t[1].max(t[3]).max(t[5]) {
            return None;
        }
        let (e0, e1, e2) = (edge(t[0], t[1], t[2], t[3]), edge(t[2], t[3], t[4], t[5]), edge(t[4], t[5], t[0], t[1]));
        ((e0 >= 0.0 && e1 >= 0.0 && e2 >= 0.0) || (e0 <= 0.0 && e1 <= 0.0 && e2 <= 0.0)).then_some(t[6])
    })
}

/// LAKE-FISH-001: a copy of the ponds' triangles the engine's main thread may query without the
/// renderer (wildlife runs on the game thread while the worker owns `River`). Written only when
/// the world's rivers are replaced; read a few times a second.
static STILL_TRIS: std::sync::RwLock<Vec<[f32; 7]>> = std::sync::RwLock::new(Vec::new());

fn publish_still_tris(tris: &[[f32; 7]]) {
    if let Ok(mut g) = STILL_TRIS.write() {
        g.clear();
        g.extend_from_slice(tris);
    }
}

/// LAKE-FISH-001: the level of the pond covering (x, z), from any thread.
pub fn shared_still_level_at(x: f32, z: f32) -> Option<f32> {
    STILL_TRIS.read().ok().and_then(|g| level_in(&g, x, z))
}

fn env_f32(key: &str, default: f32, lo: f32, hi: f32) -> f32 {
    std::env::var(key)
        .ok()
        .and_then(|v| v.trim().parse::<f32>().ok())
        .filter(|v| v.is_finite())
        .map_or(default, |v| v.clamp(lo, hi))
}

struct RiverMesh {
    vertices: wgpu::Buffer,
    indices: wgpu::Buffer,
    /// Runs of the index buffer (a stretch of one river, or one pond) with their bounds, so a
    /// frame draws -- and pays the water pass's snapshot for -- only rivers near the camera.
    chunks: Vec<RiverChunk>,
}

#[derive(Clone, Copy, Debug)]
struct RiverChunk {
    centre: [f32; 3],
    radius: f32,
    first: u32,
    count: u32,
}

/// Indices per chunk: 64 quads of ribbon.
const CHUNK_INDICES: usize = 384;

fn build_chunks(vertices: &[RiverVertex], indices: &[u32]) -> Vec<RiverChunk> {
    let mut chunks = Vec::new();
    for (n, run) in indices.chunks(CHUNK_INDICES).enumerate() {
        let mut lo = [f32::MAX; 3];
        let mut hi = [f32::MIN; 3];
        for &i in run {
            let p = vertices[i as usize].pos;
            for c in 0..3 {
                lo[c] = lo[c].min(p[c]);
                hi[c] = hi[c].max(p[c]);
            }
        }
        let centre = [(lo[0] + hi[0]) * 0.5, (lo[1] + hi[1]) * 0.5, (lo[2] + hi[2]) * 0.5];
        let radius = ((hi[0] - lo[0]).powi(2) + (hi[1] - lo[1]).powi(2) + (hi[2] - lo[2]).powi(2)).sqrt() * 0.5;
        chunks.push(RiverChunk {
            centre,
            radius,
            first: (n * CHUNK_INDICES) as u32,
            count: run.len() as u32,
        });
    }
    chunks
}

/// The index ranges worth drawing from `cam`: within `reach` metres and not wholly behind the
/// camera (`fwd` its unit forward). Adjacent chunks merge into one range.
fn visible_ranges(chunks: &[RiverChunk], cam: [f32; 3], fwd: [f32; 3], reach: f32) -> Vec<(u32, u32)> {
    let mut out: Vec<(u32, u32)> = Vec::new();
    for c in chunks {
        let d = [c.centre[0] - cam[0], c.centre[1] - cam[1], c.centre[2] - cam[2]];
        let dist = (d[0] * d[0] + d[1] * d[1] + d[2] * d[2]).sqrt();
        if dist - c.radius > reach {
            continue;
        }
        if d[0] * fwd[0] + d[1] * fwd[1] + d[2] * fwd[2] < -c.radius {
            continue;
        }
        match out.last_mut() {
            Some(last) if last.0 + last.1 == c.first => last.1 += c.count,
            _ => out.push((c.first, c.count)),
        }
    }
    out
}

pub struct River {
    params: RiverParams,
    params_buffer: wgpu::Buffer,
    layout: wgpu::BindGroupLayout,
    pipeline: wgpu::RenderPipeline,
    scene_sampler: wgpu::Sampler,
    env_sampler: wgpu::Sampler,
    mesh: Option<RiverMesh>,
    /// This frame's index ranges (cull).
    visible: Vec<(u32, u32)>,
    bind: Option<(wgpu::BindGroup, (u64, u64))>,
    start: std::time::Instant,
    /// RFG-191: the ponds' triangles on the CPU (x, z of three corners + the level), so the frame
    /// can ask which lake, if any, the camera stands in -- the underwater gate for lakes.
    still_tris: Vec<[f32; 7]>,
}

impl River {
    pub fn new(
        device: &wgpu::Device,
        camera_layout: &wgpu::BindGroupLayout,
        color_format: wgpu::TextureFormat,
        sample_count: u32,
        composer: &mut naga_oil::compose::Composer,
    ) -> Self {
        let params_buffer = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("wgr_river_params"),
            size: std::mem::size_of::<RiverParams>() as u64,
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        let texture = |binding: u32, sample_type: wgpu::TextureSampleType| wgpu::BindGroupLayoutEntry {
            binding,
            visibility: wgpu::ShaderStages::FRAGMENT,
            ty: wgpu::BindingType::Texture {
                sample_type,
                view_dimension: wgpu::TextureViewDimension::D2,
                multisampled: false,
            },
            count: None,
        };
        let sampler = |binding: u32| wgpu::BindGroupLayoutEntry {
            binding,
            visibility: wgpu::ShaderStages::FRAGMENT,
            ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Filtering),
            count: None,
        };
        let layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("wgr_river_layout"),
            entries: &[
                wgpu::BindGroupLayoutEntry {
                    binding: 0,
                    visibility: wgpu::ShaderStages::VERTEX | wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Buffer {
                        ty: wgpu::BufferBindingType::Uniform,
                        has_dynamic_offset: false,
                        min_binding_size: None,
                    },
                    count: None,
                },
                texture(1, wgpu::TextureSampleType::Depth),
                texture(2, wgpu::TextureSampleType::Float { filterable: true }),
                sampler(3),
                texture(4, wgpu::TextureSampleType::Float { filterable: true }),
                sampler(5),
            ],
        });
        let shader = crate::shaders::make_module(
            device,
            composer,
            "wgr_river_shader",
            include_str!("river.wgsl"),
            "river/river.wgsl",
        );
        let pipeline_layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("wgr_river_pipeline_layout"),
            bind_group_layouts: &[Some(camera_layout), Some(&layout)],
            immediate_size: 0,
        });
        let attributes = wgpu::vertex_attr_array![
            0 => Float32x3, 1 => Float32, 2 => Float32x2, 3 => Float32, 4 => Float32
        ];
        let pipeline = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
            label: Some("wgr_river"),
            layout: Some(&pipeline_layout),
            vertex: wgpu::VertexState {
                module: &shader,
                entry_point: Some("vs_river"),
                compilation_options: Default::default(),
                buffers: &[Some(wgpu::VertexBufferLayout {
                    array_stride: std::mem::size_of::<RiverVertex>() as u64,
                    step_mode: wgpu::VertexStepMode::Vertex,
                    attributes: &attributes,
                })],
            },
            primitive: wgpu::PrimitiveState {
                topology: wgpu::PrimitiveTopology::TriangleList,
                // Seen from above only in practice, but a ribbon's winding follows the spline's
                // direction, which nothing guarantees: draw both sides.
                cull_mode: None,
                ..Default::default()
            },
            // The water pass's depth: reversed-Z, tested, never written -- the river is a film
            // over its bed, and the banks that cover its edges must keep winning.
            depth_stencil: Some(wgpu::DepthStencilState {
                format: depth_format(device),
                depth_write_enabled: Some(false),
                depth_compare: Some(wgpu::CompareFunction::GreaterEqual),
                stencil: wgpu::StencilState::default(),
                bias: wgpu::DepthBiasState::default(),
            }),
            multisample: wgpu::MultisampleState {
                count: sample_count,
                ..Default::default()
            },
            fragment: Some(wgpu::FragmentState {
                module: &shader,
                entry_point: Some("fs_river"),
                compilation_options: Default::default(),
                targets: &[Some(wgpu::ColorTargetState {
                    format: color_format,
                    // Opaque: the shader composites the refracted scene itself.
                    blend: None,
                    write_mask: wgpu::ColorWrites::ALL,
                })],
            }),
            multiview_mask: None,
            cache: crate::pipeline_cache::get(&device),
        });
        let linear = |label: &str, mode: wgpu::AddressMode| {
            device.create_sampler(&wgpu::SamplerDescriptor {
                label: Some(label),
                address_mode_u: mode,
                address_mode_v: wgpu::AddressMode::ClampToEdge,
                mag_filter: wgpu::FilterMode::Linear,
                min_filter: wgpu::FilterMode::Linear,
                ..Default::default()
            })
        };
        let mut river = Self {
            params: bytemuck::Zeroable::zeroed(),
            params_buffer,
            layout,
            pipeline,
            scene_sampler: linear("wgr_river_scene_samp", wgpu::AddressMode::ClampToEdge),
            env_sampler: linear("wgr_river_env_samp", wgpu::AddressMode::Repeat),
            mesh: None,
            visible: Vec::new(),
            bind: None,
            start: std::time::Instant::now(),
            still_tris: Vec::new(),
        };
        river.set_look(RiverLook::default());
        river
    }

    /// The look, from the river's MatRiver plus the WGR_RIVER_* overrides.
    pub fn set_look(&mut self, look: RiverLook) {
        self.params.colour = [look.colour[0], look.colour[1], look.colour[2], look.half_light];
        self.params.flow = [
            env_f32("WGR_RIVER_FLOW_SPEED", look.flow_speed.max(0.05) * 4.0, 0.0, 10.0),
            env_f32("WGR_RIVER_RIPPLE_TILING", 1.6, 0.05, 20.0),
            env_f32("WGR_RIVER_RIPPLE", 1.0, 0.0, 8.0),
            0.0,
        ];
        self.params.optics = [
            look.fresnel,
            look.sun_power,
            env_f32("WGR_RIVER_GLINT", look.sun_influence * 8.0, 0.0, 10.0),
            env_f32("WGR_RIVER_REFRACT", 0.06, 0.0, 1.0),
        ];
        self.params.misc = [
            env_f32("WGR_RIVER_SHORE", 0.04, 0.001, 1.0),
            env_f32("WGR_RIVER_SSR", 1.0, 0.0, 1.0),
            // RFG-211: 0.6 -> 0.3. The scattered light is Color x (sky + sun), and the native sky
            // now carries Lighting_Eden's indirect share (RFG-196): at 0.6 Everon's Lake_Center
            // read +0.81 EV over Reforger (meadowNear); 0.3 measures +0.42, 0.15 no better (the
            // rest is the sky's reflection), river1 unchanged at every value.
            env_f32("WGR_RIVER_SCATTER", 0.3, 0.0, 4.0),
            env_f32("WGR_RIVER_DEBUG", 0.0, 0.0, 1.0),
        ];
        // RFG-166: y = the water body's own colour per metre of column (Reforger's rivers read dark
        // olive even 10-30 cm deep, which ln2/HalfLightDistance absorption alone cannot give);
        // z = how much a grazing reflection that finds no screen-space hit takes the banks'
        // colour instead of open sky (a river runs in a channel; the lavender sky sheen was the
        // owner-visible "invisible river").
        self.params.bed = [
            env_f32("WGR_RIVER_WET_FLOOR", look.wet_floor.max(0.38), 0.0, 0.9),
            env_f32("WGR_RIVER_BODY", 2.5, 0.0, 20.0),
            env_f32("WGR_RIVER_BANK", 1.0, 0.0, 1.0),
            // RFG-190: w = WaterVisibility (m) for still water; 0 (WGR_POND_VISIBILITY=0) keeps
            // ponds on the HalfLightDistance extinction the rivers use.
            if std::env::var("WGR_POND_VISIBILITY").map(|v| v.trim() == "0").unwrap_or(false) { 0.0 } else { look.visibility },
        ];
    }

    /// Replace every river of the world. Empty releases them. Returns the triangle count.
    pub fn set_mesh(&mut self, device: &wgpu::Device, vertices: &[RiverVertex], indices: &[u32]) -> u32 {
        use wgpu::util::DeviceExt;
        self.mesh = None;
        self.still_tris.clear();
        publish_still_tris(&[]);
        let count = vertices.len() as u32;
        // Every index must name a vertex: a bad index is a GPU fault, not a bad picture.
        if vertices.is_empty() || indices.len() < 3 || indices.iter().any(|&i| i >= count) {
            return 0;
        }
        let usable = indices.len() / 3 * 3;
        for tri in indices[..usable].chunks_exact(3) {
            let (a, b, c) = (&vertices[tri[0] as usize], &vertices[tri[1] as usize], &vertices[tri[2] as usize]);
            if a.half_width < 0.0 && b.half_width < 0.0 && c.half_width < 0.0 {
                self.still_tris.push([a.pos[0], a.pos[2], b.pos[0], b.pos[2], c.pos[0], c.pos[2], a.pos[1]]);
            }
        }
        publish_still_tris(&self.still_tris);
        let vb = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
            label: Some("wgr_river_vertices"),
            contents: bytemuck::cast_slice(vertices),
            usage: wgpu::BufferUsages::VERTEX,
        });
        let ib = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
            label: Some("wgr_river_indices"),
            contents: bytemuck::cast_slice(&indices[..usable]),
            usage: wgpu::BufferUsages::INDEX,
        });
        self.mesh = Some(RiverMesh {
            vertices: vb,
            indices: ib,
            chunks: build_chunks(vertices, &indices[..usable]),
        });
        self.visible.clear();
        (usable / 3) as u32
    }

    /// RFG-191: the level of the pond whose surface covers (x, z), if any.
    pub fn still_level_at(&self, x: f32, z: f32) -> Option<f32> {
        level_in(&self.still_tris, x, z)
    }

    /// RFG-191: the lake water's (absorption, scattering) per metre for the underwater view, from
    /// the authored colour and WaterVisibility: extinction 3 / visibility (5% contrast left there),
    /// split by the water's colour -- a dark olive pond scatters little and absorbs the rest.
    ///
    /// LAKE-UW-001: the authored visibility is for the surface (looking INTO a pond from above),
    /// and Reforger authors 1 m. Used as the underwater medium it left ~0.3% of the light 2 m
    /// down -- a diver in Everon's big lake (4632/5970) saw a black screen (mean RGB 0.8/0.9/0.3).
    /// Underwater it is floored at WGR_LAKE_UNDERWATER_VIS (default 6 m: a little murkier than the sea's
    /// ~7 m temperate water, still a lake one can see in). 0 = the authored value as before.
    pub fn lake_water(&self) -> ([f32; 3], [f32; 3]) {
        let c = [self.params.colour[0], self.params.colour[1], self.params.colour[2]];
        let vis = if self.params.bed[3] > 0.0 { self.params.bed[3] } else { 1.0 };
        let vis = vis.max(lake_underwater_visibility());
        let sigma_t = 2.9957 / vis.max(0.05);
        let peak = c[0].max(c[1]).max(c[2]).max(1e-4);
        let albedo = 0.12; // a murky pond: mostly absorption
        let mut absorption = [0.0; 3];
        let mut scattering = [0.0; 3];
        for i in 0..3 {
            // Channels the water reflects least are absorbed most (the colour's own hue).
            let keep = (c[i] / peak).clamp(0.05, 1.0);
            scattering[i] = sigma_t * albedo * keep;
            absorption[i] = sigma_t * (1.0 - albedo * keep);
        }
        (absorption, scattering)
    }

    /// Per frame, before the water pass: which stretches are near enough and in front.
    pub fn cull(&mut self, cam: [f32; 3], fwd: [f32; 3], reach: f32) {
        self.visible.clear();
        if let Some(mesh) = self.mesh.as_ref() {
            self.visible = visible_ranges(&mesh.chunks, cam, fwd, reach.max(100.0));
        }
    }

    /// Something to draw this frame (a mesh, a stretch in view, not switched off).
    pub fn drawable(&self) -> bool {
        self.mesh.is_some() && !self.visible.is_empty() && std::env::var("WGR_RIVERS").map_or(true, |v| v != "0")
    }

    /// Bind this frame's scene snapshot, resolved depth and sky env map (rebuilt only when the
    /// snapshot or the depth target changed), and push the clock.
    pub fn prepare(
        &mut self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        scene: &wgpu::TextureView,
        scene_gen: u64,
        depth: &wgpu::TextureView,
        depth_gen: u64,
        env: &wgpu::TextureView,
    ) {
        self.params.flow[3] = (self.start.elapsed().as_secs_f64() % 3600.0) as f32;
        queue.write_buffer(&self.params_buffer, 0, bytemuck::bytes_of(&self.params));
        let key = (scene_gen, depth_gen);
        if self.bind.as_ref().is_some_and(|(_, k)| *k == key) {
            return;
        }
        let bind = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("wgr_river_bind"),
            layout: &self.layout,
            entries: &[
                wgpu::BindGroupEntry { binding: 0, resource: self.params_buffer.as_entire_binding() },
                wgpu::BindGroupEntry { binding: 1, resource: wgpu::BindingResource::TextureView(depth) },
                wgpu::BindGroupEntry { binding: 2, resource: wgpu::BindingResource::TextureView(scene) },
                wgpu::BindGroupEntry { binding: 3, resource: wgpu::BindingResource::Sampler(&self.scene_sampler) },
                wgpu::BindGroupEntry { binding: 4, resource: wgpu::BindingResource::TextureView(env) },
                wgpu::BindGroupEntry { binding: 5, resource: wgpu::BindingResource::Sampler(&self.env_sampler) },
            ],
        });
        self.bind = Some((bind, key));
    }

    pub fn draw(&self, pass: &mut wgpu::RenderPass<'_>, camera_bind: &wgpu::BindGroup, camera_offset: u32) {
        let (Some(mesh), Some((bind, _))) = (self.mesh.as_ref(), self.bind.as_ref()) else {
            return;
        };
        pass.set_pipeline(&self.pipeline);
        pass.set_bind_group(0, camera_bind, &[camera_offset]);
        pass.set_bind_group(1, bind, &[]);
        pass.set_vertex_buffer(0, mesh.vertices.slice(..));
        pass.set_index_buffer(mesh.indices.slice(..), wgpu::IndexFormat::Uint32);
        for &(first, count) in &self.visible {
            pass.draw_indexed(first..first + count, 0, 0..1);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn vertex_layout_is_the_eight_floats_the_loader_writes() {
        assert_eq!(std::mem::size_of::<RiverVertex>(), 32);
        assert_eq!(std::mem::offset_of!(RiverVertex, along), 12);
        assert_eq!(std::mem::offset_of!(RiverVertex, flow), 16);
        assert_eq!(std::mem::offset_of!(RiverVertex, across), 24);
        assert_eq!(std::mem::offset_of!(RiverVertex, half_width), 28);
    }

    #[test]
    fn cull_keeps_near_chunks_in_front_and_merges_neighbours() {
        let v = |x: f32, z: f32| RiverVertex { pos: [x, 0.0, z], ..Default::default() };
        // Three 384-index chunks along +x: near (0..10), near (10..20), far (5000).
        let mut verts = Vec::new();
        let mut idx = Vec::new();
        for (k, x0) in [0.0f32, 10.0, 5000.0].iter().enumerate() {
            let base = verts.len() as u32;
            verts.extend([v(*x0, 0.0), v(*x0 + 10.0, 0.0), v(*x0, 5.0)]);
            for _ in 0..CHUNK_INDICES / 3 {
                idx.extend([base, base + 1, base + 2]);
            }
            assert_eq!(idx.len(), (k + 1) * CHUNK_INDICES);
        }
        let chunks = build_chunks(&verts, &idx);
        assert_eq!(chunks.len(), 3);
        let ranges = visible_ranges(&chunks, [0.0, 2.0, 0.0], [1.0, 0.0, 0.0], 1000.0);
        assert_eq!(ranges, vec![(0, 2 * CHUNK_INDICES as u32)]);
        // Looking the other way: the chunk the camera stands in stays (its radius reaches it).
        let back = visible_ranges(&chunks, [30.0, 2.0, 0.0], [1.0, 0.0, 0.0], 1000.0);
        assert!(back.is_empty() || back[0].0 >= CHUNK_INDICES as u32);
    }

    #[test]
    fn look_defaults_are_river_surface_01_and_bad_values_keep_them() {
        let look = RiverLook::from_floats(&[f32::NAN, 0.5, 0.2, -3.0]);
        assert_eq!(look.colour[0], 0.035);
        assert_eq!(look.colour[1], 0.5);
        assert_eq!(look.half_light, 0.5);
        assert_eq!(look.fresnel, 0.02);
    }

    #[test]
    fn the_shader_composes_and_validates() {
        let mut composer = crate::shaders::build_composer();
        let module = composer
            .make_naga_module(naga_oil::compose::NagaModuleDescriptor {
                source: include_str!("river.wgsl"),
                file_path: "river/river.wgsl",
                shader_defs: crate::shaders::shader_defs(),
                ..Default::default()
            })
            .unwrap_or_else(|e| panic!("river.wgsl: {}", e.emit_to_string(&composer)));
        naga::valid::Validator::new(naga::valid::ValidationFlags::all(), naga::valid::Capabilities::all())
            .validate(&module)
            .expect("river.wgsl validates");
    }
}
