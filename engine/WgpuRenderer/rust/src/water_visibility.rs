//! One-frame-late visible-water coverage from an occlusion query around the real water draw.
//!
//! The CPU water quadtree covers the whole world square, including sea hidden below terrain.
//! Its projected bounds are therefore deliberately conservative but can report 100% water while
//! a land-facing camera contains no water pixels at all. This query measures samples that actually
//! pass the main scene depth test. The planar reflection uses the latest asynchronous result; the
//! query still runs while the reflection is skipped, so turning back toward water re-enables it.

use std::cell::Cell;
use std::sync::mpsc;

const RING_SLOTS: usize = 3;
const QUERY_BYTES: u64 = wgpu::QUERY_SIZE as u64;
// Query resolve offsets/buffers are conservatively aligned for every backend.
const RESOLVE_BYTES: u64 = 256;

enum SlotState {
    Idle,
    Pending,
    InFlight(mpsc::Receiver<Result<(), wgpu::BufferAsyncError>>),
}

struct Slot {
    buffer: wgpu::Buffer,
    state: SlotState,
}

pub struct WaterVisibility {
    query_set: wgpu::QuerySet,
    resolve: wgpu::Buffer,
    slots: Vec<Slot>,
    written: Cell<bool>,
    latest_samples: Option<u64>,
}

impl WaterVisibility {
    pub fn new(device: &wgpu::Device) -> Self {
        let query_set = device.create_query_set(&wgpu::QuerySetDescriptor {
            label: Some("wgr_water_visibility"),
            ty: wgpu::QueryType::Occlusion,
            count: 1,
        });
        let resolve = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("wgr_water_visibility_resolve"),
            size: RESOLVE_BYTES,
            usage: wgpu::BufferUsages::QUERY_RESOLVE | wgpu::BufferUsages::COPY_SRC,
            mapped_at_creation: false,
        });
        let slots = (0..RING_SLOTS)
            .map(|i| Slot {
                buffer: device.create_buffer(&wgpu::BufferDescriptor {
                    label: Some(&format!("wgr_water_visibility_readback_{i}")),
                    size: QUERY_BYTES,
                    usage: wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::MAP_READ,
                    mapped_at_creation: false,
                }),
                state: SlotState::Idle,
            })
            .collect();
        Self {
            query_set,
            resolve,
            slots,
            written: Cell::new(false),
            latest_samples: None,
        }
    }

    pub fn begin_frame(&self) {
        self.written.set(false);
    }

    pub fn query_set(&self) -> &wgpu::QuerySet {
        &self.query_set
    }

    /// Only one query index exists, so at most one water segment may claim it per frame.
    pub fn claim(&self) -> bool {
        if self.written.replace(true) {
            false
        } else {
            true
        }
    }

    pub fn resolve(&mut self, encoder: &mut wgpu::CommandEncoder) {
        if !self.written.get() {
            return;
        }
        let Some(slot) = self
            .slots
            .iter_mut()
            .find(|slot| matches!(slot.state, SlotState::Idle))
        else {
            return;
        };
        encoder.resolve_query_set(&self.query_set, 0..1, &self.resolve, 0);
        encoder.copy_buffer_to_buffer(&self.resolve, 0, &slot.buffer, 0, QUERY_BYTES);
        slot.state = SlotState::Pending;
    }

    pub fn harvest(&mut self, device: &wgpu::Device) {
        for slot in &mut self.slots {
            if matches!(slot.state, SlotState::Pending) {
                let (tx, rx) = mpsc::channel();
                slot.buffer
                    .slice(..)
                    .map_async(wgpu::MapMode::Read, move |result| {
                        let _ = tx.send(result);
                    });
                slot.state = SlotState::InFlight(rx);
            }
        }
        let _ = device.poll(wgpu::PollType::Poll);
        for slot in &mut self.slots {
            let SlotState::InFlight(rx) = &slot.state else {
                continue;
            };
            match rx.try_recv() {
                Ok(Ok(())) => {
                    {
                        let bytes = slot.buffer.slice(..).get_mapped_range();
                        self.latest_samples = Some(u64::from_le_bytes(
                            bytes[..8].try_into().expect("one occlusion query"),
                        ));
                    }
                    slot.buffer.unmap();
                    slot.state = SlotState::Idle;
                }
                Ok(Err(_)) | Err(mpsc::TryRecvError::Disconnected) => {
                    slot.state = SlotState::Idle;
                }
                Err(mpsc::TryRecvError::Empty) => {}
            }
        }
    }

    pub fn coverage(&self, width: u32, height: u32, samples: u32) -> Option<f32> {
        self.latest_samples
            .map(|visible| coverage_from_samples(visible, width, height, samples))
    }
}

fn coverage_from_samples(visible: u64, width: u32, height: u32, samples: u32) -> f32 {
    let possible = u64::from(width.max(1))
        .saturating_mul(u64::from(height.max(1)))
        .saturating_mul(u64::from(samples.max(1)));
    (visible as f64 / possible as f64).clamp(0.0, 1.0) as f32
}

#[cfg(test)]
mod tests {
    use super::coverage_from_samples;

    #[test]
    fn occlusion_samples_normalise_by_pixels_and_msaa() {
        assert_eq!(coverage_from_samples(0, 1600, 900, 4), 0.0);
        assert!((coverage_from_samples(2_880_000, 1600, 900, 4) - 0.5).abs() < 1.0e-6);
        assert_eq!(coverage_from_samples(u64::MAX, 1, 1, 1), 1.0);
    }
}
