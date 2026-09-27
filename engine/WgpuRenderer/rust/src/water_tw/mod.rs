//! Tidewater Native water backend (Tidewater Water Plan v5, W2+).
//!
//! W1 lands only the backend seam: this type exists so `WaterBackend` has its final shape, but
//! `AVAILABLE` is false and the selector refuses it, so no resource here is ever created.

use crate::ffi::{WgrCamera, WgrWaterParams};

/// Whether this build carries a working Tidewater backend. The Water tab greys the entry out
/// (and `WaterBackend` refuses the selection) while this is false.
pub const AVAILABLE: bool = false;

/// `WgrWaterParams::tidewater` defaults: Tidewater's own values (App.js / OceanFFT.js /
/// WaterMaterial.js / SeaDetail.js at 4811ba48). Lane meanings are in wgpu_renderer.hpp.
pub const DEFAULT_PARAMS: [[f32; 4]; 4] = [
    [1.0, 0.9, 0.48, 5.0],
    [1.0, 1.0, 1.0, 1.0],
    [1.0, 0.035, 1.0, 0.035],
    [1.0, 1.0, 0.3, 0.0],
];

pub struct Tidewater {
    last_params: Option<WgrWaterParams>,
}

impl Tidewater {
    pub fn new(
        _device: &wgpu::Device,
        _queue: &wgpu::Queue,
        _camera_layout: &wgpu::BindGroupLayout,
        _surface_format: wgpu::TextureFormat,
        _sample_count: u32,
    ) -> Self {
        Self { last_params: None }
    }

    pub fn set_params(&mut self, _queue: &wgpu::Queue, params: WgrWaterParams) {
        self.last_params = Some(params);
    }

    pub fn ocean_level(&self) -> Option<f32> {
        self.last_params.map(|p| p.sea_level)
    }

    pub fn surface_params(&self) -> Option<(f32, f32, f32)> {
        self.last_params
            .map(|p| (p.sea_level, p.time, p.fft_control[3].max(0.0)))
    }

    pub fn set_depth_view(&mut self, _device: &wgpu::Device, _depth: &wgpu::TextureView, _view_gen: u64) {}
    pub fn set_env_view(&mut self, _device: &wgpu::Device, _env: &wgpu::TextureView, _view_gen: u64) {}
    pub fn set_scene_view(&mut self, _device: &wgpu::Device, _scene: &wgpu::TextureView, _view_gen: u64) {}
    pub fn set_heightmap(
        &mut self,
        _device: &wgpu::Device,
        _queue: &wgpu::Queue,
        _heightmap: &wgpu::TextureView,
        _view_gen: u64,
        _params: &crate::terrain::TerrainConformParams,
    ) {
    }
    pub fn prepare(&mut self, _device: &wgpu::Device, _queue: &wgpu::Queue, _camera: Option<&WgrCamera>) {}
    pub fn update(&mut self, _encoder: &mut wgpu::CommandEncoder, _timers: &crate::gpu_timers::GpuTimers) {}
    pub fn draw(&self, _pass: &mut wgpu::RenderPass<'_>, _camera: &wgpu::BindGroup, _offset: u32) {}
}
