//! TW-WATER W8b (OP, no Tidewater counterpart): shells, rockets, bombs and bullets hitting the
//! water. The engine's water-interaction events (explosion / bullet, see WaterInteractionBridge.hpp)
//! carry the ammo: an explosion its `indirectHit` (mass lane) and `indirectHitRange` (depth lane),
//! a bullet its `hit`. Each becomes
//! * a plume thrown into the spray ring's CPU tail (spray.rs, the same particles as the bow spray):
//!   a core of ligaments shot up to the plume height, drops and a crown of sheet fragments thrown
//!   wider, white water filling the column, and mist hanging where the column tops out and rolling
//!   out over the water at its foot;
//! * (explosions) a ring wave, a collapsing cavity and a foam patch in the water surface
//!   (`tw_impact.wgsl`, through the surface block's `impacts` lanes).
//!
//! Size: the blast `indirectHit x indirectHitRange` (the value the engine sizes land craters from,
//! Collisions.cpp) sets the plume height by its cube root (an explosion's plume grows roughly as the
//! cube root of the charge): ~4 m for a 20 mm round, ~15 m for 120 mm HE, ~50 m for a laser-guided
//! bomb; `indirectHitRange` widens the column and the foam. A bullet's spout grows with sqrt(hit):
//! ~0.5 m for 7.62 mm. Visual only: nothing here feeds back into the simulation.
//!
//! Sinkhole W3: a swimmer's hand or foot cutting the surface (kind 6, SoldierOldSwim.cpp) is a small
//! splash of the same kind: drops, a few ligaments and a puff of white water, thrown up from the
//! wave's own height (the event carries it -- a swimmer rides the crests) and onward with the limb.

use super::spray::EmitRequest;
use crate::ffi::WgrWaterInteractionEvent;

const KIND_BULLET: f32 = 0.0;
const KIND_EXPLOSION: f32 = 3.0;
const KIND_SWIM_SPLASH: f32 = 6.0;
// spray particle kinds (tw_spray_common.wgsl)
const DROPLET: f32 = 0.0;
const MIST: f32 = 1.0;
const LIGAMENT: f32 = 2.0;
const SPRAY: f32 = 3.0;
const SHEET: f32 = 4.0;
const GRAVITY: f32 = 9.81;
/// Splashes being emitted at once (the oldest is dropped).
pub const MAX_IMPACTS: usize = 24;
/// Explosions shown in the water surface (the most recent).
pub const SURFACE_SLOTS: usize = 4;
/// How long an explosion stays in the water surface (s).
pub const SURFACE_LIFE: f32 = 14.0;

/// Plume height (m) from the ammo: explosions by the cube root of `indirectHit x indirectHitRange`,
/// bullets by the square root of `hit`.
pub fn plume_height(explosive: bool, hit: f32, range: f32) -> f32 {
    if explosive {
        (1.4 * (hit * range).max(1.0).cbrt()).clamp(1.5, 60.0)
    } else {
        (0.18 * hit.max(0.1).sqrt()).clamp(0.15, 1.5)
    }
}

/// Column radius at the foot (m).
pub fn plume_radius(explosive: bool, height: f32, range: f32) -> f32 {
    if explosive {
        (0.2 * height + 0.15 * range).clamp(0.4, 14.0)
    } else {
        (0.06 + 0.05 * height).min(0.3)
    }
}

#[derive(Clone, Copy, Debug)]
struct Impact {
    x: f32,
    z: f32,
    y: f32,
    height: f32,
    radius: f32,
    explosive: bool,
    /// a swimmer's splash (kind 6)
    swim: bool,
    /// horizontal velocity added to every particle (the limb's throw; 0 for shots)
    throw: [f32; 2],
    age: f32,
    /// sector rotation (rad), from the position
    rot: f32,
    /// fractional particles carried per class
    carry: [f32; 6],
}

impl Impact {
    fn v0(&self) -> f32 {
        (2.0 * GRAVITY * self.height).sqrt()
    }
    /// the core's emission time (s)
    fn burst(&self) -> f32 {
        if self.explosive { 0.1 + 0.012 * self.height } else { 0.06 }
    }
    /// time for the column to top out (s)
    fn peak(&self) -> f32 {
        self.v0() / GRAVITY
    }
    fn emitting(&self) -> bool {
        self.age < self.burst().max(if self.explosive { self.peak() * 1.05 } else { 0.0 })
    }
}

#[derive(Default)]
pub struct Impacts {
    list: Vec<Impact>,
    /// explosions for the water surface: (x, z, age, height, radius, range)
    surface: Vec<[f32; 6]>,
    /// splashes received since the last log line
    pub received: u32,
    /// particles thrown last frame
    pub particles: u32,
}

impl Impacts {
    /// Takes this frame's water-interaction events (the ones that carry an ammo: mass lane > 0).
    pub fn submit(&mut self, events: &[WgrWaterInteractionEvent], sea_level: f32) {
        for e in events {
            let kind = e.velocity_kind[3];
            let hit = e.time_life_foam_mass[3];
            if hit <= 0.0 || !(kind == KIND_EXPLOSION || kind == KIND_BULLET || kind == KIND_SWIM_SPLASH) {
                continue;
            }
            let explosive = kind == KIND_EXPLOSION;
            let swim = kind == KIND_SWIM_SPLASH;
            let range = e.direction_depth_flags[2].max(0.01);
            let (height, radius, y, throw) = if swim {
                // the height and radius come with the event, and the surface height: the limb is on a wave
                let t = [0.35 * e.velocity_kind[0], 0.35 * e.velocity_kind[1]];
                (hit.clamp(0.05, 1.0), e.position_radius[2].clamp(0.03, 0.4), e.position_radius[3], t)
            } else {
                let height = plume_height(explosive, hit, range);
                (height, plume_radius(explosive, height, range), sea_level, [0.0; 2])
            };
            let (x, z) = (e.position_radius[0], e.position_radius[1]);
            if self.list.len() >= MAX_IMPACTS {
                self.list.remove(0);
            }
            self.list.push(Impact {
                x,
                z,
                y,
                height,
                radius,
                explosive,
                swim,
                throw,
                age: 0.0,
                rot: (x * 0.37 + z * 0.61).fract() * std::f32::consts::TAU,
                carry: [0.0; 6],
            });
            if explosive {
                if self.surface.len() >= SURFACE_SLOTS {
                    self.surface.remove(0);
                }
                self.surface.push([x, z, 0.0, height, radius, range]);
            }
            self.received += 1;
        }
    }

    /// Ages the splashes by `dt` and appends this frame's emit requests (at most `max` in all).
    /// `cam`: the camera (far splashes throw fewer particles).
    pub fn update(&mut self, dt: f32, cam: [f32; 3], out: &mut Vec<EmitRequest>, max: usize) {
        self.particles = 0;
        if dt <= 0.0 {
            return;
        }
        for s in &mut self.surface {
            s[2] += dt;
        }
        self.surface.retain(|s| s[2] < SURFACE_LIFE);
        let mut emitted = 0u32;
        for imp in &mut self.list {
            let d = ((imp.x - cam[0]).powi(2) + (imp.y - cam[1]).powi(2) + (imp.z - cam[2]).powi(2)).sqrt();
            let lod = (300.0 / d.max(1.0)).clamp(0.25, 1.0);
            emitted += emit(imp, dt, lod, out, max);
            imp.age += dt;
        }
        self.list.retain(|i| i.emitting());
        self.particles = emitted;
    }

    /// The surface block's lanes: per slot (x, z, age, height), (radius, range, amplitude, 0).
    pub fn lanes(&self) -> [[f32; 4]; SURFACE_SLOTS * 2] {
        let mut out = [[0.0; 4]; SURFACE_SLOTS * 2];
        for (i, s) in self.surface.iter().rev().take(SURFACE_SLOTS).enumerate() {
            out[i * 2] = [s[0], s[1], s[2], s[3]];
            out[i * 2 + 1] = [s[4], s[5], (0.02 * s[3]).min(1.2), 0.0];
        }
        out
    }

    pub fn active(&self) -> usize {
        self.list.len()
    }
}

/// One class of particles: `n` of them along the chords of three sectors at radius `rr` (or along
/// the vertical `a..b` when `column`), launched up at `vy` and outward at `vout`.
#[allow(clippy::too_many_arguments)]
fn class(
    imp: &Impact,
    carry: &mut f32,
    n: f32,
    rr: f32,
    vy: f32,
    vout: f32,
    kind: f32,
    size: f32,
    life: f32,
    spread: f32,
    jitter: f32,
    column: Option<(f32, f32)>,
    out: &mut Vec<EmitRequest>,
    max: usize,
) -> u32 {
    *carry += n;
    let m = carry.floor();
    if m < 1.0 {
        return 0;
    }
    *carry -= m;
    let total = m as u32;
    if let Some((y0, y1)) = column {
        if out.len() >= max {
            return 0;
        }
        out.push(EmitRequest {
            a: [imp.x, imp.y + y0, imp.z],
            b: [imp.x, imp.y + y1, imp.z],
            vel: [imp.throw[0], vy, imp.throw[1]],
            count: total,
            size,
            kind,
            spread,
            jitter,
            life,
            size_jitter: 0.6,
        });
        return total;
    }
    let tau = std::f32::consts::TAU;
    let mut sent = 0;
    for k in 0..3u32 {
        if out.len() >= max {
            break;
        }
        let each = total / 3 + u32::from(k < total % 3);
        if each == 0 {
            continue;
        }
        let a0 = imp.rot + k as f32 * tau / 3.0;
        let a1 = a0 + tau / 3.0;
        let am = a0 + tau / 6.0;
        out.push(EmitRequest {
            a: [imp.x + rr * a0.cos(), imp.y + 0.05, imp.z + rr * a0.sin()],
            b: [imp.x + rr * a1.cos(), imp.y + 0.05, imp.z + rr * a1.sin()],
            vel: [vout * am.cos() + imp.throw[0], vy, vout * am.sin() + imp.throw[1]],
            count: each,
            size,
            kind,
            spread,
            jitter,
            life,
            size_jitter: 0.6,
        });
        sent += each;
    }
    sent
}

/// This frame's share of one splash.
fn emit(imp: &mut Impact, dt: f32, lod: f32, out: &mut Vec<EmitRequest>, max: usize) -> u32 {
    let h = imp.height;
    let r = imp.radius;
    let v0 = imp.v0();
    let burst = imp.burst();
    let mut sent = 0;
    let im = *imp;
    // the core: this frame's share of the burst, the first particles fastest
    if imp.age < burst {
        let frac = dt.min(burst - imp.age) / burst;
        let s = (imp.age / burst).clamp(0.0, 1.0);
        let fly = |vy: f32| 2.0 * vy / GRAVITY * 1.05 + 0.5;
        let (lig, drop, white, sheet) = if imp.explosive {
            ((60.0 * h).clamp(20.0, 2400.0), (80.0 * h).clamp(30.0, 3000.0), (16.0 * h).clamp(4.0, 700.0), (4.0 * h).clamp(2.0, 160.0))
        } else if imp.swim {
            // a hand slap or a kick: mostly drops, a few strands, a little white water
            // (white water only from a hard slap: at a hand's scale the puff reads as a lamp)
            ((8.0 * h).clamp(1.0, 5.0), (60.0 * h).clamp(6.0, 30.0), if h > 0.3 { 1.0 } else { 0.0 }, 0.0)
        } else {
            (6.0, 14.0, 0.0, 0.0)
        };
        let c = &mut imp.carry;
        let vy = v0 * (1.0 - 0.35 * s);
        sent += class(&im, &mut c[0], lig * frac * lod, 0.35 * r, vy, 0.07 * v0, LIGAMENT, (0.010 + 0.0012 * h).min(0.05), fly(vy), 0.10 * v0, 0.25 * r, None, out, max);
        let vy = 0.55 * v0 * (1.0 - 0.3 * s);
        sent += class(&im, &mut c[1], drop * frac * lod, 0.65 * r, vy, 0.22 * v0, DROPLET, (0.004 + 0.0003 * h).min(0.012), fly(vy), 0.16 * v0, 0.3 * r, None, out, max);
        if white > 0.0 {
            let vy = 0.8 * v0 * (1.0 - 0.3 * s);
            sent += class(&im, &mut c[2], white * frac * lod, 0.45 * r, vy, 0.10 * v0, SPRAY, (0.12 + 0.025 * h).min(1.5), fly(vy) - 0.1, 0.12 * v0, 0.3 * r, None, out, max);
        }
        if sheet > 0.0 {
            sent += class(&im, &mut c[3], sheet * frac * lod, 0.9 * r, 0.3 * v0, 0.3 * v0, SHEET, 0.1 + 0.012 * h, 1.2 + 0.03 * h, 0.1 * v0, 0.2 * r, None, out, max);
        }
    }
    // (explosions) mist where the column tops out, and rolling out at its foot
    if imp.explosive {
        let tp = imp.peak();
        let (m0, m1) = (0.4 * tp, 1.0 * tp);
        if imp.age + dt > m0 && imp.age < m1 {
            let frac = (imp.age + dt).min(m1) - imp.age.max(m0);
            let frac = frac.max(0.0) / (m1 - m0);
            let total = (5.0 * h).clamp(3.0, 250.0) * frac * lod;
            let top = (0.3 + 0.6 * ((imp.age - m0) / (m1 - m0)).clamp(0.0, 1.0)) * h;
            let life = 3.0 + 0.08 * h;
            let c = &mut imp.carry;
            sent += class(&im, &mut c[4], total * 0.6, 0.0, 0.05 * v0, 0.0, MIST, (0.4 + 0.07 * h).min(4.0), life, 0.1 * v0, 0.5 * r, Some((0.25 * h, top)), out, max);
            sent += class(&im, &mut c[5], total * 0.4, 1.2 * r, 0.03 * v0, 0.08 * v0, MIST, (0.6 + 0.08 * h).min(5.0), life, 0.05 * v0, 0.3 * r, None, out, max);
        }
    }
    sent
}

#[cfg(test)]
mod tests {
    use super::*;

    fn event(kind: f32, hit: f32, range: f32) -> WgrWaterInteractionEvent {
        WgrWaterInteractionEvent {
            position_radius: [100.0, 200.0, 3.5, 4.5],
            velocity_kind: [0.0, 0.0, -25.0, kind],
            time_life_foam_mass: [0.0, 1.8, 1.0, hit],
            direction_depth_flags: [0.0, 0.0, range, 1.0],
        }
    }

    #[test]
    fn plume_sizes_follow_the_ammo() {
        // 7.62 mm, 120 mm HE, laser-guided bomb (CfgAmmo values)
        let b = plume_height(false, 8.0, 0.1);
        let he = plume_height(true, 150.0, 8.0);
        let lgb = plume_height(true, 3500.0, 15.0);
        assert!((0.4..0.7).contains(&b), "{b}");
        assert!((13.0..17.0).contains(&he), "{he}");
        assert!((45.0..60.0).contains(&lgb), "{lgb}");
        // HEAT (small blast radius) makes less of a plume than HE of the same calibre
        assert!(plume_height(true, 300.0, 1.0) < plume_height(true, 150.0, 8.0));
    }

    #[test]
    fn an_explosion_throws_a_plume_and_marks_the_surface() {
        let mut im = Impacts::default();
        // events without an ammo (rotor wash, the old explosion path) are ignored
        im.submit(&[event(KIND_EXPLOSION, 0.0, 0.0), event(KIND_BULLET, 0.0, 0.0)], 0.0);
        assert_eq!(im.active(), 0);
        im.submit(&[event(KIND_EXPLOSION, 150.0, 8.0)], 1.0);
        assert_eq!(im.active(), 1);
        let mut total = 0;
        let mut t = 0.0;
        while im.active() > 0 && t < 5.0 {
            let mut out = Vec::new();
            im.update(1.0 / 60.0, [100.0, 5.0, 150.0], &mut out, 64);
            assert!(out.len() <= 64);
            total += out.iter().map(|r| r.count).sum::<u32>();
            for r in &out {
                assert!(r.a[1] >= 1.0 && r.vel[1] >= 0.0);
            }
            t += 1.0 / 60.0;
        }
        assert!(im.active() == 0 && t < 3.0, "the plume stops emitting ({t} s)");
        assert!((2000..4000).contains(&total), "{total} particles");
        let lanes = im.lanes();
        assert_eq!(lanes[0][0], 100.0);
        assert!(lanes[0][3] > 13.0 && lanes[1][2] > 0.2);
    }

    #[test]
    fn a_bullet_throws_a_small_spout_only() {
        let mut im = Impacts::default();
        im.submit(&[event(KIND_BULLET, 8.0, 0.1)], 0.0);
        let mut total = 0;
        for _ in 0..30 {
            let mut out = Vec::new();
            im.update(1.0 / 60.0, [100.0, 2.0, 190.0], &mut out, 64);
            total += out.iter().map(|r| r.count).sum::<u32>();
        }
        assert!((10..30).contains(&total), "{total}");
        assert_eq!(im.lanes()[0][3], 0.0, "no surface mark");
    }

    #[test]
    fn a_swimmers_splash_rises_from_the_wave_and_follows_the_limb() {
        let mut im = Impacts::default();
        // a hand slapping in on a crest 2.5 m above the mean sea, moving +x at 2 m/s
        let e = WgrWaterInteractionEvent {
            position_radius: [10.0, 20.0, 0.12, 2.5],
            velocity_kind: [2.0, 0.0, -1.5, KIND_SWIM_SPLASH],
            time_life_foam_mass: [0.0, 0.0, 0.0, 0.3],
            direction_depth_flags: [0.0; 4],
        };
        im.submit(&[e], 0.0);
        assert_eq!(im.active(), 1);
        let mut total = 0;
        for _ in 0..20 {
            let mut out = Vec::new();
            im.update(1.0 / 60.0, [12.0, 4.0, 20.0], &mut out, 64);
            for r in &out {
                assert!(r.a[1] >= 2.5 && r.vel[1] > 0.0, "from the crest, upward");
                assert!(r.vel[0] > -1.0, "thrown on with the hand");
            }
            total += out.iter().map(|r| r.count).sum::<u32>();
        }
        assert!((8..40).contains(&total), "{total}");
        assert_eq!(im.active(), 0);
        assert_eq!(im.lanes()[0][3], 0.0, "no surface mark");
    }
}
