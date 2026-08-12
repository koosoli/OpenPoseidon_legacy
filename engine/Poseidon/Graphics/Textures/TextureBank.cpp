#include <Poseidon/Core/Application.hpp>
#include <Poseidon/Graphics/Textures/TextureBank.hpp>
#include <Poseidon/Graphics/Core/Engine.hpp>
#include <Poseidon/IO/ParamFileExt.hpp>
#include <Poseidon/IO/Streams/QBStream.hpp>
#include <Poseidon/Foundation/Strings/Bstring.hpp>
#include <Poseidon/Foundation/Common/Filenames.hpp>
#include <Poseidon/Core/Global.hpp>
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <Poseidon/Foundation/Framework/DebugLog.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/Foundation/Strings/RString.hpp>
#include <Poseidon/Foundation/Types/LLinks.hpp>

namespace Poseidon
{

int AbstractTextBank::AnimatedNumber(const char* name)
{
    // animated name has structure: name.number.ext (e.g. buch.01.tga)
    const char* n = findLastSep(name);
    if (!n)
    {
        LOG_DEBUG(Graphics, "Bad texture name '{}'", name);
        return -1;
    }
    n++;
    const char* nn = strchr(n, '.');
    // no dot - cannot be animated
    if (!nn)
    {
        return -1;
    }
    // check if there is another dot after this one
    nn++;
    const char* ext = strchr(nn, '.');
    if (!ext)
    {
        return -1;
    }
    // number is placed between nn and ext
    // scan number, detect error
    int num = 0;
    while (isdigit(*nn))
    {
        num = num * 10 + *nn - '0';
        nn++;
    }
    if (*nn != '.')
    {
        LOG_DEBUG(Graphics, "Texture name {} looked like animated", name);
        return -1;
    }
    return num;
}

int AbstractTextBank::AnimatedName(const char* name, char* prefix, char* postfix)
{
    *postfix = 0;
    ::strcpy(prefix, name);
    char* n = findLastSep(prefix);
    PoseidonAssert(n);
    n++;
    char* nn = strchr(n, '.');
    if (!nn)
    {
        return -1;
    }
    int ret = atoi(nn + 1);
    char* ext = strchr(nn + 1, '.');
    if (!ext)
    {
        return -1;
    }
    ::strcpy(postfix, ext); // use postfix
    *++nn = 0;              // terminate prefix
    return ret;
}

const char* AnimatedTexture::Name() const
{
    if (Size() <= 0)
    {
        return "";
    }
    Texture* tex = Get(0);
    if (!tex)
    {
        return RStringB();
    }
    return tex->GetName();
}

RStringB AnimatedTexture::GetName() const
{
    if (Size() <= 0)
    {
        return "";
    }
    Texture* tex = Get(0);
    if (!tex)
    {
        return RStringB();
    }
    return tex->GetName();
}

AnimatedTexture* AbstractTextBank::LoadAnimated(RStringB name)
{
    // scan animated textures for a match
    int i;
    for (i = 0; i < _animatedTextures.Size(); i++)
    {
        AnimatedTexture* tex = _animatedTextures[i];
        if (tex && tex->GetName() == name)
        {
            return tex;
        }
    }
    // scan normal textures for a match

    char prefix[256];
    char postfix[256];
    AnimatedName(name, prefix, postfix);
    // try to find texture with the same prefix
    int prefLen = strlen(prefix);
    for (i = 0; i < _animatedTextures.Size(); i++)
    {
        AnimatedTexture* tex = _animatedTextures[i];
        if (!strncmp(tex->Name(), name, prefLen))
        {
            Log("Double animated texture %s", (const char*)name);
            return tex;
        }
    }
    // load data
    AnimatedTexture* ret = new AnimatedTexture(this);
    int start = AnimatedNumber(name);
    char loadName[256];
    for (i = start;; i++)
    {
        snprintf(loadName, sizeof(loadName), "%s%02d%s", prefix, i, postfix);
        if (!QIFStreamB::FileExist(loadName))
        {
            break;
        }
        Ref<Texture> texture = Load(loadName);
        if (texture)
        {
            ret->Add(texture);
            texture->_inAnimation = ret;
        }
    }
    if (ret->Size() <= 0)
    {
        // create dummy animated texture - placeholder
        Ref<Texture> texture = Load(name);
        ret->Add(texture);
    }
    int free = _animatedTextures.Find(nullptr);
    if (free >= 0)
    {
        _animatedTextures[free] = ret;
    }
    else
    {
        _animatedTextures.Add(ret);
    }
    return ret;
}

void AbstractTextBank::DeleteAnimated(AnimatedTexture* texture)
{
    int index = _animatedTextures.Find(texture);
    PoseidonAssert(index >= 0);
    if (index >= 0)
    {
        _animatedTextures.Delete(index);
    }
}

AnimatedTexture::AnimatedTexture(AbstractTextBank* bank) : _bank(bank) {}

AnimatedTexture::~AnimatedTexture()
{
    int i, n = Size();
    if (n > 0)
    {
        Log("Delete animated texture '%s'", Name());
    }
    AbstractTextBank* bank = _bank;
    _bank = nullptr;
    // remove reference from the bank
    //(bank ? bank : _bank)->DeleteAnimated(this);
    for (i = 0; i < n; i++)
    {
        // unlink animation from all its textures
        Texture* texture = Get(i);
        // forget all textures
        if (texture)
        {
            texture->_inAnimation = nullptr;
        }
        Set(i).Free();
    }
    Clear();
}

void AnimatedTexture::Remove(Texture* text)
{
    int i, n = Size();
    for (i = 0; i < n; i++)
    {
        if ((*this)[i] == text)
        {
            Delete(i);
            break;
        }
    }
}

Texture::Texture() : _inAnimation(nullptr)
{
    _refCountLocked = false;
}

void Texture::SetName(RStringB name)
{
    _name = name;
    // use surface information to acquire roughness
    const SurfaceInfo& surface = GLOB_ENGINE->TextBank()->GetSurface(name);
    _roughness = surface._roughness;
    _dustness = surface._dustness;
    _soundEnv = surface._soundEnv;
#if _ENABLE_CHEATS
    _character = surface._character;
#endif
    /*
    if (strstr(name,"\\pi.paa"))
    {
        LOG_DEBUG(Graphics, "Tex {}, rough {}",(const char *)name,surface._roughness);
    }
    */

    _mipmapNeeded = INT_MAX;
    _mipmapWanted = INT_MAX;
}

Texture::~Texture()
{
    // if it is locked, it is too late to unlock it
    // actualy, if it is locked, it should not be destroyed
    // texture bank unlock all textures before destroying itself
    // unless whole bank is
    DoAssert(!_refCountLocked);
    if (_inAnimation)
    {
        // if the texture is the last in some animated texture, remove whole animation
        // reference to any texture of the animation means reference to whole animation
        // delete any other textures in animation
        AnimatedTexture* animation = _inAnimation;
        _inAnimation = nullptr;
        if (animation->_bank)
        {
            animation->_bank->DeleteAnimated(animation);
        }
    }
}

void Texture::Lock()
{
    if (_refCountLocked)
    {
        return;
    }
    _refCountLocked = true;
    AddRef();
}

void Texture::Unlock()
{
    if (!_refCountLocked)
    {
        return;
    }
    _refCountLocked = false;
    Release();
}

int Texture::AnimationLength() const
{
    if (!_inAnimation)
    {
        return 1; // no animation
    }
    return _inAnimation->Size();
}

Texture* Texture::GetAnimation(int i) const
{
    if (!_inAnimation)
    {
        return const_cast<Texture*>(this); // no animation
    }
    PoseidonAssert(i >= 0 && i < _inAnimation->Size());
    return (*_inAnimation)[i];
}

void Texture::PrepareMipmap(int wanted, int needed)
{
    saturateMin(_mipmapNeeded, needed);
    saturateMin(_mipmapWanted, wanted);
}

void Texture::ResetMipmap()
{
    _mipmapNeeded = INT_MAX;
    _mipmapWanted = INT_MAX;
}

bool NoTextures = false;

Ref<Texture> GlobLoadTexture(RStringB name)
{
    if (NoTextures)
    {
        return nullptr;
    }
    if (name.GetLength() <= 0)
    {
        return nullptr;
    }
    if (AbstractTextBank::AnimatedNumber(name) >= 0)
    {
        AnimatedTexture* animated = GlobLoadTextureAnimated(name);
        return animated->Get(0);
    }
    return GLOB_ENGINE->TextBank()->Load(name);
}
Ref<Texture> GlobLoadTextureInterpolated(RStringB n1, RStringB n2, float factor)
{
    if (NoTextures)
    {
        return nullptr;
    }
    return GLOB_ENGINE->TextBank()->LoadInterpolated(n1, n2, factor);
}
AnimatedTexture* GlobLoadTextureAnimated(RStringB name)
{
    if (NoTextures)
    {
        return nullptr;
    }
    return GLOB_ENGINE->TextBank()->LoadAnimated(name);
}

AbstractTextBank::AbstractTextBank()
{
    // load surface information
    // load first from param file
    if (Pars.GetEntryCount() > 0)
    {
        const ParamEntry& par = Pars >> "CfgSurfaces";
        int i;
        for (i = 0; i < par.GetEntryCount(); i++)
        {
            const ParamEntry& entry = par.GetEntry(i);
            if (entry.IsClass())
            {
                SurfaceInfo info;
                info._name = entry >> "files";
                // Keep the class name, not just its file pattern: it is the only
                // authored statement of what the surface is, and callers that need
                // to ask "is this grass" must not have to guess from spelling.
                info._class = entry.GetName();
                info._roughness = entry >> "rough";
                info._dustness = entry >> "dust";
                info._soundEnv = entry >> "soundEnviron";
                // Read unconditionally. This used to be gated behind _ENABLE_CHEATS,
                // which is 0 in every build, so the field was never populated; it is
                // the map's own clutter character and costs one string per surface.
                const ParamEntry* character = entry.FindEntry("character");
                if (character)
                {
                    info._character = *character;
                }
                _surfaces.Add(info);
            }
        }
    }
}

void AbstractTextBank::LockAllTextures()
{
    int n = NTextures();
    for (int i = 0; i < n; i++)
    {
        Texture* tex = GetTexture(i);
        if (tex)
        {
            tex->Lock();
        }
    }
}

void AbstractTextBank::UnlockAllTextures()
{
    int n = NTextures();
    for (int i = 0; i < n; i++)
    {
        Texture* tex = GetTexture(i);
        if (tex)
        {
            tex->Unlock();
        }
    }
    // check if any animated textures may be released
    for (int i = 0; i < _animatedTextures.Size(); i++)
    {
        AnimatedTexture* anim = _animatedTextures[i];
        if (!anim)
        {
            continue;
        }
        bool isUsed = false;
        for (int a = 0; a < anim->Size(); a++)
        {
            // MP demo crash fix:
            Texture* animA = anim->Get(a);
            if (animA && animA->RefCounter() > 1)
            {
                isUsed = true;
                break;
            }
        }
        if (!isUsed)
        {
            _animatedTextures.Delete(i--);
        }
    }
}

//! Wildcard pattern matching function.

/*!
'*' may be used only as last character of the pattern
'*' matches any string in name
'?' matches any single character, not zero
*/

static bool PatternMatch(const char* name, const char* pattern)
{
    for (;;)
    {
        if (*name != *pattern)
        {
            if (*pattern == '*')
            {
                return true;
            }
            if (*name == 0)
            {
                return false;
            }
            // question marks matches any character, but not zero
            if (*pattern != '?')
            {
                return false;
            }
        }
        if (*pattern == 0)
        {
            return true;
        }
        PoseidonAssert(*name != 0);
        name++, pattern++;
    }
    /*NOTREACHED*/
}

//! Wildcard pattern matching function for surface texture names.

/*!
caution: nasty trick to fix error in older data
final ?????? (six question marks) can match with 6 or 0 characters
*/

static bool PatternMatchQM6(const char* name, const char* pattern)
{
    // try matching full pattern first
    bool match = PatternMatch(name, pattern);
    if (match)
    {
        return match;
    }
    // full pattern does not match
    // check if there is trailing ??????
    int patlen = strlen(pattern);
    int namlen = strlen(name);
    // quick rejection test:
    // if ?????? should be matching, length of pattern must be length of name + 6
    if (namlen + 6 != patlen)
    {
        return false;
    }
    int qmCount = 0;
    int patend = patlen;
    while (patend > 0 && pattern[patend - 1] == '?')
    {
        qmCount++;
        patend--;
    }
    if (qmCount < 6)
    {
        return false;
    }

    // create short pattern
    const int shortPatBufSize = 256;
    int shortPatLen = patlen - 6;
    // if pattern would overflow short pattern buffer, terminate
    if (shortPatLen >= shortPatBufSize)
    {
        return false;
    }
    BString<shortPatBufSize> shortPat;
    strncpy(shortPat, pattern, shortPatLen);
    shortPat[shortPatLen] = 0;
    match = PatternMatch(name, shortPat);
    return match;
}

int AbstractTextBank::FindSurface(const char* name) const
{
    // pattern matching ('?')
    // note: wildcards are not in name, but in _surface[i]
    int i;
    for (i = 0; i < _surfaces.Size(); i++)
    {
        if (PatternMatchQM6(name, _surfaces[i]._name))
        {
            return i;
        }
    }
    return -1;
}

//! Strip path and extension, leaving the bare texture basename in buf
static void PureTextureName(const char* name, char* buf, int bufSize)
{
    const char* fName = findLastSep(name);
    fName = fName ? fName + 1 : name;
    snprintf(buf, bufSize, "%s", fName);
    char* ext = strrchr(buf, '.');
    if (ext)
    {
        *ext = 0;
    }
}

bool TrySplitBlendedTerrainTextureName(const char* pureName, char quadrantTextures[4][3])
{
    // pre-blended tiles consist of four concatenated 2-char base texture codes
    if (strlen(pureName) != 8)
    {
        return false;
    }
    for (int i = 0; i < 4; i++)
    {
        quadrantTextures[i][0] = pureName[i * 2];
        quadrantTextures[i][1] = pureName[i * 2 + 1];
        quadrantTextures[i][2] = 0;
    }
    return true;
}

const SurfaceInfo& AbstractTextBank::GetSurface(const char* name) const
{
    char sName[80];
    PureTextureName(name, sName, sizeof(sName));
    // search
    int index = FindSurface(sName);
    if (index < 0)
    {
        index = FindSurface("default");
        if (index < 0)
        {
            static const SurfaceInfo info = {};
            return info;
        }
    }
    return _surfaces[index];
}

void AbstractTextBank::GetQuadrantSurfaces(const char* name, QuadrantSurfaces quadrants) const
{
    char sName[80];
    PureTextureName(name, sName, sizeof(sName));

    auto resolve = [this](const char* pure) -> const SurfaceInfo*
    {
        int index = FindSurface(pure);
        if (index < 0)
        {
            index = FindSurface("default");
        }
        if (index < 0)
        {
            static const SurfaceInfo empty = {};
            return &empty;
        }
        return &_surfaces[index];
    };

    char quadrantTextures[4][3];
    if (TrySplitBlendedTerrainTextureName(sName, quadrantTextures))
    {
        for (int i = 0; i < 4; i++)
        {
            quadrants[i] = resolve(quadrantTextures[i]);
        }
    }
    else
    {
        const SurfaceInfo* uniform = resolve(sName);
        quadrants[0] = quadrants[1] = quadrants[2] = quadrants[3] = uniform;
    }
}

AbstractTextBank::~AbstractTextBank() = default;

void AbstractTextBank::DeleteAllAnimated()
{
    PoseidonAssert(_animatedTextures.Size() == 0);
    _animatedTextures.Clear();
}

void AbstractTextBank::StartFrame() {}

void AbstractTextBank::FinishFrame() {}

LLink<Texture>& GetDefaultTexture()
{
    static LLink<Texture> DefaultTexture;
    return DefaultTexture;
}

} // namespace Poseidon
