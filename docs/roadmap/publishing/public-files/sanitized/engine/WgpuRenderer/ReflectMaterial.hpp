#pragma once

// REFLECT-001 phase B: which kind of surface a draw section is, and the physical
// parameters that kind stands for (roughness, F0, metal, clear coat).
//
// Pure functions of names and parsed material values -- no engine types -- so the rule
// table is unit-testable on its own (tests/unit/engine/WgpuRenderer/test_reflect_material.cpp)
// and the renderer only has to gather the inputs. Nothing here is read from game data
// beyond what the caller passes in.
//
// Source chain, first hit wins (design notes, owner decision 1):
//   1. Declared  -- an addon's CfgOpMaterials entry for this texture (own addons only).
//   2. RVMAT     -- Arma pixel shader id, fresnelGlass, an environment-map stage.
//   3. Part rule -- glass, mirror, chrome, lamp, rubber, interior... read from the names.
//   4. Body rule -- vehicle paint: military matt, civil gloss (owner decision 2).
//   5. Default   -- the legacy Blinn exponent converted to a roughness, else matt.

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>

namespace Poseidon::ReflectMaterial
{

enum class SurfaceClass : uint8_t
{
    None = 0,      // not classified (switch off): the lanes stay zero
    Matt,          // generic dull surface
    PaintMatt,     // military paint: no clear coat
    PaintGloss,    // civil solid paint under a clear coat
    PaintMetallic, // civil metallic / pearl paint under a clear coat
    Chrome,        // polished trim, bumpers, hub caps, reflector bowls
    Glass,         // windows, windscreens, visors
    Mirror,        // rear-view mirror glass
    LampGlass,     // headlight with a glass cover (older vehicles) over a chrome reflector
    LampPlastic,   // headlight with a polycarbonate cover (modern vehicles) over a chrome reflector
    Rubber,        // tyres, seals
    Plastic,       // trim, interior, dashboard
    BareMetal,     // unpainted metal: rifle steel, tracks, exhausts
    Count
};

enum class Source : uint8_t
{
    None = 0,
    Declared,
    Rvmat,
    PartRule,
    BodyRule,
    Default,
};

enum class Era : uint8_t
{
    Old,    // CWA / OFP data (2001)
    Mid,    // Arma 1 / Arma 2 (`ca\` prefix)
    Modern, // Arma 3 (`a3\` prefix)
};

struct Params
{
    float roughness = 0.0f;   // perceptual; GGX alpha = roughness^2
    float f0 = 0.0f;          // dielectric reflectance at normal incidence (metal: see `metal`)
    float metal = 0.0f;       // 0 dielectric, 1 conductor (F0 taken from the albedo)
    float clearCoat = 0.0f;   // strength of a second, smooth dielectric layer
    float coatRoughness = 0.0f;
    SurfaceClass cls = SurfaceClass::None;
    Source source = Source::None;
};

struct Inputs
{
    std::string_view texture;     // section texture path (any case, either slash)
    std::string_view material;    // RVMAT path or `#CfgMaterials` name, may be empty
    std::string_view model;       // model path, for the era
    std::string_view pixelShader; // RVMAT PixelShaderID, empty for OFP materials
    float specular = 0.0f;        // max(rgb) of the material's specular colour
    float specularPower = 0.0f;   // Blinn exponent, 0 = no highlight
    bool hasEnvMap = false;       // RVMAT names an environment-map stage
    bool fresnelGlass = false;    // RVMAT Fresnel stage is `fresnelGlass`
    bool isVehicle = false;       // the owner is a Transport (car, truck, tank, boat, aircraft)
    bool military = false;        // the vehicle type's typical side is not civilian
    // Declared (CfgOpMaterials) -- only used when `declared` is not None.
    SurfaceClass declared = SurfaceClass::None;
    float declaredRoughness = -1.0f; // < 0: keep the class preset
    float declaredF0 = -1.0f;
    float declaredMetal = -1.0f;
    float declaredClearCoat = -1.0f;
    float declaredCoatRoughness = -1.0f;
};

inline Params Preset(SurfaceClass cls)
{
    Params p;
    p.cls = cls;
    switch (cls)
    {
    case SurfaceClass::None: break;
    case SurfaceClass::Matt: p.roughness = 0.85f; p.f0 = 0.04f; break;
    case SurfaceClass::PaintMatt: p.roughness = 0.6f; p.f0 = 0.04f; break;
    // Stock paint is weathered: a full-strength coat read as a fresh mirror finish on every
    // 30-year-old Skoda (first test, 2026-10-22), so 0.6/0.8 in phase C. With phase D's
    // split-sum weighting the coat is no longer overstated, and the owner asked for more:
    // 0.8/0.9. Own addons declare 1.0 where they mean a fresh finish.
    case SurfaceClass::PaintGloss:
        p.roughness = 0.45f; p.f0 = 0.04f; p.clearCoat = 0.8f; p.coatRoughness = 0.08f; break;
    case SurfaceClass::PaintMetallic:
        p.roughness = 0.35f; p.f0 = 0.08f; p.clearCoat = 0.9f; p.coatRoughness = 0.06f; break;
    case SurfaceClass::Chrome: p.roughness = 0.08f; p.f0 = 0.04f; p.metal = 1.0f; break;
    case SurfaceClass::Glass: p.roughness = 0.03f; p.f0 = 0.04f; break;
    case SurfaceClass::Mirror: p.roughness = 0.02f; p.f0 = 0.04f; p.metal = 1.0f; break;
    // A lamp is a chromed reflector bowl (the base, metal) under its cover (the clear coat):
    // glass on older vehicles stays sharp, a modern polycarbonate cover is a touch softer.
    case SurfaceClass::LampGlass:
        p.roughness = 0.15f; p.f0 = 0.04f; p.metal = 1.0f; p.clearCoat = 1.0f; p.coatRoughness = 0.02f; break;
    case SurfaceClass::LampPlastic:
        p.roughness = 0.2f; p.f0 = 0.04f; p.metal = 1.0f; p.clearCoat = 1.0f; p.coatRoughness = 0.05f; break;
    case SurfaceClass::Rubber: p.roughness = 0.9f; p.f0 = 0.03f; break;
    case SurfaceClass::Plastic: p.roughness = 0.6f; p.f0 = 0.04f; break;
    case SurfaceClass::BareMetal: p.roughness = 0.45f; p.f0 = 0.04f; p.metal = 1.0f; break;
    case SurfaceClass::Count: break;
    }
    return p;
}

// Lower-case, forward slashes: the one form every name is matched in.
inline std::string Normalise(std::string_view s)
{
    std::string out(s);
    for (char& c : out)
        c = c == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// The file name without directory and extension: matching on it keeps a token in a
// folder name (`\cars\glassworks\`) from classifying every texture under it.
inline std::string_view Stem(std::string_view path)
{
    if (const auto slash = path.find_last_of('/'); slash != std::string_view::npos)
        path.remove_prefix(slash + 1);
    if (const auto dot = path.find_last_of('.'); dot != std::string_view::npos)
        path = path.substr(0, dot);
    return path;
}

inline bool HasAny(std::string_view s, std::initializer_list<std::string_view> tokens)
{
    for (auto t : tokens)
        if (s.find(t) != std::string_view::npos)
            return true;
    return false;
}

inline Era EraOf(std::string_view modelOrTexture)
{
    const std::string p = Normalise(modelOrTexture);
    std::string_view v = p;
    while (!v.empty() && v.front() == '/')
        v.remove_prefix(1);
    if (v.rfind("a3/", 0) == 0)
        return Era::Modern;
    if (v.rfind("ca/", 0) == 0)
        return Era::Mid;
    return Era::Old;
}

// Blinn-Phong exponent -> perceptual roughness. alpha^2 = 2/(n+2) is the usual
// equivalence of a Blinn lobe and a Beckmann/GGX lobe; roughness = sqrt(alpha).
inline float RoughnessFromBlinn(float n)
{
    if (n <= 0.0f)
        return 0.85f;
    const float alpha = std::sqrt(2.0f / (n + 2.0f));
    return std::clamp(std::sqrt(alpha), 0.05f, 0.95f);
}

// Rule 3: the part a texture or material name says it is. Czech tokens because the
// original data is Czech-authored (sklo = glass, zrcatko = mirror, svetlo = light,
// kolo = wheel, guma = rubber). None when no part token is present.
// A texture that names the body ("trup" is Czech for hull/body). A shiny material on it
// (Arma 1/2 put `mirror.rvmat` on the Hilux body atlas, which the second logged run showed
// as one big mirror) is a reflective paint, not a mirror or chrome part.
inline bool IsBodyTexture(std::string_view texStem)
{
    return HasAny(texStem, {"trup", "body", "_ext", "karos", "carriage", "kabin"});
}

inline SurfaceClass PartFromNames(std::string_view texStem, std::string_view matStem, Era era,
                                  bool glassShader = false)
{
    for (std::string_view s : {texStem, matStem})
    {
        if (s.empty())
            continue;
        // A glass shader is its own evidence: a mirror material with it is mirror glass.
        const bool fromMaterialOnBody = !glassShader && s.data() == matStem.data() && IsBodyTexture(texStem);
        // Same for lamps: Arma 3 draws its emissive light materials (car_headlight_emit.rvmat)
        // on the body texture (suv_01_ext_co), and a draw that starts on such a section turned
        // the whole red C_SUV_01_F into a silver lamp reflector (clean runs ef36e24 and 8f4a06b).
        if (fromMaterialOnBody && HasAny(s, {"mirror", "zrcatk", "zrcadl", "spiegel", "_zrc", "chrom", "headlight",
                                             "light", "lamp", "svetl", "reflektor", "odrazk"}))
            continue;
        // Token lists extended from the first logged class run (WGR_REFLECT_LOG, a787e47,
        // 2026-10-22): CWA names its parts in Czech (zrc = mirror, palubka = dashboard,
        // volant = steering wheel, budiky = gauges, rohoz = floor mat, odrazka = reflector,
        // podvozek/spodek = underbody, pera = springs, zasterka = mud flap, plachta = tarp,
        // drevo = wood, spz = number plate), Arma 3 marks interiors with `_int`.
        if (HasAny(s, {"mirror", "zrcatk", "zrcadl", "spiegel", "_zrc"}))
            return SurfaceClass::Mirror;
        if (HasAny(s, {"chrom", "chrome"}))
            return SurfaceClass::Chrome;
        if (HasAny(s, {"headlight", "light", "lamp", "svetl", "reflektor", "lights", "odrazk"}))
            return era == Era::Modern ? SurfaceClass::LampPlastic : SurfaceClass::LampGlass;
        if (HasAny(s, {"glass", "sklo", "skla", "okno", "okna", "window", "windshield", "windscreen", "canopy"}))
            return SurfaceClass::Glass;
        if (HasAny(s, {"tyre", "tire", "pneu", "guma", "rubber", "wheel", "kolo", "kola", "zasterk"}))
            return SurfaceClass::Rubber;
        if (HasAny(s, {"interior", "dashboard", "palub", "sedack", "seat", "cockpit", "_int", "_in_", "volant",
                       "budik", "rohoz", "kozenk", "panel", "spz", "plate", "sterac", "pristroj"}))
            return SurfaceClass::Plastic;
        if (HasAny(s, {"podvozek", "spodek", "bottom", "_pera", "plachta", "tarp", "drevo", "wood", "backpack"}))
            return SurfaceClass::Matt;
    }
    return SurfaceClass::None;
}

inline Params ClassifyRules(const Inputs& in)
{
    // 1. Declared by the addon.
    if (in.declared != SurfaceClass::None && in.declared < SurfaceClass::Count)
    {
        Params p = Preset(in.declared);
        if (in.declaredRoughness >= 0.0f) p.roughness = std::clamp(in.declaredRoughness, 0.0f, 1.0f);
        if (in.declaredF0 >= 0.0f) p.f0 = std::clamp(in.declaredF0, 0.0f, 1.0f);
        if (in.declaredMetal >= 0.0f) p.metal = std::clamp(in.declaredMetal, 0.0f, 1.0f);
        if (in.declaredClearCoat >= 0.0f) p.clearCoat = std::clamp(in.declaredClearCoat, 0.0f, 1.0f);
        if (in.declaredCoatRoughness >= 0.0f) p.coatRoughness = std::clamp(in.declaredCoatRoughness, 0.0f, 1.0f);
        p.source = Source::Declared;
        return p;
    }

    const std::string tex = Normalise(in.texture);
    const std::string mat = Normalise(in.material);
    const std::string_view texStem = Stem(tex);
    const std::string_view matStem = Stem(mat);
    const Era era = EraOf(in.model.empty() ? std::string_view(tex) : in.model);
    const std::string shader = Normalise(in.pixelShader);

    // 2. What an Arma RVMAT says outright. Glass shaders and fresnelGlass are glass;
    //    the named-part rule still decides between a pane, a mirror and a lamp.
    if (in.fresnelGlass || shader.rfind("glass", 0) == 0)
    {
        const SurfaceClass part = PartFromNames(texStem, matStem, era, true);
        Params p = Preset(part == SurfaceClass::Mirror || part == SurfaceClass::LampGlass ||
                                  part == SurfaceClass::LampPlastic
                              ? part
                              : SurfaceClass::Glass);
        p.source = Source::Rvmat;
        return p;
    }

    // 3. The part, by name -- on military and civil vehicles alike (owner decision 2).
    if (const SurfaceClass part = PartFromNames(texStem, matStem, era); part != SurfaceClass::None)
    {
        Params p = Preset(part);
        p.source = Source::PartRule;
        return p;
    }

    // An environment map on an otherwise unnamed part is the authors' own "this shines" --
    // but not on a vehicle: Arma 3 gives its PAINT an env map and a high power too, and
    // reading that as chrome turned the red C_SUV_01_F into a blue mirror (first test,
    // 2026-10-22). Vehicle bodies with an env map are metallic paint (rule 4).
    if (!in.isVehicle && in.hasEnvMap && in.specularPower >= 60.0f && in.specular >= 0.5f)
    {
        Params p = Preset(SurfaceClass::Chrome);
        p.roughness = std::max(RoughnessFromBlinn(in.specularPower), 0.06f);
        p.source = Source::Rvmat;
        return p;
    }

    // 4. Vehicle body: military matt, civil gloss. Metallic only where the authoring says
    //    so (a declared entry or an RVMAT env map); stock CWA cannot tell, so solid gloss.
    if (in.isVehicle)
    {
        Params p = Preset(in.military ? SurfaceClass::PaintMatt
                                      : (in.hasEnvMap ? SurfaceClass::PaintMetallic : SurfaceClass::PaintGloss));
        p.source = Source::BodyRule;
        return p;
    }

    // 5. Everything else keeps what its legacy highlight implied.
    Params p = Preset(SurfaceClass::Matt);
    if (in.specular > 0.02f && in.specularPower > 0.0f)
        p.roughness = RoughnessFromBlinn(in.specularPower);
    p.source = Source::Default;
    return p;
}

// Owner, 2026-10-23: military vehicles and tanks reflect nothing but their glass -- no paint
// sheen, no chrome, not even the faint sky sheen a matt finish showed in shadow. Glass, mirrors
// and lamp covers keep their class; everything else on a military vehicle is matt paint (the
// shader gives Matt and PaintMatt no environment reflection and no sun highlight at all, F.6).
// Rubber and Plastic go matt too (F.7): a tank's "wheel"/"kola" sections are steel track and road
// wheels, and the small GGX highlight Rubber keeps showed on them at dusk (opmil, 1b4166a).
// An addon's own CfgOpMaterials entry still wins.
inline bool KeepsReflectionWhenMilitary(SurfaceClass cls)
{
    return cls == SurfaceClass::Glass || cls == SurfaceClass::Mirror || cls == SurfaceClass::LampGlass ||
           cls == SurfaceClass::LampPlastic || cls == SurfaceClass::Matt || cls == SurfaceClass::PaintMatt;
}

inline Params Classify(const Inputs& in)
{
    Params p = ClassifyRules(in);
    if (in.isVehicle && in.military && p.source != Source::Declared && !KeepsReflectionWhenMilitary(p.cls))
    {
        const Source source = p.source;
        p = Preset(SurfaceClass::PaintMatt);
        p.source = source;
    }
    return p;
}

// Parse a CfgOpMaterials `surface` string. Unknown names give None (= not declared).
inline SurfaceClass ClassFromName(std::string_view name)
{
    const std::string n = Normalise(name);
    static constexpr std::array<std::pair<std::string_view, SurfaceClass>, 13> table{{
        {"matt", SurfaceClass::Matt},
        {"paint_matt", SurfaceClass::PaintMatt},
        {"paint_gloss", SurfaceClass::PaintGloss},
        {"paint_metallic", SurfaceClass::PaintMetallic},
        {"chrome", SurfaceClass::Chrome},
        {"glass", SurfaceClass::Glass},
        {"mirror", SurfaceClass::Mirror},
        {"lamp_glass", SurfaceClass::LampGlass},
        {"lamp_plastic", SurfaceClass::LampPlastic},
        {"rubber", SurfaceClass::Rubber},
        {"plastic", SurfaceClass::Plastic},
        {"bare_metal", SurfaceClass::BareMetal},
        {"paint_pearl", SurfaceClass::PaintMetallic},
    }};
    for (const auto& [key, cls] : table)
        if (n == key)
            return cls;
    return SurfaceClass::None;
}

// `fresnel(N,K)` / `#(ai,64,64,1)fresnel(1.3,7)` -> N, K. False when it is not one.
inline bool ParseFresnel(std::string_view text, float& n, float& k)
{
    const std::string t = Normalise(text);
    const auto at = t.find("fresnel(");
    if (at == std::string::npos)
        return false;
    const char* s = t.c_str() + at + 8;
    char* end = nullptr;
    n = std::strtof(s, &end);
    if (end == s || *end != ',')
        return false;
    s = end + 1;
    k = std::strtof(s, &end);
    return end != s;
}

} // namespace Poseidon::ReflectMaterial
