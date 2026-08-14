#pragma once

#include <Poseidon/Foundation/Strings/RString.hpp>
#include <Poseidon/Graphics/Rendering/Font/Pactext.hpp>
#include <Poseidon/Graphics/Textures/DDSConverter.hpp>
#include <Poseidon/Graphics/Textures/PixelFormat.hpp>

#include <vector>

namespace Poseidon
{

// DDS / Enfusion .edds texture source (DZ-002).
//
// Registered for both extensions because DayZ uses them interchangeably in material
// references and both land in the same container family: a plain .dds goes to
// DDSConverter, an .edds (DDS header carrying the 'ENF1' marker) to EddsReader.
//
// Block-compressed levels are kept compressed and handed to the renderer as blocks,
// the way the PAA path does. Decoding them to RGBA at load time would be simpler,
// but DayZ's textures are 1k-4k and there are ~28,000 of them in a world, and the
// open blocker on this branch is the renderer running out of device resources at
// exactly that scale -- so inflating every texture 8x on the way in is the one thing
// this must not do.
class TextureSourceDDS : public ITextureSource
{
    RStringB _name;
    std::vector<DDSMipLevel> _levels; //!< [0] is the largest
    PixelFormat _format = PixelFormat::Unknown;
    PacFormat _pacFormat = PacARGB8888;
    int _w = 0;
    int _h = 0;
    int _mipmaps = 0;
    bool _hasAlpha = false;
    bool _isTransparent = false;
    PackedColor _avgColor{};

  public:
    TextureSourceDDS();
    ~TextureSourceDDS() override;

    bool Init(const char* name, PacLevelMem* mips, int maxMips) override;

    int GetMipmapCount() const override { return _mipmaps; }
    PacFormat GetFormat() const override { return _pacFormat; }
    bool GetMipmapData(void* mem, const PacLevelMem& mip, int level) const override;

    PackedColor GetAverageColor() const override { return _avgColor; }

    bool IsAlpha() const override { return _hasAlpha; }
    bool IsTransparent() const override { return _isTransparent; }
    void ForceAlpha() override { _hasAlpha = true; }

    // Testable entry point -- decode from an in-memory buffer, so unit tests can
    // exercise the container without initialising the file server.
    bool InitFromMemory(const void* data, size_t size, const char* nameForErrors);
};

class TextureSourceDDSFactory : public ITextureSourceFactory
{
  public:
    bool Check(const char* name) override;
    void PreInit(const char* name) override;
    ITextureSource* Create(const char* name, PacLevelMem* mips, int maxMips) override;
};

extern TextureSourceDDSFactory* GTextureSourceDDSFactory;

} // namespace Poseidon
