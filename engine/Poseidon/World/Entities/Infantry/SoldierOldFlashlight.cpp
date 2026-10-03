// Sinkhole W1b: a handheld flashlight for soldiers (test item).
//
// A spot light held in the left hand -- the bone binoculars use ('lruka') -- and aimed where the soldier
// looks. It is the soldier's "pilot light": the existing headlights action (UAHeadlights, which
// EntityAI::SimulateWeaponActivity already toggles for whoever commands the entity, a man on foot
// included) switches the player's, and the "Light on/off" actions work as for a vehicle. AI soldiers
// switch theirs on underground (a cave or cellar: under the terrain inside a terrain hole) and, if
// POSEIDON_FLASHLIGHT_AI_NIGHT=1, at night when not cautious -- the rule AI drivers use for headlights.
// Night use by AI is opt-in so legacy night missions do not change.
//
// Off switch: POSEIDON_FLASHLIGHT=0. The hold pose (off-hand animation) is a later step; until then the
// light rides the left hand of whatever animation is playing.

#include <Poseidon/World/Entities/Infantry/SoldierOld.hpp>

#include <Poseidon/AI/AIUnit.hpp>
#include <Poseidon/Graphics/Rendering/Lighting/Lights.hpp>
#include <Poseidon/World/Scene/Scene.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/World/World.hpp>

#include <cstdlib>

namespace Poseidon
{
namespace
{
int EnvInt(const char* name, int fallback)
{
    const char* v = std::getenv(name);
    return (v && *v) ? std::atoi(v) : fallback;
}

bool FlashlightsEnabled()
{
    static const bool on = EnvInt("POSEIDON_FLASHLIGHT", 1) != 0;
    return on;
}

bool AiUsesFlashlightAtNight()
{
    static const bool on = EnvInt("POSEIDON_FLASHLIGHT_AI_NIGHT", 0) != 0;
    return on;
}

// A small hand torch: warm white, ~20 m reach, a 25 degree half-angle cone.
constexpr float kFlashlightRange = 20.0f;
constexpr float kFlashlightCone = 0.45f;
// The player's beam is aimed at the point this far down his view ray.
constexpr float kFlashlightAimDistance = 12.0f;
const Color kFlashlightColor(1.00f, 0.93f, 0.80f, 1.0f);
} // namespace

bool Man::LeftHandFrame(Matrix4& modelSpace) const
{
    const ManType* type = Type();
    if (!type || type->_handMatIndex < 0)
    {
        return false;
    }
    const int level = _shape->FindMemoryLevel();
    if (level < 0)
    {
        return false;
    }
    Shape* memory = _shape->LevelOpaque(level);
    if (!memory)
    {
        return false;
    }
    const AnimationRTWeights& wgt = type->GetWeights()[level];
    for (int i = 0; i < memory->NProxies(); i++)
    {
        const ProxyObject& proxy = memory->Proxy(i);
        const NamedSelection& sel = memory->NamedSel(proxy.selection);
        if (sel.Size() <= 0)
        {
            continue;
        }
        const AnimationRTWeight& wg = wgt[sel[0]];
        if (wg.Size() <= 0 || wg[0].GetSel() != type->_handMatIndex)
        {
            continue;
        }
        modelSpace = AnimateProxyMatrix(level, proxy);
        modelSpace.Orthogonalize();
        return true;
    }
    return false;
}

void Man::UpdateFlashlight()
{
    if (!FlashlightsEnabled() || !GScene)
    {
        return;
    }

    AIUnit* brain = Brain();
    if (IsDead() || !brain)
    {
        _pilotLight = false;
    }
    else if (!brain->IsPlayer())
    {
        // AI: on in the dark. Underground = more than 1 m under the terrain inside a terrain hole.
        const Vector3 head = AimingPosition();
        const bool underground = GLandscape && GLandscape->InTerrainHoleBelow(head, 1.0f);
        const bool night = AiUsesFlashlightAtNight() && GScene->MainLight()->NightEffect() > 0.5f &&
                           !IsCautiousOrDanger();
        _pilotLight = underground || night;
    }
    // the player's _pilotLight is toggled by the headlights action (EntityAI::SimulateWeaponActivity)

    if (!_pilotLight)
    {
        if (_flashlight)
        {
            _flashlight->Switch(false);
        }
        _flashlightLogged = false;
        return;
    }

    // Model space: the left hand when the skeleton has one, else beside the chest; the beam follows the eyes.
    Matrix4 hand;
    Vector3 pos;
    if (LeftHandFrame(hand))
    {
        pos = hand.Position();
    }
    else
    {
        pos = PositionWorldToModel(AimingPosition()) + Vector3(-0.25f, -0.25f, 0.25f);
    }
    // Aim. When the player's own camera is on him (first or third person), the beam converges on what
    // he looks at: from the torch to the point 12 m down the view ray. Following the eye direction
    // (head and gun pose) pointed it into the floor beside his feet with the weapon lowered, and a beam
    // merely PARALLEL to a slightly pitched first-person view, held at the hip, landed on the ground
    // two metres ahead (owner: "the light drags on the ground, can the beam be direct?"). AI and
    // scripted cameras keep the eye direction, which is where an AI is looking.
    Vector3 eye = GetEyeDirection();
    if (brain && brain->IsPlayer() && GWorld && GWorld->CameraOn() == this && GScene->GetCamera())
    {
        const Camera* cam = GScene->GetCamera();
        const Vector3 target = cam->Position() + cam->Direction() * kFlashlightAimDistance;
        const Vector3 torch = PositionModelToWorld(pos);
        const Vector3 toTarget = target - torch;
        eye = toTarget.SquareSize() > 1e-4f ? toTarget.Normalized() : cam->Direction();
    }
    Vector3 dir = DirectionWorldToModel(eye);
    if (dir.SquareSize() < 1e-6f)
    {
        dir = Vector3(0, 0, 1);
    }
    dir.Normalize();
    pos += dir * 0.12f; // the lens, a little ahead of the grip

    if (!_flashlight)
    {
        // No ambient term: a local light's ambient lights every direction with the same inverse-square
        // falloff as the beam, so even 3% of the colour, 0.1 m from the bulb, turned the holder's own
        // body glowing white (owner's capture: an AI with his torch on, lit up like a lamp). A torch
        // lights only where it points.
        _flashlight = new LightReflectorOnVehicle(GScene->Preloaded(HalfLight), kFlashlightColor,
                                                  Color(0, 0, 0, 1), this, pos, dir, kFlashlightCone);
        _flashlight->SetRange(kFlashlightRange);
        // Local lights are scaled by the night factor unless they opt out (PresentationSnapshot), so a torch
        // at noon -- in a cave -- would emit nothing. A flashlight shines whatever the time of day.
        _flashlight->SetDaylightVisible(true);
        GScene->AddLight(_flashlight);
    }
    _flashlight->SetAttachedPos(pos, dir);
    _flashlight->Switch(true);
    // One line each time a torch turns on (and its first frame's placement), for captures.
    if (!_flashlightLogged)
    {
        _flashlightLogged = true;
        const Vector3 w = PositionModelToWorld(pos);
        LOG_INFO(World, "Flashlight on: {} player={} hand={} world=({:.2f},{:.2f},{:.2f}) dir=({:.2f},{:.2f},{:.2f})",
                 (const char*)GetDebugName(), brain && brain->IsPlayer() ? 1 : 0, LeftHandFrame(hand) ? 1 : 0, w.X(),
                 w.Y(), w.Z(), dir.X(), dir.Y(), dir.Z());
    }
}
} // namespace Poseidon
