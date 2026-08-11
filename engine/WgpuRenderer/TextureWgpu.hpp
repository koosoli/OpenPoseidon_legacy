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

    int Init();
    void InitDynamic(int w, int h, const void* rgba, uint32_t size);

  public:
    explicit TextureWgpu(TextureBankWgpu* bank);
    ~TextureWgpu() override;

    uint64_t EnsureUploaded();
    uint64_t GpuHandle() const { return _gpuHandle; }
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
    bool HasAlphaHoles();

    bool VerifyChecksum(const MipInfo&) const override { return true; }
    bool IsGpuResident() const override { return _gpuHandle != 0; }

    void SetMaxSize(int) override {}
    int AMaxSize() const override { return 256; }
    const PacLevelMem& AMipmap(int level) const override { return _mipmaps[level < MAX_MIPMAPS ? level : 0]; }
    PacLevelMem& AMipmap(int level) override { return _mipmaps[level < MAX_MIPMAPS ? level : 0]; }
};

} // namespace Poseidon
