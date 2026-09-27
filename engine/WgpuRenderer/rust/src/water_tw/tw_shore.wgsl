// Tidewater Native — shoreline waves (W3), ported from dgreenheck/tidewater @ 4811ba48 (MIT,
// © DRG Software Solutions LLC):
//   src/ocean/ShoreWaves.js        the whole WGSL module (_code(), evaluate() variants), generated
//                                  from the source and then mechanically renamed (below)
//   src/world/TerrainGPU.js        terrainShoreSample
//   src/engine/render/wgsl/common.js  perlin2 and its MaterialX hash
//
// Appended to tw_water.wgsl (one module; see mod.rs). Renames, all mechanical:
//   shoreP.{period, amplitude, variation, gamma}   -> tw.shore0.{x, y, z, w}
//   shoreP.{breakSpan, curl, runup, enabled}       -> tw.shore1.{x, y, z, w}
//   shoreP.turbidity                               -> tw.shore2.x
//   frame.time / frame.seaLevel                    -> tw.sea.y (water clock) / tw.sea.x
// Two adaptations to OP:
// * shoreDirAt / the regional direction texture is not ported: Tidewater only builds it as a
//   cheaper lookup and falls back to terrainShoreSample when it is absent (dirOn = 0), which is
//   the path used here.
// * the along-shore coordinate is measured across the swell direction (tw.shore2.y), relative to
//   the field centre (tw.shore2.zw), instead of across the local wave direction from the world
//   origin. Tidewater's dot(xz, perp(localDir)) stretches by |xz| / R wherever the fronts curve
//   (radius R): ~5x at the edge of its 1 km island, ~20x in a bay 6 km from OP's map centre,
//   which would crush its ~100 m peaks and 1.5 m swash lobes into noise. Across the fixed swell
//   axis it never compresses (it only stretches where the waves are refracted far round, where
//   the exposure is low), and it stays one continuous function of position.
// * outside the map (the heightmap repeats its edge heights there) the exposure is faded to 0
//   over one field cell, so no shore waves are drawn beyond the field.
// The field itself (arrival time T, propagation direction x exposure, shoreline arrival time)
// is Tidewater's ShoreField.js, computed on the CPU for the whole world (shore_field.rs).

// ------------------------------------------------------------------ common.js: perlin2

fn _mxRotl(x: u32, k: u32) -> u32 { return (x << k) | (x >> (32u - k)); }
fn _mxFinal(a0: u32, b0: u32, c0: u32) -> u32 {
    var a = a0; var b = b0; var c = c0;
    c ^= b; c -= _mxRotl(b, 14u);
    a ^= c; a -= _mxRotl(c, 11u);
    b ^= a; b -= _mxRotl(a, 25u);
    c ^= b; c -= _mxRotl(b, 16u);
    a ^= c; a -= _mxRotl(c, 4u);
    b ^= a; b -= _mxRotl(a, 14u);
    c ^= b; c -= _mxRotl(b, 24u);
    return c;
}
fn mxHash2(x: i32, y: i32) -> u32 { let s = 0xdeadbeefu + (2u << 2u) + 13u; return _mxFinal(s + u32(x), s + u32(y), s); }
fn _mxGrad2(hash: u32, x: f32, y: f32) -> f32 {
    let h = hash & 7u;
    let u = select(y, x, h < 4u);
    let v = 2.0 * select(x, y, h < 4u);
    return select(u, -u, (h & 1u) != 0u) + select(v, -v, (h & 2u) != 0u);
}
fn _fade3(t: vec3<f32>) -> vec3<f32> { return t * t * t * (t * (t * 6.0 - 15.0) + 10.0); }
fn perlin2(p: vec2<f32>) -> f32 {
    let fl = floor(p);
    let X = i32(fl.x); let Y = i32(fl.y);
    let fx = p.x - fl.x; let fy = p.y - fl.y;
    let u = _fade3(vec3<f32>(fx, fy, 0.0));
    let v0 = _mxGrad2(mxHash2(X, Y), fx, fy);
    let v1 = _mxGrad2(mxHash2(X + 1, Y), fx - 1.0, fy);
    let v2 = _mxGrad2(mxHash2(X, Y + 1), fx, fy - 1.0);
    let v3 = _mxGrad2(mxHash2(X + 1, Y + 1), fx - 1.0, fy - 1.0);
    let s1 = 1.0 - u.x;
    return ((1.0 - u.y) * (v0 * s1 + v1 * u.x) + u.y * (v2 * s1 + v3 * u.x)) * 0.6616;
}

// ------------------------------------------------------------------ TerrainGPU.js: the shore field

// (T, dirX * exposure, dirZ * exposure, shoreline arrival T), bilinear via exact loads. Two
// textures in OP: T and the shoreline T in rg32float, the direction in rgba8snorm (a world-wide
// field in rgba32float would cost twice the memory). Cell centres at origin + (i + 0.5) * cell.
fn terrainShoreSample(xz: vec2<f32>) -> vec4<f32> {
    let res = vec2<f32>(textureDimensions(shoreFieldT));
    let f = (xz - tw.shoreGrid.xy) / max(tw.shoreGrid.z, 1e-3) - 0.5;
    let fc = clamp(f, vec2<f32>(0.0), max(res - 1.001, vec2<f32>(0.0)));
    let i = floor(fc);
    let t = fract(fc);
    let ii = vec2<i32>(i);
    let mx = max(vec2<i32>(res) - 1, vec2<i32>(0));
    let i1 = min(ii + vec2<i32>(1, 0), mx);
    let i2 = min(ii + vec2<i32>(0, 1), mx);
    let i3 = min(ii + vec2<i32>(1, 1), mx);
    let ta = textureLoad(shoreFieldT, ii, 0).xy;
    let tb = textureLoad(shoreFieldT, i1, 0).xy;
    let tc = textureLoad(shoreFieldT, i2, 0).xy;
    let td = textureLoad(shoreFieldT, i3, 0).xy;
    let da = textureLoad(shoreFieldDir, ii, 0).xy;
    let db = textureLoad(shoreFieldDir, i1, 0).xy;
    let dc = textureLoad(shoreFieldDir, i2, 0).xy;
    let dd = textureLoad(shoreFieldDir, i3, 0).xy;
    let a = vec4<f32>(ta.x, da, ta.y);
    let b = vec4<f32>(tb.x, db, tb.y);
    let c = vec4<f32>(tc.x, dc, tc.y);
    let d = vec4<f32>(td.x, dd, td.y);
    let s = mix(mix(a, b, t.x), mix(c, d, t.x), t.y);
    // inside the field: 1, fading to 0 over one cell beyond it (exposure only: no waves outside)
    let over = max(max(-f - 0.5, f - (res - 0.5)), vec2<f32>(0.0));
    let inside = 1.0 - sat(max(over.x, over.y));
    return vec4<f32>(s.x, s.yz * inside, s.w);
}

// (W3l, OP) The depth the surf decides its breaking from: the shore field's smoothed depth
// (~20 m blur, shore_field.rs) away from the waterline, the real depth near it (so the waves still
// run up the real beach). OP's 50 m triangle heightmaps have straight-segment depth contours, and
// Tidewater's breaking switches sharply across a contour: the surf foam ended along polygons.
fn shoreSmoothDepth(xz: vec2<f32>, d: f32) -> f32 {
    let res = vec2<f32>(textureDimensions(shoreFieldDir));
    let f = (xz - tw.shoreGrid.xy) / max(tw.shoreGrid.z, 1e-3) - 0.5;
    let fc = clamp(f, vec2<f32>(0.0), max(res - 1.001, vec2<f32>(0.0)));
    let i = vec2<i32>(floor(fc));
    let t = fract(fc);
    let mx = max(vec2<i32>(res) - 1, vec2<i32>(0));
    let a = textureLoad(shoreFieldDir, i, 0).z;
    let b = textureLoad(shoreFieldDir, min(i + vec2<i32>(1, 0), mx), 0).z;
    let c = textureLoad(shoreFieldDir, min(i + vec2<i32>(0, 1), mx), 0).z;
    let e = textureLoad(shoreFieldDir, min(i + vec2<i32>(1, 1), mx), 0).z;
    let ds = mix(mix(a, b, t.x), mix(c, e, t.x), t.y) * 32.0;
    // (no smoothed value: a field without it, or land all around)
    let use_s = select(0.0, smoothstep(1.0, 4.0, d), ds > 0.05);
    return mix(d, ds, use_s);
}

// Tidewater's terrainNormalRock(xz).xy (the terrain normal's x and z) from OP's heightmap by
// central differences, one heightmap cell apart (Tidewater bakes a normal texture instead).
fn terrainNormalXZ(xz: vec2<f32>) -> vec2<f32> {
    let e = max(cf.terrain_grid * 0.5, 0.25);
    let gx = (terrainHeightAt(xz + vec2<f32>(e, 0.0)) - terrainHeightAt(xz - vec2<f32>(e, 0.0))) / (2.0 * e);
    let gz = (terrainHeightAt(xz + vec2<f32>(0.0, e)) - terrainHeightAt(xz - vec2<f32>(0.0, e))) / (2.0 * e);
    let n = normalize(vec3<f32>(-gx, 1.0, -gz));
    return n.xz;
}

// ------------------------------------------------------------------ ShoreWaves.js (generated)


const SHORE_GRAVITY: f32 = 9.81;
const SHORE_TAU: f32 = 6.283185307179586;
const SHORE_BEACH_SLOPE: f32 = 0.066;
const SHORE_SWASH_UP: f32 = 0.4;
const SHORE_SWASH_DOWN: f32 = 0.55;
const SHORE_SWASH_OVERSHOOT: f32 = 1.2;

struct ShoreSample {
	disp: vec3f,
	nShore: vec3f,
	env: f32,
	foam: f32,
	breaking: f32,
	u: f32,
	dir: vec2f,
	exposure: f32,
	swashLevel: f32,
	swashCovered: f32,
	thick: f32,
	swashFoam: f32,
	runup: f32,
	inland: f32,
	dRdt: f32,
	tau: f32,
	flow: vec2f,
	flowSpeed: f32,
	face: f32,
	roller: f32,
};

struct ShoreMedium { scatter: vec3f, absorb: vec3f };

struct ShorePhase { sh: vec4f, T: f32, dir: vec2f, exposure: f32, along: f32, s: f32 };

struct ShoreBreak {
	db: f32, b: f32, Ash: f32, p: f32, meanP: f32, crestPeak: f32, yc: f32, yt: f32,
	H: f32, wBore: f32, Xi: f32, Hb: f32, ycB: f32, ytB: f32, Xc: f32, Wt: f32,
};

struct ShoreProfile { x: f32, y: f32, b: f32, foam: f32, face: f32, roller: f32 };

struct ShorePair { s0: vec4f, s1: vec4f };

struct ShoreRunup { tau: f32, Rt: f32, inland: f32, RhMax: f32, su: f32, sb: f32, isUp: bool };

fn shoreHash1( x: f32 ) -> f32 { return fract( sin( x * 127.1 + 311.7 ) * 43758.5453 ); }

// Bathymetry along the beach that the travel-time field doesn't resolve: a bar with rip channels
// cut through it every ~100 m (irregular spacing and width). Over the bar the waves are bigger and
// break first and farther out (the peaks); in the channels they are much smaller and roll through
// unbroken almost to the shorebreak, so a set never closes out along the whole beach at once.
// Returns 1 over the bar, less in a channel; rip (0..1) is the channel mask.
struct ShoreBar { k: f32, rip: f32 };
fn shoreBar( along: f32 ) -> ShoreBar {
	let w = sin( along * 0.021 + 1.9 ) * 1.4 + sin( along * 0.009 + 0.3 ) * 0.9;
	let r = sin( along * 0.059 + w );
	let width = 0.8 + sin( along * 0.017 + 4.1 ) * 0.08; // channels of different width
	let rip = smoothstep( width, 0.985, r );
	// the bar itself is uneven: broad peaks and lower shoulders
	let bar = 0.9 + ( sin( along * 0.031 + 7.3 ) * 0.6 + sin( along * 0.083 + 1.1 ) * 0.4 ) * 0.14;
	return ShoreBar( bar * ( 1.0 - rip * 0.58 ), rip );
}

// along-shore phase wobble (in periods): crests bend over the uneven bottom (and run ahead in the
// deeper rip channels, where the waves travel faster)
fn shoreWobble( along: f32 ) -> f32 {
	return sin( along * 0.029 + 0.7 ) * 0.07 + sin( along * 0.083 + 2.1 ) * 0.035
		+ sin( along * 0.19 + 0.4 ) * 0.022 + sin( along * 0.37 + 2.6 ) * 0.011
		+ shoreBar( along ).rip * 0.045;
}

// ------------------------------------------------------------ per-wave height

fn shoreWaveAmp( m: f32, along: f32 ) -> f32 {
	// sets: groups of ~7 waves with larger ones in the middle, plus per-wave randomness
	let waveSet = abs( sin( m * 0.4487989505128276 ) ) * 0.6 + 0.55;
	let rnd = ( shoreHash1( m ) - 0.5 ) * 2.0;
	// along-shore variation so waves peel instead of closing out
	// peaks ~40-50 m wide with lower shoulders between them, different for every wave (the warp keeps
	// them from repeating along the beach); each peak breaks first and peels outward from it
	let warp = sin( along * 0.016 + m * 0.9 ) * 1.6;
	let a1 = sin( along * 0.062 + m * 1.7 + warp );
	let a2 = sin( along * 0.13 + m * 4.1 + 1.3 - warp * 0.7 );
	// (plus a shorter ~20 m variation: bores that rise and sag along the crest, more peel sections)
	let a3 = sin( along * 0.29 + m * 2.3 + warp * 0.5 ) * 0.6 + sin( along * 0.47 + m * 5.9 + 0.8 ) * 0.4;
	let alongV = a1 * 0.6 + a2 * 0.4 + a3 * 0.28;
	return max( tw.shore0.y * waveSet * ( 1.0 + rnd * tw.shore0.z * 0.5 + alongV * tw.shore0.z * 0.7 ) * shoreBar( along ).k, 0.02 );
}

// ------------------------------------------------------------ cross-section shape

fn shoreBreakParams( A: f32, d: f32 ) -> ShoreBreak {
	var P: ShoreBreak;
	// break depth for this wave (Green's law shoaling, H = gamma d)
	P.db = pow( A * 3.556 / tw.shore0.w, 0.8 );
	P.b = ( P.db - d ) / ( P.db * tw.shore1.x ); // <0 shoaling, 0..1 plunging, >1 bore
	let shoal = pow( 10.0 / clamp( d, 0.35, 10.0 ), 0.25 );
	P.Ash = A * shoal;
	P.p = mix( 1.0, 3.0, smoothstep( -2.5, 0.0, P.b ) );
	P.meanP = 1.0 / sqrt( ( P.p + 0.25 ) * PI );
	P.crestPeak = 1.0 + smoothstep( -1.0, 0.3, P.b ) * 0.22;
	P.yc = P.Ash * 2.0 * ( 1.0 - P.meanP ) * P.crestPeak; // crest height
	P.yt = P.Ash * -2.0 * P.meanP * P.crestPeak; // trough level
	P.H = P.yc - P.yt;
	P.wBore = smoothstep( 0.85, 1.35, P.b );
	P.Xi = P.H * 0.8; // lip throw at impact
	// bore height, limited by the depth and fading out in the last few decimetres
	P.Hb = min( P.H * 0.6, max( d, 0.0 ) * 0.75 ) * ( smoothstep( 0.0, 0.3, d ) * 0.7 + 0.3 );
	P.ycB = mix( P.yc, P.yt * 0.6 + P.Hb, P.wBore ); // crest / roller top
	P.ytB = mix( P.yt, P.yt * 0.6, P.wBore ); // trough
	// the collapsing crest moves to where the lip landed (no shift once the bore has run out of height)
	P.Xc = mix( 0.0, P.Xi - P.Hb * 0.55, P.wBore ) * smoothstep( 0.03, 0.3, P.Hb );
	// horizontal extent of the face: concave tube face while plunging, short convex roller front on the bore
	P.Wt = mix( P.H * mix( 0.25, 0.55, smoothstep( 0.1, 0.9, P.b ) ), P.Hb * 0.6 + 0.08, P.wBore );
	return P;
}

// Whitewater made by the breaking wave at xi (m): the horizontal position relative to the crest's
// rest position, positive shoreward (a parcel's own position, or a fixed world point: the shore
// simulation deposits it where the water actually is, not where the parcel rests).
//  * nothing at all while the lip is in the air: the face and the lip are clear, glassy water
//  * the lip lands in the trough at the plunge point (b ~ 0.9, xi = Xi): whitewater appears there
//    and spreads out from it while the tube collapses behind it
//  * the collapsed tube becomes the roller: whitewater over the front and top of the bore,
//    shedding foam behind it (carried on by ShoreSim)
// whitewater over the face after the plunge: s = 0 at the crest .. 1 at the foot of the face
fn shoreFaceFill( b: f32, s: f32 ) -> f32 {
	// the foot turns white first (where the lip lands), the top of the face last: along a peeling
	// crest the edge of the broken section slants down and forward instead of standing vertical
	let k = ( 1.0 - s ) * 0.45;
	return smoothstep( k + 0.9, k + 1.08, b );
}

fn shoreWhitewater( xi: f32, lam: f32, P: ShoreBreak ) -> f32 {
	let landed = smoothstep( 0.86, 0.99, P.b );
	let spread = sat( ( P.b - 0.9 ) / 0.45 );
	let reach = P.Xi * 0.25 + spread * ( P.Xi * 0.9 + 1.0 ); // radius around the plunge point
	let impact = landed * smoothstep( reach, reach * 0.6, abs( xi - P.Xi ) ) * ( 1.0 - smoothstep( 1.4, 1.9, P.b ) );
	let toe = P.Xc + P.Wt;
	// the collapsing tube turns white from its foot (next to the plunge point) up to the crest
	let faceFill = shoreFaceFill( P.b, sat( ( xi - P.Xc ) / max( P.Wt, 0.05 ) ) );
	let ahead = exp( ( xi - toe ) * -3.0 ) * P.wBore;
	let behind = exp( ( P.Xc - xi ) / lam * -16.0 ) * P.wBore;
	let roller = select( select( max( faceFill, P.wBore ), behind, xi < P.Xc ), ahead, xi > toe );
	return sat( max( impact, roller ) );
}

// the cross-section for break parameters P (see shoreBreakParams)
// u: local phase in [-0.5, 0.5], crest at 0, u < 0 in front (shoreward) of the crest
// Returns x: shoreward displacement, y: height above mean, b: breaking progress (+ foam, face, roller withFoam)
fn shoreProfile( u: f32, lam: f32, P: ShoreBreak, withFoam: bool ) -> ShoreProfile {
	// --- shoaling: peaked (cnoidal-like) crest, the front compressed by a phase skew
	let skew = smoothstep( -3.0, 0.0, P.b ) * 0.55;
	let phi = u - skew * ( 1.0 - cos( u * SHORE_TAU ) ) / SHORE_TAU;
	let c = max( ( cos( phi * SHORE_TAU ) + 1.0 ) * 0.5, 0.0 ); // pow() of a rounding-negative base is NaN
	let yPre = P.Ash * 2.0 * ( pow( c, P.p ) - P.meanP ) * P.crestPeak;
	let Q = smoothstep( -3.0, 0.0, P.b ) * 0.25 + 0.1;
	let xPre = sin( u * SHORE_TAU ) * P.Ash * Q;

	// --- plunging / bore profile. Front: the upper uf of the phase is an elliptic arc from the
	// crest down to the trough (concave tube face -> convex roller front), the rest of the
	// front is trough, stretched to meet the next wave. Back: the shoaling back, decaying
	// exponentially behind the bore.
	let uf = 0.09;
	let inFace = u > - uf;
	let th = clamp( - u / uf, 0.0, 1.0 ) * 1.5707963267948966;
	let fx = mix( 1.0 - cos( th ), sin( th ), P.wBore );
	let fy = mix( 1.0 - sin( th ), cos( th ), P.wBore );
	let sTr = clamp( ( - u - uf ) / ( 0.5 - uf ), 0.0, 1.0 );
	let ul = u * lam;
	let xFront = select( mix( P.Xc + P.Wt, lam * 0.5, sTr ), P.Xc + P.Wt * fx, inFace ) + ul;
	let yFront = select( P.ytB, P.ytB + ( P.ycB - P.ytB ) * fy, inFace );
	let cb = pow( max( ( cos( u * ( SHORE_TAU * 0.85 ) ) + 1.0 ) * 0.5, 0.0 ), P.p );
	let yBack = P.ytB + ( P.ycB - P.ytB ) * mix( cb, exp( u * -7.0 ), P.wBore );
	let xBack = P.Xc * exp( u * -6.0 ) * smoothstep( 0.5, 0.35, u ) + xPre * ( 1.0 - P.wBore );
	let front = u < 0.0;
	let wC = smoothstep( -0.35, 0.25, P.b ) * tw.shore1.y;
	var r: ShoreProfile;
	r.x = mix( xPre, select( xBack, xFront, front ), wC );
	r.y = mix( yPre, select( yBack, yFront, front ), wC );
	r.b = P.b;
	if ( withFoam ) {
		// --- whitewater (a function of where the parcel is now, see shoreWhitewater) and, per parcel, the
		// clear face of the plunging wave and the relief of the roller
		r.foam = shoreWhitewater( r.x - u * lam, lam, P );
		let onFace = front && inFace;
		let faceFill = shoreFaceFill( P.b, fx );
		// the clear, concave face of a plunging wave (WaterSurface keeps the foam carried by the water off it)
		r.face = select( 0.0, 1.0, onFace ) * smoothstep( -0.4, 0.0, P.b ) * ( 1.0 - faceFill );
		// turbulent relief of the whitewater roller (m): its front and the top it tumbles over
		r.roller = select( exp( u * -25.0 ), select( 0.0, 1.0, inFace ), front ) * P.wBore * P.Hb * wC;
	}
	return r;
}

// ( x, y, foam, b )
fn shoreShape( u: f32, A: f32, d: f32, lam: f32 ) -> vec4f {
	let s = shoreProfile( u, lam, shoreBreakParams( A, d ), true );
	return vec4f( s.x, s.y, s.foam, s.b );
}

// the cross-section at u and at u + du (for the surface normal) in one call:
// ( x0, y0, foam, b ), ( x1, y1, face, roller )
fn shoreShapePair( u: f32, du: f32, A: f32, d: f32, lam: f32 ) -> ShorePair {
	let P = shoreBreakParams( A, d );
	let s0 = shoreProfile( u, lam, P, true );
	let s1 = shoreProfile( u + du, lam, P, false );
	return ShorePair( vec4f( s0.x, s0.y, s0.foam, s0.b ), vec4f( s1.x, s1.y, s0.face, s0.roller ) );
}

// the surface at a fixed WORLD point (for the Eulerian shore simulation): whitewater there and
// the water height there (the parcel shown at this point rests ~x up-wave: first-order inverse
// of the horizontal displacement, which is large on a breaking wave)
// ( foam, height, displacement of the parcel resting here, b )
fn shoreWorld( u: f32, A: f32, d: f32, lam: f32, env: f32 ) -> vec4f {
	let P = shoreBreakParams( A, d );
	let s0 = shoreProfile( u, lam, P, false );
	let s1 = shoreProfile( u + s0.x * env / lam, lam, P, false );
	return vec4f( shoreWhitewater( - u * lam, lam, P ), s1.y, s0.x, P.b );
}

// the bore that follows the plunge: roller height Hb, horizontal extent of its front Wt, forward
// shift of the crest Xc (to where the lip landed), bore weight (0 while plunging .. 1)
fn shoreBore( A: f32, d: f32 ) -> vec4f {
	let P = shoreBreakParams( A, d );
	return vec4f( P.Hb, P.Wt, P.Xc, P.wBore );
}

// breaking progress b, wave height H, trough level (relative to mean) and how far the lip is thrown
fn shoreCrest( A: f32, d: f32 ) -> vec4f {
	let P = shoreBreakParams( A, d );
	return vec4f( P.b, P.yc - P.yt, P.ytB, P.Xi );
}

// Depth that sets the breaking state of the wave a parcel belongs to: the depth under that wave's
// crest, not under the parcel. Breaking is a property of the wave: a parcel ahead of the crest is
// in shallower water and would otherwise "break" first (whitewater creeping up the foot of a still
// glassy face). Near the troughs it hands over to the local depth, where the neighbouring wave
// takes over (the profile stays continuous at u = +-0.5). u: local phase, lam: local wavelength.
fn shoreBreakDepth( xz: vec2f, dir: vec2f, u: f32, lam: f32, d: f32 ) -> f32 {
	let pc = xz + dir * ( u * lam );
	// (W3l: both depths smoothed away from the waterline, see shoreSmoothDepth)
	let dc = shoreSmoothDepth( pc, tw.sea.x - terrainHeightAt( pc ) );
	return mix( dc, shoreSmoothDepth( xz, d ), smoothstep( 0.3, 0.5, abs( u ) ) );
}

// ------------------------------------------------------------ surf zone water

// Optical properties of the water stirred up by breaking waves: suspended sand and fine bubbles
// scatter light (milky, luminous), fine sediment and dissolved matter absorb blue. Returns the
// extra { scatter, absorb } coefficients (1/m) that make the surf zone turquoise, not ocean-clear.
fn shoreSurfMedium( xz: vec2f, depth: f32 ) -> ShoreMedium {
	var k = 0.0;
	if ( depth < 4.5 && depth > -0.2 ) {
		var expo: f32;
		expo = sat( length( terrainShoreSample( xz ).yz ) * 1.4 ); // (shoreP.dirOn is 0: no regional dir texture)
		k = smoothstep( 4.5, 1.2, depth ) * smoothstep( -0.2, 0.15, depth ) * expo * tw.shore2.x * tw.shore1.w;
	}
	return ShoreMedium( vec3f( 0.9, 1.0, 0.85 ) * k, vec3f( 0.1, 0.2, 0.62 ) * k );
}

// ------------------------------------------------------------ evaluation at a point

// Local wave phase data at a (Lagrangian) point: shared by evaluate() and the crest finder.
fn shorePhaseAt( xz: vec2f ) -> ShorePhase {
	let sh = terrainShoreSample( xz );
	let T = sh.x;
	let dirE = vec2f( sh.y, sh.z );
	let exposure = length( dirE );
	let dir = dirE / max( exposure, 1e-4 );
	let along = dot( xz - tw.shore2.zw, vec2f( - sin( tw.shore2.y ), cos( tw.shore2.y ) ) );
	let s = ( tw.sea.y - T ) / tw.shore0.x + shoreWobble( along );
	return ShorePhase( sh, T, dir, exposure, along, s );
}

// ------------------------------------------------------------ light through thin crests

// Water path (m) along the refracted view ray Tv (unit, inside the water) from the surface point
// with rest position lagXZ until the ray leaves through the other side of the wave, or 1e4 if
// it doesn't within a few metres. The upper part of a steep wave is only a few metres thick
// horizontally: the view ray crosses it and exits into the sky behind, which is what makes
// breaking faces and crests glow turquoise. Marches the analytic cross-section (up to 3 steps).
fn shoreCrestPath( p: vec2f, d: f32, T: vec3f ) -> f32 {
	var out = 1e4;
	if ( d >= 6.0 || tw.shore1.w <= 0.0 ) { return out; } // (before the phase lookup)
	let ph = shorePhaseAt( p );
	let tXi = dot( T.xz, ph.dir ); // shoreward component of the ray
	let env = smoothstep( 26.0, 13.0, d ) * sat( ph.exposure * 1.4 ) * tw.shore1.w;
	// rays heading out through the back of the wave (a view from the beach side), near breakers
	if ( env > 0.05 && tXi < -0.05 && d < 6.0 ) {
		let lam = sqrt( clamp( d, 0.3, 25.0 ) * SHORE_GRAVITY ) * tw.shore0.x;
		let m = floor( ph.s + 0.5 );
		let u = ph.s - m;
		let A = shoreWaveAmp( m, ph.along );
		let P = shoreBreakParams( A, shoreBreakDepth( p, ph.dir, u, lam, d ) );
		let s0 = shoreProfile( u, lam, P, false );
		let y0 = s0.y * env;
		// only the upper part of steep (nearly breaking) waves is thin enough to see through
		if ( y0 > A * 0.2 && P.b > -1.5 ) {
			let xi0 = - u * lam + s0.x * env;
			let slope = T.y / - tXi; // ray rise per metre of horizontal travel
			var prevGap = 0.0;
			var prevDist = 0.0;
			var off = 1.1;
			for ( var k = 0; k < 3; k++ ) {
				let uk = min( u + off / lam, 0.5 );
				let sk = shoreProfile( uk, lam, P, false );
				let dist = abs( xi0 - ( - uk * lam + sk.x * env ) );
				// ray height above the surface there (> 0: the ray has left the water)
				let gap = y0 + slope * dist - sk.y * env;
				if ( gap > 0.0 ) {
					let fr = - prevGap / max( gap - prevGap, 1e-4 );
					out = mix( prevDist, dist, sat( fr ) ) / - tXi;
					break;
				}
				prevGap = gap;
				prevDist = dist;
				off *= 2.6;
			}
		}
	}
	return out;
}

// ------------------------------------------------------------ swash

// Run-up of the most recent wave at a point on the beach (distances in metres up the beach face).
// The run-up is compared with the height of the sand (converted with the nominal beach slope), so
// the front is exact at the waterline and follows the contours of the sand. sh: terrainShoreSample( xz ).
fn shoreSwashRunup( sh: vec4f, along: f32, groundH: f32 ) -> ShoreRunup {
	let Tp = tw.shore0.x;
	let exposure = length( vec2f( sh.y, sh.z ) );
	let Ts = sh.w;
	let inland = max( groundH - tw.sea.x, 0.0 ) / SHORE_BEACH_SLOPE;
	let ss = ( tw.sea.y - Ts ) / Tp + shoreWobble( along );
	let ms = floor( ss );
	let tau = ss - ms; // 0..1 time since that wave's bore reached the shoreline
	let Am = shoreWaveAmp( ms, along );
	// vertical run-up ~ H on this gentle beach, converted to a horizontal excursion
	let RhMax = Am * 2.1 * tw.shore1.z * sat( exposure * 1.4 ) / SHORE_BEACH_SLOPE;
	// decelerating uprush, then a backwash that starts slowly and accelerates as the sheet drains
	let su = sat( tau / SHORE_SWASH_UP );
	let sb = sat( ( tau - SHORE_SWASH_UP ) / SHORE_SWASH_DOWN );
	let isUp = tau < SHORE_SWASH_UP;
	let Rh = select( 1.0 - pow( sb, 1.6 ), 1.0 - pow( 1.0 - su, 1.5 ), isUp ) * RhMax - 0.3;
	// the front is lobed, not a straight line: each wave runs up a little differently along the beach
	let lobes = sin( along * 0.61 + ms * 2.3 ) * 0.5 + sin( along * 1.73 + ms * 5.1 ) * 0.3 + sin( along * 4.3 + ms * 1.7 ) * 0.2;
	// the backwash never quite exposes the lower beach face: a film of water always covers the
	// first decimetres past the shoreline, so the sea never meets the sand along mesh triangles
	let Rt = max( Rh + lobes * ( max( Rh, 0.0 ) * 0.07 + 0.35 ), 0.35 ) * tw.shore1.w;
	return ShoreRunup( tau, Rt, inland, RhMax, su, sb, isUp );
}

// Water film thickness clipped at the leading edge of the swash sheet, for the water shader's
// edge fade: min( thickness, distance to the front (m) * 0.08 ). Only evaluated where the film is
// thin, so the sheet ends on the analytic front instead of the mesh triangles at ~no cost.
fn shoreSwashEdge( p: vec2f, t: f32 ) -> vec4f {
	var out = vec4f( t, 1e3, 0.0, 0.0 );
	if ( t < 0.3 ) {
		let g = terrainHeightAt( p );
		if ( g > tw.sea.x - 0.8 ) {
			let ph = shorePhaseAt( p );
			let r = shoreSwashRunup( ph.sh, ph.along, g );
			let front = r.Rt - r.inland;
			// The lapping region reaches down the beach face past where the sea's edge sits in the trough
			// of the backwash (it stopped at sea level: a strip in between with a straight edge across the
			// draining water), and fades in from there and from 0.3 m of film instead of switching on.
			// The front distance is divided by the weight so the effects at the front recede with it.
			let w = smoothstep( tw.sea.x - 0.8, tw.sea.x - 0.4, g ) * smoothstep( 0.3, 0.15, t );
			if ( w > 0.0 ) {
				out = vec4f( mix( t, min( t, front * 0.08 ), w ), front / max( w, 1e-3 ), r.tau, r.Rt * w );
			}
		}
	}
	return out;
}

fn shoreSwashClip( p: vec2f, t: f32 ) -> f32 {
	return shoreSwashEdge( p, t ).x;
}

// ------------------------------------------------------------ evaluate()
// Returns displacement relative to (xz, seaLevel), foam, breaking indicator and the
// swash surface level (absolute height) for this point. shoreEvaluate (withNormal, the water mesh) also
// gives face: 1 on the clear concave face of a plunging wave, roller: relief of the whitewater (m).
// shoreEvaluateWorld: the shore simulation's view (a fixed world point instead of a water parcel): foam and
// height are those of the water shown at xz, the displacement is not meaningful.

fn shoreEvaluate( xz: vec2f, depth: f32, groundH: f32 ) -> ShoreSample {
	let ph = shorePhaseAt( xz );
	let sh = ph.sh;
	let exposure = ph.exposure;
	let along = ph.along;
	let dir = ph.dir;
	let Tp = tw.shore0.x;

	let d = depth;
	let c = sqrt( clamp( d, 0.3, 25.0 ) * SHORE_GRAVITY );
	let lam = c * Tp;

	let s = ph.s;
	let m = floor( s + 0.5 );
	let u = s - m;

	// wave height with smooth hand-over between consecutive waves at the trough
	let A0 = shoreWaveAmp( m, along );
	let An = shoreWaveAmp( m + sign( u ), along );
	let A = mix( A0, An, smoothstep( 0.32, 0.5, abs( u ) ) * 0.5 );
	// the breaking state of the whole wave comes from the depth under its crest
	let dB = shoreBreakDepth( xz, dir, u, lam, d );

	// offshore fade-in (FFT covers deep water) and fade on land (the swash sheet takes over there)
	let env = smoothstep( 26.0, 13.0, d ) * smoothstep( -0.25, 0.05, d ) * sat( exposure * 1.4 ) * tw.shore1.w;

	// finite difference along the propagation direction for the normal
	let e = 0.15;
	var face = 0.0;
	var roller = 0.0;
	let pair = shoreShapePair( u, - e / lam, A, dB, lam );
	let s0 = pair.s0;
	let s1 = pair.s1;
	face = s1.z * env;
	roller = s1.w * env;

	var disp = vec3f( dir.x * s0.x, s0.y, dir.y * s0.x ) * env;

	var nShore = vec3f( 0.0, 1.0, 0.0 );
	{
		let dX = e + ( s1.x - s0.x ) * env;
		let dY = ( s1.y - s0.y ) * env;
		let tAlong = vec3f( dir.x * dX, dY, dir.y * dX );
		let tAcross = vec3f( - dir.y, 0.0, dir.x );
		// epsilon: never a zero vector; never facing down (the mesh doesn't overhang, the lip sheet does)
		let n = normalize( cross( tAcross, tAlong ) + vec3f( 0.0, 1e-4, 0.0 ) );

		// the whitewater roller is not a smooth tube: lumps of foam tumble along its front and over
		// its top (relief of a few decimetres, with its slope in the normal). Noise, not a sum of sines:
		// regular bumps along the crest read as a row of identical puffs. Bigger, fewer lumps in some
		// stretches, a lower, smoother churn in others (different for every wave).
		let sx = u * lam; // rest position along the wave direction (m, seaward)
		let t = tw.sea.y;
		let mW = floor( ph.s + 0.5 );
		// (the lumps only exist on the roller: amp is 0 elsewhere)
		if ( roller != 0.0 ) {
		let lumpy = sat( perlin2( vec2f( along * 0.045, mW * 3.7 ) ) * 1.2 + 0.55 );
		let amp = roller * mix( 0.25, 0.7, lumpy );
		let q1 = vec2f( along * 0.28, sx * 0.7 - t * 0.8 );
		let q2 = vec2f( along * 0.8 + 11.3, sx * 1.6 - t * 1.5 );
		let eL = 0.25;
		let L0 = perlin2( q1 ) * 0.7 + perlin2( q2 ) * 0.3;
		let La = perlin2( q1 + vec2f( eL * 0.28, 0.0 ) ) * 0.7 + perlin2( q2 + vec2f( eL * 0.8, 0.0 ) ) * 0.3;
		let Ls = perlin2( q1 + vec2f( 0.0, eL * 0.7 ) ) * 0.7 + perlin2( q2 + vec2f( 0.0, eL * 1.6 ) ) * 0.3;
		// lumps stand up from the roller (rounded caps, flatter troughs between them)
		let lump = max( L0 * 1.5 + 0.2, -0.3 );
		disp.y += lump * amp;
		let dAlong = select( 0.0, ( La - L0 ) / eL * 1.5, L0 * 1.5 + 0.2 > -0.3 );
		let dSx = select( 0.0, ( Ls - L0 ) / eL * 1.5, L0 * 1.5 + 0.2 > -0.3 );
		let dShore = - dSx; // d/d(shoreward) = - d/dsx
		let g = ( vec2f( - dir.y, dir.x ) * dAlong + dir * dShore ) * amp * n.y;
		nShore = normalize( vec3f( n.x - g.x, max( n.y, 0.04 ), n.z - g.y ) );
		} else {
			nShore = normalize( vec3f( n.x, max( n.y, 0.04 ), n.z ) );
		}
	}

	// ---- swash: run-up of the most recent wave on the sand
	let swr = shoreSwashRunup( sh, along, groundH );
	let front = swr.Rt - swr.inland; // signed distance to the leading edge (m), > 0 under the sheet
	let covered = front > 0.0;
	// leading edge velocity along the slope (m/s), positive = uphill
	let dRdt = select( pow( max( swr.sb, 1e-3 ), 0.6 ) * ( - 1.6 / SHORE_SWASH_DOWN ), pow( 1.0 - swr.su, 0.5 ) * ( 1.5 / SHORE_SWASH_UP ), swr.isUp ) * swr.RhMax / Tp;
	// thin sheet (a few cm, thickening behind the leading edge). The mesh sheet overshoots the
	// leading edge a little; the water shader cuts it exactly on the front (swashClip), so the edge
	// doesn't follow the mesh triangles.
	let fm = front + SHORE_SWASH_OVERSHOOT;
	let thick = clamp( min( fm * 0.3, max( fm - 0.1, 0.0 ) * ( SHORE_BEACH_SLOPE * 0.22 ) + 0.03 ), -0.1, 0.12 );
	let swashLevel = groundH + thick;
	// bubbly foam line riding the leading edge all the way up (left behind as the swash mark)
	let uprush = smoothstep( 0.46, 0.32, swr.tau );
	let edge = smoothstep( 0.8, 0.0, front ) * smoothstep( -0.05, 0.05, front ) * uprush * smoothstep( 0.0, 1.0, swr.Rt );
	let swashFoam = edge * 0.9;

	// ---- depth-averaged water velocity (along dir), for foam advection
	// waves / bores: shallow-water particle velocity c * eta / h; swash sheet: the tip moves at
	// dR/dt, slower toward the shoreline during uprush, faster there while it drains
	let eta = disp.y;
	let uWave = clamp( c * eta / ( max( d, 0.0 ) + max( eta, d * -0.8 ) + 0.15 ), -2.5, 4.0 );
	let rel = sat( swr.inland / max( swr.Rt, 0.5 ) );
	let uSwash = dRdt * select( ( 1.0 - rel ) * 0.6 + 0.9, clamp( ( swr.inland + 3.0 ) / ( swr.Rt + 3.0 ), 0.15, 1.0 ), swr.isUp );
	let wSwash = smoothstep( 0.3, 0.05, d );
	let uFlow = mix( uWave, uSwash, wSwash ) * select( 0.0, 1.0, covered || d > 0.02 );

	var o: ShoreSample;
	// the churn of a bore is uneven along the crest: dense in some stretches, torn into patches and
	// lace in others (different for every wave, drifting slowly along it)
	var wwPatch = 0.0;
	if ( s0.z * env != 0.0 ) { wwPatch = smoothstep( -0.5, 0.45, perlin2( vec2f( along * 0.06 + tw.sea.y * 0.05, m * 2.9 + 0.4 ) ) ); }
	o.disp = disp; o.nShore = nShore; o.env = env; o.foam = s0.z * env * mix( 0.3, 1.0, wwPatch ); o.breaking = s0.w; o.u = u; o.dir = dir;
	o.exposure = exposure; o.swashLevel = swashLevel; o.swashCovered = select( 0.0, 1.0, covered ); o.thick = thick;
	o.swashFoam = swashFoam; o.runup = swr.Rt; o.inland = swr.inland; o.dRdt = dRdt; o.tau = swr.tau;
	o.flow = dir * uFlow; o.flowSpeed = uFlow; o.face = face; o.roller = roller;
	return o;
}


fn shoreEvaluateNoNormal( xz: vec2f, depth: f32, groundH: f32 ) -> ShoreSample {
	let ph = shorePhaseAt( xz );
	let sh = ph.sh;
	let exposure = ph.exposure;
	let along = ph.along;
	let dir = ph.dir;
	let Tp = tw.shore0.x;

	let d = depth;
	let c = sqrt( clamp( d, 0.3, 25.0 ) * SHORE_GRAVITY );
	let lam = c * Tp;

	let s = ph.s;
	let m = floor( s + 0.5 );
	let u = s - m;

	// wave height with smooth hand-over between consecutive waves at the trough
	let A0 = shoreWaveAmp( m, along );
	let An = shoreWaveAmp( m + sign( u ), along );
	let A = mix( A0, An, smoothstep( 0.32, 0.5, abs( u ) ) * 0.5 );
	// the breaking state of the whole wave comes from the depth under its crest
	let dB = shoreBreakDepth( xz, dir, u, lam, d );

	// offshore fade-in (FFT covers deep water) and fade on land (the swash sheet takes over there)
	let env = smoothstep( 26.0, 13.0, d ) * smoothstep( -0.25, 0.05, d ) * sat( exposure * 1.4 ) * tw.shore1.w;

	// finite difference along the propagation direction for the normal
	let e = 0.15;
	var face = 0.0;
	var roller = 0.0;
	let s0 = shoreShape( u, A, dB, lam );

	var disp = vec3f( dir.x * s0.x, s0.y, dir.y * s0.x ) * env;

	var nShore = vec3f( 0.0, 1.0, 0.0 );


	// ---- swash: run-up of the most recent wave on the sand
	let swr = shoreSwashRunup( sh, along, groundH );
	let front = swr.Rt - swr.inland; // signed distance to the leading edge (m), > 0 under the sheet
	let covered = front > 0.0;
	// leading edge velocity along the slope (m/s), positive = uphill
	let dRdt = select( pow( max( swr.sb, 1e-3 ), 0.6 ) * ( - 1.6 / SHORE_SWASH_DOWN ), pow( 1.0 - swr.su, 0.5 ) * ( 1.5 / SHORE_SWASH_UP ), swr.isUp ) * swr.RhMax / Tp;
	// thin sheet (a few cm, thickening behind the leading edge). The mesh sheet overshoots the
	// leading edge a little; the water shader cuts it exactly on the front (swashClip), so the edge
	// doesn't follow the mesh triangles.
	let fm = front + SHORE_SWASH_OVERSHOOT;
	let thick = clamp( min( fm * 0.3, max( fm - 0.1, 0.0 ) * ( SHORE_BEACH_SLOPE * 0.22 ) + 0.03 ), -0.1, 0.12 );
	let swashLevel = groundH + thick;
	// bubbly foam line riding the leading edge all the way up (left behind as the swash mark)
	let uprush = smoothstep( 0.46, 0.32, swr.tau );
	let edge = smoothstep( 0.8, 0.0, front ) * smoothstep( -0.05, 0.05, front ) * uprush * smoothstep( 0.0, 1.0, swr.Rt );
	let swashFoam = edge * 0.9;

	// ---- depth-averaged water velocity (along dir), for foam advection
	// waves / bores: shallow-water particle velocity c * eta / h; swash sheet: the tip moves at
	// dR/dt, slower toward the shoreline during uprush, faster there while it drains
	let eta = disp.y;
	let uWave = clamp( c * eta / ( max( d, 0.0 ) + max( eta, d * -0.8 ) + 0.15 ), -2.5, 4.0 );
	let rel = sat( swr.inland / max( swr.Rt, 0.5 ) );
	let uSwash = dRdt * select( ( 1.0 - rel ) * 0.6 + 0.9, clamp( ( swr.inland + 3.0 ) / ( swr.Rt + 3.0 ), 0.15, 1.0 ), swr.isUp );
	let wSwash = smoothstep( 0.3, 0.05, d );
	let uFlow = mix( uWave, uSwash, wSwash ) * select( 0.0, 1.0, covered || d > 0.02 );

	var o: ShoreSample;
	// the churn of a bore is uneven along the crest: dense in some stretches, torn into patches and
	// lace in others (different for every wave, drifting slowly along it)
	var wwPatch = 0.0;
	if ( s0.z * env != 0.0 ) { wwPatch = smoothstep( -0.5, 0.45, perlin2( vec2f( along * 0.06 + tw.sea.y * 0.05, m * 2.9 + 0.4 ) ) ); }
	o.disp = disp; o.nShore = nShore; o.env = env; o.foam = s0.z * env * mix( 0.3, 1.0, wwPatch ); o.breaking = s0.w; o.u = u; o.dir = dir;
	o.exposure = exposure; o.swashLevel = swashLevel; o.swashCovered = select( 0.0, 1.0, covered ); o.thick = thick;
	o.swashFoam = swashFoam; o.runup = swr.Rt; o.inland = swr.inland; o.dRdt = dRdt; o.tau = swr.tau;
	o.flow = dir * uFlow; o.flowSpeed = uFlow; o.face = face; o.roller = roller;
	return o;
}


fn shoreEvaluateWorld( xz: vec2f, depth: f32, groundH: f32 ) -> ShoreSample {
	let ph = shorePhaseAt( xz );
	let sh = ph.sh;
	let exposure = ph.exposure;
	let along = ph.along;
	let dir = ph.dir;
	let Tp = tw.shore0.x;

	let d = depth;
	let c = sqrt( clamp( d, 0.3, 25.0 ) * SHORE_GRAVITY );
	let lam = c * Tp;

	let s = ph.s;
	let m = floor( s + 0.5 );
	let u = s - m;

	// wave height with smooth hand-over between consecutive waves at the trough
	let A0 = shoreWaveAmp( m, along );
	let An = shoreWaveAmp( m + sign( u ), along );
	let A = mix( A0, An, smoothstep( 0.32, 0.5, abs( u ) ) * 0.5 );
	// the breaking state of the whole wave comes from the depth under its crest
	let dB = shoreBreakDepth( xz, dir, u, lam, d );

	// offshore fade-in (FFT covers deep water) and fade on land (the swash sheet takes over there)
	let env = smoothstep( 26.0, 13.0, d ) * smoothstep( -0.25, 0.05, d ) * sat( exposure * 1.4 ) * tw.shore1.w;

	// finite difference along the propagation direction for the normal
	let e = 0.15;
	var face = 0.0;
	var roller = 0.0;
	let r = shoreWorld( u, A, dB, lam, env );
	let s0 = vec4f( r.z, r.y, r.x, r.w );

	var disp = vec3f( dir.x * s0.x, s0.y, dir.y * s0.x ) * env;

	var nShore = vec3f( 0.0, 1.0, 0.0 );


	// ---- swash: run-up of the most recent wave on the sand
	let swr = shoreSwashRunup( sh, along, groundH );
	let front = swr.Rt - swr.inland; // signed distance to the leading edge (m), > 0 under the sheet
	let covered = front > 0.0;
	// leading edge velocity along the slope (m/s), positive = uphill
	let dRdt = select( pow( max( swr.sb, 1e-3 ), 0.6 ) * ( - 1.6 / SHORE_SWASH_DOWN ), pow( 1.0 - swr.su, 0.5 ) * ( 1.5 / SHORE_SWASH_UP ), swr.isUp ) * swr.RhMax / Tp;
	// thin sheet (a few cm, thickening behind the leading edge). The mesh sheet overshoots the
	// leading edge a little; the water shader cuts it exactly on the front (swashClip), so the edge
	// doesn't follow the mesh triangles.
	let fm = front + SHORE_SWASH_OVERSHOOT;
	let thick = clamp( min( fm * 0.3, max( fm - 0.1, 0.0 ) * ( SHORE_BEACH_SLOPE * 0.22 ) + 0.03 ), -0.1, 0.12 );
	let swashLevel = groundH + thick;
	// bubbly foam line riding the leading edge all the way up (left behind as the swash mark)
	let uprush = smoothstep( 0.46, 0.32, swr.tau );
	let edge = smoothstep( 0.8, 0.0, front ) * smoothstep( -0.05, 0.05, front ) * uprush * smoothstep( 0.0, 1.0, swr.Rt );
	let swashFoam = edge * 0.9;

	// ---- depth-averaged water velocity (along dir), for foam advection
	// waves / bores: shallow-water particle velocity c * eta / h; swash sheet: the tip moves at
	// dR/dt, slower toward the shoreline during uprush, faster there while it drains
	let eta = disp.y;
	let uWave = clamp( c * eta / ( max( d, 0.0 ) + max( eta, d * -0.8 ) + 0.15 ), -2.5, 4.0 );
	let rel = sat( swr.inland / max( swr.Rt, 0.5 ) );
	let uSwash = dRdt * select( ( 1.0 - rel ) * 0.6 + 0.9, clamp( ( swr.inland + 3.0 ) / ( swr.Rt + 3.0 ), 0.15, 1.0 ), swr.isUp );
	let wSwash = smoothstep( 0.3, 0.05, d );
	let uFlow = mix( uWave, uSwash, wSwash ) * select( 0.0, 1.0, covered || d > 0.02 );

	var o: ShoreSample;
	// the churn of a bore is uneven along the crest: dense in some stretches, torn into patches and
	// lace in others (different for every wave, drifting slowly along it)
	var wwPatch = 0.0;
	if ( s0.z * env != 0.0 ) { wwPatch = smoothstep( -0.5, 0.45, perlin2( vec2f( along * 0.06 + tw.sea.y * 0.05, m * 2.9 + 0.4 ) ) ); }
	o.disp = disp; o.nShore = nShore; o.env = env; o.foam = s0.z * env * mix( 0.3, 1.0, wwPatch ); o.breaking = s0.w; o.u = u; o.dir = dir;
	o.exposure = exposure; o.swashLevel = swashLevel; o.swashCovered = select( 0.0, 1.0, covered ); o.thick = thick;
	o.swashFoam = swashFoam; o.runup = swr.Rt; o.inland = swr.inland; o.dRdt = dRdt; o.tau = swr.tau;
	o.flow = dir * uFlow; o.flowSpeed = uFlow; o.face = face; o.roller = roller;
	return o;
}

