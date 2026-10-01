//! Current-frame physical map witness. Separate from the five artistic AO publications.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Identity {
    pub frame: u64,
    pub instance_epoch: u64,
    pub image: u64,
}

#[derive(Clone, Copy, Debug, Default)]
pub struct Publication {
    identity: Option<Identity>,
    retained_required: bool,
    culled: bool,
    drawn: bool,
    submitted: bool,
}

impl Publication {
    pub fn plan(&mut self, identity: Identity, retained_required: bool) {
        *self = Self {
            identity: Some(identity),
            retained_required,
            ..Self::default()
        };
    }
    pub fn cull_recorded(&mut self) {
        self.culled = true;
    }
    pub fn depth_recorded(&mut self, source_complete: bool) {
        self.drawn = source_complete;
    }
    pub fn readable(&self, identity: Identity) -> bool {
        self.identity == Some(identity) && self.drawn && (!self.retained_required || self.culled)
    }
    pub fn submitted(&mut self) {
        self.submitted = true;
    }
    pub fn commit(&self, identity: Identity) -> bool {
        self.readable(identity) && self.submitted
    }
    pub fn abort(&mut self) {
        *self = Self::default();
    }
}

pub fn active(enabled: bool, snow: [f32; 4], ground: [f32; 4]) -> bool {
    enabled
        && snow.iter().chain(ground.iter()).all(|v| v.is_finite())
        && (snow[0] > 0.0 || (snow[1] >= 0.0 && snow[2] > 0.0 && snow[3] > 0.0) || ground[0] > 0.03)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn weather_cover_cascades_publish_and_abort_independently() {
        let id = Identity {frame:7,instance_epoch:12,image:3};
        let mut near = Publication::default();
        let mut far = Publication::default();
        near.plan(id,true); far.plan(id,true);
        near.cull_recorded(); near.depth_recorded(true); near.submitted();
        assert!(near.commit(id)); assert!(!far.commit(id));
        far.cull_recorded(); far.depth_recorded(false); far.submitted();
        assert!(!far.commit(id),"skipped far source must not borrow near publication");
        far.depth_recorded(true); assert!(far.commit(id));
        near.abort(); assert!(!near.commit(id)); assert!(far.commit(id));
        far.plan(Identity {frame:8,..id},true);
        assert!(!far.readable(id),"new far frame cannot borrow old depth");
    }
    #[test]
    fn weather_cover_current_map_requires_exact_geometry_recording_and_submit() {
        let id = Identity {
            frame: 1,
            instance_epoch: 4,
            image: 2,
        };
        let mut p = Publication::default();
        assert!(!p.readable(id));
        p.plan(id, true);
        p.depth_recorded(true);
        assert!(!p.readable(id));
        p.cull_recorded();
        assert!(p.readable(id));
        assert!(!p.commit(id));
        p.submitted();
        assert!(p.commit(id));
        for other in [
            Identity { frame: 2, ..id },
            Identity {
                instance_epoch: 5,
                ..id
            },
            Identity { image: 3, ..id },
        ] {
            assert!(!p.readable(other));
            assert!(!p.commit(other));
        }
        p.plan(Identity { frame: 2, ..id }, false);
        assert!(!p.readable(id)); // never reuse old map under a new requested matrix
        p.depth_recorded(false);
        assert!(!p.readable(Identity { frame: 2, ..id }));
        p.depth_recorded(true);
        assert!(p.readable(Identity { frame: 2, ..id }));
        p.abort();
        assert!(!p.readable(Identity { frame: 2, ..id }));
    }
    #[test]
    fn weather_cover_dry_or_invalid_state_never_requests_work() {
        assert!(!active(true, [0.0, -1.0, 0.0, 0.0], [0.0; 4]));
        assert!(!active(true, [0.0, -1.0, 0.0, 0.0], [0.03, 0.0, 0.0, 0.0]));
        assert!(active(true, [0.0, -1.0, 0.0, 0.0], [0.031, 0.0, 0.0, 0.0]));
        assert!(active(true, [0.18, -1.0, 0.0, 0.0], [0.0; 4]));
        assert!(active(true, [0.0, 500.0, 100.0, 0.18], [0.0; 4]));
        assert!(!active(true, [0.0, 500.0, 0.0, 0.18], [0.0; 4]));
        assert!(active(true, [0.0, -1.0, 0.0, 0.0], [0.8, 0.0, 0.0, 100.0]));
        assert!(!active(false, [0.18, -1.0, 0.0, 0.0], [0.8; 4]));
        assert!(!active(true, [f32::NAN, -1.0, 0.0, 0.0], [0.8; 4]));
    }
}
