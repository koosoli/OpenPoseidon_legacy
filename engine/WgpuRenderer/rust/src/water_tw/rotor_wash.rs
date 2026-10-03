//! Rotor-only presentation descriptors. No ammunition, boat wake or impact plume.
pub const ROTOR_FLAG: u32 = 1 << 13;
pub const SOURCES: usize = 8;

#[derive(Default)]
pub struct RotorTrace {
    previous: Option<(f32, [f32; 4], usize)>,
}

impl RotorTrace {
    pub fn observe(&mut self, time: f32, domain: [f32; 4], count: usize) -> bool {
        if !time.is_finite() || !domain.iter().all(|x| x.is_finite()) || count > SOURCES { return false; }
        let due = self.previous.is_none_or(|(t,d,n)| time < t || time-t >= 1.0 || d != domain || n != count);
        if due { self.previous = Some((time,domain,count)); }
        due
    }
}

#[derive(Clone, Copy, Debug, Default)]
pub struct RotorWash {
    pub domain: [f32; 4],
    pub control: [f32; 4], // water clock, count, reserved
    pub sources: [[f32; 4]; SOURCES], // world XZ, radius, strength
}

impl RotorWash {
    pub fn frame(&mut self, domain: [f32; 4], time: f32, frozen: bool, enabled: bool) {
        // Keep observational domain/clock metadata for an empty ablation
        // frame; disabled sources still cannot reach the material.
        if !enabled { self.begin(domain,time); }
        else if !frozen { self.begin(domain,time); }
    }

    pub fn submit_frame(&mut self, events: impl IntoIterator<Item = [[f32; 4]; 4]>, frozen: bool, enabled: bool) {
        if enabled && !frozen { self.submit(events); }
    }

    /// Called before this frame's events, including frames with zero events.
    pub fn begin(&mut self, domain: [f32; 4], time: f32) {
        *self = Self::default();
        if domain.iter().all(|x| x.is_finite()) && domain[2] > 0.0 && domain[2] <= 256.0
            && time.is_finite() && time >= 0.0
        {
            self.domain = domain;
            self.control[0] = time;
        }
    }

    pub fn submit(&mut self, events: impl IntoIterator<Item = [[f32; 4]; 4]>) {
        if self.domain[2] <= 0.0 { return; }
        for [position, velocity, life, direction] in events.into_iter().take(48) {
            if self.control[1] >= SOURCES as f32 { break; }
            if !position.iter().chain(velocity.iter()).chain(life.iter()).chain(direction.iter())
                .all(|x| x.is_finite()) { continue; }
            let flags = direction[3];
            // Exact flags avoid float-to-integer saturation admitting malformed packets.
            if flags != (ROTOR_FLAG | 1) as f32 || velocity[3] != 0.0 ||
                velocity[2] != -20.0 || life[0] != self.control[0] ||
                life[1] != 0.0 || life[2] != 0.0 || life[3] != 0.0 ||
                position[2] < 3.0 || position[2] > 15.0 || position[3] <= 0.0 || position[3] > 1.1
            { continue; }
            let radius = position[2] * 2.0;
            let x = position[0]; let z = position[1];
            if x + radius <= self.domain[0] || z + radius <= self.domain[1] ||
                x - radius >= self.domain[0] + self.domain[2] ||
                z - radius >= self.domain[1] + self.domain[2] { continue; }
            let n = self.control[1] as usize;
            self.sources[n] = position;
            self.control[1] += 1.0;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn event() -> [[f32; 4]; 4] {
        [[1750.0,3100.0,15.0,1.1], [0.0,0.0,-20.0,0.0],
         [20.0,0.0,0.0,0.0], [0.0,0.0,0.0,(ROTOR_FLAG|1) as f32]]
    }
    fn frame() -> RotorWash {
        let mut r = RotorWash::default(); r.begin([1620.0,2970.0,256.0,1.0/256.0],20.0); r
    }
    #[test]
    fn admitted_rotors_are_bounded_and_never_borrow_boat_or_ammo_events() {
        let mut r=frame(); r.submit([event(); 48]); assert_eq!(r.control[1],8.0);
        assert_eq!(r.sources[0],[1750.0,3100.0,15.0,1.1]);
        for (lane, component, value) in [(3,3,1.0),(2,3,1.0),(1,3,5.0),(0,2,16.0),
            (0,3,0.0),(0,3,1.2),(2,1,1.0),(0,0,f32::NAN),(3,3,f32::INFINITY)] {
            let mut e=event(); e[lane][component]=value;
            let mut r=frame(); r.submit([e]); assert_eq!(r.control[1],0.0);
        }
        let mut e=event(); e[0][0]=5000.0;
        let mut r=frame(); r.submit([e]); assert_eq!(r.control[1],0.0);
    }
    #[test]
    fn zero_event_frames_and_invalid_domains_cannot_retain_old_wash() {
        let mut r=frame(); r.submit([event()]); assert_eq!(r.control[1],1.0);
        r.begin([1620.0,2970.0,256.0,1.0/256.0],21.0);
        assert_eq!(r.control[1],0.0); assert_eq!(r.sources,[[0.0;4];8]);
        for d in [[0.0,0.0,0.0,0.0],[0.0,0.0,257.0,0.0],[f32::NAN,0.0,256.0,0.0]] {
            r.begin(d,22.0); r.submit([event()]); assert_eq!(r.control[1],0.0);
        }
        r.begin([1620.0,2970.0,256.0,1.0/256.0],f32::NAN);
        r.submit([event()]); assert_eq!(r.control[1],0.0);
    }
    #[test]
    fn paused_identical_frame_has_identical_descriptors() {
        let mut a=frame(); a.submit([event()]);
        let mut b=frame(); b.submit([event()]);
        assert_eq!(a.domain,b.domain); assert_eq!(a.control,b.control); assert_eq!(a.sources,b.sources);
    }
    #[test]
    fn actual_frame_helpers_freeze_disable_and_switch_without_replaying_old_events() {
        let mut r=frame(); r.submit([event()]); let before=r;
        r.frame([0.0,0.0,256.0,1.0/256.0],24.0,true,true);
        r.submit_frame([event()],true,true);
        assert_eq!(r.domain,before.domain); assert_eq!(r.control,before.control); assert_eq!(r.sources,before.sources);
        r.frame(before.domain,25.0,true,false);
        r.submit_frame([event()],false,false); assert_eq!(r.control[1],0.0);
        r.frame(before.domain,26.0,false,true);
        r.submit_frame([event()],false,true); assert_eq!(r.control[1],0.0); // prior frame packet
        let mut new=event();new[2][0]=26.0;
        r.submit_frame([new],false,true);assert_eq!(r.control[1],1.0);
        let mut switched=RotorWash::default();
        switched.frame(before.domain,26.0,false,true);
        assert_eq!(switched.control[1],0.0); // replay contains params, never rotor events
    }
    #[test]
    fn trace_is_quiet_per_frame_and_reports_real_paused_domain_transitions() {
        let mut t=RotorTrace::default(); let d=[1620.0,2970.0,256.0,1.0/256.0];
        assert!(t.observe(20.0,d,1));
        assert!(!t.observe(20.0,d,1)); assert!(!t.observe(20.9,d,1));
        assert!(t.observe(21.0,d,1)); assert!(t.observe(21.0,d,0));
        let mut moved=d;moved[0]+=4.0;
        assert!(t.observe(21.0,moved,0)); assert!(!t.observe(21.0,moved,0));
        assert!(!t.observe(f32::NAN,moved,0)); assert!(!t.observe(21.0,moved,9));
        assert!(t.observe(0.0,d,0));
    }
}
