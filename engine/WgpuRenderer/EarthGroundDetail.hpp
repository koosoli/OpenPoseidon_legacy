#pragma once
#include <Poseidon/World/Terrain/EarthFrame.hpp>
#include <array>

namespace Poseidon::EarthGroundDetail
{
struct Origin { std::array<float,3> high, low; };
inline Origin SplitOrigin(double latitude,double longitude)
{
    const double lat=latitude*Earth::Pi/180.0,lon=longitude*Earth::Pi/180.0;
    const std::array<double,3> ecef={Earth::Radius*std::cos(lat)*std::cos(lon),
        Earth::Radius*std::sin(lat),Earth::Radius*std::cos(lat)*std::sin(lon)};
    Origin result{};
    for(size_t i=0;i<3;++i)
    {
        // Exact float multiples, also exact under shader frequencies 1/8,1,8.
        const double high=std::floor(ecef[i]/512.0)*512.0;
        result.high[i]=float(high);result.low[i]=float(ecef[i]-high);
        if(result.low[i]>=512.0f){result.high[i]+=512.0f;result.low[i]=0.0f;}
    }
    return result;
}
}
