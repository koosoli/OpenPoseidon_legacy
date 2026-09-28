//! Private COUNT retained only for the exact published local shadow tile.
//! The request identity is separate from the atlas allocation and tile image identity.

use super::{cull::{MainCountFact, MainCountIdentity, MainCountStatus}, local_publication::LocalTileIdentity};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Key {
    pub publication: u64,
    pub tile: LocalTileIdentity,
}

impl Key {
    pub fn current(publication: u64, tile: LocalTileIdentity,
                   target_shape: (u32, usize, u32, u64),
                   cache_shape: (u32, usize, u32, u64), target_layers: u32,
                   rendered_key: Option<u64>, epoch: u64) -> Option<Self> {
        let (res, first_local, local_count, _) = target_shape;
        (publication != 0 && tile.tile_index < 24 && tile.tile_index < local_count as usize &&
            res != 0 && local_count != 0 &&
            tile.shape == target_shape && cache_shape == target_shape &&
            first_local.checked_add(1).is_some_and(|layers| layers == target_layers as usize) &&
            first_local.checked_add(tile.tile_index) == Some(tile.cull_index) &&
            tile.instance_epoch == epoch &&
            rendered_key == Some(tile.key))
            .then_some(Self { publication, tile })
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
}

#[cfg(test)]
mod tests {
    use super::*;

    fn key(generation: u64, solar: usize) -> Key {
        Key { publication: generation, tile: LocalTileIdentity {
            tile_index: 0, key: 42, instance_epoch: 7,
            shape: (2048, solar, 1, 5), cull_index: solar,
        }}
    }
    fn id(token: u64, source_generation: u64) -> MainCountIdentity {
        MainCountIdentity { token, model_id: 4, source_generation, cull_epoch: 7, target_revision: 1 }
    }
    fn local_key(tile_index: usize, solar: usize, generation: u64) -> Key {
        Key { publication: generation, tile: LocalTileIdentity {
            tile_index, key: 42 + tile_index as u64, instance_epoch: 7,
            shape: (2048, solar, 24, 5), cull_index: solar + tile_index,
        }}
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
                Key::current(publication, tile, target_shape, cache_shape, layers, rendered_key, epoch);
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
                    Key::current(1, candidate, target, cache, layers, rendered, epoch);
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
