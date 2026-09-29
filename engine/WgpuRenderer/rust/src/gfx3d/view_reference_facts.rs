//! Bounded bookkeeping and an all-view diagnostic over existing exact COUNT facts.
//!
//! The renderer getter aggregates existing readbacks; it adds no shader or GPU copy.
//! A zero may be published only from a successful readback of an actual view's COUNT word.
//! In particular, an unrecorded view, cached image without an earlier sample, failed frame,
//! dropped copy, or stale readback remains Unknown. The caller must supply the *complete*
//! required-view set and a monotonic cache generation from actual pass publication; a
//! planning hash or requested draw is not a cache generation.

use std::mem::size_of;
use super::{cull::{MainCountFact, MainCountIdentity, MainCountStatus}, main_count_completion};

pub const VERSION: u32 = 2;
pub const VIEWS: usize = 36; // main, 4 cascades, 24 locals, 5 interior, GI, reflection
pub const MAIN: usize = 0;
pub const CASCADES: std::ops::Range<usize> = 1..5;
pub const LOCALS: std::ops::Range<usize> = 5..29;
pub const INTERIOR: std::ops::Range<usize> = 29..34;
pub const GI: usize = 34;
pub const REFLECTION: usize = 35;
pub const PENDING: usize = 3;
pub const VIEW_MASK: u64 = (1u64 << VIEWS) - 1;

/// The configured views for one exact model-scoped COUNT request. This is frozen
/// from the frame packet and feature policy before resource, cull and draw gates.
/// A missing target or skipped enabled pass therefore cannot make a view inactive.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ApplicabilityPlan {
    pub identity: MainCountIdentity,
    pub required: u64,
}

impl ApplicabilityPlan {
    pub fn from_frame(identity: MainCountIdentity, solar: u32, local: u32,
                      interior_enabled: bool, gi_rsm_eligible: bool,
                      planar_active: bool) -> Option<Self> {
        if identity.token == 0 || identity.token == u64::MAX ||
            identity.cull_epoch == 0 || identity.cull_epoch == u64::MAX ||
            identity.source_generation == 0 || identity.target_revision == 0 ||
            identity.model_id == u32::MAX ||
            solar > 4 || local > 24 { return None; }
        let mut required = 1u64 << MAIN;
        required |= ((1u64 << solar) - 1) << CASCADES.start;
        required |= ((1u64 << local) - 1) << LOCALS.start;
        if interior_enabled {
            required |= ((1u64 << (INTERIOR.end - INTERIOR.start)) - 1) << INTERIOR.start;
        }
        if gi_rsm_eligible { required |= 1u64 << GI; }
        if planar_active { required |= 1u64 << REFLECTION; }
        Some(Self { identity, required })
    }
}

/// The renderer supplies all 36 facts from one borrow. Only the publication-bound
/// interior, GI and local getters may return a fact from an earlier request.
/// Completion of the current request's final queue submission is a separate gate;
/// it does not certify pixels or mesh retirement.
pub fn aggregate_existing_counts(
    completion: main_count_completion::Fact,
    facts: &[MainCountFact; VIEWS],
    published_cached: u64,
    plan: Option<ApplicabilityPlan>,
) -> WgrViewReferenceFacts {
    let current = completion.identity;
    let mut out = WgrViewReferenceFacts {
        version: VERSION, struct_bytes: size_of::<WgrViewReferenceFacts>() as u32,
        view_count: VIEWS as u32, render_token: current.token,
        instance_epoch: current.cull_epoch, required: VIEW_MASK, unknown: VIEW_MASK,
        target_model_id: current.model_id, target_source_generation: current.source_generation,
        ..Default::default()
    };
    let Some(plan) = plan.filter(|p| p.identity == current && p.required & !VIEW_MASK == 0 &&
        p.required & (1u64 << MAIN) != 0) else { return out; };
    out.required = plan.required;
    out.unknown = plan.required;
    if completion.state != main_count_completion::COMPLETED ||
        current.token == 0 || current.cull_epoch == 0 || current.source_generation == 0 {
        return out;
    }
    for (view, fact) in facts.iter().enumerate() {
        let bit = 1u64 << view;
        if plan.required & bit == 0 { continue; }
        let older_cached = fact.identity.token < current.token && published_cached & bit != 0;
        if !same_target(fact.identity, current) ||
            !(fact.identity.token == current.token || older_cached) { continue; }
        match (fact.status, fact.count) {
            (MainCountStatus::Present, 1..) => out.present |= bit,
            (MainCountStatus::Absent, 0) => out.absent |= bit,
            _ => continue,
        }
        out.unknown &= !bit;
        if older_cached { out.cached |= bit; }
    }
    out.status = if out.unknown == 0 { 1 } else { 2 };
    out
}

fn same_target(fact: MainCountIdentity, current: MainCountIdentity) -> bool {
    fact.token != 0 && fact.model_id == current.model_id &&
        fact.source_generation == current.source_generation &&
        fact.cull_epoch == current.cull_epoch &&
        fact.target_revision != 0 && fact.target_revision == current.target_revision
}

/// Query identity. Instance handle zero means all instances of this model, required when
/// proving a whole model unused. `source_generation` is a caller-owned, nonzero generation
/// for the exact geometry source; it must change when source mesh handles/content change.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct TargetIdentity {
    pub model_id: u32,
    pub instance_handle: u32,
    pub source_generation: u64,
}

/// Copied private ABI record. The renderer getter uses the exact frozen
/// applicability mask. `status=0` means the current request's final queue
/// completion is unproven; `1` means all required COUNT facts are known;
/// `2` has at least one Unknown. Its `pending_samples` counts exact current
/// request copies still in flight and Unknown; `dropped_samples=u32::MAX`
/// means existing producer rings provide no reliable drop counter.
/// No status means pixel visibility or safe mesh release.
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
    pub target_model_id: u32,
    pub target_instance_handle: u32,
    pub target_source_generation: u64,
}

pub fn valid_layout(bytes: u32, version: u32) -> bool {
    bytes == size_of::<WgrViewReferenceFacts>() as u32 && version == VERSION
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
struct Record {
    generation: u64,
    epoch: u64,
    target: TargetIdentity,
    sampled_token: u64,
    valid: bool,
    present: bool,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct Pending {
    nonce: u64,
    token: u64,
    epoch: u64,
    target: TargetIdentity,
    generations: [u64; VIEWS],
    copied: u64,
    invalidated: u64,
    submitted: u64,
    committed: bool,
    aborted: bool,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Ticket {
    slot: u8,
    nonce: u64,
}
impl Ticket {
    /// Index of the producer-owned staging buffer. Never reuse it until resolve drains it.
    pub fn slot_index(self) -> usize { self.slot as usize }
}

/// A fixed-size state machine. The future GPU layer must never hold these tickets as pointers.
/// There are no heap allocations, scans of scene objects, sleeps or GPU calls here.
pub struct ViewReferenceFacts {
    token: u64,
    epoch: u64,
    target: TargetIdentity,
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
            target: TargetIdentity::default(),
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
    pub fn begin(&mut self, token: u64, epoch: u64, target: TargetIdentity,
                 required: u64, refresh: u64,
                 generations: [u64; VIEWS]) -> bool {
        if token == 0 || token == u64::MAX || token <= self.token || epoch == 0 ||
            epoch == u64::MAX || target.model_id == u32::MAX ||
            target.source_generation == 0 || required & !VIEW_MASK != 0 ||
            refresh & !required != 0 {
            self.committed = false;
            self.required = 0;
            self.records = [Record::default(); VIEWS];
            // Pending GPU buffers must remain occupied until their owner has drained the
            // async map/callback. Invalidating a logical ticket must not recycle storage.
            for p in self.pending.iter_mut().flatten() { p.invalidated |= p.copied; }
            return false;
        }
        self.token = token;
        self.epoch = epoch;
        self.target = target;
        let transitioned = self.required ^ required;
        self.required = required;
        self.generations = generations;
        self.committed = false;
        for p in self.pending.iter_mut().flatten() {
            if p.epoch != epoch || p.target != target {
                p.invalidated |= p.copied;
            } else {
                for view in 0..VIEWS {
                    let bit = 1u64 << view;
                    if refresh & bit != 0 || transitioned & bit != 0 ||
                        p.generations[view] != generations[view] ||
                        generations[view] == 0 {
                        p.invalidated |= bit;
                    }
                }
            }
        }
        for view in 0..VIEWS {
            let bit = 1u64 << view;
            let generation = generations[view];
            if refresh & bit != 0 || transitioned & bit != 0 || generation == 0 ||
                self.records[view].target != target ||
                self.records[view].epoch != epoch ||
                self.records[view].generation != generation {
                self.records[view].valid = false;
            }
        }
        true
    }

    /// Reserve one actual per-view COUNT copy within this render's frame slot. All 36 views
    /// may share one staging buffer; three slots bound concurrent frame readbacks, not views.
    /// The producer must copy word `view` at 4*view bytes into this ticket's staging buffer.
    pub fn queue_copy(&mut self, view: usize) -> Option<Ticket> {
        if view >= VIEWS || self.required & (1u64 << view) == 0 ||
            self.generations[view] == 0 || self.token == 0 || self.committed {
            return None;
        }
        if let Some((slot, p)) = self.pending.iter_mut().enumerate()
            .find_map(|(slot, p)| p.as_mut().filter(|p| p.token == self.token)
                .map(|p| (slot, p))) {
            if p.copied & (1u64 << view) != 0 { return None; }
            p.copied |= 1u64 << view;
            self.records[view].valid = false;
            return Some(Ticket { slot: slot as u8, nonce: p.nonce });
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
            target: self.target, generations: self.generations, copied: 1u64 << view,
            invalidated: 0,
            submitted: 0, committed: false, aborted: false });
        Some(Ticket { slot: slot as u8, nonce })
    }

    /// Mark exactly the view copies in a successfully submitted encoder. Reflection may
    /// submit before the main encoder; later queued-but-unsubmitted copies remain Unknown.
    /// Nonzero submitted bits also keep this staging buffer occupied through abort.
    pub fn mark_submitted(&mut self, token: u64, views: u64) -> bool {
        // A late submit cannot retroactively certify a committed render attempt.
        if token == 0 || token != self.token || self.committed || views == 0 { return false; }
        for p in self.pending.iter_mut().flatten() {
            if p.token == token && views & !p.copied == 0 {
                p.submitted |= views;
                return true;
            }
        }
        false
    }

    /// Accept this render call only after it returned successfully and all copied command
    /// buffers were submitted. This still does not mean GPU completion.
    pub fn commit(&mut self, token: u64) -> bool {
        if token == 0 || token != self.token || self.committed { return false; }
        for p in self.pending.iter_mut().flatten() {
            if p.token == token { p.committed = true; }
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
            if slot.as_ref().is_some_and(|p| p.token == token) {
                // An unsubmitted encoded copy cannot run. A submitted planar copy can run,
                // so keep its staging slot occupied until resolve() drains it.
                if slot.as_ref().is_some_and(|p| p.submitted != 0) {
                    slot.as_mut().unwrap().aborted = true;
                } else { *slot = None; }
            }
        }
    }

    /// Called after async map completion. `None` is a map/copy failure. `Some` contains one
    /// u32 per view; only bits in the frame's copied mask may become known, including zero.
    /// Return value means at least one copied view matched the current target/cache generation.
    pub fn resolve(&mut self, ticket: Ticket, counts: Option<&[u32; VIEWS]>) -> bool {
        let Some(slot) = self.pending.get_mut(ticket.slot as usize) else { return false; };
        let Some(p) = *slot else { return false; };
        if p.nonce != ticket.nonce { return false; }
        *slot = None;
        if p.submitted == 0 || !p.committed || p.aborted || self.epoch != p.epoch ||
            self.target != p.target { return false; }
        let Some(counts) = counts else { return false; };
        let mut accepted = false;
        for view in 0..VIEWS {
            let bit = 1u64 << view;
            if p.submitted & bit == 0 || p.invalidated & bit != 0 ||
                self.required & bit == 0 ||
                p.generations[view] == 0 ||
                self.generations[view] != p.generations[view] { continue; }
            self.records[view] = Record { generation: p.generations[view], epoch: p.epoch,
                target: p.target, sampled_token: p.token, valid: true,
                present: counts[view] != 0 };
            accepted = true;
        }
        accepted
    }

    pub fn snapshot(&self) -> WgrViewReferenceFacts {
        let mut out = WgrViewReferenceFacts { version: VERSION,
            struct_bytes: size_of::<WgrViewReferenceFacts>() as u32,
            view_count: VIEWS as u32, render_token: self.token, instance_epoch: self.epoch,
            required: self.required, dropped_samples: self.dropped,
            target_model_id: self.target.model_id,
            target_instance_handle: self.target.instance_handle,
            target_source_generation: self.target.source_generation,
            ..Default::default() };
        out.pending_samples = self.pending.iter().flatten()
            .map(|p| p.copied.count_ones()).sum();
        if !self.committed { out.unknown = self.required; return out; }
        // An empty caller-supplied mask is useful when views are disabled, but
        // it cannot certify all-view absence without observing any GPU view.
        if self.required == 0 { return out; }
        for view in 0..VIEWS {
            let bit = 1u64 << view;
            if self.required & bit == 0 { continue; }
            let r = self.records[view];
            if !r.valid || r.epoch != self.epoch || r.target != self.target ||
                r.generation != self.generations[view] ||
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

    fn count_fact(token: u64, status: MainCountStatus, count: u32) -> MainCountFact {
        MainCountFact { identity: MainCountIdentity { token, model_id: 7,
            source_generation: 9, cull_epoch: 11, target_revision: 1 }, status, count }
    }

    fn full_plan(identity: MainCountIdentity) -> ApplicabilityPlan {
        ApplicabilityPlan::from_frame(identity, 4, 24, true, true, true).unwrap()
    }

    #[test]
    fn applicability_uses_configured_views_not_recorded_draws() {
        let id = count_fact(5, MainCountStatus::Absent, 0).identity;
        let night = ApplicabilityPlan::from_frame(id, 0, 2, false, false, false).unwrap();
        assert_eq!(night.required, 1 | (1 << 5) | (1 << 6));
        let interior = ApplicabilityPlan::from_frame(id, 0, 0, true, false, false).unwrap();
        assert_eq!(interior.required, 1 | (0b1_1111 << INTERIOR.start));
        let gi = ApplicabilityPlan::from_frame(id, 0, 0, false, true, false).unwrap();
        assert_eq!(gi.required, 1 | (1 << GI));
        let all = full_plan(id);
        assert_eq!(all.required, VIEW_MASK);
        assert!(ApplicabilityPlan::from_frame(id, 5, 0, false, false, false).is_none());
        assert!(ApplicabilityPlan::from_frame(id, 0, 25, false, false, false).is_none());
        assert!(ApplicabilityPlan::from_frame(MainCountIdentity { source_generation: 0, ..id },
            0, 0, false, false, false).is_none());
        assert!(ApplicabilityPlan::from_frame(MainCountIdentity { target_revision: 0, ..id },
            0, 0, false, false, false).is_none());
    }

    #[test]
    fn exact_request_mask_keeps_enabled_skipped_views_unknown() {
        let id = count_fact(5, MainCountStatus::Absent, 0).identity;
        let completion = main_count_completion::Fact { identity: id,
            state: main_count_completion::COMPLETED };
        let plan = ApplicabilityPlan::from_frame(id, 0, 1, false, false, true).unwrap();
        let mut facts = [MainCountFact::default(); VIEWS];
        facts[MAIN] = count_fact(5, MainCountStatus::Absent, 0);
        facts[5] = count_fact(5, MainCountStatus::Absent, 0);
        facts[1] = count_fact(5, MainCountStatus::Present, 1); // inactive solar lane
        let partial = aggregate_existing_counts(completion, &facts, 0, Some(plan));
        assert_eq!(partial.required, (1 << MAIN) | (1 << 5) | (1 << REFLECTION));
        assert_eq!(partial.absent, (1 << MAIN) | (1 << 5));
        assert_eq!(partial.unknown, 1 << REFLECTION);
        assert_eq!(partial.present, 0);
        assert_eq!(partial.status, 2);
        facts[REFLECTION] = count_fact(5, MainCountStatus::Absent, 0);
        assert_eq!(aggregate_existing_counts(completion, &facts, 0, Some(plan)).status, 1);
        let wrong = ApplicabilityPlan { identity: MainCountIdentity { token: 4, ..id }, ..plan };
        let refused = aggregate_existing_counts(completion, &facts, 0, Some(wrong));
        assert_eq!((refused.status, refused.absent, refused.unknown), (0, 0, VIEW_MASK));
        let wrong_source = ApplicabilityPlan { identity: MainCountIdentity {
            source_generation: 8, ..id }, ..plan };
        assert_eq!(aggregate_existing_counts(completion, &facts, 0, Some(wrong_source)).status, 0);
        let wrong_epoch = ApplicabilityPlan { identity: MainCountIdentity {
            cull_epoch: 10, ..id }, ..plan };
        assert_eq!(aggregate_existing_counts(completion, &facts, 0, Some(wrong_epoch)).status, 0);
        assert_eq!(aggregate_existing_counts(completion, &facts, 0, None).status, 0);
    }

    #[test]
    fn all_view_snapshot_requires_completion_and_all_36_real_facts() {
        let id = count_fact(5, MainCountStatus::Absent, 0).identity;
        let mut facts = [count_fact(5, MainCountStatus::Absent, 0); VIEWS];
        let mut completion = main_count_completion::Fact { identity: id,
            state: main_count_completion::SUBMITTED };
        let plan = full_plan(id);
        let pending = aggregate_existing_counts(completion, &facts, 0, Some(plan));
        assert_eq!((pending.status, pending.absent, pending.unknown), (0, 0, VIEW_MASK));
        completion.state = main_count_completion::COMPLETED;
        assert_eq!(aggregate_existing_counts(completion, &facts, 0, Some(plan)).status, 1);
        facts[35] = MainCountFact::default();
        let partial = aggregate_existing_counts(completion, &facts, 0, Some(plan));
        assert_eq!(partial.required, VIEW_MASK);
        assert_eq!(partial.unknown, 1u64 << 35);
        assert_eq!(partial.status, 2);
        assert_eq!(partial.absent, VIEW_MASK & !(1u64 << 35));
    }

    #[test]
    fn all_view_snapshot_rejects_stale_target_and_unbound_old_tokens() {
        let id = count_fact(5, MainCountStatus::Absent, 0).identity;
        let completion = main_count_completion::Fact { identity: id,
            state: main_count_completion::COMPLETED };
        let plan = full_plan(id);
        let mut facts = [count_fact(5, MainCountStatus::Absent, 0); VIEWS];
        facts[0].identity.token = 4; // main has no published cache binding
        facts[1].identity.source_generation = 8;
        facts[2].identity.cull_epoch = 10;
        facts[3].identity.model_id = 8;
        facts[4] = count_fact(6, MainCountStatus::Absent, 0);
        facts[5] = count_fact(4, MainCountStatus::Absent, 0); // published local fact
        facts[29] = count_fact(4, MainCountStatus::Present, 3); // published interior fact
        facts[34] = count_fact(4, MainCountStatus::Absent, 0); // published GI fact
        facts[35] = count_fact(5, MainCountStatus::Absent, 4); // incoherent COUNT
        let unbound = aggregate_existing_counts(completion, &facts, 0, Some(plan));
        assert_ne!(unbound.unknown & (1u64 << 5), 0);
        assert_ne!(unbound.unknown & (1u64 << 29), 0);
        assert_ne!(unbound.unknown & (1u64 << 34), 0);
        let snapshot = aggregate_existing_counts(completion, &facts,
            (1u64 << 5) | (1u64 << 29) | (1u64 << 34), Some(plan));
        assert_eq!(snapshot.unknown & 0b1_1111, 0b1_1111);
        assert_eq!(snapshot.cached, (1u64 << 5) | (1u64 << 29) | (1u64 << 34));
        assert_eq!(snapshot.present, 1u64 << 29);
        assert_ne!(snapshot.unknown & (1u64 << 35), 0);
        assert_eq!(snapshot.status, 2);
    }

    #[test]
    fn all_view_snapshot_rejects_changed_target_revision_for_fresh_and_cached_facts() {
        let id = count_fact(5, MainCountStatus::Absent, 0).identity;
        let completion = main_count_completion::Fact { identity: id,
            state: main_count_completion::COMPLETED };
        let plan = full_plan(id);
        let mut facts = [count_fact(5, MainCountStatus::Absent, 0); VIEWS];
        facts[MAIN].identity.target_revision = 2;
        for view in [LOCALS.start, INTERIOR.start, GI] {
            facts[view].identity.token = 4;
            facts[view].identity.target_revision = 2;
        }
        let published = (1u64 << LOCALS.start) | (1u64 << INTERIOR.start) | (1u64 << GI);
        let snapshot = aggregate_existing_counts(completion, &facts, published, Some(plan));
        assert_eq!(snapshot.unknown & (published | (1u64 << MAIN)),
            published | (1u64 << MAIN));
        assert_eq!(snapshot.absent & (published | (1u64 << MAIN)), 0);
        assert_eq!(snapshot.cached & published, 0);
        assert_eq!(snapshot.status, 2);
    }

    fn generations(n: u64) -> [u64; VIEWS] { [n; VIEWS] }
    fn target(n: u64) -> TargetIdentity {
        TargetIdentity { model_id: 7, instance_handle: 0, source_generation: n }
    }
    fn counts() -> [u32; VIEWS] { [0; VIEWS] }

    #[test]
    fn copied_v2_layout_and_target_identity() {
        assert_eq!(size_of::<WgrViewReferenceFacts>(), 96);
        assert_eq!(offset_of!(WgrViewReferenceFacts, render_token), 16);
        assert_eq!(offset_of!(WgrViewReferenceFacts, required), 32);
        assert_eq!(offset_of!(WgrViewReferenceFacts, pending_samples), 72);
        assert_eq!(offset_of!(WgrViewReferenceFacts, target_model_id), 80);
        assert_eq!(offset_of!(WgrViewReferenceFacts, target_source_generation), 88);
        assert!(valid_layout(96, 2));
        assert!(!valid_layout(80, 1));
        assert!(!valid_layout(96, 1));
    }

    #[test]
    fn five_refreshed_views_fit_one_frame_slot_and_resolve_together() {
        let mut facts = ViewReferenceFacts::default();
        let required = (1u64 << 5) - 1;
        assert!(facts.begin(1, 9, target(1), required, required, generations(1)));
        let ticket = facts.queue_copy(0).unwrap();
        for view in 1..5 {
            let next = facts.queue_copy(view).unwrap();
            assert_eq!(next, ticket);
        }
        assert!(facts.queue_copy(0).is_none());
        assert_eq!(facts.snapshot().pending_samples, 5);
        assert_eq!(facts.snapshot().unknown, required);
        let mut returned = counts(); returned[2] = 1;
        assert!(!facts.resolve(ticket, Some(&returned)), "unsubmitted copy is no evidence");
        // An unsubmitted ticket was drained; reserve all five again.
        let ticket = facts.queue_copy(0).unwrap();
        for view in 1..5 { assert_eq!(facts.queue_copy(view).unwrap(), ticket); }
        assert!(facts.mark_submitted(1, required));
        assert!(facts.commit(1));
        assert!(facts.resolve(ticket, Some(&returned)));
        let s = facts.snapshot();
        assert_eq!(s.status, 1);
        assert_eq!(s.present, 1u64 << 2);
        assert_eq!(s.absent, required & !(1u64 << 2));
        assert_eq!(s.unknown, 0);
        assert_eq!(s.pending_samples, 0);
    }

    #[test]
    fn delayed_readback_never_proves_refreshed_view_but_can_prove_unchanged_cache() {
        let mut facts = ViewReferenceFacts::default();
        let required = 0b11;
        assert!(facts.begin(1, 8, target(1), required, required, generations(1)));
        let ticket = facts.queue_copy(0).unwrap();
        assert_eq!(facts.queue_copy(1).unwrap(), ticket);
        assert!(facts.mark_submitted(1, required));
        assert!(facts.commit(1));
        let mut next_gens = generations(1); next_gens[0] = 2;
        assert!(facts.begin(2, 8, target(1), required, 1, next_gens));
        assert!(facts.commit(2));
        assert!(facts.resolve(ticket, Some(&counts())));
        let s = facts.snapshot();
        assert_eq!(s.absent, 0b10);
        assert_eq!(s.cached, 0b10);
        assert_eq!(s.unknown, 0b01);
    }

    #[test]
    fn same_generation_refresh_invalidates_pending_without_reusing_gpu_slot() {
        let mut facts = ViewReferenceFacts::default();
        assert!(facts.begin(1, 8, target(1), 1, 1, generations(7)));
        let old = facts.queue_copy(0).unwrap();
        assert!(facts.mark_submitted(1, 1)); assert!(facts.commit(1));
        // Publication generation is still 7 while replacement draw is only attempted.
        assert!(facts.begin(2, 8, target(1), 1, 1, generations(7)));
        assert_eq!(facts.snapshot().pending_samples, 1);
        assert!(facts.commit(2));
        assert!(!facts.resolve(old, Some(&counts())));
        assert_eq!(facts.snapshot().unknown, 1);
    }

    #[test]
    fn disabled_then_reenabled_same_generation_requires_new_sample() {
        let mut facts = ViewReferenceFacts::default();
        let bit = 1u64 << GI;
        assert!(facts.begin(1, 8, target(1), bit, bit, generations(7)));
        let old = facts.queue_copy(GI).unwrap();
        assert!(facts.mark_submitted(1, bit)); assert!(facts.commit(1));
        assert!(facts.resolve(old, Some(&counts())));
        assert_eq!(facts.snapshot().absent, bit);
        assert!(facts.begin(2, 8, target(1), 0, 0, generations(7)));
        assert!(facts.commit(2));
        assert_eq!(facts.snapshot().status, 0, "no required views cannot prove absence");
        assert!(facts.begin(3, 8, target(1), bit, 0, generations(7)));
        assert!(facts.commit(3));
        assert_eq!(facts.snapshot().unknown, bit);
        assert_eq!(facts.snapshot().absent, 0);
    }

    #[test]
    fn early_planar_submit_does_not_certify_later_unsubmitted_main_copy() {
        let mut facts = ViewReferenceFacts::default();
        assert!(facts.begin(1, 8, target(1), 0b11, 0b11, generations(1)));
        let ticket = facts.queue_copy(0).unwrap(); // copied in early planar encoder
        assert!(facts.mark_submitted(1, 0b01));
        assert_eq!(facts.queue_copy(1).unwrap(), ticket); // main encoder recorded, not submitted
        assert!(facts.commit(1));
        assert!(facts.resolve(ticket, Some(&counts())));
        assert_eq!(facts.snapshot().absent, 0b01);
        assert_eq!(facts.snapshot().unknown, 0b10);

        assert!(facts.begin(2, 8, target(1), 0b11, 0b11, generations(2)));
        let aborted = facts.queue_copy(0).unwrap();
        assert!(facts.mark_submitted(2, 0b01));
        assert_eq!(facts.queue_copy(1).unwrap(), aborted);
        facts.abort(2);
        assert_eq!(facts.snapshot().pending_samples, 2);
        assert!(!facts.resolve(aborted, Some(&counts())));
        assert_eq!(facts.snapshot().unknown, 0b11);
    }

    #[test]
    fn commit_cannot_precede_or_invent_a_submit() {
        let mut facts = ViewReferenceFacts::default();
        assert!(!facts.commit(0), "an unstarted frame cannot commit");
        assert!(!facts.mark_submitted(0, 1));
        assert!(facts.begin(1, 8, target(1), 1, 1, generations(1)));
        let ticket = facts.queue_copy(0).unwrap();
        assert!(facts.commit(1));
        assert!(!facts.mark_submitted(1, 1), "a late submit cannot repair this frame");
        assert!(!facts.resolve(ticket, Some(&counts())));
        assert_eq!(facts.snapshot().unknown, 1);
        assert_eq!(facts.snapshot().absent, 0);
    }

    #[test]
    fn target_or_epoch_change_invalidates_prior_zero_and_old_completion() {
        let mut facts = ViewReferenceFacts::default();
        assert!(facts.begin(1, 8, target(1), 1, 1, generations(1)));
        let old = facts.queue_copy(0).unwrap();
        assert!(facts.mark_submitted(1, 1)); assert!(facts.commit(1));
        assert!(facts.begin(2, 8, target(2), 1, 1, generations(1)));
        assert!(facts.commit(2));
        assert!(!facts.resolve(old, Some(&counts())));
        assert_eq!(facts.snapshot().unknown, 1);
        assert!(facts.begin(3, 9, target(2), 1, 1, generations(1)));
        assert!(facts.commit(3));
        assert_eq!(facts.snapshot().unknown, 1);
    }

    #[test]
    fn saturation_aborted_submit_and_failed_map_remain_unknown() {
        let mut facts = ViewReferenceFacts::default();
        let mut tickets = Vec::new();
        for token in 1..=PENDING as u64 {
            assert!(facts.begin(token, 2, target(1), 1, 1, generations(token)));
            tickets.push(facts.queue_copy(0).unwrap());
            assert!(facts.mark_submitted(token, 1));
            assert!(facts.commit(token));
        }
        assert!(facts.begin(4, 2, target(1), 1, 1, generations(4)));
        assert!(facts.queue_copy(0).is_none());
        assert_eq!(facts.snapshot().dropped_samples, 1);
        assert!(facts.commit(4));
        assert!(!facts.resolve(tickets[0], Some(&counts())));
        assert!(facts.begin(5, 2, target(1), 1, 1, generations(5)));
        let latest = facts.queue_copy(0).unwrap();
        assert!(facts.mark_submitted(5, 1));
        facts.abort(5);
        assert!(!facts.resolve(latest, Some(&counts())));
        assert_eq!(facts.snapshot().status, 0);
        assert!(facts.begin(6, 2, target(1), 1, 1, generations(6)));
        let failed = facts.queue_copy(0).unwrap();
        assert!(facts.mark_submitted(6, 1)); assert!(facts.commit(6));
        assert!(!facts.resolve(failed, None));
        assert_eq!(facts.snapshot().unknown, 1);
    }

    #[test]
    fn only_actual_required_views_and_copies_can_prove_zero() {
        let mut facts = ViewReferenceFacts::default();
        assert!(!facts.begin(1, 1, TargetIdentity::default(), 1, 1, generations(1)));
        assert!(facts.begin(1, 1, target(1), 1u64 << GI, 1u64 << GI, generations(1)));
        assert!(facts.queue_copy(GI).is_some());
        assert!(facts.mark_submitted(1, 1u64 << GI)); assert!(facts.commit(1));
        assert_eq!(facts.snapshot().unknown, 1u64 << GI);
        // A scene with no sky view must not add sky to the required mask. A scene with an
        // enabled but skipped sky view must leave it required and Unknown until copied.
        assert!(facts.begin(2, 1, target(1), 1, 1, generations(2)));
        assert!(facts.commit(2));
        assert_eq!(facts.snapshot().required, 1);
        assert_eq!(facts.snapshot().unknown, 1);
    }
}
