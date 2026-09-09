// test_procedural_texture.cpp - generating the textures Real Virtuality writes as names.
//
// Three of the seven stages of a typical Arma 3 Super material are procedural
// stand-ins, and some surfaces are nothing but one: Arma 3's landing and warning
// lights carry `#(argb,8,8,3)color(...)` as their FACE texture.

#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Graphics/Textures/ProceduralTexture.hpp>

using namespace Poseidon::render::procedural;

TEST_CASE("A constant-colour procedural texture is generated at its declared size",
          "[graphics][texture][procedural]")
{
    const auto tex = Parse("#(argb,8,8,3)color(1.0,0.95,0.85,1.0,co)");
    REQUIRE(tex.recognised);
    REQUIRE(tex.generated);
    REQUIRE(tex.generator == "color");
    REQUIRE(tex.width == 8);
    REQUIRE(tex.height == 8);
    REQUIRE(tex.rgba.size() == 8u * 8u * 4u);

    // Every texel is the same colour, and the last argument is the role tag rather
    // than a component.
    for (size_t at = 0; at < tex.rgba.size(); at += 4)
    {
        REQUIRE(tex.rgba[at + 0] == 255);
        REQUIRE(tex.rgba[at + 1] == 242);
        REQUIRE(tex.rgba[at + 2] == 217);
        REQUIRE(tex.rgba[at + 3] == 255);
    }
}

TEST_CASE("The arguments are RGBA even though the type token says argb", "[graphics][texture][procedural]")
{
    // The corpus decides this. A NOHQ stand-in read as RGBA is a flat tangent-space
    // normal (0.5, 0.5, 1) at full alpha; read as ARGB it would be a
    // half-transparent cyan, which is not a normal map by any reading.
    const auto normal = Parse("#(argb,8,8,3)color(0.5,0.5,1,1,NOHQ)");
    REQUIRE(normal.generated);
    REQUIRE(normal.rgba[0] == 128);
    REQUIRE(normal.rgba[1] == 128);
    REQUIRE(normal.rgba[2] == 255);
    REQUIRE(normal.rgba[3] == 255);

    // And an ambient-shadow stand-in is opaque grey, not a nearly invisible one.
    const auto ambient = Parse("#(argb,8,8,3)color(0.2,0.2,0.2,1.0,AS)");
    REQUIRE(ambient.generated);
    REQUIRE(ambient.rgba[3] == 255);
}

TEST_CASE("Procedural normals upload in NOHQ channels without parallax height", "[graphics][texture][procedural]")
{
    auto normal = Parse("#(argb,8,8,3)color(0.5,0.5,1,1,NOHQ)");
    EncodeNormalUpload(normal);
    REQUIRE(normal.rgba[0] == 0);
    REQUIRE(normal.rgba[1] == 128);
    REQUIRE(normal.rgba[2] == 255);
    REQUIRE(normal.rgba[3] == 128);
    auto colour = Parse("#(argb,8,8,3)color(0.2,0.3,0.4,0.7,co)");
    const auto original = colour.rgba;
    EncodeNormalUpload(colour);
    REQUIRE(colour.rgba == original);
}

TEST_CASE("A lookup-table generator is recognised but not invented", "[graphics][texture][procedural]")
{
    // `fresnel` builds a table from its parameters. There is no implementation of
    // that here, and producing a plausible-looking one would be a guess -- so it
    // reports unsupported and a caller can say which generator it was.
    const auto fresnel = Parse("#(ai,64,64,1)fresnel(2.0,0.1)");
    REQUIRE(fresnel.recognised);
    REQUIRE_FALSE(fresnel.generated);
    REQUIRE(fresnel.generator == "fresnel");
    REQUIRE(fresnel.rgba.empty());

    const auto irradiance = Parse("#(ai,32,128,1)irradiance(8)");
    REQUIRE(irradiance.recognised);
    REQUIRE_FALSE(irradiance.generated);
}

TEST_CASE("An ordinary texture path is not procedural", "[graphics][texture][procedural]")
{
    REQUIRE_FALSE(Parse("a3\\structures_f\\data\\wall_04_co.paa").recognised);
    REQUIRE_FALSE(Parse("").recognised);
    REQUIRE_FALSE(Parse(nullptr).recognised);
}

TEST_CASE("A malformed procedural name fails instead of allocating", "[graphics][texture][procedural]")
{
    REQUIRE_FALSE(Parse("#(argb,8,8,3").recognised);   // no closing header
    REQUIRE_FALSE(Parse("#(argb,8)color(1,1,1)").recognised); // header too short

    // A size the header cannot mean is clamped rather than believed: this must not
    // become a multi-gigabyte allocation on the strength of a typo.
    const auto huge = Parse("#(argb,999999,999999,1)color(1,0,0,1)");
    REQUIRE(huge.generated);
    REQUIRE(huge.width == 256);
    REQUIRE(huge.height == 256);
}

TEST_CASE("Colour components outside 0..1 saturate", "[graphics][texture][procedural]")
{
    // A `specular` of 1.4 appears in the corpus; an 8-bit texture cannot carry it,
    // and wrapping would turn an over-bright value into a dark one.
    const auto tex = Parse("#(argb,4,4,1)color(1.4,-0.5,0.5,1)");
    REQUIRE(tex.generated);
    REQUIRE(tex.rgba[0] == 255);
    REQUIRE(tex.rgba[1] == 0);
    REQUIRE(tex.rgba[2] == 128);
}
