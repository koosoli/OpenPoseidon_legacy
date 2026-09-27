//! Explicit owner-only mesh record/range facts. No lifetime policy or GPU wait.
use crate::ffi::{WgrMeshHandleFact, WgrMeshHandleFactSummary};
use std::collections::HashSet;

pub(super) const MAX_HANDLES: usize = 8192;

/// Lookup must use the full generational mesh handle. Absent means no current
/// renderer mesh record; it does not acknowledge GPU completion/device release.
pub(super) fn collect(
    handles: &[u64], out: &mut [WgrMeshHandleFact],
    mut lookup: impl FnMut(u64) -> Option<(u64, u64)>,
) -> WgrMeshHandleFactSummary {
    let mut summary = WgrMeshHandleFactSummary {
        handles_requested: u32::try_from(handles.len()).unwrap_or(u32::MAX),
        ..Default::default()
    };
    let count = handles.len().min(out.len()).min(MAX_HANDLES);
    if count != handles.len() { summary.flags |= 1; }
    let mut seen = HashSet::with_capacity(count);
    for (&handle, row) in handles.iter().zip(out.iter_mut()).take(count) {
        *row = WgrMeshHandleFact { mesh_handle: handle, ..Default::default() };
        summary.handles_inspected += 1;
        if handle == 0 { summary.invalid += 1; continue; }
        let unique = seen.insert(handle);
        if !unique { summary.duplicate_handles += 1; }
        let Some((vertex, index)) = lookup(handle) else {
            row.state = 2; summary.absent += 1; continue;
        };
        row.state = 1; row.vertex_bytes = vertex; row.index_bytes = index;
        summary.present += 1;
        if unique {
            for (sum, value) in [(&mut summary.unique_vertex_bytes, vertex), (&mut summary.unique_index_bytes, index)] {
                if let Some(total) = sum.checked_add(value) { *sum = total; }
                else { *sum = u64::MAX; summary.flags |= 2; }
            }
        }
    }
    summary.complete = u32::from(summary.flags == 0 && summary.invalid == 0 && summary.duplicate_handles == 0);
    summary
}

// Attached only by the renderer owner after this nonyielding requested-handle
// lookup. No registry walk: live record count is O(1); the producer explicitly
// calculates existing allocator free-span/retired metadata once for this cut.
// This is not current-frame/uploader reference or physical memory coverage.
pub(super) fn with_pool_scope(mut facts: WgrMeshHandleFactSummary, records: u64, live: u64, generation: u64)
    -> WgrMeshHandleFactSummary {
    facts.live_mesh_records=records; facts.pool_live_bytes=live;
    facts.pool_generation=generation; facts.record_scope_valid=1;
    facts
}

#[cfg(test)]
mod tests {
    use super::*;
    use slotmap::{new_key_type, Key, KeyData, SlotMap};
    new_key_type! { struct TestMeshKey; }

    #[test]
    fn full_generation_distinguishes_reused_slot_and_old_record_absence() {
        let mut meshes: SlotMap<TestMeshKey, (u64, u64)> = SlotMap::with_key();
        let old = meshes.insert((136, 12));
        let old_handle = old.data().as_ffi();
        let mut rows = [WgrMeshHandleFact::default(); 2];
        let before = collect(&[old_handle], &mut rows, |id| meshes.get(KeyData::from_ffi(id).into()).copied());
        assert_eq!((before.present, rows[0].vertex_bytes), (1, 136));
        meshes.remove(old);
        let new = meshes.insert((272, 24));
        let new_handle = new.data().as_ffi();
        assert_ne!(old_handle, new_handle);
        // SlotMap's FFI key encodes slot in the low word and generation in
        // the high word; this fixture actually reuses the removed slot.
        assert_eq!(old_handle as u32, new_handle as u32);
        let after = collect(&[old_handle, new_handle], &mut rows, |id| meshes.get(KeyData::from_ffi(id).into()).copied());
        assert_eq!((after.absent, after.present, after.complete), (1, 1, 1));
        assert_eq!((rows[0].mesh_handle, rows[0].state, rows[0].vertex_bytes), (old_handle, 2, 0));
        assert_eq!((rows[1].mesh_handle, rows[1].state, rows[1].vertex_bytes), (new_handle, 1, 272));
        assert_eq!((after.unique_vertex_bytes, after.unique_index_bytes), (272, 24));
    }

    #[test]
    fn duplicates_charge_union_once_but_do_not_prove_complete_identity() {
        let mut rows = [WgrMeshHandleFact::default(); 3];
        let s = collect(&[7, 7, 8], &mut rows, |id| Some(if id == 7 { (136, 12) } else { (68, 4) }));
        assert_eq!((s.present, s.duplicate_handles, s.complete), (3, 1, 0));
        assert_eq!((s.unique_vertex_bytes, s.unique_index_bytes), (204, 16));
        assert_eq!((rows[0].mesh_handle, rows[1].mesh_handle, rows[2].mesh_handle), (7, 7, 8));
    }

    #[test]
    fn invalid_zero_and_truncated_prefix_never_claim_complete() {
        let mut rows = [WgrMeshHandleFact::default(); 2];
        let invalid = collect(&[0, 9], &mut rows, |id| { assert_ne!(id, 0); None });
        assert_eq!((invalid.invalid, invalid.absent, invalid.complete), (1, 1, 0));
        assert_eq!((rows[0].state, rows[1].state), (0, 2));
        let short = collect(&[1, 2], &mut rows[..1], |_| Some((68, 4)));
        assert_eq!((short.handles_requested, short.handles_inspected, short.complete, short.flags), (2, 1, 0, 1));
        let ids: Vec<u64> = (1..=8193).collect();
        let mut output = vec![WgrMeshHandleFact::default(); ids.len()];
        let mut visits = 0;
        let capped = collect(&ids, &mut output, |_| { visits += 1; Some((68, 4)) });
        assert_eq!((visits, capped.handles_inspected, capped.complete), (8192, 8192, 0));
        assert_eq!(capped.flags & 1, 1);
        assert_eq!(output[8192].state, 0);
    }

    #[test]
    fn supplied_generational_subset_keeps_total_live_pool_scope_separate() {
        let mut rows=[WgrMeshHandleFact::default();3];
        let full=with_pool_scope(collect(&[7,8,9],&mut rows,|id| if id==9 {None} else {Some((68,4))}),2,144,5);
        assert_eq!((full.present,full.absent,full.complete,full.live_mesh_records,full.pool_live_bytes,full.record_scope_valid),(2,1,1,2,144,1));
        let partial=with_pool_scope(collect(&[7],&mut rows,|_| Some((68,4))),2,144,5);
        assert_eq!((partial.present,partial.live_mesh_records,partial.unique_vertex_bytes),(1,2,68));
        let duplicate=with_pool_scope(collect(&[7,7],&mut rows,|_| Some((68,4))),2,144,5);
        assert_eq!((duplicate.complete,duplicate.duplicate_handles,duplicate.live_mesh_records),(0,1,2));
        let missing=with_pool_scope(collect(&[9],&mut rows,|_| None),2,144,5);
        assert_eq!((missing.absent,missing.present,missing.pool_live_bytes),(1,0,144));
        let unknown=collect(&[],&mut rows,|_| None);
        assert_eq!((unknown.complete,unknown.record_scope_valid),(1,0));
    }

    #[test]
    fn byte_sum_overflow_is_explicit_not_wrapped_complete() {
        let mut rows = [WgrMeshHandleFact::default(); 2];
        let s = collect(&[1, 2], &mut rows, |_| Some((u64::MAX, 1)));
        assert_eq!(s.unique_vertex_bytes, u64::MAX);
        assert_eq!(s.unique_index_bytes, 2);
        assert_eq!((s.flags & 2, s.complete, s.present), (2, 0, 2));
    }
}
