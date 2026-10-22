//! PERF-LOD-USAGE: which retained LOD levels the main view actually draws, and how much pool
//! geometry the levels it does NOT draw hold. Diagnostic only (WGR_LOD_USAGE_REPORT=1).
//!
//! Why: a native Everon window keeps ~1.3 GB of geometry live, every LOD of every registered
//! model, while the main view's histogram is overwhelmingly LOD 3-4. Releasing the fine levels
//! nobody draws is the obvious VRAM lever; this measures the prize before anything is built.
//!
//! How: the cull's COUNT pass leaves `sec_count[s]` = how many surviving instances chose a LOD
//! containing global section `s`, before EMIT overwrites it. Every SAMPLE_EVERY frames the main
//! view's scratch is copied (between the COUNT and EMIT passes) into a readback buffer, mapped
//! asynchronously and OR-ed into a per-section "used in this window" flag. Nothing waits.

use std::sync::mpsc;

const SAMPLE_EVERY: u64 = 15;

pub(super) fn enabled() -> bool {
    static V: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *V.get_or_init(|| std::env::var("WGR_LOD_USAGE_REPORT").as_deref() == Ok("1"))
}

enum State {
    Idle,
    Pending(usize),
    Mapping(usize, mpsc::Receiver<Result<(), wgpu::BufferAsyncError>>),
}

struct Slot {
    buffer: Option<wgpu::Buffer>,
    cap: usize,
    state: State,
}

pub(super) struct LodUsage {
    slots: Vec<Slot>,
    frame: u64,
    /// Section count the last capture wanted; harvest (which has the device) sizes slots to it.
    wanted: usize,
    /// Per global section: drawn by the main view at least once in this window.
    pub used: Vec<bool>,
    pub samples: u32,
    pub window_start: std::time::Instant,
}

impl LodUsage {
    pub fn new() -> Self {
        Self {
            slots: (0..2).map(|_| Slot { buffer: None, cap: 0, state: State::Idle }).collect(),
            frame: 0,
            wanted: 0,
            used: Vec::new(),
            samples: 0,
            window_start: std::time::Instant::now(),
        }
    }

    /// Between COUNT and EMIT of the main view: maybe copy `sec_count[..sections]`.
    pub fn capture(&mut self, encoder: &mut wgpu::CommandEncoder, sec_count: &wgpu::Buffer, sections: usize) {
        self.frame += 1;
        if sections == 0 || self.frame % SAMPLE_EVERY != 0 {
            return;
        }
        self.wanted = sections;
        let Some(slot) = self.slots.iter_mut().find(|s| matches!(s.state, State::Idle) && s.cap >= sections) else { return };
        let bytes = (sections as u64 * 4).min(sec_count.size());
        encoder.copy_buffer_to_buffer(sec_count, 0, slot.buffer.as_ref().unwrap(), 0, bytes);
        slot.state = State::Pending((bytes / 4) as usize);
    }

    /// After submit: start maps, fold finished ones into `used`. Never waits.
    pub fn harvest(&mut self, device: &wgpu::Device) {
        for slot in &mut self.slots {
            if matches!(slot.state, State::Idle) && slot.cap < self.wanted {
                slot.cap = self.wanted.next_power_of_two();
                slot.buffer = Some(device.create_buffer(&wgpu::BufferDescriptor {
                    label: Some("wgr_lod_usage_readback"),
                    size: slot.cap as u64 * 4,
                    usage: wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::MAP_READ,
                    mapped_at_creation: false,
                }));
            }
        }
        for slot in &mut self.slots {
            if let State::Pending(n) = slot.state {
                let (tx, rx) = mpsc::channel();
                slot.buffer.as_ref().unwrap().slice(..n as u64 * 4).map_async(wgpu::MapMode::Read, move |r| {
                    let _ = tx.send(r);
                });
                slot.state = State::Mapping(n, rx);
            }
        }
        let _ = device.poll(wgpu::PollType::Poll);
        for slot in &mut self.slots {
            let State::Mapping(n, ref rx) = slot.state else { continue };
            match rx.try_recv() {
                Ok(Ok(())) => {
                    let buffer = slot.buffer.as_ref().unwrap();
                    {
                        let view = buffer.slice(..n as u64 * 4).get_mapped_range().expect("wgpu: mapped range of a mapped buffer");
                        let words: &[u32] = bytemuck::cast_slice(&view);
                        if self.used.len() < words.len() {
                            self.used.resize(words.len(), false);
                        }
                        for (u, w) in self.used.iter_mut().zip(words) {
                            *u |= *w != 0;
                        }
                    }
                    buffer.unmap();
                    self.samples += 1;
                    slot.state = State::Idle;
                }
                Ok(Err(_)) => slot.state = State::Idle,
                Err(mpsc::TryRecvError::Empty) => {}
                Err(mpsc::TryRecvError::Disconnected) => slot.state = State::Idle,
            }
        }
    }

    pub fn reset_window(&mut self) {
        self.used.iter_mut().for_each(|u| *u = false);
        self.samples = 0;
        self.window_start = std::time::Instant::now();
    }
}

/// The window's verdict, in bytes of pool geometry (vertices + indices, each mesh once).
#[derive(Default, Debug)]
pub(super) struct UsageVerdict {
    pub total: u64,
    /// Meshes of levels the main view drew, plus ONE finer level kept as the pop-in margin.
    pub needed_with_margin: u64,
    /// Meshes of levels the main view drew, exactly.
    pub drawn: u64,
    /// Meshes of models the main view did not draw at all in the window (outside the frustum or
    /// too small): candidates only for a coarse-only residency, never measured as safe to drop.
    pub undrawn_models: u64,
    pub models: u32,
    pub models_drawn: u32,
}

/// `levels_of(model)` -> the model's LOD rows as (section range); `mesh_of(section)` -> a mesh
/// identity (its pool vbase) and its bytes.
pub(super) fn verdict(used: &[bool], models: &[(u32, u32)], lod_sections: &[(u32, u32)],
                      mesh_of: impl Fn(usize) -> Option<(u32, u64)>) -> UsageVerdict {
    use std::collections::HashMap;
    let mut v = UsageVerdict::default();
    // Per mesh: 0 = referenced, 1 = model undrawn, 2 = kept as margin, 3 = drawn.
    let mut class: HashMap<u32, (u8, u64)> = HashMap::new();
    let mut bump = |key: u32, bytes: u64, c: u8| {
        let e = class.entry(key).or_insert((0, bytes));
        e.0 = e.0.max(c);
    };
    for &(lod_base, lod_count) in models {
        if lod_count == 0 {
            continue;
        }
        v.models += 1;
        let level_used = |l: u32| {
            let (b, n) = lod_sections[(lod_base + l) as usize];
            (b..b + n).any(|s| used.get(s as usize).copied().unwrap_or(false))
        };
        let finest = (0..lod_count).find(|&l| level_used(l));
        if finest.is_some() {
            v.models_drawn += 1;
        }
        for l in 0..lod_count {
            let c = match finest {
                None => 1,
                Some(f) if level_used(l) => { let _ = f; 3 }
                Some(f) if l + 1 == f => 2,
                Some(f) if l > f => 2, // coarser than the finest drawn: cheap, keep (fallback levels)
                _ => 0,
            };
            let (b, n) = lod_sections[(lod_base + l) as usize];
            for s in b..b + n {
                if let Some((key, bytes)) = mesh_of(s as usize) {
                    bump(key, bytes, c);
                }
            }
        }
    }
    for (_, (c, bytes)) in class {
        v.total += bytes;
        match c {
            3 => { v.drawn += bytes; v.needed_with_margin += bytes; }
            2 => v.needed_with_margin += bytes,
            1 => v.undrawn_models += bytes,
            _ => {}
        }
    }
    v
}
