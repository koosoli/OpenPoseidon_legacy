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
