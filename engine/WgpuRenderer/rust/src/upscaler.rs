// REN-TEMP-001L — the vendor-neutral TemporalUpscaler boundary (plan Phase 13).
//
// The renderer owns an ordered chain of backends and asks each in turn to fill the
// output-extent target from the render-extent inputs; the first `Done` wins. Today the
// chain is [DLSS (dlss builds, when the NGX route is up), Bilinear]; FSR/XeSS slot in
// as further implementations of the same trait, and "Native" is the absence of the
// chain entirely (at 100%-scale-without-DLAA no upscale target exists and the post
// chain reads the resolve directly — see ensure_hdr).
//
// Boundary rules (plan §18): no vendor type leaves its backend; backends communicate
// with the renderer ONLY through `UpscaleContext` and `UpscaleOutcome`. A backend that
// records through a raw API may SPLIT the frame (submit the current encoder, submit its
// own, leave a fresh encoder in the context) — queue order preserves the data flow.

pub struct UpscaleSettings {
    pub temporal_enabled: bool,
    // FSR 1 spatial backend (EASU+RCAS): the license-clean upscaler every build
    // ships. sharpness is in RCAS stops (0 = sharpest, 2 = mildest).
    pub fsr_on: bool,
    pub fsr_sharpness: f32,
    pub dlss_runtime_on: bool,
    pub dlss_auto_exposure: bool,
    pub jitter_y_flip: bool,
    pub mv_render_space: bool,
    pub mv_flip: bool,
    pub exposure_scale: f32,
}

// All handles are cheap Arc clones, owned by the context so backends never fight the
// renderer's field borrows.
pub struct UpscaleContext<'a> {
    pub device: &'a wgpu::Device,
    pub queue: &'a wgpu::Queue,
    pub encoder: &'a mut wgpu::CommandEncoder,
    // None only on an exotic path with no resolved scene texture handle; backends
    // that need the raw image (DLSS) skip then, view-only backends don't care.
    pub color_tex: Option<wgpu::Texture>,
    pub color_view: wgpu::TextureView,
    pub depth: Option<(wgpu::Texture, wgpu::TextureView)>,
    pub velocity: Option<(wgpu::Texture, wgpu::TextureView)>,
    pub exposure: Option<(wgpu::Texture, wgpu::TextureView)>,
    pub history_control: Option<(wgpu::Texture, wgpu::TextureView)>,
    pub output_tex: wgpu::Texture,
    pub output_view: wgpu::TextureView,
    pub render: (u32, u32),
    pub output: (u32, u32),
    pub jitter_px: [f32; 2],
    pub reset: bool,
    pub settings: UpscaleSettings,
    // INFO-level messages a backend wants logged (feature creation, one-time init);
    // the renderer drains these after the chain runs.
    pub notes: Vec<String>,
}

pub enum UpscaleOutcome {
    // Output written. `quality` is the backend's mode id (-1 when meaningless).
    Done { quality: i32 },
    // Not applicable this frame (disabled, missing inputs) — try the next backend.
    Skip,
    // Permanently out of service; the message must be logged ONCE by the caller. The
    // backend latches itself to Skip afterwards.
    Disabled(String),
}

pub trait TemporalUpscaler {
    fn label(&self) -> &'static str;
    fn evaluate(&mut self, ctx: &mut UpscaleContext) -> UpscaleOutcome;
}

// ---------------------------------------------------------------------------------
// Bilinear: the diagnostic-grade spatial fallback (and the native upscale when no
// temporal backend is active). Never fails.

pub struct BilinearUpscaler {
    blitter: Option<wgpu::util::TextureBlitter>,
}

impl BilinearUpscaler {
    pub fn new() -> Self {
        BilinearUpscaler { blitter: None }
    }
}

impl TemporalUpscaler for BilinearUpscaler {
    fn label(&self) -> &'static str {
        "bilinear"
    }

    fn evaluate(&mut self, ctx: &mut UpscaleContext) -> UpscaleOutcome {
        let blitter = self.blitter.get_or_insert_with(|| {
            wgpu::util::TextureBlitterBuilder::new(ctx.device, crate::HDR_FORMAT)
                .sample_type(wgpu::FilterMode::Linear)
                .build()
        });
        ctx.encoder.push_debug_group("wgr_upscale");
        blitter.copy(ctx.device, ctx.encoder, &ctx.color_view, &ctx.output_view);
        ctx.encoder.pop_debug_group();
        UpscaleOutcome::Done { quality: -1 }
    }
}

// ---------------------------------------------------------------------------------
// FSR 1 (EASU + RCAS), ported from AMD's MIT-licensed FidelityFX sources — see
// fsr1.wgsl for provenance and the SRTM wrapper that lets it run on our HDR slot.
// Spatial-only: needs no jitter, no motion vectors, works identically in every build.
// This is the shipped upscaler for non-NVIDIA players (plan follow-up "FSR", staged:
// FSR1 now, the temporal FSR 3.1 port only if this proves insufficient).

pub struct FsrUpscaler {
    kit: Option<FsrKit>,
    intermediate: Option<(wgpu::Texture, wgpu::TextureView)>,
}

struct FsrKit {
    easu: wgpu::ComputePipeline,
    rcas: wgpu::ComputePipeline,
    layout: wgpu::BindGroupLayout,
    params: wgpu::Buffer,
    sampler: wgpu::Sampler,
}

impl FsrUpscaler {
    pub fn new() -> Self {
        FsrUpscaler {
            kit: None,
            intermediate: None,
        }
    }

    fn ensure_kit(&mut self, device: &wgpu::Device) {
        self.kit.get_or_insert_with(|| {
            let module = device.create_shader_module(wgpu::ShaderModuleDescriptor {
                label: Some("wgr_fsr1"),
                source: wgpu::ShaderSource::Wgsl(include_str!("fsr1.wgsl").into()),
            });
            let layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
                label: Some("wgr_fsr1"),
                entries: &[
                    wgpu::BindGroupLayoutEntry {
                        binding: 0,
                        visibility: wgpu::ShaderStages::COMPUTE,
                        ty: wgpu::BindingType::Texture {
                            sample_type: wgpu::TextureSampleType::Float { filterable: true },
                            view_dimension: wgpu::TextureViewDimension::D2,
                            multisampled: false,
                        },
                        count: None,
                    },
                    wgpu::BindGroupLayoutEntry {
                        binding: 1,
                        visibility: wgpu::ShaderStages::COMPUTE,
                        ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Filtering),
                        count: None,
                    },
                    wgpu::BindGroupLayoutEntry {
                        binding: 2,
                        visibility: wgpu::ShaderStages::COMPUTE,
                        ty: wgpu::BindingType::Buffer {
                            ty: wgpu::BufferBindingType::Uniform,
                            has_dynamic_offset: false,
                            min_binding_size: wgpu::BufferSize::new(80),
                        },
                        count: None,
                    },
                    wgpu::BindGroupLayoutEntry {
                        binding: 3,
                        visibility: wgpu::ShaderStages::COMPUTE,
                        ty: wgpu::BindingType::StorageTexture {
                            access: wgpu::StorageTextureAccess::WriteOnly,
                            format: wgpu::TextureFormat::Rgba16Float,
                            view_dimension: wgpu::TextureViewDimension::D2,
                        },
                        count: None,
                    },
                ],
            });
            let pl = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
                label: Some("wgr_fsr1"),
                bind_group_layouts: &[Some(&layout)],
                immediate_size: 0,
            });
            let mk = |entry: &str| {
                device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
                    label: Some(entry),
                    layout: Some(&pl),
                    module: &module,
                    entry_point: Some(entry),
                    compilation_options: Default::default(),
                    cache: None,
                })
            };
            FsrKit {
                easu: mk("easu"),
                rcas: mk("rcas"),
                layout,
                params: device.create_buffer(&wgpu::BufferDescriptor {
                    label: Some("wgr_fsr1_params"),
                    size: 80,
                    usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
                    mapped_at_creation: false,
                }),
                sampler: device.create_sampler(&wgpu::SamplerDescriptor {
                    label: Some("wgr_fsr1"),
                    address_mode_u: wgpu::AddressMode::ClampToEdge,
                    address_mode_v: wgpu::AddressMode::ClampToEdge,
                    mag_filter: wgpu::FilterMode::Linear,
                    min_filter: wgpu::FilterMode::Linear,
                    ..Default::default()
                }),
            }
        });
    }
}

// FsrEasuCon (ffx_fsr1.h), viewport == full input texture in our use.
fn easu_constants(render: (u32, u32), output: (u32, u32)) -> [f32; 16] {
    let (iw, ih) = (render.0 as f32, render.1 as f32);
    let (ow, oh) = (output.0 as f32, output.1 as f32);
    [
        iw / ow,
        ih / oh,
        0.5 * iw / ow - 0.5,
        0.5 * ih / oh - 0.5,
        1.0 / iw,
        1.0 / ih,
        1.0 / iw,
        -1.0 / ih,
        -1.0 / iw,
        2.0 / ih,
        1.0 / iw,
        2.0 / ih,
        0.0,
        4.0 / ih,
        0.0,
        0.0,
    ]
}

impl TemporalUpscaler for FsrUpscaler {
    fn label(&self) -> &'static str {
        "FSR1"
    }

    fn evaluate(&mut self, ctx: &mut UpscaleContext) -> UpscaleOutcome {
        if !ctx.settings.fsr_on {
            return UpscaleOutcome::Skip;
        }
        // EASU only upscales; SSAA (render above output) stays with the bilinear
        // downsample. render == output is allowed: EASU is ~identity and RCAS still
        // buys its sharpening.
        if ctx.render.0 > ctx.output.0 || ctx.render.1 > ctx.output.1 {
            return UpscaleOutcome::Skip;
        }
        let device = ctx.device;
        // Intermediate (EASU output, tonemapped domain) at the output size.
        let need_new = self
            .intermediate
            .as_ref()
            .map(|(t, _)| (t.width(), t.height()) != ctx.output)
            .unwrap_or(true);
        if need_new {
            let t = device.create_texture(&wgpu::TextureDescriptor {
                label: Some("wgr_fsr1_mid"),
                size: wgpu::Extent3d {
                    width: ctx.output.0,
                    height: ctx.output.1,
                    depth_or_array_layers: 1,
                },
                mip_level_count: 1,
                sample_count: 1,
                dimension: wgpu::TextureDimension::D2,
                format: wgpu::TextureFormat::Rgba16Float,
                usage: wgpu::TextureUsages::TEXTURE_BINDING
                    | wgpu::TextureUsages::STORAGE_BINDING,
                view_formats: &[],
            });
            let v = t.create_view(&wgpu::TextureViewDescriptor::default());
            self.intermediate = Some((t, v));
        }
        let mut data = [0f32; 20];
        data[..16].copy_from_slice(&easu_constants(ctx.render, ctx.output));
        data[16] = (-ctx.settings.fsr_sharpness.clamp(0.0, 2.0)).exp2();
        self.ensure_kit(device);
        let kit = self.kit.as_ref().expect("ensured above");
        ctx.queue
            .write_buffer(&kit.params, 0, bytemuck::cast_slice(&data));
        let (_, mid_view) = self.intermediate.as_ref().expect("ensured above");
        let easu_bind = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("wgr_fsr1_easu"),
            layout: &kit.layout,
            entries: &[
                wgpu::BindGroupEntry {
                    binding: 0,
                    resource: wgpu::BindingResource::TextureView(&ctx.color_view),
                },
                wgpu::BindGroupEntry {
                    binding: 1,
                    resource: wgpu::BindingResource::Sampler(&kit.sampler),
                },
                wgpu::BindGroupEntry {
                    binding: 2,
                    resource: kit.params.as_entire_binding(),
                },
                wgpu::BindGroupEntry {
                    binding: 3,
                    resource: wgpu::BindingResource::TextureView(mid_view),
                },
            ],
        });
        let rcas_bind = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("wgr_fsr1_rcas"),
            layout: &kit.layout,
            entries: &[
                wgpu::BindGroupEntry {
                    binding: 0,
                    resource: wgpu::BindingResource::TextureView(mid_view),
                },
                wgpu::BindGroupEntry {
                    binding: 1,
                    resource: wgpu::BindingResource::Sampler(&kit.sampler),
                },
                wgpu::BindGroupEntry {
                    binding: 2,
                    resource: kit.params.as_entire_binding(),
                },
                wgpu::BindGroupEntry {
                    binding: 3,
                    resource: wgpu::BindingResource::TextureView(&ctx.output_view),
                },
            ],
        });
        let gx = ctx.output.0.div_ceil(8);
        let gy = ctx.output.1.div_ceil(8);
        ctx.encoder.push_debug_group("wgr_fsr1");
        {
            let mut cp = ctx
                .encoder
                .begin_compute_pass(&wgpu::ComputePassDescriptor {
                    label: Some("wgr_fsr1_easu"),
                    timestamp_writes: None,
                });
            cp.set_pipeline(&kit.easu);
            cp.set_bind_group(0, &easu_bind, &[]);
            cp.dispatch_workgroups(gx, gy, 1);
        }
        {
            let mut cp = ctx
                .encoder
                .begin_compute_pass(&wgpu::ComputePassDescriptor {
                    label: Some("wgr_fsr1_rcas"),
                    timestamp_writes: None,
                });
            cp.set_pipeline(&kit.rcas);
            cp.set_bind_group(0, &rcas_bind, &[]);
            cp.dispatch_workgroups(gx, gy, 1);
        }
        ctx.encoder.pop_debug_group();
        {
            static ONCE: std::sync::Once = std::sync::Once::new();
            let (rw, rh) = ctx.render;
            let (ow, oh) = ctx.output;
            ONCE.call_once(|| {
                eprintln!("[wgr] FSR1 active: {rw}x{rh} -> {ow}x{oh} (EASU+RCAS, SRTM-wrapped)");
            });
        }
        UpscaleOutcome::Done { quality: -1 }
    }
}

// ---------------------------------------------------------------------------------
// DLSS Super Resolution over NGX Vulkan (dlss builds only). Owns every NGX object and
// the raw device handles; nothing NGX-shaped escapes this impl.

#[cfg(feature = "dlss")]
pub struct DlssUpscaler {
    raw: crate::dlss_device::NgxRawHandles,
    ngx: Option<crate::dlss::Ngx>,
    // (render, output, auto_exposure) the current feature was created for.
    feature_key: Option<((u32, u32), (u32, u32), bool)>,
    failed: bool,
}

#[cfg(feature = "dlss")]
impl DlssUpscaler {
    pub fn new(raw: crate::dlss_device::NgxRawHandles) -> Self {
        DlssUpscaler {
            raw,
            ngx: None,
            feature_key: None,
            failed: false,
        }
    }

    fn disable(&mut self, msg: String) -> UpscaleOutcome {
        self.failed = true;
        UpscaleOutcome::Disabled(msg)
    }
}

#[cfg(feature = "dlss")]
impl TemporalUpscaler for DlssUpscaler {
    fn label(&self) -> &'static str {
        "DLSS"
    }

    fn evaluate(&mut self, ctx: &mut UpscaleContext) -> UpscaleOutcome {
        use crate::dlss;
        use wgpu::hal::vulkan as hal_vk;
        if self.failed || !ctx.settings.dlss_runtime_on {
            return UpscaleOutcome::Skip;
        }
        // DLSS consumes the temporal contract; without jitter + motion vectors it would
        // only produce the artefacts the plan spent five phases preventing.
        if !ctx.settings.temporal_enabled {
            return self.disable("WGR_DLSS needs the temporal path (jitter + motion vectors)".into());
        }
        let Some(color_tex) = ctx.color_tex.clone() else {
            return self.disable("no raw source texture handle".into());
        };
        let (Some(depth), Some(velocity), Some(exposure)) =
            (ctx.depth.as_ref(), ctx.velocity.as_ref(), ctx.exposure.as_ref())
        else {
            return self.disable("missing depth/velocity/exposure inputs".into());
        };
        if self.ngx.is_none() {
            let ngx = match unsafe {
                dlss::Ngx::init(self.raw.instance, self.raw.physical_device, self.raw.device)
            } {
                Ok(n) => n,
                Err(r) => return self.disable(format!("NGX init failed: {r:#x}")),
            };
            if !ngx.supersampling_available() {
                let diag = ngx.availability_diagnostics();
                return self.disable(format!("DLSS unavailable ({diag})"));
            }
            ctx.notes
                .push("NGX initialised; DLSS Super Resolution available".into());
            self.ngx = Some(ngx);
        }
        let render = ctx.render;
        let output = ctx.output;
        // SSAA territory: DLSS only upscales; above 100% the bilinear pass downsamples.
        if render.0 > output.0 || render.1 > output.1 {
            return UpscaleOutcome::Skip;
        }
        let scale = render.0 as f32 / output.0.max(1) as f32;
        let quality = if scale >= 0.99 {
            dlss::Quality::Dlaa
        } else if scale >= 0.63 {
            dlss::Quality::Quality
        } else if scale >= 0.54 {
            dlss::Quality::Balanced
        } else {
            dlss::Quality::Performance
        };
        let auto_exposure = ctx.settings.dlss_auto_exposure;
        let need_create = self.feature_key != Some((render, output, auto_exposure));

        // Hand wgpu the layout truth BEFORE going raw: inputs sampled, output storage.
        ctx.encoder.transition_resources(
            std::iter::empty(),
            [
                wgpu::TextureTransition {
                    texture: &color_tex,
                    selector: None,
                    state: wgpu::TextureUses::RESOURCE,
                },
                wgpu::TextureTransition {
                    texture: &depth.0,
                    selector: None,
                    state: wgpu::TextureUses::RESOURCE,
                },
                wgpu::TextureTransition {
                    texture: &velocity.0,
                    selector: None,
                    state: wgpu::TextureUses::RESOURCE,
                },
                wgpu::TextureTransition {
                    texture: &exposure.0,
                    selector: None,
                    state: wgpu::TextureUses::RESOURCE,
                },
                wgpu::TextureTransition {
                    texture: &ctx.output_tex,
                    selector: None,
                    state: wgpu::TextureUses::STORAGE_READ_WRITE,
                },
            ]
            .into_iter(),
        );
        if let Some((hc_tex, _)) = ctx.history_control.as_ref() {
            ctx.encoder.transition_resources(
                std::iter::empty(),
                std::iter::once(wgpu::TextureTransition {
                    texture: hc_tex,
                    selector: None,
                    state: wgpu::TextureUses::RESOURCE,
                }),
            );
        }

        // Raw handles for NGX. VK_FORMAT values: R16G16B16A16_SFLOAT=97,
        // R16G16_SFLOAT=83, R32_SFLOAT=100, D32_SFLOAT=126, R8_UNORM=9.
        // Aspects: COLOR=1, DEPTH=2.
        let vk_res = |tex: &wgpu::Texture,
                      view: &wgpu::TextureView,
                      format: u32,
                      aspect: u32,
                      size: (u32, u32),
                      rw: bool|
         -> Option<dlss::NgxResourceVk> {
            use ash::vk::Handle;
            let image = unsafe { tex.as_hal::<hal_vk::Api>()?.raw_handle() };
            let image_view = unsafe { view.as_hal::<hal_vk::Api>()?.raw_handle() };
            Some(dlss::NgxResourceVk {
                image_view_info: dlss::NgxImageViewInfoVk {
                    image_view: image_view.as_raw(),
                    image: image.as_raw(),
                    subresource_range: dlss::VkImageSubresourceRange {
                        aspect_mask: aspect,
                        base_mip_level: 0,
                        level_count: 1,
                        base_array_layer: 0,
                        layer_count: 1,
                    },
                    format,
                    width: size.0,
                    height: size.1,
                },
                ty: dlss::NGX_RESOURCE_VK_TYPE_IMAGEVIEW,
                read_write: rw,
            })
        };
        let (Some(mut color), Some(mut depth_r), Some(mut motion), Some(mut expo), Some(mut out)) = (
            vk_res(&color_tex, &ctx.color_view, 97, 1, render, false),
            vk_res(&depth.0, &depth.1, 126, 2, render, false),
            vk_res(&velocity.0, &velocity.1, 83, 1, render, false),
            vk_res(&exposure.0, &exposure.1, 100, 1, (1, 1), false),
            vk_res(&ctx.output_tex, &ctx.output_view, 97, 1, output, true),
        ) else {
            return self.disable("as_hal returned no Vulkan handles".into());
        };
        let mut hcm = ctx
            .history_control
            .as_ref()
            .and_then(|(t, v)| vk_res(t, v, 9, 1, render, false));

        let jy = ctx.settings.jitter_y_flip;
        let j = ctx.jitter_px;
        let jitter = [j[0], if jy { -j[1] } else { j[1] }];
        let mv_sign = if ctx.settings.mv_flip { -1.0f32 } else { 1.0 };
        let (mvw, mvh) = if ctx.settings.mv_render_space {
            render
        } else {
            output
        };
        let mv_scale = [mvw as f32 * mv_sign, mvh as f32 * mv_sign];
        let reset = ctx.reset;
        let exposure_scale = ctx.settings.exposure_scale;

        // wgpu forbids mixing raw hal encoding with its own API in one encoder, so the
        // frame splits here: everything recorded so far (scene + transitions) submits,
        // NGX records into a dedicated raw encoder submitted next, and the caller
        // continues on a fresh encoder.
        let scene_encoder = std::mem::replace(
            ctx.encoder,
            ctx.device
                .create_command_encoder(&wgpu::CommandEncoderDescriptor {
                    label: Some("wgr_frame_post_dlss"),
                }),
        );
        ctx.queue.submit([scene_encoder.finish()]);
        let mut dlss_encoder = ctx
            .device
            .create_command_encoder(&wgpu::CommandEncoderDescriptor {
                label: Some("wgr_dlss_raw"),
            });
        let ngx = self.ngx.as_mut().expect("ngx initialised above");
        let result: Result<(), dlss::NgxResult> = unsafe {
            use ash::vk::Handle;
            let mut r = Ok(());
            dlss_encoder.as_hal_mut::<hal_vk::Api, _, _>(|hal_enc| {
                let Some(hal_enc) = hal_enc else {
                    r = Err(0);
                    return;
                };
                let cmd = hal_enc.raw_handle().as_raw() as usize as *mut std::os::raw::c_void;
                if need_create {
                    if let Err(e) = ngx.create_feature(cmd, render, output, quality, auto_exposure)
                    {
                        r = Err(e);
                        return;
                    }
                }
                r = ngx.evaluate(
                    cmd,
                    &mut color,
                    &mut depth_r,
                    &mut motion,
                    Some(&mut expo),
                    hcm.as_mut(),
                    &mut out,
                    jitter,
                    mv_scale,
                    reset,
                    render,
                    exposure_scale,
                );
            });
            r
        };
        ctx.queue.submit([dlss_encoder.finish()]);
        match result {
            Ok(()) => {
                if need_create {
                    self.feature_key = Some((render, output, auto_exposure));
                    ctx.notes.push(format!(
                        "DLSS feature created: {}x{} -> {}x{} ({quality:?})",
                        render.0, render.1, output.0, output.1
                    ));
                }
                UpscaleOutcome::Done {
                    quality: quality as i32,
                }
            }
            Err(r) => self.disable(format!("NGX create/evaluate failed: {r:#x}")),
        }
    }
}
