#include <WgpuRenderer/EarthGroundDetail.hpp>
#include <cassert>
#include <iostream>

int main()
{
    using namespace Poseidon;
    for(double latitude : {-84.0,-49.61,0.0,49.61,84.0})
        for(double longitude : {-180.0,-6.13,0.0,6.13,179.999})
        {
            const auto origin=EarthGroundDetail::SplitOrigin(latitude,longitude);
            const double lat=latitude*Earth::Pi/180,lon=longitude*Earth::Pi/180;
            const std::array<double,3> expected={Earth::Radius*std::cos(lat)*std::cos(lon),Earth::Radius*std::sin(lat),Earth::Radius*std::cos(lat)*std::sin(lon)};
            for(size_t i=0;i<3;++i)
            {
                assert(origin.low[i]>=0 && origin.low[i]<512);
                assert(std::fmod(origin.high[i],512.0f)==0);
                assert(std::abs(double(origin.high[i])+origin.low[i]-expected[i])<0.00002);
                for(float frequency : {0.125f,1.0f,8.0f})
                    assert(double(origin.high[i]*frequency)==double(origin.high[i])*frequency);
            }
        }
    // The same real geographic point expressed in two ENU frames must recover
    // the same ECEF noise position to centimetre precision after float uploads.
    const auto ecef=[](double lat,double lon,double altitude) {
        lat*=Earth::Pi/180;lon*=Earth::Pi/180;
        return std::array<double,3>{(Earth::Radius+altitude)*std::cos(lat)*std::cos(lon),
            (Earth::Radius+altitude)*std::sin(lat),(Earth::Radius+altitude)*std::cos(lat)*std::sin(lon)};
    };
    const auto point=ecef(49.612,6.134,200);
    for(const auto location : {Earth::Coordinate{49.61,6.13},Earth::Coordinate{49.9,6.6}})
    {
        const auto split=EarthGroundDetail::SplitOrigin(location.latitude,location.longitude);
        const auto origin=ecef(location.latitude,location.longitude,0);
        const double a=location.latitude*Earth::Pi/180,b=location.longitude*Earth::Pi/180;
        const std::array<std::array<double,3>,3> basis={{{-std::sin(b),0,std::cos(b)},
            {std::cos(a)*std::cos(b),std::sin(a),std::cos(a)*std::sin(b)},
            {-std::sin(a)*std::cos(b),std::cos(a),-std::sin(a)*std::sin(b)}}};
        std::array<float,3> local{};
        for(size_t axis=0;axis<3;++axis)
        {
            double v=0;for(size_t i=0;i<3;++i)v+=(point[i]-origin[i])*basis[axis][i];
            local[axis]=float(v+(axis==1?0:Earth::Anchor))-(axis==1?0:Earth::Anchor);
        }
        const auto normalize=[](std::array<float,3> v) {
            const float size=std::sqrt(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]);
            for(auto& x:v)x/=size;return v;
        };
        const auto up=normalize({split.high[0]+split.low[0],split.high[1]+split.low[1],split.high[2]+split.low[2]});
        const auto east=normalize({-up[2],0,up[0]});
        const std::array<float,3> north={east[1]*up[2]-east[2]*up[1],east[2]*up[0]-east[0]*up[2],east[0]*up[1]-east[1]*up[0]};
        for(size_t i=0;i<3;++i)
        {
            const float residual=split.low[i]+east[i]*local[0]+up[i]*local[1]+north[i]*local[2];
            assert(std::abs(double(split.high[i])+residual-point[i])<0.02);
        }
    }
    std::cout << "Earth ground ECEF split precision/frequency/rebase invariance PASS\n";
}
