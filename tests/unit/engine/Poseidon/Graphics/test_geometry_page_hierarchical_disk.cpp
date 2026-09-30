#include <catch2/catch_test_macros.hpp>
#include <stdexcept>
#include <Poseidon/Graphics/Rendering/GeometryPageHierarchicalDiskCodec.hpp>
#include <Poseidon/Graphics/Rendering/GeometryPageHierarchicalDiskSource.hpp>
#ifdef _WIN32
#include <Windows.h>
#endif
using namespace Poseidon;
using namespace Poseidon::GeometryPages;
namespace
{
HierarchicalPackage DiskHierarchy()
{
    ShapeExport source;source.source.sourceSha256[0]=91;source.source.vertexLayout=sizeof(SVertex);
    source.source.materialMapping=1;source.source.coarseRepresentation=0;source.source.fineRepresentation=1;
    constexpr uint32_t side=17;
    for(uint32_t y=0;y<side;++y)for(uint32_t x=0;x<side;++x) {
        const float a=float(x)/16,b=float(y)/16,z=.9f*(1-a*a)*(1-b*b);SVertex v{};
        v.pos=Vector3P(a*4,b*4,z);v.norm=Vector3P(0,0,1);v.tangent=Vector3P(1,0,0);v.binormal=Vector3P(0,1,0);
        v.t0={a*a,b*b};v.t1={-0.f,b};source.fine.vertices.push_back(v);source.fine.positions.push_back({a*4,b*4,z});
    }
    for(uint32_t y=0;y<side-1;++y)for(uint32_t x=0;x<side-1;++x) {
        const auto a=y*side+x,b=a+1,c=a+side,d=c+1;
        source.fine.indices.insert(source.fine.indices.end(),{a,b,d,a,d,c});source.fine.materials.insert(source.fine.materials.end(),2,4);
    }
    source.fine.vertices.push_back(source.fine.vertices.front());source.fine.positions.push_back(source.fine.positions.front());
    source.fine.vertices.back().t0={7,7};source.fine.indices[0]=uint32_t(source.fine.vertices.size()-1);
    ClodBake bake;REQUIRE(BakeClodPilot(source,true,bake)==ClodBakeStatus::Baked);
    HierarchicalPackage package;REQUIRE(BuildHierarchicalPackage(bake,package)==HierarchicalBuildStatus::Built);return package;
}
uint64_t ReadNumber(std::span<const uint8_t> bytes,size_t at,unsigned width=4)
{uint64_t n=0;for(unsigned i=0;i<width;++i)n|=uint64_t(bytes[at+i])<<(i*8);return n;}
void WriteNumber(std::vector<uint8_t>& bytes,size_t at,uint64_t n,unsigned width=4)
{for(unsigned i=0;i<width;++i)bytes[at+i]=uint8_t(n>>(i*8));}
size_t MetadataBytes(const HierarchicalDiskImage& image){return size_t(ReadNumber(image.bytes,204,8));}
std::span<const uint8_t> Prefix(const HierarchicalDiskImage& image){return {image.bytes.data(),MetadataBytes(image)};}
std::span<const uint8_t> PageBytes(const HierarchicalDiskImage& image,const HierarchicalDiskManifest& manifest,uint32_t p)
{const auto& range=manifest.pages[p];return {image.bytes.data()+range.offset,range.bytes};}
HierarchicalDiskImage Image(const HierarchicalPackage& package)
{HierarchicalDiskImage image;REQUIRE(EncodeHierarchicalDisk(package,image)==HierarchicalDiskStatus::Encoded);return image;}
HierarchicalDiskManifest Manifest(const HierarchicalDiskImage& image)
{HierarchicalDiskManifest value;REQUIRE(DecodeHierarchicalManifest(Prefix(image),image.bytes.size(),image.identity,image.metadataSha256,value)==HierarchicalDiskStatus::Decoded);return value;}
void CheckVertex(const SVertex& a,const SVertex& b)
{
    const auto check=[](float a,float b){REQUIRE(std::bit_cast<uint32_t>(a)==std::bit_cast<uint32_t>(b));};
    for(const auto pair:{std::pair{&a.pos,&b.pos},std::pair{&a.norm,&b.norm},std::pair{&a.tangent,&b.tangent},std::pair{&a.binormal,&b.binormal}})
    {check(pair.first->X(),pair.second->X());check(pair.first->Y(),pair.second->Y());check(pair.first->Z(),pair.second->Z());}
    check(a.t0.u,b.t0.u);check(a.t0.v,b.t0.v);check(a.t1.u,b.t1.u);check(a.t1.v,b.t1.v);REQUIRE(a.conform==b.conform);
}
// Deliberately sign a malformed producer record to exercise structural rejection
// after integrity succeeded. Production expected digests are external authority.
void RefreshMetadataDigest(HierarchicalDiskImage& image)
{image.metadataSha256=HierarchicalDiskDetail::Hash(Prefix(image));}
void RefreshPageDigest(HierarchicalDiskImage& image,const HierarchicalDiskManifest& manifest,uint32_t p)
{
    const auto digest=HierarchicalDiskDetail::Hash(PageBytes(image,manifest,p));
    const size_t row=HierarchicalDiskDetail::HeaderBytes+manifest.metadata.groups.size()*HierarchicalDiskDetail::GroupBytes+
        manifest.metadata.clusters.size()*HierarchicalDiskDetail::ClusterBytes+p*HierarchicalDiskDetail::PageRowBytes;
    std::copy(digest.begin(),digest.end(),image.bytes.begin()+row+36);RefreshMetadataDigest(image);
}
}
#ifdef _WIN32
namespace
{
class HeldDeleteHierarchyFixture
{
    std::filesystem::path root_,directory_;
    HANDLE held_=INVALID_HANDLE_VALUE;
public:
    HeldDeleteHierarchyFixture()
    {
        root_=std::filesystem::canonical(std::filesystem::temp_directory_path());
        static std::atomic<uint64_t> serial{0};
        for(unsigned attempt=0;attempt<64;++attempt) {
            auto candidate=root_/("OpenPoseidon-hierarchy-owned-"+std::to_string(GetCurrentProcessId())+"-"+
                std::to_string(GetTickCount64())+"-"+std::to_string(++serial));
            if(std::filesystem::create_directory(candidate)){directory_=std::move(candidate);return;}
        }
        throw std::runtime_error("cannot reserve owned hierarchy fixture directory");
    }
    ~HeldDeleteHierarchyFixture()
    {
        if(held_!=INVALID_HANDLE_VALUE) {
            FILE_DISPOSITION_INFO disposition{};disposition.DeleteFile=TRUE;
            SetFileInformationByHandle(held_,FileDispositionInfo,&disposition,sizeof(disposition));
            CloseHandle(held_);
        }
        std::error_code error;const auto resolved=std::filesystem::canonical(directory_,error);
        if(error||resolved!=directory_||resolved.parent_path()!=root_||
           resolved.filename().string().rfind("OpenPoseidon-hierarchy-owned-",0)!=0)return;
        std::filesystem::remove(resolved/"sentinel.txt",error);
        std::filesystem::remove(resolved,error); // never recursively remove unrelated entries
    }
    std::filesystem::path Path()const{return directory_/"hierarchy.ghp";}
    std::filesystem::path Sentinel()const{return directory_/"sentinel.txt";}
    bool Write(std::span<const uint8_t> bytes)
    {
        if(held_!=INVALID_HANDLE_VALUE||bytes.empty()||bytes.size()>1024*1024)return false;
        const auto path=Path();
        held_=CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE|DELETE,FILE_SHARE_READ,
            nullptr,CREATE_NEW,FILE_ATTRIBUTE_TEMPORARY,nullptr);
        if(held_==INVALID_HANDLE_VALUE)return false;
        DWORD written=0;
        return WriteFile(held_,bytes.data(),DWORD(bytes.size()),&written,nullptr)&&
            written==bytes.size()&&FlushFileBuffers(held_);
    }
    bool DeleteExactHeldFile()
    {
        if(held_==INVALID_HANDLE_VALUE)return false;
        FILE_DISPOSITION_INFO disposition{};disposition.DeleteFile=TRUE;
        if(!SetFileInformationByHandle(held_,FileDispositionInfo,&disposition,sizeof(disposition)))return false;
        const bool closed=CloseHandle(held_)!=0;held_=INVALID_HANDLE_VALUE;return closed;
    }
};
}
TEST_CASE("Windows independent hierarchy read shares the held DELETE lease without permitting replacement",
          "[geometry-page-hierarchical-disk][geometry-page-hierarchy-held-delete]")
{
    // Trusted private producer image only; neither filesystem ownership nor
    // successful decoding certifies a live model, material or GPU admission.
    const auto package=DiskHierarchy();const auto image=Image(package);const auto manifest=Manifest(image);
    HeldDeleteHierarchyFixture files;REQUIRE(files.Write(image.bytes));
    {std::ofstream sentinel(files.Sentinel(),std::ios::binary);sentinel<<"preserve-neighbor";REQUIRE(bool(sentinel));}
    const auto path=files.Path();
    HANDLE writer=CreateFileW(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    const DWORD writerError=writer==INVALID_HANDLE_VALUE?GetLastError():ERROR_SUCCESS;
    if(writer!=INVALID_HANDLE_VALUE)CloseHandle(writer);
    REQUIRE(writer==INVALID_HANDLE_VALUE);REQUIRE(writerError==ERROR_SHARING_VIOLATION);
    const bool deleted=DeleteFileW(path.c_str())!=0;
    const DWORD deleteError=deleted?ERROR_SUCCESS:GetLastError();
    REQUIRE_FALSE(deleted);REQUIRE(deleteError==ERROR_SHARING_VIOLATION);
    HierarchicalDiskPageInput input;input.path=path;
    input.metadataBytes.assign(image.bytes.begin(),image.bytes.begin()+manifest.metadataBytes);
    input.expectedIdentity=image.identity;input.expectedMetadataSha256=image.metadataSha256;
    input.fileBytes=image.bytes.size();input.pageId=package.rootPages.front();
    std::atomic<bool> cancelled{false};HierarchicalPage page;
    REQUIRE(ReadHierarchicalDiskPage(input,page,cancelled)==HierarchicalDiskReadStatus::Read);
    const auto& expected=package.pages[input.pageId];
    REQUIRE(page.group==expected.group);REQUIRE(page.clusters.size()==expected.clusters.size());
    for(size_t c=0;c<page.clusters.size();++c) {
        REQUIRE(page.clusters[c].cluster==expected.clusters[c].cluster);
        REQUIRE(page.clusters[c].material==expected.clusters[c].material);
        REQUIRE(page.clusters[c].vertices.size()==expected.clusters[c].vertices.size());
        REQUIRE(page.clusters[c].originalVertexIds==expected.clusters[c].originalVertexIds);
        REQUIRE(page.clusters[c].indices==expected.clusters[c].indices);
        for(size_t v=0;v<page.clusters[c].vertices.size();++v)
            CheckVertex(page.clusters[c].vertices[v],expected.clusters[c].vertices[v]);
    }
    REQUIRE(files.DeleteExactHeldFile());REQUIRE_FALSE(std::filesystem::exists(path));
    std::ifstream sentinel(files.Sentinel(),std::ios::binary);
    std::string contents;std::getline(sentinel,contents);REQUIRE(contents=="preserve-neighbor");
    HierarchicalPage retained=page;
    REQUIRE(ReadHierarchicalDiskPage(input,retained,cancelled)==HierarchicalDiskReadStatus::Missing);
    REQUIRE(retained.clusters.front().originalVertexIds==page.clusters.front().originalVertexIds);
}
#endif
TEST_CASE("Hierarchical independent disk pages preserve graph and exact scalar vertex records", "[geometry-page-hierarchical-disk]")
{
    const auto package=DiskHierarchy();const auto image=Image(package);const auto manifest=Manifest(image);
    REQUIRE(manifest.metadata.identity==package.identity);REQUIRE(manifest.metadata.groups.size()==package.groups.size());
    REQUIRE(manifest.metadata.clusters.size()==package.clusters.size());REQUIRE(manifest.metadata.rootClusters==package.rootClusters);
    REQUIRE(manifest.metadata.rootPages==package.rootPages);REQUIRE(manifest.knownCapacityBytes<=128*1024);
    uint64_t next=manifest.metadataBytes;bool seam=false;
    for(uint32_t p=0;p<package.pages.size();++p) {
        REQUIRE(manifest.pages[p].offset==next);REQUIRE(manifest.pages[p].bytes<=65536);next+=manifest.pages[p].bytes;
        for(const auto& stub:manifest.metadata.pages[p].clusters){REQUIRE(stub.vertices.empty());REQUIRE(stub.indices.empty());}
        HierarchicalPage page;REQUIRE(DecodeHierarchicalDiskPage(manifest,p,PageBytes(image,manifest,p),page)==HierarchicalDiskStatus::Decoded);
        REQUIRE(HierarchicalDiskPageKnownBytes(page)<=128*1024);
        REQUIRE(page.group==package.pages[p].group);REQUIRE(page.logicalBytes==package.pages[p].logicalBytes);
        REQUIRE(page.uploadBytes==package.pages[p].uploadBytes);REQUIRE(page.clusters.size()==package.pages[p].clusters.size());
        for(size_t c=0;c<page.clusters.size();++c) {
            const auto& actual=page.clusters[c];const auto& expected=package.pages[p].clusters[c];
            REQUIRE(actual.cluster==expected.cluster);REQUIRE(actual.material==expected.material);
            REQUIRE(actual.originalVertexIds==expected.originalVertexIds);REQUIRE(actual.indices==expected.indices);
            for(size_t v=0;v<actual.vertices.size();++v){CheckVertex(actual.vertices[v],expected.vertices[v]);
                if(actual.vertices[v].t0.u==7&&actual.vertices[v].t0.v==7)seam=true;}
        }
    }
    REQUIRE(seam);REQUIRE(next==image.bytes.size());
}
TEST_CASE("Metadata-only disk frontier retains root fallback during independent partial page reads", "[geometry-page-hierarchical-disk]")
{
    const auto package=DiskHierarchy();auto image=Image(package);const auto manifest=Manifest(image);
    std::vector<uint8_t> resident(package.pages.size());HierarchicalCutPlan plan;
    REQUIRE(PlanHierarchicalCut(manifest.metadata,0,{package.identity,resident},plan));REQUIRE(plan.state==HierarchicalCutState::MissingRoots);
    // Only root ranges are supplied to the decoder. Fine ranges may be absent or
    // corrupt without preventing safe publication of the independent coarse cut.
    for(auto p:package.rootPages){HierarchicalPage page;REQUIRE(DecodeHierarchicalDiskPage(manifest,p,PageBytes(image,manifest,p),page)==HierarchicalDiskStatus::Decoded);resident[p]=1;}
    REQUIRE(PlanHierarchicalCut(manifest.metadata,0,{package.identity,resident},plan));REQUIRE(plan.state==HierarchicalCutState::RootFallback);
    REQUIRE(plan.selectedClusters==package.rootClusters);REQUIRE_FALSE(plan.missingPages.empty());
    const auto failed=plan.missingPages.front();image.bytes[manifest.pages[failed].offset+manifest.pages[failed].bytes-1]^=1;
    HierarchicalPage retained=package.pages[package.rootPages.front()];const auto retainedCluster=retained.clusters.front().cluster;
    REQUIRE(DecodeHierarchicalDiskPage(manifest,failed,PageBytes(image,manifest,failed),retained)==HierarchicalDiskStatus::Invalid);
    REQUIRE(retained.clusters.front().cluster==retainedCluster);
    REQUIRE(PlanHierarchicalCut(manifest.metadata,0,{package.identity,resident},plan));REQUIRE(plan.selectedClusters==package.rootClusters);
    image.bytes[manifest.pages[failed].offset+manifest.pages[failed].bytes-1]^=1;
    for(auto p:plan.missingPages){HierarchicalPage page;REQUIRE(DecodeHierarchicalDiskPage(manifest,p,PageBytes(image,manifest,p),page)==HierarchicalDiskStatus::Decoded);resident[p]=1;}
    REQUIRE(PlanHierarchicalCut(manifest.metadata,0,{package.identity,resident},plan));REQUIRE(plan.state==HierarchicalCutState::RequestedCut);
    std::vector<uint8_t> all(package.pages.size(),1);HierarchicalCutPlan reference;
    REQUIRE(PlanHierarchicalCut(package,0,{package.identity,all},reference));REQUIRE(plan.selectedClusters==reference.selectedClusters);
}
TEST_CASE("Hierarchical metadata refuses wrong authority, layout, DAG, range alias and overflow transactionally", "[geometry-page-hierarchical-disk]")
{
    auto image=Image(DiskHierarchy());auto retained=Manifest(image);const auto identity=retained.metadata.identity;
    auto expected=image.identity;auto expectedHash=image.metadataSha256;uint64_t total=image.bytes.size();
    const auto row=HierarchicalDiskDetail::HeaderBytes+retained.metadata.groups.size()*HierarchicalDiskDetail::GroupBytes+
        retained.metadata.clusters.size()*HierarchicalDiskDetail::ClusterBytes;
    SECTION("foreign identity"){expected.source.materialOptions^=1;}
    SECTION("missing digest"){expectedHash.fill(0);}
    SECTION("corrupt authenticated metadata"){image.bytes[0]^=1;}
    SECTION("wrong native vertex layout"){WriteNumber(image.bytes,16,sizeof(SVertex)+4);RefreshMetadataDigest(image);expectedHash=image.metadataSha256;}
    SECTION("wrong pinned library"){image.bytes[128]^=1;RefreshMetadataDigest(image);expectedHash=image.metadataSha256;}
    SECTION("group cycle"){const auto cluster=HierarchicalDiskDetail::HeaderBytes+retained.metadata.groups.size()*HierarchicalDiskDetail::GroupBytes;
        WriteNumber(image.bytes,cluster+12,0);RefreshMetadataDigest(image);expectedHash=image.metadataSha256;}
    SECTION("aliased page range"){REQUIRE(retained.pages.size()>1);WriteNumber(image.bytes,row+HierarchicalDiskDetail::PageRowBytes+24,retained.pages[0].offset,8);
        RefreshMetadataDigest(image);expectedHash=image.metadataSha256;}
    SECTION("overflowing page end"){WriteNumber(image.bytes,row+24,UINT64_MAX-4,8);RefreshMetadataDigest(image);expectedHash=image.metadataSha256;}
    SECTION("hole before page"){WriteNumber(image.bytes,row+24,retained.pages[0].offset+1,8);RefreshMetadataDigest(image);expectedHash=image.metadataSha256;}
    SECTION("oversized page"){WriteNumber(image.bytes,row+32,65537);RefreshMetadataDigest(image);expectedHash=image.metadataSha256;}
    SECTION("trailing file data"){++total;}
    REQUIRE(DecodeHierarchicalManifest(Prefix(image),total,expected,expectedHash,retained)!=HierarchicalDiskStatus::Decoded);
    REQUIRE(retained.metadata.identity==identity);
}
TEST_CASE("Independent page decoder rejects authenticated malformed controls and payloads", "[geometry-page-hierarchical-disk]")
{
    const auto package=DiskHierarchy();auto image=Image(package);const auto original=Manifest(image);const uint32_t p=0;
    const auto at=size_t(original.pages[p].offset);const auto vertices=ReadNumber(image.bytes,at+24+8);
    REQUIRE(vertices>1);
    SECTION("page ID"){WriteNumber(image.bytes,at+8,UINT32_MAX);}
    SECTION("cluster ID"){WriteNumber(image.bytes,at+24,UINT32_MAX);}
    SECTION("vertex count"){WriteNumber(image.bytes,at+24+8,65);}
    SECTION("triangle index count"){WriteNumber(image.bytes,at+24+12,1);}
    SECTION("duplicate original IDs"){WriteNumber(image.bytes,at+24+16+4,ReadNumber(image.bytes,at+24+16));}
    SECTION("nonfinite vertex"){WriteNumber(image.bytes,at+24+16+vertices*4,0x7fc00000);}
    SECTION("terrain conform"){WriteNumber(image.bytes,at+24+16+vertices*4+32,1);}
    SECTION("invalid local index"){WriteNumber(image.bytes,at+24+16+vertices*(4+HierarchicalDiskDetail::VertexScalarBytes),uint32_t(vertices));}
    RefreshPageDigest(image,original,p);const auto manifest=Manifest(image);
    HierarchicalPage retained=package.pages.back();const auto cluster=retained.clusters.front().cluster;
    REQUIRE(DecodeHierarchicalDiskPage(manifest,p,PageBytes(image,manifest,p),retained)==HierarchicalDiskStatus::Invalid);
    REQUIRE(retained.clusters.front().cluster==cluster);
}
TEST_CASE("Independent hierarchical reads refuse truncated, extended, swapped and corrupt page bytes", "[geometry-page-hierarchical-disk]")
{
    const auto package=DiskHierarchy();auto image=Image(package);auto manifest=Manifest(image);REQUIRE(manifest.pages.size()>1);
    HierarchicalPage retained=package.pages.back();const auto cluster=retained.clusters.front().cluster;auto bytes=PageBytes(image,manifest,0);
    SECTION("truncated"){bytes=bytes.first(bytes.size()-1);}
    SECTION("extended"){bytes={bytes.data(),bytes.size()+1};}
    SECTION("swapped"){bytes=PageBytes(image,manifest,1);}
    SECTION("corrupt"){image.bytes[manifest.pages[0].offset+manifest.pages[0].bytes-1]^=1;}
    REQUIRE(DecodeHierarchicalDiskPage(manifest,0,bytes,retained)==HierarchicalDiskStatus::Invalid);REQUIRE(retained.clusters.front().cluster==cluster);
    auto metadata=Prefix(image);REQUIRE(DecodeHierarchicalManifest(metadata.first(metadata.size()-1),image.bytes.size(),image.identity,image.metadataSha256,
        manifest)!=HierarchicalDiskStatus::Decoded);
}
TEST_CASE("Hierarchical disk producer refuses invalid payload without replacing prior image", "[geometry-page-hierarchical-disk]")
{
    auto package=DiskHierarchy();auto retained=Image(package);const auto bytes=retained.bytes;const auto digest=retained.metadataSha256;
    SECTION("out of range index"){package.pages.back().clusters.back().indices.back()=UINT32_MAX;}
    SECTION("duplicate source vertex"){auto& c=package.pages.back().clusters.back();REQUIRE(c.originalVertexIds.size()>1);c.originalVertexIds[1]=c.originalVertexIds[0];}
    SECTION("unsupported deforming vertex"){package.pages.back().clusters.back().vertices.back().conform=1;}
    SECTION("mismatched declared payload charge"){++package.pages.back().uploadBytes;}
    REQUIRE(EncodeHierarchicalDisk(package,retained)==HierarchicalDiskStatus::Invalid);
    REQUIRE(retained.bytes==bytes);REQUIRE(retained.metadataSha256==digest);
}
