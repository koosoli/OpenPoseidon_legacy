//! Optional CPU publication witness for the cached GI sun-proxy RSM (sky view 5).
//! This records queue acceptance, not GPU completion or geometry absence.

pub const GI_CULL_VIEW: usize = super::sky_vis::DIRECTION_COUNT;

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct GiRsmPublication {
    generation: u64,
    published_epoch: Option<u64>,
    pending_epoch: Option<u64>,
    culled: bool,
    draw_closed: bool,
    submitted: bool,
}

impl GiRsmPublication {
    pub fn needs_retry(self) -> bool {
        self.published_epoch.is_none()
    }
    pub fn planned_epoch(self) -> Option<u64> {
        self.pending_epoch
    }

    pub fn plan(&mut self, epoch: u64) -> bool {
        self.abort();
        if epoch == 0 || epoch == u64::MAX || self.generation == u64::MAX {
            return false;
        }
        self.pending_epoch = Some(epoch);
        true
    }

    pub fn cull_recorded(&mut self, view: usize) {
        if view == GI_CULL_VIEW && self.pending_epoch.is_some() {
            self.culled = true;
        }
    }

    pub fn draw_closed(&mut self, view: usize) {
        if view == GI_CULL_VIEW && self.pending_epoch.is_some() {
            self.draw_closed = true;
        }
    }

    pub fn final_submit_accepted(&mut self) {
        if self.pending_epoch.is_some() {
            self.submitted = true;
        }
    }

    pub fn commit(&mut self, epoch: u64) -> Option<(u64, u64)> {
        let planned = self.pending_epoch.take()?;
        if planned != epoch || !(self.culled && self.draw_closed && self.submitted) {
            return None;
        }
        let next = self.generation.checked_add(1)?;
        self.generation = next;
        self.published_epoch = Some(epoch);
        Some((next, epoch))
    }

    pub fn abort(&mut self) {
        self.pending_epoch = None;
        self.published_epoch = None;
        self.culled = false;
        self.draw_closed = false;
        self.submitted = false;
    }

    pub fn published(self) -> Option<(u64, u64)> {
        self.published_epoch.map(|epoch| (self.generation, epoch))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn view5_needs_fresh_cull_closed_draw_final_submit_and_success() {
        for missing in 0..3 {
            let mut state = GiRsmPublication::default();
            assert!(state.plan(7));
            if missing != 0 {
                state.cull_recorded(GI_CULL_VIEW);
            }
            if missing != 1 {
                state.draw_closed(GI_CULL_VIEW);
            }
            if missing != 2 {
                state.final_submit_accepted();
            }
            assert_eq!(state.commit(7), None);
            assert!(state.needs_retry());
            assert!(state.plan(7));
            state.cull_recorded(GI_CULL_VIEW);
            state.draw_closed(GI_CULL_VIEW);
            state.final_submit_accepted();
            assert_eq!(state.commit(7), Some((1, 7)));
        }
    }

    #[test]
    fn interior_sky_cull_or_wrong_epoch_cannot_confirm_gi() {
        let mut state = GiRsmPublication::default();
        assert!(state.plan(9));
        state.cull_recorded(0);
        state.draw_closed(GI_CULL_VIEW);
        state.final_submit_accepted();
        assert_eq!(state.commit(9), None);
        assert!(state.plan(9));
        state.cull_recorded(GI_CULL_VIEW);
        state.draw_closed(GI_CULL_VIEW);
        state.final_submit_accepted();
        assert_eq!(state.commit(10), None);
    }

    #[test]
    fn abort_after_submit_invalidates_and_retry_keeps_generation_monotone() {
        let mut state = GiRsmPublication::default();
        assert!(state.plan(11));
        state.cull_recorded(GI_CULL_VIEW);
        state.draw_closed(GI_CULL_VIEW);
        state.final_submit_accepted();
        assert_eq!(state.commit(11), Some((1, 11)));
        assert!(state.plan(12));
        assert_eq!(state.published(), None);
        state.cull_recorded(GI_CULL_VIEW);
        state.draw_closed(GI_CULL_VIEW);
        state.final_submit_accepted();
        state.abort();
        assert_eq!(state.commit(12), None);
        assert!(state.needs_retry());
        assert!(state.plan(12));
        state.cull_recorded(GI_CULL_VIEW);
        state.draw_closed(GI_CULL_VIEW);
        state.final_submit_accepted();
        assert_eq!(state.commit(12), Some((2, 12)));
    }
}
