#include <Poseidon/Foundation/Math/MathOpt.hpp>
#include <Poseidon/Foundation/Common/FltOpts.hpp>
#include <float.h>
#include <cmath>
#include <Poseidon/Foundation/Framework/DebugLog.hpp>
#include <Poseidon/Foundation/Math/Math3DP.hpp>
#include <Poseidon/Foundation/Math/MathDefs.hpp>

#pragma optimize("t", on)

namespace Poseidon::Foundation
{

float Interpolativ(float control, float cMin, float cMax, float vMin, float vMax)
{
    if (control <= cMin)
    {
        return vMin;
    }
    if (control >= cMax)
    {
        return vMax;
    }
    return (control - cMin) / (cMax - cMin) * (vMax - vMin) + vMin;
}

float AngleDifference(float a, float b)
{
    float d = a - b;
    if (fabs(d) <= H_PI)
    {
        return d;
    }
    d = fastFmod(d, 2 * H_PI);
    if (d > +H_PI)
    {
        d -= 2 * H_PI;
    }
    if (d < -H_PI)
    {
        d += 2 * H_PI;
    }
    return d;
}

#ifdef _MSC_VER

bool VerifyFloat(float x)
{
    switch (_fpclass(x))
    {
        case _FPCLASS_SNAN:
        case _FPCLASS_QNAN:
        case _FPCLASS_PINF:
        case _FPCLASS_NINF:
            RptF("Non-finite number %g", x);
            return false;
    }
    // all tests on indefinite number will fail
    if (x <= 0)
    {
        return true;
    }
    if (x >= 0)
    {
        return true;
    }
    // we have indefinite number
    RptF("Invalid number %g", x);
    return false;
}

#else

bool VerifyFloat(float x)
{
    if (_finite(x))
        return true;
    if (isinf(x))
    {
        RptF("Non-finite number %g", x);
        return false;
    }
    RptF("Invalid number %g", x);
    return false;
}

#endif

} // namespace Poseidon::Foundation
