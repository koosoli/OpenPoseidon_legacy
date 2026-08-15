#include "TextureBankWgpu.hpp"

#include <algorithm>
#include <cstring>
#include <cstdlib>

#include <Poseidon/Foundation/Framework/Log.hpp>

namespace Poseidon
{

TextureBankWgpu::TextureBankWgpu(WgrRenderer* renderer) : _renderer(renderer) {}

TextureBankWgpu::~TextureBankWgpu()
{
    UnlockAllTextures();
    DeleteAllAnimated();
}

void TextureBankWgpu::BeginModelTextureCapture(std::vector<TextureWgpu*>& out)
{
    out.clear();
    _capture = &out;
}

void TextureBankWgpu::EndModelTextureCapture()
{
    _capture = nullptr;
}

void TextureBankWgpu::NoteTextureUse(TextureWgpu* texture)
{
    if (!texture)
        return;
    texture->_lastUsedFrame = _residencyFrame;
    if (_capture && std::find(_capture->begin(), _capture->end(), texture) == _capture->end())
        _capture->push_back(texture);
}

namespace
{
bool IsTerrainOwnedTexture(const TextureWgpu* texture)
{
    const char* name = texture ? texture->Name() : nullptr;
    if (!name)
        return false;
    auto contains = [name](const char* needle)
    {
        const size_t n = std::strlen(needle);
        for (const char* at = name; *at; ++at)
            if (strnicmp(at, needle, n) == 0)
                return true;
        return false;
    };
    return contains("\\layers\\") || contains("/layers/") || contains("_lca.") || contains("_lco.");
}
} // namespace

void TextureBankWgpu::TrimResidency()
{
    ++_residencyFrame;
    if (!_renderer)
        return;
    WgrMemoryStats stats{};
    if (!wgr_get_memory_stats(_renderer, &stats) || stats.budget_bytes == 0 ||
        stats.tracked_bytes <= stats.budget_bytes)
        return;

    uint64_t grace = 120;
    if (const char* value = std::getenv("WGR_TEXTURE_EVICT_GRACE_FRAMES"))
        grace = static_cast<uint64_t>(std::clamp<long long>(std::strtoll(value, nullptr, 10), 2, 36000));
    std::vector<TextureWgpu*> candidates;
    candidates.reserve(_texture.Size());
    for (int i = 0; i < _texture.Size(); ++i)
    {
        TextureWgpu* texture = _texture[i];
        if (!texture || !texture->GpuHandle() || texture->IsDynamicTexture() || texture->ResidentPins() != 0 ||
            IsTerrainOwnedTexture(texture) || _residencyFrame < texture->LastUsedFrame() + grace)
            continue;
        candidates.push_back(texture);
    }
    std::sort(candidates.begin(), candidates.end(), [](const TextureWgpu* a, const TextureWgpu* b)
              { return a->LastUsedFrame() < b->LastUsedFrame(); });

    const uint64_t target = stats.budget_bytes * 9 / 10;
    uint32_t evicted = 0;
    for (TextureWgpu* texture : candidates)
    {
        texture->EvictGpu();
        ++evicted;
        // Amortise the FFI query while still stopping close to the target.
        if ((evicted & 15u) == 0 && wgr_get_memory_stats(_renderer, &stats) && stats.tracked_bytes <= target)
            break;
    }
    if (evicted && wgr_get_memory_stats(_renderer, &stats))
        LOG_INFO(Graphics, "Wgpu texture LRU: evicted={} tracked={} budget={} candidates={}", evicted,
                 stats.tracked_bytes, stats.budget_bytes, candidates.size());
}

int TextureBankWgpu::Find(RStringB name) const
{
    // Was a linear scan of _texture comparing `texture->GetName() == name`.
    // _textureByName uses that same operator== as its equality predicate, so the
    // result is identical - see the NameHash/NameEqual comment in the header.
    const auto it = _textureByName.find(name);
    return it == _textureByName.end() ? -1 : it->second;
}

Ref<Texture> TextureBankWgpu::Load(RStringB name)
{
    int index = Find(name);
    if (index >= 0)
    {
        return (Texture*)_texture[index];
    }

    int iFree = _texture.Add();
    TextureWgpu* texture = new TextureWgpu(this);
    texture->SetName(name);
    _texture[iFree] = texture;
    // Key off the stored name rather than the argument: SetName copies the RStringB
    // verbatim, so the two share one interned buffer, and this cannot drift if that
    // ever stops being true.
    _textureByName.emplace(texture->GetName(), iFree);
    texture->Init();
    return texture;
}

MipInfo TextureBankWgpu::UseMipmap(Texture* tex, int /*level*/, int /*top*/)
{
    if (auto* t = static_cast<TextureWgpu*>(tex))
    {
        t->EnsureUploaded();
    }
    return MipInfo(tex, 0);
}

Texture* TextureBankWgpu::CreateDynamic(int w, int h, const void* rgba, uint32_t size, bool /*mipmap*/)
{
    auto* texture = new TextureWgpu(this);
    texture->InitDynamic(w, h, rgba, size);
    return texture;
}

void TextureBankWgpu::UpdateDynamic(Texture* tex, const void* rgba, uint32_t size)
{
    if (auto* t = static_cast<TextureWgpu*>(tex))
    {
        t->UpdateDynamic(rgba, size);
    }
}

} // namespace Poseidon
