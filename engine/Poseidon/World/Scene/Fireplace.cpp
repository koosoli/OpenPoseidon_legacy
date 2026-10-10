#include <Poseidon/World/Scene/Fireplace.hpp>
#include <Poseidon/World/Scene/Scene.hpp>
#include <Poseidon/World/Effects/FireVolumes.hpp>
#include <Poseidon/World/Effects/SmokeWorldQuery.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Random/randomGen.hpp>
#include <Poseidon/AI/AI.hpp>
#include <Poseidon/Game/UiActions.hpp>
#include <Poseidon/UI/Locale/StringtableExt.hpp>
#include <Poseidon/Network/Network.hpp>
#include <Poseidon/Foundation/Common/FltOpts.hpp>
#include <Poseidon/Foundation/Framework/DebugLog.hpp>
#include <Poseidon/Foundation/Math/Math3D.hpp>
#include <Poseidon/Foundation/Math/Math3DP.hpp>
#include <Poseidon/Foundation/Strings/RString.hpp>
#include <Poseidon/Foundation/platform.hpp>

namespace Poseidon
{
DEFINE_CASTING(Fireplace)

Fireplace::Fireplace(VehicleType* type) : VehicleWithAI(type)
{
    _burning = false;

    const ParamEntry& cls = *type->_par;
    _smoke.Load(cls >> "Smoke");

    _light = new LightPointOnVehicle(this, VZero);
    _light->Switch(_burning);
    _light->Load(cls >> "Light");
    _lightColor = _light->GetDiffuse();
    GLOB_SCENE->AddLight(_light);

    RString sound = cls >> "sound";
    _sound = new SoundObject(sound, this, true);
}

void Fireplace::Simulate(float deltaT, SimulationImportance prec)
{
    if (_burning)
    {
        Vector3 position = Position();
        float fade = GRandGen.PlusMinus(0.8, 0.2);
        if (_light)
        {
            _light->Switch(true);
            _light->SetDiffuse(_lightColor * fade);
            position = _light->Transform().Position();
        }
        _smoke.Simulate(position, Speed(), deltaT, prec);
        // FIRE-001: the bed on the ground under the fireplace, for the volumetric fire
        {
            // the floor it stands on: terrain, or the storey of the house it is in
            const Vector3 p = Position();
            const float ground = GSmokeWorldQuery().FloorHeightBelow(p + Vector3(0, 0.5f, 0));
            ReportFireVolume(this, Vector3(p.X(), ground, p.Z()), 0.55f, 1.0f, 0.12f, FireKindBed);
        }
        if (_sound)
        {
            _sound->Pause(false);
            _sound->Simulate(deltaT, prec);
        }
    }
    else
    {
        if (_light)
        {
            _light->Switch(false);
        }
        if (_sound)
        {
            _sound->Pause();
            _sound->Simulate(deltaT, prec);
        }
    }

    base::Simulate(deltaT, prec);
}

bool Fireplace::IsAnimated(int level) const
{
    return Object::IsAnimated(level);
}
bool Fireplace::IsAnimatedShadow(int level) const
{
    return Object::IsAnimatedShadow(level);
}
void Fireplace::Animate(int level)
{
    Object::Animate(level);
}
void Fireplace::Deanimate(int level)
{
    Object::Deanimate(level);
}

void Fireplace::Sound(bool inside, float deltaT) {}

void Fireplace::UnloadSound() {}

LSError Fireplace::Serialize(ParamArchive& ar)
{
    PARAM_CHECK(base::Serialize(ar))
    PARAM_CHECK(ar.Serialize("burning", _burning, 1, false))
    return LSOK;
}

NetworkMessageType Fireplace::GetNMType(NetworkMessageClass cls) const
{
    switch (cls)
    {
        case NMCUpdateGeneric:
            return NMTUpdateFireplace;
        default:
            return base::GetNMType(cls);
    }
}

class IndicesUpdateFireplace : public IndicesUpdateVehicleAI
{
    typedef IndicesUpdateVehicleAI base;

  public:
    int burning;

    IndicesUpdateFireplace();
    NetworkMessageIndices* Clone() const override { return new IndicesUpdateFireplace; }
    void Scan(NetworkMessageFormatBase* format) override;
};

IndicesUpdateFireplace::IndicesUpdateFireplace()
{
    burning = -1;
}

void IndicesUpdateFireplace::Scan(NetworkMessageFormatBase* format)
{
    base::Scan(format);

    SCAN(burning)
}

} // namespace Poseidon
NetworkMessageIndices* GetIndicesUpdateFireplace()
{
    using namespace Poseidon;
    return new IndicesUpdateFireplace();
}
namespace Poseidon
{

NetworkMessageFormat& Fireplace::CreateFormat(NetworkMessageClass cls, NetworkMessageFormat& format)
{
    switch (cls)
    {
        case NMCUpdateGeneric:
            base::CreateFormat(cls, format);

            format.Add("burning", NDTBool, NCTNone, DEFVALUE(bool, false), DOC_MSG("Fire is burning"), ET_NOT_EQUAL,
                       ERR_COEF_MODE);

            break;
        default:
            base::CreateFormat(cls, format);
            break;
    }
    return format;
}

TMError Fireplace::TransferMsg(NetworkMessageContext& ctx)
{
    switch (ctx.GetClass())
    {
        case NMCUpdateGeneric:
            TMCHECK(base::TransferMsg(ctx))
            {
                PoseidonAssert(dynamic_cast<const IndicesUpdateFireplace*>(ctx.GetIndices()))
                    const IndicesUpdateFireplace* indices =
                        static_cast<const IndicesUpdateFireplace*>(ctx.GetIndices());

                ITRANSF(burning)
            }
            break;
        default:
            return base::TransferMsg(ctx);
    }
    return TMOK;
}

float Fireplace::CalculateError(NetworkMessageContext& ctx)
{
    float error = 0;
    switch (ctx.GetClass())
    {
        case NMCUpdateGeneric:
            error += base::CalculateError(ctx);
            {
                PoseidonAssert(dynamic_cast<const IndicesUpdateFireplace*>(ctx.GetIndices()))
                    const IndicesUpdateFireplace* indices =
                        static_cast<const IndicesUpdateFireplace*>(ctx.GetIndices());

                ICALCERR_NEQ(bool, burning, ERR_COEF_MODE)
            }
            break;
        default:
            error += base::CalculateError(ctx);
            break;
    }
    DoAssert(_finite(error));
    return error;
}

void Fireplace::GetActions(UIActions& actions, AIUnit* unit, bool now)
{
    base::GetActions(actions, unit, now);
    if (!unit)
    {
        return;
    }

    if (unit->IsFreeSoldier())
    {
        Person* person = unit->GetPerson();
        if (person->CheckActionProcessing(ATFireInflame, unit))
        {
            return;
        }
        if (person->CheckActionProcessing(ATFirePutDown, unit))
        {
            return;
        }
        if (now)
        {
            float dist2 = Position().Distance2(person->Position());
            if (dist2 > Square(5))
            {
                return;
            }

            Vector3Val relPos = person->PositionWorldToModel(Position());
            if (relPos.Z() <= 0)
            {
                return;
            }
        }
        if (_burning)
        {
            actions.Add(ATFirePutDown, this, 0.99, 0, true, true);
        }
        else
        {
            actions.Add(ATFireInflame, this, 0.99, 0, true, true);
        }
    }
}

RString Fireplace::GetActionName(const UIAction& action)
{
    switch (action.type)
    {
        case ATFireInflame:
            return LocalizeString(IDS_ACTION_FIRE_INFLAME);
        case ATFirePutDown:
            return LocalizeString(IDS_ACTION_FIRE_PUT_DOWN);
    }
    return base::GetActionName(action);
}

void Fireplace::PerformAction(const UIAction& action, AIUnit* unit)
{
    switch (action.type)
    {
        case ATFireInflame:
            if (IsLocal())
            {
                Inflame(true);
            }
            else
            {
                GetNetworkManager().AskForInflameFire(this, true);
            }
            return;
        case ATFirePutDown:
            if (IsLocal())
            {
                Inflame(false);
            }
            else
            {
                GetNetworkManager().AskForInflameFire(this, false);
            }
            return;
    }
    base::PerformAction(action, unit);
}

} // namespace Poseidon
