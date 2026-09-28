//! Private-fixture, bounded bookkeeping for a future exact GPU cull reference readback.
//!
//! No shader, GPU copy, renderer getter, or retirement policy calls this yet. A zero may be
//! published only from a successful readback of an actual view's target-model COUNT word.
//! In particular, an unrecorded view, cached image without an earlier sample, failed frame,
//! dropped copy, or stale readback remains Unknown. The caller must supply the *complete*
//! required-view set and a monotonic cache generation from actual pass publication; a
//! planning hash or requested draw is not a cache generation.

use std::mem::size_of;

pub const VERSION: u32 = 1;
pub const VIEWS: usize = 36; // main, 4 cascades, 24 locals, 5 interior, GI, reflection
pub const MAIN: usize = 0;
pub const CASCADES: std::ops::Range<usize> = 1..5;
pub const LOCALS: std::ops::Range<usize> = 5..29;
pub const INTERIOR: std::ops::Range<usize> = 29..34;
pub const GI: usize = 34;
pub const REFLECTION: usize = 35;
pub const PENDING: usize = 3;
pub const VIEW_MASK: u64 = (1u64 << VIEWS) - 1;

/// Proposed copied ABI record. Not exported until an actual readback producer is wired.
/// `status=0` means no successful returned frame; `1` is a complete input observation;
/// `2` has at least one Unknown. No status means GPU/device completion or safe mesh release.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct WgrViewReferenceFacts {
    pub version: u32,
    pub struct_bytes: u32,
    pub status: u32,
    pub view_count: u32,
    pub render_token: u64,
    pub instance_epoch: u64,
    pub required: u64,
    pub present: u64,
    pub absent: u64,
    pub unknown: u64,
    pub cached: u64,
    pub pending_samples: u32,
    pub dropped_samples: u32,
}

pub fn valid_layout(bytes: u32, version: u32) -> bool {
    bytes == size_of::<WgrViewReferenceFacts>() as u32 && version == VERSION
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
struct Record {
    generation: u64,
    epoch: u64,
    sampled_token: u64,
    valid: bool,
    present: bool,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
struct Pending {
    nonce: u64,
    token: u64,
    epoch: u64,
    generation: u64,
    view: u8,
    submitted: bool,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Ticket {
    slot: u8,
    nonce: u64,
}

/// A fixed-size state machine. The future GPU layer must never hold these tickets as pointers.
/// There are no heap allocations, scans of scene objects, sleeps or GPU calls here.
pub struct ViewReferenceFacts {
    token: u64,
    epoch: u64,
    required: u64,
    generations: [u64; VIEWS],
    records: [Record; VIEWS],
    pending: [Option<Pending>; PENDING],
    next_nonce: u64,
    committed: bool,
    dropped: u32,
}

impl Default for ViewReferenceFacts {
    fn default() -> Self {
        Self {
            token: 0,
            epoch: 0,
            required: 0,
            generations: [0; VIEWS],
            records: [Record::default(); VIEWS],
            pending: [None; PENDING],
            next_nonce: 1,
            committed: false,
            dropped: 0,
        }
    }
}

impl ViewReferenceFacts {
    /// Called at the start of a real render attempt. `refresh` invalidates an old cached view
    /// before its replacement is attempted, even when the instance epoch did not change.
    /// Generation zero refuses a cached-view proof. Caller must use an exact monotonic actual
    /// pass publication generation, never a hash of view/light/caster planning inputs.
    pub fn begin(&mut self, token: u64, epoch: u64, required: u64, refresh: u64,
                 generations: [u64; VIEWS]) -> bool {
        if token == 0 || token == u64::MAX || token <= self.token || epoch == 0 ||
            epoch == u64::MAX || required & !VIEW_MASK != 0 || refresh & !required != 0 {
            self.committed = false;
            self.required = 0;
            self.records = [Record::default(); VIEWS];
            self.pending = [None; PENDING];
            return false;
        }
        self.token = token;
        self.epoch = epoch;
        self.required = required;
        self.generations = generations;
        self.committed = false;
        for view in 0..VIEWS {
            let bit = 1u64 << view;
            let generation = generations[view];
            if refresh & bit != 0 || generation == 0 || self.records[view].epoch != epoch ||
                self.records[view].generation != generation {
                self.records[view].valid = false;
            }
            // A delayed committed copy remains eligible across a cache hit. Only an actual
            // refresh, missing generation or changed epoch/generation invalidates its ticket.
            for slot in &mut self.pending {
                if slot.as_ref().is_some_and(|p| p.view as usize == view &&
                    (refresh & bit != 0 || generation == 0 || p.epoch != epoch ||
                     p.generation != generation)) { *slot = None; }
            }
        }
        true
    }

    /// Reserve one actual per-view COUNT copy. No free slot, duplicate pending view, or nonce
    /// exhaustion leaves that view Unknown. A queued copy is not yet a returned-frame sample.
    pub fn queue_copy(&mut self, view: usize) -> Option<Ticket> {
        if view >= VIEWS || self.required & (1u64 << view) == 0 ||
            self.generations[view] == 0 || self.token == 0 || self.committed ||
            self.pending.iter().flatten().any(|p| p.view as usize == view) {
            return None;
        }
        let Some(slot) = self.pending.iter().position(Option::is_none) else {
            self.dropped = self.dropped.saturating_add(1);
            self.records[view].valid = false;
            return None;
        };
        let nonce = self.next_nonce;
        if nonce == 0 || nonce == u64::MAX {
            self.dropped = self.dropped.saturating_add(1);
            self.records[view].valid = false;
            return None;
        }
        self.next_nonce += 1;
        self.records[view].valid = false;
        self.pending[slot] = Some(Pending { nonce, token: self.token, epoch: self.epoch,
            generation: self.generations[view], view: view as u8, submitted: false });
        Some(Ticket { slot: slot as u8, nonce })
    }

    /// Mark copies from this render call as submitted only after that call returned successfully.
    pub fn commit(&mut self, token: u64) -> bool {
        if token != self.token || self.committed { return false; }
        for p in self.pending.iter_mut().flatten() {
            if p.token == token { p.submitted = true; }
        }
        self.committed = true;
        true
    }

    /// A failed frame cannot publish any newly queued sample. Previously committed cached
    /// records remain internal but snapshot() refuses a result for the aborted current token.
    pub fn abort(&mut self, token: u64) {
        if token != self.token { return; }
        self.committed = false;
        for record in &mut self.records {
            if record.sampled_token == token { record.valid = false; }
        }
        for slot in &mut self.pending {
            if slot.as_ref().is_some_and(|p| p.token == token) { *slot = None; }
        }
    }

    /// Called on nonblocking readback completion. `None` is a map/copy failure; `Some(false)`
    /// is an actual zero COUNT bit. A late sample is accepted only if its exact view generation
    /// and instance epoch are still current and its render call was committed.
    pub fn resolve(&mut self, ticket: Ticket, present: Option<bool>) -> bool {
        let Some(slot) = self.pending.get_mut(ticket.slot as usize) else { return false; };
        let Some(p) = *slot else { return false; };
        if p.nonce != ticket.nonce { return false; }
        *slot = None;
        let view = p.view as usize;
        if !p.submitted || self.epoch != p.epoch ||
            self.generations[view] != p.generation ||
            self.required & (1u64 << view) == 0 { return false; }
        let Some(present) = present else { return false; };
        self.records[view] = Record { generation: p.generation, epoch: p.epoch,
            sampled_token: p.token, valid: true, present };
        true
    }

    pub fn snapshot(&self) -> WgrViewReferenceFacts {
        let mut out = WgrViewReferenceFacts { version: VERSION,
            struct_bytes: size_of::<WgrViewReferenceFacts>() as u32,
            view_count: VIEWS as u32, render_token: self.token, instance_epoch: self.epoch,
            required: self.required, dropped_samples: self.dropped, ..Default::default() };
        out.pending_samples = self.pending.iter().flatten().count() as u32;
        if !self.committed { out.unknown = self.required; return out; }
        for view in 0..VIEWS {
            let bit = 1u64 << view;
            if self.required & bit == 0 { continue; }
            let r = self.records[view];
            if !r.valid || r.epoch != self.epoch || r.generation != self.generations[view] ||
                r.sampled_token > self.token {
                out.unknown |= bit;
            } else {
                if r.present { out.present |= bit; } else { out.absent |= bit; }
                if r.sampled_token < self.token { out.cached |= bit; }
            }
        }
        out.status = if out.unknown == 0 { 1 } else { 2 };
        out
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::mem::offset_of;

    fn generations() -> [u64; VIEWS] { [1; VIEWS] }

    #[test]
    fn proposed_copied_layout_requires_exact_size_and_version() {
        assert_eq!(size_of::<WgrViewReferenceFacts>(), 80);
        assert_eq!(offset_of!(WgrViewReferenceFacts, render_token), 16);
        assert_eq!(offset_of!(WgrViewReferenceFacts, required), 32);
        assert_eq!(offset_of!(WgrViewReferenceFacts, pending_samples), 72);
        assert!(valid_layout(80, 1));
        assert!(!valid_layout(72, 1));
        assert!(!valid_layout(80, 0));
        assert!(!valid_layout(88, 1));
    }

    #[test]
    fn actual_zero_requires_committed_copy_and_cached_generation() {
        let mut facts = ViewReferenceFacts::default();
        assert!(facts.begin(10, 4, 1, 1, generations()));
        let ticket = facts.queue_copy(0).unwrap();
        assert_eq!(facts.snapshot().unknown, 1);
        assert!(!facts.resolve(ticket, Some(false)), "unsubmitted sample cannot prove absence");
        let ticket = facts.queue_copy(0).unwrap();
        assert!(facts.commit(10));
        assert_eq!(facts.snapshot().unknown, 1);
        assert!(facts.resolve(ticket, Some(false)));
        assert_eq!(facts.snapshot().absent, 1);
        assert_eq!(facts.snapshot().status, 1);
        assert!(facts.begin(11, 4, 1, 0, generations()));
        assert!(facts.commit(11));
        let cached = facts.snapshot();
        assert_eq!(cached.absent, 1);
        assert_eq!(cached.cached, 1);
        let mut changed = generations(); changed[0] = 2;
        assert!(facts.begin(12, 4, 1, 1, changed));
        assert!(facts.commit(12));
        assert_eq!(facts.snapshot().unknown, 1);
        assert_eq!(facts.snapshot().absent, 0);
    }

    #[test]
    fn stale_aba_failed_map_and_abort_never_publish_zero() {
        let mut facts = ViewReferenceFacts::default();
        assert!(facts.begin(1, 1, 1, 1, generations()));
        let old = facts.queue_copy(0).unwrap();
        assert!(facts.commit(1));
        assert!(facts.begin(2, 2, 1, 1, generations()));
        assert!(!facts.resolve(old, Some(false)));
        let current = facts.queue_copy(0).unwrap();
        assert!(facts.commit(2));
        assert!(!facts.resolve(old, Some(false)));
        assert!(!facts.resolve(current, None));
        assert_eq!(facts.snapshot().unknown, 1);
        assert!(facts.begin(3, 2, 1, 1, generations()));
        let failed = facts.queue_copy(0).unwrap();
        facts.abort(3);
        assert!(!facts.resolve(failed, Some(false)));
        assert_eq!(facts.snapshot().status, 0);
        assert_eq!(facts.snapshot().unknown, 1);
    }

    #[test]
    fn three_slots_and_complete_36_view_mask_never_invent_absence() {
        let mut facts = ViewReferenceFacts::default();
        assert!(facts.begin(1, 1, VIEW_MASK, VIEW_MASK, generations()));
        let a = facts.queue_copy(0).unwrap();
        let b = facts.queue_copy(1).unwrap();
        let c = facts.queue_copy(35).unwrap();
        assert!(facts.queue_copy(2).is_none());
        assert!(facts.commit(1));
        assert!(facts.resolve(a, Some(true)));
        assert!(facts.resolve(b, Some(false)));
        assert!(facts.resolve(c, Some(false)));
        let s = facts.snapshot();
        assert_eq!(s.present, 1);
        assert_eq!(s.absent, (1u64 << 1) | (1u64 << 35));
        assert_eq!(s.unknown.count_ones(), 33);
        assert_eq!(s.dropped_samples, 1);
        assert_eq!(s.status, 2);
    }

    #[test]
    fn late_committed_sample_can_validate_same_cached_generation_only() {
        let mut facts = ViewReferenceFacts::default();
        assert!(facts.begin(1, 8, 1, 1, generations()));
        let late = facts.queue_copy(0).unwrap();
        assert!(facts.commit(1));
        assert!(facts.begin(2, 8, 1, 0, generations()));
        assert!(facts.commit(2));
        assert_eq!(facts.snapshot().unknown, 1);
        assert!(facts.resolve(late, Some(false)));
        assert_eq!(facts.snapshot().cached, 1);
        assert_eq!(facts.snapshot().absent, 1);
        assert!(facts.begin(3, 8, 1, 1, generations()));
        assert!(facts.commit(3));
        assert_eq!(facts.snapshot().unknown, 1, "same-epoch refresh invalidates cached image");
    }

    #[test]
    fn invalid_new_frame_discards_previous_absence() {
        let mut facts = ViewReferenceFacts::default();
        assert!(facts.begin(10, 2, 1, 1, generations()));
        let sample = facts.queue_copy(0).unwrap();
        assert!(facts.commit(10));
        assert!(facts.resolve(sample, Some(false)));
        assert_eq!(facts.snapshot().absent, 1);
        assert!(!facts.begin(11, 2, 1, 2, generations()));
        assert_eq!(facts.snapshot().status, 0);
        assert_eq!(facts.snapshot().absent, 0);
        assert!(facts.begin(12, 2, 1, 0, generations()));
        assert!(facts.commit(12));
        assert_eq!(facts.snapshot().unknown, 1);
    }
    #[test]
    fn invalid_tokens_masks_and_generation_zero_refuse_evidence() {
        let mut facts = ViewReferenceFacts::default();
        assert!(!facts.begin(0, 1, 1, 1, generations()));
        assert!(!facts.begin(1, 0, 1, 1, generations()));
        assert!(!facts.begin(1, 1, VIEW_MASK | (1u64 << 36), 0, generations()));
        assert!(!facts.begin(1, 1, 1, 2, generations()));
        let mut zero = generations(); zero[0] = 0;
        assert!(facts.begin(1, 1, 1, 0, zero));
        assert!(facts.queue_copy(0).is_none());
        assert!(facts.commit(1));
        assert_eq!(facts.snapshot().unknown, 1);
        assert!(!facts.begin(1, 1, 1, 0, generations()));
        assert!(!facts.begin(u64::MAX, 1, 1, 0, generations()));
    }
}