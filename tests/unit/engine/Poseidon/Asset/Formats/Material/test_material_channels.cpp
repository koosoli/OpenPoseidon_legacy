// test_material_channels.cpp - MAT-020: what each slot's channels carry.
//
// Every mapping asserted here was measured by decoding real Arma 3 textures:
// SMDI n=40 (R and A dead flat in 40/40), AS n=25 (R, B, A dead in 25/25),
// NOHQ n=40 (R dead in 40/40). The two roadmap prohibitions -- do not map SMDI to
// roughness, do not read a normal's X from R -- fall straight out of that.

#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Asset/Formats/Material/MaterialChannels.hpp>
#include <Poseidon/IO/ParamFile/ParamFile.hpp>
#include <Poseidon/IO/Streams/QStream.hpp>
#include <algorithm>
#include <string>

using namespace Poseidon::Asset::Material;

static TranslatedMaterial translate(const char* text)
{
    std::string buffer(text);
    QIStream    in(buffer.data(), static_cast<int>(buffer.size()));
    ParamFile   file;
    file.Parse(in);
    auto source = ParseRvMaterial(file, "test.rvmat");
    return TranslateSemantics(source, TranslateMaterial(source));
}

static const char* kSuper = R"CFG(
PixelShaderID="Super";
specularPower=100.0;
specular[]={0.5,0.5,0.5,1.0};
class Stage1 { texture="ca\t\x_nohq.tga"; uvSource="tex"; };
class Stage4 { texture="ca\t\x_as.tga"; uvSource="tex"; };
class Stage5 { texture="ca\t\x_smdi.tga"; uvSource="tex"; };
class Stage7 { texture="ca\data\env_land_co.tga"; uvSource="none"; };
)CFG";

static bool listed(const std::vector<TextureChannel>& v, TextureChannel c)
{
    return std::find(v.begin(), v.end(), c) != v.end();
}

TEST_CASE("SMDI: specular level in G, gloss in B, R and A dead", "[asset][material][mat-020]")
{
    auto        ir   = translate(kSuper);
    const auto* smdi = ir.Find(MaterialSlot::SpecularDetail);
    REQUIRE(smdi != nullptr);

    REQUIRE(smdi->channels.specularLevel == TextureChannel::G);
    REQUIRE(smdi->channels.gloss == TextureChannel::B);

    // Measured constant 255 across all 40 sampled SMDI textures. A consumer
    // sampling either is reading a constant, not data.
    REQUIRE(listed(smdi->channels.unused, TextureChannel::R));
    REQUIRE(listed(smdi->channels.unused, TextureChannel::A));
}

TEST_CASE("SMDI is never converted to roughness during translation", "[asset][material][mat-020]")
{
    auto        ir   = translate(kSuper);
    const auto* smdi = ir.Find(MaterialSlot::SpecularDetail);

    // The roadmap forbids mapping SMDI to roughness, and the measurement shows why:
    // B is a GLOSS term, so a direct mapping is wrong in channel and inverted in
    // polarity. There is no roughness field to populate here at all.
    REQUIRE(smdi->channels.gloss != TextureChannel::None);

    // The conversion exists only as an explicitly named, opt-in helper, and its
    // name says it has not been validated against a rendered reference.
    REQUIRE(GlossToRoughnessUnvalidated(1.0f) == 0.0f);
    REQUIRE(GlossToRoughnessUnvalidated(0.0f) == 1.0f);
}

TEST_CASE("NOHQ: X in alpha, Y in green, Z reconstructed", "[asset][material][mat-020]")
{
    auto        ir     = translate(kSuper);
    const auto* normal = ir.Find(MaterialSlot::NormalMap);
    REQUIRE(normal != nullptr);

    // DXT5nm. Reading X from R -- the intuitive choice -- yields zero everywhere:
    // R was flat 0 in all 40 sampled NOHQ textures.
    REQUIRE(normal->channels.normalX == TextureChannel::A);
    REQUIRE(normal->channels.normalY == TextureChannel::G);
    REQUIRE(normal->channels.normalZReconstructed);
    REQUIRE(listed(normal->channels.unused, TextureChannel::R));
}

TEST_CASE("AS: the shadow term is in G alone", "[asset][material][mat-020]")
{
    auto        ir = translate(kSuper);
    const auto* as = ir.Find(MaterialSlot::AmbientShadow);
    REQUIRE(as != nullptr);
    REQUIRE(as->channels.ambientShadow == TextureChannel::G);
    REQUIRE(as->channels.unused.size() == 3); // R, B and A were constant in 25/25
}

TEST_CASE("Colour slots keep RGB and alpha", "[asset][material][mat-020]")
{
    auto        ir  = translate(kSuper);
    const auto* env = ir.Find(MaterialSlot::Environment);
    REQUIRE(env != nullptr);
    REQUIRE(env->channels.colour == TextureChannel::R); // RGB triple
    REQUIRE(env->channels.alpha == TextureChannel::A);
    REQUIRE(env->channels.unused.empty());
}

TEST_CASE("Render state passes through unreinterpreted", "[asset][material][mat-020]")
{
    auto ir = translate(kSuper);
    // specularPower stays the source's exponent. Converting it into whatever the
    // current shading model prefers is a rendering decision, not a translation one.
    REQUIRE(ir.specularPower == 100.0f);
    REQUIRE(ir.specular[0] == 0.5f);
    REQUIRE(ir.shaderFamily == "Super");
}

TEST_CASE("An unknown family yields no slots and no channel claims", "[asset][material][mat-020]")
{
    static const char* kUnknown = R"CFG(
PixelShaderID="SomeFutureShader";
class Stage5 { texture="ca\t\x_smdi.tga"; uvSource="tex"; };
)CFG";
    auto ir = translate(kUnknown);
    REQUIRE_FALSE(ir.schemaKnown);
    REQUIRE(ir.slots.empty());
    // The suffix says SMDI, but with no schema there is no slot, so no channel
    // meaning is asserted. Reading semantics off a filename is the guess this
    // programme keeps refusing to make.
    REQUIRE(ir.Find(MaterialSlot::SpecularDetail) == nullptr);
}

TEST_CASE("uvSource survives into the translated slot", "[asset][material][mat-020]")
{
    auto ir = translate(kSuper);
    REQUIRE(ir.Find(MaterialSlot::NormalMap)->sourceStage == 1);
    REQUIRE(ir.Find(MaterialSlot::SpecularDetail)->sourceStage == 5);
    REQUIRE(ir.Find(MaterialSlot::NormalMap)->uvSource == "tex");
    REQUIRE(ir.Find(MaterialSlot::Environment)->uvSource == "none");
}
