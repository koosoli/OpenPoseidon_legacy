#include <Poseidon/Dev/Diag/LightingDiag.hpp>

namespace Poseidon::Dev
{

LightCounters& GLightCounters()
{
    static LightCounters counters;
    return counters;
}

} // namespace Poseidon::Dev
