//! A private COUNT answer belongs to one published GI RSM image, never merely
//! to a readback token. View 5 is independent of solar shadow view 4.

use super::{cached_count_equivalence::{self, Relation}, cull::{CountCullInputKey, MainCountFact, MainCountIdentity, MainCountStatus}, SkyVisView};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Key {
    pub publication: u64,
    pub image: u64,
    pub view: [u32; 18],
    pub epoch: u64,
    pub cull: CountCullInputKey,
    pub dynamic_serial: u64,
    pub palette_serial: u64,
}

impl Key {
    pub fn new(publication: u64, image: u64, view: SkyVisView, epoch: u64,
               cull: CountCullInputKey) -> Self {
        let mut bits = [0; 18];
        bits[..16].copy_from_slice(&view.view_proj.to_cols_array().map(f32::to_bits));
        bits[16] = view.kernel_uv.to_bits();
        bits[17] = view.bias_ndc.to_bits();
        Self { publication, image, view: bits, epoch, cull, dynamic_serial: 0, palette_serial: 0 }
    }

    pub fn with_dynamic_serial(mut self, serial: u64) -> Self {
        self.dynamic_serial = serial;
        self
    }

    pub fn with_palette_serial(mut self, serial: u64) -> Self {
        self.palette_serial = serial;
        self
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
        self.bound = (key.publication != 0 && key.image != 0 &&
            key.epoch == identity.cull_epoch && identity.token != 0 &&
            identity.source_generation != 0)
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

    pub fn diagnose(&self, candidate: Option<Key>, target: Option<(u32, u64, u64)>,
                    current_epoch: u64) -> Relation {
        let (Some((key, fact)), Some(candidate)) = (self.bound, candidate) else {
            return Relation::Unknown;
        };
        let same_image = key.publication == candidate.publication &&
            key.image == candidate.image && key.view == candidate.view &&
            key.epoch == candidate.epoch && key.dynamic_serial == candidate.dynamic_serial &&
            key.palette_serial == candidate.palette_serial;
        cached_count_equivalence::classify(fact, same_image, key.cull == candidate.cull,
            target, key.epoch, current_epoch)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use bytemuck::Zeroable;

    #[test]
    fn foreign_dynamic_caster_invalidates_cached_absence_without_epoch_change() {
        let mut cache = CachedCount::default();
        let published = key(1).with_dynamic_serial(9);
        let identity = id(1, 4, 9);
        cache.publish(published, identity);
        cache.observe(MainCountFact { identity, status: MainCountStatus::Absent, count: 0 });
        assert_eq!(cache.fact(Some(published)).status, MainCountStatus::Absent);
        let moved_caster = published.with_dynamic_serial(10);
        assert_eq!(cache.fact(Some(moved_caster)).status, MainCountStatus::Unknown);
        assert_eq!(cache.diagnose(Some(moved_caster), Some((4, 9, 1)), 7),
            Relation::Unknown);
    }

    #[test]
    fn foreign_palette_change_invalidates_cached_absence_with_stable_target_revision() {
        let mut cache = CachedCount::default();
        let published = key(1).with_palette_serial(2);
        let identity = id(1, 4, 9);
        cache.publish(published, identity);
        cache.observe(MainCountFact { identity, status: MainCountStatus::Absent, count: 0 });
        assert_eq!(cache.fact(Some(published)).status, MainCountStatus::Absent);
        let changed = published.with_palette_serial(3);
        assert_eq!(cache.fact(Some(changed)).status, MainCountStatus::Unknown);
        assert_eq!(cache.diagnose(Some(changed), Some((4, 9, 1)), 7), Relation::Unknown);
        assert_eq!(cache.fact(None).status, MainCountStatus::Unknown);
    }

    #[test]
    fn changed_cull_input_rejects_published_absence() {
        let mut cache = CachedCount::default();
        let old = key(1);
        let identity = id(1, 4, 9);
        cache.publish(old, identity);
        cache.observe(MainCountFact { identity, status: MainCountStatus::Absent, count: 0 });
        assert_eq!(cache.fact(Some(old)).status, MainCountStatus::Absent);
        let mut params = super::super::cull::CullParamsGpu::zeroed();
        params.objects_z2 = 1.0;
        let changed = Key { cull: CountCullInputKey::from_params(&params), ..old };
        assert_eq!(cache.fact(Some(changed)).status, MainCountStatus::Unknown);
        assert_eq!(cache.diagnose(Some(changed), Some((4, 9, 1)), 8),
            Relation::CountInputsChanged);
        assert_eq!(cache.diagnose(Some(old), Some((4, 9, 1)), 8),
            Relation::CountEquivalentImageStale);
        assert_eq!(cache.diagnose(Some(Key { image: 4, ..old }), Some((4, 9, 1)), 8),
            Relation::Unknown);
        assert_eq!(cache.fact(Some(Key { epoch: 8, ..old })).status,
            MainCountStatus::Unknown);
    }

    fn key(generation: u64) -> Key {
        Key::new(generation, 3, super::super::sky_vis::build_views(
            glam::Vec3::ZERO, &super::super::sky_vis::SkyVisSettings::default())[0], 7,
            CountCullInputKey::default())
    }
    fn id(token: u64, model_id: u32, source_generation: u64) -> MainCountIdentity {
        MainCountIdentity { token, model_id, source_generation, cull_epoch: 7, target_revision: 1 }
    }
    #[test]
    fn mapped_count_requires_exact_publication_image_view_epoch_model_and_source() {
        for (status, count) in [(MainCountStatus::Present, 8), (MainCountStatus::Absent, 0)] {
            let mut cache = CachedCount::default();
            cache.publish(key(1), id(2, 4, 9));
            assert_eq!(cache.fact(Some(key(1))).status, MainCountStatus::Unknown);
            cache.observe(MainCountFact { identity: id(2, 4, 9), status, count });
            assert_eq!(cache.fact(Some(key(1))).status, status);
            for changed in [
                key(2), Key { image: 4, ..key(1) },
                Key { epoch: 8, ..key(1) },
                Key { view: { let mut v = key(1).view; v[0] ^= 1; v }, ..key(1) },
            ] {
                assert_eq!(cache.fact(Some(changed)).status, MainCountStatus::Unknown);
            }
            cache.source_changed(id(3, 4, 9));
            assert_eq!(cache.fact(Some(key(1))).status, status);
            cache.source_changed(id(4, 5, 9));
            assert_eq!(cache.fact(Some(key(1))).status, MainCountStatus::Unknown);
        }
    }

    #[test]
    fn drawless_refresh_abort_and_late_old_map_fail_closed() {
        let mut cache = CachedCount::default();
        cache.publish(Key { publication: 0, ..key(1) }, id(2, 4, 9));
        assert_eq!(cache.fact(Some(key(1))).status, MainCountStatus::Unknown);
        cache.publish(Key { image: 0, ..key(1) }, id(2, 4, 9));
        assert_eq!(cache.fact(Some(key(1))).status, MainCountStatus::Unknown);
        cache.publish(key(1), MainCountIdentity { cull_epoch: 8, ..id(2, 4, 9) });
        assert_eq!(cache.fact(Some(key(1))).status, MainCountStatus::Unknown);
        cache.publish(key(1), id(2, 4, 9));
        cache.observe(MainCountFact { identity: id(2, 5, 9), status: MainCountStatus::Present, count: 8 });
        assert_eq!(cache.fact(Some(key(1))).status, MainCountStatus::Unknown);
        cache.invalidate();
        cache.observe(MainCountFact { identity: id(2, 4, 9), status: MainCountStatus::Absent, count: 0 });
        assert_eq!(cache.fact(Some(key(1))).status, MainCountStatus::Unknown);
        cache.publish(key(2), id(3, 4, 9));
        cache.observe(MainCountFact { identity: id(2, 4, 9), status: MainCountStatus::Present, count: 8 });
        assert_eq!(cache.fact(Some(key(2))).status, MainCountStatus::Unknown);
        cache.observe(MainCountFact { identity: id(3, 4, 9), status: MainCountStatus::Present, count: 8 });
        assert_eq!(cache.fact(Some(key(2))).status, MainCountStatus::Present);
        cache.source_changed(id(4, 4, 10));
        assert_eq!(cache.fact(Some(key(2))).status, MainCountStatus::Unknown);
    }
}
