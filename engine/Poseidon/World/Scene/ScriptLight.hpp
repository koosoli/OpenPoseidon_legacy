// LGT-SCRIPT-001: lights a mission script creates with "#lightpoint" createVehicle.
//
// Arma 1 introduced them and every later title kept the same five commands:
//     _l = "#lightpoint" createVehicle _pos;      // or createVehicleLocal
//     _l setLightColor [r,g,b];  _l setLightAmbient [r,g,b];  _l setLightBrightness b;
//     _l lightAttachObject [object, [x,y,z]];      lightDetachObject _l;   deleteVehicle _l;
// Until now they came back as objNull and their setters were ignored, so neither Arma
// missions nor CWA addons could light anything from a script (a vehicle's own lamps are
// limited to the config Reflectors, which only follow the headlight switch).
//
// The handle is a shapeless Object that never enters the world's vehicle lists: it is only
// something a script can hold, compare and delete. The light itself is the engine's existing
// LightPointOnVehicle (the fireplace and marker lights), so it is drawn by the same local-light
// path and follows the object it is attached to through World::_attached, every frame.
//
// One extension over Arma: lightAttachObject also accepts a memory-point NAME in place of the
// offset, [object, "point"], so an addon can put a lamp exactly where its model says the bulb is
// without knowing the model's bounding-centre offset.
#pragma once

#include <Poseidon/World/Scene/Object.hpp>

namespace Poseidon
{
class LightPointOnVehicle;

class ScriptLight : public Object
{
    typedef Object base;

    Ref<LightPointOnVehicle> _light;
    Color _diffuse;
    Color _ambient;
    float _brightness;
    OLink<Object> _attachedTo; //!< null while free-standing
    Vector3 _offset;           //!< model-space offset on _attachedTo

    void Rebuild(); //!< (re)creates the engine light for the current attachment and settings

  public:
    explicit ScriptLight(Vector3Par position);
    ~ScriptLight() override;

    void SetColor(ColorVal color);
    void SetAmbient(ColorVal color);
    //! Arma semantics: the light's reach scales with it (LightPoint core = 50 m * brightness).
    //! 0 or less switches the light off without deleting it.
    void SetBrightness(float brightness);
    void AttachTo(Object* object, Vector3Par modelOffset);
    void Detach();
    void MoveTo(Vector3Par position);
    void SwitchOff();

    Object* AttachedTo() const { return _attachedTo; }

    USE_CASTING(base)
};

//! Creates a light and keeps it alive until deleteVehicle or the end of the mission.
ScriptLight* CreateScriptLight(Vector3Par position);
//! deleteVehicle on a script light. Returns false if obj is not one.
bool DeleteScriptLight(Object* obj);
//! Mission end (World::CleanUpDeinit): every script light goes.
void ClearScriptLights();
int NScriptLights();
} // namespace Poseidon
