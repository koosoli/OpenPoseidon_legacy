#pragma once

#include "TextureWgpu.hpp"
#include <Poseidon/Graphics/Textures/TextureBirthBeforeUse.hpp>

#include <Poseidon/Graphics/Textures/TextureBank.hpp>
#include <Poseidon/Foundation/Containers/Array.hpp>

#include <cstddef>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

struct WgrRenderer;

namespace Poseidon
{

using WgpuTextureBirthObserver = render::TextureBirthBeforeUse<Ref<Texture>, ArchiveSourceBinding>;

class TextureBankWgpu : public AbstractTextBank
{
  private:
    // The bank is the owner of cached file-backed textures. LLinkArray only
    // observes an object's lifetime; it does not retain it. That left a texture
    // loaded for a one-frame material binding with no live owner after the local
    // Ref returned by Load() was released, so its WGPU handle was destroyed and
    // later draws silently sampled bindless slot 0 (white fallback).
    RefArray<TextureWgpu> _texture;

    // Name -> index into _texture. Find() used to scan the whole array, which is
    // O(n) per material bind during model registration and quadratic over a world
    // that holds several thousand textures.
    //
    // Matching semantics are preserved exactly by reusing RStringB::operator==
    // as the equality predicate. That operator is interned-pointer identity
    // (RString.hpp: `_ref == with._ref`), and RStringB's bank interns byte-exactly
    // (MapClassTraits::CmpKey is strcmp), so equality here is case-SENSITIVE and
    // Texture::SetName stores the RStringB verbatim. Mounted native entries also
    // register their archive-canonical alias in this same map; loose files and
    // synthetic material names retain exact matching. The hash agrees with pointer identity, not with the
    // characters: equal names always share one interned buffer, so hashing the
    // address of that buffer is consistent with the predicate.
    struct NameHash
    {
        std::size_t operator()(const RStringB& name) const noexcept
        {
            return std::hash<const void*>{}(static_cast<const void*>(name.Data()));
        }
    };
    struct NameEqual
    {
        bool operator()(const RStringB& a, const RStringB& b) const noexcept { return a == b; }
    };
    std::unordered_map<RStringB, int, NameHash, NameEqual> _textureByName;
    struct LegacyMaterial
    {
        std::string albedo;
        std::string normal;
        std::string sourceArchive;
        std::string detailNormal;
        float detailNormalMetres = 0.0f;
    };
    std::unordered_map<std::string, LegacyMaterial> _legacyMaterials;
    bool _legacyMaterialsLoaded = false;
    const LegacyMaterial* ResolveLegacyMaterial(const char* name);

    WgrRenderer* _renderer;
    uint64_t _residencyFrame = 0;
    // RegisterGpuModel brackets its material scan with this optional capture.
    // It records the Texture objects whose stable WGPU handles are baked into a
    // retained model, allowing EngineWgpu to pin only models with live instances.
    std::vector<TextureWgpu*>* _capture = nullptr;
    // Set by EngineWgpu once its retained-model liveness sweep has completed a FULL pass, so
    // every registered model in the working set has had one chance to refresh its textures'
    // _lastUsedFrame. Until then a pin is the only thing standing between a retained texture
    // and an eviction nothing would ever repair -- see TrimResidency.
    bool _retainedLivenessActive = false;
    // The oldest a retained mark may be while the sweep is healthy, in residency frames -- the
    // sweep is incremental, so a live model's mark is legitimately up to a pass old. A pinned
    // texture must be unmarked for at least this long before the LRU may touch it, whatever the
    // configured grace: with WGR_TEXTURE_EVICT_GRACE_FRAMES=2 a plain grace would condemn a
    // model that is merely waiting its turn in the sweep.
    uint64_t _retainedLivenessMaxAgeFrames = 0;

    // AST-012A -- GPU mip feedback, held across calls so the per-slot arrays are allocated once
    // rather than per report. Sized to the bindless array (8,192 slots), so 24 KB total.
    std::vector<uint8_t> _mipDesired;
    std::vector<uint16_t> _mipAge;
    // Residency frame at which the next report is due. The instrument runs every frame; the
    // REPORT is periodic, because the interesting quantity is a working set, and a working set
    // does not change meaningfully between two consecutive frames.
    uint64_t _mipReportDue = 0;
    // What the GPU says about the CPU's residency decisions. See ReportMipFeedback.
    void ReportMipFeedback();
    uint64_t _adaptiveViewEpoch = 1;
    bool _adaptiveViewValid = false;
    float _adaptiveViewX = 0, _adaptiveViewY = 0, _adaptiveViewZ = 0;
    float _adaptiveDirX = 0, _adaptiveDirY = 0, _adaptiveDirZ = 0;
    size_t _adaptiveCursor = 0;
    float _adaptiveProjectionX = 0, _adaptiveProjectionY = 0;

  public:
    void UpdateAdaptiveTextureDetail(); // InitDraw, before any frame records reference texture handles
    explicit TextureBankWgpu(WgrRenderer* renderer);
    ~TextureBankWgpu() override;

    WgrRenderer* Renderer() const { return _renderer; }
    void Detach() { _renderer = nullptr; }
    void BeginModelTextureCapture(std::vector<TextureWgpu*>& out);
    void EndModelTextureCapture();
    void NoteTextureUse(TextureWgpu* texture);
    uint64_t ResidencyFrame() const { return _residencyFrame; }
    // EngineWgpu::MarkRetainedModelLiveness calls this with `true` after its first complete
    // sweep and with `false` whenever the sweep cannot run (no camera / no draw distance /
    // WGR_RETAINED_LIVENESS=0) or has stopped completing passes. While false, TrimResidency
    // keeps skipping pinned textures. `maxAgeFrames` is how stale a healthy mark may be.
    void SetRetainedLivenessActive(bool active, uint64_t maxAgeFrames = 0)
    {
        _retainedLivenessActive = active;
        _retainedLivenessMaxAgeFrames = maxAgeFrames;
    }
    bool RetainedLivenessActive() const { return _retainedLivenessActive; }
    // Enforce the shared WGR_DYNAMIC_VRAM_MB budget after submission. Retained
    // model textures are pinned explicitly; direct-path textures remain resident
    // while used and become LRU candidates only after a grace period.
    void TrimResidency();

    int Find(RStringB name) const;
    Ref<Texture> Load(RStringB name) override;
    std::string LegacyNormalPath(const char* name);
    Ref<Texture> LoadLegacyEnhancementNormal(const char* name);
    Ref<Texture> LoadLegacyTerrainDetailNormal(const char* name, float& repeatsPerMetre);
    // TODO: real two-texture blend; for now load whichever source dominates.
    Ref<Texture> LoadInterpolated(RStringB n1, RStringB n2, float factor) override
    {
        return Load(factor < 0.5f ? n1 : n2);
    }
    MipInfo UseMipmap(Texture* tex, int level, int top) override;

    void Compact() override {}
    void Preload() override {}

    int NTextures() const override { return _texture.Size(); }
    Texture* GetTexture(int i) const override { return _texture[i]; }

    void FlushTextures() override {}
    // The only path that removes entries from _texture; the name index has to go with it.
    void ReleaseAllTextures() override
    {
        _texture.Clear();
        _textureByName.clear();
        _legacyMaterials.clear();
        _legacyMaterialsLoaded = false;
    }
    void FlushBank(QFBank*) override {}

    Texture* CreateDynamic(int w, int h, const void* rgba, uint32_t size, bool mipmap = false,
                           int alphaClass = -1) override;
    void UpdateDynamic(Texture* tex, const void* rgba, uint32_t size) override;
};

} // namespace Poseidon
