// Shared FXY font data parser; used by the engine (Font::Load) and the tools.

#pragma once

#include <Poseidon/IO/Streams/QStream.hpp>
#include <cstdint>


namespace Poseidon
{
using Poseidon::QIStream;
} // namespace Poseidon
#include <set>
#include <string>
#include <vector>
namespace Poseidon
{

// Parsed glyph entry from FXY file
struct FXYGlyph
{
    int charCode = 0;
    int setNum = 0;
    int x = 0;
    int y = 0;
    int w = 0; // original width (w-1 from file)
    int h = 0; // original height (h-1 from file)
    int wTex = 0; // nearest power of 2
    int hTex = 0;
    // Arma 3's fonts (the "BIFo" .fxy) pack glyphs tightly: each is drawn at an offset in its line
    // and advances the pen by its own amount. OFP's fonts are whole cells: offsets 0, advance -1
    // (the cell's width).
    int offsetX = 0;
    int offsetY = 0;
    int advance = -1;
    std::string textureName;
};

// Result of parsing an FXY file
struct FXYData
{
    std::string name;
    std::vector<FXYGlyph> glyphs; // indexed by char code
    int nChars = 0;
    int maxHeight = 0;
    int maxWidth = 0;
    std::set<int> textureSetNums;

    // Get texture names for all sets
    std::vector<std::string> GetTextureNames() const;

    bool valid() const { return nChars > 0; }
};

FXYData ParseFXY(QIStream& stream, const char* name);

// Round idealPx to the nearest multiple of 4, clamped to [8, 160]. Used by the
// FreeType atlas to bound the per-size glyph cache.
int BucketFTPixelSize(float idealPx);

} // namespace Poseidon
