#ifdef _MSC_VER
#pragma once
#endif

#ifndef _MATHOPT_HPP
#define _MATHOPT_HPP

#include <Poseidon/Foundation/Math/MathDefs.hpp>
#include <Poseidon/Foundation/platform.hpp>
#include <cfloat>
#include <cmath>

namespace Poseidon::Foundation
{
inline constexpr double InvEps = 1e-4;
inline constexpr double InvSqrtEps = 1e-3;

#define INV_EPS (InvEps)
#define INVSQRT_EPS (InvSqrtEps)

[[nodiscard]] constexpr Coord HDegree(Coord angle)
{ // convert degree to radian
    return angle * coord(H_PI / 180);
}

[[nodiscard]] constexpr float fSign(float x)
{
    if (x > 0)
    {
        return +1.0f;
    }
    if (x < 0)
    {
        return -1.0f;
    }
    return 0.0f;
}

[[nodiscard]] constexpr int sign(float x)
{
    if (x > 0)
    {
        return +1;
    }
    if (x < 0)
    {
        return -1;
    }
    return 0;
}

#ifndef _KNI

[[nodiscard]] inline float Inv(float c)
{
    return 1.0f / c;
}

[[nodiscard]] constexpr float Square(float x)
{
    return x * x;
}

#else

// consider: NewtonRaphson Reciprocal
//   [2 * rcpps(x) - (x * rcpps(x) * rcpps(x))]
[[nodiscard]] inline float Inv(float c)
{
    return 1.0f / c;
}

[[nodiscard]] constexpr float Square(float x)
{
    return x * x;
}

#endif

// 1/sqrt(x). (PERF 2026-10-14) This was the 2001 Graphics Gems V method -- a 16 KB seed table
// plus one Newton step in double precision -- compiled out of line behind a function-local
// static, i.e. never inlined: 5.2% of the main thread's SELF time in perf_combat, because every
// Vector3 Size()/Normalized()/InvSize() goes through it. A modern sqrtss + divss is exact,
// deterministic across CPU vendors (unlike rsqrtss) and inlines. Inputs at or below FLT_MIN
// (zero, denormals, negatives, NaN) are clamped to FLT_MIN so the result stays FINITE, as the
// table's did: InvSize() of a zero vector must not become inf and turn 0*inf into NaN.
[[nodiscard]] inline float InvSqrt(float x)
{
    return 1.0f / std::sqrt(x > FLT_MIN ? x : FLT_MIN);
}

} // namespace Poseidon::Foundation

using ::Poseidon::Foundation::fSign;
using ::Poseidon::Foundation::HDegree;
using ::Poseidon::Foundation::Inv;
using ::Poseidon::Foundation::InvEps;
using ::Poseidon::Foundation::InvSqrt;
using ::Poseidon::Foundation::InvSqrtEps;
using ::Poseidon::Foundation::sign;
using ::Poseidon::Foundation::Square;

#endif
