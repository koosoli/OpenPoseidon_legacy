#pragma once

#include <Poseidon/Graphics/Textures/TextureBank.hpp>
#include <Poseidon/Foundation/Types/Pointers.hpp>

#include <wgpu_renderer.hpp>

#include <cstdint>

namespace Poseidon
{
class ITextureSource;
class TextureBankWgpu;

// A texture for the wgpu backend. Two kinds:
//   * file-backed - pixels are uploaded lazily on first use (EnsureUploaded)
//   * dynamic     - uploaded on instantiation from RGBA bytes
class TextureWgpu : public Texture
{
    friend class TextureBankWgpu;

  private:
    SRef<ITextureSource> _src;
    TextureBankWgpu* _bank = nullptr;

    int _aRatio = 0;
    int _w = 0, _h = 0;
    int _nMipmaps = 0;
    PacLevelMem _mipmaps[MAX_MIPMAPS];

    uint64_t _gpuHandle = 0;
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
    // Cached three-way alpha classification (-1 = not computed yet). Drives the section-sort
    // renderer's opaque-vs-blend pass routing (Shape::Draw / HasBlendSections). Mirrors
    // TextureGL33::GetAlphaClass — without this override the base default (always Opaque) left
    // every wgpu section in the opaque pass, so alpha-blended sections composited against the
    // sky fill (see-through grills/badges).
    signed char _alphaClass = -1;
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
    void InitDynamic(int w, int h, const void* rgba, uint32_t size);
    // Reserve _slotLease if not already held; returns it (0 = none available).
    uint32_t AcquireSlotLease();

    // How a needed alpha scan was served. The classification is identical in all three
    // cases; only the work to reach it differs, and the split is what the streaming stats
    // report.
    enum class AlphaScanPath
    {
        Handoff,   // the upload had just inflated this level; no file access at all
        BlockRead, // one level read from the file, alpha decoded off the blocks
        FullDecode // the whole file re-read and decoded to RGBA8 (non-block formats)
    };
    // Fill _alphaStats from the top mip's compressed blocks, if this texture's format
    // stores alpha in blocks. Returns false when it does not, leaving _alphaStats alone —
    // the caller then falls back to the whole-file decode.
    bool ScanTopMipAlphaBlocks(AlphaScanPath& path);

  public:
    explicit TextureWgpu(TextureBankWgpu* bank);
    ~TextureWgpu() override;

    uint64_t EnsureUploaded();
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
    bool HasAlphaHoles();

    // RFG-047: true when this texture, used as a normal map, packs X in RED.
    //
    // Answered from the form it was actually LOADED in, not from the file on disk: the
    // same `_NMO` reaching the decode path keeps RFG-045's R-into-A copy and must return
    // false, or the shader would read the copy out of the wrong channel and the fix would
    // invert itself. That is why this asks the destination format rather than the name
    // alone. Cheap and const: two integer compares plus, for BC7, one substring test.
    bool NormalIsRgPacked() const;

    bool VerifyChecksum(const MipInfo&) const override { return true; }
    bool IsGpuResident() const override { return _gpuHandle != 0; }

    void SetMaxSize(int) override {}
    int AMaxSize() const override { return 256; }
    const PacLevelMem& AMipmap(int level) const override { return _mipmaps[level < MAX_MIPMAPS ? level : 0]; }
    PacLevelMem& AMipmap(int level) override { return _mipmaps[level < MAX_MIPMAPS ? level : 0]; }
};

} // namespace Poseidon
