#pragma once
#include <Poseidon/World/Weather/RainWaterField.hpp>
#include <bit>
#include <array>
#include <filesystem>
#include <ostream>
#include <string>

namespace Poseidon::RainWaterCapture
{
inline constexpr size_t MaxCells=size_t(513)*513,HeaderBytes=88;
inline constexpr size_t MaxBytes=HeaderBytes+2*MaxCells*sizeof(float);
inline constexpr char Magic[]="RWCAP001";
static_assert(sizeof(float)==4 && sizeof(double)==8 && std::numeric_limits<float>::is_iec559 &&
              std::numeric_limits<double>::is_iec559);
// JSON decimal formatting need not round-trip a binary64. Carry its native
// bits as a canonical string, independent of locale/serializer precision.
inline constexpr std::array<char,17> DoubleBits(double value) {
    constexpr char hex[]="0123456789ABCDEF";
    const auto bits=std::bit_cast<uint64_t>(value);
    std::array<char,17> result{};
    for(unsigned i=0;i<16;++i)result[i]=hex[(bits>>(4*(15-i)))&15];
    return result;
}
static_assert(DoubleBits(std::bit_cast<double>(uint64_t{0x3FC555737C000000}))[15]=='0');
static_assert(DoubleBits(0.166670260950923)[15]=='1');
static_assert(std::bit_cast<uint64_t>(0.166670260950923)==uint64_t{0x3FC555737C000001});
inline std::optional<std::filesystem::path> CampaignOutputPath(const std::filesystem::path& directory) {
    if(!directory.is_absolute())return {};
    const auto path=directory.lexically_normal();bool designated=false;std::string previous;
    for(const auto& part:path) {
        std::string name=part.string();
        for(char& c:name)if(c>='A'&&c<='Z')c=char(c-'A'+'a');
        designated|=previous=="build"&&name=="rain-helicopter";
        previous=name;
    }
    if(!designated || (previous!="default-dry" && previous!="default-rain" &&
        previous!="farcoveroff-dry" && previous!="farcoveroff-rain"))return {};
    return path/"coarse-runoff.rwcap";
}
inline void U32(std::ostream& out,uint32_t value) {
    const char bytes[]={char(value),char(value>>8),char(value>>16),char(value>>24)};
    out.write(bytes,sizeof(bytes));
}
inline void U64(std::ostream& out,uint64_t value) {
    for(unsigned i=0;i<8;++i)out.put(char(value>>(8*i)));
}
inline void F32(std::ostream& out,float value){U32(out,std::bit_cast<uint32_t>(value));}
inline void F64(std::ostream& out,double value){U64(out,std::bit_cast<uint64_t>(value));}
inline bool Write(std::ostream& out,const RainWaterField::CoarseCaptureView& view) {
    if(view.width<2||view.height<2||view.width>513||view.height>513||
       view.bed.size()!=size_t(view.width)*view.height||view.bed.size()>MaxCells||
       view.depth.size()!=view.bed.size())return false;
    out.write(Magic,8);U32(out,uint32_t(view.width));U32(out,uint32_t(view.height));
    F32(out,view.spacing);F32(out,view.originX);F32(out,view.originZ);F32(out,view.seaLevel);
    F64(out,view.pending);U64(out,view.generation);U64(out,view.revision);
    F64(out,view.budget.rain);F64(out,view.budget.infiltration);
    F64(out,view.budget.evaporation);F64(out,view.budget.outlet);
    for(const auto cells:{view.bed,view.depth}) {
        if constexpr(std::endian::native==std::endian::little)
            out.write(reinterpret_cast<const char*>(cells.data()),std::streamsize(cells.size_bytes()));
        else for(float value:cells)F32(out,value);
    }
    return bool(out);
}
}
