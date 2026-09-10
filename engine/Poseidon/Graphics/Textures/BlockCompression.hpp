#pragma once

#include <cstddef>
#include <cstdint>

// The DXGI-era block formats: BC4, BC5 and BC7.
//
// Why these three and not the whole family: Arma Reforger's texture corpus is
// BC7-dominated in a way the Arma/DayZ corpora never were. Measured over a
// stratified 1,692-file sample of its 29,026 `.edds` (ARF-001), the formats this
// build could not decode broke down as BC7 1,372 (87.2% of all failures), BC4 122,
// BC1 34, BC5 30. BC1 and BC3 already have decoders and only needed the DXGI
// enumerators routed onto them, so BC4, BC5 and BC7 are the entire remaining codec
// gap, and BC7 alone is most of it.
//
// All three write straight RGBA8 in memory order, matching what `Image` and the
// texture bank already expect from the DXT decoders beside them.
//
// BC6H (HDR) is deliberately absent: it does not occur in the sampled corpus, and
// a decoder with no input to check it against is a liability rather than coverage.

namespace Poseidon
{

// One 4x4 block, 8 bytes, one channel. This is exactly the algorithm DXT5 already
// uses for its alpha channel -- two endpoints plus 3-bit indices, with the
// endpoint ordering selecting a 6- or 4-interpolant palette -- which is why BC4
// and BC5 come almost free once it is factored out.
//
// `stride` is the distance in bytes between consecutive output samples, so the
// same routine can scatter into one channel of an interleaved RGBA image.
void DecodeBc4Block(const uint8_t* block, uint8_t* out, size_t rowPitch, size_t stride, int usableW, int usableH);

// Two BC4 blocks back to back, 16 bytes: red then green. The blue and alpha
// channels are left untouched by the decode itself -- a BC5 texture carries no
// information about them, and inventing Z here would bake a normal-map convention
// into a general decoder. `DecodeBc5Image` fills B=0, A=255.
void DecodeBc5Block(const uint8_t* block, uint8_t* out, size_t rowPitch, size_t stride, int usableW, int usableH);

// One 4x4 BC7 block, 16 bytes, RGBA out. Returns false only for the reserved
// mode (a byte-0 of zero, i.e. no mode bit set at all), which no encoder emits.
bool DecodeBc7Block(const uint8_t* block, uint8_t* out, size_t rowPitch, int usableW, int usableH);

// Whole-image helpers. `dst` is width*height*4 RGBA8. `src` must hold
// ceil(w/4)*ceil(h/4) blocks of the format's block size.
void DecodeBc4Image(const uint8_t* src, uint8_t* dst, int width, int height);
void DecodeBc5Image(const uint8_t* src, uint8_t* dst, int width, int height);
bool DecodeBc7Image(const uint8_t* src, uint8_t* dst, int width, int height);

class TaskPool;
// Synchronous, disjoint block-row batches. Call only from the pool's owner thread.
// Input/output storage remains owned by the caller until all batches finish.
bool DecodeBc7ImageBatched(const uint8_t* src, uint8_t* dst, int width, int height, TaskPool& pool);

} // namespace Poseidon
