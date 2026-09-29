#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Graphics/Rendering/AuthoredGeometryPageWorker.hpp>
#include <chrono>
#include <stdexcept>
using namespace Poseidon;
using namespace Poseidon::GeometryPages;
namespace
{
template<class Predicate>bool Until(Predicate predicate)
{
    const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    do{if(predicate())return true;std::this_thread::sleep_for(std::chrono::milliseconds(1));}while(std::chrono::steady_clock::now()<end);
    return predicate();
}
struct Source
{
    HierarchicalPackage package;HierarchicalDiskImage image;HierarchicalDiskManifest manifest;
};
Source ActualSource()
{
    ShapeExport original;original.source.sourceSha256[0]=103;original.source.vertexLayout=sizeof(SVertex);
    original.source.materialMapping=1;original.source.coarseRepresentation=0;original.source.fineRepresentation=1;
    constexpr uint32_t side=17;
    for(uint32_t y=0;y<side;++y)for(uint32_t x=0;x<side;++x) {
        const float a=float(x)/16,b=float(y)/16,z=.9f*(1-a*a)*(1-b*b);SVertex v{};
        v.pos=Vector3P(a*4,b*4,z);v.norm=Vector3P(0,0,1);v.tangent=Vector3P(1,0,0);v.binormal=Vector3P(0,1,0);
        v.t0={a*a,b*b};v.t1=v.t0;original.fine.vertices.push_back(v);original.fine.positions.push_back({a*4,b*4,z});
    }
    for(uint32_t y=0;y<side-1;++y)for(uint32_t x=0;x<side-1;++x) {
        const auto a=y*side+x,b=a+1,c=a+side,d=c+1;
        original.fine.indices.insert(original.fine.indices.end(),{a,b,d,a,d,c});original.fine.materials.insert(original.fine.materials.end(),2,0);
    }
    ClodBake bake;REQUIRE(BakeClodPilot(original,true,bake)==ClodBakeStatus::Baked);Source result;
    REQUIRE(BuildHierarchicalPackage(bake,result.package)==HierarchicalBuildStatus::Built);
    REQUIRE(EncodeHierarchicalDisk(result.package,result.image)==HierarchicalDiskStatus::Encoded);
    uint64_t metadataBytes=0;for(unsigned i=0;i<8;++i)metadataBytes|=uint64_t(result.image.bytes[204+i])<<(8*i);
    REQUIRE(DecodeHierarchicalManifest(std::span<const uint8_t>(result.image.bytes.data(),size_t(metadataBytes)),result.image.bytes.size(),
        result.image.identity,result.image.metadataSha256,result.manifest)==HierarchicalDiskStatus::Decoded);
    return result;
}
class Files
{
    std::filesystem::path root_,directory_;bool owned_=false;
public:
    Files() {
        static std::atomic<uint64_t> serial{0};root_=std::filesystem::canonical(std::filesystem::temp_directory_path());
        for(unsigned attempt=0;attempt<64;++attempt) {
            const auto candidate=root_/("OpenPoseidon-hierarchy-worker-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"-"+std::to_string(++serial));
            if(std::filesystem::create_directory(candidate)){directory_=candidate;owned_=true;return;}
        }
        throw std::runtime_error("cannot reserve hierarchy worker temp directory");
    }
    ~Files() {
        if(!owned_)return;
        try {std::error_code error;const auto resolved=std::filesystem::canonical(directory_,error);
            if(error||resolved!=directory_||resolved.parent_path()!=root_||resolved.filename().string().rfind("OpenPoseidon-hierarchy-worker-",0)!=0)return;
            std::filesystem::remove(resolved/"hierarchy.hgp",error);std::filesystem::remove(resolved,error);
        }catch(...){}
    }
    const std::filesystem::path& Directory()const{return directory_;}
    void Write(std::span<const uint8_t> bytes)const {
        std::ofstream stream(directory_/"hierarchy.hgp",std::ios::binary|std::ios::trunc);REQUIRE(stream.is_open());
        stream.write(reinterpret_cast<const char*>(bytes.data()),std::streamsize(bytes.size()));REQUIRE(bool(stream));stream.close();REQUIRE(bool(stream));
    }
    std::shared_ptr<HierarchicalDiskPageInput> Input(const Source& source,uint32_t page)const {
        auto input=std::make_shared<HierarchicalDiskPageInput>();input->path=directory_/"hierarchy.hgp";
        input->metadataBytes.assign(source.image.bytes.begin(),source.image.bytes.begin()+source.manifest.metadataBytes);
        input->expectedIdentity=source.image.identity;input->expectedMetadataSha256=source.image.metadataSha256;
        input->fileBytes=source.image.bytes.size();input->pageId=page;return input;
    }
};
void CheckPage(const HierarchicalPage& actual,const HierarchicalPage& expected)
{
    REQUIRE(actual.group==expected.group);REQUIRE(actual.logicalBytes==expected.logicalBytes);REQUIRE(actual.uploadBytes==expected.uploadBytes);
    REQUIRE(actual.clusters.size()==expected.clusters.size());
    for(size_t c=0;c<actual.clusters.size();++c) {
        const auto& a=actual.clusters[c];const auto& e=expected.clusters[c];REQUIRE(a.cluster==e.cluster);REQUIRE(a.material==e.material);
        REQUIRE(a.originalVertexIds==e.originalVertexIds);REQUIRE(a.indices==e.indices);REQUIRE(a.vertices.size()==e.vertices.size());
        for(size_t v=0;v<a.vertices.size();++v){REQUIRE(a.vertices[v].pos.X()==e.vertices[v].pos.X());REQUIRE(a.vertices[v].pos.Y()==e.vertices[v].pos.Y());
            REQUIRE(a.vertices[v].pos.Z()==e.vertices[v].pos.Z());REQUIRE(a.vertices[v].t0.u==e.vertices[v].t0.u);REQUIRE(a.vertices[v].t0.v==e.vertices[v].t0.v);}
    }
}
}
TEST_CASE("Existing authored worker reads only requested hierarchy page and retains result reservation", "[geometry-page-hierarchy-worker]")
{
    auto source=ActualSource();const auto root=source.package.rootPages.front();REQUIRE(root!=0);
    // A corrupt unrelated range proves this request is not whole-file decoding.
    const auto other=source.manifest.pages[0];source.image.bytes[other.offset+other.bytes-1]^=1;
    Files files;files.Write(source.image.bytes);auto input=files.Input(source,root);AuthoredPageWorker worker;
    REQUIRE(worker.SubmitHierarchyDisk(7,19,input,AuthoredPageWorker::MaxReserved)==AuthoredPageWorker::SubmitStatus::Queued);
    REQUIRE(Until([&]{return worker.Snapshot().ready==1;}));auto result=worker.Take(7,19);REQUIRE(result);
    REQUIRE(result->status==DecodeStatus::Decoded);REQUIRE(result->hierarchyDiskStatus==HierarchicalDiskReadStatus::Read);
    REQUIRE(result->hierarchyPageId==root);REQUIRE(result->hierarchyIdentity==source.package.identity);REQUIRE(result->hierarchySource);
    REQUIRE(result->decoded.pages.empty());REQUIRE_FALSE(result->source);REQUIRE_FALSE(result->ownership->disk);
    CheckPage(result->hierarchyPage,source.package.pages[root]);
    const auto charge=HierarchicalDiskPageInputKnownBytes(*result->hierarchySource)+AuthoredPageWorker::HierarchyFileReservation+
        AuthoredPageWorker::HierarchyManifestReservation+AuthoredPageWorker::ResultReservation+AuthoredPageWorker::MetadataReservation;
    REQUIRE(worker.Snapshot().liveJobs==1);REQUIRE(worker.Snapshot().reservedBytes==charge);
    REQUIRE(std::filesystem::remove(input->path));CheckPage(result->hierarchyPage,source.package.pages[root]);
    result.reset();REQUIRE(Until([&]{return worker.Snapshot().liveJobs==0;}));REQUIRE(worker.Snapshot().reservedBytes==0);
}
TEST_CASE("Hierarchy submission freezes page path metadata and source authority before actual I/O", "[geometry-page-hierarchy-worker]")
{
    const auto source=ActualSource();Files files;files.Write(source.image.bytes);auto input=files.Input(source,0);AuthoredPageWorker worker;
    worker.HoldBeforeDiskOpen(true);
    REQUIRE(worker.SubmitHierarchyDisk(1,11,input,AuthoredPageWorker::MaxReserved)==AuthoredPageWorker::SubmitStatus::Queued);
    REQUIRE(Until([&]{return worker.Snapshot().active==1;}));
    REQUIRE(worker.SubmitHierarchyDisk(1,11,input,AuthoredPageWorker::MaxReserved)==AuthoredPageWorker::SubmitStatus::Duplicate);
    auto conflicting=std::make_shared<HierarchicalDiskPageInput>(*input);++conflicting->pageId;
    REQUIRE(worker.SubmitHierarchyDisk(1,11,conflicting,AuthoredPageWorker::MaxReserved)==AuthoredPageWorker::SubmitStatus::Invalid);
    input->path=files.Directory()/"missing";input->pageId=UINT32_MAX;input->metadataBytes[0]^=1;input->expectedIdentity.source.geometryOptions^=1;
    worker.HoldBeforeDiskOpen(false);REQUIRE(Until([&]{return worker.Snapshot().ready==1;}));auto result=worker.Take(1,11);REQUIRE(result);
    REQUIRE(result->status==DecodeStatus::Decoded);REQUIRE(result->hierarchyPageId==0);REQUIRE(result->hierarchyIdentity==source.package.identity);
    REQUIRE(result->hierarchySource.get()!=input.get());CheckPage(result->hierarchyPage,source.package.pages[0]);
    result.reset();REQUIRE(Until([&]{return worker.Snapshot().reservedBytes==0;}));
}
TEST_CASE("Cancelled hierarchy work retains actual debt while replacement uses same bounded worker", "[geometry-page-hierarchy-worker]")
{
    const auto source=ActualSource();Files files;files.Write(source.image.bytes);auto input=files.Input(source,0);AuthoredPageWorker worker;
    bool afterDecode=false;
    SECTION("before file open"){worker.HoldBeforeDiskOpen(true);}
    SECTION("after decode before publication"){afterDecode=true;worker.HoldBeforePublish(true);}
    REQUIRE(worker.SubmitHierarchyDisk(1,11,input,AuthoredPageWorker::MaxReserved)==AuthoredPageWorker::SubmitStatus::Queued);
    REQUIRE(Until([&]{const auto s=worker.Snapshot();return s.active==1&&(!afterDecode||s.awaitingPublication==1);}));
    worker.Cancel(1);const auto cancelled=worker.Snapshot();REQUIRE(cancelled.liveJobs==1);REQUIRE(cancelled.reservedBytes>0);
    auto next=files.Input(source,source.package.rootPages.front());
    REQUIRE(worker.SubmitHierarchyDisk(2,22,next,AuthoredPageWorker::MaxReserved)==AuthoredPageWorker::SubmitStatus::Queued);
    REQUIRE(worker.SubmitHierarchyDisk(3,33,next,AuthoredPageWorker::MaxReserved)==AuthoredPageWorker::SubmitStatus::Busy);
    REQUIRE(worker.Snapshot().liveJobs==2);REQUIRE(worker.Snapshot().reservedBytes<=AuthoredPageWorker::MaxReserved);
    worker.HoldBeforeDiskOpen(false);worker.HoldBeforePublish(false);
    REQUIRE(Until([&]{return worker.Snapshot().ready==1;}));auto result=worker.Take(2,22);REQUIRE(result);
    REQUIRE(result->epoch==2);REQUIRE(result->request==22);REQUIRE(result->hierarchyPageId==next->pageId);REQUIRE(result->status==DecodeStatus::Decoded);
    REQUIRE(worker.Snapshot().cancelled>=1);REQUIRE(worker.Snapshot().liveJobs==1);
    result.reset();REQUIRE(Until([&]{return worker.Snapshot().reservedBytes==0;}));
}
TEST_CASE("Hierarchy file source failures publish no page and preserve authenticated ticket", "[geometry-page-hierarchy-worker]")
{
    auto source=ActualSource();Files files;auto input=files.Input(source,0);auto expected=HierarchicalDiskReadStatus::Invalid;
    SECTION("missing"){expected=HierarchicalDiskReadStatus::Missing;}
    SECTION("wrong externally expected source"){files.Write(source.image.bytes);input->expectedIdentity.source.sourceSha256[0]^=1;}
    SECTION("wrong externally expected metadata"){files.Write(source.image.bytes);input->expectedMetadataSha256[0]^=1;}
    SECTION("corrupt requested page"){const auto& p=source.manifest.pages[0];source.image.bytes[p.offset+p.bytes-1]^=1;files.Write(source.image.bytes);}
    SECTION("truncated actual file"){source.image.bytes.pop_back();files.Write(source.image.bytes);expected=HierarchicalDiskReadStatus::ReadFailed;}
    SECTION("grown actual file"){source.image.bytes.push_back(0);files.Write(source.image.bytes);expected=HierarchicalDiskReadStatus::ReadFailed;}
    SECTION("directory"){input->path=files.Directory();expected=HierarchicalDiskReadStatus::Unsupported;}
    AuthoredPageWorker worker;REQUIRE(worker.SubmitHierarchyDisk(1,3,input,AuthoredPageWorker::MaxReserved)==AuthoredPageWorker::SubmitStatus::Queued);
    REQUIRE(Until([&]{return worker.Snapshot().ready==1;}));auto result=worker.Take(1,3);REQUIRE(result);
    REQUIRE(result->hierarchyDiskStatus==expected);REQUIRE(result->status!=DecodeStatus::Decoded);REQUIRE(result->hierarchyPage.clusters.empty());
    REQUIRE(result->epoch==1);REQUIRE(result->request==3);REQUIRE(result->hierarchySource);
    result.reset();REQUIRE(Until([&]{return worker.Snapshot().reservedBytes==0;}));
}
TEST_CASE("Hierarchy reservations refuse excess input bytes and stale takes release their debt", "[geometry-page-hierarchy-worker]")
{
    const auto source=ActualSource();Files files;files.Write(source.image.bytes);auto input=files.Input(source,0);AuthoredPageWorker worker;
    REQUIRE(worker.SubmitHierarchyDisk(1,1,input,1)==AuthoredPageWorker::SubmitStatus::Capacity);REQUIRE(worker.Snapshot().reservedBytes==0);
    auto oversized=std::make_shared<HierarchicalDiskPageInput>(*input);oversized->metadataBytes.reserve(65537);
    REQUIRE(worker.SubmitHierarchyDisk(1,1,oversized,AuthoredPageWorker::MaxReserved)==AuthoredPageWorker::SubmitStatus::Invalid);
    REQUIRE(worker.SubmitHierarchyDisk(1,1,input,AuthoredPageWorker::MaxReserved)==AuthoredPageWorker::SubmitStatus::Queued);
    REQUIRE(Until([&]{return worker.Snapshot().ready==1;}));REQUIRE_FALSE(worker.Take(2,2));
    REQUIRE(Until([&]{return worker.Snapshot().reservedBytes==0;}));REQUIRE(worker.Snapshot().stale==1);
}
TEST_CASE("Hierarchy owned file reader is transactional on cancellation and corrupt metadata", "[geometry-page-hierarchy-worker]")
{
    const auto source=ActualSource();Files files;files.Write(source.image.bytes);auto input=files.Input(source,0);
    HierarchicalPage retained=source.package.pages.back();const auto cluster=retained.clusters.front().cluster;
    std::atomic<bool> cancelled{true};
    REQUIRE(ReadHierarchicalDiskPage(*input,retained,cancelled)==HierarchicalDiskReadStatus::Cancelled);
    REQUIRE(retained.clusters.front().cluster==cluster);cancelled.store(false);input->metadataBytes[0]^=1;
    REQUIRE(ReadHierarchicalDiskPage(*input,retained,cancelled)==HierarchicalDiskReadStatus::Invalid);
    REQUIRE(retained.clusters.front().cluster==cluster);
}
TEST_CASE("Hierarchy and legacy whole representation requests share a single queue and budget", "[geometry-page-hierarchy-worker]")
{
    const auto source=ActualSource();Files files;files.Write(source.image.bytes);auto input=files.Input(source,0);
    auto legacy=std::make_shared<AuthoredPageInput>();legacy->original.source=source.package.identity.source;
    for(auto* mesh:{&legacy->original.coarse,&legacy->original.fine}) {
        mesh->positions={{0,0,0},{1,0,0},{0,1,0}};mesh->vertices.resize(3);
        for(size_t i=0;i<3;++i){const auto p=mesh->positions[i];mesh->vertices[i].pos=Vector3P(p.x,p.y,p.z);mesh->vertices[i].norm=Vector3P(0,0,1);}
        mesh->indices={0,1,2};mesh->materials={0};
    }
    REQUIRE(Build(legacy->original.coarse.Input(),legacy->original.fine.Input(),legacy->original.source,true,legacy->package)==Status::Built);
    legacy->identity=legacy->package.Identity();legacy->knownCapacityBytes=AuthoredPageKnownBytes(*legacy);
    AuthoredPageWorker worker;worker.HoldBeforeDiskOpen(true);
    REQUIRE(worker.SubmitHierarchyDisk(1,11,input,AuthoredPageWorker::MaxReserved)==AuthoredPageWorker::SubmitStatus::Queued);
    REQUIRE(Until([&]{return worker.Snapshot().active==1;}));
    REQUIRE(worker.Submit(2,22,legacy,AuthoredPageWorker::MaxReserved)==AuthoredPageWorker::SubmitStatus::Queued);
    REQUIRE(worker.SubmitHierarchyDisk(3,33,input,AuthoredPageWorker::MaxReserved)==AuthoredPageWorker::SubmitStatus::Busy);
    REQUIRE(worker.Snapshot().liveJobs==2);REQUIRE(worker.Snapshot().reservedBytes<=AuthoredPageWorker::MaxReserved);
    worker.Cancel(1);worker.HoldBeforeDiskOpen(false);
    REQUIRE(Until([&]{return worker.Snapshot().ready==1;}));auto result=worker.Take(2,22);REQUIRE(result);
    REQUIRE(result->status==DecodeStatus::Decoded);REQUIRE(result->source==legacy);REQUIRE(result->decoded.indices==3);
    REQUIRE(result->hierarchyDiskStatus==HierarchicalDiskReadStatus::NotRequested);REQUIRE(result->hierarchyPage.clusters.empty());
    result.reset();REQUIRE(Until([&]{return worker.Snapshot().reservedBytes==0;}));
}
