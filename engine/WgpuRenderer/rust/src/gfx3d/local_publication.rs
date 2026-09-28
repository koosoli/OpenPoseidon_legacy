//! Opt-in CPU publication gate for the first local shadow atlas tile.
//! A planned cache key is not a published image until its exact day/night cull
//! view, GPU depth pass, final queue submit, and successful render return occur.

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Local0Identity {
    pub key: u64,
    pub instance_epoch: u64,
    pub shape: (u32, usize, u32, u64),
    pub cull_index: usize,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Local0Publication {
    generation: u64,
    published: Option<(u64, Local0Identity)>,
    pending: Option<Local0Identity>,
    culled: bool,
    draw_closed: bool,
    submitted: bool,
}

impl Local0Publication {
    pub fn needs_refresh(self) -> bool {
        self.published.is_none()
    }
    pub fn pending(self) -> Option<Local0Identity> {
        self.pending
    }

    pub fn plan(&mut self, identity: Local0Identity) -> bool {
        self.abort();
        if identity.instance_epoch == 0
            || identity.instance_epoch == u64::MAX
            || identity.shape.0 == 0
            || identity.shape.2 == 0
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

    pub fn commit(&mut self, current: Local0Identity) -> Option<(u64, Local0Identity)> {
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

    #[cfg(test)]
    pub fn published(self) -> Option<(u64, Local0Identity)> {
        self.published
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn identity(cull_index: usize) -> Local0Identity {
        Local0Identity {
            key: 42,
            instance_epoch: 7,
            shape: (2048, cull_index, 1, 5),
            cull_index,
        }
    }

    #[test]
    fn day_and_night_require_exact_cull_and_closed_draw() {
        for index in [0, 4] {
            for missing in 0..3 {
                let mut state = Local0Publication::default();
                let id = identity(index);
                assert!(state.plan(id));
                if missing != 0 {
                    state.cull_recorded(index);
                }
                if missing != 1 {
                    state.draw_closed(index);
                }
                if missing != 2 {
                    state.final_submit_accepted();
                }
                assert_eq!(state.commit(id), None);
                assert!(state.needs_refresh());
                assert!(state.plan(id));
                state.cull_recorded(index);
                state.draw_closed(index);
                state.final_submit_accepted();
                assert_eq!(state.commit(id), Some((1, id)));
            }
        }
    }

    #[test]
    fn wrong_cull_route_or_changed_identity_cannot_publish() {
        let mut state = Local0Publication::default();
        let id = identity(4);
        assert!(state.plan(id));
        state.cull_recorded(0);
        state.draw_closed(4);
        state.final_submit_accepted();
        assert_eq!(state.commit(id), None);
        assert!(state.plan(id));
        state.cull_recorded(4);
        state.draw_closed(4);
        state.final_submit_accepted();
        let changed = Local0Identity {
            key: id.key + 1,
            ..id
        };
        assert_eq!(state.commit(changed), None);
        assert!(state.needs_refresh());
    }

    #[test]
    fn abort_and_retry_keep_generation_monotone() {
        let mut state = Local0Publication::default();
        let id = identity(0);
        assert!(state.plan(id));
        state.cull_recorded(0);
        state.draw_closed(0);
        state.final_submit_accepted();
        assert_eq!(state.commit(id), Some((1, id)));
        assert!(state.plan(id));
        assert_eq!(state.published(), None);
        state.cull_recorded(0);
        state.draw_closed(0);
        state.final_submit_accepted();
        state.abort();
        assert_eq!(state.commit(id), None);
        assert!(state.needs_refresh());
        assert!(state.plan(id));
        state.cull_recorded(0);
        state.draw_closed(0);
        state.final_submit_accepted();
        assert_eq!(state.commit(id), Some((2, id)));
    }

    #[test]
    fn invalid_epoch_or_shape_refuses_publication() {
        let mut state = Local0Publication::default();
        let id = identity(0);
        assert!(!state.plan(Local0Identity {
            instance_epoch: 0,
            ..id
        }));
        assert!(!state.plan(Local0Identity {
            shape: (0, 0, 1, 5),
            ..id
        }));
        assert!(state.needs_refresh());
    }
}
