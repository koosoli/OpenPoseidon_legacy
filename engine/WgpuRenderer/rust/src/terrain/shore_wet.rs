//! TW-WATER W3b — Tidewater's wet sand on OP's terrain (bind group 3 of the terrain pipelines).
//!
//! In Tidewater mode the coast's wetness comes from the shore simulation's wet channel, and the
//! foam the draining water strands on the sand is drawn from its residue channel (Tidewater
//! App.js `terrain.wetness` → `terrainWetness(xz, h)`; plan §6 W3: "terrain wetness in this mode
//! comes only from the ShoreSim wet channel"). Outside the simulated regions Tidewater's own
//! fallback, a static damp band, applies. With Current OP (or the shore simulation off) the
//! group is a placeholder with `on = 0` and terrain.wgsl keeps OP's coast wet band unchanged.

/// `TwWet` in terrain.wgsl.
#[repr(C)]
#[derive(Clone, Copy, Debug, bytemuck::Pod, bytemuck::Zeroable)]
pub struct TwWetParams {
    /// per shore-simulation region: min xz (world), size (m), weight (0 = unused)
    pub regions: [[f32; 4]; 2],
    /// x = on, y = sea level, zw unused
    pub p: [f32; 4],
}

pub struct ShoreWet {
    layout: wgpu::BindGroupLayout,
    ubo: wgpu::Buffer,
    bind: wgpu::BindGroup,
    dummy_state: wgpu::TextureView,
    dummy_lace: wgpu::TextureView,
    /// generation of the views in `bind` (0 = the placeholders)
    generation: u64,
}

fn dummy(device: &wgpu::Device, label: &str, format: wgpu::TextureFormat, array: bool) -> wgpu::TextureView {
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
        .create_view(&wgpu::TextureViewDescriptor {
            dimension: Some(if array { wgpu::TextureViewDimension::D2Array } else { wgpu::TextureViewDimension::D2 }),
            ..Default::default()
        })
}

impl ShoreWet {
    pub fn new(device: &wgpu::Device, queue: &wgpu::Queue) -> Self {
        let f = wgpu::ShaderStages::FRAGMENT;
        let tex = |binding, dim| wgpu::BindGroupLayoutEntry {
            binding,
            visibility: f,
            ty: wgpu::BindingType::Texture {
                sample_type: wgpu::TextureSampleType::Float { filterable: true },
                view_dimension: dim,
                multisampled: false,
            },
            count: None,
        };
        let layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("wgr_terrain_tw_wet_layout"),
            entries: &[
                wgpu::BindGroupLayoutEntry {
                    binding: 0,
                    visibility: f,
                    ty: wgpu::BindingType::Buffer {
                        ty: wgpu::BufferBindingType::Uniform,
                        has_dynamic_offset: false,
                        min_binding_size: None,
                    },
                    count: None,
                },
                tex(1, wgpu::TextureViewDimension::D2Array),
                tex(2, wgpu::TextureViewDimension::D2),
            ],
        });
        let ubo = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("wgr_terrain_tw_wet"),
            size: std::mem::size_of::<TwWetParams>() as u64,
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        queue.write_buffer(&ubo, 0, bytemuck::bytes_of(&<TwWetParams as bytemuck::Zeroable>::zeroed()));
        let dummy_state = dummy(device, "wgr_terrain_tw_wet_state_dummy", wgpu::TextureFormat::Rgba16Float, true);
        let dummy_lace = dummy(device, "wgr_terrain_tw_wet_lace_dummy", wgpu::TextureFormat::Rgba8Unorm, false);
        let bind = Self::make_bind(device, &layout, &ubo, &dummy_state, &dummy_lace);
        Self { layout, ubo, bind, dummy_state, dummy_lace, generation: 0 }
    }

    fn make_bind(
        device: &wgpu::Device,
        layout: &wgpu::BindGroupLayout,
        ubo: &wgpu::Buffer,
        state: &wgpu::TextureView,
        lace: &wgpu::TextureView,
    ) -> wgpu::BindGroup {
        device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("wgr_terrain_tw_wet"),
            layout,
            entries: &[
                wgpu::BindGroupEntry { binding: 0, resource: ubo.as_entire_binding() },
                wgpu::BindGroupEntry { binding: 1, resource: wgpu::BindingResource::TextureView(state) },
                wgpu::BindGroupEntry { binding: 2, resource: wgpu::BindingResource::TextureView(lace) },
            ],
        })
    }

    pub fn layout(&self) -> &wgpu::BindGroupLayout {
        &self.layout
    }

    pub fn bind(&self) -> &wgpu::BindGroup {
        &self.bind
    }

    /// Per frame: Tidewater's wetness inputs, or None for OP's own coast wet band.
    pub fn set(
        &mut self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        wet: Option<(&wgpu::TextureView, &wgpu::TextureView, u64, TwWetParams)>,
    ) {
        match wet {
            Some((state, lace, generation, params)) => {
                if self.generation != generation {
                    self.bind = Self::make_bind(device, &self.layout, &self.ubo, state, lace);
                    self.generation = generation;
                }
                queue.write_buffer(&self.ubo, 0, bytemuck::bytes_of(&params));
            }
            None => {
                if self.generation != 0 {
                    self.bind = Self::make_bind(device, &self.layout, &self.ubo, &self.dummy_state, &self.dummy_lace);
                    self.generation = 0;
                    queue.write_buffer(&self.ubo, 0, bytemuck::bytes_of(&<TwWetParams as bytemuck::Zeroable>::zeroed()));
                }
            }
        }
    }
}

#[cfg(test)]
mod tests {
    #[test]
    fn params_match_the_wgsl_struct() {
        assert_eq!(std::mem::size_of::<super::TwWetParams>(), 48);
    }
}
