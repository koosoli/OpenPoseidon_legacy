#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Graphics/Textures/TextureReuseOnlyBinding.hpp>

#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

namespace
{
struct FakeTexture {};
using Guard = Poseidon::render::TextureReuseOnlyBinding<std::shared_ptr<FakeTexture>>;

struct Flag
{
    std::string prior;
    bool had = false;
    explicit Flag(const char* value)
    {
        if (const char* old = std::getenv("WGR_OBJECT_STREAM_DAYZ_REUSE_ONLY"))
        { prior = old; had = true; }
        Set(value);
    }
    ~Flag() { Set(had ? prior.c_str() : nullptr); }
    static void Set(const char* value)
    {
#ifdef _WIN32
        _putenv_s("WGR_OBJECT_STREAM_DAYZ_REUSE_ONLY", value ? value : "");
#else
        if (value) setenv("WGR_OBJECT_STREAM_DAYZ_REUSE_ONLY", value, 1);
        else unsetenv("WGR_OBJECT_STREAM_DAYZ_REUSE_ONLY");
#endif
    }
};

Guard::Snapshot Physical(uint64_t handle = 17, uint32_t slot = 5)
{ return {handle, slot, false, false}; }
Guard::Snapshot Generated(uint64_t handle = 29, bool deferred = false)
{ return {handle, 0, true, deferred}; }
}

TEST_CASE("Reuse-only scope requires exact owner opt-in and restores nesting", "[texture-reuse-only]")
{
    Flag flag("0");
    Guard outer, inner;
    { Guard::Scope scope(outer, true, true); REQUIRE(Guard::Active() == nullptr); }
    Flag::Set("1");
    { Guard::Scope scope(outer, false, true); REQUIRE(Guard::Active() == nullptr); }
    { Guard::Scope scope(outer, true, false); REQUIRE(Guard::Active() == nullptr); }
    {
        Guard::Scope scope(outer, true, true);
        REQUIRE(Guard::Active() == &outer);
        { Guard::Scope nested(inner, true, true); REQUIRE(Guard::Active() == &inner); }
        REQUIRE(Guard::Active() == &outer);
    }
    REQUIRE(Guard::Active() == nullptr);
}

TEST_CASE("Reuse-only final binding accepts only held unchanged resident handles", "[texture-reuse-only]")
{
    Flag flag("1");
    Guard guard;
    auto texture = std::make_shared<FakeTexture>();
    const void* identity = texture.get();
    REQUIRE(guard.Add(texture, Physical()));
    REQUIRE(guard.Add(texture, Physical()));
    REQUIRE(guard.Seal());
    std::weak_ptr<FakeTexture> weak = texture;
    texture.reset();
    REQUIRE_FALSE(weak.expired());
    {
        Guard::Scope scope(guard, true, true);
        REQUIRE(Guard::Active()->Check(identity, Physical()) == 17);
    }
    const auto report = guard.Read();
    REQUIRE(report.rows == 1);
    REQUIRE(report.duplicateAdds == 1);
    REQUIRE(report.allowedChecks == 1);
    REQUIRE_FALSE(report.failed);
    REQUIRE_FALSE(guard.FinalRoleClosureProven());
    guard.Cancel();
    REQUIRE(weak.expired());
}

TEST_CASE("Reuse-only gate refuses unsealed, unexpected, cold and changed images", "[texture-reuse-only]")
{
    auto texture = std::make_shared<FakeTexture>();
    auto other = std::make_shared<FakeTexture>();
    {
        Guard unsealed;
        REQUIRE(unsealed.Add(texture, Physical()));
        REQUIRE(unsealed.Check(texture.get(), Physical()) == 0);
        REQUIRE(unsealed.Read().deniedNotSealed == 1);
    }
    {
        Guard unexpected;
        REQUIRE(unexpected.Add(texture, Physical()));
        REQUIRE(unexpected.Seal());
        REQUIRE(unexpected.Check(other.get(), Physical()) == 0);
        REQUIRE(unexpected.Read().deniedUnexpected == 1);
    }
    {
        Guard cold;
        REQUIRE(cold.Add(texture, Physical()));
        REQUIRE(cold.Seal());
        REQUIRE(cold.Check(texture.get(), Physical(0)) == 0);
        REQUIRE(cold.Read().deniedCold == 1);
    }
    {
        Guard changed;
        REQUIRE(changed.Add(texture, Physical()));
        REQUIRE(changed.Seal());
        REQUIRE(changed.Check(texture.get(), Physical(17, 6)) == 0);
        REQUIRE(changed.Read().deniedChanged == 1);
        REQUIRE(changed.Check(texture.get(), Physical()) == 0); // poison is sticky
        REQUIRE(changed.Read().deniedPoisoned == 1);
    }
}

TEST_CASE("Reuse-only gate leaves deferred generated image for a later normal owner stage", "[texture-reuse-only]")
{
    auto generated = std::make_shared<FakeTexture>();
    {
        Guard notResident;
        REQUIRE_FALSE(notResident.Add(generated, Generated(0, true)));
        REQUIRE_FALSE(notResident.Seal());
        REQUIRE(notResident.Check(generated.get(), Generated(0, true)) == 0);
    }
    {
        Guard changed;
        REQUIRE(changed.Add(generated, Generated()));
        REQUIRE(changed.Seal());
        REQUIRE(changed.Check(generated.get(), Generated(0, true)) == 0);
        REQUIRE(changed.Read().deniedDeferred == 1);
    }
    // This pure gate does not own or mutate the actual deferred RGBA payload.
    // The integration test must prove EnsureUploaded can stage it after scope exit.
}

TEST_CASE("Reuse-only allowlist is bounded across overlapping owner packets", "[texture-reuse-only]")
{
    Guard first, second;
    std::vector<std::shared_ptr<FakeTexture>> held;
    for (size_t i = 0; i < Guard::MaxRows; ++i)
    {
        held.push_back(std::make_shared<FakeTexture>());
        REQUIRE(first.Add(held.back(), Physical()));
    }
    auto extra = std::make_shared<FakeTexture>();
    REQUIRE_FALSE(first.Add(extra, Physical()));
    REQUIRE_FALSE(second.Add(extra, Physical()));
    REQUIRE(first.Read().capacityRefused == 1);
    REQUIRE(second.Read().capacityRefused == 1);
    first.Cancel();
    // The failed packet is poisoned even after global capacity becomes free.
    REQUIRE_FALSE(second.Add(extra, Physical()));
    Guard fresh;
    REQUIRE(fresh.Add(extra, Physical()));
}
