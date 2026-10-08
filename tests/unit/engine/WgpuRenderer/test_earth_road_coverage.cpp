#include "../../../../engine/WgpuRenderer/EarthRoadCoverage.hpp"
#include <cassert>
#include <iostream>

int main()
{
    using namespace Poseidon::EarthRoadCoverage;
    assert(StockRoad("o\\road\\sil25.p3d"));
    assert(StockRoad("O/ROAD/ces25.P3D"));
    for (const auto name : {"data3d\\str smrk.p3d", "O\\Hous\\domek_sedy.p3d",
         "ca\\roads\\road.p3d", "o\\road\\..\\glass.p3d", "o\\road\\nested\\a.p3d",
         "o\\road\\c:a.p3d", "o\\road\\a.paa", "o\\road\\", "o\\road\\.p3d"})
        assert(!StockRoad(name));
    // Actual installed sil25 diagnostic: alpha-blend, OnSurface, texture present.
    assert(Section(true, true, true, true, false));
    assert(!Section(false, true, true, true, false)); // native owner stays CPU-owned
    assert(!Section(true, true, false, true, false)); // glass/default surfaces
    assert(!Section(true, false, true, true, false));
    assert(!Section(true, true, true, false, false)); // no fabricated road texture
    assert(!Section(true, true, true, true, true)); // hidden markers
    assert(CanShade(true, true, true));
    assert(!CanShade(true, false, true));
    assert(!CanShade(true, true, false)); // translucent material must not become solid
    assert(!CanShade(false, true, true));
    assert(AlphaReference == 0.5f && SurfaceLiftMetres == 0.02f);
    std::cout << "Earth-only stock road coverage policy PASS\n";
}
