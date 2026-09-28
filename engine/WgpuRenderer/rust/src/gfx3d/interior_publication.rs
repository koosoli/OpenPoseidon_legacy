//! Opt-in, CPU-only publication gate for one interior-sky cache direction.
//! A selected refresh is not a published image. The caller must observe the
//! actual cull, closed indirect draw, accepted final submit, and successful
//! render return before this generation can describe cached contents.

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct InteriorPublication {
    generation: u64,
    published_epoch: u64,
    published_view: usize,
    valid: bool,
    pending: Option<(usize, u64)>,
    culled: bool,
    draw_closed: bool,
    submitted: bool,
}

impl InteriorPublication {
    pub fn needs_refresh(self) -> bool {
        !self.valid
    }
    pub fn planned(self) -> Option<(usize, u64)> {
        self.pending
    }

    pub fn plan(&mut self, view: usize, epoch: u64) -> bool {
        self.valid = false;
        self.pending = None;
        self.culled = false;
        self.draw_closed = false;
        self.submitted = false;
        if view >= super::sky_vis::DIRECTION_COUNT
            || epoch == 0
            || epoch == u64::MAX
            || self.generation == u64::MAX
        {
            return false;
        }
        self.pending = Some((view, epoch));
        true
    }

    pub fn cull_recorded(&mut self, view: usize) {
        if self.pending.is_some_and(|(selected, _)| selected == view) {
            self.culled = true;
        }
    }

    pub fn draw_closed(&mut self, view: usize) {
        if self.pending.is_some_and(|(selected, _)| selected == view) {
            self.draw_closed = true;
        }
    }

    pub fn final_submit_accepted(&mut self) {
        if self.pending.is_some() {
            self.submitted = true;
        }
    }

    pub fn commit(&mut self, view: usize, epoch: u64) -> Option<(u64, usize, u64)> {
        let Some((selected, selected_epoch)) = self.pending.take() else {
            return None;
        };
        if selected != view
            || selected_epoch != epoch
            || !(self.culled && self.draw_closed && self.submitted)
        {
            self.valid = false;
            return None;
        }
        let Some(next) = self.generation.checked_add(1) else {
            self.valid = false;
            return None;
        };
        self.generation = next;
        self.published_epoch = epoch;
        self.published_view = view;
        self.valid = true;
        Some((next, view, epoch))
    }

    pub fn abort(&mut self) {
        self.pending = None;
        self.valid = false;
        self.culled = false;
        self.draw_closed = false;
        self.submitted = false;
    }

    pub fn published(self) -> Option<(u64, usize, u64)> {
        self.valid
            .then_some((self.generation, self.published_view, self.published_epoch))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn every_direction_requires_cull_closed_draw_submit_and_successful_return() {
        for view in 0..super::super::sky_vis::DIRECTION_COUNT {
            for missing in 0..3 {
                let mut p = InteriorPublication::default();
                assert!(p.plan(view, 7));
                if missing != 0 {
                    p.cull_recorded(view);
                }
                if missing != 1 {
                    p.draw_closed(view);
                }
                if missing != 2 {
                    p.final_submit_accepted();
                }
                assert_eq!(p.commit(view, 7), None);
                assert!(p.needs_refresh());
                assert_eq!(p.published(), None);
                assert!(p.plan(view, 7), "a refused pass must be eligible for retry");
                p.cull_recorded(view);
                p.draw_closed(view);
                p.final_submit_accepted();
                assert_eq!(p.commit(view, 7), Some((1, view, 7)));
            }
        }
    }

    #[test]
    fn planned_refresh_invalidates_old_generation_until_a_new_submit() {
        let mut p = InteriorPublication::default();
        assert!(p.plan(3, 3));
        p.cull_recorded(3);
        p.draw_closed(3);
        p.final_submit_accepted();
        assert_eq!(p.commit(3, 3), Some((1, 3, 3)));
        assert_eq!(p.published(), Some((1, 3, 3)));
        assert!(p.plan(3, 4));
        assert_eq!(p.published(), None);
        p.cull_recorded(3);
        p.draw_closed(3);
        assert_eq!(p.commit(3, 4), None); // encoded but never submitted
        assert!(p.needs_refresh());
        assert!(p.plan(3, 4));
        p.cull_recorded(3);
        p.draw_closed(3);
        p.final_submit_accepted();
        assert_eq!(p.commit(3, 4), Some((2, 3, 4)));
    }

    #[test]
    fn abort_after_submit_cannot_publish_and_retries() {
        let mut p = InteriorPublication::default();
        assert!(p.plan(4, 9));
        p.cull_recorded(4);
        p.draw_closed(4);
        p.final_submit_accepted();
        p.abort(); // e.g. later present/panic after queue.submit
        assert_eq!(p.commit(4, 9), None);
        assert_eq!(p.published(), None);
        assert!(p.plan(4, 9));
        p.cull_recorded(4);
        p.draw_closed(4);
        p.final_submit_accepted();
        assert_eq!(p.commit(4, 9), Some((1, 4, 9)));
        p.abort(); // failed frame with only cached contents must also require revalidation
        assert!(p.needs_refresh());
    }

    #[test]
    fn invalid_epoch_fails_closed_without_wrapping_generation() {
        let mut p = InteriorPublication::default();
        assert!(!p.plan(0, 0));
        p.cull_recorded(0);
        p.draw_closed(0);
        p.final_submit_accepted();
        assert_eq!(p.commit(0, 0), None);
        assert!(!p.plan(0, u64::MAX));
        assert!(!p.plan(super::super::sky_vis::DIRECTION_COUNT, 1));
        assert!(p.needs_refresh());
    }

    #[test]
    fn a_different_direction_cannot_satisfy_a_selected_view() {
        let mut p = InteriorPublication::default();
        assert!(p.plan(2, 11));
        p.cull_recorded(1);
        p.draw_closed(2);
        p.final_submit_accepted();
        assert_eq!(p.commit(2, 11), None);
        assert!(p.plan(2, 11));
        p.cull_recorded(2);
        p.draw_closed(1);
        p.final_submit_accepted();
        assert_eq!(p.commit(2, 11), None);
        assert!(p.plan(2, 11));
        p.cull_recorded(2);
        p.draw_closed(2);
        p.final_submit_accepted();
        assert_eq!(p.commit(1, 11), None);
        assert!(p.plan(2, 11));
        p.cull_recorded(2);
        p.draw_closed(2);
        p.final_submit_accepted();
        assert_eq!(p.commit(2, 12), None);
    }
}
