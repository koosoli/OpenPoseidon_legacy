// Tidewater Native — the thrown lip of plunging breakers (W3d): Breakers.js `_buildMesh`'s material
// (vertex + output) (dgreenheck/tidewater @ 4811ba48, MIT, © DRG Software Solutions LLC). Appended
// to the water module (the lace is the shore simulation's; the crest records come from
// tw_breakers_kernel.wgsl). Premultiplied alpha over the water, depth test read-only, both sides.
//
// OP adaptations (see breakers.rs): positions are carried camera-relative; the sky reflection is
// OP's sky/cloud env bake, the sun is OP's (sunColor = PI x sun_diffuse, times the cloud shadow as
// in Tidewater); OP's aerial perspective is applied; and since OP's water pass does not write
// depth, the part of the curtain a sight line from the sea side reaches below the crest line
// is hidden analytically (Tidewater: the wave's own depth hides it).

const BRK_NV: f32 = 20.0;
const BRK_SPACING: f32 = 0.6;

struct LipOut {
    @builtin(position) clip: vec4<f32>,
    @location(0) rel: vec3<f32>,
    @location(1) vLipN: vec3<f32>,
    @location(2) vLip: vec4<f32>,    // v, b, along, curtain length
    @location(3) vLipFade: f32,
    @location(4) vRoot: vec4<f32>,   // crest root (world), valid
    @location(5) vDir: vec2<f32>,    // shoreward direction of the wave
};

@vertex
fn vs_lip(@location(0) sheetId: vec4<f32>) -> LipOut {
    let id = sheetId;
    let seg = u32(id.x);
    let slot = u32(id.y);
    let side = u32(id.z);
    let k = id.w;
    let st = seg + side;
    let ot = seg + (1u - side);
    let e = st * 6u + slot * 3u;
    let eo = ot * 6u + slot * 3u;
    let c0 = breakersCrest[e];
    let c1 = breakersCrest[e + 1u];
    let c2 = breakersCrest[e + 2u];
    let mOther = breakersCrest[eo + 2u].w;
    let bOther = breakersCrest[eo].w;
    // (both ends of a segment must pass the b test, else a vertex pair collapses to one side only)
    let valid = c2.w > 0.5 && abs(mOther - c2.w) < 0.5 && c0.w < 1.25 && bOther < 1.25;

    let root = c0.xyz;
    let b = c0.w;
    let back = c1.xyz;
    let H = c1.w;
    let d3 = vec3<f32>(c2.x, 0.0, c2.y);
    let trough = c2.z;
    let q = clamp(b / 0.9, 0.0, 1.35);
    let Xi = max(H * 0.8, 0.05);
    let Yi = max(root.y - trough, 0.05);
    // profile parameter: k = 0, 1 on the back of the crest (-1, -0.45), then 0..1 along the curtain
    let pv = select(select((k - 2.0) / (BRK_NV - 3.0), -0.45, k < 1.5), -1.0, k < 0.5);
    let xl = Xi * q * max(pv, 0.0);
    let fl = xl / Xi;
    let yl = -Yi * (fl * fl); // ballistic: the jet leaves the crest horizontally
    let onLip = root + d3 * xl + vec3<f32>(0.0, yl, 0.0);
    let onCap = mix(root, back, max(-pv, 0.0)) + vec3<f32>(0.0, 0.012, 0.0);
    let P = select(onLip, onCap, pv < 0.0);
    let slope = Yi * 2.0 * xl / (Xi * Xi);
    var o: LipOut;
    o.vLipN = normalize(vec3<f32>(0.0, 1.0, 0.0) + d3 * slope);
    o.vLip = vec4<f32>(pv, b, f32(st) * BRK_SPACING, Xi * q + Yi * q * q);
    let capA = smoothstep(-1.0, -0.1, pv);
    o.vLipFade = capA * smoothstep(0.02, 0.12, q) * (1.0 - smoothstep(1.0, 1.2, b));
    let world = select(vec3<f32>(0.0, -1e5, 0.0), P, valid);
    let rel = world - frame.cam_pos.xyz;
    o.clip = reverse_z(frame.proj * frame.view * vec4<f32>(rel, 1.0));
    o.rel = rel;
    o.vRoot = vec4<f32>(root, select(0.0, 1.0, valid));
    o.vDir = c2.xy;
    return o;
}

// the lip's premultiplied colour and coverage (fs_lip; fs_lip_mask: the reactive mask, W4b)
fn lipShade(in: LipOut, front: bool) -> vec4<f32> {
    let v = in.vLip.x;
    let b = in.vLip.y;
    let a = in.vLip.z;
    let len = in.vLip.w;
    let pos = in.rel + frame.cam_pos.xyz;
    let V = normalize(-in.rel);
    let Nw = normalize(in.vLipN);
    // (W10a, OP) front_facing is the other side in OP's left-handed world (see the water
    // material's view side, W9d): the curtain was shaded with its normal turned away from the eye
    let N0 = select(Nw, -Nw, front);
    let t = tw.sea.y;
    // the water of the jet is stretched along the flow: streaks and ripples running down the curtain
    let flowS = v * len - t * 1.1;
    // along-crest coordinate warped by low-frequency noise (the streak spacing drifts, no comb)
    let aw = a + sin(a * 0.23 + 1.7) * 1.6 + sin(a * 0.61 + 4.2) * 0.4 + sin(a * 1.37 + 0.4) * 0.12;
    let sv = textureSample(shoreSimLace, smpAniso4Repeat, vec2<f32>(aw / 0.83, flowS / 3.0));
    let sbv = textureSample(shoreSimLace, smpAniso4Repeat, vec2<f32>(aw / 2.9, flowS / 9.0) + vec2<f32>(0.37, 0.61));
    let sb = sbv.z;
    let hS = sv.x * 0.02 + sv.z * 0.012;
    let dpx = dpdx(pos);
    let dpy = dpdy(pos);
    let r1 = cross(dpy, N0);
    let r2 = cross(N0, dpx);
    let det = dot(dpx, r1);
    let grad = (r1 * dpdx(hS) + r2 * dpdy(hS)) * sign(det);
    let N = normalize(N0 * abs(det) - grad + N0 * 1e-9);
    let NdV = max(dot(N, V), 1e-3);
    let F = fresnelDielectric(NdV, 1.333);
    let L = normalize(-frame.sun_dir_world.xyz);
    // OP light in Tidewater's units (see the water material)
    var sunDiffuse = frame.sun_diffuse.rgb;
    var skyIrr = frame.sun_ambient.rgb;
    if (linear > 0.5 && frame.sun_diffuse.w <= 0.5) {
        sunDiffuse = srgb_to_linear(sunDiffuse);
        skyIrr = srgb_to_linear(skyIrr);
    }
    let sun = sunDiffuse * PI * cloud_sun_shadow(pos.xz);

    // reflection: sky + sun glint
    let Rr = reflect(-V, N);
    let R = normalize(vec3<f32>(Rr.x, max(Rr.y, 0.004), Rr.z));
    let refl = sky_env_sample(R);
    let Hh = normalize(L + V);
    let spec = sun * (pow(max(dot(N, Hh), 0.0), 180.0) * 12.0) * F;

    // light through the thin sheet: turquoise when backlit
    let tint = vec3<f32>(0.16, 0.62, 0.56);
    let back = pow(sat(dot(-V, L) * 0.5 + 0.5), 4.0);
    let thin = mix(1.5, 0.7, v) * (sv.z * 0.3 + 0.8) * (sb * 0.8 + 0.6);
    // (W10d, OP) the backlit terms at about half Tidewater's (0.9 -> 0.5 here, 1.2 -> 0.5 in the
    // foam below): against a low sun the lip and its foam clipped to flat white slabs
    let glow = (sun * tint * (back * 0.5 + 0.08) + skyIrr * tint * 1.4) * thin;
    let aW = F + min((1.0 - F) * mix(0.42, 0.16, v) * (sv.x * 0.5 + 0.75) * (sb * 0.7 + 0.65), 0.85);
    let cW = refl * F + spec + glow * (1.0 - F) * 0.42;

    // the jet stays clear, glassy water while it is in the air: only its leading edge tears into
    // aerated fingers
    let flowM = v * len - t * 1.1;
    let fuv = vec2<f32>(aw / 1.9 + 0.53, flowM / 1.8);
    let fl = textureSample(shoreSimLace, smpAniso4Repeat, fuv);
    let sect = fl.z;
    let vary = sect - 0.5 + sin(aw * 0.29 + b * 2.1) * 0.15;
    let tipN = sin(aw * 1.3 + t * 0.3) * 0.6 + sin(aw * 4.7 + 1.3) * 0.4;
    let edge = v + tipN * 0.03 + vary * 0.04;
    let thrown = smoothstep(0.35, 0.8, b);
    let rim = smoothstep(0.9, 0.96, edge) * (fl.z * 0.2 + 0.2) * thrown;
    let fing = (sv.w * 0.55 + sv.z * 0.2) * (smoothstep(0.25, 0.75, sbv.w * 0.6 + sb * 0.4) * 0.8 + 0.2) * 0.34;
    let wh = smoothstep((1.0 - v) * 0.18 + 0.9 - fing, (1.0 - v) * 0.18 + 1.0 - fing, b);
    let lc = textureSample(shoreSimLace, smpAniso4Repeat, vec2<f32>(aw * 0.83 + 1.9, v * len - t * 1.3) / (SHORE_SIM_LACE_TILE * 0.8));
    let blot = lc.z * 0.7 + lc.y * 0.3;
    let gone = smoothstep(1.0, 1.25, b);
    let clumpW = smoothstep(gone - 0.12, gone + 0.12, blot);
    let clumps = sat((blot - gone) * 2.5);
    let fil = (1.0 - smoothstep(0.02, 0.12, sv.x)) * smoothstep(0.15, 0.8, v) * (sv.w * 0.5 + 0.25) * (sb * 0.9 + 0.3) * thrown;
    let aer = mix(max(rim, fil), 0.95, wh);
    let foamLit = (sun * (max(dot(N, L), 0.0) * 0.5 + 0.5 + back * 0.5) / PI + skyIrr) * 0.9;

    // (OP) the wave's back hides the curtain from the sea side: where the sight line crosses the
    // vertical plane through the crest below the crest line
    var hidden = 0.0;
    {
        let root = in.vRoot.xyz;
        let d = vec2<f32>(in.vDir.x, in.vDir.y);
        let cam = frame.cam_pos.xyz;
        let camS = dot(cam.xz - root.xz, d);   // < 0: the camera is on the sea side of the crest
        let pS = dot(pos.xz - root.xz, d);     // > 0: this point is on the shore side (the curtain)
        if (camS < 0.0 && pS > 0.0) {
            let tau = -camS / max(pS - camS, 1e-4);
            let y = cam.y + (pos.y - cam.y) * tau;
            hidden = smoothstep(root.y + 0.05, root.y - 0.15, y);
        }
    }

    let tip = 1.0 - smoothstep(0.93, 1.0, edge);
    // (tw.brk.y: the lip opacity multiplier, Tidewater's `sheet`)
    let alpha = in.vLipFade * tip * tw.brk.y * mix(1.0, clumpW, wh) * (1.0 - hidden) * in.vRoot.w;
    let aF = aer * 0.92;
    var col = foamLit * mix(1.0, clumps * 0.4 + 0.75, wh) * aF + cW * (1.0 - aF);
    col = apply_fog(min(col, vec3<f32>(16000.0)), in.rel);
    // (W10b, OP) premultiplied coverage above 1 subtracts the water behind the lip (red and cyan
    // specks along the crests where the Water tab's lip opacity is above 1): coverage stays <= 1
    let aOut = min((aF + aW * (1.0 - aF)) * alpha, 1.0);
    return vec4<f32>(col * min(alpha, 1.0), aOut);
}

@fragment
fn fs_lip(in: LipOut, @builtin(front_facing) front: bool) -> @location(0) vec4<f32> {
    return lipShade(in, front);
}

// (W4b, OP) the lip's coverage into the history-control ("reactive") mask (see fs_spray_mask)
@fragment
fn fs_lip_mask(in: LipOut, @builtin(front_facing) front: bool) -> @location(0) vec4<f32> {
    let c = lipShade(in, front);
    // no depth attachment on the mask pass: behind the opaque scene (reversed Z) marks nothing
    let dsz = vec2<f32>(textureDimensions(waterSceneDepth));
    let hidden = in.clip.z < _waterSceneDepthAt(in.clip.xy / dsz);
    return vec4<f32>(select(sat(c.a * 2.0), 0.0, hidden), 0.0, 0.0, 1.0);
}
