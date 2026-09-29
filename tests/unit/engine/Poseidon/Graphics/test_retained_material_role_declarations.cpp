#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Graphics/Rendering/Shape/RetainedMaterialRoleDeclarations.hpp>
#include <string_view>

using Poseidon::render::RetainedMaterialRoleDeclarations;
namespace {
namespace Material = Poseidon::Asset::Material;
using Plan = RetainedMaterialRoleDeclarations;

Material::TranslatedSlot Stage(Material::MaterialSlot slot, const char* name)
{
    Material::TranslatedSlot out;
    out.slot = slot;
    out.present = true;
    out.texture = Material::RvTextureRef::Parse(name);
    return out;
}
size_t Find(const Plan& plan, Plan::Role role)
{
    for (size_t i = 0; i < plan.Count(); ++i)
        if (plan.At(i).role == role) return i;
    return plan.Count();
}
}

TEST_CASE("Retained Multi declarations enumerate physical and generated roles with mask dependencies",
          "[retained-role-declarations]")
{
    Material::TranslatedMaterial multi;
    multi.shaderFamily = "Multi";
    multi.normalPower = 0.7f;
    multi.slots.push_back(Stage(Material::MaterialSlot::BaseColour, R"(dz\building\wall_co.paa)"));
    multi.slots.push_back(Stage(Material::MaterialSlot::SpecularDetail,
                                "#(argb,8,8,3)color(1,1,1,1,SMDI)"));
    multi.slots.push_back(Stage(Material::MaterialSlot::NormalMap,
                                "#(argb,8,8,3)color(0.5,0.5,1,1,NOHQ)"));
    multi.slots.push_back(Stage(Material::MaterialSlot::Mask, R"(dz\building\wall_mask.paa)"));
    auto colour = Stage(Material::MaterialSlot::LayerColour1, R"(dz\building\brick_co.paa)");
    colour.uvSource = "tex1";
    colour.tint[0] = 0.4f;
    colour.tintPresent = true;
    multi.slots.push_back(colour);
    multi.slots.push_back(Stage(Material::MaterialSlot::LayerNormal1, R"(dz\building\brick_nohq.paa)"));
    multi.slots.push_back(Stage(Material::MaterialSlot::LayerColour2,
                                "#(argb,8,8,3)color(0.2,0.2,0.2,1,DT)")); // layer bind skips procedural

    const auto plan = Plan::Build(&multi,
        {true, false, R"(dz\building\wall_face.paa)", R"(dz\building\wall_face.paa)"}, {});
    REQUIRE(plan.DeclarationsComplete());
    REQUIRE_FALSE(plan.FinalBindingsComplete());
    REQUIRE(plan.Count() == 7);
    REQUIRE(plan.ScalarFacts().normalPower == 0.7f);
    REQUIRE(plan.At(Find(plan, Plan::Role::Face)).sourceKind == Plan::SourceKind::PhysicalPaaPac);
    REQUIRE(plan.At(Find(plan, Plan::Role::SpecularDetail)).sourceKind == Plan::SourceKind::GeneratedColour);
    REQUIRE(plan.At(Find(plan, Plan::Role::NormalMap)).generatedBytes == 8 * 8 * 4);
    REQUIRE(plan.At(Find(plan, Plan::Role::Mask)).condition == Plan::Condition::Always);
    const auto& brick = plan.At(Find(plan, Plan::Role::LayerColour1));
    REQUIRE(brick.condition == Plan::Condition::IfMaskBinds);
    REQUIRE(brick.uvSourceTex1);
    REQUIRE(brick.tintPresent);
    REQUIRE(brick.tint[0] == 0.4f);
    REQUIRE(Find(plan, Plan::Role::LayerColour2) == plan.Count());
}

TEST_CASE("Retained base declaration uses first physical layer only when Stage0 is absent",
          "[retained-role-declarations]")
{
    Material::TranslatedMaterial multi;
    multi.shaderFamily = "Multi";
    multi.slots.push_back(Stage(Material::MaterialSlot::LayerColour1,
                                "#(argb,8,8,3)color(1,0,0,1,CO)"));
    multi.slots.push_back(Stage(Material::MaterialSlot::LayerColour2, R"(dz\building\plaster_co.pac)"));
    const char* selectedStage = nullptr;
    const auto* selected = Plan::EffectiveBaseColourStage(multi, selectedStage);
    REQUIRE(selected == &multi.slots[1]);
    REQUIRE(std::string_view(selectedStage) == "Stage2");
    const auto plan = Plan::Build(&multi, {}, {});
    REQUIRE(plan.DeclarationsComplete());
    REQUIRE(plan.Count() == 1);
    REQUIRE(plan.At(0).role == Plan::Role::EffectiveBaseColour);
    REQUIRE(plan.At(0).slot == Material::MaterialSlot::LayerColour2);
    REQUIRE(plan.At(0).sourceKind == Plan::SourceKind::PhysicalPaaPac);
}

TEST_CASE("Retained Super declarations refuse unsupported generators and hidden resolver branches",
          "[retained-role-declarations]")
{
    Material::TranslatedMaterial super;
    super.shaderFamily = "Super";
    super.slots.push_back(Stage(Material::MaterialSlot::NormalMap,
                                "#(ai,64,64,1)fresnel(2.0,0.1)"));
    auto plan = Plan::Build(&super, {}, {});
    REQUIRE(plan.GetStatus() == Plan::Status::UnsupportedSource);
    REQUIRE_FALSE(plan.DeclarationsComplete());
    super.slots.clear();
    plan = Plan::Build(&super, {}, {.nativePbrPreview = true});
    REQUIRE(plan.GetStatus() == Plan::Status::UnsupportedBranch);
    super.shaderFamily = "CalmWater";
    plan = Plan::Build(&super, {}, {});
    REQUIRE(plan.GetStatus() == Plan::Status::UnsupportedBranch);
}

TEST_CASE("Retained declarations refuse unproved dynamic faces and bounded slot overflow",
          "[retained-role-declarations]")
{
    const auto dynamic = Plan::Build(nullptr, {true, true, "runtime-atlas", "runtime-atlas"}, {});
    REQUIRE(dynamic.GetStatus() == Plan::Status::UnsupportedSource);
    const auto generated = Plan::Build(nullptr,
        {true, true, "#(argb,8,8,3)color(0,0,0,1,CO)", "#(argb,8,8,3)color(0,0,0,1,CO)"}, {});
    REQUIRE(generated.DeclarationsComplete());
    REQUIRE(generated.Count() == 1);
    REQUIRE(generated.At(0).sourceKind == Plan::SourceKind::GeneratedColour);
    REQUIRE_FALSE(generated.FinalBindingsComplete());

    Material::TranslatedMaterial crowded;
    crowded.slots.resize(Plan::MaxTranslatedSlots + 1);
    const auto overflow = Plan::Build(&crowded, {}, {});
    REQUIRE(overflow.GetStatus() == Plan::Status::Overflow);
}
