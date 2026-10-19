// (OP) Tidewater's humpback whale (@OP_TidewaterWildlife, owner 2026-10-19: "port them over as well").
//
// A port of Tidewater (dgreenheck/tidewater, MIT) src/world/marine/WhaleBrain.js and the rig half of Whale.js:
// * Brain -- WhaleBrain: a closed route through deep water, cruising at 2.6 m/s and ~9.5 m down; once a loop a
//   surfacing sequence of 3-6 breaths (rise, blow, roll back under), about every other time a breach, now and
//   then a flipper raised and slapped down, and a fluke-up dive back to depth. Tidewater's route is drawn by hand
//   through its one bay; here it is a loop of about 700 m placed through the deepest water near where the whale
//   was put. The sequences, springs and constants are Tidewater's.
// * Pose -- Whale._pose: 40 spine frames posed from the brain (the body follows the route behind the head, a
//   travelling stroke wave, the arch before a dive), and the flippers' rotations. Animate moves every vertex as
//   Tidewater's vertex shader does (whalePose): a vertex follows the frames at its rest position along the body;
//   a flipper vertex turns about the flipper root first.
// * Effects -- the blow, the breach landing and the flipper slap go to the Tidewater water's spray as
//   water-interaction events (WaterInteractionBridge.hpp).
// OFP additions: a hit whale sounds (dives away fast), a dead one rolls belly up and floats.
//
// Frames: the brain and the rig work in Tidewater's frame (right handed: +Z the whale's forward, +X its left).
// The world is mirrored in x into it (Tidewater x = -OFP x) and the results mirrored back, so Tidewater's
// numbers carry over unchanged; the model stores Tidewater's whale frame as (-x, y, z) about the memory point
// "tw_origin" (generate_tidewater_whale.py).

#include <Poseidon/World/Entities/Vehicles/Misc/Whale.hpp>

#include <Poseidon/Core/Global.hpp>
#include <Poseidon/Core/Config/EngineConfig.hpp>
#include <Poseidon/Core/Application.hpp>
#include <Poseidon/Graphics/Rendering/Effects/Smokes.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/Graphics/Rendering/WaterInteractionBridge.hpp>
#include <Poseidon/IO/ParamFile/ParamFile.hpp>
#include <Poseidon/World/Entities/Infantry/SwimView.hpp>
#include <Poseidon/World/SeaSoundscape.hpp>
#include <Poseidon/World/Scene/Scene.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/World/World.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <unordered_map>
#include <vector>

namespace Poseidon
{
using namespace Foundation;

namespace
{
constexpr float TAU = 6.28318530718f;
constexpr int MaxFrames = 64;
constexpr float CruiseSpeed = 2.6f; // m/s underwater (WhaleBrain CRUISE_SPEED)
constexpr float SurfaceSpeed = 1.5f;
constexpr float MinDepth = 12.0f; // water the route looks for (Tidewater's route: >= 8.8 m everywhere)

bool WhaleDiag()
{
    static const bool on = std::getenv("POSEIDON_WHALE_DIAG") != nullptr;
    return on;
}

// ---- quaternions (three.js conventions: Euler orders, slerp)
struct Q
{
    float x = 0, y = 0, z = 0, w = 1;
};

Q Mul(const Q& a, const Q& b)
{
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

Q Conj(const Q& q)
{
    return {-q.x, -q.y, -q.z, q.w};
}

float Dot(const Q& a, const Q& b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
}

Q Normalized(const Q& q)
{
    const float l = std::sqrt(Dot(q, q));
    return l > 1e-12f ? Q{q.x / l, q.y / l, q.z / l, q.w / l} : Q{};
}

Q AxisAngle(float ax, float ay, float az, float angle)
{
    const float s = std::sin(angle * 0.5f);
    return {ax * s, ay * s, az * s, std::cos(angle * 0.5f)};
}

Q Slerp(const Q& a, Q b, float t)
{
    if (t <= 0)
        return a;
    if (t >= 1)
        return b;
    float c = Dot(a, b);
    if (c < 0)
    {
        b = {-b.x, -b.y, -b.z, -b.w};
        c = -c;
    }
    if (c >= 1.0f)
        return a;
    const float s2 = 1.0f - c * c;
    if (s2 <= 1e-7f)
    {
        const float u = 1 - t;
        return Normalized({a.x * u + b.x * t, a.y * u + b.y * t, a.z * u + b.z * t, a.w * u + b.w * t});
    }
    const float s = std::sqrt(s2);
    const float h = std::atan2(s, c);
    const float ra = std::sin((1 - t) * h) / s, rb = std::sin(t * h) / s;
    return {a.x * ra + b.x * rb, a.y * ra + b.y * rb, a.z * ra + b.z * rb, a.w * ra + b.w * rb};
}

// three.js Euler 'YXZ' (R = Ry Rx Rz) and 'YZX' (R = Ry Rz Rx)
Q EulerYXZ(float x, float y, float z)
{
    const float c1 = std::cos(x / 2), c2 = std::cos(y / 2), c3 = std::cos(z / 2);
    const float s1 = std::sin(x / 2), s2 = std::sin(y / 2), s3 = std::sin(z / 2);
    return {s1 * c2 * c3 + c1 * s2 * s3, c1 * s2 * c3 - s1 * c2 * s3, c1 * c2 * s3 - s1 * s2 * c3,
            c1 * c2 * c3 + s1 * s2 * s3};
}

Q EulerYZX(float x, float y, float z)
{
    const float c1 = std::cos(x / 2), c2 = std::cos(y / 2), c3 = std::cos(z / 2);
    const float s1 = std::sin(x / 2), s2 = std::sin(y / 2), s3 = std::sin(z / 2);
    return {s1 * c2 * c3 + c1 * s2 * s3, c1 * s2 * c3 + s1 * c2 * s3, c1 * c2 * s3 - s1 * s2 * c3,
            c1 * c2 * c3 - s1 * s2 * s3};
}

Vector3 Rot(const Q& q, Vector3Par v)
{
    const Vector3 u(q.x, q.y, q.z);
    return v + u.CrossProduct(u.CrossProduct(v) + v * q.w) * 2.0f;
}

// Rodrigues
Vector3 RotAxis(Vector3Par ax, float ang, Vector3Par v)
{
    const float c = std::cos(ang), s = std::sin(ang);
    return v * c + ax.CrossProduct(v) * s + ax * (ax.DotProduct(v) * (1 - c));
}

float WrapAngle(float a)
{
    return std::atan2(std::sin(a), std::cos(a));
}

Vector3 Mirror(Vector3Par v)
{
    return Vector3(-v.X(), v.Y(), v.Z());
}

// sea or lake level and the waves at an OFP world point
float WaterAt(float x, float z)
{
    return SwimWaterSurfaceY(x, z);
}

float FloorAt(float twX, float twZ)
{
    return GLandscape ? GLandscape->SurfaceY(-twX, twZ) : -50.0f;
}

struct Key
{
    float t, depth, pitch, arch, follow, speed, stroke;
    bool blow = false;
    bool fluke = false;
    int breach = 0; // 1 launch, 2 air
};

enum
{
    BreachNone,
    BreachLaunch,
    BreachAir
};
} // namespace

// ---- the type's rig (config class TidewaterWhale) and, per level, each vertex's place on it
struct Whale::Rig
{
    bool valid = false;
    int k = 0, root = 0;
    float zHead = 0, dz = 1, notchZ = 0, flipperLength = 4.5f;
    float restY[MaxFrames] = {};
    Vector3 blowhole = VZero, flipperRoot[2] = {VZero, VZero};
    Vector3 origin = VZero; // the model point of Tidewater's whale origin
    // Tidewater's whale frame in model space: model = origin + sign * tw, per axis (y is never turned). Measured
    // from the memory points "blowhole" and "flipper_l", not assumed: this model loads with its snout at model -z,
    // where the boat's generator had measured a half turn (2026-10-19).
    float sx = -1, sz = 1;
    Vector3 ToTw(Vector3Par m) const { return Vector3(sx * m.X(), m.Y(), sz * m.Z()); } // a model offset or direction

    struct Level
    {
        bool built = false;
        std::vector<float> restZ; // the rest position along the body each vertex follows (Tidewater rig.x)
        std::vector<unsigned char> part; // 0 body, 1 left flipper, 2 right flipper
        std::vector<float> flex; // a flipper vertex's distance out from the root / its length
    };
    mutable std::mutex lock;
    mutable std::vector<Level> levels;

    const Level& LevelFor(LODShape* shape, int level) const
    {
        std::lock_guard<std::mutex> g(lock);
        if ((int)levels.size() < shape->NLevels())
            levels.resize(shape->NLevels());
        Level& l = levels[level];
        if (l.built)
            return l;
        l.built = true;
        Shape* s = shape->Level(level);
        if (!s)
            return l;
        s->SaveOriginalPos();
        const int n = s->NPos();
        l.restZ.resize(n);
        l.part.assign(n, 0);
        l.flex.assign(n, 0.0f);
        for (int i = 0; i < n; i++)
            l.restZ[i] = sz * (s->OrigPos(i).Z() - origin.Z());
        const char* names[2] = {"tw_flipper_l", "tw_flipper_r"};
        for (int side = 0; side < 2; side++)
        {
            const int sel = s->FindNamedSel(names[side]);
            if (sel < 0)
                continue;
            const NamedSelection& ns = s->NamedSel(sel);
            for (int j = 0; j < ns.Size(); j++)
            {
                const int i = ns[j];
                if (i < 0 || i >= n)
                    continue;
                l.part[i] = (unsigned char)(side + 1);
                // a flipper rides the spine frame at its root (Tidewater: rig.x is the root's z)
                l.restZ[i] = flipperRoot[side].Z();
                const Vector3 tw = ToTw(s->OrigPos(i) - origin);
                l.flex[i] = std::min(1.0f, (tw - flipperRoot[side]).Size() / flipperLength);
            }
        }
        return l;
    }

    static std::shared_ptr<const Rig> For(const EntityType* type, LODShape* shape)
    {
        static std::mutex mapLock;
        static std::unordered_map<const LODShape*, std::shared_ptr<Rig>> map;
        std::lock_guard<std::mutex> g(mapLock);
        auto& slot = map[shape];
        if (slot)
            return slot;
        slot = std::make_shared<Rig>();
        Rig& r = *slot;
        const ParamEntry* cls = type->GetParamEntry().FindEntry("TidewaterWhale");
        const Shape* memory = shape ? shape->MemoryLevel() : nullptr;
        if (!cls || !cls->IsClass() || !memory)
        {
            LOG_WARN(World, "{}: no class TidewaterWhale or memory level; the whale is not posed",
                     (const char*)type->GetName());
            return slot;
        }
        const RString originName = (*cls) >> "origin";
        const int oi = memory->PointIndex(originName);
        if (oi < 0)
        {
            LOG_WARN(World, "{}: no memory point '{}'", (const char*)type->GetName(), (const char*)originName);
            return slot;
        }
        r.origin = memory->Pos(oi);
        const int bi = memory->PointIndex("blowhole"), fi = memory->PointIndex("flipper_l");
        if (bi >= 0 && fi >= 0)
        {
            const ParamEntry& bh = (*cls) >> "blowhole";
            const ParamEntry& fl = (*cls) >> "flipperLeft";
            r.sz = (memory->Pos(bi).Z() - r.origin.Z()) * float(bh[2]) < 0 ? -1.0f : 1.0f;
            r.sx = (memory->Pos(fi).X() - r.origin.X()) * float(fl[0]) < 0 ? -1.0f : 1.0f;
        }
        LOG_INFO(World, "{}: Tidewater whale frame in the model: x {:+.0f}, z {:+.0f}", (const char*)type->GetName(), r.sx,
                 r.sz);
        const ParamEntry& ry = (*cls) >> "restY";
        r.k = std::min(ry.GetSize(), MaxFrames);
        for (int i = 0; i < r.k; i++)
            r.restY[i] = ry[i];
        r.zHead = (*cls) >> "zHead";
        r.dz = (*cls) >> "dz";
        r.root = std::clamp((int)((*cls) >> "rootIndex"), 0, std::max(r.k - 1, 0));
        r.notchZ = (*cls) >> "notchZ";
        r.flipperLength = (*cls) >> "flipperLength";
        const auto vec = [&](const char* name)
        {
            const ParamEntry& e = (*cls) >> name;
            return Vector3(float(e[0]), float(e[1]), float(e[2]));
        };
        r.blowhole = vec("blowhole");
        r.flipperRoot[0] = vec("flipperLeft");
        r.flipperRoot[1] = vec("flipperRight");
        r.valid = r.k >= 4 && r.dz > 0;
        return slot;
    }
};

// ---- what Animate draws: the frames relative to the root (Tidewater's whale frame)
struct Whale::Pose
{
    bool valid = false;
    Vector3 pos[MaxFrames];
    Q rot[MaxFrames];
    Vector3 flipAxis[2] = {Vector3(1, 0, 0), Vector3(1, 0, 0)};
    float flipAngle[2] = {0, 0};
};

// ---- WhaleBrain.js
struct Whale::Brain
{
    const Rig* rig = nullptr;
    // route (Tidewater frame: x mirrored), every 0.25 m
    std::vector<float> rx, rz, ryaw;
    int rN = 0;
    double length = 1;
    float yawWrap = 0;
    float surfaceFrac = 0.25f;

    double u = 0;
    Vector3 position = VZero;
    Q quat;
    float speed = CruiseSpeed, yaw = 0, pitch = 0, roll = 0, rollV = 0, breachRoll = 0, bob = 0;
    float y = -9, vy = 0, water = 0;
    float strokePhase = 0, strokeAmp = 0.13f, arch = 0, headPitch = 0, follow = 0.85f;
    float blow = 0, flukeUp = 0;
    struct Fin
    {
        float sweep = 0, lift = 0, twist = 0, vs = 0, vl = 0, vt = 0;
    } fin[2];
    bool slapOn = false, slapHit = false;
    int slapSide = 0;
    float slapT = 0, slapTimer = 12;
    int slaps = 0, breaches = 0, splashes = 0, blows = 0;
    float yawRate = 0, time = 0;

    static constexpr int HN = 2048;
    std::vector<double> hArc = std::vector<double>(HN, 0.0);
    std::vector<Q> hQ = std::vector<Q>(HN);
    int hHead = -1, hCount = 0;
    double arc = 0;

    std::vector<Key> keys;
    int ki = 0;
    float kt = 0;
    bool seq = false, blown = false, splashed = false, lastWrap = false;
    bool dead = false;
    uint32_t seed = 1;

    // posed frames (Tidewater frame, world)
    Vector3 pos[MaxFrames];
    Q rot[MaxFrames];
    Q flip[2];

    float Rand()
    {
        // mulberry32
        uint32_t a = (seed += 0x6D2B79F5u);
        uint32_t t = (a ^ (a >> 15)) * (1u | a);
        t = (t + ((t ^ (t >> 7)) * (61u | t))) ^ t;
        return float((t ^ (t >> 14)) >> 0) / 4294967296.0f;
    }

    // -- route
    void RouteIndex(double uu, int& i, float& t, int& loops) const
    {
        loops = (int)std::floor(uu / length);
        const double f = (uu - loops * length) / length * rN;
        i = std::min((int)std::floor(f), rN - 1);
        t = float(f - i);
    }
    void RouteAt(double uu, float& x, float& z) const
    {
        int i, loops;
        float t;
        RouteIndex(uu, i, t, loops);
        x = rx[i] + (rx[i + 1] - rx[i]) * t;
        z = rz[i] + (rz[i + 1] - rz[i]) * t;
    }
    float RouteYaw(double uu) const
    {
        float s = 0;
        for (const float o : {-2.0f, -1.0f, 0.0f, 1.0f, 2.0f})
        {
            int i, loops;
            float t;
            RouteIndex(uu + o, i, t, loops);
            s += ryaw[i] + (ryaw[i + 1] - ryaw[i]) * t + loops * yawWrap;
        }
        return s / 5;
    }

    // a closed uniform Catmull-Rom through `pts`, resampled every 0.25 m
    void BuildRoute(const std::vector<std::pair<float, float>>& pts)
    {
        std::vector<float> px, pz, cum;
        const int n = (int)pts.size();
        for (int s = 0; s < n; s++)
        {
            const auto& p0 = pts[(s + n - 1) % n];
            const auto& p1 = pts[s];
            const auto& p2 = pts[(s + 1) % n];
            const auto& p3 = pts[(s + 2) % n];
            for (int j = 0; j < 64; j++)
            {
                const float t = j / 64.0f, t2 = t * t, t3 = t2 * t;
                const auto cr = [&](float a, float b, float c, float d)
                { return 0.5f * (2 * b + (-a + c) * t + (2 * a - 5 * b + 4 * c - d) * t2 + (-a + 3 * b - 3 * c + d) * t3); };
                px.push_back(cr(p0.first, p1.first, p2.first, p3.first));
                pz.push_back(cr(p0.second, p1.second, p2.second, p3.second));
            }
        }
        px.push_back(px[0]);
        pz.push_back(pz[0]);
        cum.assign(px.size(), 0.0f);
        for (size_t i = 1; i < px.size(); i++)
            cum[i] = cum[i - 1] + std::hypot(px[i] - px[i - 1], pz[i] - pz[i - 1]);
        length = std::max(1.0, (double)cum.back());
        rN = std::max(8, (int)std::ceil(length / 0.25));
        rx.assign(rN + 1, 0);
        rz.assign(rN + 1, 0);
        ryaw.assign(rN + 1, 0);
        size_t seg = 0;
        for (int i = 0; i <= rN; i++)
        {
            const float d = float(length * i / rN);
            while (seg + 2 < px.size() && cum[seg + 1] < d)
                seg++;
            const float span = std::max(cum[seg + 1] - cum[seg], 1e-6f);
            const float t = std::clamp((d - cum[seg]) / span, 0.0f, 1.0f);
            rx[i] = px[seg] + (px[seg + 1] - px[seg]) * t;
            rz[i] = pz[seg] + (pz[seg + 1] - pz[seg]) * t;
        }
        float prev = 0;
        for (int i = 0; i <= rN; i++)
        {
            const int a = std::max(i - 1, 0), b = std::min(i + 1, rN);
            float yw = std::atan2(rx[b] - rx[a], rz[b] - rz[a]);
            if (i > 0)
                yw = prev + WrapAngle(yw - prev);
            ryaw[i] = prev = yw;
        }
        yawWrap = ryaw[rN] - ryaw[0];
    }

    // the least water under a loop of radius r about (cx, cz) (Tidewater frame)
    static float LoopDepth(float cx, float cz, float r, float sea)
    {
        float least = 1e9f;
        const int steps = std::max(24, (int)(TAU * r / 6.0f));
        for (int i = 0; i < steps; i++)
        {
            const float a = TAU * i / steps;
            least = std::min(least, sea - FloorAt(cx + r * std::sin(a), cz + r * std::cos(a)));
            if (least < MinDepth)
                break;
        }
        return least;
    }

    // Tidewater's route is its bay's; here: the loop through the deepest water near the spawn point
    void Plan(float twX, float twZ)
    {
        const float sea = GLandscape ? GLandscape->GetSeaLevel() : 0.0f;
        float bestX = twX, bestZ = twZ, bestR = 60.0f, bestDepth = -1e9f;
        bool found = false;
        // first a loop through the spawn point itself (the whale shows up where it was put), then the deepest
        // loop anywhere near
        for (const float r : {120.0f, 95.0f, 70.0f, 50.0f})
        {
            for (int k = 0; k < 8 && !found; k++)
            {
                const float a = TAU * k / 8;
                const float cx = twX + r * std::sin(a), cz = twZ + r * std::cos(a);
                const float depth = LoopDepth(cx, cz, r, sea);
                if (depth > bestDepth)
                {
                    bestDepth = depth;
                    bestX = cx;
                    bestZ = cz;
                    bestR = r;
                }
                found = depth >= MinDepth;
            }
            if (found)
                break;
        }
        for (const float r : {120.0f, 95.0f, 70.0f, 50.0f})
        {
            if (found)
                break;
            for (float d = 0; d <= 360.0f && !found; d += 60.0f)
            {
                for (int k = 0; k < (d > 0 ? 8 : 1) && !found; k++)
                {
                    const float a = TAU * k / 8;
                    const float cx = twX + d * std::sin(a), cz = twZ + d * std::cos(a);
                    const float depth = LoopDepth(cx, cz, r, sea);
                    if (depth > bestDepth)
                    {
                        bestDepth = depth;
                        bestX = cx;
                        bestZ = cz;
                        bestR = r;
                    }
                    found = depth >= MinDepth;
                }
            }
            if (found)
                break;
        }
        // nine points around the loop, a little irregular (Tidewater's route is drawn by hand)
        std::vector<std::pair<float, float>> pts;
        const float phase = Rand() * TAU;
        for (int i = 0; i < 9; i++)
        {
            const float a = TAU * i / 9;
            const float r = bestR * (1.0f + 0.12f * std::sin(3 * a + phase));
            pts.emplace_back(bestX + r * std::sin(a), bestZ + r * std::cos(a));
        }
        BuildRoute(pts);
        // start on the loop where it passes nearest the spawn point
        double nearest = 0;
        float best = 1e18f;
        for (int i = 0; i < rN; i++)
        {
            const float d = (rx[i] - twX) * (rx[i] - twX) + (rz[i] - twZ) * (rz[i] - twZ);
            if (d < best)
            {
                best = d;
                nearest = length * i / rN;
            }
        }
        u = nearest;
        // the loop's surfacing point: half a loop on (the first sequence starts at once, see Start)
        surfaceFrac = float(std::fmod(u / length + 0.5, 1.0));
        LOG_INFO(World, "Whale: loop of {:.0f} m about ({:.0f}, {:.0f}), least water {:.1f} m{}", length, -bestX, bestZ,
                 bestDepth, found ? "" : " (no loop with 12 m of water found: the shallowest is used)");
    }

    void Record(double a)
    {
        hHead = (hHead + 1) % HN;
        hArc[hHead] = a;
        hQ[hHead] = quat;
        hCount = std::min(hCount + 1, HN);
    }

    Q PathRotation(float d) const
    {
        const double target = arc - d;
        const auto at = [&](int k) { return hArc[(hHead - k + HN) % HN]; };
        int lo = 0, hi = hCount - 1;
        if (target >= at(0))
            return hQ[hHead];
        if (target <= at(hi))
            return hQ[(hHead - hi + HN) % HN];
        while (hi - lo > 1)
        {
            const int m = (lo + hi) >> 1;
            if (at(m) > target)
                lo = m;
            else
                hi = m;
        }
        const double a0 = at(lo), a1 = at(hi);
        const float t = float((a0 - target) / std::max(a0 - a1, 1e-9));
        return Slerp(hQ[(hHead - lo + HN) % HN], hQ[(hHead - hi + HN) % HN], t);
    }

    Q FlipperRotation(int side) const
    {
        const float sg = side == 0 ? 1.0f : -1.0f;
        const Fin& f = fin[side];
        return EulerYZX(f.twist, sg * (0.12f + f.sweep), sg * (-0.28f + f.lift));
    }

    void Flippers(float dt, const Key& target)
    {
        const bool surface = seq && water - y < 2.5f;
        const float turn = std::clamp(yawRate * 25, -1.0f, 1.0f);
        const float row = time * TAU / 11;
        const bool air = target.breach == BreachAir;
        if (surface && water - y < 1.35f && !slapOn && !air && !dead)
        {
            slapTimer -= dt;
            if (slapTimer < 0)
            {
                slapOn = true;
                slapHit = false;
                slapSide = Rand() < 0.5f ? 0 : 1;
                slapT = 0;
                slapTimer = 18 + Rand() * 25;
            }
        }
        for (int s = 0; s < 2; s++)
        {
            Fin& f = fin[s];
            const float sg = s == 0 ? 1.0f : -1.0f;
            const float ph = row + s * 0.35f;
            float sweep = 0.18f * std::sin(ph) + 0.1f + (surface ? 0.12f : 0.0f);
            float lift = 0.1f * std::sin(ph + 0.8f) - sg * turn * 0.45f + (surface ? 0.08f : 0.0f);
            float twist = 0.3f * std::cos(ph) + pitch * 0.6f;
            sweep += std::max(0.0f, sg * turn) * 0.25f;
            float stiff = 0.45f;
            if (dead)
            {
                // limp, spread a little
                sweep = 0.2f;
                lift = 0.35f;
                twist = 0;
                stiff = 0.3f;
            }
            else if (air)
            {
                sweep = 0.45f;
                lift = 0.7f;
                twist = 0.3f;
                stiff = 2.5f;
            }
            else if (slapOn && slapSide == s)
            {
                if (slapT < 3.5f)
                {
                    lift = 1.55f;
                    sweep = 0.35f;
                    twist = -0.2f;
                    stiff = 0.9f;
                }
                else
                {
                    lift = -0.45f;
                    twist = 0.35f;
                    stiff = 9;
                    if (slapT > 3.8f && !slapHit)
                    {
                        slapHit = true;
                        slaps++;
                    }
                }
            }
            const float k = stiff, c = 2 * std::sqrt(stiff) * 0.9f;
            f.vs += ((sweep - f.sweep) * k - f.vs * c) * dt;
            f.vl += ((lift - f.lift) * k - f.vl * c) * dt;
            f.vt += ((twist - f.twist) * k - f.vt * c) * dt;
            f.sweep += f.vs * dt;
            f.lift += f.vl * dt;
            f.twist += f.vt * dt;
        }
        if (slapOn)
        {
            slapT += dt;
            if (slapT > 6)
                slapOn = false;
        }
    }

    void Place(float dt)
    {
        float px, pz;
        RouteAt(u, px, pz);
        const float yaw0 = yaw;
        yaw = RouteYaw(u);
        if (dt > 0)
            yawRate += (WrapAngle(yaw - yaw0) / dt - yawRate) * std::min(1.0f, dt * 1.2f);
        const float curv = (RouteYaw(u + 3) - RouteYaw(u - 3)) / 6;
        const float rollT = std::clamp(-curv * speed * 1.6f, -0.22f, 0.22f);
        rollV += ((rollT - roll) * 0.6f - rollV * 1.4f) * dt;
        roll += rollV * dt;
        position = Vector3(px, y, pz);
        quat = EulerYXZ(-(pitch + bob), yaw, roll + breachRoll);
    }

    // Surfacing sequence: keyframes of (duration, root depth below the water, pitch, arch, follow, speed, stroke)
    void StartSequence()
    {
        // (POSEIDON_WHALE_BREATHS=<n>: a test sets the number of breaths, to reach the breach sooner)
        static const int breaths = []
        {
            const char* v = std::getenv("POSEIDON_WHALE_BREATHS");
            return v ? std::max(1, std::atoi(v)) : 0;
        }();
        const int n = breaths ? breaths : 3 + (int)std::floor(Rand() * 4);
        keys.clear();
        keys.push_back({9, 1.3f, 0.08f, 0, 0.85f, SurfaceSpeed, 0.07f});
        for (int i = 0; i < n; i++)
        {
            Key b{3.5f, 1.12f, 0.1f, 0.02f, 0.85f, SurfaceSpeed, 0.03f};
            b.blow = true;
            keys.push_back(b);
            if (i < n - 1)
            {
                keys.push_back({4, 2.2f, -0.14f, -0.05f, 0.92f, SurfaceSpeed, 0.04f});
                keys.push_back({7 + Rand() * 6, 3.4f, 0, 0, 0.85f, SurfaceSpeed, 0.06f});
                keys.push_back({5, 1.3f, 0.08f, 0, 0.85f, SurfaceSpeed, 0.06f});
            }
        }
        static const bool forceBreach = std::getenv("POSEIDON_WHALE_BREACH") != nullptr;
        if (Rand() < (forceBreach ? 1.0f : 0.5f))
        {
            keys.push_back({7, 9, -0.2f, 0, 0.6f, 2.4f, 0.1f});
            Key launch{6, 9, 0.45f, 0, 0.3f, 3.5f, 0.14f};
            launch.breach = BreachLaunch;
            keys.push_back(launch);
            Key airborne{6, 2, 0, 0, 0.3f, 2.5f, 0.05f};
            airborne.breach = BreachAir;
            keys.push_back(airborne);
            keys.push_back({6, 2.4f, 0.05f, 0, 0.85f, SurfaceSpeed, 0.05f});
        }
        keys.push_back({3, 1.25f, -0.12f, -0.14f, 0.85f, 1.8f, 0.02f});
        Key dive{4.5f, 5.5f, -0.95f, -0.12f, 0.12f, 1.9f, 0.0f};
        dive.fluke = true;
        keys.push_back(dive);
        keys.push_back({6, 11, -0.45f, 0, 0.6f, 2.2f, 0.08f});
        keys.push_back({8, 10, 0, 0, 0.85f, CruiseSpeed, 0.1f});
        ki = 0;
        kt = 0;
        blown = splashed = false;
        seq = true;
    }

    // (OP) shot at: a fluke-up dive and away, fast and deep, for a while
    void Sound()
    {
        keys.clear();
        Key dive{4, 6, -0.9f, -0.1f, 0.2f, 3.0f, 0.12f};
        dive.fluke = water - y < 3.0f;
        keys.push_back(dive);
        keys.push_back({40, 22, -0.2f, 0, 0.75f, 5.0f, 0.22f});
        keys.push_back({10, 10, 0, 0, 0.85f, CruiseSpeed, 0.1f});
        ki = 0;
        kt = 0;
        blown = splashed = false;
        seq = true;
        slapOn = false;
    }

    void NextKey(int expect)
    {
        if (!seq || ki + 1 >= (int)keys.size() || keys[ki + 1].breach != expect)
            return;
        ki++;
        kt = 0;
        breaches++;
    }

    void Update(float dt)
    {
        dt = std::min(dt, 0.1f);
        time += dt;
        // the water at the head (OFP world: x mirrored back)
        {
            const float hx = position.X() + std::sin(yaw) * 3, hz = position.Z() + std::cos(yaw) * 3;
            const float h = WaterAt(-hx, hz);
            if (std::isfinite(h))
                water += (h - water) * std::min(1.0f, dt * 4);
        }

        // ---- sequencer
        const float frac = float(std::fmod(std::fmod(u / length, 1.0) + 1.0, 1.0));
        float rel = frac - surfaceFrac;
        rel -= std::floor(rel); // 0..1 past the surfacing point
        if (!dead && !seq && rel < 0.05f && !lastWrap)
        {
            StartSequence();
            lastWrap = true;
        }
        if (rel > 0.2f && rel < 0.8f)
            lastWrap = false;
        Key target{0, 9.5f, 0, 0, 0.85f, CruiseSpeed, 0.13f};
        blow = 0;
        flukeUp = 0;
        if (dead)
        {
            seq = false;
            // belly up at the surface, floating high (a carcass bloats): the belly a metre out of the water
            // (follow 0: the body rigid with the root -- stopped, the path behind it holds the living whale's
            // orientation, and only the head rolled over)
            target = Key{0, 0.5f, 0, 0, 0.0f, 0.0f, 0.0f};
        }
        else if (seq)
        {
            const Key& key = keys[ki];
            kt += dt;
            target = key;
            if (key.blow)
            {
                const float tb = kt - 0.6f;
                blow = tb > 0 && tb < 1.4f ? std::sqrt(std::sin(3.14159265f * std::min(tb / 1.4f, 1.0f))) : 0.0f;
                if (tb > 0 && !blown)
                {
                    blown = true;
                    blows++;
                }
            }
            if (key.fluke)
                flukeUp = std::min(1.0f, kt / key.t);
            if (kt >= key.t)
            {
                ki++;
                kt = 0;
                blown = false;
                if (ki >= (int)keys.size())
                {
                    seq = false;
                    target = keys.back();
                }
            }
        }

        // ---- depth: critically damped toward the target, never near the seafloor
        const float x = position.X(), z = position.Z();
        const float sy = std::sin(yaw), cy = std::cos(yaw);
        float floor = -1e9f;
        for (const float d : {-9.0f, -5.0f, 0.0f, 5.0f, 10.0f, 16.0f, 24.0f})
            floor = std::max(floor, FloorAt(x + sy * d, z + cy * d));
        const float yT = std::max(water - target.depth, floor + 5.2f);
        const float w = target.fluke ? 1.0f : 0.7f;
        float ay = (yT - y) * w * w - 2 * w * vy;
        if (target.breach == BreachLaunch)
        {
            ay = 14;
            if (y > water - 2.5f)
                NextKey(BreachAir);
        }
        else if (target.breach == BreachAir)
        {
            ay = vy > 0 || y > water - 1 ? -9.81f : (yT - y) * 0.5f - 1.5f * vy;
            breachRoll += (1.9f - breachRoll) * std::min(1.0f, dt * 1.2f);
            if (vy < 0 && y < water - 1.5f && !splashed)
            {
                splashed = true;
                splashes++;
            }
        }
        if (dead)
            breachRoll += (3.14159265f - breachRoll) * std::min(1.0f, dt * 0.12f);
        else if (target.breach != BreachAir)
            breachRoll += (0 - breachRoll) * std::min(1.0f, dt * 0.6f);
        vy += ay * dt;
        y += vy * dt;
        const float hard = floor + 4.6f;
        if (y < hard)
        {
            y = hard;
            vy = std::max(vy, 0.0f);
        }
        const float k = std::min(1.0f, dt * 1.2f);
        speed += (target.speed - speed) * std::min(1.0f, dt * 0.5f);
        // (a dead whale lies level: riding the waves, its heave pitched it up and down by 0.3 rad)
        const float pitchT = dead ? 0.0f : std::atan2(vy, std::max(speed, 0.5f)) * 0.8f + target.pitch;
        pitch += (pitchT - pitch) * std::min(1.0f, dt * (target.breach ? 2.5f : target.fluke ? 1.1f : 0.9f));
        arch += (target.arch - arch) * k;
        follow += (target.follow - follow) * std::min(1.0f, dt * (target.fluke ? 1.5f : 0.8f));
        strokeAmp += (target.stroke - strokeAmp) * std::min(1.0f, dt * 0.6f);
        headPitch += ((target.blow ? 0.03f : 0.0f) - headPitch) * k;
        if (!dead)
            strokePhase += TAU * (0.09f + 0.042f * speed) * dt;
        bob = -strokeAmp * 0.12f * std::sin(strokePhase + 0.5f);
        Flippers(dt, target);

        const float du = speed * std::cos(pitch) * dt;
        u += du;
        Place(dt);
        arc += du;
        Record(arc);
    }

    // Whale._pose: the spine frames from the brain's state
    void PoseFrames()
    {
        const int K = rig->k, ri = rig->root;
        const Vector3 xAxis(1, 0, 0);
        for (int f = 0; f < K; f++)
        {
            const float zr = rig->zHead - f * rig->dz;
            Q q;
            if (zr >= 0)
            {
                const float recoil = -strokeAmp * 0.1f * std::sin(strokePhase + 0.6f) * std::min(1.0f, zr / 5);
                q = Mul(quat, AxisAngle(1, 0, 0, recoil + headPitch * std::min(1.0f, zr / 4)));
            }
            else
            {
                const float d = -zr;
                q = Slerp(quat, PathRotation(d), follow);
                const float L = 9.6f;
                const float uu = std::min(d / L, 1.15f);
                const float env = uu * uu * 1.1f + 0.05f * uu;
                const float ph = strokePhase - d * 0.52f;
                float beta = strokeAmp * env * (std::sin(ph) + 0.22f * std::sin(2 * ph));
                if (d > 7.6f)
                    beta += strokeAmp * 2.0f * std::sin(ph + 1.35f) * std::min(1.0f, (d - 7.6f) / 1.2f);
                beta += arch * 3.0f * std::min(d / 6, 1.0f);
                q = Mul(q, AxisAngle(1, 0, 0, beta));
            }
            rot[f] = q;
        }
        const auto restZ = [&](int f) { return rig->zHead - f * rig->dz; };
        pos[ri] = position + Rot(rot[ri], Vector3(0, rig->restY[ri], restZ(ri)));
        for (int f = ri - 1; f >= 0; f--)
        {
            const Q h = Slerp(rot[f], rot[f + 1], 0.5f);
            pos[f] = pos[f + 1] + Rot(h, Vector3(0, rig->restY[f] - rig->restY[f + 1], restZ(f) - restZ(f + 1)));
        }
        for (int f = ri + 1; f < K; f++)
        {
            const Q h = Slerp(rot[f], rot[f - 1], 0.5f);
            pos[f] = pos[f - 1] + Rot(h, Vector3(0, rig->restY[f] - rig->restY[f - 1], restZ(f) - restZ(f - 1)));
        }
        for (int s = 0; s < 2; s++)
            flip[s] = FlipperRotation(s);
    }

    // world position (Tidewater frame) of a rest-frame point: the CPU copy of the vertex pose
    Vector3 ToWorld(Vector3Par local) const
    {
        const int K = rig->k;
        const float fi = std::clamp((rig->zHead - local.Z()) / rig->dz, 0.0f, K - 1.001f);
        const int i = (int)std::floor(fi);
        const float t = fi - i;
        const float yc = rig->restY[i] + (rig->restY[i + 1] - rig->restY[i]) * t;
        const Q q = Slerp(rot[i], rot[i + 1], t);
        return pos[i] + (pos[i + 1] - pos[i]) * t + Rot(q, Vector3(local.X(), local.Y() - yc, 0));
    }

    // the depth of the top of the back below the water
    float BackDepth() const
    {
        return water - (y + 1.25f);
    }
};

// ---- the entity

DEFINE_CASTING(Whale)

Whale::Whale(VehicleType* type) : base(type), _pose(std::make_unique<Pose>())
{
    _isStopped = false;
    _landContact = false;
    _objectContact = false;
    // Entity::SimulateOptimized steps by exactly this (0 handed Simulate no time at all)
    SetSimulationPrecision(1.0f / 30.0f);
}

Whale::~Whale() = default;

void Whale::Start()
{
    _rig = Rig::For(GetType(), _shape);
    _brain = std::make_unique<Brain>();
    Brain& b = *_brain;
    b.rig = _rig.get();
    b.seed = (uint32_t)(Position().X() * 13.0f + Position().Z() * 7.0f) | 1u;
    // the whale's origin where it was placed (Tidewater frame: x mirrored)
    const Vector3 at = Position() + DirectionModelToWorld(_rig->origin);
    b.Plan(-at.X(), at.Z());
    b.water = WaterAt(at.X(), at.Z());
    // where it was put, a little under: and it comes up to breathe at once (Zeus spawns it to be seen)
    b.y = std::max(b.water - 6.0f, FloorAt(-at.X(), at.Z()) + 4.6f);
    b.Place(0);
    for (int i = 60; i >= 0; i--)
        b.Record(-i * 0.4);
    b.StartSequence();
    b.lastWrap = true;
    _lastDammage = GetTotalDammage();
    LOG_INFO(World, "Whale {} placed at {:.0f}, {:.0f}: water {:.1f} m deep", (const char*)GetDebugName(), at.X(),
             at.Z(), b.water - FloorAt(-at.X(), at.Z()));
}

float Whale::WaterLevel() const
{
    return _brain ? _brain->water : (GLandscape ? GLandscape->GetSeaLevel() : 0.0f);
}

void Whale::Bleed(Vector3Par at, float amount)
{
    if (!ENGINE_CONFIG.blood || amount <= 0)
        return;
    HydroWaterInteractionEvent e{};
    e.positionRadius[0] = at.X();
    e.positionRadius[1] = at.Z();
    e.positionRadius[3] = WaterLevel();
    e.velocityKind[3] = HydroWaterInteractionBlood;
    e.timeLifeFoamMass[3] = amount;
    SubmitWaterInteraction(e);
}

// An explosion tears pieces of skin, blubber and meat off and throws them (config chunks[], the stock explosion
// debris' simulation: they splash down and sink)
void Whale::TearChunks(Vector3Par at, float val)
{
    const ParamEntry* list = GetType()->GetParamEntry().FindEntry("chunks");
    if (!list || list->GetSize() <= 0 || !ENGINE_CONFIG.blood)
        return;
    const int n = std::clamp((int)(val / 40.0f), 1, 10);
    for (int i = 0; i < n; i++)
    {
        const RString name = (*list)[(int)(GRandGen.RandomValue() * list->GetSize()) % list->GetSize()];
        VehicleType* type = dynamic_cast<VehicleType*>(VehicleTypes.New(name));
        if (!type)
            continue;
        const float a = GRandGen.RandomValue() * TAU;
        const Vector3 out(std::sin(a), 0, std::cos(a));
        Matrix4 trans;
        trans.SetOrientation(Matrix3(MRotationY, GRandGen.RandomValue() * TAU));
        // from the wound, above the water
        trans.SetPosition(at + out * 0.8f + VUp * (std::max(WaterLevel() - at.Y(), 0.0f) + 0.6f));
        const Vector3 vel = out * (2.0f + 5.0f * GRandGen.RandomValue()) + VUp * (5.0f + 8.0f * GRandGen.RandomValue());
        CreateThing(type, trans, vel);
    }
}

void Whale::DoDammage(EntityAI* owner, Vector3Par pos, float val, float valRange, RString ammo)
{
    base::DoDammage(owner, pos, val, valRange, ammo);
    if (val <= 0)
        return;
    const Vector3 at = PositionModelToWorld(pos);
    // (val arrives divided by the armor: the ammo's hit, scaled by distance for a blast)
    const float hit = val * GetArmor();
    // a rifle round (hit ~8) bleeds a little, a shell's blast a lot; the wounds go on bleeding
    const float amount = std::clamp(hit / 25.0f, 0.2f, 3.0f);
    Bleed(at, amount);
    _bleed = std::min(_bleed + amount * 0.12f, 1.0f);
    // where the hit is out of the water: OFP's spurt of blood
    if (ENGINE_CONFIG.blood && at.Y() > WaterLevel() - 0.2f)
    {
        Crater* spurt = new Crater(nullptr, VehicleTypes.New("crater"), 1.0f, 0.3f, false, true, false);
        Matrix4 trans(MIdentity);
        trans.SetPosition(at);
        spurt->SetTransform(trans);
        GLOB_WORLD->AddAnimal(spurt);
    }
    // a blast (indirect hit) of a shell or more tears it open
    if (valRange > 0 && hit >= 40.0f)
        TearChunks(at, hit);
    if (WhaleDiag())
        LOG_INFO(World, "Whale hit: {} val {:.1f} range {:.1f} at {:.1f},{:.1f},{:.1f} (water {:.2f}): blood {:.2f}",
                 (const char*)ammo, hit, valRange, at.X(), at.Y(), at.Z(), WaterLevel(), amount);
}

void Whale::HitBy(EntityAI* owner, float howMuch, RString ammo)
{
    base::HitBy(owner, howMuch, ammo);
    if (_brain && !_brain->dead && howMuch > 0)
        _brain->Sound();
}

void Whale::Publish()
{
    const Brain& b = *_brain;
    const int K = _rig->k;
    // the frames relative to the root: the entity carries the root's position and orientation
    const Q inv = Conj(b.quat);
    std::lock_guard<std::mutex> g(_poseLock);
    Pose& p = *_pose;
    for (int f = 0; f < K; f++)
    {
        p.pos[f] = Rot(inv, b.pos[f] - b.position);
        Q r = Mul(inv, b.rot[f]);
        // keep the hemisphere continuous along the chain (Animate interpolates neighbours)
        if (f > 0 && Dot(r, p.rot[f - 1]) < 0)
            r = {-r.x, -r.y, -r.z, -r.w};
        p.rot[f] = r;
    }
    for (int s = 0; s < 2; s++)
    {
        const Q& q = b.flip[s];
        const float w = std::min(1.0f, std::fabs(q.w));
        const float ang = 2 * std::acos(w);
        const float sn = std::sqrt(std::max(1e-12f, 1 - q.w * q.w));
        const float sg = q.w < 0 ? -1.0f : 1.0f;
        if (ang < 1e-5f)
        {
            p.flipAxis[s] = Vector3(1, 0, 0);
            p.flipAngle[s] = 0;
        }
        else
        {
            p.flipAxis[s] = Vector3(q.x / sn * sg, q.y / sn * sg, q.z / sn * sg);
            p.flipAngle[s] = ang;
        }
    }
    p.valid = true;
}

namespace
{
void Splash(Vector3Par at, float surface, float hit, float range, float radius)
{
    HydroWaterInteractionEvent e{};
    e.positionRadius[0] = at.X();
    e.positionRadius[1] = at.Z();
    e.positionRadius[2] = radius;
    e.positionRadius[3] = surface;
    e.velocityKind[2] = -6.0f;
    e.velocityKind[3] = HydroWaterInteractionExplosion;
    e.timeLifeFoamMass[1] = 1.8f;
    e.timeLifeFoamMass[2] = 1.0f;
    e.timeLifeFoamMass[3] = hit;
    e.directionDepthFlags[2] = range;
    e.directionDepthFlags[3] = HydroWaterInteractionPendingImpulse;
    SubmitWaterInteraction(e);
}
} // namespace

void Whale::Effects(float deltaT)
{
    const Brain& b = *_brain;
    // the blow: a column of spray and mist from the blowholes when they clear the water (Whale._effects)
    if (b.blow > 0 && !_blowSent)
    {
        const Vector3 p = Mirror(b.ToWorld(_rig->blowhole));
        if (p.Y() > b.water - 0.3f)
        {
            _blowSent = true;
            HydroWaterInteractionEvent e{};
            e.positionRadius[0] = p.X();
            e.positionRadius[1] = p.Z();
            e.positionRadius[2] = 0.45f;
            e.positionRadius[3] = std::max(p.Y(), b.water) + 0.15f;
            // Tidewater's column leaves at 7.5-10 m/s: about 4 m high; its direction: forward with the whale
            e.velocityKind[0] = -std::sin(b.yaw) * 0.8f;
            e.velocityKind[1] = std::cos(b.yaw) * 0.8f;
            e.velocityKind[3] = HydroWaterInteractionWhaleBlow;
            e.timeLifeFoamMass[3] = 4.2f;
            SubmitWaterInteraction(e);
            // (Tidewater: a cetacean's breath pitched down as the humpback's)
            if (Heard(p))
                PlaySeaSound("whale_blow", p + VUp * 0.6f, VZero, (GRandGen.RandomValue() - 0.5f) * 3,
                             0.72f + GRandGen.RandomValue() * 0.12f);
            if (WhaleDiag())
                LOG_INFO(World, "Whale blow at {:.1f},{:.1f},{:.1f} (water {:.2f})", p.X(), p.Y(), p.Z(), b.water);
        }
    }
    if (b.blow <= 0)
        _blowSent = false;
    // a flipper slap: a sheet of spray where it comes down
    if (b.slaps != _slapsSeen)
    {
        _slapsSeen = b.slaps;
        const Vector3 r = _rig->flipperRoot[b.slapSide];
        Vector3 p = Mirror(b.ToWorld(Vector3(r.X() + (b.slapSide == 0 ? 3.2f : -3.2f), r.Y(), r.Z() - 1.2f)));
        Splash(p, b.water, 6.0f, 2.0f, 2.5f);
        if (Heard(p))
            PlaySeaSound("big_splash", p, VZero, -8, 0.8f + GRandGen.RandomValue() * 0.1f);
    }
    // a breach: bursting clear of the surface, then coming down on its side (Tidewater's sounds for both)
    const Vector3 root = Mirror(b.position);
    const Vector3 surface(root.X(), b.water, root.Z());
    if (b.breaches != _breachesSeen)
    {
        _breachesSeen = b.breaches;
        if (Heard(root))
            PlaySeaSound("big_splash", surface + VUp, VZero, -6, 0.6f + GRandGen.RandomValue() * 0.1f);
    }
    if (b.splashes != _splashesSeen)
    {
        _splashesSeen = b.splashes;
        Splash(root, b.water, 90.0f, 4.0f, 6.0f);
        if (Heard(root))
        {
            PlaySeaSound("big_splash", surface + VUp * 0.5f, VZero, 0, 0.5f + GRandGen.RandomValue() * 0.08f);
            PlaySeaSound("surf_crash", surface + VUp * 0.5f, VZero, 0, 0.72f + GRandGen.RandomValue() * 0.08f);
        }
    }
    // the fluke-up dive: water sheeting off the raised flukes, then the flukes slipping under
    const Vector3 tail = Mirror(b.ToWorld(Vector3(0, 0, _rig->notchZ)));
    if (b.flukeUp > 0.05f && _flukeSeen <= 0.05f && Heard(tail))
        PlaySeaSound("emerge", Vector3(tail.X(), b.water + 2, tail.Z()), VZero, 0, 0.7f + GRandGen.RandomValue() * 0.1f);
    if (b.flukeUp == 0 && _flukeSeen > 0.8f && Heard(tail))
        PlaySeaSound("big_splash", Vector3(tail.X(), b.water, tail.Z()), VZero, -11, 0.78f + GRandGen.RandomValue() * 0.1f);
    _flukeSeen = b.flukeUp;
}

// Tidewater plays the whale's one-shots only within 100 m of it
bool Whale::Heard(Vector3Par at) const
{
    return GScene && GScene->GetCamera() && GScene->GetCamera()->Position().Distance2(at) < Square(100.0f);
}

// Its song: sung at depth while cruising (it stops to breathe), clear underwater, faint and dull from above, and only
// near the whale (Tidewater's SoundScape._whale)
void Whale::Sound(bool inside, float deltaT)
{
    if (!_brain || !GScene || !GScene->GetCamera())
        return;
    const Brain& b = *_brain;
    const Vector3 cam = GScene->GetCamera()->Position();
    const float d = cam.Distance(Position());
    _sing += ((b.seq || b.dead ? (b.dead ? 0.0f : 0.1f) : 1.0f) - _sing) * (1 - std::exp(-deltaT / 4));
    const float fade = 1 - std::clamp((d - 70) / 30, 0.0f, 1.0f);
    if (fade <= 0 && !_song)
        return;
    if (!_song)
        _song = OpenSeaLoop("whale_song", Position());
    if (!_song)
        return;
    const bool under = cam.Y() < WaterAt(cam.X(), cam.Z()) - 0.2f;
    _song->SetVolume(SeaSoundVolume("whale_song", _sing * fade * (under ? 1.0f : 0.1f)), 1.0f);
    _song->SetPosition(Position(), Speed());
}

void Whale::UnloadSound()
{
    _song.Free();
}

void Whale::Simulate(float deltaT, SimulationImportance prec)
{
    _isDead = IsDammageDestroyed();
    if (!_brain)
        Start();
    if (!_rig->valid)
    {
        VehicleSupply::Simulate(deltaT, prec);
        return;
    }
    Brain& b = *_brain;
    if (_isDead && !b.dead)
    {
        b.dead = true;
        LOG_INFO(World, "Whale {} killed: it rolls over and floats", (const char*)GetDebugName());
    }
    // (any loss of health: HitBy is not the only way damage arrives)
    const float dammage = GetTotalDammage();
    if (!b.dead && dammage > _lastDammage + 0.01f)
        b.Sound();
    _lastDammage = dammage;

    const Vector3 oldPos = Position();
    if (deltaT > 0)
    {
        // Tidewater steps the brain once a frame (its dt clamped to 0.1 s); here in steps of at most 1/30 s
        float rest = std::min(deltaT, 0.5f);
        while (rest > 1e-5f)
        {
            const float dt = std::min(rest, 1.0f / 30.0f);
            b.Update(dt);
            rest -= dt;
        }
    }
    b.PoseFrames();
    Publish();

    // the entity is the root: Tidewater's root pose mirrored into the OFP world, the model's whale origin on it
    // (model +z is Tidewater's sz * z: the entity's forward)
    const Vector3 fwd = Mirror(Rot(b.quat, Vector3(0, 0, _rig->sz)));
    const Vector3 up = Mirror(Rot(b.quat, Vector3(0, 1, 0)));
    Matrix4 trans;
    trans.SetDirectionAndUp(fwd, up);
    static const float lift = []
    {
        const char* v = std::getenv("POSEIDON_WHALE_LIFT"); // a visibility test: the whale drawn this far up
        return v ? (float)std::atof(v) : 0.0f;
    }();
    trans.SetPosition(Mirror(b.position) - trans.Rotate(_rig->origin) + Vector3(0, lift, 0));
    Move(trans);
    if (deltaT > 0)
        _speed = (trans.Position() - oldPos) * (1.0f / deltaT);
    Effects(deltaT);
    // the wounds bleed on: a trail behind a whale running, a slick round a dead one
    if (_bleed > 0.01f)
    {
        _bleedClock += deltaT;
        if (_bleedClock >= 1.0f)
        {
            _bleedClock -= 1.0f;
            Bleed(trans.Position(), _bleed);
        }
        _bleed *= std::exp(-deltaT / (b.dead ? 90.0f : 40.0f));
    }

    if (WhaleDiag() && Glob.time.toFloat() >= _logNext)
    {
        _logNext = Glob.time.toFloat() + 1.0f;
        LOG_INFO(World,
                 "Whale: t={:.1f} pos={:.1f},{:.1f},{:.1f} water={:.2f} back={:.2f} speed={:.2f} pitch={:.2f} "
                 "key={}/{} seq={} blows={} breaches={} slaps={} dead={} dammage={:.2f}",
                 Glob.time.toFloat(), trans.Position().X(), trans.Position().Y(), trans.Position().Z(), b.water,
                 b.BackDepth(), b.speed, b.pitch, b.ki, (int)b.keys.size(), b.seq, b.blows, b.breaches, b.slaps,
                 b.dead, dammage);
    }

    VehicleSupply::Simulate(deltaT, prec);
}

void Whale::Animate(int level)
{
    base::Animate(level);
    Shape* shape = _shape->Level(level);
    if (!shape || !_rig || !_rig->valid || !_shape->IsNormalLevel(level))
        return;
    const Rig& rig = *_rig;
    const Rig::Level& lv = rig.LevelFor(_shape, level);
    if ((int)lv.restZ.size() != shape->NPos())
        return;
    Pose p;
    {
        std::lock_guard<std::mutex> g(_poseLock);
        if (!_pose->valid)
            return;
        p = *_pose;
    }
    shape->SaveOriginalPos();
    const int K = rig.k;
    const float invDz = 1.0f / rig.dz;
    const int n = shape->NPos();
    for (int i = 0; i < n; i++)
    {
        // the rest point and normal in Tidewater's whale frame
        Vector3 pt = rig.ToTw(shape->OrigPos(i) - rig.origin);
        Vector3 nm = rig.ToTw(shape->OrigNorm(i));
        const int part = lv.part[i];
        if (part)
        {
            // the flipper turns about its root, the tip a little further than the root (Whale.js whalePose)
            const int s = part - 1;
            const float ang = p.flipAngle[s] * (lv.flex[i] * 0.35f + 0.8f);
            const Vector3& root = rig.flipperRoot[s];
            pt = root + RotAxis(p.flipAxis[s], ang, pt - root);
            nm = RotAxis(p.flipAxis[s], ang, nm);
        }
        const float rz = lv.restZ[i];
        const float fi = std::clamp((rig.zHead - rz) * invDz, 0.0f, K - 1.001f);
        const int f = (int)fi;
        const float t = fi - f;
        const Q& q0 = p.rot[f];
        const Q& q1 = p.rot[f + 1];
        const Q q = Normalized({q0.x + (q1.x - q0.x) * t, q0.y + (q1.y - q0.y) * t, q0.z + (q1.z - q0.z) * t,
                                q0.w + (q1.w - q0.w) * t});
        const float yc = rig.restY[f] + (rig.restY[f + 1] - rig.restY[f]) * t;
        const Vector3 off(pt.X(), pt.Y() - yc, pt.Z() - rz);
        const Vector3 world = p.pos[f] + (p.pos[f + 1] - p.pos[f]) * t + Rot(q, off);
        static const bool noPose = std::getenv("POSEIDON_WHALE_NOPOSE") != nullptr;
        if (noPose)
            continue;
        shape->SetPos(i) = rig.origin + rig.ToTw(world);
        shape->SetNorm(i) = rig.ToTw(Rot(q, nm));
    }
    if (WhaleDiag())
    {
        static int reported = 0;
        if (reported < 6)
        {
            reported++;
            const Vector3 r0 = shape->OrigPos(0), p0 = shape->Pos(0);
            LOG_INFO(World, "Whale animate: level {} n={} v0 rest {:.2f},{:.2f},{:.2f} posed {:.2f},{:.2f},{:.2f} origin {:.2f},{:.2f},{:.2f} entity {:.1f},{:.1f},{:.1f}",
                     level, n, r0.X(), r0.Y(), r0.Z(), p0.X(), p0.Y(), p0.Z(), rig.origin.X(), rig.origin.Y(),
                     rig.origin.Z(), Position().X(), Position().Y(), Position().Z());
        }
    }
    shape->InvalidateNormals();
    shape->InvalidateBuffer();
}

void Whale::Deanimate(int level)
{
    Shape* shape = _shape->Level(level);
    if (shape && _rig && _rig->valid && _shape->IsNormalLevel(level) && shape->OriginalPosValid())
    {
        const int n = shape->NPos();
        for (int i = 0; i < n; i++)
        {
            shape->SetPos(i) = shape->OrigPos(i);
            shape->SetNorm(i) = shape->OrigNorm(i);
        }
        shape->InvalidateNormals();
        shape->InvalidateBuffer();
    }
    base::Deanimate(level);
}

} // namespace Poseidon
