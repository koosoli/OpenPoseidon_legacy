#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Graphics/Rendering/Font/PaaLzoReplayCache.hpp>
#include <Poseidon/Graphics/Rendering/Font/Pactext.hpp>
#include <Poseidon/Foundation/Algorithms/Lzo1x.hpp>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>
using Poseidon::render::PaaLzoReplayCache;
namespace { uint64_t Collision(const uint8_t*,size_t) { return 5; } }

TEST_CASE("LZO replay requires every input byte and exact output capacity", "[paa-lzo-replay]")
{
    PaaLzoReplayCache cache(32*1024,4,Collision);
    const std::array<uint8_t,3> a{1,2,3}, b{1,2,4};
    const std::array<uint8_t,4> decoded{7,8,9,10}; std::array<uint8_t,5> out{};
    CHECK_FALSE(cache.TryCopy(a.data(),a.size(),4,out.data()));
    REQUIRE(cache.StoreSuccessful(a.data(),a.size(),4,decoded.data(),4));
    CHECK_FALSE(cache.TryCopy(b.data(),b.size(),4,out.data())); CHECK_FALSE(cache.TryCopy(a.data(),a.size(),5,out.data()));
    REQUIRE(cache.TryCopy(a.data(),a.size(),4,out.data())); CHECK(std::memcmp(out.data(),decoded.data(),4)==0);
    const auto stats=cache.Snapshot(); CHECK(stats.hits==1); CHECK(stats.misses==3); CHECK(stats.hitOutputBytes==4);
    CHECK(stats.entries==1); CHECK(stats.storeBytes==PaaLzoReplayCache::KnownMetadataBytes()+7);
    CHECK_FALSE(cache.StoreSuccessful(b.data(),b.size(),4,decoded.data(),0));
    CHECK_FALSE(cache.StoreSuccessful(b.data(),b.size(),4,decoded.data(),3)); CHECK(cache.Snapshot().entries==1);
    REQUIRE(cache.StoreSuccessful(b.data(),b.size(),4,decoded.data(),4)); CHECK(cache.Snapshot().entries==2);
}

TEST_CASE("LZO replay evicts before bounded allocation and respects LRU", "[paa-lzo-replay]")
{
    PaaLzoReplayCache cache(PaaLzoReplayCache::KnownMetadataBytes()+16,2,Collision);
    const std::array<uint8_t,4> a{1,0,0,0}, b{2,0,0,0}, c{3,0,0,0}, d{4,0,0,0}; std::array<uint8_t,4> out{};
    REQUIRE(cache.StoreSuccessful(a.data(),4,4,a.data(),4)); REQUIRE(cache.StoreSuccessful(b.data(),4,4,b.data(),4));
    REQUIRE(cache.TryCopy(a.data(),4,4,out.data())); REQUIRE(cache.StoreSuccessful(c.data(),4,4,c.data(),4));
    CHECK_FALSE(cache.TryCopy(b.data(),4,4,out.data())); REQUIRE(cache.TryCopy(a.data(),4,4,out.data()));
    auto stats=cache.Snapshot(); CHECK(stats.entries==2); CHECK(stats.evictions==1); CHECK(stats.peakStoreBytes<=PaaLzoReplayCache::KnownMetadataBytes()+16);
    CHECK_FALSE(cache.StoreSuccessful(d.data(),4,20,d.data(),20)); // never reads oversized output
    CHECK(cache.Snapshot().oversize==1); CHECK(cache.Snapshot().entries==2);
    PaaLzoReplayCache tooSmall(1); CHECK_FALSE(tooSmall.StoreSuccessful(a.data(),4,4,a.data(),4));
    CHECK(tooSmall.Snapshot().entries==0); CHECK(tooSmall.Snapshot().oversize==1);
}

TEST_CASE("LZO replay threads share one retained budget and no escaped payloads", "[paa-lzo-replay]")
{
    const size_t budget=PaaLzoReplayCache::KnownMetadataBytes()+64;
    PaaLzoReplayCache cache(budget,8,Collision); std::atomic<bool> valid{true}; std::array<std::thread,4> workers;
    for(size_t t=0;t<workers.size();++t) workers[t]=std::thread([&,t] {
        for(unsigned i=0;i<40;++i) {
            std::array<uint8_t,8> input{},output{}; input.fill(uint8_t(t*40+i));
            if(!cache.StoreSuccessful(input.data(),8,8,input.data(),8)) valid=false;
            if(cache.TryCopy(input.data(),8,8,output.data()) && input!=output) valid=false;
            if(cache.Snapshot().storeBytes>budget) valid=false;
        }
    });
    for(auto& worker:workers) worker.join(); CHECK(valid.load());
    auto stats=cache.Snapshot(); CHECK(stats.peakStoreBytes<=budget); CHECK(stats.entries<=4); CHECK(stats.evictions>0);
}

TEST_CASE("Production PAA replay opt-in preserves fresh bytes aliases and malformed decoding", "[paa-lzo-replay][paa-lzo-replay-production]")
{
    const char* flag=std::getenv("WGR_PAA_LZO_REPLAY_CACHE");
    if(!flag || std::strcmp(flag,"1")!=0) { SKIP("Requires fresh process WGR_PAA_LZO_REPLAY_CACHE=1 for actual production opt-in"); }
    Poseidon::PacLevelMem level; level._w=4; level._h=4; level._sFormat=Poseidon::PacDXT3; level._dFormat=Poseidon::PacDXT3;
    Poseidon::PacPalette palette;
    for(int value:{17,17,33}) for(size_t offset:{size_t(0),size_t(7),size_t(40)}) {
        std::vector<char> bytes{char(0x04),char(0x80),4,0,20,0,0,char(33)};
        for(int i=0;i<16;++i) bytes.push_back(char(value+i)); bytes.insert(bytes.end(),{char(0x11),0,0});
        bytes.resize(80,char(0x5a)); auto reference=bytes;
        std::array<uint8_t,16> expected{};
        REQUIRE(Poseidon::Foundation::Lzo1x::Decompress(reinterpret_cast<const uint8_t*>(reference.data()+7),20,expected.data(),16)==16);
        Poseidon::QIStream in(bytes.data(),int(bytes.size()));
        REQUIRE(level.LoadPaa(in,bytes.data()+offset,&palette)==0); CHECK(std::memcmp(bytes.data()+offset,expected.data(),16)==0);
        CHECK(in.tellg()==27); CHECK_FALSE(in.fail());
    }
    for(int repeat=0;repeat<2;++repeat) {
        std::vector<char> malformed{4,char(0x80),4,0,20,0,0,char(33)};
        for(int i=0;i<16;++i) malformed.push_back(char(i+77));
        malformed.insert(malformed.end(),{char(0x11),0,1});
        std::array<uint8_t,24> actual{},reference{}; actual.fill(0xa7); reference=actual;
        REQUIRE(Poseidon::Foundation::Lzo1x::Decompress(reinterpret_cast<const uint8_t*>(malformed.data()+7),20,reference.data(),16)==0);
        Poseidon::QIStream bad(malformed.data(),int(malformed.size()));
        CHECK(level.LoadPaa(bad,actual.data(),&palette)==-1); CHECK(actual==reference);
        CHECK(bad.tellg()==27); CHECK_FALSE(bad.fail());
    }
    std::vector<char> truncated{4,char(0x80),4,0,20,0,0,char(33),17,18};
    Poseidon::QIStream in(truncated.data(),int(truncated.size())); std::array<uint8_t,16> output{};
    CHECK(level.LoadPaa(in,output.data(),&palette)==-1); CHECK(in.fail());
}

TEST_CASE("Unsampled input mutation never becomes an exact replay hit", "[paa-lzo-replay]")
{
    // 512-byte fingerprint windows are [0,64), [224,288), [448,512).
    // Change a byte outside all windows. The default fingerprint therefore
    // collides; correctness must come from full memcmp, not the shortlist.
    PaaLzoReplayCache cache(32*1024,4);
    std::array<uint8_t,512> original{};
    for(size_t i=0;i<original.size();++i) original[i]=static_cast<uint8_t>(i*37+13);
    auto modified=original; modified[100]^=0x80;
    REQUIRE(std::memcmp(original.data(),modified.data(),64)==0);
    REQUIRE(std::memcmp(original.data()+224,modified.data()+224,64)==0);
    REQUIRE(std::memcmp(original.data()+448,modified.data()+448,64)==0);
    std::array<uint8_t,16> a{},b{},out{};
    a.fill(0x2a); b.fill(0xd3);
    REQUIRE(cache.StoreSuccessful(original.data(),original.size(),a.size(),a.data(),a.size()));
    CHECK_FALSE(cache.TryCopy(modified.data(),modified.size(),b.size(),out.data()));
    REQUIRE(cache.StoreSuccessful(modified.data(),modified.size(),b.size(),b.data(),b.size()));
    CHECK(cache.Snapshot().entries==2); // A sample collision cannot deduplicate distinct inputs.
    REQUIRE(cache.TryCopy(original.data(),original.size(),a.size(),out.data())); CHECK(out==a);
    REQUIRE(cache.TryCopy(modified.data(),modified.size(),b.size(),out.data())); CHECK(out==b);
    CHECK(cache.Snapshot().misses==1); CHECK(cache.Snapshot().hits==2);
}