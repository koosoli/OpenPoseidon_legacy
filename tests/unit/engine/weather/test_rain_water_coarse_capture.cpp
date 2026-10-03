#include "../../../../engine/Poseidon/Dev/Diag/RainWaterCapture.hpp"
#include <cassert>
#include <cstring>
#include <sstream>
#include <iostream>
using namespace Poseidon;
int main(){
    const auto observed=std::bit_cast<double>(uint64_t{0x3FC555737C000000});
    assert(std::string(RainWaterCapture::DoubleBits(observed).data())=="3FC555737C000000");
    assert(std::string(RainWaterCapture::DoubleBits(0.166670260950923).data())=="3FC555737C000001");
    assert(std::string(RainWaterCapture::DoubleBits(-0.0).data())=="8000000000000000");
    assert(std::string(RainWaterCapture::DoubleBits(0.1).data())=="3FB999999999999A");
    RainWaterField field;assert(!field.CoarseCapture());
    std::vector<float> bed={-0.f,12345.678f,7,8,9,10};
    assert(field.Configure(3,2,25,bed,0,0,-1));
    assert(field.AddWater(1,0,.1234567f));field.Advance(.125f,.8f);
    const auto state=field.Snapshot();const auto budget=field.WaterBudget();
    const auto revision=field.Revision(),generation=field.Generation();const auto pending=field.PendingSeconds();
    const auto capture=field.CoarseCapture();assert(capture);
    assert(capture->bed.size()==6 && capture->depth.size()==6 && capture->pending==pending);
    assert(std::memcmp(capture->bed.data(),bed.data(),bed.size()*sizeof(float))==0);
    assert(std::bit_cast<uint32_t>(capture->depth[1])==std::bit_cast<uint32_t>(.1234567f));
    std::ostringstream stream(std::ios::binary);
    assert(RainWaterCapture::Write(stream,*capture));const auto bytes=stream.str();
    assert(bytes.size()==RainWaterCapture::HeaderBytes+6*8 && bytes.substr(0,8)=="RWCAP001");
    assert(std::memcmp(bytes.data()+88,bed.data(),bed.size()*sizeof(float))==0);
    assert(std::memcmp(bytes.data()+88+bed.size()*sizeof(float),capture->depth.data(),bed.size()*sizeof(float))==0);
    const auto after=field.Snapshot();assert(state==after);
    assert(field.Revision()==revision && field.Generation()==generation && field.PendingSeconds()==pending);
    assert(std::memcmp(&budget,&field.WaterBudget(),sizeof(budget))==0);
    auto invalid=*capture;invalid.width=514;std::ostringstream refused;
    assert(!RainWaterCapture::Write(refused,invalid)&&refused.str().empty());
    RainWaterField huge;assert(huge.Configure(514,2,25,std::vector<float>(1028,1)));
    assert(!huge.CoarseCapture());
    RainWaterField maximum;std::vector<float> maxBed(RainWaterCapture::MaxCells,1);maxBed.back()=2.34567f;
    assert(maximum.Configure(513,513,25,maxBed));const auto maxView=maximum.CoarseCapture();assert(maxView);
    std::ostringstream maxStream(std::ios::binary);assert(RainWaterCapture::Write(maxStream,*maxView));
    const auto maxData=maxStream.str();assert(maxData.size()==RainWaterCapture::MaxBytes);
    assert(std::memcmp(maxData.data()+88+(maxBed.size()-1)*sizeof(float),&maxBed.back(),sizeof(float))==0);
    int owner=0;constexpr int range=1028;const auto grid=RainWaterField::AlignedSourceGrid(range,6.25f);
    RainWaterField fine;assert(fine.Configure(grid.side,grid.side,grid.spacing,std::vector<float>(size_t(grid.side)*grid.side,10)));
    fine.UpdateTerrainSource(&owner,range,6.25f,1,true,[](int,int){return 10.f;});
    fine.RecordTerrainVertex(&owner,range,6.25f,82,82,2);
    fine.UpdateTerrainSource(&owner,range,6.25f,2,true,[](int x,int z){return x==82&&z==82 ? 9.f : 10.f;});
    assert(fine.FineActive()&&!fine.CoarseCapture());
    assert(field.AddWater(0,0,std::numeric_limits<float>::max()));
    assert(field.AddWater(0,0,std::numeric_limits<float>::max()));assert(!field.CoarseCapture());
    const auto base=std::filesystem::temp_directory_path();
    assert(RainWaterCapture::CampaignOutputPath(base/"build/rain-helicopter/campaign/Default-Rain"));
    assert(RainWaterCapture::CampaignOutputPath(base/"build/rain-helicopter/campaign/FarCoverOff-Dry"));
    assert(!RainWaterCapture::CampaignOutputPath("build/rain-helicopter/campaign/Default-Rain"));
    assert(!RainWaterCapture::CampaignOutputPath(base/"build/rain-helicopter/campaign/user"));
    assert(!RainWaterCapture::CampaignOutputPath(base/"build/rain-helicopter/../../other/Default-Rain"));
    assert(RainWaterCapture::MaxBytes==2105440);
    std::cout<<"Exact raw coarse capture, IEEE bits, readonly identity/budget, fine refusal, maximum edge and bounded path admission PASS\n";
}
