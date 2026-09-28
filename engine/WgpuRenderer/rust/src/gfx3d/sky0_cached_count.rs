//! Private COUNT retained only for the exact published zenith cache image.
//! The readback identity is a request identity; the cache identity also needs
//! the publication and texture generations and the view used to draw it.

use super::{cull::{MainCountFact, MainCountIdentity, MainCountStatus}, SkyVisView};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Key {
    pub publication: u64,
    pub target: u64,
    pub view: [u32; 18],
    pub epoch: u64,
}

impl Key {
    pub fn new(publication: u64, target: u64, view: SkyVisView, epoch: u64) -> Self {
        let mut bits = [0; 18];
        bits[..16].copy_from_slice(&view.view_proj.to_cols_array().map(f32::to_bits));
        bits[16] = view.kernel_uv.to_bits();
        bits[17] = view.bias_ndc.to_bits();
        Self { publication, target, view: bits, epoch }
    }
}

#[derive(Clone, Copy, Debug, Default)]
pub struct CachedCount {
    bound: Option<(Key, MainCountFact)>,
}

impl CachedCount {
    pub fn invalidate(&mut self) { self.bound = None; }

    pub fn source_changed(&mut self, identity: MainCountIdentity) {
        if self.bound.is_some_and(|(_, fact)|
            fact.identity.model_id != identity.model_id ||
            fact.identity.source_generation != identity.source_generation) {
            self.invalidate();
        }
    }

    pub fn publish(&mut self, key: Key, identity: MainCountIdentity) {
        self.bound = (key.publication != 0 && key.epoch == identity.cull_epoch &&
            identity.token != 0 && identity.source_generation != 0)
            .then_some((key, MainCountFact { identity, ..MainCountFact::default() }));
    }

    pub fn observe(&mut self, raw: MainCountFact) {
        let Some((_, fact)) = self.bound.as_mut() else { return; };
        if raw.identity != fact.identity { return; }
        if matches!(raw.status, MainCountStatus::Present) && raw.count > 0 ||
            matches!(raw.status, MainCountStatus::Absent) && raw.count == 0 {
            *fact = raw;
        }
    }

    pub fn fact(&self, current: Option<Key>) -> MainCountFact {
        let Some((key, fact)) = self.bound else { return MainCountFact::default(); };
        if Some(key) == current { fact } else { MainCountFact::default() }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn key(generation: u64) -> Key {
        Key::new(generation, 3, super::super::sky_vis::build_views(
            glam::Vec3::ZERO, &super::super::sky_vis::SkyVisSettings::default())[0], 7)
    }
    fn id(token: u64, source_generation: u64) -> MainCountIdentity {
        MainCountIdentity { token, model_id: 4, source_generation, cull_epoch: 7 }
    }
    #[test]
    fn retains_only_mapped_result_for_the_exact_cached_generation() {
        for (status, count) in [(MainCountStatus::Present, 8), (MainCountStatus::Absent, 0)] {
            let mut cache = CachedCount::default();
            cache.publish(key(1), id(2, 9));
            assert_eq!(cache.fact(Some(key(1))).status, MainCountStatus::Unknown);
            cache.observe(MainCountFact { identity: id(2, 9), status, count });
            assert_eq!(cache.fact(Some(key(1))).status, status);
            assert_eq!(cache.fact(Some(key(1))).count, count);
            assert_eq!(cache.fact(Some(key(2))).status, MainCountStatus::Unknown);
            let mut changed = key(1); changed.target += 1;
            assert_eq!(cache.fact(Some(changed)).status, MainCountStatus::Unknown);
            changed = key(1); changed.view[0] ^= 1;
            assert_eq!(cache.fact(Some(changed)).status, MainCountStatus::Unknown);
            changed = key(1); changed.epoch += 1;
            assert_eq!(cache.fact(Some(changed)).status, MainCountStatus::Unknown);
            cache.source_changed(id(3, 9));
            assert_eq!(cache.fact(Some(key(1))).status, status);
            cache.source_changed(id(4, 10));
            assert_eq!(cache.fact(Some(key(1))).status, MainCountStatus::Unknown);
        }
    }

    #[test]
    fn refresh_abort_and_late_old_map_cannot_revive_a_fact() {
        let mut cache = CachedCount::default();
        cache.publish(key(1), id(2, 9));
        cache.invalidate();
        cache.observe(MainCountFact { identity: id(2, 9), status: MainCountStatus::Absent, count: 0 });
        assert_eq!(cache.fact(Some(key(1))).status, MainCountStatus::Unknown);
        cache.publish(key(2), id(3, 9));
        cache.observe(MainCountFact { identity: id(2, 9), status: MainCountStatus::Present, count: 8 });
        assert_eq!(cache.fact(Some(key(2))).status, MainCountStatus::Unknown);
        cache.observe(MainCountFact { identity: id(3, 9), status: MainCountStatus::Present, count: 8 });
        assert_eq!(cache.fact(Some(key(2))).status, MainCountStatus::Present);
        cache.invalidate();
        assert_eq!(cache.fact(Some(key(2))).status, MainCountStatus::Unknown);
    }
}
