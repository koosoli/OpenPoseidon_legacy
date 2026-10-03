#include <Poseidon/Core/Application.hpp>
#include <Poseidon/Core/Config/EngineConfig.hpp>

#include <Poseidon/World/Entities/Vehicles/Air/Airplane.hpp>
#include <Poseidon/World/Entities/Vehicles/Air/FlightCeiling.hpp>
#include <Poseidon/World/Entities/Weapons/Shots.hpp>
#include <Poseidon/World/Scene/Scene.hpp>
#include <Poseidon/World/World.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/Input/InputSubsystem.hpp>
#include <Poseidon/Core/Global.hpp>
#include <Poseidon/Graphics/Rendering/Lighting/Lights.hpp>
#include <Poseidon/AI/AI.hpp>
#include <Random/randomGen.hpp>
#include <Poseidon/World/Scene/Camera/Camera.hpp>
#include <Poseidon/World/Simulation/FrameInv.hpp>
#include <Poseidon/Dev/Diag/DiagModes.hpp>

#include <Poseidon/Network/Network.hpp>
#include <Poseidon/Game/UiActions.hpp>
#include <Poseidon/UI/Locale/StringtableExt.hpp>
#include <stdio.h>
#include <string.h>
#include <cmath>
#include <Poseidon/Foundation/Common/FltOpts.hpp>
#include <Poseidon/Foundation/Containers/Array.hpp>
#include <Poseidon/Foundation/Framework/DebugLog.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/Foundation/Math/MathDefs.hpp>
#include <Poseidon/Foundation/Math/MathOpt.hpp>
#include <Poseidon/Foundation/Strings/RString.hpp>
#include <Poseidon/Foundation/platform.hpp>

#if _ENABLE_CHEATS
#define ARROWS 1
#else
#define ARROWS 0
#endif

#if _ENABLE_CHEATS
#define LOG_SWEEP 1
#endif

namespace Poseidon
{
Airplane::Airplane(VehicleType* name, Person* pilot)
    : Transport(name, pilot),

      _rpm(0), // on/off
      _rotorSpeed(0), _rotorPosition(0),

      _lastAngVelocity(VZero),

      _thrust(0), _thrustWanted(0), // turning motor on/off
      _elevator(0), _elevatorWanted(0), _rudder(0), _rudderWanted(0), _aileron(0), _aileronWanted(0), _gearsUp(0),
      _flaps(0), _brake(0), _rndFrequency(1 - GRandGen.RandomValue() * 0.05), // do not use same sound frequency

      _pilotGear(true), _rocketLRToggle(false), _pilotFlaps(0), _servoVol(0),

      _gunYRot(0), _gunYRotWanted(0), _gunXRot(0), _gunXRotWanted(0), _gunXSpeed(0), _gunYSpeed(0),

      _gearDammage(false),

      _pilotBrake(1)
{
    _head.SetPars("Air");
    _head.Init(Type()->_pilotPos - Vector3(0, 0.2, 0), Type()->_pilotPos, this);
    SetSimulationPrecision(1.0 / 15);

    _mGunClouds.Load((*Type()->_par) >> "MGunClouds");

    _mGunFireFrames = 0;
    _mGunFireTime = UITIME_MIN;
    _mGunFirePhase = 0;
}

Airplane::~Airplane() = default;

AirplaneType::AirplaneType(const ParamEntry* param) : base(param)
{
    _scopeLevel = 1;

    _gunPos = VZero;
    _gunDir = VForward;
}

void AirplaneType::Load(const ParamEntry& par)
{
    base::Load(par);

    _minGunElev = (float)(par >> "minGunElev") * (H_PI / 180);
    _maxGunElev = (float)(par >> "maxGunElev") * (H_PI / 180);
    _minGunTurn = (float)(par >> "minGunTurn") * (H_PI / 180);
    _maxGunTurn = (float)(par >> "maxGunTurn") * (H_PI / 180);

    _gearRetracting = par >> "gearRetracting";

    _aileronSensitivity = par >> "aileronSensitivity";
    _elevatorSensitivity = par >> "elevatorSensitivity";
    _wheelSteeringSensitivity = par >> "wheelSteeringSensitivity";
    _landingSpeed = par >> "landingSpeed";
    if (_landingSpeed < 1)
    {
        _landingSpeed = GetMaxSpeedMs() * 0.33f;
        saturateMax(_landingSpeed, 120 / 3.6f);
    }
    else
    {
        _landingSpeed *= 1.0f / 3.6f;
    }
    if (_landingSpeed > 21)
    {
        _stallSpeed = _landingSpeed * 0.65f;
    }
    else
    {
        _stallSpeed = _landingSpeed * 0.87f;
    }
    _takeOffSpeed = floatMin(_landingSpeed, 65);

    _flapsFrictionCoef = par >> "flapsFrictionCoef";
    _noseDownCoef = par >> "noseDownCoef";
    _landingAoa = par >> "landingAoa";

    _ejectSpeed = Vector3((par >> "ejectSpeed")[0], (par >> "ejectSpeed")[1], (par >> "ejectSpeed")[2]);
}

void AirplaneType::InitShape()
{
    const ParamEntry& par = *_par;

    _scopeLevel = 2;
    base::InitShape();

    _lRudder.Init(_shape, "leva smerovka", nullptr, "osa leve smerovky");
    _rRudder.Init(_shape, "prava smerovka", nullptr, "osa prave smerovky");
    _lElevator.Init(_shape, "leva vejskovka", nullptr, "osa leve vejskovky");
    _rElevator.Init(_shape, "prava vejskovka", nullptr, "osa prave vejskovky");
    _lAileronT.Init(_shape, "lkh klapka", nullptr, "osa lk klapky", "osa lkh klapky");
    _rAileronT.Init(_shape, "pkh klapka", nullptr, "osa pk klapky", "osa pkh klapky");
    _lAileronB.Init(_shape, "lkd klapka", nullptr, "osa lk klapky", "osa lkd klapky");
    _rAileronB.Init(_shape, "pkd klapka", nullptr, "osa pk klapky", "osa pkd klapky");
    _lFlap.Init(_shape, "ls klapka", nullptr, "osa ls klapky");
    _rFlap.Init(_shape, "ps klapka", nullptr, "osa ps klapky");

    _rotors[0].Init(_shape, "vrtule", "vrtule 0", "osa vrtule", "osa vrtule 0");
    for (int i = 1; i < MaxRotors; i++)
    {
        char selName[64];
        char axisName[64];
        snprintf(selName, sizeof(selName), "vrtule %d", i);
        snprintf(axisName, sizeof(axisName), "osa vrtule %d", i);
        _rotors[i].Init(_shape, selName, nullptr, axisName);
    }

    _rotorStill.Init(_shape, "vrtule staticka", nullptr);
    _rotorMove.Init(_shape, "vrtule blur", nullptr);

    _lWheel.Init(_shape, "levy kolo", nullptr, "osa leveho kola");
    _rWheel.Init(_shape, "pravy kolo", nullptr, "osa praveho kola");
    _fWheel.Init(_shape, "predni kolo", nullptr, "osa predniho kola");

    _altRadarIndicator.Init(_shape, par >> "IndicatorAltRadar");
    _altRadarIndicator2.Init(_shape, par >> "IndicatorAltRadar2");
    _altBaroIndicator.Init(_shape, par >> "IndicatorAltBaro");
    _speedIndicator.Init(_shape, par >> "IndicatorSpeed");
    _vertSpeedIndicator.Init(_shape, par >> "IndicatorVertSpeed");
    _vertSpeedIndicator2.Init(_shape, par >> "IndicatorVertSpeed2");
    _rpmIndicator.Init(_shape, par >> "IndicatorRPM");
    _compass.Init(_shape, par >> "IndicatorCompass");
    _compass2.Init(_shape, par >> "IndicatorCompass2");
    _watch.Init(_shape, par >> "IndicatorWatch");
    _watch2.Init(_shape, par >> "IndicatorWatch2");

    _vario.Init(_shape, "horizont", nullptr, "osa_horizont", nullptr);
    for (int i = 0; i < _shape->NLevels(); i++)
    {
        int sel = _vario.GetSelection(i);
        int selCenter = _vario.GetCenterSelection(i);
        if (sel >= 0 && selCenter >= 0)
        {
            Shape* shape = _shape->Level(i);
            _varioDirection[i] = _vario.Center(i) - shape->CalculateCenter(shape->NamedSel(sel));
        }
        else
        {
            _varioDirection[i] = VForward;
        }
    }

    if (_shape->MemoryLevel()->FindNamedSel("kulomet"))
    {
        _gunPos = _shape->MemoryPoint("kulomet");
    }

    _rocketLPos = _shape->MemoryPoint("L raketa");
    _rocketRPos = _shape->MemoryPoint("P raketa");

    _lDust = _shape->MemoryPoint("levy prach");
    _rDust = _shape->MemoryPoint("pravy prach");

    int level;
    level = _shape->FindLevel(1100);
    if (level >= 0)
    {
        _shape->LevelOpaque(level)->MakeCockpit();
        _pilotPos = _shape->LevelOpaque(level)->NamedPosition("pilot");
    }
    level = _shape->FindLevel(1000);
    if (level >= 0)
    {
        _shape->LevelOpaque(level)->MakeCockpit();
        _gunnerPos = _shape->LevelOpaque(level)->NamedPosition("pilot");
    }

    if (_pilotPos.SquareSize() < 0.1)
    {
        _pilotPos = _shape->MemoryPoint("pilot");
    }
    if (_gunnerPos.SquareSize() < 0.1)
    {
        _gunnerPos = _shape->MemoryPoint("gunner");
    }

    _animFire.Init(_shape, "zasleh", nullptr);
}

static float UpForce(float speed, float aoa, float maxSpeed)
{
    Vector3 force(VZero);
    // speed in knots, force in G; indexed 0–450 knots in 50-knot steps
    const static float maxG[] = {0.0, 0.1, 0.4, 2.2, 3.3, 3.5, 3.2, 2.5, 1.0, 0.0};
    const int maxI = sizeof(maxG) / sizeof(*maxG) - 1;
    const float maxAoa = 16 * H_PI / 180;
    const float minAoa = -3 * H_PI / 180;
    const float neutralAoa = 5 * H_PI / 180;
    aoa += neutralAoa;

    if (aoa > maxAoa)
    {
        aoa = 2 * maxAoa - aoa;
        if (aoa < 0)
        {
            aoa = 0;
        }
    }
    if (aoa < minAoa)
    {
        aoa = minAoa;
    }
    aoa *= 1 / maxAoa;
    float fSpeed = maxI * (speed * 0.7 / maxSpeed);
    int iLow = toIntFloor(fSpeed);
    float frac = fSpeed - iLow;
    if (iLow >= maxI)
    {
        return 0; // no up force
    }
    if (iLow < 0)
    {
        return 0;
    }
    float fLow = maxG[iLow];
    float fHigh = maxG[iLow + 1];
    float speedCoef = fLow + (fHigh - fLow) * frac;
    float ret = speedCoef * aoa;
    return ret;
}

#define FV(x) (x)[0], (x)[1], (x)[2]
#define FFV "[%.0f,%.0f,%.0f]"

static const Color HeliLightColor(0.8, 0.8, 1.0);
static const Color HeliLightAmbient(0.07, 0.07, 0.1);

bool Airplane::CastProxyShadow(int level, int index) const
{
    return base::CastProxyShadow(level, index);
}

static int CountMissiles(const EntityAI* ai)
{
    int total = 0;
    for (int i = 0; i < ai->NMagazines(); i++)
    {
        const Magazine* mag = ai->GetMagazine(i);
        if (mag->_type->_modes.Size() <= 0)
        {
            continue;
        }
        const WeaponModeType* mode = mag->_type->_modes[0];
        const AmmoType* ammo = mode->_ammo;
        if (!ammo)
        {
            continue;
        }
        if (ammo->_simulation != AmmoShotMissile)
        {
            continue;
        }
        if (ammo->maxControlRange < 10)
        {
            continue;
        }
        total += mag->_ammo;
    }
    return total;
}

static int CountAmmo(const EntityAI* ai, const char* weapon)
{
    int total = 0;
    for (int i = 0; i < ai->NMagazines(); i++)
    {
        const Magazine* mag = ai->GetMagazine(i);
        if (strcmpi(mag->_type->GetName(), weapon) != 0)
        {
            continue;
        }
        total += mag->_ammo;
    }
    return total;
}

int Airplane::GetProxyComplexity(int level, const FrameBase& pos, float dist2) const
{
    return base::GetProxyComplexity(level, pos, dist2);
}

void Airplane::DrawProxies(int level, ClipFlags clipFlags, const Matrix4& transform, const Matrix4& invTransform,
                           float dist2, float z2, const LightList& lights)
{
    base::DrawProxies(level, clipFlags, transform, invTransform, dist2, z2, lights);
}

Object* Airplane::GetProxy(LODShapeWithShadow*& shape, int level, Matrix4& transform, Matrix4& invTransform,
                           const FrameBase& parentPos, int i) const
{
    return base::GetProxy(shape, level, transform, invTransform, parentPos, i);
}

void Airplane::Draw(int level, ClipFlags clipFlags, const FrameBase& pos)
{
    base::Draw(level, clipFlags, pos);
}

void AirplaneAuto::DrawDiags()
{
    // draw ILS position and direction

    LODShapeWithShadow* forceArrow = GScene->ForceArrow();

    Vector3 ilsPos, ilsDir;
    GWorld->GetILS(ilsPos, ilsDir, _targetSide);
    {
        Ref<Object> arrow = new ObjectColored(forceArrow, -1);

        float size = 3;
        arrow->SetPosition(ilsPos);
        arrow->SetOrient(VUp, VForward);
        arrow->SetPosition(arrow->PositionModelToWorld(forceArrow->BoundingCenter() * size));
        arrow->SetScale(size);
        arrow->SetConstantColor(PackedColor(Color(0, 0, 0)));
        GScene->ObjectForDrawing(arrow);
    }

    // if (!QIsManual())
    {
        Vector3 dir = Matrix3(MRotationY, -_pilotHeading).Direction();

        Ref<Object> arrow = new ObjectColored(forceArrow, -1);

        float size = 0.3f;
        arrow->SetPosition(Position() + VUp * 2);
        arrow->SetOrient(dir, VUp);
        arrow->SetPosition(arrow->PositionModelToWorld(forceArrow->BoundingCenter() * size));
        arrow->SetScale(size);
        arrow->SetConstantColor(PackedColor(Color(0, 1, 0)));
        GScene->ObjectForDrawing(arrow);
    }

    {
        Ref<Object> arrow = new ObjectColored(forceArrow, -1);

        float size = 1;
        arrow->SetPosition(ilsPos + ilsDir * 10);
        arrow->SetOrient(VUp, VForward);
        arrow->SetPosition(arrow->PositionModelToWorld(forceArrow->BoundingCenter() * size));
        arrow->SetScale(size);
        arrow->SetConstantColor(PackedColor(Color(0, 0, 0)));
        GScene->ObjectForDrawing(arrow);
    }

    if (_sweepTarget)
    {
        {
            Ref<Object> arrow = new ObjectColored(forceArrow, -1);

            float size = 5;
            arrow->SetPosition(_sweepTarget->AimingPosition());
            arrow->SetOrient(VUp, VForward);
            arrow->SetPosition(arrow->PositionModelToWorld(forceArrow->BoundingCenter() * size));
            arrow->SetScale(size);
            arrow->SetConstantColor(PackedColor(Color(1, 0, 0)));
            GScene->ObjectForDrawing(arrow);
        }
        {
            Ref<Object> arrow = new ObjectColored(forceArrow, -1);

            float size = 5;
            Vector3 pos = Position() + Direction() * 10;
            arrow->SetPosition(pos);
            arrow->SetOrient(_sweepTarget->AimingPosition() - pos, VUp);
            arrow->SetPosition(arrow->PositionModelToWorld(forceArrow->BoundingCenter() * size));
            arrow->SetScale(size);
            arrow->SetConstantColor(PackedColor(Color(1, 0, 0)));
            GScene->ObjectForDrawing(arrow);
        }
    }

    base::DrawDiags();
}

RString AirplaneAuto::DiagText() const
{
    RString text = base::DiagText();
    char buf[256];
    snprintf(buf, sizeof(buf), " h=%.1f, d=%.2f", _pilotHeight, _forceDive);
    if (_sweepTarget)
    {
        sprintf(buf + strlen(buf), " sweep %d", _sweepState);
    }
    return text + RString(buf);
}

inline const float DecreaseAbs(float x, float y)
{
    if (fabs(x) < y)
    {
        return 0;
    }
    if (x > 0)
    {
        return x - y;
    }
    return x + y;
}

inline float CalculateAOA(Vector3Par speed)
{
    float speedSize2 = speed.SquareSize();
    return speedSize2 > 1e-6 ? -speed.Y() * InvSqrt(speedSize2) : 0;
}

float Airplane::DetectStall() const
{
    if (_landContact)
    {
        return 0;
    }
    float aoa = CalculateAOA(ModelSpeed());

    float stallSpeed = Type()->_stallSpeed;

    float stall = 1.5 - ModelSpeed().Z() / stallSpeed;
    saturateMax(stall, 0);
    stall += 0.3f;
    saturateMin(stall, 1);

    const float maxAoa = 10 * H_PI / 180;
    const float okAoa = 5 * H_PI / 180;

    float aoaStall = (aoa - okAoa) / maxAoa;
    saturate(aoaStall, 0, 1);

    return stall * aoaStall;
}

void Airplane::PerformFF(FFEffects& eff)
{
    base::PerformFF(eff);
    eff.engineFreq = 7;
    eff.engineMag = DetectStall() * 0.25f;
    eff.stiffnessX = 0.6f;
    eff.stiffnessY = 0.6f;
}

void Airplane::Simulate(float deltaT, SimulationImportance prec)
{
    _isDead = IsDammageDestroyed();

    if (_thrust > 0)
    {
        ConsumeFuel(deltaT * 0.03f);
    }

    float delta;

    if (_isDead)
    {
        _engineOff = true, _pilotBrake = 1;
    }
    if (_fuel <= 0)
    {
        _engineOff = true;
    }
    if (_engineOff)
    {
        _thrustWanted = 0;
        if (_landContact)
        {
            _pilotBrake = 1;
        }
    }

    delta = !_engineOff - _rpm;
    Limit(delta, -0.1f * deltaT, +0.1f * deltaT);
    _rpm += delta;
    Limit(_rpm, 0, 1);

    delta = _thrustWanted - _thrust;
    Limit(delta, -0.7f * deltaT, +0.4f * deltaT);
    _thrust += delta;
    Limit(_thrust, 0, 1);

    _rotorSpeed = 10.0f * _rpm * (0.4f + _thrust);
    _rotorPosition += _rotorSpeed * deltaT * 20.0f;

    Vector3Val speed = ModelSpeed();

    // AIR-010: available force in thin air. Was a straight line from 5000 m to
    // zero at 13000 m -- and PAST 13000 m it went negative, inverting thrust,
    // lift and (below) drag. Now an exponential falloff with a positive floor;
    // FlightCeiling.hpp has the derivation and the runtime switch back to the
    // stock curve.
    const float alt = Position().Y();
    const float altCoef = Air::PlaneAirCoef(alt);

    float flapEff = fabs(speed[2]) / (Type()->_landingSpeed * 0.65f) - 0.5f;
    saturate(flapEff, 0, 1);
    flapEff *= altCoef;

    Matrix3 undive;
    undive.SetUpAndDirection(VUp, Direction());
    Vector3 bankSide = undive.InverseRotation() * DirectionAside();
    float bank = bankSide.Y();
    float rudderWanted = bank * flapEff + _rudderWanted; //*(1-flapEff);
    const float fullElevSpeed = floatMin(25, Type()->_landingSpeed * 0.8f);
    const float noElevSpeed = fullElevSpeed * 0.5;
    if (speed[2] < fullElevSpeed && _landContact)
    {
        float maxElev = (speed[2] - 13.5f) / (fullElevSpeed - noElevSpeed);
        saturate(maxElev, 0, 1);
        // do not use ailerons and elevators when ground steering
        saturate(_aileronWanted, -maxElev, +maxElev);
        saturate(_elevatorWanted, -maxElev, +maxElev);
    }

    delta = rudderWanted - _rudder;
    Limit(delta, -4 * deltaT, 4 * deltaT);
    _rudder += delta;
    Limit(_rudder, -1, +1);

    delta = _aileronWanted - _aileron;
    Limit(delta, -4 * deltaT, 4 * deltaT);
    _aileron += delta;
    Limit(_aileron, -1, +1);

    delta = _elevatorWanted - _elevator;
    Limit(delta, -4 * deltaT, 4 * deltaT);
    _elevator += delta;
    Limit(_elevator, -1, +1);

    bool doServo = false;

    float gearWanted = 1 - _pilotGear;
    if (_gearDammage)
    {
        gearWanted = -0.66f;
    }
    if (!Type()->_gearRetracting)
    {
        gearWanted = 0;
    }
    delta = gearWanted - _gearsUp;
    if (fabs(delta) > 0.01)
    {
        doServo = true;
    }
    Limit(delta, -0.5f * deltaT, +0.3f * deltaT);
    float oldGears = _gearsUp;
    _gearsUp += delta;
    const float tholdUp = 0.95f;
    const float tholdDown = 0.05f;
    if (_gearsUp >= tholdUp && oldGears < tholdUp)
    {
        OnEvent(EEGear, false);
    }
    if (_gearsUp <= tholdDown && oldGears > tholdDown)
    {
        OnEvent(EEGear, true);
    }
    Limit(_gearsUp, -1, 1);

    delta = _pilotFlaps * 0.5 - _flaps;
    if (fabs(delta) > 0.01)
    {
        doServo = true;
    }
    Limit(delta, -0.33 * deltaT, 0.33 * deltaT);
    _flaps += delta;
    Limit(_flaps, 0, 1);

    delta = doServo - _servoVol;
    Limit(delta, -2 * deltaT, 2 * deltaT);
    _servoVol += delta;

    float brakeWanted = _pilotBrake;
    if (_landContact && fabs(speed[2]) < 10)
    {
        brakeWanted = 0;
    }
    if (_isDead)
    {
        brakeWanted = 0;
    }
    delta = brakeWanted - _brake;
    Limit(delta, -1 * deltaT, 1 * deltaT);
    _brake += delta;
    Limit(_brake, 0, 1);

    // do not predict too much (MP)
    if (!CheckPredictionFrozen())
    {
        Vector3 force(VZero), friction(VZero);
        Vector3 torque(VZero), torqueFriction(VZero);

        Vector3 wCenter(VFastTransform, ModelToWorld(), GetCenterOfMass());

        Vector3 pForce(VZero), pCenter(VZero);

        float maxSpeed = GetType()->GetMaxSpeedMs();
        float spdCoef = Interpolativ(speed.Z(), maxSpeed * 0.66f, maxSpeed * 1.15f, 1, 0);
        force[2] += _thrust * GetMass() * sqrt(maxSpeed) * (0.4 * G_CONST / 13) * altCoef * spdCoef;
        // real aoa - angle between plane axis and plane speed direction
        // assume x==sin(x) for small x
        float aoa = CalculateAOA(ModelSpeed()) + _flaps * (3 * H_PI / 180);

        float upForce = UpForce(fabs(speed[2]), aoa, maxSpeed) * altCoef;
        force[1] += upForce * G_CONST * GetMass();
#if ARROWS
        AddForce(wCenter, DirectionModelToWorld(force) * InvMass(), Color(0, 1, 0));
#endif
        PoseidonAssert(force.Size() < G_CONST * GetMass() * 10);
        if (_landContact && speed[2] < 15)
        {
            // steering engaged
            pCenter = Vector3(0, 0, 0.75);
            float steerEff = fabs(speed[2] * 6);
            saturateMin(steerEff, 40);
            steerEff *= Type()->_wheelSteeringSensitivity;
            pForce = Vector3(-_rudder * GetMass() * steerEff, 0, 0);
            torque += pCenter.CrossProduct(pForce);
#if ARROWS
            AddForce(DirectionModelToWorld(pCenter) + wCenter, DirectionModelToWorld(pForce) * InvMass());
#endif
        }
        else
        {
            float sizeCoef = GetRadius() * (1.0 / 6) * Type()->_aileronSensitivity;
            pCenter = Vector3(5, 0, 0);
            pForce = Vector3(0, _aileron * GetMass() * flapEff * G_CONST * 0.8 * sizeCoef, 0);
            torque += pCenter.CrossProduct(pForce);
#if ARROWS
            AddForce(DirectionModelToWorld(pCenter) + wCenter, DirectionModelToWorld(pForce) * InvMass());
#endif
        }
        pForce = Vector3(0, _elevator * GetMass() * flapEff * G_CONST * 0.6 * Type()->_elevatorSensitivity, 0);
        pCenter = Vector3(0, 0, -5);
        torque += pCenter.CrossProduct(pForce);
#if ARROWS
        AddForce(DirectionModelToWorld(pCenter) + wCenter, DirectionModelToWorld(pForce) * InvMass(), Color(0, 0, 1));
#endif

        if (_isDead)
        {
            torque += Vector3(1.5, 3.2, -2.5).CrossProduct(Vector3(0, GetMass(), 0));
        }
        else if (fabs(ModelSpeed().Z()) < Type()->_landingSpeed * 0.65f)
        {
            float stall = DetectStall();
            torque += Vector3(1.5, 3.2, -2.5).CrossProduct(Vector3(0, GetMass() * stall, 0));
        }

        float rudderZ = GetRadius() * 0.38f;
        pForce = Vector3(_rudder * GetMass() * flapEff, 0, 0);
        pCenter = Vector3(0, -0.25, -rudderZ); // rudder causes some change in bank as well
        torque += pCenter.CrossProduct(pForce);
        PoseidonAssert(torque.IsFinite());
#if ARROWS
        AddForce(DirectionModelToWorld(pCenter) + wCenter, DirectionModelToWorld(pForce) * InvMass());
#endif

        pForce = Vector3(bank * 0.3 * GetMass() * flapEff, 0, 0);
        pCenter = Vector3(0, 0, -3);
        torque += pCenter.CrossProduct(pForce);
#if ARROWS
        AddForce(DirectionModelToWorld(pCenter) + wCenter, DirectionModelToWorld(pForce) * InvMass());
#endif

        float highSpeed = speed[2] / (Type()->_landingSpeed * 0.65f * 0.7f) - 0.3f;
        saturate(highSpeed, 0, 1);
        pForce[1] = (speed[1] * -0.05f + speed[1] * fabs(speed[1]) * -0.10f) * GetMass();
        pForce[0] = (speed[0] * -0.1f + speed[0] * fabs(speed[0]) * -0.04f) * GetMass();
        pForce[2] = 0;
        pCenter = VZero;
        pForce *= highSpeed * altCoef;
        force += pForce;
#if ARROWS
        AddForce(DirectionModelToWorld(pCenter) + wCenter, DirectionModelToWorld(pForce) * InvMass(), Color(1, 1, 0));
#endif
        if (highSpeed < 1)
        {
            pForce = -speed * 0.25f * (1 - highSpeed) * GetMass();
            pCenter = Vector3(0, 0, -4);
            torque += pCenter.CrossProduct(pForce);
#if ARROWS
            AddForce(DirectionModelToWorld(pCenter) + wCenter, DirectionModelToWorld(pForce) * InvMass(),
                     Color(0, 0, 1));
#endif
        }

        friction[0] = speed[0] * fabs(speed[0]) * 0.00028 + speed[0] * 0.028;
        friction[1] = speed[1] * fabs(speed[1]) * 0.00050 + speed[1] * 0.050;
        friction[2] = speed[2] * fabs(speed[2]) * 0.00006 + speed[2] * 0.006;
        friction[2] *= (1.0 + _flaps * Type()->_flapsFrictionCoef + fabs(1 - _gearsUp) * 0.5 + 3 * _brake);
        friction *= GetMass() * altCoef;

#if ARROWS
        AddForce(wCenter, DirectionModelToWorld(friction) * InvMass(), Color(1, 0, 0));
#endif

        DirectionModelToWorld(torque, torque);
        DirectionModelToWorld(force, force);
        DirectionModelToWorld(friction, friction);

        // when the plane is banking, nose goes down
        // I have no proper explanation for this, but it happens
        // this force is key for turning
        // bank causes plane front tip to go down (in world coordinate system)
        float noseCoef = fabs(bank);

        saturateMin(noseCoef, 0.6f);
        pForce = Vector3(0, noseCoef * GetMass() * 7 * noseCoef * Type()->_noseDownCoef, 0);
        pCenter = -3 * Direction();
        torque += pCenter.CrossProduct(pForce);

#if ARROWS
        AddForce(pCenter + wCenter, pForce * InvMass(), Color(0, 1, 1));
#endif

        torqueFriction = _angMomentum * 2.0;

        Matrix4 movePos;
        ApplySpeed(movePos, deltaT);
        Frame moveTrans;
        moveTrans.SetTransform(movePos);

#if ARROWS
        AddForce(wCenter, -friction * InvMass());
#endif

        pForce = Vector3(0, -1, 0) * (GetMass() * G_CONST);
        force += pForce;
#if ARROWS
        AddForce(wCenter, pForce * InvMass());
#endif

        float totalPressure = 0;

        wCenter.SetFastTransform(movePos, GetCenterOfMass());

        float soft = 0, dust = 0;
        if (deltaT > 0)
        {
            bool wasLandContact = _landContact;
            _objectContact = false;
            _landContact = false;
            _waterContact = false;

            Vector3 totForce(VZero);

            float crash = 0;
            if (IsLocal())
            {
                CollisionBuffer collision;
                GLandscape->ObjectCollision(collision, this, moveTrans);
                if (collision.Size() > 0)
                {
#define MAX_IN 0.4
#define MAX_IN_FORCE 0.2
#define MAX_IN_FRICTION 0.4

                    _objectContact = true;
                    for (int i = 0; i < collision.Size(); i++)
                    {
                        CollisionInfo& info = collision[i];
                        if (info.object)
                        {
                            if (info.object->GetMass() >= 1000)
                            {
                                Point3 pos = info.object->PositionModelToWorld(info.pos);
                                Vector3 dirOut = info.object->DirectionModelToWorld(info.dirOut);
                                float forceIn = floatMin(info.under, MAX_IN_FORCE);
                                Vector3 pForce = dirOut * GetMass() * 30 * forceIn;
                                pCenter = pos - wCenter;
                                totForce += pForce;
                                torque += pCenter.CrossProduct(pForce);

                                Vector3Val objSpeed = info.object->ObjectSpeed();
                                Vector3 colSpeed = _speed - objSpeed;
                                // if info.under is bigger than MAX_IN, move out
                                if (info.under > MAX_IN)
                                {
                                    Point3 newPos = moveTrans.Position();
                                    float moveOut = info.under - MAX_IN;
                                    newPos += dirOut * moveOut * 0.1;
                                    moveTrans.SetPosition(newPos);
                                }

                                saturateMax(crash, (colSpeed.SquareSize() - 25) * (1.0 / 25));

                                const float maxRelSpeed = 5;
                                if (colSpeed.SquareSize() > Square(maxRelSpeed))
                                {
                                    // adapt _speed to match criterion
                                    colSpeed.Normalize();
                                    colSpeed *= maxRelSpeed;
                                    // only slow down
                                    float oldSize = _speed.Size();
                                    _speed = colSpeed + objSpeed;
                                    if (_speed.SquareSize() > Square(oldSize))
                                    {
                                        _speed = _speed.Normalized() * oldSize;
                                    }
                                }

                                // second is "land friction" - causing no momentum

                                float frictionIn = floatMin(info.under, MAX_IN_FRICTION);
                                pForce[0] = fSign(speed[0]) * 20000;
                                pForce[1] =
                                    speed[1] * fabs(speed[1]) * 1000 + speed[1] * 8000 + fSign(speed[1]) * 10000;
                                pForce[2] = speed[2] * fabs(speed[2]) * 150 + speed[2] * 250 + fSign(speed[2]) * 2000;

                                pForce = DirectionModelToWorld(pForce) * GetMass() * (1.0 / 1000) * frictionIn;
#if ARROWS
                                AddForce(wCenter + pCenter, -pForce * InvMass());
#endif
                                friction += pForce;
                                torqueFriction += _angMomentum * 0.15;
                            }
                        }
                    }
                }
            }

            GroundCollisionBuffer gCollision;
            bool enableLandcontact = true;
            if (wasLandContact)
            {
                float dx, dz;
                GLandscape->RoadSurfaceY(Position(), &dx, &dz);
                Vector3 up(-dx, 1, -dz);
                up.Normalize();
                // check aside and front-back separetelly
                Vector3 relUp = DirectionWorldToModel(up);

                if (fabs(relUp.X()) > 0.17f || fabs(relUp.Z()) > 0.34f) // sin 10 deg, sin 20 deg
                {
                    enableLandcontact = false;
                }
            }
            GLOB_LAND->GroundCollision(gCollision, this, moveTrans, 0.05, 1, enableLandcontact);

            if (gCollision.Size() > 0)
            {
                Vector3 gFriction(VZero);
                float maxUnder = 0;
#define MAX_UNDER 0.1
#define MAX_UNDER_FORCE 0.1
                Shape* landcontact = GetShape()->LandContactLevel();
                int nContactPoint = landcontact ? landcontact->NPos() : 3;
                saturateMax(nContactPoint, gCollision.Size());
                const float nPointCoef = 3.0f / nContactPoint;
                for (int i = 0; i < gCollision.Size(); i++)
                {
                    UndergroundInfo& info = gCollision[i];
                    if (info.under < 0)
                    {
                        continue;
                    }
                    float under;
                    if (info.type == GroundWater)
                    {
                        under = floatMin(info.under, 2) * 0.00025 * nPointCoef;
                        _waterContact = true;
                        if (_speed.SquareSize() > Square(8))
                        {
                            crash = 2;
                        }
                    }
                    else
                    {
                        _landContact = true;
                        if (maxUnder < info.under)
                        {
                            maxUnder = info.under;
                        }
                        under = floatMin(info.under, MAX_UNDER_FORCE);
                    }

                    Vector3 dirOut = Vector3(0, info.dZ, 1).CrossProduct(Vector3(1, info.dX, 0)).Normalized();
                    pForce = dirOut * (GetMass() * 120.0 * under * nPointCoef);
                    pCenter = info.pos - wCenter;
                    torque += pCenter.CrossProduct(pForce);
                    totForce += pForce;

                    if (pForce.SquareSize() > Square(GetMass() * G_CONST * 2))
                    {
                        if (IsLocal())
                        {
                            if (_gearsUp < 0.5)
                            {
                                _gearDammage = true;
                            }
                        }
                    }

#if ARROWS
                    AddForce(wCenter + pCenter, pForce * InvMass());
#endif

                    Vector3 sSpeed = speed - dirOut * (dirOut * speed);
                    float pressure = under * 10 * nPointCoef;
                    pForce[0] = fSign(sSpeed[0]) * 24000 * pressure;
                    pForce[1] = fSign(sSpeed[1]) * 24000 * pressure;
                    pForce[2] = sSpeed[2] * 300 * pressure + fSign(sSpeed[2]) * 500 * pressure;

                    if (info.texture)
                    {
                        soft = info.texture->Roughness() * 2.5;
                        dust = info.texture->Dustness() * 2.5;
                    }

                    pForce[0] += pressure * soft * sSpeed[0] * fabs(sSpeed[0]) * 100;
                    pForce[1] += pressure * soft * sSpeed[1] * fabs(sSpeed[1]) * 100;
                    pForce[2] += pressure * soft * sSpeed[2] * fabs(sSpeed[2]) * 25;

                    if (IsDammageDestroyed() || !enableLandcontact)
                    {
                        if (fabs(sSpeed[2]) > 2)
                        {
                            pForce[2] += fSign(sSpeed[2]) * 160000 * pressure;
                        }
                        else
                        {
                            pForce[2] += fSign(sSpeed[2]) * 40000 * pressure;
                        }
                    }
                    else if (fabs(sSpeed[2]) < 25)
                    {
                        pForce[2] += fSign(sSpeed[2]) * 20000 * pressure * _pilotBrake;
                    }

                    totalPressure += pressure;

                    if (fabs(sSpeed[0]) < 0.1)
                    {
                        pForce[0] = 0;
                    }
                    if (fabs(sSpeed[1]) < 0.1)
                    {
                        pForce[1] = 0;
                    }
                    if (fabs(sSpeed[2]) < 0.1)
                    {
                        pForce[2] = 0;
                    }

                    pForce = DirectionModelToWorld(pForce) * nPointCoef * (GetMass() * (1.0 / 10000));

                    if (pForce.SquareSize() > Square(GetMass() * G_CONST))
                    {
                        if (IsLocal())
                        {
                            if (_gearsUp < 0.5)
                            {
                                _gearDammage = true;
                            }
                        }
                    }

                    friction += pForce;

#if ARROWS
                    AddForce(wCenter + pCenter + Vector3(0, 0.2, 0), -pForce * InvMass());
#endif
                    torque -= pCenter.CrossProduct(pForce); // sub: it is friction

                    torqueFriction += _angMomentum * 0.5;
                }
                if (_waterContact)
                {
                    const SurfaceInfo& info = GLandscape->GetWaterSurface();
                    soft = info._roughness * 2.5;
                    dust = info._dustness * 2.5;
                }
                if (_gearsUp > 0.5)
                {
                    saturateMax(crash, (_speed.SquareSize() - 25) * (1.0 / 20));
                }
                else if (_gearsUp > -0.5)
                {
                    Vector3 crashSpeed = ModelSpeed();
                    crashSpeed[0] = DecreaseAbs(crashSpeed[0], 2);
                    crashSpeed[1] = DecreaseAbs(crashSpeed[1], 3);
                    crashSpeed[2] = DecreaseAbs(crashSpeed[2], maxSpeed * 0.40);
                    saturateMax(crash, (crashSpeed.SquareSize() - 15) * (1.0 / 20));
                }
                else
                {
                    Vector3 crashSpeed = ModelSpeed();
                    crashSpeed[0] = DecreaseAbs(crashSpeed[0], 2);
                    crashSpeed[1] = DecreaseAbs(crashSpeed[1], 2);
                    crashSpeed[2] = DecreaseAbs(crashSpeed[2], maxSpeed * 0.25);
                    saturateMax(crash, (crashSpeed.SquareSize() - 10) * (1.0 / 20));
                }
                if (maxUnder > MAX_UNDER)
                {
                    Point3 newPos = moveTrans.Position();
                    float moveUp = maxUnder - MAX_UNDER;
                    newPos[1] += moveUp;
                    moveTrans.SetPosition(newPos);

                    if (_speed.SquareSize() > Square(70))
                    {
                        _speed.Normalize();
                        _speed *= 70;
                    }
                    saturateMax(_speed[1], -0.1);
                }
                if (_waterContact)
                {
                    friction += 8 * _speed;
                }
            }
            force += totForce;

            if (crash > 0)
            {
                if (Glob.time > _disableDammageUntil)
                {
                    _disableDammageUntil = Glob.time + 0.5;
                    _doCrash = CrashLand;
                    if (_objectContact)
                    {
                        _doCrash = CrashObject;
                    }
                    if (_waterContact)
                    {
                        _doCrash = CrashWater;
                    }
                    _crashVolume = crash;
                    saturateMax(_crashVolume, crash);

                    CrashDammage(crash);
                    if (IsLocal() && crash > 5)
                    {
                        float maxTime = 5 - crash * 0.2;
                        saturate(maxTime, 0.5, 3);
                        if (_explosionTime > Glob.time + maxTime)
                        {
                            _explosionTime = Glob.time + GRandGen.Gauss(0, maxTime * 0.3, maxTime);
                        }
                    }
                }
            }
        }

        if (_pilotBrake > 0.5f && _landContact)
        {
            if (_speed.SquareSize() < 0.5f)
            {
                _speed = VZero;
            }
        }

        _lastAngVelocity = _angVelocity; // helper for prediction
        ApplyForces(deltaT, force, torque, friction, torqueFriction);

        if (prec <= SimulateCamera)
        {
            _head.Move(deltaT, moveTrans, *this);
        }

        if (prec <= SimulateVisibleFar && _landContact && DirectionUp().Y() >= 0.3 && fabs(ModelSpeed()[2]) > 1)
        {
            float dSoft = floatMax(dust, 0.001) * totalPressure;
            float density = fabs(ModelSpeed()[2]) * (1.0 / 10) * dSoft * (2 + _pilotBrake * 8);
            density *= GetMass() * (1.0 / 40000);
            if (density >= 0.002)
            {
                Vector3 lPos = PositionModelToWorld(Type()->_lDust);
                Vector3 rPos = PositionModelToWorld(Type()->_rDust);
                saturate(density, 0, 1);
                float dustColor = dSoft * 8;
                saturate(dustColor, 0, 1);
                Color color = Color(0.51, 0.46, 0.33) * dustColor + Color(0.5, 0.5, 0.5) * (1 - dustColor);
                _leftDust.SetColor(color);
                _rightDust.SetColor(color);
                _leftDust.Simulate(lPos + _speed * 0.1, _speed * 0.66, density, deltaT);
                _rightDust.Simulate(rPos + _speed * 0.1, _speed * 0.66, density, deltaT);
            }
        }

        if (_isDead && (_landContact || _objectContact))
        {
            NeverDestroy();
            SmokeSourceVehicle* smoke = dyn_cast<SmokeSourceVehicle>(GetSmoke());
            if (smoke)
            {
                smoke->Explode();
            }
        }

        Move(moveTrans); // finally apply move
        DirectionWorldToModel(_modelSpeed, _speed);
    } // if (!CheckPredictionFrozen())

    if (EnableVisualEffects(prec))
    {
        if (_mGunClouds.Active() || _mGunFire.Active())
        {
            Matrix4Val toWorld = Transform();
            Vector3Val dir = toWorld.Direction();
            Vector3 gunPos(VFastTransform, toWorld, Type()->_gunPos);
            _mGunClouds.Simulate(gunPos, Speed() * 0.7 + dir * 5.0, 0.35, deltaT);
            _mGunFire.Simulate(gunPos, deltaT);
        }
    }

    base::Simulate(deltaT, prec);
}

Vector3 Airplane::GetEyeDirection() const
{
    AIUnit* unit = CommanderUnit();
    if (!unit)
    {
        return base::GetEyeDirection();
    }
    if (unit != PilotUnit())
    {
        return base::GetEyeDirection();
    }
    if (QIsManual(unit))
    {
        return base::GetEyeDirection();
    }
    Person* person = unit->GetPerson();
    Target* tgt = _fire._fireTarget;
    if (!tgt)
    {
        tgt = unit->GetTargetAssigned();
    }
    if (!tgt)
    {
        return base::GetEyeDirection();
    }
    Vector3 tgtPos = tgt->LandAimingPosition();
    Vector3 relDir = PositionWorldToModel(tgtPos);
    float heading = atan2(relDir.X(), relDir.Z());
    float dive = 0;
    float fov = 1;
    Type()->_viewPilot.LimitVirtual(CamInternal, heading, dive, fov);
    Matrix3 rotHeading(MRotationY, -heading);
    Vector3 dir = rotHeading.Direction();
    Vector3 wDir = DirectionModelToWorld(dir);
    if (person)
    {
        // Matrix4P invTrans = person->WorldTransform().InverseRotation();
        //  our model space is worldspace of the person
        // Vector3 personDir = invTrans.Rotate(wDir);
        person->AimObserver(dir);
    }
    return wDir;
}

bool Airplane::IsContinuous(CameraType camType) const
{
    return false; // input.IsLookAroundEnabled();
}

bool Airplane::Airborne() const
{
    return !_landContact;
}

bool AirplaneAuto::IsAway(float factor)
{
    return false;
}

void AirplaneAuto::EngineOn()
{
    base::EngineOn();
}
void AirplaneAuto::EngineOff()
{
    _thrustWanted = 0;
    base::EngineOff();
}

float Airplane::GetEngineVol(float& freq) const
{
    freq = _rndFrequency * (_thrust + 0.5) * _rpm;
    return (_thrust + 0.5);
}

float Airplane::GetEnvironVol(float& freq) const
{
    freq = 1;
    return _speed.Size() / Type()->GetMaxSpeedMs();
}

void Airplane::Sound(bool inside, float deltaT)
{
    if (_servoVol > 0.001)
    {
        const SoundPars& pars = Type()->GetServoSound();
        if (!_servoSound)
        {
            _servoSound = GSoundScene->OpenAndPlay(pars.name, Position(), Speed());
        }
        if (_servoSound)
        {
            float vol = pars.vol * _servoVol;
            float freq = pars.freq;
            _servoSound->SetVolume(vol, freq); // volume, frequency
            _servoSound->Set3D(!inside);
            _servoSound->SetPosition(Position(), Speed());
        }
    }
    else
    {
        _servoSound.Free();
    }

    base::Sound(inside, deltaT);
}

void Airplane::UnloadSound()
{
    base::UnloadSound();
}

Matrix4 Airplane::InsideCamera(CameraType camType) const
{
    return base::InsideCamera(camType);
}

int Airplane::InsideLOD(CameraType camType) const
{
    return base::InsideLOD(camType);
}

void Airplane::InitVirtual(CameraType camType, float& heading, float& dive, float& fov) const
{
    base::InitVirtual(camType, heading, dive, fov);
}

void Airplane::LimitVirtual(CameraType camType, float& heading, float& dive, float& fov) const
{
    base::LimitVirtual(camType, heading, dive, fov);
}

bool Airplane::IsAnimated(int level) const
{
    return true;
}
bool Airplane::IsAnimatedShadow(int level) const
{
    return !ShadowPoseFrozen();
}

void Airplane::Animate(int level)
{
    Type()->_lFlap.Rotate(_shape, -_flaps * 0.5, level);
    Type()->_rFlap.Rotate(_shape, +_flaps * 0.5, level);
    Type()->_lElevator.Rotate(_shape, -_elevator * 0.5, level);
    Type()->_rElevator.Rotate(_shape, -_elevator * 0.5, level);
    float abOff = _brake * 1.5;
    float tAileron = floatMin(_aileron * 0.5 + abOff, +1.4);
    float bAileron = floatMax(_aileron * 0.5 - abOff, -1.4);
    Type()->_lAileronT.Rotate(_shape, tAileron, level);
    Type()->_lAileronB.Rotate(_shape, bAileron, level);
    Type()->_rAileronT.Rotate(_shape, bAileron, level);
    Type()->_rAileronB.Rotate(_shape, tAileron, level);
    Type()->_lRudder.Rotate(_shape, -_rudder * 0.4, level);
    Type()->_rRudder.Rotate(_shape, -_rudder * 0.4, level);
    for (int i = 0; i < Type()->MaxRotors; i++)
    {
        Type()->_rotors[i].Rotate(_shape, _rotorPosition, level);
    }
    if (_rotorSpeed < 1.0)
    {
        Type()->_rotorMove.Hide(_shape, level);
    }
    else
    {
        Type()->_rotorStill.Hide(_shape, level);
    }

    Matrix4 fGear(MRotationX, -_gearsUp * (H_PI * 0.58));
    Type()->_fWheel.Apply(_shape, fGear, level);
    Matrix4 bGear(MRotationX, -_gearsUp * (H_PI * 0.45));
    Type()->_rWheel.Apply(_shape, bGear, level);
    Type()->_lWheel.Apply(_shape, bGear, level);

    if (_mGunFireFrames > 0 || Glob.uiTime < _mGunFireTime + 0.05)
    {
        Type()->_animFire.Unhide(_shape, level);
        Type()->_animFire.SetPhase(_shape, level, _mGunFirePhase);
        _mGunFireFrames--;
    }
    else
    {
        Type()->_animFire.Hide(_shape, level);
    }

    // indicators
    float value = fastFmod(Position().Y(), 304);
    Type()->_altRadarIndicator.SetValue(_shape, level, value);
    Type()->_altRadarIndicator2.SetValue(_shape, level, value);
    value = Position().Y() - GLandscape->SurfaceYAboveWater(Position().X(), Position().Z());
    Type()->_altBaroIndicator.SetValue(_shape, level, value);
    value = fabs(ModelSpeed()[2]);
    Type()->_speedIndicator.SetValue(_shape, level, value);
    value = _speed.Y();
    Type()->_vertSpeedIndicator.SetValue(_shape, level, value);
    Type()->_vertSpeedIndicator2.SetValue(_shape, level, value);
    value = _rpm * _thrust;
    Type()->_rpmIndicator.SetValue(_shape, level, value);
    value = atan2(Direction().X(), Direction().Z());
    Type()->_compass.SetValue(_shape, level, value);
    Type()->_compass2.SetValue(_shape, level, value);
    Type()->_watch.SetTime(_shape, level, Glob.clock);
    Type()->_watch2.SetTime(_shape, level, Glob.clock);

    // vario
    {
        Vector3Val dir = Type()->_varioDirection[level];
        Matrix3 rot(MDirection, dir, VUp);
        Matrix3 invRot(MInverseRotation, rot);

        Vector3 up = InvTransform().DirectionUp();
        up[2] = -up[2];

        up = invRot * up;
        Matrix3 orient(MUpAndDirection, up, VForward);
        orient = rot * orient;

        Matrix4 trans;
        trans.SetPosition(VZero);
        trans.SetOrientation(orient);
        Type()->_vario.Apply(_shape, trans, level);
    }

    base::Animate(level);
}

void Airplane::Deanimate(int level)
{
    Type()->_lFlap.Restore(_shape, level);
    Type()->_rFlap.Restore(_shape, level);
    Type()->_lElevator.Restore(_shape, level);
    Type()->_rElevator.Restore(_shape, level);
    Type()->_lAileronT.Restore(_shape, level);
    Type()->_rAileronT.Restore(_shape, level);
    Type()->_lAileronB.Restore(_shape, level);
    Type()->_rAileronB.Restore(_shape, level);
    Type()->_lRudder.Restore(_shape, level);
    Type()->_rRudder.Restore(_shape, level);
    for (int i = 0; i < Type()->MaxRotors; i++)
    {
        Type()->_rotors[i].Restore(_shape, level);
    }

    Type()->_rotorStill.Unhide(_shape, level);
    Type()->_rotorMove.Unhide(_shape, level);

    Type()->_fWheel.Restore(_shape, level);
    Type()->_lWheel.Restore(_shape, level);
    Type()->_rWheel.Restore(_shape, level);
    base::Deanimate(level);
}

AirplaneAuto::AirplaneAuto(VehicleType* name, Person* pilot)
    : Airplane(name, pilot), _pilotHeading(0), _pilotSpeed(0), _pilotHeight(0), _defPilotHeight(100),

      _pilotAvoidHigh(TIME_MIN), _pilotAvoidHighHeight(0), _pilotAvoidLow(TIME_MIN), _pilotAvoidLowHeight(100000),

      _dirCompensate(0.5), _state(AutopilotNear), _pressedForward(false), _pressedBack(false), _pilotBank(0),
      _pilotDive(0), _pilotHelperHeight(true), _pilotHelperDir(true), _pilotHelperThrust(true),
      _pilotHelperBankDive(true), _forceDive(1), _pilotHeadingSet(false), _pilotDiveSet(false),

      _targetOutOfAim(false), _planeState(TaxiIn) // get ready for take-off
{
}

void AirplaneAuto::Simulate(float deltaT, SimulationImportance prec)
{
    SimulateUnits(deltaT);

    if (!_isDead)
    {
        if (PilotUnit() && PilotUnit()->GetLifeState() == AIUnit::LSAlive)
        {
            if (_planeState == Flight && _landContact && DirectionUp().Y() > 0.5)
            {
                if (!QIsManual())
                {
                    _planeState = TaxiOff;
                }
                else
                {
                    _planeState = Takeoff;
                }
            }

            float surfaceY = GLOB_LAND->SurfaceYAboveWater(Position().X(), Position().Z());

            {
                bool setControls = _pilotHelperBankDive;

                float aoa = CalculateAOA(ModelSpeed());
                float height = Position().Y() - surfaceY;

                Vector3 ilsPos, ilsDir;
                GWorld->GetILS(ilsPos, ilsDir, _targetSide);
                if (_pilotHelperThrust)
                {
#define EST_DELTA 2.0f
                    float relSpeed = ModelSpeed().Z();
                    float speedWanted = _pilotSpeed;
                    Vector3 accel = DirectionWorldToModel(_acceleration);
                    float changeAccel = (speedWanted - relSpeed) * (1 / EST_DELTA) - accel[2];

                    if (fabs(speedWanted) < 2)
                    {
                        _thrustWanted = 0;
                        _pilotBrake = 1;
                    }
                    else if (relSpeed - speedWanted > 5)
                    {
                        _thrustWanted = 0;
                        _pilotBrake = (relSpeed - speedWanted) * 0.1f;
                        saturate(_pilotBrake, 0, 1);
                    }
                    else
                    {
                        _thrustWanted = changeAccel * 0.1f + _thrust;
                        _pilotBrake = 0;
                    }
                }

                Matrix3Val orientation = Orientation();

                const float dirEstT = _dirCompensate;
                Matrix3Val derOrientation = _angVelocity.Tilda() * orientation;
                Matrix3Val estOrientation = orientation + derOrientation * dirEstT;
                Vector3Val estDirection = estOrientation.Direction().Normalized();

                float curHeading = atan2(estDirection[0], estDirection[2]);
                float changeHeading = AngleDifference(_pilotHeading, curHeading);

                const float estT = 2.0f;
                const int nSamples = 4;
                Vector3 step = _speed * 0.5f;

                Vector3 estPos = Position() + _speed * estT + _acceleration * estT * estT * 0.5;

                float estSurfY = GLOB_LAND->SurfaceYAboveWater(estPos.X(), estPos.Z());
                float maxSurfaceY = estSurfY;

                Vector3 tPos = Position();
                for (int s = nSamples; --s >= 0;)
                {
                    const float surfY = GLOB_LAND->SurfaceYAboveWater(tPos.X(), tPos.Z());
                    saturateMax(maxSurfaceY, surfY);
                    tPos += step;
                }

                float estHeight = estPos.Y() - maxSurfaceY;

                float minHeight = ModelSpeed()[2] * 0.5f + 10;
                saturateMin(minHeight, 20);
                saturateMax(_pilotHeight, minHeight);
                float diveWanted = 0;
                bool diveWantedSet = false;

                // landing speed: 200 km/h for 600 km/h max (A10)
                // landing speed: 100 km/h for 300 km/h max and less (Cessna)

                float landingSpeed = Type()->_landingSpeed;
                float stallSpeed = Type()->_stallSpeed;

                // The landing pattern was written for runways that run north-south (every
                // OFP/CWA island): its offsets were +X and its "off the centreline" tests
                // read the X components. Measure them across the runway instead. For a
                // north-south runway (ilsDir = 0,y,-1) runwaySide is +X, so those islands
                // behave exactly as before; an east-west runway (Sahrani's Paraiso) now works.
                Vector3 runwaySide(-ilsDir.Z(), 0, ilsDir.X());
                if (runwaySide.SquareSize() > 1e-6f)
                {
                    runwaySide.Normalize();
                }
                else
                {
                    runwaySide = Vector3(1, 0, 0);
                }
                const float dirAcrossRunway = Direction() * runwaySide;
                // The pattern heights (100 m, 92 m on final) are above the ground under the
                // plane. That only works when the runway sits near sea level: Paraiso is
                // 140 m up a cliff with its whole approach over the sea, so a plane 100 m
                // over the water was 200-280 m under the approach points it had to reach
                // within 200/500 m -- it circled them for ever -- and on final it would
                // have been below the runway. Hold at least that height above the runway
                // instead; over ground at runway level this is the same as before.
                const float runwayAboveGround = ilsPos.Y() - surfaceY;
                auto patternHeight = [runwayAboveGround](float h) { return floatMax(h, h + runwayAboveGround); };

                bool elevatorSet = false;
                if (_planeState == Marshall)
                {
                    Vector3 approachPos = ilsPos + ilsDir * 3000 + runwaySide * 650;
                    Vector3 relPos = approachPos - Position();
                    _pilotHeading = atan2(relPos.X(), relPos.Z());
                    changeHeading = AngleDifference(_pilotHeading, curHeading);
                    Limit(changeHeading, -0.7f, +0.7f);
                    _pilotHeight = patternHeight(100);
                    _pilotSpeed = landingSpeed * 2;
                    // horizontal distance: the height is the height controller's job
                    if (relPos.SquareSizeXZ() < Square(500))
                    {
                        _planeState = Approach;
                    }
                    setControls = true;
                    _rudderWanted = 0;
                }
                else if (_planeState == Approach)
                {
                    Vector3 approachPos = ilsPos + ilsDir * 2000 + runwaySide * 90;
                    Vector3 relPos = approachPos - Position();
                    _pilotHeading = atan2(relPos.X(), relPos.Z());
                    changeHeading = AngleDifference(_pilotHeading, curHeading);
                    Limit(changeHeading, -0.7f, +0.7f);
                    _pilotHeight = patternHeight(100);
                    _pilotSpeed = landingSpeed * 1.5f;
                    if (relPos.SquareSizeXZ() < Square(500))
                    {
                        _pilotSpeed = landingSpeed * 1.2f;
                    }
                    if (relPos.SquareSizeXZ() < Square(200))
                    {
                        _planeState = Final;
                    }
                    setControls = true;
                    _rudderWanted = 0;
                }
                else if (_planeState == Final)
                {
                    Vector3 finalPos = ilsPos + ilsDir * 1000;
                    Vector3 relPos = finalPos - Position();
                    _pilotHeading = atan2(relPos.X(), relPos.Z());
                    changeHeading = AngleDifference(_pilotHeading, curHeading);
                    Limit(changeHeading, -0.5f, +0.5f);
                    _pilotHeight = patternHeight(92);

                    _pilotSpeed = landingSpeed * 1.0f;
                    if (relPos.SquareSizeXZ() < Square(200))
                    {
                        if (ModelSpeed()[2] < landingSpeed * 1.5f && fabs(dirAcrossRunway) < H_PI / 4)
                        {
                            _planeState = Landing;
                        }
                        else
                        {
                            _planeState = Marshall;
                        }
                    }
                    setControls = true;
                    _rudderWanted = 0;
                }
                else if (_planeState == Landing || _planeState == Touchdown)
                {
                    _pilotHelperHeight = false;
                    _pilotHelperThrust = true;
                    _pilotHelperBankDive = true;
                    _pilotFlaps = 2;
                    _pilotGear = true;
                    _pilotSpeed = landingSpeed;
                    if (!_landContact)
                    {
                        float distance = (Position() - ilsPos) * ilsDir; //.Distance(ilsPos);
                        Vector3 ilsCPos = ilsPos + Vector3(0, 1.5f, 0) + ilsDir * distance;
                        Vector3 relPos = ilsPos - Position();
                        if (_planeState == Touchdown)
                        {
                            float y = ilsCPos[1];
                            ilsCPos = ilsPos - ilsDir * 500;
                            ilsCPos[1] = y;
                            relPos = ilsCPos - Position();
                        }
                        _pilotHeading = atan2(relPos.X(), relPos.Z());
                        changeHeading = AngleDifference(_pilotHeading, curHeading);
                        Limit(changeHeading, -0.2f, +0.2f);
                        float touchdownY = ilsPos[1];
                        float ilsY = ilsCPos.Y() - touchdownY;
                        saturateMin(ilsY, 100);
                        ilsY += touchdownY;
                        float above = Position().Y() - ilsY - 2.0f;
                        if (distance < 200)
                        {
                            if (Position().Y() - touchdownY > 40)
                            {
                                _planeState = WaveOff;
                                LOG_DEBUG(Physics, "WaveOff far: too high {:.1f}", Position().Y() - touchdownY);
                            }
                            else if (fabs(relPos * runwaySide > 25) || fabs(dirAcrossRunway) > 0.15f)
                            {
                                _planeState = WaveOff;
                                LOG_DEBUG(Physics, "WaveOff far: pos.x {:.1f}, dir.x {:.4f}", relPos * runwaySide,
                                          dirAcrossRunway);
                            }
                            else if (_planeState != Touchdown && Position().Y() - touchdownY < 15)
                            {
                                if (fabs(relPos * runwaySide < 13) && fabs(dirAcrossRunway) < 0.06)
                                {
                                    _planeState = Touchdown;
                                }
                                else
                                {
                                    _planeState = WaveOff;
                                    LOG_DEBUG(Physics, "WaveOff: pos.x {:.1f}, dir.x {:.4f}", relPos * runwaySide,
                                              dirAcrossRunway);
                                }
                            }
                        }
                        const float estT = 3.0f;
                        float estDescent = -_speed[1] - _acceleration[1] * estT;
                        const float landDescent = ilsDir[1] * _speed.SizeXZ();
                        float descent = landDescent + above * 0.5f;
                        float aoaWanted = Type()->_landingAoa;
                        _thrustWanted = _thrust + (aoa - aoaWanted) * 6.0f;
                        diveWanted = 6.5f * (H_PI / 180);
                        diveWantedSet = true;
                        float maxDescent = landDescent * 1.25f;
                        if (_planeState == Touchdown)
                        {
                            const float normDescent = ilsDir[1] * landingSpeed;
                            Limit(descent, normDescent * 0.25f, normDescent * 0.5f);
                            Limit(changeHeading, -0.05f, +0.05f);
                        }
                        else
                        {
                            Limit(descent, 0, maxDescent);
                        }
                        float dive = Direction().Y();
                        diveWanted = dive - (descent - estDescent) * 0.05f;
                        saturate(diveWanted, -0.2f, +0.15f);
#if _DEBUG
                        GlobalShowMessage(100, "dive %.2f->%.2f, descent %.2f, est %.2f", dive, diveWanted, descent,
                                          estDescent);
#endif
                        if (_planeState == Touchdown && Position().Y() - touchdownY < 4)
                        {
                            _thrustWanted = -1;
                        }
                        if (_thrustWanted < 0)
                        {
                            _pilotBrake = -_thrustWanted;
                        }
                        else
                        {
                            _pilotBrake = 0;
                        }
                    }
                    else
                    {
                        Vector3 ilsCPos = ilsPos - ilsDir * 700;
                        Vector3 relPos = ilsCPos - Position();
                        _pilotHeading = atan2(relPos.X(), relPos.Z());
                        changeHeading = AngleDifference(_pilotHeading, curHeading);
                        Limit(changeHeading, -0.2, +0.2);
                        _thrustWanted = 0;
                        _pilotSpeed = 0;
                        _pilotBrake = 1;
                        diveWanted = 0;
                        diveWantedSet = true;
                        if (fabs(ModelSpeed()[2] < 20))
                        {
                            _pilotFlaps = 0;
                            _planeState = TaxiOff;
                        }
                    }
                    setControls = true;
                    _rudderWanted = 0;
                }
                else if (_planeState == WaveOff)
                {
                    _pilotHeight = fabs(ModelSpeed()[2]) * 1.5 - 30;
                    saturateMax(_pilotHeight, 10);
                    _pilotSpeed = 50;
                    Vector3 ilsCPos = ilsPos - ilsDir * 700;
                    Vector3 relPos = ilsCPos - Position();
                    _pilotHeading = atan2(relPos.X(), relPos.Z());
                    changeHeading = AngleDifference(_pilotHeading, curHeading);
                    Limit(changeHeading, -0.5, +0.5);
                    if (relPos * ilsDir > 0)
                    {
                        _planeState = Marshall;
                    }
                    setControls = true;
                    _rudderWanted = 0;
                }
                else if (_planeState == TaxiOff)
                {
                    if (!_landContact && ModelSpeed()[2] > 60)
                    {
                        _planeState = Flight;
                    }
                    else
                    {
                        if (TaxiOffAutopilot())
                        {
                            EngineOff();
                            _planeState = TaxiIn;
                        }

                        changeHeading = AngleDifference(_pilotHeading, curHeading);
                        Limit(changeHeading, -0.4, +0.4);
                        _elevatorWanted = 0;
                        elevatorSet = true;
                    }
                    setControls = true;
                    _rudderWanted = 0;
                }
                else if (_planeState == TaxiIn)
                {
                    if (!_landContact && ModelSpeed()[2] > 60)
                    {
                        _planeState = Flight;
                    }
                    else
                    {
                        if (TaxiInAutopilot())
                        {
                            _planeState = Takeoff;
                        }

                        changeHeading = AngleDifference(_pilotHeading, curHeading);
                        Limit(changeHeading, -0.4, +0.4);
                        elevatorSet = true;
                    }
                    setControls = true;
                    _rudderWanted = 0;
                }

                saturateMin(_pilotSpeed, GetType()->GetMaxSpeedMs() * 1.5f);

                float bankWanted = 0;
                if (_pilotHelperDir)
                {
                    float bankFactor = 1;
                    if (ModelSpeed().Z() < landingSpeed * 2)
                    {
                        float slow = (landingSpeed * 2 - ModelSpeed().Z()) / landingSpeed;
                        // slow is 0 at zSpeed == landingSpeed*2
                        // slow is 1 at zSpeed == landingSpeed
                        saturate(slow, 0, 1);
                        bankFactor = 1 + slow;
                    }
                    bankWanted = -changeHeading * 4 * bankFactor;
                    if (!QIsManual() && _sweepTarget)
                    {
                        const float bankTurningWell = 0.5f;
                        if (fabs(bankWanted) < bankTurningWell)
                        {
                            float changeFactor = floatMin(10, fabs(changeHeading) * 300 + 1);
                            bankWanted *= changeFactor;
                            saturate(bankWanted, -bankTurningWell, +bankTurningWell);
                        }
                    }
                }
                else
                {
                    bankWanted = _pilotBank;
                }
                if (_pilotHelperHeight)
                {
                    if (ModelSpeed().Z() < stallSpeed * 2)
                    {
                        float fast = (ModelSpeed().Z() - stallSpeed) / stallSpeed;
                        saturate(fast, 0, 1);
                        float maxBank = fast * 0.5 + 0.4;
                        saturate(bankWanted, -maxBank, +maxBank);
                    }
                }
                saturate(bankWanted, -0.9, 0.9);

                float bank = estOrientation.DirectionAside().Y();
                float dive = estOrientation.Direction().Y();

                float maxAilerons = 1;

                if (_planeState == Takeoff)
                {
                    if (!_pilotHelperDir)
                    {
                        if (!_landContact)
                        {
                            if (!QIsManual() || height >= 2)
                            {
                                _planeState = Flight;
                            }
                        }
                    }
                    if (!QIsManual())
                    {
                        const float takeOffSpeed = Type()->_takeOffSpeed;
                        float actDive = Direction().Y();
                        if (ModelSpeed()[2] >= takeOffSpeed)
                        {
                            float diveWanted = 9 * H_PI / 180;
                            elevatorSet = true;
                            _elevatorWanted = (actDive - diveWanted) * 10;
                            if (!_landContact && height >= 10)
                            {
                                maxAilerons = 0.3f;
                            }
                            else
                            {
                                maxAilerons = 0;
                            }
                        }
                        else if (ModelSpeed()[2] >= takeOffSpeed * 0.66f)
                        {
                            float diveWanted = 0 * H_PI / 180;
                            if (ModelSpeed()[2] >= takeOffSpeed * 0.8f)
                            {
                                diveWanted = 7 * H_PI / 180;
                            }
                            elevatorSet = true;
                            _elevatorWanted = (actDive - diveWanted) * 10;
                            maxAilerons = 0;
                        }
                        else
                        {
                            elevatorSet = true;
                            _elevatorWanted = 0;
                        }
                    }

                    if (!_landContact && height >= 10)
                    {
                        _pilotGear = false;
                        if (!_landContact && height >= 30)
                        {
                            _planeState = Flight;
                            if (!QIsManual())
                            {
                                _pilotHeight = height + 5;
                                _pilotFlaps = 0;
                            }
                        }
                    }
                }

                float flapEff = ModelSpeed()[2] * (1.0 / 40);
                saturate(flapEff, 0.1, 1);
                float invFlapEff = 1 / flapEff;

                if (setControls)
                {
                    if (_pilotHelperDir)
                    {
                        _rudderWanted = -changeHeading * invFlapEff;
                    }
                    if (fabs(changeHeading) > H_PI / 4 && ModelSpeed().Z() < _pilotSpeed + 20)
                    {
                        _pilotBrake = 0;
                    }
                }

                if (setControls && !elevatorSet)
                {
                    if (!diveWantedSet)
                    {
                        if (fabs(_forceDive) < 0.99)
                        {
                            diveWanted = _forceDive;
                        }
                        else
                        {
                            if (_pilotHelperHeight)
                            {
                                float diveChange = _pilotHeight - estHeight;

                                float diveFactor = 0.5f;
                                if (ModelSpeed().Z() > 1)
                                {
                                    diveFactor /= ModelSpeed().Z();
                                }

                                diveWanted = dive + diveChange * diveFactor;

                                float limitDive = floatMin(fabs(bank) * 1.4 - 1, 0);
                                saturateMax(diveWanted, limitDive);

                                float zSpeed = ModelSpeed().Z();
                                if (zSpeed < landingSpeed * 1.15)
                                {
                                    _pilotFlaps = 2;
                                }
                                else if (zSpeed > landingSpeed * 1.2 && zSpeed < landingSpeed * 1.4)
                                {
                                    _pilotFlaps = 1;
                                }
                                else if (zSpeed > landingSpeed * 1.5)
                                {
                                    _pilotFlaps = 0;
                                    _pilotGear = false;
                                }

                                if (!_landContact && !_waterContact && !_objectContact)
                                {
                                    float stall = 0;
                                    const float maxNormalAoa = 5 * H_PI / 180;
                                    const float maxStallAoa = 30 * H_PI / 180;
                                    if (aoa > maxNormalAoa)
                                    {
                                        saturateMax(stall, (aoa - maxNormalAoa) * (1.0 / (maxStallAoa - maxNormalAoa)));
                                    }
                                    if (ModelSpeed().Z() < stallSpeed * 2)
                                    {
                                        saturateMax(stall, (stallSpeed * 2 - ModelSpeed().Z()) / stallSpeed);
                                    }
                                    saturate(stall, 0, 1);
                                    if (_planeState != Final)
                                    {
                                        saturateMax(_thrustWanted, stall);
                                    }
                                    float maxBank = 0.7f * (1 - stall) + 0.2f;
                                    if (GetType()->GetMaxSpeed() < 450 && !_sweepTarget)
                                    {
                                        saturateMin(maxBank, 0.7f);
                                    }
                                    float maxDive = 0.7f * (1 - stall);
                                    Limit(bankWanted, -maxBank, +maxBank);
                                    Limit(diveWanted, -0.7f, maxDive);

                                    if (stall > 0.3f)
                                    {
                                        _pilotBrake = 0;
                                    }
                                }
                            }
                            else
                            {
                                diveWanted = _pilotDive;
                            }
                        }
                        Limit(diveWanted, -0.7, +0.7);
                    }
                    float bankElevator = floatMax(0, fabs(bank) - 0.4);
                    float elevChange = (diveWanted - dive) * invFlapEff * 4;
                    if (!QIsManual() && _sweepTarget)
                    {
                        const float diveEnhancerMax = 0.1f;
                        if (fabs(elevChange) < diveEnhancerMax)
                        {
                            float factor = floatMin(10, fabs(elevChange) * 300 + 1);
                            elevChange *= factor;
                            saturate(elevChange, -diveEnhancerMax, +diveEnhancerMax);
                        }
                    }

                    _elevatorWanted = -elevChange - bankElevator;
                } // pilot helper full
                if (setControls)
                {
                    if (_pilotHelperHeight)
                    {
                        const float loSafeHeight = _defPilotHeight;
                        const float hiSafeHeight = _defPilotHeight + 100;
                        const float loSafeFactor = 10;
                        const float hiSafeFactor = 40;
                        float heightScale = loSafeFactor;
                        if (height > hiSafeHeight)
                        {
                            heightScale = hiSafeFactor;
                        }
                        else if (height < loSafeHeight)
                        {
                            heightScale = loSafeFactor;
                        }
                        else
                        {
                            float factor = (height - loSafeFactor) * 1 / (hiSafeFactor - loSafeFactor);
                            heightScale = factor * hiSafeFactor + loSafeFactor - factor * loSafeFactor;
                        }
                        float lowWarning = (_pilotHeight - height) / heightScale;
                        float heightSafe = 1 - lowWarning * 0.5f;
                        saturate(heightSafe, 0.1f, 1);
                        Limit(bankWanted, -0.9f * heightSafe, +0.9f * heightSafe);
                    }
                    _aileronWanted = +(bankWanted - bank) * 2;

                    if (_angVelocity.SquareSize() >= 10 * 10)
                    { // if we are spinning fast, leave all controls in neutral position
                        _rudderWanted = 0;
                        _aileronWanted = 0;
                        _elevatorWanted = 0;
                        _thrustWanted = 0.5;
                    }
                }
                Limit(_aileronWanted, -maxAilerons, +maxAilerons);

#if _ENABLE_CHEATS
                if (CHECK_DIAG(DECombat) && GLOB_WORLD->CameraOn() == this)
                {
                    const char* state = "???";
                    switch (_planeState)
                    {
                        case Flight:
                            state = "Flight";
                            break;
                        case Takeoff:
                            state = "Takeoff";
                            break;
                        case TaxiIn:
                            state = "TaxiIn";
                            break;
                        case Marshall:
                            state = "Marshall";
                            break;
                        case Approach:
                            state = "Approach";
                            break;
                        case Final:
                            state = "Final";
                            break;
                        case Landing:
                            state = "Landing";
                            break;
                        case Touchdown:
                            state = "Touchdown";
                            break;
                        case WaveOff:
                            state = "WaveOff";
                            break;
                        case TaxiOff:
                            state = "TaxiOff";
                            break;
                    }
                    /*
                    GlobalShowMessage
                    (
                        100,
                        "%s: AoA %.1f, v=%.0f, dive=%.2f->%.2f, thr %.2f->%.2f, bank %.2f->%.2f",
                        state,aoa*(180/H_PI),ModelSpeed()[2]*3.6,
                        dive,diveWanted,
                        _thrust,_thrustWanted,bank,bankWanted
                    );
                    */
                    GlobalShowMessage(100, "AoA %.1f, dive=%.2f->%.2f, elev %.2f->%.2f, thr %.2f, height %.2f",
                                      aoa * (180 / H_PI), dive, diveWanted, _elevator, _elevatorWanted, _thrust,
                                      _pilotHeight);
                }
#endif
            }
        }
        else
        {
            _rudderWanted = 0.1;
            _aileronWanted = 0.1;
            _elevatorWanted = 0;
            _thrustWanted = 0.3;
        }
    }

    saturate(_thrustWanted, 0, 1);

    MoveWeapons(deltaT);

    base::Simulate(deltaT, prec);
}

} // namespace Poseidon
