//! Opt-in CPU publication gate for a local shadow atlas tile.
//! A planned cache key is not a published image until its exact day/night cull
//! view, GPU depth pass, final queue submit, and successful render return occur.

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct LocalTileIdentity {
    pub tile_index: usize,
    pub key: u64,
    pub instance_epoch: u64,
    pub shape: (u32, usize, u32, u64),
    pub cull_index: usize,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct LocalTilePublication {
    generation: u64,
    published: Option<(u64, LocalTileIdentity)>,
    pending: Option<LocalTileIdentity>,
    culled: bool,
    draw_closed: bool,
    submitted: bool,
}

impl LocalTilePublication {
    pub fn needs_refresh(self) -> bool {
        self.published.is_none()
    }
    pub fn pending(self) -> Option<LocalTileIdentity> {
        self.pending
    }

    pub fn plan(&mut self, identity: LocalTileIdentity) -> bool {
        self.abort();
        if identity.instance_epoch == 0
            || identity.instance_epoch == u64::MAX
            || identity.shape.0 == 0
            || identity.shape.2 == 0
            || identity.tile_index >= 24
            || identity.tile_index >= identity.shape.2 as usize
            || identity.shape.1.checked_add(identity.tile_index) != Some(identity.cull_index)
            || identity.cull_index >= u32::BITS as usize
            || self.generation == u64::MAX
        {
            return false;
        }
        self.pending = Some(identity);
        true
    }

    pub fn cull_recorded(&mut self, cull_index: usize) {
        if self
            .pending
            .is_some_and(|identity| identity.cull_index == cull_index)
        {
            self.culled = true;
        }
    }

    pub fn draw_closed(&mut self, cull_index: usize) {
        if self
            .pending
            .is_some_and(|identity| identity.cull_index == cull_index)
        {
            self.draw_closed = true;
        }
    }

    pub fn final_submit_accepted(&mut self) {
        if self.pending.is_some() {
            self.submitted = true;
        }
    }

    pub fn commit(&mut self, current: LocalTileIdentity) -> Option<(u64, LocalTileIdentity)> {
        let pending = self.pending.take()?;
        if pending != current || !(self.culled && self.draw_closed && self.submitted) {
            self.published = None;
            return None;
        }
        let next = self.generation.checked_add(1)?;
        self.generation = next;
        self.published = Some((next, pending));
        Some((next, pending))
    }

    pub fn abort(&mut self) {
        self.pending = None;
        self.published = None;
        self.culled = false;
        self.draw_closed = false;
        self.submitted = false;
    }

    pub fn published(self) -> Option<(u64, LocalTileIdentity)> {
        self.published
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn identity(tile_index: usize, solar_count: usize) -> LocalTileIdentity {
        LocalTileIdentity {
            tile_index,
            key: 42,
            instance_epoch: 7,
            shape: (2048, solar_count, 24, 5),
            cull_index: solar_count + tile_index,
        }
    }

    #[test]
    fn day_and_night_require_exact_cull_and_closed_draw() {
        for (tile, solar) in [(0, 0), (0, 4), (1, 0), (23, 4)] {
            for missing in 0..3 {
                let mut state = LocalTilePublication::default();
                let id = identity(tile, solar);
                assert!(state.plan(id));
                if missing != 0 {
                    state.cull_recorded(id.cull_index);
                }
                if missing != 1 {
                    state.draw_closed(id.cull_index);
                }
                if missing != 2 {
                    state.final_submit_accepted();
                }
                assert_eq!(state.commit(id), None);
                assert!(state.needs_refresh());
                assert!(state.plan(id));
                state.cull_recorded(id.cull_index);
                state.draw_closed(id.cull_index);
                state.final_submit_accepted();
                assert_eq!(state.commit(id), Some((1, id)));
            }
        }
    }

    #[test]
    fn wrong_cull_route_or_changed_identity_cannot_publish() {
        let mut state = LocalTilePublication::default();
        let id = identity(1, 4);
        assert!(state.plan(id));
        state.cull_recorded(0);
        state.draw_closed(id.cull_index);
        state.final_submit_accepted();
        assert_eq!(state.commit(id), None);
        assert!(state.plan(id));
        state.cull_recorded(id.cull_index);
        state.draw_closed(id.cull_index);
        state.final_submit_accepted();
        let changed = LocalTileIdentity {
            key: id.key + 1,
            ..id
        };
        assert_eq!(state.commit(changed), None);
        assert!(state.needs_refresh());
    }

    #[test]
    fn abort_and_retry_keep_generation_monotone() {
        let mut state = LocalTilePublication::default();
        let id = identity(23, 0);
        assert!(state.plan(id));
        state.cull_recorded(id.cull_index);
        state.draw_closed(id.cull_index);
        state.final_submit_accepted();
        assert_eq!(state.commit(id), Some((1, id)));
        assert!(state.plan(id));
        assert_eq!(state.published(), None);
        state.cull_recorded(id.cull_index);
        state.draw_closed(id.cull_index);
        state.final_submit_accepted();
        state.abort();
        assert_eq!(state.commit(id), None);
        assert!(state.needs_refresh());
        assert!(state.plan(id));
        state.cull_recorded(id.cull_index);
        state.draw_closed(id.cull_index);
        state.final_submit_accepted();
        assert_eq!(state.commit(id), Some((2, id)));
    }

    #[test]
    fn invalid_epoch_or_shape_refuses_publication() {
        let mut state = LocalTilePublication::default();
        let id = identity(0, 0);
        assert!(!state.plan(LocalTileIdentity {
            instance_epoch: 0,
            ..id
        }));
        assert!(!state.plan(LocalTileIdentity {
            shape: (0, 0, 1, 5),
            ..id
        }));
        assert!(!state.plan(LocalTileIdentity { tile_index: 24, ..id }));
        assert!(!state.plan(LocalTileIdentity { cull_index: 1, ..id }));
        assert!(state.needs_refresh());
    }

    #[test]
    fn tiles_publish_independently_and_reject_changed_atlas_shape() {
        let mut states = [LocalTilePublication::default(); 24];
        let first = identity(0, 4);
        let last = identity(23, 4);
        assert!(states[0].plan(first));
        assert!(states[23].plan(last));
        states[0].cull_recorded(first.cull_index);
        states[0].draw_closed(first.cull_index);
        states[0].final_submit_accepted();
        assert_eq!(states[0].commit(first), Some((1, first)));
        assert_eq!(states[23].commit(last), None);
        assert!(!states[0].needs_refresh());
        assert!(states[23].needs_refresh());

        assert!(states[23].plan(last));
        states[23].cull_recorded(last.cull_index);
        states[23].draw_closed(last.cull_index);
        states[23].final_submit_accepted();
        assert_eq!(states[23].commit(LocalTileIdentity {
            shape: (last.shape.0, last.shape.1, last.shape.2, last.shape.3 + 1),
            ..last
        }), None);
        assert!(states[23].needs_refresh());
    }
}
