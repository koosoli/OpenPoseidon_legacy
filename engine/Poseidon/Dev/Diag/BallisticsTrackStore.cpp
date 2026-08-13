#include <Poseidon/Dev/Diag/BallisticsTrackStore.hpp>

#include <cmath>
#include <cstring>

namespace Poseidon::Dev
{

const char* BallisticTerminusName(BallisticTerminus terminus)
{
    switch (terminus)
    {
        case BallisticTerminus::InFlight:
            return "in flight";
        case BallisticTerminus::HitObject:
            return "hit object";
        case BallisticTerminus::HitGround:
            return "hit ground";
        case BallisticTerminus::HitWater:
            return "hit water";
        case BallisticTerminus::Expired:
            return "expired (TTL)";
        case BallisticTerminus::Lost:
            return "lost";
    }
    return "?";
}

void BallisticName::Set(const char* value)
{
    if (!value)
    {
        text[0] = '\0';
        return;
    }
    std::size_t n = std::strlen(value);
    if (n >= static_cast<std::size_t>(Capacity))
    {
        n = static_cast<std::size_t>(Capacity) - 1;
    }
    std::memcpy(text, value, n);
    text[n] = '\0';
}

// ---------------------------------------------------------------------------
// BallisticTrack — derived facts
// ---------------------------------------------------------------------------

namespace
{
inline float Dot(float ax, float ay, float az, float bx, float by, float bz)
{
    return ax * bx + ay * by + az * bz;
}
} // namespace

float BallisticTrack::TimeOfFlight() const
{
    if (samples.empty())
    {
        return 0.0f;
    }
    return samples.back().t - samples.front().t;
}

float BallisticTrack::PathLength() const
{
    float total = 0.0f;
    for (std::size_t i = 1; i < samples.size(); ++i)
    {
        const float dx = samples[i].x - samples[i - 1].x;
        const float dy = samples[i].y - samples[i - 1].y;
        const float dz = samples[i].z - samples[i - 1].z;
        total += std::sqrt(dx * dx + dy * dy + dz * dz);
    }
    return total;
}

float BallisticTrack::StraightDistance() const
{
    if (samples.empty())
    {
        return 0.0f;
    }
    const BallisticSample& last = samples.back();
    const float dx = last.x - originX;
    const float dy = last.y - originY;
    const float dz = last.z - originZ;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

float BallisticTrack::MaxDrop() const
{
    float worst = 0.0f;
    for (const BallisticSample& s : samples)
    {
        const float vx = s.x - originX;
        const float vy = s.y - originY;
        const float vz = s.z - originZ;
        const float along = Dot(vx, vy, vz, dirX, dirY, dirZ);
        // Height of the un-dropped line of departure at this range, minus the
        // height actually reached. Positive = the round is below the line.
        const float lineY = dirY * along;
        const float drop = lineY - vy;
        if (drop > worst)
        {
            worst = drop;
        }
    }
    return worst;
}

float BallisticTrack::MaxLateral() const
{
    // Right-hand normal to the line of departure, in the horizontal plane:
    // right = normalize(cross(up, dir)) with up = (0,1,0)  ->  (dz, 0, -dx).
    float rx = dirZ;
    float rz = -dirX;
    const float len = std::sqrt(rx * rx + rz * rz);
    if (len < 1e-6f)
    {
        return 0.0f; // shot fired straight up or down: "lateral" is meaningless
    }
    rx /= len;
    rz /= len;

    float worst = 0.0f;
    for (const BallisticSample& s : samples)
    {
        const float lateral = (s.x - originX) * rx + (s.z - originZ) * rz;
        if (std::fabs(lateral) > std::fabs(worst))
        {
            worst = lateral;
        }
    }
    return worst;
}

float BallisticTrack::TerminalSpeed() const
{
    if (samples.size() < 2)
    {
        return 0.0f;
    }
    const BallisticSample& a = samples[samples.size() - 2];
    const BallisticSample& b = samples.back();
    const float dt = b.t - a.t;
    if (dt <= 1e-6f)
    {
        return 0.0f;
    }
    const float dx = b.x - a.x;
    const float dy = b.y - a.y;
    const float dz = b.z - a.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz) / dt;
}

// ---------------------------------------------------------------------------
// BallisticsTrackStore
// ---------------------------------------------------------------------------

void BallisticsTrackStore::SetCapacity(int shots)
{
    if (shots < 1)
    {
        shots = 1;
    }
    if (shots > 256)
    {
        shots = 256;
    }
    _capacity = shots;
    EvictToCapacity();
}

void BallisticsTrackStore::SetMaxSamples(int samples)
{
    if (samples < 8)
    {
        samples = 8;
    }
    if (samples > 4096)
    {
        samples = 4096;
    }
    _maxSamples = samples;
}

void BallisticsTrackStore::EvictToCapacity()
{
    while (static_cast<int>(_tracks.size()) > _capacity)
    {
        _tracks.pop_front();
    }
}

uint32_t BallisticsTrackStore::Begin(const char* ammo, const char* shooter, bool byPlayer, float x, float y, float z,
                                     float velX, float velY, float velZ, bool windActive, float windSpeed,
                                     float windDirRad)
{
    BallisticTrack track;
    track.id = _nextId++;
    if (_nextId == 0)
    {
        _nextId = 1; // ids are never 0; 0 means "no track"
    }
    track.byPlayer = byPlayer;
    track.ammo.Set(ammo);
    track.shooter.Set(shooter);
    track.originX = x;
    track.originY = y;
    track.originZ = z;

    const float speed = std::sqrt(velX * velX + velY * velY + velZ * velZ);
    track.muzzleSpeed = speed;
    if (speed > 1e-6f)
    {
        track.dirX = velX / speed;
        track.dirY = velY / speed;
        track.dirZ = velZ / speed;
    }

    track.windActive = windActive;
    track.windSpeed = windSpeed;
    track.windDirRad = windDirRad;
    track.samples.reserve(32);

    _tracks.push_back(std::move(track));
    EvictToCapacity();
    return _tracks.back().id;
}

BallisticTrack* BallisticsTrackStore::Find(uint32_t id)
{
    if (id == 0)
    {
        return nullptr;
    }
    // Newest first: the caller is almost always appending to a recent track.
    for (auto it = _tracks.rbegin(); it != _tracks.rend(); ++it)
    {
        if (it->id == id)
        {
            return &(*it);
        }
    }
    return nullptr;
}

const BallisticTrack* BallisticsTrackStore::Find(uint32_t id) const
{
    return const_cast<BallisticsTrackStore*>(this)->Find(id);
}

void BallisticsTrackStore::AddSample(uint32_t id, float x, float y, float z, float t)
{
    BallisticTrack* track = Find(id);
    if (!track)
    {
        return;
    }

    // Decimation: keep 1 in `decimation`. The first sample is always kept, so
    // the origin of the drawn trail is the muzzle.
    if (!track->samples.empty())
    {
        if (++track->skipCounter < track->decimation)
        {
            return;
        }
        track->skipCounter = 0;
    }

    track->samples.push_back(BallisticSample{x, y, z, t});

    if (static_cast<int>(track->samples.size()) > _maxSamples)
    {
        // Halve in place: keep every other sample. Shape is preserved across
        // the whole flight rather than truncated, and the cost is O(n) once
        // per doubling — i.e. amortised O(1) per sample.
        std::size_t write = 0;
        for (std::size_t read = 0; read < track->samples.size(); read += 2)
        {
            track->samples[write++] = track->samples[read];
        }
        track->samples.resize(write);
        track->decimation *= 2;
        track->skipCounter = 0;
    }
}

void BallisticsTrackStore::Finish(uint32_t id, BallisticTerminus terminus, const char* hit)
{
    BallisticTrack* track = Find(id);
    if (!track)
    {
        return;
    }
    track->terminus = terminus;
    if (hit)
    {
        track->hit.Set(hit);
    }
}

void BallisticsTrackStore::Clear()
{
    _tracks.clear();
}

int BallisticsTrackStore::TotalSamples() const
{
    int total = 0;
    for (const BallisticTrack& track : _tracks)
    {
        total += static_cast<int>(track.samples.size());
    }
    return total;
}

} // namespace Poseidon::Dev
