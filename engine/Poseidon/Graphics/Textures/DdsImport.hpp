#pragma once

#include <Poseidon/Foundation/Strings/RString.hpp>
#include <Poseidon/Graphics/Rendering/Font/Pactext.hpp>
#include <Poseidon/Graphics/Textures/DDSConverter.hpp>
#include <Poseidon/Graphics/Textures/PixelFormat.hpp>

#include <vector>

namespace Poseidon
{
namespace render { class PreparedTextureStore; }

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
// RFG-047 -- hand BC4/BC5/BC7 to the backend COMPRESSED instead of decoding them.
//
// RFG-016 decodes these three to 32-bit because the 2001 Pac container has no name for
// them; that costs 4x the bytes of BC7 and 8x those of BC5, and it is what made Everon
// exhaust GPU memory at ~1,200 models (RFG-044 then capped the decoded edge at 1024 as a
// stopgap). Every D3D12-class GPU decodes all three in hardware, so the limit was never
// the hardware -- it was this intermediate format.
//
// A process-wide switch rather than a parameter because the decision belongs to the
// BACKEND (wgpu speaks BC4/5/7; the GL33 fallback is a GL 3.3 core context and does not),
// while the call site is a texture factory that knows nothing about either. Default OFF,
// so nothing changes until a backend that can take the blocks turns it on:
// EngineWgpu::Init does, when the adapter reports TEXTURE_COMPRESSION_BC.
void SetDdsCompressedPassthrough(bool on);
bool DdsCompressedPassthrough();

// RFG-047 -- true when `name` is an Enfusion normal map whose X lives in RED, not alpha.
//
// The shader's `decode_nohq` reads (alpha, green), the DXT5nm convention of every
// `_nohq.paa` in the OFP/Arma corpus. RFG-045 satisfied it by copying R into A while
// decoding. A compressed upload cannot do that -- there is no CPU-side pixel to write --
// so the CONSUMER has to be told instead, and this is the single predicate both sides
// use. Exposed here (rather than duplicated in the renderer) so the decode path's channel
// copy and the compressed path's shader flag can never disagree about which files it is.
bool IsEnfusionRgNormalName(const char* name);

// RFG-070 -- rescale decoded ARGB8888 (B,G,R,A in memory) levels so the TOP level's
// per-channel mean becomes the linear colour `rgb`, sRGB-encoded.
//
// The `enft|r,g,b|` texture name reaches this; see the definition for the rule and for
// which two things it does differently from the converting exporter it copies. Declared
// here rather than left file-local so the channel ORDER can be pinned by a test: the whole
// defect class this fixes is a per-channel one, and applying r,g,b in index order to a
// B,G,R buffer turns beige walls blue -- which looks like a working tint until you look.
void ApplyEnfusionLayerTint(std::vector<DDSMipLevel>& levels, const float rgb[3]);

class TextureSourceDDS : public ITextureSource
{
    friend class render::PreparedTextureStore;
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
    //
    // `forceDecode` suppresses the RFG-047 compressed pass-through for this one texture.
    // Init() sets it for both halves of an `enfa|` composite, whose whole purpose is to
    // write one file's coverage into another file's alpha channel -- which needs pixels.
    bool InitFromMemory(const void* data, size_t size, const char* nameForErrors, bool forceDecode = false);
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
