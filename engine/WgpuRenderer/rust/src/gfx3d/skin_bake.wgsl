// Compute skinning "bake" (docs/compute-skin-bake-plan.md). Skins every skinned
// vertex ONCE per frame into a shared vertex buffer, so the shadow cascades, the
// depth prepass, and the forward pass all read plain pos/norm/uv instead of
// re-evaluating linear-blend skinning in the VS 5-6x/frame. The output is
// byte-identical to WgrMeshVertex (36 B / 9 words = pos+norm+uv+conform), a
// drop-in for the rigid pipelines' vertex buffer — a baked instance draws through
// vs_main / vs_solid with an identity world and base_vertex = its slice offset.
//
// Vertices are read/written as array<u32> (not array<f32>) so the uv + conform
// words pass through BIT-EXACT (a bitcast, not an f32 copy that could flush the
// conform word's denormal bit-pattern to zero). Skinning math is inlined here (a
// copy of skin.wgsl's linear blend) against a STORAGE palette indexed by absolute
// block, rather than #importing skin.wgsl, whose palette is the fallback VS path's
// dynamic-offset UBO.

// One WgrMeshVertex = 17 u32: [pos.xyz, norm.xyz, uv.xy, conform, tangent.xyz,
// binormal.xyz, uv1.xy]. THE STRIDE IS THE WHOLE BUG CLASS: this shader was written
// (and validated) when the vertex was 9 words, the vertex later grew to 17 for the
// Multi material stages, and the packed 9-word output it kept writing is exactly the
// interleaved garbage every 17-word reader then shattered on (2026-08-30 hunt).
const WORDS_PER_VERT: u32 = 17u;
// The engine's bone-palette cap (skin.wgsl: MATRIX_4_ARRAY(matrix, 128)).
const PALETTE_BLOCK: u32 = 128u;

@group(0) @binding(0) var<storage, read>       in_v:    array<u32>;         // source mesh verts (9 u32/vertex)
@group(0) @binding(1) var<storage, read>       in_s:    array<u32>;         // skin data (2 u32/vertex: bones, weights)
@group(0) @binding(2) var<storage, read>       palette: array<mat4x4<f32>>; // all blocks; world pre-multiplied in
@group(0) @binding(3) var<storage, read_write> out_v:   array<u32>;         // baked verts (9 u32/vertex)

struct BakeParams {
    vert_count: u32,      // vertices per instance (the source mesh's count)
    instance_count: u32,  // baked instances of this mesh (Phase 1: always 1)
    palette_base: u32,    // absolute block index of instance 0 (== palette_slot in Phase 1)
    out_base_vertex: u32, // first output vertex of this group in the mega buffer
    in_base_vertex: u32,  // first SOURCE vertex of this mesh in the shared pool vbuf
    // Tail padding to a 16-byte multiple, mirroring BakeParamsGpu's `_pad: [u32; 3]`.
    _pad0: u32,
    _pad1: u32,
    _pad2: u32,
};
@group(1) @binding(0) var<uniform> gp: BakeParams;

@compute @workgroup_size(64)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
    let total = gp.vert_count * gp.instance_count;
    if (gid.x >= total) {
        return;
    }
    let inst = gid.x / gp.vert_count;
    let v = gid.x % gp.vert_count;

    // Source verts live in the shared geometry pool at in_base_vertex; skin data (in_s)
    // stays a per-mesh standalone buffer, so it is indexed mesh-local by `v`.
    let i = (gp.in_base_vertex + v) * WORDS_PER_VERT;
    let pos = vec3<f32>(bitcast<f32>(in_v[i]), bitcast<f32>(in_v[i + 1u]), bitcast<f32>(in_v[i + 2u]));
    let norm = vec3<f32>(bitcast<f32>(in_v[i + 3u]), bitcast<f32>(in_v[i + 4u]), bitcast<f32>(in_v[i + 5u]));

    // Skin data: bones = Uint8x4 (byte 0 in the low bits), weights = Unorm8x4.
    // Unpacked by hand so there is no unpack4xU8 backend dependency.
    let bw0 = in_s[v * 2u];
    let bw1 = in_s[v * 2u + 1u];
    let bones = vec4<u32>(bw0 & 0xffu, (bw0 >> 8u) & 0xffu, (bw0 >> 16u) & 0xffu, (bw0 >> 24u) & 0xffu);
    let weights = vec4<f32>(
        f32(bw1 & 0xffu) / 255.0,
        f32((bw1 >> 8u) & 0xffu) / 255.0,
        f32((bw1 >> 16u) & 0xffu) / 255.0,
        f32((bw1 >> 24u) & 0xffu) / 255.0,
    );

    // Absolute matrix base for this instance's block. palette[base + bone] already
    // has the caster/draw's camera-relative world pre-multiplied in, so the baked
    // position/normal are in the SAME camera-relative world space every pass agrees
    // on (shadow applies light_vp, forward applies proj*view) — one bake, all passes.
    let base = (gp.palette_base + inst) * PALETTE_BLOCK;
    let p = vec4<f32>(pos, 1.0);
    let sp = weights.x * (palette[base + bones.x] * p)
        + weights.y * (palette[base + bones.y] * p)
        + weights.z * (palette[base + bones.z] * p)
        + weights.w * (palette[base + bones.w] * p);
    // Normal uses the same palette matrices (no inverse-transpose) — behaviour-
    // preserving vs. the VS path, do not "fix" here. `norm` is already the negated
    // stored normal (SetSkinData uploads -OrigNorm); rigid vs_main lights it as-is.
    let n = vec4<f32>(norm, 0.0);
    let sn = weights.x * (palette[base + bones.x] * n).xyz
        + weights.y * (palette[base + bones.y] * n).xyz
        + weights.z * (palette[base + bones.z] * n).xyz
        + weights.w * (palette[base + bones.w] * n).xyz;

    // Tangent frame: skinned with the same palette blend as the normal (w = 0
    // directions, no inverse-transpose - behaviour-preserving vs the VS path).
    let tan = vec3<f32>(bitcast<f32>(in_v[i + 9u]), bitcast<f32>(in_v[i + 10u]), bitcast<f32>(in_v[i + 11u]));
    let bin = vec3<f32>(bitcast<f32>(in_v[i + 12u]), bitcast<f32>(in_v[i + 13u]), bitcast<f32>(in_v[i + 14u]));
    let t4 = vec4<f32>(tan, 0.0);
    let st = weights.x * (palette[base + bones.x] * t4).xyz
        + weights.y * (palette[base + bones.y] * t4).xyz
        + weights.z * (palette[base + bones.z] * t4).xyz
        + weights.w * (palette[base + bones.w] * t4).xyz;
    let b4 = vec4<f32>(bin, 0.0);
    let sb = weights.x * (palette[base + bones.x] * b4).xyz
        + weights.y * (palette[base + bones.y] * b4).xyz
        + weights.z * (palette[base + bones.z] * b4).xyz
        + weights.w * (palette[base + bones.w] * b4).xyz;

    let o = (gp.out_base_vertex + inst * gp.vert_count + v) * WORDS_PER_VERT;
    out_v[o] = bitcast<u32>(sp.x);
    out_v[o + 1u] = bitcast<u32>(sp.y);
    out_v[o + 2u] = bitcast<u32>(sp.z);
    out_v[o + 3u] = bitcast<u32>(sn.x);
    out_v[o + 4u] = bitcast<u32>(sn.y);
    out_v[o + 5u] = bitcast<u32>(sn.z);
    out_v[o + 6u] = in_v[i + 6u]; // uv.x passthrough (bit-exact)
    out_v[o + 7u] = in_v[i + 7u]; // uv.y passthrough
    out_v[o + 8u] = in_v[i + 8u]; // conform passthrough
    out_v[o + 9u] = bitcast<u32>(st.x);
    out_v[o + 10u] = bitcast<u32>(st.y);
    out_v[o + 11u] = bitcast<u32>(st.z);
    out_v[o + 12u] = bitcast<u32>(sb.x);
    out_v[o + 13u] = bitcast<u32>(sb.y);
    out_v[o + 14u] = bitcast<u32>(sb.z);
    out_v[o + 15u] = in_v[i + 15u]; // uv1.x passthrough
    out_v[o + 16u] = in_v[i + 16u]; // uv1.y passthrough
}
