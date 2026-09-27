//! Bounded explicit snapshot barriers. Acknowledgement covers preceding queue
//! submissions, not future references, device release, or reusable pool holes.
use crate::ffi::{WgrMeshHandleFact, WgrMeshHandleFactSummary, WgrMeshSnapshotAck};
use std::sync::{Arc, atomic::{AtomicBool, AtomicU32, Ordering}};

pub(super) const DISABLED: u32 = 0;
pub(super) const QUEUED: u32 = 1;
pub(super) const SUBMITTED: u32 = 2;
pub(super) const ACKNOWLEDGED: u32 = 3;
pub(super) const CANCELLED: u32 = 4;
pub(super) const INVALID: u32 = 5;
pub(super) const BUSY: u32 = 6;
pub(super) const UNKNOWN: u32 = 7;
const MAX_BATCHES: usize = 3;
const MAX_HANDLES: usize = 8192;

struct Batch {
    rows: Vec<WgrMeshHandleFact>,
    summary: WgrMeshHandleFactSummary,
    meta: WgrMeshSnapshotAck,
    done: Arc<AtomicBool>,
}

/// Callback owns only atomics, not renderer/ring pointers. Outstanding callback
/// capacity survives cancellation/reset. Drop releases capacity even on teardown,
/// but only complete() acknowledges queue work.
pub(super) struct CompletionSignal {
    done: Arc<AtomicBool>,
    outstanding: Arc<AtomicU32>,
}
impl CompletionSignal {
    pub(super) fn complete(self) { self.done.store(true, Ordering::Release); }
}
impl Drop for CompletionSignal {
    fn drop(&mut self) { self.outstanding.fetch_sub(1, Ordering::AcqRel); }
}

pub(super) struct MeshAck {
    epoch: u64,
    next_ticket: u64,
    main_serial: u64,
    slots: [Option<Batch>; MAX_BATCHES],
    cancelled: [u64; MAX_BATCHES],
    cancel_cursor: usize,
    outstanding: Arc<AtomicU32>,
}

impl MeshAck {
    pub(super) fn new() -> Self {
        Self { epoch: 0, next_ticket: 1, main_serial: 0,
            slots: std::array::from_fn(|_| None), cancelled: [0; MAX_BATCHES], cancel_cursor: 0,
            outstanding: Arc::new(AtomicU32::new(0)) }
    }
    pub(super) fn reset(&mut self, epoch: u64) {
        self.epoch = epoch;
        self.slots = std::array::from_fn(|_| None);
        self.cancelled = [0; MAX_BATCHES];
        // Ticket and main-submission serial never recycle across epochs. Old
        // callbacks only own their old AtomicBool, never these slots.
    }
    pub(super) fn can_request(&self, epoch: u64, count: usize) -> u32 {
        if epoch == 0 || count > MAX_HANDLES || self.next_ticket == u64::MAX || self.main_serial == u64::MAX { return INVALID; }
        if epoch == self.epoch && self.slots.iter().all(Option::is_some) { return BUSY; }
        let queued = if epoch == self.epoch {
            self.slots.iter().flatten().filter(|b| b.meta.state == QUEUED).count()
        } else { 0 };
        if queued + self.outstanding.load(Ordering::Acquire) as usize >= MAX_BATCHES { return BUSY; }
        QUEUED
    }
    pub(super) fn request(&mut self, epoch: u64, rows: Vec<WgrMeshHandleFact>,
        summary: WgrMeshHandleFactSummary, mut meta: WgrMeshSnapshotAck) -> Result<u64, u32> {
        let status = self.can_request(epoch, rows.len());
        if status != QUEUED { return Err(status); }
        if summary.complete != 1 || summary.handles_requested as usize != rows.len() ||
            summary.handles_inspected as usize != rows.len() { return Err(INVALID); }
        if epoch != self.epoch { self.reset(epoch); }
        let slot = self.slots.iter_mut().find(|slot| slot.is_none()).ok_or(BUSY)?;
        let ticket = self.next_ticket;
        self.next_ticket += 1;
        meta.ticket = ticket; meta.epoch = epoch; meta.state = QUEUED;
        meta.main_submission_serial = 0; meta.row_count = rows.len() as u32;
        *slot = Some(Batch { rows, summary, meta, done: Arc::new(AtomicBool::new(false)) });
        Ok(ticket)
    }
    pub(super) fn snapshot(&self, ticket: u64)
        -> Result<(&[WgrMeshHandleFact], WgrMeshHandleFactSummary, WgrMeshSnapshotAck), u32> {
        if ticket == 0 { return Err(INVALID); }
        if let Some(batch) = self.slots.iter().flatten().find(|b| b.meta.ticket == ticket) {
            let mut meta = batch.meta;
            // An early callback cannot acknowledge a batch not yet submitted.
            if meta.state == SUBMITTED && batch.done.load(Ordering::Acquire) { meta.state = ACKNOWLEDGED; }
            return Ok((&batch.rows, batch.summary, meta));
        }
        Err(if self.cancelled.contains(&ticket) { CANCELLED } else { UNKNOWN })
    }
    pub(super) fn cancel(&mut self, ticket: u64) -> u32 {
        if ticket == 0 { return INVALID; }
        if let Some(slot) = self.slots.iter_mut().find(|slot| slot.as_ref().is_some_and(|b| b.meta.ticket == ticket)) {
            *slot = None;
            self.cancelled[self.cancel_cursor] = ticket;
            self.cancel_cursor = (self.cancel_cursor + 1) % MAX_BATCHES;
            return CANCELLED;
        }
        if self.cancelled.contains(&ticket) { CANCELLED } else { UNKNOWN }
    }
    pub(super) fn main_submitted(&mut self, mut register: impl FnMut(CompletionSignal)) {
        let Some(serial) = self.main_serial.checked_add(1) else { return; };
        self.main_serial = serial;
        for batch in self.slots.iter_mut().flatten() {
            if batch.meta.state == QUEUED {
                if self.outstanding.fetch_update(Ordering::AcqRel, Ordering::Acquire,
                    |count| (count < MAX_BATCHES as u32).then_some(count + 1)).is_err() { continue; }
                batch.meta.main_submission_serial = serial;
                batch.meta.state = SUBMITTED;
                register(CompletionSignal { done: batch.done.clone(), outstanding: self.outstanding.clone() });
            }
        }
    }
    pub(super) fn completion_pending(&self) -> bool {
        // Cancelled/reset callbacks still need nonblocking device maintenance.
        self.outstanding.load(Ordering::Acquire) != 0
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn request(state: &mut MeshAck, epoch: u64) -> u64 {
        state.request(epoch, vec![WgrMeshHandleFact { mesh_handle: 7, state: 2, ..Default::default() }],
            WgrMeshHandleFactSummary { complete: 1, handles_requested: 1, handles_inspected: 1, absent: 1, ..Default::default() },
            WgrMeshSnapshotAck { pool_generation: 9, pool_capacity_bytes: 4096, ..Default::default() }).unwrap()
    }
    #[test]
    fn record_pool_scope_stays_immutable_across_later_cuts_and_ack() {
        let mut state=MeshAck::new();
        let original=WgrMeshHandleFactSummary { complete:1,handles_requested:1,handles_inspected:1,present:1,
            live_mesh_records:2,pool_live_bytes:288,pool_generation:7,record_scope_valid:1,..Default::default() };
        let id=state.request(1,vec![WgrMeshHandleFact {mesh_handle:7,state:1,..Default::default()}],original,
            WgrMeshSnapshotAck {pool_generation:7,pool_live_bytes:288,..Default::default()}).unwrap();
        // A fresh source cut may differ; accepted ticket retains its own copied facts.
        let later=WgrMeshHandleFactSummary {live_mesh_records:3,pool_live_bytes:432,pool_generation:8,..original};
        assert_ne!(later.pool_generation,state.snapshot(id).unwrap().1.pool_generation);
        let mut signal=None; state.main_submitted(|s| signal=Some(s)); signal.unwrap().complete();
        let (_,summary,ack)=state.snapshot(id).unwrap();
        assert_eq!((summary.live_mesh_records,summary.pool_live_bytes,summary.pool_generation),(2,288,7));
        assert_eq!((ack.pool_live_bytes,ack.pool_generation,ack.state),(288,7,ACKNOWLEDGED));
    }

    #[test]
    fn snapshot_waits_for_next_submission_and_actual_callback() {
        let mut state = MeshAck::new(); let id = request(&mut state, 1);
        assert_eq!(state.snapshot(id).unwrap().2.state, QUEUED);
        assert!(!state.completion_pending());
        let mut callback = None;
        state.main_submitted(|signal| callback = Some(signal));
        assert_eq!(state.snapshot(id).unwrap().2.state, SUBMITTED);
        assert_eq!(state.snapshot(id).unwrap().2.main_submission_serial, 1);
        assert!(state.completion_pending());
        callback.unwrap().complete();
        let (rows, summary, meta) = state.snapshot(id).unwrap();
        assert_eq!((rows[0].state, summary.absent, meta.state), (2, 1, ACKNOWLEDGED));
        assert_eq!((meta.pool_generation, meta.pool_capacity_bytes), (9, 4096));
        assert!(!state.completion_pending());
    }
    #[test]
    fn cancelled_and_reset_callbacks_cannot_ack_replacement() {
        let mut state = MeshAck::new(); let old = request(&mut state, 1); let mut late = None;
        state.main_submitted(|signal| late = Some(signal));
        assert_eq!(state.cancel(old), CANCELLED);
        let replacement = request(&mut state, 1);
        late.unwrap().complete();
        assert_eq!(state.snapshot(old).err(), Some(CANCELLED));
        assert_eq!(state.snapshot(replacement).unwrap().2.state, QUEUED);
        let mut late_reset = None; state.main_submitted(|signal| late_reset = Some(signal));
        let next_epoch = request(&mut state, 2);
        late_reset.unwrap().complete();
        assert_eq!(state.snapshot(replacement).err(), Some(UNKNOWN));
        assert_eq!(state.snapshot(next_epoch).unwrap().2.state, QUEUED);
        assert_ne!(old, replacement); assert_ne!(replacement, next_epoch);
    }
    #[test]
    fn bounded_slots_keep_completed_snapshots_until_explicit_release() {
        let mut state = MeshAck::new(); let first = request(&mut state, 1);
        request(&mut state, 1); request(&mut state, 1);
        assert_eq!(state.can_request(1, 1), BUSY);
        state.main_submitted(CompletionSignal::complete);
        assert_eq!(state.can_request(1, 1), BUSY);
        assert_eq!(state.snapshot(first).unwrap().2.state, ACKNOWLEDGED);
        assert_eq!(state.cancel(first), CANCELLED);
        assert_eq!(state.can_request(1, 1), QUEUED);
        assert_eq!(state.can_request(1, MAX_HANDLES + 1), INVALID);
        assert_eq!(state.can_request(0, 1), INVALID);
    }
    #[test]
    fn reused_real_slot_does_not_change_captured_absence() {
        use slotmap::{new_key_type, Key, KeyData, SlotMap};
        new_key_type! { struct TestMesh; }
        let mut meshes: SlotMap<TestMesh, (u64, u64)> = SlotMap::with_key();
        let old = meshes.insert((136, 12)); let id = old.data().as_ffi(); meshes.remove(old);
        let mut rows = vec![WgrMeshHandleFact::default()];
        let summary = super::super::mesh_facts::collect(&[id], &mut rows, |handle| meshes.get(KeyData::from_ffi(handle).into()).copied());
        let mut state = MeshAck::new(); let ticket = state.request(1, rows, summary, Default::default()).unwrap();
        let new = meshes.insert((272, 24)); assert_eq!(id as u32, new.data().as_ffi() as u32);
        assert_ne!(id, new.data().as_ffi());
        state.main_submitted(CompletionSignal::complete);
        assert_eq!((state.snapshot(ticket).unwrap().0[0].mesh_handle, state.snapshot(ticket).unwrap().0[0].state), (id, 2));
    }
    #[test]
    fn malformed_facts_do_not_allocate_a_ticket_or_cancel_old_epoch() {
        let mut state = MeshAck::new(); let old = request(&mut state, 1);
        assert_eq!(state.request(2, vec![WgrMeshHandleFact::default()],
            WgrMeshHandleFactSummary { complete: 0, ..Default::default() }, Default::default()).err(), Some(INVALID));
        assert_eq!(state.snapshot(old).unwrap().2.state, QUEUED);
        assert_eq!(request(&mut state, 1), old + 1);
        state.main_serial = u64::MAX;
        assert_eq!(state.can_request(1, 1), INVALID);
        state.main_serial = 0; state.next_ticket = u64::MAX;
        assert_eq!(state.can_request(1, 1), INVALID);
    }
    #[test]
    fn optional_ffi_record_has_exact_additive_size() {
        assert_eq!(std::mem::size_of::<WgrMeshSnapshotAck>(), 64);
        assert_eq!(std::mem::align_of::<WgrMeshSnapshotAck>(), 8);
    }
    #[test]
    fn cancelled_inflight_callbacks_remain_bounded_and_are_drained() {
        let mut state = MeshAck::new();
        let ids = [request(&mut state, 1), request(&mut state, 1), request(&mut state, 1)];
        let mut callbacks = Vec::new(); state.main_submitted(|signal| callbacks.push(signal));
        for id in ids { assert_eq!(state.cancel(id), CANCELLED); }
        assert!(state.completion_pending());
        assert_eq!(state.can_request(1, 1), BUSY);
        assert_eq!(state.can_request(2, 1), BUSY);
        callbacks.pop().unwrap().complete();
        let next = request(&mut state, 2);
        state.main_submitted(|signal| callbacks.push(signal));
        assert_eq!(state.outstanding.load(Ordering::Acquire), 3);
        assert_eq!(state.snapshot(next).unwrap().2.state, SUBMITTED);
        for signal in callbacks { signal.complete(); }
        assert!(!state.completion_pending());
        assert_eq!(state.snapshot(next).unwrap().2.state, ACKNOWLEDGED);
    }
    #[test]
    fn callback_drop_does_not_fabricate_work_completion() {
        let mut state = MeshAck::new(); let ticket = request(&mut state, 1);
        state.main_submitted(drop);
        assert!(!state.completion_pending());
        assert_eq!(state.snapshot(ticket).unwrap().2.state, SUBMITTED);
        assert_eq!(state.cancel(ticket), CANCELLED);
    }
}
