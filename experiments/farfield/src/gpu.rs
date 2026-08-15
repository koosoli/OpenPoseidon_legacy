// Headless wgpu context + measurement plumbing shared by every experiment.
//
// Measurement hygiene follows the house rule from the renderer's profiling work:
// run many frames, quote the CHEAPEST steady frame, never the mean. A shared GPU
// (a compositor, another process) inflates every timed region by a common factor,
// so the minimum is the only figure that is not contaminated by that.

use std::borrow::Cow;

pub struct Gpu {
    pub device: wgpu::Device,
    pub queue: wgpu::Queue,
    pub adapter_name: String,
    pub backend: String,
    pub timestamps: bool,
    period_ns: f32,
}

impl Gpu {
    pub fn new() -> Self {
        let instance = wgpu::Instance::new(wgpu::InstanceDescriptor::new_without_display_handle());
        let adapter = pollster::block_on(instance.request_adapter(&wgpu::RequestAdapterOptions {
            power_preference: wgpu::PowerPreference::HighPerformance,
            compatible_surface: None,
            force_fallback_adapter: false,
        }))
        .expect("no wgpu adapter");

        let info = adapter.get_info();
        let ts = wgpu::Features::TIMESTAMP_QUERY | wgpu::Features::TIMESTAMP_QUERY_INSIDE_ENCODERS;
        let have_ts = adapter.features().contains(ts);

        let mut limits = wgpu::Limits::default();
        // The 100 km world at 12.5 m is an 8192^2 heightfield; the default 8192 texture
        // limit is exactly enough, but the mip chain and the readbacks want headroom.
        limits.max_texture_dimension_2d = limits.max_texture_dimension_2d.max(8192);
        limits.max_storage_buffer_binding_size = limits
            .max_storage_buffer_binding_size
            .max(256 * 1024 * 1024);
        limits.max_buffer_size = limits.max_buffer_size.max(512 * 1024 * 1024);

        let (device, queue) = pollster::block_on(adapter.request_device(&wgpu::DeviceDescriptor {
            label: Some("farfield"),
            required_features: if have_ts { ts } else { wgpu::Features::empty() },
            required_limits: limits,
            ..Default::default()
        }))
        .expect("request_device failed");

        device.on_uncaptured_error(std::sync::Arc::new(|e| panic!("wgpu error: {e}")));

        let period_ns = queue.get_timestamp_period();
        Self {
            device,
            queue,
            adapter_name: info.name,
            backend: format!("{:?}", info.backend),
            timestamps: have_ts,
            period_ns,
        }
    }

    pub fn shader(&self, label: &str, src: &str) -> wgpu::ShaderModule {
        self.device
            .create_shader_module(wgpu::ShaderModuleDescriptor {
                label: Some(label),
                source: wgpu::ShaderSource::Wgsl(Cow::Borrowed(src)),
            })
    }

    /// Blocking readback of a GPU buffer's contents.
    pub fn read_buffer(&self, src: &wgpu::Buffer, bytes: u64) -> Vec<u8> {
        let staging = self.device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("readback"),
            size: bytes,
            usage: wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::MAP_READ,
            mapped_at_creation: false,
        });
        let mut enc = self
            .device
            .create_command_encoder(&wgpu::CommandEncoderDescriptor { label: None });
        enc.copy_buffer_to_buffer(src, 0, &staging, 0, bytes);
        self.queue.submit([enc.finish()]);
        let slice = staging.slice(..);
        slice.map_async(wgpu::MapMode::Read, |_| {});
        self.device
            .poll(wgpu::PollType::wait_indefinitely())
            .unwrap();
        let out = slice.get_mapped_range().to_vec();
        drop(slice);
        staging.unmap();
        out
    }
}

/// One timestamp pair, resolved on demand. Deliberately synchronous: these are
/// benchmarks, not a frame loop, so a stall per frame costs nothing and removes a
/// whole class of "which frame did that number come from" errors.
pub struct Timer {
    set: wgpu::QuerySet,
    resolve: wgpu::Buffer,
    period_ns: f32,
    enabled: bool,
}

impl Timer {
    pub fn new(gpu: &Gpu) -> Self {
        let set = gpu.device.create_query_set(&wgpu::QuerySetDescriptor {
            label: Some("farfield-timer"),
            ty: wgpu::QueryType::Timestamp,
            count: 2,
        });
        let resolve = gpu.device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("farfield-timer-resolve"),
            size: 16,
            usage: wgpu::BufferUsages::QUERY_RESOLVE | wgpu::BufferUsages::COPY_SRC,
            mapped_at_creation: false,
        });
        Self {
            set,
            resolve,
            period_ns: gpu.period_ns,
            enabled: gpu.timestamps,
        }
    }

    pub fn writes(&self) -> Option<wgpu::RenderPassTimestampWrites<'_>> {
        self.enabled.then(|| wgpu::RenderPassTimestampWrites {
            query_set: &self.set,
            beginning_of_pass_write_index: Some(0),
            end_of_pass_write_index: Some(1),
        })
    }

    pub fn resolve_into(&self, enc: &mut wgpu::CommandEncoder) {
        if self.enabled {
            enc.resolve_query_set(&self.set, 0..2, &self.resolve, 0);
        }
    }

    /// Milliseconds between the two timestamps, or None when unsupported.
    pub fn read_ms(&self, gpu: &Gpu) -> Option<f32> {
        if !self.enabled {
            return None;
        }
        let bytes = gpu.read_buffer(&self.resolve, 16);
        let t0 = u64::from_le_bytes(bytes[0..8].try_into().unwrap());
        let t1 = u64::from_le_bytes(bytes[8..16].try_into().unwrap());
        if t1 <= t0 {
            return None;
        }
        Some((t1 - t0) as f32 * self.period_ns / 1.0e6)
    }
}

/// Summary of a repeated measurement. `min` is the figure to quote.
#[derive(Clone, Copy, Debug)]
pub struct Stat {
    pub min: f32,
    pub median: f32,
    pub max: f32,
    pub n: usize,
}

impl Stat {
    pub fn of(mut v: Vec<f32>) -> Self {
        v.sort_by(|a, b| a.partial_cmp(b).unwrap());
        let n = v.len();
        if n == 0 {
            return Self { min: 0.0, median: 0.0, max: 0.0, n: 0 };
        }
        Self { min: v[0], median: v[n / 2], max: v[n - 1], n }
    }
}

/// Minimal 24-bit BMP writer, so screenshots need no image-codec dependency.
pub fn write_bmp(path: &str, w: u32, h: u32, rgba: &[u8]) -> std::io::Result<()> {
    let row = (w * 3 + 3) & !3;
    let size = 54 + row * h;
    let mut f = Vec::with_capacity(size as usize);
    f.extend_from_slice(b"BM");
    f.extend_from_slice(&size.to_le_bytes());
    f.extend_from_slice(&0u32.to_le_bytes());
    f.extend_from_slice(&54u32.to_le_bytes());
    f.extend_from_slice(&40u32.to_le_bytes());
    f.extend_from_slice(&(w as i32).to_le_bytes());
    f.extend_from_slice(&(h as i32).to_le_bytes()); // positive = bottom-up
    f.extend_from_slice(&1u16.to_le_bytes());
    f.extend_from_slice(&24u16.to_le_bytes());
    f.extend_from_slice(&[0u8; 24]);
    for y in (0..h).rev() {
        for x in 0..w {
            let i = ((y * w + x) * 4) as usize;
            f.push(rgba[i + 2]);
            f.push(rgba[i + 1]);
            f.push(rgba[i]);
        }
        for _ in 0..(row - w * 3) {
            f.push(0);
        }
    }
    std::fs::write(path, f)
}
