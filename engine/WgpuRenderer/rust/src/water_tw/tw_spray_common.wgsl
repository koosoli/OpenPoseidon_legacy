// Tidewater Native — spray particles (W4a): the helpers shared by the emitters (the breaker
// kernel), the update kernel and the sprite material. `Spray.js` sprayCommon (dgreenheck/tidewater
// @ 4811ba48, MIT, © DRG Software Solutions LLC), plus OP's per-frame spray uniform.

const SPRAY_DROPLET: f32 = 0.0;
const SPRAY_MIST: f32 = 1.0;
const SPRAY_LIGAMENT: f32 = 2.0;
const SPRAY_SPRAY: f32 = 3.0;
const SPRAY_SHEET: f32 = 4.0;

// OP: the per-frame spray block (spray.rs SprayParams)
struct SprayParams {
    seed: vec4<u32>,  // x = frame seed, y = particle slots (GPU ring + CPU tail: the arrays' stride),
                      // z = GPU ring size (power of two), w = OP sprite diagnostics (WGR_TW_SPRAY_DEBUG)
    a: vec4<f32>,     // x = dt, y = time, z = unused (the budget lives in the head buffer), w = emission gain (`spray`)
    b: vec4<f32>,     // x = emit range m, y = sprite intensity, z = max draw distance m, w = breaker spray on
    cam: vec4<f32>,   // camera position (world), w unused
    // W6i: the CPU tail (Spray.js cpuStart / cpuCount / reqCount) and the collision bodies
    req: vec4<u32>,   // x = first tail slot of this frame's requests, y = particles requested, z = requests, w = bodies
    // per body (the boats, up to 2), 8 lanes: origin xyz (the hull centre at the design waterline),
    // its x / y (up) / z (forward) axes, velocity, hull at the sheer (z aft, z shoulder, z stem,
    // half beam), at the waterline (z shoulder, z stem, half beam), sheer (y aft, y stem, y bottom)
    bodies: array<vec4<f32>, 16>,
};

// PCG hash of a uint -> [0, 1)
fn sprayHash(seed: u32) -> f32 {
    let state = seed * 747796405u + 2891336453u;
    let word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return f32((word >> 22u) ^ word) * (1.0 / 4294967296.0);
}

// select a per-kind constant
fn sprayByKind(kind: f32, a: f32, b: f32, c: f32, d: f32, e: f32) -> f32 {
    return select(select(select(select(e, d, kind < 3.5), c, kind < 2.5), b, kind < 1.5), a, kind < 0.5);
}

// Henyey-Greenstein phase (1/sr)
fn sprayPhaseHG(cosT: f32, g: f32) -> f32 {
    let g2 = g * g;
    return ((1.0 - g2) / (4.0 * PI)) / pow(max(1.0 + g2 - 2.0 * g * cosT, 1e-4), 1.5);
}
