#include <Poseidon/World/Scene/ScriptLight.hpp>

#include <Poseidon/Graphics/Rendering/Lighting/Lights.hpp>
#include <Poseidon/World/Scene/Scene.hpp>

#include <cmath>

namespace Poseidon
{
DEFINE_CASTING(ScriptLight)

namespace
{
// Deliberately never destroyed: a static RefArray would release the lights during static
// destruction at process exit, after the world is gone, and ~AttachedOnVehicle unregisters
// through GLOB_WORLD. Mission ends empty it (ClearScriptLights); at exit the OS takes it.
RefArray<ScriptLight>& Registry()
{
    static RefArray<ScriptLight>* lights = new RefArray<ScriptLight>;
    return *lights;
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

// Arma 3 -> LightPoint. LightPoint has a flat core of 50 m * brightness and a tail cut at
// 10x the core, so a hard limit of R metres is brightness R/500. Without a limit the reach
// follows the intensity (sqrt, so 3000 -> ~0.033: a few metres of core, like A3's default
// lamps). The intensity also scales the colour, gently, around A3's common 3000.
// An approximation: A3's physically based curve has no exact counterpart here.
float ScriptLight::EffectiveBrightness() const
{
    if (_a3Intensity < 0)
    {
        return _brightness > 0 ? _brightness : 0;
    }
    if (_a3Intensity <= 0)
    {
        return 0;
    }
    return _a3Reach > 0 ? _a3Reach / 500.0f : 0.0006f * sqrtf(_a3Intensity);
}

Color ScriptLight::EffectiveDiffuse() const
{
    if (_a3Intensity <= 0)
    {
        return _diffuse;
    }
    float scale = sqrtf(_a3Intensity / 3000.0f);
    scale = scale < 0.25f ? 0.25f : (scale > 2.0f ? 2.0f : scale);
    return _diffuse * scale;
}

void ScriptLight::Apply()
{
    if (!_light)
    {
        return;
    }
    const float b = EffectiveBrightness();
    _light->SetDiffuse(EffectiveDiffuse());
    _light->SetAmbient(_ambient);
    _light->SetBrightness(b);
    _light->Switch(b > 0);
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
    Apply();
    _light->UpdatePosition();
    if (GLOB_SCENE)
    {
        GLOB_SCENE->AddLight(_light);
    }
}

void ScriptLight::SetColor(ColorVal color)
{
    _diffuse = color;
    Apply();
}

void ScriptLight::SetAmbient(ColorVal color)
{
    _ambient = color;
    Apply();
}

void ScriptLight::SetBrightness(float brightness)
{
    _brightness = brightness;
    _a3Intensity = -1; // an explicit Arma 1/2 brightness wins over an earlier A3 intensity
    Apply();
}

void ScriptLight::SetIntensity(float intensity)
{
    _a3Intensity = intensity > 0 ? intensity : 0;
    Apply();
}

void ScriptLight::SetAttenuationLimit(float hardLimitEnd)
{
    _a3Reach = hardLimitEnd > 0 ? hardLimitEnd : -1;
    Apply();
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
    ScriptLight* light = obj ? dyn_cast<ScriptLight>(obj) : nullptr;
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
