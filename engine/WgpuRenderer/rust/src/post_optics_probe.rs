//! Screenshot-only float receipt. No frame-wide reduction/readback or appearance changes.
use crate::ffi::WgrCamera;
use wgpu::util::DeviceExt;
pub const WIDTH: u32 = 25;
pub const HEIGHT: u32 = 28;
pub const STRIDE: u32 = 512;
pub const READBACK_BYTES: u64 = STRIDE as u64 * HEIGHT as u64;
const _: () = assert!(READBACK_BYTES <= 16384);
pub fn enabled() -> bool {
    static ENABLED: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *ENABLED
        .get_or_init(|| std::env::var("WGR_SCREENSHOT_POST_OPTICS").ok().as_deref() == Some("1"))
}
pub fn words(n: u64) -> [u32; 4] {
    std::array::from_fn(|i| ((n >> (i * 16)) & 65535) as u32)
}
fn number(words: [u32; 4]) -> u64 {
    words
        .into_iter()
        .enumerate()
        .fold(0, |n, (i, w)| n | ((w as u64) << (16 * i)))
}
#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
pub struct Input {
    pub dims: [u32; 4],
    pub origins: [[u32; 4]; 4],
    pub request: [u32; 4],
    pub frame: [u32; 4],
    pub source: [u32; 4],
}
pub fn input(
    size: (u32, u32),
    request: u64,
    frame: u64,
    source: u64,
    tiles: Option<&str>,
) -> Result<Input, &'static str> {
    if size.0 < 5
        || size.1 < 5
        || size.0 > 16384
        || size.1 > 16384
        || [request, frame, source].contains(&0)
    {
        return Err("invalid dimensions/identity");
    }
    let centres = if let Some(text) = tiles {
        let parts: Vec<_> = text.split(';').collect();
        if parts.len() != 4 {
            return Err("exactly four tile centres required");
        }
        let mut centres = [[0u32; 2]; 4];
        for (i, p) in parts.iter().enumerate() {
            let xy: Vec<_> = p.split(',').collect();
            if xy.len() != 2 {
                return Err("invalid tile centre");
            }
            centres[i] = [
                xy[0].trim().parse().map_err(|_| "invalid tile x")?,
                xy[1].trim().parse().map_err(|_| "invalid tile y")?,
            ];
        }
        centres
    } else {
        [
            [size.0 / 2, size.1 * 2 / 5],
            [size.0 / 2, size.1 * 13 / 20],
            [size.0 / 4, size.1 / 2],
            [size.0 * 3 / 4, size.1 / 2],
        ]
    };
    let mut origins = [[0; 4]; 4];
    for (i, [x, y]) in centres.into_iter().enumerate() {
        if x < 2 || y < 2 || x > size.0 - 3 || y > size.1 - 3 {
            return Err("tile outside output");
        }
        origins[i] = [x - 2, y - 2, 0, 0];
    }
    Ok(Input {
        dims: [size.0, size.1, 0, 0],
        origins,
        request: words(request),
        frame: words(frame),
        source: words(source),
    })
}
pub struct State {
    pub request: u64,
    pub frame: u64,
    pub valid: bool,
    pub attempted: bool,
    pub cameras: Option<(u64, u64)>,
    pub pending: Option<Pending>,
}
impl State {
    pub fn new() -> Self {
        Self {
            request: 0,
            frame: 0,
            valid: false,
            attempted: false,
            cameras: None,
            pending: None,
        }
    }
    pub fn request(&mut self, overlap: bool) {
        self.request = self.request.checked_add(1).unwrap_or(u64::MAX);
        self.valid = !overlap && self.request != 0 && self.request != u64::MAX;
        self.pending = None;
    }
    pub fn begin(&mut self) {
        self.frame = self.frame.checked_add(1).unwrap_or(u64::MAX);
        self.attempted = false;
        self.cameras = None;
        self.pending = None;
        if self.frame == 0 || self.frame == u64::MAX {
            self.valid = false;
        }
    }
    pub fn capture(
        &mut self,
        original: Option<&WgrCamera>,
        prepared: Option<&WgrCamera>,
        source: u32,
    ) {
        self.cameras = None;
        fn hash(c: &WgrCamera) -> Option<u64> {
            if c.proj
                .iter()
                .chain(c.view.iter())
                .chain(c.cam_pos.iter())
                .any(|x| !x.is_finite())
            {
                return None;
            }
            Some(
                c.proj
                    .iter()
                    .chain(c.view.iter())
                    .chain(c.cam_pos.iter())
                    .fold(14695981039346656037u64, |mut h, x| {
                        for b in x.to_bits().to_le_bytes() {
                            h = (h ^ b as u64).wrapping_mul(1099511628211);
                        }
                        h
                    }),
            )
        }
        if source > 0 && source <= 4 {
            self.cameras = original.and_then(hash).zip(prepared.and_then(hash));
        }
    }
}
pub struct Gpu {
    pipeline: wgpu::RenderPipeline,
    layout: wgpu::BindGroupLayout,
}
impl Gpu {
    pub fn new(
        device: &wgpu::Device,
        shader: &wgpu::ShaderModule,
        tone_layout: &wgpu::BindGroupLayout,
        constants: &[(&str, f64)],
    ) -> Self {
        let layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("post_optics_probe"),
            entries: &[
                wgpu::BindGroupLayoutEntry {
                    binding: 0,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Float { filterable: false },
                        view_dimension: wgpu::TextureViewDimension::D2,
                        multisampled: false,
                    },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 1,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Buffer {
                        ty: wgpu::BufferBindingType::Uniform,
                        has_dynamic_offset: false,
                        min_binding_size: None,
                    },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 2,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Buffer {
                        ty: wgpu::BufferBindingType::Uniform,
                        has_dynamic_offset: false,
                        min_binding_size: None,
                    },
                    count: None,
                },
            ],
        });
        let pl = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("post_optics_probe"),
            bind_group_layouts: &[Some(tone_layout), Some(&layout)],
            immediate_size: 0,
        });
        let pipeline = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
            label: Some("post_optics_probe"),
            layout: Some(&pl),
            vertex: wgpu::VertexState {
                module: shader,
                entry_point: Some("vs_main"),
                buffers: &[],
                compilation_options: Default::default(),
            },
            primitive: Default::default(),
            depth_stencil: None,
            multisample: Default::default(),
            fragment: Some(wgpu::FragmentState {
                module: shader,
                entry_point: Some("fs_post_optics_probe"),
                targets: &[Some(wgpu::ColorTargetState {
                    format: wgpu::TextureFormat::Rgba32Float,
                    blend: None,
                    write_mask: wgpu::ColorWrites::ALL,
                })],
                compilation_options: wgpu::PipelineCompilationOptions {
                    constants,
                    ..Default::default()
                },
            }),
            multiview_mask: None,
            cache: None,
        });
        Self { pipeline, layout }
    }
    pub fn encode(
        &self,
        device: &wgpu::Device,
        encoder: &mut wgpu::CommandEncoder,
        tone: &wgpu::BindGroup,
        meter: &wgpu::TextureView,
        exposure_params: &wgpu::Buffer,
        input: Input,
        cameras: (u64, u64),
    ) -> Pending {
        let uniform = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
            label: Some("post_optics_input"),
            contents: bytemuck::bytes_of(&input),
            usage: wgpu::BufferUsages::UNIFORM,
        });
        let bind = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("post_optics_probe"),
            layout: &self.layout,
            entries: &[
                wgpu::BindGroupEntry {
                    binding: 0,
                    resource: wgpu::BindingResource::TextureView(meter),
                },
                wgpu::BindGroupEntry {
                    binding: 1,
                    resource: uniform.as_entire_binding(),
                },
                wgpu::BindGroupEntry {
                    binding: 2,
                    resource: exposure_params.as_entire_binding(),
                },
            ],
        });
        let target = device.create_texture(&wgpu::TextureDescriptor {
            label: Some("post_optics_probe"),
            size: wgpu::Extent3d {
                width: WIDTH,
                height: HEIGHT,
                depth_or_array_layers: 1,
            },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: wgpu::TextureFormat::Rgba32Float,
            usage: wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::COPY_SRC,
            view_formats: &[],
        });
        let view = target.create_view(&Default::default());
        {
            let mut pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
                label: Some("post_optics_probe"),
                color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                    view: &view,
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
            pass.set_pipeline(&self.pipeline);
            pass.set_bind_group(0, tone, &[]);
            pass.set_bind_group(1, &bind, &[]);
            pass.draw(0..3, 0..1);
        }
        let buffer = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("post_optics_readback"),
            size: READBACK_BYTES,
            usage: wgpu::BufferUsages::MAP_READ | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        encoder.copy_texture_to_buffer(
            target.as_image_copy(),
            wgpu::TexelCopyBufferInfo {
                buffer: &buffer,
                layout: wgpu::TexelCopyBufferLayout {
                    offset: 0,
                    bytes_per_row: Some(STRIDE),
                    rows_per_image: Some(HEIGHT),
                },
            },
            wgpu::Extent3d {
                width: WIDTH,
                height: HEIGHT,
                depth_or_array_layers: 1,
            },
        );
        Pending {
            buffer,
            input,
            cameras,
        }
    }
}
pub struct Pending {
    buffer: wgpu::Buffer,
    input: Input,
    cameras: (u64, u64),
}
pub fn decode(bytes: &[u8], input: &Input) -> Result<Vec<[f32; 4]>, &'static str> {
    if bytes.len() != READBACK_BYTES as usize {
        return Err("wrong byte count");
    }
    let mut out = Vec::with_capacity((WIDTH * HEIGHT) as usize);
    for row in 0..HEIGHT as usize {
        for col in 0..WIDTH as usize {
            let base = row * STRIDE as usize + col * 16;
            let v = std::array::from_fn(|i| {
                f32::from_le_bytes(bytes[base + i * 4..base + i * 4 + 4].try_into().unwrap())
            });
            if v.iter().any(|x| !x.is_finite()) {
                return Err("nonfinite GPU value");
            }
            out.push(v);
        }
    }
    for (row, expected) in [(22, input.request), (23, input.frame), (24, input.source)] {
        if out[row * WIDTH as usize] != expected.map(|x| x as f32) {
            return Err("stale GPU identity");
        }
    }
    for row in 16..HEIGHT as usize {
        for col in 1..WIDTH as usize {
            if out[row * WIDTH as usize + col] != out[row * WIDTH as usize] {
                return Err("partial metadata row");
            }
        }
    }
    if out[20 * WIDTH as usize][0] != 0.0 {
        return Err("night vision unsupported");
    }
    if out[16 * WIDTH as usize][1] <= 0.0
        || out[16 * WIDTH as usize][2] <= 0.0
        || out[16 * WIDTH as usize][3] < 1.0
    {
        return Err("invalid meter/scale");
    }
    Ok(out)
}
impl Pending {
    fn values(&self, device: &wgpu::Device) -> Result<Vec<[f32; 4]>, &'static str> {
        let slice = self.buffer.slice(..);
        let (tx, rx) = std::sync::mpsc::channel();
        slice.map_async(wgpu::MapMode::Read, move |r| {
            let _ = tx.send(r);
        });
        if device.poll(wgpu::PollType::wait_indefinitely()).is_err()
            || !matches!(rx.recv(), Ok(Ok(())))
        {
            return Err("map failed");
        }
        let mapped = slice.get_mapped_range();
        let values = decode(&mapped, &self.input);
        drop(mapped);
        self.buffer.unmap();
        values
    }
    pub fn finish(self, device: &wgpu::Device) -> Result<Vec<String>, &'static str> {
        let values = self.values(device)?;
        let mut lines = vec![format!(
            "PostOptics version=1 requestWords={:?} frameWords={:?} sourceWords={:?} output={}x{} originalCameraHash={} preparedCameraHash={} stagingBytes={}",
            self.input.request,
            self.input.frame,
            self.input.source,
            self.input.dims[0],
            self.input.dims[1],
            self.cameras.0,
            self.cameras.1,
            READBACK_BYTES
        )];
        for tile in 0..4usize {
            for pixel in 0..25usize {
                lines.push(format!(
                    "PostOptics tile={} x={} y={} hdr={:?} bloom={:?} precurve={:?} curve={:?}",
                    tile,
                    self.input.origins[tile][0] + pixel as u32 % 5,
                    self.input.origins[tile][1] + pixel as u32 / 5,
                    values[(tile * 4) * 25 + pixel],
                    values[(tile * 4 + 1) * 25 + pixel],
                    values[(tile * 4 + 2) * 25 + pixel],
                    values[(tile * 4 + 3) * 25 + pixel]
                ));
            }
        }
        for row in 16..HEIGHT as usize {
            lines.push(format!(
                "PostOptics metadataRow={} value={:?}",
                row,
                values[row * 25]
            ));
        }
        lines.push("PostOptics completed pairedScreenshot=1".into());
        let tag = format!(
            "request={} frame={} source={}",
            number(self.input.request),
            number(self.input.frame),
            number(self.input.source)
        );
        Ok(lines
            .into_iter()
            .map(|line| format!("{} {}", line, tag))
            .collect())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn packet(input: &Input) -> Vec<u8> {
        let mut b = vec![0; READBACK_BYTES as usize];
        for (row, v) in [
            (16, [0.0, 1.0, 1.0, 11.2]),
            (22, input.request.map(|x| x as f32)),
            (23, input.frame.map(|x| x as f32)),
            (24, input.source.map(|x| x as f32)),
        ] {
            for col in 0..WIDTH as usize {
                for (i, f) in v.iter().enumerate() {
                    let o = row * STRIDE as usize + col * 16 + i * 4;
                    b[o..o + 4].copy_from_slice(&f.to_le_bytes());
                }
            }
        }
        b
    }
    #[test]
    fn byte_cap_tiles_and_exact_large_ids() {
        let i = input((1280, 720), u64::MAX, u64::MAX - 1, 1, None).unwrap();
        assert_eq!(i.request, [65535; 4]);
        assert_eq!(i.frame, [65534, 65535, 65535, 65535]);
        assert!(decode(&packet(&i), &i).is_ok());
        for tile in [
            "1,10;10,10;10,10;10,10",
            "1280,10;10,10;10,10;10,10",
            "10,10;10,10",
            "NaN,10;10,10;10,10;10,10",
        ] {
            assert!(input((1280, 720), 1, 1, 1, Some(tile)).is_err());
        }
        assert!(input((4, 720), 1, 1, 1, None).is_err());
        assert!(input((1280, 720), 0, 1, 1, None).is_err());
        assert_eq!(std::mem::size_of::<Input>(), 128);
    }
    #[test]
    fn stale_truncated_nonfinite_and_nv_packets_refused() {
        let i = input((640, 480), 1, 2, 3, None).unwrap();
        let b = packet(&i);
        assert!(decode(&b[..b.len() - 1], &i).is_err());
        for (offset, v) in [
            (22 * STRIDE as usize, 2.0),
            (5 * STRIDE as usize, f32::INFINITY),
            (20 * STRIDE as usize, 1.0),
            (16 * STRIDE as usize + 4, 0.0),
        ] {
            let mut bad = b.clone();
            bad[offset..offset + 4].copy_from_slice(&v.to_le_bytes());
            assert!(decode(&bad, &i).is_err());
        }
    }
    #[test]
    fn request_overlap_failed_attempt_and_serial_exhaustion_refused() {
        let mut s = State::new();
        s.request(false);
        s.begin();
        assert!(s.valid);
        assert!(s.pending.is_none());
        s.request(true);
        assert!(!s.valid);
        s.request(false);
        assert!(s.valid);
        s.cameras = Some((1, 2));
        s.begin();
        assert!(s.cameras.is_none());
        s.frame = u64::MAX;
        s.begin();
        assert!(!s.valid);
        s.request = u64::MAX;
        s.request(false);
        assert!(!s.valid);
    }
    #[test]
    fn actual_original_and_prepared_camera_bits_and_refusals() {
        let mut a: WgrCamera = bytemuck::Zeroable::zeroed();
        a.proj[0] = 1.;
        a.view[0] = 1.;
        let mut b = a;
        b.proj[8] = 0.125;
        let mut s = State::new();
        s.request(false);
        s.begin();
        s.capture(Some(&a), Some(&b), 1);
        let hashes = s.cameras.unwrap();
        assert_ne!(hashes.0, hashes.1);
        s.begin();
        s.capture(Some(&a), Some(&b), 0);
        assert!(s.cameras.is_none());
        s.capture(Some(&a), None, 1);
        assert!(s.cameras.is_none());
        b.view[0] = f32::NAN;
        s.capture(Some(&a), Some(&b), 1);
        assert!(s.cameras.is_none());
        assert_eq!(number(words(u64::MAX - 33)), u64::MAX - 33);
    }
    #[test]
    fn production_and_diagnostic_wgsl_validate_and_live_bind_contract() {
        let source = include_str!("tonemap.wgsl");
        let module = naga::front::wgsl::parse_str(source).unwrap();
        naga::valid::Validator::new(
            naga::valid::ValidationFlags::all(),
            naga::valid::Capabilities::all(),
        )
        .validate(&module)
        .unwrap();
        assert!(module.entry_points.iter().any(|e| e.name == "fs_main"));
        assert!(
            module
                .entry_points
                .iter()
                .any(|e| e.name == "fs_post_optics_probe")
        );
        let lib = include_str!("lib.rs");
        assert!(
            lib.find("exposure_published=exposure.render(encoder)")
                .unwrap()
                < lib.find("tone.probe(&self.device,encoder").unwrap()
        );
        assert!(include_str!("tonemap.rs").contains("self.probe_source.as_ref() != Some(source)"));
    }
    #[test]
    #[ignore = "real device; WGR_SCREENSHOT_POST_OPTICS=1 required; root serializes"]
    fn actual_same_encoder_meter_hdr_bloom_exposure_and_stale_bind_refusal() {
        assert!(enabled(), "run with WGR_SCREENSHOT_POST_OPTICS=1");
        let (device, queue) = crate::gfx3d::cull::tests::headless().expect("actual GPU required");
        let texture = |label: &str, colour: [f32; 4]| {
            let t = device.create_texture(&wgpu::TextureDescriptor {
                label: Some(label),
                size: wgpu::Extent3d {
                    width: 16,
                    height: 16,
                    depth_or_array_layers: 1,
                },
                mip_level_count: 1,
                sample_count: 1,
                dimension: wgpu::TextureDimension::D2,
                format: wgpu::TextureFormat::Rgba16Float,
                usage: wgpu::TextureUsages::RENDER_ATTACHMENT
                    | wgpu::TextureUsages::TEXTURE_BINDING,
                view_formats: &[],
            });
            let v = t.create_view(&Default::default());
            let mut e = device.create_command_encoder(&Default::default());
            {
                let _pass = e.begin_render_pass(&wgpu::RenderPassDescriptor {
                    label: Some(label),
                    color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                        view: &v,
                        depth_slice: None,
                        resolve_target: None,
                        ops: wgpu::Operations {
                            load: wgpu::LoadOp::Clear(wgpu::Color {
                                r: colour[0] as f64,
                                g: colour[1] as f64,
                                b: colour[2] as f64,
                                a: colour[3] as f64,
                            }),
                            store: wgpu::StoreOp::Store,
                        },
                    })],
                    depth_stencil_attachment: None,
                    timestamp_writes: None,
                    occlusion_query_set: None,
                    multiview_mask: None,
                });
            }
            queue.submit([e.finish()]);
            (t, v)
        };
        let (_hdr, hdr) = texture("actual_hdr", [4., 8., 16., 1.]);
        let (_bloom, bloom) = texture("actual_bloom", [0.5, 0.25, 0.125, 1.]);
        let (_stale, stale) = texture("stale_hdr", [0., 0., 0., 1.]);
        let mut exposure = crate::exposure::Exposure::new(&device, &queue);
        exposure.resize(&device, 16, 16, &hdr);
        let mut ep = crate::ffi::WgrExposure::default();
        ep.enabled = 1.;
        ep.key = 1.;
        ep.rate = 1.;
        ep.min_scale = 0.01;
        ep.max_scale = 2.;
        exposure.upload_params(&queue, &ep);
        let mut tone = crate::tonemap::Tonemap::new(&device, wgpu::TextureFormat::Rgba8Unorm);
        tone.set_source(&device, &hdr, &bloom, exposure.scale_view());
        let mut tp = crate::ffi::WgrTonemap::default();
        tp.exposure = 2.;
        tp.bloom_intensity = 0.4;
        tp.temperature = 0.3;
        tp.tint = -0.2;
        tone.upload_params(&queue, &tp, 0.0);
        let mut s = State::new();
        s.request(false);
        s.begin();
        s.cameras = Some((11, 22));
        let mut e = device.create_command_encoder(&Default::default());
        assert!(exposure.render(&mut e));
        tone.probe(&device, &mut e, &exposure, &mut s, (16, 16))
            .unwrap();
        queue.submit([e.finish()]);
        let values = s.pending.take().unwrap().values(&device).unwrap();
        let m = values[16 * 25];
        assert_eq!(
            values[25 * 25],
            [ep.enabled, ep.key, ep.min_scale, ep.max_scale]
        );
        assert_eq!(
            values[26 * 25],
            [ep.rate, ep.sky_weight, ep._pad1, ep._pad2]
        );
        assert_eq!(values[27 * 25], [16., 16., 16., 16.]);
        let compiled_white = std::env::var("WGR_TONEMAP_WHITE")
            .ok()
            .and_then(|x| x.trim().parse::<f64>().ok())
            .filter(|x| *x >= 1.)
            .unwrap_or(11.2) as f32;
        assert_eq!(m[3], compiled_white);
        assert!(m[1] > 0.0);
        let avg = (m[0] / m[1]).exp2();
        let expected_lum = 4. * 0.2126 + 8. * 0.7152 + 16. * 0.0722;
        assert!((avg - expected_lum).abs() < 0.03, "actual meter {:?}", m);
        assert!((m[2] - 1. / avg).abs() < 0.001);
        for tile in 0..4usize {
            for p in 0..25usize {
                assert_eq!(&values[(tile * 4) * 25 + p][..3], &[4., 8., 16.]);
                assert_eq!(&values[(tile * 4 + 1) * 25 + p][..3], &[0.5, 0.25, 0.125]);
                let expected = [
                    (4. + 0.5 * 0.4) * 2. * m[2] * 1.06,
                    (8. + 0.25 * 0.4) * 2. * m[2] * 1.04,
                    (16. + 0.125 * 0.4) * 2. * m[2] * 0.94,
                ];
                for c in 0..3 {
                    assert!((values[(tile * 4 + 2) * 25 + p][c] - expected[c]).abs() < 1e-5);
                    let partial = |x: f32| {
                        ((x * (0.15 * x + 0.10 * 0.50) + 0.20 * 0.02)
                            / (x * (0.15 * x + 0.50) + 0.20 * 0.30))
                            - 0.02 / 0.30
                    };
                    let curve = partial(expected[c]) / partial(compiled_white.max(1.));
                    assert!((values[(tile * 4 + 3) * 25 + p][c] - curve).abs() < 2e-5);
                }
            }
        }
        s.begin();
        s.cameras = Some((11, 22));
        exposure.set_source(&device, &stale);
        let mut e = device.create_command_encoder(&Default::default());
        assert!(
            tone.probe(&device, &mut e, &exposure, &mut s, (16, 16))
                .is_err()
        );
        assert!(s.pending.is_none());
        let mut other_exposure = crate::exposure::Exposure::new(&device, &queue);
        other_exposure.resize(&device, 16, 16, &hdr);
        s.begin();
        s.cameras = Some((11, 22));
        assert!(
            tone.probe(&device, &mut e, &other_exposure, &mut s, (16, 16))
                .is_err(),
            "same source but independently replaced scale must be refused"
        );
        assert!(s.pending.is_none());
    }
}
