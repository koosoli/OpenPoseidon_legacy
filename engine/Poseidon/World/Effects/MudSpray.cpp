// SPDX-License-Identifier: GPL-3.0-or-later
#include <Poseidon/World/Effects/MudSpray.hpp>

#include <cmath>
#include <cstdlib>
#include <mutex>

namespace Poseidon
{
namespace
{
std::mutex& Lock()
{
    static std::mutex m;
    return m;
}
std::vector<MudSprayReport>& Pending()
{
    static std::vector<MudSprayReport> v;
    return v;
}
// a report's fraction of a blob, carried into the next frame so a slow wheel still throws mud
float& Carry()
{
    static float c = 0;
    return c;
}
} // namespace

bool MudSprayEnabled()
{
    static const bool on = []
    {
        const char* v = std::getenv("WGR_MUD");
        return !(v && v[0] == '0');
    }();
    return on;
}

bool& MudFountain()
{
    static bool on = []
    {
        const char* v = std::getenv("POSEIDON_MUD_FOUNTAIN");
        return v && v[0] == '1';
    }();
    return on;
}

void ReportMudSpray(const MudSprayReport& spray)
{
    if (!MudSprayEnabled() || !(spray.count > 0) || !std::isfinite(spray.count))
        return;
    std::lock_guard<std::mutex> guard(Lock());
    auto& pending = Pending();
    // a wheel reports every simulation step: merge with its last report from the same spot
    for (auto& p : pending)
        if ((p.pos - spray.pos).SquareSize() < 0.04f && (p.vel - spray.vel).SquareSize() < 4.0f)
        {
            p.count += spray.count;
            return;
        }
    if (pending.size() < 64)
        pending.push_back(spray);
}

void TakeMudSprays(std::vector<MudSprayReport>& out)
{
    out.clear();
    std::lock_guard<std::mutex> guard(Lock());
    auto& pending = Pending();
    float& carry = Carry();
    for (auto& p : pending)
    {
        const float total = p.count + carry;
        const float whole = std::floor(total);
        carry = total - whole;
        if (whole >= 1.0f)
        {
            out.push_back(p);
            out.back().count = whole;
        }
    }
    pending.clear();
}
} // namespace Poseidon
