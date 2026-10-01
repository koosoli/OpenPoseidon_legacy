//! Validate complete simulation ownership before publishing any fine/coarse draw pair.
use crate::ffi::{WgrRainWaterFineCell, WgrRainWaterParams, WgrRainWaterSourceKey};
use std::collections::BTreeMap;

pub(super) const MAX_RECORDS: usize = 8192;
pub(super) const TEXTURE_WIDTH: u32 = 1024;
pub(super) const TEXTURE_HEIGHT: u32 = 24;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) struct SourceIdentity(pub [u64; 3], pub u32, pub u32);
pub(super) fn source_identity(key: WgrRainWaterSourceKey) -> Option<SourceIdentity> {
    (key.world_token != 0
        && key.terrain_range >= 2
        && key.terrain_spacing.is_finite()
        && key.terrain_spacing > 0.0
        && key.terrain_spacing < 1_000_000.0)
        .then_some(SourceIdentity(
            [key.world_token, key.generation, key.height_revision],
            key.terrain_range,
            key.terrain_spacing.to_bits(),
        ))
}

#[repr(C)]
#[derive(Clone, Copy, Debug, bytemuck::Pod, bytemuck::Zeroable)]
pub(super) struct TileMask {
    pub meta: [u32; 4],       // count, record count, reserved
    pub rects: [[f32; 4]; 8], // lower x/z, exclusive upper x/z
}
const _: () = assert!(std::mem::size_of::<TileMask>() == 144);

#[cfg(test)]
pub(super) fn fine_bed(corners: [f32; 4], uv: [f32; 2]) -> f32 {
    let [a, b, c, d] = corners;
    let [x, z] = uv;
    if x + z <= 1.0 {
        a + (b - a) * x + (c - a) * z
    } else {
        d + (c - d) * (1.0 - x) + (b - d) * (1.0 - z)
    }
}

/// Every child and dry owner is required: a sparse wet-only list cannot mask its tile.
pub(super) fn validate_tiles(
    p: &WgrRainWaterParams,
    source: WgrRainWaterSourceKey,
    cells: &[WgrRainWaterFineCell],
    fine_ready: bool,
) -> Option<TileMask> {
    source_identity(source)?;
    let (w, h) = super::dimensions(p)?;
    if p.generation != source.generation
        || p.reserved != 0
        || p.domain[0] != 0.0
        || p.domain[1] != 0.0
    {
        return None;
    }
    let stride = p.domain[2] / source.terrain_spacing;
    if !stride.is_finite()
        || stride < 1.0
        || stride.fract() != 0.0
        || stride > 16384.0
        || !(stride as u32).is_power_of_two()
    {
        return None;
    }
    let expected = (source.terrain_range - 1).div_ceil(stride as u32) + 1;
    if w != expected || h != expected {
        return None;
    }
    let mut mask: TileMask = bytemuck::Zeroable::zeroed();
    if cells.is_empty() {
        return (!fine_ready).then_some(mask);
    }
    if !fine_ready
        || cells.len() > MAX_RECORDS
        || cells.len() % 1024 != 0
        || source.terrain_spacing != 6.25
        || p.domain[2] != 25.0
    {
        return None;
    }
    let extent = (source.terrain_range - 1) as f32 * source.terrain_spacing;
    if !extent.is_finite() {
        return None;
    }
    let mut tiles: BTreeMap<(u32, u32), [u16; 64]> = BTreeMap::new();
    for cell in cells {
        if cell.reserved != [0; 3]
            || !cell
                .rect
                .iter()
                .chain(cell.corners.iter())
                .chain(cell.flow_depth.iter())
                .all(|v| v.is_finite())
            || cell.flow_depth[3] != 0.0
            || !(0.0..=100.0).contains(&cell.flow_depth[2])
            || cell.flow_depth[0].abs() > 100.0
            || cell.flow_depth[1].abs() > 100.0
            || cell.rect[2] != 6.25
            || cell.parent as usize >= w as usize * h as usize
        {
            return None;
        }
        let x = cell.parent % w;
        let z = cell.parent / w;
        let lower = [x as f32 * 25.0 - 12.5, z as f32 * 25.0 - 12.5];
        let mut child = [0u32; 2];
        for axis in 0..2 {
            let coordinate = (cell.rect[axis] - lower[axis]) / 6.25;
            if !(0.0..=3.0).contains(&coordinate)
                || coordinate.fract() != 0.0
                || cell.rect[axis] < 0.0
                || cell.rect[axis] + 6.25 > extent
            {
                return None;
            }
            child[axis] = coordinate as u32;
        }
        let owners = tiles.entry((x / 8, z / 8)).or_insert([0; 64]);
        let bits = &mut owners[((z % 8) * 8 + x % 8) as usize];
        let bit = 1u16 << ((child[1] * 4 + child[0]) as u16);
        if *bits & bit != 0 {
            return None;
        }
        *bits |= bit;
        if tiles.len() > 8 {
            return None;
        }
    }
    for (i, ((tx, tz), owners)) in tiles.iter().enumerate() {
        if owners.iter().any(|&bits| bits != u16::MAX) {
            return None;
        }
        let lo = [*tx as f32 * 200.0 - 12.5, *tz as f32 * 200.0 - 12.5];
        if lo.iter().any(|&v| v < 0.0 || v + 200.0 > extent) {
            return None;
        }
        mask.rects[i] = [lo[0], lo[1], lo[0] + 200.0, lo[1] + 200.0];
    }
    mask.meta = [tiles.len() as u32, cells.len() as u32, 0, 0];
    Some(mask)
}

pub(super) fn packed_cells(cells: &[WgrRainWaterFineCell]) -> Vec<[f32; 4]> {
    let mut result = vec![[0.0; 4]; TEXTURE_WIDTH as usize * TEXTURE_HEIGHT as usize];
    for (i, cell) in cells.iter().enumerate() {
        result[i * 3] = cell.rect;
        result[i * 3 + 1] = cell.corners;
        result[i * 3 + 2] = cell.flow_depth;
    }
    result
}

#[cfg(test)]
pub(super) mod tests {
    use super::*;
    pub fn fixture(
        tx: u32,
        tz: u32,
    ) -> (
        WgrRainWaterParams,
        WgrRainWaterSourceKey,
        Vec<WgrRainWaterFineCell>,
    ) {
        let source = WgrRainWaterSourceKey {
            world_token: 7,
            generation: 1,
            height_revision: 0,
            terrain_range: 512,
            terrain_spacing: 6.25,
        };
        let p = WgrRainWaterParams {
            domain: [0., 0., 25., 100.],
            control: [129., 129., 0., 1.],
            generation: 1,
            reserved: 0,
        };
        let mut cells = vec![];
        for z in tz * 8..tz * 8 + 8 {
            for x in tx * 8..tx * 8 + 8 {
                for cz in 0..4 {
                    for cx in 0..4 {
                        cells.push(WgrRainWaterFineCell {
                            rect: [
                                x as f32 * 25. - 12.5 + cx as f32 * 6.25,
                                z as f32 * 25. - 12.5 + cz as f32 * 6.25,
                                6.25,
                                2.3,
                            ],
                            corners: [2.; 4],
                            flow_depth: [0., 0., 0.3, 0.],
                            parent: z * 129 + x,
                            reserved: [0; 3],
                        });
                    }
                }
            }
        }
        (p, source, cells)
    }
    #[test]
    fn fine_complete_tiles_include_dry_children_and_exact_physical_union() {
        let (p, key, mut cells) = fixture(2, 3);
        cells[0].flow_depth[2] = 0.;
        cells[0].rect[3] = 2.;
        let mask = validate_tiles(&p, key, &cells, true).unwrap();
        assert_eq!(mask.meta, [1, 1024, 0, 0]);
        assert_eq!(mask.rects[0], [387.5, 587.5, 587.5, 787.5]);
        cells.extend(fixture(4, 5).2);
        let mask = validate_tiles(&p, key, &cells, true).unwrap();
        assert_eq!(mask.meta[0], 2);
        let packed = packed_cells(&cells);
        assert_eq!(packed[0], cells[0].rect);
        assert_eq!(packed[4], cells[1].corners);
        assert_eq!(packed.len() * 16, 393216);
    }
    #[test]
    fn fine_eight_complete_tiles_are_bounded_and_ninth_is_refused(){
        let(p,key,_)=fixture(1,1);let mut cells=Vec::new();
        for i in 0..8 {cells.extend(fixture(1+i%3,1+i/3).2);}
        assert_eq!(validate_tiles(&p,key,&cells,true).unwrap().meta,[8,8192,0,0]);
        cells.extend(fixture(3,3).2);assert!(validate_tiles(&p,key,&cells,true).is_none());
    }
    #[test]
    fn fine_refuses_duplicate_partial_misaligned_border_domain_range_and_nonfinite() {
        let (p, key, cells) = fixture(2, 3);
        assert!(validate_tiles(&p, key, &cells[..1023], true).is_none());
        let mut bad = cells.clone();
        bad[1] = bad[0];
        assert!(validate_tiles(&p, key, &bad, true).is_none());
        bad = cells.clone();
        bad[0].rect[0] += 0.01;
        assert!(validate_tiles(&p, key, &bad, true).is_none());
        bad = cells.clone();
        bad[0].corners[2] = f32::NAN;
        assert!(validate_tiles(&p, key, &bad, true).is_none());
        bad = cells.clone();
        bad[0].parent = u32::MAX;
        assert!(validate_tiles(&p, key, &bad, true).is_none());
        assert!(validate_tiles(&p, key, &fixture(0, 3).2, true).is_none());
        let mut wrong = p;
        wrong.domain[0] = 10.;
        assert!(validate_tiles(&wrong, key, &cells, true).is_none());
        let mut wrong_key = key;
        wrong_key.terrain_range = 100;
        assert!(validate_tiles(&p, wrong_key, &cells, true).is_none());
        assert!(validate_tiles(&p, key, &cells, false).is_none());
        assert!(validate_tiles(&p, key, &[], true).is_none());
    }
    #[test]
    fn fine_zero_owners_legal_only_with_exact_aligned_coarse_source_and_finite_key() {
        let (mut p, mut key, _) = fixture(2, 3);
        key.terrain_spacing = 50.;
        p.domain[2] = 50.;
        p.control[0] = 512.;
        p.control[1] = 512.;
        assert_eq!(validate_tiles(&p, key, &[], false).unwrap().meta, [0; 4]);
        key.terrain_spacing = f32::INFINITY;
        assert!(validate_tiles(&p, key, &[], false).is_none());
        key.terrain_spacing = 50.;
        key.world_token = 0;
        assert!(source_identity(key).is_none());
    }
    #[test]
    fn fine_native_antidiagonal_preserves_saddle_and_horizontal_head_depth() {
        let corners = [2., 2.5, 3., 7.];
        assert_eq!(fine_bed(corners, [0.25, 0.25]), 2.375);
        assert_eq!(fine_bed(corners, [0.75, 0.75]), 4.875);
        assert_eq!(fine_bed(corners, [0.5, 0.5]), 2.75);
        assert!((3.0 - fine_bed(corners, [0.25, 0.25])) > 0.0);
        assert!((3.0 - fine_bed(corners, [0.75, 0.75])) < 0.0);
    }
}
