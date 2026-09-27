//! Fixture-only actual indirect command-recording facts. No survivor/pixel,
//! submission/completion or all-reference/all-pass proof. Planning keys are NOT used.
use std::mem::size_of;
pub const VERSION: u32 = 2;
pub const CASCADES: u32 = 1;
pub const LOCAL: u32 = 2;
pub const INTERIOR: u32 = 4;
pub const GI: u32 = 8;
pub const REFLECTION: u32 = 16;
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct WgrGeometryPassFacts {
    pub version: u32,
    pub struct_bytes: u32,
    pub enabled: u32,
    pub capabilities: u32,
    pub required: u32,
    pub pending: u32,
    pub recorded_this_frame: u32,
    pub reserved: u32,
    pub frame: u64,
    pub instance_epoch: u64,
    pub cascade_epoch: u64,
    pub cascade_frame: u64,
    pub gi_rsm_epoch: u64,
    pub gi_rsm_frame: u64,
    pub local_count: u32,
    pub local_draw_mask: u32,
    pub local_valid_mask: u32,
    pub interior_draw_mask: u32,
    pub interior_valid_mask: u32,
    pub cascade_draw_mask: u32,
    pub cascade_count: u32,
    pub reserved2: u32,
    pub local_epochs: [u64; 24],
    pub local_frames: [u64; 24],
    pub interior_epochs: [u64; 5],
    pub interior_frames: [u64; 5],
    // Appended in v2: independently culled reflection commands from this exact frame.
    pub reflection_epoch: u64,
    pub reflection_frame: u64,
}
impl WgrGeometryPassFacts {
    pub fn new() -> Self {
        Self {
            version: VERSION,
            struct_bytes: size_of::<Self>() as u32,
            enabled: 1,
            capabilities: CASCADES | LOCAL | INTERIOR | GI | REFLECTION,
            ..Self::default()
        }
    }
    pub fn begin(&mut self, epoch: u64, cascades: u32, locals: u32) {
        self.frame = self.frame.saturating_add(1);
        self.instance_epoch = epoch;
        self.required = 0;
        self.recorded_this_frame = 0;
        self.local_draw_mask = 0;
        self.interior_draw_mask = 0;
        self.cascade_draw_mask = 0;
        self.cascade_count = cascades.min(4);
        self.local_count = locals.min(24);
        if self.cascade_count > 0 {
            self.required |= CASCADES;
        }
        if self.local_count > 0 {
            self.required |= LOCAL;
        }
    }
    // Refresh selection can change view/light/caster content without changing instance epoch.
    // Refuse old evidence BEFORE attempting the draw; only record_* can restore it.
    pub fn invalidate_local(&mut self, k: usize) {
        if k < 24 {
            self.local_valid_mask &= !(1 << k);
        }
    }
    pub fn invalidate_interior(&mut self, k: usize) {
        if k < 5 {
            self.interior_valid_mask &= !(1 << k);
        }
    }
    pub fn invalidate_gi(&mut self) {
        self.gi_rsm_frame = 0;
    }
    pub fn record_shadow(&mut self, view: usize, cascades: usize) {
        if view < cascades && view < 4 {
            self.cascade_draw_mask |= 1 << view;
            self.cascade_epoch = self.instance_epoch;
            self.cascade_frame = self.frame;
            self.recorded_this_frame |= CASCADES;
        } else {
            let k = view.saturating_sub(cascades);
            if k < 24 {
                self.local_valid_mask |= 1 << k;
                self.local_draw_mask |= 1 << k;
                self.local_epochs[k] = self.instance_epoch;
                self.local_frames[k] = self.frame;
                self.recorded_this_frame |= LOCAL;
            }
        }
    }
    pub fn record_interior(&mut self, k: usize) {
        if k < 5 {
            self.interior_valid_mask |= 1 << k;
            self.interior_draw_mask |= 1 << k;
            self.interior_epochs[k] = self.instance_epoch;
            self.interior_frames[k] = self.frame;
            self.recorded_this_frame |= INTERIOR;
        }
    }
    pub fn record_gi(&mut self) {
        self.gi_rsm_epoch = self.instance_epoch;
        self.gi_rsm_frame = self.frame;
        self.recorded_this_frame |= GI;
    }
    pub fn record_reflection(&mut self) {
        self.reflection_epoch = self.instance_epoch;
        self.reflection_frame = self.frame;
        self.recorded_this_frame |= REFLECTION;
    }
    pub fn snapshot(&self) -> Self {
        let mut r = *self;
        r.pending = r.required & !r.capabilities;
        if r.required & CASCADES != 0
            && (r.cascade_frame != r.frame
                || r.cascade_epoch != r.instance_epoch
                || r.cascade_draw_mask != ((1 << r.cascade_count) - 1))
        {
            r.pending |= CASCADES;
        }
        if r.required & LOCAL != 0
            && (0..r.local_count as usize).any(|i| {
                r.local_valid_mask & (1 << i) == 0 || r.local_epochs[i] != r.instance_epoch
            })
        {
            r.pending |= LOCAL;
        }
        if r.required & INTERIOR != 0
            && (0..5).any(|i| {
                r.interior_valid_mask & (1 << i) == 0 || r.interior_epochs[i] != r.instance_epoch
            })
        {
            r.pending |= INTERIOR;
        }
        if r.required & GI != 0 && (r.gi_rsm_frame == 0 || r.gi_rsm_epoch != r.instance_epoch) {
            r.pending |= GI;
        }
        if r.required & REFLECTION != 0
            && (r.reflection_frame != r.frame || r.reflection_epoch != r.instance_epoch)
        {
            r.pending |= REFLECTION;
        }
        // Saturating frame exhaustion cannot establish a new per-frame observation.
        if r.frame == 0 || r.frame == u64::MAX {
            r.pending = u32::MAX;
        }
        r
    }
}
pub fn valid_layout(bytes: u32, version: u32) -> bool {
    bytes == size_of::<WgrGeometryPassFacts>() as u32 && version == VERSION
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn layout_and_old_size_refusal() {
        assert_eq!(size_of::<WgrGeometryPassFacts>(), 592);
        assert_eq!(
            std::mem::offset_of!(WgrGeometryPassFacts, local_epochs),
            112
        );
        assert_eq!(
            std::mem::offset_of!(WgrGeometryPassFacts, interior_epochs),
            496
        );
        assert_eq!(std::mem::offset_of!(WgrGeometryPassFacts, reflection_epoch), 576);
        assert_eq!(std::mem::offset_of!(WgrGeometryPassFacts, reflection_frame), 584);
        assert!(valid_layout(592, 2));
        assert!(!valid_layout(576, 1));
        assert!(!valid_layout(592, 1));
        assert!(!valid_layout(576, 2));
        assert!(!valid_layout(591, 2));
    }
    #[test]
    fn configured_draws_are_pending_until_actual_recording() {
        let mut f = WgrGeometryPassFacts::new();
        f.begin(7, 2, 2);
        f.required |= INTERIOR | GI;
        assert_eq!(f.snapshot().pending, 15);
        f.record_shadow(0, 2);
        f.record_shadow(1, 2);
        f.record_shadow(2, 2);
        f.record_shadow(3, 2);
        for i in 0..5 {
            f.record_interior(i);
        }
        f.record_gi();
        assert_eq!(f.snapshot().pending, 0);
        f.begin(8, 2, 2);
        f.required |= INTERIOR | GI;
        assert_eq!(f.snapshot().pending, 15);
        f.record_interior(0);
        assert_eq!(f.snapshot().pending & INTERIOR, INTERIOR);
    }
    #[test]
    fn reflection_no_draw_refusal() {
        let mut f = WgrGeometryPassFacts::new();
        f.begin(4, 0, 0);
        f.required |= REFLECTION;
        assert_eq!(f.snapshot().pending, REFLECTION);
        f.required |= GI;
        assert_eq!(f.snapshot().pending, REFLECTION | GI);
    }
    #[test]
    fn reflection_requires_actual_recording_in_the_same_frame_and_epoch() {
        let mut f = WgrGeometryPassFacts::new();
        assert_eq!(f.capabilities, 31);
        f.begin(4, 0, 0);
        f.required |= REFLECTION;
        assert_eq!(f.snapshot().pending, REFLECTION);
        f.record_reflection();
        assert_eq!(f.snapshot().pending, 0);
        assert_eq!(f.snapshot().recorded_this_frame, REFLECTION);
        f.begin(4, 0, 0);
        f.required |= REFLECTION;
        // Resource refusal/no closed reflected draw cannot reuse last frame's evidence.
        assert_eq!(f.snapshot().pending, REFLECTION);
        f.record_reflection();
        assert_eq!(f.snapshot().pending, 0);
        f.instance_epoch = 5;
        assert_eq!(f.snapshot().pending, REFLECTION);
        f.record_reflection();
        assert_eq!(f.snapshot().pending, 0);
        f.begin(5, 0, 0);
        // An unconfigured reflection is not required or falsely recorded.
        assert_eq!(f.snapshot().pending, 0);
        assert_eq!(f.snapshot().recorded_this_frame, 0);
    }
    #[test]
    fn same_epoch_selected_refresh_refusal_stays_pending_until_actual_record() {
        let mut f = WgrGeometryPassFacts::new();
        f.begin(9, 0, 1);
        f.required |= INTERIOR | GI;
        f.record_shadow(0, 0);
        for i in 0..5 {
            f.record_interior(i);
        }
        f.record_gi();
        assert_eq!(f.snapshot().pending, 0);
        f.begin(9, 0, 1);
        f.required |= INTERIOR | GI;
        assert_eq!(f.snapshot().pending, 0);
        f.invalidate_local(0);
        f.invalidate_interior(3);
        f.invalidate_gi();
        assert_eq!(f.snapshot().pending, LOCAL | INTERIOR | GI);
        // No recording when resource gates refuse: prior same-epoch stamps do not certify content.
        assert_eq!(f.snapshot().pending, LOCAL | INTERIOR | GI);
        f.record_shadow(0, 0);
        f.record_interior(3);
        f.record_gi();
        assert_eq!(f.snapshot().pending, 0);
    }
    #[test]
    fn cache_can_reuse_only_same_geometry_epoch() {
        let mut f = WgrGeometryPassFacts::new();
        f.begin(2, 0, 1);
        f.record_shadow(0, 0);
        f.begin(2, 0, 1);
        assert_eq!(f.snapshot().pending, 0);
        assert_eq!(f.local_draw_mask, 0);
        f.begin(3, 0, 1);
        assert_eq!(f.snapshot().pending, LOCAL);
    }
}
