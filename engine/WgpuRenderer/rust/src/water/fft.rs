use bytemuck::Zeroable;
use wgpu::util::DeviceExt;

// The optimized gameplay default is 512. GodotOceanWaves' 1024 reference resolution and a
// 256 performance tier are live-selectable; changing tier reconstructs only the FFT resources.
// The FFT is the most expensive water pass and scales as O(N^2 log N), so 512 keeps the short
// wave detail that matters in motion without forcing the roughly 4x reference cost on everyone.
//
// This costs less visually than it sounds, because of how k is indexed: the spectrum builds
// k = 2*pi*(id - N/2)/L, so the mode SPACING (2*pi/L) is set by the cascade length and does not
// change with N — only the maximum wavenumber does. Dropping to 512 therefore keeps every
// low-wavenumber mode (where a wind sea carries essentially all of its energy, the TMA spectrum
// falling off as omega^-5) and discards only the shortest-wavelength tail. Significant wave height
// is effectively unchanged; what is lost is the finest ripple detail.
//
// The de-tiling warp (Water tab "De-tile warp") is what keeps the ocean from reading as a repeating
// grid at distance, not the raw mode count.
pub const FFT_RESOLUTION: u32 = 512;
const FFT_LAYERS: u32 = 4;
pub const FFT_MIN_RESOLUTION: u32 = 256;
pub const FFT_MAX_RESOLUTION: u32 = 1024;
// Spectra packed into RGBA32F texture pairs: pack0 = (hx,hy),(hz,dhy_dx); pack1 = (dhy_dz,dhx_dx),
// (dhz_dz,dhz_dx). A third pack exists in the bindings but carries no data — see dispatch().
const FFT_ACTIVE_PACKS: u32 = 2;

// These are the only WaterParams values used to construct h0. Compare their raw bits so live
// per-frame uploads do not recreate the spectrum, while every authored spectrum change does.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct SpectrumInputs {
    enabled: u32,
    seed: u32,
    wave_amp: u32,
    wind_sea: [u32; 4],
    cascade_lengths: [u32; 4],
}

impl SpectrumInputs {
    fn from_params(params: &crate::ffi::WgrWaterParams) -> Self {
        Self {
            enabled: params.fft_control[0].to_bits(),
            seed: params.fft_control[1].to_bits(),
            wave_amp: params.wave_amp.to_bits(),
            wind_sea: params.fft_wind_sea.map(f32::to_bits),
            cascade_lengths: params.fft_cascade_lengths.map(f32::to_bits),
        }
    }
}

#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
struct StageParams {
    data: [u32; 4],
}

pub struct Fft {
    resolution: u32,
    displacement: wgpu::TextureView,
    dynamics: wgpu::TextureView,
    auxiliary: wgpu::TextureView,
    cascade_config_ubo: wgpu::Buffer,
    cascade_configs: [crate::ffi::WgrWaterCascadeConfig; FFT_LAYERS as usize],
    h0_init_bind: wgpu::BindGroup,
    spectrum_bind: wgpu::BindGroup,
    stage_binds: Vec<wgpu::BindGroup>,
    compose_bind: wgpu::BindGroup,
    h0_init_pipeline: wgpu::ComputePipeline,
    spectrum_pipeline: wgpu::ComputePipeline,
    stage_pipeline: wgpu::ComputePipeline,
    compose_pipeline: wgpu::ComputePipeline,
    stage_alignment: u32,
    spectrum_inputs: Option<SpectrumInputs>,
    spectrum_dirty: bool,
    // Highest enabled cascade + 1. Every FFT pass dispatches over the array layers, and this used
    // to be hardcoded to all 4 even when a preset enabled fewer — the GodotOceanWaves preset only
    // uses 3, so a quarter of the most expensive pass in the whole water system was transforming a
    // cascade that is zeroed out and never sampled.
    active_layers: u32,
}

impl Fft {
    pub fn new(
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        water_params: &wgpu::Buffer,
        storage_supported: bool,
        requested_resolution: u32,
    ) -> Option<Self> {
        if !storage_supported || std::env::var("WGR_WATER_FFT").ok().as_deref() == Some("0") {
            return None;
        }
        let resolution = match requested_resolution {
            FFT_MIN_RESOLUTION | FFT_RESOLUTION | FFT_MAX_RESOLUTION => requested_resolution,
            _ => FFT_RESOLUTION,
        };
        let array_view = |format, usage, label: &str| {
            device
                .create_texture(&wgpu::TextureDescriptor {
                    label: Some(label),
                    size: wgpu::Extent3d {
                        width: resolution,
                        height: resolution,
                        depth_or_array_layers: FFT_LAYERS,
                    },
                    mip_level_count: 1,
                    sample_count: 1,
                    dimension: wgpu::TextureDimension::D2,
                    format,
                    usage,
                    view_formats: &[],
                })
                .create_view(&wgpu::TextureViewDescriptor {
                    dimension: Some(wgpu::TextureViewDimension::D2Array),
                    ..Default::default()
                })
        };
        let packs = [
            [
                array_view(
                    wgpu::TextureFormat::Rgba32Float,
                    wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::STORAGE_BINDING,
                    "wgr_fft_pack0_a",
                ),
                array_view(
                    wgpu::TextureFormat::Rgba32Float,
                    wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::STORAGE_BINDING,
                    "wgr_fft_pack0_b",
                ),
            ],
            [
                array_view(
                    wgpu::TextureFormat::Rgba32Float,
                    wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::STORAGE_BINDING,
                    "wgr_fft_pack1_a",
                ),
                array_view(
                    wgpu::TextureFormat::Rgba32Float,
                    wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::STORAGE_BINDING,
                    "wgr_fft_pack1_b",
                ),
            ],
            [
                array_view(
                    wgpu::TextureFormat::Rgba32Float,
                    wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::STORAGE_BINDING,
                    "wgr_fft_pack2_a",
                ),
                array_view(
                    wgpu::TextureFormat::Rgba32Float,
                    wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::STORAGE_BINDING,
                    "wgr_fft_pack2_b",
                ),
            ],
        ];
        // h0 is the persistent random field. It is written only after spectrum inputs change and
        // read by the per-frame evolution pass.
        let h0 = array_view(
            wgpu::TextureFormat::Rgba32Float,
            wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::STORAGE_BINDING,
            "wgr_fft_h0",
        );
        let out_usage = wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::STORAGE_BINDING;
        let displacement = array_view(
            wgpu::TextureFormat::Rgba16Float,
            out_usage,
            "wgr_fft_displacement",
        );
        let dynamics = array_view(
            wgpu::TextureFormat::Rgba16Float,
            out_usage,
            "wgr_fft_dynamics",
        );
        let auxiliary = array_view(
            wgpu::TextureFormat::Rgba16Float,
            out_usage,
            "wgr_fft_auxiliary",
        );
        let uniform_layout = |binding, ty| wgpu::BindGroupLayoutEntry {
            binding,
            visibility: wgpu::ShaderStages::COMPUTE,
            ty,
            count: None,
        };
        // The C++ renderer supplies one of these for every FFT layer.  Keep the
        // exact FFI layout in a GPU uniform instead of flattening the reference
        // parameters back into the legacy shared wind vector.
        let cascade_configs = [crate::ffi::WgrWaterCascadeConfig::zeroed(); FFT_LAYERS as usize];
        let cascade_config_ubo = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
            label: Some("wgr_fft_cascade_configs"),
            contents: bytemuck::cast_slice(&cascade_configs),
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
        });
        let h0_init_layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("wgr_fft_h0_init_layout"),
            entries: &[
                uniform_layout(
                    0,
                    wgpu::BindingType::Buffer {
                        ty: wgpu::BufferBindingType::Uniform,
                        has_dynamic_offset: false,
                        min_binding_size: None,
                    },
                ),
                uniform_layout(
                    1,
                    wgpu::BindingType::StorageTexture {
                        access: wgpu::StorageTextureAccess::WriteOnly,
                        format: wgpu::TextureFormat::Rgba32Float,
                        view_dimension: wgpu::TextureViewDimension::D2Array,
                    },
                ),
                uniform_layout(
                    2,
                    wgpu::BindingType::Buffer {
                        ty: wgpu::BufferBindingType::Uniform,
                        has_dynamic_offset: false,
                        min_binding_size: None,
                    },
                ),
            ],
        });
        let h0_init_bind = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("wgr_fft_h0_init_bind"),
            layout: &h0_init_layout,
            entries: &[
                wgpu::BindGroupEntry {
                    binding: 0,
                    resource: water_params.as_entire_binding(),
                },
                wgpu::BindGroupEntry {
                    binding: 1,
                    resource: wgpu::BindingResource::TextureView(&h0),
                },
                wgpu::BindGroupEntry {
                    binding: 2,
                    resource: cascade_config_ubo.as_entire_binding(),
                },
            ],
        });
        let spectrum_layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("wgr_fft_spectrum_layout"),
            entries: &[
                uniform_layout(
                    0,
                    wgpu::BindingType::Buffer {
                        ty: wgpu::BufferBindingType::Uniform,
                        has_dynamic_offset: false,
                        min_binding_size: None,
                    },
                ),
                uniform_layout(
                    1,
                    wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Float { filterable: false },
                        view_dimension: wgpu::TextureViewDimension::D2Array,
                        multisampled: false,
                    },
                ),
                uniform_layout(
                    2,
                    wgpu::BindingType::StorageTexture {
                        access: wgpu::StorageTextureAccess::WriteOnly,
                        format: wgpu::TextureFormat::Rgba32Float,
                        view_dimension: wgpu::TextureViewDimension::D2Array,
                    },
                ),
                uniform_layout(
                    3,
                    wgpu::BindingType::StorageTexture {
                        access: wgpu::StorageTextureAccess::WriteOnly,
                        format: wgpu::TextureFormat::Rgba32Float,
                        view_dimension: wgpu::TextureViewDimension::D2Array,
                    },
                ),
                uniform_layout(
                    4,
                    wgpu::BindingType::StorageTexture {
                        access: wgpu::StorageTextureAccess::WriteOnly,
                        format: wgpu::TextureFormat::Rgba32Float,
                        view_dimension: wgpu::TextureViewDimension::D2Array,
                    },
                ),
                uniform_layout(
                    5,
                    wgpu::BindingType::Buffer {
                        ty: wgpu::BufferBindingType::Uniform,
                        has_dynamic_offset: false,
                        min_binding_size: None,
                    },
                ),
            ],
        });
        let spectrum_bind = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("wgr_fft_spectrum_bind"),
            layout: &spectrum_layout,
            entries: &[
                wgpu::BindGroupEntry {
                    binding: 0,
                    resource: water_params.as_entire_binding(),
                },
                wgpu::BindGroupEntry {
                    binding: 1,
                    resource: wgpu::BindingResource::TextureView(&h0),
                },
                wgpu::BindGroupEntry {
                    binding: 2,
                    resource: wgpu::BindingResource::TextureView(&packs[0][0]),
                },
                wgpu::BindGroupEntry {
                    binding: 3,
                    resource: wgpu::BindingResource::TextureView(&packs[1][0]),
                },
                wgpu::BindGroupEntry {
                    binding: 4,
                    resource: wgpu::BindingResource::TextureView(&packs[2][0]),
                },
                wgpu::BindGroupEntry {
                    binding: 5,
                    resource: cascade_config_ubo.as_entire_binding(),
                },
            ],
        });
        let stage_layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("wgr_fft_stage_layout"),
            entries: &[
                uniform_layout(
                    0,
                    wgpu::BindingType::Buffer {
                        ty: wgpu::BufferBindingType::Uniform,
                        has_dynamic_offset: true,
                        min_binding_size: None,
                    },
                ),
                uniform_layout(
                    1,
                    wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Float { filterable: false },
                        view_dimension: wgpu::TextureViewDimension::D2Array,
                        multisampled: false,
                    },
                ),
                uniform_layout(
                    2,
                    wgpu::BindingType::StorageTexture {
                        access: wgpu::StorageTextureAccess::WriteOnly,
                        format: wgpu::TextureFormat::Rgba32Float,
                        view_dimension: wgpu::TextureViewDimension::D2Array,
                    },
                ),
            ],
        });
        // One source/destination pair per axis per complex pack. The whole transform for an
        // axis is now a single dispatch (see fft_row.wgsl), so this is 2 x 3 bind groups
        // rather than the former 2 x FFT_STAGES x 3 — the per-stage ping-pong is gone.
        //
        // Parity: axis 0 reads pack[..][0] and writes pack[..][1]; axis 1 reads back from [1]
        // and writes [0], leaving the result in parity 0 for `compose_bind` below. The old
        // per-stage code arrived at the same place by flipping 9 times per axis (odd, so it
        // also ended in [1] then [0]) — the final resting texture is unchanged.
        let mut stage_binds = Vec::new();
        let aligned = device.limits().min_uniform_buffer_offset_alignment as u64;
        let stage_buffer = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("wgr_fft_stage_params"),
            size: aligned * 2,
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        for axis in 0..2usize {
            for pack in 0..3 {
                stage_binds.push(device.create_bind_group(&wgpu::BindGroupDescriptor {
                    label: Some("wgr_fft_row_bind"),
                    layout: &stage_layout,
                    entries: &[
                        wgpu::BindGroupEntry {
                            binding: 0,
                            resource: wgpu::BindingResource::Buffer(wgpu::BufferBinding {
                                buffer: &stage_buffer,
                                offset: 0,
                                size: std::num::NonZeroU64::new(16),
                            }),
                        },
                        wgpu::BindGroupEntry {
                            binding: 1,
                            resource: wgpu::BindingResource::TextureView(&packs[pack][axis]),
                        },
                        wgpu::BindGroupEntry {
                            binding: 2,
                            resource: wgpu::BindingResource::TextureView(&packs[pack][1 - axis]),
                        },
                    ],
                }));
            }
        }
        // Parameters are immutable structural values, uploaded once to the dynamically addressed UBO.
        let mut bytes = vec![0u8; (aligned * 2) as usize];
        for axis in 0..2u32 {
            let offset = (axis as u64 * aligned) as usize;
            bytes[offset..offset + 16].copy_from_slice(bytemuck::bytes_of(&StageParams {
                data: [
                    resolution,
                    // 0 = transform along x (rows), 1 = along y (columns).
                    axis,
                    resolution.ilog2(),
                    // GodotOceanWaves synthesizes physical Fourier-series
                    // coefficients and intentionally leaves its inverse FFT
                    // unnormalised. Dividing the final stage by N^2 reduced a
                    // 256x256 ocean by 65,536 and made it visually flat.
                    0,
                ],
            }));
        }
        queue.write_buffer(&stage_buffer, 0, &bytes);
        let compose_layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("wgr_fft_compose_layout"),
            entries: &[
                uniform_layout(
                    0,
                    wgpu::BindingType::Buffer {
                        ty: wgpu::BufferBindingType::Uniform,
                        has_dynamic_offset: false,
                        min_binding_size: None,
                    },
                ),
                uniform_layout(
                    1,
                    wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Float { filterable: false },
                        view_dimension: wgpu::TextureViewDimension::D2Array,
                        multisampled: false,
                    },
                ),
                uniform_layout(
                    2,
                    wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Float { filterable: false },
                        view_dimension: wgpu::TextureViewDimension::D2Array,
                        multisampled: false,
                    },
                ),
                uniform_layout(
                    3,
                    wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Float { filterable: false },
                        view_dimension: wgpu::TextureViewDimension::D2Array,
                        multisampled: false,
                    },
                ),
                uniform_layout(
                    4,
                    wgpu::BindingType::StorageTexture {
                        access: wgpu::StorageTextureAccess::WriteOnly,
                        format: wgpu::TextureFormat::Rgba16Float,
                        view_dimension: wgpu::TextureViewDimension::D2Array,
                    },
                ),
                uniform_layout(
                    5,
                    wgpu::BindingType::StorageTexture {
                        access: wgpu::StorageTextureAccess::WriteOnly,
                        format: wgpu::TextureFormat::Rgba16Float,
                        view_dimension: wgpu::TextureViewDimension::D2Array,
                    },
                ),
                uniform_layout(
                    6,
                    wgpu::BindingType::StorageTexture {
                        access: wgpu::StorageTextureAccess::WriteOnly,
                        format: wgpu::TextureFormat::Rgba16Float,
                        view_dimension: wgpu::TextureViewDimension::D2Array,
                    },
                ),
                uniform_layout(
                    7,
                    wgpu::BindingType::Buffer {
                        ty: wgpu::BufferBindingType::Uniform,
                        has_dynamic_offset: false,
                        min_binding_size: None,
                    },
                ),
            ],
        });
        // X starts in ping (0) and ends in pong; Y starts from that pong and ends in ping.
        let final_parity = 0usize;
        let compose_bind = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("wgr_fft_compose_bind"),
            layout: &compose_layout,
            entries: &[
                wgpu::BindGroupEntry {
                    binding: 0,
                    resource: water_params.as_entire_binding(),
                },
                wgpu::BindGroupEntry {
                    binding: 1,
                    resource: wgpu::BindingResource::TextureView(&packs[0][final_parity]),
                },
                wgpu::BindGroupEntry {
                    binding: 2,
                    resource: wgpu::BindingResource::TextureView(&packs[1][final_parity]),
                },
                wgpu::BindGroupEntry {
                    binding: 3,
                    resource: wgpu::BindingResource::TextureView(&packs[2][final_parity]),
                },
                wgpu::BindGroupEntry {
                    binding: 4,
                    resource: wgpu::BindingResource::TextureView(&displacement),
                },
                wgpu::BindGroupEntry {
                    binding: 5,
                    resource: wgpu::BindingResource::TextureView(&dynamics),
                },
                wgpu::BindGroupEntry {
                    binding: 6,
                    resource: wgpu::BindingResource::TextureView(&auxiliary),
                },
                wgpu::BindGroupEntry {
                    binding: 7,
                    resource: cascade_config_ubo.as_entire_binding(),
                },
            ],
        });
        let pipeline = |label, source: &'static str, entry, layout: &wgpu::BindGroupLayout| {
            // FFT shaders are deliberately standalone WGSL (no naga-oil imports), allowing
            // this resource set to be reconstructed live when the dev quality tier changes.
            let shader = device.create_shader_module(wgpu::ShaderModuleDescriptor {
                label: Some(label),
                source: wgpu::ShaderSource::Wgsl(source.into()),
            });
            device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
                label: Some(label),
                layout: Some(
                    &device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
                        label: Some(label),
                        bind_group_layouts: &[Some(layout)],
                        immediate_size: 0,
                    }),
                ),
                module: &shader,
                entry_point: Some(entry),
                compilation_options: Default::default(),
                cache: None,
            })
        };
        Some(Self {
            resolution,
            displacement,
            dynamics,
            auxiliary,
            cascade_config_ubo,
            cascade_configs,
            h0_init_bind,
            spectrum_bind,
            stage_binds,
            compose_bind,
            h0_init_pipeline: pipeline(
                "water/fft_spectrum_init.wgsl",
                include_str!("fft_spectrum_init.wgsl"),
                "fft_spectrum_init",
                &h0_init_layout,
            ),
            spectrum_pipeline: pipeline(
                "water/fft_spectrum.wgsl",
                include_str!("fft_spectrum.wgsl"),
                "fft_spectrum_evolve",
                &spectrum_layout,
            ),
            stage_pipeline: pipeline(
                "water/fft_row.wgsl",
                include_str!("fft_row.wgsl"),
                "fft_row",
                &stage_layout,
            ),
            compose_pipeline: pipeline(
                "water/fft_compose.wgsl",
                include_str!("fft_compose.wgsl"),
                "fft_compose",
                &compose_layout,
            ),
            stage_alignment: aligned as u32,
            spectrum_inputs: None,
            spectrum_dirty: true,
            active_layers: FFT_LAYERS,
        })
    }
    pub fn displacement_view(&self) -> &wgpu::TextureView {
        &self.displacement
    }
    pub fn dynamics_view(&self) -> &wgpu::TextureView {
        &self.dynamics
    }
    pub fn auxiliary_view(&self) -> &wgpu::TextureView {
        &self.auxiliary
    }
    pub fn active_layers(&self) -> u32 {
        self.active_layers
    }
    pub fn resolution(&self) -> u32 {
        self.resolution
    }
    pub fn cascade_config_buffer(&self) -> &wgpu::Buffer {
        &self.cascade_config_ubo
    }
    pub fn set_params(&mut self, params: &crate::ffi::WgrWaterParams) {
        let inputs = SpectrumInputs::from_params(params);
        if self.spectrum_inputs != Some(inputs) {
            self.spectrum_inputs = Some(inputs);
            self.spectrum_dirty = true;
        }
    }
    /// `WGR_WATER_FFT_CASCADES=<1..4>` caps how many spectrum cascades are live. Unset means
    /// the preset decides, which is the shipped behaviour. Every FFT phase dispatches over the
    /// live layers, so the cap scales the whole block (spectrum evolve, butterfly h+v, compose)
    /// nearly linearly -- 1.36 ms of a 21.8 ms Everon `perf_combat` frame at four cascades,
    /// for a sea strip on the horizon.
    ///
    /// It disables the cascade rather than skipping its dispatch, so the picture is honest:
    /// a skipped dispatch would leave stale displacement in the array layer and the shader
    /// would still read it.
    fn cascade_cap() -> u32 {
        static CAP: std::sync::OnceLock<u32> = std::sync::OnceLock::new();
        *CAP.get_or_init(|| {
            std::env::var("WGR_WATER_FFT_CASCADES")
                .ok()
                .and_then(|v| v.parse::<u32>().ok())
                .map(|v| v.clamp(1, FFT_LAYERS))
                .unwrap_or(FFT_LAYERS)
        })
    }
    pub fn set_cascade_config(
        &mut self,
        _device: &wgpu::Device,
        queue: &wgpu::Queue,
        index: u32,
        config: crate::ffi::WgrWaterCascadeConfig,
    ) {
        let mut config = config;
        if index >= Self::cascade_cap() {
            config.enabled = 0;
        }
        if let Some(slot) = self.cascade_configs.get_mut(index as usize) {
            if bytemuck::bytes_of(&*slot) == bytemuck::bytes_of(&config) {
                return;
            }
            *slot = config;
            queue.write_buffer(
                &self.cascade_config_ubo,
                0,
                bytemuck::cast_slice(&self.cascade_configs),
            );
        }
        // Recompute how many array layers the FFT passes actually have to cover. Enabled cascades
        // are authored as a prefix, but take the highest enabled index rather than a count so a
        // gap cannot silently drop a live cascade.
        self.active_layers = self
            .cascade_configs
            .iter()
            .rposition(|c| c.enabled != 0)
            .map(|i| i as u32 + 1)
            .unwrap_or(1);
        self.spectrum_dirty = true;
    }
    pub fn dispatch(
        &mut self,
        encoder: &mut wgpu::CommandEncoder,
        timers: &crate::gpu_timers::GpuTimers,
    ) {
        use crate::gpu_timers::Region;
        let layers = self.active_layers.clamp(1, FFT_LAYERS);
        // WTR-002 — each FFT phase runs in its own compute pass so the encoder-level
        // timestamps bracket exactly one phase (spectrum init / evolve / horizontal /
        // vertical / compose). Pass splitting adds no barriers beyond those wgpu already
        // inserts between the dependent storage writes, so the GPU work is unchanged.
        fn compute<'e>(
            encoder: &'e mut wgpu::CommandEncoder,
            label: &str,
        ) -> wgpu::ComputePass<'e> {
            encoder.begin_compute_pass(&wgpu::ComputePassDescriptor {
                label: Some(label),
                timestamp_writes: None,
            })
        }
        if self.spectrum_dirty {
            timers.begin(encoder, Region::SpectrumInit);
            let mut pass = compute(encoder, "wgr_water_fft_spectrum_init");
            pass.set_pipeline(&self.h0_init_pipeline);
            pass.set_bind_group(0, &self.h0_init_bind, &[]);
            pass.dispatch_workgroups(self.resolution / 8, self.resolution / 8, layers);
            drop(pass);
            timers.end(encoder, Region::SpectrumInit);
            self.spectrum_dirty = false;
        }
        timers.begin(encoder, Region::SpectrumEvolve);
        {
            let mut pass = compute(encoder, "wgr_water_fft_spectrum_evolve");
            pass.set_pipeline(&self.spectrum_pipeline);
            pass.set_bind_group(0, &self.spectrum_bind, &[]);
            pass.dispatch_workgroups(self.resolution / 8, self.resolution / 8, layers);
        }
        timers.end(encoder, Region::SpectrumEvolve);
        for axis in 0..2 {
            let (region, label) = if axis == 0 {
                (Region::FftHorizontal, "wgr_water_fft_horizontal")
            } else {
                (Region::FftVertical, "wgr_water_fft_vertical")
            };
            timers.begin(encoder, region);
            {
                // The whole axis is one dispatch now: fft_row.wgsl keeps all FFT_STAGES
                // butterflies in workgroup storage behind barriers, so the cross-stage
                // visibility that previously forced one compute pass per stage is handled
                // inside the workgroup instead of by the encoder. Only the axis boundary
                // still needs a real barrier, and the two passes below provide it.
                let mut pass = compute(encoder, label);
                pass.set_pipeline(&self.stage_pipeline);
                // Only the packs that actually carry a spectrum. pack2 is written as all zeros by
                // fft_spectrum_evolve and its result is loaded by fft_compose into a variable that
                // is never read — so a third of the most expensive pass in the renderer was
                // transforming zeros through every butterfly stage and throwing the result away.
                // The bind groups are still built for all three (keeping the stride below valid
                // and the layouts untouched); this simply stops dispatching the dead one.
                for pack in 0..FFT_ACTIVE_PACKS {
                    let index = (axis * 3 + pack) as usize;
                    pass.set_bind_group(
                        0,
                        &self.stage_binds[index],
                        &[axis * self.stage_alignment],
                    );
                    // One workgroup per line of the transform, per cascade layer. Each
                    // One 256-thread workgroup covers each complete transform line.
                    pass.dispatch_workgroups(self.resolution, layers, 1);
                }
            }
            timers.end(encoder, region);
        }
        timers.begin(encoder, Region::FftCompose);
        {
            let mut pass = compute(encoder, "wgr_water_fft_compose");
            pass.set_pipeline(&self.compose_pipeline);
            pass.set_bind_group(0, &self.compose_bind, &[]);
            pass.dispatch_workgroups(self.resolution / 8, self.resolution / 8, layers);
        }
        timers.end(encoder, Region::FftCompose);
    }
}

#[cfg(test)]
mod tests {
    use super::SpectrumInputs;
    use bytemuck::Zeroable;

    const TAU: f32 = std::f32::consts::TAU;

    fn hash(value: u32) -> u32 {
        let mut x = value;
        x = (x ^ (x >> 16)).wrapping_mul(0x7feb_352d);
        x = (x ^ (x >> 15)).wrapping_mul(0x846c_a68b);
        x ^ (x >> 16)
    }

    fn cascade_transition(k: f32, split: f32) -> f32 {
        let edge0 = split * 0.72;
        let edge1 = split * 1.38;
        let t = ((k - edge0) / (edge1 - edge0)).clamp(0.0, 1.0);
        t * t * (3.0 - 2.0 * t)
    }

    fn cascade_bands(k: f32, lengths: [f32; 4]) -> [f32; 4] {
        let split =
            |lower: usize, upper: usize| (TAU / lengths[lower] * TAU / lengths[upper]).sqrt();
        let low_to_mid_low = cascade_transition(k, split(2, 3));
        let mid_low_to_mid_high = cascade_transition(k, split(1, 2));
        let mid_high_to_high = cascade_transition(k, split(0, 1));
        [
            mid_high_to_high,
            mid_low_to_mid_high * (1.0 - mid_high_to_high),
            low_to_mid_low * (1.0 - mid_low_to_mid_high),
            1.0 - low_to_mid_low,
        ]
    }

    fn swell_angle(seed: u32) -> f32 {
        let random = (hash(seed ^ 0x68bc_21eb) & 0x00ff_ffff) as f32 / 16_777_216.0;
        let sign = if hash(seed ^ 0x02e5_be93) & 1 == 0 {
            1.0
        } else {
            -1.0
        };
        sign * (0.45 + (0.95 - 0.45) * random)
    }

    fn jonswap_k_shape(k: f32, peak_k: f32, alpha: f32, gamma: f32) -> f32 {
        let ratio = (k / peak_k).max(1e-4);
        let sigma = if ratio <= 1.0 { 0.07 } else { 0.09 };
        let peak = (-0.5 * ((ratio - 1.0) / sigma).powi(2)).exp();
        alpha / 0.0081 * (-1.25 / (ratio * ratio)).exp() * gamma.max(1.0).powf(peak)
            / (k * k * k * k).max(1e-5)
    }

    fn directional_spreading(alignment: f32, omega: f32, peak_omega: f32, swell: f32) -> f32 {
        let ratio = (omega / peak_omega).max(1e-4);
        let base_s = if ratio <= 1.0 {
            6.97 * ratio.powi(5)
        } else {
            9.77 * ratio.powf(-2.5)
        };
        let s = base_s + 16.0 * (ratio.min(20.0)).tanh() * swell * swell;
        let s2 = s * s;
        let normalization = if s < 5.0 {
            -0.000564 * s2 * s2 + 0.00776 * s2 * s - 0.044 * s2 + 0.192 * s + 0.163
        } else {
            -4.80e-8 * s2 * s2 + 1.07e-5 * s2 * s - 9.53e-4 * s2 + 0.059 * s + 0.393
        };
        normalization * (0.5 * (1.0 + alignment)).max(0.0).powf(s)
    }

    fn opposite_index(index: u32, size: u32) -> u32 {
        (size - index) % size
    }

    fn conjugate(value: [f32; 2]) -> [f32; 2] {
        [value[0], -value[1]]
    }

    fn complex_mul(a: [f32; 2], b: [f32; 2]) -> [f32; 2] {
        [a[0] * b[0] - a[1] * b[1], a[0] * b[1] + a[1] * b[0]]
    }

    // Mirrors the shader's Tessendorf pair.
    fn evolve_pair(h0: [f32; 2], h0_opposite: [f32; 2], phase: [f32; 2]) -> [f32; 2] {
        let inverse_phase = conjugate(phase);
        let first = complex_mul(h0, phase);
        let second = complex_mul(conjugate(h0_opposite), inverse_phase);
        [first[0] + second[0], first[1] + second[1]]
    }

    fn evolve_time_derivative(
        h0: [f32; 2],
        h0_opposite: [f32; 2],
        phase: [f32; 2],
        omega: f32,
    ) -> [f32; 2] {
        let first = complex_mul(h0, phase);
        let second = complex_mul(conjugate(h0_opposite), conjugate(phase));
        [
            -(first[1] - second[1]) * omega,
            (first[0] - second[0]) * omega,
        ]
    }

    fn horizontal_jacobian_derivatives(
        height: f32,
        kx: f32,
        kz: f32,
        k_length: f32,
        chop: f32,
    ) -> [f32; 3] {
        let scale = chop / k_length;
        [
            height * kx * kx * scale,
            height * kx * kz * scale,
            height * kz * kz * scale,
        ]
    }

    #[test]
    fn opposite_index_is_an_involution_including_dc_and_nyquist() {
        let size = 256;
        for index in 0..size {
            assert_eq!(opposite_index(opposite_index(index, size), size), index);
        }
        assert_eq!(opposite_index(0, size), 0);
        assert_eq!(opposite_index(size / 2, size), size / 2);
    }

    #[test]
    fn tessendorf_pair_is_hermitian_for_regular_and_self_paired_bins() {
        let phase = [0.6, 0.8];
        let h0 = [1.25, -0.75];
        let h0_opposite = [-0.4, 0.9];
        let h = evolve_pair(h0, h0_opposite, phase);
        let opposite = evolve_pair(h0_opposite, h0, phase);
        assert!((opposite[0] - h[0]).abs() < 1e-6);
        assert!((opposite[1] + h[1]).abs() < 1e-6);

        let self_paired = evolve_pair(h0, h0, phase);
        assert!(self_paired[1].abs() < 1e-6);

        let dhdt = evolve_time_derivative(h0, h0_opposite, phase, 2.0);
        let dhdt_opposite = evolve_time_derivative(h0_opposite, h0, phase, 2.0);
        assert!((dhdt_opposite[0] - dhdt[0]).abs() < 1e-6);
        assert!((dhdt_opposite[1] + dhdt[1]).abs() < 1e-6);

        let self_paired_dhdt = evolve_time_derivative(h0, h0, phase, 2.0);
        assert!(self_paired_dhdt[1].abs() < 1e-6);
    }

    #[test]
    fn only_spectrum_inputs_invalidate_h0() {
        let mut params = crate::ffi::WgrWaterParams::zeroed();
        let initial = SpectrumInputs::from_params(&params);
        params.time = 42.0;
        params.wave_speed = 3.0;
        assert_eq!(SpectrumInputs::from_params(&params), initial);
        params.fft_control[1] = 1337.0;
        assert_ne!(SpectrumInputs::from_params(&params), initial);
    }

    #[test]
    fn cascade_bands_are_a_smooth_conservative_partition() {
        let lengths = [48.0, 144.0, 432.0, 1296.0];
        for step in 1..=400 {
            let k = 0.002 + step as f32 * 0.05;
            let bands = cascade_bands(k, lengths);
            assert!(bands.iter().all(|weight| (0.0..=1.0).contains(weight)));
            assert!((bands.iter().sum::<f32>() - 1.0).abs() < 1e-5);
        }
        assert!(cascade_bands(0.005, lengths)[3] > 0.9);
        assert!(cascade_bands(1.0, lengths)[0] > 0.9);
    }

    #[test]
    fn swell_direction_is_seeded_and_cross_wind() {
        let angle = swell_angle(1337);
        assert_eq!(angle, swell_angle(1337));
        assert_ne!(angle, swell_angle(1338));
        assert!((0.45..=0.95).contains(&angle.abs()));
    }

    #[test]
    fn jonswap_shape_is_nonnegative_and_peak_enhancement_is_localized() {
        let peak_k = 9.81 / (11.0 * 11.0);
        let gamma_one = jonswap_k_shape(peak_k, peak_k, 0.0081, 1.0);
        let gamma_three = jonswap_k_shape(peak_k, peak_k, 0.0081, 3.0);
        assert!(gamma_one.is_finite() && gamma_one > 0.0);
        assert!(gamma_three > gamma_one * 2.9);
        assert!(jonswap_k_shape(peak_k * 0.1, peak_k, 0.0081, 3.0) < gamma_one);
        assert!(jonswap_k_shape(peak_k * 8.0, peak_k, 0.0081, 3.0) < gamma_one);
    }

    #[test]
    fn directional_spreading_prefers_the_wind_and_remains_nonnegative() {
        let peak_omega = (9.81_f32 * (9.81_f32 / (11.0_f32 * 11.0_f32))).sqrt();
        let forward = directional_spreading(1.0, peak_omega, peak_omega, 0.45);
        let crosswind = directional_spreading(0.0, peak_omega, peak_omega, 0.45);
        let backward = directional_spreading(-1.0, peak_omega, peak_omega, 0.45);
        assert!(forward.is_finite() && forward > crosswind && crosswind > backward);
        assert_eq!(backward, 0.0);
    }

    #[test]
    fn horizontal_displacement_derivatives_have_the_spectral_sign_and_jacobian() {
        // D = -i*h*k/|k|*chop, so dD/dx = i*k*D has h's positive sign.
        let derivatives = horizontal_jacobian_derivatives(2.0, 3.0, -4.0, 5.0, 0.25);
        assert!((derivatives[0] - 0.9).abs() < 1e-6);
        assert!((derivatives[1] + 1.2).abs() < 1e-6);
        assert!((derivatives[2] - 1.6).abs() < 1e-6);

        let jacobian =
            (1.0 + derivatives[0]) * (1.0 + derivatives[2]) - derivatives[1] * derivatives[1];
        assert!((jacobian - 3.5).abs() < 1e-6);
    }

    #[test]
    fn spectrum_shaders_validate() {
        for source in [
            include_str!("fft_spectrum_init.wgsl"),
            include_str!("fft_spectrum.wgsl"),
            include_str!("fft_row.wgsl"),
            include_str!("fft_compose.wgsl"),
        ] {
            let module = naga::front::wgsl::parse_str(source).expect("FFT spectrum WGSL parse");
            naga::valid::Validator::new(
                naga::valid::ValidationFlags::all(),
                naga::valid::Capabilities::all(),
            )
            .validate(&module)
            .expect("FFT spectrum WGSL validate");
        }
    }

    #[test]
    fn fft_row_supports_every_live_resolution_tier() {
        let row = include_str!("fft_row.wgsl");
        // The workgroup array is sized for the largest live tier; N/log2(N) come
        // from the uniform so the same validated pipeline handles all three.
        assert!(row.contains("let fft_n = stage_params.data.x"));
        assert!(row.contains("let fft_bits = stage_params.data.z"));
        assert!(row.contains("FFT_THREADS: u32 = 256u"));
        assert!(row.contains("workgroup_size(256, 1, 1)"));
        assert!(row.contains(&format!("array<vec4<f32>, {}>", super::FFT_MAX_RESOLUTION)));
        for n in [
            super::FFT_MIN_RESOLUTION,
            super::FFT_RESOLUTION,
            super::FFT_MAX_RESOLUTION,
        ] {
            assert!(n.is_power_of_two());
            assert!(n <= 1024);
        }
        // Spectrum coefficients already include dkx*dky and are summed as a
        // physical Fourier series, matching GodotOceanWaves. A conventional
        // DFT 1/N^2 normalisation here makes every visible wave disappear.
        assert!(!row.contains("/ f32(n * n)"));
    }

    // The butterfly schedule must cover every element exactly once per stage — that
    // disjointness is what lets the kernel read and write in place with a single barrier
    // per stage. An overlap would be a data race that only shows up as intermittent
    // corruption on some drivers, so pin it here rather than trusting the reasoning.
    #[test]
    fn in_place_butterfly_pairs_partition_every_stage() {
        for n in [256usize, 512, 1024] {
            for stage in 0..n.ilog2() {
                let span = 1usize << stage;
                let width = span << 1;
                let mut touched = vec![0u8; n];
                for butterfly in 0..n / 2 {
                    let group = butterfly / span;
                    let j = butterfly % span;
                    let i0 = group * width + j;
                    let i1 = i0 + span;
                    assert!(i1 < n, "N={n} stage {stage} index out of range");
                    touched[i0] += 1;
                    touched[i1] += 1;
                }
                assert!(
                    touched.iter().all(|&c| c == 1),
                    "N={n} stage {stage} does not partition the line"
                );
            }
        }
    }

    // Decimation-in-time reads its input bit-reversed; the permutation must be a bijection
    // or the load would drop and duplicate spectrum bins.
    #[test]
    fn bit_reversal_is_a_permutation_at_the_transform_width() {
        let bit_reverse = |v: u32, bits: u32| {
            let mut x = v;
            let mut out = 0u32;
            for _ in 0..bits {
                out = (out << 1) | (x & 1);
                x >>= 1;
            }
            out
        };
        for n in [256u32, 512, 1024] {
            let bits = n.ilog2();
            let mut seen = vec![false; n as usize];
            for i in 0..n {
                let r = bit_reverse(i, bits);
                assert!(r < n);
                assert!(!seen[r as usize], "N={n} bit reversal collided at {i}");
                seen[r as usize] = true;
                assert_eq!(bit_reverse(r, bits), i);
            }
        }
    }
}
