#include <Poseidon/Dev/Diag/BallisticsRecorder.hpp>

#include <Poseidon/Core/Application.hpp>
#include <Poseidon/Core/Global.hpp>
#include <Poseidon/World/World.hpp>
#include <Poseidon/World/Scene/Object.hpp>
#include <Poseidon/World/Scene/ObjLine.hpp>
#include <Poseidon/World/Scene/Scene.hpp>
#include <Poseidon/World/Entities/Weapons/Shots.hpp>
#include <Poseidon/World/Weather/WindModel.hpp>
#include <Poseidon/AI/EntityAI.hpp>
#include <Poseidon/Foundation/Math/Math3D.hpp>

#include <cmath>
#include <cstdlib>
#include <unordered_map>
#include <vector>

namespace Poseidon::Dev::Ballistics
{

/// Off unless asked for: recording samples every projectile and retains their tracks, so an
/// always-on default would make every mission pay for a diagnostic nobody opened.
///
/// `POSEIDON_BALLISTICS_RECORD=1` arms it at startup. That exists because the dev panel is the
/// ONLY way to turn this on today, which means the trajectory view cannot be reached from a
/// scripted launch at all -- no capture harness, no smoke test, no repro attached to a bug
/// report. Anything the owner can only see by clicking is something nobody can hand to anyone
/// else. The tab's checkbox still works exactly as before and overrides this either way.
bool g_enabled = []
{
    const char* v = std::getenv("POSEIDON_BALLISTICS_RECORD");
    return v != nullptr && v[0] != '\0' && v[0] != '0';
}();

namespace
{

/// Live association between an in-world projectile and its track.  The key is
/// the Shot pointer used purely as an identity: it is only ever compared, and
/// it is only ever obtained from the current fast-vehicle list, so a stale
/// entry is dropped without being dereferenced.
struct LiveShot
{
    uint32_t trackId = 0;
    float startTime = 0.0f; ///< Glob.time (seconds) when first seen
    bool seenThisFrame = false;
};

BallisticsTrackStore s_store;
std::unordered_map<const void*, LiveShot> s_live;

uint32_t s_selected = 0;
bool s_drawTrails = true;
int s_segmentBudget = 1024;
int s_lastSegments = 0;

// Reusable line objects.  ObjectLine::CreateShape builds a two-vertex shape and
// ObjectLine::SetPos rewrites it, so one (shape, object) pair can draw a
// different segment every frame.  Building them once and keeping them means a
// magazine dump does not turn into thousands of allocations per frame; the pool
// only ever grows to the segment budget.
std::vector<Ref<LODShapeWithShadow>> s_lineShapes;
std::vector<Ref<Object>> s_lineObjects;
int s_poolUsed = 0;

void SubmitLine(Vector3Par from, Vector3Par to, PackedColor color)
{
    if (s_poolUsed >= s_segmentBudget)
    {
        return;
    }
    if (s_poolUsed >= static_cast<int>(s_lineObjects.size()))
    {
        Ref<LODShapeWithShadow> shape = ObjectLine::CreateShape();
        Ref<Object> obj = new ObjectLineDiag(shape);
        s_lineShapes.push_back(shape);
        s_lineObjects.push_back(obj);
    }

    LODShapeWithShadow* shape = s_lineShapes[static_cast<std::size_t>(s_poolUsed)];
    Object* obj = s_lineObjects[static_cast<std::size_t>(s_poolUsed)];
    ++s_poolUsed;

    obj->SetConstantColor(color);
    obj->SetPosition(from);
    ObjectLine::SetPos(shape, VZero, to - from);
    GScene->ObjectForDrawing(obj);
    ++s_lastSegments;
}

/// Player shots are warm (amber), AI shots cold (cyan); the selected track is
/// drawn white.  Deliberately not a gradient along the trail — a diagnostic
/// wants "whose round is this" answerable at a glance, not a pretty streak.
PackedColor TrackColor(const BallisticTrack& track, bool selected)
{
    if (selected)
    {
        return PackedColor(Color(1.0f, 1.0f, 1.0f, 0.9f));
    }
    if (track.byPlayer)
    {
        return PackedColor(Color(1.0f, 0.75f, 0.15f, 0.7f));
    }
    return PackedColor(Color(0.25f, 0.8f, 1.0f, 0.7f));
}

PackedColor TerminusColor(BallisticTerminus terminus)
{
    switch (terminus)
    {
        case BallisticTerminus::HitObject:
            return PackedColor(Color(1.0f, 0.1f, 0.1f, 0.95f));
        case BallisticTerminus::HitGround:
            return PackedColor(Color(0.8f, 0.5f, 0.2f, 0.9f));
        case BallisticTerminus::HitWater:
            return PackedColor(Color(0.2f, 0.5f, 1.0f, 0.9f));
        case BallisticTerminus::Expired:
            return PackedColor(Color(0.6f, 0.6f, 0.6f, 0.8f));
        case BallisticTerminus::Lost:
            return PackedColor(Color(0.5f, 0.3f, 0.5f, 0.8f));
        case BallisticTerminus::InFlight:
            break;
    }
    return PackedColor(Color(1.0f, 1.0f, 1.0f, 0.5f));
}

/// A projectile belongs to "the player" if its firing entity is the player's
/// own person or the vehicle the camera is riding.  Covers both infantry (owner
/// is the Person) and crewed weapons (owner is the Transport).
bool IsPlayerShot(const EntityAI* owner)
{
    if (!owner || !GWorld)
    {
        return false;
    }
    // Compare as raw addresses. PlayerOn() returns Person*, which is only forward-declared
    // here, so a Person* -> const Object* conversion would need Person's full definition
    // pulled into a diagnostics TU purely to do a pointer equality test. Identity is all
    // this needs, and identity survives the cast to void.
    const void* self = static_cast<const void*>(owner);
    if (const void* player = static_cast<const void*>(GWorld->PlayerOn()); player && self == player)
    {
        return true;
    }
    const void* camOn = static_cast<const void*>(GWorld->CameraOn());
    return camOn && self == camOn;
}

} // namespace

BallisticsTrackStore& Store()
{
    return s_store;
}

void SetEnabled(bool enabled)
{
    if (enabled == g_enabled)
    {
        return;
    }
    g_enabled = enabled;
    if (!enabled)
    {
        // Turning the diagnostic off must actually give the memory back — an
        // idle store that still holds a hundred trails is not "costs nothing".
        ClearAll();
        s_lineShapes.clear();
        s_lineObjects.clear();
        s_poolUsed = 0;
        s_lastSegments = 0;
    }
}

void ClearAll()
{
    s_store.Clear();
    s_live.clear();
    s_selected = 0;
}

uint32_t SelectedTrack()
{
    return s_selected;
}

void SetSelectedTrack(uint32_t id)
{
    s_selected = id;
}

bool DrawTrails()
{
    return s_drawTrails;
}

void SetDrawTrails(bool draw)
{
    s_drawTrails = draw;
}

int LastSegmentsDrawn()
{
    return s_lastSegments;
}

int SegmentBudget()
{
    return s_segmentBudget;
}

void SetSegmentBudget(int segments)
{
    if (segments < 64)
    {
        segments = 64;
    }
    if (segments > 16384)
    {
        segments = 16384;
    }
    s_segmentBudget = segments;
}

void Sample()
{
    if (!g_enabled || !GWorld)
    {
        return;
    }

    // Mission reload / world teardown guard.  The pooled line objects hold a
    // shape built against the scene's preloaded white texture, and every track
    // holds coordinates in the old world's frame.  Both are meaningless — and
    // the shapes are unsafe — once the scene is rebuilt, so drop everything the
    // first frame a different Scene is seen.  One pointer compare per frame.
    static const void* s_lastScene = nullptr;
    const void* scene = static_cast<const void*>(GScene);
    if (scene != s_lastScene)
    {
        s_lastScene = scene;
        s_store.Clear();
        s_live.clear();
        s_selected = 0;
        s_lineShapes.clear();
        s_lineObjects.clear();
        s_poolUsed = 0;
    }

    const float now = Glob.time.toFloat();

    for (auto& entry : s_live)
    {
        entry.second.seenThisFrame = false;
    }

    const int count = GWorld->NFastVehicles();
    for (int i = 0; i < count; ++i)
    {
        Entity* entity = GWorld->GetFastVehicle(i);
        if (!entity)
        {
            continue;
        }
        Shot* shot = dyn_cast<Shot>(entity);
        if (!shot)
        {
            continue;
        }

        // Key on the Entity* the list handed us, not the dyn_cast Shot*: the impact
        // hook normalises to Entity* too, so both sides agree on one address
        // regardless of how the class chain lays its bases out.
        const void* key = static_cast<const void*>(entity);
        auto it = s_live.find(key);
        if (it == s_live.end())
        {
            const Vector3 position = shot->Position();
            const Vector3 velocity = shot->Speed();
            const EntityAI* owner = shot->GetOwner();

            const WindSample& wind = GWind.Sample();
            const bool windActive = WindModel::BallisticsEnabled();

            // RString locals, not `const char*` — RString::GetName returns by
            // value, and binding a raw pointer to the temporary would dangle.
            const EntityType* type = shot->GetNonAIType();
            const RString ammoName = type ? type->GetName() : RString("(unknown)");
            const RString ownerName = owner ? owner->GetDebugName() : RString("(no owner)");

            const uint32_t id = s_store.Begin(static_cast<const char*>(ammoName),
                                              static_cast<const char*>(ownerName), IsPlayerShot(owner),
                                              position.X(), position.Y(), position.Z(), velocity.X(), velocity.Y(),
                                              velocity.Z(), windActive, wind.speed, wind.directionRad);

            LiveShot live;
            live.trackId = id;
            live.startTime = now;
            live.seenThisFrame = true;
            s_live.emplace(key, live);

            s_store.AddSample(id, position.X(), position.Y(), position.Z(), 0.0f);
        }
        else
        {
            it->second.seenThisFrame = true;
            const Vector3 position = shot->Position();
            s_store.AddSample(it->second.trackId, position.X(), position.Y(), position.Z(),
                              now - it->second.startTime);
        }
    }

    // Sweep out projectiles that left the world.  A track whose terminus was
    // already reported by NotifyImpact keeps that answer; anything else is
    // honestly "lost" rather than a guessed impact.
    for (auto it = s_live.begin(); it != s_live.end();)
    {
        if (it->second.seenThisFrame)
        {
            ++it;
            continue;
        }
        BallisticTrack* track = s_store.Find(it->second.trackId);
        if (track && track->terminus == BallisticTerminus::InFlight)
        {
            track->terminus = BallisticTerminus::Lost;
        }
        it = s_live.erase(it);
    }
}

void NotifyImpactImpl(const void* shot, float x, float y, float z, BallisticTerminus terminus, const char* hitName)
{
    auto it = s_live.find(shot);
    if (it == s_live.end())
    {
        return;
    }
    // The impact point is the one sample the polling loop can never see: the
    // projectile is deleted in the same simulation step it reaches it.
    const float t = Glob.time.toFloat() - it->second.startTime;
    BallisticTrack* track = s_store.Find(it->second.trackId);
    if (track)
    {
        track->samples.push_back(BallisticSample{x, y, z, t});
    }
    s_store.Finish(it->second.trackId, terminus, hitName);
}

void Draw()
{
    s_lastSegments = 0;
    s_poolUsed = 0;

    if (!g_enabled || !s_drawTrails || !GScene)
    {
        return;
    }

    const int trackCount = s_store.Size();
    for (int t = 0; t < trackCount; ++t)
    {
        const BallisticTrack& track = s_store.At(t);
        if (track.samples.size() < 2)
        {
            continue;
        }
        const bool selected = (s_selected != 0 && s_selected == track.id);
        const PackedColor color = TrackColor(track, selected);

        for (std::size_t i = 1; i < track.samples.size(); ++i)
        {
            const BallisticSample& a = track.samples[i - 1];
            const BallisticSample& b = track.samples[i];
            SubmitLine(Vector3(a.x, a.y, a.z), Vector3(b.x, b.y, b.z), color);
        }

        // Terminus marker: a small axis cross, colour-coded by what ended the
        // flight.  Readable from any angle without needing text in the world.
        if (track.terminus != BallisticTerminus::InFlight)
        {
            const BallisticSample& end = track.samples.back();
            const Vector3 p(end.x, end.y, end.z);
            const float s = selected ? 0.6f : 0.35f;
            const PackedColor mark = TerminusColor(track.terminus);
            SubmitLine(p - Vector3(s, 0, 0), p + Vector3(s, 0, 0), mark);
            SubmitLine(p - Vector3(0, s, 0), p + Vector3(0, s, 0), mark);
            SubmitLine(p - Vector3(0, 0, s), p + Vector3(0, 0, s), mark);
        }
    }
}

} // namespace Poseidon::Dev::Ballistics
