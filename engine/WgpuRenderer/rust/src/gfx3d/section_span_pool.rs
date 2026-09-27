//! Optional bounded CPU payload reuse only. Model births never recycle; no GPU memory claim.
use std::collections::VecDeque;
pub(super) const MAX_OWNERS: usize = 8192;
const MAX_RANGES: usize = 4096;
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) struct Span { pub base: usize, pub count: usize }
#[derive(Clone, Copy)]
struct Owner { span: Span, retired: bool }
/// Relative LODs may overlap within one birth, but none may escape its submitted rows.
/// No temporary list: the production mapped slice iterator has an exact bounded length.
pub(super) fn valid_ownership(
    sections: usize, materials: usize,
    lods: impl ExactSizeIterator<Item = (usize, usize)>,
) -> bool {
    materials == sections && lods.len() <= 4096 && lods.into_iter().all(|(base, count)|
        base.checked_add(count).is_some_and(|end| end <= sections))
}
/// Bounded ON-only history. Capacity or invalid ownership disables reuse for this epoch.
pub(super) struct SectionSpanPool {
    owners: Vec<Owner>, pending: VecDeque<Span>, free: Vec<Span>, enabled: bool,
    activation_notice: bool, disabled_notice: bool, reuse_events: u8, table: &'static str,
}
impl SectionSpanPool {
    pub fn new() -> Self { Self::new_table("SECTION") }
    pub fn new_table(table: &'static str) -> Self {
        Self { owners: Vec::with_capacity(MAX_OWNERS), pending: VecDeque::with_capacity(MAX_RANGES),
               free: Vec::with_capacity(MAX_RANGES), enabled: true,
               activation_notice: true, disabled_notice: false, reuse_events: 0, table }
    }
    pub fn disable(&mut self) {
        if self.enabled { self.disabled_notice = true; }
        self.enabled = false; self.pending.clear(); self.free.clear();
    }
    pub fn take_notice(&mut self) -> Option<String> {
        if self.activation_notice {
            self.activation_notice = false;
            let known_payload = self.owners.capacity() * std::mem::size_of::<Owner>()
                + (self.pending.capacity() + self.free.capacity()) * std::mem::size_of::<Span>();
            let work_scope = if self.table == "LOD" {
                "retireCallRows=4096 retireCallRanges=1 noFrameGlobalBudget"
            } else { "cleanupRows=4096 cleanupRanges=32" };
            return Some(format!("gpu {} reuse enabled: ownerCap={} rangeCap={} knownContainerPayloadBytes={} {}; optionalCPUmetadata notRSS/notGPUfree",
                self.table, MAX_OWNERS, MAX_RANGES, known_payload, work_scope));
        }
        if self.disabled_notice {
            self.disabled_notice = false;
            return Some(format!("gpu {} reuse disabled for renderer epoch: invalidOwnership/capacity/parallelTableOrMutableEscape; append fallback", self.table));
        }
        None
    }
    /// Called only after source, resolved, material and model/LOD commits have completed.
    pub fn note_committed_reuse(&mut self, model: u32, span: Span, old_len: usize, new_len: usize) -> Option<String> {
        if !self.enabled || span.count == 0 || span.base >= old_len { return None; }
        if self.reuse_events < 64 {
            self.reuse_events += 1;
            let length_label = if self.table == "SECTION" { "sectionLength" } else { "lodLength" };
            return Some(format!("gpu {} reuse committed: modelBirth={} base={} count={} {}={} event={}; CPUtableindices only",
                self.table, model, span.base, span.count, length_label, new_len, self.reuse_events));
        }
        if self.reuse_events == 64 {
            self.reuse_events = 65;
            return Some(format!("gpu {} reuse event log truncated after64commits; reuse policy unchanged", self.table));
        }
        None
    }
    /// Active immutable birth bounds survive sticky disable; a retired birth never reclaims them twice.
    pub fn birth_span(&self, model: usize) -> Option<Span> {
        self.owners.get(model).filter(|owner| !owner.retired).map(|owner| owner.span)
    }
    pub fn enabled(&self) -> bool { self.enabled }
    pub fn accepts_birth(&self, model: usize, append: usize, count: usize) -> bool {
        self.enabled && model == self.owners.len() && model < MAX_OWNERS
            && append.checked_add(count).is_some_and(|end| end <= u32::MAX as usize)
    }
    /// Caller reserves every destination array before this nonallocating commit.
    pub fn register(&mut self, model: usize, append: usize, count: usize, valid: bool) -> Span {
        if !valid || !self.accepts_birth(model, append, count) { self.disable(); }
        let mut span = Span { base: append, count };
        if self.enabled && count != 0 {
            if let Some(i) = self.free.iter().position(|s| s.count >= count) {
                span.base = self.free[i].base;
                if self.free[i].count == count { self.free.swap_remove(i); }
                else { self.free[i].base += count; self.free[i].count -= count; }
            }
        }
        if self.enabled { self.owners.push(Owner { span, retired: false }); }
        span
    }
    pub fn retire(&mut self, model: usize) {
        if !self.enabled { return; }
        let Some(owner) = self.owners.get_mut(model) else { return; };
        if owner.retired { return; }
        owner.retired = true;
        if owner.span.count == 0 { return; }
        if self.pending.len() == MAX_RANGES { self.disable(); return; }
        self.pending.push_back(owner.span);
    }
    /// At most `ranges` completed spans and `rows` cleared rows. Partial spans remain unavailable.
    /// Callback completes the owning domain transition; availability publication follows callback.
    pub fn drain(&mut self, mut rows: usize, mut ranges: usize, mut clear: impl FnMut(Span)) -> bool {
        if !self.enabled { return false; }
        let mut changed = false;
        while rows != 0 && ranges != 0 {
            let Some(span) = self.pending.front().copied() else { break; };
            let count = rows.min(span.count);
            clear(Span { base: span.base, count });
            changed = true; rows -= count;
            if count == span.count {
                self.pending.pop_front(); ranges -= 1;
                // Store only the completed suffix: earlier cleared prefixes are deliberately
                // unreused bounded holes, avoiding another partial-progress owner record.
                if self.free.len() == MAX_RANGES { self.disable(); break; }
                self.free.push(span);
            } else {
                *self.pending.front_mut().unwrap() = Span { base: span.base + count, count: span.count - count };
            }
        }
        changed
    }
}
/// Shared source-table publication; callers reserve append room before choosing a span.
pub(super) fn write_span<T: Copy>(table: &mut Vec<T>, span: Span, rows: &[T]) {
    assert_eq!(span.count, rows.len());
    if span.base == table.len() { table.extend_from_slice(rows); }
    else { table[span.base..span.base + span.count].copy_from_slice(rows); }
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test] fn actual_relative_ownership_gate_rejects_cross_birth_alias_and_overflow() {
        assert!(valid_ownership(3,3,[(0,2),(1,2)].into_iter()));
        assert!(!valid_ownership(1,1,[(1,1)].into_iter()));
        assert!(!valid_ownership(1,1,[(usize::MAX,1)].into_iter()));
        assert!(!valid_ownership(1,0,[(0,1)].into_iter()));
        assert!(!valid_ownership(1,1,std::iter::repeat((0,0)).take(4097)));
        let mut pool=SectionSpanPool::new();
        pool.register(0,0,1,valid_ownership(1,1,[(1,1)].into_iter()));
        pool.register(1,1,1,true);pool.retire(1);
        assert!(!pool.drain(4096,32, |_|panic!("earlier alias forbids reclaim")));
    }
    #[test] fn clear_all_three_before_reuse_and_keep_births_distinct() {
        let mut pool=SectionSpanPool::new(); let mut src=vec![10,20];let mut resolved=src.clone();let mut mat=src.clone();
        assert_eq!(pool.register(0,0,1,true),Span{base:0,count:1});
        pool.register(1,1,1,true);pool.retire(0);pool.retire(0);
        assert!(pool.drain(4096,32,|s| {for i in s.base..s.base+s.count {src[i]=0;resolved[i]=0;mat[i]=0;}}));
        assert_eq!((&src,&resolved,&mat),(&vec![0,20],&vec![0,20],&vec![0,20]));
        let c=pool.register(2,2,1,true);assert_eq!(c.base,0);
        write_span(&mut src,c,&[30]);write_span(&mut resolved,c,&[31]);write_span(&mut mat,c,&[32]);
        assert_eq!((src,resolved,mat),(vec![30,20],vec![31,20],vec![32,20]));
    }
    #[test] fn invalid_any_owner_sticky_disables_even_preexisting_free_spans() {
        let mut pool=SectionSpanPool::new();pool.register(0,0,1,true);pool.retire(0);pool.drain(1,1, |_|{});
        assert_eq!(pool.register(1,1,1,false).base,1);
        assert_eq!(pool.register(2,2,1,true).base,2);pool.retire(2);
        assert!(!pool.drain(4096,32, |_|panic!("disabled")));
    }
    #[test] fn partial_clear_never_exposes_uncleared_rows_and_callback_failure_does_not_commit() {
        let mut pool=SectionSpanPool::new();pool.register(0,0,5000,true);pool.retire(0);
        assert!(std::panic::catch_unwind(std::panic::AssertUnwindSafe(||pool.drain(10,1, |_|panic!("failure")))).is_err());
        let mut calls=Vec::new();pool.drain(4096,32, |s|calls.push(s));
        assert_eq!(calls,vec![Span{base:0,count:4096}]);
        assert_eq!(pool.register(1,5000,1,true).base,5000);
        pool.drain(4096,32, |s|calls.push(s));
        assert_eq!(pool.register(2,5001,904,true).base,4096);
    }
    #[test] fn bounded_diagnostics_only_publish_actual_postwrite_reuse() {
        let mut pool=SectionSpanPool::new();assert!(pool.take_notice().unwrap().contains("enabled"));
        assert!(pool.take_notice().is_none());
        let append=Span{base:1,count:1};assert!(pool.note_committed_reuse(1,append,1,2).is_none());
        let reused=Span{base:0,count:1};
        for _ in 0..64 {assert!(pool.note_committed_reuse(2,reused,2,2).unwrap().contains("committed"));}
        assert!(pool.note_committed_reuse(2,reused,2,2).unwrap().contains("truncated"));
        assert!(pool.note_committed_reuse(2,reused,2,2).is_none());
        pool.disable();assert!(pool.take_notice().unwrap().contains("disabled"));
        pool.disable();assert!(pool.take_notice().is_none());
    }
    #[test] fn lod_payload_allocator_cannot_own_permanent_sentinel_and_reuses_only_retired_birth() {
        let mut pool=SectionSpanPool::new_table("LOD");
        assert!(pool.take_notice().unwrap().starts_with("gpu LOD reuse enabled:"));
        let a=pool.register(0,1,2,true);let b=pool.register(1,3,1,true);
        assert_eq!(a.base,1);assert_eq!(b.base,3);
        pool.retire(0);assert!(pool.birth_span(0).is_none());let mut cleared=Vec::new();pool.drain(4096,1,|span|cleared.push(span));
        assert_eq!(cleared,vec![a]);
        let c=pool.register(2,4,2,true);assert_eq!(c,a);assert_ne!(c.base,0);
        let line=pool.note_committed_reuse(2,c,4,4).unwrap();
        assert!(line.starts_with("gpu LOD reuse committed:"));assert!(line.contains("lodLength=4"));
        pool.retire(0);assert!(!pool.drain(4096,1,|_|panic!("repeat retire")));
    }
    #[test] fn capacity_fallback_is_append_only() {
        let mut pool=SectionSpanPool::new();for i in 0..MAX_OWNERS {pool.register(i,i,1,true);}
        pool.retire(0);pool.drain(1,1, |_|{});
        assert_eq!(pool.register(MAX_OWNERS,MAX_OWNERS,1,true).base,MAX_OWNERS);
        assert!(!pool.enabled);
    }
}
