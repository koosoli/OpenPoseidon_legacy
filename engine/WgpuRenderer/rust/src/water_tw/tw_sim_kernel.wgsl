// Tidewater Native — the shore simulation kernel (W3b): ShoreSim.js `this.kernel`
// (dgreenheck/tidewater @ 4811ba48, MIT, © DRG Software Solutions LLC), line for line apart from
// the adaptations listed in shore_sim.rs (regions as array layers, heights relative to OP's sea
// level, no spray deposits before W4). Composed as tw_common + tw_shore + tw_sim + this file.

@group(0) @binding(0) var<uniform> tw: TwSurface;
@group(0) @binding(1) var<uniform> cf: ConformParams;
@group(0) @binding(2) var seabed_heightmap: texture_2d<f32>;
@group(0) @binding(3) var shoreFieldT: texture_2d<f32>;
@group(0) @binding(4) var shoreFieldDir: texture_2d<f32>;
@group(0) @binding(5) var shoreSimStateTex: texture_2d_array<f32>;
@group(0) @binding(6) var shoreSimOut: texture_storage_2d_array<rgba16float, write>;
@group(0) @binding(7) var shoreSimLace: texture_2d<f32>;
@group(0) @binding(8) var smpLinearRepeat: sampler;

@compute @workgroup_size(8, 8, 1)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
    let res = u32(SHORE_SIM_RES);
    let r = i32(gid.z);
    // (an unplaced region: nothing to do)
    if (gid.x >= res || gid.y >= res || tw.sim[r].z <= 0.0) { return; }
    let ij = vec2<f32>(gid.xy);
    let uv = (ij + 0.5) / SHORE_SIM_RES;
    let simSize = tw.sim[r].z;
    let p = tw.sim[r].xy + uv * simSize;
    let ground = terrainHeightAt(p);
    let depth = tw.sea.x - ground;
    let dt = tw.simP1.z;
    let dryTime = tw.simP0.x;

    let here = textureLoad(shoreSimStateTex, vec2<i32>(gid.xy), r, 0);
    // (spray deposits arrive with the spray, W4)
    let drops = 0.0;

    // far from the surf and swash zone nothing happens: just let everything decay
    // (W3n, OP: Tidewater cuts off at 7 m. Its beach falls away steeply, so nothing is there to
    // cut; Everon's south coast is a wide shelf the bores cross at 5-7 m, and the 7 m contour of a
    // 50 m triangle heightmap drew the foam sheet's straight-edged outline. The cutoff is now at
    // 12 m and the surf fades into it between 4 and 9 m of smoothed depth, below.)
    if (depth > 12.0 || ground - tw.sea.x > 3.2) {
        let k = exp(-dt / 2.0);
        textureStore(shoreSimOut, gid.xy, r, vec4<f32>(here.x * k, here.y * exp(-dt / dryTime), here.z * k, 0.0));
        return;
    }

    let sw = shoreEvaluateWorld(p, depth, ground);

    // fraction of this texel covered by water: open water, or the swash sheet up to its leading
    // edge (soft over one texel, so no field stored here shows the texel grid)
    let cov = select(sat((sw.runup - sw.inland) / (simSize / SHORE_SIM_RES) + 0.5), 1.0, depth > 0.03);
    let covered = cov > 0.5;
    let vel = select(vec2<f32>(0.0), sw.flow, covered);

    // semi-Lagrangian advection (backtrace)
    let back = uv - vel * dt / simSize;
    let prev = shoreSimStateAt(back, r);

    // foam: made where the bore roller / plunge point / swash front pass, torn into patches by
    // the mottling of the lace texture, then it decays (bubbles rising and popping) and drains
    // into the sand once the water has gone
    let mott = textureSampleLevel(shoreSimLace, smpLinearRepeat, p / (SHORE_SIM_LACE_TILE * 4.3), 2.0).z;
    let patchK = smoothstep(0.25, 0.75, mott) * 0.8 + 0.35;
    let swashy = smoothstep(0.35, 0.05, depth);
    // dense foam collapses within a second or two (big bubbles burst first), the lace it leaves lingers
    let lace = mix(tw.simP0.z, tw.simP0.y, swashy);
    let life = mix(0.7, mix(lace, 0.8, smoothstep(0.3, 0.8, prev.x)), cov);
    // (W3n) 0 in the surf zone .. 1 where Tidewater's cutoff decay takes over
    let deepK = smoothstep(4.0, 9.0, shoreSmoothDepth(p, depth));
    let made = (sw.foam * 2.2 + sw.swashFoam * 1.4) * patchK * cov * (1.0 - deepK);
    // the foam is diluted where the flow spreads it out (the uprush thinning as it climbs, the
    // backwash draining): d(foam)/dt = - foam * du/ds along the flow
    let h = simSize / SHORE_SIM_RES;
    let dirH = sw.dir * (h / simSize);
    let uAhead = shoreSimStateAt(uv + dirH, r).w;
    let uBehind = shoreSimStateAt(uv - dirH, r).w;
    let spreadRate = max((uAhead - uBehind) / (2.0 * h), 0.0);
    let splash = drops * tw.simP1.y * cov;
    let foam = min(prev.x * exp(-dt * (1.0 / life + spreadRate + deepK * 0.5)) + made * tw.simP1.x * dt + splash, 1.0);

    // wetness: saturated while covered, then dries
    let wet = max(here.y * exp(-dt / dryTime), cov);

    // residue: foam stranded on the sand when the water leaves (the draining film gathers its
    // bubbles into lines, so it concentrates); washed away by the next uprush
    let stranded = min(here.x * 2.4, 1.0) * (1.0 - cov);
    let residue = max(here.z * mix(exp(-dt / tw.simP0.w), 0.85, cov), stranded);

    textureStore(shoreSimOut, gid.xy, r, vec4<f32>(foam, wet, residue, dot(vel, sw.dir)));
}
