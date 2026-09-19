#pragma once

#include <Poseidon/World/Entities/Weapons/Ballistics.hpp>
#include <algorithm>
#include <cmath>
#include <vector>

namespace Poseidon::InfantryCombat
{
enum class PathDecision { Continue, Block, Ground };

inline bool IntersectsBody(Vector3Par from, Vector3Par to, Vector3Par body, float radius)
{
    const Vector3 segment = to - from;
    const float length2 = segment.SquareSize();
    const float t = length2 > 1e-8f ? std::clamp(((body - from) * segment) / length2, 0.0f, 1.0f) : 0;
    return body.Distance2(from + segment * t) <= radius * radius;
}

// Group consecutive segments into conservative boxes. Each friendly body is
// read once, then distant blocks are rejected before segment-distance tests.
class BulletEnvelope
{
    struct Segment { Vector3 from, to; float time, radius; };
    struct Block { Vector3 low, high; float time; size_t begin, end; };
    std::vector<Segment> _segments;
    std::vector<Block> _blocks;
public:
    void Clear() { _segments.clear(); _blocks.clear(); }
    void Add(Vector3Par from, Vector3Par to, float time, float radius)
    {
        const size_t index = _segments.size();
        _segments.push_back({from, to, time, radius});
        if (index % 16 == 0) _blocks.push_back({from, from, time, index, index});
        auto& block = _blocks.back();
        for (int axis = 0; axis < 3; ++axis)
        {
            block.low[axis] = std::min(block.low[axis], std::min(from[axis], to[axis]) - radius);
            block.high[axis] = std::max(block.high[axis], std::max(from[axis], to[axis]) + radius);
        }
        block.time = time;
        block.end = index + 1;
    }
    bool Intersects(Vector3Par body, float extent, float speed) const
    {
        for (const auto& block : _blocks)
        {
            float distance2 = 0;
            for (int axis = 0; axis < 3; ++axis)
            {
                const float delta = body[axis] - std::clamp(body[axis], block.low[axis], block.high[axis]);
                distance2 += delta * delta;
            }
            const float reach = extent + speed * block.time;
            if (distance2 > reach * reach) continue;
            for (size_t i = block.begin; i < block.end; ++i)
            {
                const auto& segment = _segments[i];
                if (IntersectsBody(segment.from, segment.to, body, extent + speed * segment.time + segment.radius))
                    return true;
            }
        }
        return false;
    }
};

// This is a pre-emission envelope, not another projectile simulation. Do not
// stop at an AI engagement range or at the intended enemy: neither stops a miss.
// Sampling consumes no random numbers and uses the shot's drag/gravity settings.
template<class CheckSegment>
bool BulletPathSafe(Vector3 position, Vector3 velocity, const Ballistics::ShellParams& shell,
                    float spreadSpeed, Vector3Par wind, CheckSegment&& check)
{
    if (!std::isfinite(shell.timeToLive) || shell.timeToLive <= 0 || shell.timeToLive > 30 ||
        shell.initTime > 0 || !std::isfinite(velocity.SquareSize()) ||
        !std::isfinite(spreadSpeed) || spreadSpeed < 0)
        return false;
    constexpr float step = 1.0f / 60;
    for (float time = 0; time < shell.timeToLive; )
    {
        const float dt = std::min(step, shell.timeToLive - time);
        const Vector3 next = position + velocity * dt;
        time += dt;
        // Full rectangular dispersion support (both axes), without assuming
        // that drag will shrink it. Extra padding covers integration/pose lag.
        const float envelope = 0.35f + spreadSpeed * time + 0.1f * time;
        const auto decision = check(position, next, time, envelope);
        if (decision == PathDecision::Block) return false;
        if (decision == PathDecision::Ground) return true;
        const Vector3 airspeed = velocity - wind;
        const float speed = airspeed.Size();
        Vector3 acceleration = airspeed * (speed * Ballistics::DragRetardation(
            shell.dragModel, shell.airFriction, shell.ballisticCoefficient, speed));
        acceleration[1] -= shell.coefGravity * 9.8066f;
        velocity += acceleration * dt;
        position = next;
    }
    return true;
}
}
