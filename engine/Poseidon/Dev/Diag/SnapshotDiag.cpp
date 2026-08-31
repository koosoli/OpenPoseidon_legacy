#include <Poseidon/Dev/Diag/SnapshotDiag.hpp>

namespace Poseidon::Dev
{

SnapshotCounters& GSnapshotCounters()
{
    static SnapshotCounters counters;
    return counters;
}

} // namespace Poseidon::Dev
