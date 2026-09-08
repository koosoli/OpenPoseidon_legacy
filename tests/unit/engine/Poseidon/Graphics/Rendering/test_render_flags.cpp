#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Core/Types.hpp>
#include <Poseidon/Graphics/Rendering/RenderFlags.hpp>
#include <Poseidon/Graphics/Rendering/DecalDepth.hpp>
#include <cstdint>

// Phase 1 starts by mirroring each legacy bit into one of three typed
// categories.  The categories must (a) cover every bit, (b) never overlap,
// and (c) be losslessly roundtripable to and from the legacy int form so
// the migration can happen one consumer at a time.
//
// The static_asserts in `RenderFlags.hpp` already enforce (a) and (b) at
// compile time.  These runtime tests pin the specific bit values, so a
// future rename / renumber of any legacy enum constant in `types.hpp` (or
// an accidental drift between the int and the typed enum) is caught
// before it can confuse the bridge code in phase 1.2.

using namespace Poseidon::render;

TEST_CASE("Screen flares bypass particle depth without disabling smoke depth", "[Graphics][DecalDepth]")
{
    const int flare = NoZBuf | IsLight | IsAlphaFog;
    const int smoke = NoZWrite | IsAlpha | IsAlphaFog;
    REQUIRE(SelectDecalDepth(flare, true) == DecalDepth::None);
    REQUIRE(SelectDecalDepth(flare, false) == DecalDepth::None);
    REQUIRE(SelectDecalDepth(smoke, true) == DecalDepth::Soft);
    REQUIRE(SelectDecalDepth(smoke, false) == DecalDepth::Test);
    REQUIRE(SelectDecalDepth(NoZBuf | NoZWrite, true) == DecalDepth::None);
    REQUIRE(SelectDecalDepth(0, false) == DecalDepth::Test);
}

namespace
{

// Helper that fails the test with a clear message if `bit` doesn't end up
// in the routing category after SplitLegacy.
void requireRouting(int bit)
{
    LegacySpec s = SplitLegacy(bit);
    REQUIRE(static_cast<std::uint32_t>(s.routing) == static_cast<std::uint32_t>(bit));
    REQUIRE(s.material == Material::None);
    REQUIRE(s.backend == Backend::None);
}

void requireMaterial(int bit)
{
    LegacySpec s = SplitLegacy(bit);
    REQUIRE(static_cast<std::uint32_t>(s.material) == static_cast<std::uint32_t>(bit));
    REQUIRE(s.routing == Routing::None);
    REQUIRE(s.backend == Backend::None);
}

void requireBackend(int bit)
{
    LegacySpec s = SplitLegacy(bit);
    REQUIRE(static_cast<std::uint32_t>(s.backend) == static_cast<std::uint32_t>(bit));
    REQUIRE(s.routing == Routing::None);
    REQUIRE(s.material == Material::None);
}

} // namespace

TEST_CASE("RenderFlags: every legacy bit routes to exactly one category", "[render-flags][phase1]")
{
    SECTION("Routing")
    {
        requireRouting(OnSurface);
        requireRouting(IsOnSurface);
        requireRouting(NoShadow);
        requireRouting(ShadowDisabled);
        requireRouting(IsAlphaOrdered);
        requireRouting(NoDropdown);
        requireRouting(FogDisabled);
        requireRouting(IsColored);
        requireRouting(IsHidden);
        requireRouting(IsHiddenProxy);
        requireRouting(NoTexMerger);
    }

    SECTION("Material")
    {
        requireMaterial(IsAnimated);
        requireMaterial(BestMipmap);
        // DisableSun is bit 0x80000000 — careful with int sign.  The legacy
        // constant is declared as a hex literal in types.hpp; widen to
        // uint32_t for comparison.
        LegacySpec s = SplitLegacy(static_cast<std::uint32_t>(DisableSun));
        REQUIRE(static_cast<std::uint32_t>(s.material) == static_cast<std::uint32_t>(DisableSun));
        REQUIRE(s.routing == Routing::None);
        REQUIRE(s.backend == Backend::None);
    }

    SECTION("Backend")
    {
        requireBackend(GrassTexture);
        requireBackend(NoZBuf);
        requireBackend(NoZWrite);
        requireBackend(IsShadow);
        requireBackend(IsAlpha);
        requireBackend(IsTransparent);
        requireBackend(IsWater);
        requireBackend(IsLight);
        requireBackend(PointSampling);
        requireBackend(NoClamp);
        requireBackend(ClampU);
        requireBackend(ClampV);
        requireBackend(IsAlphaFog);
        requireBackend(DetailTexture);
        requireBackend(SpecularTexture);
        // ZBiasMask is a 2-bit field (low + high).  SpecLighting follows.
        requireBackend(ZBiasStep);
        requireBackend(ZBiasMask & ~ZBiasStep); // high bit alone
        requireBackend(SpecLighting);
    }
}

TEST_CASE("RenderFlags: SplitLegacy / MergeLegacy roundtrip is lossless", "[render-flags][phase1]")
{
    SECTION("zero")
    {
        REQUIRE(MergeLegacy(SplitLegacy(0)) == 0u);
    }

    SECTION("all bits set")
    {
        const std::uint32_t all = 0xFFFFFFFFu;
        REQUIRE(MergeLegacy(SplitLegacy(all)) == all);
    }

    SECTION("cockpit-like spec")
    {
        const std::uint32_t s = static_cast<std::uint32_t>(NoDropdown | FogDisabled | DisableSun | IsColored);
        REQUIRE(MergeLegacy(SplitLegacy(s)) == s);
    }

    SECTION("alpha-cutout backend spec")
    {
        const std::uint32_t s = static_cast<std::uint32_t>(IsAlpha | IsTransparent | NoZWrite | ClampU);
        REQUIRE(MergeLegacy(SplitLegacy(s)) == s);
    }

    SECTION("merge of category-split values equals the originating int")
    {
        const std::uint32_t s = static_cast<std::uint32_t>(IsAlpha | FogDisabled | DisableSun | NoTexMerger);
        LegacySpec split = SplitLegacy(s);
        // The typed pieces carry exactly the bits in their category.
        REQUIRE((static_cast<std::uint32_t>(split.routing) & static_cast<std::uint32_t>(FogDisabled)) != 0);
        REQUIRE((static_cast<std::uint32_t>(split.routing) & static_cast<std::uint32_t>(NoTexMerger)) != 0);
        REQUIRE((static_cast<std::uint32_t>(split.material) & static_cast<std::uint32_t>(DisableSun)) != 0);
        REQUIRE((static_cast<std::uint32_t>(split.backend) & static_cast<std::uint32_t>(IsAlpha)) != 0);
        // And the merge reconstitutes the int losslessly.
        REQUIRE(MergeLegacy(split) == s);
    }
}

TEST_CASE("RenderFlags: bitwise operators respect category boundaries", "[render-flags][phase1]")
{
    SECTION("Routing combines as expected")
    {
        Routing r = Routing::NoDropdown | Routing::FogDisabled;
        REQUIRE(Has(r, Routing::NoDropdown));
        REQUIRE(Has(r, Routing::FogDisabled));
        REQUIRE_FALSE(Has(r, Routing::IsColored));

        r |= Routing::IsColored;
        REQUIRE(Has(r, Routing::IsColored));

        Routing onlyFog = r & Routing::FogDisabled;
        REQUIRE(onlyFog == Routing::FogDisabled);
    }

    SECTION("Backend combines as expected")
    {
        Backend b = Backend::NoZBuf | Backend::IsAlpha | Backend::PointSampling;
        REQUIRE(Has(b, Backend::NoZBuf));
        REQUIRE(Has(b, Backend::PointSampling));
        REQUIRE_FALSE(Has(b, Backend::IsWater));
    }

    SECTION("Material is independent of Routing/Backend bit values")
    {
        // Material::DisableSun is bit 0x80000000.  Mixing categories must
        // be a compile error (not tested here at runtime), but the bit
        // values themselves shouldn't collide.
        REQUIRE((static_cast<std::uint32_t>(Material::DisableSun) & RoutingMask) == 0);
        REQUIRE((static_cast<std::uint32_t>(Material::DisableSun) & BackendMask) == 0);
    }
}

TEST_CASE("RenderFlags: LegacySpec equality", "[render-flags][phase1]")
{
    LegacySpec a{};
    LegacySpec b{};
    REQUIRE(a == b);

    a.routing = Routing::NoDropdown;
    REQUIRE(a != b);

    b.routing = Routing::NoDropdown;
    REQUIRE(a == b);

    a.backend = Backend::IsAlpha;
    REQUIRE(a != b);
}

TEST_CASE("RenderFlags: IsOnSurfaceSpec detects surface overlays", "[render-flags][surface]")
{
    const int onSurface = static_cast<int>(Routing::OnSurface);
    const int isOnSurface = static_cast<int>(Routing::IsOnSurface);
    const int noZWrite = static_cast<int>(Backend::NoZWrite);
    const int isAlpha = static_cast<int>(Backend::IsAlpha);

    // Either OnSurface bit marks the object as a surface overlay (road/decal).
    REQUIRE(IsOnSurfaceSpec(onSurface));
    REQUIRE(IsOnSurfaceSpec(isOnSurface));
    // A real track/road spec combines OnSurface with NoZWrite/IsAlpha — still a
    // surface overlay.
    REQUIRE(IsOnSurfaceSpec(onSurface | noZWrite | isAlpha));
    REQUIRE(IsOnSurfaceSpec(isOnSurface | noZWrite | isAlpha));

    // Non-surface specs are not.
    REQUIRE_FALSE(IsOnSurfaceSpec(0));
    REQUIRE_FALSE(IsOnSurfaceSpec(noZWrite | isAlpha));
    REQUIRE_FALSE(IsOnSurfaceSpec(static_cast<int>(Routing::NoDropdown)));
}
