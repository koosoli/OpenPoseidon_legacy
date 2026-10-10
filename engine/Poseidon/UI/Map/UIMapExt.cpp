#include <Poseidon/UI/Map/UIMap.hpp>
#include <Poseidon/Foundation/Common/Win.h>
#include <SDL3/SDL_scancode.h>
#include <SDL3/SDL_keycode.h>

#include <Poseidon/Core/resincl.hpp>
#include <Poseidon/Input/InputSubsystem.hpp>
#include <Poseidon/Foundation/Strings/StrFormat.hpp>
#include <Poseidon/IO/Streams/QBStream.hpp>

#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/Graphics/Textures/TexturePreload.hpp>

#include <Poseidon/IO/PackFiles.hpp>
#include <stdio.h>
#include <string.h>
#include <cmath>
#include <string>
#include <system_error>
#include <Poseidon/Foundation/Common/FltOpts.hpp>
#include <Poseidon/Foundation/Containers/Array.hpp>
#include <Poseidon/Foundation/Framework/DebugLog.hpp>
#include <Poseidon/Foundation/Math/Math3D.hpp>
#include <Poseidon/Foundation/Math/Math3DP.hpp>
#include <Poseidon/Foundation/Math/MathDefs.hpp>
#include <Poseidon/Foundation/Math/MathOpt.hpp>
#include <Poseidon/Foundation/Strings/RString.hpp>
#include <Poseidon/Foundation/Types/LLinks.hpp>
#include <Poseidon/Foundation/Types/Pointers.hpp>

#if defined _WIN32
#include <mapi.h>
#endif

#include <Poseidon/Dev/Debug/DebugTrap.hpp>
#include <Poseidon/UI/Locale/StringtableExt.hpp>

#include <Poseidon/Foundation/Strings/Bstring.hpp>
#include <filesystem>
#include <Poseidon/UI/WholeWorldDisplay.hpp>
#include <Poseidon/World/Terrain/EarthStreamingMode.hpp>
#include <Poseidon/World/Terrain/EarthFrame.hpp>
#include <cstdlib>

// Defined at global scope in AI/ArcadeTemplate.cpp.
void SelectLeader(ArcadeGroupInfo& gInfo);

namespace Poseidon
{
ArcadeTemplate& GetGClipboard()
{
    static ArcadeTemplate GClipboard;
    return GClipboard;
}

static void DeleteROFile(RString name)
{
    std::error_code ec;
    std::filesystem::permissions(std::string(name), std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::add, ec);
    std::filesystem::remove(std::string(name), ec);
}

void CStaticMap::DrawEllipse(Vector3Val position, float a, float b, float angle, PackedColor color)
{
    DrawCoord posMap = WorldToScreen(position);
    float cx = posMap.x * _wScreen;
    float cy = posMap.y * _hScreen;

    const float invSizeLand = InvLandSize;
    float aMap = a * invSizeLand * _invScaleX * _wScreen;
    float bMap = b * invSizeLand * _invScaleY * _hScreen;

    float r = floatMax(aMap, bMap);
    if (cx + r < _clipRect.x)
    {
        return;
    }
    if (cy + r < _clipRect.y)
    {
        return;
    }
    if (cx - r > _clipRect.x + _clipRect.w)
    {
        return;
    }
    if (cy - r > _clipRect.y + _clipRect.h)
    {
        return;
    }

    float angleRad = HDegree(angle);
    float s = sin(angleRad);
    float c = cos(angleRad);

    static const float linePts = 5.0f;
    int nSteps = toIntCeil(2 * H_PI * aMap * (1.0f / linePts));
    saturate(nSteps, 6, 720);
    float step = 2 * H_PI * (1.0f / nSteps);
    float lastX = cx, lastY = cy;
    bool lastValid = false;
    float st = 0;
    float ct = 1;
    float sstep = sin(step);
    float cstep = cos(step);
    for (float t = 0; t < 2 * H_PI + 0.5 * step; t += step)
    {
        float sta = st * aMap;
        float ctb = ct * bMap;
        float x = cx + c * sta + s * ctb;
        float y = cy + s * sta - c * ctb;
        if (lastValid)
        {
            GLOB_ENGINE->DrawLine(Line2DPixel(lastX, lastY, x, y), color, color, _clipRect);
        }
        lastX = x;
        lastY = y;
        lastValid = true;
        float stnew = st * cstep + ct * sstep;
        float ctnew = ct * cstep - st * sstep;
        st = stnew;
        ct = ctnew;
    }
}

void CStaticMap::DrawRectangle(Vector3Val position, float a, float b, float angle, PackedColor color)
{
    DrawCoord posMap = WorldToScreen(position);
    float cx = posMap.x * _wScreen;
    float cy = posMap.y * _hScreen;

    const float invSizeLand = InvLandSize;
    float aMap = a * invSizeLand * _invScaleX * _wScreen;
    float bMap = b * invSizeLand * _invScaleY * _hScreen;

    float r = sqrt(Square(aMap) + Square(bMap));
    if (cx + r < _clipRect.x)
    {
        return;
    }
    if (cy + r < _clipRect.y)
    {
        return;
    }
    if (cx - r > _clipRect.x + _clipRect.w)
    {
        return;
    }
    if (cy - r > _clipRect.y + _clipRect.h)
    {
        return;
    }

    float angleRad = HDegree(angle);
    float s = sin(angleRad);
    float c = cos(angleRad);

    float x1 = cx + c * aMap + s * bMap;
    float y1 = cy + s * aMap - c * bMap;
    float x2 = cx + c * aMap + s * (-bMap);
    float y2 = cy + s * aMap - c * (-bMap);
    float x3 = cx + c * (-aMap) + s * (-bMap);
    float y3 = cy + s * (-aMap) - c * (-bMap);
    float x4 = cx + c * (-aMap) + s * bMap;
    float y4 = cy + s * (-aMap) - c * bMap;

    GLOB_ENGINE->DrawLine(Line2DPixel(x1, y1, x2, y2), color, color, _clipRect);
    GLOB_ENGINE->DrawLine(Line2DPixel(x2, y2, x3, y3), color, color, _clipRect);
    GLOB_ENGINE->DrawLine(Line2DPixel(x3, y3, x4, y4), color, color, _clipRect);
    GLOB_ENGINE->DrawLine(Line2DPixel(x4, y4, x1, y1), color, color, _clipRect);
}

inline PackedColor ModAlpha(PackedColor color, float alpha)
{
    int a = toIntFloor(alpha * color.A8());
    saturate(a, 0, 255);
    return PackedColorRGB(color, a);
}

const static int NoClamp2D = (DefSpecFlags2D | NoClamp) & ~(ClampU | ClampV);

void CStaticMap::FillEllipse(Vector3Val position, float a, float b, float angle, PackedColor color, Texture* texture)
{
    DrawCoord posMap = WorldToScreen(position);
    float cx = posMap.x * _wScreen;
    float cy = posMap.y * _hScreen;

    const float invSizeLand = InvLandSize;
    float aMap = a * invSizeLand * _invScaleX * _wScreen;
    float bMap = b * invSizeLand * _invScaleY * _hScreen;

    float r = floatMax(aMap, bMap);
    if (cx + r < _clipRect.x)
    {
        return;
    }
    if (cy + r < _clipRect.y)
    {
        return;
    }
    if (cx - r > _clipRect.x + _clipRect.w)
    {
        return;
    }
    if (cy - r > _clipRect.y + _clipRect.h)
    {
        return;
    }

    float angleRad = HDegree(angle);
    float s = sin(angleRad);
    float c = cos(angleRad);

    PackedColor fillColor = color;

    float coefU = 0;
    float coefV = 0;
    float offsetX = (_mapX + _x) * _wScreen;
    float offsetY = (_mapY + _y) * _hScreen;
    if (texture)
    {
        coefU = 1.0 / texture->AWidth();
        coefV = 1.0 / texture->AHeight();
    }
    else
    {
        // special case
        fillColor = ModAlpha(color, 0.5);
    }
    MipInfo mip = GEngine->TextBank()->UseMipmap(texture, 0, 0);

    const int n = 3;
    Vertex2DPixel vs[n];
    // 0
    vs[0].x = cx;
    vs[0].y = cy;
    vs[0].u = coefU * (vs[0].x - offsetX);
    vs[0].v = coefV * (vs[0].y - offsetY);
    vs[0].color = fillColor;
    // 1
    vs[1].color = fillColor;
    // 2
    vs[2].color = fillColor;

    static const float linePts = 5.0f;
    int nSteps = toIntCeil(2 * H_PI * aMap * (1.0f / linePts));
    saturate(nSteps, 6, 720);
    float step = 2 * H_PI * (1.0f / nSteps);
    float lastX = cx, lastY = cy;
    bool lastValid = false;
    float st = 0;
    float ct = 1;
    float sstep = sin(step);
    float cstep = cos(step);
    for (float t = 0; t < 2 * H_PI + 0.5 * step; t += step)
    {
        float sta = st * aMap;
        float ctb = ct * bMap;
        float x = cx + c * sta + s * ctb;
        float y = cy + s * sta - c * ctb;
        if (lastValid)
        {
            // 1
            vs[1].x = lastX;
            vs[1].y = lastY;
            vs[1].u = coefU * (vs[1].x - offsetX);
            vs[1].v = coefV * (vs[1].y - offsetY);
            // 2
            vs[2].x = x;
            vs[2].y = y;
            vs[2].u = coefU * (vs[2].x - offsetX);
            vs[2].v = coefV * (vs[2].y - offsetY);
            GEngine->DrawPoly(mip, vs, n, _clipRect, NoClamp2D);
        }
        lastX = x;
        lastY = y;
        lastValid = true;
        float stnew = st * cstep + ct * sstep;
        float ctnew = ct * cstep - st * sstep;
        st = stnew;
        ct = ctnew;
    }
}

void CStaticMap::FillRectangle(Vector3Val position, float a, float b, float angle, PackedColor color, Texture* texture)
{
    DrawCoord posMap = WorldToScreen(position);
    float cx = posMap.x * _wScreen;
    float cy = posMap.y * _hScreen;

    const float invSizeLand = InvLandSize;
    float aMap = a * invSizeLand * _invScaleX * _wScreen;
    float bMap = b * invSizeLand * _invScaleY * _hScreen;

    float r = sqrt(Square(aMap) + Square(bMap));
    if (cx + r < _clipRect.x)
    {
        return;
    }
    if (cy + r < _clipRect.y)
    {
        return;
    }
    if (cx - r > _clipRect.x + _clipRect.w)
    {
        return;
    }
    if (cy - r > _clipRect.y + _clipRect.h)
    {
        return;
    }

    float angleRad = HDegree(angle);
    float s = sin(angleRad);
    float c = cos(angleRad);

    PackedColor fillColor = color;

    float coefU = 0;
    float coefV = 0;
    float offsetX = (_mapX + _x) * _wScreen;
    float offsetY = (_mapY + _y) * _hScreen;
    if (texture)
    {
        coefU = 1.0 / texture->AWidth();
        coefV = 1.0 / texture->AHeight();
    }
    else
    {
        // special case
        fillColor = ModAlpha(color, 0.5);
    }

    const int n = 4;
    Vertex2DPixel vs[n];
    // 0
    vs[0].x = cx + c * aMap + s * bMap;
    vs[0].y = cy + s * aMap - c * bMap;
    vs[0].u = coefU * (vs[0].x - offsetX);
    vs[0].v = coefV * (vs[0].y - offsetY);
    vs[0].color = fillColor;
    // 1
    vs[1].x = cx + c * aMap + s * (-bMap);
    vs[1].y = cy + s * aMap - c * (-bMap);
    vs[1].u = coefU * (vs[1].x - offsetX);
    vs[1].v = coefV * (vs[1].y - offsetY);
    vs[1].color = fillColor;
    // 2
    vs[2].x = cx + c * (-aMap) + s * (-bMap);
    vs[2].y = cy + s * (-aMap) - c * (-bMap);
    vs[2].u = coefU * (vs[2].x - offsetX);
    vs[2].v = coefV * (vs[2].y - offsetY);
    vs[2].color = fillColor;
    // 3
    vs[3].x = cx + c * (-aMap) + s * bMap;
    vs[3].y = cy + s * (-aMap) - c * bMap;
    vs[3].u = coefU * (vs[3].x - offsetX);
    vs[3].v = coefV * (vs[3].y - offsetY);
    vs[3].color = fillColor;

    MipInfo mip = GEngine->TextBank()->UseMipmap(texture, 0, 0);
    GEngine->DrawPoly(mip, vs, n, _clipRect, NoClamp2D);
}

// Arcade Map control

CStaticMapArcadeViewer::CStaticMapArcadeViewer(ControlsContainer* parent, int idc, const ParamEntry& cls,
                                               float scaleMin, float scaleMax, float scaleDefault)
    : CStaticMap(parent, idc, cls, scaleMin, scaleMax, scaleDefault)
{
    CreateObjectList();
    _sizeEmptyMarker = 32;
}

AIUnit* CStaticMapArcadeViewer::GetMyUnit()
{
    return nullptr;
}

AICenter* CStaticMapArcadeViewer::GetMyCenter()
{
    return nullptr;
}

void CStaticMapArcadeViewer::CreateObjectList()
{
    _objects.Clear();

    const VehicleType* type = GWorld->Preloaded(VTypeStatic);
    int i, n = GWorld->NBuildings();
    for (i = 0; i < n; i++)
    {
        Vehicle* veh = GWorld->GetBuilding(i);
        VehicleWithAI* vehai = dyn_cast<VehicleWithAI>(veh);
        if (!vehai)
        {
            continue;
        }
        if (!vehai->GetType()->IsKindOf(type))
        {
            continue;
        }

        _objects.Add(vehai);
    }
}

VehicleWithAI* CStaticMapArcadeViewer::FindObject(int id)
{
    int n = _objects.Size();
    for (int i = 0; i < n; i++)
    {
        VehicleWithAI* veh = _objects[i];
        if (!veh)
        {
            continue;
        }
        if (veh->ID() == id)
        {
            return veh;
        }
    }
    return nullptr;
}

// drawing

void CStaticMapArcadeViewer::DrawObjects() {}

void CStaticMapArcadeViewer::DrawExt(float alpha)
{
    CStaticMap::DrawExt(alpha);

#if _ENABLE_CHEATS
    if (!_show)
        return;
#endif

    DrawObjects();

    // update waypoint positions
    int i, n = _template->groups.Size();
    for (i = 0; i < n; i++)
    {
        ArcadeGroupInfo& gInfo = _template->groups[i];
        int j, m = gInfo.waypoints.Size();
        for (j = 0; j < m; j++)
        {
            ArcadeWaypointInfo& wpInfo = gInfo.waypoints[j];
            if (wpInfo.id >= 0)
            {
                int idGroup, idUnit;
                ArcadeUnitInfo* uInfo = _template->FindUnit(wpInfo.id, idGroup, idUnit);
                if (uInfo)
                {
                    wpInfo.position = uInfo->position;
                }
            }
        }
    }

    // draw synchronizations
    AutoArray<AutoArray<DrawCoord>> synchroMap;
    AutoArray<AutoArray<bool>> synchroSel;
    synchroMap.Resize(_template->nextSyncId);
    synchroSel.Resize(_template->nextSyncId);
    for (i = 0; i < n; i++)
    {
        ArcadeGroupInfo& gInfo = _template->groups[i];
        int j, m = gInfo.waypoints.Size();
        for (j = 0; j < m; j++)
        {
            ArcadeWaypointInfo& wpInfo = gInfo.waypoints[j];
            bool isActive1 = wpInfo.selected;
            DrawCoord ptMap = WorldToScreen(wpInfo.position);
            ptMap.x *= _wScreen;
            ptMap.y *= _hScreen;
            int p = wpInfo.synchronizations.Size();
            for (int l = 0; l < p; l++)
            {
                int sync = wpInfo.synchronizations[l];
                PoseidonAssert(sync >= 0);
                int o = synchroMap[sync].Size();
                for (int k = 0; k < o; k++)
                {
                    bool isActive2 = synchroSel[sync][k];

                    PackedColor color = isActive1 || isActive2 ? _colorSync : InactiveColor(_colorSync);
                    GLOB_ENGINE->DrawLine(Line2DPixel(ptMap.x, ptMap.y, synchroMap[sync][k].x, synchroMap[sync][k].y),
                                          color, color, _clipRect);
                }
                synchroMap[sync].Add(ptMap);
                synchroSel[sync].Add(isActive1);
            }
        }
    }
    for (i = 0; i < n; i++)
    {
        ArcadeGroupInfo& gInfo = _template->groups[i];
        int j, m = gInfo.sensors.Size();
        for (j = 0; j < m; j++)
        {
            ArcadeSensorInfo& sInfo = gInfo.sensors[j];
            bool isActive1 = sInfo.selected;
            DrawCoord ptMap = WorldToScreen(sInfo.position);
            ptMap.x *= _wScreen;
            ptMap.y *= _hScreen;
            int p = sInfo.synchronizations.Size();
            for (int l = 0; l < p; l++)
            {
                int sync = sInfo.synchronizations[l];
                PoseidonAssert(sync >= 0);
                int o = synchroMap[sync].Size();
                for (int k = 0; k < o; k++)
                {
                    bool isActive2 = synchroSel[sync][k];
                    PackedColor color = isActive1 || isActive2 ? _colorSync : InactiveColor(_colorSync);
                    GLOB_ENGINE->DrawLine(Line2DPixel(ptMap.x, ptMap.y, synchroMap[sync][k].x, synchroMap[sync][k].y),
                                          color, color, _clipRect);
                }
            }
        }
    }
    int j, m = _template->sensors.Size();
    for (j = 0; j < m; j++)
    {
        ArcadeSensorInfo& sInfo = _template->sensors[j];
        bool isActive1 = sInfo.selected;
        DrawCoord ptMap = WorldToScreen(sInfo.position);
        ptMap.x *= _wScreen;
        ptMap.y *= _hScreen;
        int p = sInfo.synchronizations.Size();
        for (int l = 0; l < p; l++)
        {
            int sync = sInfo.synchronizations[l];
            PoseidonAssert(sync >= 0);
            int k, o = synchroMap[sync].Size();
            for (k = 0; k < o; k++)
            {
                bool isActive2 = synchroSel[sync][k];
                PackedColor color = isActive1 || isActive2 ? _colorSync : InactiveColor(_colorSync);
                GLOB_ENGINE->DrawLine(Line2DPixel(ptMap.x, ptMap.y, synchroMap[sync][k].x, synchroMap[sync][k].y),
                                      color, color, _clipRect);
            }
        }
    }

    // draw units / waypoints
    for (i = 0; i < n; i++)
    {
        ArcadeGroupInfo& gInfo = _template->groups[i];
        bool activeGroup = false;
        for (int j = 0; j < gInfo.units.Size(); j++)
        {
            if (gInfo.units[j].selected)
            {
                activeGroup = true;
                break;
            }
        }
        if (!activeGroup)
        {
            for (int j = 0; j < gInfo.waypoints.Size(); j++)
            {
                if (gInfo.waypoints[j].selected)
                {
                    activeGroup = true;
                    break;
                }
            }
        }

        DrawCoord ptLeader(0, 0);
        bool leaderVisible = false;
        int j, m = gInfo.units.Size();
        for (j = 0; j < m; j++)
        {
            ArcadeUnitInfo& uInfo = gInfo.units[j];
            if (uInfo.leader)
            {
                ptLeader = WorldToScreen(uInfo.position);
                ptLeader.x *= _wScreen;
                ptLeader.y *= _hScreen;
                leaderVisible = HasFullRights() || uInfo.side == Glob.header.playerSide || uInfo.age != AAUnknown;
                break;
            }
        }

        // Draw waypoints
        if (leaderVisible)
        {
            DrawCoord pt1 = ptLeader;
            m = gInfo.waypoints.Size();
            for (j = 0; j < m; j++)
            {
                ArcadeWaypointInfo& wInfo = gInfo.waypoints[j];
                // draw line
                PackedColor color = activeGroup ? _colorActiveMission : InactiveColor(_colorActiveMission);
                DrawCoord pt2 = WorldToScreen(wInfo.position);
                pt2.x *= _wScreen;
                pt2.y *= _hScreen;
                GLOB_ENGINE->DrawLine(Line2DPixel(pt1.x, pt1.y, pt2.x, pt2.y), color, color, _clipRect);
                // arrow
                float dx = pt2.x - pt1.x;
                float dy = pt2.y - pt1.y;
                float size2 = Square(dx) + Square(dy);
                float invSize = InvSqrt(size2);
                dx *= 12.0 * invSize;
                dy *= 12.0 * invSize;
                float x = pt2.x - dx + 0.5 * dy;
                float y = pt2.y - dy - 0.5 * dx;
                GEngine->DrawLine(Line2DPixel(pt2.x, pt2.y, x, y), color, color, _clipRect);
                x = pt2.x - dx - 0.5 * dy;
                y = pt2.y - dy + 0.5 * dx;
                GEngine->DrawLine(Line2DPixel(pt2.x, pt2.y, x, y), color, color, _clipRect);
                pt1 = pt2;
            }
            for (j = 0; j < m; j++)
            {
                ArcadeWaypointInfo& wInfo = gInfo.waypoints[j];
                PackedColor color = wInfo.selected ? _infoWaypoint.color : InactiveColor(_infoWaypoint.color);
                DrawCoord pt = WorldToScreen(wInfo.position);
                DrawSign(_infoWaypoint.icon, color, pt, _infoWaypoint.size, _infoWaypoint.size, 0);
                if (wInfo.placement > 0)
                {
                    DrawEllipse(wInfo.position, wInfo.placement, wInfo.placement, 0, color);
                }
                if (wInfo.HasEffect())
                {
                    PackedColor color = activeGroup ? _colorCamera : InactiveColor(_colorCamera);
                    pt.x += 0.02;
                    DrawSign(_iconCamera, color, pt, 12, 12, 0);
                }
            }
        }

        // Draw sensors
        m = gInfo.sensors.Size();
        if (leaderVisible)
        {
            for (j = 0; j < m; j++)
            {
                ArcadeSensorInfo& sInfo = gInfo.sensors[j];
                PoseidonAssert(sInfo.idStatic == -1);
                PoseidonAssert(sInfo.idVehicle == -1);

                PackedColor color;
                if (sInfo.selected)
                {
                    color = _colorActiveGroup;
                }
                else
                {
                    color = _colorGroups;
                }

                // draw line
                DrawCoord pt = WorldToScreen(sInfo.position);
                pt.x *= _wScreen;
                pt.y *= _hScreen;
                GLOB_ENGINE->DrawLine(Line2DPixel(ptLeader.x, ptLeader.y, pt.x, pt.y), color, color, _clipRect);
            }
        }

        for (j = 0; j < m; j++)
        {
            ArcadeSensorInfo& sInfo = gInfo.sensors[j];

            PackedColor color = _colorSensor;
            if (!sInfo.selected)
            {
                color.SetA8(color.A8() / 2);
            }

            if (sInfo.rectangular)
            {
                DrawRectangle(sInfo.position, sInfo.a, sInfo.b, sInfo.angle, color);
            }
            else
            {
                DrawEllipse(sInfo.position, sInfo.a, sInfo.b, sInfo.angle, color);
            }
            DrawSign(_iconSensor, color, sInfo.position, 16, 16, 0);
        }

        // draw groups
        m = gInfo.units.Size();
        if (leaderVisible)
        {
            for (j = 0; j < m; j++)
            {
                ArcadeUnitInfo& uInfo = gInfo.units[j];
                if (uInfo.leader)
                {
                    continue;
                }
                PackedColor color = activeGroup ? _colorActiveGroup : _colorGroups;
                DrawCoord pt = WorldToScreen(uInfo.position);
                pt.x *= _wScreen;
                pt.y *= _hScreen;
                GLOB_ENGINE->DrawLine(Line2DPixel(ptLeader.x, ptLeader.y, pt.x, pt.y), color, color, _clipRect);
            }
        }

        // draw units
        for (j = 0; j < m; j++)
        {
            ArcadeUnitInfo& uInfo = gInfo.units[j];
            if (uInfo.side != Glob.header.playerSide && uInfo.age == AAUnknown && !HasFullRights())
            {
                continue;
            }

            DrawCoord pt = WorldToScreen(uInfo.position);
            DrawCoord pt1(pt.x * _wScreen, pt.y * _hScreen);

            bool activeUnit = uInfo.selected;

            int o = uInfo.markers.Size();
            for (int k = 0; k < o; k++)
            {
                ArcadeMarkerInfo* mInfo = _template->FindMarker(uInfo.markers[k]);
                if (mInfo)
                {
                    DrawCoord pt2 = WorldToScreen(mInfo->position);
                    pt2.x *= _wScreen;
                    pt2.y *= _hScreen;
                    PackedColor color = activeUnit ? _colorActiveGroup : _colorGroups;
                    GLOB_ENGINE->DrawLine(Line2DPixel(pt1.x, pt1.y, pt2.x, pt2.y), color, color, _clipRect);
                }
            }

            PackedColor color;
            if (uInfo.side == TEmpty)
            {
                color = _colorUnknown;
            }
            else if (uInfo.side == TCivilian || uInfo.side == TLogic)
            {
                color = _colorCivilian;
            }
            else
            {
                float friends = _template->intel.friends[Glob.header.playerSide][uInfo.side];
                if (friends >= 0.5)
                {
                    color = _colorFriendly;
                }
                else
                {
                    color = _colorEnemy;
                }
            }
            if (!activeUnit)
            {
                color.SetA8(color.A8() / 2);
            }

            const float invSizeLand = InvLandSize;
            float size = uInfo.size * invSizeLand * _invScaleX * 640;
            saturateMax(size, 16);

            if (uInfo.icon)
            {
                DrawSign(uInfo.icon, color, pt, size, size, HDegree(uInfo.azimut));
            }
            if (uInfo.placement > 0)
            {
                DrawEllipse(uInfo.position, uInfo.placement, uInfo.placement, 0, color);
            }
            switch (uInfo.player)
            {
                case APNonplayable:
                    break;
                case APPlayerCommander:
                case APPlayerDriver:
                case APPlayerGunner:
                    DrawSign(_iconPlayer, _colorMe, pt, 16, 16, 0);
                    break;
                case APPlayableC:
                case APPlayableD:
                case APPlayableG:
                case APPlayableCD:
                case APPlayableCG:
                case APPlayableDG:
                case APPlayableCDG:
                    DrawSign(_iconPlayer, _colorPlayable, pt, 16, 16, 0);
                    break;
            }
        }
    }

    // draw empty vehicles
    n = _template->emptyVehicles.Size();
    for (i = 0; i < n; i++)
    {
        ArcadeUnitInfo& uInfo = _template->emptyVehicles[i];
        if (uInfo.age == AAUnknown && !HasFullRights())
        {
            continue;
        }

        PoseidonAssert(uInfo.side == TEmpty);

        bool activeUnit = uInfo.selected;

        DrawCoord pt = WorldToScreen(uInfo.position);
        DrawCoord pt1(pt.x * _wScreen, pt.y * _hScreen);
        int o = uInfo.markers.Size();
        for (int k = 0; k < o; k++)
        {
            ArcadeMarkerInfo* mInfo = _template->FindMarker(uInfo.markers[k]);
            if (mInfo)
            {
                DrawCoord pt2 = WorldToScreen(mInfo->position);
                pt2.x *= _wScreen;
                pt2.y *= _hScreen;
                PackedColor color = activeUnit ? _colorActiveGroup : _colorGroups;
                GLOB_ENGINE->DrawLine(Line2DPixel(pt1.x, pt1.y, pt2.x, pt2.y), color, color, _clipRect);
            }
        }

        PackedColor color = _colorUnknown;
        if (!activeUnit)
        {
            color.SetA8(color.A8() / 2);
        }

        const float invSizeLand = InvLandSize;
        float size = uInfo.size * invSizeLand * _invScaleX * 640;
        saturateMax(size, 16);

        if (uInfo.icon)
        {
            DrawSign(uInfo.icon, color, pt, size, size, HDegree(uInfo.azimut));
        }
        if (uInfo.placement > 0)
        {
            DrawEllipse(uInfo.position, uInfo.placement, uInfo.placement, 0, color);
        }
    }

    // draw sensors
    n = _template->sensors.Size();
    for (i = 0; i < n; i++)
    {
        ArcadeSensorInfo& sInfo = _template->sensors[i];
        bool active = sInfo.selected;

        if (sInfo.idStatic >= 0)
        {
            Object* obj = FindObject(sInfo.idStatic);
            if (obj)
            {
                PackedColor color = active ? _colorActiveGroup : _colorGroups;
                DrawCoord pt1 = WorldToScreen(sInfo.position);
                pt1.x *= _wScreen;
                pt1.y *= _hScreen;
                DrawCoord pt2 = WorldToScreen(obj->Position());
                pt2.x *= _wScreen;
                pt2.y *= _hScreen;
                GLOB_ENGINE->DrawLine(Line2DPixel(pt1.x, pt1.y, pt2.x, pt2.y), color, color, _clipRect);
            }
        }
        else if (sInfo.idVehicle >= 0)
        {
            int indexUnit, indexGroup;
            ArcadeUnitInfo* uInfo = _template->FindUnit(sInfo.idVehicle, indexUnit, indexGroup);
            if (uInfo)
            {
                PackedColor color = active ? _colorActiveGroup : _colorGroups;
                DrawCoord pt1 = WorldToScreen(sInfo.position);
                pt1.x *= _wScreen;
                pt1.y *= _hScreen;
                DrawCoord pt2 = WorldToScreen(uInfo->position);
                pt2.x *= _wScreen;
                pt2.y *= _hScreen;
                GLOB_ENGINE->DrawLine(Line2DPixel(pt1.x, pt1.y, pt2.x, pt2.y), color, color, _clipRect);
            }
        }

        PackedColor color = _colorSensor;
        if (!active)
        {
            color.SetA8(color.A8() / 2);
        }

        if (sInfo.rectangular)
        {
            DrawRectangle(sInfo.position, sInfo.a, sInfo.b, sInfo.angle, color);
        }
        else
        {
            DrawEllipse(sInfo.position, sInfo.a, sInfo.b, sInfo.angle, color);
        }
        DrawSign(_iconSensor, color, sInfo.position, 16, 16, 0);
    }

    // draw markers
    if (GetMode() == IMMarkers)
    {
        n = _template->markers.Size();
        for (i = 0; i < n; i++)
        {
            ArcadeMarkerInfo& mInfo = _template->markers[i];

            PackedColor color = mInfo.color;
            if (!mInfo.selected)
            {
                color.SetA8(color.A8() / 2);
            }

            switch (mInfo.markerType)
            {
                case MTIcon:
                {
                    float size = mInfo.size;
                    if (size == 0)
                    {
                        size = _sizeEmptyMarker;
                    }
                    if (!mInfo.icon)
                    {
                        break;
                    }
                    DrawSign(mInfo.icon, color, mInfo.position, size * mInfo.a, size * mInfo.b,
                             mInfo.angle * (H_PI / 180.0), Localize(mInfo.text));
                }
                break;
                case MTRectangle:
                    FillRectangle(mInfo.position, mInfo.a, mInfo.b, mInfo.angle, color, mInfo.fill);
                    break;
                case MTEllipse:
                    FillEllipse(mInfo.position, mInfo.a, mInfo.b, mInfo.angle, color, mInfo.fill);
                    break;
            }
        }
    }

    DrawLabel(_infoMove, _colorInfoMove);
}

void CStaticMapArcade::DrawExt(float alpha)
{
    CStaticMapArcadeViewer::DrawExt(alpha);

#if _ENABLE_CHEATS
    if (!_show)
        return;
#endif

    if (_dragging)
    {
        DrawCoord pt;
        if (GetMode() == IMGroups)
        {
            switch (_infoClick._type)
            {
                case signArcadeUnit:
                    if (_infoClick._indexGroup >= 0)
                    {
                        PoseidonAssert(_infoClick._indexGroup < _template->groups.Size());
                        ArcadeGroupInfo& gInfo = _template->groups[_infoClick._indexGroup];
                        PoseidonAssert(_infoClick._index < gInfo.units.Size());
                        ArcadeUnitInfo& uInfo = gInfo.units[_infoClick._index];
                        pt = WorldToScreen(uInfo.position);
                    }
                    else
                    {
                        ArcadeUnitInfo& uInfo = _template->emptyVehicles[_infoClick._index];
                        pt = WorldToScreen(uInfo.position);
                    }
                    break;
                case signArcadeSensor:
                    if (_infoClick._indexGroup >= 0)
                    {
                        ArcadeGroupInfo& gInfo = _template->groups[_infoClick._indexGroup];
                        ArcadeSensorInfo& sInfo = gInfo.sensors[_infoClick._index];
                        pt = WorldToScreen(sInfo.position);
                    }
                    else
                    {
                        ArcadeSensorInfo& sInfo = _template->sensors[_infoClick._index];
                        pt = WorldToScreen(sInfo.position);
                    }
                    break;
                case signArcadeMarker:
                {
                    ArcadeMarkerInfo& mInfo = _template->markers[_infoClick._index];
                    pt = WorldToScreen(mInfo.position);
                }
                break;
                case signStatic:
                {
                    Object* obj = _infoClick._id;
                    PoseidonAssert(obj);
                    if (!obj)
                    {
                        return;
                    }
                    pt = WorldToScreen(obj->Position());
                }
                break;
                default:
                    return;
            }
        }
        else if (GetMode() == IMSynchronize)
        {
            switch (_infoClick._type)
            {
                case signArcadeWaypoint:
                {
                    ArcadeGroupInfo& gInfo = _template->groups[_infoClick._indexGroup];
                    ArcadeWaypointInfo& wInfo = gInfo.waypoints[_infoClick._index];
                    pt = WorldToScreen(wInfo.position);
                }
                break;
                case signArcadeSensor:
                    if (_infoClick._indexGroup >= 0)
                    {
                        ArcadeGroupInfo& gInfo = _template->groups[_infoClick._indexGroup];
                        ArcadeSensorInfo& sInfo = gInfo.sensors[_infoClick._index];
                        pt = WorldToScreen(sInfo.position);
                    }
                    else
                    {
                        ArcadeSensorInfo& sInfo = _template->sensors[_infoClick._index];
                        pt = WorldToScreen(sInfo.position);
                    }
                    break;
                default:
                    return;
            }
        }
        else
        {
            return;
        }

        DrawCoord pt2 = WorldToScreen(_special);

        GEngine->DrawLine(Line2DPixel(pt.x * _wScreen, pt.y * _hScreen, pt2.x * _wScreen, pt2.y * _hScreen),
                          _colorDragging, _colorDragging, _clipRect);
    }
    else if (_selecting)
    {
        PackedColor color = PackedColor(Color(0, 1, 0, 1));
        DrawCoord pt1 = WorldToScreen(_special);
        DrawCoord pt2 = WorldToScreen(_lastPos);
        GEngine->DrawLine(Line2DPixel(pt1.x * _wScreen, pt1.y * _hScreen, pt2.x * _wScreen, pt1.y * _hScreen), color,
                          color, _clipRect);
        GEngine->DrawLine(Line2DPixel(pt2.x * _wScreen, pt1.y * _hScreen, pt2.x * _wScreen, pt2.y * _hScreen), color,
                          color, _clipRect);
        GEngine->DrawLine(Line2DPixel(pt2.x * _wScreen, pt2.y * _hScreen, pt1.x * _wScreen, pt2.y * _hScreen), color,
                          color, _clipRect);
        GEngine->DrawLine(Line2DPixel(pt1.x * _wScreen, pt2.y * _hScreen, pt1.x * _wScreen, pt1.y * _hScreen), color,
                          color, _clipRect);
    }
}

// simulation

void CStaticMapArcadeViewer::Center()
{
    ArcadeUnitInfo* uInfo = _template->FindPlayer();
    Point3 pt = _defaultCenter;
    if (uInfo)
    {
        pt = uInfo->position;
    }
    CStaticMap::Center(pt);
}

Vector3 GClipboardPos;

void CStaticMapArcade::ClipboardDelete()
{
    for (int i = 0; i < _template->emptyVehicles.Size();)
    {
        ArcadeUnitInfo& uInfo = _template->emptyVehicles[i];
        if (uInfo.selected)
        {
            _template->emptyVehicles.Delete(i);
        }
        else
        {
            i++;
        }
    }
    for (int i = 0; i < _template->sensors.Size();)
    {
        ArcadeSensorInfo& sInfo = _template->sensors[i];
        if (sInfo.selected)
        {
            _template->sensors.Delete(i);
        }
        else
        {
            i++;
        }
    }
    for (int i = 0; i < _template->markers.Size();)
    {
        ArcadeMarkerInfo& mInfo = _template->markers[i];
        if (mInfo.selected)
        {
            _template->markers.Delete(i);
        }
        else
        {
            i++;
        }
    }
    for (int i = 0; i < _template->groups.Size();)
    {
        ArcadeGroupInfo& gInfo = _template->groups[i];
        for (int j = 0; j < gInfo.units.Size();)
        {
            ArcadeUnitInfo& uInfo = gInfo.units[j];
            if (uInfo.selected)
            {
                gInfo.units.Delete(j);
            }
            else
            {
                j++;
            }
        }
        if (gInfo.units.Size() == 0)
        {
            _template->groups.Delete(i);
            continue;
        }
        for (int j = 0; j < gInfo.sensors.Size();)
        {
            ArcadeSensorInfo& sInfo = gInfo.sensors[j];
            if (sInfo.selected)
            {
                gInfo.sensors.Delete(j);
            }
            else
            {
                j++;
            }
        }
        for (int j = 0; j < gInfo.waypoints.Size();)
        {
            ArcadeWaypointInfo& wInfo = gInfo.waypoints[j];
            if (wInfo.selected)
            {
                gInfo.waypoints.Delete(j);
            }
            else
            {
                j++;
            }
        }
        SelectLeader(gInfo);
        i++;
    }
    _template->CheckSynchro();
    _template->Compact();
}

void CStaticMapArcade::ClipboardCut()
{
    ClipboardCopy();
    ClipboardDelete();
}

void CStaticMapArcade::ClipboardCopy()
{
    GetGClipboard().Clear();
    float mouseX = 0.5 + InputSubsystem::Instance().GetCursorX() * 0.5;
    float mouseY = 0.5 + InputSubsystem::Instance().GetCursorY() * 0.5;
    GClipboardPos = ScreenToWorld(DrawCoord(mouseX, mouseY));
    for (int i = 0; i < _template->emptyVehicles.Size(); i++)
    {
        ArcadeUnitInfo& uInfo = _template->emptyVehicles[i];
        if (uInfo.selected)
        {
            GetGClipboard().emptyVehicles.Add(uInfo);
        }
    }
    for (int i = 0; i < _template->sensors.Size(); i++)
    {
        ArcadeSensorInfo& sInfo = _template->sensors[i];
        if (sInfo.selected)
        {
            GetGClipboard().sensors.Add(sInfo);
        }
    }
    for (int i = 0; i < _template->markers.Size(); i++)
    {
        ArcadeMarkerInfo& mInfo = _template->markers[i];
        if (mInfo.selected)
        {
            GetGClipboard().markers.Add(mInfo);
        }
    }
    for (int i = 0; i < _template->groups.Size(); i++)
    {
        ArcadeGroupInfo& gInfo = _template->groups[i];
        bool grpSelected = false;
        for (int j = 0; j < gInfo.units.Size(); j++)
        {
            ArcadeUnitInfo& uInfo = gInfo.units[j];
            if (uInfo.selected)
            {
                grpSelected = true;
                break;
            }
        }
        if (!grpSelected)
        {
            continue;
        }
        int index = GetGClipboard().groups.Add();
        ArcadeGroupInfo& gDest = GetGClipboard().groups[index];
        for (int j = 0; j < gInfo.units.Size(); j++)
        {
            ArcadeUnitInfo& uInfo = gInfo.units[j];
            if (uInfo.selected)
            {
                gDest.units.Add(uInfo);
            }
        }
        for (int j = 0; j < gInfo.sensors.Size(); j++)
        {
            ArcadeSensorInfo& sInfo = gInfo.sensors[j];
            if (sInfo.selected)
            {
                gDest.sensors.Add(sInfo);
            }
        }
        for (int j = 0; j < gInfo.waypoints.Size(); j++)
        {
            ArcadeWaypointInfo& wInfo = gInfo.waypoints[j];
            if (wInfo.selected)
            {
                gDest.waypoints.Add(wInfo);
            }
        }
        SelectLeader(gDest);
    }
    GetGClipboard().nextVehId = _template->nextVehId;
    GetGClipboard().nextSyncId = _template->nextSyncId;
    GetGClipboard().CheckSynchro();
    GetGClipboard().Compact();
}

void CStaticMapArcade::ClipboardPaste()
{
    _template->ClearSelection();

    float mouseX = 0.5 + InputSubsystem::Instance().GetCursorX() * 0.5;
    float mouseY = 0.5 + InputSubsystem::Instance().GetCursorY() * 0.5;
    Vector3 pos = ScreenToWorld(DrawCoord(mouseX, mouseY));

    // offset inserted objects
    GetGClipboard().AddOffset(pos - GClipboardPos);
    GClipboardPos = pos;

    _template->Merge(GetGClipboard());
}

void CStaticMapArcade::ClipboardPasteAbsolute()
{
    _template->ClearSelection();
    _template->Merge(GetGClipboard());
}

InsertMode CStaticMapArcadeViewer::GetMode()
{
    return IMUnits;
}

InsertMode CStaticMapArcade::GetMode()
{
    if (_parent->IDD() == IDD_ARCADE_MAP)
    {
        DisplayArcadeMap* disp = dynamic_cast<DisplayArcadeMap*>(_parent);
        return disp->_mode;
    }
    else if (_parent->IDD() == IDD_INTEL_GETREADY)
    {
        return IMWaypoints;
    }
    else
    {
        return IMUnits;
    }
}

bool CStaticMapArcade::IsAdvanced()
{
    PoseidonAssert(_parent->IDD() == IDD_ARCADE_MAP);
    if (_parent->IDD() == IDD_ARCADE_MAP)
    {
        DisplayArcadeMap* disp = dynamic_cast<DisplayArcadeMap*>(_parent);
        return disp->_advanced;
    }
    else
    {
        return false;
    }
}

ArcadeUnitInfo* CStaticMapArcade::GetLastUnit()
{
    PoseidonAssert(_parent->IDD() == IDD_ARCADE_MAP);
    if (_parent->IDD() == IDD_ARCADE_MAP)
    {
        DisplayArcadeMap* disp = dynamic_cast<DisplayArcadeMap*>(_parent);
        return &disp->_lastUnit;
    }
    else
    {
        return nullptr;
    }
}

bool CStaticMapArcade::HasRight(int ig)
{
    switch (_editRights)
    {
        case ERNone:
            return false;
        case ERGroupWP:
        {
            if (ig < 0)
            {
                return false;
            }
            ArcadeGroupInfo& gInfo = _template->groups[ig];
            int i, n = gInfo.units.Size();
            for (i = 0; i < n; i++)
            {
                switch (gInfo.units[i].player)
                {
                    case APPlayerCommander:
                    case APPlayerDriver:
                    case APPlayerGunner:
                        return true;
                }
            }
            return false;
        }
        case ERSideWP:
        {
            if (ig < 0)
            {
                return false;
            }
            ArcadeGroupInfo& gInfo = _template->groups[ig];
            return gInfo.side == Glob.header.playerSide;
        }
        case ERFull:
            return true;
            ;
        default:
            Fail("Edit rights");
            return false;
    }
}

bool CStaticMapArcadeViewer::OnKeyDown(unsigned nChar, unsigned nRepCnt, unsigned nFlags)
{
    switch (nChar)
    {
        case SDLK_F1:
        {
            if (_parent->IDD() != IDD_INTEL_GETREADY)
            {
                break;
            }
            CToolBox* box = dynamic_cast<CToolBox*>(_parent->GetCtrl(IDC_GETREADY_EDITMODE));
            if (!box)
            {
                break;
            }
            box->SetCurSel(0);
            return true;
        }
        case SDLK_F2:
        {
            if (_parent->IDD() != IDD_INTEL_GETREADY)
            {
                break;
            }
            CToolBox* box = dynamic_cast<CToolBox*>(_parent->GetCtrl(IDC_GETREADY_EDITMODE));
            if (!box)
            {
                break;
            }
            box->SetCurSel(1);
            return true;
        }
    }
    return CStaticMap::OnKeyDown(nChar, nRepCnt, nFlags);
}

bool CStaticMapArcade::OnKeyDown(unsigned nChar, unsigned nRepCnt, unsigned nFlags)
{
    auto& input = InputSubsystem::Instance();
    switch (nChar)
    {
        case SDLK_KP_5:
        {
            const float sizeLand = LandGrid * LandRange;
            const float invSizeLand = 1.0 / sizeLand;
            Vector3 curPos = GetCenter();
            ArcadeUnitInfo* uInfo = _template->FindPlayer();
            Vector3 pos = Vector3(0.5 * sizeLand, 0, 0.5 * sizeLand);
            if (uInfo)
            {
                pos = uInfo->position;
            }
            float diff = curPos.Distance(pos);
            float scale = 2.0 * diff * invSizeLand;
            if (scale > _scaleX)
            {
                float time = log(scale / _scaleX);
                ClearAnimation();
                AddAnimationPhase(time, scale, curPos);
                AddAnimationPhase(1.0, scale, pos);
                AddAnimationPhase(time, _scaleX, pos);
                CreateInterpolator();
            }
            else
            {
                float time = diff * invSizeLand * _invScaleX;
                ClearAnimation();
                AddAnimationPhase(time, _scaleX, pos);
                CreateInterpolator();
            }
        }
            return true;
        case SDLK_F1:
        {
            if (_parent->IDD() != IDD_ARCADE_MAP)
            {
                break;
            }

            DisplayArcadeMap* disp = dynamic_cast<DisplayArcadeMap*>(_parent);
            disp->_mode = IMUnits;
            CToolBox* box = dynamic_cast<CToolBox*>(_parent->GetCtrl(IDC_ARCMAP_MODE));
            PoseidonAssert(box);
            box->SetCurSel(disp->_mode);
            return true;
        }
        case SDLK_F2:
        {
            if (_parent->IDD() != IDD_ARCADE_MAP)
            {
                break;
            }

            DisplayArcadeMap* disp = dynamic_cast<DisplayArcadeMap*>(_parent);
            disp->_mode = IMGroups;
            CToolBox* box = dynamic_cast<CToolBox*>(_parent->GetCtrl(IDC_ARCMAP_MODE));
            PoseidonAssert(box);
            box->SetCurSel(disp->_mode);
            return true;
        }
        case SDLK_F3:
        {
            if (_parent->IDD() != IDD_ARCADE_MAP)
            {
                break;
            }

            DisplayArcadeMap* disp = dynamic_cast<DisplayArcadeMap*>(_parent);
            disp->_mode = IMSensors;
            CToolBox* box = dynamic_cast<CToolBox*>(_parent->GetCtrl(IDC_ARCMAP_MODE));
            PoseidonAssert(box);
            box->SetCurSel(disp->_mode);
            return true;
        }
        case SDLK_F4:
        {
            if (_parent->IDD() != IDD_ARCADE_MAP)
            {
                break;
            }

            DisplayArcadeMap* disp = dynamic_cast<DisplayArcadeMap*>(_parent);
            disp->_mode = IMWaypoints;
            CToolBox* box = dynamic_cast<CToolBox*>(_parent->GetCtrl(IDC_ARCMAP_MODE));
            PoseidonAssert(box);
            box->SetCurSel(disp->_mode);
            return true;
        }
        case SDLK_F5:
        {
            if (_parent->IDD() != IDD_ARCADE_MAP)
            {
                break;
            }

            DisplayArcadeMap* disp = dynamic_cast<DisplayArcadeMap*>(_parent);
            disp->_mode = IMSynchronize;
            CToolBox* box = dynamic_cast<CToolBox*>(_parent->GetCtrl(IDC_ARCMAP_MODE));
            PoseidonAssert(box);
            box->SetCurSel(disp->_mode);
            return true;
        }
        case SDLK_F6:
        {
            if (_parent->IDD() != IDD_ARCADE_MAP)
            {
                break;
            }

            DisplayArcadeMap* disp = dynamic_cast<DisplayArcadeMap*>(_parent);
            disp->_mode = IMMarkers;
            CToolBox* box = dynamic_cast<CToolBox*>(_parent->GetCtrl(IDC_ARCMAP_MODE));
            PoseidonAssert(box);
            box->SetCurSel(disp->_mode);
            return true;
        }
        case SDLK_DELETE:
        {
            if (input.IsKeyDown(SDL_SCANCODE_LSHIFT) || input.GetKey(SDL_SCANCODE_RSHIFT))
            {
                if (HasFullRights())
                {
                    ClipboardCut();
                }
            }
            else
            {
                switch (_infoMove._type)
                {
                    case signArcadeUnit:
                        if (HasFullRights())
                        {
                            _template->UnitDelete(_infoMove._indexGroup, _infoMove._index);
                            _infoClick._type = signNone;
                            _infoClickCandidate._type = signNone;
                            _infoMove._type = signNone;
                            _dragging = false;
                            _selecting = false;
                        }
                        break;
                    case signArcadeWaypoint:
                        if (HasRight(_infoMove._indexGroup))
                        {
                            _template->WaypointDelete(_infoMove._indexGroup, _infoMove._index);
                            _infoClick._type = signNone;
                            _infoClickCandidate._type = signNone;
                            _infoMove._type = signNone;
                            _dragging = false;
                            _selecting = false;
                        }
                        break;
                    case signArcadeSensor:
                        if (HasFullRights())
                        {
                            _template->SensorDelete(_infoMove._indexGroup, _infoMove._index);
                            _infoClick._type = signNone;
                            _infoClickCandidate._type = signNone;
                            _infoMove._type = signNone;
                            _dragging = false;
                            _selecting = false;
                        }
                        break;
                    case signArcadeMarker:
                        if (HasFullRights())
                        {
                            _template->MarkerDelete(_infoMove._index);
                            _infoClick._type = signNone;
                            _infoClickCandidate._type = signNone;
                            _infoMove._type = signNone;
                            _dragging = false;
                            _selecting = false;
                        }
                        break;
                }
            }

            if (_parent->IDD() == IDD_ARCADE_MAP)
            {
                DisplayArcadeMap* disp = dynamic_cast<DisplayArcadeMap*>(_parent);
                PoseidonAssert(disp);
                disp->ShowButtons();
            }
            return true;
        }
        case SDLK_INSERT:
            if (input.IsKeyDown(SDL_SCANCODE_LCTRL) || input.IsKeyDown(SDL_SCANCODE_RCTRL))
            {
                if (HasFullRights())
                {
                    ClipboardCopy();
                    return true;
                }
            }
            else if (input.IsKeyDown(SDL_SCANCODE_LSHIFT) || input.GetKey(SDL_SCANCODE_RSHIFT))
            {
                if (HasFullRights())
                {
                    ClipboardPaste();
                    return true;
                }
            }
            break;
        case 'x':
            if (input.IsKeyDown(SDL_SCANCODE_LCTRL) || input.IsKeyDown(SDL_SCANCODE_RCTRL))
            {
                if (HasFullRights())
                {
                    ClipboardCut();
                    return true;
                }
            }
            break;
        case 'c':
            if (input.IsKeyDown(SDL_SCANCODE_LCTRL) || input.IsKeyDown(SDL_SCANCODE_RCTRL))
            {
                if (HasFullRights())
                {
                    ClipboardCopy();
                    return true;
                }
            }
            break;
        case 'v':
            if (input.IsKeyDown(SDL_SCANCODE_LCTRL) || input.IsKeyDown(SDL_SCANCODE_RCTRL))
            {
                if (HasFullRights())
                {
                    if (input.IsKeyDown(SDL_SCANCODE_LSHIFT) || input.GetKey(SDL_SCANCODE_RSHIFT))
                    {
                        ClipboardPasteAbsolute();
                    }
                    else
                    {
                        ClipboardPaste();
                    }
                    return true;
                }
            }
            break;
    }
    return CStaticMapArcadeViewer::OnKeyDown(nChar, nRepCnt, nFlags);
}

void CStaticMapArcade::ProcessCheats()
{
#if _ENABLE_CHEATS
    auto& input = InputSubsystem::Instance();
    if (input.GetActionToDo(UAFire))
    {
        QOFStream out;

        float mouseX = 0.5 + input.GetCursorX() * 0.5;
        float mouseY = 0.5 + input.GetCursorY() * 0.5;
        Vector3 pos = ScreenToWorld(DrawCoord(mouseX, mouseY));

        char buf[1024];
        snprintf(buf, sizeof(buf), "[%.3f,%.3f,%.3f]\r\n", pos.X(), pos.Z(), pos.Y());
        out.write(buf, strlen(buf));

        out.export_clip("clipboard.txt");
    }
#endif

    CStaticMapArcadeViewer::ProcessCheats();
}

static int CharRPos(const char* t, char c)
{
    const char* cp = strrchr(t, c);
    if (!cp)
    {
        return 0;
    }
    return cp - t;
}

static RString ShortExpress(RString ex)
{
    if (ex.GetLength() < 32)
    {
        return ex;
    }
    // try to shorten it in some reasonable way first
    RString shortEx = ex.Substring(0, 29);
    int cp;
    if ((cp = CharRPos(shortEx, ';')) > 22)
    {
        return ex.Substring(0, cp + 1) + RString("...");
    }
    return ex.Substring(0, 26) + RString("...");
}

static RString CatWords(RString cond, RString cond2)
{
    if (cond.GetLength() > 0 && cond2.GetLength() > 0)
    {
        return cond + RString(" ") + cond2;
    }
    else
    {
        return cond + cond2;
    }
}

void CStaticMapArcadeViewer::DrawLabel(struct SignInfo& info, PackedColor color)
{
    RString text;
    // additional text
    AutoArray<RString> addText;

    Point3 pos;
    switch (info._type)
    {
        case signNone:
            break;
        case signArcadeUnit:
        {
            ArcadeUnitInfo* uInfo = nullptr;
            if (info._indexGroup == -1)
            {
                // empty vehicle
                uInfo = &_template->emptyVehicles[info._index];
            }
            else
            {
                ArcadeGroupInfo& gInfo = _template->groups[info._indexGroup];
                uInfo = &gInfo.units[info._index];
            }
            text = Pars >> "CfgVehicles" >> uInfo->vehicle >> "displayName";
            if (uInfo->name && uInfo->name.GetLength() > 0)
            {
                text = text + RString(" - ");
                text = text + uInfo->name;
            }
            pos = uInfo->position;

            RString cond;
            if (uInfo->presence < 0.9999f)
            {
                cond = Format("%d %%", toInt(uInfo->presence * 100));
            }
            if (strcmp(uInfo->presenceCondition, "true"))
            {
                cond = CatWords(cond, RString("? ") + ShortExpress(uInfo->presenceCondition));
            }
            if (cond.GetLength() > 0)
            {
                addText.Add(cond);
            }
            if (uInfo->init.GetLength() > 0)
            {
                addText.Add(ShortExpress(uInfo->init));
            }

            break;
        }
        case signArcadeWaypoint:
        {
            PoseidonAssert(info._indexGroup >= 0);
            PoseidonAssert(info._indexGroup < _template->groups.Size());
            ArcadeGroupInfo& gInfo = _template->groups[info._indexGroup];
            PoseidonAssert(info._index < gInfo.waypoints.Size());
            ArcadeWaypointInfo& wInfo = gInfo.waypoints[info._index];
            text = LocalizeString(IDS_AC_MOVE + wInfo.type - ACMOVE);

            ArcadeUnitInfo* leader = nullptr;
            for (int i = 0; i < gInfo.units.Size(); i++)
            {
                ArcadeUnitInfo& uInfo = gInfo.units[i];
                if (uInfo.leader)
                {
                    leader = &uInfo;
                    break;
                }
            }
            if (leader && leader->name.GetLength() > 0)
            {
                text = text + RString(" (") + leader->name + RString(")");
            }

            pos = wInfo.position;
            // if (wInfo.type!=ACMOVE) addText.Add(LocalizeString(IDS_AC_MOVE + wInfo.type - ACMOVE));
            if (wInfo.combatMode >= 0)
            {
                addText.Add(LocalizeString(IDS_IGNORE + wInfo.combatMode));
            }
            if (wInfo.combat > CMUnchanged)
            {
                addText.Add(LocalizeString(IDS_COMBAT_UNCHANGED + wInfo.combat));
            }
            if (wInfo.formation >= 0)
            {
                addText.Add(LocalizeString(IDS_COLUMN + wInfo.formation));
            }
            if (wInfo.speed > SpeedUnchanged)
            {
                addText.Add(LocalizeString(IDS_SPEED_UNCHANGED + wInfo.speed));
            }
            if (wInfo.expActiv.GetLength() > 0)
            {
                addText.Add(ShortExpress(wInfo.expActiv));
            }
            break;
        }
        case signArcadeSensor:
        {
            ArcadeSensorInfo* sInfo = nullptr;
            if (info._indexGroup < 0)
            {
                sInfo = &_template->sensors[info._index];
            }
            else
            {
                ArcadeGroupInfo& gInfo = _template->groups[info._indexGroup];
                sInfo = &gInfo.sensors[info._index];
            }
            if (sInfo->text && sInfo->text.GetLength() > 0)
            {
                text = sInfo->text;
            }
            else
            {
                text = LocalizeString(IDS_SENSOR);
            }
            if (strcmp(sInfo->expCond, "this"))
            {
                addText.Add(RString("? ") + ShortExpress(sInfo->expCond));
            }
            else if (sInfo->activationBy > ASANone && sInfo->activationBy < ASAStatic)
            {
                const static char* act[] = {
                    "STR_SENSORACTIV_NONE",
                    "STR_EAST",
                    "STR_WEST",
                    "STR_GUERRILA",
                    "STR_CIVILIAN",
                    "STR_LOGIC",
                    "STR_SENSORACTIV_ANYBODY",
                    "STR_SENSORACTIV_ALPHA",
                    "STR_SENSORACTIV_BRAVO",
                    "STR_SENSORACTIV_CHARLIE",
                    "STR_SENSORACTIV_DELTA",
                    "STR_SENSORACTIV_ECHO",
                    "STR_SENSORACTIV_FOXTROT",
                    "STR_SENSORACTIV_GOLF",
                    "STR_SENSORACTIV_HOTEL",
                    "STR_SENSORACTIV_INDIA",
                    "STR_SENSORACTIV_JULIET",
                };
                RString at = LocalizeString(act[sInfo->activationBy]);
                addText.Add(at);
            }
            if (sInfo->type > ASTNone)
            {
                addText.Add(LocalizeString(IDS_SENSORTYPE_NONE + sInfo->type));
            }

            if (sInfo->expActiv.GetLength() > 0)
            {
                addText.Add(ShortExpress(sInfo->expActiv));
            }
            pos = sInfo->position;
            break;
        }
        case signArcadeMarker:
        {
            ArcadeMarkerInfo& mInfo = _template->markers[info._index];
            text = mInfo.name;
            pos = mInfo.position;
            break;
        }
        case signStatic:
        {
            Object* obj = info._id;
            VehicleWithAI* veh = dyn_cast<VehicleWithAI>(obj);
            if (veh)
            {
                text = veh->GetType()->GetDisplayName();
                pos = veh->Position();
            }
        }
        break;
    }

    if (text.GetLength() > 0)
    {
        DrawCoord posMap = WorldToScreen(pos);

        // place label
        float w = GEngine->GetTextWidth(_sizeLabel, _fontLabel, text);
        float h = _sizeLabel;
        float ha = 0;
        float wa = 0;
        float offset = 0.01;

        const int maxAddTexts = 6;
        if (addText.Size() > maxAddTexts)
        {
            addText.Resize(maxAddTexts);
            addText[maxAddTexts - 1] = "...";
        }
        for (int i = 0; i < addText.Size(); i++)
        {
            float wt = GEngine->GetTextWidth(_sizeLabel, _fontLabel, addText[i]);
            saturateMax(wa, wt);
            ha += _sizeLabel;
        }

        posMap.x -= 0.5 * w;
        if (posMap.y < 0.7)
        {
            posMap.y += offset;
        }
        else
        {
            posMap.y -= offset + h + ha;
        }

        Texture* texture = GLOB_SCENE->Preloaded(TextureWhite);
        MipInfo mip = GEngine->TextBank()->UseMipmap(texture, 0, 0);
        GEngine->Draw2D(mip, _colorLabelBackground,
                        Rect2DPixel(posMap.x * _wScreen, posMap.y * _hScreen, w * _wScreen, h * _hScreen), _clipRect);
        if (ha > 0)
        {
            GEngine->Draw2D(mip, PackedColorRGB(_colorLabelBackground, _colorLabelBackground.A8() * 3 / 4),
                            Rect2DPixel(posMap.x * _wScreen, (posMap.y + h) * _hScreen, wa * _wScreen, ha * _hScreen),
                            _clipRect);
        }

        GEngine->DrawText(posMap, _sizeLabel, Rect2DFloat(_x, _y, _w, _h), _fontLabel, color, text);
        for (int i = 0; i < addText.Size(); i++)
        {
            GLOB_ENGINE->DrawText(Point2DFloat(posMap.x, posMap.y + h + _sizeLabel * i), _sizeLabel,
                                  Rect2DFloat(_x, _y, _w, _h), _fontLabel, color, addText[i]);
        }
    }
}

SignInfo CStaticMapArcadeViewer::FindSign(float x, float y)
{
    bool reverse = InputSubsystem::Instance().IsKeyDown(SDL_SCANCODE_LSHIFT) ||
                   InputSubsystem::Instance().GetKey(SDL_SCANCODE_RSHIFT) > 0;

    struct SignInfo info;
    info._type = signNone;
    info._unit = nullptr;
    info._id = nullptr;

    Point3 pt = ScreenToWorld(DrawCoord(x, y));
    const float sizeLand = LandGrid * LandRange;
    float dist, minDist = 0.02 * sizeLand * _scaleX;
    minDist *= minDist;

    int i, n = _template->groups.Size();
    if (!reverse) // search in waypoints first
    {
        for (i = 0; i < n; i++)
        {
            ArcadeGroupInfo& gInfo = _template->groups[i];
            int j, m;
            m = gInfo.waypoints.Size();
            for (j = 0; j < m; j++)
            {
                ArcadeWaypointInfo& wInfo = gInfo.waypoints[j];
                dist = (pt - wInfo.position).SquareSizeXZ();
                if (dist < minDist)
                {
                    minDist = dist;
                    info._type = signArcadeWaypoint;
                    info._indexGroup = i;
                    info._index = j;
                }
            }
        }
    }

    // units, flags and sensors in groups
    for (i = 0; i < n; i++)
    {
        ArcadeGroupInfo& gInfo = _template->groups[i];
        int j, m;
        m = gInfo.units.Size();
        for (j = 0; j < m; j++)
        {
            ArcadeUnitInfo& uInfo = gInfo.units[j];
            if (uInfo.side != Glob.header.playerSide && uInfo.age == AAUnknown && !HasFullRights())
            {
                continue;
            }

            dist = (pt - uInfo.position).SquareSizeXZ();
            if (dist < minDist)
            {
                minDist = dist;
                info._type = signArcadeUnit;
                info._indexGroup = i;
                info._index = j;
            }
        }

        m = gInfo.sensors.Size();
        for (j = 0; j < m; j++)
        {
            ArcadeSensorInfo& sInfo = gInfo.sensors[j];
            dist = (pt - sInfo.position).SquareSizeXZ();
            if (dist < minDist)
            {
                minDist = dist;
                info._type = signArcadeSensor;
                info._indexGroup = i;
                info._index = j;
            }
        }
    }

    // empty units
    n = _template->emptyVehicles.Size();
    for (i = 0; i < n; i++)
    {
        ArcadeUnitInfo& uInfo = _template->emptyVehicles[i];
        if (uInfo.age == AAUnknown && !HasFullRights())
        {
            continue;
        }

        dist = (pt - uInfo.position).SquareSizeXZ();
        if (dist < minDist)
        {
            minDist = dist;
            info._type = signArcadeUnit;
            info._indexGroup = -1;
            info._index = i;
        }
    }

    // sensors
    n = _template->sensors.Size();
    for (i = 0; i < n; i++)
    {
        ArcadeSensorInfo& sInfo = _template->sensors[i];
        dist = (pt - sInfo.position).SquareSizeXZ();
        if (dist < minDist)
        {
            minDist = dist;
            info._type = signArcadeSensor;
            info._indexGroup = -1;
            info._index = i;
        }
    }

    // markers
    n = _template->markers.Size();
    for (i = 0; i < n; i++)
    {
        ArcadeMarkerInfo& mInfo = _template->markers[i];
        dist = (pt - mInfo.position).SquareSizeXZ();
        if (dist < minDist)
        {
            minDist = dist;
            info._type = signArcadeMarker;
            info._index = i;
        }
    }

    // buildings
    n = _objects.Size();
    for (i = 0; i < n; i++)
    {
        VehicleWithAI* veh = _objects[i];
        if (!veh)
        {
            continue;
        }
        dist = (pt - veh->Position()).SquareSizeXZ();
        if (dist < minDist)
        {
            minDist = dist;
            info._type = signStatic;
            info._id = veh;
        }
    }

    if (reverse) // search in waypoints last
    {
        n = _template->groups.Size();
        for (i = 0; i < n; i++)
        {
            ArcadeGroupInfo& gInfo = _template->groups[i];
            int j, m;
            m = gInfo.waypoints.Size();
            for (j = 0; j < m; j++)
            {
                ArcadeWaypointInfo& wInfo = gInfo.waypoints[j];
                dist = (pt - wInfo.position).SquareSizeXZ();
                if (dist < minDist)
                {
                    minDist = dist;
                    info._type = signArcadeWaypoint;
                    info._indexGroup = i;
                    info._index = j;
                }
            }
        }
    }

    return info;
}

bool CStaticMapArcadeViewer::IsSelected(const SignInfo& info) const
{
    switch (info._type)
    {
        case signArcadeUnit:
        {
            ArcadeUnitInfo* uInfo = nullptr;
            if (info._indexGroup == -1)
            {
                uInfo = &_template->emptyVehicles[info._index];
            }
            else
            {
                ArcadeGroupInfo& gInfo = _template->groups[info._indexGroup];
                uInfo = &gInfo.units[info._index];
            }
            PoseidonAssert(uInfo);
            return uInfo->selected;
        }
        case signArcadeSensor:
        {
            ArcadeSensorInfo* sInfo = nullptr;
            if (info._indexGroup == -1)
            {
                sInfo = &_template->sensors[info._index];
            }
            else
            {
                ArcadeGroupInfo& gInfo = _template->groups[info._indexGroup];
                sInfo = &gInfo.sensors[info._index];
            }
            PoseidonAssert(sInfo);
            return sInfo->selected;
        }
        case signArcadeMarker:
        {
            ArcadeMarkerInfo& mInfo = _template->markers[info._index];
            return mInfo.selected;
        }
        case signArcadeWaypoint:
        {
            ArcadeGroupInfo& gInfo = _template->groups[info._indexGroup];
            ArcadeWaypointInfo& wInfo = gInfo.waypoints[info._index];
            return wInfo.selected;
        }
        default:
            return false;
    }
}

void CStaticMapArcadeViewer::InvertSelection(const SignInfo& info)
{
    switch (info._type)
    {
        case signArcadeUnit:
        {
            ArcadeUnitInfo* uInfo = nullptr;
            if (info._indexGroup == -1)
            {
                uInfo = &_template->emptyVehicles[info._index];
            }
            else
            {
                ArcadeGroupInfo& gInfo = _template->groups[info._indexGroup];
                uInfo = &gInfo.units[info._index];
            }
            PoseidonAssert(uInfo);
            uInfo->selected = !uInfo->selected;
            break;
        }
        case signArcadeSensor:
        {
            ArcadeSensorInfo* sInfo = nullptr;
            if (info._indexGroup == -1)
            {
                sInfo = &_template->sensors[info._index];
            }
            else
            {
                ArcadeGroupInfo& gInfo = _template->groups[info._indexGroup];
                sInfo = &gInfo.sensors[info._index];
            }
            PoseidonAssert(sInfo);
            sInfo->selected = !sInfo->selected;
            break;
        }
        case signArcadeMarker:
        {
            ArcadeMarkerInfo& mInfo = _template->markers[info._index];
            mInfo.selected = !mInfo.selected;
            break;
        }
        case signArcadeWaypoint:
        {
            ArcadeGroupInfo& gInfo = _template->groups[info._indexGroup];
            ArcadeWaypointInfo& wInfo = gInfo.waypoints[info._index];
            wInfo.selected = !wInfo.selected;
            break;
        }
    }
}

void CStaticMapArcadeViewer::OnLButtonDown(float x, float y)
{
    _infoClick = FindSign(x, y);

    // set selection
    _template->ClearSelection();
    InvertSelection(_infoClick);
}

void CStaticMapArcade::OnLButtonDown(float x, float y)
{
    SignInfo info = FindSign(x, y);
    _infoClickCandidate = info;
    _lastPos = ScreenToWorld(DrawCoord(x, y));
}

void CStaticMapArcade::OnLButtonUp(float x, float y)
{
    if (_dragging)
    {
        _dragging = false;
        if (GetMode() != IMGroups && GetMode() != IMSynchronize)
        {
            return;
        }

        SignInfo info = FindSign(x, y);

        if (GetMode() == IMGroups)
        {
            switch (_infoClick._type)
            {
                case signArcadeUnit:
                    if (!HasFullRights())
                    {
                        return;
                    }
                    if (info._type == signArcadeUnit)
                    {
                        // try to assign unit into another group
                        if (_infoClick._indexGroup >= 0 && info._indexGroup >= 0)
                        {
                            bool ok =
                                _template->UnitChangeGroup(_infoClick._indexGroup, _infoClick._index, info._indexGroup);
                            (void)ok;
                        }
                    }
                    else if (info._type == signNone)
                    {
                        // unassign from group
                        if (_infoClick._indexGroup >= 0)
                        {
                            bool ok = _template->UnitChangeGroup(_infoClick._indexGroup, _infoClick._index, -1);
                            (void)ok;
                        }
                    }
                    else if (info._type == signArcadeSensor)
                    {
                        ArcadeUnitInfo* uInfo = nullptr;
                        if (_infoClick._indexGroup >= 0)
                        {
                            ArcadeGroupInfo& gInfo = _template->groups[_infoClick._indexGroup];
                            uInfo = &gInfo.units[_infoClick._index];
                        }
                        else
                        {
                            uInfo = &_template->emptyVehicles[_infoClick._index];
                        }
                        _template->SensorChangeVehicle(info._indexGroup, info._index, uInfo->id);
                    }
                    else if (info._type == signArcadeMarker)
                    {
                        _template->UnitAddMarker(_infoClick._indexGroup, _infoClick._index, info._index);
                    }
                    break;
                case signArcadeSensor:
                    if (info._type == signArcadeUnit)
                    {
                        ArcadeUnitInfo* uInfo = nullptr;
                        if (info._indexGroup >= 0)
                        {
                            ArcadeGroupInfo& gInfo = _template->groups[info._indexGroup];
                            uInfo = &gInfo.units[info._index];
                        }
                        else
                        {
                            uInfo = &_template->emptyVehicles[info._index];
                        }
                        _template->SensorChangeVehicle(_infoClick._indexGroup, _infoClick._index, uInfo->id);
                    }
                    else if (info._type == signStatic)
                    {
                        Object* obj = info._id;
                        PoseidonAssert(obj);
                        if (!obj)
                        {
                            return;
                        }
                        _template->SensorChangeStatic(_infoClick._indexGroup, _infoClick._index, obj->ID());
                    }
                    else if (info._type == signNone)
                    {
                        // unassign
                        // ArcadeSensorInfo *sInfo = nullptr;
                        if (_infoClick._indexGroup >= 0)
                        {
                            _template->SensorChangeGroup(_infoClick._indexGroup, _infoClick._index, -1);
                        }
                        else
                        {
                            ArcadeSensorInfo& sInfo = _template->sensors[_infoClick._index];
                            if (sInfo.idStatic >= 0)
                            {
                                _template->SensorChangeStatic(_infoClick._indexGroup, _infoClick._index, -1);
                            }
                            else if (sInfo.idVehicle >= 0)
                            {
                                _template->SensorChangeVehicle(_infoClick._indexGroup, _infoClick._index, -1);
                            }
                        }
                    }
                    break;
                case signArcadeMarker:
                    if (info._type == signArcadeUnit)
                    {
                        _template->UnitAddMarker(info._indexGroup, info._index, _infoClick._index);
                    }
                    else if (info._type == signNone)
                    {
                        _template->RemoveMarker(_infoClick._index);
                    }
                    break;
                case signStatic:
                    if (info._type == signArcadeSensor)
                    {
                        Object* obj = _infoClick._id;
                        PoseidonAssert(obj);
                        if (!obj)
                        {
                            return;
                        }
                        _template->SensorChangeStatic(info._indexGroup, info._index, obj->ID());
                    }
                    // remove from this side is too hard - do not use it
                    break;
            }
        }
        else // GetMode() == IMSynchronize
        {
            switch (_infoClick._type)
            {
                case signArcadeWaypoint:
                    if (info._type == signArcadeWaypoint)
                    {
                        if (HasRight(_infoClick._indexGroup) || HasRight(info._indexGroup))
                        {
                            _template->WaypointChangeSynchro(_infoClick._indexGroup, _infoClick._index,
                                                             info._indexGroup, info._index);
                        }
                    }
                    else if (info._type == signArcadeSensor)
                    {
                        if (HasRight(_infoClick._indexGroup))
                        {
                            _template->SensorChangeSynchro(info._indexGroup, info._index, _infoClick._indexGroup,
                                                           _infoClick._index);
                        }
                    }
                    else if (info._type == signNone)
                    {
                        if (HasRight(_infoClick._indexGroup))
                        {
                            _template->WaypointChangeSynchro(_infoClick._indexGroup, _infoClick._index, -1, -1);
                        }
                    }
                    break;
                case signArcadeSensor:
                    if (info._type == signArcadeWaypoint)
                    {
                        if (HasRight(info._indexGroup))
                        {
                            _template->SensorChangeSynchro(_infoClick._indexGroup, _infoClick._index, info._indexGroup,
                                                           info._index);
                        }
                    }
                    else if (info._type == signNone)
                    {
                        if (HasFullRights())
                        {
                            _template->SensorChangeSynchro(_infoClick._indexGroup, _infoClick._index, -1, -1);
                        }
                    }
                    break;
            }
        }

        _infoClick._type = signNone;
        _infoClickCandidate._type = signNone;
        _infoMove._type = signNone;
    }
    else if (_selecting)
    {
        _selecting = false;
        if (!InputSubsystem::Instance().IsKeyDown(SDL_SCANCODE_LCTRL) &&
            !InputSubsystem::Instance().IsKeyDown(SDL_SCANCODE_RCTRL))
        {
            _template->ClearSelection();
        }

        float xMin = floatMin(_special.X(), _lastPos.X());
        float xMax = floatMax(_special.X(), _lastPos.X());
        float zMin = floatMin(_special.Z(), _lastPos.Z());
        float zMax = floatMax(_special.Z(), _lastPos.Z());

        for (int i = 0; i < _template->emptyVehicles.Size(); i++)
        {
            ArcadeUnitInfo& info = _template->emptyVehicles[i];
            if (info.position.X() >= xMin && info.position.X() < xMax && info.position.Z() >= zMin &&
                info.position.Z() < zMax)
            {
                info.selected = !info.selected;
            }
        }
        for (int i = 0; i < _template->sensors.Size(); i++)
        {
            ArcadeSensorInfo& info = _template->sensors[i];
            if (info.position.X() >= xMin && info.position.X() < xMax && info.position.Z() >= zMin &&
                info.position.Z() < zMax)
            {
                info.selected = !info.selected;
            }
        }
        for (int i = 0; i < _template->markers.Size(); i++)
        {
            ArcadeMarkerInfo& info = _template->markers[i];
            if (info.position.X() >= xMin && info.position.X() < xMax && info.position.Z() >= zMin &&
                info.position.Z() < zMax)
            {
                info.selected = !info.selected;
            }
        }
        for (int i = 0; i < _template->groups.Size(); i++)
        {
            ArcadeGroupInfo& gInfo = _template->groups[i];
            for (int j = 0; j < gInfo.units.Size(); j++)
            {
                ArcadeUnitInfo& info = gInfo.units[j];
                if (info.position.X() >= xMin && info.position.X() < xMax && info.position.Z() >= zMin &&
                    info.position.Z() < zMax)
                {
                    info.selected = !info.selected;
                }
            }
            for (int j = 0; j < gInfo.sensors.Size(); j++)
            {
                ArcadeSensorInfo& info = gInfo.sensors[j];
                if (info.position.X() >= xMin && info.position.X() < xMax && info.position.Z() >= zMin &&
                    info.position.Z() < zMax)
                {
                    info.selected = !info.selected;
                }
            }
            for (int j = 0; j < gInfo.waypoints.Size(); j++)
            {
                ArcadeWaypointInfo& info = gInfo.waypoints[j];
                if (info.position.X() >= xMin && info.position.X() < xMax && info.position.Z() >= zMin &&
                    info.position.Z() < zMax)
                {
                    info.selected = !info.selected;
                }
            }
        }
    }
}

void CStaticMapArcade::OnLButtonClick(float x, float y)
{
    auto& input = InputSubsystem::Instance();
    bool canSelectGroup = false;
    switch (_infoClickCandidate._type)
    {
        case signArcadeUnit:
        case signArcadeSensor:
            canSelectGroup = _infoClickCandidate._indexGroup >= 0;
        case signArcadeMarker:
            _infoClick = _infoClickCandidate;
            _dragging = input.IsMouseLeftDown() && HasFullRights();
            break;
        case signStatic:
            _infoClick = _infoClickCandidate;
            _dragging = false;
            break;
        case signArcadeWaypoint:
            _infoClick = _infoClickCandidate;
            _dragging = input.IsMouseLeftDown() && HasRight(_infoClick._indexGroup);
            canSelectGroup = true;
            {
                ArcadeGroupInfo& gInfo = _template->groups[_infoClick._indexGroup];
                ArcadeWaypointInfo& wInfo = gInfo.waypoints[_infoClick._index];
                if (wInfo.id >= 0)
                {
                    int idGroup, idUnit;
                    _template->FindUnit(wInfo.id, idGroup, idUnit);
                    _infoClick._type = signArcadeUnit;
                    _infoClick._indexGroup = idGroup;
                    _infoClick._index = idUnit;
                }
                else if (wInfo.idStatic >= 0)
                {
                    _dragging = false;
                }
            }
            break;
        case signNone:
            if (input.IsMouseLeftDown())
            {
                _dragging = false;
                _selecting = true;
                _special = _lastPos;
                return;
            }
            break;
    }

    bool selected = IsSelected(_infoClickCandidate);
    if (!_dragging || !selected)
    {
        // change selection
        if (!input.IsKeyDown(SDL_SCANCODE_LCTRL) && !input.IsKeyDown(SDL_SCANCODE_RCTRL))
        {
            _template->ClearSelection();
            selected = false;
        }
        if ((input.IsKeyDown(SDL_SCANCODE_LSHIFT) || input.GetKey(SDL_SCANCODE_RSHIFT)) && canSelectGroup)
        {
            ArcadeGroupInfo& gInfo = _template->groups[_infoClickCandidate._indexGroup];
            gInfo.Select(!selected);
        }
        else
        {
            InvertSelection(_infoClickCandidate);
        }
    }

    CStatic::OnLButtonClick(x, y);
}

void CStaticMapArcade::OnLButtonDblClick(float x, float y)
{
    ClearAnimation();
    _moveKey = 0;
    _mouseKey = 0;

    // Whole World world map: a double-click outside the mission square moves the mission area there. The
    // mission is saved (units keep their place relative to the area) and the editor restarts at that spot.
    if (EarthStreaming::Enabled())
    {
        const Point3 world = ScreenToWorld(DrawCoord(x, y));
        const float size = LandGrid * LandRange;
        if (world.X() < 0 || world.X() > size || world.Z() < 0 || world.Z() > size)
        {
            const char* latText = std::getenv("POSEIDON_EARTH_LATITUDE");
            const char* lonText = std::getenv("POSEIDON_EARTH_LONGITUDE");
            const double lat0 = latText ? std::atof(latText) : 0.0, lon0 = lonText ? std::atof(lonText) : 0.0;
            const double lat = std::clamp(lat0 + (world.Z() - Earth::Anchor) / Earth::Radius * 180.0 / Earth::Pi, -84.0, 84.0);
            const double lon = Earth::WrapLongitude(
                lon0 + (world.X() - Earth::Anchor) / (Earth::Radius * std::max(0.05, std::cos(lat0 * Earth::Pi / 180.0))) *
                           180.0 / Earth::Pi);
            auto* editor = dynamic_cast<DisplayArcadeMap*>(_parent);
            std::string error;
            if (editor && !editor->SaveMissionTo(WholeWorldEditorMission().c_str()))
                error = "The mission could not be saved before moving.";
            if (error.empty() && !RelaunchWholeWorldEditor(lat, lon, error))
                LOG_ERROR(Core, "Whole World editor move: {}", error);
            if (!error.empty())
                _parent->CreateMsgBox(MB_BUTTON_OK, RString(error.c_str()));
            return;
        }
    }

    SignInfo info = FindSign(x, y);

    if (info._type == signArcadeWaypoint)
    {
        if (HasRight(info._indexGroup))
        {
            // Edit waypoint
            ArcadeGroupInfo& gInfo = _template->groups[info._indexGroup];
            ArcadeWaypointInfo& wInfo = gInfo.waypoints[info._index];
            _parent->CreateChild(
                new DisplayArcadeWaypoint(_parent, _template, info._indexGroup, info._index, wInfo, IsAdvanced()));
        }
    }
    else if (GetMode() == IMWaypoints && (_infoClick._type == signArcadeUnit && _infoClick._indexGroup >= 0 ||
                                          _infoClick._type == signArcadeWaypoint))
    {
        if (HasRight(_infoClick._indexGroup))
        {
            // Insert waypoint
            // ArcadeGroupInfo &gInfo = _template->groups[_infoClick._indexGroup];
            ArcadeWaypointInfo wInfo;
            if (info._type == signArcadeUnit)
            {
                ArcadeUnitInfo* uInfo = nullptr;
                if (info._indexGroup == -1)
                {
                    uInfo = &_template->emptyVehicles[info._index];
                }
                else
                {
                    ArcadeGroupInfo& gInfo = _template->groups[info._indexGroup];
                    uInfo = &gInfo.units[info._index];
                }
                PoseidonAssert(uInfo);
                wInfo.position = uInfo->position;
                wInfo.id = uInfo->id;
            }
            else if (info._type == signStatic)
            {
                Object* obj = info._id;
                PoseidonAssert(obj);
                wInfo.position = obj->Position();
                wInfo.idStatic = obj->ID();
            }
            else
            {
                wInfo.position = ScreenToWorld(DrawCoord(x, y));
                wInfo.position[1] = GLOB_LAND->RoadSurfaceY(wInfo.position[0], wInfo.position[2]);
            }
            _parent->CreateChild(
                new DisplayArcadeWaypoint(_parent, _template, _infoClick._indexGroup, -1, wInfo, IsAdvanced()));
        }
    }
    else if (info._type == signArcadeSensor)
    {
        if (HasFullRights())
        {
            // Edit sensor
            ArcadeSensorInfo* sInfo = nullptr;
            if (info._indexGroup == -1)
            {
                sInfo = &_template->sensors[info._index];
            }
            else
            {
                ArcadeGroupInfo& gInfo = _template->groups[info._indexGroup];
                sInfo = &gInfo.sensors[info._index];
            }
            PoseidonAssert(sInfo);
            _parent->CreateChild(
                new DisplayArcadeSensor(_parent, info._indexGroup, info._index, *sInfo, _template, IsAdvanced()));
        }
    }
    else if (info._type == signArcadeMarker)
    {
        if (HasFullRights())
        {
            // Edit marker
            ArcadeMarkerInfo& mInfo = _template->markers[info._index];
            _parent->CreateChild(new DisplayArcadeMarker(_parent, info._index, mInfo, _template, IsAdvanced()));
        }
    }
    else if (info._type == signArcadeUnit)
    {
        if (HasFullRights())
        {
            // Edit unit
            ArcadeUnitInfo* uInfo = nullptr;
            if (info._indexGroup == -1)
            {
                uInfo = &_template->emptyVehicles[info._index];
            }
            else
            {
                ArcadeGroupInfo& gInfo = _template->groups[info._indexGroup];
                uInfo = &gInfo.units[info._index];
            }
            PoseidonAssert(uInfo);

            _parent->CreateChild(
                new DisplayArcadeUnit(_parent, info._indexGroup, info._index, *uInfo, _template, IsAdvanced()));
        }
    }
    else if (GetMode() == IMSensors)
    {
        // Insert sensor
        if (HasFullRights())
        {
            ArcadeSensorInfo sInfo;
            sInfo.position = ScreenToWorld(DrawCoord(x, y));
            sInfo.position[1] = GLOB_LAND->RoadSurfaceY(sInfo.position[0], sInfo.position[2]);
            _parent->CreateChild(new DisplayArcadeSensor(_parent, -1, -1, sInfo, _template, IsAdvanced()));
        }
    }
    else if (GetMode() == IMMarkers)
    {
        // Insert marker
        if (HasFullRights())
        {
            ArcadeMarkerInfo mInfo;
            mInfo.position = ScreenToWorld(DrawCoord(x, y));
            mInfo.position[1] = GLOB_LAND->RoadSurfaceY(mInfo.position[0], mInfo.position[2]);
            _parent->CreateChild(new DisplayArcadeMarker(_parent, -1, mInfo, _template, IsAdvanced()));
        }
    }
    else if (GetMode() == IMGroups)
    {
        // Insert group
        if (HasFullRights())
        {
            Vector3 position = ScreenToWorld(DrawCoord(x, y));
            position[1] = GLandscape->RoadSurfaceY(position[0], position[2]);
            _parent->CreateChild(
                new DisplayArcadeGroup(_parent, position, _lastGroupSide, _lastGroupType, _lastGroupName, 0));
        }
    }
    else if (GetMode() == IMUnits)
    {
        if (HasFullRights())
        {
            // Insert unit
            ArcadeUnitInfo uInfo, *lastInfo = GetLastUnit();
            if (lastInfo)
            {
                uInfo = *lastInfo;
            }

            uInfo.position = ScreenToWorld(DrawCoord(x, y));
            uInfo.position[1] = GLOB_LAND->RoadSurfaceY(uInfo.position[0], uInfo.position[2]);

            uInfo.name = "";
            uInfo.init = "";

            uInfo.player = APNonplayable;
            if (!_template->FindPlayer())
            {
                uInfo.player = APPlayerCommander;
            }

            if (uInfo.player != APNonplayable && uInfo.side == TEmpty)
            {
                uInfo.side = TWest;
            }

            uInfo.health = 1.0;
            uInfo.fuel = 1.0;
            uInfo.ammo = 1.0;
            uInfo.presence = 1.0;
            uInfo.placement = 0;

            uInfo.markers.Clear();

            _parent->CreateChild(new DisplayArcadeUnit(_parent, -1, -1, uInfo, _template, IsAdvanced()));
        }
    }
}

void CStaticMapArcade::OnMouseHold(float x, float y, bool active)
{
    if (_dragging)
    {
        Vector3 curPos = ScreenToWorld(DrawCoord(x, y));
        switch (GetMode())
        {
            case IMGroups:
                switch (_infoClick._type)
                {
                    case signArcadeUnit:
                    case signArcadeSensor:
                    case signArcadeMarker:
                    case signStatic:
                        _special = curPos;
                        break;
                    default:
                        goto MoveSelection;
                }
                break;
            case IMSynchronize:
                switch (_infoClick._type)
                {
                    case signArcadeSensor:
                    case signArcadeWaypoint:
                        _special = curPos;
                        break;
                    default:
                        goto MoveSelection;
                }
                break;
            default:
            MoveSelection:
                if (InputSubsystem::Instance().IsKeyDown(SDL_SCANCODE_LSHIFT) ||
                    InputSubsystem::Instance().IsKeyDown(SDL_SCANCODE_RSHIFT))
                {
                    // rotate
                    Vector3 sum = VZero;
                    int count = 0;
                    _template->CalculateCenter(sum, count, true);
                    if (count > 0)
                    {
                        Vector3 center = (1.0f / count) * sum;
                        Vector3 oldDir = _lastPos - center;
                        Vector3 dir = curPos - center;
                        float angle = AngleDifference(atan2(dir.X(), dir.Z()), atan2(oldDir.X(), oldDir.Z()));
                        _template->Rotate(center, angle, true);
                    }
                }
                else
                {
                    Vector3 offset = curPos - _lastPos;
                    // move all selected items by offset
                    for (int i = 0; i < _template->emptyVehicles.Size(); i++)
                    {
                        if (_template->emptyVehicles[i].selected)
                        {
                            Vector3 pos = _template->emptyVehicles[i].position + offset;
                            pos[1] = GLOB_LAND->RoadSurfaceYAboveWater(pos[0], pos[2]);
                            if (_template->UnitChangePosition(-1, i, pos))
                            {
                            }
                        }
                    }
                    for (int i = 0; i < _template->sensors.Size(); i++)
                    {
                        if (_template->sensors[i].selected)
                        {
                            Vector3 pos = _template->sensors[i].position + offset;
                            pos[1] = GLOB_LAND->RoadSurfaceYAboveWater(pos[0], pos[2]);
                            if (_template->SensorChangePosition(-1, i, pos))
                            {
                            }
                        }
                    }
                    for (int i = 0; i < _template->markers.Size(); i++)
                    {
                        if (_template->markers[i].selected)
                        {
                            Vector3 pos = _template->markers[i].position + offset;
                            pos[1] = GLOB_LAND->RoadSurfaceYAboveWater(pos[0], pos[2]);
                            if (_template->MarkerChangePosition(i, pos))
                            {
                            }
                        }
                    }
                    for (int i = 0; i < _template->groups.Size(); i++)
                    {
                        ArcadeGroupInfo& gInfo = _template->groups[i];
                        for (int j = 0; j < gInfo.units.Size(); j++)
                        {
                            if (gInfo.units[j].selected)
                            {
                                Vector3 pos = gInfo.units[j].position + offset;
                                pos[1] = GLOB_LAND->RoadSurfaceYAboveWater(pos[0], pos[2]);
                                if (_template->UnitChangePosition(i, j, pos))
                                {
                                }
                            }
                        }
                        for (int j = 0; j < gInfo.sensors.Size(); j++)
                        {
                            if (gInfo.sensors[j].selected)
                            {
                                Vector3 pos = gInfo.sensors[j].position + offset;
                                pos[1] = GLOB_LAND->RoadSurfaceYAboveWater(pos[0], pos[2]);
                                if (_template->SensorChangePosition(i, j, pos))
                                {
                                }
                            }
                        }
                        for (int j = 0; j < gInfo.waypoints.Size(); j++)
                        {
                            if (gInfo.waypoints[j].selected)
                            {
                                Vector3 pos = gInfo.waypoints[j].position + offset;
                                pos[1] = GLOB_LAND->RoadSurfaceYAboveWater(pos[0], pos[2]);
                                if (_template->WaypointChangePosition(i, j, pos))
                                {
                                }
                            }
                        }
                    }
                }
        }

        _lastPos = curPos;
    }
    else if (_selecting)
    {
        _lastPos = ScreenToWorld(DrawCoord(x, y));
    }

    CStaticMap::OnMouseHold(x, y, active);
}

// Arcade map display

RString GetMissionsDirectory();
DisplayArcadeMap::DisplayArcadeMap(ControlsContainer* parent, bool multiplayer) : DisplayMapEditor(parent)
{
    _enableSimulation = false;
    _enableDisplay = false;
    //	GWorld->EnableDisplay(false);

    _multiplayer = multiplayer;
    _running = false;

    _alwaysShow = true;
    _mode = IMUnits;

    Load("RscDisplayArcadeMap");

    if (*Glob.header.filename)
    {
        char buffer[256];

        snprintf(buffer, sizeof(buffer), "%smission.sqm", (const char*)GetMissionDirectory());

        LoadTemplates(buffer);

        ArcadeUnitInfo* uInfo = _templateMission.FindPlayer();
        if (uInfo)
        {
            Glob.header.playerSide = (TargetSide)uInfo->side;
        }
    }

    if (_map)
    {
        _map->SetScale(-1); // default
        _map->Center();
        _map->Reset();
    }

    LoadParams();
    SetAdvancedMode(_advanced);
    ShowButtons();
    UpdateIdsButton();
    UpdateTexturesButton();
    _langCbToken = RegisterLanguageChangedCallback([this]() { RefreshLanguage(); });
}

} // namespace Poseidon
#include <Poseidon/Core/SaveVersion.hpp>
#include <Poseidon/Core/Version.hpp>
namespace Poseidon
{

} // namespace Poseidon
