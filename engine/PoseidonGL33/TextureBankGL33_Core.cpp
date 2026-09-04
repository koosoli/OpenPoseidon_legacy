#include <Poseidon/Core/Application.hpp>

#include <Poseidon/Dev/Diag/ScopedTimer.hpp>
#include <Poseidon/IO/Streams/QBStream.hpp>
#include <PoseidonGL33/TextureGL33.hpp>
#include <PoseidonGL33/EngineGL33.hpp>
#include <Poseidon/Graphics/Textures/LooseTextures.hpp>
#include <Poseidon/Foundation/Math/Statistics.hpp>
#include <Poseidon/Core/Global.hpp>

#include <glad/gl.h>

extern int MipmapSizeGL33(PacFormat format, int w, int h);

static const char* FormatNameGL(PacFormat format)
{
    switch (format)
    {
        case PacP8:
            return "PacP8";
        case PacAI88:
            return "PacAI88";
        case PacRGB565:
            return "PacRGB565";
        case PacARGB1555:
            return "PacARGB1555";
        case PacARGB4444:
            return "PacARGB4444";
        case PacARGB8888:
            return "PacARGB8888";
        case PacDXT1:
            return "PacDXT1";
        case PacDXT3:
            return "PacDXT3";
        case PacDXT5:
            return "PacDXT5";
        default:
            return "???";
    }
}

static void SurfaceName(char* buf, int bufSize, const SurfaceInfoGL33& surface)
{
    snprintf(buf, bufSize, "\t%s,%d,%d,", FormatNameGL(surface._format), surface._w, surface._h);
}

TextBankGL33::TextBankGL33(EngineGL33* engine) : _totalAllocated(0)
{
    _reserveTextureMemory = 0;
    _engine = engine;

    CheckTextureMemory();

    _maxTextureMemory = FreeTextureMemory();

    // Small texture limit: use ~1/8 of texture memory for small textures
    int limitPixels = _limitAllocatedTextures / (2 * 1024 * 8);
    int limitPixelsPow2 = 1;
    while (limitPixelsPow2 + limitPixelsPow2 < limitPixels)
    {
        limitPixelsPow2 += limitPixelsPow2;
    }

    _maxSmallTexturePixels = limitPixelsPow2;
    saturateMax(_maxSmallTexturePixels, 4 * 4);
    LOG_DEBUG(Graphics, "GL33 Max small texture pixels: {}, {:.0f}", _maxSmallTexturePixels,
              sqrt(_maxSmallTexturePixels));

    // Surface texture residency in the memory-budget panel. Textures are the
    // largest consumer; _totalAllocated is live GPU bytes, _maxTextureMemory the
    // detected VRAM budget. Observability only — no Free hook (see header).
    _memProbe.Register(
        "Textures", 0.5f, [this] { return (size_t)_totalAllocated; }, [this] { return (size_t)_maxTextureMemory; },
        [this] { return (size_t)_texture.Size(); });
}

TextBankGL33::~TextBankGL33()
{
    UnlockAllTextures();
    DeleteAllAnimated();
    _texture.Compact();
    PoseidonAssert(_texture.Size() == 0);
    _texture.Clear();
    _detail.Free();
    _waterBump.Free();
    _specular.Free();
    _grass.Free();

    for (int i = 0; i < _freeTextures.Size(); i++)
    {
        _freeTextures[i].Free(true);
    }
    _freeTextures.Clear();
}

void TextBankGL33::Compact()
{
    _texture.Compact();
}

void TextBankGL33::StopAll() {}

bool TextBankGL33::VerifyChecksums()
{
    StatisticsByName stats;
    for (int i = 0; i < _freeTextures.Size(); i++)
    {
        const SurfaceInfoGL33& surface = _freeTextures[i];
        char name[80];
        SurfaceName(name, sizeof(name), surface);
        stats.Count(name);
    }
    LOG_DEBUG(Graphics, "GL33 Unused texture surfaces");
    stats.Report();
    stats.Clear();

    LOG_DEBUG(Graphics, "GL33 Used texture surfaces");
    for (int i = 0; i < _texture.Size(); i++)
    {
        TextureGL33* texture = _texture[i];
        if (!texture)
            continue;
        if (texture->_smallSurface.GetTexture())
        {
            char name[80];
            SurfaceName(name, sizeof(name), texture->_smallSurface);
            stats.Count(name);
        }
        if (texture->_surface.GetTexture())
        {
            char name[80];
            SurfaceName(name, sizeof(name), texture->_surface);
            stats.Count(name);
        }
    }
    stats.Report();
    stats.Clear();

    return true;
}

int TextBankGL33::Find(RStringB name1, TextureGL33* interpolate)
{
    for (int i = 0; i < _texture.Size(); i++)
    {
        TextureGL33* texture = _texture[i];
        if (texture)
        {
            if (name1 != texture->GetName())
                continue;
            if (texture->_interpolate != interpolate)
                continue;
            return i;
        }
    }
    return -1;
}

int TextBankGL33::FindFree()
{
    for (int i = 0; i < _texture.Size(); i++)
    {
        TextureGL33* texture = _texture[i];
        if (!texture)
            return i;
    }
    return _texture.Size();
}

TextureGL33* TextBankGL33::Copy(int from)
{
    if (from < 0)
        return nullptr;
    const TextureGL33* source = _texture[from];
    if (!source)
        return nullptr;
    const char* sName = source->Name();

    int iFree = FindFree();
    PoseidonAssert(iFree >= 0);
    TextureGL33* texture = new TextureGL33;

    if (texture->Init(sName))
        return nullptr;

    _texture.Access(iFree);
    _texture[iFree] = texture;
    return texture;
}

Ref<Texture> TextBankGL33::Load(RStringB name)
{
    int i = Find(name);
    if (i >= 0)
        return _texture[i].GetLink();

    // Cache-miss texture load triggers PAA/PAC decode and a sequence of
    // per-mip GL uploads — one of the top-three first-touch hitch sources.
    // Scoped per-call so the recursive load chain a model triggers shows up
    // as a stack of PERF lines, one per texture.  See
    const auto _perfTexLoadStart = ::Poseidon::Dev::Perf::Now();

    int iFree = _texture.Size();

    // Check existence against the loose-resolved path so a p3d-referenced
    // .pac that has a .png/.tga/.jpg sibling on disk still loads.
    RString resolved = Poseidon::Graphics::ResolveLooseTexturePath(name);
    if (!QIFStreamB::FileExist(resolved))
    {
        // A genuinely missing texture file is silently replaced by the default
        // (placeholder) texture in the caller, so a broken/incomplete asset set
        // is invisible at runtime. Surface it at WARN so it shows up in logs.
        // Not ERROR: dangling texture refs are endemic in the original data
        // (e.g. data\oblacno.pac, o\guns\beretta_bmp.pac) and the engine has
        // always tolerated them with the default texture; promoting them to a
        // --strict abort kills the stock game/viewer on its own assets.
        // Procedural names (#...) have no backing file and are not asset errors.
        const char* cname = static_cast<const char*>(name);
        if (cname && cname[0] && cname[0] != '#')
            LOG_WARN(Graphics, "GL33: Cannot load texture {}", cname);
        else
            LOG_DEBUG(Graphics, "GL33: Cannot load texture {}", cname);
        return nullptr;
    }

    Ref<TextureGL33> texture = new TextureGL33;
    if (!texture)
        return nullptr;

    if (texture->Init(name))
        return nullptr;

    _texture.Access(iFree);
    TextureGL33* txt = texture;
    _texture[iFree] = txt;

    const double _perfTexLoadMs = ::Poseidon::Dev::Perf::ElapsedMs(_perfTexLoadStart);
    if (_perfTexLoadMs >= 0.5)
    {
        LOG_DEBUG(Graphics, "PERF: TextBank::Load {} took {:.2f}ms", static_cast<const char*>(name), _perfTexLoadMs);
    }
    ::Poseidon::Dev::Perf::EmitTraceEventAsset(Poseidon::Foundation::LogCategory::Graphics, "TextBank::Load",
                                               _perfTexLoadStart, static_cast<const char*>(name));
    return txt;
}

Ref<Texture> TextBankGL33::LoadInterpolated(RStringB n1, RStringB n2, float factor)
{
    const float eps = 1.0 / 256;
    if (factor >= 1.0 - eps)
        return Load(n2);
    if (factor <= eps)
        return Load(n1);

    Ref<Texture> txt2 = Load(n2);
    TextureGL33* interpolate = static_cast<TextureGL33*>(txt2.GetRef());
    int index = Find(n1, interpolate);
    if (index >= 0)
    {
        TextureGL33* t = _texture[index];
        const float iPolEps = 1.0 / 64;
        if (fabs(t->_iFactor - factor) > iPolEps)
        {
            t->ReleaseMemory(true);
            t->ReleaseSmall(true);
            t->_iFactor = factor;
        }
        return t;
    }
    Ref<Texture> temp = Load(n1);
    int index1 = Find(n1);
    Ref<TextureGL33> t = Copy(index1);
    if (t)
    {
        t->_interpolate = interpolate;
        t->_iFactor = factor;
    }
    return t.GetRef();
}

void TextBankGL33::ReleaseAllTextures()
{
    LOG_DEBUG(Graphics, "GL33: Allocated before ReleaseAllTextures {}", _totalAllocated);
    ReserveMemory(_previousUsed, 0);
    ReserveMemory(_lastFramePartialUsed, 0);
    ReserveMemory(_lastFrameWholeUsed, 0);
    ReserveMemory(_thisFramePartialUsed, 0);
    ReserveMemory(_thisFrameWholeUsed, 0);
    LOG_DEBUG(Graphics, "GL33: Allocated after ReleaseAllTextures {}", _totalAllocated);
}

bool TextBankGL33::ReserveMemory(GL33MipCacheRoot& root, int limit)
{
    bool someReleased = false;
    while (_freeTextures.Size() > 0 && _totalAllocated > limit)
    {
        DeleteLastReleased();
        someReleased = true;
    }
    HMipCacheGL33* last = root.Last();
    while (_totalAllocated > limit)
    {
        if (!last)
            break;

        TextureGL33* texture = last->texture;
        if (texture->_inUse)
        {
            last = root.Prev(last);
            continue;
        }

        PoseidonAssert(texture->_cache == last);
        someReleased = true;
        texture->ReleaseMemory(false);
        last = root.Last();
    }
    return someReleased;
}

void TextBankGL33::ReportTextures(const char* name) {}

bool TextBankGL33::ReserveMemory(int size)
{
    if (size >= INT_MAX)
    {
        _reserveTextureMemory = 0;
    }
    return ReserveMemory(_previousUsed, _limitAllocatedTextures - size);
}

bool TextBankGL33::ForcedReserveMemory(int size)
{
    const int minRelease = 4 * 1024;
    if (size < minRelease)
        size = minRelease;

    int newLimit = _totalAllocated - size;
    LOG_DEBUG(Graphics, "GL33: ForcedReserveMemory newLimit={} _totalAllocated={}", newLimit, _totalAllocated);

    bool ret = ReserveMemory(_previousUsed, newLimit);

    if (!ret)
    {
        ret = ReserveMemory(_lastFramePartialUsed, newLimit);
        if (!ret)
        {
            ret = ReserveMemory(_lastFrameWholeUsed, newLimit);
            if (!ret)
            {
                Glob.fullDropDownChange = 0.1;
                LOG_DEBUG(Graphics, "GL33: Evicting this frame partially used texture");
                ret = ReserveMemory(_thisFramePartialUsed, newLimit);
                if (!ret)
                {
                    LOG_DEBUG(Graphics, "GL33: Evicting this frame whole used texture");
                    ret = ReserveMemory(_thisFrameWholeUsed, newLimit);
                }
            }
        }
    }
    CheckTextureMemory();
    LOG_DEBUG(Graphics, "  done {} _totalAllocated={}", ret, _totalAllocated);
    return ret;
}

int TextBankGL33::FindSurface(int w, int h, int nMipmaps, PacFormat format,
                              const AutoArray<SurfaceInfoGL33>& array) const
{
    for (int i = 0; i < array.Size(); i++)
    {
        const SurfaceInfoGL33& surface = array[i];
        if (nMipmaps != surface._nMipmaps)
            continue;
        if (surface._w != w)
            continue;
        if (surface._h != h)
            continue;
        if (surface._format == format)
            return i;
    }
    return -1;
}

void TextBankGL33::DeleteLastReleased()
{
    SurfaceInfoGL33& free = _freeTextures[0];
    int size = free.SizeUsed();
    _totalAllocated -= size;
    _thisFrameAlloc++;
    free.Free(true);
    _freeTextures.Delete(0);
}

void TextBankGL33::AddReleased(SurfaceInfoGL33& surf)
{
    _freeTextures.Add(surf);
}

void TextBankGL33::UseReleased(SurfaceInfoGL33& surf, const TextureDescGL33& desc, PacFormat format)
{
    int reuse = FindReleased(desc.w, desc.h, desc.nMipmaps, format);
    if (reuse < 0)
        return;

    SurfaceInfoGL33& reused = _freeTextures[reuse];
    surf = reused;
    reused.Free(false);
    _freeTextures.Delete(reuse);
}

void TextBankGL33::Reuse(SurfaceInfoGL33& surf, const TextureDescGL33& desc, PacFormat format)
{
    int w = desc.w;
    int h = desc.h;
    int nMipmaps = desc.nMipmaps;
    for (HMipCacheGL33* last = _previousUsed.Last(); last; last = _previousUsed.Prev(last))
    {
        TextureGL33* texture = last->texture;
        if (texture->_inUse)
            continue;

        const SurfaceInfoGL33& surface = texture->_surface;
        if (nMipmaps != surface._nMipmaps)
            continue;
        if (surface._w != w)
            continue;
        if (surface._h != h)
            continue;
        if (surface._format != format)
            continue;

        texture->ReuseMemory(surf);
        return;
    }
}

int TextBankGL33::CreateGPUSurface(SurfaceInfoGL33& surface, const TextureDescGL33& desc, PacFormat format,
                                   int totalSize)
{
    int ret = surface.CreateSurface(desc, format, totalSize);
    if (ret < 0)
    {
        LOG_DEBUG(Graphics, "GL33: CreateGPUSurface failed, trying ForcedReserveMemory");
        ForcedReserveMemory(totalSize);
        ret = surface.CreateSurface(desc, format, totalSize);
        if (ret < 0)
        {
            Poseidon::Foundation::ErrorMessage("GL33: Cannot create texture (%dx%d)", desc.w, desc.h);
            return -1;
        }
    }
    _totalAllocated += surface.SizeUsed();
    return 0;
}

Texture* TextBankGL33::CreateDynamic(int w, int h, const void* rgba, uint32_t size, bool mipmap, int /*alphaClass*/)
{
    int idx = FindFree();
    TextureGL33* tex = new TextureGL33();
    _texture.Access(idx);
    _texture[idx] = tex;
    if (!tex->InitFromRGBA(w, h, rgba, size, mipmap))
    {
        LOG_WARN(Graphics, "GL33: Failed to create dynamic texture {}x{}", w, h);
        return nullptr;
    }
    return tex;
}

void TextBankGL33::UpdateDynamic(Texture* tex, const void* rgba, uint32_t size)
{
    if (!tex || !rgba)
        return;
    static_cast<TextureGL33*>(tex)->UpdateRGBA(rgba, size);
}
