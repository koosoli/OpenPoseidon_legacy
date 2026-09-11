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
    RString action;
    float azimuth = 0;
    float halfAngle = 0;
    float minElevation = 0;
    float maxElevation = 0;

    bool ContainsDirection(Vector3Par direction) const
    {
        if (cargoIndex < 0 || halfAngle <= 0)
            return false;
        if (!std::isfinite(direction.X()) || !std::isfinite(direction.Y()) || !std::isfinite(direction.Z()))
            return false;
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

    bool Load(const ParamEntry& vehicle, int cargoCount)
    {
        _seats.clear();
        _error.clear();
        const auto* table = vehicle.FindEntry("OPCargoWeapons");
        if (!table)
            return true; // Original and ordinary cargo remain unsupported.
        const auto fail = [&](const char* message) {
            _seats.clear(); // Never publish a partially valid permission table.
            _error = message;
            return false;
        };
        if (!table->IsClass())
            return fail("OPCargoWeapons must be a class");
        const auto* version = table->FindEntry("version");
        const auto* seats = table->FindEntry("Seats");
        if (!version || !version->IsIntValue() || int(*version) != 1 || !seats || !seats->IsClass())
            return fail("expected OPCargoWeapons version=1 and class Seats");
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
            const auto* action = entry.FindEntry("action");
            if (!index || !index->IsIntValue() || !weapon || !weapon->IsTextValue() ||
                !action || !action->IsTextValue())
                return fail("seat requires integer cargoIndex and explicit weapon/action names");
            seat.cargoIndex = int(*index);
            seat.weapon = RStringB(*weapon);
            seat.action = RStringB(*action);
            if (seat.cargoIndex < 0 || seat.cargoIndex >= cargoCount || Find(seat.cargoIndex) ||
                !seat.weapon.GetLength() || !seat.action.GetLength())
                return fail("invalid or duplicate cargo index, or empty weapon/action");
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
            _seats.push_back(seat);
        }
        return true;
    }
};
}
