#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Graphics/Rendering/AuthoredGeometryPageWorker.hpp>
#include <Poseidon/Graphics/Rendering/GeometryPageControlledClodPilot.hpp>
#include <chrono>
#include <cstring>
#include <thread>
#include <stdexcept>
using namespace Poseidon;
using namespace Poseidon::GeometryPages;
namespace
{
std::shared_ptr<AuthoredPageInput> OriginalInput()
{
    auto input=std::make_shared<AuthoredPageInput>();
    auto& dto=input->original;dto.source.sourceSha256[0]=21;
    dto.source.vertexLayout=sizeof(SVertex);dto.source.materialMapping=1;
    dto.source.coarseRepresentation=0;dto.source.fineRepresentation=1;
    for(auto* mesh:{&dto.coarse,&dto.fine}) {
        mesh->positions={{0,0,0},{2,0,0},{0,2,0},{2,2,0}};mesh->vertices.resize(4);
        for(size_t v=0;v<4;++v) {
            const auto p=mesh->positions[v];auto& vertex=mesh->vertices[v];
            vertex.pos=Vector3P(p.x,p.y,p.z);vertex.norm=Vector3P(0,0,1);
            vertex.t0={float(v),0};vertex.t1={0,float(v)};vertex.conform=0;
            vertex.tangent=Vector3P(1,0,0);vertex.binormal=Vector3P(0,1,0);
        }
    }
    dto.coarse.indices={0,1,2};dto.coarse.materials={7};
    dto.fine.indices={0,1,2,1,3,2};dto.fine.materials={7,8};
    Limits limits;limits.clusterTriangles=1;limits.pageBytes=uint32_t(16+48+3*(4+sizeof(SVertex))+3*4);
    REQUIRE(Build(dto.coarse.Input(),dto.fine.Input(),dto.source,true,input->package,limits)==Status::Built);
    input->identity=input->package.Identity();input->knownCapacityBytes=AuthoredPageKnownBytes(*input);
    REQUIRE(input->knownCapacityBytes<=AuthoredPageInput::MaxSourceBytes);
    return input;
}
template<class Predicate> bool Until(Predicate predicate)
{
    const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    do {if(predicate()) return true;std::this_thread::sleep_for(std::chrono::milliseconds(1));}
    while(std::chrono::steady_clock::now()<end);
    return predicate();
}
void CheckOriginal(const ResidentRepresentation& decoded,const ExportedMesh& source,Frontier expected=Frontier::Fine)
{
    REQUIRE(decoded.representation==expected);REQUIRE(decoded.indices==source.indices.size());
    size_t triangle=0;
    for(const auto& page:decoded.pages) for(const auto& cluster:page.clusters) {
        REQUIRE(cluster.firstTriangle==triangle);
        for(size_t i=0;i<cluster.indices.size();++i) {
            REQUIRE(cluster.indices[i]<cluster.vertices.size());
            const auto& actual=cluster.vertices[cluster.indices[i]];
            const auto& original=source.vertices[source.indices[triangle*3+i]];
            REQUIRE(std::memcmp(&actual,&original,sizeof(SVertex))==0);
        }
        for(size_t t=0;t<cluster.indices.size()/3;++t) REQUIRE(cluster.material==source.materials[triangle+t]);
        triangle+=cluster.indices.size()/3;
    }
    REQUIRE(triangle==source.materials.size());
}
}
TEST_CASE("Authored page worker holds cancellation debt until exit and rejects stale publication", "[geometry-page-worker]")
{
    auto input=OriginalInput();AuthoredPageWorker worker;worker.HoldBeforePublish(true);
    REQUIRE(worker.Submit(1,11,input,AuthoredPageWorker::MaxReserved)==AuthoredPageWorker::SubmitStatus::Queued);
    REQUIRE(Until([&]{return worker.Snapshot().active==1;}));
    REQUIRE(worker.Submit(1,11,input,AuthoredPageWorker::MaxReserved)==AuthoredPageWorker::SubmitStatus::Duplicate);
    REQUIRE(worker.Submit(2,22,input,AuthoredPageWorker::MaxReserved)==AuthoredPageWorker::SubmitStatus::Queued);
    REQUIRE(worker.Submit(3,33,input,AuthoredPageWorker::MaxReserved)==AuthoredPageWorker::SubmitStatus::Busy);
    const auto held=worker.Snapshot();REQUIRE(held.liveJobs==2);REQUIRE(held.reservedBytes>input->knownCapacityBytes);
    worker.Cancel(1);REQUIRE_FALSE(worker.Take(1,11));
    REQUIRE(worker.Snapshot().active==1);REQUIRE(worker.Snapshot().liveJobs==2);
    worker.HoldBeforePublish(false);
    REQUIRE(Until([&]{return worker.Snapshot().ready==1;}));
    auto result=worker.Take(2,22);REQUIRE(result);REQUIRE(result->epoch==2);REQUIRE(result->request==22);
    REQUIRE(result->status==DecodeStatus::Decoded);REQUIRE(result->source==input);
    CheckOriginal(result->decoded,input->original.fine);
    REQUIRE(worker.Snapshot().cancelled>=1);REQUIRE(worker.Snapshot().liveJobs==1);
    result.reset();REQUIRE(Until([&]{return worker.Snapshot().liveJobs==0;}));
    REQUIRE(worker.Snapshot().reservedBytes==0);
}
TEST_CASE("Authored page result ownership bounds new jobs and a wrong epoch cannot take it", "[geometry-page-worker]")
{
    auto input=OriginalInput();AuthoredPageWorker worker;
    REQUIRE(worker.Submit(1,1,input,AuthoredPageWorker::MaxReserved)==AuthoredPageWorker::SubmitStatus::Queued);
    REQUIRE(Until([&]{return worker.Snapshot().ready==1;}));
    REQUIRE_FALSE(worker.Take(2,2)); // discard rather than publish an earlier fixture result
    REQUIRE(Until([&]{return worker.Snapshot().liveJobs==0;}));REQUIRE(worker.Snapshot().stale==1);
    REQUIRE(worker.Submit(2,2,input,AuthoredPageWorker::MaxReserved)==AuthoredPageWorker::SubmitStatus::Queued);
    REQUIRE(Until([&]{return worker.Snapshot().ready==1;}));
    auto retained=worker.Take(2,2);REQUIRE(retained);worker.HoldBeforePublish(true);
    REQUIRE(worker.Submit(3,3,input,AuthoredPageWorker::MaxReserved)==AuthoredPageWorker::SubmitStatus::Queued);
    REQUIRE(Until([&]{return worker.Snapshot().active==1;}));
    REQUIRE(worker.Submit(4,4,input,AuthoredPageWorker::MaxReserved)==AuthoredPageWorker::SubmitStatus::Busy);
    retained.reset();worker.Cancel(3);worker.HoldBeforePublish(false);
    REQUIRE(Until([&]{return worker.Snapshot().liveJobs==0;}));REQUIRE(worker.Snapshot().reservedBytes==0);
}
TEST_CASE("Authored worker refusal and page failure publish no partial fine representation", "[geometry-page-worker]")
{
    auto input=OriginalInput();AuthoredPageWorker worker;
    SECTION("mismatched immutable identity") {
        ++input->identity.source.geometryOptions;
        REQUIRE(worker.Submit(1,1,input,AuthoredPageWorker::MaxReserved)==AuthoredPageWorker::SubmitStatus::Invalid);
        REQUIRE(worker.Snapshot().liveJobs==0);
    }
    SECTION("source or logical budget refusal") {
        REQUIRE(worker.Submit(1,1,input,1)==AuthoredPageWorker::SubmitStatus::Capacity);
        input->knownCapacityBytes=AuthoredPageInput::MaxSourceBytes+1;
        REQUIRE(worker.Submit(1,1,input,AuthoredPageWorker::MaxReserved)==AuthoredPageWorker::SubmitStatus::Invalid);
        REQUIRE(worker.Snapshot().reservedBytes==0);
    }
    SECTION("last page corruption after valid earlier page") {
        REQUIRE(input->package.pages.size()>=3);input->package.pages.back().bytes.back()^=1;
        REQUIRE(worker.Submit(1,1,input,AuthoredPageWorker::MaxReserved)==AuthoredPageWorker::SubmitStatus::Queued);
        REQUIRE(Until([&]{return worker.Snapshot().ready==1;}));auto result=worker.Take(1,1);REQUIRE(result);
        REQUIRE(result->status==DecodeStatus::Invalid);REQUIRE(result->decoded.pages.empty());REQUIRE(result->decoded.indices==0);
    }
    SECTION("cancelled decoder keeps previous destination") {
        std::atomic<bool> cancelled{true};ResidentRepresentation sentinel;sentinel.indices=991;
        REQUIRE(DecodeResidentRepresentation(input->package,input->identity,Frontier::Fine,input->original,sentinel,{},&cancelled)==DecodeStatus::Cancelled);
        REQUIRE(sentinel.indices==991);REQUIRE(sentinel.pages.empty());
    }
}
TEST_CASE("Authored worker shutdown releases an explicitly held active job", "[geometry-page-worker]")
{
    auto input=OriginalInput();auto worker=std::make_unique<AuthoredPageWorker>();worker->HoldBeforePublish(true);
    REQUIRE(worker->Submit(1,1,input,AuthoredPageWorker::MaxReserved)==AuthoredPageWorker::SubmitStatus::Queued);
    REQUIRE(Until([&]{return worker->Snapshot().active==1;}));
    worker.reset(); // destructor unholds, cancels and joins; no Engine or renderer dependency
}

namespace
{
ClodRamPackage DiskWorkerPilot()
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
class DiskWorkerFiles
{
    std::filesystem::path directory_,root_;
    bool uniquelyCreated_=false;
    void Cleanup() noexcept
    {
        if(!uniquelyCreated_) return;
        try {
            std::error_code error;const auto resolved=std::filesystem::canonical(directory_,error);
            if(error || resolved!=directory_ || resolved==root_ || resolved.parent_path()!=root_ ||
               resolved.filename().string().rfind("OpenPoseidon-clod-worker-",0)!=0 ||
               !std::filesystem::is_directory(resolved,error) || error) return;
            // Exact uniquely created child of the resolved intended temp root;
            // never recursively remove an arbitrary caller-computed path.
            std::filesystem::remove_all(resolved,error);
        } catch(...) {} // Destructor must not replace a test failure.
        uniquelyCreated_=false;
    }
public:
    DiskWorkerFiles()
    {
        static std::atomic<uint64_t> serial{0};
        root_=std::filesystem::canonical(std::filesystem::temp_directory_path());
        for(unsigned attempt=0;attempt<64;++attempt) {
            const auto label=std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"-"+std::to_string(++serial);
            auto candidate=root_/("OpenPoseidon-clod-worker-"+label);
            if(std::filesystem::create_directory(candidate)) {
                directory_=std::move(candidate);uniquelyCreated_=true;return;
            }
        }
        throw std::runtime_error("cannot reserve private disk worker test directory");
    }
    ~DiskWorkerFiles() {Cleanup();}
    DiskWorkerFiles(const DiskWorkerFiles&)=delete;
    const std::filesystem::path& Directory() const {return directory_;}
    void Write(std::span<const uint8_t> bytes) const {
        std::ofstream file(directory_/"selected.gcd",std::ios::binary|std::ios::trunc);
        REQUIRE(file.is_open());file.write(reinterpret_cast<const char*>(bytes.data()),std::streamsize(bytes.size()));
        REQUIRE(bool(file));file.close();REQUIRE(bool(file));
    }
    std::shared_ptr<DiskPageInput> Input(const ClodRamPackage& source) const {
        auto input=std::make_shared<DiskPageInput>();input->path=directory_/"selected.gcd";
        input->originalSource=source.originalSource;input->selectedCut=source.package.Identity();return input;
    }
};
}
TEST_CASE("Disk worker reads actual immutable selected package and holds complete debts through result release", "[geometry-page-worker][geometry-page-disk-worker]")
{
    const auto source=DiskWorkerPilot();std::vector<uint8_t> bytes;REQUIRE(EncodeClodDisk(source,bytes)==ClodDiskStatus::Encoded);
    DiskWorkerFiles files;files.Write(bytes);auto input=files.Input(source);AuthoredPageWorker worker;
    const uint64_t baseReservation=AuthoredPageWorker::DiskFileReservation+AuthoredPageWorker::DiskSourceReservation+
        AuthoredPageWorker::ResultReservation+AuthoredPageWorker::MetadataReservation;
    REQUIRE(worker.SubmitDisk(7,19,input,AuthoredPageWorker::MaxReserved)==AuthoredPageWorker::SubmitStatus::Queued);
    REQUIRE(Until([&]{return worker.Snapshot().ready==1;}));auto result=worker.Take(7,19);REQUIRE(result);
    REQUIRE(result->diskStatus==DiskReadStatus::Read);REQUIRE(result->status==DecodeStatus::Decoded);REQUIRE(result->source);
    REQUIRE(result->source->identity==input->selectedCut);CheckOriginal(result->decoded,source.selectedGeometry.fine);
    REQUIRE(worker.Snapshot().liveJobs==1);REQUIRE(worker.Snapshot().reservedBytes==baseReservation+DiskPageKnownBytes(*result->ownership->disk));
    // File lifetime and result ownership are separate; no retained file stream/buffer.
    REQUIRE(std::filesystem::remove(input->path));CheckOriginal(result->decoded,source.selectedGeometry.fine);
    result.reset();REQUIRE(Until([&]{return worker.Snapshot().liveJobs==0;}));REQUIRE(worker.Snapshot().reservedBytes==0);
}
TEST_CASE("Disk worker missing stale corrupt and oversized actual files never publish fine geometry", "[geometry-page-worker][geometry-page-disk-worker]")
{
    const auto source=DiskWorkerPilot();std::vector<uint8_t> bytes;REQUIRE(EncodeClodDisk(source,bytes)==ClodDiskStatus::Encoded);
    DiskWorkerFiles files;auto input=files.Input(source);DiskReadStatus expected=DiskReadStatus::Invalid;
    SECTION("missing") {expected=DiskReadStatus::Missing;}
    SECTION("stale externally expected source") {files.Write(bytes);input->originalSource.sourceSha256[0]^=1;}
    SECTION("stale externally expected cut") {files.Write(bytes);input->selectedCut.source.sourceSha256[0]^=1;}
    SECTION("directory is not a disk package") {input->path=files.Directory();expected=DiskReadStatus::Unsupported;}
    SECTION("corrupt") {bytes.back()^=1;files.Write(bytes);}
    SECTION("truncated") {bytes.resize(bytes.size()-1);files.Write(bytes);}
    SECTION("oversized before allocation") {bytes.assign(128*1024+1,0);files.Write(bytes);expected=DiskReadStatus::Capacity;}
    AuthoredPageWorker worker;REQUIRE(worker.SubmitDisk(1,1,input,AuthoredPageWorker::MaxReserved)==AuthoredPageWorker::SubmitStatus::Queued);
    REQUIRE(Until([&]{return worker.Snapshot().ready==1;}));auto failed=worker.Take(1,1);REQUIRE(failed);
    REQUIRE(failed->diskStatus==expected);REQUIRE(failed->status!=DecodeStatus::Decoded);
    REQUIRE_FALSE(failed->source);REQUIRE(failed->decoded.pages.empty());REQUIRE(failed->decoded.indices==0);
    failed.reset();REQUIRE(Until([&]{return worker.Snapshot().liveJobs==0;}));
}
TEST_CASE("Disk worker cancelled actual jobs retain reservations until exit and replacement is bounded", "[geometry-page-worker][geometry-page-disk-worker]")
{
    const auto source=DiskWorkerPilot();std::vector<uint8_t> bytes;REQUIRE(EncodeClodDisk(source,bytes)==ClodDiskStatus::Encoded);
    DiskWorkerFiles files;files.Write(bytes);auto input=files.Input(source);AuthoredPageWorker worker;
    SECTION("before actual open") {
        worker.HoldBeforeDiskOpen(true);
        REQUIRE(worker.SubmitDisk(1,11,input,AuthoredPageWorker::MaxReserved)==AuthoredPageWorker::SubmitStatus::Queued);
        REQUIRE(Until([&]{return worker.Snapshot().active==1;}));worker.Cancel(1);
        const auto old=worker.Snapshot();REQUIRE(old.liveJobs==1);REQUIRE(old.reservedBytes>0);
        REQUIRE(worker.SubmitDisk(2,22,input,AuthoredPageWorker::MaxReserved)==AuthoredPageWorker::SubmitStatus::Queued);
        REQUIRE(worker.SubmitDisk(3,33,input,AuthoredPageWorker::MaxReserved)==AuthoredPageWorker::SubmitStatus::Busy);
        REQUIRE(worker.Snapshot().liveJobs==2);REQUIRE(worker.Snapshot().reservedBytes<=AuthoredPageWorker::MaxReserved);
        worker.HoldBeforeDiskOpen(false);
    }
    SECTION("after actual read and decode before publication") {
        worker.HoldBeforePublish(true);
        REQUIRE(worker.SubmitDisk(1,11,input,AuthoredPageWorker::MaxReserved)==AuthoredPageWorker::SubmitStatus::Queued);
        REQUIRE(Until([&]{return worker.Snapshot().awaitingPublication==1;}));worker.Cancel(1);
        REQUIRE(worker.Snapshot().liveJobs==1);REQUIRE_FALSE(worker.Take(1,11));
        REQUIRE(worker.SubmitDisk(2,22,input,AuthoredPageWorker::MaxReserved)==AuthoredPageWorker::SubmitStatus::Queued);
        REQUIRE(worker.SubmitDisk(3,33,input,AuthoredPageWorker::MaxReserved)==AuthoredPageWorker::SubmitStatus::Busy);
        worker.HoldBeforePublish(false);
    }
    REQUIRE(Until([&]{return worker.Snapshot().ready==1;}));auto replacement=worker.Take(2,22);REQUIRE(replacement);
    REQUIRE(replacement->diskStatus==DiskReadStatus::Read);REQUIRE(replacement->status==DecodeStatus::Decoded);
    CheckOriginal(replacement->decoded,source.selectedGeometry.fine);REQUIRE(worker.Snapshot().cancelled>=1);
    replacement.reset();REQUIRE(Until([&]{return worker.Snapshot().liveJobs==0;}));REQUIRE(worker.Snapshot().reservedBytes==0);
}
TEST_CASE("Disk worker reads on worker time rejects wrong epochs and preserves RAM submission contracts", "[geometry-page-worker][geometry-page-disk-worker]")
{
    const auto source=DiskWorkerPilot();std::vector<uint8_t> bytes;REQUIRE(EncodeClodDisk(source,bytes)==ClodDiskStatus::Encoded);
    DiskWorkerFiles files;files.Write(bytes);auto input=files.Input(source);AuthoredPageWorker worker;
    SECTION("actual file content is read after enqueue rather than cached by submit") {
        worker.HoldBeforeDiskOpen(true);REQUIRE(worker.SubmitDisk(1,1,input,AuthoredPageWorker::MaxReserved)==AuthoredPageWorker::SubmitStatus::Queued);
        REQUIRE(Until([&]{return worker.Snapshot().active==1;}));bytes.back()^=1;files.Write(bytes);worker.HoldBeforeDiskOpen(false);
        REQUIRE(Until([&]{return worker.Snapshot().ready==1;}));auto failed=worker.Take(1,1);REQUIRE(failed);
        REQUIRE(failed->diskStatus==DiskReadStatus::Invalid);REQUIRE(failed->decoded.pages.empty());failed.reset();
    }
    SECTION("queued disk job owns frozen path and expected keys despite later caller mutation") {
        const auto originalPath=input->path;const auto originalSource=input->originalSource;const auto originalCut=input->selectedCut;
        worker.HoldBeforeDiskOpen(true);
        REQUIRE(worker.SubmitDisk(1,1,input,AuthoredPageWorker::MaxReserved)==AuthoredPageWorker::SubmitStatus::Queued);
        REQUIRE(Until([&]{return worker.Snapshot().active==1;}));
        input->path=files.Directory()/"caller-replacement-missing.gcd";
        input->originalSource.sourceSha256[0]^=1;input->selectedCut.source.sourceSha256[0]^=1;
        worker.HoldBeforeDiskOpen(false);REQUIRE(Until([&]{return worker.Snapshot().ready==1;}));
        auto result=worker.Take(1,1);REQUIRE(result);REQUIRE(result->diskStatus==DiskReadStatus::Read);
        REQUIRE(result->status==DecodeStatus::Decoded);REQUIRE(result->ownership->disk.get()!=input.get());
        REQUIRE(result->ownership->disk->path==originalPath);REQUIRE(result->ownership->disk->originalSource==originalSource);
        REQUIRE(result->source->identity==originalCut);CheckOriginal(result->decoded,source.selectedGeometry.fine);result.reset();
    }
    SECTION("held returned disk result shares two-job limit with RAM work") {
        REQUIRE(worker.SubmitDisk(1,1,input,AuthoredPageWorker::MaxReserved)==AuthoredPageWorker::SubmitStatus::Queued);
        REQUIRE(Until([&]{return worker.Snapshot().ready==1;}));auto result=worker.Take(1,1);REQUIRE(result);
        auto ram=OriginalInput();worker.HoldBeforePublish(true);
        REQUIRE(worker.Submit(2,2,ram,AuthoredPageWorker::MaxReserved)==AuthoredPageWorker::SubmitStatus::Queued);
        REQUIRE(Until([&]{return worker.Snapshot().awaitingPublication==1;}));
        REQUIRE(worker.SubmitDisk(3,3,input,AuthoredPageWorker::MaxReserved)==AuthoredPageWorker::SubmitStatus::Busy);
        result.reset();worker.Cancel(2);worker.HoldBeforePublish(false);
    }
    SECTION("wrong epoch discards result instead of accepting stale fine") {
        REQUIRE(worker.SubmitDisk(1,1,input,AuthoredPageWorker::MaxReserved)==AuthoredPageWorker::SubmitStatus::Queued);
        REQUIRE(Until([&]{return worker.Snapshot().ready==1;}));REQUIRE_FALSE(worker.Take(2,2));REQUIRE(worker.Snapshot().stale==1);
    }
    SECTION("logical budget refuses before I/O") {
        REQUIRE(worker.SubmitDisk(1,1,input,1)==AuthoredPageWorker::SubmitStatus::Capacity);REQUIRE(worker.Snapshot().liveJobs==0);
    }
    SECTION("invalid oversized native path") {
        input->path=std::filesystem::path(std::filesystem::path::string_type(DiskPageInput::MaxPathCharacters+1,'x'));
        REQUIRE(worker.SubmitDisk(1,1,input,AuthoredPageWorker::MaxReserved)==AuthoredPageWorker::SubmitStatus::Invalid);
    }
    SECTION("invalid native embedded null path") {
        auto name=input->path.native();name.push_back(0);name.push_back('x');input->path=std::filesystem::path(name);
        REQUIRE(worker.SubmitDisk(1,1,input,AuthoredPageWorker::MaxReserved)==AuthoredPageWorker::SubmitStatus::Invalid);
    }
    REQUIRE(Until([&]{return worker.Snapshot().liveJobs==0;}));REQUIRE(worker.Snapshot().reservedBytes==0);
}

TEST_CASE("Disk worker shutdown cancels and joins a job held before actual file open", "[geometry-page-worker][geometry-page-disk-worker]")
{
    const auto source=DiskWorkerPilot();DiskWorkerFiles files;auto input=files.Input(source);
    auto worker=std::make_unique<AuthoredPageWorker>();worker->HoldBeforeDiskOpen(true);
    REQUIRE(worker->SubmitDisk(1,1,input,AuthoredPageWorker::MaxReserved)==AuthoredPageWorker::SubmitStatus::Queued);
    REQUIRE(Until([&]{return worker->Snapshot().active==1;}));worker.reset();
}

TEST_CASE("Shared controlled Shape pilot has deterministic actual packed source and unchanged helper geometry", "[geometry-page-offline-pilot]")
{
    Foundation::CaptureMainThread();ControlledClodSource first,second;
    REQUIRE(BuildControlledClodSource(first)==ControlledClodStatus::Built);
    REQUIRE(BuildControlledClodSource(second)==ControlledClodStatus::Built);
    REQUIRE(first.original.coarse.vertices.size()==4);REQUIRE(first.original.coarse.indices.size()==6);
    REQUIRE(first.original.fine.vertices.size()==289);REQUIRE(first.original.fine.indices.size()==512*3);
    REQUIRE(first.original.source==second.original.source);REQUIRE(first.helperSha256==second.helperSha256);
    REQUIRE(first.helperSha256.size()==64);
    for(const auto* pair:{&first.original.coarse,&first.original.fine}) {
        const auto& other=pair==&first.original.coarse?second.original.coarse:second.original.fine;
        REQUIRE(pair->indices==other.indices);REQUIRE(pair->materials==other.materials);
        REQUIRE(std::memcmp(pair->vertices.data(),other.vertices.data(),pair->vertices.size()*sizeof(SVertex))==0);
        for(auto material:pair->materials)REQUIRE(material==0);
    }
    ControlledClodSource sentinel;sentinel.original.source.sourceSha256[0]=199;ControlledClodStatus wrong=ControlledClodStatus::Built;
    std::thread worker([&]{wrong=BuildControlledClodSource(sentinel);});worker.join();
    REQUIRE(wrong==ControlledClodStatus::WrongOwner);REQUIRE(sentinel.original.source.sourceSha256[0]==199);
}
TEST_CASE("Offline actual Shape CLOD producer roundtrips selected disk file using independently held keys", "[geometry-page-offline-pilot][geometry-page-disk-worker]")
{
    Foundation::CaptureMainThread();ControlledClodSource source;REQUIRE(BuildControlledClodSource(source)==ControlledClodStatus::Built);
    ControlledClodCache cache;REQUIRE(BakeControlledClodCache(cache)==ControlledClodStatus::Built);
    REQUIRE(cache.selected.originalSource==source.original.source);REQUIRE(cache.helperSha256==source.helperSha256);
    REQUIRE(cache.groups>0);REQUIRE(cache.clusters>0);REQUIRE(cache.groups<=128);REQUIRE(cache.clusters<=64);
    REQUIRE(cache.selected.selectedGeometry.coarse.indices.size()<cache.selected.selectedGeometry.fine.indices.size());
    REQUIRE(cache.selected.package.pages.size()<=8);REQUIRE(cache.selected.knownCapacityBytes<=128*1024);
    std::vector<uint8_t> bytes;REQUIRE(EncodeClodDisk(cache.selected,bytes)==ClodDiskStatus::Encoded);REQUIRE(bytes.size()<=128*1024);
    DiskWorkerFiles files;files.Write(bytes);auto expected=files.Input(cache.selected);
    // Expected identities originate in the successful original producer, not in
    // bytes read from the disk artifact or a self-declared file header.
    AuthoredPageWorker worker;REQUIRE(worker.SubmitDisk(1,1,expected,AuthoredPageWorker::MaxReserved)==AuthoredPageWorker::SubmitStatus::Queued);
    REQUIRE(Until([&]{return worker.Snapshot().ready==1;}));auto result=worker.Take(1,1);REQUIRE(result);
    REQUIRE(result->diskStatus==DiskReadStatus::Read);REQUIRE(result->status==DecodeStatus::Decoded);
    CheckOriginal(result->decoded,cache.selected.selectedGeometry.fine);result.reset();
    REQUIRE(Until([&]{return worker.Snapshot().liveJobs==0;}));
    bytes.back()^=1;files.Write(bytes);
    REQUIRE(worker.SubmitDisk(2,2,expected,AuthoredPageWorker::MaxReserved)==AuthoredPageWorker::SubmitStatus::Queued);
    REQUIRE(Until([&]{return worker.Snapshot().ready==1;}));auto changed=worker.Take(2,2);REQUIRE(changed);
    REQUIRE(changed->diskStatus==DiskReadStatus::Invalid);REQUIRE(changed->decoded.pages.empty());
}

TEST_CASE("Disk requests freeze explicit coarse frontier and decode exact selected coarse source", "[geometry-page-disk-frontier][geometry-page-disk-worker]")
{
    const auto source=DiskWorkerPilot();std::vector<uint8_t> bytes;REQUIRE(EncodeClodDisk(source,bytes)==ClodDiskStatus::Encoded);
    DiskWorkerFiles files;files.Write(bytes);auto input=files.Input(source);input->representation=Frontier::Coarse;
    AuthoredPageWorker worker;worker.HoldBeforeDiskOpen(true);
    REQUIRE(worker.SubmitDisk(1,1,input,AuthoredPageWorker::MaxReserved)==AuthoredPageWorker::SubmitStatus::Queued);
    REQUIRE(Until([&]{return worker.Snapshot().active==1;}));input->representation=Frontier::Fine;
    worker.HoldBeforeDiskOpen(false);REQUIRE(Until([&]{return worker.Snapshot().ready==1;}));auto result=worker.Take(1,1);REQUIRE(result);
    REQUIRE(result->requestedRepresentation==Frontier::Coarse);REQUIRE(result->ownership->disk->representation==Frontier::Coarse);
    REQUIRE(result->diskStatus==DiskReadStatus::Read);REQUIRE(result->status==DecodeStatus::Decoded);
    REQUIRE(result->source->identity==source.package.Identity());
    CheckOriginal(result->decoded,source.selectedGeometry.coarse,Frontier::Coarse);
    REQUIRE(result->decoded.pages.size()==source.package.coarsePages);REQUIRE(result->decoded.clusters==source.package.coarseClusters);
    REQUIRE(result->decoded.indices<source.selectedGeometry.fine.indices.size());result.reset();
    REQUIRE(Until([&]{return worker.Snapshot().liveJobs==0;}));
    // Default input contract is still Fine, including its exact requested witness.
    auto fine=files.Input(source);REQUIRE(fine->representation==Frontier::Fine);
    REQUIRE(worker.SubmitDisk(2,2,fine,AuthoredPageWorker::MaxReserved)==AuthoredPageWorker::SubmitStatus::Queued);
    REQUIRE(Until([&]{return worker.Snapshot().ready==1;}));auto next=worker.Take(2,2);REQUIRE(next);
    REQUIRE(next->requestedRepresentation==Frontier::Fine);CheckOriginal(next->decoded,source.selectedGeometry.fine);
}
TEST_CASE("Disk frontier validation refuses unknown enums while failed coarse retains request witness only", "[geometry-page-disk-frontier][geometry-page-disk-worker]")
{
    const auto source=DiskWorkerPilot();DiskWorkerFiles files;auto input=files.Input(source);AuthoredPageWorker worker;
    SECTION("unavailable is not a representation") {input->representation=Frontier::Unavailable;}
    SECTION("unknown enum is not a representation") {input->representation=static_cast<Frontier>(255);}
    SECTION("valid missing coarse returns no decoded geometry") {
        input->representation=Frontier::Coarse;
        REQUIRE(worker.SubmitDisk(1,1,input,AuthoredPageWorker::MaxReserved)==AuthoredPageWorker::SubmitStatus::Queued);
        REQUIRE(Until([&]{return worker.Snapshot().ready==1;}));auto failed=worker.Take(1,1);REQUIRE(failed);
        REQUIRE(failed->requestedRepresentation==Frontier::Coarse);REQUIRE(failed->diskStatus==DiskReadStatus::Missing);
        REQUIRE(failed->status!=DecodeStatus::Decoded);REQUIRE(failed->decoded.representation==Frontier::Unavailable);
        REQUIRE(failed->decoded.pages.empty());REQUIRE(failed->decoded.indices==0);failed.reset();
        REQUIRE(Until([&]{return worker.Snapshot().liveJobs==0;}));return;
    }
    REQUIRE(worker.SubmitDisk(1,1,input,AuthoredPageWorker::MaxReserved)==AuthoredPageWorker::SubmitStatus::Invalid);
    REQUIRE(worker.Snapshot().liveJobs==0);REQUIRE(worker.Snapshot().reservedBytes==0);
}
