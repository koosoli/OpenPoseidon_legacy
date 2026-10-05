#pragma once

#include <cmath>

namespace Poseidon
{
// A wheel loads new soil by distance travelled, never once per simulation frame.
// Admission is checked at every anchor, so a road/airborne gap cannot be joined.
class MudWheelTrail
{
public:
    static constexpr float Spacing = 0.25f;
    static constexpr float MaxSegment = 4.0f;

    void Reset() { _valid = false; _remaining = Spacing; }

    template<class Contact>
    bool Update(float x, float y, float z, bool grounded, Contact&& contact)
    {
        if (!grounded || !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
        {
            Reset();
            return false;
        }
        const float distance = _valid ? std::hypot(x - _x, z - _z) : 0.0f;
        if (!_valid || distance > MaxSegment)
        {
            Reset();
            if (!contact(x, y, z)) return false;
            _valid = true;
            _x = x; _y = y; _z = z;
            return true;
        }
        bool admitted = true;
        for (float at = _remaining; at <= distance; at += Spacing)
        {
            const float t = at / distance;
            if (!contact(std::lerp(_x, x, t), std::lerp(_y, y, t), std::lerp(_z, z, t)))
            {
                admitted = false;
                break;
            }
        }
        if (!admitted) { Reset(); return false; }
        _remaining -= distance;
        while (_remaining <= 0.0f) _remaining += Spacing;
        _x = x; _y = y; _z = z;
        return true;
    }

private:
    float _x = 0, _y = 0, _z = 0, _remaining = Spacing;
    bool _valid = false;
};
} // namespace Poseidon
