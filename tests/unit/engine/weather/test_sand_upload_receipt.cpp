#include <Poseidon/World/Weather/SandField.hpp>
#include <WgpuRenderer/SandUploadReceipt.hpp>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <string>

using namespace Poseidon;
float SnapshotSample(const std::vector<float>& data,float x,float z)
{
    const float tx=(x-data[0])/data[2]-.5f,tz=(z-data[1])/data[2]-.5f;
    const int ix=int(std::floor(tx)),iz=int(std::floor(tz));
    assert(ix>=24 && iz>=24 && ix<487 && iz<487); // no window feather here
    const float fx=tx-ix,fz=tz-iz;
    const size_t at=4+size_t(iz)*512+size_t(ix);
    return std::lerp(std::lerp(data[at],data[at+1],fx),std::lerp(data[at+512],data[at+513],fx),fz);
}
int main(int argc,char** argv)
{
    assert(SandUploadTraceRequested("1"));
    for(const char* value:std::array<const char*,6>{nullptr,"","0","2","1.0","1extra"})assert(!SandUploadTraceRequested(value));
    SandField sand;
    // Real saved Everon contact positions. Orientation is a controlled replay
    // (90 degrees), not invented proof of the unlogged original actor heading.
    constexpr std::array<std::array<float,2>,4> contacts{{
        {5975.80419921875f,3325.572998046875f},
        {5976.9609375f,3326.405029296875f},
        {5978.1240234375f,3327.39208984375f},
        {5979.20703125f,3328.111083984375f}}};
    for(const auto& p:contacts) assert(sand.StampBoot(p[0],p[1],1,0,.10f,.014f));
    const auto low=sand.Snapshot(5973.80419921875f,3325.572998046875f);
    const auto top=sand.Snapshot(5976.00419921875f,3325.572998046875f);
    assert(low.size()==262148 && top.size()==262148);
    const auto receipt=InspectSandUpload(low);
    assert(receipt.finite && receipt.negative>0 && receipt.positive>0);
    assert(receipt.minimum<-.08f && receipt.minimum>=-.15f && receipt.maximum>0 && receipt.maximum<=.02f);
    for(const auto& p:contacts) {
        assert(std::abs(SnapshotSample(low,p[0],p[1])-sand.HeightOffsetAt(p[0],p[1]))<1e-7f);
        assert(std::abs(SnapshotSample(top,p[0],p[1])-sand.HeightOffsetAt(p[0],p[1]))<1e-7f);
        assert(SnapshotSample(top,p[0],p[1])<-.05f);
    }
    assert(InspectSandUpload(low).hash==receipt.hash);
    assert(InspectSandUpload(top).hash!=receipt.hash); // window bytes differ, physical samples agree
    auto changed=low;changed[4]=-0.0f;
    assert(InspectSandUpload(changed).hash!=receipt.hash); // signed zero is a distinct exact payload
    changed[4]=std::numeric_limits<float>::quiet_NaN();
    assert(!InspectSandUpload(changed).finite);
    sand.enabled=false;
    const auto off=InspectSandUpload(sand.Snapshot(5975,3325));
    assert(off.finite && off.negative==0 && off.positive==0);
    sand.enabled=true;
    const auto remote=InspectSandUpload(sand.Snapshot(0,0));
    assert(remote.negative==0 && remote.positive==0);
    assert(InspectSandUpload(sand.Snapshot(5973.80419921875f,3325.572998046875f)).hash==receipt.hash);
    if(argc>1) {
        const auto path=std::filesystem::absolute(argv[1]);
        const auto fixtureDirectory=path.parent_path();
        assert(DumpSandUpload(low,path,fixtureDirectory));
        assert(std::filesystem::file_size(path)==1048592);
        assert(!DumpSandUpload(low,"relative.f32",fixtureDirectory));
        assert(!DumpSandUpload(low,path.parent_path()/"file.exe",fixtureDirectory));
        assert(!DumpSandUpload(low,path.parent_path()/"not-created"/"file.f32",fixtureDirectory));
        assert(!DumpSandUpload(low,path,fixtureDirectory.parent_path()));
        assert(!DumpSandUpload(std::span(low).first(low.size()-1),path,fixtureDirectory));
        assert(!DumpSandUpload(changed,path,fixtureDirectory));
        assert(std::filesystem::file_size(path)==1048592);
        std::ofstream expected(std::string(argv[1])+".expected");
        expected<<receipt.hash<<' '<<receipt.negative<<' '<<receipt.positive<<' '
            <<std::bit_cast<uint32_t>(receipt.minimum)<<' '<<std::bit_cast<uint32_t>(receipt.maximum);
        assert(expected.good());
    }
    std::printf("Sand actual field replay/snapshot parity and receipt PASS hash=%016llx negative=%zu positive=%zu min=%.8f max=%.8f\n",
        static_cast<unsigned long long>(receipt.hash),receipt.negative,receipt.positive,double(receipt.minimum),double(receipt.maximum));
}
