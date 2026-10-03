// DIAG-001: diag_camera - pin the render camera on a unit.
//   diag_camera {unit, view?, dist?, height?, az?, elev?, follow?, off?}
//     view "front" | "rear" | "left" | "right" | "top" | "corner" (front-right, 25 deg up) | "corner2" (rear-left) |
//     "corner3" | "corner4" | "low" (front, knee height), or a free az (deg from the unit's front, + = its right)
//     and elev (deg up). dist defaults to fit the model (1.7 x its size); the focus is the middle of the posed
//     model (height = metres above the unit's feet to override). follow (default true) keeps the camera on the
//     unit every frame. Works while paused (diag_pose), so a pose can be shot from every side.
//     {off:true} gives the camera back to the mission.
// Drives the same render-view override as the triSetView test verb (World_SetTriViewOverride in World.cpp).
//
// Ported from Malprave (Dec's GPL fork of the same Poseidon source), MwDiag9.cpp. Not ported: "clear" (hiding the
// objects between camera and unit), which needs the x-ray hook in the scene draw (MwDiag7); diag_melee and
// diag_explode from the same file (Cry of Fear melee / undead damage tests).
// Compiled only with POSEIDON_DIAG=1.

#include <Poseidon/Dev/Diag/OpDiag.hpp>
#include <Poseidon/Dev/Diag/OpDiagInternal.hpp>

#if POSEIDON_DIAG

// GameStateExtCommon.hpp first: the harness headers pull in windows.h (GetObject -> GetObjectA)
#include <Poseidon/Game/Commands/GameStateExtCommon.hpp>
#include <Poseidon/Dev/Harness/HarnessServer.hpp>
#include <Poseidon/Dev/Harness/HarnessProtocol.hpp>
#include <Poseidon/Core/Global.hpp>
#include <Poseidon/World/World.hpp>
#include <Poseidon/World/Scene/Object.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/World/Entities/Infantry/SoldierOld.hpp>
#include <Poseidon/Graphics/Rendering/Shape/Shape.hpp>

#include <cjson/cJSON.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#ifdef GetObject
#undef GetObject
#endif

// World.cpp: the render-view override (also used by the triSetView test verb)
extern void World_SetTriViewOverride(Vector3Par pos, Vector3Par dir, Vector3Par up);
extern void World_ClearTriViewOverride();

namespace Poseidon::Dev::OpDiag
{
namespace
{
const char* StrArg(cJSON* root, const char* key)
{
    cJSON* v = cJSON_GetObjectItemCaseSensitive(root, key);
    return v && cJSON_IsString(v) ? v->valuestring : nullptr;
}

bool HasNum(cJSON* root, const char* key)
{
    cJSON* v = cJSON_GetObjectItemCaseSensitive(root, key);
    return v && cJSON_IsNumber(v);
}

double NumArg(cJSON* root, const char* key, double def)
{
    cJSON* v = cJSON_GetObjectItemCaseSensitive(root, key);
    return v && cJSON_IsNumber(v) ? v->valuedouble : def;
}

bool BoolArg(cJSON* root, const char* key, bool def)
{
    cJSON* v = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!v)
        return def;
    return cJSON_IsTrue(v) || (cJSON_IsNumber(v) && v->valuedouble != 0);
}

struct CamState
{
    OLink<Object> unit;
    bool active = false;
    bool follow = true;
    float az = 0, elev = 10, dist = -1, height = -1;
};
CamState s_cam;

bool ViewAngles(const char* view, float& az, float& elev)
{
    struct V
    {
        const char* name;
        float az, elev;
    };
    static const V kViews[] = {{"front", 0, 8},      {"rear", 180, 8},   {"left", -90, 8},     {"right", 90, 8},
                               {"top", 0, 89},       {"corner", 45, 25}, {"corner2", 225, 25}, {"corner3", -45, 25},
                               {"corner4", 135, 25}, {"low", 0, -5}};
    for (const V& v : kViews)
        if (stricmp(view, v.name) == 0)
        {
            az = v.az;
            elev = v.elev;
            return true;
        }
    return false;
}

//! camera position / direction / up for the current state; false when the unit is gone
bool CamTransform(Vector3& pos, Vector3& dir, Vector3& up, Vector3& focus, float& dist)
{
    Object* o = s_cam.unit.GetLink();
    if (!o)
        return false;
    Vector3 fwd = o->Direction();
    fwd[1] = 0;
    if (fwd.SquareSize() < 1e-6f)
        fwd = Vector3(0, 0, 1);
    fwd.Normalize();
    const Vector3 right = VUp.CrossProduct(fwd); // +X = the unit's right
    float lo = 0, hi = 1.8f, size = 1.8f;
    if (LODShape* sh = o->GetShape())
    {
        lo = sh->Min().Y();
        hi = sh->Max().Y();
        const float w = std::max(sh->Max().X() - sh->Min().X(), sh->Max().Z() - sh->Min().Z());
        size = std::max(hi - lo, w);
    }
    focus = o->Position() + Vector3(0, s_cam.height >= 0 ? s_cam.height : (lo + hi) * 0.5f, 0);
    if (s_cam.height < 0)
        if (Man* m = dyn_cast<Man>(o))
            if (Shape* hs = o->GetShape() ? o->GetShape()->HitpointsLevel() : nullptr; hs && hs->NPos() > 0)
            {
                // a kneeling / fallen / dead pose has left the bind-pose middle: aim at the middle of the pose
                const int level = o->GetShape()->FindHitpoints();
                Vector3 mn(1e9f, 1e9f, 1e9f), mx(-1e9f, -1e9f, -1e9f);
                for (int i = 0; i < hs->NPos(); i++)
                {
                    const Vector3 p = m->AnimatePoint(level, i);
                    for (int k = 0; k < 3; k++)
                    {
                        mn[k] = std::min(mn[k], p[k]);
                        mx[k] = std::max(mx[k], p[k]);
                    }
                }
                focus = o->PositionModelToWorld((mn + mx) * 0.5f);
            }
    dist = s_cam.dist > 0 ? s_cam.dist : std::max(2.0f, size * 1.7f);
    const float a = s_cam.az * (H_PI / 180.0f), e = s_cam.elev * (H_PI / 180.0f);
    const Vector3 flat = fwd * std::cos(a) + right * std::sin(a);
    pos = focus + flat * (std::cos(e) * dist) + VUp * (std::sin(e) * dist);
    if (GLandscape)
    {
        const float g = GLandscape->SurfaceY(pos.X(), pos.Z());
        if (pos.Y() < g + 0.15f)
            pos[1] = g + 0.15f;
    }
    dir = focus - pos;
    dir.Normalize();
    up = std::fabs(dir.Y()) > 0.97f ? fwd : VUp; // straight down: the unit's front is "up" in the picture
    return true;
}

bool CamApply()
{
    Vector3 pos, dir, up, focus;
    float dist;
    if (!CamTransform(pos, dir, up, focus, dist))
        return false;
    // orthonormal up
    Vector3 side = up.CrossProduct(dir);
    side.Normalize();
    up = dir.CrossProduct(side);
    World_SetTriViewOverride(pos, dir, up);
    return true;
}

std::string Err(const char* what)
{
    return HarnessProtocol::ErrorResponse(what);
}

std::string CameraCmd(cJSON* root)
{
    if (BoolArg(root, "off", false))
    {
        s_cam = CamState{};
        World_ClearTriViewOverride();
        Json::Object j;
        j.Str("camera", "mission");
        return j.Ok();
    }
    std::string err;
    Object* o = Internal::EvalObject(StrArg(root, "unit"), err);
    if (!o)
        return Err(err.empty() ? "unit required (a mission variable name)" : err.c_str());
    CamState c;
    c.unit = o;
    c.active = true;
    float az = 0, elev = 8;
    if (const char* view = StrArg(root, "view"))
        if (!ViewAngles(view, az, elev))
            return Err("view: front, rear, left, right, top, corner, corner2, corner3, corner4, low (or az / elev)");
    c.az = static_cast<float>(NumArg(root, "az", az));
    c.elev = std::clamp(static_cast<float>(NumArg(root, "elev", elev)), -30.0f, 89.0f);
    c.dist = static_cast<float>(NumArg(root, "dist", -1));
    c.height = HasNum(root, "height") ? static_cast<float>(NumArg(root, "height", -1)) : -1.0f;
    c.follow = BoolArg(root, "follow", true);
    s_cam = c;
    if (!CamApply())
        return Err("unit gone");
    Vector3 pos, dir, up, focus;
    float dist = 0;
    CamTransform(pos, dir, up, focus, dist);
    Json::Object j;
    j.Str("unit", ObjName(o).c_str())
        .Pos("pos", pos.X(), pos.Y(), pos.Z())
        .Pos("focus", focus.X(), focus.Y(), focus.Z());
    j.Num("az", s_cam.az, "%.1f")
        .Num("elev", s_cam.elev, "%.1f")
        .Num("dist", dist, "%.2f")
        .Bool("follow", s_cam.follow);
    return j.Ok();
}
} // namespace

namespace Internal
{
void TickCamera()
{
    if (!s_cam.active)
        return;
    if (!s_cam.unit.GetLink())
    {
        s_cam = CamState{};
        World_ClearTriViewOverride();
        return;
    }
    if (s_cam.follow)
        CamApply();
}

void RegisterCamera(HarnessServer& hs)
{
    hs.RegisterCommand({"diag_camera",
                        "diag: pin the render camera on a unit (front / rear / left / right / top / corner... or "
                        "az / elev) and follow it; {off:true} back to the mission camera",
                        {{"unit", "string", false},
                         {"view", "string", false},
                         {"dist", "number", false},
                         {"height", "number", false},
                         {"az", "number", false},
                         {"elev", "number", false},
                         {"follow", "bool", false},
                         {"off", "bool", false}}},
                       [](const std::string&, cJSON* root) -> std::string { return CameraCmd(root); });
}
} // namespace Internal

} // namespace Poseidon::Dev::OpDiag

#endif // POSEIDON_DIAG
