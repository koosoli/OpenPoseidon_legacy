// Corner peeking shares Q/E with the OFP on-foot defaults (Q: stance-up and slow forward,
// E: fast forward). While the player can lean, the lean actions claim those keys in the
// infantry context; swimming (Q surfaces a diver) and every vehicle context keep the old
// meaning. These cases pin that rule at the input layer.

#include <Poseidon/Input/InputSubsystem.hpp>
#include <Poseidon/Input/InputBinding.hpp>
#include <Poseidon/Input/InputProfile.hpp>
#include <Poseidon/Input/KeyInput.hpp>
#include <SDL3/SDL_scancode.h>
#include <catch2/catch_test_macros.hpp>

using namespace Poseidon;
namespace Poseidon
{
extern Input GInput;
}

namespace
{
struct LeanFixture
{
    LeanFixture()
        : context(InputSubsystem::Instance().GetContext()),
          infantry(InputSubsystem::Instance().GetProfile(InputContext::Infantry)),
          heli(InputSubsystem::Instance().GetProfile(InputContext::HeliPilot)), focus(GInput.gameFocusLost),
          q(GInput.keyboard.keys[SDL_SCANCODE_Q]), pageUp(GInput.keyboard.keys[SDL_SCANCODE_PAGEUP])
    {
        auto& sub = InputSubsystem::Instance();
        GInput.gameFocusLost = 0;
        InputProfile& p = sub.GetProfile(InputContext::Infantry);
        p.ClearAll();
        p.Bind(UAMoveUp, InputCode::Key(SDL_SCANCODE_Q));
        p.Bind(UAMoveUp, InputCode::Key(SDL_SCANCODE_PAGEUP));
        p.Bind(UALeanLeft, InputCode::Key(SDL_SCANCODE_Q));
        InputProfile& h = sub.GetProfile(InputContext::HeliPilot);
        h.ClearAll();
        h.Bind(UAMoveUp, InputCode::Key(SDL_SCANCODE_Q));
    }
    ~LeanFixture()
    {
        auto& sub = InputSubsystem::Instance();
        sub.SetLeanClaimsSharedKeys(false);
        sub.GetProfile(InputContext::Infantry) = infantry;
        sub.GetProfile(InputContext::HeliPilot) = heli;
        sub.SetContext(context);
        GInput.gameFocusLost = focus;
        GInput.keyboard.keys[SDL_SCANCODE_Q] = q;
        GInput.keyboard.keys[SDL_SCANCODE_PAGEUP] = pageUp;
    }
    InputContext context;
    InputProfile infantry, heli;
    int focus;
    float q, pageUp;
};
} // namespace

TEST_CASE("Lean claims a shared key only on foot", "[input][lean]")
{
    LeanFixture fixture;
    auto& sub = InputSubsystem::Instance();
    sub.SetContext(InputContext::Infantry);
    GInput.keyboard.keys[SDL_SCANCODE_Q] = 1.0f;
    GInput.keyboard.keys[SDL_SCANCODE_PAGEUP] = 0.0f;

    sub.SetLeanClaimsSharedKeys(false); // swimming, prone: Q is stance-up / surface
    REQUIRE_FALSE(sub.LeanClaimsSharedKeys());
    REQUIRE(sub.GetAction(UAMoveUp) > 0.5f);

    sub.SetLeanClaimsSharedKeys(true); // standing or crouched: Q leans
    REQUIRE(sub.LeanClaimsSharedKeys());
    REQUIRE(sub.GetActionExcluding(UAMoveUp, UALeanLeft, UALeanRight) == 0.0f);
    REQUIRE(sub.GetAction(UALeanLeft) > 0.5f);

    // The binding the lean does not share still works.
    GInput.keyboard.keys[SDL_SCANCODE_Q] = 0.0f;
    GInput.keyboard.keys[SDL_SCANCODE_PAGEUP] = 1.0f;
    REQUIRE(sub.GetActionExcluding(UAMoveUp, UALeanLeft, UALeanRight) > 0.5f);

    // A vehicle context ignores the claim: a helicopter's collective keeps Q.
    sub.SetContext(InputContext::HeliPilot);
    REQUIRE_FALSE(sub.LeanClaimsSharedKeys());
}
