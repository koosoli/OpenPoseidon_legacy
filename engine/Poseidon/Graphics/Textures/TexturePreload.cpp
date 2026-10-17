#include <Poseidon/Game/Compat/ArmaOriginalUi.hpp>
#include <Poseidon/Graphics/Textures/TextureBank.hpp>
#include <Poseidon/World/Scene/Scene.hpp>
#include <Poseidon/Graphics/Textures/TexturePreload.hpp>
#include <Poseidon/Graphics/Core/Engine.hpp>
#include <Poseidon/IO/ParamFileExt.hpp>
#include <Poseidon/Foundation/Containers/Array.hpp>
#include <Poseidon/Foundation/Strings/RString.hpp>
#include <Poseidon/Foundation/Types/LLinks.hpp>
#include <Poseidon/Foundation/Types/Pointers.hpp>

namespace Poseidon
{
PreloadedTextures::PreloadedTextures() = default;

PreloadedTextures::~PreloadedTextures()
{
    _data.Clear();
}

void PreloadedTextures::Preload(bool all)
{
    if (_data.Size() >= MaxPreloadedTexture)
    {
        return;
    }
    _data.Clear();
    _data.Resize(MaxPreloadedTexture);

    const ParamEntry& tp = Remaster >> "CfgPreloadTextures";
    auto loadTex = [&](const char* key) -> Ref<Texture>
    {
        RStringB name = tp >> key;
        if (name.GetLength() == 0)
            return nullptr;
        return GlobLoadTexture(name);
    };

    _data[TextureDefault] = loadTex("textureDefault");
    GetDefaultTexture() = _data[TextureDefault].GetRef();
    _data[TextureWhite] = loadTex("textureWhite");
    _data[TextureBlack] = loadTex("textureBlack");
    _data[TextureLine] = loadTex("textureLine"); // 8x8
    // we want 4x4 mipmap level only
    if (_data[TextureLine])
    {
        _data[TextureLine]->SetMipmapRange(1, 1);
    }

    if (!all)
    {
        return;
    }

    _data[TrackTexture] = loadTex("trackTexture");
    _data[TrackTextureFour] = loadTex("trackTextureFour");

    _data[Corner] = GlobLoadTexture(GetPictureName(ArmaOriginalUi::Config("CfgInGameUI") >> "imageCornerElement"));

    const ParamEntry& wrapper = Pars >> "CfgWrapperUI";
    _data[DialogBackground] = GlobLoadTexture(GetPictureName(wrapper >> "Background" >> "texture"));
    _data[DialogTitle] = GlobLoadTexture(GetPictureName(wrapper >> "TitleBar" >> "texture"));
    _data[DialogGroup] = GlobLoadTexture(GetPictureName(wrapper >> "GroupBox2" >> "texture"));

    const ParamEntry& cursor = ArmaOriginalUi::Config("CfgInGameUI") >> "Cursor";
    _data[CursorLocked] = GlobLoadTexture(GetPictureName(cursor >> "lock_target"));
    _data[CursorTarget] = GlobLoadTexture(GetPictureName(cursor >> "select_target"));
    _data[CursorAim] = GlobLoadTexture(GetPictureName(cursor >> "aim"));
    _data[CursorWeapon] = GlobLoadTexture(GetPictureName(cursor >> "weapon"));
    _data[CursorStrategy] = GlobLoadTexture(GetPictureName(cursor >> "tactical"));
    _data[CursorStrategyMove] = GlobLoadTexture(GetPictureName(cursor >> "move"));
    _data[CursorStrategyAttack] = GlobLoadTexture(GetPictureName(cursor >> "attack"));
    _data[CursorStrategyGetIn] = GlobLoadTexture(GetPictureName(cursor >> "getin"));
    _data[CursorStrategySelect] = GlobLoadTexture(GetPictureName(cursor >> "select"));
    _data[CursorStrategyWatch] = GlobLoadTexture(GetPictureName(cursor >> "watch"));
    _data[CursorOutArrow] = GlobLoadTexture(GetPictureName(cursor >> "outArrow"));

    _data[Compass000] = loadTex("compass000");
    _data[Compass090] = loadTex("compass090");
    _data[Compass180] = loadTex("compass180");
    _data[Compass270] = loadTex("compass270");
#define MAX_S(id)  \
    if (_data[id]) \
    _data[id]->SetMaxSize(256)
    MAX_S(Compass000);
    MAX_S(Compass090);
    MAX_S(Compass180);
    MAX_S(Compass270);
#undef MAX_S

    _data[SkyBright] = loadTex("skyBright");
    _data[SkyCloudy] = loadTex("skyCloudy");
    _data[SkyRainy] = loadTex("skyRainy");

    _data[TextureRain] = loadTex("textureRain");

    const ParamEntry& entry = Pars >> "CfgWorlds";
    _data[SignSideE] = GlobLoadTexture(GetPictureName(entry >> "eastSign"));
    _data[SignSideW] = GlobLoadTexture(GetPictureName(entry >> "westSign"));
    _data[SignSideG] = GlobLoadTexture(GetPictureName(entry >> "guerrilaSign"));
    _data[FlagSideE] = GlobLoadTexture(GetPictureName(entry >> "eastFlag"));
    _data[FlagSideW] = GlobLoadTexture(GetPictureName(entry >> "westFlag"));
    _data[FlagSideG] = GlobLoadTexture(GetPictureName(entry >> "guerrilaFlag"));

    // Flares: stored as an array entry; each element is a filename.
    const ParamEntry& flares = tp >> "flares";
    for (int s = 0; s < 16; s++)
    {
        if (s >= flares.GetSize())
            break;
        RStringB name = flares[s];
        if (name.GetLength() == 0)
            continue;
        _data[Flare0 + s] = GlobLoadTexture(name);
    }
}

void PreloadedTextures::Clear()
{
    _data.Clear();
}

Texture* PreloadedTextures::New(RStringB name)
{
    // make texture permanent
    // assign new id
    // search data for the same texture
    for (int i = 0; i < _data.Size(); i++)
    {
        Texture* dataI = _data[i];
        if (dataI && dataI->GetName() == name)
        {
            return dataI;
        }
    }
    Ref<Texture> txt = GlobLoadTexture(name);
    _data.Add(txt);
    return txt;
}

Texture* PreloadedTextures::New(PreloadedTexture id)
{
    // predefined texture ids
    return _data[id];
}

PreloadedTextures GPreloadedTextures;

} // namespace Poseidon
Texture* GlobPreloadTexture(RStringB name)
{
    using namespace Poseidon;
    return GPreloadedTextures.New(name);
}
namespace Poseidon
{

} // namespace Poseidon
