use slotmap::{Key, KeyData, SlotMap};
use std::sync::{Arc, OnceLock, atomic::{AtomicU64, Ordering}};

use crate::ffi::WgrSampler2D;
use crate::log::{LogSink, log_level};

// Experimental first-consumer work shift; eager group creation remains default
// until matched normal-renderer performance is accepted. Read once per process.
fn lazy_texture_bind_groups_enabled() -> bool {
    static ENABLED: OnceLock<bool> = OnceLock::new();
    *ENABLED.get_or_init(|| std::env::var("WGR_LAZY_TEXTURE_BIND_GROUPS").is_ok_and(|v| v == "1"))
}

slotmap::new_key_type! {
    struct TextureKey;
}

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum TextureFormat {
    Rgba8,
    Bc1,
    Bc2,
    Bc3,
    // RFG-047 -- the DXGI-era block formats Enfusion content is authored in. All three
    // ride the SAME wgpu feature bit as BC1-BC3 (TEXTURE_COMPRESSION_BC), so an adapter
    // that can take a DXT texture can take these; `bc_supported` already gates them.
    Bc4,
    Bc5,
    Bc7,
}

impl TextureFormat {
    pub fn from_i32(v: i32) -> Option<Self> {
        match v {
            0 => Some(TextureFormat::Rgba8),
            1 => Some(TextureFormat::Bc1),
            2 => Some(TextureFormat::Bc2),
            3 => Some(TextureFormat::Bc3),
            4 => Some(TextureFormat::Bc4),
            5 => Some(TextureFormat::Bc5),
            6 => Some(TextureFormat::Bc7),
            _ => None,
        }
    }

    pub(crate) fn wgpu_format(self) -> wgpu::TextureFormat {
        match self {
            TextureFormat::Rgba8 => wgpu::TextureFormat::Rgba8Unorm,
            TextureFormat::Bc1 => wgpu::TextureFormat::Bc1RgbaUnorm,
            TextureFormat::Bc2 => wgpu::TextureFormat::Bc2RgbaUnorm,
            TextureFormat::Bc3 => wgpu::TextureFormat::Bc3RgbaUnorm,
            // Unorm, not UnormSrgb, and not Snorm: the C++ side hands these over for
            // Enfusion albedos (already treated as unorm by every other path here) and
            // for _NMO normal maps, whose x/y are stored 0..1 and unpacked in the
            // shader. Bc5RgUnorm samples as (r, g, 0, 1) -- there is no alpha to read,
            // which is exactly why decode_nohq needs the RG flag for these.
            TextureFormat::Bc4 => wgpu::TextureFormat::Bc4RUnorm,
            TextureFormat::Bc5 => wgpu::TextureFormat::Bc5RgUnorm,
            TextureFormat::Bc7 => wgpu::TextureFormat::Bc7RgbaUnorm,
        }
    }

    pub(crate) fn is_block_compressed(self) -> bool {
        !matches!(self, TextureFormat::Rgba8)
    }

    pub(crate) fn bytes_per_row(self, width: u32) -> u32 {
        match self {
            TextureFormat::Rgba8 => width * 4,
            TextureFormat::Bc1 | TextureFormat::Bc4 => width.div_ceil(4) * 8,
            TextureFormat::Bc2 | TextureFormat::Bc3 | TextureFormat::Bc5 | TextureFormat::Bc7 => {
                width.div_ceil(4) * 16
            }
        }
    }

    pub(crate) fn rows(self, height: u32) -> u32 {
        if self.is_block_compressed() {
            height.div_ceil(4)
        } else {
            height
        }
    }

    pub(crate) fn expected_len(self, width: u32, height: u32) -> u32 {
        self.bytes_per_row(width) * self.rows(height)
    }
}

pub struct TextureData<'a> {
    pub width: u32,
    pub height: u32,
    pub format: TextureFormat,
    /// Mip levels present in `bytes`, tightly packed coarsest-last; level i is
    /// (max(1, width>>i), max(1, height>>i)).
    pub mip_count: u32,
    /// Generate the rest of the chain from level 0 with a box filter (RGBA8,
    /// mip_count 1 only).
    pub gen_mips: bool,
    pub bytes: &'a [u8],
}

pub struct Texture2D {
    texture: wgpu::Texture,
    pub view: wgpu::TextureView,
    // Single-texture group used by the shadow-depth pass (per-caster alpha cutout).
    bind_group: OnceLock<wgpu::BindGroup>,
    // Dense index into the bindless object-texture array (0 = white fallback). Read
    // per-instance by the lit-mesh + prepass fragment shaders. Assigned by
    // SharedTextures on create, freed on destroy.
    pub slot: u32,
    // Exact payload size of every allocated mip level. Driver alignment and
    // metadata are deliberately excluded: WebGPU does not expose them, so this
    // is the portable lower bound used by residency diagnostics.
    resident_bytes: u64,
}

// Handle 0 is reserved for the white fallback; slotmap versions start at 1, so a
// real key never encodes to 0.
struct TextureRegistry {
    map: SlotMap<TextureKey, Texture2D>,
    bc_supported: bool,
}

impl TextureRegistry {
    fn new(bc_supported: bool) -> Self {
        TextureRegistry {
            map: SlotMap::with_key(),
            bc_supported,
        }
    }

    fn get(&self, handle: u64) -> Option<&Texture2D> {
        if handle == 0 {
            return None;
        }
        self.map.get(KeyData::from_ffi(handle).into())
    }

    fn destroy(&mut self, handle: u64) -> bool {
        handle != 0 && self.map.remove(KeyData::from_ffi(handle).into()).is_some()
    }

    fn create(
        &mut self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        layout: &wgpu::BindGroupLayout,
        tex: &TextureData,
        slot: u32,
    ) -> u64 {
        let TextureData {
            width,
            height,
            format,
            mip_count,
            gen_mips,
            bytes,
        } = *tex;
        if width == 0 || height == 0 {
            return 0;
        }
        if format.is_block_compressed() && !self.bc_supported {
            return 0;
        }
        let mip_count = mip_count.clamp(1, mip_chain_len(width, height));
        let total: u32 = (0..mip_count)
            .map(|i| format.expected_len((width >> i).max(1), (height >> i).max(1)))
            .sum();
        if (bytes.len() as u32) < total {
            return 0;
        }
        let gen_mips = gen_mips && format == TextureFormat::Rgba8 && mip_count == 1;
        let mip_level_count = if gen_mips {
            mip_chain_len(width, height)
        } else {
            mip_count
        };

        let texture = device.create_texture(&wgpu::TextureDescriptor {
            label: Some("wgr_texture"),
            size: wgpu::Extent3d {
                width,
                height,
                depth_or_array_layers: 1,
            },
            mip_level_count,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: format.wgpu_format(),
            usage: wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::COPY_DST,
            view_formats: &[],
        });

        if gen_mips {
            write_rgba8_mip_chain(queue, &texture, 0, width, height, mip_level_count, bytes);
        } else {
            let mut off = 0usize;
            for mip in 0..mip_count {
                let (mw, mh) = ((width >> mip).max(1), (height >> mip).max(1));
                let len = format.expected_len(mw, mh) as usize;
                write_mip(queue, &texture, mip, mw, mh, format, &bytes[off..off + len]);
                off += len;
            }
        }

        let view = texture.create_view(&wgpu::TextureViewDescriptor::default());
        let bind_group = OnceLock::new();
        if !lazy_texture_bind_groups_enabled() {
            // Original eager order: texture -> mip writes -> view -> group -> registry.
            let group = device.create_bind_group(&wgpu::BindGroupDescriptor {
                label: Some("wgr_texture_bind"),
                layout,
                entries: &[wgpu::BindGroupEntry {
                    binding: 0,
                    resource: wgpu::BindingResource::TextureView(&view),
                }],
            });
            let _ = bind_group.set(group); // brand-new cell cannot already be initialised
        }
        let key = self.map.insert(Texture2D {
            texture,
            view,
            // Opt-in lazy mode makes the immutable image's group on first use;
            // default mode already filled the cell above, in the original order.
            bind_group,
            slot,
            resident_bytes: (0..mip_level_count)
                .map(|i| format.expected_len((width >> i).max(1), (height >> i).max(1)) as u64)
                .sum(),
        });
        key.data().as_ffi()
    }

    fn update_rgba(&mut self, queue: &wgpu::Queue, handle: u64, data: &[u8]) -> bool {
        if handle == 0 {
            return false;
        }
        if let Some(t) = self.map.get(KeyData::from_ffi(handle).into()) {
            let size = t.texture.size();
            if (data.len() as u32) >= size.width * size.height * 4 {
                write_pixels(
                    queue,
                    &t.texture,
                    size.width,
                    size.height,
                    TextureFormat::Rgba8,
                    data,
                );
                return true;
            }
        }
        false
    }
}

pub struct SharedTextures {
    registry: TextureRegistry,
    // Single-texture layout (binding_array of 1), used by the shadow-depth pass.
    pub texture_layout: wgpu::BindGroupLayout,
    pub sampler_layout: wgpu::BindGroupLayout,
    // Bindless object textures (docs/bindless-textures-plan.md): one
    // binding_array<texture_2d> covering every live object texture, bound ONCE for the
    // whole lit-mesh + prepass, indexed per-instance by a dense slot (see Texture2D::slot).
    // Rebuilt lazily by ensure_bindless when a texture is created/destroyed.
    pub bindless_layout: wgpu::BindGroupLayout,
    bindless_bind: wgpu::BindGroup,
    // slot -> view (slot 0 = white fallback / holes). `bindless_free` recycles slots
    // freed by destroy so the array stays dense. A destroyed texture's slot is NOT
    // recycled within the frame that destroyed it: material buffers and encoded draws
    // may still carry the slot index, and a same-frame create reusing it would make
    // them sample the wrong texture instead of the white fallback. Destroy therefore
    // parks the slot in `bindless_retired`; `frame_submitted()` (the same lifetime
    // boundary GeometryPool uses) moves it to the free list.
    bindless_slots: Vec<Option<wgpu::TextureView>>,
    bindless_free: Vec<u32>,
    bindless_retired: Vec<u32>,
    retired_payload: Arc<AtomicU64>,
    retiring_this_submission: u64,
    bindless_dirty: bool,
    // LEASED slots (REN-RES-001). A lease belongs to a logical texture -- one C++
    // `TextureWgpu`, i.e. one texture NAME -- not to a single GPU upload of it. While a
    // slot is leased it is never recycled: destroying the upload points the slot at the
    // white fallback but leaves the index reserved, so a later re-upload of the SAME
    // texture reclaims the SAME index and every model that baked that integer into its
    // material keeps working, with no re-registration and no shader indirection.
    //
    // This is the whole of the fix: `wgr_model_register` resolves a handle to a slot ONCE
    // and stores the integer permanently (gfx3d/mod.rs -> SectionMaterialGpu.texture_slot),
    // and cull.rs's material table is append-only. Before this, eviction did two silent
    // wrongs -- the model fell to white forever, AND the freed slot went to the next
    // upload, so the model could come back wearing a stranger's texture.
    //
    // Bounded, not leaky: a lease is held for the lifetime of the texture object, so the
    // high-water mark is the number of distinct texture NAMES ever loaded, not the number
    // of evict/re-upload CYCLES. Measured worst case in the corpus is DayZ Chernarus at
    // 3,552 distinct object textures against an `object_cap` of 8,192 (lib.rs).
    bindless_leased: std::collections::HashSet<u32>,
    object_cap: u32,
    partially_bound: bool,
    // Bindless sampler layout + the 8-variant array bind (frame-constant), for the
    // lit-mesh + prepass object path. The single-sampler `sampler_binds` below stay for
    // the shadow-depth pass.
    pub sampler_array_layout: wgpu::BindGroupLayout,
    sampler_array_bind: wgpu::BindGroup,
    #[allow(dead_code)] // kept alive: the sampler bind groups reference these
    samplers: [wgpu::Sampler; 8],
    sampler_binds: [wgpu::BindGroup; 8],
    #[allow(dead_code)] // kept alive: white_bind references its view
    white_tex: wgpu::Texture,
    white_view: wgpu::TextureView,
    white_bind: wgpu::BindGroup,
}

// The 1x1 texel behind bindless slot 0 -- the fallback every unresolved texture samples.
//
// Default is opaque WHITE and nothing about that changes here. The problem with white is
// purely diagnostic: albedo 1.0 under a sunlit frame is brighter than any real surface
// (measured on the Stratis corpus: of 4,273 authored Arma 3 textures only 35 average
// >= 250 on all three channels, and every one of them is a UI/map icon, a `_ca` overlay,
// a `_dt` detail map or the sun sprite -- not one building albedo, and not one of the 298
// `*_mlod_co` placeholders, which average ~100-150). So a pure-white surface in a daylight
// capture cannot be an authored texture; it is this texel. But nothing SAYS so, and a
// blown-out white object reads exactly like an over-bright light, which is how the same
// defect has been reported twice: "bright orbs almost as bright as the sun" and "building
// textures look wrong".
//
// WGR_TEXTURE_FALLBACK_RGBA=R,G,B[,A] (0-255) repaints it, and the shorthand
// WGR_TEXTURE_FALLBACK_DEBUG=1 makes it magenta. One capture with that set separates
// "this surface has no texture" from "this surface is genuinely bright" without a build,
// a shader change or a guess.
//
// Deliberately NOT the default: the white fallback is load-bearing for anything that
// multiplies against it (an untextured section tinted by a material colour still wants
// 1.0), so changing it silently would alter shipped pixels.
// ANNOUNCE IT. A capture with zero magenta pixels has two readings — "nothing sampled slot 0"
// and "the lever never fired" — and without a line in the log there is no way to tell them apart.
// That is the same trap that made `WGR_ALPHA_BLOCK_SCAN=0` a worthless A/B on a game with no BC2
// textures: a null that could not have been anything else, read as exoneration. A diagnostic whose
// inactivity is indistinguishable from a clean result is worse than none, so this says which it is.
//
// AND IT MUST REACH THE LOG, not stderr. `eprintln!` here was the same as saying nothing: the
// capture launcher deliberately never redirects stderr (redirecting kills the renderer with os
// error 232, and capture-arms.ps1 records that as a trap), so a line written there is
// unrecoverable. That is exactly how one magenta A/B came back with zero magenta pixels and no
// way to tell "nothing sampled slot 0" from "the lever never fired". It now goes through
// `LogSink` — the same C++ callback every other Rust-side gate announcement uses, appearing in
// the game log prefixed `wgpu:` — with `eprintln!` kept for a console-attached run.
fn announce_fallback_texel(log: &LogSink, texel: [u8; 4]) {
    let default = texel == [0xFF, 0xFF, 0xFF, 0xFF];
    let line = if default {
        "wgpu texture fallback texel: default opaque white at bindless slot 0 \
         (WGR_TEXTURE_FALLBACK_DEBUG=1 recolours it magenta)"
            .to_string()
    } else {
        format!(
            "wgpu texture fallback texel: DEBUG rgba({},{},{},{}) IS ACTIVE -- every tinted \
             surface in this capture is sampling bindless slot 0, i.e. an UNRESOLVED texture, \
             not shading",
            texel[0], texel[1], texel[2], texel[3]
        )
    };
    eprintln!("[wgr] {line}");
    // WARN when the lever is on: a capture whose pixels are deliberately falsified must be
    // impossible to mistake for a production one when the log is read back months later.
    log.log(
        if default {
            log_level::INFO
        } else {
            log_level::WARN
        },
        &line,
    );
}

fn fallback_texel(log: &LogSink) -> [u8; 4] {
    let texel = fallback_texel_inner();
    announce_fallback_texel(log, texel);
    texel
}

fn fallback_texel_inner() -> [u8; 4] {
    if let Ok(raw) = std::env::var("WGR_TEXTURE_FALLBACK_RGBA") {
        let parts: Vec<u8> = raw
            .split(',')
            .filter_map(|p| p.trim().parse::<u8>().ok())
            .collect();
        if parts.len() >= 3 {
            let alpha = parts.get(3).copied().unwrap_or(0xFF);
            return [parts[0], parts[1], parts[2], alpha];
        }
    }
    if std::env::var("WGR_TEXTURE_FALLBACK_DEBUG")
        .map(|v| v != "0")
        .unwrap_or(false)
    {
        return [0xFF, 0x00, 0xFF, 0xFF]; // magenta: never authored, unmistakable
    }
    [0xFF, 0xFF, 0xFF, 0xFF]
}

// Build the bindless object-texture bind group from the current slot views, padding
// holes / the tail with the white fallback. With PARTIALLY_BOUND we bind only up to the
// high-water slot; otherwise the whole declared array must be bound.
fn build_bindless_bind(
    device: &wgpu::Device,
    layout: &wgpu::BindGroupLayout,
    slots: &[Option<wgpu::TextureView>],
    white_view: &wgpu::TextureView,
    object_cap: u32,
    partially_bound: bool,
) -> wgpu::BindGroup {
    let len = if partially_bound {
        slots.len().max(1)
    } else {
        object_cap as usize
    };
    let refs: Vec<&wgpu::TextureView> = (0..len)
        .map(|i| slots.get(i).and_then(|o| o.as_ref()).unwrap_or(white_view))
        .collect();
    device.create_bind_group(&wgpu::BindGroupDescriptor {
        label: Some("wgr_bindless_texture_bind"),
        layout,
        entries: &[wgpu::BindGroupEntry {
            binding: 0,
            resource: wgpu::BindingResource::TextureViewArray(&refs),
        }],
    })
}

impl SharedTextures {
    pub fn new(
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        bc_supported: bool,
        object_cap: u32,
        partially_bound: bool,
        // Only used to announce the slot-0 texel (see `announce_fallback_texel`). Taken by
        // reference and not stored: `LogSink` holds a raw C++ pointer and is neither Send nor
        // Sync, and this type is built once on the renderer's own thread.
        log: &LogSink,
    ) -> Self {
        let texture_layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("wgr_texture_layout"),
            entries: &[wgpu::BindGroupLayoutEntry {
                binding: 0,
                visibility: wgpu::ShaderStages::FRAGMENT,
                ty: wgpu::BindingType::Texture {
                    sample_type: wgpu::TextureSampleType::Float { filterable: true },
                    view_dimension: wgpu::TextureViewDimension::D2,
                    multisampled: false,
                },
                count: None,
            }],
        });

        // Bindless object-texture layout: a fragment-visible binding_array sized to the
        // object-texture cap. Same element type as texture_layout; only `count` differs.
        let object_cap = object_cap.max(1);
        let bindless_layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("wgr_bindless_texture_layout"),
            entries: &[wgpu::BindGroupLayoutEntry {
                binding: 0,
                visibility: wgpu::ShaderStages::FRAGMENT,
                ty: wgpu::BindingType::Texture {
                    sample_type: wgpu::TextureSampleType::Float { filterable: true },
                    view_dimension: wgpu::TextureViewDimension::D2,
                    multisampled: false,
                },
                count: Some(std::num::NonZeroU32::new(object_cap).unwrap()),
            }],
        });

        let sampler_layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("wgr_sampler_layout"),
            entries: &[wgpu::BindGroupLayoutEntry {
                binding: 0,
                visibility: wgpu::ShaderStages::FRAGMENT,
                ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Filtering),
                count: None,
            }],
        });

        // Bindless sampler layout: the 8 fixed sampler variants as a binding_array,
        // bound ONCE for the lit-mesh + prepass and indexed per-instance (sampler arrays
        // ride the same TEXTURE_BINDING_ARRAY feature; DX12/Vulkan/Metal).
        let sampler_array_layout =
            device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
                label: Some("wgr_sampler_array_layout"),
                entries: &[wgpu::BindGroupLayoutEntry {
                    binding: 0,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Filtering),
                    count: Some(std::num::NonZeroU32::new(8).unwrap()),
                }],
            });

        let samplers: [wgpu::Sampler; 8] = std::array::from_fn(|i| {
            let i = i as u32;
            let point = i & WgrSampler2D::POINT != 0;
            let wrap = |clamp: bool| {
                if clamp {
                    wgpu::AddressMode::ClampToEdge
                } else {
                    wgpu::AddressMode::Repeat
                }
            };
            let filter = if point {
                wgpu::FilterMode::Nearest
            } else {
                wgpu::FilterMode::Linear
            };
            device.create_sampler(&wgpu::SamplerDescriptor {
                label: Some("wgr_sampler"),
                address_mode_u: wrap(i & WgrSampler2D::CLAMP_U != 0),
                address_mode_v: wrap(i & WgrSampler2D::CLAMP_V != 0),
                address_mode_w: wgpu::AddressMode::ClampToEdge,
                mag_filter: filter,
                min_filter: filter,
                // Textures now carry mip chains; trilinear + 16x anisotropy
                // matches GL33's non-point samplers (EngineGL33_State.cpp).
                mipmap_filter: if point {
                    wgpu::MipmapFilterMode::Nearest
                } else {
                    wgpu::MipmapFilterMode::Linear
                },
                anisotropy_clamp: if point { 1 } else { 16 },
                ..Default::default()
            })
        });
        let sampler_binds: [wgpu::BindGroup; 8] = std::array::from_fn(|i| {
            device.create_bind_group(&wgpu::BindGroupDescriptor {
                label: Some("wgr_sampler_bind"),
                layout: &sampler_layout,
                entries: &[wgpu::BindGroupEntry {
                    binding: 0,
                    resource: wgpu::BindingResource::Sampler(&samplers[i]),
                }],
            })
        });
        // All 8 variants in one bind group for the bindless object path.
        let sampler_refs: Vec<&wgpu::Sampler> = samplers.iter().collect();
        let sampler_array_bind = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("wgr_sampler_array_bind"),
            layout: &sampler_array_layout,
            entries: &[wgpu::BindGroupEntry {
                binding: 0,
                resource: wgpu::BindingResource::SamplerArray(&sampler_refs),
            }],
        });

        let white_tex = device.create_texture(&wgpu::TextureDescriptor {
            label: Some("wgr_white"),
            size: wgpu::Extent3d {
                width: 1,
                height: 1,
                depth_or_array_layers: 1,
            },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: wgpu::TextureFormat::Rgba8Unorm,
            usage: wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::COPY_DST,
            view_formats: &[],
        });
        write_pixels(
            queue,
            &white_tex,
            1,
            1,
            TextureFormat::Rgba8,
            &fallback_texel(log),
        );
        let white_view = white_tex.create_view(&wgpu::TextureViewDescriptor::default());
        let white_bind = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("wgr_white_bind"),
            layout: &texture_layout,
            entries: &[wgpu::BindGroupEntry {
                binding: 0,
                resource: wgpu::BindingResource::TextureView(&white_view),
            }],
        });

        // Slot 0 = the white fallback, so a draw with a missing/zero texture samples
        // white (matching texture_bind's fallback). Real textures take slots >= 1.
        let bindless_slots = vec![Some(white_view.clone())];
        let bindless_bind = build_bindless_bind(
            device,
            &bindless_layout,
            &bindless_slots,
            &white_view,
            object_cap,
            partially_bound,
        );

        SharedTextures {
            registry: TextureRegistry::new(bc_supported),
            texture_layout,
            sampler_layout,
            bindless_layout,
            bindless_bind,
            bindless_slots,
            bindless_free: Vec::new(),
            bindless_retired: Vec::new(),
            retired_payload: Arc::new(AtomicU64::new(0)),
            retiring_this_submission: 0,
            bindless_dirty: false,
            bindless_leased: std::collections::HashSet::new(),
            object_cap,
            partially_bound,
            sampler_array_layout,
            sampler_array_bind,
            samplers,
            sampler_binds,
            white_tex,
            white_view,
            white_bind,
        }
    }

    // Texture bind group for `handle`, falling back to the 1x1 white texture.
    pub fn texture_bind(&self, device: &wgpu::Device, handle: u64) -> &wgpu::BindGroup {
        self.registry.get(handle).map_or(&self.white_bind, |t| {
            t.bind_group.get_or_init(|| device.create_bind_group(&wgpu::BindGroupDescriptor {
                label: Some("wgr_texture_bind"),
                layout: &self.texture_layout,
                entries: &[wgpu::BindGroupEntry {
                    binding: 0,
                    resource: wgpu::BindingResource::TextureView(&t.view),
                }],
            }))
        })
    }

    // Texture view for `handle`, falling back to the 1x1 white texture.
    pub fn texture_view(&self, handle: u64) -> &wgpu::TextureView {
        self.registry
            .get(handle)
            .map_or(&self.white_view, |t| &t.view)
    }

    pub fn white_view(&self) -> &wgpu::TextureView {
        &self.white_view
    }

    // Sampler bind group for a `point<<2 | clampV<<1 | clampU` index.
    pub fn sampler_bind(&self, index: usize) -> &wgpu::BindGroup {
        self.sampler_binds
            .get(index)
            .unwrap_or(&self.sampler_binds[0])
    }

    pub fn create(&mut self, device: &wgpu::Device, queue: &wgpu::Queue, tex: &TextureData) -> u64 {
        // Reserve a bindless slot up front (recycled free slot, else grow); None = the
        // array is at cap, so the texture still loads but samples via slot 0 (white).
        let slot = self.alloc_slot();
        let slot_idx = slot.unwrap_or(0);
        let handle = self
            .registry
            .create(device, queue, &self.texture_layout, tex, slot_idx);
        if handle == 0 {
            // Creation failed (bad data / unsupported format): return the slot.
            if let Some(s) = slot {
                self.bindless_free.push(s);
            }
            return 0;
        }
        if slot.is_some() {
            if let Some(t) = self.registry.get(handle) {
                self.bindless_slots[slot_idx as usize] = Some(t.view.clone());
                self.bindless_dirty = true;
            }
        }
        handle
    }

    /// Reserve a bindless slot for one logical texture, held across evict/re-upload
    /// cycles. Returns 0 when the array is at cap, which is not an error: the caller
    /// falls back to `create` and gets today's per-upload slot behaviour.
    ///
    /// The caller owns the lease and MUST return it with `slot_release` when the logical
    /// texture dies -- not when an upload of it is destroyed.
    pub fn slot_acquire(&mut self) -> u32 {
        match self.alloc_slot() {
            Some(slot) => {
                self.bindless_leased.insert(slot);
                slot
            }
            None => 0,
        }
    }

    /// Return a lease. The slot goes back to the free list and may be handed to an
    /// unrelated texture, so this is only safe once no registered model can still be
    /// baked against it -- i.e. at the death of the logical texture, not its eviction.
    pub fn slot_release(&mut self, slot: u32) -> bool {
        if slot == 0 || !self.bindless_leased.remove(&slot) {
            return false;
        }
        if let Some(entry) = self.bindless_slots.get_mut(slot as usize)
            && entry.is_some()
        {
            *entry = None;
            self.bindless_dirty = true;
        }
        self.bindless_free.push(slot);
        true
    }

    /// Upload into an already-leased slot, so this texture takes the index its previous
    /// upload had. A zero or unknown `lease` degrades to `create` rather than failing:
    /// an out-of-slots renderer must still load textures.
    pub fn create_in_slot(
        &mut self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        tex: &TextureData,
        lease: u32,
    ) -> u64 {
        if lease == 0 || !self.bindless_leased.contains(&lease) {
            return self.create(device, queue, tex);
        }
        let handle = self
            .registry
            .create(device, queue, &self.texture_layout, tex, lease);
        // On failure the lease is deliberately NOT released: it still belongs to the
        // caller's texture, which may retry. Any previous valid image stays bound.
        if handle != 0
            && let Some(t) = self.registry.get(handle)
        {
            self.bindless_slots[lease as usize] = Some(t.view.clone());
            self.bindless_dirty = true;
        }
        handle
    }

    pub fn update_rgba(&mut self, queue: &wgpu::Queue, handle: u64, data: &[u8]) -> bool {
        self.registry.update_rgba(queue, handle, data)
    }

    pub fn destroy(&mut self, handle: u64) -> bool {
        let mut existed = false;
        if let Some(t) = self.registry.get(handle) {
            existed = true;
            self.retiring_this_submission += t.resident_bytes;
            self.retired_payload.fetch_add(t.resident_bytes, Ordering::Relaxed);
            let slot = t.slot;
            // A newer image may already occupy the same lease (upload-before-retire).
            // Only the image currently bound to the slot may clear it.
            if slot != 0 && (slot as usize) < self.bindless_slots.len()
                && self.bindless_slots[slot as usize].as_ref() == Some(&t.view) {
                self.bindless_slots[slot as usize] = None;
                self.bindless_dirty = true;
                // TWO rules apply here and they compose (REN-RES-001 + the submit-boundary
                // retire). A LEASED slot survives its upload entirely: it stays reserved,
                // now sampling the white fallback, so the re-upload reclaims this exact
                // index and no model ever learns a new number. An UNLEASED slot may be
                // reused -- but not before the frame's queue submit, because material
                // buffers and already-encoded draws still carry the index this frame.
                if !self.bindless_leased.contains(&slot) {
                    self.bindless_retired.push(slot);
                }
            }
        }
        let removed = self.registry.destroy(handle);
        debug_assert_eq!(removed, existed);
        removed
    }

    // Called immediately after the frame's queue submit — the point at which no
    // encoded work can still reference a slot freed by destroy this frame. Slots
    // parked by destroy become reusable here (mirrors GeometryPool::frame_submitted).
    pub fn frame_submitted(&mut self, queue: &wgpu::Queue) {
        self.bindless_free.append(&mut self.bindless_retired);
        let bytes = std::mem::take(&mut self.retiring_this_submission);
        if bytes != 0 {
            let retired = self.retired_payload.clone();
            queue.on_submitted_work_done(move || {
                retired.fetch_sub(bytes, Ordering::Relaxed);
            });
        }
    }

    pub fn retired_payload_bytes(&self) -> u64 {
        self.retired_payload.load(Ordering::Relaxed)
    }

    // Dense bindless slot for `handle` (0 = white fallback for missing/zero handles).
    // Packed per-instance into the material array so the fragment shader can index the
    // bindless texture array.
    pub fn texture_slot(&self, handle: u64) -> u32 {
        self.registry.get(handle).map_or(0, |t| t.slot)
    }

    // The bindless object-texture bind group (valid after ensure_bindless this frame).
    pub fn bindless_bind(&self) -> &wgpu::BindGroup {
        &self.bindless_bind
    }

    // The 8-variant bindless sampler bind group (frame-constant).
    pub fn sampler_array_bind(&self) -> &wgpu::BindGroup {
        &self.sampler_array_bind
    }

    // Rebuild the bindless bind group if any texture was created/destroyed since the
    // last call. Cheap no-op on frames with no texture churn (the common case).
    pub fn ensure_bindless(&mut self, device: &wgpu::Device) {
        if !self.bindless_dirty {
            return;
        }
        self.bindless_bind = build_bindless_bind(
            device,
            &self.bindless_layout,
            &self.bindless_slots,
            &self.white_view,
            self.object_cap,
            self.partially_bound,
        );
        self.bindless_dirty = false;
    }

    /// Declared size of the bindless object-texture array — the number of distinct slots any
    /// shader can index, and therefore the width the AST-012A mip-feedback buffer needs.
    pub fn object_cap(&self) -> u32 {
        self.object_cap
    }

    /// Portable authored object-texture payload and live texture count.
    /// The renderer's 1x1 fallback is not authored content and is excluded.
    pub fn residency(&self) -> (u64, u32) {
        let bytes = self
            .registry
            .map
            .values()
            .map(|texture| texture.resident_bytes)
            .sum();
        (bytes, self.registry.map.len() as u32)
    }

    fn alloc_slot(&mut self) -> Option<u32> {
        if let Some(s) = self.bindless_free.pop() {
            return Some(s);
        }
        let next = self.bindless_slots.len() as u32;
        if next >= self.object_cap {
            return None;
        }
        self.bindless_slots.push(None);
        Some(next)
    }
}

fn write_pixels(
    queue: &wgpu::Queue,
    texture: &wgpu::Texture,
    width: u32,
    height: u32,
    format: TextureFormat,
    data: &[u8],
) {
    write_mip(queue, texture, 0, width, height, format, data);
}

fn write_mip(
    queue: &wgpu::Queue,
    texture: &wgpu::Texture,
    mip_level: u32,
    width: u32,
    height: u32,
    format: TextureFormat,
    data: &[u8],
) {
    queue.write_texture(
        wgpu::TexelCopyTextureInfo {
            texture,
            mip_level,
            origin: wgpu::Origin3d::ZERO,
            aspect: wgpu::TextureAspect::All,
        },
        data,
        wgpu::TexelCopyBufferLayout {
            offset: 0,
            bytes_per_row: Some(format.bytes_per_row(width)),
            rows_per_image: Some(format.rows(height)),
        },
        wgpu::Extent3d {
            // RFG-049a: a block-compressed copy extent must be a multiple of the block
            // size. The tail of a mip chain is not: a 1024 texture ends 2x2 and 1x1, and
            // wgpu refuses those with "Copy height is not a multiple of block height" --
            // one validation error per mip, after which the device is poisoned and every
            // later frame fails `get_current_texture`. The picture is a black window and
            // the auto-screenshot still reports success, so the mip tail looks like a
            // renderer crash rather than an alignment rule.
            //
            // Rounded UP rather than skipped: the level exists in the file, the physical
            // size of a 2x2 BC7 level is one 4x4 block, and that block is what the file
            // holds. Skipping would leave the texture with fewer mips than it declared.
            width: if format.is_block_compressed() { width.max(4) } else { width },
            height: if format.is_block_compressed() { height.max(4) } else { height },
            depth_or_array_layers: 1,
        },
    );
}

// Number of mip levels for a w x h texture (down to 1x1).
pub(crate) fn mip_chain_len(w: u32, h: u32) -> u32 {
    32 - w.max(h).max(1).leading_zeros()
}

// Upload an RGBA8 image and its box-filtered mip chain into one array layer.
pub(crate) fn write_rgba8_mip_chain(
    queue: &wgpu::Queue,
    texture: &wgpu::Texture,
    layer: u32,
    width: u32,
    height: u32,
    mip_level_count: u32,
    base: &[u8],
) {
    let mut level = base.to_vec();
    let (mut lw, mut lh) = (width, height);
    for mip in 0..mip_level_count {
        queue.write_texture(
            wgpu::TexelCopyTextureInfo {
                texture,
                mip_level: mip,
                origin: wgpu::Origin3d {
                    x: 0,
                    y: 0,
                    z: layer,
                },
                aspect: wgpu::TextureAspect::All,
            },
            &level,
            wgpu::TexelCopyBufferLayout {
                offset: 0,
                bytes_per_row: Some(lw * 4),
                rows_per_image: Some(lh),
            },
            wgpu::Extent3d {
                width: lw,
                height: lh,
                depth_or_array_layers: 1,
            },
        );
        if mip + 1 < mip_level_count {
            // GPU mip extents round down, including odd-sized source images.
            // Rounding up overran the allocated mip (e.g. 1230 -> 615 -> 307).
            let (nw, nh) = ((lw / 2).max(1), (lh / 2).max(1));
            level = downsample_rgba8(&level, lw, lh, nw, nh);
            lw = nw;
            lh = nh;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn odd_sized_rgba_mips_fit_gpu_texture_extents() {
        let Some((device, queue)) = crate::gfx3d::cull::tests::headless() else {
            eprintln!("SKIP odd-sized mip validation: no headless GPU");
            return;
        };
        let scope = device.push_error_scope(wgpu::ErrorFilter::Validation);
        for (width, height) in [(1230, 1278), (5, 7), (1, 7)] {
            let levels = mip_chain_len(width, height);
            let texture = device.create_texture(&wgpu::TextureDescriptor {
                label: Some("odd_sized_menu_picture"),
                size: wgpu::Extent3d { width, height, depth_or_array_layers: 1 },
                mip_level_count: levels,
                sample_count: 1,
                dimension: wgpu::TextureDimension::D2,
                format: wgpu::TextureFormat::Rgba8Unorm,
                usage: wgpu::TextureUsages::COPY_DST | wgpu::TextureUsages::TEXTURE_BINDING,
                view_formats: &[],
            });
            let pixels = vec![255; (width * height * 4) as usize];
            write_rgba8_mip_chain(&queue, &texture, 0, width, height, levels, &pixels);
        }
        assert!(pollster::block_on(scope.pop()).is_none());
    }

    #[test]
    fn mutation_results_distinguish_success_from_refused_texture_operations() {
        let Some((device, queue)) = bindless_headless() else { return; };
        let mut textures = shared(&device, &queue);
        assert!(!textures.update_rgba(&queue, 0, &[1, 2, 3, 4]));
        assert!(!textures.destroy(0));
        assert!(!textures.slot_release(0));
        let lease = textures.slot_acquire();
        assert_ne!(lease, 0);
        let first = textures.create_in_slot(&device, &queue, &data(&[1, 2, 3, 4]), lease);
        assert_ne!(first, 0);
        assert!(!textures.update_rgba(&queue, first, &[1, 2, 3]));
        assert!(textures.update_rgba(&queue, first, &[4, 3, 2, 1]));
        let replacement = textures.create_in_slot(
            &device, &queue, &data(&[5, 6, 7, 8]), lease);
        assert_ne!(replacement, 0);
        assert!(textures.destroy(first), "old handle exists despite replacement");
        assert!(!textures.destroy(first), "stale handle is not a mutation");
        assert!(textures.slot_release(lease));
        assert!(!textures.slot_release(lease));
        assert!(textures.destroy(replacement));
    }

    // A 1x1 RGBA8 upload, so a slot's identity is readable as a colour.
    fn texel(rgba: [u8; 4]) -> Vec<u8> {
        rgba.to_vec()
    }

    fn data(bytes: &[u8]) -> TextureData<'_> {
        TextureData {
            width: 1,
            height: 1,
            format: TextureFormat::Rgba8,
            mip_count: 1,
            gen_mips: false,
            bytes,
        }
    }

    // Headless device WITH the bindless features SharedTextures needs. Returns None (test
    // skips) when there is no adapter or the adapter cannot do binding arrays -- the same
    // convention as gfx3d::cull::tests::headless.
    fn bindless_headless() -> Option<(wgpu::Device, wgpu::Queue)> {
        let instance =
            wgpu::Instance::new(wgpu::InstanceDescriptor::new_without_display_handle_from_env());
        let adapter = pollster::block_on(instance.request_adapter(&wgpu::RequestAdapterOptions {
            power_preference: wgpu::PowerPreference::default(),
            compatible_surface: None,
            force_fallback_adapter: false,
        }))
        .ok()?;
        let needed = wgpu::Features::TEXTURE_BINDING_ARRAY
            | wgpu::Features::SAMPLED_TEXTURE_AND_STORAGE_BUFFER_ARRAY_NON_UNIFORM_INDEXING;
        if !adapter.features().contains(needed) {
            return None;
        }
        // The binding-array limit defaults to 0, so it must be asked for explicitly or the
        // bindless layout fails validation -- the same request lib.rs makes at startup.
        let mut limits = wgpu::Limits::default();
        limits.max_binding_array_elements_per_shader_stage = SLOTS + 8;
        // SharedTextures also builds an 8-sampler binding array.
        limits.max_binding_array_sampler_elements_per_shader_stage = 8;
        let have = adapter.limits();
        if have.max_binding_array_elements_per_shader_stage
            < limits.max_binding_array_elements_per_shader_stage
            || have.max_binding_array_sampler_elements_per_shader_stage
                < limits.max_binding_array_sampler_elements_per_shader_stage
        {
            return None;
        }
        pollster::block_on(adapter.request_device(&wgpu::DeviceDescriptor {
            required_features: needed,
            required_limits: limits,
            ..Default::default()
        }))
        .ok()
    }

    // Small enough to keep the probe cheap, large enough that slot reuse is observable.
    const SLOTS: u32 = 64;

    fn shared(device: &wgpu::Device, queue: &wgpu::Queue) -> SharedTextures {
        SharedTextures::new(device, queue, false, SLOTS, false, &LogSink::none())
    }

    // Render one pixel by sampling the REAL bindless bind group at `slot`, exactly as the
    // lit-mesh fragment shader does. This is the point of the test: asserting on
    // `texture_slot()` alone would only prove this module's own bookkeeping, not that the
    // GPU sees the right texels behind that index.
    fn sample_slot(device: &wgpu::Device, queue: &wgpu::Queue, shared: &SharedTextures, slot: u32) -> [u8; 4] {
        sample_binding(device, queue, shared, slot, None)
    }

    fn sample_binding(
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        shared: &SharedTextures,
        slot: u32,
        single_handle: Option<u64>,
    ) -> [u8; 4] {
        let shader = device.create_shader_module(wgpu::ShaderModuleDescriptor {
            label: Some("bindless_probe"),
            source: wgpu::ShaderSource::Wgsl(
                {
                    let source = r#"
@group(0) @binding(0) var textures: binding_array<texture_2d<f32>>;
// vec4 so the uniform block is exactly 16 bytes; only .x is meaningful.
struct Which { slot: vec4<u32> };
@group(1) @binding(0) var<uniform> which: Which;

@vertex
fn vs(@builtin(vertex_index) i: u32) -> @builtin(position) vec4<f32> {
    // One full-viewport triangle; the target is 1x1 so any covering triangle works.
    var p = array<vec2<f32>, 3>(
        vec2<f32>(-1.0, -1.0), vec2<f32>(3.0, -1.0), vec2<f32>(-1.0, 3.0));
    return vec4<f32>(p[i], 0.0, 1.0);
}

@fragment
fn fs() -> @location(0) vec4<f32> {
    return textureLoad(textures[which.slot.x], vec2<i32>(0, 0), 0);
}
"#;
                    if single_handle.is_some() {
                        source.replace("binding_array<texture_2d<f32>>", "texture_2d<f32>")
                            .replace("textures[which.slot.x]", "textures").into()
                    } else { source.into() }
                },
            ),
        });

        let which_layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: None,
            entries: &[wgpu::BindGroupLayoutEntry {
                binding: 0,
                visibility: wgpu::ShaderStages::FRAGMENT,
                ty: wgpu::BindingType::Buffer {
                    ty: wgpu::BufferBindingType::Uniform,
                    has_dynamic_offset: false,
                    min_binding_size: None,
                },
                count: None,
            }],
        });
        let which_buf = device.create_buffer(&wgpu::BufferDescriptor {
            label: None,
            size: 16,
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        queue.write_buffer(&which_buf, 0, bytemuck::cast_slice(&[slot, 0u32, 0, 0]));
        let which_bind = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: None,
            layout: &which_layout,
            entries: &[wgpu::BindGroupEntry {
                binding: 0,
                resource: which_buf.as_entire_binding(),
            }],
        });

        let pipeline_layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: None,
            bind_group_layouts: &[Some(if single_handle.is_some() { &shared.texture_layout } else { &shared.bindless_layout }), Some(&which_layout)],
            immediate_size: 0,
        });
        let pipeline = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
            label: None,
            layout: Some(&pipeline_layout),
            vertex: wgpu::VertexState {
                module: &shader,
                entry_point: Some("vs"),
                compilation_options: Default::default(),
                buffers: &[],
            },
            fragment: Some(wgpu::FragmentState {
                module: &shader,
                entry_point: Some("fs"),
                compilation_options: Default::default(),
                targets: &[Some(wgpu::TextureFormat::Rgba8Unorm.into())],
            }),
            primitive: Default::default(),
            depth_stencil: None,
            multisample: Default::default(),
            multiview_mask: None,
            cache: None,
        });

        let target = device.create_texture(&wgpu::TextureDescriptor {
            label: None,
            size: wgpu::Extent3d {
                width: 1,
                height: 1,
                depth_or_array_layers: 1,
            },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: wgpu::TextureFormat::Rgba8Unorm,
            usage: wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::COPY_SRC,
            view_formats: &[],
        });
        let target_view = target.create_view(&wgpu::TextureViewDescriptor::default());
        // 256 is the minimum bytes_per_row for a texture->buffer copy.
        let readback = device.create_buffer(&wgpu::BufferDescriptor {
            label: None,
            size: 256,
            usage: wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::MAP_READ,
            mapped_at_creation: false,
        });

        let mut enc =
            device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: None });
        {
            let mut pass = enc.begin_render_pass(&wgpu::RenderPassDescriptor {
                label: None,
                color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                    view: &target_view,
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
            pass.set_pipeline(&pipeline);
            pass.set_bind_group(0, single_handle.map_or_else(|| shared.bindless_bind(), |h| shared.texture_bind(device, h)), &[]);
            pass.set_bind_group(1, &which_bind, &[]);
            pass.draw(0..3, 0..1);
        }
        enc.copy_texture_to_buffer(
            wgpu::TexelCopyTextureInfo {
                texture: &target,
                mip_level: 0,
                origin: wgpu::Origin3d::ZERO,
                aspect: wgpu::TextureAspect::All,
            },
            wgpu::TexelCopyBufferInfo {
                buffer: &readback,
                layout: wgpu::TexelCopyBufferLayout {
                    offset: 0,
                    bytes_per_row: Some(256),
                    rows_per_image: Some(1),
                },
            },
            wgpu::Extent3d {
                width: 1,
                height: 1,
                depth_or_array_layers: 1,
            },
        );
        queue.submit(std::iter::once(enc.finish()));

        let slice = readback.slice(..);
        let (tx, rx) = std::sync::mpsc::channel();
        slice.map_async(wgpu::MapMode::Read, move |r| {
            let _ = tx.send(r);
        });
        device.poll(wgpu::PollType::wait_indefinitely()).unwrap();
        rx.recv().unwrap().unwrap();
        let mapped = slice.get_mapped_range();
        [mapped[0], mapped[1], mapped[2], mapped[3]]
    }

    #[test]
    fn default_eager_single_texture_group_samples_replacement_and_reuses_first_binding() {
        single_texture_group_samples_replacement(false);
    }

    #[test]
    fn lazy_single_texture_group_samples_replacement_and_reuses_first_binding() {
        single_texture_group_samples_replacement(true);
    }

    fn single_texture_group_samples_replacement(lazy: bool) {
        if lazy_texture_bind_groups_enabled() != lazy {
            eprintln!("SKIP single_texture_group: requested lazy={lazy} differs from cached WGR_LAZY_TEXTURE_BIND_GROUPS; use a fresh process");
            return;
        }
        let Some((device, queue)) = bindless_headless() else {
            eprintln!("SKIP single_texture_group lazy={lazy}: no adapter/device with required bindless capabilities");
            return;
        };
        let mut shared = shared(&device, &queue);
        let lease = shared.slot_acquire();
        let red = texel([255, 0, 0, 255]);
        let green = texel([0, 255, 0, 255]);
        let old = shared.create_in_slot(&device, &queue, &data(&red), lease);
        assert_ne!(old, 0);
        assert_eq!(shared.registry.get(old).unwrap().bind_group.get().is_none(), lazy);
        assert_eq!(sample_binding(&device, &queue, &shared, 0, Some(old)), [255, 0, 0, 255]);
        assert!(shared.registry.get(old).unwrap().bind_group.get().is_some());
        assert!(std::ptr::eq(shared.texture_bind(&device, old), shared.texture_bind(&device, old)));
        let replacement = shared.create_in_slot(&device, &queue, &data(&green), lease);
        assert_ne!(replacement, 0);
        assert_eq!(shared.texture_slot(replacement), lease);
        assert_eq!(shared.registry.get(replacement).unwrap().bind_group.get().is_none(), lazy);
        assert_eq!(sample_binding(&device, &queue, &shared, 0, Some(replacement)), [0, 255, 0, 255]);
        assert!(!std::ptr::eq(shared.texture_bind(&device, old), shared.texture_bind(&device, replacement)));
        // Old per-image direct group must still sample its own image; a lease
        // replacement only changes the bindless route, not old handle identity.
        assert_eq!(sample_binding(&device, &queue, &shared, 0, Some(old)), [255, 0, 0, 255]);
        shared.destroy(old);
        assert_eq!(sample_binding(&device, &queue, &shared, 0, Some(old)), [255, 255, 255, 255]);
        assert_eq!(sample_binding(&device, &queue, &shared, 0, Some(replacement)), [0, 255, 0, 255]);
        shared.destroy(replacement);
        assert!(std::ptr::eq(shared.texture_bind(&device, replacement), &shared.white_bind));
        assert_eq!(sample_binding(&device, &queue, &shared, 0, Some(replacement)), [255, 255, 255, 255]);
    }

    // THE invariant REN-RES-001 exists for: a model bakes a slot index once, the texture is
    // evicted and re-uploaded, and the model must still sample ITS OWN texture.
    //
    // The "model" here is `baked` -- the integer wgr_model_register would have stored. It is
    // captured BEFORE the eviction and never refreshed, which is precisely the situation
    // cull.rs's append-only material table leaves a registered model in.
    #[test]
    fn leased_slot_replacement_survives_old_handle_retirement() {
        let Some((device, queue)) = bindless_headless() else { return; };
        let mut shared = shared(&device, &queue);
        let red = texel([255, 0, 0, 255]);
        let green = texel([0, 255, 0, 255]);
        let lease = shared.slot_acquire();
        let old = shared.create_in_slot(&device, &queue, &data(&red), lease);
        let replacement = shared.create_in_slot(&device, &queue, &data(&green), lease);
        assert_ne!(old, 0);
        assert_ne!(replacement, 0);
        shared.ensure_bindless(&device);
        assert_eq!(sample_slot(&device, &queue, &shared, lease), [0, 255, 0, 255]);
        // Upload first, retire second: the old handle must not erase its replacement.
        shared.destroy(old);
        assert!(shared.retired_payload_bytes() > 0);
        shared.ensure_bindless(&device);
        assert_eq!(sample_slot(&device, &queue, &shared, lease), [0, 255, 0, 255]);
        shared.frame_submitted(&queue);
        device.poll(wgpu::PollType::wait_indefinitely()).unwrap();
        assert_eq!(shared.retired_payload_bytes(), 0);
        shared.destroy(old); // a stale duplicate retire must also be harmless
        shared.destroy(replacement);
        shared.ensure_bindless(&device);
        assert_eq!(sample_slot(&device, &queue, &shared, lease), [255, 255, 255, 255]);
    }

    #[test]
    fn leased_slot_survives_evict_and_reupload() {
        let Some((device, queue)) = bindless_headless() else {
            return;
        };
        let mut shared = shared(&device, &queue);

        let red = texel([0xFF, 0x00, 0x00, 0xFF]);
        let lease = shared.slot_acquire();
        assert_ne!(lease, 0, "a fresh 64-slot array must have a lease to give");
        let handle = shared.create_in_slot(&device, &queue, &data(&red), lease);
        assert_ne!(handle, 0);

        // What a model registered NOW would bake, permanently.
        let baked = shared.texture_slot(handle);
        assert_eq!(baked, lease);
        shared.ensure_bindless(&device);
        assert_eq!(
            sample_slot(&device, &queue, &shared, baked),
            [0xFF, 0x00, 0x00, 0xFF],
            "the baked slot must show the texture before eviction (guards a vacuous test)"
        );

        // Evict. The model is untouched and still holds `baked`.
        shared.destroy(handle);
        shared.ensure_bindless(&device);
        assert_eq!(
            sample_slot(&device, &queue, &shared, baked),
            [0xFF, 0xFF, 0xFF, 0xFF],
            "an evicted texture's slot must fall back to white, not to stale texels"
        );

        // Re-upload the same logical texture into its lease.
        let handle2 = shared.create_in_slot(&device, &queue, &data(&red), lease);
        assert_ne!(handle2, 0);
        assert_ne!(handle2, handle, "a re-upload is a NEW handle");
        assert_eq!(
            shared.texture_slot(handle2),
            baked,
            "...but it must reclaim the SAME slot the model baked"
        );
        shared.ensure_bindless(&device);
        assert_eq!(
            sample_slot(&device, &queue, &shared, baked),
            [0xFF, 0x00, 0x00, 0xFF],
            "the model must sample its own texture again after re-upload"
        );
    }

    // The other half, and the one that was silently WRONG rather than merely white: a
    // texture uploaded while another is evicted must never inherit the evicted one's slot.
    // Before REN-RES-001 `destroy` pushed the slot onto a LIFO free list that the next
    // upload popped first, so this was not a rare race -- it was the guaranteed outcome.
    #[test]
    fn evicted_slot_is_never_inherited_by_another_texture() {
        let Some((device, queue)) = bindless_headless() else {
            return;
        };
        let mut shared = shared(&device, &queue);

        let red = texel([0xFF, 0x00, 0x00, 0xFF]);
        let green = texel([0x00, 0xFF, 0x00, 0xFF]);

        let lease = shared.slot_acquire();
        let a = shared.create_in_slot(&device, &queue, &data(&red), lease);
        let baked = shared.texture_slot(a);
        shared.destroy(a); // evicted; the model still holds `baked`

        // An unrelated texture arrives in between -- the next upload, which is exactly the
        // one that used to be handed the recycled slot.
        let b = shared.create(&device, &queue, &data(&green));
        assert_ne!(b, 0);
        assert_ne!(
            shared.texture_slot(b),
            baked,
            "an unrelated upload must not be handed a leased slot"
        );

        shared.ensure_bindless(&device);
        assert_eq!(
            sample_slot(&device, &queue, &shared, baked),
            [0xFF, 0xFF, 0xFF, 0xFF],
            "the model's slot must be white, NOT the stranger's green"
        );
        assert_eq!(
            sample_slot(&device, &queue, &shared, shared.texture_slot(b)),
            [0x00, 0xFF, 0x00, 0xFF],
            "and the stranger must still be sampleable at its own slot"
        );

        // The re-upload still lands correctly with a stranger in the array.
        let a2 = shared.create_in_slot(&device, &queue, &data(&red), lease);
        assert_eq!(shared.texture_slot(a2), baked);
        shared.ensure_bindless(&device);
        assert_eq!(
            sample_slot(&device, &queue, &shared, baked),
            [0xFF, 0x00, 0x00, 0xFF]
        );
    }

    // Nothing changes for a texture that never takes a lease: slots are still recycled, so
    // the array stays as dense as it was. This is the "invisible except under memory
    // pressure" half of the requirement.
    //
    // "Recycled" means AT THE SUBMIT BOUNDARY, not immediately: an unleased slot is
    // parked in bindless_retired until frame_submitted(), because material buffers and
    // already-encoded draws of THIS frame still carry the index. The two rules compose
    // -- lease decides WHETHER a slot ever returns, the retire decides WHEN -- so this
    // test drives the frame boundary the renderer drives after every queue submit.
    #[test]
    fn unleased_slots_are_still_recycled() {
        let Some((device, queue)) = bindless_headless() else {
            return;
        };
        let mut shared = shared(&device, &queue);
        let red = texel([0xFF, 0x00, 0x00, 0xFF]);

        let a = shared.create(&device, &queue, &data(&red));
        let slot_a = shared.texture_slot(a);
        assert_ne!(slot_a, 0);
        shared.destroy(a);
        // The within-frame half of the rule has its own test
        // (destroyed_slot_is_not_reused_until_frame_submitted); here we only drive the
        // boundary and assert the slot does come back afterwards.
        shared.frame_submitted(&queue);
        let b = shared.create(&device, &queue, &data(&red));
        assert_eq!(
            shared.texture_slot(b),
            slot_a,
            "after the submit boundary an unleased slot is recycled, exactly as before"
        );
    }

    // A released lease rejoins the free list, so slot space is bounded by live texture
    // NAMES rather than leaking one slot per evict/re-upload cycle.
    #[test]
    fn released_lease_returns_to_the_pool() {
        let Some((device, queue)) = bindless_headless() else {
            return;
        };
        let mut shared = shared(&device, &queue);
        let red = texel([0xFF, 0x00, 0x00, 0xFF]);

        let lease = shared.slot_acquire();
        let a = shared.create_in_slot(&device, &queue, &data(&red), lease);
        shared.destroy(a);
        shared.slot_release(lease);

        let b = shared.create(&device, &queue, &data(&red));
        assert_eq!(
            shared.texture_slot(b),
            lease,
            "a released lease must be reusable"
        );
        // Double release must not corrupt the free list.
        shared.slot_release(lease);
        shared.slot_release(lease);
        let c = shared.create(&device, &queue, &data(&red));
        assert_ne!(shared.texture_slot(c), shared.texture_slot(b));
    }

    // Cap exhaustion must degrade, not fail: `create_in_slot` with no lease is `create`.
    #[test]
    fn zero_lease_degrades_to_plain_create() {
        let Some((device, queue)) = bindless_headless() else {
            return;
        };
        let mut shared = shared(&device, &queue);
        let red = texel([0xFF, 0x00, 0x00, 0xFF]);

        let handle = shared.create_in_slot(&device, &queue, &data(&red), 0);
        assert_ne!(handle, 0, "a zero lease must still upload the texture");
        let slot = shared.texture_slot(handle);
        assert_ne!(slot, 0);
        shared.ensure_bindless(&device);
        assert_eq!(
            sample_slot(&device, &queue, &shared, slot),
            [0xFF, 0x00, 0x00, 0xFF]
        );
        // And an unknown (never-leased) index behaves the same way rather than writing
        // into someone else's slot.
        let other = shared.create_in_slot(&device, &queue, &data(&red), 61);
        assert_ne!(other, 0);
        assert_ne!(shared.texture_slot(other), 61);
    }
}

// 2x2 box-filter downsample of an RGBA8 image to (nw, nh).
fn downsample_rgba8(src: &[u8], sw: u32, sh: u32, nw: u32, nh: u32) -> Vec<u8> {
    let mut out = vec![0u8; (nw * nh * 4) as usize];
    for y in 0..nh {
        let y0 = (y * 2).min(sh - 1);
        let y1 = (y * 2 + 1).min(sh - 1);
        for x in 0..nw {
            let x0 = (x * 2).min(sw - 1);
            let x1 = (x * 2 + 1).min(sw - 1);
            let p = |px: u32, py: u32, c: usize| src[((py * sw + px) * 4) as usize + c] as u32;
            let o = ((y * nw + x) * 4) as usize;
            for c in 0..4 {
                out[o + c] =
                    ((p(x0, y0, c) + p(x1, y0, c) + p(x0, y1, c) + p(x1, y1, c) + 2) / 4) as u8;
            }
        }
    }
    out
}

#[cfg(test)]
mod slot_lease_tests {
    use super::*;

    fn shared(device: &wgpu::Device, queue: &wgpu::Queue) -> SharedTextures {
        SharedTextures::new(
            device,
            queue,
            false,
            64,
            false,
            &crate::log::LogSink::none(),
        )
    }

    // The bindless layout needs TEXTURE_BINDING_ARRAY, which the default headless
    // device does not enable; request it explicitly (skip when unsupported).
    fn headless_bindless() -> Option<(wgpu::Device, wgpu::Queue)> {
        let instance =
            wgpu::Instance::new(wgpu::InstanceDescriptor::new_without_display_handle_from_env());
        let adapter = pollster::block_on(instance.request_adapter(&wgpu::RequestAdapterOptions {
            power_preference: wgpu::PowerPreference::default(),
            compatible_surface: None,
            force_fallback_adapter: false,
        }))
        .ok()?;
        if !adapter
            .features()
            .contains(wgpu::Features::TEXTURE_BINDING_ARRAY)
        {
            return None;
        }
        let mut limits = wgpu::Limits::default();
        limits.max_binding_array_elements_per_shader_stage = 64;
        limits.max_binding_array_sampler_elements_per_shader_stage = 8;
        let desc = wgpu::DeviceDescriptor {
            required_features: wgpu::Features::TEXTURE_BINDING_ARRAY,
            required_limits: limits,
            ..Default::default()
        };
        pollster::block_on(adapter.request_device(&desc)).ok()
    }

    fn tex_1x1() -> [u8; 4] {
        [255, 0, 0, 255]
    }

    // A destroyed texture's bindless slot must NOT be handed to a create within the
    // same frame: material buffers and already-encoded draws still carry the slot
    // index, and reuse would make them sample the new (wrong) texture instead of the
    // white fallback. Only frame_submitted() makes the slot reusable.
    #[test]
    fn destroyed_slot_is_not_reused_until_frame_submitted() {
        let Some((device, queue)) = headless_bindless() else {
            return;
        };
        let mut st = shared(&device, &queue);
        let bytes = tex_1x1();
        let data = TextureData {
            width: 1,
            height: 1,
            format: TextureFormat::Rgba8,
            mip_count: 1,
            gen_mips: false,
            bytes: &bytes,
        };
        let a = st.create(&device, &queue, &data);
        assert_ne!(a, 0);
        let slot_a = st.texture_slot(a);
        assert_ne!(slot_a, 0, "a real texture gets a non-fallback slot");

        st.destroy(a);
        let b = st.create(&device, &queue, &data);
        assert_ne!(b, 0);
        let slot_b = st.texture_slot(b);
        assert_ne!(
            slot_b, slot_a,
            "same-frame create must not reuse the destroyed slot"
        );

        st.frame_submitted(&queue);
        let c = st.create(&device, &queue, &data);
        assert_ne!(c, 0);
        assert_eq!(
            st.texture_slot(c),
            slot_a,
            "after the submit boundary the retired slot is recycled"
        );
    }

    // The registry handle itself stays generation-safe: a destroyed handle resolves
    // to the fallback slot 0, never to whatever texture reused the storage.
    #[test]
    fn destroyed_handle_resolves_to_fallback() {
        let Some((device, queue)) = headless_bindless() else {
            return;
        };
        let mut st = shared(&device, &queue);
        let bytes = tex_1x1();
        let data = TextureData {
            width: 1,
            height: 1,
            format: TextureFormat::Rgba8,
            mip_count: 1,
            gen_mips: false,
            bytes: &bytes,
        };
        let a = st.create(&device, &queue, &data);
        st.destroy(a);
        st.frame_submitted(&queue);
        let b = st.create(&device, &queue, &data);
        assert_ne!(
            a, b,
            "slotmap keys are versioned; the handle is not reissued"
        );
        assert_eq!(st.texture_slot(a), 0, "stale handle falls back to white");
        assert_ne!(st.texture_slot(b), 0);
    }
}
