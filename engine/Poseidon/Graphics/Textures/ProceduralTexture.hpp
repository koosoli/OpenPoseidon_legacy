#pragma once

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace Poseidon::render::procedural
{

// A Real Virtuality "procedural texture": a texture the engine GENERATES from a
// name instead of loading from disk.
//
//   #(argb,8,8,3)color(0.5,0.5,0.5,1,DT)
//   #(ai,64,64,1)fresnel(2.0,0.1)
//    ^type ^w ^h ^mips ^generator ^arguments
//
// They are not rare and not decorative. Three of the seven stages of a typical
// Arma 3 Super material are procedural stand-ins, and a surface whose whole
// appearance is a flat authored colour -- Arma 3's landing lights and warning
// lights are exactly this -- has nothing else to draw.
//
// Only `color` is generated here. `fresnel`, `irradiance` and the rest are LOOKUP
// TABLES built from their parameters, and inventing one would be a guess with its
// own evidence requirement; they report unsupported so a caller can say so rather
// than silently drawing a wrong table.

struct ProceduralTexture
{
    bool        recognised = false; // the name parses as `#(...)generator(...)`
    bool        generated  = false; // ...and this generator is one we can build
    std::string generator;          // "color", "fresnel", ...
    std::string encodingTag;
    int         width  = 0;
    int         height = 0;
    // RGBA8, row-major, width*height*4 bytes. Empty unless `generated`.
    std::vector<uint8_t> rgba;
};

namespace detail
{
inline bool LooksProcedural(const char* name)
{
    return name && name[0] == '#';
}

// The tag a `color` stage carries as its last argument -- `DT`, `NOHQ`, `SMDI`.
// Does not select a material slot. NOHQ also specifies the normal upload encoding.
inline bool IsTag(const std::string& token)
{
    if (token.empty())
        return false;
    for (const char ch : token)
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')))
            return false;
    return true;
}

inline std::vector<std::string> Split(const std::string& text, char sep)
{
    std::vector<std::string> parts;
    std::string              current;
    for (const char ch : text)
    {
        if (ch == sep)
        {
            parts.push_back(current);
            current.clear();
        }
        else if (ch != ' ' && ch != '\t')
        {
            current += ch;
        }
    }
    parts.push_back(current);
    return parts;
}
} // namespace detail

// Parse a procedural texture name and build it where we can.
//
// The declared size is honoured rather than collapsed to 1x1. A constant colour
// needs one texel, but the name is the asset author's statement about the texture
// and a caller that mipmaps or tiles it should get what the material asked for.
// Sizes are clamped: a malformed header must not turn into an allocation.
inline ProceduralTexture Parse(const char* name)
{
    ProceduralTexture out;
    if (!detail::LooksProcedural(name))
        return out;

    const std::string text(name);
    const size_t      headerBegin = text.find('(');
    const size_t      headerEnd   = text.find(')', headerBegin == std::string::npos ? 0 : headerBegin);
    if (headerBegin == std::string::npos || headerEnd == std::string::npos)
        return out;

    const size_t argsBegin = text.find('(', headerEnd);
    const size_t argsEnd   = text.rfind(')');
    if (argsBegin == std::string::npos || argsEnd == std::string::npos || argsEnd <= argsBegin)
        return out;

    const auto header = detail::Split(text.substr(headerBegin + 1, headerEnd - headerBegin - 1), ',');
    if (header.size() < 3)
        return out;

    out.recognised = true;
    out.generator  = text.substr(headerEnd + 1, argsBegin - headerEnd - 1);
    // `argb,W,H,mips` -- the first field is the pixel type, which does not change
    // what is generated here: everything is produced as RGBA8 for upload.
    out.width  = std::atoi(header[1].c_str());
    out.height = std::atoi(header[2].c_str());
    if (out.width < 1 || out.height < 1 || out.width > 256 || out.height > 256)
    {
        out.width  = out.width < 1 ? 1 : (out.width > 256 ? 256 : out.width);
        out.height = out.height < 1 ? 1 : (out.height > 256 ? 256 : out.height);
    }

    if (out.generator != "color")
        return out; // recognised, not generated

    const auto args = detail::Split(text.substr(argsBegin + 1, argsEnd - argsBegin - 1), ',');
    if (args.size() < 3)
        return out;
    if (detail::IsTag(args.back()))
        out.encodingTag = args.back();

    // The arguments are R,G,B,A even though the type token says `argb`, and the
    // corpus is unambiguous about it. `color(0.5,0.5,1,1,NOHQ)` read as RGBA is a
    // flat tangent-space normal, exactly what a NOHQ stand-in should be; read as
    // ARGB it is a half-transparent cyan. `color(0.2,0.2,0.2,1.0,AS)` is grey and
    // opaque one way, and almost fully transparent the other.
    float rgba[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    for (size_t i = 0; i < args.size() && i < 4; ++i)
    {
        if (detail::IsTag(args[i]))
            break; // the trailing role tag, not a component
        rgba[i] = static_cast<float>(std::atof(args[i].c_str()));
    }

    out.generated = true;
    out.rgba.resize(static_cast<size_t>(out.width) * static_cast<size_t>(out.height) * 4);
    uint8_t texel[4];
    for (int i = 0; i < 4; ++i)
    {
        // Values above 1 do occur (a `specular` of 1.4 is in the corpus); an 8-bit
        // texture cannot carry them, so they saturate rather than wrap.
        const float v = rgba[i] < 0.0f ? 0.0f : (rgba[i] > 1.0f ? 1.0f : rgba[i]);
        texel[i]      = static_cast<uint8_t>(v * 255.0f + 0.5f);
    }
    for (size_t at = 0; at < out.rgba.size(); at += 4)
    {
        out.rgba[at + 0] = texel[0];
        out.rgba[at + 1] = texel[1];
        out.rgba[at + 2] = texel[2];
        out.rgba[at + 3] = texel[3];
    }
    return out;
}

// color() arguments remain RGB normals; the WGPU NOHQ sampler expects X in A,
// Y in G and reconstructs Z. R must be zero, not an accidental parallax height.
inline void EncodeNormalUpload(ProceduralTexture& texture)
{
    if (texture.encodingTag != "NOHQ" && texture.encodingTag != "nohq") return;
    for (size_t at = 0; at + 3 < texture.rgba.size(); at += 4)
    {
        texture.rgba[at + 3] = texture.rgba[at];
        texture.rgba[at] = 0;
        texture.rgba[at + 2] = 255;
    }
}

} // namespace Poseidon::render::procedural
