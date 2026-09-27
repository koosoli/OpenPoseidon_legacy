//! Exact bounded pose witnesses for CPU-listed LOCAL shadow casters only.
//! No proof for retained casters, interior sky, or concurrent vertex uploads.
const MAX_POSES: usize = 64;
const MAX_OBSERVATIONS: usize = 1024;
const BONES: usize = super::PALETTE_SIZE;
const WORDS: usize = BONES * 16;

pub(super) fn refresh_mask(observation: Option<(u64, bool)>, affected: u32) -> u32 {
    match observation { Some((revision, false)) if revision != 0 => 0, _ => affected }
}
pub(super) fn unwitnessed_mask(observation: Option<(u64, bool)>, affected: u32) -> u32 {
    match observation { Some((revision, _)) if revision != 0 => 0, _ => affected }
}
pub(super) fn published_key(key: u64, unwitnessed: bool) -> Option<u64> {
    if unwitnessed { None } else { Some(key) }
}
pub(super) fn reset_if_uncached(cache: &mut Option<Box<LocalPoseCache>>, enabled: bool, forced: bool) {
    if !enabled || forced { *cache = None; }
}

struct Pose {
    slot: u32,
    bits: Vec<u32>,
    revision: u64,
    frame: u64,
    changed: bool,
}
pub(super) struct LocalPoseCache {
    poses: [Option<Pose>; MAX_POSES],
    frame: u64,
    next_revision: u64,
    observations: usize,
    audits: usize,
}
impl LocalPoseCache {
    pub(super) fn new() -> Self {
        Self { poses: std::array::from_fn(|_| None), frame: 0,
            next_revision: 1, observations: 0, audits: 0 }
    }
    pub(super) fn begin_frame(&mut self) {
        self.frame = self.frame.checked_add(1).unwrap_or(0);
        self.observations = 0; self.audits = 0;
        if self.frame == 0 { self.next_revision = 0; } // Never recycle witnesses.
    }
    /// Actual submitted-frame palette is immutable throughout this operation.
    /// None means refresh this caster's affected views, never accept an old pose.
    /// At most64 block audits/copies (512KiB) and1024 observations per frame.
    pub(super) fn observe(&mut self, slot: u32, palette: &[[f32; 16]]) -> Option<(u64, bool)> {
        if self.frame == 0 || self.next_revision == 0 || self.observations >= MAX_OBSERVATIONS { return None; }
        self.observations += 1;
        let found = self.poses.iter().position(|p| p.as_ref().is_some_and(|p| p.slot == slot));
        if let Some(pose) = found.and_then(|i| self.poses[i].as_ref()) {
            if pose.frame == self.frame { return Some((pose.revision, pose.changed)); }
        }
        if self.audits >= MAX_POSES { return None; }
        self.audits += 1;
        let start = (slot as usize).checked_mul(BONES)?;
        let block = palette.get(start..start.checked_add(BONES)?)?;
        if block.iter().flatten().any(|v| !v.is_finite()) { return None; }
        let bits: &[u32] = bytemuck::cast_slice(block);
        let index = found.or_else(|| self.poses.iter().position(|p| p.is_none()))
            .or_else(|| self.poses.iter().position(|p| p.as_ref().is_some_and(|p| p.frame != self.frame)))?;
        let same = self.poses[index].as_ref().is_some_and(|p| p.slot == slot && p.bits == bits);
        if same {
            let pose = self.poses[index].as_mut().unwrap();
            pose.frame = self.frame; pose.changed = false;
            return Some((pose.revision, false));
        }
        let revision = self.next_revision;
        self.next_revision = self.next_revision.checked_add(1).unwrap_or(0);
        let mut storage = self.poses[index].take().map_or_else(Vec::new, |p| p.bits);
        if storage.capacity() < WORDS && storage.try_reserve_exact(WORDS).is_err() { return None; }
        storage.clear(); storage.extend_from_slice(bits);
        self.poses[index] = Some(Pose { slot, bits: storage, revision, frame: self.frame, changed: true });
        Some((revision, true))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn palette(slots: usize) -> Vec<[f32; 16]> {
        let mut p = vec![[0.0; 16]; slots * BONES];
        for m in &mut p { m[0] = 1.0; m[5] = 1.0; m[10] = 1.0; m[15] = 1.0; }
        p
    }
    #[test]
    fn unchanged_pose_reuses_exact_witness_but_same_slot_motion_changes_it() {
        let mut cache = LocalPoseCache::new(); let mut p = palette(2);
        cache.begin_frame(); let original = cache.observe(0, &p).unwrap();
        assert!(original.1);
        assert_eq!(cache.observe(0, &p), Some(original)); // duplicate sibling caster
        cache.begin_frame(); assert_eq!(cache.observe(0, &p), Some((original.0, false)));
        p[4][12] = 3.0; // stationary root, same slot; one bone's pose changed
        cache.begin_frame(); let moved = cache.observe(0, &p).unwrap();
        assert!(moved.1); assert_ne!(moved.0, original.0);
        assert_eq!(cache.observe(0, &p), Some(moved));
        assert!(cache.observe(1, &p).unwrap().1);
        cache.begin_frame(); assert!(!cache.observe(0, &p).unwrap().1);
        assert!(!cache.observe(1, &p).unwrap().1);
    }
    #[test]
    fn invalid_ranges_and_nonfinite_values_never_validate_previous_pose() {
        let mut cache = LocalPoseCache::new(); let mut p = palette(1);
        cache.begin_frame(); cache.observe(0, &p).unwrap();
        cache.begin_frame(); assert!(cache.observe(1, &p).is_none());
        assert!(cache.observe(u32::MAX, &p).is_none());
        p[17][6] = f32::NAN; assert!(cache.observe(0, &p).is_none());
        p[17][6] = f32::INFINITY; assert!(cache.observe(0, &p).is_none());
        assert!(cache.observe(0, &p[..BONES - 1]).is_none());
    }
    #[test]
    fn bounds_refuse_only_extra_dependencies_and_replaced_slots_get_new_witnesses() {
        let mut cache = LocalPoseCache::new(); let p = palette(MAX_POSES + 1);
        cache.begin_frame(); let first = cache.observe(0, &p).unwrap();
        for slot in 1..MAX_POSES { cache.observe(slot as u32, &p).unwrap(); }
        assert!(cache.observe(MAX_POSES as u32, &p).is_none());
        assert_eq!(cache.observe(0, &p), Some(first));
        cache.begin_frame(); let replacement = cache.observe(MAX_POSES as u32, &p).unwrap();
        assert!(replacement.1); assert_ne!(replacement.0, first.0);
        let restored = cache.observe(0, &p).unwrap();
        assert!(restored.1); assert_ne!(restored.0, first.0);
        for _ in 0..MAX_OBSERVATIONS { cache.observe(0, &p); }
        assert!(cache.observe(0, &p).is_none());
    }
    #[test]
    fn counter_exhaustion_never_recycles_a_pose_revision() {
        let mut cache = LocalPoseCache::new(); let p = palette(1);
        cache.frame = u64::MAX; cache.begin_frame(); assert!(cache.observe(0, &p).is_none());
        cache.begin_frame(); assert!(cache.observe(0, &p).is_none());
    }
    #[test]
    fn changed_duplicate_casters_refresh_only_their_views_without_xor_cancellation() {
        let mut cache = LocalPoseCache::new(); let mut p = palette(1);
        cache.begin_frame(); cache.observe(0, &p).unwrap();
        cache.begin_frame(); assert_eq!(refresh_mask(cache.observe(0, &p), 0b0101), 0);
        p[3][9] = 0.125; cache.begin_frame();
        let first = refresh_mask(cache.observe(0, &p), 0b0101);
        let duplicate = refresh_mask(cache.observe(0, &p), 0b0101);
        assert_eq!(first | duplicate, 0b0101);
        assert_eq!(refresh_mask(None, 0b0010), 0b0010);
        assert_eq!(refresh_mask(Some((0, false)), 0b1000), 0b1000);
    }
    #[test]
    fn refused_pose_tile_is_not_reused_when_old_valid_pose_returns() {
        let mut cache = LocalPoseCache::new(); let valid = palette(1); let mut bad = valid.clone();
        cache.begin_frame(); cache.observe(0, &valid).unwrap();
        let ordinary_key = 19; let unaffected = Some(31);
        bad[9][12] = f32::NAN; cache.begin_frame(); let observation = cache.observe(0, &bad);
        assert_eq!(refresh_mask(observation, 0b0010), 0b0010);
        assert_eq!(unwitnessed_mask(observation, 0b0010), 0b0010);
        let previous_key = published_key(ordinary_key, true);
        assert_eq!(previous_key, None);
        assert_eq!(published_key(31, false), unaffected);
        cache.begin_frame(); let restored = cache.observe(0, &valid);
        assert_eq!(refresh_mask(restored, 0b0010), 0); // old exact witness still matches
        assert_ne!(previous_key, Some(ordinary_key)); // production stale test MUST refresh
        assert_eq!(published_key(ordinary_key, false), Some(ordinary_key));
    }
    #[test]
    fn disabling_or_forcing_refresh_discards_old_pose_witnesses_without_allocating() {
        for (enabled, forced) in [(false, false), (true, true)] {
            let p = palette(1); let mut cache = Some(Box::new(LocalPoseCache::new()));
            cache.as_mut().unwrap().begin_frame(); cache.as_mut().unwrap().observe(0, &p).unwrap();
            reset_if_uncached(&mut cache, enabled, forced); assert!(cache.is_none());
            reset_if_uncached(&mut cache, enabled, forced); assert!(cache.is_none());
            reset_if_uncached(&mut cache, true, false); assert!(cache.is_none());
            let current = cache.get_or_insert_with(|| Box::new(LocalPoseCache::new()));
            current.begin_frame(); assert!(current.observe(0, &p).unwrap().1);
        }
    }
}
