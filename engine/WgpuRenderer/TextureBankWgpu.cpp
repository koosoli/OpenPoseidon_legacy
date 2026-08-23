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

// The share of the dynamic budget object textures may hold before the LRU starts evicting.
//
// Half, because the other tracked pool is merged model geometry and it is NOT evictable from
// here: a split that leaves textures the whole budget is the unwinnable test this replaces, and
// one that leaves them too little evicts a working set that fits. Tunable because the right
// split is a per-corpus judgement -- Everon is geometry-heavy, an interior-heavy world is not.
static uint64_t TextureBudgetBytes(uint64_t budgetBytes)
{
    static const double share = []
    {
        if (const char* value = std::getenv("WGR_TEXTURE_BUDGET_SHARE"))
            return std::clamp(std::strtod(value, nullptr), 0.05, 1.0);
        return 0.5;
    }();
    return static_cast<uint64_t>(static_cast<double>(budgetBytes) * share);
}

void TextureBankWgpu::TrimResidency()
{
    ++_residencyFrame;
    if (!_renderer)
        return;
    WgrMemoryStats stats{};
    if (!wgr_get_memory_stats(_renderer, &stats) || stats.budget_bytes == 0)
        return;
    // TEXTURES against a TEXTURE budget, not textures+geometry against the whole one.
    //
    // `tracked_bytes` is textures plus geometry and only textures are evictable here, so on a
    // world whose GEOMETRY alone passes the budget the old test was unwinnable: it fired every
    // frame, evicted every candidate it had, and the number it chased went UP regardless.
    // Measured on Reforger Everon at WGR_DYNAMIC_VRAM_MB=1200 -- three consecutive trims
    // evicted 14/14, 4/4 and 1/1 candidates while tracked climbed 1558 -> 1596 -> 1682 MB.
    // Everon is ~84% geometry, so no amount of texture eviction could ever reach the target.
    //
    // That is not merely a wasted pass, it is a DESTRUCTIVE one: `wgr_model_register` bakes
    // each material's bindless slot permanently, so evicting a texture leaves every model
    // using it on the white fallback (see the pin note below, and InvalidateStaleGpuModels,
    // which now repairs exactly this). The owner's report -- a building's texture appearing,
    // then reverting to white, and never arriving at all closer in -- is this loop.
    const uint64_t textureBudget = TextureBudgetBytes(stats.budget_bytes);
    if (stats.object_texture_bytes <= textureBudget)
        return;

    // NOTE on the ResidentPins() check below, before anyone relaxes it: a pin means "some
    // REGISTERED model references this", and on DayZ Chernarus that starves this LRU to 1-31
    // candidates while tracked VRAM climbs past twice the budget. Making pinned textures
    // evictable is the right goal but CANNOT be done from C++ -- wgr_model_register bakes each
    // material's bindless SLOT INDEX permanently, and destroying the texture both hands that
    // slot to the next upload (wrong texture, silently, forever) and leaves the model on the
    // white fallback even after the re-upload. Full argument at TextureMipBias() in
    // TextureWgpu.cpp; it needs stable slots or a slot indirection on the Rust side first.
    //
    // NOTE on the budget test above: stats.tracked_bytes is textures + geometry, but only
    // textures are evictable here. A world whose GEOMETRY alone exceeds the budget (Everon is
    // 84% geometry) therefore evicts every candidate texture every frame chasing a target it
    // can never reach. Comparing stats.object_texture_bytes against a texture share of the
    // budget would be the honest test; left alone deliberately, as changing it moves the
    // numbers currently being benchmarked.
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

    const uint64_t target = textureBudget * 9 / 10;
    uint32_t evicted = 0;
    for (TextureWgpu* texture : candidates)
    {
        texture->EvictGpu();
        ++evicted;
        // Amortise the FFI query while still stopping close to the target.
        // Measured on the same quantity the gate used, so the loop can actually reach it.
        if ((evicted & 15u) == 0 && wgr_get_memory_stats(_renderer, &stats) &&
            stats.object_texture_bytes <= target)
            break;
    }
    if (evicted && wgr_get_memory_stats(_renderer, &stats))
        LOG_INFO(Graphics,
                 "Wgpu texture LRU: evicted={} textureBytes={} textureBudget={} tracked={} budget={} candidates={}",
                 evicted, stats.object_texture_bytes, textureBudget, stats.tracked_bytes, stats.budget_bytes,
                 candidates.size());
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
