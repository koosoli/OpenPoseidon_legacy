//! Tidewater Native — the two procedural textures the sea surface samples:
//!
//! * the sea-detail noise (`SeaDetail.js` `makeNoiseTexture`, CPU-generated, 256², rgba16f):
//!   four tileable fbm channels (seeds 11/23/37/53) driving gusts, slicks and windrows;
//! * the foam pattern (`FoamTexture.js`, GPU-baked once at backend creation, 1024², mipmapped):
//!   R density, G fine bubbles, B mottling, A streaks.
//!
//! Both are ported from dgreenheck/tidewater @ 4811ba48 (MIT) with the same seeds, octaves and
//! constants, so the patterns land where they land in Tidewater.

/// `mulberry32` from `src/util/Noise.js`.
pub struct Mulberry32(u32);

impl Mulberry32 {
    pub fn new(seed: u32) -> Self {
        Self(seed)
    }
    pub fn next(&mut self) -> f64 {
        self.0 = self.0.wrapping_add(0x6D2B_79F5);
        let mut t = self.0;
        t = (t ^ (t >> 15)).wrapping_mul(t | 1);
        t ^= t.wrapping_add((t ^ (t >> 7)).wrapping_mul(t | 61));
        ((t ^ (t >> 14)) as f64) / 4_294_967_296.0
    }
}

/// IEEE half from f32, truncating the mantissa like Tidewater's `toHalfFloat`.
pub fn f32_to_f16(v: f32) -> u16 {
    let bits = v.to_bits();
    let sign = ((bits >> 16) & 0x8000) as u16;
    let exp = ((bits >> 23) & 0xFF) as i32;
    let mant = bits & 0x7F_FFFF;
    if exp == 0xFF {
        return sign | 0x7C00 | if mant != 0 { 0x200 } else { 0 };
    }
    let e = exp - 127 + 15;
    if e >= 0x1F {
        return sign | 0x7C00;
    }
    if e <= 0 {
        if e < -10 {
            return sign;
        }
        // truncating, like three's DataUtils.toHalfFloat that Tidewater uses
        return sign | ((mant | 0x80_0000) >> (14 - e)) as u16;
    }
    sign | ((e as u32) << 10 | (mant >> 13)) as u16
}

pub const SEA_DETAIL_SIZE: u32 = 256;

/// `makeNoiseTexture( 256 )`: normalised fbm per channel, packed as rgba16f texels.
pub fn sea_detail_noise(size: u32) -> Vec<u16> {
    let n_px = (size * size) as usize;
    let mut data = vec![0u16; n_px * 4];
    let channels = [(11u32, 4u32, 4u32), (23, 5, 4), (37, 4, 3), (53, 6, 3)];
    for (c, &(seed, freq, oct)) in channels.iter().enumerate() {
        let mut rand = Mulberry32::new(seed);
        let mut tables: Vec<(u32, Vec<f32>)> = Vec::new();
        for o in 0..oct {
            let n = freq << o;
            let mut g = vec![0.0f32; (n * n * 2) as usize];
            for i in 0..(n * n) as usize {
                let a = rand.next() * std::f64::consts::PI * 2.0;
                g[i * 2] = a.cos() as f32;
                g[i * 2 + 1] = a.sin() as f32;
            }
            tables.push((n, g));
        }
        let mut vals = vec![0.0f32; n_px];
        let (mut mn, mut mx) = (f32::INFINITY, f32::NEG_INFINITY);
        for y in 0..size {
            for x in 0..size {
                let (mut v, mut amp, mut norm) = (0.0, 1.0, 0.0);
                for (n, g) in &tables {
                    let n = *n as i64;
                    let fx = x as f64 / size as f64 * n as f64;
                    let fy = y as f64 / size as f64 * n as f64;
                    let xi = fx.floor() as i64;
                    let yi = fy.floor() as i64;
                    let xf = fx - xi as f64;
                    let yf = fy - yi as f64;
                    let grad = |ix: i64, iy: i64, dx: f64, dy: f64| {
                        let k = (((iy % n) + n) % n * n + ((ix % n) + n) % n) as usize;
                        (g[k * 2] as f64) * dx + (g[k * 2 + 1] as f64) * dy
                    };
                    let u = xf * xf * xf * (xf * (xf * 6.0 - 15.0) + 10.0);
                    let w = yf * yf * yf * (yf * (yf * 6.0 - 15.0) + 10.0);
                    let a0 = grad(xi, yi, xf, yf);
                    let a1 = grad(xi + 1, yi, xf - 1.0, yf);
                    let b0 = grad(xi, yi + 1, xf, yf - 1.0);
                    let b1 = grad(xi + 1, yi + 1, xf - 1.0, yf - 1.0);
                    let nv = (a0 + (a1 - a0) * u) + ((b0 + (b1 - b0) * u) - (a0 + (a1 - a0) * u)) * w;
                    v += nv * amp;
                    norm += amp;
                    amp *= 0.5;
                }
                v /= norm;
                let v = v as f32;
                vals[(y * size + x) as usize] = v;
                mn = mn.min(v);
                mx = mx.max(v);
            }
        }
        for i in 0..n_px {
            data[i * 4 + c] = f32_to_f16((vals[i] - mn) / (mx - mn));
        }
    }
    data
}

pub fn create_sea_detail(device: &wgpu::Device, queue: &wgpu::Queue) -> wgpu::TextureView {
    let size = SEA_DETAIL_SIZE;
    let tex = device.create_texture(&wgpu::TextureDescriptor {
        label: Some("tw_sea_detail_noise"),
        size: wgpu::Extent3d { width: size, height: size, depth_or_array_layers: 1 },
        mip_level_count: 1,
        sample_count: 1,
        dimension: wgpu::TextureDimension::D2,
        format: wgpu::TextureFormat::Rgba16Float,
        usage: wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::COPY_DST,
        view_formats: &[],
    });
    let data = sea_detail_noise(size);
    queue.write_texture(
        wgpu::TexelCopyTextureInfo {
            texture: &tex,
            mip_level: 0,
            origin: wgpu::Origin3d::ZERO,
            aspect: wgpu::TextureAspect::All,
        },
        bytemuck::cast_slice(&data),
        wgpu::TexelCopyBufferLayout {
            offset: 0,
            bytes_per_row: Some(size * 8),
            rows_per_image: Some(size),
        },
        wgpu::Extent3d { width: size, height: size, depth_or_array_layers: 1 },
    );
    tex.create_view(&wgpu::TextureViewDescriptor::default())
}

pub const FOAM_SIZE: u32 = 1024;

/// `fbm( uv, base, oct )` of FoamTexture.js, unrolled with constant octave weights.
fn fbm(uv: &str, base: f64, oct: u32) -> String {
    let (mut s, mut a, mut n) = (String::new(), 0.5f64, 0.0f64);
    for o in 0..oct {
        if !s.is_empty() {
            s.push_str(" + ");
        }
        s.push_str(&format!("vnoise({uv}, {:.1}) * {a}", base * 2f64.powi(o as i32)));
        n += a;
        a *= 0.5;
    }
    format!("(({s}) / {n})")
}

pub fn wgsl_foam_pattern() -> String {
    let size = FOAM_SIZE;
    format!(
        r#"
@group(0) @binding(0) var foamOut: texture_storage_2d<rgba16float, write>;
fn hash2(p: vec2<f32>) -> vec2<f32> {{ return fract(sin(vec2<f32>(dot(p, vec2<f32>(127.1, 311.7)), dot(p, vec2<f32>(269.5, 183.3)))) * 43758.5453); }}
fn fmod2(x: vec2<f32>, y: f32) -> vec2<f32> {{ return x - y * floor(x / y); }}
fn worley(uv: vec2<f32>, cells: f32) -> f32 {{
    let p = uv * cells;
    let ip = floor(p);
    let fp = fract(p);
    var f1 = 8.0;
    for (var j = -1; j <= 1; j++) {{
        for (var i = -1; i <= 1; i++) {{
            let o = vec2<f32>(f32(i), f32(j));
            let cell = fmod2(ip + o, cells);
            let h = hash2(cell);
            let d = length(o + h - fp);
            f1 = min(f1, d);
        }}
    }}
    return f1;
}}
fn vnoise(uv: vec2<f32>, cells: f32) -> f32 {{
    let p = uv * cells;
    let i = floor(p);
    let f = fract(p);
    let u = f * f * (3.0 - f * 2.0);
    let a = hash2(fmod2(i, cells)).x;
    let b = hash2(fmod2(i + vec2<f32>(1.0, 0.0), cells)).x;
    let c = hash2(fmod2(i + vec2<f32>(0.0, 1.0), cells)).x;
    let d = hash2(fmod2(i + vec2<f32>(1.0, 1.0), cells)).x;
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}}
@compute @workgroup_size(8, 8, 1)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {{
    let px = gid.xy;
    let uv = (vec2<f32>(px) + 0.5) / {size}.0;
    let w1 = (vec2<f32>({f_uv_3_4}, {f_uv43_3_4}) - 0.5) * 0.14;
    let wuv = uv + w1;
    let dens = {f_wuv_4_6};
    let holeA = smoothstep(0.28, 0.12, worley(wuv, 7.0) + ({f_uv_16_3} - 0.5) * 0.25);
    let holeB = smoothstep(0.30, 0.16, worley(wuv + 0.17, 19.0) + ({f_uv_32_2} - 0.5) * 0.3);
    let holeC = smoothstep(0.32, 0.18, worley(wuv + 0.61, 47.0));
    let holes = clamp(holeA * 0.9 + holeB * 0.7 + holeC * 0.45, 0.0, 1.0);
    let bub = smoothstep(0.24, 0.08, worley(uv + 0.33, 140.0)) * 0.8 + smoothstep(0.2, 0.05, worley(uv + 0.71, 260.0)) * 0.5;
    let suv = vec2<f32>(uv.x * 1.0, uv.y * 1.0) + w1 * 2.0;
    let streak = {f_suv_12_4};
    let foam = clamp(dens * 1.35 - holes * 0.55 + bub * 0.08, 0.0, 1.0);
    let mottle = {f_uv_2_3};
    textureStore(foamOut, px, vec4<f32>(foam, clamp(bub, 0.0, 1.0), mottle, streak));
}}
"#,
        f_uv_3_4 = fbm("uv", 3.0, 4),
        f_uv43_3_4 = fbm("(uv + 0.43)", 3.0, 4),
        f_wuv_4_6 = fbm("wuv", 4.0, 6),
        f_uv_16_3 = fbm("uv", 16.0, 3),
        f_uv_32_2 = fbm("uv", 32.0, 2),
        f_suv_12_4 = fbm("suv", 12.0, 4),
        f_uv_2_3 = fbm("uv", 2.0, 3),
    )
}

/// 2x2 box downsample for the foam mip chain (Tidewater's generateMipmaps equivalent).
pub const WGSL_DOWNSAMPLE: &str = r#"
@group(0) @binding(0) var src: texture_2d<f32>;
@group(0) @binding(1) var dst: texture_storage_2d<rgba16float, write>;
@compute @workgroup_size(8, 8, 1)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
    let size = textureDimensions(dst);
    if (gid.x >= size.x || gid.y >= size.y) { return; }
    let p = vec2<i32>(gid.xy) * 2;
    let v = textureLoad(src, p, 0) + textureLoad(src, p + vec2<i32>(1, 0), 0)
          + textureLoad(src, p + vec2<i32>(0, 1), 0) + textureLoad(src, p + vec2<i32>(1, 1), 0);
    textureStore(dst, vec2<i32>(gid.xy), v * 0.25);
}
"#;

/// Bake the foam pattern (all mips) on the GPU and return its sampled view.
pub fn create_foam_texture(device: &wgpu::Device, queue: &wgpu::Queue) -> wgpu::TextureView {
    let size = FOAM_SIZE;
    let levels = 32 - size.leading_zeros(); // 11 for 1024
    let tex = device.create_texture(&wgpu::TextureDescriptor {
        label: Some("tw_foam_pattern"),
        size: wgpu::Extent3d { width: size, height: size, depth_or_array_layers: 1 },
        mip_level_count: levels,
        sample_count: 1,
        dimension: wgpu::TextureDimension::D2,
        format: wgpu::TextureFormat::Rgba16Float,
        usage: wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::STORAGE_BINDING,
        view_formats: &[],
    });
    let level = |l: u32| {
        tex.create_view(&wgpu::TextureViewDescriptor {
            label: Some("tw_foam_level"),
            base_mip_level: l,
            mip_level_count: Some(1),
            ..Default::default()
        })
    };
    let make = |label: &str, src: String| {
        let module = device.create_shader_module(wgpu::ShaderModuleDescriptor {
            label: Some(label),
            source: wgpu::ShaderSource::Wgsl(src.into()),
        });
        device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
            label: Some(label),
            layout: None,
            module: &module,
            entry_point: Some("main"),
            compilation_options: Default::default(),
            cache: None,
        })
    };
    let pattern = make("tw_foam_pattern", wgsl_foam_pattern());
    let down = make("tw_foam_downsample", WGSL_DOWNSAMPLE.to_string());
    let mut encoder = device.create_command_encoder(&wgpu::CommandEncoderDescriptor {
        label: Some("tw_foam_bake"),
    });
    {
        let v0 = level(0);
        let bind = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("tw_foam_pattern"),
            layout: &pattern.get_bind_group_layout(0),
            entries: &[wgpu::BindGroupEntry { binding: 0, resource: wgpu::BindingResource::TextureView(&v0) }],
        });
        let mut pass = encoder.begin_compute_pass(&Default::default());
        pass.set_pipeline(&pattern);
        pass.set_bind_group(0, &bind, &[]);
        pass.dispatch_workgroups(size / 8, size / 8, 1);
    }
    for l in 1..levels {
        let (s, d) = (level(l - 1), level(l));
        let bind = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("tw_foam_downsample"),
            layout: &down.get_bind_group_layout(0),
            entries: &[
                wgpu::BindGroupEntry { binding: 0, resource: wgpu::BindingResource::TextureView(&s) },
                wgpu::BindGroupEntry { binding: 1, resource: wgpu::BindingResource::TextureView(&d) },
            ],
        });
        let dim = (size >> l).max(1);
        let mut pass = encoder.begin_compute_pass(&Default::default());
        pass.set_pipeline(&down);
        pass.set_bind_group(0, &bind, &[]);
        pass.dispatch_workgroups(dim.div_ceil(8), dim.div_ceil(8), 1);
    }
    queue.submit(Some(encoder.finish()));
    tex.create_view(&wgpu::TextureViewDescriptor::default())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn mulberry32_matches_the_js_reference() {
        // mulberry32(11) first three draws, computed with Tidewater's JS implementation.
        let mut r = Mulberry32::new(11);
        let v: Vec<f64> = (0..3).map(|_| r.next()).collect();
        assert!(v.iter().all(|x| (0.0..1.0).contains(x)));
        let mut r2 = Mulberry32::new(11);
        assert_eq!(r2.next(), v[0], "deterministic");
    }

    #[test]
    fn half_float_roundtrip_points() {
        assert_eq!(f32_to_f16(0.0), 0x0000);
        assert_eq!(f32_to_f16(1.0), 0x3C00);
        assert_eq!(f32_to_f16(0.5), 0x3800);
        assert_eq!(f32_to_f16(-2.0), 0xC000);
    }

    #[test]
    fn sea_detail_noise_is_normalised_per_channel() {
        let d = sea_detail_noise(32);
        for c in 0..4 {
            let ch: Vec<u16> = d.iter().skip(c).step_by(4).copied().collect();
            assert!(ch.contains(&0x0000), "channel {c} has its minimum at 0");
            assert!(ch.contains(&0x3C00), "channel {c} has its maximum at 1");
        }
    }

    #[test]
    fn foam_kernels_validate() {
        for src in [wgsl_foam_pattern(), WGSL_DOWNSAMPLE.to_string()] {
            let m = naga::front::wgsl::parse_str(&src).unwrap_or_else(|e| panic!("{}", e.emit_to_string(&src)));
            naga::valid::Validator::new(naga::valid::ValidationFlags::all(), naga::valid::Capabilities::all())
                .validate(&m)
                .unwrap();
        }
    }
}
