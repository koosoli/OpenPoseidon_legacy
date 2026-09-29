#pragma once

#include <Poseidon/Asset/Formats/Material/MaterialChannels.hpp>
#include <Poseidon/Graphics/Textures/ProceduralTexture.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace Poseidon::render
{
// Declaration-only half of a future resumable retained-model admission. The
// caller must have selected a GPU-owned section with the ordinary classifier.
// This enumerates the texture branches used by RegisterGpuModel's current
// BaseColour/SpecularDetail/NormalMap/ResolveMaterialLayers resolvers. It does
// not Load a bank texture, read pixels, create a GPU image, prove a mounted
// archive member, or set ResumableModelAdmission::finalRolesComplete.
class RetainedMaterialRoleDeclarations
{
public:
    static constexpr size_t MaxRows = 16;
    static constexpr size_t MaxName = 255;
    static constexpr size_t MaxTranslatedSlots = 64;
    static constexpr size_t MaxGeneratedBytes = 256 * 256 * 4;

    enum class Status : uint8_t
    { Empty, DeclarationsComplete, UnsupportedBranch, UnsupportedSource, Overflow };
    enum class Role : uint8_t
    { Face, EffectiveBaseColour, SpecularDetail, NormalMap, Mask, LayerColour1,
      LayerColour2, LayerColour3, LayerNormal1, LayerNormal2, LayerNormal3 };
    enum class SourceKind : uint8_t { PhysicalPaaPac, GeneratedColour };
    enum class Condition : uint8_t { Always, IfMaskBinds };
    struct Face
    {
        // Existing ShapeSection texture, not a new lexical _wbank->Load request.
        // sourceName must come from that texture's actual SourceName().
        bool present = false;
        bool dynamic = false;
        std::string_view logicalName;
        std::string_view sourceName;
    };
    struct Options
    {
        bool multiLayerNormals = true;
        // These resolver branches can load additional textures outside the
        // four ordinary slot resolvers. Refuse until separately represented.
        bool legacyEnhancement = false;
        bool nativePbrPreview = false;
        bool untexturedStandIn = false;
    };
    struct Row
    {
        Role role = Role::Face;
        SourceKind sourceKind = SourceKind::PhysicalPaaPac;
        Condition condition = Condition::Always;
        Asset::Material::MaterialSlot slot = Asset::Material::MaterialSlot::Count;
        int sourceStage = -1;
        std::array<char, MaxName + 1> name{}; // actual face source or translated stage declaration
        size_t nameLength = 0;
        size_t generatedBytes = 0;
        Asset::Material::RvUvTransform uvTransform;
        std::array<float, 4> tint{{1, 1, 1, 1}};
        bool tintPresent = false;
        bool uvSourceTex1 = false;
    };
    struct Scalars
    {
        std::array<float, 4> ambient{}, diffuse{}, forcedDiffuse{}, emissive{}, specular{};
        float specularPower = 1, normalPower = 1;
        std::array<float, 3> crownAoCenter{};
        float crownAoHeight = 0, crownAoWidth = 0, crownAoIntensity = 0;
        bool globalNormalUv1 = false;
    };

    static const Asset::Material::TranslatedSlot* EffectiveBaseColourStage(
        const Asset::Material::TranslatedMaterial& material, const char*& whichStage)
    {
        using Slot = Asset::Material::MaterialSlot;
        whichStage = "Stage0";
        if (const auto* base = material.Find(Slot::BaseColour)) return base;
        struct Fallback { Slot slot; const char* label; };
        constexpr Fallback fallback[] = {{Slot::LayerColour1, "Stage1"},
                                         {Slot::LayerColour2, "Stage2"},
                                         {Slot::LayerColour3, "Stage3"}};
        for (const auto& candidate : fallback)
            if (const auto* layer = material.Find(candidate.slot);
                layer && !layer->texture.isProcedural && !layer->texture.path.empty())
            { whichStage = candidate.label; return layer; }
        return nullptr;
    }

    static RetainedMaterialRoleDeclarations Build(
        const Asset::Material::TranslatedMaterial* material, Face face, Options options)
    {
        RetainedMaterialRoleDeclarations out;
        if (options.legacyEnhancement || options.nativePbrPreview || options.untexturedStandIn ||
            (material && material->shaderFamily == "CalmWater"))
            return out.RefuseBranch(Status::UnsupportedBranch);
        if (material && (material->slots.size() > MaxTranslatedSlots ||
                         material->shaderFamily.size() > MaxName || material->origin.size() > MaxName))
            return out.RefuseBranch(Status::Overflow);

        if (face.present)
        {
            // A dynamic face is supported only when its name is the same
            // generated colour declaration. Other dynamic images have no
            // immutable file or generator witness for this path.
            if (face.logicalName.empty() || face.sourceName.empty() ||
                (face.dynamic && face.logicalName != face.sourceName))
                return out.RefuseBranch(Status::UnsupportedSource);
            if (!out.Add(Role::Face, Asset::Material::MaterialSlot::Count,
                         face.sourceName, face.dynamic, Condition::Always, nullptr))
                return out;
        }
        if (!material)
        {
            out._status = Status::DeclarationsComplete;
            return out;
        }
        out._scalars = {material->ambient, material->diffuse, material->forcedDiffuse,
                        material->emissive, material->specular, material->specularPower,
                        material->normalPower,
                        {material->crownAoCenter[0], material->crownAoCenter[1], material->crownAoCenter[2]},
                        material->crownAoHeight, material->crownAoWidth, material->crownAoIntensity,
                        material->globalNormalUv1};

        using Slot = Asset::Material::MaterialSlot;
        // Runtime integration should route its existing EffectiveBaseColourStage
        // wrapper through this helper so declaration and final selection coincide.
        const char* baseStage = nullptr;
        const Asset::Material::TranslatedSlot* base = EffectiveBaseColourStage(*material, baseStage);
        if (base && !out.AddStage(Role::EffectiveBaseColour, *base, Condition::Always, true)) return out;
        if (const auto* slot = material->Find(Slot::SpecularDetail))
            if (!out.AddStage(Role::SpecularDetail, *slot, Condition::Always, true)) return out;
        if (const auto* slot = material->Find(Slot::NormalMap))
            if (!out.AddStage(Role::NormalMap, *slot, Condition::Always, true)) return out;

        // ResolveMaterialLayers binds Mask first and returns immediately if its
        // handle is zero. Procedural/empty Mask is never loaded by its bind().
        const auto* mask = material->Find(Slot::Mask);
        if (mask && !mask->texture.isProcedural && !mask->texture.path.empty())
        {
            if (!out.AddStage(Role::Mask, *mask, Condition::Always, false)) return out;
            constexpr Role colours[] = {Role::LayerColour1, Role::LayerColour2, Role::LayerColour3};
            constexpr Slot colourSlots[] = {Slot::LayerColour1, Slot::LayerColour2, Slot::LayerColour3};
            constexpr Role normals[] = {Role::LayerNormal1, Role::LayerNormal2, Role::LayerNormal3};
            constexpr Slot normalSlots[] = {Slot::LayerNormal1, Slot::LayerNormal2, Slot::LayerNormal3};
            for (size_t i = 0; i < 3; ++i)
            {
                if (const auto* colour = material->Find(colourSlots[i]))
                    if (!out.AddStage(colours[i], *colour, Condition::IfMaskBinds, false)) return out;
                if (options.multiLayerNormals)
                    if (const auto* normal = material->Find(normalSlots[i]))
                        if (!out.AddStage(normals[i], *normal, Condition::IfMaskBinds, false)) return out;
            }
        }
        out._status = Status::DeclarationsComplete;
        return out;
    }

    Status GetStatus() const { return _status; }
    bool DeclarationsComplete() const { return _status == Status::DeclarationsComplete; }
    // Always false here: only the owner, after actual Ref<Texture>, physical
    // Init binding/current mount or generated-image identity, upload outcome,
    // and final section classification checks, may complete binding admission.
    bool FinalBindingsComplete() const { return false; }
    size_t Count() const { return _count; }
    const Row& At(size_t index) const { return _rows[index]; }
    const Scalars& ScalarFacts() const { return _scalars; }

private:
    std::array<Row, MaxRows> _rows{};
    Scalars _scalars{};
    Status _status = Status::Empty;
    size_t _count = 0;
    RetainedMaterialRoleDeclarations RefuseBranch(Status why)
    { _status = why; _count = 0; return *this; }
    static bool EndsPaaPac(std::string_view name)
    {
        if (name.size() < 4 || name[name.size() - 4] != '.') return false;
        const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; };
        return lower(name[name.size() - 3]) == 'p' && lower(name[name.size() - 2]) == 'a' &&
               (lower(name[name.size() - 1]) == 'a' || lower(name[name.size() - 1]) == 'c');
    }
    bool Add(Role role, Asset::Material::MaterialSlot slot, std::string_view name,
             bool generated, Condition condition, const Asset::Material::TranslatedSlot* source)
    {
        if (_count == MaxRows || name.empty() || name.size() > MaxName)
        { _status = Status::Overflow; _count = 0; return false; }
        size_t generatedBytes = 0;
        if (generated)
        {
            if (name[0] != '#') { _status = Status::UnsupportedSource; _count = 0; return false; }
            const auto parsed = procedural::Parse(std::string(name).c_str());
            if (!parsed.generated || parsed.generator != "color" || parsed.rgba.empty() ||
                parsed.rgba.size() > MaxGeneratedBytes)
            { _status = Status::UnsupportedSource; _count = 0; return false; }
            generatedBytes = parsed.rgba.size();
        }
        else if (!EndsPaaPac(name))
        { _status = Status::UnsupportedSource; _count = 0; return false; }
        Row& row = _rows[_count++];
        row.role = role; row.slot = slot; row.condition = condition;
        row.sourceKind = generated ? SourceKind::GeneratedColour : SourceKind::PhysicalPaaPac;
        row.nameLength = name.size(); row.generatedBytes = generatedBytes;
        std::memcpy(row.name.data(), name.data(), name.size());
        row.name[name.size()] = '\0';
        if (source)
        {
            row.sourceStage = source->sourceStage;
            row.uvTransform = source->uvTransform;
            for (size_t i = 0; i < 4; ++i) row.tint[i] = source->tint[i];
            row.tintPresent = source->tintPresent;
            const auto& uv = source->uvSource;
            row.uvSourceTex1 = uv.size() == 4 && lower(uv[0]) == 't' && lower(uv[1]) == 'e' &&
                               lower(uv[2]) == 'x' && uv[3] == '1';
        }
        return true;
    }
    static char lower(char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; }
    bool AddStage(Role role, const Asset::Material::TranslatedSlot& slot,
                  Condition condition, bool acceptsGenerated)
    {
        if (slot.texture.isProcedural)
        {
            // ResolveMaterialLayers::bind deliberately returns zero for a
            // procedural stage; the other three resolvers Load generated colour.
            if (!acceptsGenerated) return true;
            return Add(role, slot.slot, slot.texture.raw, true, condition, &slot);
        }
        if (slot.texture.path.empty()) return true; // ordinary resolver returns 0
        const std::string canonical = slot.texture.path.canonical();
        return Add(role, slot.slot, canonical, false, condition, &slot);
    }
};
} // namespace Poseidon::render
