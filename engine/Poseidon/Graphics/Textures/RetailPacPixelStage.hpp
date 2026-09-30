#pragma once

#include <Poseidon/Graphics/Textures/ColdPaaHandoff.hpp>
#include <Poseidon/Foundation/Algorithms/Sha256.hpp>
#include <Poseidon/IO/Streams/RetailRigidAssetProfile.hpp>
#include <optional>
#include <string>
#include <utility>

namespace Poseidon { class TextureWgpu; }

namespace Poseidon::render
{
// One selected retail, exact-source BC1 pixel payload. Preparation reads only the
// retained Init-born archive lease and uses the existing pure DXT parser. It
// neither publishes a name-keyed cache entry nor touches a texture bank/GPU.
class RetailPacPrepared
{
    friend class ::Poseidon::TextureWgpu;
    ColdPaaRead _header;
    PAABlockChain _chain;
    std::string _rawSha256;
    RetailPacPrepared(ColdPaaRead header, PAABlockChain chain, std::string rawSha256)
        : _header(std::move(header)), _chain(std::move(chain)), _rawSha256(std::move(rawSha256)) {}
public:
    static constexpr size_t MaxSourceBytes = 128 * 1024;
    static constexpr size_t MaxBlockBytes = 128 * 1024;
    static bool MatchesSelectedSource(const ColdPaaRead& header)
    {
        const auto* profile = RetailRigidAssetProfile::Selected();
        return profile && header.source &&
            profile->MatchesTexturePath(header.key) &&
            profile->MatchesTextureArchive(header.source->Request().archive);
    }
    RetailPacPrepared(const RetailPacPrepared&) = delete;
    RetailPacPrepared& operator=(const RetailPacPrepared&) = delete;
    RetailPacPrepared(RetailPacPrepared&&) = default;
    RetailPacPrepared& operator=(RetailPacPrepared&&) = default;

    template<class Cancelled>
    static std::optional<RetailPacPrepared> Prepare(ColdPaaRead header, Cancelled cancelled)
    {
        if (!MatchesSelectedSource(header) ||
            header.key.capacity() > 256 ||
            !header.Valid() || header.magic != 0xff01 || header.count > 7 ||
            header.source->Request().bytes > MaxSourceBytes || cancelled()) return {};
        try
        {
            std::vector<char> raw;
            if (!header.source->Request().Read(raw) || raw.empty() ||
                raw.capacity() > MaxSourceBytes || cancelled()) return {};
            PAABlockChain chain;
            if (!ReadPAABlockChainBuffer(raw.data(), raw.size(), chain, nullptr, MaxBlockBytes) ||
                chain.magic != 0xff01 || chain.blocks.capacity() > MaxBlockBytes ||
                chain.levels.capacity() > 32 ||
                !header.Matches(chain) || cancelled()) return {};
            auto rawSha256 = Foundation::Sha256::Of(raw.data(), raw.size());
            if (cancelled()) return {};
            return RetailPacPrepared(std::move(header), std::move(chain), std::move(rawSha256));
        }
        catch (...) { return {}; }
    }
    static std::optional<RetailPacPrepared> Prepare(ColdPaaRead header)
    { return Prepare(std::move(header), [] { return false; }); }
    bool Valid() const
    {
        return MatchesSelectedSource(_header) &&
            _header.key.capacity() <= 256 && _header.Valid() && _header.magic == 0xff01 && _header.count <= 7 &&
            _header.source->Request().bytes <= MaxSourceBytes && _chain.magic == 0xff01 &&
            _chain.blocks.capacity() <= MaxBlockBytes && _chain.levels.capacity() <= 32 &&
            _header.Matches(_chain) && _rawSha256.size() == 64 && _rawSha256.capacity() <= 128;
    }
    const ColdPaaRead& Header() const { return _header; }
    const std::string& RawSha256() const { return _rawSha256; }
    size_t BlockBytes() const { return _chain.blocks.size(); }
    size_t KnownBytes() const
    {
        return sizeof(*this) + _header.key.capacity() + 1 + _rawSha256.capacity() + 1 +
            _chain.blocks.capacity() + _chain.levels.capacity() * sizeof(PAABlockLevel) +
            (_header.source ? _header.source->KnownCppBytes() : 0);
    }
};
}
