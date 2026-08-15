#pragma once

#include "TextureWgpu.hpp"

#include <Poseidon/Graphics/Textures/TextureBank.hpp>
#include <Poseidon/Foundation/Containers/Array.hpp>

#include <cstddef>
#include <functional>
#include <unordered_map>
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

    // Name -> index into _texture. Find() used to scan the whole array, which is
    // O(n) per material bind during model registration and quadratic over a world
    // that holds several thousand textures.
    //
    // Matching semantics are preserved exactly by reusing RStringB::operator==
    // as the equality predicate. That operator is interned-pointer identity
    // (RString.hpp: `_ref == with._ref`), and RStringB's bank interns byte-exactly
    // (MapClassTraits::CmpKey is strcmp), so equality here is case-SENSITIVE and
    // no normalisation happens anywhere - Texture::SetName stores the RStringB
    // verbatim. The hash must therefore agree with pointer identity, not with the
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
    // The only path that removes entries from _texture; the name index has to go with it.
    void ReleaseAllTextures() override
    {
        _texture.Clear();
        _textureByName.clear();
    }
    void FlushBank(QFBank*) override {}

    Texture* CreateDynamic(int w, int h, const void* rgba, uint32_t size, bool mipmap = false) override;
    void UpdateDynamic(Texture* tex, const void* rgba, uint32_t size) override;
};

} // namespace Poseidon
