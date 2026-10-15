#include <Poseidon/World/Entities/Infantry/SoldierOldCommon.hpp>
#include <Poseidon/World/Weather/MudField.hpp>
#include <Poseidon/World/Weather/SandField.hpp>
#include <Poseidon/Core/Application.hpp>
#include <Poseidon/Input/InputSubsystem.hpp>
#include <Poseidon/UI/Settings/AspectRatio.hpp>
#include <Poseidon/Foundation/Algorithms/Qsort.hpp>
#include <Poseidon/Foundation/Common/FltOpts.hpp>
#include <Poseidon/Foundation/Containers/Array.hpp>
#include <Poseidon/Foundation/Containers/BankArray.hpp>
#include <Poseidon/Foundation/Containers/StaticArray.hpp>
#include <Poseidon/Foundation/Enums/EnumNames.hpp>
#include <Poseidon/Foundation/Framework/DebugLog.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/Foundation/Math/Math3D.hpp>
#include <Poseidon/Foundation/Math/Math3DP.hpp>
#include <Poseidon/Foundation/Math/MathDefs.hpp>
#include <Poseidon/Foundation/Memory/FastAlloc.hpp>
#include <Poseidon/Foundation/Strings/RString.hpp>
#include <Poseidon/Foundation/Time/Time.hpp>
#include <Poseidon/Foundation/Types/Pointers.hpp>
#include <Poseidon/Foundation/Types/RemoveLinks.hpp>
#include <Poseidon/Foundation/platform.hpp>
#include <Random/randomGen.hpp>

#include <set>
#include <string>

namespace Poseidon
{
using namespace Foundation;
using Foundation::EnumName;

bool Man::CastProxyShadow(int level, int index) const
{
    return true;
}

namespace
{
// A held item (rifle, launcher, binocular) is drawn with a DIFFERENT model than the proxy
// placeholder it replaces. The proxy's matrix already carries the placeholder's own
// source-origin offset (ShapeAdapter); undo it here, and add the drawn model's
// (HeldModelSourceOrigin). Both are zero for every OFP/CWA model, so nothing changes there;
// an Arma ODOL model is stored around its bounding centre, and the corepatch M16A4 (centre
// 0.98 m up, 0.48 m along) was drawn at its bearer's feet without this.
Matrix4 HeldModelPlaceholderUndo(const ProxyObject& proxy)
{
    const LODShapeWithShadow* placeholder = proxy.obj ? proxy.obj->GetShape() : nullptr;
    if (!placeholder || placeholder->AnimationOrigin().SquareSize() < 1e-8f)
        return MIdentity;
    return Matrix4(MTranslation, placeholder->AnimationOrigin());
}

Matrix4 HeldModelSourceOrigin(const LODShapeWithShadow* drawn)
{
    if (!drawn || drawn->AnimationOrigin().SquareSize() < 1e-8f)
        return MIdentity;
    return Matrix4(MTranslation, -drawn->AnimationOrigin());
}

// OP_A2_PROXYLOG=1, first person: where the view-pilot LOD's arms, fingers and collar land
// against the same bones of LOD 0 posed with the same palette, what sits in front of the eye,
// and which textures the LOD draws with. The Arma 2 first-person view showed spread pink
// fingers (USMC) or a piece of gear filling the bottom of the screen (FR, ACR), while the
// same soldiers looked right from outside.
void LogViewLodPose(LODShapeWithShadow* lshape, const WeightInfo& weights, int level, const Matrix4Array& matrix,
                    Vector3Par eye)
{
    static const bool enabled = [] {
        const char* e = std::getenv("OP_A2_PROXYLOG");
        return e && *e == '1';
    }();
    if (!enabled || !lshape)
        return;
    static DWORD last = 0;
    static bool texturesLogged = false;
    const DWORD now = Poseidon::Foundation::GlobalTickCount();
    if (now - last < 2000)
        return;
    last = now;
    Shape* view = lshape->Level(level);
    if (!view)
        return;
    // posed centroid of a named selection, from the palette (weights as AnimationRT applies them)
    const auto posed = [&](int lod, const char* name, Vector3& out, int& count, float& weightSum) -> bool
    {
        Shape* s = lshape->Level(lod);
        if (!s)
            return false;
        const int si = s->FindNamedSel(name);
        if (si < 0)
            return false;
        const NamedSelection& sel = s->NamedSel(si);
        const AnimationRTWeights& w = weights[lod];
        Vector3 sum = VZero;
        count = 0;
        weightSum = 0;
        for (int i = 0; i < sel.Size(); i++)
        {
            const int v = sel[i];
            const Vector3 orig = s->OriginalPosValid() ? s->OrigPos(v) : s->Pos(v);
            const AnimationRTWeight& wg = w[v];
            Vector3 p = VZero;
            float ws = 0;
            for (int k = 0; k < wg.Size(); k++)
            {
                const int bone = wg[k].GetSel();
                if (bone < 0 || bone >= matrix.Size())
                    continue;
                p += matrix[bone].FastTransform(orig) * wg[k].GetWeight();
                ws += wg[k].GetWeight();
            }
            if (ws <= 0)
                continue;
            sum += p;
            weightSum += ws;
            count++;
        }
        if (count > 0)
        {
            out = sum / float(count);
            weightSum /= float(count);
        }
        return count > 0;
    };
    LOG_INFO(Graphics, "A2 view LOD: level {} res {} of {} bones {} eye {:.2f},{:.2f},{:.2f} verts {} (lod 0: {})", level,
             lshape->Resolution(level), (const char*)lshape->GetName(), matrix.Size(), eye.X(), eye.Y(), eye.Z(), view->NPos(),
             lshape->Level(0) ? lshape->Level(0)->NPos() : 0);
    static const char* const names[] = {"head",          "neck1",          "spine3",         "leftshoulder",
                                        "leftforearm",   "lefthand",       "lefthandindex3", "lefthandthumb3",
                                        "rightforearm",  "righthand",      "righthandindex3", "weapon"};
    for (const char* name : names)
    {
        Vector3 a = VZero, b = VZero;
        int na = 0, nb = 0;
        float wa = 0, wb = 0;
        const bool hasA = posed(level, name, a, na, wa);
        const bool hasB = posed(0, name, b, nb, wb);
        if (!hasA && !hasB)
            continue;
        LOG_INFO(Graphics,
                 "A2 view LOD bone {}: view {} verts at {:.2f},{:.2f},{:.2f} (weight {:.2f}) | lod 0 {} verts at "
                 "{:.2f},{:.2f},{:.2f} (weight {:.2f}) | apart {:.2f} m",
                 name, na, a.X(), a.Y(), a.Z(), wa, nb, b.X(), b.Y(), b.Z(), wb, hasA && hasB ? a.Distance(b) : -1.0f);
    }
    // what the drawn (CPU-skinned) view LOD puts in front of the eye: model +z is forward
    int ahead = 0, unweighted = 0;
    Vector3 nearest = VZero;
    float nearestD = 1e9f;
    Vector3 mn(1e9f, 1e9f, 1e9f), mx(-1e9f, -1e9f, -1e9f);
    const AnimationRTWeights& vw = weights[level];
    for (int v = 0; v < view->NPos(); v++)
    {
        if (vw[v].Size() <= 0)
            unweighted++;
        const Vector3 d = view->Pos(v) - eye;
        if (d.Z() < 0.03f || fabs(d.X()) > d.Z() * 0.8f || fabs(d.Y()) > d.Z() * 0.5f)
            continue;
        ahead++;
        mn = Vector3(std::min(mn.X(), d.X()), std::min(mn.Y(), d.Y()), std::min(mn.Z(), d.Z()));
        mx = Vector3(std::max(mx.X(), d.X()), std::max(mx.Y(), d.Y()), std::max(mx.Z(), d.Z()));
        if (d.Size() < nearestD)
        {
            nearestD = d.Size();
            nearest = d;
        }
    }
    LOG_INFO(Graphics,
             "A2 view LOD in view of the eye: {} of {} vertices (unweighted {}), relative box {:.2f},{:.2f},{:.2f} .. "
             "{:.2f},{:.2f},{:.2f}, nearest {:.2f},{:.2f},{:.2f}",
             ahead, view->NPos(), unweighted, mn.X(), mn.Y(), mn.Z(), mx.X(), mx.Y(), mx.Z(), nearest.X(), nearest.Y(),
             nearest.Z());
    if (!texturesLogged)
    {
        texturesLogged = true;
        for (int i = 0; i < view->NSections(); i++)
        {
            const ShapeSection& sec = view->GetSection(i);
            Texture* tex = sec.properties.GetTexture();
            LOG_INFO(Graphics, "A2 view LOD section {}: texture {} special {:x}", i,
                     tex ? (const char*)tex->GetName() : "(none)", sec.properties.Special());
        }
    }
}
} // namespace

Object* Man::GetProxy(LODShapeWithShadow*& shape, int level, Matrix4& transform, Matrix4& invTransform,
                      const FrameBase& parentPos, int i) const
{
    // draw visible guns / weapons
    Shape* sShape = _shape->LevelOpaque(level);
    const ManType* type = Type();

    const WeightInfo& weights = type->GetWeights();
    const AnimationRTWeights& wgt = weights[level];

    if (Type()->_nvGogglesProxyIndex[level] == i)
    {
        // night vision
        // check if night vision should be shown
        if (!IsNVWanted() || !IsNVEnabled())
        {
            return nullptr;
        }
    }
    const ProxyObject& proxy = sShape->Proxy(i);
    if (proxy.selection < 0 || proxy.selection >= sShape->NNamedSel())
        return nullptr;
    const NamedSelection& sel = sShape->NamedSel(proxy.selection);
    if (sel.Size() <= 0)
        return nullptr;
    int point = sel[0];
    if (point < 0 || point >= wgt.Size() || wgt[point].Size() <= 0)
        return nullptr; // see DrawProxies: an Arma 2 proxy without skeleton weight
    const AnimationRTWeight& wg = wgt[point];
    int matIndex = wg[0].GetSel();

    const WeaponType* weapon = nullptr;
    LODShapeWithShadow* pshape = nullptr;
    if (matIndex == type->_rpgMatIndex)
    {
        if (!_showSecondaryWeapon)
        {
            return nullptr;
        }

        int index = FindWeaponType(MaskSlotSecondary, MaskSlotPrimary);
        if (index >= 0)
        {
            weapon = GetWeaponSystem(index);
            pshape = weapon->_model;
            if (_currentWeapon >= 0 && _currentWeapon < NMagazineSlots())
            {
                const MagazineSlot& slot = GetMagazineSlot(_currentWeapon);
                if (slot._weapon == weapon && slot._magazine && slot._magazine->_ammo > 0)
                {
                    LODShapeWithShadow* special = slot._magazine->_type->_model;
                    if (special)
                    {
                        pshape = special;
                    }
                }
            }
            weapon = nullptr; // store only primary weapon
        }
    }
    else if (matIndex == type->_gunMatIndex)
    {
        if (!_showPrimaryWeapon)
        {
            return nullptr;
        }

        int index = FindWeaponType(MaskSlotPrimary);
        if (index >= 0)
        {
            weapon = GetWeaponSystem(index);
            pshape = weapon->_model;
            if (_currentWeapon >= 0 && _currentWeapon < NMagazineSlots())
            {
                const MagazineSlot& slot = GetMagazineSlot(_currentWeapon);
                if (slot._weapon == weapon && slot._magazine && slot._magazine->_ammo > 0)
                {
                    LODShapeWithShadow* special = slot._magazine->_type->_model;
                    if (special)
                    {
                        pshape = special;
                    }
                }
            }
        }
    }
    else if (matIndex == type->_handMatIndex)
    {
        if (ShowItemInHand())
        {
            const WeaponType* weapon = Poseidon::FindBinocularWeapon(_weapons);
            if (weapon)
            {
                pshape = weapon->_model;
            }
        }
    }
    else if (matIndex == type->_rightHandMatIndex)
    {
        if (ShowItemInRightHand())
        {
            const WeaponType* weapon = Poseidon::FindBinocularWeapon(_weapons);
            if (weapon)
            {
                pshape = weapon->_model;
            }
        }
        if (ShowHandGun())
        {
            const WeaponType* weapon = nullptr;
            for (int i = 0; i < NWeaponSystems(); i++)
            {
                if (GetWeaponSystem(i)->_weaponType & MaskSlotHandGun)
                {
                    weapon = GetWeaponSystem(i);
                    break;
                }
            }
            if (weapon)
            {
                pshape = weapon->_model;
            }
        }
    }

    if (!pshape)
    {
        return nullptr;
    }

    Matrix4 animTransform = AnimateProxyMatrix(level, proxy);

    animTransform.Orthogonalize();
    transform = transform * animTransform * HeldModelPlaceholderUndo(proxy);

    // if shape is reversed, reverse orientation
    if (pshape->Remarks() & REM_REVERSED)
    {
        Matrix4 swapM(MScale, -1, +1, -1);
        transform = transform * swapM;
    }
    transform = transform * HeldModelSourceOrigin(pshape);
    invTransform = transform.InverseScaled();

    shape = pshape;
    return proxy.obj;
}

int Man::GetProxyComplexity(int level, const FrameBase& pos, float dist2) const
{
    int nFaces = base::GetProxyComplexity(level, pos, dist2);
    // Note: calculate weapons complexity
    return nFaces;
}

namespace
{
// Once per model and LOD: which proxy could not be placed, so a missing weapon names itself.
void ReportUnweightedProxy(const LODShapeWithShadow* shape, int level, const ProxyObject& proxy, const char* why)
{
    static std::set<std::string> reported;
    const std::string key = std::string(shape ? (const char*)shape->Name() : "?") + "#" + std::to_string(level) + "#" +
                            std::string((const char*)proxy.name);
    if (reported.insert(key).second)
        LOG_WARN(Graphics, "Man proxy '{}' on {} level {} not drawn: {}", (const char*)proxy.name,
                 shape ? (const char*)shape->Name() : "?", level, why);
}
} // namespace

bool Man::HeldWeaponTransform(int level, const WeaponType* weapon, Matrix4& out) const
{
    if (!weapon || !weapon->_model || level < 0 || level >= _shape->NLevels())
        return false;
    const ManType* type = Type();
    const int wantMat = (weapon->_weaponType & MaskSlotPrimary)     ? type->_gunMatIndex
                        : (weapon->_weaponType & MaskSlotSecondary) ? type->_rpgMatIndex
                                                                    : -1;
    if (wantMat < 0)
        return false;
    const Shape* sShape = _shape->LevelOpaque(level);
    const AnimationRTWeights& wgt = type->GetWeights()[level];
    for (int i = 0; i < sShape->NProxies(); i++)
    {
        const ProxyObject& proxy = sShape->Proxy(i);
        if (proxy.selection < 0 || proxy.selection >= sShape->NNamedSel())
            continue;
        const NamedSelection& sel = sShape->NamedSel(proxy.selection);
        if (sel.Size() <= 0)
            continue;
        const int point = sel[0];
        if (point < 0 || point >= wgt.Size() || wgt[point].Size() <= 0 || wgt[point][0].GetSel() != wantMat)
            continue;
        // DrawProxies' chain for this proxy, less the man's world transform
        Matrix4 t = AnimateProxyMatrix(level, proxy) * HeldModelPlaceholderUndo(proxy);
        if (weapon->_model->Remarks() & REM_REVERSED)
            t = t * Matrix4(MScale, -1, +1, -1);
        out = t * HeldModelSourceOrigin(weapon->_model);
        return true;
    }
    return false;
}

void Man::DrawProxies(int level, ClipFlags clipFlags, const Matrix4& transform, const Matrix4& invTransform,
                      float dist2, float z2, const LightList& lights)
{
    base::DrawProxies(level, clipFlags, transform, invTransform, dist2, z2, lights);

    Shape* sShape = _shape->LevelOpaque(level);
    const ManType* type = Type();

    const WeightInfo& weights = type->GetWeights();
    const AnimationRTWeights& wgt = weights[level];

    for (int i = 0; i < sShape->NProxies(); i++)
    {
        if (Type()->_nvGogglesProxyIndex[level] == i)
        {
            // night vision
            // check if night vision should be shown
            if (!IsNVWanted() || !IsNVEnabled() || !_showHead)
            {
                continue;
            }
        }
        const ProxyObject& proxy = sShape->Proxy(i);
        // A proxy whose selection has no point, or whose point carries no skeleton weight,
        // cannot be placed on the skeleton. OFP models never do this; Arma 2 ODOL can (its
        // skinning lives in per-vertex bone references), and indexing past the end here was
        // the first-frame 0xC0000005 with Arma 2 soldiers. Such a proxy is not drawn.
        if (proxy.selection < 0 || proxy.selection >= sShape->NNamedSel())
            continue;
        const NamedSelection& sel = sShape->NamedSel(proxy.selection);
        if (sel.Size() <= 0)
        {
            ReportUnweightedProxy(_shape, level, proxy, "selection has no point");
            continue;
        }
        int point = sel[0];
        if (point < 0 || point >= wgt.Size() || wgt[point].Size() <= 0)
        {
            ReportUnweightedProxy(_shape, level, proxy, "point has no skeleton weight");
            continue;
        }
        const AnimationRTWeight& wg = wgt[point];
        int matIndex = wg[0].GetSel();

        const WeaponType* weapon = nullptr;
        LODShapeWithShadow* pshape = nullptr;
        const AnimationAnimatedTexture* animFire = nullptr;
        if (matIndex == type->_rpgMatIndex)
        {
            if (!_showSecondaryWeapon)
            {
                continue;
            }

            int index = FindWeaponType(MaskSlotSecondary, MaskSlotPrimary);
            if (index >= 0)
            {
                weapon = GetWeaponSystem(index);
                pshape = weapon->_model;
                animFire = &weapon->_animFire;
                if (_currentWeapon >= 0 && _currentWeapon < NMagazineSlots())
                {
                    const MagazineSlot& slot = GetMagazineSlot(_currentWeapon);
                    if (slot._weapon == weapon && slot._magazine && slot._magazine->_ammo > 0)
                    {
                        LODShapeWithShadow* special = slot._magazine->_type->_model;
                        if (special)
                        {
                            pshape = special;
                            animFire = &slot._magazine->_type->_animFire;
                        }
                    }
                }
                weapon = nullptr; // store only primary weapon
            }
        }
        else if (matIndex == type->_gunMatIndex)
        {
            if (!_showPrimaryWeapon)
            {
                continue;
            }

            int index = FindWeaponType(MaskSlotPrimary);
            if (index >= 0)
            {
                weapon = GetWeaponSystem(index);
                pshape = weapon->_model;
                animFire = &weapon->_animFire;
                if (_currentWeapon >= 0 && _currentWeapon < NMagazineSlots())
                {
                    const MagazineSlot& slot = GetMagazineSlot(_currentWeapon);
                    if (slot._weapon == weapon && slot._magazine && slot._magazine->_ammo > 0)
                    {
                        LODShapeWithShadow* special = slot._magazine->_type->_model;
                        if (special)
                        {
                            pshape = special;
                            animFire = &slot._magazine->_type->_animFire;
                        }
                    }
                }
            }
        }
        else if (matIndex == type->_handMatIndex)
        {
            if (ShowItemInHand())
            {
                weapon = Poseidon::FindBinocularWeapon(_weapons);
                if (weapon)
                {
                    pshape = weapon->_model;
                }
            }
        }
        else if (matIndex == type->_rightHandMatIndex)
        {
            if (ShowItemInRightHand())
            {
                weapon = Poseidon::FindBinocularWeapon(_weapons);
                if (weapon)
                {
                    pshape = weapon->_model;
                }
            }
            if (ShowHandGun())
            {
                for (int i = 0; i < NWeaponSystems(); i++)
                {
                    if (GetWeaponSystem(i)->_weaponType & MaskSlotHandGun)
                    {
                        weapon = GetWeaponSystem(i);
                        break;
                    }
                }
                if (weapon)
                {
                    pshape = weapon->_model;
                    animFire = &weapon->_animFire;
                    if (_currentWeapon >= 0 && _currentWeapon < NMagazineSlots())
                    {
                        const MagazineSlot& slot = GetMagazineSlot(_currentWeapon);
                        if (slot._weapon == weapon && slot._magazine && slot._magazine->_ammo > 0)
                        {
                            LODShapeWithShadow* special = slot._magazine->_type->_model;
                            if (special)
                            {
                                pshape = special;
                                animFire = &slot._magazine->_type->_animFire;
                            }
                        }
                    }
                }
            }
        }
        else if (Type()->_nvGogglesProxyIndex[level] == i)
        {
            pshape = proxy.obj->GetShape();
        }

        if (!pshape)
        {
            continue;
        }

        Matrix4 animTransform = AnimateProxyMatrix(level, proxy);

        Matrix4 pTransform = transform * animTransform * HeldModelPlaceholderUndo(proxy);

        // OP_A2_PROXYLOG=1: where a held weapon lands, the player against one other man,
        // every two seconds (the player's rifle was drawn at his feet, the AI's in hand).
        static const bool proxyLog = [] {
            const char* e = std::getenv("OP_A2_PROXYLOG");
            return e && *e == '1';
        }();
        if (proxyLog && matIndex == type->_gunMatIndex)
        {
            static DWORD lastPlayer = 0, lastOther = 0;
            const bool isPlayer = GWorld && GWorld->CameraOn() == this;
            DWORD& last = isPlayer ? lastPlayer : lastOther;
            const DWORD now = Poseidon::Foundation::GlobalTickCount();
            if (now - last > 2000)
            {
                last = now;
                const Vector3 ap = animTransform.Position();
                const Vector3 gp = _gunTrans.Position();
                LOG_INFO(Graphics,
                         "A2 proxy {}: level {} res {} insideView {} proxy '{}' shape {} anim pos {:.2f},{:.2f},{:.2f} "
                         "up {:.2f},{:.2f},{:.2f} gunIdent {} gunTrans pos {:.2f},{:.2f},{:.2f} aimX {:.3f} aimY {:.3f} "
                         "move {} cam {}",
                         isPlayer ? "PLAYER" : "other", level, _shape->Resolution(level), type->_insideView,
                         (const char*)proxy.name, (const char*)pshape->Name(), ap.X(), ap.Y(), ap.Z(),
                         animTransform.DirectionUp().X(), animTransform.DirectionUp().Y(),
                         animTransform.DirectionUp().Z(), _gunTransIdent, gp.X(), gp.Y(), gp.Z(), _gunXRot, _gunYRot,
                         (const char*)Type()->GetMoveName(_primaryMove.id),
                         GWorld ? static_cast<int>(GWorld->GetCameraType()) : -1);
            }
        }

        if (pshape->Remarks() & REM_REVERSED)
        {
            Matrix4 swapM(MScale, -1, +1, -1);
            pTransform = pTransform * swapM;
        }
        pTransform = pTransform * HeldModelSourceOrigin(pshape);
        Matrix4Val invPTransform = pTransform.InverseScaled();

        int pLevel = -1;
        if (!pshape)
        {
            pshape = proxy.obj->GetShape();
        }
        if (level == type->_insideView)
        {
            pLevel = pshape->FindSpecLevel(VIEW_PILOT);
        }
        if (pLevel < 0)
        {
            pLevel = GScene->LevelFromDistance2(pshape, dist2, pTransform.Scale(), pTransform.Direction(),
                                                GScene->GetCamera()->Direction());
        }
        if (proxyLog && GWorld && GWorld->CameraOn() == this)
        {
            static DWORD lastLevel[8] = {};
            const DWORD now = Poseidon::Foundation::GlobalTickCount();
            DWORD& last = lastLevel[i & 7];
            if (now - last > 2000)
            {
                last = now;
                const Vector3 wp = pTransform.Position();
                const Vector3 up = pTransform.DirectionUp();
                LOG_INFO(Graphics,
                         "A2 proxy PLAYER draw #{} '{}' as {} ({}): man level {} -> level {} of {} (scale {:.2f}) world "
                         "pos {:.2f},{:.2f},{:.2f} up {:.2f},{:.2f},{:.2f} man at {:.2f},{:.2f},{:.2f} filter {} "
                         "reversed {} camera {:.2f},{:.2f},{:.2f} looking {:.2f},{:.2f},{:.2f} man facing {:.2f},{:.2f},{:.2f}",
                         i, (const char*)proxy.name, (const char*)pshape->Name(),
                         matIndex == type->_gunMatIndex ? "gun" : matIndex == type->_rpgMatIndex ? "launcher"
                         : matIndex == type->_handMatIndex ? "hand" : matIndex == type->_rightHandMatIndex ? "righthand"
                                                                                                         : "other",
                         level, pLevel, pshape->NLevels(), pTransform.Scale(), wp.X(), wp.Y(), wp.Z(), up.X(), up.Y(),
                         up.Z(),
                         transform.Position().X(), transform.Position().Y(), transform.Position().Z(),
                         static_cast<int>(GSectionFilter), (pshape->Remarks() & REM_REVERSED) != 0,
                         GScene->GetCamera()->Position().X(), GScene->GetCamera()->Position().Y(),
                         GScene->GetCamera()->Position().Z(), GScene->GetCamera()->Direction().X(),
                         GScene->GetCamera()->Direction().Y(), GScene->GetCamera()->Direction().Z(),
                         transform.Direction().X(), transform.Direction().Y(), transform.Direction().Z());
            }
        }
        if (pLevel == LOD_INVISIBLE)
        {
            continue;
        }

        if (animFire)
        {
            if (weapon == _mGunFireWeapon && (_mGunFireFrames > 0 || Glob.uiTime < _mGunFireTime + 0.05f))
            {
                animFire->Unhide(pshape, pLevel);
                animFire->SetPhase(pshape, pLevel, _mGunFirePhase);
            }
            else
            {
                animFire->Hide(pshape, pLevel);
            }
        }

        if (weapon && weapon->_revolving.GetSelection(pLevel) >= 0)
        {
            const MagazineSlot* slot = nullptr;
            for (int i = 0; i < NMagazineSlots(); i++)
            {
                const MagazineSlot& s = GetMagazineSlot(i);
                if (s._weapon == weapon)
                {
                    slot = &s;
                    break;
                }
            }

            if (slot)
            {
                const Magazine* magazine = slot->_magazine;
                if (magazine)
                {
                    const MagazineType* type = magazine->_type;
                    if (type->_modes.Size() > 0)
                    {
                        float reload = magazine->_reload / type->_modes[0]->_reloadTime;
                        saturate(reload, 0, 1);
                        float coef = (magazine->_ammo + reload) / type->_maxAmmo;
                        weapon->_revolving.Rotate(pshape, -2.0f * H_PI * coef, pLevel);
                        // LOG_DEBUG(Physics, "reload {:.2f}, ammo {}, coef {:.2f}", reload, magazine->_ammo, coef);
                    }
                }
            }
        }

        Shape* shape = pshape->LevelOpaque(pLevel);
        int shapeSpec = shape->Special();
        const bool isCockpitProxy = (level == type->_insideView || GWorld->GetCameraType() == CamGunner);
        // The parent's blend census does not include the held weapon. Submit its
        // complete cockpit mesh once, rather than losing displays/glass when the
        // soldier has no blend pass of its own.
        if (isCockpitProxy && GSectionFilter == SectionClassFilter::BlendOnly)
            continue;
        const SectionClassFilter savedFilter = GSectionFilter;
        if (isCockpitProxy)
        {
            shapeSpec |= NoDropdown | FogDisabled;
            GSectionFilter = SectionClassFilter::All;
        }
        const render::PassKindHint savedHint = GEngine->GetPassKindHint();
        if (isCockpitProxy)
        {
            // FirstPersonBody, not Cockpit: this is the soldier's held weapon.
            // It needs the late first-person pass, but it is not an enclosed
            // interior and must keep the full sky ambient (MAT-052 follow-up).
            GEngine->SetPassKindHint(render::PassKindHint::FirstPersonBody);
        }
        shape->PrepareTextures(z2, shapeSpec);
        shape->Draw(this, lights, ClipAll, shapeSpec, pTransform, invPTransform);
        GSectionFilter = savedFilter;
        GEngine->SetPassKindHint(savedHint);
    }

    DrawArma2Head(level, transform, dist2, z2, lights);

    _mGunFireFrames--;
}

// An Arma 2 head model is skinned to the same skeleton as the body and modelled in the
// body's source coordinates. It is drawn moved by the head bone as one piece: the face does
// not move its own bones (no grimaces, no lip sync) and the neck seam follows the head.
void Man::DrawArma2Head(int level, const Matrix4& transform, float dist2, float z2, const LightList& lights)
{
    const ManType* type = Type();
    if (!_a2Head || !_showHead || type->_headMatIndex < 0 || level == type->_insideView ||
        _shape->Resolution(level) >= 900.0f)
        return;
    MATRIX_4_ARRAY(matrix, 128);
    PrepareCorrectedPose(level, matrix);
    const Matrix4 bone = type->_headMatIndex < matrix.Size() ? matrix[type->_headMatIndex] : Matrix4(MIdentity);
    // head-model stored coordinates -> body stored coordinates: both are stored around their
    // own bounding centre, AnimationOrigin is where their common source origin lies
    const Matrix4 toBody(MTranslation, _shape->AnimationOrigin() - _a2Head->AnimationOrigin());
    const Matrix4 pTransform = transform * bone * toBody;
    const int pLevel = GScene->LevelFromDistance2(_a2Head, dist2, pTransform.Scale(), pTransform.Direction(),
                                                  GScene->GetCamera()->Direction());
    if (pLevel < 0 || pLevel == LOD_INVISIBLE)
        return;
    if (_head._texture)
        _a2HeadFace.SetTexture(_a2Head, pLevel, _head._texture);
    Shape* shape = _a2Head->LevelOpaque(pLevel);
    if (!shape)
        return;
    int spec = shape->Special();
    shape->PrepareTextures(z2, spec);
    shape->Draw(this, lights, ClipAll, spec, pTransform, pTransform.InverseScaled());
}

void Man::DrawNVOptics()
{
    if (IsNVEnabled())
    {
        const int nvgIdx = getNvgIdx();
        Ref<WeaponType> goggles = WeaponTypes.New(nvgIdx >= 0 ? GetWeaponSystem(nvgIdx)->GetName() : "NVGoggles");
        const MuzzleType* muzzle = goggles->_muzzles[0];
        LODShapeWithShadow* oShape = muzzle->_opticsModel;
        if (oShape)
        {
            int phase = toIntFloor(5.0f * GRandGen.RandomValue());
            muzzle->_animFire.SetPhase(oShape, 0, phase);
            // 4:3 vignette — preserve 4:3 + pillarbox while bars are on, else stretch.
            const bool preserve4x3 = AspectRatio::ArePillarboxBarsEnabled();
            Draw2D(oShape, 0, PackedWhite, /*preserveAspect4x3*/ preserve4x3);
            Object::DrawWidescreenPillarbox();
        }
    }
}

void Man::DrawCameraCockpit()
{
    const float timeDelay = 0.3f;
    const float timePlus = 0.3f;
    const float timeMinus = 2;
    if (_whenScreamed < Glob.time + timeDelay && _whenScreamed >= Glob.time - (timeDelay + timePlus + timeMinus))
    {
        float redTime = Glob.time - _whenScreamed - timeDelay;
        float redFactor;
        if (redTime > timePlus)
        {
            redFactor = 1 - (redTime - timePlus) * (1.0f / timeMinus);
        }
        else
        {
            redFactor = redTime * (1.0f / timePlus);
        }
        if (redFactor > 0)
        {
            float alpha = redFactor * 0.5f;
            PackedColor color = PackedColor(Color(0.5f, 0, 0, alpha));
            const float w = GEngine->Width();
            const float h = GEngine->Height();
            MipInfo mip = GEngine->TextBank()->UseMipmap(nullptr, 0, 0);
            GEngine->Draw2D(mip, color, Rect2DAbs(0, 0, w, h));
        }
    }
}

int Man::PassNum(int lod)
{
    if (GEngine->CanGrass())
    {
        if (!_shape)
        {
            return 0;
        }
        Shape* shape = _shape->Level(lod);
        if (!shape)
        {
            return 0;
        }
        const int spec = shape->Special() | GetObjSpecial();
        const render::LegacySpec specT = render::SplitLegacy(spec);
        // soldier cockpit is pass 2 (alpha-transparent objects)
        // it may often interact with the scene, especially with grass
        // therefore it should not be drawn in pass 3 as other cockpits
        if (render::Has(specT.routing, render::Routing::NoDropdown))
        {
            if (GWorld->GetCameraType() == CamGunner)
            {
                return 3;
            }
            if (render::Has(specT.backend, render::Backend::IsAlpha))
            {
                return 2; // alpha-transparent objects interacting with grass
            }
            return 1;
        }
    }
    return base::PassNum(lod);
}

void Man::Draw(int level, ClipFlags clipFlags, const FrameBase& pos)
{
    static const bool playerTrace = [] {
        const char* v = std::getenv("POSEIDON_PLAYER_GROUND_TRACE");
        return v && v[0] == '1';
    }();
    static float nextTrace = -1.0f;
    const float now = playerTrace ? Glob.time.toFloat() : 0.0f;
    if (playerTrace && GWorld && GWorld->PlayerOn() == this && (now >= nextTrace || now < nextTrace-1.0f))
    {
        nextTrace = now + 0.5f;
        const auto p = pos.Position();
        LOG_INFO(Graphics, "PLAYER_GROUND_DRAW time={:.3f} camera={} level={} inside={} model={} x={:.3f} y={:.3f} z={:.3f} ground={:.3f} mud={:.3f} sand={:.3f} dead={} hidden={:.3f}",
            now, int(GWorld->GetCameraType()), level, InsideLOD(GWorld->GetCameraType()), _shape->Name(),
            p.X(), p.Y(), p.Z(), GLandscape ? GLandscape->SurfaceY(p.X(),p.Z()) : p.Y(),
            GMud().HeightOffsetAt(p.X(),p.Z()), GSand().HeightOffsetAt(p.X(),p.Z()), IsDead(), _hideBody);
    }
    if (level == LOD_INVISIBLE)
    {
        return;
    }
#if _ENABLE_CHEATS
    if (GWorld->CameraOn() == this)
    {
        if (CHECK_DIAG(DETransparent))
        {
            return;
        }
    }
#endif
    // Through Arma 2 3D sights the soldier is drawn as in first person -- the camera sits
    // at the weapon's "eye" -- rather than replaced by the 2D optics overlay.
    if (GWorld->GetCameraType() == CamGunner && GWorld->CameraOn() == this && !Optics3DSelected())
    {
        if (_currentWeapon >= 0 && _currentWeapon < NMagazineSlots())
        {
            WeaponType* weapon = GetMagazineSlot(_currentWeapon)._weapon;
            MuzzleType* muzzle = GetMagazineSlot(_currentWeapon)._muzzle;
            if (muzzle)
            {
                LODShapeWithShadow* oShape = GetOpticsModel(this);
                if (oShape)
                {
                    bool showFire = false;
                    if (_mGunFireFrames > 0 || Glob.uiTime < _mGunFireTime + 0.05f)
                    {
                        if ((weapon->_weaponType & MaskSlotPrimary) != 0)
                        {
                            showFire = true;
                        }
                    }

                    if (showFire)
                    {
                        muzzle->_animFire.Unhide(oShape, 0);
                        muzzle->_animFire.SetPhase(oShape, 0, _mGunFirePhase);
                    }
                    else
                    {
                        muzzle->_animFire.Hide(oShape, 0);
                    }
                    _mGunFireFrames--;
                    // binoculars are a 4:3 vignette (scopes keep full width) — stretch when bars off.
                    const bool isBinocular = BinocularSelected();
                    const bool preserve4x3 = isBinocular && AspectRatio::ArePillarboxBarsEnabled();
                    Draw2D(oShape, 0, GetOpticsColor(this), /*preserveAspect4x3*/ preserve4x3);
                    if (isBinocular)
                        Object::DrawWidescreenPillarbox();
                }
            }
        }
        return;
    }

    base::Draw(level, clipFlags, pos);
}

#if ALPHA_SPLIT

void Man::DrawAlpha(int level, ClipFlags clipFlags)
{
    if (level == LOD_INVISIBLE)
        return;
    float resol = _shape->Resolution(level);
    if (resol >= VIEW_GUNNER * 0.99f && resol <= VIEW_GUNNER * 1.01f)
    {
        Draw2D(level);
        return;
    }
    base::DrawAlpha(level, clipFlags);
}
#endif

bool Man::IsAnimated(int level) const
{
    return true;
}
bool Man::IsAnimatedShadow(int level) const
{
    return !ShadowPoseFrozen(); // settled corpse shadow no longer changes
}

void Man::AttachWave(IWave* wave, float freq)
{
    _head.AttachWave(wave, freq);
}

float Man::GetSpeaking() const
{
    if (_head._randomLip || _head._lipInfo)
    {
        return 1;
    }
    return 0;
}

void Man::SetRandomLip(bool set)
{
    _head.SetRandomLip(set);
}

void Man::ShowHead(int level, bool show)
{
    _showHead = show;
    if (show)
    {
        Type()->_headHide.Unhide(_shape, level);
        Type()->_neckHide.Unhide(_shape, level);
    }
    else
    {
        Type()->_headHide.Hide(_shape, level);
        Type()->_neckHide.Hide(_shape, level);
    }
}

void Man::ShowWeapons(bool showPrimary, bool showSecondary)
{
    _showPrimaryWeapon = showPrimary;
    _showSecondaryWeapon = showSecondary;
}

float Man::LandSlope(bool& forceStand) const
{
    float dx, dz;
    Texture* tex = nullptr;
    Object* obj = nullptr;
    forceStand = false;
    GLandscape->RoadSurfaceY(Position() + VUp * 0.5f, &dx, &dz, &tex, &obj);
    if (obj)
    {
        forceStand = true;
    }
    float slope = dx * Direction().X() + dz * Direction().Z();
    return slope;
}

bool Man::IsAbleToStand() const
{
    if (GetHit(Type()->_legsHit) >= 0.9f)
    {
        return false;
    }
    // check landscape slope
    return true;
}

bool Man::IsAbleToFire() const
{
    return true;
}

bool Man::Optics3DSelected() const
{
    if (_currentWeapon < 0 || _currentWeapon >= NMagazineSlots() || !EnableOptics())
        return false;
    if (LauncherSelected() && !EnableMissile()) // as GetOpticsModel
        return false;
    const MagazineSlot& slot = GetMagazineSlot(_currentWeapon);
    return slot._muzzle && slot._muzzle->_optics3D && slot._weapon && slot._weapon->_model &&
           slot._weapon->_hasEyePos;
}

bool Man::LooksThroughSights(Person* person) const
{
    return person == this && Optics3DSelected();
}

LODShapeWithShadow* Man::GetOpticsModel(Person* person)
{
    LODShapeWithShadow* ret = base::GetOpticsModel(person);
    if (!ret)
    {
        return ret;
    }
    if (BinocularSelected() && !EnableBinocular())
    {
        return nullptr;
    }
    if (LauncherSelected() && !EnableMissile())
    {
        return nullptr;
    }
    if (!EnableOptics())
    {
        return nullptr;
    }
    return ret;
}

bool Man::GetForceOptics(Person* person) const
{
    bool ret = base::GetForceOptics(person);
    if (!ret)
    {
        return false;
    }
    if (!EnableBinocular())
    {
        return false;
    }
    return true;
}

const float ManHitLimit = 0.7f;
const float ManHitLimitHead = 0.8f;

void Man::WoundsAnimation(int level)
{
    if (!ENGINE_CONFIG.blood)
    {
        return;
    }

    const ManType* type = Type();
    bool bodyWound = GetHitCont(type->_bodyHit) >= ManHitLimit;
    bool headWound = GetHitCont(type->_headHit) >= ManHitLimitHead;
    bool handsWound = GetHitCont(type->_handsHit) >= ManHitLimit;
    bool legsWound = GetHitCont(type->_legsHit) >= ManHitLimit;
    if (bodyWound)
    {
        type->_bodyWound.Apply(_shape, level);
    }
    if (/*_showHead && */ headWound)
    {
        type->_headWound.ApplyModified(_shape, level, type->_head._textureOrig, _head._textureWounded);
    }
    if (handsWound)
    {
        type->_lArmWound.ApplyModified(_shape, level, type->_head._textureOrig, _head._textureWounded);
        type->_rArmWound.ApplyModified(_shape, level, type->_head._textureOrig, _head._textureWounded);
    }
    if (legsWound)
    {
        type->_lLegWound.Apply(_shape, level);
        type->_rLegWound.Apply(_shape, level);
    }
}

void Man::WoundsDeanimation(int level)
{
    if (!ENGINE_CONFIG.blood)
    {
        return;
    }

    const ManType* type = Type();
    bool bodyWound = GetHitCont(type->_bodyHit) >= ManHitLimit;
    bool headWound = GetHitCont(type->_headHit) >= ManHitLimitHead;
    bool handsWound = GetHitCont(type->_handsHit) >= ManHitLimit;
    bool legsWound = GetHitCont(type->_legsHit) >= ManHitLimit;
    if (bodyWound)
    {
        type->_bodyWound.Restore(_shape, level);
    }
    if (/*_showHead && */ headWound)
    {
        type->_headWound.Restore(_shape, level);
    }
    if (handsWound)
    {
        type->_lArmWound.Restore(_shape, level);
        type->_rArmWound.Restore(_shape, level);
    }
    if (legsWound)
    {
        type->_lLegWound.Restore(_shape, level);
        type->_rLegWound.Restore(_shape, level);
    }
}

void Man::BasicAnimation(int level)
{
    const ManType* type = Type();

    Shape* shape = _shape->Level(level);
    if (shape)
    {
        {
            Matrix4 headOrient(MDirection, -VAside, VUp);
            AnimateMatrix(headOrient, Type()->_headOnlyWeight);
            Matrix3 headTrans;
            if (_headTransIdent && _gunTransIdent)
            {
                headTrans = headOrient.Orientation();
            }
            else
            {
                headTrans = headOrient.Orientation() * GunTransform().Orientation() * _headTrans.Orientation();
            }
            _head.Animate(type->_head, _shape, level, _isDead, headTrans, !_showHead);
        }

        WoundsAnimation(level);

        Vector3 min = shape->MinOrig();
        Vector3 max = shape->MaxOrig();
        Vector3 bCenter = shape->BSphereCenterOrig();
        float bRadius = shape->BSphereRadiusOrig();
        // enlarge to encompass the full animated pose range
        Vector3 factor(2, 1.2f, 3);
        float sFactor = 3;
        min = bCenter + factor.Modulate(min - bCenter);
        max = bCenter + factor.Modulate(max - bCenter);
        bRadius *= sFactor;
        Vector3 articulated[2]; float articulatedRadius = 0;
        if (CorpseArticulationBounds(articulated,articulatedRadius))
        { min = articulated[0]; max = articulated[1]; bCenter = VZero; bRadius = articulatedRadius; }
        shape->SetMinMax(min, max, bCenter, bRadius);
    }
    base::Animate(level);
}

void Man::AnimatedMinMax(int level, Vector3* minMax)
{
    float articulatedRadius = 0;
    if (CorpseArticulationBounds(minMax,articulatedRadius)) return;
    Shape* shape = _shape->Level(level);

    Vector3 min = shape->MinOrig();
    Vector3 max = shape->MaxOrig();
    Vector3 bCenter = shape->BSphereCenterOrig();
    float factor = 2;
    minMax[0] = bCenter + (min - bCenter) * factor;
    minMax[1] = bCenter + (max - bCenter) * factor;
}

void Man::AnimatedBSphere(int level, Vector3& bCenter, float& bRadius, bool isAnimated)
{
    Shape* shape = _shape->Level(level);

    bCenter = shape->BSphereCenterOrig();
    float factor = 2;
    bRadius = shape->BSphereRadiusOrig() * factor;
}

void Man::BasicDeanimation(int level)
{
    const ManType* type = Type();
    Shape* shape = _shape->Level(level);
    if (shape)
    {
        WoundsDeanimation(level);

        {
            Matrix4 headOrient(MDirection, -VAside, VUp);
            AnimateMatrix(headOrient, Type()->_headOnlyWeight);

            Matrix3 headTrans;
            if (_headTransIdent && _gunTransIdent)
            {
                headTrans = headOrient.Orientation();
            }
            else
            {
                headTrans = headOrient.Orientation() * GunTransform().Orientation() * _headTrans.Orientation();
            }
            _head.Deanimate(type->_head, _shape, level, _isDead, headTrans, !_showHead);
        }
    }

    base::Deanimate(level);
}

UnitPosition Man::GetUnitPosition() const
{
    return _unitPos;
}

void Man::SetUnitPosition(UnitPosition status)
{
    _unitPos = status;
}

void Man::ApplyAnimation(int level, RStringB move, float time)
{
    // An explicitly requested authored pose supersedes the diagnostic hold.
    if (_corpsePoseLease.world) ClearCorpsePose();
    const ManType* type = Type();
    WeightInfo& weights = type->GetWeights();
    MoveId moveId = type->GetMoveId(move);
    AnimationRT* anim = type->GetAnimation(moveId);
    RequestFaceCpuPose(level); // FACE-001, before Apply reaches ApplyMatrices
    if (anim)
    {
        // FACE-002: AnimationRT::Apply's own Prepare+ApplyMatrices is inlined here so the
        // face bones can be written between the two. Without that step they would reach
        // ApplyMatrices as the identity PrepareMatrices leaves them at, and the face
        // vertices -- which are bound to them -- would stay at bind pose while the head
        // moved, i.e. the face would come off the head.
        MATRIX_4_ARRAY(matrix, 128);
        anim->PrepareMatrices(matrix, time, 1);
        // Arma 1 shapes are stored around their bounding centre (see PrepareBasePose).
        if (const Vector3 o = GetShape()->AnimationOrigin(); o.SquareSize() > 1e-8f)
            for (int i = 0; i < matrix.Size(); i++)
                matrix[i].SetPosition(matrix[i].FastTransform(-o) + o);
        if (matrix.Size() > 0)
        {
            // AnimationRT::Apply returns early on an animation with no phases; matching
            // that here matters, because ApplyMatrices on an empty array means "restore
            // the bind pose", which Apply would never have asked for.
            _head.ApplyFaceBones(Type()->_head, weights, level, matrix, _isDead);
            AnimationRT::ApplyMatrices(weights, GetShape(), level, matrix);
        }
    }
    BasicAnimation(level);
}

void Man::ApplyDeanimation(int level)
{
    BasicDeanimation(level);
}

float Man::GetAnimSpeed(RStringB move)
{
    const ManType* type = Type();
    MoveId moveId = type->GetMoveId(move);
    const MoveInfo* info = Type()->GetMoveInfo(moveId);
    if (info)
    {
        return info->GetSpeed();
    }
    return 1;
}

Vector3 Man::GetPilotPosition(CameraType camType) const
{
    const ManType* type = Type();
    int level = _shape->FindMemoryLevel();

    int selIndex = type->_pilotPoint;
    if (selIndex < 0 || level < 0 || !_shape->Level(level))
    {
        return VZero;
    }
    const NamedSelection& sel = _shape->Level(level)->NamedSel(selIndex);
    if (sel.Size() <= 0)
    {
        return VZero;
    }
    return AnimatePoint(level, sel[0]);
}

// FACE-001: the face rig writes CPU vertex offsets (Head::Animate -> OffsetAnimation).
// A GPU-skinned mesh is uploaded once at bind pose and posed in the vertex shader, so
// those offsets never reach the screen. Ask for a CPU-posed frame while the head has any
// such offset to apply -- the request is read by AnimationRT::ApplyMatrices (which then
// skins on the CPU) and by the backend (which then draws this mesh unskinned and uploads
// the posed vertices). It MUST be set before ApplyMatrices, not in BasicAnimation, which
// runs after it.
void Man::RequestFaceCpuPose(int level)
{
    Shape* shape = _shape->Level(level);
    VertexBuffer* vb = shape ? shape->GetVertexBuffer() : nullptr;
    FaceAnimDiag& d = GFaceDiag();
    d.request++;
    if (!vb)
    {
        d.requestNoVb++;
    }
    else
    {
        vb->cpuPosed = _head.HasVertexAnimation();
        if (vb->cpuPosed)
        {
            d.requestSet++;
            d.poseLevel = level;
            d.poseVerts = shape->NVertex();
            d.vbSet = reinterpret_cast<unsigned long long>(vb);
        }
        else
        {
            d.vbUnset = reinterpret_cast<unsigned long long>(vb);
        }
    }
    FaceDiagTick("request");
}

void Man::PrepareCorrectedPose(int level, Matrix4Array& matrix) const
{
    if (_automaticCorpsePose && UseCorpsePose() && level >= 0 && level < int(_automaticCorpsePose->levels.size()))
    {
        const auto& palette = _automaticCorpsePose->levels[level].palette;
        matrix.Resize(int(palette.size()));
        for (size_t bone = 0; bone < palette.size(); ++bone) matrix[int(bone)] = palette[bone];
        return;
    }
    const ManType* type = Type();
    PrepareBasePose(matrix);

    if (matrix.Size() > 0)
    {
        int memory = _shape->FindMemoryLevel();
        if (type->_headAxisPoint >= 0 && !_headTransIdent)
        {
            BLEND_ANIM(head);
            const BlendAnimSelections& headRes = GetHead(head);

            AnimationRT::CombineTransform(type->GetWeights(), _shape, memory, matrix, _headTrans, headRes.Data(),
                                          headRes.Size());
        }

        if (type->_aimingAxisPoint >= 0 && !_gunTransIdent)
        {
            BLEND_ANIM(aiming);
            const BlendAnimSelections& aimingRes = GetAiming(aiming);

            AnimationRT::CombineTransform(type->GetWeights(), _shape, memory, matrix, _gunTrans, aimingRes.Data(),
                                          aimingRes.Size());
        }

        // legs selection needs to be applied only to graphical lods
        if (_shape->Resolution(level) < 1000 || level == type->_insideView)
        {
            BLEND_ANIM(legs);
            const BlendAnimSelections& legsRes = GetLegs(legs);

            // apply legs selections
            AnimationRT::CombineTransform(type->GetWeights(), _shape, memory, matrix, _legTrans, legsRes.Data(),
                                          legsRes.Size());
        }

    }
}

void Man::Animate(int level)
{
    Shape* shape = _shape->Level(level);
    if (!shape)
    {
        return;
    }
    RequestFaceCpuPose(level);
    const ManType* type = Type();

    // check for special case: animating one-point level
    if (shape->NPos() == 1)
    {
        // singular case
        shape->SaveOriginalPos();
        shape->SetPos(0) = AnimatePoint(level, 0);
        return;
    }
    MATRIX_4_ARRAY(matrix, 128);

    PrepareCorrectedPose(level, matrix);

    if (matrix.Size() > 0)
    {
        // FACE-002: the face pose rides reserved bones of this object's own palette. It has
        // to be written after the head/aiming/legs CombineTransform above (it reads the
        // parent bone's final orientation) and before ApplyMatrices consumes the array.
        if (_automaticCorpsePose && UseCorpsePose()) _head._faceBonesActive = true;
        else _head.ApplyFaceBones(type->_head, type->GetWeights(), level, matrix, _isDead);

        AnimationRT::ApplyMatrices(type->GetWeights(), _shape, level, matrix);
        if (level == type->_insideView && level > 0 && GWorld && GWorld->CameraOn() == this)
            LogViewLodPose(_shape, type->GetWeights(), level, matrix, GetPilotPosition(CamInternal));
    }

    BasicAnimation(level);
}

void Man::Deanimate(int level)
{
    Shape* shape = _shape->Level(level);
    if (!shape)
    {
        return;
    }
    if (shape->NPos() == 1)
    {
        return;
    }

    BasicDeanimation(level);
}

DEFINE_FAST_ALLOCATOR(ActionMap)

template <>
const EnumName* Foundation::GetEnumNames(ManAction action)
{
    static const EnumName ManActionNames[] = {
#define ACTION(x) EnumName(ManAct##x, #x),
#include <Poseidon/World/Entities/Infantry/ManActions.hpp>
#undef ACTION
        EnumName()};
    return ManActionNames;
}

// The move an action map gives for `action`. Arma 1 move sets (A1_CfgMovesMaleSdr)
// name the vehicle boarding actions by height instead of by vehicle; an absent
// OFP action falls back to its Arma 1 equivalent rather than to a config error.
static RStringB ActionMove(const ParamEntry& map, const char* action)
{
    if (const ParamEntry* entry = map.FindEntry(action))
        return *entry;
    static const char* const aliases[][2] = {
        {"GetInCar", "GetInLow"}, {"GetOutCar", "GetOutLow"}, {"GetInTank", "GetInMedium"}, {"GetOutTank", "GetOutMedium"}};
    for (const auto& alias : aliases)
        if (_stricmp(action, alias[0]) == 0)
            if (const ParamEntry* entry = map.FindEntry(alias[1]))
                return *entry;
    return map >> action;
}

ActionMap::ActionMap(const ActionMapName& name)
{
    _name = name;
    const ParamEntry& e = *_name.entry;

    _turnSpeed = e >> "turnSpeed";
    _upDegree = e >> "upDegree";
    _limitFast = e >> "limitFast";

#define ACTION(x) _actions[ManAct##x] = _name.motion->GetMoveId(ActionMove(e, #x));
#include <Poseidon/World/Entities/Infantry/ManActions.hpp>
#undef ACTION
}

MovesType::MovesType(const MovesTypeName& name)
{
    _name = name;

    _moves.Realloc(_name.motionType->MoveIdN());
    _moves.Resize(_name.motionType->MoveIdN());

    const ParamEntry& moves = _name.motionType->GetEntry();
    const ParamEntry& states = moves >> "States";

    const ParamEntry* statesExt = moves.FindEntry("StatesExt");

    // load appropriate skeleton?
    WeightInfoName wname;
    wname.shape = _name.shape;
    wname.skeleton = _name.motionType->GetSkeleton();

    LODShapeWithShadow* shape = _name.shape;
    _weights = WeigthBank.New(wname);
    bool anyAnimation = false;
    for (int i = 0; i < _moves.Size(); i++)
    {
        MoveId id = MoveId(i);
        RStringB moveName = _name.motionType->GetMoveName(id);
        if (!states.FindEntry(moveName))
        {
            continue;
        }
        const ParamEntry* entry = &(states >> moveName);
        if (statesExt)
        {
            const ParamEntry* entryExt = statesExt->FindEntry(moveName);
            if (entryExt)
            {
                entry = entryExt;
            }
        }
        MoveInfo& moveI = _moves[i];
        moveI = MoveInfo(_name.motionType, *entry);

        AnimationRT* anim = moveI;
        if (anim)
        {
            anyAnimation = true;
            anim->SetLooped((*entry) >> "looped");
        }
    }
    // The weights depend only on the skeleton's bone list, which the moves above have now
    // completed, so one Prepare after the loop gives what the last per-move Prepare gave.
    // Preparing after every move re-weighted every LOD hundreds of times: with Arma 2's
    // ~2000 moves and 132-bone soldiers that alone took over a minute per soldier model on
    // the laptop, and the stall watchdog ended the test before the mission started.
    if (anyAnimation)
        _name.motionType->GetSkeleton()->Prepare(shape, GetWeights());

    // FACE-002: after the LAST Prepare -- Prepare re-Inits the weights, so a rig built
    // inside the loop would be wiped by the next move.
    SetupFaceBoneRig(_name.motionType->GetSkeleton(), shape, GetWeights());
}

MovesType::~MovesType() = default;

DynEnum GActionVehNames;

void ManCompact()
{
    MovesTypes.Compact();
}

} // namespace Poseidon
void ManCleanUp()
{
    using namespace Poseidon;
    AnimationRTBank.Clear();
    WeigthBank.Clear();
    MovesTypes.Clear();
}
namespace Poseidon
{

BlendAnimSelections::BlendAnimSelections() = default;

static int CompBlendAnim(const BlendAnimInfo* i1, const BlendAnimInfo* i2)
{
    return i1->matrixIndex - i2->matrixIndex;
}

void BlendAnimSelections::Load(Skeleton* skelet, const ParamEntry& cfg)
{
    Clear();
    Realloc(cfg.GetSize() / 2);
    for (int i = 0; i < cfg.GetSize() - 1; i += 2)
    {
        RStringB name = cfg[i];
        float factor = cfg[i + 1];
        int index = skelet->FindBone(name);
        if (index < 0)
        {
            // Skeleton bone names are lower case (the RTM loader lowers them); OFP's
            // configs name bones in lower case too, Arma 1's do not ("LeftShoulder").
            char lower[64];
            snprintf(lower, sizeof(lower), "%s", (const char*)name);
            for (char* c = lower; *c; c++)
                *c = static_cast<char>(tolower(static_cast<unsigned char>(*c)));
            index = skelet->FindBone(RStringB(lower));
        }
        if (index < 0)
        {
            // An unknown bone used to stay in the list as matrix index -1, and the
            // blend then read the matrix before the palette: a garbage transform.
            continue;
        }
        BlendAnimInfo& info = Set(Add());
        info.matrixIndex = index;
        info.factor = factor;
    }
    // sort by matrix index
    QSort(Data(), Size(), CompBlendAnim);
}

void BlendAnimSelections::AddOther(const BlendAnimSelections& src, float factor)
{
    // both arrays are sorted by matrixIndex; process the lower one each step to stay in sync
    int i = 0, srcI = 0;

    while (i < Size() && srcI < src.Size())
    {
        BlendAnimInfo& info = Set(i);
        const BlendAnimInfo& srcInfo = src.Get(srcI);
        if (info.matrixIndex == srcInfo.matrixIndex)
        {
            info.factor += srcInfo.factor * factor;
            i++;
            srcI++;
        }
        else if (info.matrixIndex > srcInfo.matrixIndex)
        {
            BlendAnimInfo& set = Set(Add());
            set.matrixIndex = srcInfo.matrixIndex;
            set.factor = srcInfo.factor * factor;
            srcI++;
        }
        else
        {
            i++;
        }
    }
    for (; srcI < src.Size(); srcI++)
    {
        const BlendAnimInfo& srcInfo = src.Get(srcI);
        BlendAnimInfo& set = Set(Add());
        set.matrixIndex = srcInfo.matrixIndex;
        set.factor = srcInfo.factor * factor;
    }
}

BlendAnimType::BlendAnimType(const BlendAnimTypeName& name)
{
    _name = name;
    Load(name.motion->GetSkeleton(), *name.cfg);
}

MoveInfo::MoveInfo(MotionType* motion, const ParamEntry& entry)
{
    _motion = motion;

    const ParamEntry& actionMaps = motion->GetEntry() >> "Actions";
    const ParamEntry& blendAnimTypes = motion->GetEntry() >> "BlendAnims";

    motion->InitNoActions(&(actionMaps >> "NoActions"));

    RStringB file = entry >> "file";
    if (file.GetLength() > 0)
    {
        AnimationRTName name;
        name.name = GetAnimationName(file);
        name.skeleton = motion->GetSkeleton();

        bool preload = entry >> "preload";
        _move = AnimationRTBank.New(name);
        if (preload)
        {
            _move->AddPreloadCount();
        }
    }
    RStringB actionsName = entry >> "Actions";
    _actions = motion->NewActionMap(&(actionMaps >> actionsName));
    _disableWeapons = entry >> "disableWeapons";
    _disableWeaponsLong = entry >> "disableWeaponsLong";
    _enableOptics = entry >> "enableOptics";
    _showWeaponAim = entry >> "showWeaponAim";
    _enableMissile = entry >> "enableMissile";
    _enableBinocular = entry >> "enableBinocular";

    _showItemInHand = entry >> "showItemInHand";
    _showItemInRightHand = entry >> "showItemInRightHand";
    _showHandGun = entry >> "showHandGun";

    _onLandBeg = entry >> "onLandBeg";
    _onLandEnd = entry >> "onLandEnd";
    _onLadder = entry >> "onLadder";

    _speed = entry >> "speed";
    if (_speed < 0)
    {
        _speed = -1 / _speed;
    }
    _duty = entry >> "duty";
    _soundEnabled = entry >> "soundEnabled";

    _soundOverride = entry >> "soundOverride"; // default - no override
    _soundEdge1 = entry >> "soundEdge1";       // default - no override
    _soundEdge2 = entry >> "soundEdge2";       // default - no override

    _relSpeedMin = entry >> "relSpeedMin";
    _relSpeedMax = entry >> "relSpeedMax";
    RStringB aimingName = entry >> "aiming";
    RStringB legsName = entry >> "legs";
    RStringB headName = entry >> "head";
    _aiming = motion->NewBlendAnimType(blendAnimTypes >> aimingName);
    _legs = motion->NewBlendAnimType(blendAnimTypes >> legsName);
    _head = motion->NewBlendAnimType(blendAnimTypes >> headName);

    _equivalentTo = motion->GetMoveId(entry >> "equivalentTo");
    _variantAfterMin = (entry >> "variantAfter")[0];
    _variantAfterMid = (entry >> "variantAfter")[1];
    _variantAfterMax = (entry >> "variantAfter")[2];
    _interpolSpeed = entry >> "interpolationSpeed";
    _interpolRestart = entry >> "interpolationRestart";

    _limitGunMovement = entry >> "limitGunMovement";

    _terminal = entry >> "terminal"; // all movement stops here

    _aimPrecision = entry >> "aimPrecision";

    _visibleSize = entry >> "visibleSize";

    LoadVariants(_variantsPlayer, nullptr, entry >> "variantsPlayer");
    LoadVariants(_variantsAI, &_variantsPlayer, entry >> "variantsAI");
}

void MoveInfo::LoadVariants(AutoArray<MoveVariant>& vars, AutoArray<MoveVariant>* defaultVars,
                            const ParamEntry& cfg) const
{
    // check first name
    // if the array is {""}, use default variants (if any)
    if (cfg.GetSize() == 1)
    {
        RStringB single = cfg[0];
        if (single.GetLength() == 0)
        {
            if (defaultVars)
            {
                vars = *defaultVars;
                return;
            }
            else
            {
                RptF("No default vars in %s", (const char*)cfg.GetContext());
                return;
            }
        }
    }

    float totalP = 0;
    for (int i = 0; i < cfg.GetSize(); i += 2)
    {
        RStringB name = cfg[i];
        float prop = i + 1 < cfg.GetSize() ? cfg[i + 1] : 1 - totalP;
        MoveVariant& var = vars[vars.Add()];
        var._move = _motion->GetMoveId(name);
        var._probab = prop;
        totalP += prop;
    }
}

MoveId MoveInfo::RandomVariant(const AutoArray<MoveVariant>& vars, float rnd) const
{
    int i = 0;
    for (; i < vars.Size(); i++)
    {
        float prop = vars[i]._probab;
        rnd -= prop;
        if (rnd <= 0)
        {
            break;
        }
    }
    if (i >= vars.Size())
    {
        return MoveIdNone;
    }
    return vars[i]._move;
}

float MoveInfo::GetVariantAfter() const
{
    return GRandGen.Gauss(_variantAfterMin, _variantAfterMid, _variantAfterMax);
}

MoveId MoveInfo::RandomVariantPlayer() const
{
    float rnd = GRandGen.RandomValue();
    return RandomVariant(_variantsPlayer, rnd);
}

MoveId MoveInfo::RandomVariantAI() const
{
    float rnd = GRandGen.RandomValue();
    return RandomVariant(_variantsAI, rnd);
}

ManType::ManType(const ParamEntry* param) : VehicleType(param)
{
    _scopeLevel = 1;
}

void ManType::Load(const ParamEntry& par)
{
    base::Load(par);

    _lightPos = VZero;
    _lightDir = VZero;

    _isMan = par >> "isMan";
    _minGunElev = (float)(par >> "minGunElev") * (H_PI / 180);
    _maxGunElev = (float)(par >> "maxGunElev") * (H_PI / 180);
    _minGunTurn = (float)(par >> "minGunTurn") * (H_PI / 180);
    _maxGunTurn = (float)(par >> "maxGunTurn") * (H_PI / 180);
    _minGunTurnAI = (float)(par >> "minGunTurnAI") * (H_PI / 180);
    _maxGunTurnAI = (float)(par >> "maxGunTurnAI") * (H_PI / 180);

    // trieder fixed to ±30°/±60° — not config-driven
    _minTriedrElev = -30 * (H_PI / 180);
    _maxTriedrElev = +60 * (H_PI / 180);
    _minTriedrTurn = -30 * (H_PI / 180);
    _maxTriedrTurn = +30 * (H_PI / 180);

    _minHeadTurnAI = (float)(par >> "minHeadTurnAI") * (H_PI / 180);
    _maxHeadTurnAI = (float)(par >> "maxHeadTurnAI") * (H_PI / 180);

    _canHideBodies = par >> "canHideBodies";
    _canDeactivateMines = par >> "canDeactivateMines";

    _head.Load(par);

    _hitSound.Load(par, "hitSounds");

    GetValue(_addSound, par >> "additionalSound");

    _woman = false;
    const ParamEntry* entry = par.FindEntry("woman");
    if (entry)
    {
        _woman = *entry;
    }
}

// Engine-facing names (bones, memory points, selections) default to OFP's Czech
// names. A class may name its own -- the Arma 1 properties (boneHead, weaponBone,
// memoryPointAim, selectionHeadWound, ...) are read where present, so an Arma 1
// model can be driven by this code without renaming its selections.
static RStringB ManNameOr(const ParamEntry& par, const char* key, const char* fallback)
{
    const ParamEntry* entry = par.FindEntry(key);
    if (!entry || entry->IsClass() || entry->IsArray())
        return RStringB(fallback);
    RStringB value = *entry;
    return value.GetLength() > 0 ? value : RStringB(fallback);
}

void ManType::InitShape()
{
    _scopeLevel = 2;

    base::InitShape();

    const ParamEntry& par = *_par;

    for (int l = 0; l < MAX_LOD_LEVELS; l++)
    {
        _nvGogglesProxyIndex[l] = -1;
        if (l >= _shape->NLevels())
        {
            continue;
        }
        Shape* shape = _shape->Level(l);
        if (!shape)
        {
            continue;
        }
        for (int p = 0; p < shape->NProxies(); p++)
        {
            const ProxyObject& po = shape->Proxy(p);
            if (strcmpi(po.name, "nvg_proxy"))
            {
                continue;
            }
            _nvGogglesProxyIndex[l] = p;
            break;
        }
    }

    Shape* mem = _shape->MemoryLevel();
    PoseidonAssert(mem);

    // An Arma 2 man names its memory points in English ("aimpoint", "aiming_axis",
    // "head_axis", "footstepl"); the imported class does not carry CAManBase's
    // memoryPoint* entries, so the OFP default is tried first and then the Arma 2 name.
    // Without "aiming_axis" the player's aim turned his rifle about his feet.
    const auto pointOr = [&](const char* key, const char* ofpName, const char* a2Name) {
        if (!mem)
            return -1;
        int index = mem->PointIndex(ManNameOr(par, key, ofpName));
        if (index < 0 && a2Name)
            index = mem->PointIndex(a2Name);
        return index;
    };
    _cameraPoint = pointOr("memoryPointAim", "zamerny", "aimpoint");
    _pilotPoint = mem ? mem->FindNamedSel("pilot") : -1;
    // An EMPTY "pilot" selection is no pilot point. Every reader takes sel[0] of it (the camera /
    // eye position), and community MLODs ship memory selections with no points -- fml_vorhees'
    // Jason models (loco*.p3d) crashed the game in Man::CalculateCameraPosition on spawn, in a
    // mission or an editor preview. OFP shrugged these off; the readers already fall back to the
    // aiming position when this is -1.
    if (_pilotPoint >= 0 && mem->NamedSel(_pilotPoint).Size() <= 0)
    {
        LOG_WARN(Physics, "{}: memory selection 'pilot' has no points; using the aiming position",
                 (const char*)GetName());
        _pilotPoint = -1;
    }

    _gunPosIndex = mem ? mem->PointIndex("usti hlavne") : -1;
    _gunEndIndex = mem ? mem->PointIndex("konec hlavne") : -1;

    WoundInfo woundInfo;
    woundInfo.LoadAndRegister(_shape, par >> "wounds");
    _headWound.Init(_shape, woundInfo, ManNameOr(par, "selectionHeadWound", "head injury"), nullptr);
    _bodyWound.Init(_shape, woundInfo, ManNameOr(par, "selectionBodyWound", "body injury"), nullptr);
    _lArmWound.Init(_shape, woundInfo, ManNameOr(par, "selectionLArmWound", "l arm injury"), nullptr);
    _rArmWound.Init(_shape, woundInfo, ManNameOr(par, "selectionRArmWound", "r arm injury"), "p arm injury");
    _lLegWound.Init(_shape, woundInfo, ManNameOr(par, "selectionLLegWound", "l leg injury"), nullptr);
    _rLegWound.Init(_shape, woundInfo, ManNameOr(par, "selectionRLegWound", "r leg injury"), "p leg injury");

    _head.InitShape(par, _shape);

    _headHide.Init(_shape, ManNameOr(par, "selectionHeadHide", "hlava"), nullptr);
    _neckHide.Init(_shape, ManNameOr(par, "selectionNeckHide", "krk"), nullptr);

    _lightPos = _shape->MemoryPoint("L svetlo");
    _lightDir = _shape->MemoryPoint("konec L svetla") - _lightPos;
    _lightDir.Normalize();

    _insideView = 0;
    _gunnerView = 0;

    _stepLIndex = pointOr("memoryPointLStep", "stopaL", "footstepl");
    _stepRIndex = pointOr("memoryPointRStep", "stopaP", "footstepr");

    _aimPoint = -1;
    _aimingAxisPoint = -1;
    _headAxisPoint = -1;
    if (mem)
    {
        _aimPoint = pointOr("memoryPointAim", "zamerny", "aimpoint");
        if (_aimPoint < 0)
        {
            LOG_DEBUG(Physics, "No aim point in {}", (const char*)GetName());
        }
        _aimingAxisPoint = pointOr("memoryPointAimingAxis", "osa mireni", "aiming_axis");
        _headAxisPoint = pointOr("memoryPointHeadAxis", "osa otaceni", "head_axis");
        if (_headAxisPoint < 0)
        {
            _headAxisPoint = _aimingAxisPoint;
        }
    }

    int level;
    level = _shape->FindSpecLevel(VIEW_PILOT);
    if (level >= 0)
    {
        _insideView = level;
        Shape* shape = _shape->LevelOpaque(level);
        shape->MakeCockpit();
    }
    level = _shape->FindSpecLevel(VIEW_GUNNER);
    if (level >= 0)
    {
        _gunnerView = level;
        Shape* shape = _shape->LevelOpaque(level);
        shape->MakeCockpit();
    }

    RStringB moveName = par >> "Moves";
    const ParamEntry& moves = Pars >> moveName;
    MotionType::Load(moves);

    MovesTypeName name;
    name.shape = _shape;
    name.motionType = this;
    _moveType = MovesTypes.New(name);

    if (_isMan)
    {
        _gunMatIndex = _moveType->GetSkeleton()->FindBone(ManNameOr(par, "weaponBone", "zbran"));
        _rpgMatIndex = _moveType->GetSkeleton()->FindBone(ManNameOr(par, "boneLauncher", "roura"));
        _handMatIndex = _moveType->GetSkeleton()->FindBone(ManNameOr(par, "boneLeftHand", "lruka"));
        _rightHandMatIndex = _moveType->GetSkeleton()->FindBone(ManNameOr(par, "boneRightHand", "pruka"));
        _headMatIndex = _moveType->GetSkeleton()->FindBone(ManNameOr(par, "boneHead", "head"));
        if (_gunMatIndex < 0)
        {
            LOG_ERROR(Physics, "Bone 'zbran' not found in {}", (const char*)_shape->GetName());
        }
        if (_rpgMatIndex < 0)
        {
            LOG_ERROR(Physics, "Bone 'roura' not found in {}", (const char*)_shape->GetName());
        }
        if (_handMatIndex < 0)
        {
            LOG_ERROR(Physics, "Bone 'lruka' not found in {}", (const char*)_shape->GetName());
        }
        if (_rightHandMatIndex < 0)
        {
            LOG_ERROR(Physics, "Bone 'pruka' not found in {}", (const char*)_shape->GetName());
        }
    }
    else
    {
        _gunMatIndex = -1;
        _rpgMatIndex = -1;
        _handMatIndex = -1;
        _rightHandMatIndex = -1;
    }

    // Note: search proxies in memory level instead of MemoryLevel()
    Shape* proxyContainer = _shape->MemoryLevel();
    _priWeaponIndex = -1; // index of corresponding proxy
    _secWeaponIndex = -1;
    _handGunIndex = -1;
    if (proxyContainer)
    {
        for (int i = 0; i < proxyContainer->NProxies(); i++)
        {
            const ProxyObject& proxy = proxyContainer->Proxy(i);
            if (dyn_cast<ProxySecWeapon, Object>(proxy.obj))
            {
                _secWeaponIndex = i;
            }
            else if (dyn_cast<ProxyHandGun, Object>(proxy.obj))
            {
                _handGunIndex = i;
            }
            else if (dyn_cast<ProxyWeapon, Object>(proxy.obj))
            {
                _priWeaponIndex = i;
            }
        }
    }

    int headIndex = GetSkeleton()->FindBone(ManNameOr(par, "boneHead", "hlava"));
    if (headIndex >= 0)
    {
        _headOnlyWeight.Add(AnimationRTPair(headIndex, 1));
    }
    else
    {
        if (_isMan)
        {
            LOG_ERROR(Physics, "Bone 'hlava' not found in {}", (const char*)_shape->GetName());
        }
    }

    DEF_HIT(_shape, _headHit, ManNameOr(par, "hitNameHead", "hlava"), nullptr, GetArmor() * (float)(par >> "armorHead"));
    DEF_HIT(_shape, _bodyHit, ManNameOr(par, "hitNameBody", "telo"), nullptr, GetArmor() * (float)(par >> "armorBody"));
    DEF_HIT(_shape, _handsHit, ManNameOr(par, "hitNameHands", "ruce"), nullptr, GetArmor() * (float)(par >> "armorHands"));
    DEF_HIT(_shape, _legsHit, ManNameOr(par, "hitNameLegs", "nohy"), nullptr, GetArmor() * (float)(par >> "armorLegs"));
}

void ManType::DeinitShape()
{
    _headWound.Unload();
    _bodyWound.Unload();
    _lArmWound.Unload();
    _rArmWound.Unload();
    _lLegWound.Unload();
    _rLegWound.Unload();

    MotionType::Unload();
    _moveType = nullptr;
    // Note: compact MovesTypes bank
}

ActionMap* ManType::GetActionMap(MoveId move) const
{
    ActionMap* map = _moveType->GetActionMap(move);
    if (!map)
    {
        map = MotionType::GetNoActions();
    }
    return map;
}

} // namespace Poseidon
