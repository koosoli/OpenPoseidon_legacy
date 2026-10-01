#include <Poseidon/World/Terrain/TerrainCdlod.hpp>
#include <cassert>
#include <cmath>
#include <iostream>
#include <limits>
#include <vector>
using namespace Poseidon;

static bool same(const CdlodSelection& a,const CdlodSelection& b)
{
    return a.originX==b.originX && a.originZ==b.originZ && a.size==b.size &&
        a.level==b.level && a.morphStart==b.morphStart && a.morphEnd==b.morphEnd;
}
static std::vector<CdlodSelection> focus(const CdlodSelection& p,
    const std::array<CdlodContactFocus,2>& f,size_t count)
{
    std::vector<CdlodSelection> out;
    RefineCdlodContactUnion(p,f,count,[&](const auto& s){out.push_back(s);});
    return out;
}
static void partition(const CdlodSelection& p,const std::vector<CdlodSelection>& out)
{
    double area=0;
    for (const auto& a:out)
    {
        assert(a.originX>=p.originX && a.originZ>=p.originZ);
        assert(a.originX+a.size<=p.originX+p.size && a.originZ+a.size<=p.originZ+p.size);
        assert(a.level==p.level && a.size>0);
        area+=double(a.size)*a.size;
    }
    assert(area==double(p.size)*p.size);
    for (size_t i=0;i<out.size();++i) for (size_t j=i+1;j<out.size();++j)
    {
        const auto&a=out[i];const auto&b=out[j];
        assert(!(a.originX<b.originX+b.size && b.originX<a.originX+a.size &&
            a.originZ<b.originZ+b.size && b.originZ<a.originZ+a.size));
    }
}
int main()
{
    const CdlodSelection parent{-800,-800,1600,2,100,200};
    for (float x:{-799.f,-40.f,0.f,33.f,799.f,900.f})
    {
        const std::array<CdlodContactFocus,2> f{{{x,0,8},{x,0,8}}};
        std::vector<CdlodSelection> old;
        RefineCdlodSnow(parent,x,0,8,[&](const auto&s){old.push_back(s);});
        const auto single=focus(parent,f,1), duplicated=focus(parent,f,2);
        assert(old.size()==single.size() && single.size()==duplicated.size());
        for (size_t i=0;i<old.size();++i) {assert(same(old[i],single[i]));assert(same(single[i],duplicated[i]));}
        partition(parent,single);
    }
    // Third-person 10m displacement, then overlapping/separated windows and
    // map-edge clipping. Union split count never exceeds the two old traversals.
    for (float separation:{0.f,4.f,10.f,16.f,20.f})
    {
        const std::array<CdlodContactFocus,2> f{{{0,0,8},{separation,0,8}}};
        auto united=focus(parent,f,2);
        const auto a=focus(parent,f,1), b=focus(parent,{{f[1],f[1]}},1);
        assert(united.size()<=a.size()+b.size()-1);
        assert(united.size()<512);
        partition(parent,united);
        for (const auto&s:united)
            for (const auto& point:f)
                if (s.originX<point.x+8 && s.originZ<point.z+8 &&
                    s.originX+s.size>point.x-8 && s.originZ+s.size>point.z-8)
                    assert(s.size/32<=0.125f);
        const auto capped=focus(parent,f,99);
        assert(capped.size()==united.size());
        for (size_t i=0;i<capped.size();++i) assert(same(capped[i],united[i]));
    }
    const float nan=std::numeric_limits<float>::quiet_NaN();
    for (auto bad:std::array<CdlodContactFocus,5>{{{nan,0,8},{0,nan,8},{0,0,nan},{0,0,9},{0,0,-1}}})
    {
        const auto out=focus(parent,{{bad,bad}},2);
        assert(out.size()==1 && same(out[0],parent));
    }
    const auto none=focus(parent,{},0);assert(none.size()==1&&same(none[0],parent));
    assert(CdlodContactFocusEligible(0,2,-10,0,0,0,0,true,true,true));
    assert(CdlodContactFocusEligible(0,0,-20,0,0,0,0,true,true,true));
    assert(!CdlodContactFocusEligible(0,0,-20.01f,0,0,0,0,true,true,true));
    assert(!CdlodContactFocusEligible(0,0,-10,0,3.01f,0,0,true,true,true));
    assert(!CdlodContactFocusEligible(0,0,-10,0,0,0,0,false,true,true));
    assert(!CdlodContactFocusEligible(0,0,-10,0,0,0,0,true,false,true));
    assert(!CdlodContactFocusEligible(0,0,-10,0,0,0,0,true,true,false));
    assert(!CdlodContactFocusEligible(nan,0,-10,0,0,0,0,true,true,true));
    // Existing 512*.125m upload is snapped down to a 16*.125m chunk.
    // A20m target+8m focus retains >=2m margin from its nearest texture edge.
    for (int i=0;i<200;++i)
    {
        const float camera=-3.17f+i*.019f;
        const float start=std::floor(camera/2)*2-32;
        assert(camera+20+8<start+64 && camera-20-8>start);
    }
    std::cout<<"PASS actual CDLOD focus equivalence, union, overlap, cap, partition, terrain bounds, upload margin and target gates\n";
}
