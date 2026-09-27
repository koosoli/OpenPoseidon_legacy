#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Graphics/Rendering/Font/Pactext.hpp>
#include <Poseidon/Graphics/Core/MipmapLayout.hpp>
#include <Poseidon/Foundation/Algorithms/Lzo1x.hpp>
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>
namespace
{
std::vector<char> DxtMip(int w,int h,size_t declared,size_t available,bool lzo=false)
{
    std::vector<char> bytes;
    const auto word=[&](int v){bytes.push_back(char(v));bytes.push_back(char(v>>8));};
    word(w|(lzo?0x8000:0));word(h);
    bytes.push_back(char(declared));bytes.push_back(char(declared>>8));bytes.push_back(char(declared>>16));
    for(size_t i=0;i<available;++i) bytes.push_back(char(i*37+13));
    return bytes;
}
Poseidon::PacLevelMem DxtLevel(Poseidon::PacFormat src,Poseidon::PacFormat dst,int w,int h)
{
    Poseidon::PacLevelMem level;level._sFormat=src;level._dFormat=dst;level._w=short(w);level._h=short(h);return level;
}
size_t Capacity(Poseidon::PacFormat format,int w,int h)
{
    // Use the same destination allocation layout as the WGPU texture loader.
    const auto layout=Poseidon::render::mipmap::ComputeLayout(format,w,h);
    REQUIRE(layout.dataSize>0);
    CHECK(size_t(layout.dataSize)==size_t((w+3)/4)*size_t((h+3)/4)*(format==Poseidon::PacDXT1?8:16));
    return size_t(layout.dataSize);
}
}

TEST_CASE("Complete PAA terminator permits no destination but truncated terminator refuses", "[paa-dxt-bounds]")
{
    Poseidon::PacPalette palette; const auto level=DxtLevel(Poseidon::PacDXT1,Poseidon::PacDXT1,4,4);
    const std::array<char,4> terminator{};
    Poseidon::QIStream complete(terminator.data(),int(terminator.size()));
    REQUIRE(level.LoadPaa(complete,nullptr,&palette)==1); CHECK(complete.tellg()==4); CHECK_FALSE(complete.fail());
    Poseidon::QIStream truncated(terminator.data(),2);
    CHECK(level.LoadPaa(truncated,nullptr,&palette)==-1); CHECK(truncated.eof()); CHECK_FALSE(truncated.fail());
}

TEST_CASE("Raw PAA BC mips preserve exact thin and short payloads inside canaries", "[paa-dxt-bounds]")
{
    Poseidon::PacPalette palette;
    for(auto fmt:{Poseidon::PacDXT1,Poseidon::PacDXT3,Poseidon::PacDXT5})
    for(const auto dims:std::array<std::array<int,2>,6>{{{4,4},{2,2},{2,8},{8,2},{4,1},{2,8192}}})
    {
        const auto capacity=Capacity(fmt,dims[0],dims[1]);
        const auto level=DxtLevel(fmt,fmt,dims[0],dims[1]);
        for(size_t stored:{capacity,capacity-1,size_t(0)}) {
            const auto bytes=DxtMip(dims[0],dims[1],stored,stored);
            Poseidon::QIStream in(bytes.data(),int(bytes.size()));
            std::vector<uint8_t> output(capacity+16,0xa7),expected=output;
            std::memcpy(expected.data()+8,bytes.data()+7,stored);
            REQUIRE(level.LoadPaa(in,output.data()+8,&palette)==0); CHECK(output==expected);
            CHECK(in.tellg()==int(7+stored)); CHECK_FALSE(in.fail());
        }
    }
}

TEST_CASE("Source PAA dimensions and oversized BC payloads cannot write destination", "[paa-dxt-bounds]")
{
    Poseidon::PacPalette palette;
    for(auto fmt:{Poseidon::PacDXT1,Poseidon::PacDXT3,Poseidon::PacDXT5})
    for(int variant=0;variant<6;++variant) {
        const auto capacity=Capacity(fmt,4,4); auto level=DxtLevel(fmt,fmt,4,4);
        auto bytes=DxtMip(4,4,capacity+1,capacity+1);
        if(variant==1) bytes=DxtMip(8,4,capacity,capacity);
        if(variant==2) bytes=DxtMip(4,0,capacity,capacity);
        if(variant==3) {
            // Valid LZO for the changed source dimensions would successfully
            // overwrite twice the metadata-sized BC destination without the guard.
            const auto changedCapacity=Capacity(fmt,8,4);
            bytes=DxtMip(8,4,changedCapacity+4,changedCapacity+4,true);
            bytes[7]=char(17+changedCapacity);
            for(size_t i=0;i<changedCapacity;++i) bytes[8+i]=char(i+13);
            bytes[8+changedCapacity]=char(0x11);bytes[9+changedCapacity]=0;bytes[10+changedCapacity]=0;
            std::vector<uint8_t> decoded(changedCapacity);
            REQUIRE(Poseidon::Foundation::Lzo1x::Decompress(reinterpret_cast<const uint8_t*>(bytes.data()+7),changedCapacity+4,decoded.data(),changedCapacity)==changedCapacity);
        }
        if(variant==5) bytes=DxtMip(4,0,20,20,true);
        if(variant==4) bytes.resize(5); // Header failure must not reach a payload write.
        Poseidon::QIStream in(bytes.data(),int(bytes.size())); std::array<uint8_t,64> output{};output.fill(0xa7);const auto expected=output;
        CHECK(level.LoadPaa(in,output.data()+8,&palette)==-1); CHECK(output==expected);
    }
    const auto bytes=DxtMip(4,4,8,8); auto level=DxtLevel(Poseidon::PacDXT1,Poseidon::PacDXT1,4,4);
    Poseidon::QIStream nullOut(bytes.data(),int(bytes.size())); CHECK(level.LoadPaa(nullOut,nullptr,&palette)==-1);
    Poseidon::QIStream failed(bytes.data(),int(bytes.size())); failed.seekg(1000,Poseidon::QIOS::beg);
    std::array<uint8_t,32> output{}; output.fill(0x62);const auto expected=output;
    CHECK(level.LoadPaa(failed,output.data()+8,&palette)==-1); CHECK(output==expected);
}

TEST_CASE("Raw DXT1 pixel conversion uses exact complete source and golden pixels", "[paa-dxt-bounds]")
{
    Poseidon::PacPalette palette; const auto level=DxtLevel(Poseidon::PacDXT1,Poseidon::PacARGB1555,4,4);
    auto bytes=DxtMip(4,4,8,8);
    const std::array<uint8_t,8> redBlock{0,0xf8,0xe0,7,0,0,0,0};std::memcpy(bytes.data()+7,redBlock.data(),8);
    Poseidon::QIStream in(bytes.data(),int(bytes.size())); std::array<uint16_t,24> output{};output.fill(0xa7a7);
    REQUIRE(level.LoadPaa(in,output.data()+4,&palette)==0);
    for(size_t i=0;i<output.size();++i) CHECK(output[i]==(i>=4&&i<20?0xfc00:0xa7a7));
    CHECK(in.tellg()==15);CHECK_FALSE(in.fail());
}

TEST_CASE("Unsupported short truncated and thin PAA conversions refuse before output", "[paa-dxt-bounds]")
{
    Poseidon::PacPalette palette;
    for(int variant=0;variant<8;++variant) {
        auto level=DxtLevel(Poseidon::PacDXT1,Poseidon::PacARGB1555,4,4);
        auto bytes=DxtMip(4,4,8,8);
        if(variant==0) bytes=DxtMip(4,4,7,7);
        if(variant==1) bytes=DxtMip(4,4,9,9);
        if(variant==2) bytes=DxtMip(4,4,8,7);
        if(variant==3) {level=DxtLevel(Poseidon::PacDXT3,Poseidon::PacARGB1555,4,4);bytes=DxtMip(4,4,16,16);}
        if(variant==4) level=DxtLevel(Poseidon::PacDXT1,Poseidon::PacARGB8888,4,4);
        if(variant==5) {level=DxtLevel(Poseidon::PacDXT1,Poseidon::PacARGB1555,2,2);bytes=DxtMip(2,2,8,8);}
        if(variant==6) {level=DxtLevel(Poseidon::PacDXT1,Poseidon::PacARGB1555,4,1);bytes=DxtMip(4,1,8,8);}
        if(variant==7) bytes=DxtMip(4,4,20,20,true); // BC LZO output must not pretend to be pixel conversion.
        Poseidon::QIStream in(bytes.data(),int(bytes.size())); std::array<uint16_t,40> output{};output.fill(0xa7a7);const auto expected=output;
        CHECK(level.LoadPaa(in,output.data()+4,&palette)==-1); CHECK(output==expected);
    }
}
