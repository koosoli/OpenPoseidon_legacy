#pragma once

#include <vector>
#include <cstdint>
#include <string>

namespace Poseidon
{

struct DecodedImage
{
    std::vector<uint8_t> rgba;
    int width = 0;
    int height = 0;
    bool valid() const { return width > 0 && height > 0 && !rgba.empty(); }
};

// The TAGG block a PAA carries before its mip chain (AST-013).
//
// Previously skipped wholesale. They are not decoration: SWIZ says how stored
// channels map onto meaning, and FLAG carries the alpha/transparency policy, so
// discarding them means guessing both. Measured across 3,283 archived Arma 3
// textures: AVGC/MAXC/OFFS on every one, FLAG on 1,345, SWIZ on 1,146.
struct PAATaggs
{
    bool     hasAvgColor = false;
    uint32_t avgColor    = 0;      // AVGC, ARGB
    bool     hasMaxColor = false;
    uint32_t maxColor    = 0;      // MAXC, ARGB
    bool     hasFlags    = false;
    uint32_t flags       = 0;      // FLAG; observed values 1 and 2
    bool     hasSwizzle  = false;
    uint8_t  swizzle[4]  = {0, 0, 0, 0}; // SWIZ, one byte per output channel
    bool     hasOffsets  = false;        // OFFS, per-mip file offsets

    // Anything not recognised, kept by name so a fixture that needs it is visible
    // rather than silently dropped. AST-013 asks for lesser tags to be captured
    // and reported until a real fixture shows ignoring one changes correctness.
    std::vector<std::string> unknown;

    // SWIZ is recorded but NOT applied: the byte encoding is ambiguous across the
    // observed values (0x00-0x09 appear, which does not fit a plain
    // channel-plus-invert reading) and no local fixture settles it. Applying a
    // guessed mapping would silently corrupt every normal map it touched.
    bool isIdentitySwizzle() const
    {
        return !hasSwizzle || (swizzle[0] == 0 && swizzle[1] == 1 && swizzle[2] == 2 && swizzle[3] == 3);
    }
};

struct PAAInfo
{
    std::string path;
    uint16_t magic = 0;
    bool isPaa = false;
    int width = 0;
    int height = 0;
    int mipmapCount = 0;
    int paletteColors = 0;
    const char* formatName = nullptr;
    bool hasTransparentBlocks = false; // DXT1: any blocks with c0 <= c1 (1-bit alpha mode)
    PAATaggs taggs;

    // A mip whose stored width has bit 15 set holds compressed data. Recognising
    // the flag is what stops the dimension being read as 0x8800 = 34816 and
    // reported as an "extreme texture size" -- a diagnostic that blamed the size
    // when the real problem is an unsupported compression.
    bool hasCompressedMips = false;
};

// Read metadata without decoding pixel data
bool ReadPAAInfo(const std::string& path, PAAInfo& info);

// A block-compressed PAA's whole mip chain, as the GPU wants it: raw DXT blocks per level,
// largest first, tightly packed in one buffer. This is the worker-thread half of texture
// streaming (ObjectStreamPreparer): it is PURE -- std::ifstream, std::vector and
// Foundation::Lzo1x only; no file server, no texture bank, no logging, no statics -- so it may
// run on any thread. The main thread's TextureWgpu::EnsureUploaded consumes it in place of its
// own file read + LZO, then does the upload itself (see Graphics/Textures/PreparedTextures.hpp).
//
// DXT1/2/3/4/5 only. Every other PAA format (ARGB4444, AI88, palette PAC...) returns false and
// the caller takes the ordinary path; those go through the RGBA8 whole-file decode on the main
// thread and are not what a Reforger/Arma-3 world streams.
struct PAABlockLevel
{
    int width = 0;
    int height = 0;
    size_t offset = 0; // into PAABlockChain::blocks
    size_t size = 0;   // bytes of raw blocks at that offset
    // Source-member-relative level HEADER position, not the decoded block
    // offset above. Unknown for synthetic/in-memory chains without parser proof.
    size_t sourceHeaderOffset = SIZE_MAX;
    bool legacyBlockReadCompatible = false;
};

struct PAABlockChain
{
    uint16_t magic = 0;   // 0xFF01 DXT1 ... 0xFF05 DXT5
    int width = 0;        // level 0
    int height = 0;
    bool hadLzoLevels = false;
    std::vector<PAABlockLevel> levels; // [0] is the largest
    std::vector<uint8_t> blocks;       // all levels, contiguous, in `levels` order
    bool valid() const { return width > 0 && height > 0 && !levels.empty() && !blocks.empty(); }
};

// Returns false (and leaves `out` invalid) on: unopenable file, non-DXT format, malformed
// header/chain, an LZO level that did not inflate to its dimensions' block size, or a level
// whose stored size disagrees with its dimensions. `errorOut` (optional) names which.
bool ReadPAABlockChain(const std::string& path, PAABlockChain& out, std::string* errorOut = nullptr);
// Optional output bound is checked BEFORE allocating/decompressing each level.
bool ReadPAABlockChainBuffer(const void* data, size_t size, PAABlockChain& out,
                            std::string* errorOut = nullptr, size_t maxBlockBytes = SIZE_MAX);

// Decode PAA/PAC file to RGBA8888 pixel buffer
DecodedImage DecodePAAFile(const std::string& path);

// Decode specific mipmap level
DecodedImage DecodePAAFileMip(const std::string& path, int mipLevel);

// Decode from memory buffer (for embedded/archive use)
DecodedImage DecodePAABuffer(const void* data, size_t size, bool isPaa);

// Three-way alpha classification of a decoded RGBA8888 buffer. This is the
// per-texture signal a section-sort renderer needs (ArmA1-style): only a Blend
// texture (partial-alpha texels present) must be deferred to the back-to-front
// pass; Opaque and Cutout occlude (write depth, alpha-test the holes).
struct AlphaStats
{
    enum Kind
    {
        Opaque, // alpha essentially all 255
        Cutout, // fully-transparent holes (a=0), no meaningful partial alpha
        Blend   // partial-alpha texels (0 < a < 255) present
    };
    Kind kind = Opaque;
    int aMin = 255, aMax = 255, aMean = 255;
    double pctClear = 0.0, pctOpaque = 100.0, pctPartial = 0.0; // % of texels a=0 / a=255 / 0<a<255
    // % of texels with 16 <= a <= 239: partial alpha that is a real intermediate opacity,
    // not the 1..15 / 240..254 fringe a filtered or dithered edge leaves around a mask.
    // Genuine translucency (glass, smoke) is mostly here; a solid surface with a soft
    // seam or a few decal edges is almost never here. Purely additive to the verdict.
    double pctMid = 0.0;
};

// Classify the alpha channel of an RGBA8888 buffer (4 bytes/texel, alpha = byte 3).
// `partialThreshold`/`clearThreshold` are the percentages above which partial / clear
// texels are considered significant (default 2.0%, tolerating AA-edge noise).
AlphaStats ClassifyAlpha(const uint8_t* rgba, size_t pixelCount, double partialThreshold = 2.0,
                         double clearThreshold = 2.0);

// Same histogram and policy as ClassifyAlpha; alpha points to the first sample.
// stride=1 accepts a compact alpha plane, stride=4 an RGBA channel.
AlphaStats ClassifyAlphaSamples(const uint8_t* alpha, size_t pixelCount, size_t stride,
                               double partialThreshold = 2.0, double clearThreshold = 2.0);

// Decode only the alpha half of BC2 (bc3=false) or BC3 blocks, one byte/texel.
// Rejects invalid dimensions/truncated input before allocating; edge blocks are clamped.
// Colour bytes are never decoded. On failure out is empty.
bool DecodeBlockAlpha(const uint8_t* blocks, size_t size, int width, int height, bool bc3,
                      std::vector<uint8_t>& out);

const char* AlphaKindName(AlphaStats::Kind kind);

// Decide a texture's alpha class from its cheap header flags + format, deferring
// to a full decoded-alpha scan only when the format can carry partial alpha. This
// is the tiering policy a section-sort renderer uses at texture-load time:
//   - no alpha channel        -> Cutout if chroma-keyed, else Opaque   (no decode)
//   - 1-bit-alpha format       -> Cutout (punch-through holes only)      (no decode)
//   - multi-bit-alpha format   -> `decoded` verdict (caller must decode + ClassifyAlpha)
// `decoded` is null when the caller has not (or could not) decode; the safe fallback
// is Opaque (preserves occlusion / batching — never routes an unknown to the blend pass).
AlphaStats::Kind ClassifyTextureAlpha(bool hasAlpha, bool isChromaKey, bool oneBitAlphaFormat,
                                      const AlphaStats* decoded);

// AI88 is a legacy intensity+coverage format. An entirely clear AI88 mask is
// intentionally invisible; the generic histogram's all-clear fallback exists
// for modern textures that may pack non-coverage data in their alpha channel.
AlphaStats::Kind LegacyAi88CoverageClass(bool isAi88, const AlphaStats& stats);

} // namespace Poseidon
