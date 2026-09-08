#pragma once

#include <algorithm>
#include <optional>
#include "GameSettingsConfig.hpp"

namespace Poseidon
{
// The three render distances the engine derives from one master view distance.
struct ViewDistances
{
    float view = 900.0f;    // tacticalZ / horizontZ — terrain + fog far plane
    float objects = 600.0f; // objectsZ — object cull distance
    float shadows = 250.0f; // shadowsZ — shadow caster / receiver distance
};

// Clamp + ratio parameters.  Defaults reproduce the original OFP derivation
// (objects = 2/3 of the view distance, shadows = 5/18) and the engine min/max
// clamps.  Passed in by value so the resolver has no dependency on engine /
// config globals and can be unit-tested in isolation.
struct ViewDistanceLimits
{
    float minView = 100.0f;
    // Keep in step with GameSettingsConfig::kMaxViewDistance (VD-001, 2026-09-08); the two
    // clamps are applied one after the other and the lower of them is what a player gets.
    float maxView = GameSettingsConfig::kMaxViewDistance;
    float minObject = 100.0f;
    // 5 km, raised from 3 km on 2026-09-08 (VD-001). This is the same value the --vd CLI
    // override has been granting all along (GameStateExtUi.cpp SetVisibility), so it is not a
    // new operating point -- it is the one that was already reachable in dev builds, measured
    // and made available to players.
    //
    // Measured, stock Everon aerial pose (see GameSettingsConfig.hpp for the full table):
    // going from 900 m to 5 km of objects took the drawn set from 742 to 6,857 instances and
    // 36.5k to 214k triangles, for +0.05 ms in `Objects: colour alpha-test` and +1.44 ms of
    // GPU frame -- of which +1.24 ms was the water surface, not the objects. Objects are
    // close to free here; that is why this cap could move and shadowCap below could not.
    float objectCap = 5000.0f; // objects never exceed this
    float minShadow = 50.0f;
    float shadowCap = 500.0f;            // shadows never exceed this
    float objectRatio = 600.0f / 900.0f; // objects = objectRatio * view
    float shadowRatio = 250.0f / 900.0f; // shadows = shadowRatio * view
};

// Pure resolver: turns the configured / mission view-distance inputs into the
// three render distances.  No engine dependencies — the caller supplies the
// values, applies the result, and decides separately whether a debug-menu
// override replaces the engine-resolved view distance.
class ViewDistanceResolver
{
  public:
    // Master view distance: the mission's value when it is respected and
    // present, otherwise the engine/user preferred value.  Clamped to
    // [minView, maxView].
    static float EffectiveView(float preferredView, bool respectMission, const std::optional<float>& missionView,
                               const ViewDistanceLimits& limits = {})
    {
        const float v = (respectMission && missionView.has_value()) ? *missionView : preferredView;
        return Clamp(v, limits.minView, limits.maxView);
    }

    // Derive object + shadow distances from a master view distance.  Shadows
    // never exceed the object distance (no casters past where objects draw).
    static ViewDistances Derive(float view, const ViewDistanceLimits& limits = {})
    {
        view = Clamp(view, limits.minView, limits.maxView);
        const float objects = Clamp(view * limits.objectRatio, limits.minObject, limits.objectCap);
        float shadows = view * limits.shadowRatio;
        shadows = std::min(shadows, limits.shadowCap);
        shadows = std::max(shadows, limits.minShadow);
        shadows = std::min(shadows, objects);
        return ViewDistances{view, objects, shadows};
    }

    // EffectiveView followed by Derive — the full config/mission -> 3 distances.
    static ViewDistances Resolve(float preferredView, bool respectMission, const std::optional<float>& missionView,
                                 const ViewDistanceLimits& limits = {})
    {
        return Derive(EffectiveView(preferredView, respectMission, missionView, limits), limits);
    }

  private:
    static float Clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
};
} // namespace Poseidon
