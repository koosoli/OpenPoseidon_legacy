//! Read-only completion of one explicit main COUNT request. Completion covers
//! preceding queue work only; it says nothing about pixels or mesh release.
use super::cull::MainCountIdentity;
use std::sync::{Arc, atomic::{AtomicBool, AtomicU32, Ordering}};

const MAX_OUTSTANDING: u32 = 3;
pub(super) const UNKNOWN: u32 = 0;
pub(super) const ARMED: u32 = 1;
pub(super) const SUBMITTED: u32 = 2;
pub(super) const COMPLETED: u32 = 3;

#[derive(Clone, Copy, Default)]
pub(crate) struct Fact { pub identity: MainCountIdentity, pub state: u32 }

struct Current {
    identity: MainCountIdentity,
    state: u32,
    committed: bool,
    done: Arc<AtomicBool>,
}

pub(super) struct Signal { done: Arc<AtomicBool>, outstanding: Arc<AtomicU32> }
impl Signal { pub(super) fn complete(self) { self.done.store(true, Ordering::Release); } }
impl Drop for Signal {
    fn drop(&mut self) { self.outstanding.fetch_sub(1, Ordering::AcqRel); }
}

pub(super) struct Witness { current: Option<Current>, outstanding: Arc<AtomicU32> }
impl Witness {
    pub(super) fn new() -> Self {
        Self { current: None, outstanding: Arc::new(AtomicU32::new(0)) }
    }
    pub(super) fn arm(&mut self, identity: MainCountIdentity) {
        self.current = (identity.token != 0 && identity.cull_epoch != 0 &&
            identity.source_generation != 0).then(|| Current {
                identity, state: ARMED, committed: false,
                done: Arc::new(AtomicBool::new(false)),
            });
    }
    pub(super) fn invalidate(&mut self) { self.current = None; }
    pub(super) fn abort(&mut self, token: u64) {
        if self.current.as_ref().is_some_and(|c| c.identity.token == token) { self.invalidate(); }
    }
    pub(super) fn submitted(&mut self, identity: MainCountIdentity,
        mut register: impl FnMut(Signal)) -> bool {
        let Some(current) = self.current.as_mut() else { return false; };
        if current.identity != identity || current.state != ARMED { return false; }
        if self.outstanding.fetch_update(Ordering::AcqRel, Ordering::Acquire,
            |count| (count < MAX_OUTSTANDING).then_some(count + 1)).is_err() {
            self.invalidate(); return false;
        }
        current.state = SUBMITTED;
        register(Signal { done: current.done.clone(), outstanding: self.outstanding.clone() });
        true
    }
    pub(super) fn commit(&mut self, token: u64) {
        if let Some(current) = self.current.as_mut() {
            if current.identity.token == token && current.state == SUBMITTED {
                current.committed = true;
            }
        }
    }
    pub(super) fn fact(&self, current_epoch: u64) -> Fact {
        let Some(current) = self.current.as_ref() else { return Fact::default(); };
        if current.identity.cull_epoch != current_epoch { return Fact::default(); }
        Fact { identity: current.identity,
            state: if current.state == SUBMITTED && current.committed &&
                current.done.load(Ordering::Acquire) { COMPLETED } else { current.state } }
    }
    pub(super) fn completion_pending(&self) -> bool {
        self.outstanding.load(Ordering::Acquire) != 0
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn id(token: u64, source_generation: u64) -> MainCountIdentity {
        MainCountIdentity { token, model_id: 7, source_generation, cull_epoch: 12, target_revision: 1 }
    }
    #[test]
    fn completion_needs_exact_identity_submit_and_successful_commit() {
        let mut w = Witness::new(); w.arm(id(1, 9));
        assert_eq!(w.fact(12).state, ARMED);
        assert!(!w.submitted(id(1, 10), Signal::complete));
        let mut signal = None; assert!(w.submitted(id(1, 9), |s| signal = Some(s)));
        signal.unwrap().complete();
        assert_eq!(w.fact(12).state, SUBMITTED);
        w.commit(1);
        assert_eq!(w.fact(12).state, COMPLETED);
        assert_eq!(w.fact(13).state, UNKNOWN);
    }
    #[test]
    fn abort_replacement_and_bounded_late_callbacks_fail_closed() {
        let mut w = Witness::new(); let mut late = Vec::new();
        for token in 1..=3 {
            w.arm(id(token, 9));
            assert!(w.submitted(id(token, 9), |s| late.push(s)));
            w.abort(token);
        }
        w.arm(id(4, 10));
        assert!(!w.submitted(id(4, 10), Signal::complete));
        assert_eq!(w.fact(12).state, UNKNOWN);
        late.pop().unwrap().complete();
        w.arm(id(5, 11));
        assert!(w.submitted(id(5, 11), |s| late.push(s)));
        for signal in late { signal.complete(); }
        assert_eq!(w.fact(12).state, SUBMITTED);
        w.commit(5); assert_eq!(w.fact(12).state, COMPLETED);
        w.arm(id(6, 12));
        assert_eq!(w.fact(12).state, ARMED);
    }
    #[test]
    fn dropped_callback_cannot_claim_completion() {
        let mut w = Witness::new(); w.arm(id(1, 9));
        assert!(w.submitted(id(1, 9), drop)); w.commit(1);
        assert_eq!(w.fact(12).state, SUBMITTED);
        assert!(!w.completion_pending());
    }
}
