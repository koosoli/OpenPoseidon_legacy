//! On-demand retained geometry attribution; no GPU readback or residency policy.
use super::{cull::LodGpu, GpuSectionSrc};
use crate::ffi::{WgrGeometryAllocationRow, WgrGeometryAllocationSummary};
use std::collections::{HashMap, HashSet};

pub(super) fn collect(
    models: &[(u32, Option<&[LodGpu]>)],
    sources: &[GpuSectionSrc],
    max_rows: usize,
    max_visits: usize,
    mut lookup: impl FnMut(u64) -> Option<(u64, u64)>,
) -> (Vec<WgrGeometryAllocationRow>, WgrGeometryAllocationSummary) {
    let mut rows = Vec::new();
    let mut summary = WgrGeometryAllocationSummary::default();
    // At most max_visits entries, created only for this diagnostic. Retain the
    // first distinct (model, LOD) reference, not section occurrence counts.
    let mut union = HashMap::new();
    for &(model_id, lods) in models {
        let Some(lods) = lods else { summary.missing_models += 1; continue };
        summary.models_visited += 1;
        summary.lods_total += lods.len() as u32;
        for (lod_index, lod) in lods.iter().enumerate() {
            if rows.len() == max_rows { summary.truncated = 1; return (rows, summary) }
            let mut row = WgrGeometryAllocationRow {
                model_id, lod_index: lod_index as u32, resolution: lod.resolution,
                section_count: lod.section_count, ..Default::default()
            };
            let start = lod.section_base as usize;
            let end = start.checked_add(lod.section_count as usize);
            let Some(sections) = end.and_then(|end| sources.get(start..end)) else {
                summary.invalid_ranges += 1;
                rows.push(row);
                continue;
            };
            // Never emit a partially inspected LOD as a complete allocation row.
            if sections.len() > max_visits.saturating_sub(summary.section_visits as usize) {
                summary.truncated = 1; return (rows, summary)
            }
            let mut seen = HashSet::new();
            for section in sections {
                summary.section_visits += 1;
                if !seen.insert(section.mesh) { continue }
                let Some((vertex_bytes, index_bytes)) = lookup(section.mesh) else {
                    row.missing_meshes += 1; summary.missing_meshes += 1; continue
                };
                row.live_meshes += 1;
                row.vertex_bytes += vertex_bytes;
                row.index_bytes += index_bytes;
                match union.entry(section.mesh) {
                    std::collections::hash_map::Entry::Vacant(entry) => {
                        entry.insert(((model_id, lod_index), false));
                        summary.unique_meshes += 1;
                        summary.unique_vertex_bytes += vertex_bytes;
                        summary.unique_index_bytes += index_bytes;
                    }
                    std::collections::hash_map::Entry::Occupied(mut entry) => {
                        let (first, overlap) = entry.get_mut();
                        if *first != (model_id, lod_index) && !*overlap {
                            *overlap = true;
                            summary.overlap_within_inspected_meshes += 1;
                            summary.overlap_within_inspected_vertex_bytes += vertex_bytes;
                            summary.overlap_within_inspected_index_bytes += index_bytes;
                        }
                    }
                }
            }
            rows.push(row);
        }
    }
    (rows, summary)
}

#[cfg(test)]
mod tests {
    use super::*;
    fn section(mesh: u64) -> GpuSectionSrc { GpuSectionSrc { mesh, index_begin: 0, index_count: 3, variant: 0 } }
    fn lod(base: u32, count: u32) -> LodGpu { LodGpu { resolution: 1., section_base: base, section_count: count, is_decal: 0 } }
    #[test]
    fn shared_sections_lods_and_models_charge_union_once() {
        let levels = [lod(0, 2), lod(2, 1)];
        let other = [lod(2, 1)];
        let sources = [section(7), section(7), section(7)];
        let (rows, s) = collect(&[(1, Some(&levels)), (2, Some(&other))], &sources, 8, 8, |_| Some((136, 12)));
        assert_eq!(rows.len(), 3); assert!(rows.iter().all(|r| r.vertex_bytes == 136 && r.index_bytes == 12 && r.live_meshes == 1));
        assert_eq!((s.unique_meshes, s.unique_vertex_bytes, s.unique_index_bytes), (1, 136, 12));
        assert_eq!(s.section_visits, 4);
        assert_eq!((s.overlap_within_inspected_meshes, s.overlap_within_inspected_vertex_bytes,
            s.overlap_within_inspected_index_bytes), (1, 136, 12));
    }
    #[test]
    fn duplicate_sections_and_repeated_model_requests_do_not_imply_overlap() {
        let levels = [lod(0, 2)]; let sources = [section(7), section(7)];
        let (_, s) = collect(&[(1, Some(&levels)), (1, Some(&levels))], &sources, 8, 8, |_| Some((136, 12)));
        assert_eq!(s.unique_meshes, 1); assert_eq!(s.section_visits, 4);
        assert_eq!((s.overlap_within_inspected_meshes, s.overlap_within_inspected_vertex_bytes,
            s.overlap_within_inspected_index_bytes), (0, 0, 0));
    }
    #[test]
    fn overlap_counts_unique_allocations_not_extra_references_or_unshared_meshes() {
        let first = [lod(0, 2), lod(2, 1)]; let second = [lod(2, 1)];
        let sources = [section(7), section(8), section(7)];
        let (_, s) = collect(&[(1, Some(&first)), (2, Some(&second))], &sources, 8, 8,
            |id| Some(if id == 7 { (136, 12) } else { (68, 4) }));
        assert_eq!((s.unique_meshes, s.unique_vertex_bytes, s.unique_index_bytes), (2, 204, 16));
        assert_eq!((s.overlap_within_inspected_meshes, s.overlap_within_inspected_vertex_bytes,
            s.overlap_within_inspected_index_bytes), (1, 136, 12));
    }
    #[test]
    fn missing_ranges_and_handles_never_fabricate_bytes() {
        let levels = [lod(0, 1), lod(99, 1)];
        let (rows, s) = collect(&[(1, Some(&levels)), (9, None)], &[section(0)], 8, 8, |_| None);
        assert_eq!(rows[0].missing_meshes, 1); assert_eq!(s.missing_meshes, 1);
        assert_eq!(s.invalid_ranges, 1); assert_eq!(s.missing_models, 1);
        assert_eq!(s.unique_vertex_bytes + s.unique_index_bytes, 0);
        assert_eq!(s.overlap_within_inspected_meshes, 0);
    }
    #[test]
    fn row_and_visit_limits_do_not_publish_partial_lods() {
        let levels = [lod(0, 1), lod(1, 2)]; let sources = [section(1), section(2), section(3)];
        for (rows_cap, visit_cap) in [(1, 8), (8, 2)] {
            let (rows, s) = collect(&[(1, Some(&levels))], &sources, rows_cap, visit_cap, |_| Some((68, 4)));
            assert_eq!(rows.len(), 1); assert_eq!(s.truncated, 1);
            assert_eq!(s.section_visits, 1); assert_eq!(s.unique_meshes, 1);
        }
    }
    #[test]
    fn truncation_reports_only_overlap_in_fully_inspected_rows() {
        let levels = [lod(0, 1), lod(1, 1), lod(2, 1)];
        let sources = [section(7), section(7), section(7)];
        for (rows_cap, visit_cap, expected_overlap) in [(1, 8, 0), (8, 1, 0), (2, 8, 1), (8, 2, 1)] {
            let (rows, s) = collect(&[(1, Some(&levels))], &sources, rows_cap, visit_cap, |_| Some((136, 12)));
            assert_eq!(rows.len(), if expected_overlap == 0 { 1 } else { 2 });
            assert_eq!(s.truncated, 1); assert_eq!(s.unique_meshes, 1);
            assert_eq!(s.overlap_within_inspected_meshes, expected_overlap);
            assert_eq!(s.overlap_within_inspected_vertex_bytes, 136 * expected_overlap);
            assert_eq!(s.overlap_within_inspected_index_bytes, 12 * expected_overlap);
        }
    }
}
