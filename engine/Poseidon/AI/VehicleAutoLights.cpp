// LGT-AUTO-001: tail and brake lamps of OFP / CWA vehicles cast light.
//
// The original engine only shows or hides the glowing faces of the "zadni svetlo" (tail) and
// "brzdove svetlo" (brake) selections; nothing they would light up around the vehicle is lit.
// Forward light was always cast, through the config Reflectors. This adds a small red point
// light at each tail lamp while the vehicle's lights are on, and at each brake lamp while the
// brake glow shows -- driven by exactly the same conditions as those faces, so an unmodified
// model behaves as before and only gains the light it would have cast.
//
// Nothing is changed when:
//  - the model has no such selection in LOD 0 (most non-cars),
//  - the model already casts light backwards through its own Reflectors (two reflectors facing
//    opposite ways): its author chose that light, the tail part is left out then,
//  - the class says opAutoLights = 0 (CfgVehicles), or POSEIDON_AUTO_VEHICLE_LIGHTS=0.
// Faces of a tail selection that sit at the FRONT (some addons put the headlight glow there,
// since the engine shows both with the lights) get no point light: the headlights already cast.
// Marker lights (class MarkerLights: aircraft, ships, tanks) are untouched; they always were
// point lights (TransportCore).
#include <Poseidon/AI/VehicleAI.hpp>
#include <Poseidon/Graphics/Rendering/Lighting/Lights.hpp>
#include <Poseidon/World/Scene/Scene.hpp>
#include <Poseidon/World/World.hpp>

#include <cstdlib>

namespace Poseidon
{
namespace
{
bool AutoLightsEnabled()
{
    static const bool on = []
    {
        const char* v = std::getenv("POSEIDON_AUTO_VEHICLE_LIGHTS");
        return !(v && *v == '0');
    }();
    return on;
}

// Lamp centres of one selection in LOD 0: its points split left/right and front/back of the
// model centre, one centre per non-empty quarter. `forward` is the model's forward direction;
// with rearOnly the front quarters are dropped.
void CollectLamps(LODShape* shape, const char* selName, Vector3Par forward, bool rearOnly,
                  AutoArray<Vector3>& out)
{
    out.Clear();
    if (!shape || shape->NLevels() <= 0)
    {
        return;
    }
    Shape* level = shape->Level(0);
    if (!level)
    {
        return;
    }
    const int si = level->FindNamedSel(selName);
    if (si < 0)
    {
        return;
    }
    const NamedSelection& sel = level->NamedSel(si);
    Vector3 sum[4] = {VZero, VZero, VZero, VZero};
    int count[4] = {0, 0, 0, 0};
    for (int k = 0; k < sel.Size(); k++)
    {
        const Vector3 p = level->Pos(sel[k]);
        const int q = (p.X() < 0 ? 0 : 1) + (p.DotProduct(forward) > 0 ? 2 : 0);
        sum[q] += p;
        count[q]++;
    }
    for (int q = 0; q < 4; q++)
    {
        if (count[q] == 0)
        {
            continue;
        }
        const bool front = q >= 2;
        if (rearOnly && front)
        {
            continue;
        }
        Vector3 c = sum[q] * (1.0f / count[q]);
        // a hand's breadth outside the lens, so the light falls on the road and not into the body
        c += (front ? forward : -forward) * 0.12f;
        out.Add(c);
    }
}

void SwitchLamps(EntityAI* owner, RefArray<LightPointOnVehicle>& lights, const AutoArray<Vector3>& pos, bool on,
                 ColorVal color, ColorVal ambient, float brightness)
{
    if (lights.Size() == 0)
    {
        if (!on || pos.Size() == 0)
        {
            return; // created only once a lamp first lights up
        }
        for (int i = 0; i < pos.Size(); i++)
        {
            LightPointOnVehicle* light = new LightPointOnVehicle(owner, pos[i]);
            light->SetDiffuse(color);
            light->SetAmbient(ambient);
            light->SetBrightness(brightness);
            light->Switch(false);
            lights.Add(light);
            if (GLOB_SCENE)
            {
                GLOB_SCENE->AddLight(light);
            }
        }
    }
    for (int i = 0; i < lights.Size(); i++)
    {
        if (lights[i])
        {
            lights[i]->Switch(on);
        }
    }
}
} // namespace

void EntityAIType::PrepareAutoLights() const
{
    _autoLightsReady = true;
    _autoTailCasts = false;
    _autoTailPos.Clear();
    _autoBrakePos.Clear();
    if (!AutoLightsEnabled() || !_shape)
    {
        return;
    }
    if (_par && _par->FindEntry("opAutoLights") && static_cast<int>((*_par) >> "opAutoLights") == 0)
    {
        return;
    }
    // forward: the mean of the Reflectors that face forward; without reflectors the model's +Z
    Vector3 forward = VZero;
    bool rearReflector = false;
    for (int i = 0; i < _reflectors.Size(); i++)
    {
        for (int j = i + 1; j < _reflectors.Size(); j++)
        {
            if (_reflectors[i].direction.DotProduct(_reflectors[j].direction) < -0.3f)
            {
                rearReflector = true;
            }
        }
    }
    if (_reflectors.Size() > 0)
    {
        // with opposite reflectors the first one (the classic headlight) decides the forward side
        const Vector3 first = _reflectors[0].direction;
        for (int i = 0; i < _reflectors.Size(); i++)
        {
            if (_reflectors[i].direction.DotProduct(first) > 0)
            {
                forward += _reflectors[i].direction;
            }
        }
    }
    if (forward.SquareSize() < 1e-6f)
    {
        forward = VForward;
    }
    forward.Normalize();
    if (!rearReflector)
    {
        CollectLamps(_shape, "zadni svetlo", forward, true, _autoTailPos);
        _autoTailCasts = _autoTailPos.Size() > 0;
    }
    CollectLamps(_shape, "brzdove svetlo", forward, true, _autoBrakePos);
    LOG_INFO(World, "LGT-AUTO-001 {}: tail lamps {}{}, brake lamps {}", (const char*)GetName(), _autoTailPos.Size(),
             rearReflector ? " (own rear reflector, not added)" : "", _autoBrakePos.Size());
}

void EntityAI::UpdateAutoVehicleLights()
{
    const EntityAIType* type = GetType();
    if (!type)
    {
        return;
    }
    if (!type->_autoLightsReady)
    {
        type->PrepareAutoLights();
    }
    if (!type->_autoTailCasts && type->_autoBrakePos.Size() == 0)
    {
        return;
    }
    const bool intact = GetTotalDammage() < 0.9f;
    // tail: same switch as the "zadni svetlo" faces (EntityAI::Animate, _pilotLight)
    SwitchLamps(this, _autoTailLights, type->_autoTailPos, type->_autoTailCasts && intact && _pilotLight,
                Color(1.0f, 0.05f, 0.02f), Color(0.03f, 0.0f, 0.0f), 0.010f);
    // brake: same condition as the "brzdove svetlo" faces (TankOrCar::Animate)
    SwitchLamps(this, _autoBrakeLights, type->_autoBrakePos, intact && AutoBrakeLightsOn(),
                Color(1.0f, 0.04f, 0.02f), Color(0.05f, 0.0f, 0.0f), 0.025f);
}
} // namespace Poseidon
