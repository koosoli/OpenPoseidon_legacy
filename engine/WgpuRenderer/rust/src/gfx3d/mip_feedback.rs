// GPU MIP FEEDBACK — what the frame actually sampled, as opposed to how far away the CPU
// thought the object was.
//
// Residency here has always been decided on the CPU: a distance sweep marks a texture used,
// an LRU evicts the ones that have not been marked lately, and whole mip chains are uploaded
// in one shot. Nothing in that loop knows what DETAIL the frame needed. A wall filling the
// screen and the same wall at a grazing angle across a valley are the same "used" mark, and a
// budget tuned so that assumption holds on one world does not hold on the next.
//
// This is the missing measurement. The object fragment shader derives the mip level it would
// sample (dpdx/dpdy of the UV against the texture's dimensions — WGSL has no textureQueryLod)
// and atomicMax-es it into one word per bindless slot. The buffer is copied into a readback
// ring, mapped asynchronously, and read a couple of frames later. Nothing waits on the GPU.
//
// Three decisions worth knowing when reading this:
//
//   * ONE SLOT PER BINDLESS INDEX, not per texture handle. The bindless slot is the only
//     identity the shader has, and it is LEASED by the C++ TextureWgpu for that texture's
//     whole life (REN-RES-001), so the slot is a stable name for the texture across an
//     evict/re-upload cycle. That is exactly what a residency signal needs.
//
//   * A ROTATING GROUP, not the whole table every frame. Each frame only slots congruent to
//     `group` modulo `groups` write. That keeps the atomic traffic to a fraction of the
//     fragments, which is what makes the pass cost nothing measurable; the price is that a
//     slot's answer is up to `groups` frames old. For a residency decision measured in
//     hundreds of frames of grace that is free.
//
//   * ENCODED SO THAT ZERO MEANS "NOT SAMPLED". The stored word is `MIP_CODE_BASE - level`,
//     so a finer mip is a LARGER number and the shader can use atomicMax. That matters
//     because the only cheap whole-buffer reset wgpu offers is `clear_buffer`, which writes
//     zeros — an atomicMin scheme would need the buffer filled with a sentinel every frame
//     instead. Zero therefore reads as "no fragment sampled this slot in its window", which
//     is itself the interesting signal: a resident texture nothing sampled.

use std::sync::mpsc;

/// Words reserved at the head of the feedback buffer before the per-slot codes begin.
///
/// Nothing writes them today: the rotation pair reaches the shader through the frame uniform's
/// two spare `renscale` lanes, which is a uniform-buffer read rather than a storage load in
/// every fragment. They are held anyway so the per-slot region starts at a 16-byte offset —
/// `clear_buffer` only demands 4, but a header is the obvious place for a future per-frame
/// scalar and moving the slot base later would silently reinterpret every stored code.
pub const HEADER_WORDS: u32 = 4;
const HEADER_BYTES: u64 = HEADER_WORDS as u64 * 4;

/// Level 0 encodes to this; level 15 encodes to `MIP_CODE_BASE - 15`. Kept in step with the
/// constant of the same name in gpu_driven.wgsl.
pub const MIP_CODE_BASE: u32 = 16;

/// Readback depth. Three, like the cull stats ring and the GPU timers': enough to ride out
/// triple-buffered presentation without ever blocking on a map.
const RING_SLOTS: usize = 3;

/// How many frames one full sweep of the texture table takes. 32 is the fraction at which the
/// added fragment work stopped being visible in the object-geometry GPU region; the whole
/// point of the rotation is that this number can be raised if it ever becomes visible again.
const DEFAULT_GROUPS: u32 = 32;

enum Slot {
    Idle,
    Pending,
    InFlight(mpsc::Receiver<Result<(), wgpu::BufferAsyncError>>),
}

/// One harvested observation for a bindless slot.
#[derive(Clone, Copy, Default)]
pub struct SlotObservation {
    /// The finest mip any fragment asked of this slot, in its most recent sampled window.
    pub desired_mip: u32,
    /// Frame index at which that was harvested. 0 = never observed.
    pub frame: u64,
}

pub struct MipFeedback {
    /// Per-slot codes on the GPU. Slot region starts at HEADER_WORDS.
    buf: Option<wgpu::Buffer>,
    capacity: u32,
    readback: Vec<(wgpu::Buffer, Slot)>,
    /// Latest observation per bindless slot, indexed by slot.
    latest: Vec<SlotObservation>,
    groups: u32,
    group: u32,
    frame: u64,
    /// Frames whose readback actually completed and was decoded — the denominator that says
    /// whether this instrument ran at all, as opposed to reporting zeros because it never did.
    harvests: u64,
    enabled: bool,
}

impl MipFeedback {
    pub fn new() -> Self {
        // Off by default is the wrong default for an instrument nobody would remember to turn
        // on, but ON with a cheap kill switch is the right one: WGR_MIP_FEEDBACK=0 restores
        // bit-identical shading and an empty buffer, which is the A/B the perf claim needs.
        let enabled = std::env::var("WGR_MIP_FEEDBACK")
            .map(|v| v != "0")
            .unwrap_or(true);
        let groups = std::env::var("WGR_MIP_FEEDBACK_GROUPS")
            .ok()
            .and_then(|v| v.parse::<u32>().ok())
            .map(|v| v.clamp(1, 1024))
            .unwrap_or(DEFAULT_GROUPS);
        eprintln!(
            "[wgr] gpu mip feedback: {} groups={} (WGR_MIP_FEEDBACK / WGR_MIP_FEEDBACK_GROUPS)",
            if enabled { "on" } else { "off" },
            groups
        );
        Self {
            buf: None,
            capacity: 0,
            readback: Vec::new(),
            latest: Vec::new(),
            groups,
            group: 0,
            frame: 0,
            harvests: 0,
            enabled,
        }
    }

    pub fn enabled(&self) -> bool {
        self.enabled
    }

    /// Allocate for `capacity` bindless slots. Returns true when the buffer moved, so the
    /// group-1 bind groups that borrow it are rebuilt. Idempotent once sized.
    pub fn ensure(&mut self, device: &wgpu::Device, capacity: u32) -> bool {
        let capacity = capacity.max(1);
        if self.buf.is_some() && self.capacity >= capacity {
            return false;
        }
        let words = HEADER_WORDS as u64 + capacity as u64;
        let bytes = words * 4;
        self.buf = Some(device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("wgr_mip_feedback"),
            usage: wgpu::BufferUsages::STORAGE
                | wgpu::BufferUsages::COPY_DST
                | wgpu::BufferUsages::COPY_SRC,
            size: bytes,
            mapped_at_creation: false,
        }));
        self.readback = (0..RING_SLOTS)
            .map(|i| {
                (
                    device.create_buffer(&wgpu::BufferDescriptor {
                        label: Some(&format!("wgr_mip_feedback_readback_{i}")),
                        size: bytes,
                        usage: wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::MAP_READ,
                        mapped_at_creation: false,
                    }),
                    Slot::Idle,
                )
            })
            .collect();
        self.capacity = capacity;
        self.latest.resize(capacity as usize, SlotObservation::default());
        true
    }

    pub fn buffer(&self) -> Option<&wgpu::Buffer> {
        self.buf.as_ref()
    }

    /// This frame's (group index, group count) — the pair the shader reads out of `renscale`'s
    /// spare lanes to decide whether a given slot writes at all. `groups == 0` means off.
    pub fn lane(&self) -> (u32, u32) {
        if self.enabled && self.buf.is_some() {
            (self.group, self.groups)
        } else {
            (0, 0)
        }
    }

    /// Step the rotation. Called once per frame BEFORE `lane()` is read into the camera
    /// uniform — the group the shader writes and the group the CPU believes it asked for have
    /// to be the same number, and the camera upload happens well before the frame's encoder
    /// exists, so the advance cannot live in `begin_frame`.
    pub fn advance(&mut self) {
        self.frame = self.frame.wrapping_add(1);
        if self.enabled {
            self.group = (self.group + 1) % self.groups;
        }
    }

    /// Zero the per-slot region. MUST be recorded before the frame's first object draw. The
    /// header is left alone — nothing on the GPU writes it, and the shader reads its copy of
    /// the rotation pair out of the frame uniform instead.
    pub fn begin_frame(&mut self, encoder: &mut wgpu::CommandEncoder) {
        if !self.enabled {
            return;
        }
        if let Some(buf) = self.buf.as_ref() {
            encoder.clear_buffer(buf, HEADER_BYTES, None);
        }
    }

    /// Copy the frame's codes into a free readback slot, after the last object draw and before
    /// submit. Skips silently when the ring is saturated — dropping a sample is correct here;
    /// stalling to collect one would distort the frame it is measuring.
    pub fn resolve(&mut self, encoder: &mut wgpu::CommandEncoder) {
        if !self.enabled {
            return;
        }
        let Some(src) = self.buf.as_ref() else {
            return;
        };
        let size = src.size();
        let Some((dst, state)) = self.readback.iter_mut().find(|(_, s)| matches!(s, Slot::Idle))
        else {
            return;
        };
        encoder.copy_buffer_to_buffer(src, 0, dst, 0, size);
        *state = Slot::Pending;
    }

    /// Kick map_async on freshly copied slots and drain completed ones (non-blocking). Called
    /// once per frame after queue.submit, alongside the other readback harvests.
    pub fn harvest(&mut self, device: &wgpu::Device) {
        if !self.enabled {
            return;
        }
        for (buf, state) in &mut self.readback {
            if matches!(state, Slot::Pending) {
                let (tx, rx) = mpsc::channel();
                buf.slice(..).map_async(wgpu::MapMode::Read, move |r| {
                    let _ = tx.send(r);
                });
                *state = Slot::InFlight(rx);
            }
        }
        let _ = device.poll(wgpu::PollType::Poll);
        let frame = self.frame;
        let mut decoded = false;
        for (buf, state) in &mut self.readback {
            let Slot::InFlight(rx) = state else {
                continue;
            };
            match rx.try_recv() {
                Ok(Ok(())) => {
                    {
                        let data = buf.slice(..).get_mapped_range();
                        let words = data.len() / 4;
                        for i in HEADER_WORDS as usize..words {
                            let code = u32::from_le_bytes(
                                data[i * 4..i * 4 + 4].try_into().unwrap_or([0; 4]),
                            );
                            if code == 0 {
                                // Not sampled in this window. KEEP the previous observation —
                                // the rotation means most slots are silent on most frames, and
                                // overwriting with "nothing" here would turn a sparse sampler
                                // into a signal that is zero nearly always.
                                continue;
                            }
                            let slot = i - HEADER_WORDS as usize;
                            if let Some(obs) = self.latest.get_mut(slot) {
                                obs.desired_mip = MIP_CODE_BASE.saturating_sub(code.min(MIP_CODE_BASE));
                                obs.frame = frame;
                            }
                        }
                    }
                    buf.unmap();
                    *state = Slot::Idle;
                    decoded = true;
                }
                Ok(Err(_)) | Err(mpsc::TryRecvError::Disconnected) => {
                    // Mapping failed (device loss etc.) — recycle the slot, keep last values.
                    *state = Slot::Idle;
                }
                Err(mpsc::TryRecvError::Empty) => {}
            }
        }
        if decoded {
            self.harvests += 1;
        }
    }

    /// Slots the harvested table covers.
    pub fn capacity(&self) -> u32 {
        self.capacity
    }

    pub fn frame(&self) -> u64 {
        self.frame
    }

    pub fn harvest_count(&self) -> u64 {
        self.harvests
    }

    pub fn observation(&self, slot: u32) -> SlotObservation {
        self.latest
            .get(slot as usize)
            .copied()
            .unwrap_or_default()
    }
}
