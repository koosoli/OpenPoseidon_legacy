#pragma once
#include <Poseidon/IO/Streams/ArchiveSourceBinding.hpp>
#include <Poseidon/Graphics/Textures/PAADecoder.hpp>
#include <Poseidon/Graphics/Textures/PreparedDdsPublication.hpp>
#include <array>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

namespace Poseidon::render
{
// Explicit experiment. No global texture cache, borrowed Texture, config or file-server state.
inline bool ColdPaaHandoffEnabled()
{
    static const bool enabled = [] { const char* v = std::getenv("WGR_OBJECT_STREAM_COLD_TEXTURE_HANDOFF");
        return v && std::strcmp(v, "1") == 0; }();
    return enabled;
}
struct ColdPaaRead
{
    static constexpr size_t Limit = 16 * 1024 * 1024;
    static constexpr size_t MaxLevels = 7;
    struct Level { int width = 0, height = 0; size_t bytes = 0, header = SIZE_MAX; };
    std::string key;
    std::shared_ptr<const ArchiveSourceBinding> source;
    uint16_t magic = 0;
    size_t count = 0;
    std::array<Level, MaxLevels> levels{};
    bool Valid() const
    {
        if (!source || !source->Request().archiveLease || source->Request().bytes > Limit ||
            !source->Request().bytes || key.empty() || key.size() > 8191 || key.find('\0') != std::string::npos ||
            count == 0 || count > MaxLevels || magic < 0xff01 || magic > 0xff05) return false;
        size_t sum = 0;
        for (size_t i = 0; i < count; ++i) {
            const auto& l = levels[i];
            if (l.width <= 0 || l.height <= 0 || l.header == SIZE_MAX || !l.bytes || l.bytes > Limit - sum) return false;
            sum += l.bytes;
        }
        return true;
    }
    size_t KnownMetadataBytes() const
    { return sizeof(*this) + key.capacity() + 1 + (source ? source->KnownCppBytes() : 0) + 128; }
    bool Matches(const PAABlockChain& chain) const
    {
        if (!Valid() || !chain.valid() || chain.magic != magic || chain.levels.size() < count ||
            chain.blocks.capacity() > Limit || chain.levels.capacity() > 64) return false;
        size_t offset = 0;
        for (size_t i = 0; i < count; ++i) {
            const auto& l = levels[i]; const auto& p = chain.levels[i];
            if (!p.legacyBlockReadCompatible || p.sourceHeaderOffset != l.header ||
                p.width != l.width || p.height != l.height || p.size != l.bytes || p.offset != offset ||
                p.offset > chain.blocks.size() || p.size > chain.blocks.size() - p.offset) return false;
            offset += p.size;
        }
        return true;
    }
};
struct ColdPaaOwned
{
    static constexpr uint64_t ReadyLimit = 128ull * 1024 * 1024; // optional chains only, independent of an unlimited legacy budget
    ColdPaaRead read;
    DdsPublicationToken publication;
    PAABlockChain chain;
    // Intrusive retirement list: Reset/DropStale move payloads out of the queue lock without allocating.
    std::unique_ptr<ColdPaaOwned> retiredNext;
    size_t KnownBytes() const { return sizeof(*this) + read.KnownMetadataBytes() + chain.blocks.capacity() +
        chain.levels.capacity() * sizeof(PAABlockLevel) + (publication.inventory ? publication.inventory->MetadataBytes() : 0); }
};
// Same immutable source/bank-header snapshot; current mount identity must be checked by the
// caller before entering Claim. Token visibility is checked again immediately before transfer.
// Successful decoder bytes only. Cancellation cannot interrupt an indivisible LZO invocation.
template<class Cancelled> std::unique_ptr<ColdPaaOwned> PrepareColdPaa(
    ColdPaaRead read, DdsPublicationToken publication, Cancelled cancelled)
{
    if (!read.Valid() || !publication.Valid() || cancelled()) return {};
    try {
        std::vector<char> input;
        if (!read.source->Request().Read(input) || input.capacity() > ColdPaaRead::Limit || cancelled()) return {};
        PAABlockChain chain;
        if (!ReadPAABlockChainBuffer(input.data(), input.size(), chain, nullptr, ColdPaaRead::Limit) ||
            !read.Matches(chain) || cancelled() || !publication.Valid()) return {};
        // Source and final retained chain capacities each <=16 MiB. Decoder logical output
        // is bounded before growth; 64 MiB remains the existing scheduling estimate,
        // not an allocator-overhead, native-handle or process-RSS theorem.
        std::vector<char>().swap(input);
        auto result = std::make_unique<ColdPaaOwned>();
        result->read = std::move(read); result->publication = std::move(publication); result->chain = std::move(chain);
        return result;
    } catch (...) { return {}; } // Optional preparation failure never changes ordinary conversion success.
}
class ColdPaaAdmissionScope
{
    inline static thread_local ColdPaaOwned* current = nullptr;
    ColdPaaOwned* previous = nullptr;
    bool entered = false;
public:
    // Caller: joined owner, no yielding, exactly the admission that took this ConvertedShape.
    // A null scope leaves TLS untouched. No payload/ref/pointer escapes a claim.
    explicit ColdPaaAdmissionScope(ColdPaaOwned* payload) : previous(payload ? current : nullptr), entered(payload != nullptr)
    { if (entered) current = payload; }
    ~ColdPaaAdmissionScope() { if (entered) current = previous; }
    ColdPaaAdmissionScope(const ColdPaaAdmissionScope&) = delete;
    ColdPaaAdmissionScope& operator=(const ColdPaaAdmissionScope&) = delete;
    static bool Claim(const std::string& key, const std::shared_ptr<const ArchiveSourceBinding>& source,
        const ColdPaaRead& currentHeader, PAABlockChain& out)
    {
        auto* p = current;
        if (!p || !source || !p->publication.Valid() || p->read.key != key ||
            !p->read.source->Request().SameArchiveMember(source->Request()) || !currentHeader.Matches(p->chain)) return false;
        out = std::move(p->chain); // Exactly once; an empty chain never matches again.
        return true;
    }
};
}
