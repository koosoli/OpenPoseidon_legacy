#pragma once

#include <Poseidon/Graphics/Textures/TextureBank.hpp>
#include <Poseidon/Foundation/Types/Pointers.hpp>

#include <wgpu_renderer.hpp>
#include <Poseidon/Graphics/Textures/MipDemand.hpp>
#include <Poseidon/Graphics/Textures/TextureReuseOnlyBinding.hpp>
#include <Poseidon/Graphics/Textures/RetailPacPixelStage.hpp>

#include <cstdint>
#include <memory>
#include <string>

namespace Poseidon
{
class ITextureSource;
class TextureBankWgpu;

using WgpuTextureReuseOnlyBinding = render::TextureReuseOnlyBinding<Ref<Texture>>;

// A texture for the wgpu backend. Two kinds:
//   * file-backed - pixels are uploaded lazily on first use (EnsureUploaded)
//   * dynamic     - uploaded on instantiation from RGBA bytes
class TextureWgpu : public Texture
{
    friend class TextureBankWgpu;

  private:
    // Declared before _src so a borrowed CPU tint view dies BEFORE its owner.
    // RFG-086: the inner image also supplies the existing GPU tint delegate.
    Ref<Texture> _tintDelegate;
    SRef<ITextureSource> _src;
    bool _cpuTintView = false;
    RString _sourceName; // Optional visual replacement; Name() remains the simulation-facing identity.
    TextureBankWgpu* _bank = nullptr;

    int _aRatio = 0;
    int _w = 0, _h = 0;
    int _nMipmaps = 0;
    PacLevelMem _mipmaps[MAX_MIPMAPS];

    uint64_t _gpuHandle = 0;
    int _uploadedFirstLevel = 0;
    render::MipDemand _mipDemand;
    uint64_t _adaptiveViewEpoch = 0;
    bool AdaptiveMipCandidate();
    bool ReplaceMipTail(int firstLevel, uint64_t frame);
    uint64_t _lastUsedFrame = 0;
    uint32_t _residentPins = 0;
    // REN-RES-001: the bindless slot this texture's uploads occupy, reserved for the
    // lifetime of THIS OBJECT rather than of one upload. Acquired lazily at the first
    // upload, released in the destructor -- never in EvictGpu, which is the whole point:
    // an eviction leaves the slot reserved (sampling white) so the re-upload reclaims the
    // same index and every model that baked it keeps working. 0 = none (array at cap),
    // which simply degrades to the old per-upload slot behaviour.
    uint32_t _slotLease = 0;
    // don't keep retrying a texture that fails to load
    bool _uploadTried = false;
    bool _dynamic = false;
    struct DeferredGenerated;
    std::unique_ptr<DeferredGenerated> _deferredGenerated;
    // Cached three-way alpha classification (-1 = not computed yet). Drives the section-sort
    // renderer's opaque-vs-blend pass routing (Shape::Draw / HasBlendSections). Mirrors
    // TextureGL33::GetAlphaClass — without this override the base default (always Opaque) left
    // every wgpu section in the opaque pass, so alpha-blended sections composited against the
    // sky fill (see-through grills/badges).
    signed char _alphaClass = -1;
    // Optional one-shot Init-proof retirement. Reset only by actual Init, not GPU eviction.
    bool _hotSourceProofRetireAttempted = false;
    void RetireHotSourceProof();
    // Preserve the decoded coverage summary alongside the three-way class. TreeAdv's close LOD
    // can bind both needles and solid bark through the same foliage material; only the former
    // has clear alpha texels and may receive an alpha test.
    bool _alphaStatsScanned = false;
    AlphaStats _alphaStats;
    // True when the ONLY alpha signal was the pixel format (ARGB4444 / AI88 / ARGB8888 ->
    // Init() calls ForceAlpha) and no FLAG tagg said so. GetAlphaClass refuses to route such a
    // texture to the blend pass unless it measures or is named as glass — see
    // ApplyFormatAlphaPolicy in TextureWgpu.cpp (the translucent-cockpit-body fix).
    bool _alphaForcedByFormat = false;

    int Init();
    // `genMips` asks the backend to build the mip chain from level 0. A dynamic
    // texture is single-level by default, which is right for a font atlas or a
    // clutter card and WRONG for anything that gets minified -- an Enfusion ground
    // layer tiled every few metres (RFG-065) is aliasing noise without it.
    void InitDynamic(int w, int h, const void* rgba, uint32_t size, bool genMips = false);
    // Reserve _slotLease if not already held; returns it (0 = none available).
    uint32_t AcquireSlotLease();

    // How a needed alpha scan was served. The classification is identical across these
    // cases; only the work to reach it differs, and the split is what the streaming stats
    // report.
    enum class AlphaScanPath
    {
        ColdOwned, // non-consuming Init-bound admission span; upload still owns the full chain
        PreparedFacts, // validated leased worker facts; no owner pixel decode
        Handoff,   // the upload had just inflated this level; no file access at all
        BlockRead, // one level read from the file, alpha decoded off the blocks
        FullDecode // the whole file re-read and decoded to RGBA8 (non-block formats)
    };
    // Fill _alphaStats from the top mip's compressed blocks, if this texture's format
    // stores alpha in blocks. Returns false when it does not, leaving _alphaStats alone —
    // the caller then falls back to the whole-file decode.
    bool ScanTopMipAlphaBlocks(AlphaScanPath& path);

  public:
    struct RetailPacUploadWitness
    {
        bool currentInit = false, layoutMatched = false, createAttempted = false;
        bool created = false, postMountMatched = false, retiredAfterMismatch = false;
        bool alphaFromPrepared = false;
        std::shared_ptr<const ArchiveSourceBinding> birth;
        BankReadMemberIdentity member{};
        std::string rawSha256, uploadedSha256;
        uint64_t sourceBytes = 0, uploadedBytes = 0, handle = 0;
        uint32_t slotLease = 0, width = 0, height = 0, levels = 0, firstLevel = 0;
        uint64_t alphaTopPixels = 0;
        double alphaClearPercent = 0.0;
    };
    bool HasAdaptiveReplacement() const { return _gpuHandle && _slotLease && _mipDemand.replaced != 0; }
    explicit TextureWgpu(TextureBankWgpu* bank);
    ~TextureWgpu() override;

    uint64_t EnsureUploaded();
    // Private exact-model diagnostic only: construct a fresh TextureWgpu(bank),
    // SetName("data\\skala_piskovec2.pac"), then call InitRetailPacSource()
    // inside RetailPacReadScope("data3d\\skala_new.p3d"). The object must be
    // retained by the owner and destroyed before its bank/renderer. It is not
    // inserted in TextureBankWgpu's shared name cache.
    bool InitRetailPacSource();
    bool CaptureRetailPacRead(render::ColdPaaRead& out) const;
    // One source-bound BC1 create, no path reopen, store claim, or fallback.
    // A refusal before create leaves normal EnsureUploaded retry state intact.
    uint64_t UploadRetailPacPrepared(const render::RetailPacPrepared& prepared,
                                     RetailPacUploadWitness& witness);
    const char* SourceName() const { return _sourceName.GetLength() ? (const char*)_sourceName : Name(); }
    // Owner-only metadata/native-handle observation. Never loads a bank or
    // recaptures provenance by name; unsupported/currently different mounts refuse.
    std::shared_ptr<const ArchiveSourceBinding> CurrentArchiveSourceBinding() const;
    // Diagnostic-only facts about the existing Init/source; no VFS read or recapture.
    bool HasTextureSourceObject() const { return _src != nullptr; }
    bool HasInitializedWarmSource() const { return _src != nullptr && _nMipmaps > 0; }
    bool HasAttemptedGpuUpload() const { return _uploadTried; }
    // The optional owner pre-touch must not poison the ordinary registration
    // retry when its upload failed before a GPU handle was created.
    void RetryAfterFailedOwnerStage() { if (!_gpuHandle && !_dynamic) _uploadTried = false; }
    bool HasInitializedArchiveBinding() const;
    bool CaptureColdPaaRead(render::ColdPaaRead& out) const override;
    bool CanWarmPrepare() const { return !_gpuHandle && !_uploadTried && HasReloadableSource(); }
    // Drop only the GPU allocation while preserving the decoded file source and
    // Texture object.  Terrain page residency uses this to reload the same
    // authored page when the camera returns to it.
    void EvictGpu();
    uint64_t GpuHandle() const { return _gpuHandle; }
    uint64_t LastUsedFrame() const { return _lastUsedFrame; }
    uint32_t ResidentPins() const { return _residentPins; }
    // REN-RES-001: the leased bindless slot, or 0 if none was available at the first upload.
    // Non-zero is the promise the retained path leans on -- a re-upload after EvictGpu lands
    // in THIS index, so every material that baked it keeps sampling the right texture and no
    // model needs re-registering. 0 means the array was at cap and this texture fell back to
    // per-upload slots, where that promise does NOT hold.
    uint32_t SlotLease() const { return _slotLease; }
    bool IsDynamicTexture() const { return _dynamic; }
    // Owner registration may distinguish a held generated upload from an
    // already-created dynamic texture without touching the renderer.
    bool HasDeferredGeneratedUpload() const { return _deferredGenerated != nullptr; }
    // CPU source presence only; external file availability is checked by upload.
    bool HasReloadableSource() const
    {
        if (_dynamic || _src == nullptr || _nMipmaps <= 0)
            return false;
        const char* name = SourceName();
        return name && *name;
    }
    void PinResident() { ++_residentPins; }
    void UnpinResident()
    {
        if (_residentPins > 0)
            --_residentPins;
    }
    void UpdateDynamic(const void* rgba, uint32_t size);

    int AWidth(int) const override { return _w; }
    int AHeight(int) const override { return _h; }
    int ANMipmaps() const override { return _nMipmaps > 0 ? _nMipmaps : 1; }
    void ASetNMipmaps(int) override {}
    Color GetPixel(int level, float u, float v) const override;

    bool IsTransparent() const override { return _src && _src->IsTransparent(); }
    bool IsAlpha() const override { return _dynamic || (_src && _src->IsAlpha()); }
    Color GetColor() override { return _src ? _src->GetAverageColor() : HBlack; }
    AlphaStats::Kind GetAlphaClass() override;
    //! Set by a caller that BUILT the pixels; see TextureBank::CreateDynamic.
    void SetAlphaClassHint(AlphaStats::Kind kind) { _alphaClass = static_cast<signed char>(kind); }
    // The full histogram behind GetAlphaClass — valid after GetAlphaClass()
    // has run its scan. MAT-051 keys the glass sheen on pctClear/pctPartial,
    // which the Kind alone cannot express.
    const AlphaStats& AlphaStatsView() const { return _alphaStats; }
    bool HasAlphaHoles() override;

    // RFG-047: true when this texture, used as a normal map, packs X in RED.
    //
    // Answered from the form it was actually loaded in. Original Enfusion `_NMO`/`_NTC`
    // maps retain R/G normal XY and their separate B/A data in BC7 and decoded DDS;
    // legacy BC5 normals still follow their existing packing rule.
    bool NormalIsRgPacked() const;
    bool NormalHasNativeCavity() const;
    bool NormalHasNativePbrChannels() const;

    bool VerifyChecksum(const MipInfo&) const override { return true; }
    bool IsGpuResident() const override { return _gpuHandle != 0; }

    void SetMaxSize(int) override {}
    int AMaxSize() const override { return 256; }
    const PacLevelMem& AMipmap(int level) const override { return _mipmaps[level < MAX_MIPMAPS ? level : 0]; }
    PacLevelMem& AMipmap(int level) override { return _mipmaps[level < MAX_MIPMAPS ? level : 0]; }
};

} // namespace Poseidon
