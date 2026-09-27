//! Explicit registered-section census, not allocation ownership or reclamation.
use super::cull::{LodGpu, ModelGpu};
use crate::ffi::{WgrRegisteredMeshRef, WgrRegisteredMeshRefSummary};
use std::collections::HashMap;

pub(super) const MAX_HANDLES: usize = 8192;
pub(super) const CAPACITY: u32 = 1;
pub(super) const INVALID_SPAN: u32 = 2;
pub(super) const OVERFLOW: u32 = 4;
pub(super) const MISSING_RECORD: u32 = 1;

#[derive(Clone, Copy)]
pub(super) struct Limits { pub models: usize, pub lods: usize, pub sections: usize }
pub(super) const LIMITS: Limits = Limits { models: 8192, lods: 262144, sections: 1048576 };

pub(super) fn valid_handles(handles: &[u64]) -> bool {
    if handles.len() > MAX_HANDLES { return false; }
    let mut seen = HashMap::with_capacity(handles.len());
    handles.iter().all(|&h| h != 0 && seen.insert(h, ()).is_none())
}

fn increment(value: &mut u64, flags: &mut u32) {
    if let Some(next) = value.checked_add(1) { *value = next; }
    else { *flags |= OVERFLOW; }
}

/// Inputs are current owner-borrowed tables. Every model/LOD/section visited,
/// including empty entries, consumes the budget. Historical source rows outside
/// current nonempty LOD spans are not registered references. Missing generations
/// are facts, never evidence that a zero-ref row is safe to destroy.
pub(super) fn collect<S>(handles: &[u64], models: &[ModelGpu], lods: &[LodGpu],
    sources: &[S], mut handle_of: impl FnMut(&S) -> u64,
    mut present: impl FnMut(u64) -> bool, limits: Limits,
) -> (Vec<WgrRegisteredMeshRef>, WgrRegisteredMeshRefSummary) {
    debug_assert!(valid_handles(handles));
    let absent: Vec<_> = handles.iter().map(|&mesh| !present(mesh)).collect();
    let mut rows: Vec<_> = handles.iter().map(|&mesh| WgrRegisteredMeshRef {
        mesh, ..Default::default()
    }).collect();
    let index: HashMap<_, _> = handles.iter().enumerate().map(|(i, &h)| (h, i)).collect();
    let mut last_model = vec![usize::MAX; handles.len()];
    let mut last_lod = vec![usize::MAX; handles.len()];
    let mut summary = WgrRegisteredMeshRefSummary { rows_written: rows.len() as u32, ..Default::default() };
    'models: for (model_id, model) in models.iter().enumerate() {
        if summary.models_visited >= limits.models as u64 { summary.refusal_flags |= CAPACITY; break; }
        summary.models_visited += 1;
        let start = model.lod_base as usize;
        let Some(end) = start.checked_add(model.lod_count as usize) else {
            summary.refusal_flags |= INVALID_SPAN; continue;
        };
        let Some(model_lods) = lods.get(start..end) else { summary.refusal_flags |= INVALID_SPAN; continue; };
        for (local_lod, lod) in model_lods.iter().enumerate() {
            if summary.lods_visited >= limits.lods as u64 { summary.refusal_flags |= CAPACITY; break 'models; }
            summary.lods_visited += 1;
            // Retirement deliberately retains old bases but zeroes the count.
            if lod.section_count == 0 { continue; }
            let first = lod.section_base as usize;
            let Some(end) = first.checked_add(lod.section_count as usize) else {
                summary.refusal_flags |= INVALID_SPAN; continue;
            };
            let Some(sections) = sources.get(first..end) else { summary.refusal_flags |= INVALID_SPAN; continue; };
            for section in sections {
                if summary.sections_visited >= limits.sections as u64 { summary.refusal_flags |= CAPACITY; break 'models; }
                summary.sections_visited += 1;
                let Some(&i) = index.get(&handle_of(section)) else { continue; };
                let row = &mut rows[i];
                increment(&mut row.section_occurrences, &mut summary.refusal_flags);
                if last_model[i] != model_id {
                    increment(&mut row.distinct_models, &mut summary.refusal_flags);
                    last_model[i] = model_id; last_lod[i] = usize::MAX;
                }
                if last_lod[i] != local_lod {
                    increment(&mut row.distinct_model_lods, &mut summary.refusal_flags);
                    last_lod[i] = local_lod;
                }
            }
        }
    }
    for (row, absent) in rows.iter_mut().zip(absent) {
        if absent && row.section_occurrences != 0 { row.flags |= MISSING_RECORD; summary.missing_records += 1; }
    }
    summary.complete = u32::from(summary.refusal_flags == 0);
    (rows, summary)
}

#[cfg(test)]
mod tests {
    use super::*;
    use slotmap::{new_key_type, Key, KeyData, SlotMap};
    new_key_type! { struct TestKey; }
    fn model(base:u32,count:u32)->ModelGpu { ModelGpu { lod_base:base,lod_count:count,bounding_sphere:1.0,_pad:0 } }
    fn lod(base:u32,count:u32)->LodGpu { LodGpu { resolution:1.0,section_base:base,section_count:count,is_decal:0 } }

    #[test]
    fn shared_lods_duplicate_sections_and_actual_retirement_exclude_historical_rows() {
        let models = [model(0,2), model(2,1)];
        let mut lods = [lod(0,3),lod(3,1),lod(4,2)];
        let sources = [11,11,22,11,11,22,11]; // Last source is historical/unreachable.
        let (rows,s) = collect(&[11,22],&models,&lods,&sources,|&h|h,|_|true,LIMITS);
        assert_eq!((rows[0].distinct_models,rows[0].distinct_model_lods,rows[0].section_occurrences),(2,3,4));
        assert_eq!((rows[1].distinct_models,rows[1].distinct_model_lods,rows[1].section_occurrences),(2,2,2));
        assert_eq!((s.complete,s.sections_visited),(1,6));
        super::super::cull::retire_model_lods(&models,&mut lods,0);
        let (rows,s) = collect(&[11,22],&models,&lods,&sources,|&h|h,|_|true,LIMITS);
        assert_eq!((rows[0].distinct_models,rows[0].distinct_model_lods,rows[0].section_occurrences),(1,1,1));
        assert_eq!((s.complete,s.sections_visited),(1,2));
        super::super::cull::retire_model_lods(&models,&mut lods,1);
        let (rows,_) = collect(&[11],&models,&lods,&sources,|&h|h,|_|true,LIMITS);
        assert_eq!(rows[0].section_occurrences,0);
    }

    #[test]
    fn reused_slot_does_not_retarget_old_registered_reference() {
        let mut bank:SlotMap<TestKey,()> = SlotMap::with_key();
        let old = bank.insert(()); let old_handle = old.data().as_ffi(); bank.remove(old);
        let new = bank.insert(()); let new_handle = new.data().as_ffi();
        assert_eq!(old_handle as u32,new_handle as u32); assert_ne!(old_handle,new_handle);
        let (rows,s) = collect(&[old_handle,new_handle],&[model(0,1)],&[lod(0,2)],
            &[old_handle,new_handle],|&h|h,|h|bank.contains_key(KeyData::from_ffi(h).into()),LIMITS);
        assert_eq!((rows[0].flags,rows[1].flags,s.missing_records),(MISSING_RECORD,0,1));
        assert_eq!((rows[0].section_occurrences,rows[1].section_occurrences),(1,1));
        let (rows,s)=collect(&[old_handle],&[model(0,1)],&[lod(u32::MAX,0)],
            &[] as &[u64],|&h|h,|_|false,LIMITS);
        assert_eq!((rows[0].flags,s.missing_records,s.complete),(0,0,1));
    }

    #[test]
    fn all_visit_dimensions_and_invalid_spans_refuse_complete_prefix() {
        let models = [model(0,2),model(2,1)]; let lods = [lod(0,0),lod(0,2),lod(2,1)];
        for limits in [Limits{models:1,lods:9,sections:9}, Limits{models:9,lods:1,sections:9}, Limits{models:9,lods:9,sections:1}] {
            let (_,s)=collect(&[1],&models,&lods,&[1,1,1],|&h|h,|_|true,limits);
            assert_eq!(s.complete,0); assert_eq!(s.refusal_flags & CAPACITY,CAPACITY);
        }
        for (m,l) in [([model(9,1)],[lod(0,1)]),([model(0,1)],[lod(u32::MAX,2)])] {
            let (_,s)=collect(&[1],&m,&l,&[1],|&h|h,|_|true,LIMITS);
            assert_eq!((s.complete,s.refusal_flags & INVALID_SPAN),(0,INVALID_SPAN));
        }
        assert!(!valid_handles(&[1,1])); assert!(!valid_handles(&[0]));
        assert!(!valid_handles(&(1..=8193).collect::<Vec<_>>()));
        let mut n=u64::MAX;let mut flags=0;increment(&mut n,&mut flags);assert_eq!((n,flags),(u64::MAX,OVERFLOW));
        assert_eq!(std::mem::size_of::<WgrRegisteredMeshRef>(),40);
        assert_eq!(std::mem::size_of::<WgrRegisteredMeshRefSummary>(),48);
    }
}
