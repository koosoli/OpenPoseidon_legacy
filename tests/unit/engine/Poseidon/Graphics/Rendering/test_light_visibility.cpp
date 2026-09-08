#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Graphics/Rendering/Lighting/Lights.hpp>
#include <Poseidon/World/Scene/Object.hpp>

using namespace Poseidon;

namespace
{
class AttachedReflectorProbe final : public LightReflector
{
    Object* _carrier;
public:
    explicit AttachedReflectorProbe(Object* carrier)
        : LightReflector(nullptr, Color(HWhite), Color(HBlack), 0.5f), _carrier(carrier) {}
    Object* AttachedOn() override { return _carrier; }
};
}

TEST_CASE("Light-base visibility dispatch preserves the carrier and bulb switch", "[Graphics][LightVisibility]")
{
    Ref<ObjectPlain> carrier = new ObjectPlain(nullptr, -1);
    AttachedReflectorProbe reflector(carrier);
    Light* light = &reflector;
    REQUIRE(light->AttachedOn() == carrier.GetRef());
    REQUIRE(light->HasVisibleBulb());
    reflector.SetDrawHalo(false);
    REQUIRE_FALSE(light->HasVisibleBulb());

    LightPoint point;
    light = &point;
    REQUIRE(light->AttachedOn() == nullptr);
    REQUIRE_FALSE(light->HasVisibleBulb());
}
