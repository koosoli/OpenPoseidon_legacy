// Interior sky visibility, Stage 1 (docs/interior-sky-visibility-plan.md §4).
//
// A top-down orthographic DEPTH map of the retained object set over a world box around the
// camera. A fragment whose depth in that map sits BELOW the stored occluder depth has geometry
// above it — it is indoors, or under a porch/canopy — and its SKY AMBIENT is attenuated toward a
// floor. Direct sun and local lights are never touched: sun occlusion is already the cascade
// shadow maps' and the terrain sun-shadow mask's job, and the gap this closes is the ambient.
//
// Two things this module is responsible for getting right, both from the plan's risk list:
//
//   * The ortho origin is SNAPPED to the map's texel size. An unsnapped view resamples the world
//     every frame and the result crawls; there is no TAA here to hide it (which is precisely why
//     the GTAO mip march ended up default-off).
//   * Terrain is not in this map at all — it is not part of the retained instance set the sky cull
//     runs over. That is deliberate: terrain is never "above" the player, and including it would
//     make every hillside a roof.
//
// Stage 1.5 (2026-08-05) samples the DOME, not one direction: a zenith map plus four tilted ones.
// This is what the plan's §3b asked for and §3c deferred on a resolution argument — an argument
// taken at a 256 m box. At the small box these use (64 m half-extent, 2048 texels = 6 cm/texel) a
// 1.5 m window reveal is ~25 texels across, so a tilted map resolves it comfortably. A tilted map
// matters because its rays arrive near-HORIZONTALLY: it can see through a window, which a zenith
// map structurally cannot, at any resolution.

use crate::ffi::WgrSkyVis;

/// How many sky directions the map samples: the zenith plus `DIRECTION_COUNT - 1` tilted ones,
/// evenly spaced in azimuth. Fixed at compile time because the DIRECTIONS THEMSELVES are fixed in
/// world space — rotating them with the camera or the time of day would make every surface's
/// occlusion wobble, and there is no TAA here to hide that.
pub const DIRECTION_COUNT: usize = 5;

/// Tilt of the non-zenith directions from straight up, in degrees. ~50 deg keeps them steep
/// enough to still be dominated by the zenith in the cosine weighting, while being shallow enough
/// that a ray can clear a window reveal and reach a metre or two into the room.
const TILT_DEG: f32 = 50.0;

/// The sampled directions, pointing FROM the surface TOWARD the sky, world space. Index 0 is the
/// zenith. `w` carries the cosine weight (dot with up), so the caller never re-derives it and the
/// shader reads the same number the CPU used.
pub fn directions() -> [glam::Vec4; DIRECTION_COUNT] {
    let mut out = [glam::Vec4::new(0.0, 1.0, 0.0, 1.0); DIRECTION_COUNT];
    let tilt = TILT_DEG.to_radians();
    let (st, ct) = tilt.sin_cos();
    for (i, slot) in out.iter_mut().enumerate().skip(1) {
        let az = std::f32::consts::TAU * (i - 1) as f32 / (DIRECTION_COUNT - 1) as f32;
        let (sa, ca) = az.sin_cos();
        // cos(tilt) is both the y component and the cosine weight — the same quantity, which is
        // why the weight is not a second, drift-prone constant.
        *slot = glam::Vec4::new(st * ca, ct, st * sa, ct);
    }
    out
}

/// Runtime knobs. Default OFF: this changes global lighting, so it opts in.
#[derive(Clone, Copy, Debug)]
pub struct SkyVisSettings {
    pub enabled: bool,
    /// Depth-map resolution (square). 1024 over a 256 m box is 25 cm/texel.
    pub resolution: u32,
    /// HALF the world extent covered, in metres (the box is 2*extent on a side).
    pub extent: f32,
    /// How far above / below the camera the ortho box reaches, in metres. Must clear the
    /// tallest roof the player can stand under, plus the deepest floor they can stand on.
    pub height: f32,
    /// Blend toward the occluded result, 0 = feature inert, 1 = full effect.
    pub strength: f32,
    /// Minimum ambient multiplier in a fully enclosed volume. OFP interiors carry very few
    /// local lights, so a hard 0 here is an unplayable black box — see the plan's risk list.
    pub floor: f32,
    /// Softening kernel radius in METRES. Roughly how far light appears to reach in past an
    /// opening: under the middle of a roof every tap is blocked, at its edge some see sky.
    pub kernel: f32,
    /// Depth bias in metres, subtracted from the receiver's depth before the comparison, so a
    /// surface that is its OWN highest geometry (open ground, a crate in the street) does not
    /// shadow itself.
    pub bias: f32,
    /// How far to steer the sky-irradiance lookup toward the direction light actually arrives
    /// from, 0 = off (uniform dimming only), 1 = fully along the open direction. This is the part
    /// that makes a room read as LIT THROUGH ITS WINDOW rather than evenly darker: visibility
    /// alone only scales brightness, it never says where the light came from.
    pub directional: f32,
    /// Apply the per-model BAKED volumes (Stage 2) rather than the per-frame maps. The volumes
    /// themselves are produced at load time behind WGR_SKY_BAKE_VOLUMES; this is the runtime
    /// switch that decides whether shading reads them, so the two approaches can be A/B'd live.
    pub baked: bool,
    /// Map-coverage probe REQUEST COUNTER (see WgrSkyVis::probe). Not a look setting: the
    /// renderer compares it against the last value it acted on and logs the diagnostic when it
    /// changes, so the owner can measure the map where they are standing rather than only at
    /// the ~2 s startup one-shot, which measures the loading screen.
    pub probe: u32,
    /// 1 = draw the reach factor as greyscale instead of lighting with it. Shipped WITH the
    /// effect, not after it: judging this through sun + SH ambient + fog + tonemap is much
    /// harder than looking at the buffer.
    pub debug: bool,
}

impl Default for SkyVisSettings {
    fn default() -> Self {
        SkyVisSettings {
            // Both stages default ON (owner call, 2026-08-05); C++ pushes every frame and wins,
            // so this only decides frame 0.
            enabled: true,
            // 2048 over a 64 m half-box = 6 cm/texel: enough to resolve a window reveal, which
            // is the whole reason the tilted directions exist (see the module header).
            resolution: 2048,
            extent: 64.0,
            height: 300.0,
            strength: 1.0,
            // Interiors carry almost no local lights, and with the sun shadowed the sky ambient
            // is the ONLY light in the room. 0.32 chosen by the tester in a real building.
            floor: 0.32,
            kernel: 1.0,
            bias: 0.25,
            // 0 by default: with only five directions the steered normal jumps between them
            // across a surface, and that quantisation IS the shadow-patch artifact. See the C++
            // Engine::InteriorSkySettings comment for the full finding.
            directional: 0.0,
            baked: true,
            probe: 0,
            debug: false,
        }
    }
}

impl SkyVisSettings {
    /// Apply the C++-pushed knobs. Mirrors the GTAO settings path: the layer furthest from the
    /// renderer wins, so a renderer-side default is only ever the pre-push value.
    pub fn apply(&mut self, p: &WgrSkyVis) {
        self.enabled = p.enabled != 0;
        self.resolution = p.resolution.clamp(256, 4096);
        self.extent = p.extent.clamp(16.0, 2048.0);
        self.height = p.height.clamp(16.0, 4096.0);
        self.strength = p.strength.clamp(0.0, 1.0);
        self.floor = p.floor.clamp(0.0, 1.0);
        self.kernel = p.kernel.clamp(0.0, 32.0);
        self.bias = p.bias.clamp(0.0, 16.0);
        self.directional = p.directional.clamp(0.0, 1.0);
        self.baked = p.baked != 0;
        self.probe = p.probe;
        self.debug = p.debug != 0;
    }

    /// World metres per depth-map texel.
    pub fn texel_size(&self) -> f32 {
        2.0 * self.extent / self.resolution.max(1) as f32
    }
}

/// The ortho view-projection for this frame, plus the shader-side sampling constants derived
/// from it. Absolute world space in, clip space out (w = 1; the projection is orthographic).
#[derive(Clone, Copy, Debug)]
pub struct SkyVisView {
    pub view_proj: glam::Mat4,
    /// Softening kernel radius expressed in UV units (the shader offsets taps in UV).
    pub kernel_uv: f32,
    /// Depth bias in NDC depth units (the box spans 2*height metres over 0..1).
    pub bias_ndc: f32,
}

/// Build the snapped ortho view looking along `-dir` (i.e. FROM the sky toward the surface) for
/// one sampled direction.
///
/// SNAPPING is the whole point of doing this in one place, and it generalises to a tilted
/// direction by snapping in the VIEW's own axes rather than in world X/Z: the eye's position
/// along the view's right and up axes is quantised to the map's texel size, so as the camera
/// walks, the world keeps landing on the same texel centres and the map's contents are stable.
/// An unsnapped ortho view resamples the world every frame and the result crawls — there is no
/// TAA here to hide it, which is exactly why the GTAO mip march ended up default-off.
pub fn build_view_for(cam_pos: glam::Vec3, dir: glam::Vec3, s: &SkyVisSettings) -> SkyVisView {
    let texel = s.texel_size().max(1e-4);
    let snap = |v: f32, q: f32| (v / q).floor() * q;
    // `back` is the view's +Z (the camera looks down its own -Z, right-handed), so back = dir:
    // points BELOW the sky along -dir get negative view z, which is what the projection expects.
    let back = dir.normalize();
    // Any stable perpendicular pair. World up degenerates for a horizontal direction, so fall
    // back to world Z there; these directions are fixed constants, so the choice never flickers.
    let seed = if back.y.abs() > 0.99 {
        glam::Vec3::Z
    } else {
        glam::Vec3::Y
    };
    // (right, up, back) right-handed. For the zenith this reproduces the original basis exactly:
    // right = world +X, up = world -Z — which is what every snapping test below was written
    // against, and getting the sign backwards here silently mirrors the map.
    let right = back.cross(seed).normalize();
    let up = back.cross(right);
    // Eye sits `height` up the direction from the camera; its offsets along the view axes are
    // quantised (right/up to a texel, depth to a metre — a depth shift moves the stored and the
    // reference depth together, so it only has to beat depth quantisation).
    let tx = snap(cam_pos.dot(right), texel);
    let ty = snap(cam_pos.dot(up), texel);
    let tz = snap(cam_pos.dot(back), 1.0) + s.height;
    let view = glam::Mat4::from_cols(
        glam::Vec4::new(right.x, up.x, back.x, 0.0),
        glam::Vec4::new(right.y, up.y, back.y, 0.0),
        glam::Vec4::new(right.z, up.z, back.z, 0.0),
        glam::Vec4::new(-tx, -ty, -tz, 1.0),
    );
    // Depth 0 at the sky end of the box, 1 at the far end: a receiver under a roof has a LARGER
    // depth than the roof, which is what the LessEqual comparison sampler tests.
    // "directx" is glam's name for the NDC convention wgpu uses: Z in [0, 1], Y-up.
    let proj = glam::camera::rh::proj::directx::orthographic(
        -s.extent,
        s.extent,
        -s.extent,
        s.extent,
        0.0,
        2.0 * s.height,
    );
    SkyVisView {
        view_proj: proj * view,
        // UV spans the full box: 2*extent metres across 1.0 of UV.
        kernel_uv: s.kernel / (2.0 * s.extent).max(1e-4),
        bias_ndc: s.bias / (2.0 * s.height).max(1e-4),
    }
}

/// Every sampled direction's view for this frame, index-aligned with [`directions`].
pub fn build_views(cam_pos: glam::Vec3, s: &SkyVisSettings) -> [SkyVisView; DIRECTION_COUNT] {
    let dirs = directions();
    std::array::from_fn(|i| build_view_for(cam_pos, dirs[i].truncate(), s))
}

/// The zenith view alone — the one every existing test and the debug path reason about.
pub fn build_view(cam_pos: glam::Vec3, s: &SkyVisSettings) -> SkyVisView {
    build_view_for(cam_pos, glam::Vec3::Y, s)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn project(v: &SkyVisView, p: glam::Vec3) -> glam::Vec3 {
        let clip = v.view_proj * p.extend(1.0);
        // Orthographic: w == 1, so clip IS ndc.
        glam::Vec3::new(
            clip.x * 0.5 + 0.5,
            -clip.y * 0.5 + 0.5, // wgpu UV has +V down
            clip.z,
        )
    }

    // The receiver-below-occluder relationship the whole feature rests on: a point lower in the
    // world must land at a GREATER depth, so the LessEqual comparison reads it as occluded.
    #[test]
    fn lower_points_have_greater_depth() {
        let s = SkyVisSettings::default();
        let v = build_view(glam::Vec3::ZERO, &s);
        let roof = project(&v, glam::Vec3::new(0.0, 6.0, 0.0));
        let floor = project(&v, glam::Vec3::new(0.0, 0.0, 0.0));
        assert!(
            floor.z > roof.z,
            "floor depth {} must exceed roof depth {}",
            floor.z,
            roof.z
        );
        // ...and both must be inside the box's depth range, or the comparison is meaningless.
        assert!((0.0..=1.0).contains(&roof.z) && (0.0..=1.0).contains(&floor.z));
    }

    // A point directly under the camera lands at the middle of the map, and the box covers
    // exactly ±extent — the mapping the shader's UV derivation assumes.
    #[test]
    fn box_covers_extent_around_camera() {
        let s = SkyVisSettings::default();
        let cam = glam::Vec3::new(1000.0, 20.0, -2000.0);
        let v = build_view(cam, &s);
        let under = project(&v, glam::Vec3::new(cam.x, 0.0, cam.z));
        // Snapping moves the centre by at most one texel, which is well under half a percent
        // of the box.
        assert!((under.x - 0.5).abs() < 0.01, "u = {}", under.x);
        assert!((under.y - 0.5).abs() < 0.01, "v = {}", under.y);
        let edge = project(&v, glam::Vec3::new(cam.x + s.extent * 0.98, 0.0, cam.z));
        assert!(edge.x > 0.9 && edge.x < 1.0, "edge u = {}", edge.x);
    }

    // The anti-crawl guarantee, stated as a test: sub-texel camera motion must not move the
    // world's image in the map AT ALL. This is the property the plan calls non-negotiable, and
    // it is the one that silently regresses if someone "simplifies" the snap away.
    #[test]
    fn subtexel_camera_motion_does_not_move_the_map() {
        let s = SkyVisSettings::default();
        let texel = s.texel_size();
        // BOTH positions inside the SAME texel cell — the claim is that motion which does not
        // cross a cell boundary changes nothing. (Stepping to -0.3 * texel would land in the
        // cell below, which legitimately moves the map by one texel; see the next test.)
        let a = build_view(glam::Vec3::new(texel * 0.1, 10.0, texel * 0.2), &s);
        let b = build_view(glam::Vec3::new(texel * 0.9, 10.4, texel * 0.8), &s);
        let p = glam::Vec3::new(7.0, 3.0, -11.0);
        let (pa, pb) = (project(&a, p), project(&b, p));
        assert!((pa.x - pb.x).abs() < 1e-6, "u moved: {} -> {}", pa.x, pb.x);
        assert!((pa.y - pb.y).abs() < 1e-6, "v moved: {} -> {}", pa.y, pb.y);
    }

    // Crossing a texel boundary moves the image by EXACTLY one texel, never a fraction — the
    // other half of the snap contract (a snap that quantised to the wrong grid would still pass
    // the test above while smearing here).
    #[test]
    fn texel_crossing_moves_the_map_by_one_texel() {
        let s = SkyVisSettings::default();
        let texel = s.texel_size();
        let a = build_view(glam::Vec3::ZERO, &s);
        let b = build_view(glam::Vec3::new(texel, 0.0, 0.0), &s);
        let p = glam::Vec3::new(7.0, 3.0, -11.0);
        let du = project(&a, p).x - project(&b, p).x;
        let expect = texel / (2.0 * s.extent);
        assert!((du - expect).abs() < 1e-6, "du = {du}, expected {expect}");
    }

    // The kernel is authored in metres but consumed in UV; a wrong conversion here is invisible
    // (it just looks like the wrong softness) which is exactly why it is pinned.
    #[test]
    fn kernel_converts_metres_to_uv() {
        let mut s = SkyVisSettings::default();
        s.kernel = 2.0;
        s.extent = 100.0;
        let v = build_view(glam::Vec3::ZERO, &s);
        assert!((v.kernel_uv - 0.01).abs() < 1e-6, "{}", v.kernel_uv);
    }

    // Bias is authored in metres too, against the box's FULL vertical span.
    #[test]
    fn bias_converts_metres_to_ndc_depth() {
        let mut s = SkyVisSettings::default();
        s.bias = 1.0;
        s.height = 250.0;
        let v = build_view(glam::Vec3::ZERO, &s);
        assert!((v.bias_ndc - 1.0 / 500.0).abs() < 1e-9, "{}", v.bias_ndc);
    }
}
