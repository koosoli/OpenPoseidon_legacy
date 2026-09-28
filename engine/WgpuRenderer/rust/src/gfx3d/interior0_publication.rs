//! Opt-in, CPU-only publication gate for the zenith interior-sky cache layer.
//! A selected refresh is not a published image. The caller must observe the
//! actual cull, closed indirect draw, accepted final submit, and successful
//! render return before this generation can describe cached contents.

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Interior0Publication {
    generation: u64,
    published_epoch: u64,
    valid: bool,
    pending_epoch: Option<u64>,
    culled: bool,
    draw_closed: bool,
    submitted: bool,
}

impl Interior0Publication {
    pub fn needs_refresh(self) -> bool {
        !self.valid
    }
    pub fn planned_epoch(self) -> Option<u64> {
        self.pending_epoch
    }

    pub fn plan(&mut self, epoch: u64) -> bool {
        self.valid = false;
        self.pending_epoch = None;
        self.culled = false;
        self.draw_closed = false;
        self.submitted = false;
        if epoch == 0 || epoch == u64::MAX || self.generation == u64::MAX {
            return false;
        }
        self.pending_epoch = Some(epoch);
        true
    }

    pub fn cull_recorded(&mut self) {
        if self.pending_epoch.is_some() {
            self.culled = true;
        }
    }

    pub fn draw_closed(&mut self) {
        if self.pending_epoch.is_some() {
            self.draw_closed = true;
        }
    }

    pub fn final_submit_accepted(&mut self) {
        if self.pending_epoch.is_some() {
            self.submitted = true;
        }
    }

    pub fn commit(&mut self) -> Option<(u64, u64)> {
        let Some(epoch) = self.pending_epoch.take() else {
            return None;
        };
        if !(self.culled && self.draw_closed && self.submitted) {
            self.valid = false;
            return None;
        }
        let Some(next) = self.generation.checked_add(1) else {
            self.valid = false;
            return None;
        };
        self.generation = next;
        self.published_epoch = epoch;
        self.valid = true;
        Some((next, epoch))
    }

    pub fn abort(&mut self) {
        self.pending_epoch = None;
        self.valid = false;
        self.culled = false;
        self.draw_closed = false;
        self.submitted = false;
    }

    pub fn published(self) -> Option<(u64, u64)> {
        self.valid
            .then_some((self.generation, self.published_epoch))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn publication_requires_cull_closed_draw_submit_and_successful_return() {
        for missing in 0..3 {
            let mut p = Interior0Publication::default();
            assert!(p.plan(7));
            if missing != 0 {
                p.cull_recorded();
            }
            if missing != 1 {
                p.draw_closed();
            }
            if missing != 2 {
                p.final_submit_accepted();
            }
            assert_eq!(p.commit(), None);
            assert!(p.needs_refresh());
            assert_eq!(p.published(), None);
            assert!(p.plan(7), "a refused pass must be eligible for retry");
            p.cull_recorded();
            p.draw_closed();
            p.final_submit_accepted();
            assert_eq!(p.commit(), Some((1, 7)));
        }
    }

    #[test]
    fn planned_refresh_invalidates_old_generation_until_a_new_submit() {
        let mut p = Interior0Publication::default();
        assert!(p.plan(3));
        p.cull_recorded();
        p.draw_closed();
        p.final_submit_accepted();
        assert_eq!(p.commit(), Some((1, 3)));
        assert_eq!(p.published(), Some((1, 3)));
        assert!(p.plan(4));
        assert_eq!(p.published(), None);
        p.cull_recorded();
        p.draw_closed();
        assert_eq!(p.commit(), None); // encoded but never submitted
        assert!(p.needs_refresh());
        assert!(p.plan(4));
        p.cull_recorded();
        p.draw_closed();
        p.final_submit_accepted();
        assert_eq!(p.commit(), Some((2, 4)));
    }

    #[test]
    fn abort_after_submit_cannot_publish_and_retries() {
        let mut p = Interior0Publication::default();
        assert!(p.plan(9));
        p.cull_recorded();
        p.draw_closed();
        p.final_submit_accepted();
        p.abort(); // e.g. later present/panic after queue.submit
        assert_eq!(p.commit(), None);
        assert_eq!(p.published(), None);
        assert!(p.plan(9));
        p.cull_recorded();
        p.draw_closed();
        p.final_submit_accepted();
        assert_eq!(p.commit(), Some((1, 9)));
        p.abort(); // failed frame with only cached contents must also require revalidation
        assert!(p.needs_refresh());
    }

    #[test]
    fn invalid_epoch_fails_closed_without_wrapping_generation() {
        let mut p = Interior0Publication::default();
        assert!(!p.plan(0));
        p.cull_recorded();
        p.draw_closed();
        p.final_submit_accepted();
        assert_eq!(p.commit(), None);
        assert!(!p.plan(u64::MAX));
        assert!(p.needs_refresh());
    }
}
