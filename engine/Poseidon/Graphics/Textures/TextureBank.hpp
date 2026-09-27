#ifndef _TEXTBANK_HPP
#define _TEXTBANK_HPP

#include <Poseidon/Foundation/Strings/RString.hpp>
#include <Poseidon/Foundation/Types/LLinks.hpp>
#include <Poseidon/Graphics/Rendering/Font/Pactext.hpp>
#include <Poseidon/Graphics/Rendering/Colors.hpp>
#include <Poseidon/Graphics/Textures/PAADecoder.hpp>

#define MAX_MIPMAPS 7
// that would be enough for 256*256 texture
// last mipmap will have dimensions 4x4

namespace Poseidon
{

struct SurfaceInfo;
namespace render { struct ColdPaaRead; }

//! The surface infos for the four quadrants of a blended terrain texture.
//! The quadrants are ordered: top-left, top-right, bottom-left, bottom-right.
using QuadrantSurfaces = const SurfaceInfo* [4];

class Texture : public RemoveLLinks
{
    friend class AnimatedTexture;
    friend class AbstractTextBank;

  private:
    AnimatedTexture* _inAnimation;
    float _roughness; // for physical simulation
    float _dustness;  // for physical simulation
    RStringB _name;
    RStringB _soundEnv;
#if _ENABLE_CHEATS
    RStringB _character;
#endif
    bool _refCountLocked;

  protected:
    int _mipmapWanted, _mipmapNeeded;

  public:
    Texture();

    void PrepareMipmap(int wanted, int needed);
    void ResetMipmap();

    Texture* GetAnimation(int i) const;
    int AnimationLength() const;
    bool IsAnimated() const { return _inAnimation != nullptr; }
    AnimatedTexture* GetAnimatedTexture() const { return _inAnimation; }

    //! load all headers so any data-accessing function can be used
    virtual void LoadHeaders() {}

    virtual void SetMaxSize(int maxSize) = 0;
    virtual int AMaxSize() const = 0;

    virtual int AWidth(int level = 0) const = 0;
    virtual int AHeight(int level = 0) const = 0;
    virtual int ANMipmaps() const = 0;
    virtual AbstractMipmapLevel& AMipmap(int level) = 0;
    virtual const AbstractMipmapLevel& AMipmap(int level) const = 0;
    virtual void ASetNMipmaps(int n) = 0;
    virtual Color GetPixel(int level, float u, float v) const = 0;
    virtual bool IsTransparent() const = 0;
    virtual Color GetColor() = 0;
    virtual bool IsAlpha() const = 0;

    //! three-way alpha class (opaque / cutout / blend) for per-section transparency
    //! routing; computed once and cached by the backend. Default: opaque.
    virtual AlphaStats::Kind GetAlphaClass() { return AlphaStats::Opaque; }

    //! Whether the alpha histogram holds real fully-clear texels (>= 2% clear,
    //! the ClassifyAlpha clear-coverage floor). Lets routers tell a punch-through
    //! mask (leaf cards, netting: holes + rim) from hole-less data alpha (glass
    //! dirt, roughness): same Blend class, opposite routing. Default: none.
    virtual bool HasAlphaHoles() { return false; }

    // some APIs (Glide) require u,v conversion
    virtual float UToPhysical(float u) const { return u; }
    virtual float VToPhysical(float v) const { return v; }

    virtual float UToLogical(float u) const { return u; }
    virtual float VToLogical(float v) const { return v; }

    virtual void SetMipmapRange(int min, int max) {}

    virtual bool VerifyChecksum(const MipInfo& mip) const = 0; // verify consistency

    //! True if this texture currently has live GPU storage. Used by the
    //! FreeType glyph-atlas sync to detect a texture whose backend surface was
    //! freed underneath it (e.g. after a hot-reload) so it re-uploads instead of
    //! sampling a dead handle.
    virtual bool IsGpuResident() const { return true; }

    const char* Name() const { return _name; }
    const RStringB& GetName() const { return _name; }
    void SetName(RStringB name);

    float Roughness() const { return _roughness; } // for physical simulation
    void SetRoughness(float roughness) { _roughness = roughness; }

    float Dustness() const { return _dustness; }
    void SetDustness(float dustness) { _dustness = dustness; }

    RStringB GetSoundEnv() const { return _soundEnv; }
#if _ENABLE_CHEATS
    RStringB GetCharacter() const { return _character; }
#endif

    virtual void SetMultitexturing(int type) {}

    ~Texture() override;

  protected:
    void Lock();
    void Unlock();

  public:
    // Appended virtual: conservative default, no Init/load/read/upload. Owner-only.
    virtual bool CaptureColdPaaRead(render::ColdPaaRead&) const { return false; }

    NoCopy(Texture)
};

// each texture bank knows type of its textures
// overloads function "Load" with different return type (if neccessary")
// casts parameter of function UseMipmap to know type - this is potentially unsafe

struct SurfaceInfo
{
    // The `files=` pattern this surface matches (e.g. "tn??????"), which is what
    // FindSurface tests against a texture name.
    RStringB _name;
    // The authored CfgSurfaces CLASS name (e.g. "Grass", "GrassHigh", "TKTrava").
    // The pattern says which textures are this surface; the class says what the
    // surface IS, and only the class can answer "should grass grow here" without
    // guessing from the texture's spelling.
    RStringB _class;
    float _roughness;
    float _dustness;
    RStringB _soundEnv;
    // Arma-generation CfgSurfaces name a CfgSurfaceCharacters class here, which is
    // the entry point to the map's own clutter. Empty on Operation Flashpoint data,
    // which defines no characters at all.
    RStringB _character;
    bool operator==(const SurfaceInfo& with) const { return _name == with._name; }
};

class AbstractTextBank
{
    friend class Texture;

    FindArray<SurfaceInfo> _surfaces;
    RefArray<AnimatedTexture> _animatedTextures;

  public:
    AnimatedTexture* LoadAnimated(RStringB name);
    void DeleteAnimated(AnimatedTexture* texture);
    void DeleteAllAnimated();

    virtual Ref<Texture> Load(RStringB text) = 0;
    virtual Ref<Texture> LoadInterpolated(RStringB n1, RStringB n2, float factor) = 0;
    virtual void AddGamma(float g) {}

    virtual int NTextures() const = 0;
    virtual Texture* GetTexture(int i) const = 0;

    void PrepareMipmap(Texture* texture, int level, int levelTop)
    {
        // hint - if we will use mipmap, we will want this one
        texture->PrepareMipmap(level, levelTop);
    }
    virtual MipInfo UseMipmap(Texture* texture, int level, int levelTop) = 0; // request - we want this some mipmap

    virtual void Compact() = 0;
    virtual void Preload() = 0;

    // Lift the per-frame mip-upload budget for the next `frames` frames so a
    // freshly-loaded scene reaches its wanted texture levels behind the
    // loading screen instead of visibly sharpening over the first second.
    // Default no-op for headless banks.
    virtual void BoostLoadBudget(int /*frames*/) {}
    virtual void FlushTextures() = 0;
    //! Hot-reload — drop every cached texture's GPU + decoded source so
    //! the next bind re-reads from disk.  Powers the viewer's F5 hot-
    //! reload.  Default no-op; the GL33 backend overrides.
    virtual void ForceReloadAll() {}
    //! Drop all cached textures (and their bookkeeping) so the bank starts empty.
    //! Called on an in-process re-mount so a kept-alive engine doesn't accumulate
    //! stale entries across reloads.  Default no-op; backends that hold a cache override.
    virtual void ReleaseAllTextures() {}
    virtual void FlushBank(QFBank* bank) = 0;

    // Dynamic texture from raw RGBA pixels. mipmap=true: backend generates a full
    // mip chain on create and regenerates on update.
    //! `alphaClass` is an `AlphaStats::Kind`, or -1 for "decide as usual". It exists
    //! because a dynamic texture has no ITextureSource, so `GetAlphaClass` has nothing
    //! to scan and keeps the base Opaque -- which silently turns a cutout into a solid
    //! quad. A caller that BUILT the pixels knows what they are; nobody else can.
    virtual Texture* CreateDynamic(int /*w*/, int /*h*/, const void* /*rgba*/, uint32_t /*size*/,
                                   bool /*mipmap*/ = false, int /*alphaClass*/ = -1)
    {
        return nullptr;
    }
    virtual void UpdateDynamic(Texture* /*tex*/, const void* /*rgba*/, uint32_t /*size*/) {}

    virtual void StartFrame();
    virtual void FinishFrame();
    virtual bool NeedUVConversion() const { return false; }

    virtual bool VerifyChecksums() { return true; }

    static int AnimatedName(const char* name, char* prefix, char* postfix);
    static int AnimatedNumber(const char* name);

    void GetQuadrantSurfaces(const char* name, QuadrantSurfaces quadrants) const;

  protected:
    int FindSurface(const char* name) const; // pattern matching ('?')
    const SurfaceInfo& GetSurface(const char* name) const;

  public:
    AbstractTextBank();
    virtual ~AbstractTextBank();

    void LockAllTextures();   // disable automatic release
    void UnlockAllTextures(); // enable automatic release

    NoCopy(AbstractTextBank)
};

extern bool NoTextures;

bool TrySplitBlendedTerrainTextureName(const char* pureName, char quadrantTextures[4][3]);

} // namespace Poseidon
Poseidon::Texture* GlobPreloadTexture(Poseidon::Foundation::RStringB name);
namespace Poseidon
{
Ref<Texture> GlobLoadTexture(RStringB name);
Ref<Texture> GlobLoadTextureInterpolated(RStringB n1, RStringB n2, float factor);
AnimatedTexture* GlobLoadTextureAnimated(RStringB name);

class AnimatedTexture : public RefArray<Texture>, public RefCount
{
    friend class Texture;

    AbstractTextBank* _bank;

  public:
    AnimatedTexture(AbstractTextBank* bank);
    ~AnimatedTexture() override;

    void Remove(Texture* text);
    const char* Name() const; // name of first texture
    RStringB GetName() const; // name of first texture

  private:
    void operator=(const AnimatedTexture& src);
    AnimatedTexture(const AnimatedTexture& src);
};

LLink<Texture>& GetDefaultTexture();

} // namespace Poseidon
#endif
