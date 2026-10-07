#pragma once
#include <Poseidon/World/Entities/Weapons/Ballistics.hpp>
#include <Poseidon/World/Entities/Vehicles/Vehicle.hpp>
#include <algorithm>
#include <cmath>

namespace Poseidon::CombatAssists
{
inline int BearingDegrees(float x, float z)
{
    if (!std::isfinite(x) || !std::isfinite(z)) return 0;
    const float degrees = std::atan2(x, z) * (180.0f / 3.14159265358979323846f);
    return (int(std::lround(degrees)) + 360) % 360;
}

// Ground-only estimate: bounded to 500 steps and called at 5 Hz by the HUD.
// Uses the actual ammo's drag/gravity/delay, with the same explicit Euler ordering
// as ShotShell::Simulate. It does not predict buildings, bounces or dispersion.
template<class Surface>
bool PredictGroundImpact(const Ballistics::ShellParams& shell, Vector3 start, Vector3 speed,
                         Vector3 wind, Surface surface, float& range)
{
    constexpr float step = 0.02f;
    Vector3 position = start;
    float delay = shell.initTime;
    float life = std::min(shell.timeToLive, Ballistics::MaxFlightTime);
    if (!std::isfinite(life) || !std::isfinite(delay) || !std::isfinite(speed.SquareSize()) ||
        !std::isfinite(start.SquareSize()) || !std::isfinite(wind.SquareSize()) || life <= 0) return false;
    for (int i = 0; i < 500; ++i)
    {
        const Vector3 before = position;
        const Vector3 airflow = speed - wind;
        const float magnitude = airflow.Size();
        Vector3 accel = airflow * (magnitude * Ballistics::DragRetardation(
            shell.dragModel, shell.airFriction, shell.ballisticCoefficient, magnitude));
        accel[1] -= shell.coefGravity * G_CONST;
        delay -= step;
        if (delay <= 0)
        {
            life -= step;
            if (life < 0) return false;
            position += speed * step;
        }
        speed += accel * step;
        if (!std::isfinite(position.SquareSize()) || !std::isfinite(speed.SquareSize())) return false;
        if (delay > 0) continue;
        const float ground = surface(position.X(), position.Z());
        if (!std::isfinite(ground)) return false;
        if (position.Y() <= ground)
        {
            const float beforeHeight = before.Y() - surface(before.X(), before.Z());
            const float afterHeight = position.Y() - ground;
            if (!std::isfinite(beforeHeight)) return false;
            const float span = beforeHeight - afterHeight;
            const float t = span > 0.000001f ? std::clamp(beforeHeight / span, 0.0f, 1.0f) : 0;
            range = (before + (position - before) * t - start).SizeXZ();
            return std::isfinite(range);
        }
    }
    return false;
}
}
