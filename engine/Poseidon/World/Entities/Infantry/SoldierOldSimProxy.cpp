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

namespace Poseidon
{
using namespace Foundation;
using Foundation::EnumName;

bool Man::CastProxyShadow(int level, int index) const
{
    return true;
}

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
    const NamedSelection& sel = sShape->NamedSel(proxy.selection);
    int point = sel[0];
    PoseidonAssert(sel.Size() > 0);
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
    transform = transform * animTransform;

    // if shape is reversed, reverse orientation
    if (pshape->Remarks() & REM_REVERSED)
    {
        Matrix4 swapM(MScale, -1, +1, -1);
        transform = transform * swapM;
    }
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
        const NamedSelection& sel = sShape->NamedSel(proxy.selection);
        int point = sel[0];
        PoseidonAssert(sel.Size() > 0);
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

        Matrix4 pTransform = transform * animTransform;

        if (pshape->Remarks() & REM_REVERSED)
        {
            Matrix4 swapM(MScale, -1, +1, -1);
            pTransform = pTransform * swapM;
        }
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

    _mGunFireFrames--;
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
    if (GWorld->GetCameraType() == CamGunner && GWorld->CameraOn() == this)
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

ActionMap::ActionMap(const ActionMapName& name)
{
    _name = name;
    const ParamEntry& e = *_name.entry;

    _turnSpeed = e >> "turnSpeed";
    _upDegree = e >> "upDegree";
    _limitFast = e >> "limitFast";

#define ACTION(x) _actions[ManAct##x] = _name.motion->GetMoveId(e >> #x);
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
            _name.motionType->GetSkeleton()->Prepare(shape, GetWeights());
            anim->SetLooped((*entry) >> "looped");
        }
    }

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
        BlendAnimInfo& info = Set(Add());
        int index = skelet->FindBone(name);
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

    _cameraPoint = mem ? mem->PointIndex("zamerny") : -1;
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
    _headWound.Init(_shape, woundInfo, "head injury", nullptr);
    _bodyWound.Init(_shape, woundInfo, "body injury", nullptr);
    _lArmWound.Init(_shape, woundInfo, "l arm injury", nullptr);
    _rArmWound.Init(_shape, woundInfo, "r arm injury", "p arm injury");
    _lLegWound.Init(_shape, woundInfo, "l leg injury", nullptr);
    _rLegWound.Init(_shape, woundInfo, "r leg injury", "p leg injury");

    _head.InitShape(par, _shape);

    _headHide.Init(_shape, "hlava", nullptr);
    _neckHide.Init(_shape, "krk", nullptr);

    _lightPos = _shape->MemoryPoint("L svetlo");
    _lightDir = _shape->MemoryPoint("konec L svetla") - _lightPos;
    _lightDir.Normalize();

    _insideView = 0;
    _gunnerView = 0;

    _stepLIndex = mem ? mem->PointIndex("stopaL") : -1;
    _stepRIndex = mem ? mem->PointIndex("stopaP") : -1;

    _aimPoint = -1;
    _aimingAxisPoint = -1;
    _headAxisPoint = -1;
    if (mem)
    {
        _aimPoint = mem->PointIndex("zamerny");
        if (_aimPoint < 0)
        {
            LOG_DEBUG(Physics, "No aim point in {}", (const char*)GetName());
        }
        _aimingAxisPoint = mem->PointIndex("osa mireni");
        _headAxisPoint = mem->PointIndex("osa otaceni");
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
        _gunMatIndex = _moveType->GetSkeleton()->FindBone("zbran");
        _rpgMatIndex = _moveType->GetSkeleton()->FindBone("roura");
        _handMatIndex = _moveType->GetSkeleton()->FindBone("lruka");
        _rightHandMatIndex = _moveType->GetSkeleton()->FindBone("pruka");
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

    int headIndex = GetSkeleton()->FindBone("hlava");
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

    DEF_HIT(_shape, _headHit, "hlava", nullptr, GetArmor() * (float)(par >> "armorHead"));
    DEF_HIT(_shape, _bodyHit, "telo", nullptr, GetArmor() * (float)(par >> "armorBody"));
    DEF_HIT(_shape, _handsHit, "ruce", nullptr, GetArmor() * (float)(par >> "armorHands"));
    DEF_HIT(_shape, _legsHit, "nohy", nullptr, GetArmor() * (float)(par >> "armorLegs"));
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
