// Tidewater Native — spray sprites (W4a): Spray.js `_buildMesh`'s material (vertex + output) and
// Breakers.js's `breakersSprayShadow` (dgreenheck/tidewater @ 4811ba48, MIT, © DRG Software
// Solutions LLC). Appended to the water module (after tw_lip.wgsl); drawn after the lips, one
// camera-facing quad per ring slot (instanced, dead ones collapse off-screen), premultiplied
// alpha, depth test read-only, both sides.
//
// OP adaptations (see spray.rs): positions are carried camera-relative; the light is OP's in
// Tidewater's units as for the lip (sun = PI x sun_diffuse x cloud shadow, sky irradiance =
// sun_ambient); soft intersections read OP's opaque depth copy (binding 9); OP's aerial
// perspective is applied; the ring holds only the GPU emitters' particles.

@group(1) @binding(20) var<uniform> sprayR: SprayParams;
// the ring: pos | vel | info, RING vec4 each (see tw_spray_emit.wgsl)
@group(1) @binding(21) var<storage, read> sprayBufR: array<vec4<f32>>;
// OP: Tidewater's two sprite textures in one (r: puff noise, g: drops), mipmapped, repeat
@group(1) @binding(22) var sprayTex: texture_2d<f32>;

// Sun visibility (0..1) for a spray particle made by one of the crests: in front of the wave with
// the sun behind it (a beach view into the sun) the particles below the crest line are in the
// shadow of the wave (and of the overhanging lip while it plunges). seedTag: the particle's tag.
fn sprayWaveShadow(p: vec3<f32>, seedTag: f32, L: vec3<f32>) -> f32 {
    var out = 1.0;
    let idx = floor(seedTag) - 1.0;
    if (idx >= 0.0 && idx < tw.brk.x * 2.0) {
        let k = u32(idx) * 3u;
        let c0 = breakersCrest[k];
        let c1 = breakersCrest[k + 1u];
        let c2 = breakersCrest[k + 2u];
        let d2 = c2.xy;
        let Ld = dot(L.xz, d2); // < 0: the sun is on the sea side of the wave
        if (c2.w > 0.5 && Ld < -0.02) {
            let root = c0.xyz;
            let b = c0.w;
            let H = c1.w;
            let q = clamp(b / 0.9, 0.0, 1.0) * (1.0 - smoothstep(1.0, 1.3, b));
            // vertical plane through the crest (moved forward under the overhanging lip)
            let plane = root.xz + d2 * (H * 0.8 * q * 0.6);
            let s = dot(p.xz - plane, d2); // > 0: in front of it
            let tau = s / -Ld;
            let yRay = p.y + L.y * tau; // height of the ray toward the sun where it crosses the plane
            let top = root.y + 0.05;
            let shade = smoothstep(top, top - 0.4, yRay) * smoothstep(-0.1, 0.1, s) * smoothstep(14.0, 6.0, s);
            out = 1.0 - shade * 0.55;
        }
    }
    return out;
}

// (W10b, OP) the forward-scattering lobes are capped: looking toward a low sun through the
// breakers' spray, Tidewater's Henyey-Greenstein peaks (up to ~6.5 x the sun for g = 0.85) summed
// over hundreds of overlapping sprites into a blinding white band with a bloom star (the owner's
// glowing surf). 0.9 keeps the backlit glow of spray, about 1/7 of the drops' peak.
// (W10g, OP) 0.35: at 20 m/s the breakers' spray, lit through the lobe, still stacked into a
// clipped white wall a metre or two tall along every wave (captures with and without spray).
const SPRAY_LOBE_MAX: f32 = 0.35;

struct SprayOut {
    @builtin(position) clip: vec4<f32>,
    @location(0) rel: vec3<f32>,
    @location(1) vUV: vec2<f32>,
    @location(2) vCol: vec4<f32>,  // radiance, opacity
    @location(3) vMisc: vec4<f32>, // kind + 0.45 x life fraction, water height, size, seed
    @location(4) vFwd: vec4<f32>,  // forward-scattered sun (thin parts glow with it), w: half-size in pixels
    @location(5) vSun: vec3<f32>,  // the sun-lit part of vCol (x the cloud shadow in the fragment)
};

@vertex
fn vs_spray(@builtin(vertex_index) vi: u32, @builtin(instance_index) ii: u32) -> SprayOut {
    var corners = array<vec2<f32>, 6>(
        vec2<f32>(-1.0, -1.0), vec2<f32>(1.0, -1.0), vec2<f32>(1.0, 1.0),
        vec2<f32>(-1.0, -1.0), vec2<f32>(1.0, 1.0), vec2<f32>(-1.0, 1.0));
    let corner = corners[vi];
    let n = sprayR.seed.y;
    let posA = sprayBufR[ii];
    let velA = sprayBufR[n + ii];
    let info = sprayBufR[2u * n + ii];
    let p = posA.xyz;
    let age = posA.w;
    let vel = velA.xyz;
    let kind = info.x;
    let life = info.y;
    let isDrop = kind < 0.5;
    let isLig = kind > 1.5 && kind < 2.5;
    let isMist = kind > 0.5 && kind < 1.5;
    let water = isDrop || isLig; // clear water: drops and ligaments
    let alive = life > 0.0 && age < life;

    let toCam = frame.cam_pos.xyz - p;
    let dist = max(length(toCam), 0.05);
    let Vd = toCam / dist;

    // pixel footprint at this distance: drops are drawn at least ~1.3 px wide
    let p11 = frame.proj[1][1];
    let resY = f32(textureDimensions(waterSceneDepth).y);
    let pixel = dist * 2.0 / (abs(p11) * resY);
    let r0 = velA.w; // radius (m)
    // dense spray is thrown out of the splash as a compact mass and spreads (grows from ~half size)
    let tAge = age / max(life, 1e-3);
    let r = select(r0, r0 * mix(0.45, 1.0, smoothstep(0.0, 0.25, tAge)), kind > 2.5 && kind < 3.5);
    // (OP diagnostics: sprite intensity >= 20, e.g. WGR_TW_SPRAY=1,20, draws every live particle as
    // an opaque magenta dot of at least 4 px, to show where the spray is)
    let dbgDots = sprayR.b.y >= 20.0;
    let size = max(r, pixel * select(1.3, 4.0, dbgDots || sprayR.seed.w > 0u));

    // motion blur along the velocity projected on the view plane; ligaments are elongated anyway
    let vPerp = vel - Vd * dot(vel, Vd);
    let speed = length(vPerp);
    let stretchLen = speed * sprayByKind(kind, 1.0 / 40.0, 0.0, 1.0 / 40.0, 1.0 / 30.0, 1.0 / 60.0);
    let elong = select(0.0, r * 2.0, isLig);
    let up = select(vec3<f32>(0.0, 1.0, 0.0), vPerp / max(speed, 1e-3), speed > 1e-3);
    // clouds: random rotation that slowly turns
    let rot = fract(info.w) * 6.283 + age * (fract(info.w) - 0.5);
    let camRight = normalize(cross(vec3<f32>(0.0, 1.0, 0.0), Vd) + vec3<f32>(1e-5, 0.0, 0.0));
    let camUp = cross(Vd, camRight);
    let mRight = camRight * cos(rot) + camUp * sin(rot);
    let mUp = camUp * cos(rot) - camRight * sin(rot);
    // torn sheets (dense spray) are drawn along their motion too, tilted a little at random and
    // stretched by their speed: fibrous, streaked silhouettes instead of round puffs
    let isSheet = kind > 2.5;
    let isClear = kind > 3.5;
    let side = normalize(cross(Vd, up) + vec3<f32>(0.0, 0.0, 1e-6));
    let tilt = (fract(info.w * 7.31) - 0.5) * 0.7;
    let sUp = up * cos(tilt) + side * sin(tilt);
    let sSide = side * cos(tilt) - up * sin(tilt);
    // mist streams with the air: drawn along its motion, stretched by its speed (wisps, not discs)
    let alongMotion = isSheet || (isMist && speed > 0.3);
    let axisY = select(select(mUp, sUp, alongMotion), up, water);
    let axisX = select(select(mRight, sSide, alongMotion), side, water);
    let sheetLen = select(0.0, size * clamp(speed * 0.08, 0.0, 0.8), isSheet);
    let mistLen = select(0.0, size * clamp((speed - 0.3) * 0.35, 0.0, 1.4), isMist);
    let halfY = size + stretchLen * 0.5 + elong + sheetLen + mistLen;
    let halfX = select(size, size * 1.05, isSheet);
    let world = p + axisX * (corner.x * halfX) + axisY * (corner.y * halfY);

    // coverage that conserves the water's cross-section: the drop's projected area (and the time it
    // spends on each pixel of its streak) spread over the drawn footprint
    let kShape = select(3.5, 4.5, isLig);
    let cover = select(sat(r / size), min(r * (r + elong) * kShape / (halfX * halfY), 1.0), water);

    // ---- lighting (per particle), OP light in Tidewater's units (see tw_lip.wgsl)
    let L = normalize(-frame.sun_dir_world.xyz);
    var sunDiffuse = frame.sun_diffuse.rgb;
    var skyIrr = frame.sun_ambient.rgb;
    if (linear > 0.5 && frame.sun_diffuse.w <= 0.5) {
        sunDiffuse = srgb_to_linear(sunDiffuse);
        skyIrr = srgb_to_linear(skyIrr);
    }
    let cosT = dot(-Vd, L); // 1 = looking toward the sun through the particle
    let cosA = dot(Vd, L);
    // (OP: the cloud shadow is a fragment-stage texture in the camera group; it is applied in
    // fs_spray to the sun-lit terms, which are carried apart from the sky-lit ones)
    let sunVis = sprayWaveShadow(p, info.w, L);
    let sun = sunDiffuse * PI * sunVis;
    // clear water (drop, ligament): the bright sky it refracts and reflects, a strong forward lobe
    // when backlit, a small glint from any side
    let sWater = sun * (min(sprayPhaseHG(cosT, 0.85), SPRAY_LOBE_MAX) * 1.2 + 0.12);
    // dense spray: multiply scattered, white from any side (a diffuse sphere), plus a forward lobe
    let lambert = (sqrt(max(1.0 - cosA * cosA, 0.0)) + (PI - acos(clamp(cosA, -1.0, 1.0))) * cosA) / PI;
    let sSpray = sun * ((lambert * 0.65 + 0.35) / PI);
    let fSpray = sun * (min(sprayPhaseHG(cosT, 0.6), SPRAY_LOBE_MAX) * 0.9);
    // mist: a thin veil of fine drops, strongly forward scattering, tinted by the sky
    let sMist = sun * (0.25 / PI);
    let fMist = sun * min(sprayPhaseHG(cosT, 0.75), SPRAY_LOBE_MAX);
    // clear sheet: thin water, the sky it shows and a little sun off its surface
    let sClear = sun * 0.05;
    let fClear = sun * (min(sprayPhaseHG(cosT, 0.8), SPRAY_LOBE_MAX) * 1.1);
    let skyK = select(select(select(1.15, 0.95, isClear), 0.9, isMist), 0.9, water);
    let col = skyIrr * skyK;
    let sunCol = select(select(select(sSpray, sClear, isClear), sMist, isMist), sWater, water);

    // opacity over the particle's life
    let t = age / max(life, 1e-3);
    let fadeIn = smoothstep(0.0, sprayByKind(kind, 0.02, 0.2, 0.02, 0.12, 0.02), t);
    let fadeOut = 1.0 - smoothstep(sprayByKind(kind, 0.8, 0.45, 0.8, 0.7, 0.5), 1.0, t);
    // (W10e, OP) dense spray 0.8 -> 0.45, sheets 0.66 -> 0.5, mist 0.22 -> 0.12 (drops and
    // ligaments as Tidewater's): at the rates a 12-20 m/s surf reaches here (~16 000 particles/s,
    // ~20 000 live) the breakers' spray stood as an opaque white wall 1-2 m tall along every wave,
    // hiding the curl and the whitewater on the surface behind it
    let baseA = sprayByKind(kind, 0.55, 0.06, 0.45, 0.5, 0.12);
    // far: fade out; very near the eye: sheets and mist would fill the screen
    let maxD = sprayR.b.z;
    let distFade = smoothstep(maxD, maxD * 0.55, dist) * select(smoothstep(0.6, 3.0, dist), 1.0, water);
    let a = select(baseA * fadeIn * fadeOut * cover * distFade * sprayR.b.y * sprayR.cam.w, 1.0, dbgDots && sprayR.cam.w > 0.5);

    var o: SprayOut;
    o.vUV = corner;
    o.vCol = vec4<f32>(col, a);
    o.vSun = sunCol;
    o.vFwd = vec4<f32>(select(select(select(fSpray, fClear, isClear), fMist, isMist), vec3<f32>(0.0), water), halfX / pixel);
    // x: kind + 0.45 x life fraction (the kind tests below have 0.5 of margin)
    o.vMisc = vec4<f32>(kind + sat(t) * 0.45, info.z, size, fract(info.w));
    // dead particles collapse off-screen (OP diagnostics: every live one, WGR_TW_SPRAY_DEBUG)
    let show = alive && (a > 1e-4 || sprayR.seed.w > 0u);
    let rel = select(vec3<f32>(0.0, -1e5, 0.0), world - frame.cam_pos.xyz, show);
    o.rel = rel;
    o.clip = select(vec4<f32>(2.0, 2.0, 2.0, 1.0), reverse_z(frame.proj * frame.view * vec4<f32>(rel, 1.0)), show);
    return o;
}

// the sprite's premultiplied colour and coverage (fs_spray; fs_spray_mask writes the coverage
// into the upscalers' reactive mask, W4b)
fn sprayShade(in: SprayOut) -> vec4<f32> {
    let uv = in.vUV;
    if (sprayR.b.y >= 20.0) {
        if (dot(uv, uv) > 1.0) { discard; }
        return vec4<f32>(1.0, 0.0, 1.0, 1.0);
    }
    let vMisc = in.vMisc;
    let kind = vMisc.x;
    let isDrop = kind < 0.5;
    let isLig = kind > 1.5 && kind < 2.5;
    let isSheet = kind > 2.5;
    let isClear = kind > 3.5;
    let t = sat(fract(kind) / 0.45); // life fraction
    let r2 = dot(uv, uv);
    let sd = vec2<f32>(vMisc.w, vMisc.w * 1.7);
    // drop: gaussian streak; ligament: a slightly sharper, beaded blob
    let drop = max(exp(r2 * -3.5) - 0.03, 0.0);
    let lig = max(exp(r2 * -4.5) * (sin(uv.y * 5.0 + vMisc.w * 40.0) * 0.2 + 0.9) - 0.03, 0.0);
    // torn sheet: noise streaked along the motion (uv.y), eroded from its edges inward and more and
    // more as it ages, so it tears into strands and fragments instead of shrinking
    let env = sat(1.0 - r2);
    let fib = textureSample(sprayTex, smpLinearRepeat, vec2<f32>(uv.x * 0.5, uv.y * 0.26) + sd).x;
    let fine = textureSample(sprayTex, smpLinearRepeat, vec2<f32>(uv.x * 1.2, uv.y * 0.6) + sd * 2.3).x;
    let field = fib * 0.6 + fine * 0.4 + (env - 0.55) * 0.75 - smoothstep(0.8, 1.0, r2);
    // a torn sheet only a few pixels across can't show its tears: drawn thinner and more torn
    let farK = smoothstep(14.0, 3.0, in.vFwd.w) * select(0.0, 1.0, isSheet && !isClear);
    let erode = mix(0.26, 0.7, t) + farK * 0.16;
    let dens = sat((field - erode) * 3.0);
    let torn = smoothstep(erode - 0.04, erode + 0.2, field) * (dens * 0.45 + 0.55);
    // ... which breaks up into a cluster of drops that thins out as it ages
    let dots = textureSample(sprayTex, smpLinearRepeat, uv * vec2<f32>(0.5, 0.32) + sd * 3.7).y;
    let swarm = dots * smoothstep(0.25, 0.55, fib + env * 0.45 - t * 0.2) * (1.0 - t * 0.5);
    let sheet = max(torn * (1.0 - smoothstep(0.15, 0.6, t)), swarm);
    // mist: a soft veil of low-frequency noise that drifts and thins, never a disc
    let m1 = textureSample(sprayTex, smpLinearRepeat, uv * 0.2 + sd).x;
    let m2 = textureSample(sprayTex, smpLinearRepeat, uv * 0.55 + sd * 3.1).x;
    let veil = smoothstep(0.28, 0.8, m1 * 0.7 + m2 * 0.3 + env * 0.3 - 0.12) * sqrt(env) * (1.0 - t * 0.4);
    let shape = select(select(select(veil, sheet, isSheet), lig, isLig), drop, isDrop);
    // self-shadowing inside thick sheets; the forward-scattered sun lights up the thin parts
    let shade = select(1.0, 1.0 - dens * 0.15, isSheet && !isClear);
    let glow = select(select(1.0, 1.0 - dens * 0.6, isSheet), 1.0 - dens * 0.3, isClear);

    // soft intersections: the opaque scene and the water surface under the particle
    let dsz = vec2<f32>(textureDimensions(waterSceneDepth));
    let suv = in.clip.xy / dsz;
    let sceneZ = _waterSceneZAt(suv);
    // (W6i.7: in the material's view space, -z forward; see twView in tw_water.wgsl)
    let posViewZ = twView(vec4<f32>(in.rel, 1.0)).z;
    let soft = vMisc.z * 1.5 + 0.03;
    let fadeScene = sat((posViewZ - sceneZ) / soft);
    let pos = in.rel + frame.cam_pos.xyz;
    let fadeWater = sat((pos.y - vMisc.y) / (soft * 0.6) + 0.15);
    // (W10b, OP) coverage <= 1: the sprite intensity lane multiplies it, and premultiplied
    // coverage above 1 subtracts the scene behind the spray
    let aOut = min(in.vCol.w * shape * fadeScene * fadeWater * (1.0 - farK * 0.4), 1.0);
    // (OP diagnostics, WGR_TW_SPRAY_DEBUG=<n>: each live sprite as a disc whose brightness is one of
    // its opacity factors: 1 vertex opacity (x10), 2 shape, 3 soft scene fade, 4 water fade; 5-7 below)
    let dbgMode = sprayR.seed.w;
    if (dbgMode > 0u) {
        if (dot(uv, uv) > 1.0) { discard; }
        var f = select(select(select(fadeWater, fadeScene, dbgMode == 3u), shape, dbgMode == 2u), sat(in.vCol.w * 10.0), dbgMode == 1u);
        // 5: particle view depth minus the opaque scene's (0.5 = equal, 1 = 10 m in front),
        // 6: the opaque scene's view distance / 100 m, 7: the particle's view distance / 100 m
        if (dbgMode == 5u) { f = sat(0.5 + (posViewZ - sceneZ) / 20.0); }
        if (dbgMode == 6u) { f = sat(-sceneZ / 100.0); }
        if (dbgMode == 7u) { f = sat(-posViewZ / 100.0); }
        // 8: the opaque depth sample x20 (reverse z), 9: the sprite's own depth x20, 10: the scene
        // view distance / 20 m, 11: the sprite's view distance / 20 m
        if (dbgMode == 8u) { f = sat(_waterSceneDepthAt(suv) * 20.0); }
        if (dbgMode == 9u) { f = sat(in.clip.z * 20.0); }
        if (dbgMode == 10u) { f = sat(-sceneZ / 20.0); }
        if (dbgMode == 11u) { f = sat(-posViewZ / 20.0); }
        return vec4<f32>(f, f * 0.2, 1.0 - f, 1.0);
    }
    if (aOut < 0.002) { discard; }
    let cloud = cloud_sun_shadow(pos.xz);
    let lit = (in.vCol.rgb + in.vSun * cloud) * shade + in.vFwd.xyz * (glow * cloud);
    let col = apply_fog(min(lit, vec3<f32>(16000.0)), in.rel);
    return vec4<f32>(col * aOut, aOut);
}

@fragment
fn fs_spray(in: SprayOut) -> @location(0) vec4<f32> {
    return sprayShade(in);
}

// (W4b, OP) the spray's coverage into the history-control ("reactive") mask: Tidewater marks
// spray and lip pixels reactive so the temporal upscalers do not smear them (they have no
// motion vectors of their own). R8, max-blended over the engine's sky / water mask.
@fragment
fn fs_spray_mask(in: SprayOut) -> @location(0) vec4<f32> {
    let c = sprayShade(in);
    return vec4<f32>(sat(c.a * 2.0), 0.0, 0.0, 1.0);
}
