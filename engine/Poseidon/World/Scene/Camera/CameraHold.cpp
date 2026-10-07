
#include <Poseidon/World/Scene/Camera/CameraHold.hpp>
#include <Poseidon/IO/ParamFile/ParamFile.hpp>
#include <Poseidon/IO/Serialization/ParamArchive.hpp>
#include <Poseidon/Core/Global.hpp>
#include <stdio.h>
#include <string.h>
#include <cmath>
#include <Poseidon/Foundation/Common/FltOpts.hpp>
#include <Poseidon/Foundation/Math/MathDefs.hpp>
#include <Poseidon/Foundation/Types/Pointers.hpp>
#include <Poseidon/Foundation/platform.hpp>

#include <Poseidon/Input/InputSubsystem.hpp>
#include <Poseidon/World/Scene/Camera/Camera.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/World/Terrain/EarthStreamingMode.hpp>
#include <Poseidon/Graphics/Core/Engine.hpp>
#include <Poseidon/World/World.hpp>
#include <Poseidon/World/Scene/Scene.hpp>
#include <SDL3/SDL_scancode.h>
#include <Poseidon/Graphics/Textures/TexturePreload.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <sys/types.h>
#include <sys/timeb.h>
#include <time.h>

namespace Poseidon
{
using namespace Foundation;

DEFINE_CASTING(CameraHolder)

CameraHolder::CameraHolder(LODShapeWithShadow* shape, const VehicleNonAIType* type, int id)
    : base(shape, type, id), _manual(false), _camPos(VZero), _camFov(0.7), _camFovMin(1.5), _camFovMax(0.07),
      _camDive(0), _camBank(0), _camHeading(0)
{
}
CameraHolder::~CameraHolder() = default;

void CameraHolder::Command(RString mode)
{
    // camera dependend special command
    if (!strcmpi(mode, "manual on"))
    {
        SetManual(true);
    }
    else if (!strcmpi(mode, "manual off"))
    {
        SetManual(false);
    }
}

Vector3 CameraTarget::GetPos() const
{
    if (_target)
    {
        return _target->CameraPosition();
    }
    else if (_pos.SquareSize() > 0.5)
    {
        return _pos;
    }
    else
    {
        return VZero;
    }
}

Vector3 CameraTarget::PositionAbsToRel(Vector3Val pos) const
{
    if (!_target)
    {
        return pos - _pos;
    }
    Matrix4 toAbs = _target->ModelToWorld();
    toAbs.SetDirectionAndUp(toAbs.Direction(), VUp);
    // make direction up pointing up
    return toAbs.InverseScaled().FastTransform(pos);
}

Vector3 CameraTarget::PositionRelToAbs(Vector3Val pos) const
{
    if (!_target)
    {
        return pos + _pos;
    }

    Matrix4 toAbs = _target->ModelToWorld();
    toAbs.SetDirectionAndUp(toAbs.Direction(), VUp);
    // make direction up pointing up
    return toAbs.FastTransform(pos);
}

LSError CameraTarget::Serialize(ParamArchive& ar)
{
    PARAM_CHECK(ar.SerializeRef("target", _target, 1))
    PARAM_CHECK(ar.Serialize("pos", _pos, 1, VZero))
    return LSOK;
}

LSError CameraHolder::Serialize(ParamArchive& ar)
{
    PARAM_CHECK(base::Serialize(ar))
    PARAM_CHECK(_camTgt.Serialize(ar))
    PARAM_CHECK(ar.Serialize("camPos", _camPos, 1, VZero))
    PARAM_CHECK(ar.Serialize("camFov", _camFov, 1, 0))
    PARAM_CHECK(ar.Serialize("camFovMin", _camFovMin, 1, 0))
    PARAM_CHECK(ar.Serialize("camFovMax", _camFovMax, 1, 0))
    PARAM_CHECK(ar.Serialize("camDive", _camDive, 1, 0))
    PARAM_CHECK(ar.Serialize("camBank", _camBank, 1, 0))
    PARAM_CHECK(ar.Serialize("camHeading", _camHeading, 1, 0))
    PARAM_CHECK(ar.Serialize("manual", _manual, 1, false))
    return LSOK;
}

CameraVehicle::CameraVehicle()
    : base(nullptr, VehicleTypes.New("Camera"), -1), _inertia(false), // manual camera simulation
      _crossHairs(true), _altitudeSpeedScaling(false), _reportedWorldBounds(false), _mouseLookRequiresRightButton(false)
{
    // set all target properties to invalid values
    _movePos = VZero;
    _minFovT = _maxFovT = -1;
    // set all times to infinite future
    _movePosTime = TIME_MAX;
    _minMaxFovTime = TIME_MAX;
    _targetTime = TIME_MAX;
    _oldTargetTime = TIME_MAX;

    // all commited
    _timeCommitted = TIME_MIN;
    // set all actual properties to default values
    _minFov = 0.7, _maxFov = 0.7;
    _lastFov = 0.7;
    _lastTgtPos = VZero;
}

CameraVehicle::~CameraVehicle() = default;

// camera effect parameters
void CameraVehicle::DrawCameraCockpit()
{
    if (!_manual)
    {
        return; // nothing to draw
    }
    if (!_crossHairs)
    {
        return;
    }

    // check if we are drawing from inside
    // assume yes - we are manual

    {
        // draw circle around locked target
        Color lockColor(1, 1, 0, 1);
        Vector3 lockPos = _target._pos;
        if (_target._target)
        {
            lockColor = Color(1, 0, 0, 1);
            lockPos = _target._target->CameraPosition();
        }
        float ls2 = lockPos.SquareSize();
        if (ls2 > 0.5f && ls2 < 1e9f)
        {
            PackedColor color = PackedColor(lockColor);
            float w = GEngine->Width();
            float h = GEngine->Height();

            const Camera& camera = *GScene->GetCamera();
            Matrix4Val camInvTransform = camera.GetInvTransform();

            Point3 pos = camInvTransform.FastTransform(lockPos);
            if (pos.Z() >= 0)
            {
                float invZ = 1.0f / pos.Z();
                float cx = pos.X() * invZ * camera.InvLeft();
                float cy = -pos.Y() * invZ * camera.InvTop();

                float wScreen = toInt(w * 0.05f);
                float hScreen = toInt(h * (0.05f * 4 / 3));
                float xScreen = w * cx * 0.5f + w * 0.5f - wScreen * 0.5f;
                float yScreen = h * cy * 0.5f + h * 0.5f - hScreen * 0.5f;
                Texture* texture = GScene->Preloaded(CursorTarget);
                MipInfo mip = GLOB_ENGINE->TextBank()->UseMipmap(texture, 0, 0);
                GLOB_ENGINE->Draw2D(mip, PackedBlack, Rect2DAbs(xScreen + 1, yScreen + 1, wScreen, hScreen));
                GLOB_ENGINE->Draw2D(mip, color, Rect2DAbs(xScreen, yScreen, wScreen, hScreen));
            }
        }
    }
    {
        // draw 2D cursor in screen center
        float w = GEngine->Width();
        float h = GEngine->Height();

        float wScreen = toInt(w * 0.05f);
        float hScreen = toInt(h * (0.05f * 4 / 3));
        float xScreen = toInt(w * 0.5f) - wScreen * 0.5f;
        float yScreen = toInt(h * 0.5f) - hScreen * 0.5f;
        Texture* texture = GScene->Preloaded(CursorAim);
        MipInfo mip = GLOB_ENGINE->TextBank()->UseMipmap(texture, 0, 0);
        PackedColor color = PackedWhite;
        GLOB_ENGINE->Draw2D(mip, PackedBlack, Rect2DAbs(xScreen + 1, yScreen + 1, wScreen, hScreen));
        GLOB_ENGINE->Draw2D(mip, color, Rect2DAbs(xScreen, yScreen, wScreen, hScreen));
    }
}

void CameraVehicle::StartFrame() {}

Vector3 CameraVehicle::GetTarget() const
{
    if (GetManual())
    {
        return _target.GetPos();
    }
    Vector3 nTgt = _target.GetPos();
    Vector3 oTgt = _oldTarget.GetPos();
    if (oTgt.SquareSize() < 0.5)
    {
        return nTgt;
    }
    if (nTgt.SquareSize() < 0.5)
    {
        return oTgt;
    }
    // both targets - interpolate
    if (Glob.time >= _targetTime)
    {
        return nTgt;
    }
    Vector3 oRel = oTgt - Position();
    Vector3 nRel = nTgt - Position();
    Vector3 oDir = oRel.Normalized();
    Vector3 nDir = nRel.Normalized();
    float oDist = oDir * oRel;
    float nDist = nDir * nRel;

    float ipol = 1;
    if (_targetTime > _oldTargetTime)
    {
        ipol = (Glob.time - _oldTargetTime) / (_targetTime - _oldTargetTime);
    }
    saturate(ipol, 0, 1);
    // total time
    // interpolate distance and direction separately
    Vector3 iDir = nDir * ipol + oDir * (1 - ipol);
    float iDist = nDist * ipol + oDist * (1 - ipol);
    return iDir * iDist + Position();
}

float CameraVehicle::CamEffectFOV() const
{
    // transition between old and new target

    Vector3 tPos = GetTarget();

    if (tPos.SquareSize() >= 0.5)
    {
        _lastTgtPos = tPos;
    }

    // calculate FOV based on target
    if (_lastTgtPos.SquareSize() > 0.5)
    {
        Vector3 norm = _lastTgtPos - Position();
        Vector3 dir = norm.Normalized();

        const_cast<CameraVehicle*>(this)->SetDirectionAndUp(dir, VUp);

        float invDist = (_lastTgtPos - Position()).InvSize();
        float fov = invDist * 10;
        saturate(fov, _minFov, _maxFov);
        _lastFov = fov;
    }
    return _lastFov;
}

void ClipboardSaveText(QOStream& stream);

void CameraVehicle::ResetTargets()
{
    Vector3 tPos = Position() + Direction() * 1e5; // virtual infinity
    _target = tPos;
    _oldTarget = nullptr;
    _lastTgtPos = _target._pos;
}

void CameraVehicle::Simulate(float deltaT, SimulationImportance prec)
{
    if (!GetManual())
    {
        if (_movePos.SquareSize() > 0.5)
        {
            if (Glob.time >= _movePosTime)
            {
                Move(_movePos);
                _speed = VZero;
                // TW-WATER W5c: arrived. Forget the target so a later setPos / setPosASL on the
                // camera is not snapped back to it every frame until the next camCommit.
                _movePos = VZero;
            }
            else
            {
                // smooth interpolation
                // control speed so that you are in position in time
                Vector3 offset = _movePos - Position();
                _speed = offset * (1 / (_movePosTime - Glob.time));

                Move(Position() + _speed * deltaT);
            }
        }
        if (_minFovT >= 0)
        {
            // interpolate minFov
            if (Glob.time < _minMaxFovTime)
            {
                float invTime = deltaT / (_minMaxFovTime - Glob.time);
                _minFov += (_minFovT - _minFov) * invTime;
                _maxFov += (_maxFovT - _maxFov) * invTime;
            }
            else
            {
                _minFov = _minFovT;
                _maxFov = _maxFovT;
            }
        }
    }
    else
    {
        if (_target.GetPos().SquareSize() < 0.5)
        {
            ResetTargets();
        }

        auto& input = InputSubsystem::Instance();
        // Manual cameras run in real time so they remain usable during time
        // acceleration. A paused or nearly-paused simulation must not turn
        // this conversion into an unbounded movement step.
        const float acceleratedTime = GWorld->GetAcceleratedTime();
        if (acceleratedTime > 0.01f)
        {
            deltaT /= acceleratedTime;
        }
        // manual controls
        // use basic controls to control movement

        float forward = (input.GetMoveForward() * 4 + input.GetMoveFastForward() * 16 + input.GetMoveBack() * -4);

        float aside =
            ((input.GetMoveRight() - input.GetMoveLeft()) * 16 + (input.GetTurnRight() - input.GetTurnLeft()) * 4);
        float up = (input.GetMoveUp() - input.GetMoveDown()) * 4;

        const float ZoomSpeed = 4;

        float expChange = input.GetKeyValue(SDL_SCANCODE_KP_MINUS) - input.GetKeyValue(SDL_SCANCODE_KP_PLUS);
        float change = pow(ZoomSpeed, expChange * deltaT);
        _lastFov *= change;
        saturate(_lastFov, 0.01, 2.0);
        _minFov = _maxFov = _lastFov; // force manual FOV

        // turn to follow camera direction
        // (i.e follow weapon)

        Matrix3 speedOrient;
        speedOrient.SetUpAndDirection(VUp, Direction());

        float speedScale = 1.0f;
        if (_altitudeSpeedScaling)
        {
            // The curve steepens with altitude: precise placement close to the
            // ground, then rapidly increasing travel speed high above the map.
            float groundY = GLandscape->SurfaceYAboveWater(Position()[0], Position()[2]);
            float altitude = fmax(Position()[1] - groundY, 0.0f);
            speedScale = fmin(1.0f + altitude * 0.03f + altitude * altitude * 0.00002f, 64.0f);

            // Shift is an absolute two-times multiplier for Zeus flight: it
            // accelerates sideways, vertical, and forward movement equally.
            if (input.IsKeyDown(SDL_SCANCODE_LSHIFT) || input.IsKeyDown(SDL_SCANCODE_RSHIFT))
            {
                speedScale *= 2.0f;
            }
        }

        Vector3 speedWanted = speedOrient * Vector3(aside, up, forward) * speedScale;
        if (_inertia)
        {
            // smooth changes
            speedWanted *= 4;

            Vector3 accel = speedWanted - _speed;
            saturate(accel[0], -5, +5);
            saturate(accel[1], -5, +5);
            saturate(accel[2], -10, +10);
            _speed += accel * deltaT;
        }
        else
        {
            _speed = speedWanted;
        }

        Vector3 position = Position() + _speed * deltaT;
        const bool earth = EarthStreaming::Enabled();
        if (_altitudeSpeedScaling)
        {
            // Zeus is a map-level camera. Its altitude speed boost makes it
            // easy to cross the landscape in seconds, but terrain sampling
            // only has valid coordinates inside the loaded world. Keep the
            // opt-in Zeus camera inside that domain instead of passing an
            // invalid frustum to Landscape::Draw.
            const float landSize = GLandscape->GetLandRange() * GLandscape->GetLandGrid();
            bool constrained = false;
            if (!position.IsFinite())
            {
                position = Vector3(landSize * 0.5f, 50.0f, landSize * 0.5f);
                _speed = VZero;
                constrained = true;
            }
            else
            {
                const float oldX = position[0];
                const float oldZ = position[2];
                const float lower = earth ? -EarthStreaming::CoordinateLimit : 0.0f;
                const float upper = earth ? EarthStreaming::CoordinateLimit : landSize;
                saturate(position[0], lower, upper);
                saturate(position[2], lower, upper);
                if (position[0] != oldX)
                {
                    _speed[0] = 0.0f;
                    constrained = true;
                }
                if (position[2] != oldZ)
                {
                    _speed[2] = 0.0f;
                    constrained = true;
                }
            }

            if (constrained && !_reportedWorldBounds)
            {
                LOG_WARN(Input, "Zeus camera reached the terrain boundary; movement was constrained");
                _reportedWorldBounds = true;
            }
            else if (!constrained)
            {
                _reportedWorldBounds = false;
            }
        }
        // TW-WATER W5c: the free camera flies under the sea too (cut-scene authoring); it is held
        // above the ground / sea bed only. The clipboard export below keeps heights relative to
        // the sea surface, so an underwater shot exports as a negative camSetPos height.
        // Sinkhole W1 (Malprave terrainHole4): inside a terrain hole it may go down to the hole's floor.
        Vector3 floorPos = position;
        float surfY = earth ? -12000.0f : GLandscape->CameraFloorY(floorPos, nullptr, false);
        saturateMax(position[1], surfY + 0.05);
        Move(position);

        // get mouse movement
        // or numeric keyboard

        float headSpeed = input.GetKeyValue(SDL_SCANCODE_KP_4) - input.GetKeyValue(SDL_SCANCODE_KP_6);
        float diveSpeed = input.GetKeyValue(SDL_SCANCODE_KP_2) - input.GetKeyValue(SDL_SCANCODE_KP_8);
        // Manual cameras are used by the debug Zeus mode as well as scripted
        // cut-scenes.  Match the viewer's mouse-look scale so a free camera
        // can be flown naturally without relying on the numeric keypad.
        constexpr float mouseLookScale = 0.005f;
        // Zeus/free-fly uses inverted mouse axes: move left to look right,
        // and move down to look up. Keyboard controls retain their normal
        // directions.
        const bool mouseLookActive = !_mouseLookRequiresRightButton || input.IsMouseRightDown();
        const float mouseX = mouseLookActive ? input.GetMouseDeltaX() : 0.0f;
        const float mouseY = mouseLookActive ? input.GetMouseDeltaY() : 0.0f;
        float headChange = headSpeed * 2 * deltaT * _lastFov - mouseX * mouseLookScale;
        float diveChange = diveSpeed * 2 * deltaT * _lastFov + mouseY * mouseLookScale;
        Matrix3 orient = Orientation();
        if (headChange)
        {
            Matrix3 rotY(MRotationY, headChange);
            orient = rotY * orient;
        }
        if (diveChange)
        {
            Matrix3 rotX(MRotationX, diveChange);
            // neutralize heading
            Matrix3 head, invHead;
            head.SetDirectionAndUp(Direction(), VUp);
            invHead = head.InverseRotation();
            orient = head * rotX * invHead * orient;
            ResetTargets();
        }
        SetOrientation(orient);

        if (headChange || diveChange)
        {
            ResetTargets();
        }

        if (input.IsKeyDown(SDL_SCANCODE_KP_5))
        {
            // unlock camera
            ResetTargets();
        } // fire
        if (input.GetActionToDo(UAToggleWeapons) || input.IsKeyPressed(SDL_SCANCODE_KP_DIVIDE))
        {
            // lock camera
            // shoot probe ray
            // if nothing is hit, fix at ground point
            // try to find some lock
            CollisionBuffer retVal;
            GLandscape->ObjectCollision(retVal, this, nullptr, Position(), Position() + Direction() * 500, 2,
                                        ObjIntersectGeom);
            // find nearest intersection
            Object* minObj = nullptr;
            float minDist2 = 1e10;
            for (int i = 0; i < retVal.Size(); i++)
            {
                Object* obj = retVal[0].object;
                if (!dyn_cast<Vehicle>(obj))
                {
                    continue;
                }
                float dist2 = obj->Position().Distance2(Position());
                if (dist2 < minDist2)
                {
                    minDist2 = dist2;
                    minObj = obj;
                }
            }
            if (minObj)
            {
                _target = minObj;
            }
            else
            {
                Vector3 land = GLandscape->IntersectWithGroundOrSea(Position(), Direction());
                _target = land;
            }
        } // selectweapon
        if (input.IsKeyPressed(SDL_SCANCODE_DELETE))
        {
            _inertia = !_inertia;
        }
        if (input.GetActionToDo(UAHeadlights))
        {
            _crossHairs = !_crossHairs;
        }
        if (!_mouseLookRequiresRightButton && input.IsKeyPressed(SDL_SCANCODE_V))
        {
            SetDelete(); // vehicle should be removed
            input.ConsumeKeyPress(SDL_SCANCODE_V);
        }
        if (!_mouseLookRequiresRightButton && input.GetActionToDo(UAFire))
        {
            LOG_INFO(Input, "Camera: clipboard save triggered");
            // save text (preferably to clipboard)
            char buf[1024];
            QOFStream f;
            const char* myName = "_camera";

            {
                time_t tb;
                time(&tb);
                struct tm* lt = localtime(&tb);

                snprintf(buf, sizeof(buf), ";=== %d:%02d:%02d\r\n", lt->tm_hour, lt->tm_min, lt->tm_sec);
                f.write(buf, strlen(buf));
            }

            if (_target._target)
            {
                RString aiName = _target._target->GetVarName();
                if (aiName.GetLength() > 0)
                {
                    snprintf(buf, sizeof(buf), "%s camSetTarget %s\r\n", myName, (const char*)aiName);
                }
                else
                {
                    RString symName = _target._target->GetDebugName();
                    snprintf(buf, sizeof(buf), "%s camSetTarget '%s'\r\n", myName, (const char*)symName);
                }
                f.write(buf, strlen(buf));
            }
            else
            {
                if (_target._pos.SquareSize() > 0.5)
                {
                    float surfY = GLandscape->SurfaceYAboveWater(_target._pos[0], _target._pos[2]);
                    snprintf(buf, sizeof(buf), "%s camSetTarget [%.2f,%.2f,%.2f]\r\n", myName, _target._pos[0],
                             _target._pos[2], _target._pos[1] - surfY);
                    f.write(buf, strlen(buf));
                }
            }
            if (_target._target && input.IsKeyDown(SDL_SCANCODE_LSHIFT))
            {
                Vector3 relPos = _target.PositionAbsToRel(Position());
                snprintf(buf, sizeof(buf), "%s camSetRelPos [%.2f,%.2f,%.2f]\r\n", myName, relPos[0], relPos[2],
                         relPos[1]);
                f.write(buf, strlen(buf));
            }
            else
            {
                float surfY = GLandscape->SurfaceYAboveWater(Position()[0], Position()[2]);
                snprintf(buf, sizeof(buf), "%s camSetPos [%.2f,%.2f,%.2f]\r\n", myName, Position()[0], Position()[2],
                         Position()[1] - surfY);
                f.write(buf, strlen(buf));
            }
            {
                snprintf(buf, sizeof(buf), "%s camSetFOV %.3f\r\n", myName, _lastFov);
                f.write(buf, strlen(buf));
            }

            {
                snprintf(buf, sizeof(buf), "%s camCommit 0\r\n", myName);
                f.write(buf, strlen(buf));
            }

            {
                snprintf(buf, sizeof(buf), "@camCommitted _camera\r\n");
                f.write(buf, strlen(buf));
            }

            LOG_DEBUG(Input, "Camera: clipboard buffer size={}", f.pcount());
            f.export_clip("clipboard.txt");
            LOG_DEBUG(Input, "Camera: export_clip done");
        } // fire - save parameters
    }
}

void CameraVehicle::Command(RString mode)
{
    // camera dependend special command
    if (!strcmpi(mode, "inertia on"))
    {
        _inertia = true;
    }
    else if (!strcmpi(mode, "inertia off"))
    {
        _inertia = false;
    }
    else
    {
        base::Command(mode);
    }
}

void CameraVehicle::Commit(float time)
{
    Time cTime = Glob.time + time;
    // commit all settings
    if (_set._camPos)
    {
        _movePos = _camPos;
        _movePosTime = cTime;
        _set._camPos = false;
    }
    if (_set._camTgt)
    {
        _oldTarget = _target;
        _target = _camTgt;
        _targetTime = cTime;
        _oldTargetTime = Glob.time;
        _set._camTgt = false;
    }
    if (_set._camFovMinMax)
    {
        _minFovT = _camFovMin;
        _maxFovT = _camFovMax;
        _minMaxFovTime = cTime;
    }

    if (cTime > _timeCommitted)
    {
        _timeCommitted = cTime;
    }
    if (time <= 0)
    {
        // commit immediatelly
        Simulate(0, SimulateVisibleNear);
    }
}

bool CameraVehicle::GetCommited() const
{
    // check if all times have been reached
    return Glob.time >= _timeCommitted;
}

} // namespace Poseidon
