#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Graphics/Textures/TextureBirthBeforeUse.hpp>

#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

namespace
{
struct FakeTexture { int incarnation = 0; };
struct FakeLease { int volume = 0; };
using Observer = Poseidon::render::TextureBirthBeforeUse<std::shared_ptr<FakeTexture>, FakeLease>;

Observer::Member Member()
{
    Observer::Member m;
    m.volume = 7; m.archiveBytes = 4096; m.offset = 8; m.bytes = 128; m.fileId[0] = 0x42;
    return m;
}
struct Flag
{
    std::string old;
    bool had = false;
    explicit Flag(const char* value)
    {
        if (const char* prior = std::getenv("WGR_OBJECT_STREAM_DAYZ_BIRTH_OBSERVER"))
        { old = prior; had = true; }
        Set(value);
    }
    ~Flag() { Set(had ? old.c_str() : nullptr); }
    static void Set(const char* value)
    {
#ifdef _WIN32
        _putenv_s("WGR_OBJECT_STREAM_DAYZ_BIRTH_OBSERVER", value ? value : "");
#else
        if (value) setenv("WGR_OBJECT_STREAM_DAYZ_BIRTH_OBSERVER", value, 1);
        else unsetenv("WGR_OBJECT_STREAM_DAYZ_BIRTH_OBSERVER");
#endif
    }
};
Observer::Facts Physical(std::shared_ptr<FakeTexture> texture,
                         std::shared_ptr<const FakeLease> lease)
{
    Observer::Facts facts;
    facts.texture = std::move(texture);
    facts.sourceName = R"(dz\wall_co.paa)";
    facts.currentBirthLease = std::move(lease);
    facts.member = Member();
    facts.mountedCurrent = true;
    return facts;
}
Observer::Facts Generated(std::shared_ptr<FakeTexture> texture)
{
    Observer::Facts facts;
    facts.texture = std::move(texture);
    facts.sourceName = "#(argb,8,8,3)color(1,1,1,1)";
    facts.dynamic = true;
    facts.generatedDeferred = true;
    return facts;
}
}

TEST_CASE("Before-use birth scope is exact opt-in and restores nested owner scopes",
          "[texture-birth-before-use]")
{
    Observer outer, inner;
    Flag flag("0");
    {
        Observer::Scope scope(outer, true, true);
        REQUIRE(Observer::Active() == nullptr);
    }
    Flag::Set("1");
    {
        Observer::Scope scope(outer, false, true);
        REQUIRE(Observer::Active() == nullptr);
    }
    {
        Observer::Scope scope(outer, true, false);
        REQUIRE(Observer::Active() == nullptr);
    }
    {
        Observer::Scope scope(outer, true, true);
        REQUIRE(Observer::Active() == &outer);
        {
            Observer::Scope nested(inner, true, true);
            REQUIRE(Observer::Active() == &inner);
        }
        REQUIRE(Observer::Active() == &outer);
    }
    REQUIRE(Observer::Active() == nullptr);
}

TEST_CASE("Before-use birth observer holds exact physical source before later use",
          "[texture-birth-before-use]")
{
    Observer observer;
    auto texture = std::make_shared<FakeTexture>();
    auto lease = std::make_shared<const FakeLease>();
    const auto* identity = texture.get();
    int gathers = 0;
    REQUIRE(observer.Observe(identity, [&] { ++gathers; return Physical(texture, lease); }));
    REQUIRE(observer.Observe(identity, [&]() -> Observer::Facts {
        ++gathers; return {}; // duplicate must never call this callback
    }));
    REQUIRE(gathers == 1);
    const auto report = observer.Read();
    REQUIRE(report.captured == 1);
    REQUIRE(report.physical == 1);
    REQUIRE(report.generated == 0);
    REQUIRE(report.duplicateCalls == 1);
    REQUIRE_FALSE(report.partial);
    REQUIRE_FALSE(observer.FinalRoleClosureProven());
    REQUIRE(observer.At(0).token == 1);
    REQUIRE(observer.At(0).member == Member());
    REQUIRE(observer.At(0).SourceName() == R"(dz\wall_co.paa)");
    std::weak_ptr<FakeTexture> weakTexture = texture;
    std::weak_ptr<const FakeLease> weakLease = lease;
    texture.reset(); lease.reset();
    REQUIRE_FALSE(weakTexture.expired());
    REQUIRE_FALSE(weakLease.expired());
    observer.Cancel();
    REQUIRE(weakTexture.expired());
    REQUIRE(weakLease.expired());
}

TEST_CASE("Before-use birth observer separates generated from unsupported dynamic",
          "[texture-birth-before-use]")
{
    Observer observer;
    auto generated = std::make_shared<FakeTexture>();
    REQUIRE(observer.Observe(generated.get(), [&] { return Generated(generated); }));
    REQUIRE(observer.At(0).kind == Observer::Kind::Generated);
    REQUIRE(observer.At(0).generatedDeferred);
    REQUIRE(observer.Read().generated == 1);

    auto arbitrary = std::make_shared<FakeTexture>();
    REQUIRE_FALSE(observer.Observe(arbitrary.get(), [&] {
        Observer::Facts facts;
        facts.texture = arbitrary;
        facts.sourceName = "dynamic-font-atlas";
        facts.dynamic = true;
        return facts;
    }));
    REQUIRE(observer.Read().unsupportedDynamic == 1);
    REQUIRE(observer.Read().partial);
    REQUIRE(observer.Read().captured == 1);
}

TEST_CASE("Before-use birth observer refuses unknown source and bounds total retained rows",
          "[texture-birth-before-use]")
{
    Observer first, second;
    auto missing = std::make_shared<FakeTexture>();
    REQUIRE_FALSE(first.Observe(missing.get(), [&] {
        auto facts = Physical(missing, {});
        return facts;
    }));
    REQUIRE(first.Read().missingSource == 1);
    std::vector<std::shared_ptr<FakeTexture>> textures;
    auto lease = std::make_shared<const FakeLease>();
    for (size_t i = 0; i < Observer::MaxRows; ++i)
    {
        textures.push_back(std::make_shared<FakeTexture>());
        REQUIRE(first.Observe(textures.back().get(), [&] { return Physical(textures.back(), lease); }));
    }
    REQUIRE(first.Read().captured == Observer::MaxRows);
    auto extra = std::make_shared<FakeTexture>();
    int called = 0;
    REQUIRE_FALSE(first.Observe(extra.get(), [&] { ++called; return Physical(extra, lease); }));
    REQUIRE_FALSE(second.Observe(extra.get(), [&] { ++called; return Physical(extra, lease); }));
    REQUIRE(called == 0);
    REQUIRE(first.Read().capacityRefused == 1);
    REQUIRE(second.Read().capacityRefused == 1); // process cap across nested/overlapping observers
    first.Cancel();
    REQUIRE(second.Observe(extra.get(), [&] { ++called; return Physical(extra, lease); }));
    REQUIRE(called == 1);
}
