//! Explicit bounded retained-cull demand samples, never a paging/ownership proof.
use crate::ffi::{WgrLodDemandReport, WgrLodDemandRow};
use std::sync::mpsc;
use std::borrow::Cow;

pub(super) const TARGETS: usize = 8;
pub(super) const DISPATCHES: usize = 64;
pub(super) const WORDS: u64 = TARGETS as u64;
const FRAME_BYTES: u64 = DISPATCHES as u64 * WORDS * 4;
const SOURCE_MARKER: &str = "fn count_sections(idx: u32, level: u32) {";
const INSTRUMENTED_COUNT: &str = "fn count_sections(idx: u32, level: u32) {\n\
    let demand_slot = models[instances[idx].model]._pad;\n\
    let selected = lods[models[instances[idx].model].lod_base + level];\n\
    if (demand_slot > 0u && demand_slot <= 8u && level < 32u && selected.section_count > 0u) {\n\
        atomicOr(&counters[28u + demand_slot - 1u], 1u << level);\n\
    }";

// OFF returns the exact original borrowed WGSL, with no composer/preprocessing
// or source allocation. ON refuses a changed/missing/ambiguous injection seam.
pub(super) fn shader_source(base: &'static str, enabled: bool) -> Result<Cow<'static, str>, &'static str> {
    if !enabled { return Ok(Cow::Borrowed(base)); }
    if base.matches(SOURCE_MARKER).count() != 1 { return Err("LOD diagnostic COUNT injection marker must occur exactly once"); }
    Ok(Cow::Owned(base.replacen(SOURCE_MARKER, INSTRUMENTED_COUNT, 1)))
}

pub(super) fn enabled() -> bool {
    static ENABLED: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *ENABLED.get_or_init(|| std::env::var("WGR_GEOMETRY_LOD_FEEDBACK").as_deref() == Ok("1"))
}
pub(super) fn validate_request(epoch: u64, ids: &[u32], frames: u32, previous: Option<(u64, u32)>) -> u32 {
    if ids.is_empty() || ids.len() > TARGETS || frames == 0 || frames > 64 || epoch == 0 ||
        ids.iter().enumerate().any(|(i, id)| *id == u32::MAX || ids[..i].contains(id)) { return 2; }
    if let Some((old_epoch, status)) = previous {
        if status == 1 { return 3; }
        if epoch <= old_epoch { return 2; }
    }
    1
}

#[derive(Clone, Copy, Default)]
struct Sample { epoch: u64, frame: u64, dispatches: usize, passes: u32 }
enum SlotState { Idle, Capturing(Sample), Pending(Sample), Mapping(Sample, mpsc::Receiver<Result<(), wgpu::BufferAsyncError>>) }
struct Slot { buffer: wgpu::Buffer, state: SlotState }

pub(super) struct LodFeedback {
    pub report: WgrLodDemandReport,
    slots: Vec<Slot>,
    capturing: Option<usize>,
}

impl LodFeedback {
    pub fn new(device: &wgpu::Device) -> Self {
        Self { report: WgrLodDemandReport::default(), capturing: None,
            slots: (0..3).map(|_| Slot { buffer: device.create_buffer(&wgpu::BufferDescriptor {
                label: Some("wgr_lod_demand_readback"), size: FRAME_BYTES,
                usage: wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::MAP_READ,
                mapped_at_creation: false,
            }), state: SlotState::Idle }).collect() }
    }
    pub fn start(&mut self, epoch: u64, frames: u32, rows: &[WgrLodDemandRow]) {
        self.capturing = None;
        for slot in &mut self.slots { free_unsent(&mut slot.state); }
        self.report = WgrLodDemandReport { epoch, status: 1, frames_requested: frames,
            row_count: rows.len() as u32, ..Default::default() };
        self.report.rows[..rows.len()].copy_from_slice(rows);
        // Old mappings keep their buffers alive and are drained/discarded by epoch.
    }
    pub fn cancel(&mut self, epoch: u64) {
        if self.report.epoch == epoch {
            self.report.status = 3; self.capturing = None;
            for slot in &mut self.slots { free_unsent(&mut slot.state); }
        }
    }
    pub fn begin(&mut self) {
        // A frame that exited before submit/resolve did not yield a sample.
        if let Some(index) = self.capturing.take() {
            self.slots[index].state = SlotState::Idle; self.report.dropped_frames += 1;
        }
        self.report.current_frame = self.report.current_frame.saturating_add(1);
        if self.report.status != 1 || self.report.frames_attempted >= self.report.frames_requested { return; }
        self.report.frames_attempted += 1;
        let Some(index) = select_slot(self.slots.iter().map(|s| matches!(s.state, SlotState::Idle))) else {
            self.report.dropped_frames += 1; return;
        };
        self.slots[index].state = SlotState::Capturing(Sample { epoch: self.report.epoch,
            frame: self.report.current_frame, ..Default::default() });
        self.capturing = Some(index);
    }
    pub fn capture(&mut self, encoder: &mut wgpu::CommandEncoder, counters: &wgpu::Buffer, offset: u64, pass: u32) {
        let Some(index) = self.capturing else { return; };
        let slot = &mut self.slots[index];
        let SlotState::Capturing(ref mut sample) = slot.state else { return; };
        if sample.dispatches == DISPATCHES { self.report.dropped_dispatches += 1; return; }
        encoder.copy_buffer_to_buffer(counters, offset, &slot.buffer,
            sample.dispatches as u64 * WORDS * 4, WORDS * 4);
        sample.dispatches += 1; sample.passes |= pass;
    }
    pub fn resolve(&mut self) {
        if let Some(index) = self.capturing.take() {
            if let SlotState::Capturing(sample) = self.slots[index].state {
                self.slots[index].state = SlotState::Pending(sample);
            }
        }
    }
    pub fn retire(&mut self, model: u32) {
        for row in &mut self.report.rows[..self.report.row_count as usize] {
            if row.model_id == model { row.state = 2; row.lod_mask = 0; }
        }
    }
    pub fn harvest(&mut self, device: &wgpu::Device) {
        for slot in &mut self.slots {
            if let SlotState::Pending(sample) = slot.state {
                let (tx, rx) = mpsc::channel();
                slot.buffer.slice(..).map_async(wgpu::MapMode::Read, move |r| { let _ = tx.send(r); });
                slot.state = SlotState::Mapping(sample, rx);
            }
        }
        // Poll only; never wait for the diagnostic or use a synchronous readback.
        if self.slots.iter().any(|s| matches!(s.state, SlotState::Mapping(_, _))) {
            let _ = device.poll(wgpu::PollType::Poll);
        }
        for slot in &mut self.slots {
            let SlotState::Mapping(sample, ref rx) = slot.state else { continue; };
            match rx.try_recv() {
                Ok(Ok(())) => {
                    { let bytes = slot.buffer.slice(..).get_mapped_range();
                      accept_sample(&mut self.report, sample.epoch, sample.frame, sample.dispatches,
                          sample.passes, |i| u32::from_le_bytes(bytes[i * 4..i * 4 + 4].try_into().unwrap())); }
                    slot.buffer.unmap(); slot.state = SlotState::Idle;
                }
                Ok(Err(_)) | Err(mpsc::TryRecvError::Disconnected) => {
                    if sample.epoch == self.report.epoch { self.report.map_failures += 1; }
                    slot.buffer.unmap(); slot.state = SlotState::Idle;
                }
                Err(mpsc::TryRecvError::Empty) => {}
            }
        }
        if self.report.status == 1 && self.report.frames_attempted == self.report.frames_requested &&
            !self.slots.iter().any(|s| match &s.state {
                SlotState::Capturing(a) | SlotState::Pending(a) | SlotState::Mapping(a, _) => a.epoch == self.report.epoch,
                SlotState::Idle => false,
            }) { self.report.status = 2; }
    }
}

fn select_slot(free: impl Iterator<Item = bool>) -> Option<usize> { free.take(3).position(|idle| idle) }
fn free_unsent(state: &mut SlotState) { if matches!(state, SlotState::Capturing(_)) { *state = SlotState::Idle; } }

fn accept_sample(report: &mut WgrLodDemandReport, epoch: u64, frame: u64,
    dispatches: usize, passes: u32, mut word: impl FnMut(usize) -> u32) {
    if report.epoch != epoch || report.status != 1 { return; }
    report.frames_sampled += 1; report.last_sample_frame = report.last_sample_frame.max(frame);
    report.dispatched_views += dispatches as u32; report.pass_mask |= passes;
    for d in 0..dispatches {
        for (i, row) in report.rows[..report.row_count as usize].iter_mut().enumerate() {
            if row.state == 0 { row.lod_mask |= word(d * TARGETS + i); }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn report() -> WgrLodDemandReport { WgrLodDemandReport { epoch: 7, status: 1, row_count: 1,
        rows: [WgrLodDemandRow { model_id: 42, lod_count: 8, ..Default::default() }; 8], ..Default::default() } }
    #[test] fn all_dispatched_views_union_without_last_view_overwrite() {
        let mut r = report();
        accept_sample(&mut r, 7, 1, 3, 1 | 4 | 8, |i| match i { 0 => 1, 8 => 4, 16 => 128, _ => 0 });
        assert_eq!(r.rows[0].lod_mask, 133); assert_eq!(r.pass_mask, 13); assert_eq!(r.dispatched_views, 3);
    }
    #[test] fn stale_epochs_cancelled_and_retired_rows_do_not_gain_demand() {
        let mut r = report(); accept_sample(&mut r, 6, 1, 1, 1, |_| u32::MAX);
        assert_eq!(r.frames_sampled, 0);
        r.rows[0].state = 2; accept_sample(&mut r, 7, 2, 1, 2, |_| u32::MAX);
        assert_eq!(r.rows[0].lod_mask, 0);
        r.status = 3; accept_sample(&mut r, 7, 3, 1, 1, |_| u32::MAX);
        assert_eq!(r.frames_sampled, 1);
    }
    #[test] fn bounded_request_and_ring_storage_contract() {
        assert_eq!(FRAME_BYTES, 2048); assert_eq!(3 * FRAME_BYTES, 6144);
        assert!(TARGETS <= 8 && DISPATCHES <= 64);
    }
    #[test] fn saturated_ring_refuses_sample_instead_of_reusing_in_flight_storage() {
        assert_eq!(select_slot([false, false, false].into_iter()), None);
        assert_eq!(select_slot([false, true, false].into_iter()), Some(1));
        assert_eq!(select_slot([false, false, false, true].into_iter()), None);
    }
    #[test] fn repeated_cancel_or_reset_releases_only_unsent_slots() {
        let mut slots = [SlotState::Capturing(Sample::default()), SlotState::Capturing(Sample::default()), SlotState::Capturing(Sample::default())];
        for _ in 0..3 { for slot in &mut slots { free_unsent(slot); } }
        assert_eq!(select_slot(slots.iter().map(|s| matches!(s, SlotState::Idle))), Some(0));
        let mut submitted = SlotState::Pending(Sample { epoch: 6, ..Default::default() });
        free_unsent(&mut submitted); assert!(matches!(submitted, SlotState::Pending(_)));
        let (_, rx) = mpsc::channel(); let mut mapped = SlotState::Mapping(Sample::default(), rx);
        free_unsent(&mut mapped); assert!(matches!(mapped, SlotState::Mapping(_, _)));
    }
    #[test] fn request_caps_identity_and_busy_are_checked_before_allocating_a_ring() {
        assert_eq!(validate_request(1, &[0, 42], 64, None), 1);
        assert_eq!(validate_request(1, &[42, 42], 1, None), 2);
        assert_eq!(validate_request(1, &[42; 9], 1, None), 2);
        assert_eq!(validate_request(0, &[42], 1, None), 2);
        assert_eq!(validate_request(1, &[u32::MAX], 1, None), 2);
        assert_eq!(validate_request(1, &[42], 65, None), 2);
        assert_eq!(validate_request(2, &[42], 1, Some((1, 1))), 3);
        assert_eq!(validate_request(2, &[42], 1, Some((1, 3))), 1);
        assert_eq!(validate_request(1, &[42], 1, Some((1, 3))), 2);
    }
    #[test] fn off_shader_is_exact_borrowed_base_and_on_marker_must_be_unique() {
        let base = include_str!("cull.wgsl");
        let off = shader_source(base, false).unwrap();
        assert!(matches!(off, Cow::Borrowed(_))); assert_eq!(off.as_ref(), base);
        assert!(!off.contains("atomicOr"));
        assert!(shader_source(base, true).unwrap().contains("atomicOr"));
        assert!(shader_source("missing marker", true).is_err());
        assert!(shader_source("fn count_sections(idx: u32, level: u32) {\nfn count_sections(idx: u32, level: u32) {", true).is_err());
    }
}
