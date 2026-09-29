//! Private COUNT retained only for the exact published local shadow tile.
//! The request identity is separate from the atlas allocation and tile image identity.

use super::{cached_count_equivalence::{self, Relation}, cull::{CountCullInputKey, MainCountFact, MainCountIdentity, MainCountStatus}, local_publication::LocalTileIdentity};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Key {
    pub publication: u64,
    pub tile: LocalTileIdentity,
    pub cull: CountCullInputKey,
    pub dynamic_serial: u64,
    pub palette_serial: u64,
}

impl Key {
    pub fn current(publication: u64, tile: LocalTileIdentity,
                   target_shape: (u32, usize, u32, u64),
                   cache_shape: (u32, usize, u32, u64), target_layers: u32,
                   rendered_key: Option<u64>, epoch: u64, cull: CountCullInputKey) -> Option<Self> {
        let (res, first_local, local_count, _) = target_shape;
        (publication != 0 && tile.tile_index < 24 && tile.tile_index < local_count as usize &&
            res != 0 && local_count != 0 &&
            tile.shape == target_shape && cache_shape == target_shape &&
            first_local.checked_add(1).is_some_and(|layers| layers == target_layers as usize) &&
            first_local.checked_add(tile.tile_index) == Some(tile.cull_index) &&
            tile.instance_epoch == epoch &&
            rendered_key == Some(tile.key))
            .then_some(Self { publication, tile, cull, dynamic_serial: 0, palette_serial: 0 })
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
        self.bound = (key.publication != 0 && key.tile.tile_index < 24 &&
            key.tile.instance_epoch == identity.cull_epoch &&
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

    pub fn diagnose(&self, candidate: Option<Key>, target: Option<(u32, u64, u64)>,
                    current_epoch: u64) -> Relation {
        let (Some((key, fact)), Some(candidate)) = (self.bound, candidate) else {
            return Relation::Unknown;
        };
        let same_image = key.publication == candidate.publication && key.tile == candidate.tile &&
            key.dynamic_serial == candidate.dynamic_serial &&
            key.palette_serial == candidate.palette_serial;
        cached_count_equivalence::classify(fact, same_image, key.cull == candidate.cull,
            target, key.tile.instance_epoch, current_epoch)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use bytemuck::Zeroable;

    #[test]
    fn foreign_dynamic_caster_invalidates_cached_absence_without_epoch_change() {
        let mut cache = CachedCount::default();
        let published = key(1, 4).with_dynamic_serial(9);
        let identity = id(1, 9);
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
        let published = key(1, 4).with_palette_serial(2);
        let identity = id(1, 9);
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
        let old = key(1, 4);
        let identity = id(1, 9);
        cache.publish(old, identity);
        cache.observe(MainCountFact { identity, status: MainCountStatus::Absent, count: 0 });
        assert_eq!(cache.fact(Some(old)).status, MainCountStatus::Absent);
        let mut params = super::super::cull::CullParamsGpu::zeroed();
        params.frustum[0][0] = 1.0;
        let changed = Key { cull: CountCullInputKey::from_params(&params), ..old };
        assert_eq!(cache.fact(Some(changed)).status, MainCountStatus::Unknown);
        assert_eq!(cache.diagnose(Some(changed), Some((4, 9, 1)), 8),
            Relation::CountInputsChanged);
        assert_eq!(cache.diagnose(Some(old), Some((4, 9, 1)), 8),
            Relation::CountEquivalentImageStale);
        assert_eq!(cache.diagnose(Some(Key { publication: 2, ..old }), Some((4, 9, 1)), 8),
            Relation::Unknown);
        assert_eq!(cache.fact(Some(Key { tile: LocalTileIdentity {
            instance_epoch: 8, ..old.tile }, ..old })).status, MainCountStatus::Unknown);
    }

    fn key(generation: u64, solar: usize) -> Key {
        Key { publication: generation, tile: LocalTileIdentity {
            tile_index: 0, key: 42, instance_epoch: 7,
            shape: (2048, solar, 1, 5), cull_index: solar,
        }, cull: CountCullInputKey::default(), dynamic_serial: 0, palette_serial: 0 }
    }
    fn id(token: u64, source_generation: u64) -> MainCountIdentity {
        MainCountIdentity { token, model_id: 4, source_generation, cull_epoch: 7, target_revision: 1 }
    }
    fn local_key(tile_index: usize, solar: usize, generation: u64) -> Key {
        Key { publication: generation, tile: LocalTileIdentity {
            tile_index, key: 42 + tile_index as u64, instance_epoch: 7,
            shape: (2048, solar, 24, 5), cull_index: solar + tile_index,
        }, cull: CountCullInputKey::default(), dynamic_serial: 0, palette_serial: 0 }
    }

    #[test]
    fn mapped_results_follow_only_the_exact_published_tile() {
        for (status, count) in [(MainCountStatus::Present, 8), (MainCountStatus::Absent, 0)] {
            let mut cache = CachedCount::default();
            let original = key(1, 4);
            cache.publish(original, id(2, 9));
            assert_eq!(cache.fact(Some(original)).status, MainCountStatus::Unknown);
            cache.observe(MainCountFact { identity: id(2, 9), status, count });
            assert_eq!(cache.fact(Some(original)).status, status);
            assert_eq!(cache.fact(Some(original)).count, count);
            for changed in [
                key(2, 4), key(1, 0),
                Key { tile: LocalTileIdentity { key: 43, ..original.tile }, ..original },
                Key { tile: LocalTileIdentity { instance_epoch: 8, ..original.tile }, ..original },
                Key { tile: LocalTileIdentity { shape: (1024, 4, 1, 5), ..original.tile }, ..original },
                Key { tile: LocalTileIdentity { shape: (2048, 4, 1, 6), ..original.tile }, ..original },
            ] {
                assert_eq!(cache.fact(Some(changed)).status, MainCountStatus::Unknown);
            }
            cache.source_changed(id(3, 9));
            assert_eq!(cache.fact(Some(original)).status, status);
            let mut new_model = id(4, 9); new_model.model_id += 1;
            cache.source_changed(new_model);
            assert_eq!(cache.fact(Some(original)).status, MainCountStatus::Unknown);
        }
    }

    #[test]
    fn refresh_abort_and_late_map_cannot_revive_a_fact() {
        let mut cache = CachedCount::default();
        cache.publish(key(1, 4), id(2, 9));
        cache.invalidate();
        cache.observe(MainCountFact { identity: id(2, 9), status: MainCountStatus::Absent, count: 0 });
        assert_eq!(cache.fact(Some(key(1, 4))).status, MainCountStatus::Unknown);
        cache.publish(key(2, 0), id(3, 9));
        cache.observe(MainCountFact { identity: id(2, 9), status: MainCountStatus::Present, count: 8 });
        assert_eq!(cache.fact(Some(key(2, 0))).status, MainCountStatus::Unknown);
        cache.observe(MainCountFact { identity: id(3, 9), status: MainCountStatus::Present, count: 8 });
        assert_eq!(cache.fact(Some(key(2, 0))).status, MainCountStatus::Present);
        cache.invalidate();
        assert_eq!(cache.fact(Some(key(2, 0))).status, MainCountStatus::Unknown);
    }

    #[test]
    fn current_key_requires_exact_live_atlas_and_day_night_route() {
        for solar in [0, 4] {
            let original = key(1, solar);
            let tile = original.tile;
            let shape = tile.shape;
            let current = |publication, tile, target_shape, cache_shape, layers, rendered_key, epoch|
                Key::current(publication, tile, target_shape, cache_shape, layers, rendered_key, epoch,
                    CountCullInputKey::default());
            assert_eq!(current(1, tile, shape, shape, solar as u32 + 1, Some(42), 7), Some(original));
            assert_eq!(current(0, tile, shape, shape, solar as u32 + 1, Some(42), 7), None);
            assert_eq!(current(1, tile, shape, shape, solar as u32, Some(42), 7), None);
            assert_eq!(current(1, tile, shape, shape, solar as u32 + 1, Some(43), 7), None);
            assert_eq!(current(1, tile, shape, shape, solar as u32 + 1, Some(42), 8), None);
            let resized = (1024, solar, 1, shape.3);
            assert_eq!(current(1, tile, resized, resized, solar as u32 + 1, Some(42), 7), None);
            let reallocated = (shape.0, solar, 1, shape.3 + 1);
            assert_eq!(current(1, tile, reallocated, reallocated, solar as u32 + 1, Some(42), 7), None);
            let shifted = (shape.0, solar + 1, 1, shape.3);
            assert_eq!(current(1, tile, shifted, shifted, solar as u32 + 2, Some(42), 7), None);
        }
    }

    #[test]
    fn all_batch_tiles_require_exact_publication_and_atlas_route() {
        for solar in [0, 4] {
            for tile_index in 1..24 {
                let key = local_key(tile_index, solar, 1);
                let tile = key.tile;
                let shape = tile.shape;
                let live = |candidate, target, cache, layers, rendered, epoch|
                    Key::current(1, candidate, target, cache, layers, rendered, epoch,
                        CountCullInputKey::default());
                assert_eq!(live(tile, shape, shape, solar as u32 + 1, Some(tile.key), 7), Some(key));
                assert_eq!(live(tile, shape, shape, solar as u32, Some(tile.key), 7), None);
                assert_eq!(live(tile, shape, shape, solar as u32 + 1, None, 7), None);
                assert_eq!(live(tile, shape, shape, solar as u32 + 1, Some(tile.key), 8), None);
                assert_eq!(live(LocalTileIdentity { cull_index: solar, ..tile }, shape, shape,
                    solar as u32 + 1, Some(tile.key), 7), None);
                assert_eq!(live(tile, (1024, solar, 24, 5), shape,
                    solar as u32 + 1, Some(tile.key), 7), None);
                assert_eq!(live(tile, shape, (2048, solar, 24, 6),
                    solar as u32 + 1, Some(tile.key), 7), None);

                let mut count = CachedCount::default();
                count.publish(key, id(2, 9));
                count.observe(MainCountFact { identity: id(2, 9),
                    status: MainCountStatus::Absent, count: 0 });
                assert_eq!(count.fact(Some(key)).status, MainCountStatus::Absent);
                assert_eq!(count.fact(Some(local_key(tile_index, solar, 2))).status,
                    MainCountStatus::Unknown);
                assert_eq!(count.fact(Some(local_key(if tile_index == 23 { 1 } else { tile_index + 1 },
                    solar, 1))).status, MainCountStatus::Unknown);
                count.invalidate();
                count.observe(MainCountFact { identity: id(2, 9),
                    status: MainCountStatus::Absent, count: 0 });
                assert_eq!(count.fact(Some(key)).status, MainCountStatus::Unknown);
            }
        }
    }
}
