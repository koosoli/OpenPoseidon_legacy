#pragma once

#include <Poseidon/World/Entities/Vehicles/Vehicle.hpp>

namespace Poseidon
{
struct CameraTarget
{
    OLink<Object> _target;
    Vector3 _pos;

    Vector3 GetPos() const;

    CameraTarget() : _target(nullptr), _pos(VZero) {}
    CameraTarget(Object* obj) : _target(obj), _pos(VZero) {}
    CameraTarget(Vector3Par pos) : _target(nullptr), _pos(pos) {}

    Vector3 PositionAbsToRel(Vector3Val pos) const;
    Vector3 PositionRelToAbs(Vector3Val pos) const;

    LSError Serialize(ParamArchive& ar);
};

class CameraHolder : public Vehicle
{
    typedef Vehicle base;

  protected:
    bool _manual;

    Vector3 _camPos;
    float _camFov, _camFovMin, _camFovMax;
    float _camDive, _camBank, _camHeading;
    // note: target need not be commited
    CameraTarget _camTgt;
    struct Set
    { // what has been set since last commit
        bool _camPos;
        bool _camFov, _camFovMinMax;
        bool _camDive, _camBank, _camHeading;
        bool _camTgt;
        Set()
        {
            _camPos = false;
            _camFov = false, _camFovMinMax = false;
            _camDive = false, _camBank = false, _camHeading = false;
            _camTgt = false;
        }
    } _set;

  public:
    CameraHolder(LODShapeWithShadow* shape, const VehicleNonAIType* type, int id);
    ~CameraHolder() override;

    virtual void SetPos(Vector3Par pos) { _camPos = pos, _set._camPos = true; } // set destination
    virtual void SetFOV(float fov)
    {
        _camFov = fov, _set._camFov = true;
        SetFOVRange(fov, fov);
    } // set camera pars
    virtual void SetFOVRange(float minFov, float maxFov)
    {
        _camFovMin = minFov, _camFovMax = maxFov, _set._camFovMinMax = true;
    }
    virtual void SetDive(float dive) { _camDive = dive, _set._camDive = true; }
    virtual void SetBank(float bank) { _camBank = bank, _set._camBank = true; }
    virtual void SetHeading(float head) { _camHeading = head, _set._camHeading = true; }

    const CameraTarget& GetTarget() const { return _camTgt; }

    virtual void SetTarget(Object* target) { _camTgt = target, _set._camTgt = true; }
    virtual void SetTarget(Vector3Par target) { _camTgt = target, _set._camTgt = true; }
    virtual void Command(RString mode);  // camera dependend special command
    virtual void Commit(float time) = 0; // commit all deferred settings

    virtual void SetManual(bool manual) { _manual = manual; }
    virtual bool GetManual() const { return _manual; }
    virtual void AimDriver(Vector3Val val) {}

    virtual bool GetCommited() const { return true; }

    LSError Serialize(ParamArchive& ar) override;

    USE_CASTING(base)
};

class CameraVehicle : public CameraHolder
{
    typedef CameraHolder base;
    Vector3 _movePos; // where should I move
    CameraTarget _oldTarget;
    CameraTarget _target;
    float _minFovT, _maxFovT; // target values

    float _minFov, _maxFov; // negative means invalid

    Foundation::Time _oldTargetTime;
    Foundation::Time _targetTime;
    Foundation::Time _movePosTime; // when target should be reached
    Foundation::Time _minMaxFovTime;

    Foundation::Time _timeCommitted;

    mutable float _lastFov;
    mutable Vector3 _lastTgtPos;

    bool _inertia; // manual camera simulation
    bool _crossHairs;
    bool _altitudeSpeedScaling;
    bool _reportedWorldBounds;
    bool _mouseLookRequiresRightButton;

  public:
    Vector3 GetTarget() const;

    CameraVehicle();
    ~CameraVehicle() override;

    void ResetTargets();
    //! A manual camera's own fov (what keypad zoom drives), set directly.
    void SetManualFov(float fov) { _lastFov = _minFov = _maxFov = fov; }
    //! Harness only (POSEIDON_FREEFLY_DRIFT): a constant velocity applied inside Simulate, i.e.
    //! per simulation tick exactly as keyboard flight moves the camera.
    void SetHarnessDrift(Vector3Par metresPerSecond) { _harnessDrift = metresPerSecond; }
    Vector3 _harnessDrift = VZero;
    // Test harness: turn about the vertical at this many radians per second (pans, for temporal
    // upscaler/grass motion probes a translating camera cannot reproduce).
    void SetHarnessYawRate(float radiansPerSecond) { _harnessYawRate = radiansPerSecond; }
    float _harnessYawRate = 0.0f;
    // Debug free-fly can opt into Game Master-style speed scaling.
    void SetAltitudeSpeedScaling(bool enabled) { _altitudeSpeedScaling = enabled; }
    void SetMouseLookRequiresRightButton(bool enabled) { _mouseLookRequiresRightButton = enabled; }
    void SetCrossHairs(bool enabled) { _crossHairs = enabled; }

    void Simulate(float deltaT, SimulationImportance prec) override;

    bool OcclusionFire() const override { return false; }
    bool OcclusionView() const override { return false; }

    bool Invisible() const override { return true; }

    // camera effect parameters
    void DrawCameraCockpit() override;
    bool CameraAutoTerminate() override { return _manual; }

    float OutsideCameraDistance(CameraType camType) const override { return 0; }
    float CamEffectFOV() const override;

    void StartFrame() override; // start frame - used for motion blur

    void Command(RString mode) override; // camera dependend special command
    void Commit(float time) override;    // commit all settings

    bool GetCommited() const override;
};

} // namespace Poseidon
