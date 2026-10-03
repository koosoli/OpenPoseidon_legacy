// DIAG-001 3D overlay: shot paths, impacts, damage numbers, STUCK / SPIN / DEAD labels and, for watched units,
// a name / damage / move label, a facing line, the last 10 s trail and the planned AI path. Projected with the
// scene camera exactly as the dev panel's Zeus markers are (ScaledInvTransform + Camera::Projection, framebuffer
// pixels scaled to ImGui's display size) and drawn on ImGui's foreground list inside the dev overlay's frame
// (DebugOverlay.cpp Render, before ImGui::Render), so it composites over the wgpu or GL33 frame the same way the
// dev panel does and screenshots taken through the harness show it. --diag-draw / diag_draw turn it on; it needs
// the dev overlay (non-release build, --dev, the default).
//
// Ported from Malprave (Dec's GPL fork of the same Poseidon source), MwDiagOverlay.cpp. Not ported: the x-ray
// wireframes and the chase line (Cry of Fear chase AI). The projection was rewritten for Oli's renderer.
// Compiled only with POSEIDON_DIAG=1.

#include <Poseidon/Dev/Diag/OpDiag.hpp>
#include <Poseidon/Dev/Diag/OpDiagInternal.hpp>

#if POSEIDON_DIAG

#include <Poseidon/Core/Global.hpp>
#include <Poseidon/World/World.hpp>
#include <Poseidon/World/Scene/Scene.hpp>
#include <Poseidon/World/Scene/Camera/Camera.hpp>
#include <Poseidon/World/Scene/Object.hpp>
#include <Poseidon/World/Entities/Infantry/SoldierOld.hpp>
#include <Poseidon/AI/VehicleAI.hpp>
#include <Poseidon/AI/AI.hpp>
#include <Poseidon/AI/Path/PathSteer.hpp>
#include <Poseidon/Graphics/Core/Engine.hpp>

// Logging.hpp (PCH) #defines DebugLog(), which collides with ImGui::DebugLog() (as in DebugOverlay.cpp)
#ifdef DebugLog
#undef DebugLog
#endif
#include <imgui.h>

#include <cmath>
#include <cstdio>
#include <string>

namespace Poseidon::Dev::OpDiag
{
namespace
{
struct Proj
{
    const Camera* cam;
    Matrix4 inv;  //!< world -> camera space
    float sx, sy; //!< framebuffer pixels -> ImGui display units
    float nearZ;
    ImDrawList* dl;
};

// rgba as 0xRRGGBBAA -> ImGui ABGR
ImU32 Col(uint32_t rgba)
{
    return IM_COL32((rgba >> 24) & 0xff, (rgba >> 16) & 0xff, (rgba >> 8) & 0xff, rgba & 0xff);
}

ImVec2 ToScreen(const Proj& p, Vector3Par c)
{
    const Matrix4& projection = p.cam->Projection();
    const float invZ = 1.0f / c.Z();
    const float px = projection(0, 2) + projection(0, 0) * c[0] * invZ;
    const float py = projection(1, 2) + projection(1, 1) * c[1] * invZ;
    return ImVec2(px * p.sx, py * p.sy);
}

void Line(const Proj& p, Vector3Par a, Vector3Par b, ImU32 col, float thick)
{
    Vector3 ca = p.inv * a;
    Vector3 cb = p.inv * b;
    if (ca.Z() < p.nearZ && cb.Z() < p.nearZ)
        return;
    // clip against the near plane
    if (ca.Z() < p.nearZ)
        ca = ca + (cb - ca) * ((p.nearZ - ca.Z()) / (cb.Z() - ca.Z()));
    else if (cb.Z() < p.nearZ)
        cb = cb + (ca - cb) * ((p.nearZ - cb.Z()) / (ca.Z() - cb.Z()));
    p.dl->AddLine(ToScreen(p, ca), ToScreen(p, cb), col, thick);
}

bool Point(const Proj& p, Vector3Par a, ImVec2& out)
{
    const Vector3 c = p.inv * a;
    if (c.Z() < p.nearZ)
        return false;
    out = ToScreen(p, c);
    return true;
}

void Text(const Proj& p, ImVec2 at, ImU32 col, const char* text)
{
    p.dl->AddText(ImVec2(at.x + 1, at.y + 1), IM_COL32(0, 0, 0, 200), text);
    p.dl->AddText(at, col, text);
}

void DrawItemFn(int kind, Vector3Par a, Vector3Par b, float size, uint32_t rgba, const char* text, void* ctx)
{
    const Proj& p = *static_cast<const Proj*>(ctx);
    const ImU32 col = Col(rgba);
    switch (kind)
    {
        case 0:
            Line(p, a, b, col, 2.0f);
            break;
        case 1:
        {
            const float s = size > 0 ? size : 0.2f;
            Line(p, a - Vector3(s, 0, 0), a + Vector3(s, 0, 0), col, 2.0f);
            Line(p, a - Vector3(0, s, 0), a + Vector3(0, s, 0), col, 2.0f);
            Line(p, a - Vector3(0, 0, s), a + Vector3(0, 0, s), col, 2.0f);
            break;
        }
        case 2:
        {
            ImVec2 s;
            if (Point(p, a, s) && text && text[0])
                Text(p, s, col, text);
            break;
        }
        default:
            break;
    }
}

void DrawWatched(const Proj& p, Object* obj)
{
    ImVec2 s;
    if (Point(p, obj->Position() + VUp * 2.1f, s))
    {
        std::string label = ObjName(obj);
        char buf[64];
        std::snprintf(buf, sizeof(buf), "  dmg %.2f", obj->GetTotalDammage());
        label += buf;
        if (Man* m = dyn_cast<Man>(obj))
            label += "\n" + m->DiagMoveName();
        Text(p, s, IM_COL32(255, 255, 120, 255), label.c_str());
    }
    // facing line on the ground
    const Vector3 base = obj->Position() + VUp * 0.1f;
    Line(p, base, base + obj->Direction() * 1.5f, IM_COL32(255, 255, 120, 255), 2.0f);

    // last 10 s trail (white, fading) and the planned path (cyan); drawn over everything, so they show through walls
    if (const std::deque<Vector3>* tr = Internal::TrailOf(obj); tr && tr->size() > 1)
    {
        const size_t n = tr->size();
        for (size_t i = 1; i < n; i++)
        {
            const int alpha = static_cast<int>(60 + 195 * i / n);
            Line(p, (*tr)[i - 1] + VUp * 0.15f, (*tr)[i] + VUp * 0.15f, IM_COL32(255, 255, 255, alpha), 1.5f);
        }
    }
    if (Man* m = dyn_cast<Man>(obj))
    {
        if (AIUnit* unit = m->Brain())
        {
            const Path& path = unit->GetPath();
            for (int i = 1; i < path.Size(); i++)
                Line(p, path[i - 1]._pos + VUp * 0.3f, path[i]._pos + VUp * 0.3f, IM_COL32(60, 220, 255, 220), 2.0f);
            for (int i = 0; i < path.Size(); i++)
            {
                ImVec2 s2;
                if (Point(p, path[i]._pos + VUp * 0.3f, s2))
                    p.dl->AddCircleFilled(s2, 3.0f, IM_COL32(60, 220, 255, 255));
            }
        }
    }
}
} // namespace

void RenderOverlay()
{
    if (!DrawEnabled() || !GScene || !GEngine)
        return;
    const Camera* cam = GScene->GetCamera();
    if (!cam || GEngine->Width() <= 0 || GEngine->Height() <= 0)
        return;
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    Proj p;
    p.cam = cam;
    p.inv = GScene->ScaledInvTransform();
    p.sx = display.x / static_cast<float>(GEngine->Width());
    p.sy = display.y / static_cast<float>(GEngine->Height());
    p.nearZ = std::max(0.05f, static_cast<float>(cam->Near()));
    p.dl = ImGui::GetForegroundDrawList();
    if (!p.dl)
        return;
    Internal::ForEachDrawItem(&DrawItemFn, &p);
    for (Object* o : Internal::Watched())
        DrawWatched(p, o);
    if (Paused())
        Text(p, ImVec2(10, 10), IM_COL32(255, 80, 80, 255), "diag: PAUSED");
}

} // namespace Poseidon::Dev::OpDiag

#endif // POSEIDON_DIAG
