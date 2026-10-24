//! LOAD-004: a Vulkan pipeline cache kept on disk between launches.
//!
//! The renderer spent ~14 s building pipelines on EVERY launch of the owner's PC (gfx3d 5.2 s for
//! about a dozen pipelines, terrain 3.7 s, grass 3.3 s, measured 2026-10-24 with build 1c9e098),
//! the second launch of a build exactly as slow as the first. The SPIR-V wgpu hands the driver is
//! identical from run to run (`shader_cpu_timing`), and the CPU part of a pipeline (compose,
//! validate, SPIR-V) is a few ms -- the time is the driver's compile, and the driver's own
//! implicit disk cache evidently does not serve it there. A `wgpu::PipelineCache` seeded from the
//! last run's data lets vkCreate*Pipelines reuse the compiled code.
//!
//! One cache, for the one device the renderer creates. [`get`] answers only for that device, so
//! a test that opens a second device in the same process builds without a cache rather than with
//! one that belongs to another device. The data is written after start-up and when the renderer
//! is dropped, via a temporary file and a rename, so a crash mid-write cannot leave a torn file;
//! the driver validates what it is given and an outdated or foreign blob is ignored (`fallback`).
//!
//! `WGR_PIPELINE_CACHE=0` turns it off; `WGR_PIPELINE_CACHE_DIR` overrides the directory
//! (default `%LOCALAPPDATA%\OpenPoseidon`, elsewhere `$XDG_CACHE_HOME` / `~/.cache`).

use std::path::PathBuf;
use std::sync::OnceLock;

struct Installed {
    device: wgpu::Device,
    cache: wgpu::PipelineCache,
    path: PathBuf,
    loaded_bytes: usize,
}

static INSTALLED: OnceLock<Installed> = OnceLock::new();

pub fn enabled() -> bool {
    std::env::var("WGR_PIPELINE_CACHE").as_deref() != Ok("0")
}

/// The feature to request at device creation, if the adapter has it and the cache is on.
pub fn feature(adapter: &wgpu::Adapter) -> wgpu::Features {
    if enabled() {
        adapter.features() & wgpu::Features::PIPELINE_CACHE
    } else {
        wgpu::Features::empty()
    }
}

fn cache_dir() -> Option<PathBuf> {
    if let Some(d) = std::env::var_os("WGR_PIPELINE_CACHE_DIR") {
        return Some(PathBuf::from(d));
    }
    if let Some(d) = std::env::var_os("LOCALAPPDATA") {
        return Some(PathBuf::from(d).join("OpenPoseidon"));
    }
    if let Some(d) = std::env::var_os("XDG_CACHE_HOME") {
        return Some(PathBuf::from(d).join("openposeidon"));
    }
    std::env::var_os("HOME").map(|h| PathBuf::from(h).join(".cache").join("openposeidon"))
}

/// Create the cache for `device` from the last run's data. Returns a log line, or None when the
/// cache is off, unsupported or already installed.
pub fn install(device: &wgpu::Device, adapter_info: &wgpu::AdapterInfo) -> Option<String> {
    if !enabled() || !device.features().contains(wgpu::Features::PIPELINE_CACHE) || INSTALLED.get().is_some() {
        return None;
    }
    let key = wgpu::util::pipeline_cache_key(adapter_info)?;
    let dir = cache_dir()?;
    let path = dir.join(format!("{key}.bin"));
    let data = std::fs::read(&path).ok();
    let loaded_bytes = data.as_ref().map_or(0, |d| d.len());
    // SAFETY: the data is only ever bytes this module wrote from `PipelineCache::get_data` (or
    // nothing); wgpu and the driver validate the header (vendor, device, driver version, wgpu
    // version) and with `fallback: true` an unusable blob yields an empty cache instead.
    let cache = unsafe {
        device.create_pipeline_cache(&wgpu::PipelineCacheDescriptor {
            label: Some("wgr_pipeline_cache"),
            data: data.as_deref(),
            fallback: true,
        })
    };
    let line = format!(
        "wgpu pipeline cache: {} ({} KB loaded) (WGR_PIPELINE_CACHE=0 turns it off)",
        path.display(),
        loaded_bytes / 1024
    );
    let _ = INSTALLED.set(Installed { device: device.clone(), cache, path, loaded_bytes });
    Some(line)
}

/// The cache for pipelines built on `device`; None for any other device or when not installed.
pub fn get(device: &wgpu::Device) -> Option<&'static wgpu::PipelineCache> {
    match INSTALLED.get() {
        Some(i) if i.device == *device => Some(&i.cache),
        _ => None,
    }
}

/// Write the cache's current data to disk. Returns a log line describing what happened.
pub fn save() -> Option<String> {
    let i = INSTALLED.get()?;
    let data = i.cache.get_data()?;
    if data.is_empty() {
        return None;
    }
    let dir = i.path.parent()?;
    if let Err(e) = std::fs::create_dir_all(dir) {
        return Some(format!("wgpu pipeline cache: cannot create {}: {e}", dir.display()));
    }
    let tmp = i.path.with_extension(format!("tmp{}", std::process::id()));
    let result = std::fs::write(&tmp, &data).and_then(|_| std::fs::rename(&tmp, &i.path));
    Some(match result {
        Ok(()) => format!(
            "wgpu pipeline cache saved: {} KB (was {} KB at start)",
            data.len() / 1024,
            i.loaded_bytes / 1024
        ),
        Err(e) => {
            let _ = std::fs::remove_file(&tmp);
            format!("wgpu pipeline cache: write {} failed: {e}", i.path.display())
        }
    })
}
