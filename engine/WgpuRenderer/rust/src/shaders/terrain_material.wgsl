#define_import_path terrain_material

struct TerrainUv {
    u: vec4<f32>,
    v: vec4<f32>,
};
// Mirrors WgrTerrainMaterial (wgpu_renderer.hpp / ffi.rs), 384 bytes. The two
// pad words are load-bearing: they carry the header to the 16-byte boundary the
// TerrainUv rows need, and a skew here renders as the wrong ground, not as an
// error. Both sides carry a size assert; keep all three in step. 384 bytes after the 16-byte middle-color row; 368 bytes since
// RFG-065 appended the native Enfusion block at the end.
const TERRAIN_SURFACE_SLOTS: u32 = 6u;
struct TerrainMaterial {
    legacy: u32,
    satellite: u32,
    mask: u32,
    tile_normal: u32,
    // Surface colours BY SLOT: slot k is the source's Stage(4 + 2k). 0 is a hole
    // the source left empty, not a surface -- slot 0 of the bindless array is the
    // white fallback, so every one of these is tested before it is sampled.
    surfaces: array<u32, 6>,
    surface_normals: array<u32, 6>,
    surface_count: u32,
    uv_source_mask: u32,
    legacy_satellite_generated: u32, // same offset as the existing C++/Rust _pad0 lane
    legacy_detail_normal: u32,
    satellite_uv: TerrainUv,
    mask_uv: TerrainUv,
    surface_uvs: array<TerrainUv, 6>,
    // RFG-065 -- the native Enfusion surface block. `surface_count` is 0 on those
    // materials (an `.emat` has no LCA mask and no satellite), so these ride the
    // LEGACY branch, not the authored one.
    enfusion: u32,
    detail_scale: f32,
    middle: u32,
    middle_scale: f32,
    middle_blend: f32,
    detail_max: f32,
    detail_fade: f32,
    legacy_detail_scale: f32,
    // MiddleColor: a LINEAR multiplier on the middle map's texel (1,1,1 when the
    // material names none). Own 16-byte row; puddle_flags closes it to 384 bytes.
    middle_color: vec3<f32>,
    puddle_flags: u32, // former padding: ground bit 0; legacy/native soft soil bit 1; authored soft slots bits 8..13
};
