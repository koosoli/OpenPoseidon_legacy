// Shared FXY font data parser; used by the engine and the asset tools.

#include <Poseidon/Graphics/Rendering/Draw/FontData.hpp>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <ctype.h>

namespace Poseidon
{

static int getiw(QIStream& f)
{
    int low = (unsigned char)f.get();
    int high = (unsigned char)f.get();
    return low | (high << 8);
}

static int nextPow2(int v)
{
    int p = 1;
    while (p < v)
        p <<= 1;
    return p;
}

static void lowerName(const char* name, char* out, int outSize)
{
    snprintf(out, outSize, "%s", name);
    for (char* p = out; *p; ++p)
        *p = static_cast<char>(tolower(static_cast<unsigned char>(*p)));
}

static int getid(QIStream& f)
{
    const int low = getiw(f);
    const int high = getiw(f);
    return static_cast<int>(static_cast<unsigned>(low) | (static_cast<unsigned>(high) << 16));
}

// Arma 3's .fxy ("BIFo"): after the magic and a 4-byte version, one block per texture page
// <name>-NN.paa: u16 page, u16 line height, u16 0, u16 ascent, u16 0, u32 kerning pairs (8 bytes
// each, skipped), u32 glyphs; a glyph is u16 char, x, y, w, h and i32 left offset, top offset (from
// the line's top) and advance. Glyphs are kept by char - 32, as OFP's are; those past 255 are
// dropped (the engine draws Latin-1).
static bool ParseBIFo(QIStream& stream, const char* lowName, FXYData& result)
{
    if (stream.rest() < 8)
        return false;
    const int start = stream.tellg();
    char header[8];
    stream.read(header, 8);
    if (std::memcmp(header, "BIFo", 4) != 0)
    {
        stream.seekg(start, QIOS::beg);
        return false;
    }
    constexpr int kStartChar = 32;
    result.nChars = 256 - kStartChar;
    result.glyphs.resize(static_cast<size_t>(result.nChars));
    int maxW = 0;
    while (stream.rest() >= 14)
    {
        const int page = getiw(stream);
        const int lineHeight = getiw(stream);
        getiw(stream);
        getiw(stream); // ascent
        getiw(stream);
        const int kerning = getid(stream);
        for (int k = 0; k < kerning && stream.rest() >= 8; k++)
            getid(stream), getid(stream);
        if (stream.rest() < 4)
            break;
        const int glyphs = getid(stream);
        result.maxHeight = std::max(result.maxHeight, lineHeight);
        for (int g = 0; g < glyphs && stream.rest() >= 22; g++)
        {
            const int c = getiw(stream);
            const int x = getiw(stream), y = getiw(stream), w = getiw(stream), h = getiw(stream);
            const int ox = getid(stream), oy = getid(stream), advance = getid(stream);
            const int index = c - kStartChar;
            if (index < 0 || index >= result.nChars)
                continue;
            char texName[256];
            snprintf(texName, sizeof(texName), "%s-%02d.paa", lowName, page);
            FXYGlyph& glyph = result.glyphs[static_cast<size_t>(index)];
            glyph.charCode = index;
            glyph.setNum = page;
            glyph.x = x;
            glyph.y = y;
            glyph.w = w;
            glyph.h = h;
            glyph.wTex = w;
            glyph.hTex = h;
            glyph.offsetX = ox;
            glyph.offsetY = oy;
            glyph.advance = advance;
            glyph.textureName = texName;
            result.textureSetNums.insert(page);
            maxW = std::max(maxW, w);
        }
    }
    result.maxWidth = maxW;
    return result.maxHeight > 0;
}

FXYData ParseFXY(QIStream& stream, const char* name)
{
    FXYData result;

    char lowName[256];
    lowerName(name, lowName, sizeof(lowName));
    result.name = lowName;

    if (stream.rest() <= 0)
        return result;

    if (ParseBIFo(stream, lowName, result))
        return result;

    result.nChars = stream.rest() / (6 * sizeof(short));
    result.glyphs.resize(static_cast<size_t>(result.nChars));

    int maxW = 0;
    result.maxHeight = 0;

    while (stream.rest() > 0)
    {
        int c = getiw(stream);
        int setNum = getiw(stream);
        int x = getiw(stream);
        int y = getiw(stream);
        int w = getiw(stream);
        int h = getiw(stream);

        char texName[256];
        snprintf(texName, sizeof(texName), "%s-%02d.paa", lowName, setNum);

        if (c >= 0 && c < result.nChars)
        {
            FXYGlyph& glyph = result.glyphs[static_cast<size_t>(c)];
            glyph.charCode = c;
            glyph.setNum = setNum;
            glyph.x = x;
            glyph.y = y;
            glyph.w = w - 1;
            glyph.h = h - 1;
            glyph.wTex = nextPow2(w);
            glyph.hTex = nextPow2(h);
            glyph.textureName = texName;
        }

        result.textureSetNums.insert(setNum);

        if (w - 1 > maxW)
            maxW = w - 1;
        if (h - 1 > result.maxHeight)
            result.maxHeight = h - 1;
    }

    result.maxWidth = maxW;

    // Space char width = maxW * 3/4 (same as Font::Load)
    if (result.nChars > 0)
        result.glyphs[0].w = maxW * 3 / 4;

    return result;
}

std::vector<std::string> FXYData::GetTextureNames() const
{
    std::vector<std::string> names;
    for (int setNum : textureSetNums)
    {
        char texName[256];
        snprintf(texName, sizeof(texName), "%s-%02d.paa", name.c_str(), setNum);
        names.emplace_back(texName);
    }
    return names;
}

} // namespace Poseidon
