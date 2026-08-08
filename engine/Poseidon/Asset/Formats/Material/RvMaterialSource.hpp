#pragma once

#include <Poseidon/Asset/Addon/VirtualPath.hpp>
#include <Poseidon/IO/ParamFile/ParamFile.hpp>
#include <array>
#include <string>
#include <vector>

namespace Poseidon::Asset::Material
{

// AST-014 -- the common source form of a Real Virtuality material.
//
// A material reaches the engine two ways: as a text RVMAT beside the model, and
// embedded inside a later ODOL. They describe the same thing, so they converge
// here rather than each growing its own translator; MAT-030 reads this one shape
// and MAT-020 decides what it means.
//
// This layer deliberately INTERPRETS NOTHING. It records which shader the material
// names, which stages it declares and what each stage points at -- and stops. In
// particular it does not assign meaning to a stage index: the roadmap is explicit
// that Stage0/Stage1 have no global meaning, because what stage 1 holds depends on
// the shader family. A "normal map is always stage 1" shortcut taken here would be
// wrong for the 1,217 non-Super materials in the indexed corpus and invisible until
// something rendered badly.
//
// Everything unrecognised is preserved rather than dropped, so an unhandled key is
// visible to the ticket that needs it instead of silently absent.

// A texture reference as the material wrote it.
struct RvTextureRef
{
    std::string raw;          // exactly as written, for diagnostics
    VirtualPath path;         // canonical form; empty when procedural
    bool        isProcedural = false;

    // Procedural textures are generated, not loaded: `#(argb,8,8,3)color(0.5,...,DT)`
    // is a solid colour standing in for a detail map. They appear in 3 of the 7
    // stages of a typical Arma 2 Super material, so treating them as file paths
    // would report a third of all texture references as missing.
    static bool LooksProcedural(const std::string& text) { return !text.empty() && text[0] == '#'; }

    static RvTextureRef Parse(const std::string& text)
    {
        RvTextureRef ref;
        ref.raw          = text;
        ref.isProcedural = LooksProcedural(text);
        if (!ref.isProcedural && !text.empty())
            ref.path = VirtualPath::Parse(text);
        return ref;
    }
};

struct RvUvTransform
{
    bool                 present = false;
    std::array<float, 3> aside{{1.0f, 0.0f, 0.0f}};
    std::array<float, 3> up{{0.0f, 1.0f, 0.0f}};
    std::array<float, 3> dir{{0.0f, 0.0f, 0.0f}};
    std::array<float, 3> pos{{0.0f, 0.0f, 0.0f}};
};

struct RvStage
{
    int          index = -1; // the N in StageN; -1 when the name had no number
    RvTextureRef texture;
    // Which UV channel feeds this stage. 3,793 of 4,070 indexed RVMATs declare one,
    // so TexGen is not optional -- and this is the value AST-011C's preserved
    // #UVSet# channels have to be selected by.
    std::string   uvSource;
    RvUvTransform uvTransform;
    std::vector<std::pair<std::string, std::string>> extra; // unrecognised keys
};

// A `TexGen<N>` block: a named UV generator a stage can point at, rather than a
// texture stage itself.
//
// These carry an index exactly like a stage does, so anything treating "indexed
// class" as "stage" swallows them -- 56 of them across the indexed corpus, which
// would appear as stages with no texture and displace nothing visibly until a
// shader schema looked for the generator that had gone missing.
struct RvTexGen
{
    int           index = -1;
    std::string   uvSource;
    RvUvTransform uvTransform;
};

struct RvMaterialSource
{
    std::string origin; // where it came from, for diagnostics
    // ODOL carries material data inline; a text RVMAT is a separate file. Both land
    // here, and a consumer that must know which it was can ask.
    bool embedded = false;

    std::string pixelShaderId;
    std::string vertexShaderId;

    std::array<float, 4> ambient{{1, 1, 1, 1}};
    std::array<float, 4> diffuse{{1, 1, 1, 1}};
    std::array<float, 4> forcedDiffuse{{0, 0, 0, 0}};
    std::array<float, 4> emissive{{0, 0, 0, 1}};
    std::array<float, 4> specular{{1, 1, 1, 1}};
    float                specularPower = 1.0f;

    std::vector<RvStage> stages; // source order, index preserved, meaning not assigned
    std::vector<RvTexGen> texGens;
    // Any other indexed or named class, preserved by name so an unhandled block is
    // visible to the ticket that needs it rather than silently gone.
    std::vector<std::string> otherClasses;
    std::vector<std::pair<std::string, std::string>> extra;

    const RvStage* FindStage(int index) const
    {
        for (const auto& stage : stages)
            if (stage.index == index)
                return &stage;
        return nullptr;
    }

    // Every non-procedural texture the material references, canonicalised.
    std::vector<VirtualPath> TextureDependencies() const
    {
        std::vector<VirtualPath> found;
        for (const auto& stage : stages)
            if (!stage.texture.isProcedural && !stage.texture.path.empty())
                found.push_back(stage.texture.path);
        return found;
    }
};

namespace detail
{
inline std::string Text(const ParamEntry& entry)
{
    return std::string(entry.GetValue().Data() ? entry.GetValue().Data() : "");
}

inline int TrailingNumber(const std::string& name)
{
    size_t at = name.size();
    while (at > 0 && name[at - 1] >= '0' && name[at - 1] <= '9')
        --at;
    if (at == name.size())
        return -1;
    return std::atoi(name.c_str() + at);
}

inline bool EqualsNoCase(const std::string& a, const char* b)
{
    size_t i = 0;
    for (; a[i] && b[i]; ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    return a[i] == '\0' && b[i] == '\0';
}

inline void ReadArray(const ParamClass& owner, const char* name, float* out, int count)
{
    const ParamEntry* entry = owner.FindEntry(name);
    if (!entry || !entry->IsArray())
        return;
    for (int i = 0; i < count && i < entry->GetSize(); ++i)
        out[i] = entry->operator[](i).GetFloat();
}

inline void ReadVec3(const ParamClass& owner, const char* name, std::array<float, 3>& out)
{
    ReadArray(owner, name, out.data(), 3);
}
} // namespace detail

// Build the IR from an already-parsed config class.
//
// The caller owns parsing, and must hand over a PREPROCESSED config: real RVMATs
// contain `//` comments, and ParamFile's raw stream parser loses sync on them --
// 148 of the 4,075 corpus materials reported fewer stages than they declare until
// the preprocessing entry point (`ParamFile::Parse(path)`) was used instead.
//
// Taking a ParamClass rather than a path is what lets a text RVMAT and an ODOL's
// embedded material use the same code: the caller supplies whichever it has. It
// also means this reuses the engine's own config parser -- including its binarized
// support -- instead of a second, subtly different one.
inline RvMaterialSource ParseRvMaterial(const ParamClass& root, std::string origin, bool embedded = false)
{
    RvMaterialSource material;
    material.origin   = std::move(origin);
    material.embedded = embedded;

    detail::ReadArray(root, "ambient", material.ambient.data(), 4);
    detail::ReadArray(root, "diffuse", material.diffuse.data(), 4);
    detail::ReadArray(root, "forcedDiffuse", material.forcedDiffuse.data(), 4);
    // Spelled "emmisive" in the files. Reproducing the source spelling here rather
    // than correcting it is the difference between reading the value and silently
    // defaulting it on every material in the corpus.
    detail::ReadArray(root, "emmisive", material.emissive.data(), 4);
    detail::ReadArray(root, "specular", material.specular.data(), 4);

    // ParamEntry converts via operator float(); GetFloat() lives on the array-value
    // interface, which is what ReadArray uses for element access.
    if (const ParamEntry* power = root.FindEntry("specularPower"))
        material.specularPower = static_cast<float>(*power);

    for (int i = 0; i < root.GetEntryCount(); ++i)
    {
        const ParamEntry& entry = root.GetEntry(i);
        const std::string name(entry.GetName().Data() ? entry.GetName().Data() : "");

        if (entry.IsClass())
        {
            const ParamClass* stageClass = entry.GetClassInterface();
            if (!stageClass)
                continue;

            // Match on the name with its trailing digits removed. `Stage1` and
            // `TexGen0` both look like "indexed class"; only the former is a stage.
            std::string bare = name;
            while (!bare.empty() && bare.back() >= '0' && bare.back() <= '9')
                bare.pop_back();

            if (detail::EqualsNoCase(bare, "TexGen"))
            {
                RvTexGen texGen;
                texGen.index = detail::TrailingNumber(name);
                if (const ParamEntry* uvSource = stageClass->FindEntry("uvSource"))
                    texGen.uvSource = detail::Text(*uvSource);
                if (const ParamEntry* transform = stageClass->FindEntry("uvTransform"))
                {
                    if (const ParamClass* uv = transform->GetClassInterface())
                    {
                        texGen.uvTransform.present = true;
                        detail::ReadVec3(*uv, "aside", texGen.uvTransform.aside);
                        detail::ReadVec3(*uv, "up", texGen.uvTransform.up);
                        detail::ReadVec3(*uv, "dir", texGen.uvTransform.dir);
                        detail::ReadVec3(*uv, "pos", texGen.uvTransform.pos);
                    }
                }
                material.texGens.push_back(std::move(texGen));
                continue;
            }
            if (!detail::EqualsNoCase(bare, "Stage"))
            {
                material.otherClasses.push_back(name);
                continue;
            }

            RvStage stage;
            stage.index = detail::TrailingNumber(name);
            if (const ParamEntry* texture = stageClass->FindEntry("texture"))
                stage.texture = RvTextureRef::Parse(detail::Text(*texture));
            if (const ParamEntry* uvSource = stageClass->FindEntry("uvSource"))
                stage.uvSource = detail::Text(*uvSource);

            if (const ParamEntry* transform = stageClass->FindEntry("uvTransform"))
            {
                if (const ParamClass* uv = transform->GetClassInterface())
                {
                    stage.uvTransform.present = true;
                    detail::ReadVec3(*uv, "aside", stage.uvTransform.aside);
                    detail::ReadVec3(*uv, "up", stage.uvTransform.up);
                    detail::ReadVec3(*uv, "dir", stage.uvTransform.dir);
                    detail::ReadVec3(*uv, "pos", stage.uvTransform.pos);
                }
            }

            for (int j = 0; j < stageClass->GetEntryCount(); ++j)
            {
                const ParamEntry& inner = stageClass->GetEntry(j);
                const std::string key(inner.GetName().Data() ? inner.GetName().Data() : "");
                if (inner.IsClass() || detail::EqualsNoCase(key, "texture") ||
                    detail::EqualsNoCase(key, "uvSource"))
                    continue;
                stage.extra.emplace_back(key, detail::Text(inner));
            }
            material.stages.push_back(std::move(stage));
            continue;
        }

        if (detail::EqualsNoCase(name, "PixelShaderID"))
            material.pixelShaderId = detail::Text(entry);
        else if (detail::EqualsNoCase(name, "VertexShaderID"))
            material.vertexShaderId = detail::Text(entry);
        else if (!detail::EqualsNoCase(name, "ambient") && !detail::EqualsNoCase(name, "diffuse") &&
                 !detail::EqualsNoCase(name, "forcedDiffuse") && !detail::EqualsNoCase(name, "emmisive") &&
                 !detail::EqualsNoCase(name, "specular") && !detail::EqualsNoCase(name, "specularPower"))
        {
            material.extra.emplace_back(name, detail::Text(entry));
        }
    }

    return material;
}

} // namespace Poseidon::Asset::Material
