// Renderer-private advection: changing weather changes velocity, not position.
#[derive(Default)]
pub(super) struct WindPhase {
    previous_time: Option<f32>,
    offsets: [f64; 6],
    // Lock the world-space front orientation at the first valid wind sample.
    // Rotating absolute world positions with changing weather would sweep a
    // many-kilometre meadow through the noise field and create phase jumps.
    front_direction: Option<[f32; 2]>,
}

impl WindPhase {
    pub(super) fn advance(&mut self, time: f32, strength: f32, direction: f32, scroll: f32) -> ([f32; 4], [f32; 4]) {
        if !time.is_finite() { return self.uniforms(); }
        let dt = match self.previous_time {
            Some(previous) if time >= previous => (time - previous).min(0.25) as f64,
            Some(_) => { self.offsets = [0.0; 6]; self.front_direction = None; 0.0 },
            None => 0.0,
        };
        self.previous_time = Some(time);
        if !strength.is_finite() || !direction.is_finite() || !scroll.is_finite() { return self.uniforms(); }
        let strength = strength.clamp(0.0, 3.0) as f64;
        let scroll = scroll.clamp(0.02, 4.0) as f64;
        let angle = (direction as f64).to_radians();
        let direction = [angle.cos(), angle.sin()];
        if self.front_direction.is_none() {
            self.front_direction = Some([direction[0] as f32, direction[1] as f32]);
        }
        let broad = (16.0 + strength * 9.0) * scroll;
        let gust = (30.0 + strength * 15.0) * scroll;
        for axis in 0..2 {
            self.offsets[axis] -= direction[axis] * broad * dt;
            self.offsets[axis + 2] -= direction[axis] * gust * dt;
            self.offsets[axis + 4] += (-direction[axis] * gust * 2.1 + [3.7, -2.9][axis] * scroll) * dt;
        }
        self.uniforms()
    }

    fn uniforms(&self) -> ([f32; 4], [f32; 4]) {
        let front = self.front_direction.unwrap_or([1.0, 0.0]);
        ([self.offsets[0] as f32, self.offsets[1] as f32, self.offsets[2] as f32, self.offsets[3] as f32],
         [self.offsets[4] as f32, self.offsets[5] as f32, front[0], front[1]])
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn late_weather_change_changes_velocity_without_a_phase_jump() {
        let mut phase = WindPhase::default();
        phase.advance(3600.0, 1.2, 0.0, 0.125);
        let (before, _) = phase.advance(3600.125, 1.2, 0.0, 0.125);
        let (same_time, _) = phase.advance(3600.125, 3.0, 90.0, 0.2);
        assert_eq!(before, same_time);
        let (after, _) = phase.advance(3600.25, 3.0, 90.0, 0.2);
        assert!((after[2] - before[2]).abs() < 0.0001);
        assert!((after[3] - before[3] + 1.875).abs() < 0.0001);
    }

    #[test]
    fn weather_turn_does_not_rotate_absolute_world_front_coordinates() {
        let mut phase = WindPhase::default();
        let (position, before) = phase.advance(900.0, 1.2, 35.0, 0.15);
        let (same_position, turned) = phase.advance(900.0, 1.2, -95.0, 0.15);
        assert_eq!(position, same_position);
        assert_eq!(&before[2..], &turned[2..]);
        let (moving, after) = phase.advance(900.125, 1.2, -95.0, 0.15);
        assert_ne!(position, moving);
        assert_eq!(&before[2..], &after[2..]);
    }

    #[test]
    fn pause_rewind_and_long_gap_are_bounded() {
        let mut phase = WindPhase::default();
        phase.advance(0.0, 1.2, 0.0, 0.125);
        let moving = phase.advance(0.125, 1.2, 0.0, 0.125);
        assert_eq!(phase.advance(0.125, 1.2, 0.0, 0.125), moving);
        let (after_gap, _) = phase.advance(7200.0, 1.2, 0.0, 0.125);
        assert!((after_gap[2] - moving.0[2] + 1.5).abs() < 0.0001);
        assert_eq!(phase.advance(0.0, 1.2, 0.0, 0.125), ([0.0; 4], [0.0, 0.0, 1.0, 0.0]));
    }
}
