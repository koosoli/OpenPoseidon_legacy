// REFLECT-001 phase B: gathers what ReflectMaterial::Classify needs for one draw section
// and writes the result into WgrDraw3D::mat_reflect0/1.
//
// Off by default: with WGR_REFLECT unset (or 0) and WGR_REFLECT_DEBUG unset this returns
// before touching the draw, so both lanes stay at the zero WgrDraw3D{} gave them and the
// shader reads them nowhere -- the frame is today's frame.
//
//   WGR_REFLECT=1         classify every section and shade it (phase C: GGX sun + clear coat,
//                         sky env reflection) -- per-draw path only, i.e. vehicles and entities
//   WGR_REFLECT_DEBUG=N   classify and show debug view N instead of the lit colour:
//                         1 class, 2 roughness, 3 F0/metal, 4 clear coat, 5 source
//
// Addons may declare their own surfaces (owner decision 1); CWA 1.99 ignores the class:
//
//   class CfgOpMaterials
//   {
//       class P406_Paint_KRF
//       {
//           textures[] = {"\p406\tex\paint_krf.paa"};
//           surface = "paint_metallic";   // matt, paint_matt, paint_gloss, paint_metallic, paint_pearl,
//                                         // chrome, glass, mirror, lamp_glass, lamp_plastic,
//                                         // rubber, plastic, bare_metal
//           roughness = 0.3;              // optional overrides of the class preset
//           f0 = 0.05; metal = 0; clearCoat = 1; clearCoatRoughness = 0.04;
//       };
//   };

#include "EngineWgpuReflect.hpp"
#include "ReflectMaterial.hpp"

#include <Poseidon/AI/EntityAI.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/AI/EntityAIType.hpp>
#include <Poseidon/Graphics/Rendering/Lighting/Material.hpp>
#include <Poseidon/Graphics/Textures/TextureBank.hpp>
#include <Poseidon/IO/ParamFileExt.hpp>
#include <Poseidon/World/Entities/Vehicles/Transport.hpp>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unordered_map>

namespace Poseidon
{
namespace
{
namespace RM = ReflectMaterial;

int EnvInt(const char* name)
{
    const char* value = std::getenv(name);
    return value && *value ? std::atoi(value) : 0;
}

// Texture key: normalised, no leading slash, no extension -- `\p406\tex\a.paa`,
// `p406/tex/a.pac` and `P406\Tex\A` are one texture.
std::string TextureKey(std::string_view path)
{
    std::string key = RM::Normalise(path);
    const auto start = key.find_first_not_of('/');
    key.erase(0, start == std::string::npos ? key.size() : start);
    const auto slash = key.find_last_of('/');
    const auto dot = key.find_last_of('.');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
        key.erase(dot);
    return key;
}

struct Declared
{
    RM::SurfaceClass cls = RM::SurfaceClass::None;
    float roughness = -1, f0 = -1, metal = -1, clearCoat = -1, coatRoughness = -1;
};

float OptionalFloat(const ParamEntry& entry, const char* name)
{
    const ParamEntry* value = entry.FindEntry(name);
    return value ? std::clamp(float(*value), 0.0f, 1.0f) : -1.0f;
}

// CfgOpMaterials, read once. Config is complete before the first frame is drawn
// (addons and mods mount at startup), so there is nothing to invalidate.
const std::unordered_map<std::string, Declared>& DeclaredTable()
{
    static std::unordered_map<std::string, Declared> table;
    static std::once_flag once;
    std::call_once(once,
                   []
                   {
                       const ParamEntry* root = Pars.FindEntry("CfgOpMaterials");
                       if (!root || !root->IsClass())
                           return;
                       for (int i = 0; i < root->GetEntryCount(); ++i)
                       {
                           const ParamEntry& entry = root->GetEntry(i);
                           if (!entry.IsClass())
                               continue;
                           const ParamEntry* surface = entry.FindEntry("surface");
                           const ParamEntry* textures = entry.FindEntry("textures");
                           if (!surface || !textures || !textures->IsArray())
                               continue;
                           Declared declared;
                           declared.cls = RM::ClassFromName(static_cast<RStringB>(*surface).Data());
                           if (declared.cls == RM::SurfaceClass::None)
                           {
                               LOG_WARN(Graphics, "CfgOpMaterials {}: unknown surface '{}'", entry.GetName().Data(),
                                        static_cast<RStringB>(*surface).Data());
                               continue;
                           }
                           declared.roughness = OptionalFloat(entry, "roughness");
                           declared.f0 = OptionalFloat(entry, "f0");
                           declared.metal = OptionalFloat(entry, "metal");
                           declared.clearCoat = OptionalFloat(entry, "clearCoat");
                           declared.coatRoughness = OptionalFloat(entry, "clearCoatRoughness");
                           for (int t = 0; t < textures->GetSize(); ++t)
                               table[TextureKey((*textures)[t].GetValue().Data())] = declared;
                       }
                       LOG_INFO(Graphics, "REFLECT: {} texture(s) declared in CfgOpMaterials", table.size());
                   });
    return table;
}

struct CacheKey
{
    const void* texture;
    uint64_t material;
    uint32_t context; // bit 0 vehicle, bit 1 military
    bool operator==(const CacheKey&) const = default;
};

struct CacheKeyHash
{
    size_t operator()(const CacheKey& k) const noexcept
    {
        size_t h = std::hash<const void*>{}(k.texture);
        h ^= std::hash<uint64_t>{}(k.material) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
        return h ^ (size_t(k.context) * 0x85ebca6bu);
    }
};

struct CacheValue
{
    const char* textureName; // interned bank name; a recycled Texture address shows up here
    RM::Params params;
};

} // namespace

bool ReflectLanesEnabled()
{
    static const bool on = EnvInt("WGR_REFLECT") != 0 || EnvInt("WGR_REFLECT_DEBUG") != 0;
    return on;
}

void FillReflectLanes(WgrDraw3D& d, const Object* owner, const Texture* texture, const TexMaterial* material)
{
    if (!ReflectLanesEnabled())
        return;
    static const int debugView = std::clamp(EnvInt("WGR_REFLECT_DEBUG"), 0, 5);

    bool isVehicle = false;
    bool military = false;
    if (owner)
    {
        if (const auto* transport = dyn_cast<const Transport>(owner))
        {
            isVehicle = true;
            if (const EntityAIType* type = transport->GetType())
            {
                const TargetSide side = type->GetTypicalSide();
                military = side == TEast || side == TWest || side == TGuerrila;
                // A civilian class name wins over the side: the first logged run (2026-10-22)
                // read Arma 3's C_Van_01_transport_F as military (its side comes from a base
                // class), which painted a white civilian van matt.
                if (military)
                {
                    const std::string cls = RM::Normalise(type->GetName().Data());
                    if (cls.rfind("c_", 0) == 0 || cls.find("civ") != std::string::npos)
                        military = false;
                }
            }
        }
    }

    const char* textureName = texture ? texture->Name() : nullptr;
    const CacheKey key{texture, material ? material->Serial() : 0u, uint32_t(isVehicle) | uint32_t(military) << 1};
    // Per thread, so the draw path takes no lock; a section classified on two threads
    // is classified twice, to the same answer.
    thread_local std::unordered_map<CacheKey, CacheValue, CacheKeyHash> cache;
    auto it = cache.find(key);
    if (it == cache.end() || it->second.textureName != textureName)
    {
        RM::Inputs in;
        in.texture = textureName ? std::string_view(textureName) : std::string_view();
        if (material)
        {
            in.material = material->GetName().Data();
            in.pixelShader = material->_pixelShaderId;
            in.specular = std::max(material->_specular.R(), std::max(material->_specular.G(), material->_specular.B()));
            in.specularPower = material->_specularPower;
            in.hasEnvMap = material->_hasEnvMap;
            in.fresnelGlass = RM::Normalise(material->_fresnelStage).find("fresnelglass") != std::string::npos;
        }
        in.isVehicle = isVehicle;
        in.military = military;
        if (!in.texture.empty())
        {
            const auto& table = DeclaredTable();
            if (const auto found = table.find(TextureKey(in.texture)); found != table.end())
            {
                in.declared = found->second.cls;
                in.declaredRoughness = found->second.roughness;
                in.declaredF0 = found->second.f0;
                in.declaredMetal = found->second.metal;
                in.declaredClearCoat = found->second.clearCoat;
                in.declaredCoatRoughness = found->second.coatRoughness;
            }
        }
        it = cache.insert_or_assign(key, CacheValue{textureName, RM::Classify(in)}).first;
        // WGR_REFLECT_LOG=1 (or any debug view): one line per newly classified section, so a
        // wrong colour in the debug view can be traced to its texture, material and rule. The
        // first test read whole cars as lamps and mirrors; the pictures alone could not say why.
        static const bool logClasses = EnvInt("WGR_REFLECT_LOG") != 0 || debugView != 0;
        static std::atomic<int> logged{0};
        if (logClasses && logged.fetch_add(1, std::memory_order_relaxed) < 4000)
        {
            const RM::Params& c = it->second.params;
            LOG_INFO(Graphics,
                     "REFLECT class={} src={} r={:.2f} f0={:.2f} metal={:.1f} coat={:.1f} veh={} mil={} env={} "
                     "ps='{}' spec={:.2f}/{:.0f} owner='{}' tex='{}' mat='{}'",
                     int(c.cls), int(c.source), c.roughness, c.f0, c.metal, c.clearCoat, int(isVehicle), int(military),
                     int(in.hasEnvMap), std::string(in.pixelShader), in.specular, in.specularPower,
                     owner ? owner->GetDebugName().Data() : "-", std::string(in.texture), std::string(in.material));
        }
    }
    const RM::Params& p = it->second.params;
    d.mat_reflect0 = {p.roughness, p.f0, p.metal, p.clearCoat};
    d.mat_reflect1 = {float(p.cls), p.coatRoughness, float(p.source), float(debugView)};
}

} // namespace Poseidon
