// Unlit textured 3D, plain (vs_main) and GPU-skinned (vs_skinned) variants
// sharing fs_main. The two group(1) declarations coexist because each entry
// point statically uses only one (binding collisions are validated per entry
// point): the plain pipeline binds `object`, the skinned pipeline binds the
// bone `palette` (from the skin module). Shadowing is the shared cascade kernel.

#import frame::{frame, reverse_z, fog_factor, terrain_sun_shadow, apply_fog, veg_sway_offset, matdbg_flag, MATDBG_DISABLE_FRESNEL_ENV}
#import shadow::shadow_strength
#import skin::{skin_pos, skin_normal}
#import lighting::lights_contrib
#import color::srgb_to_linear
#import gbuffer::{oct_encode, a2c_coverage, mip_alpha_boost}
// Shared fragment shading, also used by the GPU-driven indirect path (gpu_driven.wgsl).
#import shading::{shade, ShadeMaterial}
// Group(4) terrain heightmap + surface_y, shared with the shadow depth pass.
// surface_grad tilts conformed veg normals to follow the ground slope (color pass only).
#import conform::{surface_y, surface_grad, surface_y_raster}

struct Object {
    world: mat4x4<f32>,
    // Terrain-conform plane, published per instance by the CPU (ForestPlain).
    // When conform2.z (mode) > 0 the vertex shader conforms this instance to the ground
    // exactly like ForestPlain::Animate's two-triangle bilinear fit, so the shared
    // forest mesh can be uploaded ONCE undeformed instead of rewritten per instance.
    conform0: vec4<f32>,   // inv_land_grid, -xf, -zf, bias(=BoundingCenter().y)
    conform1: vec4<f32>,   // y00, y10, d1000, d0100
    conform2: vec4<f32>,   // d1011, d0111, mode(0=none,1=forest), _pad
};

// Per-draw material lighting, folded on the CPU exactly like GL33's
// UploadVSMaterialConstants (raw sun colour x material, sun-enable already in the
// sun terms). Bound at group(1)/binding(1) for BOTH the plain and skinned
// pipelines — binding(0) is `object` (plain) or the skin module's `palette`
// (skinned), so the material coexists with either. Only rgb is read.
struct Material {
    emissive: vec4<f32>,
    sun_ambient: vec4<f32>,
    sun_diffuse: vec4<f32>,
    // Modulation for the frame-global point/spot lights (GL33's matDif/matAmb).
    light_diffuse: vec4<f32>,
    light_ambient: vec4<f32>,
    // Sun-only Blinn-Phong highlight (GL33's c18): rgb = sun diffuse x material
    // specular (sun-enable folded in), w = power. Added per-fragment when w > 0.
    specular: vec4<f32>,
    // x = bindless Stage1 NOHQ texture/sampler, y = MaterialDebug View,
    // z = Stage1 enabled. NOHQ is decoded strictly as X=A, Y=G, Z reconstructed.
    // w is a small BITFIELD (see MATFLAG_* below), not a bool: WgrDraw3D is ABI-frozen
    // and the other six vec4s here are fully spoken for.
    normal_map: vec4<f32>,
};

// Material.normal_map.w bits.
const MATFLAG_INVERT_NORMAL_Y: u32 = 1u;
// DZ-003: this draw's material declared itself reflective (WGR_DRAW3D_REFLECTIVE) --
// DayZ's CalmWater family, and nothing else.
const MATFLAG_REFLECTIVE: u32 = 2u;
// WGR_DRAW3D_NIGHT_EMITTER: a recognised light fixture, exempt from the emissive_night fade.
const MATFLAG_NIGHT_EMITTER: u32 = 4u;

fn material_flags(m: Material) -> u32 {
    return u32(m.normal_map.w + 0.5);
}

// Per-draw world matrix + material as read-only storage arrays, indexed by
// @builtin(instance_index) (fed as the draw's base_instance). One upload per
// frame, group(1) bound once — no per-draw dynamic offsets. The skinned pipeline
// binds the bone `palette` (from the skin module) at binding(0) instead of
// `objects`; each entry point statically uses only one, so both coexist.
@group(1) @binding(0) var<storage, read> objects: array<Object>;   // plain pipeline
@group(1) @binding(1) var<storage, read> materials: array<Material>;
// DZ-003 -- Sky's reflection environment map (equirect, LINEAR radiance) and its sampler
// (U wraps across the azimuth seam, V clamps at the poles). The same view water reflects.
// Sampled only by draws flagged MATFLAG_REFLECTIVE; a 1x1 dummy is bound until Sky lends
// the real view, so the binding is always valid.
@group(1) @binding(2) var sky_env: texture_2d<f32>;
@group(1) @binding(3) var sky_env_samp: sampler;

// Equirect lookup into the sky reflection env map. Copied from water.wgsl's sky_env_sample
// so both consumers agree with fs_sky_env's convention in sky.wgsl: u = azimuth
// (atan2(z, x)/2pi + 0.5, U-wrapped), v = 0 at zenith .. 1 at nadir (acos(y)/pi). `dir` is
// a world-space direction; the result is LINEAR radiance.
const ENV_TWO_PI: f32 = 6.28318530718;
fn sky_env_sample(dir: vec3<f32>) -> vec3<f32> {
    let u = 0.5 + atan2(dir.z, dir.x) / ENV_TWO_PI;
    let v = acos(clamp(dir.y, -1.0, 1.0)) / (ENV_TWO_PI * 0.5);
    return textureSampleLevel(sky_env, sky_env_samp, vec2<f32>(u, v), 0.0).rgb;
}

// Schlick with the air/water normal-incidence reflectance; see gpu_driven.wgsl's twin.
const ENV_F0: f32 = 0.02;
fn env_fresnel(cos_view_normal: f32) -> f32 {
    let c = clamp(1.0 - cos_view_normal, 0.0, 1.0);
    let c2 = c * c;
    return ENV_F0 + (1.0 - ENV_F0) * (c2 * c2 * c);
}

// DZ-005 -- the world-space water ripple. Verbatim twin of gpu_driven.wgsl's copy, which
// carries the full derivation; the short version is that DayZ water cannot be normal-mapped
// through its mesh uv (all 185 Enoch pond meshes carry UV (0,0) at every vertex; the 804 river
// meshes are ~360x anisotropic), and its `.emat` addresses the map in WORLD space instead --
// `NormalMapping 0.1` is one tile per 10 m.
const WATER_RIPPLE_PERIOD_A: f32 = 10.0;
const WATER_RIPPLE_PERIOD_B: f32 = 3.3;
const WATER_RIPPLE_POWER_A: f32 = 1.55;
const WATER_RIPPLE_POWER_B: f32 = 0.80;

// The still-water drift; twin of gpu_driven.wgsl's, which carries the reasoning. Two octaves
// crawling in different directions at different speeds, slowly -- a pond ripples, it does not
// flow. Negative because sampling at `uv - d` shifts the pattern by `+d`.
const WATER_DRIFT_DIR_A: vec2<f32> = vec2<f32>(0.8, 0.6);
const WATER_DRIFT_DIR_B: vec2<f32> = vec2<f32>(-0.5547, 0.8321);
const WATER_DRIFT_SPEED_A: f32 = 0.12;
const WATER_DRIFT_SPEED_B: f32 = 0.075;
fn water_drift(dir: vec2<f32>, speed: f32, period: f32, t: f32) -> vec2<f32> {
    return -dir * (speed * t / period);
}

// One octave, returned as a world-xz slope. `samplers[0]` is linear + REPEAT on both axes; the
// material's own sampler may clamp, which on a world-space uv smears one edge texel across a
// whole lake. textureSampleGrad takes the world-position derivatives from the caller so the
// call stays legal inside the reflective branch and still picks a correct mip.
//
// The decode is s.rg, NOT the OFP NOHQ (X in alpha, Y in green) convention used above: these
// maps are DXT1 with a constant 255 alpha and a unit normal in RGB (measured on
// enoch_pond_nohq.edds / enoch_river_nohq.edds, which are byte-identical).
fn water_octave(slot: u32, xz: vec2<f32>, cam_xz: vec2<f32>, dxz_dx: vec2<f32>, dxz_dy: vec2<f32>,
                period: f32, drift: vec2<f32>) -> vec2<f32> {
    let inv = 1.0 / period;
    // fract() on the camera's share keeps the uv inside f32's useful mantissa at 12.8 km out.
    let uv = xz * inv + fract(cam_xz * inv) + drift;
    let s = textureSampleGrad(textures[slot], samplers[0], uv, dxz_dx * inv, dxz_dy * inv);
    return s.rg * 2.0 - vec2<f32>(1.0);
}

// The rippled shading normal for a reflective (CalmWater) fragment. `drift` is the per-octave
// uv scroll -- zero for a still surface, the flow term for a river.
fn water_ripple_normal(slot: u32, geometric: vec3<f32>, world_pos: vec3<f32>,
                       dwx: vec3<f32>, dwy: vec3<f32>, drift_a: vec2<f32>, drift_b: vec2<f32>,
                       strength: f32) -> vec3<f32> {
    let xz = vec2<f32>(world_pos.x, world_pos.z);
    let cam_xz = vec2<f32>(frame.cam_pos.x, frame.cam_pos.z);
    let dxz_dx = vec2<f32>(dwx.x, dwx.z);
    let dxz_dy = vec2<f32>(dwy.x, dwy.z);
    var slope = water_octave(slot, xz, cam_xz, dxz_dx, dxz_dy, WATER_RIPPLE_PERIOD_A, drift_a)
                * WATER_RIPPLE_POWER_A;
    slope = slope + water_octave(slot, xz, cam_xz, dxz_dx, dxz_dy, WATER_RIPPLE_PERIOD_B, drift_b)
                    * WATER_RIPPLE_POWER_B;
    slope = slope * strength;
    // Horizontal quads, so the map's tangent X/Y are world X/Z; water.wgsl's convention turns
    // an xz slope into normalize(vec3(-sx, 1, -sy)). The up axis keeps the geometric sign.
    let up = select(-1.0, 1.0, geometric.y >= 0.0);
    return normalize(vec3<f32>(-slope.x, up, -slope.y));
}

// Fresnel-weighted sky reflection. `world_pos` is CAMERA-RELATIVE (camera at the origin),
// `fog` is frame::fog_factor (1 = clear). Verbatim twin of gpu_driven.wgsl's env_reflection
// -- the two paths must not disagree about what a reflective surface looks like.
fn env_reflection(shaded: vec3<f32>, normal: vec3<f32>, world_pos: vec3<f32>, fog: f32) -> vec3<f32> {
    let view_dir = normalize(-world_pos);
    // Face the normal at the viewer first: a quad whose winding puts its normal downward
    // would otherwise reflect into the nadir and read as a dark blotch.
    let n = select(-normal, normal, dot(normal, view_dir) >= 0.0);
    let cos_view_normal = clamp(dot(n, view_dir), 0.0, 1.0);
    let env = sky_env_sample(reflect(-view_dir, n));
    return mix(shaded, env, env_fresnel(cos_view_normal) * clamp(fog, 0.0, 1.0));
}
// Bindless object textures + the 8-variant sampler array, bound once for the whole
// lit-mesh + prepass. Each draw's texture/sampler index is packed per-instance into
// material.emissive.w (docs/bindless-textures-plan.md): (tex_slot << 3) | sampler.
@group(2) @binding(0) var textures: binding_array<texture_2d<f32>>;
@group(3) @binding(0) var samplers: binding_array<sampler, 8>;

// Baked per-pipeline (pipeline-overridable constants): the alpha-test cutout
// threshold and whether this is a shadow-darken pipeline. Keeping them out of a
// per-draw binding avoids a 5th bind group (wgpu's default maxBindGroups is 4).
override alpha_ref: f32 = 0.0;   // discard fragments with alpha below this (0 = off)
override is_shadow: f32 = 0.0;   // 1 = output black + shadow-strength alpha
// 1 = this cutout pipeline uses alpha-to-coverage (MSAA foliage): the fragment emits a
// sharpened coverage alpha instead of a hard discard, and the pipeline has
// alpha_to_coverage_enabled. Set only on cutout (alpha_ref > 0), non-shadow pipelines under
// MSAA; the prepass twin (fs_prepass_a2c) emits the same coverage so the masks match.
override a2c: f32 = 0.0;
// 1 = an alpha-blended (glass) pipeline: the shared shading damps its diffuse sky-irradiance
// ambient so transparent cockpit canopies don't wash out to the sky (and crash auto-exposure).
override translucent: f32 = 0.0;
// Brightness a fully terrain-shadowed alpha-tested surface (foliage cutout) keeps.
// Dense canopy self-occludes its sky ambient — which the world-space terrain mask
// can't model and foliage materials inflate for the sunlit look — so shadowed
// leaves stay too bright under the ambient-preserving model that suits solid
// ground/decals. This extra multiply darkens only alpha-tested foliage in terrain
// shadow toward the close-up CSM look. 1 = off (no extra darkening).
override foliage_shadow_ao: f32 = 0.35;
// Decal/overlay depth bias in reversed-NDC depth units, pulling the draw toward the
// camera. Applied in the vertex shader (not DepthBiasState, which is a no-op on the
// float depth format this backend gets) so roads/decals/overlays win the depth test
// against coplanar geometry.
override depth_bias: f32 = 0.0;
// HDR path (docs/hdr-pipeline-plan.md): 1 = decode the sampled albedo + folded
// material/light colours from sRGB to linear and drop the [0,1] radiance clamp, so
// shading writes linear radiance (that can exceed 1.0) into the HDR target. 0 = the
// exact gamma-naive GL33 behaviour for the LDR-direct path.
override linear: f32 = 0.0;
// Fraction of a draw's MATERIAL EMISSIVE that survives at full night. 1 = the shipped
// behaviour (emissive is absolute radiance, unaffected by time of day) and the default.
//
// WHY IT EXISTS (owner report 2026-08-16, Arma 2 Takistan, hour 0 vs hour 12, capture
// .tmp-shadow/owner/road-night/): the road reads (40.6, 41.0, 44.5) at midnight while the
// terrain one metre beside it reads (0.26, 0.22, 0.09) -- ~195x. By DAY the same road is
// correctly DARKER than the sand (186.9 vs 233.7), so this is not albedo and not the
// texture. Terrain and objects share one ambient expression (terrain.wgsl:852 and
// shading.wgsl:176 are the same `sky_irradiance(n) * sun_ambient.w`), so the only term an
// object can carry that terrain cannot is `m_emissive` -- which shade() adds as absolute
// radiance and nothing scales by the day/night level. Note the road's night brightness still
// falls with distance (40.6 -> 3.8), i.e. fog IS applied: it is specifically the sun/night
// factor that is missing, which is exactly the shape of an unattenuated emissive.
//
// A lit window or a lamp glass SHOULD stay lit at night, and this constant cannot tell one
// from the other -- so the CPU does: EngineWgpu's MaterialEmitsAtNight marks recognised
// fixture families with WGR_DRAW3D_NIGHT_EMITTER (bit 2 of material.normal_map.w) and those
// are exempt. Everything else fades with the sun. Default 0 since 2026-08-16: with 1.0 the
// forests self-illuminated at midnight on Arma 2, Arma 3 and DayZ (Takistan hour 0 with
// WGR_AUTO_EXPOSURE=0: dead trees ~150/255, canopy ~60, terrain 0.2 -- exposure exonerated).
// WGR_EMISSIVE_NIGHT=1 is the A/B back to the old behaviour.
override emissive_night: f32 = 0.0;
// Per-mip alpha gain for cutout foliage (WGR_CUTOUT_MIP_ALPHA); see gbuffer.wgsl
// mip_alpha_boost and gpu_driven.wgsl's twin.
override cutout_mip_alpha: f32 = 0.0;

struct VsOut {
    @builtin(position) clip: vec4<f32>,
    @location(0) uv: vec2<f32>,
    @location(1) fog: f32,             // 1 = keep colour, 0 = full fog
    @location(2) world_pos: vec3<f32>, // camera-relative
    @location(3) normal: vec3<f32>,    // world space, outward
    // Draw slot, carried flat so fs_main can index its material. Equals the draw's
    // base_instance (see draw_one); both vertex paths pass it through.
    @location(4) @interpolate(flat) instance: u32,
    // Authored ODOL tangent/binormal frame when supplied, otherwise zero. Kept
    // separate from the normal so fs_main can retain its safe derivative fallback.
    @location(5) tangent: vec3<f32>,
    @location(6) binormal: vec3<f32>,
};

// world_pos is camera-relative (the world matrix / palette is offset by the
// camera position on the C++ side), so its length is the camera distance.
fn finish_vertex(
    world_pos: vec4<f32>, normal_ws: vec3<f32>, tangent_ws: vec3<f32>, binormal_ws: vec3<f32>,
    uv: vec2<f32>, instance: u32,
) -> VsOut {
    var out: VsOut;
    out.clip = reverse_z(frame.proj * frame.view * world_pos);
    // Bias toward the camera (larger reversed depth) so decals/overlays win the
    // depth test against coplanar geometry.
    //
    // SCALED BY DEPTH SQUARED, not constant. This replaced glPolygonOffset(-1,-1),
    // which is denominated in RESOLVABLE DEPTH UNITS AT THE FRAGMENT and therefore
    // self-scales with distance; a constant offset in NDC does not, and reversed-Z NDC
    // is wildly non-linear in distance (stored depth is near/z for this
    // forward-infinite-far projection). A constant 1e-5 pulls a surface toward the
    // camera by z^2 * bias / near: about 1 m at 100 m, but ~95 m at 1 km and ~716 m at
    // 3 km. That is why a road could be seen THROUGH a mountain ridge - it was being
    // depth-tested as though it stood hundreds of metres in front of itself, and
    // roads carry NoZWrite so the ridge could not paint back over them.
    //
    // ndc^2 undoes exactly that: with depth = near/z, holding (bias * ndc^2) constant in
    // NDC is holding a constant WORLD-SPACE pull, which is what the GL call meant. Near
    // geometry is unaffected (ndc ~ 1 there, so this matches the old value), and the
    // far-field error disappears.
    //
    // Not env-gated, unlike most changes on this branch: the constant-NDC form is not a
    // behaviour worth preserving, it is a porting error with a measured failure mode.
    // WGR_DECAL_SCALE still tunes the magnitude, and near geometry is unchanged.
    let ndc_z = select(1.0, out.clip.z / out.clip.w, out.clip.w != 0.0);
    out.clip.z += depth_bias * ndc_z * ndc_z * out.clip.w;
    out.uv = uv;
    out.world_pos = world_pos.xyz;
    out.normal = normal_ws;
    out.tangent = tangent_ws;
    out.binormal = binormal_ws;
    out.fog = fog_factor(length(world_pos.xyz));
    out.instance = instance;
    return out;
}

@vertex
fn vs_main(
    @builtin(instance_index) instance: u32,
    @location(0) pos: vec3<f32>,
    @location(1) norm: vec3<f32>,
    @location(2) uv: vec2<f32>,
    @location(5) conform_sel: u32,   // per-vertex conform selector (mode 2): 0/1/2
    @location(6) tangent: vec3<f32>,
    @location(7) binormal: vec3<f32>,
) -> VsOut {
    let obj = objects[instance];
    let world = obj.world;
    var world_pos = world * vec4<f32>(pos, 1.0);
    // Vertex normals arrive already negated (MeshBuild::BuildVertices stores
    // -Norm, the D3D convention). GL33 rotates that stored normal to world space
    // and lights with it as-is (EngineGL33_Shaders VSNormal: `mat3(world)*normal`,
    // no extra negation), so do the same here — negating again would flip N and
    // invert the diffuse/specular/local-light terms relative to the sun.
    let rot = mat3x3<f32>(world[0].xyz, world[1].xyz, world[2].xyz);
    var normal_ws = rot * norm;
    // Terrain conform: the shared base mesh is uploaded undeformed and conformed here
    // per instance. world_pos is camera-relative, so heights are evaluated in ABSOLUTE
    // world xz (+ frame.cam_pos) and written back camera-relative. conform2.z = mode:
    // 1 = ForestPlain bilinear plane, 2 = per-vertex ClipLand vegetation (heightmap).
    let mode = obj.conform2.z;
    if (mode > 1.5) {
        // Mode 2: individual ClipLand vegetation, conformed per vertex to SurfaceY,
        // matching Object::Animate (Object.cpp:395-423). conform_sel: 1 = ClipLandKeep
        // (keep height above the surface), 2 = ClipLandOn (pin onto it), 0 = rigid.
        let abs_x = world_pos.x + frame.cam_pos.x;
        let abs_z = world_pos.z + frame.cam_pos.z;
        let abs_xz = vec2<f32>(abs_x, abs_z);
        let sy = surface_y(abs_xz);
        if (conform_sel == 1u) {
            // world.y_abs = SurfaceY + undeformedWorldY - bcSurfaceY (conform0.x); the
            // cam.y offset cancels between the two camera-relative terms.
            //
            // ClipLandKeep deliberately keeps the FINE surface even under WGR_CONFORM_RASTER:
            // this vertex sits a fixed height ABOVE the ground, where a chord error of a few
            // decimetres is invisible, and the raster surface is camera-distance dependent --
            // a plant tracking it would visibly rise and sink as the camera walked. The
            // coplanar depth-fight the raster surface exists to fix is a ClipLandOn problem.
            world_pos.y = sy + world_pos.y - obj.conform0.x;
        } else if (conform_sel == 2u) {
            // WLD-023(c): pin to the surface the terrain RASTERISES, not the one it
            // evaluates, so a road cannot sit above the coarse chord of the ridge in front
            // of it. Returns surface_y exactly when WGR_CONFORM_RASTER is unset (default).
            world_pos.y = surface_y_raster(abs_xz, frame.cam_pos.xyz) - frame.cam_pos.y;
        }
        // Tilt the conformed vertex's normal by the terrain slope (inverse-transpose of
        // the vertical height-field shear, same as mode 1) so lighting follows the ground
        // the CPU's InvalidateNormals would have produced. Rigid verts (sel 0) keep theirs.
        //
        // This stays on the FINE gradient under WGR_CONFORM_RASTER, deliberately: terrain
        // shades from the fine field too, per-fragment and explicitly LOD/morph-independent
        // (terrain.wgsl:112-123 — a mesh-derived normal banded the lighting into radial
        // stripes). Matching the drawn POSITION is a depth question; matching the drawn
        // NORMAL would reintroduce the banding that comment records.
        if (conform_sel != 0u) {
            let g = surface_grad(vec2<f32>(abs_x, abs_z));
            normal_ws = vec3<f32>(normal_ws.x - g.x * normal_ws.y, normal_ws.y,
                                  normal_ws.z - g.y * normal_ws.y);
        }
    } else if (mode > 0.5) {
        // Mode 1: ForestPlain bilinear plane fit (ObjectClasses.cpp:571-605).
        let s = obj.conform0.x;                          // inv_land_grid
        let xIn = (world_pos.x + frame.cam_pos.x) * s + obj.conform0.y;  // *invLand - xf
        let zIn = (world_pos.z + frame.cam_pos.z) * s + obj.conform0.z;  // *invLand - zf
        let y00 = obj.conform1.x; let y10 = obj.conform1.y;
        let d1000 = obj.conform1.z; let d0100 = obj.conform1.w;
        let d1011 = obj.conform2.x; let d0111 = obj.conform2.y;
        let triA = xIn <= 1.0 - zIn;
        let py = select(y10 + d0111 - d1011 * xIn - zIn * d0111,
                        y00 + d1000 * zIn + d0100 * xIn,
                        triA);
        // Camera-relative conformed height: absolute plane height + the vertex's own
        // model height above surface (conform0.w = BoundingCenter().y), minus cam.y.
        world_pos.y = py - frame.cam_pos.y + pos.y + obj.conform0.w;
        // Tilt the undeformed normal by the plane gradient (inverse-transpose of the
        // affine y-shear) so lighting matches the CPU's post-deform InvalidateNormals.
        let gx = select(-d1011, d0100, triA) * s;
        let gz = select(-d0111, d1000, triA) * s;
        normal_ws = vec3<f32>(normal_ws.x - gx * normal_ws.y, normal_ws.y, normal_ws.z - gz * normal_ws.y);
    }
    // VEG-SWAY, the direct-path twin of gpu_driven::vs_gpu. Applied AFTER conform for the same
    // reason: conform samples terrain height at the vertex's XZ, so swaying first would make the
    // plant ride the ground height of wherever the wind had just pushed it.
    //
    // The vegetation gate is `sun_ambient.w` (1 = bush, 2 = tree/forest) — the direct path's
    // equivalent of the retained path's INST_CANOPY_* instance flags. Unlike the canopy-normal
    // block below, sway is NOT restricted to alpha-cut sections: a trunk has to lean with its
    // own crown or the tree tears apart at the bark line.
    let foliage_kind = materials[instance].sun_ambient.w;
    if (foliage_kind > 0.5 && frame.foliaged.x > 0.0) {
        // world[3] is the object origin, already camera-relative on this path (the C++ side
        // offsets the world matrix by the camera position), so the camera position is added
        // back to seed the phase from an ABSOLUTE world XZ. Seeding from the camera-relative
        // value would make every plant swim as the camera walks.
        let phase_xz = world[3].xz + frame.cam_pos.xz;
        let leaf = select(0.0, 1.0, alpha_ref > 0.0);
        let sway = veg_sway_offset(pos.y, phase_xz, leaf);
        world_pos = vec4<f32>(world_pos.xyz + sway, world_pos.w);
    }
    // REGRESSION GUARD (CWA stock-tree canopies): legacy leaf cards have arbitrary authored
    // normals, so a visible card can point sideways or downward and receive almost no daylight
    // or directional sky. Without this direct-path radial-crown correction, standard CWA trees
    // such as `data3d\str buk.p3d` regress to large black canopy slabs in daylight, even though
    // their PAC texture and alpha cutout are correct. The retained/GPU-driven path already bends
    // cutout canopy normals this way; keep this direct-path equivalent. sun_ambient.w carries
    // 1=bush or 2=tree/forest, so only alpha-cut vegetation changes.
    if (alpha_ref > 0.0 && foliage_kind > 0.5) {
        let is_tree = foliage_kind > 1.5;
        let crown_y = select(frame.foliageb.z, frame.foliagec.z, is_tree);
        let bend = select(frame.foliageb.y, frame.foliagec.y, is_tree);
        let crown_center = world[3].xyz + vec3<f32>(0.0, crown_y, 0.0);
        let radial = world_pos.xyz - crown_center;
        if (dot(radial, radial) > 1e-6) {
            normal_ws = normalize(mix(normal_ws, normalize(radial), clamp(bend, 0.0, 1.0)));
        }
    }
    return finish_vertex(world_pos, normal_ws, rot * tangent, rot * binormal, uv, instance);
}

// Linear-blend skinning (see the skin module). Vertices with no skin weight
// carry a single weight of 1.0 on a reserved bone whose palette entry is just
// `world`, so no zero-weight fallback is needed.
@vertex
fn vs_skinned(
    @builtin(instance_index) instance: u32,
    @location(0) pos: vec3<f32>,
    @location(1) norm: vec3<f32>,
    @location(2) uv: vec2<f32>,
    @location(3) bones: vec4<u32>,   // Uint8x4: palette indices
    @location(4) weights: vec4<f32>, // Unorm8x4: normalised weights
) -> VsOut {
    let world_pos = skin_pos(pos, bones, weights);
    // As in vs_main: `norm` is the already-negated stored normal (SetSkinData
    // uploads -OrigNorm), skin_normal rotates it into world space, and we light
    // with it as-is to match GL33 — no extra negation.
    let normal_ws = skin_normal(norm, bones, weights);
    return finish_vertex(world_pos, normal_ws, vec3<f32>(0.0), vec3<f32>(0.0), uv, instance);
}

@fragment
fn fs_main(in: VsOut) -> @location(0) vec4<f32> {
    // Derivatives must run in uniform control flow — before the discard.
    let dwx = dpdx(in.world_pos);
    let dwy = dpdy(in.world_pos);
    // The reflected camera carries an absolute-world water clip plane in Frame.
    // Terrain already honours it; apply the same plane to retained/skinned objects
    // so submerged geometry cannot leak into or occlude the planar reflection.
    // Main cameras upload a zero plane, making this branch compile to a cheap no-op.
    let clip_len2 = dot(frame.clip_plane.xyz, frame.clip_plane.xyz);
    if (clip_len2 > 0.0 &&
        dot(frame.clip_plane.xyz, in.world_pos + frame.cam_pos.xyz) + frame.clip_plane.w < 0.0) {
        discard;
    }
    // Per-draw material for this draw slot (base_instance == draw slot). Its emissive.w
    // packs the bindless texture + sampler indices ((tex_slot << 3) | sampler); the index
    // is uniform across a derivative quad (one instance per primitive), so implicit-mip
    // textureSample stays legal.
    let material = materials[in.instance];
    let packed = bitcast<u32>(material.emissive.w);
    let normal_packed = bitcast<u32>(material.normal_map.x);

    // Parallax occlusion mapping, carried in the normal map's RED channel.
    //
    // R is free and it is free by MEASUREMENT, not by hope: every shipped Arma NOHQ in this
    // programme's corpus has R constant 0 (40 of 40), because the format stores X in alpha and Y in
    // green and reconstructs Z. So a stock texture yields height 0, the loop below finds no
    // displacement, and the offset is exactly zero -- this is self-gating and needs no uniform, no
    // ABI change and no extra texture binding. An enhanced texture carries height there and gets
    // relief. Both sample the SAME already-bound normal map.
    //
    // Height taps use textureSampleLevel: the march is per-pixel control flow, and implicit-mip
    // sampling inside non-uniform flow is undefined. The final base/normal samples below stay
    // implicit-mip and unconditional, so their derivatives remain valid.
    var uv = in.uv;
    // NOT on alpha-tested surfaces. The parallax offset moves the uv the BASE colour is sampled
    // at, and the cutout test below runs on that sample's alpha -- so on a cutout texture the
    // shifted uv can land in a transparent region and discard the pixel, punching see-through
    // holes in vehicles and foliage. Displacing a surface whose silhouette is defined by its alpha
    // is wrong in any case: the hole would move with the view while its edge stayed put.
    // alpha_ref is the existing cutout threshold, so this costs nothing to ask.
    if (material.normal_map.z > 0.5 && alpha_ref <= 0.0) {
        // Tangent frame for the view vector, derived the same way the shading frame below is, so
        // the displacement runs along the surface's real UV directions.
        let duv_dx = dpdx(in.uv);
        let duv_dy = dpdy(in.uv);
        let n_geo = normalize(in.normal);
        var t_raw = dwx * duv_dy.y - dwy * duv_dx.y;
        var b_raw = dwy * duv_dx.x - dwx * duv_dy.x;
        if (dot(in.tangent, in.tangent) > 1e-10 && dot(in.binormal, in.binormal) > 1e-10) {
            t_raw = in.tangent;
            b_raw = in.binormal;
        }
        let t_ortho = t_raw - n_geo * dot(n_geo, t_raw);
        if (dot(t_ortho, t_ortho) > 1e-10 && dot(b_raw, b_raw) > 1e-10) {
            let tan = normalize(t_ortho);
            var hand = 1.0;
            if (dot(cross(n_geo, tan), b_raw) < 0.0) {
                hand = -1.0;
            }
            let bit = normalize(cross(n_geo, tan)) * hand;
            // Surface -> eye. world_pos is camera-relative, so the eye is at the origin.
            let v = normalize(-in.world_pos);
            let v_ts = vec3<f32>(dot(v, tan), dot(v, bit), dot(v, n_geo));
            // Distance fade: relief is a close-up cue and the march is the expensive part, so it
            // fades out well before the texture does. Beyond the fade there is no march at all.
            let dist_fade = 1.0 - smoothstep(35.0, 60.0, length(in.world_pos));
            // Self-gating needs an explicit early-out, not just the maths working out. A FLAT R=0
            // field is not "no displacement": the march would walk the ray to the bottom of the
            // height range and apply the FULL offset, shifting the texture on every stock
            // normal-mapped surface in the game. Stock NOHQ has R exactly 0, so one tap at the
            // undisplaced uv separates "no height authored" from "height authored and dark here".
            let h_probe = textureSampleLevel(textures[normal_packed >> 3u], samplers[normal_packed & 7u], in.uv, 0.0).r;
            if (dist_fade > 0.001 && v_ts.z > 0.15 && h_probe > 0.004) {
                // Step count falls off at grazing angles rather than rising: a grazing ray needs the
                // most steps and is also where a bounded march looks worst, so this trades a little
                // accuracy there for a hard cost ceiling.
                let steps = mix(8.0, 20.0, clamp(v_ts.z, 0.0, 1.0)) ;
                let amplitude = 0.02 * dist_fade;
                let max_offset = (v_ts.xy / max(v_ts.z, 0.15)) * amplitude;
                let dstep = 1.0 / steps;
                var cur_h = 1.0;
                var cur_uv = in.uv;
                var h = textureSampleLevel(textures[normal_packed >> 3u], samplers[normal_packed & 7u], cur_uv, 0.0).r;
                var i = 0.0;
                loop {
                    if (i >= steps || h >= cur_h) { break; }
                    cur_h = cur_h - dstep;
                    cur_uv = in.uv - max_offset * (1.0 - cur_h);
                    h = textureSampleLevel(textures[normal_packed >> 3u], samplers[normal_packed & 7u], cur_uv, 0.0).r;
                    i = i + 1.0;
                }
                // One linear refine between the last two steps: without it the silhouette of the
                // relief stairsteps at exactly the step count, which is more obvious than the
                // undersampling it comes from.
                let prev_uv = cur_uv + max_offset * dstep;
                let prev_h = textureSampleLevel(textures[normal_packed >> 3u], samplers[normal_packed & 7u], prev_uv, 0.0).r - (cur_h + dstep);
                let this_h = h - cur_h;
                let w = this_h / max(this_h - prev_h, 1e-5);
                uv = mix(cur_uv, prev_uv, clamp(w, 0.0, 1.0));
            }
        }
    }

    let base = textureSample(textures[packed >> 3u], samplers[packed & 7u], uv);
    let normal_sample = textureSample(textures[normal_packed >> 3u], samplers[normal_packed & 7u], uv);
    var normal_ts_xy = vec2<f32>(normal_sample.a, normal_sample.g) * 2.0 - vec2<f32>(1.0);
    if ((material_flags(material) & MATFLAG_INVERT_NORMAL_Y) != 0u) {
        normal_ts_xy.y = -normal_ts_xy.y;
    }
    let normal_ts = vec3<f32>(normal_ts_xy, sqrt(max(0.0, 1.0 - dot(normal_ts_xy, normal_ts_xy))));
    // Cutout: A2C emits a sharpened coverage alpha (edge dithered across MSAA samples);
    // otherwise the classic hard discard. a2c is a compile-time override, so exactly one
    // path is compiled and control flow stays uniform for the derivatives above/below.
    // Cutout alpha with the mip-coverage lift (the sampled alpha stays what it was for the
    // shading below). alpha_ref is a pipeline constant, so this folds away on opaque draws.
    var cut_a = base.a;
    if (alpha_ref > 0.0) {
        let dims0 = vec2<f32>(textureDimensions(textures[packed >> 3u]));
        cut_a = mip_alpha_boost(base.a, uv, dims0, cutout_mip_alpha);
    }
    var out_a = base.a;
    if (a2c > 0.5) {
        out_a = a2c_coverage(cut_a, alpha_ref);
        if (out_a <= 0.0) {
            discard;
        }
    } else if (cut_a < alpha_ref) {
        discard;
    }
    if (is_shadow > 0.5) {
        return vec4<f32>(0.0, 0.0, 0.0, frame.params.shadow_strength * base.a);
    }
    // Material Debug View: Base Color Only is the controlled reference; Normal Map
    // displays decoded tangent-space X=A, Y=G, reconstructed Z, independent of lighting.
    let material_view = u32(material.normal_map.y + 0.5);
    if (material_view == 1u) {
        return vec4<f32>(base.rgb, out_a);
    }
    if (material_view == 2u) {
        return vec4<f32>(normal_ts * 0.5 + vec3<f32>(0.5), out_a);
    }
    // The material colours are already sun-FOLDED on the CPU (mat_sun_* = raw × sun); hand
    // them to the shared shading straight. The GPU-driven path folds raw × frame-sun instead,
    // then calls the same shade(). srgb-decode of albedo + terms happens inside shade().
    var m: ShadeMaterial;
    m.emissive = material.emissive.rgb;
    if (emissive_night < 1.0 && (material_flags(material) & MATFLAG_NIGHT_EMITTER) == 0u) {
        // At EVERY hour, not only at night. The first cut faded this in with the sun-only
        // daylight factor, and the owner's 05:25 Takistan shots showed why that is not enough:
        // at dawn the ramp is already up while the real light is at its weakest, so the dead
        // trees went white and the bushes lit up exactly when they should be darkest. Legacy
        // compensation emissive is not radiance at noon either -- it is merely invisible under
        // full sun. Fixtures (MATFLAG_NIGHT_EMITTER) are the only exemption.
        m.emissive = m.emissive * clamp(emissive_night, 0.0, 1.0);
    }
    m.sun_ambient = material.sun_ambient.rgb;
    m.sun_diffuse = material.sun_diffuse.rgb;
    m.light_diffuse = material.light_diffuse.rgb;
    m.light_ambient = material.light_ambient.rgb;
    m.specular = material.specular.rgb;
    m.spec_power = material.specular.w;
    // The per-draw path carries no specular map: this is the ~7 sections a frame that reach
    // DrawSectionTL, not the retained set. 1.0 leaves the flat constant exactly as before.
    m.spec_mask = 1.0;
    // Stage 2 MapType gate: foliage lighting (leaf SSS + canopy AO) applies only to real
    // VEGETATION cutouts, so roads / characters / fences don't pick up the leaf look. The object's
    // MapType is published per draw in the free .w of the sun-ambient material lane (only .rgb is
    // read for shading) — set from GCurrentIsVegetation in EngineWgpu::DrawSectionTL. The
    // alpha-test discard above stays keyed on alpha_ref (every cutout still discards).
    let veg_cutout = material.sun_ambient.w > 0.5 && alpha_ref > 0.0;
    // The mesh has no authored tangent stream in this production path. Recover a
    // tangent frame from the actual interpolated UV0/world-position derivatives;
    // this is deterministic and lets Stage1 use its declared `tex` source without
    // fabricating tangent handedness in the canonical model representation.
    let geometric_normal = normalize(in.normal);
    var shading_normal = geometric_normal;
    if (material.normal_map.z > 0.5) {
        let authored_frame = dot(in.tangent, in.tangent) > 1e-10 && dot(in.binormal, in.binormal) > 1e-10;
        let duv_dx = dpdx(in.uv);
        let duv_dy = dpdy(in.uv);
        // Scale-invariant cotangent frame; see the matching block in gpu_driven.wgsl.
        // The previous form multiplied the world-space and UV footprints together and
        // tested the result against an absolute epsilon, so the frame was rejected --
        // and the normal map silently dropped -- once a surface came close enough that
        // both footprints were small. Crossing with the geometric normal first also
        // makes the frame perpendicular to it by construction, which is what the
        // orthonormalisation step used to be for.
        var tangent_raw = cross(dwy, geometric_normal) * duv_dx.x + cross(geometric_normal, dwx) * duv_dy.x;
        var bitangent_raw = cross(dwy, geometric_normal) * duv_dx.y + cross(geometric_normal, dwx) * duv_dy.y;
        if (authored_frame) {
            // An authored frame is already the right shape; only make it perpendicular.
            let tangent_ortho = in.tangent - geometric_normal * dot(geometric_normal, in.tangent);
            tangent_raw = tangent_ortho;
            bitangent_raw = cross(geometric_normal, tangent_ortho) *
                            select(-1.0, 1.0, dot(cross(geometric_normal, tangent_ortho), in.binormal) >= 0.0);
        }
        let frame_scale2 = max(dot(tangent_raw, tangent_raw), dot(bitangent_raw, bitangent_raw));
        if (frame_scale2 > 0.0) {
            let inv_scale = inverseSqrt(frame_scale2);
            let tangent = tangent_raw * inv_scale;
            let bitangent = bitangent_raw * inv_scale;
            shading_normal = normalize(tangent * normal_ts.x + bitangent * normal_ts.y + geometric_normal * normal_ts.z);
        }
    }
    // DZ-005 -- reflective (CalmWater) draws OVERRIDE the tangent-space result with a
    // world-space sample of the same normal map; see water_ripple_normal. Kept in step with
    // gpu_driven.wgsl's copy on purpose: DayZ water reaches the retained path in practice, but
    // the two paths must not disagree about what a water surface looks like.
    if ((material_flags(material) & MATFLAG_REFLECTIVE) != 0u && frame.wateranim.y > 0.0
        && !matdbg_flag(MATDBG_DISABLE_FRESNEL_ENV)) {
        let t = frame.wateranim.x;
        shading_normal = water_ripple_normal(
            normal_packed >> 3u, geometric_normal, in.world_pos, dwx, dwy,
            water_drift(WATER_DRIFT_DIR_A, WATER_DRIFT_SPEED_A, WATER_RIPPLE_PERIOD_A, t),
            water_drift(WATER_DRIFT_DIR_B, WATER_DRIFT_SPEED_B, WATER_RIPPLE_PERIOD_B, t),
            frame.wateranim.y);
    }
    let rgb = shade(
        base.rgb, m, shading_normal, in.world_pos, in.fog, dwx, dwy, linear,
        // The per-draw path has no retained model id, so no baked volume applies.
        1.0, vec3<f32>(0.0), foliage_shadow_ao,
        veg_cutout, translucent > 0.5, veg_cutout, in.clip.xy,
    );
    // DZ-003 -- Fresnel-weighted sky reflection for materials that declared themselves
    // reflective. Every other draw takes the branch not-taken and is bit-identical to
    // before. `linear` gates it to the HDR target: the env map is linear radiance that
    // routinely exceeds 1.0, and on an LDR target it would clip to white.
    //
    // DZ-007: and only while the dev panel's "Disable Fresnel / Environment" checkbox is
    // clear. `frame.matdbg` is documented as the RETAINED path's lane, but it is the dev
    // panel's single copy of this state in the shared frame UBO and both paths can see it --
    // the per-draw path's own per-instance debug lane is a legacy of the material-debug
    // slice and would give the panel two switches for one checkbox.
    var out_rgb = rgb;
    if ((material_flags(material) & MATFLAG_REFLECTIVE) != 0u && linear > 0.5
        && !matdbg_flag(MATDBG_DISABLE_FRESNEL_ENV)) {
        out_rgb = env_reflection(out_rgb, shading_normal, in.world_pos, in.fog);
    }
    return vec4<f32>(out_rgb, out_a);
}

// Depth + normal prepass fragment (docs/depth-prepass-plan.md). Reuses vs_main/
// vs_skinned unchanged (same VS + override constants -> bit-for-bit depth as fs_main),
// and writes ONLY the view-space octahedral normal into the Rg16Float G-buffer; depth is
// written by the fixed-function stage. Cutout foliage (alpha_ref > 0) applies the SAME
// hard discard as fs_main so prepass coverage matches the colour pass by construction.
// `alpha_ref` is a pipeline-override constant, so the branch is compile-time uniform and
// the textureSample stays legal.
@fragment
fn fs_prepass(in: VsOut) -> @location(0) vec2<f32> {
    if (alpha_ref > 0.0) {
        let packed = bitcast<u32>(materials[in.instance].emissive.w);
        let dims0 = vec2<f32>(textureDimensions(textures[packed >> 3u]));
        let a = mip_alpha_boost(textureSample(textures[packed >> 3u], samplers[packed & 7u], in.uv).a, in.uv, dims0,
                                cutout_mip_alpha);
        if (a < alpha_ref) {
            discard;
        }
    }
    // in.normal is world space (camera-relative world uses the same orientation); the
    // view matrix's translation is zeroed, so multiplying the direction gives view space.
    let n_view = (frame.view * vec4<f32>(normalize(in.normal), 0.0)).xyz;
    return oct_encode(normalize(n_view));
}

// A2C prepass twin (MSAA cutout foliage). Same normal output as fs_prepass, but returns a
// vec4 so location(0)'s .a carries the sharpened coverage for alpha-to-coverage — the extra
// .b/.a components are dropped by the Rg16Float attachment write but still drive the coverage
// mask. Emits the SAME coverage as fs_main so the prepass writes depth to exactly the samples
// the colour pass will shade (terrain fills the rest -> no background halo at edges).
@fragment
fn fs_prepass_a2c(in: VsOut) -> @location(0) vec4<f32> {
    let packed = bitcast<u32>(materials[in.instance].emissive.w);
    let dims0 = vec2<f32>(textureDimensions(textures[packed >> 3u]));
    let alpha = mip_alpha_boost(textureSample(textures[packed >> 3u], samplers[packed & 7u], in.uv).a, in.uv, dims0,
                                cutout_mip_alpha);
    let cov = a2c_coverage(alpha, alpha_ref);
    if (cov <= 0.0) {
        discard;
    }
    let n_view = (frame.view * vec4<f32>(normalize(in.normal), 0.0)).xyz;
    let oct = oct_encode(normalize(n_view));
    return vec4<f32>(oct.x, oct.y, 0.0, cov);
}
