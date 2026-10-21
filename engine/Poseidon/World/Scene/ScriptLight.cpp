#include <Poseidon/World/Scene/ScriptLight.hpp>

#include <Poseidon/Graphics/Rendering/Lighting/Lights.hpp>
#include <Poseidon/World/Scene/Scene.hpp>

namespace Poseidon
{
DEFINE_CASTING(ScriptLight)

namespace
{
RefArray<ScriptLight>& Registry()
{
    static RefArray<ScriptLight> lights;
    return lights;
}
} // namespace

ScriptLight::ScriptLight(Vector3Par position)
    : base(nullptr, -1), _diffuse(1, 1, 1, 1), _ambient(0, 0, 0, 1), _brightness(0), _offset(VZero)
{
    _static = false;
    _destrType = DestructNo;
    SetPosition(position);
    Rebuild();
}

ScriptLight::~ScriptLight()
{
    SwitchOff();
}

void ScriptLight::SwitchOff()
{
    if (_light)
    {
        _light->Switch(false);
        // The scene keeps only a Link; dropping the last Ref removes the light from it, and the
        // AttachedOnVehicle destructor takes it out of World::_attached.
        _light.Free();
    }
}

void ScriptLight::Rebuild()
{
    SwitchOff();
    Object* carrier = _attachedTo ? _attachedTo.GetLink() : this;
    const Vector3 offset = _attachedTo ? _offset : VZero;
    _light = new LightPointOnVehicle(carrier, offset);
    _light->SetDiffuse(_diffuse);
    _light->SetAmbient(_ambient);
    _light->SetBrightness(_brightness > 0 ? _brightness : 0);
    _light->Switch(_brightness > 0);
    _light->UpdatePosition();
    if (GLOB_SCENE)
    {
        GLOB_SCENE->AddLight(_light);
    }
}

void ScriptLight::SetColor(ColorVal color)
{
    _diffuse = color;
    if (_light)
    {
        _light->SetDiffuse(color);
    }
}

void ScriptLight::SetAmbient(ColorVal color)
{
    _ambient = color;
    if (_light)
    {
        _light->SetAmbient(color);
    }
}

void ScriptLight::SetBrightness(float brightness)
{
    _brightness = brightness;
    if (!_light)
    {
        Rebuild();
        return;
    }
    _light->SetBrightness(brightness > 0 ? brightness : 0);
    _light->Switch(brightness > 0);
}

void ScriptLight::AttachTo(Object* object, Vector3Par modelOffset)
{
    if (!object || object == this)
    {
        Detach();
        return;
    }
    _attachedTo = object;
    _offset = modelOffset;
    Rebuild();
}

void ScriptLight::Detach()
{
    if (_light)
    {
        // stay where the light is now
        SetPosition(_light->Transform().Position());
    }
    _attachedTo = nullptr;
    _offset = VZero;
    Rebuild();
}

void ScriptLight::MoveTo(Vector3Par position)
{
    SetPosition(position);
    if (!_attachedTo && _light)
    {
        _light->UpdatePosition();
    }
}

ScriptLight* CreateScriptLight(Vector3Par position)
{
    Ref<ScriptLight> light = new ScriptLight(position);
    Registry().Add(light);
    return light;
}

bool DeleteScriptLight(Object* obj)
{
    ScriptLight* light = dyn_cast<ScriptLight>(obj);
    if (!light)
    {
        return false;
    }
    light->SwitchOff();
    RefArray<ScriptLight>& lights = Registry();
    for (int i = 0; i < lights.Size(); i++)
    {
        if (lights[i] == light)
        {
            lights.Delete(i);
            break;
        }
    }
    return true;
}

void ClearScriptLights()
{
    RefArray<ScriptLight>& lights = Registry();
    for (int i = 0; i < lights.Size(); i++)
    {
        if (lights[i])
        {
            lights[i]->SwitchOff();
        }
    }
    lights.Clear();
}

int NScriptLights()
{
    return Registry().Size();
}
} // namespace Poseidon
