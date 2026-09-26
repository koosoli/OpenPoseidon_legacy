#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Poseidon
{
namespace Model
{
struct Model;
}

// AST-004 -- a byte serialisation for the P3D -> Model IR, so the derived data
// cache has something to store.
//
// WHY THIS EXISTS AT ALL. `DerivedBlobStore` (AST-002) stores bytes and knows
// nothing about meshes; `QFBank::GetContentHash` (AST-003) can key one PBO
// member. The measured gap between them was a producer whose derivation is
// expensive enough to be worth caching. Measured on retail `AddOns\O.pbo`
// (see the [ast004cost] case): P3D -> Model IR costs 9.62 ms per model against
// 0.45 ms to SHA-256 the member, where a PAA block chain costs 0.019 ms and a
// full RGBA decode 0.28 ms -- both CHEAPER than their own key. The model path is
// the only one of the four where the numbers justify a cache, and it was the one
// with no serialisation. Hence this file.
//
// THREE RULES THE FORMAT FOLLOWS, each of which is a bug it would otherwise have.
//
//   1. EVERY FIELD IS WRITTEN EXPLICITLY, never by memcpy of a struct. `Vertex`,
//      `Triangle` and friends contain padding, and padding bytes are whatever the
//      allocator last left there. A memcpy serialisation would put uninitialised
//      memory into the payload, so two runs over identical input would produce
//      different bytes and the "cold and warm are byte-identical" test would be
//      testing the allocator. It also makes the format independent of the
//      compiler's layout choices.
//
//   2. FIXED WIDTHS, LITTLE-ENDIAN, EXPLICIT. Sizes are u32/u64 with a defined
//      byte order rather than whatever `size_t` is, so an entry is not silently
//      reinterpreted by a different build.
//
//   3. THE READER TRUSTS NOTHING. Every count is checked against the bytes
//      remaining before it is used to size an allocation, and any inconsistency
//      returns false rather than throwing. A damaged blob must be a MISS, not a
//      partly-filled Model -- the store's payload digest catches corruption, but
//      this reader is the second line and is what makes `Deserialize` safe to
//      point at arbitrary bytes.
//
// WHAT IT IS NOT. It is not an asset format and nothing outside the cache should
// read or write it. Base-layout changes bump kFormatVersion. The optional,
// independently versioned source-audit footer preserves the existing base/key:
// new readers accept old exact base blobs with Unknown audit; old readers reject
// new trailing data and fall back to parsing. Missing facts never mean static.
namespace ModelBlob
{

inline constexpr char     kMagic[4]      = {'P', 'M', 'D', 'L'};
// Bump on changes to the BASE layout. Optional audit facts use their own tagged
// version below; retaining the base version/key keeps old blobs readable without
// treating absent audit as known absence of motion.
inline constexpr uint32_t kFormatVersion = 1;
inline constexpr uint32_t kSourceAuditTag = 0x44554153; // "SAUD", little endian
inline constexpr uint32_t kSourceAuditVersion = 1;
inline constexpr uint32_t kSourceAuditPayloadBytes = 48;
inline constexpr uint32_t kSourceAuditFooterBytes = 12 + kSourceAuditPayloadBytes;

// Appends the model's whole IR to `out`. Deterministic: the same Model always
// produces the same bytes, and no field is read from memory the writer did not
// set.
void Serialize(const Poseidon::Model::Model& model, std::vector<uint8_t>& out);

std::vector<uint8_t> Serialize(const Poseidon::Model::Model& model);

// Fills `out` from `data`. False -- with `out` left in an unspecified but valid
// state the caller must discard -- on wrong magic, wrong version, a truncated
// buffer, or any count that does not fit the bytes remaining.
bool Deserialize(const void* data, size_t size, Poseidon::Model::Model& out);

} // namespace ModelBlob
} // namespace Poseidon
