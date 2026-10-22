#include <Poseidon/Core/Application.hpp>
#include <Poseidon/Asset/Formats/Material/RvMaterialSource.hpp>
#include <Poseidon/Asset/Formats/Material/ShaderSchema.hpp>
#include <Poseidon/IO/Streams/QBStream.hpp>

#include <cctype>
#include <cstdio>
#include <string>
#include <Poseidon/Graphics/Rendering/Lighting/Material.hpp>
#include <Poseidon/IO/ParamFileExt.hpp>
#include <Poseidon/Graphics/Core/Engine.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/Foundation/platform.hpp>

namespace Poseidon
{

static uint64_t NextTexMaterialSerial()
{
    static std::atomic<uint64_t> next{1};
    return next.fetch_add(1, std::memory_order_relaxed);
}

TexMaterial::TexMaterial()
    : _emmisive(HBlack), _ambient(HWhite), _diffuse(HWhite), _forcedDiffuse(HBlack), _specular(HBlack),
      _bumpmapFactor(0), _detailmapFactor(0), _specularPower(0), _serial(NextTexMaterialSerial())
{
}

template <class Type>
void LoadValue(Type& dst, const ParamEntry& cfg, const char* name, const Type& defVal)
{
    const ParamEntry* entry = cfg.FindEntry(name);
    if (!entry)
    {
        dst = defVal;
    }
    else
    {
        GetValue(dst, *entry);
    }
}

template <class Type>
Type LoadValue(const ParamEntry& cfg, const char* name, const Type& defVal)
{
    const ParamEntry* entry = cfg.FindEntry(name);
    if (!entry)
    {
        return defVal;
    }
    else
    {
        return *entry;
    }
}

TexMaterial::TexMaterial(const char* name) : _serial(NextTexMaterialSerial())
{
    // Keep the authored RVMAT path for section diagnostics and cache identity.
    // TextureToMaterial historically supplied it through the cache key, but a
    // model may now construct a material directly from its own materialPath.
    _name = name;
    ParamFile cfg;
    const ParamEntry* entry = nullptr;
    if (*name == '#')
    {
        // leading '#' -> name into the global material table
        entry = (Pars >> "CfgMaterials").FindEntry(name + 1);
    }
    else if (Asset::Material::IsEmatMaterialPath(name))
    {
        // An Enfusion `.emat` (Reforger sections name one, and since 2026-08-16 the
        // `xob` converter deploys it as a real text file under the texture prefix).
        // Neither parser below can read it, and `ParamFile::Parse` on an EXISTING
        // `.emat` reports "'{' encountered instead of '='" through ErrorMessage --
        // a Critical-error log line per material and exit(1) under --strict. The
        // shading it carries reaches the renderer through LoadTranslatedMaterial;
        // this object only needs the neutral constants.
        entry = nullptr;
    }
    else
    {
        // RVMAT references from model sections may be retail `\0raP` files.
        // ParseBinOrTxt only understands Poseidon's serializer layout, so consume
        // both text and retail material forms through the shared source IR.
        try
        {
            const auto source = Asset::Material::ParseRvMaterialFile(name);
            auto color = [](const std::array<float, 4>& value) {
                return Color(value[0], value[1], value[2], value[3]);
            };
            _emmisive = color(source.emissive);
            _ambient = color(source.ambient);
            _diffuse = color(source.diffuse);
            _forcedDiffuse = color(source.forcedDiffuse);
            // Arma's "Skin" shader (hands, faces: hhl.rvmat) reads emmisive and
            // forcedDiffuse as its own parameters -- subsurface tint and translucency,
            // emmisive[]={1,0.6,0.05,0.6}, forcedDiffuse[]={2.8,0.22,0.12,0.22} -- not as
            // colours. Taken as colours, every Arma 2 hand lit itself a flat salmon by day
            // and glowed orange at night (MP_Paradrop, run 19).
            if (Asset::Material::detail::EqualsNoCase(source.pixelShaderId, "Skin"))
            {
                _emmisive = HBlack;
                _forcedDiffuse = HBlack;
            }
            _specular = color(source.specular);
            _specularPower = source.specularPower;
            _pixelShaderId = source.pixelShaderId;
            if (const auto ir = Asset::Material::TranslateMaterial(source); ir.schemaKnown)
            {
                if (const auto& map = ir.Slot(Asset::Material::MaterialSlot::SpecularDetail); map.present)
                    _specularMapName = map.texture.isProcedural ? map.texture.raw : map.texture.path.canonical();
                if (const auto& fresnel = ir.Slot(Asset::Material::MaterialSlot::Fresnel); fresnel.present)
                    _fresnelStage = fresnel.texture.raw;
                _hasEnvMap = ir.Slot(Asset::Material::MaterialSlot::Environment).present;
            }
            _bumpmapFactor = 1;
            _detailmapFactor = 1;
            return;
        }
        catch (const std::exception& error)
        {
            LOG_DEBUG(Graphics, "Material {} source parse failed: {}", name, error.what());
        }

        if (cfg.ParseBinOrTxt(name))
            entry = &cfg;
    }
    if (!entry)
    {
        _emmisive = HBlack;
        _ambient = HWhite;
        _diffuse = HWhite;
        _forcedDiffuse = HBlack;
        _specular = HBlack;
        _bumpmapFactor = 0;
        _detailmapFactor = 0;
        _specularPower = 0;
        LOG_DEBUG(Graphics, "Material {} not load failed", name);
        return;
    }
    LoadValue(_emmisive, *entry, "emmisive", HBlack);
    LoadValue(_ambient, *entry, "ambient", HWhite);
    LoadValue(_diffuse, *entry, "diffuse", HWhite);
    LoadValue(_forcedDiffuse, *entry, "forcedDiffuse", HBlack);
    LoadValue(_specular, *entry, "specular", HBlack);

    _specularPower = LoadValue(*entry, "specularPower", 0);

    _bumpmapFactor = LoadValue(*entry, "bumpmapFactor", 1);
    _detailmapFactor = LoadValue(*entry, "detailmapFactor", 1);
}

// Arma's specular constant is the ceiling a per-texel map scales: Super multiplies
// it by the SMDI green channel. A backend with per-vertex lighting and no map (GL33)
// applied the ceiling everywhere, and an Arma 2 HMMWV -- specular 0.69, SMDI green
// averaging 8/255 -- came out washed to a striped grey with its camouflage gone
// (C2B_RazorTwo, focus run 10). The map's own average (TAGG AVGC) is the mean that
// per-texel product would have, so that is the factor.
float TexMaterial::SpecularMapLevel() const
{
    if (_specularMapLevel >= 0.0f)
        return _specularMapLevel;
    float level = 1.0f;
    if (!_specularMapName.empty())
    {
        if (_specularMapName[0] == '#')
        {
            // #(argb,8,8,3)color(r,g,b,a,SMDI): the specular level is g.
            std::string lower = _specularMapName;
            for (char& ch : lower)
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            float r = 0.0f, g = 0.0f;
            if (const size_t at = lower.find("color("); at != std::string::npos &&
                                                         std::sscanf(lower.c_str() + at + 6, "%f,%f", &r, &g) == 2)
                level = g;
        }
        else if (QIFStreamB::FileExist(_specularMapName.c_str()))
        {
            if (Ref<Texture> map = GlobLoadTexture(RStringB(_specularMapName.c_str())))
                level = map->GetColor().G();
        }
        level = level < 0.0f ? 0.0f : level > 1.0f ? 1.0f : level;
        LOG_DEBUG(Graphics, "Material {}: specular map {} averages {:.3f}", _name.Data(), _specularMapName, level);
    }
    _specularMapLevel = level;
    return level;
}

void TexMaterial::Combine(TLMaterial& dst, const TLMaterial& src)
{
    dst.ambient = src.ambient * _ambient;
    dst.diffuse = src.diffuse * _diffuse;
    dst.forcedDiffuse = src.forcedDiffuse * _forcedDiffuse;
    // note: combination of some properties is addition
    // those properties are zero for default material
    // and therefore it makes no sense multiplying them
    // note: src material has already acoomodation calculated in
    // this does not
    ColorVal accom = GEngine->GetAccomodateEye();
    dst.specular = src.specular + _specular * accom;
    dst.specularPower = (int)(src.specularPower + _specularPower);
    dst.emmisive = src.emmisive + _emmisive * accom;
    dst.specFlags = src.specFlags;
}

TexMaterialBank::TexMaterialBank() = default;

TexMaterialBank::~TexMaterialBank() = default;

Ref<TexMaterial> TexMaterialBank::New(const char* name)
{
    if (!name)
    {
        return nullptr;
    }
    if (auto h = _cache.Find(name); h.IsValid())
    {
        return _cache.GetStorage(h);
    }
    Ref<TexMaterial> mat = new TexMaterial(name);
    _cache.Insert(name, mat);
    return mat;
}

void TexMaterialBank::Clear()
{
    _cache.Clear();
}

TexMaterial* TexMaterialBank::TextureToMaterial(Texture* tex)
{
    if (!tex)
    {
        return nullptr;
    }
    if (TexMaterial* existing = _cache.Lookup(tex->GetName()))
    {
        return existing;
    }
    if (!Pars.FindEntry("CfgMaterials"))
    {
        return nullptr;
    }
    const ParamEntry& texmat = Pars >> "CfgTextureToMaterial";
    for (int i = 0; i < texmat.GetEntryCount(); i++)
    {
        const ParamEntry& entry = texmat.GetEntry(i);
        if (!entry.IsClass())
        {
            continue;
        }
        const ParamEntry& texnames = entry >> "textures";
        const RStringB& matname = entry >> "material";

        for (int t = 0; t < texnames.GetSize(); t++)
        {
            const RStringB& texname = texnames[t];
            if (!strcmpi(texname, tex->GetName()))
            {
                Ref<TexMaterial> mat = New(matname);
                return mat;
            }
        }
    }

    return nullptr;
}

TexMaterialBank GTexMaterialBank;
} // namespace Poseidon
