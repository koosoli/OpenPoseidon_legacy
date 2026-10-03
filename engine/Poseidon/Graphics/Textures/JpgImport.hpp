#pragma once

#include <Poseidon/Graphics/Rendering/Font/Pactext.hpp>
#include <Poseidon/Foundation/Strings/RString.hpp>

#include <memory>
#include <vector>

namespace Poseidon
{
// Image texture loader (stb_image): .jpg/.jpeg/.png/.tga/.bmp. Decodes to RGBA8 at
// Init() time, caches the decoded pixels, and downsamples on demand in
// GetMipmapData(). Cross-platform, no external DLL dependency. Requires
// power-of-two source dimensions.
//
// SINKHOLE W0: the source used to report ARGB1555 with no alpha, which threw away a
// PNG/TGA alpha channel on GL33 and loaded nothing at all on wgpu (its non-block
// fallback re-reads the file as a PAA). It now reports ARGB8888 -- the format both
// renderers upload from a decoded source -- and classifies the alpha it actually
// decoded, the way Malprave's DDS source does: any value strictly between 0 and 255
// is a blended (or anti-aliased cut-out) alpha, only 0/255 with some 0 is a
// punch-through, and an image whose alpha is all 255 stays opaque.
class TextureSourceJPEG : public ITextureSource
{
    RStringB _name;
    std::vector<uint8_t> _rgba0; //!< decoded mip 0 as RGBA8
    int _w = 0;
    int _h = 0;
    int _mipmaps = 0;
    PackedColor _avgColor{};
    bool _isAlpha = false;       //!< some alpha strictly between 0 and 255
    bool _isTransparent = false; //!< alpha only 0/255, and some pixels are 0

  public:
    TextureSourceJPEG();
    ~TextureSourceJPEG() override;

    bool Init(const char* name, PacLevelMem* mips, int maxMips) override;

    int GetMipmapCount() const override { return _mipmaps; }
    PacFormat GetFormat() const override { return PacARGB8888; }
    bool GetMipmapData(void* mem, const PacLevelMem& mip, int level) const override;

    PackedColor GetAverageColor() const override { return _avgColor; }

    bool IsAlpha() const override { return _isAlpha; }
    bool IsTransparent() const override { return _isTransparent; }
    // Deliberately a no-op: the alpha class comes from the decoded pixels. The
    // renderers force alpha on every 32-bit format (TextureWgpu::Init); an opaque PNG
    // must not become a blended texture on the format's say-so alone.
    void ForceAlpha() override {}

    // Testable entry point — decode from an in-memory JPEG buffer. Called by
    // Init() after it has loaded the file; exposed separately so unit tests
    // can exercise the decode path without initialising the file server.
    bool InitFromMemory(const uint8_t* data, size_t size, const char* nameForErrors);
};

class TextureSourceJPEGFactory : public ITextureSourceFactory
{
  public:
    bool Check(const char* name) override;
    void PreInit(const char* name) override;
    ITextureSource* Create(const char* name, PacLevelMem* mips, int maxMips) override;
};

extern TextureSourceJPEGFactory* GTextureSourceJPEGFactory;
} // namespace Poseidon
