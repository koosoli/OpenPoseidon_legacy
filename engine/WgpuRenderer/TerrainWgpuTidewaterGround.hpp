// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Tidewater's beach sand and bare soil on OFP's ground (owner, 2026-10-12: "exchange our OFP ground
// textures of sand and dirt with the ones from Tidewater"). Tidewater (dgreenheck/tidewater,
// matthewcosier/tidewater island-expansion; MIT, (c) DRG Software Solutions LLC; see CREDITS.md) shades
// its sand and soil procedurally in src/world/Terrain.js; its palettes and macro variation are baked here
// into OFP's own layer images, so the terrain shader, the far satellite albedo and every OFP world use
// them with no change of their own.
//
// Which layers: CfgSurfaces' Sand (files "ps??????": the pure "ps" image and every transition with a
// "ps" corner) and Field (files "pol", soundEnviron "dirt"). A transition image blends its four corner
// surfaces (OFP names it by them: "psszpsps"); each pixel's share of sand is estimated from its colour
// against the corner surfaces' pure images, so a sand-to-rock edge keeps its rock. Sand is on by
// default since 2026-10-13 (owner: "replace all sand"); WGR_TW_GROUND=1 adds the field soil, 0 = off.
#include <Poseidon/Graphics/Textures/PAADecoder.hpp>
#include <Poseidon/Graphics/Textures/TextureBank.hpp>
#include <array>
#include <map>
#include <string>

namespace Poseidon
{
class Landscape;

class TidewaterGround
{
  public:
    static bool Enabled();
    // The lower-case base name of a texture path ("eden\\psszpsps.paa" -> "psszpsps").
    static std::string BaseName(const char* path);
    // Whether a layer of this base name is recoloured (sand or field).
    static bool Applies(const std::string& base);
    // (2026-10-13) The surface kind the terrain shader draws for an OFP surface code (terrain.wgsl
    // TWK_*): 1 sand, 2 rock, 3 grass, 4 forest floor, 5 gravel, 6 dirt, 7 field; 0 = OFP's own image
    // (roads, runways, concrete, village ground). Matches the retail CfgSurfaces classes.
    static int KindOf(const std::string& code);
    // A layer's two kinds: A takes the per-pixel share in the layer's alpha, B the rest. A is sand
    // when a corner is sand, else the commonest procedural kind among the corners.
    struct Plan
    {
        int kindA = 0, kindB = 0;
        std::string codeA, codeB;
        bool pure = false;
    };
    static Plan PlanFor(const std::string& base);
    // A surface code's pure image mean colour, linear (the shader's per-kind colour anchor); zero
    // when unknown (the shader then keeps Tidewater's own palette).
    std::array<float, 3> AnchorLinear(const std::string& code) const;
    // Learn the pure surfaces' mean colours from the world's layers (decodes the 2-letter ones), once
    // per world.
    void Prepare(const Landscape& land);
    // Recolour a decoded layer image in place; false when the layer is not one this touches.
    bool Recolour(const std::string& base, DecodedImage& image) const;
    // The recoloured layer as a new mipmapped texture, or null (not sand / field, or no source).
    Ref<Texture> Bake(Texture* layer, AbstractTextBank* bank) const;

  private:
    std::map<std::string, std::array<float, 3>> _means; // pure surface code -> mean sRGB colour 0..1
    std::string _world;
};
} // namespace Poseidon
