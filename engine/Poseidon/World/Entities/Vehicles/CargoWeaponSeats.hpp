#pragma once

#include <Poseidon/IO/ParamFile/ParamFile.hpp>
#include <Poseidon/Foundation/Math/Math3D.hpp>
#include <cmath>
#include <string>
#include <vector>

namespace Poseidon
{
// Content contract only: a valid seat does not bypass animation, obstruction,
// authority or fire-permission checks. Angles are degrees in the vehicle frame,
// positive azimuth toward the right, positive elevation upward.
struct CargoWeaponSeat
{
    int cargoIndex = -1;
    RString weapon;
    bool rifleFamily = false;
    bool playerFreeAim = false; // Runtime player policy; never grants NPC fire permission.
    RString action;
    RString idleAction;
    RString raiseAction;
    RString reloadAction;
    RString lowerAction;
    float azimuth = 0;
    float halfAngle = 0;
    float minElevation = 0;
    float maxElevation = 0;
    float aiAimSpeed = 0; // Degrees/second at full skill/dexterity; 0 preserves legacy tracking.

    float AITrackingRate() const
    {
        return aiAimSpeed > 0 ? aiAimSpeed * (H_PI / 180.0f) : 0.2f;
    }

    bool AcceptsWeapon(const char* name, bool personalPrimary, bool bulletMode) const
    {
        return name && name[0] && bulletMode &&
            (rifleFamily ? personalPrimary : strcmpi(name, weapon) == 0);
    }

    bool HasTransitions() const { return idleAction.GetLength() > 0; }
    bool OwnsAction(const char* name) const
    {
        if (!name || !name[0])
            return false;
        return strcmpi(name, action) == 0 || (HasTransitions() &&
            (strcmpi(name, idleAction) == 0 || strcmpi(name, raiseAction) == 0 ||
             strcmpi(name, reloadAction) == 0 || strcmpi(name, lowerAction) == 0));
    }

    bool ContainsDirection(Vector3Par direction) const
    {
        if (cargoIndex < 0 || halfAngle <= 0)
            return false;
        if (!std::isfinite(direction.X()) || !std::isfinite(direction.Y()) || !std::isfinite(direction.Z()))
            return false;
        if (playerFreeAim)
            return direction.SquareSize() > 1e-12f;
        const double horizontal = std::hypot(double(direction.X()), double(direction.Z()));
        if (horizontal < 1e-8)
            return false;
        constexpr double degrees = 180.0 / 3.14159265358979323846;
        const double yaw = std::atan2(double(direction.X()), double(direction.Z())) * degrees;
        const double elevation = std::atan2(double(direction.Y()), horizontal) * degrees;
        const double delta = std::remainder(yaw - azimuth, 360.0);
        return std::abs(delta) <= halfAngle && elevation >= minElevation && elevation <= maxElevation;
    }
};

class CargoWeaponSeats
{
    std::vector<CargoWeaponSeat> _seats;
    std::string _error;

public:
    const std::string& Error() const { return _error; }
    const CargoWeaponSeat* Find(int cargoIndex) const
    {
        for (const auto& seat : _seats)
            if (seat.cargoIndex == cargoIndex)
                return &seat;
        return nullptr;
    }

    bool Load(const ParamEntry& vehicle, int cargoCount, const ParamEntry* configRoot = nullptr)
    {
        _seats.clear();
        _error.clear();
        const auto* table = vehicle.FindEntry("OPCargoWeapons");
        const auto fail = [&](const char* message) {
            _seats.clear(); // Never publish a partially valid permission table.
            _error = message;
            return false;
        };
        // Retail vehicle classes inherit access=3. Do not weaken that protection
        // to install content metadata. The same config tree can name an exact
        // vehicle outside CfgVehicles; a vehicle's own table always takes priority.
        if (!table && configRoot)
        {
            const auto* mappings = configRoot->FindEntry("CfgOPVehicleActions");
            if (mappings)
            {
                if (!mappings->IsClass())
                    return fail("CfgOPVehicleActions must be a class");
                const auto* mapping = mappings->FindEntry(vehicle.GetName());
                if (mapping)
                {
                    if (!mapping->IsClass())
                        return fail("vehicle action mapping must be a class");
                    table = mapping->FindEntry("OPCargoWeapons");
                }
            }
        }
        if (!table)
            return true; // Original and ordinary cargo remain unsupported.
        if (!table->IsClass())
            return fail("OPCargoWeapons must be a class");
        const auto* version = table->FindEntry("version");
        const auto* seats = table->FindEntry("Seats");
        if (!version || !version->IsIntValue() || int(*version) < 1 || int(*version) > 3 || !seats || !seats->IsClass())
            return fail("expected OPCargoWeapons version=1..3 and class Seats");
        if (cargoCount < 0 || seats->GetEntryCount() > cargoCount || seats->GetEntryCount() > 32)
            return fail("seat count exceeds cargo capacity or contract limit");
        for (int i = 0; i < seats->GetEntryCount(); ++i)
        {
            const auto& entry = seats->GetEntry(i);
            if (!entry.IsClass())
                return fail("Seats must contain seat classes only");
            CargoWeaponSeat seat;
            const auto* index = entry.FindEntry("cargoIndex");
            const auto* weapon = entry.FindEntry("weapon");
            const auto* family = entry.FindEntry("weaponFamily");
            const auto* action = entry.FindEntry("action");
            if (family && (int(*version) != 3 || weapon || !family->IsTextValue() ||
                           strcmpi(RStringB(*family), "rifle") != 0))
                return fail("weaponFamily requires version 3, rifle, and no weapon override");
            if (!index || !index->IsIntValue() || (!family && (!weapon || !weapon->IsTextValue())) ||
                !action || !action->IsTextValue())
                return fail("seat requires integer cargoIndex and explicit weapon/action names");
            seat.cargoIndex = int(*index);
            seat.rifleFamily = family != nullptr;
            if (weapon) seat.weapon = RStringB(*weapon);
            seat.action = RStringB(*action);
            if (seat.cargoIndex < 0 || seat.cargoIndex >= cargoCount || Find(seat.cargoIndex) ||
                (!seat.rifleFamily && !seat.weapon.GetLength()) || !seat.action.GetLength())
                return fail("invalid or duplicate cargo index, or empty weapon/action");
            if (int(*version) >= 2)
            {
                const char* keys[] = {"idleAction", "raiseAction", "reloadAction", "lowerAction"};
                RString* names[] = {&seat.idleAction, &seat.raiseAction, &seat.reloadAction, &seat.lowerAction};
                for (int j = 0; j < 4; ++j)
                {
                    const auto* value = entry.FindEntry(keys[j]);
                    if (!value || !value->IsTextValue())
                        return fail("version 2 requires four explicit transition action names");
                    *names[j] = RStringB(*value);
                    if (!names[j]->GetLength() || strcmpi(*names[j], seat.action) == 0)
                        return fail("cargo animation states must be distinct and nonempty");
                    for (int k = 0; k < j; ++k)
                        if (strcmpi(*names[j], *names[k]) == 0)
                            return fail("cargo animation states must be distinct and nonempty");
                }
            }
            const char* keys[] = {"azimuth", "halfAngle", "minElevation", "maxElevation"};
            float* values[] = {&seat.azimuth, &seat.halfAngle, &seat.minElevation, &seat.maxElevation};
            for (int j = 0; j < 4; ++j)
            {
                const auto* value = entry.FindEntry(keys[j]);
                if (!value || (!value->IsFloatValue() && !value->IsIntValue()) || !std::isfinite(float(*value)))
                    return fail("seat requires four finite numeric angles");
                *values[j] = float(*value);
            }
            if (std::abs(seat.azimuth) > 180 || seat.halfAngle <= 0 || seat.halfAngle > 90 ||
                seat.minElevation < -85 || seat.maxElevation > 85 || seat.minElevation >= seat.maxElevation)
                return fail("invalid seat arc or elevation limits");
            if (const auto* speed = entry.FindEntry("aiAimSpeed"))
            {
                if ((!speed->IsFloatValue() && !speed->IsIntValue()) || !std::isfinite(float(*speed)) ||
                    float(*speed) < 0 || float(*speed) > 90)
                    return fail("aiAimSpeed must be finite and in 0..90 degrees/second");
                seat.aiAimSpeed = float(*speed);
            }
            _seats.push_back(seat);
        }
        return true;
    }
};
}
