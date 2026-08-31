#include "TextureBankWgpu.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdlib>

#include <Poseidon/Graphics/Rendering/Shape/ObjectAdmitProfile.hpp>

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

    // NOTE on the ResidentPins() check below. A pin means "some REGISTERED model references
    // this", and while pins were unconditionally exempt this LRU starved to 1-31 candidates on
    // DayZ Chernarus while tracked VRAM climbed past twice the budget. Making pinned textures
    // evictable needed two things, and both now exist:
    //
    //   * Slot identity -- FIXED (REN-RES-001). wgr_model_register still bakes each material's
    //     bindless slot permanently, but a slot is now LEASED by the TextureWgpu rather than by
    //     one upload, so an eviction leaves it reserved-and-white and the re-upload reclaims the
    //     same index. Evicting a pinned texture no longer corrupts anything.
    //   * Liveness -- now supplied by EngineWgpu::MarkRetainedModelLiveness, which sweeps the
    //     retained instance table against the camera and calls NoteTextureUse for the textures
    //     of every registered model that has an instance within the object draw distance. That
    //     is what makes _lastUsedFrame mean something on the retained path, where nothing calls
    //     EnsureUploaded() per frame. The refill half lives in InvalidateStaleGpuModels, which
    //     re-uploads an evicted texture whose model has come back into range (the lease keeps
    //     its slot, so no re-registration is needed) and only destroys the registration when
    //     that re-upload fails.
    //
    // The exemption therefore survives only as a FAILSAFE, gated on _retainedLivenessActive:
    // until the sweep has completed one full pass, a retained texture's _lastUsedFrame is still
    // the frame it was registered on, and evicting on that would be evicting on noise -- right
    // slots, white buildings, which is exactly the A/B measured at WGR_DYNAMIC_VRAM_MB=900 on
    // 2026-08-31. The flag is also cleared whenever the sweep cannot run at all (no camera, no
    // draw distance) or WGR_RETAINED_LIVENESS=0 pins it off for an A/B from one binary.
    //
    // Granularity worth knowing when reading an eviction log: the signal is PER MODEL and
    // distance-only (no frustum, no occlusion), and it lags by up to one sweep pass. It is
    // deliberately conservative in the keep direction -- it marks more than actually drew.
    // Full argument at TextureMipBias() in TextureWgpu.cpp.
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
        if (!texture || !texture->GpuHandle() || texture->IsDynamicTexture() || IsTerrainOwnedTexture(texture))
            continue;
        const bool pinned = texture->ResidentPins() != 0;
        if (pinned && !_retainedLivenessActive)
            continue;
        // A pinned texture's mark comes from an incremental sweep, so it is allowed to be a
        // sweep-period old before it means anything. Unpinned (direct-draw) textures are marked
        // every frame they are used and keep the plain grace.
        const uint64_t effectiveGrace = pinned ? std::max(grace, _retainedLivenessMaxAgeFrames) : grace;
        if (_residencyFrame < texture->LastUsedFrame() + effectiveGrace)
            continue;
        candidates.push_back(texture);
    }
    std::sort(candidates.begin(), candidates.end(), [](const TextureWgpu* a, const TextureWgpu* b)
              { return a->LastUsedFrame() < b->LastUsedFrame(); });

    const uint64_t target = textureBudget * 9 / 10;
    uint32_t evicted = 0;
    // Counted separately because it is the number that says whether the liveness signal is
    // doing anything: before it existed this was structurally 0, and a run where evicted>0 but
    // pinned==0 means the LRU is still only reaching direct-draw textures.
    uint32_t evictedPinned = 0;
    for (TextureWgpu* texture : candidates)
    {
        if (texture->ResidentPins() != 0)
            ++evictedPinned;
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
                 "Wgpu texture LRU: evicted={} (pinned={}) textureBytes={} textureBudget={} tracked={} budget={} "
                 "candidates={} liveness={}",
                 evicted, evictedPinned, stats.object_texture_bytes, textureBudget, stats.tracked_bytes,
                 stats.budget_bytes, candidates.size(), _retainedLivenessActive ? 1 : 0);
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
    // Admission attribution: Init() opens the file through GFileServer to parse the PAA
    // header. Timed only while an admit scope is open (one bool load otherwise), because
    // whether a cold model's `adapt` milliseconds are these opens or geometry work is the
    // question the split exists to answer.
    render::ObjectAdmitProfile& admit = render::GObjectAdmitProfile;
    if (admit.active)
    {
        const auto began = std::chrono::steady_clock::now();
        texture->Init();
        admit.texHeaderLoadMs +=
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count();
        ++admit.texHeaderLoads;
    }
    else
    {
        texture->Init();
    }
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
