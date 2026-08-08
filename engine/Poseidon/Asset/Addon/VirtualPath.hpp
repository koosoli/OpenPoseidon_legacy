#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Poseidon::Asset
{

// AST-015 -- the canonical form of a Real Virtuality resource reference.
//
// Real Virtuality addresses content by a virtual path rooted at an addon prefix
// (`ca\air\data\ataka_co.paa`, `a3\plants_f\...`), never by host filesystem
// location. AST-007's corpus inventory found 1,560 models referencing `ca\...`
// and 30 referencing `a3\...`, and the same logical path is spelled
// inconsistently across a corpus: mixed case, forward or back slashes, sometimes
// a leading separator. Those are the SAME resource, and anything keying on the
// raw string treats them as different.
//
// This is deliberately a pure value type with no filesystem knowledge. Where the
// bytes actually live -- a loose file, a PBO, a mod folder -- is the resolver's
// problem; conflating the two is how an addon prefix ends up meaning a directory
// on one machine and nothing on another.
class VirtualPath
{
  public:
    VirtualPath() = default;

    static VirtualPath Parse(std::string_view raw)
    {
        VirtualPath path;
        path.original_ = std::string(raw);

        std::string text(raw);
        // Backslash is the canonical separator regardless of host platform.
        // ModelCache::normalizePath chose the separator by #ifdef _WIN32, which
        // makes the same virtual path hash differently on Windows and Linux --
        // fine for a host path, wrong for an identity that is platform-independent
        // by definition.
        std::replace(text.begin(), text.end(), '/', '\\');
        std::transform(text.begin(), text.end(), text.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

        // A leading root separator is optional in the wild and carries no meaning.
        size_t begin = text.find_first_not_of('\\');
        if (begin == std::string::npos)
            begin = text.size();

        // Collapse repeated separators; `ca\\air` and `ca\air` are one path.
        path.canonical_.reserve(text.size() - begin);
        bool previousWasSeparator = false;
        for (size_t i = begin; i < text.size(); ++i)
        {
            const char ch = text[i];
            if (ch == '\\' && previousWasSeparator)
                continue;
            previousWasSeparator = (ch == '\\');
            path.canonical_ += ch;
        }
        while (!path.canonical_.empty() && path.canonical_.back() == '\\')
            path.canonical_.pop_back();

        return path;
    }

    bool               empty() const { return canonical_.empty(); }
    const std::string& canonical() const { return canonical_; }
    // Kept so a diagnostic can quote what the file actually said. A message that
    // reports only the lowercased form makes a case-sensitivity bug in someone
    // else's tooling impossible to see from the log.
    const std::string& original() const { return original_; }

    // The addon namespace: the first component. `ca\air\data\x.paa` -> `ca`.
    std::string_view prefix() const
    {
        const size_t at = canonical_.find('\\');
        return std::string_view(canonical_).substr(0, at == std::string::npos ? canonical_.size() : at);
    }

    std::string_view extension() const
    {
        const size_t dot = canonical_.find_last_of('.');
        const size_t sep = canonical_.find_last_of('\\');
        if (dot == std::string::npos || (sep != std::string::npos && dot < sep))
            return {};
        return std::string_view(canonical_).substr(dot);
    }

    VirtualPath withExtension(std::string_view replacement) const
    {
        const std::string_view current = extension();
        std::string            rebuilt = canonical_.substr(0, canonical_.size() - current.size());
        rebuilt += replacement;
        VirtualPath path;
        path.canonical_ = rebuilt;
        path.original_  = original_;
        return path;
    }

    // Source-authored references name the artist's file, not the shipped one.
    //
    // Measured, not assumed: RVMATs in the indexed corpus name `.tga` in their
    // stages, and none of those files exist -- the shipped resource is `.paa`.
    // Verified against the Arma 3 install, where all seven dependencies of
    // `t_PinusP3s_F.p3d` resolve only after this substitution. A resolver that
    // skips it reports every texture as missing.
    VirtualPath shipped() const
    {
        const std::string_view ext = extension();
        if (ext == ".tga" || ext == ".tif" || ext == ".tiff" || ext == ".png")
            return withExtension(".paa");
        return *this;
    }

    // A host filesystem path that leaked into shipped content.
    //
    // Not hypothetical: the indexed corpus contains references like
    // `P:\ca\ca_e\data\default.rvmat` and `J:\bistudio\_helpers\...`, baked in
    // from a developer's drive. Their first component is a drive letter, so a
    // resolver that treats it as an addon prefix goes looking for an addon named
    // `p` and reports a missing addon instead of a bad reference.
    bool looksHostAbsolute() const
    {
        return canonical_.size() >= 2 && canonical_[1] == ':' &&
               ((canonical_[0] >= 'a' && canonical_[0] <= 'z'));
    }

    bool operator==(const VirtualPath& other) const { return canonical_ == other.canonical_; }
    bool operator!=(const VirtualPath& other) const { return !(*this == other); }
    bool operator<(const VirtualPath& other) const { return canonical_ < other.canonical_; }

    struct Hash
    {
        size_t operator()(const VirtualPath& path) const
        {
            return std::hash<std::string>()(path.canonical_);
        }
    };

  private:
    std::string canonical_;
    std::string original_;
};

// Which source owns an addon prefix.
//
// Separate from the paths themselves so host locations stay out of resource
// identity. AST-007 read the prefix each PBO declares -- `ca\air_acr`,
// `a3\plants_f` -- and prefixes are hierarchical: `ca\takistan\data` is declared
// by a different archive than `ca\takistan`, so lookup has to prefer the longest
// declared prefix rather than the first component.
class AddonNamespace
{
  public:
    struct Collision
    {
        std::string prefix;
        std::string existingSource;
        std::string newSource;
    };

    // Returns false when `prefix` was already declared by a different source. The
    // first declaration wins and the clash is recorded: silently rebinding a
    // prefix makes which archive a path resolves to depend on scan order, which
    // is precisely the bug that is impossible to diagnose after the fact.
    bool Declare(std::string_view prefix, std::string source)
    {
        const VirtualPath key = VirtualPath::Parse(prefix);
        auto              it  = sources_.find(key.canonical());
        if (it != sources_.end())
        {
            if (it->second != source)
                collisions_.push_back({key.canonical(), it->second, std::move(source)});
            return false;
        }
        sources_.emplace(key.canonical(), std::move(source));
        return true;
    }

    // Longest declared prefix that owns this path, or empty if none does.
    std::string_view Owner(const VirtualPath& path) const
    {
        const std::string& text = path.canonical();
        for (size_t at = text.size(); at != std::string::npos; at = text.find_last_of('\\', at - 1))
        {
            auto it = sources_.find(text.substr(0, at));
            if (it != sources_.end())
                return it->second;
            if (at == 0)
                break;
        }
        return {};
    }

    const std::vector<Collision>& collisions() const { return collisions_; }
    size_t                        size() const { return sources_.size(); }

  private:
    std::unordered_map<std::string, std::string> sources_;
    std::vector<Collision>                       collisions_;
};

} // namespace Poseidon::Asset
