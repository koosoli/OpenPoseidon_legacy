#include <Poseidon/World/Scene/Camera/CamEffects.hpp>
#include <Poseidon/World/World.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/Foundation/Math/Interpol.hpp>
#include <Poseidon/Graphics/Rendering/Shape/Shape.hpp>

#include <Poseidon/IO/ParamFile/ParamFile.hpp>
#include <Poseidon/IO/Serialization/ParamArchive.hpp>

#include <Poseidon/Foundation/Enums/EnumNames.hpp>
#include <cmath>
#include <Poseidon/Foundation/Common/FltOpts.hpp>
#include <Poseidon/Foundation/Framework/AppFrame.hpp>
#include <Poseidon/Foundation/Framework/DebugLog.hpp>
#include <Poseidon/Foundation/Math/Math3D.hpp>
#include <Poseidon/Foundation/Math/Math3DP.hpp>
#include <Poseidon/Foundation/Strings/RString.hpp>
#include <Poseidon/Foundation/Types/LLinks.hpp>
#include <Poseidon/Foundation/Types/Pointers.hpp>

namespace Poseidon
{
RString FindShape(RString name);
}

namespace Poseidon
{
using Poseidon::Foundation::EnumName;
const ParamEntry* FindCameraEffect(RString name);

template <>
const EnumName* Poseidon::Foundation::GetEnumNames(CamEffectPosition dummy)
{
    static const EnumName CamEffectPositionNames[] = {EnumName(CamEffectTop, "TOP"),
                                                      EnumName(CamEffectLeft, "LEFT"),
                                                      EnumName(CamEffectRight, "RIGHT"),
                                                      EnumName(CamEffectFront, "FRONT"),
                                                      EnumName(CamEffectBack, "BACK"),
                                                      EnumName(CamEffectLeftFront, "LEFT FRONT"),
                                                      EnumName(CamEffectRightFront, "RIGHT FRONT"),
                                                      EnumName(CamEffectLeftBack, "LEFT BACK"),
                                                      EnumName(CamEffectRightBack, "RIGHT BACK"),
                                                      EnumName(CamEffectLeftTop, "LEFT TOP"),
                                                      EnumName(CamEffectRightTop, "RIGHT TOP"),
                                                      EnumName(CamEffectFrontTop, "FRONT TOP"),
                                                      EnumName(CamEffectBackTop, "BACK TOP"),
                                                      EnumName(CamEffectBottom, "BOTTOM"),
                                                      EnumName()};
    return CamEffectPositionNames;
}

// basic positions

const float baseY = 0.1, topY = 1;

static const Vector3 BaseDir[NCamEffectPositions] = {
    Vector3(0, +1, -0.1).Normalized(),                                       // CamEffectTop,
    Vector3(+1, baseY, 0).Normalized(),  Vector3(-1, baseY, 0).Normalized(), // CamEffectLeft,CamEffectRight,
    Vector3(0, baseY, +1).Normalized(),  Vector3(0, baseY, -1).Normalized(), // CamEffectFront,CamEffectBack,

    Vector3(+1, baseY, +1).Normalized(), Vector3(-1, baseY, +1).Normalized(), // CamEffectLeftFront,CamEffectRightFront,
    Vector3(+1, baseY, -1).Normalized(), Vector3(-1, baseY, -1).Normalized(), // CamEffectLeftBack,CamEffectRightBack,

    Vector3(+1, topY, 0).Normalized(),   Vector3(-1, topY, 0).Normalized(), // CamEffectLeftTop,CamEffectRightTop,
    Vector3(0, topY, +1).Normalized(),   Vector3(0, topY, -1).Normalized(), // CamEffectFrontTop,CamEffectBackTop,

    Vector3(0, -1, +1).Normalized(), // CamEffectBottom,
};

const float DefaultTime = 10;

class CameraEffectTimed : public CameraEffect
{
    typedef CameraEffect base;

  protected:
    float _timeToLive;
    bool _infinite;

  public:
    CameraEffectTimed(Object* obj, float time, bool infinite);

    void Simulate(float deltaT) override;
    bool IsTerminated() const override { return !_infinite && _timeToLive <= 0; }

    LSError Serialize(ParamArchive& ar);
};

CameraEffectTimed::CameraEffectTimed(Object* obj, float time, bool infinite)
    : base(obj), _timeToLive(time), _infinite(infinite)
{
}

void CameraEffectTimed::Simulate(float deltaT)
{
    base::Simulate(deltaT);
    _timeToLive -= deltaT;
}

LSError CameraEffectTimed::Serialize(ParamArchive& ar)
{
    PARAM_CHECK(base::Serialize(ar))

    if (ar.IsSaving())
    {
        PARAM_CHECK(ar.Serialize("infinite", _infinite, 1, false))
    }
    PARAM_CHECK(ar.Serialize("timeToLive", _timeToLive, 1))
    return LSOK;
}

// static camera

class CameraEffectStatic : public CameraEffectTimed
{
    typedef CameraEffectTimed base;

  public:
    CameraEffectStatic(Object* object, CamEffectPosition pos, bool infinite);
    void Simulate(float deltaT) override;

    int GetType() const override;
};

class CameraEffectTracking : public CameraEffectStatic
{
    typedef CameraEffectStatic base;

  public:
    CameraEffectTracking(Object* object, CamEffectPosition pos, bool infinite);
    float GetFOV() const override;

    int GetType() const override;
};

CameraEffectTracking::CameraEffectTracking(Object* object, CamEffectPosition pos, bool infinite)
    : base(object, pos, infinite)
{
}

float CameraEffectTracking::GetFOV() const
{
    float fov = 0.5;
    if (_object)
    {
        float invDist = (_object->Position() - _transform.Position()).InvSize();
        fov = _object->OutsideCameraDistance(CamGroup) * invDist * 0.7;
    }
    saturate(fov, 0.1, 0.7);
    return fov;
}

CameraEffectStatic::CameraEffectStatic(Object* object, CamEffectPosition pos, bool infinite)
    : base(object, DefaultTime, infinite)
{
    PoseidonAssert(object);
    _transform.SetOrientation(M3Identity);
    Vector3 position = _object->PositionModelToWorld(BaseDir[pos] * _object->OutsideCameraDistance(CamGroup));
    Vector3 focus = _object->Position();
    float surfaceY = GLandscape->CameraFloorY(position, &focus, true); // terrainHole4: may be below ground in a hole
    saturateMax(position[1], surfaceY);
    _transform.SetPosition(position);
}

void CameraEffectStatic::Simulate(float deltaT)
{
    base::Simulate(deltaT);
    // update transform matrix
    // look at object
    if (_object)
    {
        Vector3 relPos = _object->Position() - _transform.Position();
        _transform.SetDirectionAndUp(relPos, VUp);
    }
}

// external (fixed) camera
class CameraEffectFixed : public CameraEffectTimed
{
    typedef CameraEffectTimed base;
    Vector3 _position, _direction;

  public:
    CameraEffectFixed(Object* object, Vector3Par pos, Vector3Par dir, bool infinite);

    void Simulate(float deltaT) override;

    LSError Serialize(ParamArchive& ar);
};

CameraEffectFixed::CameraEffectFixed(Object* object, Vector3Par pos, Vector3Par dir, bool infinite)
    : base(object, DefaultTime, infinite)
{
    PoseidonAssert(object);
    _position = pos;
    _direction = dir;
}

void CameraEffectFixed::Simulate(float deltaT)
{
    base::Simulate(deltaT);
    if (_object)
    {
        _transform.SetDirectionAndUp(_object->DirectionModelToWorld(_direction), VUp);
        _transform.SetPosition(_object->PositionModelToWorld(_position));
    }
}

LSError CameraEffectFixed::Serialize(ParamArchive& ar)
{
    PARAM_CHECK(base::Serialize(ar))

    PARAM_CHECK(ar.Serialize("position", _position, 1))
    PARAM_CHECK(ar.Serialize("direction", _direction, 1))
    return LSOK;
}

class CameraEffectExternal : public CameraEffectFixed
{
    typedef CameraEffectFixed base;

  public:
    CameraEffectExternal(Object* object, CamEffectPosition pos, bool infinite);
};

CameraEffectExternal::CameraEffectExternal(Object* object, CamEffectPosition pos, bool infinite)
    : base(object, BaseDir[pos] * object->OutsideCameraDistance(CamGroup), -BaseDir[pos], infinite)
{
}

class CameraEffectInternal : public CameraEffectTimed
{
    typedef CameraEffectTimed base;
    bool _autoTerminate{false};

  public:
    CameraEffectInternal(Object* object, CamEffectPosition pos, bool infinite);

    bool IsInside() const override { return true; }
    bool IsTerminated() const override;

    void Draw() const override;
    void Simulate(float deltaT) override;
    Matrix4 GetTransform() const override;

    int GetType() const override;
    LSError Serialize(ParamArchive& ar);
};

CameraEffectInternal::CameraEffectInternal(Object* object, CamEffectPosition pos, bool infinite)
    : base(object, 2.0, infinite)
{
}

Matrix4 CameraEffectInternal::GetTransform() const
{
    if (_object)
    {
        return _object->Transform();
    }
    else
    {
        return _transform;
    }
}

bool CameraEffectInternal::IsTerminated() const
{
    if (_autoTerminate)
    {
        // if object is destroyed, internal view is no longer possible
        if (!_object)
        {
            return true;
        }
    }
    return base::IsTerminated();
}

void CameraEffectInternal::Draw() const
{
    base::Draw();
    if (_object)
    {
        _object->DrawCameraCockpit();
    }
}

void CameraEffectInternal::Simulate(float deltaT)
{
    Object* obj = _object;
    if (obj)
    {
        _autoTerminate = obj->CameraAutoTerminate();
        _transform = obj->Transform();
    }
    base::Simulate(deltaT);
}

LSError CameraEffectInternal::Serialize(ParamArchive& ar)
{
    PARAM_CHECK(base::Serialize(ar))

    PARAM_CHECK(ar.Serialize("autoTerminate", _autoTerminate, 1, false))
    return LSOK;
}

class CameraEffectAligned : public CameraEffectFixed
{
    typedef CameraEffectFixed base;

  public:
    CameraEffectAligned(Object* object, CamEffectPosition pos, bool infinite);
};

CameraEffectAligned::CameraEffectAligned(Object* object, CamEffectPosition pos, bool infinite)
    : base(object, BaseDir[pos] * object->OutsideCameraDistance(CamGroup), VForward, infinite)
{
}

class CameraEffectInterpolated : public CameraEffectTimed
{
    typedef CameraEffectTimed base;

  protected:
    RString _name;

    SRef<M4Function> _functionC;
    SRef<M4Function> _functionT;

    float _time;
    Temp<Matrix4> _pos;
    Temp<Matrix4> _tgt;
    Temp<float> _times;
    bool _hardTrack;

  public:
    CameraEffectInterpolated(Object* obj, bool infinite, CamEffectPosition pos, RString name);
    void Simulate(float deltaT) override;

    void InitPositions(CamEffectPosition pos, const Vector3* path, const Vector3* tgt, const float* times, int size,
                       bool rescale);
    void Init(M4Function* functionC, M4Function* functionT);
    void InitLinear(CamEffectPosition pos, const Vector3* path, const Vector3* tgt, const float* times, int size,
                    bool rescale);
    void InitSpline(CamEffectPosition pos, const Vector3* path, const Vector3* tgt, const float* times, int size,
                    bool rescale);

    int GetType() const override;
    LSError Serialize(ParamArchive& ar);
};

LSError CameraEffectInterpolated::Serialize(ParamArchive& ar)
{
    PARAM_CHECK(base::Serialize(ar))

    if (ar.IsSaving())
    {
        PARAM_CHECK(ar.Serialize("name", _name, 1, ""))
    }
    PARAM_CHECK(ar.Serialize("name", _name, 1, ""))
    PARAM_CHECK(ar.Serialize("time", _time, 1, 0))
    PARAM_CHECK(ar.Serialize("hardTrack", _hardTrack, 1, true))

    return LSOK;
}

CameraEffectInterpolated::CameraEffectInterpolated(Object* obj, bool infinite, CamEffectPosition pos, RString name)
    : base(obj, 2.0, infinite)
{
    _name = name;
    const ParamEntry* pEntry = FindCameraEffect(name);
    DoAssert(pEntry);
    const ParamEntry& cfg = *pEntry;

    _hardTrack = true;

    _time = 0;
    // load model
    RString mName = cfg >> "file";
    if (mName.GetLength() == 0)
    {
        // empty animation - fixed camera
        float duration = cfg >> "duration";
        _timeToLive += duration;
        int size = 2;
        Temp<Vector3> camera(size);
        Temp<Vector3> target(size);
        Temp<float> time(size);
        for (int i = 0; i < size; i++)
        {
            time[i] = i * duration;
            camera[i] = VZero;
            target[i] = VForward;
        }
        InitLinear(pos, camera, target, time, size, false);
        return;
    }
    mName = Poseidon::FindShape(mName);
    Ref<LODShape> lShape = new LODShape(mName, false);
    Shape* shape = lShape->Level(0);
    PoseidonAssert(shape);
    int camIndex = shape->PointIndex("camera");
    if (camIndex < 0)
    {
        ErrorMessage("No camera in %s", (const char*)mName);
        return;
    }
    int tgtIndex = shape->PointIndex("target");
    if (tgtIndex < 0)
    {
        ErrorMessage("No target in %s", (const char*)mName);
        return;
    }
    int size = shape->NAnimationPhases();
    if (size < 2)
    {
        ErrorMessage("No animation in %s", (const char*)mName);
        return;
    }
    float duration = cfg >> "duration";
    _timeToLive += duration;
    float scale = cfg >> "scale";
    Temp<Vector3> camera(size);
    Temp<Vector3> target(size);
    Temp<float> time(size);
    Vector3Val bCenter = scale < 0 ? lShape->BoundingCenter() : VZero;
    for (int i = 0; i < size; i++)
    {
        const AnimationPhase& phase = shape->GetAnimationPhase(i);
        time[i] = phase.Time() * duration;
        camera[i] = (phase[camIndex] + bCenter) * fabs(scale);
        target[i] = (phase[tgtIndex] + bCenter) * fabs(scale);
    }
    bool spline = (cfg >> "spline").GetInt() > 0;
    if (spline && size > 2)
    {
        InitSpline(pos, camera, target, time, size, scale > 0);
    }
    else
    {
        InitLinear(pos, camera, target, time, size, scale > 0);
    }
}

void CameraEffectInterpolated::Init(M4Function* functionC, M4Function* functionT)
{
    _functionC = functionC;
    _functionT = functionT;
    _timeToLive = _functionC->MaxArg();
}

void CameraEffectInterpolated::Simulate(float deltaT)
{
    base::Simulate(deltaT);
    Matrix4Val camTransform = (*_functionC)(_time);
    Matrix4Val tgtTransform = (*_functionT)(_time);
    if (_object)
    {
        if (_hardTrack)
        {
            LODShape* shape = _object->GetShape();
            Vector3Val obc = shape ? shape->BoundingCenter() : VZero;
            Vector3 cPosition = _object->Transform() * (camTransform.Position() - obc);
            Vector3 tPosition = _object->Transform() * (tgtTransform.Position() - obc);

            Vector3 cFocus = _object->Position();
            float cSurfaceY = GLandscape->CameraFloorY(cPosition, &cFocus, true); // terrainHole4
            saturateMax(cPosition[1], cSurfaceY);
            _transform.SetDirectionAndUp(tPosition - cPosition, VUp);
            _transform.SetPosition(cPosition);
        }
        else
        {
            _transform = _object->Transform() * camTransform;
            _transform.Orthogonalize();
        }
    }
    _time += deltaT;
}

void CameraEffectInterpolated::InitPositions(CamEffectPosition pos, const Vector3* path, const Vector3* tgt,
                                             const float* times, int size, bool rescale)
{
    Matrix3 rotate;
    if (rescale)
    {
        rotate.SetDirectionAndUp(-BaseDir[pos], VUp);
    }
    else
    {
        rotate.SetIdentity();
    }
    _pos.Realloc(size);
    _tgt.Realloc(size);
    _times.Realloc(size);
    float scale = rescale ? _object->OutsideCameraDistance(CamGroup) * 0.5 : 1;
    for (int i = 0; i < size; i++)
    {
        _times[i] = times[i];
        Vector3 cPos = rotate * path[i] * scale;
        Vector3 tPos = VZero;
        if (tgt)
        {
            tPos = rotate * tgt[i] * scale;
        }
        Matrix3 orient;
        orient.SetDirectionAndUp(tPos - cPos, VUp);
        _pos[i].SetPosition(cPos);
        _pos[i].SetOrientation(orient);
        _tgt[i].SetPosition(tPos);
        _tgt[i].SetOrientation(orient * -1);
    }
}

void CameraEffectInterpolated::InitLinear(CamEffectPosition pos, const Vector3* path, const Vector3* tgt,
                                          const float* times, int size, bool rescale)
{
    InitPositions(pos, path, tgt, times, size, rescale);
    Init(new InterpolatorLinear(_pos, _times, size), new InterpolatorLinear(_tgt, _times, size));
}
void CameraEffectInterpolated::InitSpline(CamEffectPosition pos, const Vector3* path, const Vector3* tgt,
                                          const float* times, int size, bool rescale)
{
    InitPositions(pos, path, tgt, times, size, rescale);
    Init(new InterpolatorSpline(_pos, _times, size), new InterpolatorSpline(_tgt, _times, size));
}

class CameraEffectChained : public CameraEffect
{
    typedef CameraEffect base;

  protected:
    RString _name;
    const ParamEntry& _cfg;
    CamEffectPosition _cPos;
    Ref<CameraEffect> _act;
    int _index;
    bool _infinite;

  public:
    CameraEffectChained(Object* obj, bool infinite, CamEffectPosition pos, RString name, const ParamEntry& cfg);
    void Advance();
    void Switch(int index);
    void Simulate(float deltaT) override;
    bool IsTerminated() const override;

    Matrix4 GetTransform() const override
    {
        if (!_act)
        {
            return MIdentity;
        }
        return _act->GetTransform();
    } // get camera position

    int GetType() const override;
    LSError Serialize(ParamArchive& ar);
};

LSError CameraEffectChained::Serialize(ParamArchive& ar)
{
    PARAM_CHECK(base::Serialize(ar))

    if (ar.IsSaving())
    {
        PARAM_CHECK(ar.Serialize("name", _name, 1, ""))
    }
    PARAM_CHECK(ar.SerializeEnum("cPos", _cPos, 1, (CamEffectPosition)CamEffectTop))
    PARAM_CHECK(ar.Serialize("index", _index, 1, 0))
    PARAM_CHECK(ar.Serialize("inf", _infinite, 1, false))
    PARAM_CHECK(ar.Serialize("act", _act, 1))
    return LSOK;
}

CameraEffectChained::CameraEffectChained(Object* obj, bool infinite, CamEffectPosition pos, RString name,
                                         const ParamEntry& cfg)
    : CameraEffect(obj), _name(name), _cPos(pos), _infinite(infinite), _cfg(cfg)
{
    Switch(0);
}

void CameraEffectChained::Switch(int i)
{
    _index = i;
    const ParamEntry& chain = _cfg >> "chain";
    RString cName = chain[i];
    _act = CreateCameraEffect(GetObject(), cName, _cPos, false);
    if (_act)
    {
        _act->Simulate(0);
    }
}

void CameraEffectChained::Advance()
{
    const ParamEntry& chain = _cfg >> "chain";
    int newI = _index + 1;
    if (newI < chain.GetSize())
    {
        Switch(newI);
    }
}

void CameraEffectChained::Simulate(float deltaT)
{
    if (!_act)
    {
        return;
    }
    _act->Simulate(deltaT);
    if (_act->IsTerminated())
    {
        Advance();
    }
}

bool CameraEffectChained::IsTerminated() const
{
    if (!_act)
    {
        return true;
    }
    return !_infinite && _act->IsTerminated();
}

#define CamExternal 0       // linked directly with object
#define CamStatic 1         // fixed point in space
#define CamStaticWithZoom 2 // fixed point in space
#define CamChained 3        // chained
#define CamTerminate 4      // terminate
#define CamInternal 5       // internal view

int CameraEffectStatic::GetType() const
{
    return CamStatic;
}
int CameraEffectTracking::GetType() const
{
    return CamStaticWithZoom;
}
int CameraEffectInterpolated::GetType() const
{
    return CamExternal;
}
int CameraEffectChained::GetType() const
{
    return CamChained;
}
int CameraEffectInternal::GetType() const
{
    return CamInternal;
}

CameraEffect* CreateCameraEffect(Object* obj, const char* name, CamEffectPosition pos, bool infinite)
{
    const ParamEntry* pEntry = FindCameraEffect(name);
    DoAssert(pEntry);

    const ParamEntry& entry = *pEntry;
    int type = (entry >> "type").GetInt();
    CameraEffect* result = nullptr;
    switch (type)
    {
        case CamStatic:
            result = new CameraEffectStatic(obj, pos, infinite);
            break;
        case CamStaticWithZoom:
            result = new CameraEffectTracking(obj, pos, infinite);
            break;
        case CamExternal:
            result = new CameraEffectInterpolated(obj, infinite, pos, name);
            break;
        case CamChained:
            result = new CameraEffectChained(obj, infinite, pos, name, entry);
            break;
        case CamInternal:
            result = new CameraEffectInternal(obj, pos, infinite);
            break;
        case CamTerminate:
            result = nullptr;
            break;
        default:
            Fail("Unknown camera effect");
            break;
    }
    if (result)
    {
        result->Simulate(0);
    }
    return result;
}

CameraEffect* CameraEffect::CreateObject(ParamArchive& ar)
{
    int type = CamExternal;
    if (ar.Serialize("type", type, 1, CamExternal) != LSOK)
    {
        return nullptr;
    }
    Ref<Object> obj;
    if (ar.SerializeRef("obj", obj, 1) != LSOK)
    {
        return nullptr;
    }
    CamEffectPosition pos = CamEffectTop;
    if (ar.SerializeEnum("pos", pos, 1, (CamEffectPosition)CamEffectTop) != LSOK)
    {
        return nullptr;
    }
    bool infinite = false;
    if (ar.Serialize("infinite", infinite, 1, false) != LSOK)
    {
        return nullptr;
    }

    switch (type)
    {
        case CamStatic:
            return new CameraEffectStatic(obj, pos, infinite);
        case CamStaticWithZoom:
            return new CameraEffectTracking(obj, pos, infinite);
        case CamExternal:
        {
            RString name;
            if (ar.Serialize("name", name, 1, "") != LSOK)
            {
                return nullptr;
            }
            return new CameraEffectInterpolated(obj, infinite, pos, name);
        }
        case CamChained:
        {
            RString name;
            if (ar.Serialize("name", name, 1, "") != LSOK)
            {
                return nullptr;
            }
            const ParamEntry* pEntry = FindCameraEffect(name);
            DoAssert(pEntry);
            return new CameraEffectChained(obj, infinite, pos, name, *pEntry);
        }
        case CamInternal:
            return new CameraEffectInternal(obj, pos, infinite);
    }
    Fail("Unknown camera effect");
    return nullptr;
}

LSError CameraEffect::Serialize(ParamArchive& ar)
{
    if (ar.IsSaving())
    {
        int type = GetType();
        PARAM_CHECK(ar.Serialize("type", type, 1, CamExternal))
        PARAM_CHECK(ar.SerializeRef("obj", _object, 1))
    }
    PARAM_CHECK(ar.Serialize("transform", _transform, 1))
    return LSOK;
}

CameraEffect* CreateCameraEffect(Object* obj, const char* name, CamEffectPosition pos)
{
    bool infinite = GWorld->GetMode() == GModeIntro;
    return CreateCameraEffect(obj, name, pos, infinite);
}
} // namespace Poseidon