// test_derived_asset_key.cpp - AST-020: deterministic derived-asset identity.
//
// Reusing a derived asset is only safe if the key covers everything that could
// change the output. Each case below is a way that guarantee is normally lost.

#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Asset/Cache/DerivedAssetKey.hpp>
#include <string>

using Poseidon::Asset::VirtualPath;
using Poseidon::Asset::Cache::DerivedAssetKey;

static DerivedAssetKey sample()
{
    DerivedAssetKey key;
    key.AddInput("model", VirtualPath::Parse("ca/air/ataka.p3d"), "aaaa");
    key.AddInput("material", VirtualPath::Parse("ca/air/data/ataka.rvmat"), "bbbb");
    key.AddInput("texture", VirtualPath::Parse("ca/air/data/ataka_co.paa"), "cccc");
    return key;
}

TEST_CASE("Derived key: identical inputs give an identical key", "[asset][cache][ast-020]")
{
    REQUIRE(sample().Compute() == sample().Compute());
    REQUIRE(sample().Compute().size() == 64);
}

TEST_CASE("Derived key: discovery order does not change the key", "[asset][cache][ast-020]")
{
    // Two machines walking a directory in different orders must agree, or the cache
    // misses on one of them for no reason -- or worse, both write different
    // artefacts for the same content.
    DerivedAssetKey reversed;
    reversed.AddInput("texture", VirtualPath::Parse("ca/air/data/ataka_co.paa"), "cccc");
    reversed.AddInput("material", VirtualPath::Parse("ca/air/data/ataka.rvmat"), "bbbb");
    reversed.AddInput("model", VirtualPath::Parse("ca/air/ataka.p3d"), "aaaa");
    REQUIRE(reversed.Compute() == sample().Compute());
}

TEST_CASE("Derived key: changed source content changes the key", "[asset][cache][ast-020]")
{
    // Keyed on content, not path or timestamp. The same virtual path names
    // different bytes across addon load orders, and an mtime says nothing about
    // whether an addon was rebuilt identically.
    DerivedAssetKey changed;
    changed.AddInput("model", VirtualPath::Parse("ca/air/ataka.p3d"), "aaaa");
    changed.AddInput("material", VirtualPath::Parse("ca/air/data/ataka.rvmat"), "DIFFERENT");
    changed.AddInput("texture", VirtualPath::Parse("ca/air/data/ataka_co.paa"), "cccc");
    REQUIRE(changed.Compute() != sample().Compute());
}

TEST_CASE("Derived key: the compiler version participates", "[asset][cache][ast-020]")
{
    // Without this, fixing a translation bug leaves every machine that already
    // cached the broken output still serving it.
    DerivedAssetKey next = sample();
    next.SetCompilerVersion(DerivedAssetKey::kCompilerVersion + 1);
    REQUIRE(next.Compute() != sample().Compute());

    // And artefacts of different versions must not share a filename, or a rollback
    // silently reads forward-version output.
    REQUIRE(next.FileName() != sample().FileName());
    REQUIRE(sample().FileName().rfind("v1-", 0) == 0);
}

TEST_CASE("Derived key: options participate", "[asset][cache][ast-020]")
{
    DerivedAssetKey withOption = sample();
    withOption.AddOption("uvChannels", "2");
    REQUIRE(withOption.Compute() != sample().Compute());

    // Option order must not matter either.
    DerivedAssetKey a = sample(), b = sample();
    a.AddOption("x", "1"); a.AddOption("y", "2");
    b.AddOption("y", "2"); b.AddOption("x", "1");
    REQUIRE(a.Compute() == b.Compute());
}

TEST_CASE("Derived key: fields cannot run together", "[asset][cache][ast-020]")
{
    // Concatenating without separators lets ("ab","c") and ("a","bc") collide. The
    // role and the hash are adjacent fields, so this is a real adjacency.
    DerivedAssetKey a, b;
    a.AddInput("model", VirtualPath::Parse("ca/x"), "aa");
    b.AddInput("modela", VirtualPath::Parse("ca/x"), "a");
    REQUIRE(a.Compute() != b.Compute());
}

TEST_CASE("Derived key: path spelling does not change the key", "[asset][cache][ast-020]")
{
    // Identity is the canonical virtual path (AST-015): separator and case are
    // normalised, so a material referring to
    // the same texture in different case does not fork the cache.
    DerivedAssetKey shouted;
    shouted.AddInput("model", VirtualPath::Parse("CA/AIR/ATAKA.P3D"), "aaaa");
    shouted.AddInput("material", VirtualPath::Parse("ca/air/data/ataka.rvmat"), "bbbb");
    shouted.AddInput("texture", VirtualPath::Parse("ca/air/data/ataka_co.paa"), "cccc");
    REQUIRE(shouted.Compute() == sample().Compute());
}
