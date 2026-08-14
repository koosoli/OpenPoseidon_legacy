#pragma once

#include "TextureWgpu.hpp"

#include <Poseidon/Graphics/Textures/TextureBank.hpp>
#include <Poseidon/Foundation/Containers/Array.hpp>

#include <vector>

struct WgrRenderer;

namespace Poseidon
{

class TextureBankWgpu : public AbstractTextBank
{
  private:
    // The bank is the owner of cached file-backed textures. LLinkArray only
    // observes an object's lifetime; it does not retain it. That left a texture
    // loaded for a one-frame material binding with no live owner after the local
    // Ref returned by Load() was released, so its WGPU handle was destroyed and
    // later draws silently sampled bindless slot 0 (white fallback).
    RefArray<TextureWgpu> _texture;
    WgrRenderer* _renderer;
    uint64_t _residencyFrame = 0;
    // RegisterGpuModel brackets its material scan with this optional capture.
    // It records the Texture objects whose stable WGPU handles are baked into a
    // retained model, allowing EngineWgpu to pin only models with live instances.
    std::vector<TextureWgpu*>* _capture = nullptr;

  public:
    explicit TextureBankWgpu(WgrRenderer* renderer);
    ~TextureBankWgpu() override;

    WgrRenderer* Renderer() const { return _renderer; }
    void Detach() { _renderer = nullptr; }
    void BeginModelTextureCapture(std::vector<TextureWgpu*>& out);
    void EndModelTextureCapture();
    void NoteTextureUse(TextureWgpu* texture);
    uint64_t ResidencyFrame() const { return _residencyFrame; }
    // Enforce the shared WGR_DYNAMIC_VRAM_MB budget after submission. Retained
    // model textures are pinned explicitly; direct-path textures remain resident
    // while used and become LRU candidates only after a grace period.
    void TrimResidency();

    int Find(RStringB name) const;
    Ref<Texture> Load(RStringB name) override;
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
    void ReleaseAllTextures() override { _texture.Clear(); }
    void FlushBank(QFBank*) override {}

    Texture* CreateDynamic(int w, int h, const void* rgba, uint32_t size, bool mipmap = false) override;
    void UpdateDynamic(Texture* tex, const void* rgba, uint32_t size) override;
};

} // namespace Poseidon
