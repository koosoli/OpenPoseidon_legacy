#pragma once

#include <Poseidon/Asset/Addon/VirtualPath.hpp>

#include <cctype>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace Poseidon::Asset::Material
{

// DZ-002 -- Enfusion's .emat material, the file DayZ actually authors water in.
//
// A DayZ water surface carries TWO material files side by side, and the engine was
// reading the wrong one. The .rvmat is a 79-byte stub holding nothing but
// `PixelShaderID="CalmWater"; VertexShaderID="CalmWater"` -- no stages, no colours --
// so the shading path found nothing to work with and fell back, which is where the
// flat green came from. The .emat sibling is the real material: plain text, and every
// parameter the surface needs is in it.
//
// Like RvMaterialSource, this layer INTERPRETS NOTHING. It records the class the
// material names, every key it declares and the tokens each key carries, and stops.
// Which key means what is a question for whatever binds these to a shader, and it
// differs by class -- `MatWaterPool` (19 files) and `MatOceanOld` (7) do not agree on
// what a normal map is for. Everything unrecognised is preserved rather than dropped.
//
// The grammar, censused over all 207 .emat files in a retail DayZ install:
//
//   header    `ClassName {`                                 164 files
//             `material "some/path": ClassName` then `{`     43 files (postprocess)
//   body      `Key value...`, one per line, 1 to 4 values -- never more, in any file
//   key       a bare word, or a quoted string when it contains spaces
//             (5 such keys exist: "Enable effect", "Pixel stride_1", ...)
//   value     a number, a bare word (`translucent`, `alpha`), or a quoted texture
//             reference `"{GUID}path.edds"` -- all 291 references carry the GUID
//   close     `}`
//
// The GUID is Enfusion's asset id and this engine has no registry to resolve it
// against, so TextureOf() strips it and keeps the path, which is resolvable.
struct EmatValue
{
    std::string text; //!< the token with quotes stripped; for a texture, the path without its GUID
    std::string guid; //!< the brace-delimited asset id, empty when the token carried none
    double number = 0.0;
    bool isNumber = false;

    bool isTexture() const { return !guid.empty(); }
};

struct EmatProperty
{
    std::string name;
    std::vector<EmatValue> values;
};

struct EmatMaterial
{
    std::string className;    //!< e.g. "MatWaterPool"
    std::string declaredPath; //!< only the `material "path": Class` header form sets this
    //! The `.emat` this one derives from, path only, GUID stripped -- set by the
    //! `ClassName : "{GUID}parent.emat" {` header form and empty otherwise.
    //! Reforger uses it for 1,891 of 10,311 materials (18.3%), across 547 distinct
    //! parents; DayZ's corpus has none. Resolve with ResolveEmatInheritance.
    std::string parentPath;
    std::string parentGuid;
    std::vector<EmatProperty> properties;
    std::string error; //!< empty on success; names the constraint that failed

    bool valid() const { return !className.empty() && error.empty(); }

    static bool NameMatches(std::string_view a, std::string_view b)
    {
        if (a.size() != b.size())
            return false;
        for (size_t i = 0; i < a.size(); ++i)
        {
            const auto x = static_cast<unsigned char>(a[i]);
            const auto y = static_cast<unsigned char>(b[i]);
            if (std::tolower(x) != std::tolower(y))
                return false;
        }
        return true;
    }

    const EmatProperty* Find(std::string_view name) const
    {
        for (const EmatProperty& property : properties)
            if (NameMatches(property.name, name))
                return &property;
        return nullptr;
    }

    bool Has(std::string_view name) const { return Find(name) != nullptr; }

    //! Single scalar. False (and `out` untouched) when the key is absent or not numeric.
    bool FloatOf(std::string_view name, float& out) const
    {
        const EmatProperty* property = Find(name);
        if (!property || property->values.empty() || !property->values[0].isNumber)
            return false;
        out = static_cast<float>(property->values[0].number);
        return true;
    }

    float FloatOr(std::string_view name, float fallback) const
    {
        float value = fallback;
        FloatOf(name, value);
        return value;
    }

    //! Four scalars, as `Color`, `Emissive` and `ReflectionColor` are always written.
    bool Vec4Of(std::string_view name, float out[4]) const
    {
        const EmatProperty* property = Find(name);
        if (!property || property->values.size() != 4)
            return false;
        for (size_t i = 0; i < 4; ++i)
        {
            if (!property->values[i].isNumber)
                return false;
            out[i] = static_cast<float>(property->values[i].number);
        }
        return true;
    }

    //! Three scalars, as Enfusion writes `GeometryAOCenter` and friends.
    bool Vec3Of(std::string_view name, float out[3]) const
    {
        const EmatProperty* property = Find(name);
        if (!property || property->values.size() != 3)
            return false;
        for (size_t i = 0; i < 3; ++i)
        {
            if (!property->values[i].isNumber)
                return false;
            out[i] = static_cast<float>(property->values[i].number);
        }
        return true;
    }

    //! Texture path with the GUID stripped. Empty when the key is absent or carries no
    //! texture -- callers must not treat "" as a resolvable path.
    std::string TextureOf(std::string_view name) const
    {
        const EmatProperty* property = Find(name);
        if (!property)
            return {};
        for (const EmatValue& value : property->values)
            if (value.isTexture())
                return value.text;
        return {};
    }

    //! Bare trailing word, e.g. the `alpha` after an AlbedoMap or `Sort translucent`.
    std::string WordOf(std::string_view name) const
    {
        const EmatProperty* property = Find(name);
        if (!property)
            return {};
        for (const EmatValue& value : property->values)
            if (!value.isNumber && !value.isTexture())
                return value.text;
        return {};
    }
};

namespace Detail
{

inline bool IsSpace(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

inline std::string_view Trim(std::string_view s)
{
    size_t begin = 0;
    size_t end = s.size();
    while (begin < end && IsSpace(s[begin]))
        ++begin;
    while (end > begin && IsSpace(s[end - 1]))
        --end;
    return s.substr(begin, end - begin);
}

//! Splits `{GUID}path` into its two halves. A token with no leading brace is all path.
inline void SplitGuid(const std::string& token, std::string& guid, std::string& path)
{
    if (!token.empty() && token[0] == '{')
    {
        const size_t close = token.find('}');
        if (close != std::string::npos)
        {
            guid = token.substr(1, close - 1);
            path = token.substr(close + 1);
            return;
        }
    }
    guid.clear();
    path = token;
}

inline EmatValue MakeValue(const std::string& token, bool wasQuoted)
{
    EmatValue value;
    SplitGuid(token, value.guid, value.text);
    if (!value.guid.empty())
        return value; // a GUID reference is never a number

    if (!wasQuoted && !value.text.empty())
    {
        char* end = nullptr;
        const double parsed = std::strtod(value.text.c_str(), &end);
        if (end && *end == '\0')
        {
            value.number = parsed;
            value.isNumber = true;
        }
    }
    return value;
}

//! Pulls the next token, honouring quotes. Returns false at end of line.
inline bool NextToken(std::string_view line, size_t& pos, std::string& token, bool& wasQuoted)
{
    while (pos < line.size() && IsSpace(line[pos]))
        ++pos;
    if (pos >= line.size())
        return false;

    wasQuoted = line[pos] == '"';
    if (wasQuoted)
    {
        const size_t close = line.find('"', pos + 1);
        if (close == std::string_view::npos)
        {
            // An unterminated quote takes the rest of the line rather than dropping
            // it, so a malformed file still reports what it was trying to say.
            token.assign(line.substr(pos + 1));
            pos = line.size();
            return true;
        }
        token.assign(line.substr(pos + 1, close - pos - 1));
        pos = close + 1;
        return true;
    }

    const size_t begin = pos;
    while (pos < line.size() && !IsSpace(line[pos]))
        ++pos;
    token.assign(line.substr(begin, pos - begin));
    return true;
}

} // namespace Detail

//! Parses one .emat. Never throws; on failure the result is invalid and `error` says why.
inline EmatMaterial ParseEmat(std::string_view text)
{
    EmatMaterial material;

    std::vector<std::string_view> lines;
    size_t start = 0;
    while (start <= text.size())
    {
        const size_t nl = text.find('\n', start);
        const size_t end = nl == std::string_view::npos ? text.size() : nl;
        const std::string_view line = Detail::Trim(text.substr(start, end - start));
        if (!line.empty())
            lines.push_back(line);
        if (nl == std::string_view::npos)
            break;
        start = nl + 1;
    }

    if (lines.empty())
    {
        material.error = "file is empty";
        return material;
    }

    size_t cursor = 0;
    std::string_view header = lines[0];

    // Form two: `material "some/path": ClassName`, used by the 43 postprocess
    // materials. The brace is then on its own line.
    if (header.rfind("material", 0) == 0 && header.find(':') != std::string_view::npos)
    {
        const size_t colon = header.rfind(':');
        material.className = std::string(Detail::Trim(header.substr(colon + 1)));
        const size_t q1 = header.find('"');
        const size_t q2 = q1 == std::string_view::npos ? q1 : header.find('"', q1 + 1);
        if (q1 != std::string_view::npos && q2 != std::string_view::npos)
            material.declaredPath = std::string(header.substr(q1 + 1, q2 - q1 - 1));
        cursor = 1;
        if (cursor < lines.size() && Detail::Trim(lines[cursor]) == "{")
            ++cursor;
    }
    else
    {
        // The general header, of which `ClassName {` is only the simplest case:
        //
        //     ClassName [ "{GUID}ownPath" ] [ : "{GUID}parentPath" ] {
        //
        // Both optional parts occur in Reforger and neither occurs in DayZ, which
        // is why the original reader searched for the first '{' and took whatever
        // preceded it as the class name. In these headers the first brace opens a
        // GUID, not the body -- so that yields class names like `MatPBRBasic : "`
        // and drops the parent, and 18.3% of Reforger's materials silently lose
        // everything their parent defines. Counts over the 10,312-file corpus:
        // 1,891 inherit, and 44 declare their own path this way.
        //
        // Scanning for the first structural character rather than special-casing
        // each combination is what keeps a fifth spelling from being another bug.
        const size_t nameEnd = header.find_first_of("\":{");
        if (nameEnd == std::string_view::npos)
        {
            material.error = "first line is neither 'Class {' nor 'material \"path\": Class'";
            return material;
        }
        material.className = std::string(Detail::Trim(header.substr(0, nameEnd)));

        size_t at = nameEnd;
        // An own-path reference, before any colon.
        if (header[at] == '"')
        {
            const size_t close = header.find('"', at + 1);
            if (close == std::string_view::npos)
            {
                material.error = "header has an unterminated path reference";
                return material;
            }
            std::string guid;
            Detail::SplitGuid(std::string(header.substr(at + 1, close - at - 1)), guid, material.declaredPath);
            at = header.find_first_of(":{", close + 1);
            if (at == std::string_view::npos)
            {
                material.error = "header does not open a body";
                return material;
            }
        }
        if (header[at] == ':')
        {
            const size_t q1 = header.find('"', at + 1);
            const size_t q2 = q1 == std::string_view::npos ? q1 : header.find('"', q1 + 1);
            if (q2 == std::string_view::npos)
            {
                material.error = "inheriting header has an unterminated parent reference";
                return material;
            }
            Detail::SplitGuid(std::string(header.substr(q1 + 1, q2 - q1 - 1)), material.parentGuid,
                              material.parentPath);
            at = header.find('{', q2 + 1);
            if (at == std::string_view::npos)
            {
                material.error = "inheriting header does not open a body";
                return material;
            }
        }
        if (header[at] != '{')
        {
            material.error = "header does not open a body";
            return material;
        }
        cursor = 1;
    }

    if (material.className.empty())
    {
        material.error = "header names no class";
        return material;
    }

    // A property may open a nested block:
    //
    //     AlbedoMapTexMat {
    //      1 1 0 0
    //     }
    //
    // Two of the 207 files do this, both of them Sakhal's ice-lake water. Counting
    // braces is not pedantry here: read flat, that inner `}` closes the material and
    // everything after it is silently dropped -- which cost those two files half
    // their texture references before this was measured. The nested tokens become
    // further values of the property that opened the block, which is the natural
    // reading of a texture matrix and keeps the "preserve everything" rule.
    bool closed = false;
    int depth = 0;
    for (; cursor < lines.size(); ++cursor)
    {
        std::string_view line = lines[cursor];
        if (line == "}")
        {
            if (depth > 0)
            {
                --depth;
                continue;
            }
            closed = true;
            break;
        }
        if (line == "{")
            continue;

        // `Key {` opens a block; the key still becomes a property.
        bool opensBlock = false;
        if (!line.empty() && line.back() == '{')
        {
            opensBlock = true;
            line = Detail::Trim(line.substr(0, line.size() - 1));
            if (line.empty())
                continue;
        }

        size_t pos = 0;
        std::string token;
        bool wasQuoted = false;
        if (!Detail::NextToken(line, pos, token, wasQuoted))
            continue;

        if (depth > 0 && !material.properties.empty())
        {
            // Inside a nested block: everything on the line is a value of the
            // property that opened it, including this first token.
            EmatProperty& owner = material.properties.back();
            owner.values.push_back(Detail::MakeValue(token, wasQuoted));
            while (Detail::NextToken(line, pos, token, wasQuoted))
                owner.values.push_back(Detail::MakeValue(token, wasQuoted));
            if (opensBlock)
                ++depth;
            continue;
        }

        EmatProperty property;
        property.name = token;
        while (Detail::NextToken(line, pos, token, wasQuoted))
            property.values.push_back(Detail::MakeValue(token, wasQuoted));
        material.properties.push_back(std::move(property));
        if (opensBlock)
            ++depth;
    }

    if (!closed)
    {
        material.error = "no closing brace";
        material.className.clear();
    }
    return material;
}

//! The text of an `.emat`, in the simplest header form the parser above accepts:
//!
//!     ClassName {
//!      Key value value
//!     }
//!
//! This is the inverse of ParseEmat and lives beside it for the reason
//! MakeSuperRvmatText lives beside the RVMAT reader: a format written in one place and
//! read in another drifts silently. The `xob` converter uses it to deploy a material it
//! has already resolved -- parent chain folded in, texture keys pointing at the `.paa`
//! files it wrote -- as a file the engine's own `.emat` reader opens. What it writes:
//!
//!   numbers    the ORIGINAL token text (`0.433`, not a re-formatted double), so a
//!              value survives a round trip byte for byte
//!   textures   `"{GUID}path"` -- the GUID is what makes TextureOf() see a texture, so
//!              a value that carries none is written as a plain quoted word instead
//!   words      bare, unless the token is empty, holds a space, or would parse as a
//!              number when read back bare -- those are quoted to keep their kind
//!   keys       bare, or quoted when they hold a space (5 such keys exist in DayZ)
//!
//! Nested blocks are not reproduced: ParseEmat flattens a `Key {...}` block into further
//! values of `Key`, and this writes those values on one line, which parses back to the
//! same property. `parentPath` is NOT written -- the caller resolves inheritance first
//! (ResolveEmatInheritance) so the deployed file stands alone; a deployed material that
//! still pointed at a parent inside a `.pak` would be exactly the unresolvable reference
//! it exists to replace.
inline std::string WriteEmatText(const EmatMaterial& material)
{
    const auto quoteIfNeeded = [](const std::string& token, bool forceQuote)
    {
        bool needs = forceQuote || token.empty();
        for (char c : token)
            if (Detail::IsSpace(c) || c == '"')
                needs = true;
        if (!needs)
        {
            char* end = nullptr;
            std::strtod(token.c_str(), &end);
            if (end && *end == '\0')
                needs = true; // would come back as a number
        }
        return needs ? "\"" + token + "\"" : token;
    };

    std::string text = material.className + " {\n";
    for (const EmatProperty& property : material.properties)
    {
        text += " " + quoteIfNeeded(property.name, false);
        for (const EmatValue& value : property.values)
        {
            text += " ";
            if (value.isTexture())
                text += "\"{" + value.guid + "}" + value.text + "\"";
            else if (value.isNumber)
                text += value.text;
            else
                text += quoteIfNeeded(value.text, false);
        }
        text += "\n";
    }
    text += "}\n";
    return text;
}

//! Convenience: the .emat that sits beside an .rvmat. DayZ authors both, and only
//! the .emat carries anything to shade with -- see the comment at the top.
inline std::string EmatPathForRvmat(std::string_view rvmatPath)
{
    const size_t dot = rvmatPath.rfind('.');
    if (dot == std::string_view::npos)
        return std::string(rvmatPath) + ".emat";
    return std::string(rvmatPath.substr(0, dot)) + ".emat";
}

//! Folds a parent `.emat`'s properties into a child that derives from it.
//!
//! The child wins on any property it names; everything else is inherited. That is
//! the only rule the corpus supports and it is the one that matters: Reforger's
//! inheriting materials are overwhelmingly a base material plus two or three
//! texture overrides, so a child read WITHOUT its parent keeps its own maps and
//! silently loses the whole shading setup -- shader family, blend mode, surface
//! properties -- rather than failing in any visible way.
//!
//! `load` maps a parent path to its text. It returns false when the parent cannot
//! be found, which leaves the child untouched rather than half-merged: an
//! incompletely resolved material is worse than an unresolved one, because it
//! looks resolved.
//!
//! `depthLimit` bounds parent chains. Enfusion's own base materials do chain: over
//! Reforger's 10,311 materials, all 1,892 inheriting ones resolve fully within the
//! shipped corpus, at depths 1 (1,499), 2 (307), 3 (72) and 4 (14) -- so 8 is twice
//! the measured maximum, and the limit exists because a cycle in authored data must
//! not become a hang, not because deep chains are expected.
template <typename LoadFn>
inline bool ResolveEmatInheritance(EmatMaterial& material, LoadFn&& load, int depthLimit = 8)
{
    int depth = 0;
    std::vector<std::string> pending;
    for (std::string parent = material.parentPath; !parent.empty();)
    {
        if (++depth > depthLimit)
            return false;
        // A repeated path is a cycle, not a deep chain.
        for (const std::string& seen : pending)
            if (seen == parent)
                return false;
        pending.push_back(parent);

        std::string text;
        if (!load(parent, text))
            return false;
        EmatMaterial base = ParseEmat(text);
        if (!base.valid())
            return false;

        // Merge child over base, preserving the base's property order and
        // appending anything the child adds. Order is not cosmetic: a reader that
        // takes the first match of a repeated key would otherwise see the parent's.
        for (const EmatProperty& own : material.properties)
        {
            bool replaced = false;
            for (EmatProperty& inherited : base.properties)
            {
                if (EmatMaterial::NameMatches(inherited.name, own.name))
                {
                    inherited = own;
                    replaced = true;
                    break;
                }
            }
            if (!replaced)
                base.properties.push_back(own);
        }

        // The child's own class name and declared path stay authoritative.
        base.className = material.className;
        base.declaredPath = material.declaredPath;
        parent = base.parentPath;
        base.parentPath.clear();
        base.parentGuid.clear();
        material = std::move(base);
    }
    return true;
}

} // namespace Poseidon::Asset::Material
