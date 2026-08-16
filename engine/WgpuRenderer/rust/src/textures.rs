use slotmap::{Key, KeyData, SlotMap};

use crate::ffi::WgrSampler2D;

slotmap::new_key_type! {
    struct TextureKey;
}

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum TextureFormat {
    Rgba8,
    Bc1,
    Bc2,
    Bc3,
}

impl TextureFormat {
    pub fn from_i32(v: i32) -> Option<Self> {
        match v {
            0 => Some(TextureFormat::Rgba8),
            1 => Some(TextureFormat::Bc1),
            2 => Some(TextureFormat::Bc2),
            3 => Some(TextureFormat::Bc3),
            _ => None,
        }
    }

    pub(crate) fn wgpu_format(self) -> wgpu::TextureFormat {
        match self {
            TextureFormat::Rgba8 => wgpu::TextureFormat::Rgba8Unorm,
            TextureFormat::Bc1 => wgpu::TextureFormat::Bc1RgbaUnorm,
            TextureFormat::Bc2 => wgpu::TextureFormat::Bc2RgbaUnorm,
            TextureFormat::Bc3 => wgpu::TextureFormat::Bc3RgbaUnorm,
        }
    }

    pub(crate) fn is_block_compressed(self) -> bool {
        !matches!(self, TextureFormat::Rgba8)
    }

    pub(crate) fn bytes_per_row(self, width: u32) -> u32 {
        match self {
            TextureFormat::Rgba8 => width * 4,
            TextureFormat::Bc1 => width.div_ceil(4) * 8,
            TextureFormat::Bc2 | TextureFormat::Bc3 => width.div_ceil(4) * 16,
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
    pub bind_group: wgpu::BindGroup,
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

    fn destroy(&mut self, handle: u64) {
        if handle != 0 {
            self.map.remove(KeyData::from_ffi(handle).into());
        }
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
        let bind_group = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("wgr_texture_bind"),
            layout,
            entries: &[wgpu::BindGroupEntry {
                binding: 0,
                resource: wgpu::BindingResource::TextureView(&view),
            }],
        });

        let key = self.map.insert(Texture2D {
            texture,
            view,
            bind_group,
            slot,
            resident_bytes: (0..mip_level_count)
                .map(|i| {
                    format.expected_len((width >> i).max(1), (height >> i).max(1)) as u64
                })
                .sum(),
        });
        key.data().as_ffi()
    }

    fn update_rgba(&mut self, queue: &wgpu::Queue, handle: u64, data: &[u8]) {
        if handle == 0 {
            return;
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
            }
        }
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
    // freed by destroy so the array stays dense.
    bindless_slots: Vec<Option<wgpu::TextureView>>,
    bindless_free: Vec<u32>,
    bindless_dirty: bool,
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
fn fallback_texel() -> [u8; 4] {
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
            &fallback_texel(),
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
            bindless_dirty: false,
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
    pub fn texture_bind(&self, handle: u64) -> &wgpu::BindGroup {
        self.registry
            .get(handle)
            .map_or(&self.white_bind, |t| &t.bind_group)
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

    pub fn update_rgba(&mut self, queue: &wgpu::Queue, handle: u64, data: &[u8]) {
        self.registry.update_rgba(queue, handle, data);
    }

    pub fn destroy(&mut self, handle: u64) {
        if let Some(t) = self.registry.get(handle) {
            let slot = t.slot;
            if slot != 0 && (slot as usize) < self.bindless_slots.len() {
                self.bindless_slots[slot as usize] = None;
                self.bindless_free.push(slot);
                self.bindless_dirty = true;
            }
        }
        self.registry.destroy(handle);
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
            width,
            height,
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
            let (nw, nh) = (lw.div_ceil(2).max(1), lh.div_ceil(2).max(1));
            level = downsample_rgba8(&level, lw, lh, nw, nh);
            lw = nw;
            lh = nh;
        }
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
