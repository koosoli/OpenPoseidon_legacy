
#include <Poseidon/World/Entities/Weapons/Recoil.hpp>
#include <Poseidon/IO/ParamFileExt.hpp>
#include <cmath>
#include <Poseidon/Foundation/Framework/DebugLog.hpp>
#include <Poseidon/Foundation/Strings/RString.hpp>

namespace Poseidon
{
void RecoilFunction::AddItem(float time, float offset, float angle)
{
    // get last item time
    if (_queue.Size() > 0)
    {
        time += _queue[_queue.Size() - 1]._time;
    }
    RecoilItem& item = _queue.Append();
    Matrix4 mat;
    mat.SetRotationX(-angle);
    mat.SetPosition(Vector3(0, 0, -offset));
    item._time = time;
    item._mat = mat;
}

RecoilFunction::RecoilFunction() = default;

RecoilFunction::RecoilFunction(RStringB name) : _name(name)
{
    const ParamEntry* cfgRecoils = Pars.FindEntryNoInheritance("CfgRecoils");
    if (!cfgRecoils)
    {
        return;
    }

    const ParamEntry* entry = cfgRecoils->FindEntryNoInheritance(name);
    if (!entry || entry->GetSize() <= 0)
    {
        return;
    }
    RecoilItem item;
    item._time = 0;
    item._mat = MIdentity;
    _queue.Add(item);

    for (int i = 0; i < entry->GetSize() - 2; i += 3)
    {
        float itemTime = (*entry)[i];
        float itemMoveZ = (*entry)[i + 1];
        float itemRotX = (*entry)[i + 2];

        AddItem(itemTime, itemMoveZ, itemRotX);
    }
}

RecoilFunction::~RecoilFunction() = default;
const RStringB& RecoilFunction::GetName() const
{
    return _name;
}

float RecoilFunction::GetPeakRotX() const
{
    // Each queued item holds an absolute rotation built by AddItem, not a cumulative one,
    // so the peak is simply the largest of them.
    float peak = 0;
    for (int i = 0; i < _queue.Size(); i++)
    {
        Vector3Val dir = _queue[i]._mat.Direction();
        const float a = fabs(atan2(dir.Y(), dir.Z()));
        if (a > peak)
        {
            peak = a;
        }
    }
    return peak;
}

float RecoilFunction::GetRecoilRotX(float time) const
{
    Matrix4 mat = MIdentity;
    ApplyRecoil(time, mat);
    return atan2(mat.Direction().Y(), mat.Direction().Z());
}

void RecoilFunction::GetFFEdge(int index, float& time, float& amplitude) const
{
    if (index >= _queue.Size())
    {
        if (_queue.Size() > 0)
        {
            time = _queue[_queue.Size() - 1]._time;
        }
        else
        {
            time = 0;
        }
        amplitude = 0;
        return;
    }
    const RecoilItem& item = _queue[index];
    time = item._time;
    amplitude = atan2(item._mat.Direction().Y(), item._mat.Direction().Z());
}

bool RecoilFunction::GetFFRamp(int index, RecoilFFRamp& ramp) const
{
    if (index >= _queue.Size())
    {
        ramp._begAmplitude = 0;
        ramp._endAmplitude = 0;
        ramp._duration = 1000;
        if (_queue.Size() > 0)
        {
            ramp._startTime = _queue[_queue.Size() - 1]._time;
        }
        else
        {
            ramp._startTime = 0;
        }
        return false;
    }
    GetFFEdge(index, ramp._startTime, ramp._begAmplitude);
    GetFFEdge(index + 1, ramp._duration, ramp._endAmplitude);
    ramp._duration -= ramp._startTime;
    return true;
}

void RecoilFunction::ApplyRecoil(float time, Matrix4& mat, float factor, bool translationOnly) const
{
    if (_queue.Size() <= 1)
    {
        return;
    }
    int prev = 0;
    for (prev = 0; prev < _queue.Size() - 1; prev++)
    {
        const RecoilItem& nextItem = _queue[prev + 1];
        if (time < nextItem._time)
        {
            break;
        }
    }
    if (prev >= _queue.Size() - 1)
    {
        return;
    }
    const RecoilItem& prevItem = _queue[prev];
    const RecoilItem& nextItem = _queue[prev + 1];
    PoseidonAssert(nextItem._time > prevItem._time);
    PoseidonAssert(nextItem._time >= time);
    PoseidonAssert(prevItem._time <= time);
    float delta = nextItem._time - prevItem._time;
    float pFactor = (time - prevItem._time) / delta;
    Matrix4 res;
    res.SetMultiply(prevItem._mat, 1 - pFactor);
    res.AddMultiply(nextItem._mat, pFactor);

    // Drop the curve's rotation, keep its translation. Each item is SetRotationX(-angle)
    // plus a straight-back offset (see AddItem), and the keyframes are sparse enough that
    // the rotation arrives as a step - which is what reads as a jerk no matter how much
    // it is scaled down. The offset is the shove back along the bore and is smooth enough
    // on its own, so it stays at full strength while the spring in EntityAI takes over the
    // angular side.
    if (translationOnly)
    {
        const Vector3 pos = res.Position();
        res = MIdentity;
        res.SetPosition(pos);
    }

    if (factor < 1)
    {
        mat = res * mat * factor + mat * (1 - factor);
    }
    else
    {
        mat.SetMultiply(res, mat);
    }
}

bool RecoilFunction::GetTerminated(float time) const
{
    if (_queue.Size() <= 1)
    {
        return true;
    }
    const RecoilItem& item = _queue[_queue.Size() - 1];
    return item._time <= time;
}

} // namespace Poseidon
