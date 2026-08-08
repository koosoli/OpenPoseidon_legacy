#pragma once

#include <Poseidon/Asset/Addon/VirtualPath.hpp>
#include <Poseidon/Foundation/Algorithms/Sha256.hpp>
#include <algorithm>
#include <string>
#include <vector>

namespace Poseidon::Asset::Cache
{

// AST-020 -- the identity of a derived runtime asset.
//
// A derived asset is whatever the compile step produces from a source model, its
// materials and its textures. Reusing one is only safe if the key covers every
// input that could change the output, which is three separate things:
//
//   1. the CONTENT of every source, by hash -- not its path or timestamp, because
//      the same virtual path names different bytes across addon load orders and a
//      mtime says nothing about whether an addon was rebuilt identically;
//   2. the compiler VERSION, so changing how translation works invalidates
//      everything it previously produced. Without this, fixing a bug leaves every
//      machine that already cached the broken output still serving it;
//   3. the OPTIONS the compile ran under, since the same bytes legitimately
//      produce different results under different settings.
//
// Determinism is the point. The same inputs must produce the same key on any
// machine, so the key is built from sorted, canonical, explicitly-ordered fields
// and never from iteration order, pointer values or wall-clock time.
class DerivedAssetKey
{
  public:
    // Bump when the meaning of the compiled output changes. Deliberately a plain
    // integer a human edits, not something derived from a build timestamp: a
    // rebuild that changes nothing must not invalidate every cache in existence.
    static constexpr int kCompilerVersion = 1;

    struct Input
    {
        std::string role;   // "model", "material", "texture" -- what it was used as
        VirtualPath path;   // canonical identity
        std::string sha256; // content, not location
    };

    void SetCompilerVersion(int version) { compilerVersion_ = version; }

    void AddInput(std::string role, const VirtualPath& path, std::string contentHash)
    {
        inputs_.push_back({std::move(role), path, std::move(contentHash)});
    }

    // Options that change the output. Recorded as name/value pairs so adding one
    // is not a silent behaviour change: a new option shifts the key, which is the
    // correct outcome.
    void AddOption(std::string name, std::string value)
    {
        options_.emplace_back(std::move(name), std::move(value));
    }

    std::string Compute() const
    {
        // Sorted, so the caller's discovery order cannot leak into the key. Two
        // machines walking a directory in different orders must agree.
        std::vector<Input> inputs = inputs_;
        std::sort(inputs.begin(), inputs.end(), [](const Input& a, const Input& b) {
            if (a.role != b.role)
                return a.role < b.role;
            if (a.path.canonical() != b.path.canonical())
                return a.path.canonical() < b.path.canonical();
            return a.sha256 < b.sha256;
        });
        std::vector<std::pair<std::string, std::string>> options = options_;
        std::sort(options.begin(), options.end());

        Foundation::Sha256 hash;
        // Field separators that cannot occur in the values, so ("ab","c") and
        // ("a","bc") cannot hash alike.
        hash.Update("compat-c1/derived/v");
        hash.Update(std::to_string(compilerVersion_));
        hash.Update("\n");
        for (const Input& input : inputs)
        {
            hash.Update(input.role);
            hash.Update("\x1f");
            hash.Update(input.path.canonical());
            hash.Update("\x1f");
            hash.Update(input.sha256);
            hash.Update("\x1e");
        }
        hash.Update("\n");
        for (const auto& option : options)
        {
            hash.Update(option.first);
            hash.Update("\x1f");
            hash.Update(option.second);
            hash.Update("\x1e");
        }
        return hash.Hex();
    }

    // A cache file name that carries its own version, so artefacts from different
    // compiler versions coexist instead of overwriting each other. A shared name
    // would make a version rollback silently read forward-version output.
    std::string FileName() const
    {
        return "v" + std::to_string(compilerVersion_) + "-" + Compute() + ".pdc";
    }

    const std::vector<Input>& inputs() const { return inputs_; }
    int                       compilerVersion() const { return compilerVersion_; }

  private:
    int                                             compilerVersion_ = kCompilerVersion;
    std::vector<Input>                              inputs_;
    std::vector<std::pair<std::string, std::string>> options_;
};

} // namespace Poseidon::Asset::Cache
