#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

// Enfusion's `resourceDatabase.rdb` -- the authoritative GUID -> resource path
// index, one per addon, sitting loose beside the `.pak` rather than inside it.
//
// WHY THIS EXISTS. The importer used to rebuild that mapping by text-scanning
// every `.et` / `.emat` / `.conf` in the corpus for `{GUID}path` references. That
// recovers 51,452 GUIDs and misses the ones no text file happens to mention: on
// Everon, 562 prefab GUIDs carrying 13,449 placements resolved to nothing, and a
// rescan returned the identical count because the map was not stale, it was
// incomplete by construction. Among the missing was
// `Prefabs/Structures/Cultural/Churches/Church_01/Church_01_blue_nocellar.et`,
// which is why a town on Everon had no church. The two `.rdb` files ship 110,860
// entries between them.
//
// FORMAT, established by byte accounting rather than by documentation. Both files
// close EXACTLY -- core 81,645/81,645, data 11,509,935/11,509,935.
//
//   "FORM"                 magic
//   u32 formSize           BIG-endian, as IFF; + 8 == the file size
//   "RDBC"                 form type
//   u32 version            7 across the corpus
//   u32 totalSize          little-endian, == the file size
//   <header fields>        NOT fully decoded -- see the note on FindRecords below
//   <record stream>        to EOF, exactly
//
// A record is:
//
//   u32 nameLen            includes the terminating NUL
//   char name[nameLen]     resource path, NUL-terminated
//   u16 type               4, 5, 6, 7 and 22 occur
//   u32 flags              zero throughout the corpus
//   u8  guid[8]            LITTLE-endian; the textual {…} form is these reversed
//   u8  extra[8]           PRESENT ONLY WHEN type >= 6
//
// That last line is the whole trick and it is not guessable from one record: types
// 4 and 5 are 14 bytes past the name and types 6, 7 and 22 are 22. Assuming one
// stride desynchronises the walk a few hundred records in and then produces
// plausible-looking garbage rather than an error, which is why the reader reports
// `consumed` and callers should require it to equal the file size.
//
// Directories (types 4 and 5) carry a GUID too and are kept: they cost little and
// a caller resolving a folder reference would otherwise see a hole.

namespace Poseidon::Asset::Formats::Enfusion
{

struct ResourceDatabase
{
    uint32_t version = 0;
    //! GUID (uppercase hex, as Enfusion writes it in braces) -> resource path.
    std::unordered_map<std::string, std::string> byGuid;
    size_t records = 0;
    size_t consumed = 0; //!< bytes the walk accounted for; must equal the file size
    std::string error;

    bool valid() const { return error.empty(); }
    bool closes() const { return error.empty() && consumed == size; }
    size_t size = 0; //!< the file size the walk was given
};

namespace ResourceDatabaseDetail
{

//! One record's total size, or 0 if `at` does not begin a well-formed one.
inline size_t RecordSize(const uint8_t* data, size_t size, size_t at, std::string* path = nullptr,
                         std::string* guid = nullptr)
{
    if (at + 4 > size)
        return 0;
    uint32_t nameLen = 0;
    std::memcpy(&nameLen, data + at, 4);
    // 1024 is a sanity bound, not a format limit: the longest path in either
    // shipped database is well under it, and an absurd length is how a
    // desynchronised walk announces itself instead of allocating wildly.
    if (nameLen < 2 || nameLen > 1024 || at + 4 + nameLen + 14 > size)
        return 0;
    const uint8_t* name = data + at + 4;
    if (name[nameLen - 1] != 0)
        return 0;
    for (uint32_t i = 0; i + 1 < nameLen; ++i)
        if (name[i] < 32)
            return 0;

    const size_t tail = at + 4 + nameLen;
    uint16_t type = 0;
    std::memcpy(&type, data + tail, 2);
    const size_t tailSize = type >= 6 ? 22 : 14;
    if (tail + tailSize > size)
        return 0;

    if (path != nullptr)
        path->assign(reinterpret_cast<const char*>(name), nameLen - 1);
    if (guid != nullptr)
    {
        static const char* kHex = "0123456789ABCDEF";
        const uint8_t* eight = data + tail + 6;
        guid->clear();
        guid->reserve(16);
        for (int i = 7; i >= 0; --i)
        {
            guid->push_back(kHex[(eight[i] >> 4) & 0xF]);
            guid->push_back(kHex[eight[i] & 0xF]);
        }
    }
    return 4 + nameLen + tailSize;
}

//! Walks records from `at`, returning how many closed and where it stopped.
inline size_t Walk(const uint8_t* data, size_t size, size_t at, size_t limit,
                   std::unordered_map<std::string, std::string>* out, size_t* count)
{
    size_t n = 0;
    while (at < size)
    {
        std::string path, guid;
        const size_t step =
            RecordSize(data, size, at, out != nullptr ? &path : nullptr, out != nullptr ? &guid : nullptr);
        if (step == 0)
            break;
        if (out != nullptr)
            out->emplace(std::move(guid), std::move(path));
        at += step;
        if (++n >= limit)
            break;
    }
    if (count != nullptr)
        *count = n;
    return at;
}

} // namespace ResourceDatabaseDetail

//! Reads a `resourceDatabase.rdb`. Check `closes()`, not just `valid()`.
inline ResourceDatabase ReadResourceDatabase(const void* data, size_t size)
{
    using namespace ResourceDatabaseDetail;
    ResourceDatabase out;
    out.size = size;
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    if (size < 20 || std::memcmp(bytes, "FORM", 4) != 0 || std::memcmp(bytes + 8, "RDBC", 4) != 0)
    {
        out.error = "not a FORM/RDBC container";
        return out;
    }
    std::memcpy(&out.version, bytes + 12, 4);

    // The header between "RDBC" and the record stream is NOT decoded: it holds at
    // least a `license` key whose value block does not follow the record layout,
    // and its length differs between the two shipped files (the table starts at
    // 0x3F in core and 0x3E in data), so a fixed offset would work on one and
    // silently mis-read the other.
    //
    // The start is therefore FOUND, by the same criterion that validates the rest:
    // the first offset from which the record walk consumes the file exactly. A
    // cheap prefix probe picks the candidate, then one full walk has to close --
    // so a wrong guess is rejected rather than believed.
    constexpr size_t kProbeRecords = 64;
    constexpr size_t kMaxHeader = 64 * 1024;
    const size_t searchEnd = size < kMaxHeader ? size : kMaxHeader;
    for (size_t start = 12; start < searchEnd; ++start)
    {
        size_t probed = 0;
        Walk(bytes, size, start, kProbeRecords, nullptr, &probed);
        if (probed < kProbeRecords)
            continue;

        std::unordered_map<std::string, std::string> table;
        size_t count = 0;
        const size_t end = Walk(bytes, size, start, static_cast<size_t>(-1), &table, &count);
        if (end != size)
            continue;
        out.byGuid = std::move(table);
        out.records = count;
        out.consumed = end;
        return out;
    }
    out.error = "no record table start makes the walk consume the file exactly";
    return out;
}

} // namespace Poseidon::Asset::Formats::Enfusion
