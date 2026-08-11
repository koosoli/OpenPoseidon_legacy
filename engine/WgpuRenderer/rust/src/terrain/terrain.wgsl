// GPU terrain: a shared grid mesh instanced per node, heightmap-displaced in the
// vertex shader. Shares group 0 (the camera UBO + cascade shadow map) with the
// lit 3D pipeline, so terrain receives the same CSM shadows. The fragment shader
// blends the four surrounding land cells' detail-array layers (indexed by a
// per-cell index map) and modulates by a tiled high-frequency noise texture.

// Shares group(0) (the camera UBO + cascade shadow map) with the lit 3D
// pipeline via the frame module, so terrain receives the same CSM shadows and
// sun lighting.
#import frame::{frame, reverse_z, fog_factor, apply_fog, sky_irradiance, sky_vis_ao, cloud_sun_shadow, sky_vis_debug_on, sky_vis_debug_value, gtao_ao, gtao_debug_on, gtao_bent_normal_world, gtao_debug_colour, interior_sky_ao, interior_sky_reach, interior_sky_debug_on, interior_sky_ambient_normal}
#import shadow::shadow_strength
#import lighting::lights_contrib
#import color::srgb_to_linear
#import gbuffer::oct_encode

struct TerrainParams {
    world_origin: vec2<f32>,
    land_grid: f32,
    terrain_grid: f32,
    hm_width: u32,
    hm_height: u32,
    land_range: u32,
    data_scale: f32,
    // Coast wet band (Stage 2c). sea_level + time (+ swash) move the damp intertidal line with
    // the water's edge; wet_height = m above the swash-moved sea level the band reaches;
    // wet_darken = albedo multiplier in the band (1 = off).
    sea_level: f32,
    time: f32,
    swash_speed: f32,
    swash_amp: f32,
    wet_height: f32,
    wet_darken: f32,
    _pad0: f32,
    _pad1: f32,
};

// Must match GRID_N in terrain/mod.rs.
const GRID_N: f32 = 32.0;

@group(1) @binding(0) var<uniform> tp: TerrainParams;
@group(1) @binding(1) var heightmap: texture_2d<f32>;
// Long-distance terrain sun-shadow mask (terrain_shadow.wgsl sweep): world-aligned
// on the heightmap grid, .r = lit factor (1 = lit, 0 = fully shadowed). One
// bilinear tap gives terrain-on-terrain self-shadowing at any range; it composes
// with CSM by max() (most-occluded wins), and terrain is never a CSM caster so the
// two never double-shadow the same ground.
@group(1) @binding(2) var shadow_mask: texture_2d<f32>;
@group(1) @binding(3) var shadow_mask_samp: sampler;
// Bindless ground textures: one texture_2d per Landscape texture index, native
// size/format/mips. Indexed non-uniformly per fragment (needs the device's
// SAMPLED_TEXTURE_..._NON_UNIFORM_INDEXING feature).
@group(2) @binding(0) var ground: binding_array<texture_2d<f32>>;
@group(2) @binding(1) var ground_samp: sampler;
@group(2) @binding(2) var index_map: texture_2d<u32>;
@group(2) @binding(3) var detail: texture_2d<f32>;
@group(2) @binding(4) var ground_clamp_samp: sampler;
@group(2) @binding(5) var jitter_map: texture_2d<f32>;
struct TerrainUv {
    u: vec4<f32>,
    v: vec4<f32>,
};
struct TerrainMaterial {
    legacy: u32,
    satellite: u32,
    mask: u32,
    base: u32,
    overlays: array<u32, 4>,
    overlay_count: u32,
    uv_source_mask: u32,
    padding: array<u32, 2>,
    satellite_uv: TerrainUv,
    mask_uv: TerrainUv,
    base_uv: TerrainUv,
    overlay_uvs: array<TerrainUv, 4>,
};
@group(2) @binding(6) var<storage, read> terrain_materials: array<TerrainMaterial>;

fn hm_load(ix: i32, iz: i32) -> f32 {
    let cx = clamp(ix, 0, i32(tp.hm_width) - 1);
    let cz = clamp(iz, 0, i32(tp.hm_height) - 1);
    return textureLoad(heightmap, vec2<i32>(cx, cz), 0).x;
}

// World height at a world-xz, matching Landscape::SurfaceY's per-cell triangle
// interpolation (not bilinear) so surface decals stay coplanar with the mesh.
fn sample_height(world_xz: vec2<f32>) -> f32 {
    let t = (world_xz - tp.world_origin) / tp.terrain_grid;
    let base = floor(t);
    let ix = i32(base.x);
    let iz = i32(base.y);
    let f = t - base; // f.x = x within cell, f.y = z within cell
    let y00 = hm_load(ix, iz);
    let y01 = hm_load(ix + 1, iz);
    let y10 = hm_load(ix, iz + 1);
    let y11 = hm_load(ix + 1, iz + 1);
    if (f.x <= 1.0 - f.y)
    {
        return y00 + (y10 - y00) * f.y + (y01 - y00) * f.x;
    }
    return y10 + (y01 - y11) - (y10 - y11) * f.x - (y01 - y11) * f.y;
}

// Central-difference normal at a fixed heightmap step. Taken per-fragment (not
// per-vertex) so it depends only on world position, never on the patch's LOD or
// morph state — that independence is what keeps even terrain evenly lit. A
// mesh/morph-derived normal ramps with camera distance within each patch and
// banded the lighting into radial stripes under grazing sun.
fn sample_normal(world_xz: vec2<f32>, step: f32) -> vec3<f32> {
    let hx0 = sample_height(world_xz - vec2<f32>(step, 0.0));
    let hx1 = sample_height(world_xz + vec2<f32>(step, 0.0));
    let hz0 = sample_height(world_xz - vec2<f32>(0.0, step));
    let hz1 = sample_height(world_xz + vec2<f32>(0.0, step));
    return normalize(vec3<f32>(-(hx1 - hx0), 2.0 * step, -(hz1 - hz0)));
}

// Index-map entry for a land cell, clamped to the map (row = z, column = x;
// same orientation as the heightmap and Landscape::GetTexture(z, x)). Bits
// 0-14 = ground-array layer; bit 15 = clamped transition tile (GL33's
// ClampU|ClampV: the texture maps exactly once onto its cell, edges extended,
// instead of tiling).
const CELL_LAYER_MASK: u32 = 0x7fffu;
const CELL_CLAMPED: u32 = 0x8000u;

fn cell_entry(cell: vec2<i32>) -> u32 {
    let cx = clamp(cell.x, 0, i32(tp.land_range) - 1);
    let cz = clamp(cell.y, 0, i32(tp.land_range) - 1);
    return textureLoad(index_map, vec2<i32>(cx, cz), 0).x;
}

// One land cell's contribution to the ground blend. Simple (tileable) layers
// sample at the global one-period-per-cell UV, so where neighbouring cells
// share a layer the blend stays an exact no-op. Clamped transition tiles
// sample in their own cell's [0,1] frame through the edge-extending sampler:
// a neighbour's contribution near the shared border is then that tile's
// matching edge row, not its wrapped-around opposite edge (which both smeared
// the wrong ground type across the border and put the tile's own wrap seam on
// the cell edge). Explicit gradients keep mip selection continuous across the
// frame switch and keep the call legal in non-uniform control flow.
fn sample_cell(entry: u32, cell: vec2<f32>, tile_uv: vec2<f32>,
               ddx: vec2<f32>, ddy: vec2<f32>) -> vec3<f32> {
    let layer = entry & CELL_LAYER_MASK;
    if ((entry & CELL_CLAMPED) != 0u)
    {
        return textureSampleGrad(ground[layer], ground_clamp_samp, tile_uv - cell, ddx, ddy).rgb;
    }
    return textureSampleGrad(ground[layer], ground_samp, tile_uv, ddx, ddy).rgb;
}

fn terrain_uv(transform: TerrainUv, source_bit: u32, source_mask: u32,
              world_pos: vec3<f32>, tile_uv: vec2<f32>) -> vec2<f32> {
    let source = select(vec3<f32>(tile_uv, 0.0), world_pos, (source_mask & (1u << source_bit)) != 0u);
    return vec2<f32>(dot(source, transform.u.xyz) + transform.u.w,
                     dot(source, transform.v.xyz) + transform.v.w);
}

fn terrain_uv_gradient(transform: TerrainUv, source_bit: u32, source_mask: u32,
                       world_gradient: vec3<f32>, tile_gradient: vec2<f32>) -> vec2<f32> {
    let source = select(vec3<f32>(tile_gradient, 0.0), world_gradient, (source_mask & (1u << source_bit)) != 0u);
    return vec2<f32>(dot(source, transform.u.xyz), dot(source, transform.v.xyz));
}

// OPRW25 TerrainSNX is authored as a satellite colour plus an LCA RGBA mask
// selecting a base surface and up to four overlays.  The satellite retains its
// low-frequency tile variation; the mask controls the high-frequency surfaces.
// Legacy terrain descriptors have overlay_count == 0 and take the original
// single-texture route exactly.
fn sample_authored_cell(entry: u32, cell: vec2<f32>, tile_uv: vec2<f32>,
                        ddx: vec2<f32>, ddy: vec2<f32>, world_pos: vec3<f32>,
                        world_dwx: vec3<f32>, world_dwy: vec3<f32>) -> vec3<f32> {
    let material = terrain_materials[entry & CELL_LAYER_MASK];
    let authored_weight = clamp(a3_surface_blend, 0.0, 0.35);
    // The normal renderer must remain byte-for-byte on the established cell
    // path while the authored TerrainSNX composition is evaluated and compared.
    // This also avoids applying an unavailable texgen to legacy descriptors.
    if (material.overlay_count == 0u || authored_weight == 0.0) {
        return sample_cell(material.legacy, cell, tile_uv, ddx, ddy);
    }
    // RVMAT's worldPos is terrain-local X/Z/Y, whereas the renderer carries
    // world coordinates as X/Y/Z. Preserve height in the third component but
    // put the planar Z axis where TerrainSNX TexGen expects it.
    let material_pos = vec3<f32>(world_pos.x, world_pos.z, world_pos.y);
    let material_dwx = vec3<f32>(world_dwx.x, world_dwx.z, world_dwx.y);
    let material_dwy = vec3<f32>(world_dwy.x, world_dwy.z, world_dwy.y);
    let satellite_uv = terrain_uv(material.satellite_uv, 0u, material.uv_source_mask, material_pos, tile_uv);
    let mask_uv = terrain_uv(material.mask_uv, 1u, material.uv_source_mask, material_pos, tile_uv);
    let base_uv = terrain_uv(material.base_uv, 2u, material.uv_source_mask, material_pos, tile_uv);
    let satellite_dwx = terrain_uv_gradient(material.satellite_uv, 0u, material.uv_source_mask, material_dwx, ddx);
    let satellite_dwy = terrain_uv_gradient(material.satellite_uv, 0u, material.uv_source_mask, material_dwy, ddy);
    let mask_dwx = terrain_uv_gradient(material.mask_uv, 1u, material.uv_source_mask, material_dwx, ddx);
    let mask_dwy = terrain_uv_gradient(material.mask_uv, 1u, material.uv_source_mask, material_dwy, ddy);
    let base_dwx = terrain_uv_gradient(material.base_uv, 2u, material.uv_source_mask, material_dwx, ddx);
    let base_dwy = terrain_uv_gradient(material.base_uv, 2u, material.uv_source_mask, material_dwy, ddy);
    let mask = textureSampleGrad(ground[material.mask], ground_samp, mask_uv, mask_dwx, mask_dwy).rgba;
    // TerrainSNX has one implicit base and up to four masked detail layers:
    // Stage4 is the base, stages 6/8/10/12 map to mask RGBA respectively.
    // The unpainted mask remainder belongs to the base layer.
    let mask_weight = min(dot(mask, vec4<f32>(1.0)), 1.0);
    var surface = textureSampleGrad(ground[material.base], ground_samp, base_uv, base_dwx, base_dwy).rgb * (1.0 - mask_weight);
    for (var i = 0u; i < min(material.overlay_count, 4u); i = i + 1u) {
        let overlay_uv = terrain_uv(material.overlay_uvs[i], 3u + i, material.uv_source_mask, material_pos, tile_uv);
        let overlay_dwx = terrain_uv_gradient(material.overlay_uvs[i], 3u + i, material.uv_source_mask, material_dwx, ddx);
        let overlay_dwy = terrain_uv_gradient(material.overlay_uvs[i], 3u + i, material.uv_source_mask, material_dwy, ddy);
        surface += textureSampleGrad(ground[material.overlays[i]], ground_samp, overlay_uv, overlay_dwx, overlay_dwy).rgb * mask[i];
    }
    // Preserve the satellite as the low-frequency colour authority. The LCA
    // stack is close-range detail, not a second satellite: leaving it visible
    // at long range exposes its per-material colour blocks. Fade that detail
    // from 25 to 120 metres, exactly where the continuous satellite texture is
    // the useful anti-tiling signal. Zero remains the exact legacy fallback;
    // the pipeline override is intentionally capped while this authored path
    // is compared against the established renderer.
    // Stage 0 is not a repeating close-detail texture: TerrainSNX provides a
    // separate TexGen for its continuous, low-frequency satellite colour.
    // Sampling it through the old cell frame is what made a genuine layered
    // A3 material look like a tiled single texture.
    let satellite = textureSampleGrad(ground[material.satellite], ground_samp,
                                      satellite_uv, satellite_dwx, satellite_dwy).rgb;
    let detail_fade = 1.0 - smoothstep(25.0, 120.0, length(world_pos));
    return mix(satellite, surface, authored_weight * detail_fade);
}

struct VsOut {
    @builtin(position) clip: vec4<f32>,
    @location(0) world_xz: vec2<f32>,  // absolute world-xz
    @location(1) fog: f32,             // 1 = keep colour, 0 = full fog
    @location(2) world_pos: vec3<f32>, // camera-relative
};

// Skirt drop, as a multiple of the patch's vertex spacing. Tunable via
// WGR_TERRAIN_SKIRT_K (0 = skirts flush with the surface, i.e. effectively off).
override skirt_k: f32 = 4.0;

@vertex
fn vs_terrain(
    @location(0) grid_in: vec3<f32>, // xy = unit grid position in [0,1]^2, z = skirt flag
    @location(1) origin: vec2<f32>,  // node world-xz origin
    @location(2) size: f32,          // node world size
    @location(3) lod: u32,
    @location(4) morph: vec2<f32>,   // (morph_start, morph_end) camera-distance band
) -> VsOut {
    let grid = grid_in.xy;
    let world_xz_fine = origin + grid * size;
    let height_fine = sample_height(world_xz_fine);
    let dist = length(vec3<f32>(world_xz_fine.x, height_fine, world_xz_fine.y) - frame.cam_pos.xyz);

    // Snap toward the coarser even lattice as the vertex nears morph_end, so the
    // edge meets the parent grid at the LOD switch without a crack.
    var morph_k = 0.0;
    if (morph.y > morph.x)
    {
        morph_k = clamp((dist - morph.x) / (morph.y - morph.x), 0.0, 1.0);
    }
    let gidx = grid * GRID_N;
    let grid_coarse = (round(gidx * 0.5) * 2.0) / GRID_N;
    let world_xz = origin + mix(grid, grid_coarse, morph_k) * size;

    let height = sample_height(world_xz) - grid_in.z * (size / GRID_N) * skirt_k;
    let world_rel = vec3<f32>(world_xz.x, height, world_xz.y) - frame.cam_pos.xyz;

    var out: VsOut;
    out.clip = reverse_z(frame.proj * frame.view * vec4<f32>(world_rel, 1.0));
    // Texture + normal use the same morphed world position the geometry is drawn
    // at, so the UV stays locked to the mesh and morphs smoothly with it. (Using
    // the un-morphed position instead decouples the UV from the screen-space
    // interpolation and compresses the tiling wherever the morph collapses
    // vertices -> broken tiling at LOD > 0.)
    out.world_xz = world_xz;
    out.world_pos = world_rel;

    out.fog = fog_factor(length(world_rel));
    return out;
}

// Half-width (in land-cell fractions) of the texture cross-fade band centred on
// each cell boundary. Land cells are large (~50 m), so a full-cell linear blend
// smears a wide muddy seam; narrowing it to a band near the boundary keeps cell
// interiors crisp. 0 -> hard edges (GL33-like); 0.5 -> full-cell blend.
override blend_width: f32 = 0.15;
// TerrainSNX's authored LCA base/overlay detail contribution. Kept opt-in while
// it is verified against the legacy cell fallback; it is capped in
// sample_authored_cell so the satellite base remains continuous.
override a3_surface_blend: f32 = 0.0;
// HDR path (docs/hdr-pipeline-plan.md): 1 = decode ground albedo + sun/light/fog
// colours from sRGB to linear and drop the [0,1] radiance clamp. 0 = gamma-naive.
override linear: f32 = 0.0;

@fragment
fn fs_terrain(in: VsOut) -> @location(0) vec4<f32> {
    // The reflected pass keeps only terrain on/above the global water plane. Fragment
    // clipping is conservative for a displaced heightfield and avoids an oblique matrix.
    if (dot(frame.clip_plane.xyz, in.world_pos + frame.cam_pos.xyz) + frame.clip_plane.w < 0.0) {
        discard;
    }
    // Receiver-plane derivatives must run in uniform control flow.
    let dwx = dpdx(in.world_pos);
    let dwy = dpdy(in.world_pos);

    // Continuous land-cell position; the ground texture repeats once per cell,
    // so this (plus jitter) doubles as the tiling UV. Blend the four cells
    // around the sample by fractional distance to their centres — where
    // neighbours share a layer the blend is a no-op (seamless), elsewhere it
    // cross-fades the hard cell edge.
    let cell_pos = (in.world_xz - tp.world_origin) / tp.land_grid;
    // Random UV offset breaking up the per-cell tiling repetition: the jitter
    // map holds Landscape::_random's per-grid-point offset (texel (x,z) = grid
    // point (x,z)), so a bilinear tap at cell_pos + half a texel reproduces the
    // corner interpolation GL33 bakes into its vertex UVs. Geography zeroes and
    // smooths the field around non-simple cells, so clamped transition tiles
    // are never warped off their designed edges.
    let jdim = vec2<f32>(textureDimensions(jitter_map));
    let jitter = textureSampleLevel(jitter_map, ground_clamp_samp,
                                    (cell_pos + vec2<f32>(0.5)) / jdim, 0.0).xy;
    let tile_uv = cell_pos + jitter;
    // Gradients of the shared tiling UV, for every ground tap (see sample_cell).
    let duvdx = dpdx(tile_uv);
    let duvdy = dpdy(tile_uv);
    let cc = cell_pos - vec2<f32>(0.5);
    let base = floor(cc);
    let f = cc - base;
    let bi = vec2<i32>(i32(base.x), i32(base.y));
    let e00 = cell_entry(bi + vec2<i32>(0, 0));
    let e10 = cell_entry(bi + vec2<i32>(1, 0));
    let e01 = cell_entry(bi + vec2<i32>(0, 1));
    let e11 = cell_entry(bi + vec2<i32>(1, 1));

    // Sharpen the fraction so the cross-fade concentrates in a band around the
    // cell boundary (f = 0.5) rather than ramping across the whole cell. Endpoints
    // stay 0/1, so neighbouring samples remain seamless.
    let bw = clamp(blend_width, 0.001, 0.5);
    let fx = smoothstep(0.5 - bw, 0.5 + bw, f.x);
    let fy = smoothstep(0.5 - bw, 0.5 + bw, f.y);
    let w00 = (1.0 - fx) * (1.0 - fy);
    let w10 = fx * (1.0 - fy);
    let w01 = (1.0 - fx) * fy;
    let w11 = fx * fy;
    let absolute_world_pos = in.world_pos + frame.cam_pos.xyz;
    var rgb = w00 * sample_authored_cell(e00, base, tile_uv, duvdx, duvdy, absolute_world_pos, dwx, dwy)
            + w10 * sample_authored_cell(e10, base + vec2<f32>(1.0, 0.0), tile_uv, duvdx, duvdy, absolute_world_pos, dwx, dwy)
            + w01 * sample_authored_cell(e01, base + vec2<f32>(0.0, 1.0), tile_uv, duvdx, duvdy, absolute_world_pos, dwx, dwy)
            + w11 * sample_authored_cell(e11, base + vec2<f32>(1.0, 1.0), tile_uv, duvdx, duvdy, absolute_world_pos, dwx, dwy);

    // High-frequency detail noise: alpha modulates around neutral (matches GL33's
    // r0.rgb *= t1.a * 2.0, detail UV = base UV * 32).
    let detail_a = textureSample(detail, ground_samp, tile_uv * 32.0).a;
    rgb *= detail_a * 2.0;
    // HDR path: decode the gamma-space ground albedo (blend + detail done in gamma,
    // the §5 pragmatic fold) to linear before lighting.
    if (linear > 0.5) {
        rgb = srgb_to_linear(rgb);
    }

    // Per-pixel normal at a fixed heightmap step (independent of patch LOD/morph).
    let n = sample_normal(in.world_xz, tp.terrain_grid);

    // Combined sun shadow: CSM (objects + near contact) and the long-range
    // heightfield mask (terrain-on-terrain) compose by max() — whichever occludes
    // the sun more wins. Both fade out with fog. The mask stores, per column, the
    // world height below which that column is terrain-shadowed (.r = ceiling,
    // .g = penumbra half-width in metres, .b = strength), so a point is shadowed by
    // how far its world height sits below the ceiling. Its grid is `scale`x the
    // heightmap (sharper edges): sample in mask-texel space (world -> heightfield
    // texel -> * scale), landing on texel centres at (coord + 0.5)/dims.
    let csm_s = shadow_strength(in.world_pos, n, in.fog, dwx, dwy);
    let mask_dims = vec2<f32>(textureDimensions(shadow_mask));
    let mask_scale = mask_dims / vec2<f32>(f32(tp.hm_width), f32(tp.hm_height));
    let mask_coord = (in.world_xz - tp.world_origin) / tp.terrain_grid * mask_scale;
    let mask_uv = (mask_coord + vec2<f32>(0.5)) / mask_dims;
    let sm = textureSampleLevel(shadow_mask, shadow_mask_samp, mask_uv, 0.0);
    // Fine-surface world height for the shadow test, sampled from the heightmap at this
    // fragment rather than the interpolated mesh height. The mask's ceiling (sm.r) is
    // baked from the full-resolution heightfield, but in.world_pos.y sags toward the
    // coarse lattice at distant LODs; testing that mismatched height drops whole patches
    // below the ceiling and flashes tile-sized shadow blobs several km out. Sampling the
    // fine surface makes the test LOD/morph-independent, the same independence the
    // per-fragment normal above relies on (costs one extra heightmap tap).
    let world_y = sample_height(in.world_xz);
    let lit = smoothstep(sm.r - sm.g, sm.r + sm.g + 1e-3, world_y);
    let terrain_s = clamp(sm.b * (1.0 - lit), 0.0, 1.0) * in.fog;
    let shadow = max(csm_s, terrain_s);

    // Coast wet band: near-flat terrain around the (swash-moved) sea level reads as damp sand —
    // darker albedo — strongest at the waterline, fading out over wet_height metres. Keyed on
    // the SAME sea level + swash the water uses, so the wet line registers with the water's edge.
    // Slope-gated by n.y so cliffs/steep coast stay dry. Cosmetic; zero gameplay impact.
    let sea_ref = tp.sea_level + sin(6.2831853 * tp.time * tp.swash_speed) * tp.swash_amp;
    let above_sea = world_y - sea_ref;
    let flat = smoothstep(0.55, 0.85, n.y);
    let wet = flat * (1.0 - smoothstep(0.0, tp.wet_height, above_sea));
    rgb *= mix(1.0, tp.wet_darken, wet);

    // A shadow removes the direct sun (the N.L diffuse term); sky ambient and the
    // local point/spot lamps survive, so shadowed terrain settles to the ambient
    // tone rather than going black. This is why the darkening reads as a soft cast
    // shadow and never collapses to pure black when CSM's darkness constant is 0
    // (shadow maps disabled). Terrain has no material, so local lights modulate white.
    let cos_fi = max(dot(n, -frame.sun_dir_world.xyz), 0.0);
    var sun_diffuse = frame.sun_diffuse.rgb;
    var sun_ambient = frame.sun_ambient.rgb;
    var fog_color = frame.fog_color.rgb;
    // sun_diffuse.w = 1: sky-based lighting, sun/ambient are already physical linear radiance
    // (atmosphere-derived), so don't sRGB-decode them (that path only applies to legacy gamma sun).
    let sky_lit = frame.sun_diffuse.w > 0.5;
    if (linear > 0.5) {
        if (!sky_lit) {
            sun_diffuse = srgb_to_linear(sun_diffuse);
            sun_ambient = srgb_to_linear(sun_ambient);
        }
        fog_color = srgb_to_linear(fog_color);
    }
    // Sky-based lighting: replace the flat ambient with DIRECTIONAL sky irradiance (SH-9 env
    // projection, per surface normal), scaled by the skyAmbient knob in sun_ambient.w.
    if (sky_lit) {
        // Directional ambient (Stage 2): sky sampled along the bent normal — the average open
        // direction — rather than the surface normal, so a slope beside an occluder picks up
        // light from where it can actually see sky. Returns `n` when the path is off.
        let amb_n = interior_sky_ambient_normal(
            in.world_pos + frame.cam_pos.xyz,
            gtao_bent_normal_world(in.clip.xy, n),
        );
        sun_ambient = sky_irradiance(amb_n) * frame.sun_ambient.w;
    }
    // Ambient occlusion: scale the ambient (directional SH or legacy flat) by how much sky this
    // point can see. Orthogonal to `shadow`, which removes the DIRECT sun. The two terms MULTIPLY
    // because they occlude independently and at disjoint scales (plan §6): sky-visibility is the
    // baked km-scale column factor that darkens valleys and cliff-bases, GTAO the screen-space
    // near/mid term that resolves local folds and object contact. Each returns 1 when off.
    //
    // Interior sky visibility joins them as a third independent occluder. Terrain is deliberately
    // absent from that MAP (a hillside is not a roof), but it must still RECEIVE the term: the
    // floor of a shed, a barrack or an archway is terrain, and leaving it at full sky ambient
    // while the walls around it darkened would look worse than not having the feature.
    sun_ambient *= sky_vis_ao(in.world_xz)
        * gtao_ao(in.clip.xy)
        * interior_sky_ao(in.world_pos + frame.cam_pos.xyz, n);
    // CLD-020: cloud transmittance dims the DIRECT term only, leaving sky ambient intact, so
    // ground under a cloud settles toward ambient rather than toward black.
    let sun_raw = sun_diffuse * cos_fi * (1.0 - shadow) * cloud_sun_shadow(in.world_xz) + sun_ambient;
    // HDR keeps radiance uncapped into the float target; LDR saturates like GL33.
    let sun = select(min(sun_raw, vec3<f32>(1.0)), sun_raw, linear > 0.5);
    let local = lights_contrib(in.world_pos, n, vec3<f32>(1.0), vec3<f32>(1.0), linear);
    let light_sum = sun + local;
    rgb *= select(clamp(light_sum, vec3<f32>(0.0), vec3<f32>(1.0)), max(light_sum, vec3<f32>(0.0)), linear > 0.5);

    // Debug: output the contrast-shaped sky-view factor as greyscale (unfogged) to inspect/tune the
    // mask — responds to radius/azimuths/downsample/contrast.
    if (sky_vis_debug_on() > 0.5) {
        return vec4<f32>(vec3<f32>(sky_vis_debug_value(in.world_xz)), 1.0);
    }

    // Debug: the raw screen-space AO buffer as greyscale (unfogged), for tuning radius/strength/
    // slices/steps/blur against the buffer itself rather than through the lit result.
    if (gtao_debug_on() > 0.5) {
        return vec4<f32>(gtao_debug_colour(in.clip.xy, n), 1.0);
    }

    // Debug: the interior sky-reach factor as greyscale (unfogged). Terrain and objects switch
    // together so the whole opaque scene shows the same buffer.
    if (interior_sky_debug_on() > 0.5) {
        return vec4<f32>(vec3<f32>(interior_sky_reach(in.world_pos + frame.cam_pos.xyz)), 1.0);
    }

    // fog_enabled: 2 = aerial perspective via the froxel volume (per-fragment); 1 =
    // legacy flat distance fog; 0 = off. in.fog is still used above for distance-faded
    // shadows regardless.
    if (frame.params.fog_enabled >= 1.5) {
        rgb = apply_fog(rgb, in.world_pos);
    } else {
        rgb = mix(fog_color, rgb, in.fog);
    }
    return vec4<f32>(rgb, 1.0);
}

// Depth + normal prepass fragment (docs/depth-prepass-plan.md). Reuses vs_terrain
// unchanged and writes ONLY the view-space octahedral normal into the Rg16Float
// G-buffer (depth is written by the fixed-function stage). The normal is the same
// per-fragment heightmap central difference fs_terrain derives, transformed to view
// space (view translation is zeroed, so the direction transform is a pure rotation).
@fragment
fn fs_terrain_prepass(in: VsOut) -> @location(0) vec2<f32> {
    let n = sample_normal(in.world_xz, tp.terrain_grid);
    let n_view = (frame.view * vec4<f32>(n, 0.0)).xyz;
    return oct_encode(normalize(n_view));
}
