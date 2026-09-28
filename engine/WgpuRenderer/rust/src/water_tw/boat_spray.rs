//! Tidewater's BoatSpray.js — spray thrown off the hull — ported from dgreenheck/tidewater
//! @ 4811ba48 (MIT, © DRG Software Solutions LLC; its contact model is noted there as being
//! "after threejs-water-pro", the same author's product: used here under Tidewater's MIT grant,
//! on the owner's decision). W6i.
//!
//! Per boat (the two with a simulated wake, `wake.rs`): three contact segments per side along the
//! forebody waterline, each with a velocity-driven source (the face pushing water aside) and an
//! impact-driven one (the rate it is being immersed); emission ~ length x response, launched along
//! the face, outward and up; drops from every wetted segment, and from the side's strongest one the
//! clear sheet, ligaments and (on impacts) torn white water and mist; slams throw a burst; the
//! propeller race kicks up drops near the surface. The particles go into the spray ring's CPU
//! tail (`spray.rs`) and collide with the boats' hulls.
//!
//! What OP changes:
//! * **No hull lines.** The contacts and the collision outline follow the analytic hull of
//!   `wake.rs` (half beam, half length, draft, freeboard from the producer).
//! * **The water at the contacts** is OP's CPU sea (QueryWaterSurfaceScaled: the sea the boat
//!   floats on), sampled by the producer at the same points (PresentationSnapshot.cpp
//!   `WakeSprayContactPoint`, which must match `contact_points` here). Tidewater samples its full
//!   water query, which also carries the boat's own wake.
//! * **Point velocity is the boat's velocity** (no angular term: OP does not hand the rotation
//!   rate to the renderer; the impact source still sees pitch and heave through the immersion
//!   rate).
//! * **Thrust** is OP's 0..1 engine thrust scaled to Tidewater's 26 kN bollard pull.
//! * **Frame-rate independent cap:** Tidewater's 150 particles per frame is per 60 Hz frame here
//!   (OP runs well above 60 fps, which would otherwise wrap the 8192-slot tail within ~0.3 s).
//! * **Two boats** share the request budget (64 a frame, `spray::MAX_REQUESTS`); no slam audio
//!   hook; no wheelhouse box in the collision body.

use super::spray::{Body, EmitRequest};
use crate::ffi::WgrWaterParams;

const GRAVITY: f32 = 9.81;
const SEGMENTS: usize = 3;
const CONTACTS: usize = 2 * SEGMENTS;
/// drops per unit of demand (m of waterline x m/s of response) per second
const EMIT_RATE: f32 = 300.0;
/// particle cap per (60 Hz) frame for one bow (both sides)
const MAX_PER_FRAME: f32 = 150.0;
/// Tidewater's bollard pull (N) for the propeller race
const MAX_THRUST_N: f32 = 26000.0;

/// OP (W6i.1): white water thrown by the steady bow wave at speed, x `WGR_TW_BOAT_SPRAY=<gain>`
/// (default 1; 0 = Tidewater's model, where a steady bow throws clear water only and the white
/// comes from impacts). Tidewater's clear sheet and sub-pixel drops conserve the water's
/// cross-section, which is right for a small launch at 6-8 m/s but reads as almost nothing on a
/// patrol boat at 25 kn in a calm sea, whose bow wave breaks into a white spray sheet along the
/// forward third of the hull. Scaled with the face's response, so it only appears at planing
/// speeds.
const STEADY_WHITE: f32 = 0.06;
/// OP (W6i.8): the drops thrown from every wetted segment, x 0.5. With the view-space fix (W6i.7)
/// the spray finally drew, and at a patrol boat's 13 m/s the full count read as a bright fan of
/// streaks two beams wide in a side view; half keeps the sheet and wings of a real bow wave.
const DROP_SHARE: f32 = 0.5;
const STEADY_MIST: f32 = 0.015;

/// W6k: `WGR_TW_BOAT_SPRAY_TUNE=cap=..,drop=..,lig=..,white=..,mist=..,up=..` (any subset) scales
/// the per-frame particle cap, the drops, ligaments, white water, mist and the upward launch
/// speed, for tuning captures, relative to W6i.8's levels. Defaults (W6l, arm C of
/// .tw/shots-spray-tune*): at W6i.8's levels a patrol boat settling its bow back in at 17 m/s
/// disappeared in white from the quarter view (both the PBR and BoatE); these keep the bow sheet
/// and the wings of spray along the forward third without burying the hull.
#[derive(Clone, Copy, Debug)]
struct Tune {
    cap: f32,
    drop: f32,
    lig: f32,
    white: f32,
    mist: f32,
    up: f32,
}

fn tune() -> Tune {
    static V: std::sync::OnceLock<Tune> = std::sync::OnceLock::new();
    *V.get_or_init(|| {
        let mut t = Tune { cap: 0.45, drop: 1.0, lig: 0.35, white: 0.35, mist: 0.15, up: 0.8 };
        if let Ok(v) = std::env::var("WGR_TW_BOAT_SPRAY_TUNE") {
            for kv in v.split(',') {
                let mut it = kv.splitn(2, '=');
                let (Some(k), Some(x)) = (it.next(), it.next().and_then(|x| x.trim().parse::<f32>().ok())) else { continue };
                let x = x.clamp(0.0, 10.0);
                match k.trim() {
                    "cap" => t.cap = x,
                    "drop" => t.drop = x,
                    "lig" => t.lig = x,
                    "white" => t.white = x,
                    "mist" => t.mist = x,
                    "up" => t.up = x,
                    _ => {}
                }
            }
        }
        t
    })
}

fn white_gain() -> f32 {
    static V: std::sync::OnceLock<f32> = std::sync::OnceLock::new();
    *V.get_or_init(|| std::env::var("WGR_TW_BOAT_SPRAY").ok().and_then(|v| v.trim().parse::<f32>().ok()).unwrap_or(1.0).clamp(0.0, 10.0))
}

const DROPLET: f32 = 0.0;
const MIST: f32 = 1.0;
const LIGAMENT: f32 = 2.0;
const SPRAY: f32 = 3.0;
const SHEET: f32 = 4.0;

type V3 = [f32; 3];
fn add(a: V3, b: V3) -> V3 {
    [a[0] + b[0], a[1] + b[1], a[2] + b[2]]
}
fn scale(a: V3, s: f32) -> V3 {
    [a[0] * s, a[1] * s, a[2] * s]
}
fn dot(a: V3, b: V3) -> f32 {
    a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
}
fn norm(a: V3) -> V3 {
    let l = dot(a, a).sqrt();
    if l > 1e-6 { scale(a, 1.0 / l) } else { [0.0, 0.0, 1.0] }
}
/// three.js MathUtils.smoothstep(x, min, max)
fn smooth(x: f32, lo: f32, hi: f32) -> f32 {
    if x <= lo {
        return 0.0;
    }
    if x >= hi {
        return 1.0;
    }
    let t = (x - lo) / (hi - lo);
    t * t * (3.0 - 2.0 * t)
}

/// A spray source (BoatSpray.js `Source`): the driving speed above a threshold, scaled, bounded.
#[derive(Clone, Copy)]
struct Source {
    threshold: f32,
    scale: f32,
    intensity: f32,
    lifetime: f32,
}
impl Source {
    fn response(&self, speed: f32) -> f32 {
        ((speed - self.threshold).max(0.0) * self.scale).min(30.0)
    }
}
const VELOCITY: Source = Source { threshold: 0.4, scale: 1.6, intensity: 1.0, lifetime: 0.8 };
const IMPACT: Source = Source { threshold: 0.6, scale: 1.2, intensity: 3.0, lifetime: 1.4 };

/// The analytic waterline's half breadth (wake.rs `bottom_at`: parallel aft, a fine entry over the
/// forward 45 %: B (1 - e^1.3), about 30 degrees half-angle at the stem for a patrol boat).
fn waterline_half_beam(b: f32, l: f32, z: f32) -> f32 {
    let u = (z + l) / (2.0 * l);
    if !(0.0..=1.0).contains(&u) {
        return 0.0;
    }
    if u < 0.55 {
        return b;
    }
    let e = (u - 0.55) / 0.45;
    b * (1.0 - e.powf(1.3)).max(0.0)
}

/// The contact segments' end points (boat frame): per side (0-2: +x, 3-5: -x) from the stem aft
/// to where the entrance has opened to 92 % of the beam, stations bunched toward the stem.
/// MUST MATCH PresentationSnapshot.cpp `WakeSprayContactPoint` (which samples the sea at the mids).
fn contact_points(b: f32, l: f32) -> [(V3, V3, f32); CONTACTS] {
    let z_stem = l - 0.02;
    // where 1 - e^1.3 = 0.92: u = 0.55 + 0.45 x 0.08^(1/1.3)
    let z_shoulder = 0.2287 * l;
    let mut out = [([0.0; 3], [0.0; 3], 1.0); CONTACTS];
    for (k, o) in out.iter_mut().enumerate() {
        let side = if k < SEGMENTS { 1.0 } else { -1.0 };
        let seg = k % SEGMENTS;
        let station = |i: usize| -> V3 {
            let f = (i as f32 / SEGMENTS as f32).powf(1.3);
            let z = z_stem + (z_shoulder - z_stem) * f;
            [side * (waterline_half_beam(b, l, z) + 0.03), 0.05, z]
        };
        *o = (station(seg), station(seg + 1), side);
    }
    out
}

/// One boat's inputs (`WgrWaterParams::tidewater_wake`, the boat's 8 lanes).
#[derive(Clone, Copy, Debug, Default)]
pub struct SprayBoat {
    pub id: u32,
    pub origin: V3,
    pub x: V3,
    pub y: V3,
    pub z: V3,
    pub vel: V3,
    pub thrust: f32,
    pub driven: bool,
    pub half_beam: f32,
    pub half_len: f32,
    pub draft: f32,
    pub freeboard: f32,
    pub hw: [f32; CONTACTS],
    pub hw_prop: f32,
}

impl SprayBoat {
    pub fn from_params(p: &WgrWaterParams) -> [Option<SprayBoat>; 2] {
        let mut out = [None; 2];
        for (i, o) in out.iter_mut().enumerate() {
            let l = &p.tidewater_wake[i * 8..i * 8 + 8];
            let flags = l[2][3] as u32;
            if flags & 1 == 0 {
                continue;
            }
            let f = norm([l[4][0], l[4][1], l[4][2]]);
            let u = norm([l[5][0], l[5][1], l[5][2]]);
            // x = up x forward (the producer samples the sea along the same axis)
            let x = norm([u[1] * f[2] - u[2] * f[1], u[2] * f[0] - u[0] * f[2], u[0] * f[1] - u[1] * f[0]]);
            let all = l.iter().flatten().all(|v| v.is_finite());
            if !all || dot(u, u) < 0.5 {
                continue;
            }
            let half_len = l[2][1].clamp(1.0, 45.0);
            *o = Some(SprayBoat {
                id: l[1][3] as u32,
                origin: [l[0][0], l[3][0], l[0][1]],
                x,
                y: u,
                z: f,
                vel: [l[1][0], l[3][1], l[1][1]],
                thrust: l[1][2].abs().min(1.0),
                driven: flags & 2 != 0,
                half_beam: l[2][0].clamp(0.4, 12.0).min(0.42 * half_len),
                half_len,
                draft: l[2][2].clamp(0.15, 5.0),
                freeboard: l[3][2].clamp(0.3, 20.0),
                hw: [l[6][0], l[6][1], l[6][2], l[6][3], l[7][0], l[7][1]],
                hw_prop: l[3][3],
            });
        }
        out
    }

    fn to_world(&self, p: V3) -> V3 {
        add(add(add(self.origin, scale(self.x, p[0])), scale(self.y, p[1])), scale(self.z, p[2]))
    }
    fn dir_to_world(&self, d: V3) -> V3 {
        add(add(scale(self.x, d[0]), scale(self.y, d[1])), scale(self.z, d[2]))
    }

    /// The collision outline (BoatSpray.js `shape`, from the analytic hull): full beam aft of the
    /// shoulder, elliptic bow; flared and raked a little from the waterline to the sheer.
    fn outline(&self) -> ([f32; 4], [f32; 3], [f32; 3]) {
        let (b, l) = (self.half_beam, self.half_len);
        // at the waterline: shoulder where the entrance reaches 95 % of the beam
        let wl = [0.19 * l, l, b + 0.01];
        let sheer = [-l, 0.35 * l, l * 1.04, b * 1.05 + 0.02];
        let ys = [self.freeboard + 0.04, self.freeboard * 1.2 + 0.04, -self.draft - 0.3];
        (sheer, wl, ys)
    }

    pub fn body(&self) -> Body {
        let (hull, hull_wl, sheer) = self.outline();
        Body { origin: self.origin, x: self.x, y: self.y, z: self.z, vel: self.vel, hull, hull_wl, sheer }
    }

    /// Half breadth of the collision outline at boat-frame z and height y (Spray._collideBody).
    fn half_beam_at(&self, z: f32, y: f32) -> f32 {
        let (h, w, s) = self.outline();
        let lerp = |a: f32, b: f32, t: f32| a + (b - a) * t;
        let sheer = lerp(s[0], s[1], ((z - h[0]) / (h[2] - h[0])).clamp(0.0, 1.0));
        let f = (y / sheer.max(0.1)).clamp(0.0, 1.0);
        let (z_sh, z_st, hb) = (lerp(w[0], h[1], f), lerp(w[1], h[2], f), lerp(w[2], h[3], f));
        if z >= z_st {
            return 0.0;
        }
        let e = ((z - z_sh) / (z_st - z_sh).max(0.01)).clamp(0.0, 1.0);
        hb * (1.0 - e * e).max(0.02).sqrt()
    }
}

#[derive(Clone, Copy, Default)]
struct Contact {
    depth: f32,
    rate: f32,
    primed: bool,
    carry: [f32; 5],
}

/// W6j diagnostics per boat, accumulated over a window of stepped frames (WGR_TW_WAKE_STATS).
#[derive(Clone, Copy, Default, Debug)]
pub struct SprayDiag {
    pub frames: u32,
    /// particles thrown per kind: drops, mist, ligaments, white water, sheet
    pub kinds: [u32; 5],
    /// the largest slam burst, the mean impact share of the demand
    pub burst_max: f32,
    pub imp_sum: f32,
    /// contact depth (hw - hull point, m) min / max over the stem contacts, the largest immersion rate
    pub stem_depth: [f32; 2],
    pub rate_max: f32,
    /// the producer's frame: origin height above the sea at the stem contacts, draft, freeboard, up.y
    pub pose: [f32; 4],
}

#[derive(Clone, Copy, Default)]
struct BoatState {
    id: Option<u32>,
    contacts: [Contact; CONTACTS],
    burst: f32,
    wash_carry: f32,
    /// seen this frame
    live: bool,
    diag: SprayDiag,
}

/// The boats' spray emitters (one state per boat, kept by id).
#[derive(Default)]
pub struct BoatSpray {
    boats: [BoatState; 2],
    /// the last stepped frame's (particles, requests), for the log
    pub stats: (u32, u32),
    /// W6j: the last full diagnostics window per boat slot (id, diag)
    pub diag: [Option<(u32, SprayDiag)>; 2],
    /// W6j.1: every window since the last drain, with the water clock (WGR_TW_SPRAY_DIAG=1)
    pub history: Vec<(f32, u32, SprayDiag)>,
    clock: f32,
}

/// `WGR_TW_SPRAY_DIAG=1`: keep every bow-spray diagnostics window for the log (W6j.1).
pub fn diag_history_on() -> bool {
    static V: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *V.get_or_init(|| std::env::var("WGR_TW_SPRAY_DIAG").is_ok_and(|v| v.trim() == "1"))
}

struct Emitter<'a> {
    out: &'a mut Vec<EmitRequest>,
    max: usize,
    particles: u32,
    kinds: [u32; 5],
}

impl Emitter<'_> {
    #[allow(clippy::too_many_arguments)]
    fn emit(&mut self, carry: &mut f32, count: f32, a: V3, b: V3, vel: V3, kind: f32, size: f32, life: f32, spread: f32, jitter: f32, size_jitter: f32) {
        *carry += count;
        let m = carry.floor();
        if m <= 0.0 {
            return;
        }
        *carry -= m;
        if self.out.len() >= self.max {
            return;
        }
        self.out.push(EmitRequest { a, b, vel, count: m as u32, size, kind, spread, jitter, life, size_jitter });
        self.particles += m as u32;
        self.kinds[(kind as usize).min(4)] += m as u32;
    }
}

impl BoatSpray {
    /// This frame's emit requests (appended to `out`, at most `max_requests` in all) for the boats;
    /// `dt`: the water clock step (0 while paused: nothing is thrown).
    pub fn update(&mut self, boats: &[Option<SprayBoat>; 2], dt: f32, out: &mut Vec<EmitRequest>, max_requests: usize) {
        for s in &mut self.boats {
            s.live = false;
        }
        self.clock += dt;
        let mut em = Emitter { out, max: max_requests, particles: 0, kinds: [0; 5] };
        for b in boats.iter().flatten() {
            // the state kept for this boat, else a free one
            let i = match self.boats.iter().position(|s| s.id == Some(b.id)) {
                Some(i) => i,
                None => {
                    let Some(i) = self.boats.iter().position(|s| s.id.is_none() || !s.live) else { continue };
                    self.boats[i] = BoatState { id: Some(b.id), ..Default::default() };
                    i
                }
            };
            self.boats[i].live = true;
            if dt > 0.0 {
                let before = em.kinds;
                Self::update_boat(&mut self.boats[i], b, dt.min(1.0 / 20.0), &mut em);
                let d = &mut self.boats[i].diag;
                for k in 0..5 {
                    d.kinds[k] += em.kinds[k] - before[k];
                }
                d.frames += 1;
                if d.frames >= 60 {
                    self.diag[i] = Some((b.id, *d));
                    if diag_history_on() && self.history.len() < 4096 {
                        self.history.push((self.clock, b.id, *d));
                    }
                    *d = SprayDiag::default();
                }
            }
        }
        for s in &mut self.boats {
            if !s.live {
                s.id = None;
            }
        }
        // (the last frame the water clock moved: frames without a step throw nothing)
        if dt > 0.0 {
            self.stats = (em.particles, em.out.len() as u32);
        }
    }

    fn update_boat(st: &mut BoatState, b: &SprayBoat, dt: f32, em: &mut Emitter<'_>) {
        let fwd = b.z;
        let speed = dot(b.vel, fwd).max(0.0);
        let pts = contact_points(b.half_beam, b.half_len);

        // ---- contacts: normal speed, immersion rate, demand
        struct C {
            demand_v: f32,
            demand_i: f32,
            r_v: f32,
            r_i: f32,
            n: V3,
            hw: f32,
        }
        let mut cs: Vec<C> = Vec::with_capacity(CONTACTS);
        let mut total = 0.0;
        let mut slam: f32 = 0.0;
        for (k, (a, bb, side)) in pts.iter().enumerate() {
            let c = &mut st.contacts[k];
            let mid = scale(add(*a, *bb), 0.5);
            let m = b.to_world(mid);
            let hw = b.hw[k];
            let depth = hw - m[1];
            let rate = if c.primed { (depth - c.depth) / dt } else { 0.0 };
            c.rate += (rate - c.rate) * (dt / 0.03).min(1.0);
            c.depth = depth;
            c.primed = true;
            let wet = smooth(depth, -0.35, -0.08) * (1.0 - smooth(depth, 0.7, 1.1));
            // the outward (and forward) normal of the segment, horizontal
            let t = norm([bb[0] - a[0], 0.0, bb[2] - a[2]]);
            let nb = [-side * t[2], 0.0, side * t[0]];
            let nw = b.dir_to_world(nb);
            let n = norm([nw[0], 0.0, nw[2]]);
            let vn = (b.vel[0] * n[0] + b.vel[2] * n[2]).max(0.0);
            let len = ((bb[0] - a[0]).powi(2) + (bb[2] - a[2]).powi(2)).sqrt();
            let r_v = VELOCITY.response(vn);
            let r_i = IMPACT.response(c.rate);
            let demand_v = len * r_v * VELOCITY.intensity * wet;
            let demand_i = len * r_i * IMPACT.intensity * wet * (0.15 + 0.85 * smooth(speed, 2.0, 6.0));
            total += demand_v + demand_i;
            // slam: the stem dropping hard into the water in a head sea
            if k % SEGMENTS == 0 && wet > 0.3 && speed > 2.5 {
                slam = slam.max(((c.rate - 1.8) * 0.4 + speed * 0.02).min(1.0));
            }
            cs.push(C { demand_v, demand_i, r_v, r_i, n, hw });
        }

        // ---- emission, bounded per frame; a slam throws a burst
        st.burst = (st.burst * (-dt / 0.15).exp()).max(slam);
        let burst = st.burst;
        {
            let d = &mut st.diag;
            let (mut dmin, mut dmax, mut rmax, mut di, mut dt_) = (f32::MAX, f32::MIN, 0.0f32, 0.0, 0.0);
            for k in 0..CONTACTS {
                if k % SEGMENTS == 0 {
                    dmin = dmin.min(st.contacts[k].depth);
                    dmax = dmax.max(st.contacts[k].depth);
                }
                rmax = rmax.max(st.contacts[k].rate.abs());
                di += cs[k].demand_i;
                dt_ += cs[k].demand_v + cs[k].demand_i;
            }
            if d.frames == 0 {
                d.stem_depth = [dmin, dmax];
            } else {
                d.stem_depth = [d.stem_depth[0].min(dmin), d.stem_depth[1].max(dmax)];
            }
            d.rate_max = d.rate_max.max(rmax);
            d.burst_max = d.burst_max.max(burst);
            d.imp_sum += if dt_ > 1e-4 { di / dt_ } else { 0.0 };
            d.pose = [b.origin[1] - 0.5 * (b.hw[0] + b.hw[SEGMENTS]), b.draft, b.freeboard, b.y[1]];
        }
        let boost = 1.0 + 2.5 * burst;
        // (OP: the cap is per 60 Hz frame, so the particle rate does not grow with the frame rate)
        let tn = tune();
        let budget = (MAX_PER_FRAME * tn.cap * dt * 60.0 / (total * EMIT_RATE * dt * boost * 1.3).max(1e-6)).min(1.0);
        let making = smooth(speed, 3.0, 6.0);
        // per side: the segment that throws most also throws the sheet, ligaments, white water, mist
        let mut best = [None::<usize>; 2];
        let mut side_n = [0.0f32; 2];
        let mut n_emit = [0.0f32; CONTACTS];
        for (k, c) in cs.iter().enumerate() {
            let d = c.demand_v + c.demand_i;
            let s = k / SEGMENTS;
            n_emit[k] = d * EMIT_RATE * dt * budget * boost;
            side_n[s] += n_emit[k];
            if d > 1e-4 && best[s].is_none_or(|j| d > cs[j].demand_v + cs[j].demand_i) {
                best[s] = Some(k);
            }
        }
        for (k, c) in cs.iter().enumerate() {
            let d = c.demand_v + c.demand_i;
            if d <= 1e-4 {
                continue;
            }
            let (a, bb, side) = pts[k];
            let n = n_emit[k];
            let imp = c.demand_i / d;
            let r = c.r_v * (1.0 - imp) + c.r_i * imp;
            // launch: tangential hull velocity (part of it: the sheet streams aft past the hull),
            // outward and up (a slam throws it higher and wider)
            let vn = b.vel[0] * c.n[0] + b.vel[2] * c.n[2];
            let mut v = scale(add(b.vel, scale(c.n, -vn)), 0.6);
            v[1] = b.vel[1] * 0.3;
            let up = (c.r_v * 0.55 * (1.0 - imp) + c.r_i * 1.15 * imp + burst * 2.5) * tn.up;
            v = add(v, scale(c.n, 0.8 * r + 0.4 + burst * 1.0));
            v[1] += up;
            // the sheet leaves from where the surface meets the hull, raised by the stagnation rise
            let rise = (vn * vn / (2.0 * GRAVITY) * 0.5).min(0.5);
            let oy = b.origin[1];
            let ya = (c.hw + rise - oy).clamp(-0.3, 0.9);
            let yb = (c.hw + rise * 0.6 - oy).clamp(-0.3, 0.9);
            let mut pa = b.to_world([side * (b.half_beam_at(a[2], ya) + 0.05), ya, a[2]]);
            let mut pb = b.to_world([side * (b.half_beam_at(bb[2], yb) + 0.05), yb, bb[2]]);
            pa[1] = c.hw + rise;
            pb[1] = c.hw + rise * 0.6;
            let spread = (0.3 * (0.65 * r).hypot(up) + 0.15) * (1.0 + burst);
            let life = VELOCITY.lifetime * (1.0 - imp) + IMPACT.lifetime * imp;
            let carry = &mut st.contacts[k].carry;
            // drops (3-7 mm) from every wetted segment
            em.emit(&mut carry[0], n * DROP_SHARE * tn.drop, pa, pb, v, DROPLET, 0.005 + 0.0006 * r, life, spread, 0.05, 0.6);
            let s = k / SEGMENTS;
            if best[s] != Some(k) {
                continue;
            }
            let nn = side_n[s];
            // a few fragments of the clear sheet at the root (they tear into strands and drop clusters)
            em.emit(&mut carry[1], nn * 0.025 * making * (1.0 - burst * 0.5), pa, pb, v, SHEET, 0.12 + 0.02 * r, 0.45, spread * 0.5, 0.06, 0.6);
            // ligaments torn off the sheet: the readable blobs of water
            em.emit(&mut carry[2], nn * 0.3 * smooth(r, 1.0, 5.0) * tn.lig, pa, pb, v, LIGAMENT, 0.011 + 0.0012 * r, life, spread, 0.05, 0.6);
            // white water on impacts (torn white sheets) and (OP) from the steady bow wave at speed
            let steady = (1.0 - imp) * smooth(c.r_v, 4.0, 12.0) * white_gain();
            em.emit(
                &mut carry[3],
                nn * (0.06 * imp * smooth(c.r_i, 1.5, 5.0) + 0.1 * burst + STEADY_WHITE * steady) * making * tn.white,
                pa,
                pb,
                v,
                SPRAY,
                0.04 + 0.004 * r + 0.03 * burst,
                0.6 + 0.4 * imp,
                spread,
                0.12,
                0.6,
            );
            // a fine, faint mist on impacts that the relative wind carries aft over the boat
            em.emit(
                &mut carry[4],
                nn * (0.04 * imp + 0.15 * burst + STEADY_MIST * steady) * making * tn.mist,
                pa,
                pb,
                scale(v, 0.6),
                MIST,
                0.3 + 0.2 * burst,
                2.0,
                spread * 0.5,
                0.15,
                0.6,
            );
        }

        // ---- propeller race: drops kicked up when the churn is near the surface
        let prop = b.to_world([0.0, -0.5 * b.draft.min(1.0), -b.half_len + 0.3]);
        let prop_depth = b.hw_prop - prop[1];
        let thrust_n = b.thrust * MAX_THRUST_N;
        let activity = if b.driven && prop_depth > -0.2 { (thrust_n / 1000.0).sqrt() * (-prop_depth.max(0.0) / 0.45).exp() } else { 0.0 };
        if activity > 0.05 {
            let stern = b.to_world([0.0, 0.05, -b.half_len + 0.1]);
            if b.hw_prop - stern[1] > -0.3 {
                st.wash_carry += activity * 45.0 * dt;
                let mut v = add(scale(b.vel, 0.6), scale(fwd, -(0.8 + activity * 0.6)));
                v[1] = 0.5 + activity * 0.5;
                em.emit(&mut st.wash_carry, 0.0, stern, stern, v, DROPLET, 0.005, 0.8, 0.8, 0.35, 0.5);
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn contacts_run_from_the_stem_to_the_shoulder_on_both_sides() {
        let pts = contact_points(1.8, 4.6);
        // the first station is at the stem, the last where the entrance reaches 92 % of the beam
        assert!((pts[0].0[2] - 4.58).abs() < 1e-4);
        assert!((waterline_half_beam(1.8, 4.6, pts[2].1[2]) / 1.8 - 0.92).abs() < 0.01, "{}", waterline_half_beam(1.8, 4.6, pts[2].1[2]));
        for k in 0..3 {
            assert!(pts[k].0[0] > 0.0 && pts[k + 3].0[0] < 0.0);
            assert_eq!(pts[k].0[2], pts[k + 3].0[2]);
        }
    }

    #[test]
    fn a_boat_under_way_throws_spray_and_a_boat_at_rest_does_not() {
        let mut b = SprayBoat {
            id: 7,
            origin: [0.0, 0.0, 0.0],
            x: [1.0, 0.0, 0.0],
            y: [0.0, 1.0, 0.0],
            z: [0.0, 0.0, 1.0],
            vel: [0.0; 3],
            thrust: 0.0,
            driven: true,
            half_beam: 1.8,
            half_len: 4.6,
            draft: 0.4,
            freeboard: 1.0,
            hw: [0.0; CONTACTS],
            hw_prop: 0.0,
        };
        let mut s = BoatSpray::default();
        let mut out = Vec::new();
        for _ in 0..10 {
            out.clear();
            s.update(&[Some(b), None], 1.0 / 60.0, &mut out, 64);
        }
        assert!(out.is_empty(), "{out:?}");
        b.vel = [0.0, 0.0, 12.0];
        b.thrust = 1.0;
        let mut n = 0;
        for _ in 0..30 {
            out.clear();
            s.update(&[Some(b), None], 1.0 / 60.0, &mut out, 64);
            n += out.iter().map(|r| r.count).sum::<u32>();
        }
        assert!(n > 0);
        assert!(out.len() <= 64);
    }
}
