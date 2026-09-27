#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Graphics/Rendering/GeometryPageClodDiskCodec.hpp>
#include <cstring>
using namespace Poseidon;
using namespace Poseidon::GeometryPages;
namespace
{
ClodRamPackage OriginalDiskPilot()
{
    ShapeExport source;source.source.sourceSha256[0]=61;source.source.vertexLayout=sizeof(SVertex);
    source.source.materialMapping=1;source.source.coarseRepresentation=0;source.source.fineRepresentation=1;
    constexpr uint32_t side=9;
    for(uint32_t y=0;y<side;++y)for(uint32_t x=0;x<side;++x) {
        const float z=.08f*x*x+.035f*y*y;SVertex vertex{};
        vertex.pos=Vector3P(float(x),float(y),z);vertex.norm=Vector3P(0,0,1);
        vertex.tangent=Vector3P(1,0,0);vertex.binormal=Vector3P(0,1,0);
        vertex.t0={float(x*x)/64,float(y*y)/64};vertex.t1=vertex.t0;
        source.fine.vertices.push_back(vertex);source.fine.positions.push_back({float(x),float(y),z});
    }
    for(uint32_t y=0;y<side-1;++y)for(uint32_t x=0;x<side-1;++x) {
        const auto a=y*side+x,b=a+1,c=a+side,d=c+1;
        source.fine.indices.insert(source.fine.indices.end(),{a,b,d,a,d,c});source.fine.materials.insert(source.fine.materials.end(),2,5);
    }
    ClodBake bake;REQUIRE(BakeClodPilot(source,true,bake)==ClodBakeStatus::Baked);
    float least=FLT_MAX,most=0;
    for(const auto& group:bake.groups)if(group.simplified.error>0 && group.simplified.error<FLT_MAX && std::isfinite(group.simplified.error))
        {least=std::min(least,group.simplified.error);most=std::max(most,group.simplified.error);}
    REQUIRE(least<FLT_MAX);REQUIRE(most>0);
    const float coarseThreshold=std::nextafter(most,std::numeric_limits<float>::infinity()),fineThreshold=std::nextafter(least,0.f);
    ClodCut coarseSelection,fineSelection;REQUIRE(SelectClodCut(bake,coarseThreshold,coarseSelection));REQUIRE(SelectClodCut(bake,fineThreshold,fineSelection));
    ClodDecodedCut coarse,fine;
    REQUIRE(DecodeClodCut(bake,coarseThreshold,coarseSelection,coarse)==ClodCutDecodeStatus::Decoded);
    REQUIRE(DecodeClodCut(bake,fineThreshold,fineSelection,fine)==ClodCutDecodeStatus::Decoded);
    ClodRamPackage value;REQUIRE(BuildClodRamPackage(bake,coarseThreshold,fineThreshold,coarse,fine,value)==ClodRamStatus::Built);return value;
}
void CheckMesh(const ExportedMesh& a,const ExportedMesh& b)
{
    REQUIRE(a.vertices.size()==b.vertices.size());REQUIRE(a.positions.size()==b.positions.size());
    REQUIRE(std::memcmp(a.vertices.data(),b.vertices.data(),a.vertices.size()*sizeof(SVertex))==0);
    REQUIRE(a.indices==b.indices);REQUIRE(a.materials==b.materials);
    for(size_t i=0;i<a.positions.size();++i) {
        REQUIRE(std::bit_cast<uint32_t>(a.positions[i].x)==std::bit_cast<uint32_t>(b.positions[i].x));
        REQUIRE(std::bit_cast<uint32_t>(a.positions[i].y)==std::bit_cast<uint32_t>(b.positions[i].y));
        REQUIRE(std::bit_cast<uint32_t>(a.positions[i].z)==std::bit_cast<uint32_t>(b.positions[i].z));
    }
}
void Put32(std::vector<uint8_t>& bytes,size_t at,uint32_t value)
{REQUIRE(at+4<=bytes.size());for(unsigned i=0;i<4;++i)bytes[at+i]=uint8_t(value>>(8*i));}
void Seal(std::vector<uint8_t>& bytes)
{
    REQUIRE(bytes.size()>=ClodDiskDetail::HeaderBytes);Put32(bytes,8,uint32_t(bytes.size()));
    const auto digest=ClodDiskDetail::Checksum(bytes);std::memcpy(bytes.data()+ClodDiskDetail::DigestAt,digest.data(),32);
}
}
TEST_CASE("selected CLOD disk codec roundtrips original accepted paired cuts and existing decoders", "[geometry-clod-disk]")
{
    const auto value=OriginalDiskPilot();std::vector<uint8_t> encoded;
    REQUIRE(EncodeClodDisk(value,encoded)==ClodDiskStatus::Encoded);REQUIRE(encoded.size()<=128*1024);
    ClodRamPackage decoded;REQUIRE(DecodeClodDisk(encoded,value.originalSource,value.package.Identity(),decoded)==ClodDiskStatus::Decoded);
    REQUIRE(decoded.originalSource==value.originalSource);REQUIRE(decoded.package.Identity()==value.package.Identity());
    REQUIRE(decoded.coarseClusters==value.coarseClusters);REQUIRE(decoded.fineClusters==value.fineClusters);
    REQUIRE(decoded.coarseThreshold==value.coarseThreshold);REQUIRE(decoded.fineThreshold==value.fineThreshold);
    CheckMesh(value.selectedGeometry.coarse,decoded.selectedGeometry.coarse);CheckMesh(value.selectedGeometry.fine,decoded.selectedGeometry.fine);
    REQUIRE(decoded.package.pages.size()==value.package.pages.size());
    for(size_t i=0;i<value.package.pages.size();++i)REQUIRE(decoded.package.pages[i].bytes==value.package.pages[i].bytes);
    REQUIRE(decoded.knownCapacityBytes==ClodRamKnownBytes(decoded));REQUIRE(decoded.knownCapacityBytes<=128*1024);
    for(auto frontier:{Frontier::Coarse,Frontier::Fine}) {
        ResidentRepresentation a,b;
        REQUIRE(DecodeResidentRepresentation(value.package,value.package.Identity(),frontier,value.selectedGeometry,a)==DecodeStatus::Decoded);
        REQUIRE(DecodeResidentRepresentation(decoded.package,decoded.package.Identity(),frontier,decoded.selectedGeometry,b)==DecodeStatus::Decoded);
        REQUIRE(a.indices==b.indices);REQUIRE(a.vertexRecords==b.vertexRecords);REQUIRE(a.clusters==b.clusters);
    }
    std::vector<uint8_t> again;REQUIRE(EncodeClodDisk(decoded,again)==ClodDiskStatus::Encoded);REQUIRE(again==encoded);
}
TEST_CASE("disk codec rejects stale keys source cut tampering and unsupported layout without publication", "[geometry-clod-disk]")
{
    const auto value=OriginalDiskPilot();std::vector<uint8_t> encoded;REQUIRE(EncodeClodDisk(value,encoded)==ClodDiskStatus::Encoded);
    ClodRamPackage destination;destination.originalSource.sourceSha256[0]=211;
    SECTION("stale raw source") {
        auto expected=value.originalSource;expected.sourceSha256[0]^=1;
        REQUIRE(DecodeClodDisk(encoded,expected,value.package.Identity(),destination)==ClodDiskStatus::Invalid);
    }
    SECTION("stale derived cut") {
        auto expected=value.package.Identity();expected.source.sourceSha256[0]^=1;
        REQUIRE(DecodeClodDisk(encoded,value.originalSource,expected,destination)==ClodDiskStatus::Invalid);
    }
    SECTION("stale packing") {
        auto expected=value.package.Identity();expected.packing.clusterVertices+=1;
        REQUIRE(DecodeClodDisk(encoded,value.originalSource,expected,destination)==ClodDiskStatus::Invalid);
    }
    SECTION("raw identity rehashed inside file cannot authenticate itself") {
        encoded[ClodDiskDetail::HeaderBytes]^=1;Seal(encoded);
        REQUIRE(DecodeClodDisk(encoded,value.originalSource,value.package.Identity(),destination)==ClodDiskStatus::Invalid);
    }
    SECTION("derived identity rehashed inside file") {
        encoded[ClodDiskDetail::HeaderBytes+68]^=1;Seal(encoded);
        REQUIRE(DecodeClodDisk(encoded,value.originalSource,value.package.Identity(),destination)==ClodDiskStatus::Invalid);
    }
    SECTION("layout schema bake package versions") {
        for(size_t field:{4u,12u,16u,24u,28u,32u,36u,40u}) {
            auto broken=encoded;broken[field]^=1;Seal(broken);
            REQUIRE(DecodeClodDisk(broken,value.originalSource,value.package.Identity(),destination)==ClodDiskStatus::Unsupported);
        }
    }
    REQUIRE(destination.originalSource.sourceSha256[0]==211);REQUIRE(destination.package.pages.empty());
}
TEST_CASE("bounded disk parser rejects malformed rehashed bodies and incomplete data transactionally", "[geometry-clod-disk]")
{
    const auto value=OriginalDiskPilot();std::vector<uint8_t> encoded;REQUIRE(EncodeClodDisk(value,encoded)==ClodDiskStatus::Encoded);
    ClodRamPackage destination;destination.originalSource.sourceSha256[0]=187;
    SECTION("missing truncated and capacity input") {
        for(size_t length:{size_t(0),size_t(1),ClodDiskDetail::HeaderBytes-1,encoded.size()-1}) {
            REQUIRE(DecodeClodDisk(std::span<const uint8_t>(encoded.data(),length),value.originalSource,value.package.Identity(),destination)==ClodDiskStatus::Invalid);
        }
        std::vector<uint8_t> huge(128*1024+1,0);
        REQUIRE(DecodeClodDisk(huge,value.originalSource,value.package.Identity(),destination)==ClodDiskStatus::Capacity);
    }
    SECTION("trailing bytes even with valid total and checksum") {
        encoded.push_back(0);Seal(encoded);
        REQUIRE(DecodeClodDisk(encoded,value.originalSource,value.package.Identity(),destination)==ClodDiskStatus::Invalid);
    }
    SECTION("checksum corruption") {
        encoded.back()^=1;
        REQUIRE(DecodeClodDisk(encoded,value.originalSource,value.package.Identity(),destination)==ClodDiskStatus::Invalid);
    }
    SECTION("malformed page with valid file checksum") {
        encoded.back()^=1;Seal(encoded);
        REQUIRE(DecodeClodDisk(encoded,value.originalSource,value.package.Identity(),destination)==ClodDiskStatus::Invalid);
    }
    SECTION("nonfinite threshold with valid checksum") {
        Put32(encoded,ClodDiskDetail::HeaderBytes+68+68+12,0x7fc00000u);Seal(encoded);
        REQUIRE(DecodeClodDisk(encoded,value.originalSource,value.package.Identity(),destination)==ClodDiskStatus::Invalid);
    }
    SECTION("reference count refusal before vertex allocation") {
        const size_t firstMesh=ClodDiskDetail::HeaderBytes+68+68+12+8+8+
            (value.coarseClusters.size()+value.fineClusters.size())*4;
        Put32(encoded,firstMesh,UINT32_MAX);Seal(encoded);
        REQUIRE(DecodeClodDisk(encoded,value.originalSource,value.package.Identity(),destination)==ClodDiskStatus::Invalid);
    }
    SECTION("rehashed raw attribute mutation cannot match derived key") {
        const size_t firstVertex=ClodDiskDetail::HeaderBytes+68+68+12+8+8+
            (value.coarseClusters.size()+value.fineClusters.size())*4+12;
        encoded[firstVertex+offsetof(SVertex,t0)]^=1;Seal(encoded);
        REQUIRE(DecodeClodDisk(encoded,value.originalSource,value.package.Identity(),destination)==ClodDiskStatus::Invalid);
    }
    SECTION("cut count refusal before reference arrays") {
        const size_t firstCount=ClodDiskDetail::HeaderBytes+68+68+12+8;Put32(encoded,firstCount,UINT32_MAX);Seal(encoded);
        REQUIRE(DecodeClodDisk(encoded,value.originalSource,value.package.Identity(),destination)==ClodDiskStatus::Invalid);
    }
    REQUIRE(destination.originalSource.sourceSha256[0]==187);REQUIRE(destination.package.pages.empty());
}
TEST_CASE("disk encoding validates page partitions attributes and claimed derived provenance before output", "[geometry-clod-disk]")
{
    auto value=OriginalDiskPilot();std::vector<uint8_t> output{7,9,11};
    SECTION("mutated source") {value.originalSource.sourceSha256[0]^=1;}
    SECTION("nonfinite selected normal") {value.selectedGeometry.fine.vertices[0].norm=Vector3P(std::numeric_limits<float>::quiet_NaN(),0,1);}
    SECTION("mutated selected attribute") {value.selectedGeometry.fine.vertices[0].t0.u+=1;}
    SECTION("mutated paired cluster ID") {value.coarseClusters[0]^=1;}
    SECTION("invalid partition") {value.package.coarsePages=uint32_t(value.package.pages.size());}
    SECTION("invalid descriptor range") {value.package.clusters[0].byteLength=UINT32_MAX;}
    SECTION("mutated immutable page") {value.package.pages[0].bytes.back()^=1;}
    REQUIRE(EncodeClodDisk(value,output)==ClodDiskStatus::Invalid);REQUIRE((output==std::vector<uint8_t>{7,9,11}));
}

TEST_CASE("disk vertex field codec preserves explicit LE wire order and exact finite float bits", "[geometry-clod-disk]")
{
    const auto f=[](uint32_t bits){return std::bit_cast<float>(bits);};
    SVertex source;
    source.pos=Vector3P(f(0x80000000u),f(0x00000001u),f(0xbf812345u));
    source.norm=Vector3P(f(0x3f123456u),f(0x80000001u),f(0x3f800000u));
    source.t0={f(0x80000000u),f(0x3e654321u)};source.conform=0x12345678u;
    source.tangent=Vector3P(f(0x00800001u),f(0xc0123456u),f(0x00000000u));
    source.binormal=Vector3P(f(0x3f800001u),f(0x80000000u),f(0x3f123456u));
    source.t1={f(0x00000001u),f(0xbe654321u)};
    ClodDiskDetail::Writer writer;ClodDiskDetail::WriteVertex(writer,source);
    REQUIRE(writer.data.size()==68);
    // This comparison only READS existing objects as bytes. Construction uses
    // typed fields; there is no memcpy into Vector3P or SVertex.
    REQUIRE(std::memcmp(writer.data.data(),&source,sizeof(source))==0);
    const std::array<uint32_t,17> words={0x80000000u,0x00000001u,0xbf812345u,0x3f123456u,0x80000001u,0x3f800000u,
        0x80000000u,0x3e654321u,0x12345678u,0x00800001u,0xc0123456u,0x00000000u,
        0x3f800001u,0x80000000u,0x3f123456u,0x00000001u,0xbe654321u};
    for(size_t i=0;i<words.size();++i)for(unsigned byte=0;byte<4;++byte)
        REQUIRE(writer.data[i*4+byte]==uint8_t(words[i]>>(byte*8)));
    ClodDiskDetail::Reader reader{writer.data};const auto decoded=ClodDiskDetail::ReadVertex(reader);
    REQUIRE(reader.valid);REQUIRE(reader.Left()==0);REQUIRE(std::memcmp(&source,&decoded,sizeof(source))==0);
    REQUIRE(std::bit_cast<uint32_t>(decoded.pos.X())==0x80000000u);
    REQUIRE(std::bit_cast<uint32_t>(decoded.pos.Y())==0x00000001u);
    REQUIRE(std::bit_cast<uint32_t>(decoded.t1.v)==0xbe654321u);
}
