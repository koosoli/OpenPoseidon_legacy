#include <Poseidon/Graphics/Textures/ColdPaaHandoff.hpp>
#include <Poseidon/Graphics/Textures/TextureBank.hpp>
#include <Poseidon/IO/FileServerMT.hpp>
#include <Poseidon/IO/Streams/FileAccessPolicy.hpp>
#include <catch2/catch_test_macros.hpp>
#include <Poseidon/World/Terrain/ObjectStreamPrepare.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/World/Model/ModelCache.hpp>
#include <Poseidon/World/Model/ShapeAdapter.hpp>
#include <Poseidon/Graphics/Rendering/Shape/Shape.hpp>
#include <Poseidon/Foundation/Threads/ThreadAffinity.hpp>
#include <Poseidon/Graphics/Textures/PreparedTextures.hpp>
#include <Poseidon/IO/Streams/QBStream.hpp>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <thread>
#include <vector>
#include <condition_variable>
#include <mutex>
#include <cstdlib>

namespace
{
using Preparer = Poseidon::ObjectStreamPreparer;
using Request = Preparer::SourceEnvelopeRequest;
using Evidence = Poseidon::Streaming::StaticSourceEnvelopeState;
struct Bytes
{
    std::vector<char> data;
    void U8(uint8_t v) { data.push_back(char(v)); }
    void U16(uint16_t v) { U8(uint8_t(v)); U8(uint8_t(v >> 8)); }
    void U32(uint32_t v) { for (int i=0;i<4;++i) U8(uint8_t(v >> (i*8))); }
    void F32(float v) { uint32_t bits; std::memcpy(&bits,&v,4); U32(bits); }
    void Point(float x,float y,float z) { F32(x); F32(y); F32(z); }
};
// Original authored ODOL7 triangles, with explicit physical/fire/view roles.
// No banks, retail assets, config or renderer enter this worker fixture.
std::vector<char> Original(bool motion)
{
    Bytes b; b.data={'O','D','O','L'}; b.U32(7); b.U32(4);
    for (int lod=0;lod<4;++lod)
    {
        b.U32(3); for(int i=0;i<3;++i)b.U32(0);
        b.U32(3); for(int i=0;i<6;++i)b.F32(0);
        b.U32(3); b.Point(0,0,0); b.Point(1,0,0); b.Point(0,1,0);
        b.U32(3); for(int i=0;i<3;++i)b.Point(0,0,1);
        b.U32(0); b.U32(0);
        b.Point(0,0,0); b.Point(1,1,0); b.Point(.5f,.5f,0); b.F32(1);
        b.U32(0); b.U32(0); b.U32(0);
        b.U32(1); b.U32(0); b.U32(0); b.U16(0xffff); b.U8(3); b.U16(0); b.U16(1); b.U16(2);
        b.U32(0); b.U32(0); b.U32(0);
        b.U32(motion ? 1 : 0);
        if(motion) { b.F32(.25f); b.U32(3); b.Point(100,0,0); b.Point(101,0,0); b.Point(100,1,0); }
        for(int i=0;i<4;++i)b.U32(0);
    }
    b.F32(1); b.F32(1e13f); b.F32(7e15f); b.F32(6e15f);
    b.U32(0); b.F32(1); b.F32(1);
    for(int i=0;i<3;++i)b.U32(0);
    b.Point(0,0,0); b.U32(0); b.U32(0); b.F32(0);
    b.Point(0,0,0); b.Point(1,1,0);
    for(int i=0;i<3;++i)b.Point(0,0,0);
    for(int i=0;i<9;++i)b.F32(0);
    for(int i=0;i<4;++i)b.U8(0);
    b.U8(motion ? 1 : 0); b.U8(0); b.U32(0);
    for(int i=0;i<4;++i)b.F32(0);
    for(int i=0;i<12;++i)b.U8(0xff);
    return b.data;
}

struct SourceFile
{
    std::string path;
    explicit SourceFile(bool motion=false)
    {
        static std::atomic<uint64_t> sequence{0};
        path=(std::filesystem::temp_directory_path()/
            ("op-envelope-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+
             "-"+std::to_string(++sequence)+".p3d")).string();
        const auto bytes=Original(motion);
        std::ofstream file(path,std::ios::binary); file.write(bytes.data(),std::streamsize(bytes.size()));
        if (!file) throw std::runtime_error("original envelope fixture write failed");
    }
    ~SourceFile() { std::error_code error; std::filesystem::remove(path,error); }
};
template<class Predicate> bool Wait(Predicate predicate)
{
    const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(10);
    do { if(predicate())return true; std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
    while(std::chrono::steady_clock::now()<end);
    return predicate();
}
void RequireWorkers(Preparer& prep)
{
    if (!Preparer::AsyncEnabled()) SKIP("Requires asynchronous preparer workers");
    REQUIRE(prep.Running());
}
#ifdef _WIN32
struct PaaWorkerBank
{
    std::filesystem::path path;
    std::string prefix, key;
    int index = -1;
    bool oldUseBanks = GUseFileBanks;
    PaaWorkerBank()
    {
        const auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        prefix = "paa_worker_" + suffix + "\\";
        key = prefix + "member.paa";
        path = std::filesystem::temp_directory_path() / ("paa_worker_" + suffix + ".pbo");
        const std::vector<char> paa = {3, char(0xFF), 0, 0, 4, 0, 4, 0, 16, 0, 0,
            char(0xF0), char(0xF0), char(0xF0), char(0xF0), char(0xF0), char(0xF0), char(0xF0), char(0xF0),
            0, char(0xF8), 0, char(0xF8), 0, 0, 0, 0, 0, 0, 0, 0};
        Bytes bytes;
        const char member[] = "member.paa";
        bytes.data.insert(bytes.data.end(), member, member + sizeof(member));
        bytes.U32(0); bytes.U32(uint32_t(paa.size())); bytes.U32(0); bytes.U32(0); bytes.U32(uint32_t(paa.size()));
        bytes.U8(0); for (int i = 0; i < 5; ++i) bytes.U32(0);
        bytes.data.insert(bytes.data.end(), paa.begin(), paa.end());
        std::ofstream file(path, std::ios::binary);
        file.write(bytes.data.data(), std::streamsize(bytes.data.size()));
        if (!file) throw std::runtime_error("original PAA worker archive write failed");
        file.close();
        Poseidon::render::PreparedTextureStore::Instance().Clear();
        GUseFileBanks = true;
        index = Poseidon::GFileBanks.Add();
        auto name = path; name.replace_extension();
        if (!Poseidon::GFileBanks[index].open(Poseidon::RString(name.string().c_str())))
        {
            Poseidon::GFileBanks.Delete(index); index = -1; GUseFileBanks = oldUseBanks;
            throw std::runtime_error("original PAA worker archive mount failed");
        }
        Poseidon::GFileBanks[index].SetPrefix(Poseidon::RString(prefix.c_str()));
        Poseidon::GFileBanks[index].Lock();
    }
    ~PaaWorkerBank()
    {
        if (index >= 0) Poseidon::GFileBanks.Delete(index);
        Poseidon::render::PreparedTextureStore::Instance().Clear();
        GUseFileBanks = oldUseBanks;
        std::error_code error; std::filesystem::remove(path, error);
    }
};
struct PaaPublicationLatch
{
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false, released = false, hadFacts = false;
    unsigned calls = 0;
    static void Observe(void* context, bool hasFacts)
    {
        auto& self = *static_cast<PaaPublicationLatch*>(context);
        std::unique_lock lock(self.mutex);
        if (++self.calls != 1) return;
        self.entered = true; self.hadFacts = hasFacts; self.cv.notify_all();

        self.cv.wait(lock, [&] { return self.released; });
    }
    void Release() { std::lock_guard lock(mutex); released = true; cv.notify_all(); }
};
#endif
}

#ifdef _WIN32
TEST_CASE("Actual PAA conversion-worker publication cannot borrow a rescued cancellation epoch", "[preparer][paa-worker-publication]")
{
    const char* flag = std::getenv("WGR_OBJECT_STREAM_PBO_TEXTURES");
    if (!flag || flag[0] != '1' || !Poseidon::render::PreparedTextureStore::Enabled())
        SKIP("Run fresh with WGR_OBJECT_STREAM_PBO_TEXTURES=1");
    if (Preparer::WorkerCount() != 1)
        SKIP("Run fresh with WGR_OBJECT_STREAM_ASYNC_WORKERS=1 for deterministic replacement ordering");
    Poseidon::Foundation::CaptureMainThread();
    SourceFile file, replacement;
    PaaWorkerBank bank;
    PaaPublicationLatch latch;
    Poseidon::render::DdsPublicationObserver observer{nullptr, &latch, PaaPublicationLatch::Observe};
    Preparer prep(128ull * 1024 * 1024, nullptr, &observer);
    // Declared after prep: unwinding releases the worker before its destructor
    // joins. Both context and bank remain alive until AFTER that join.
    struct ReleaseBeforeJoin { PaaPublicationLatch& latch; ~ReleaseBeforeJoin() { latch.Release(); } } unblock{latch};
    auto& store = Poseidon::render::PreparedTextureStore::Instance();
    prep.Reset(&file.path, 1); RequireWorkers(prep);
    REQUIRE(prep.SnapshotStats().texturePublicationBytes > 0);
    if (!store.NativeDdsEnabled()) CHECK(prep.SnapshotStats().ddsPublicationBytes == 0);
    auto submit = [&](const std::string& path) {
        REQUIRE(prep.Request(0));
        REQUIRE(Wait([&] { return prep.Query(0) == Preparer::State::Ready; }));
        auto model = prep.Take(0); REQUIRE(model);
        REQUIRE(model->sourcePath == path);
        // Ordinary conversion only: the original authored triangle geometry is
        // parsed unchanged; the test-owned material names the original PAA. No
        // static source/Plain/Ready certification is claimed for modified IR.
        for (auto& lod : model->lodLevels)
            lod.mesh.materials.emplace_back("owned-PAA-worker", bank.key);
        auto tables = std::make_shared<Poseidon::Model::ShapeAdapter::AdapterBankTables>();
        REQUIRE(prep.SubmitConvert(0, model, tables));
    };
    submit(file.path);
    {
        std::unique_lock lock(latch.mutex);
        REQUIRE(latch.cv.wait_for(lock, std::chrono::seconds(10), [&] { return latch.entered; }));
        REQUIRE(latch.hadFacts); // real alpha analysis finished, before store publication
    }
    REQUIRE(prep.Query(0) == Preparer::State::Converting);
    const auto activeDebt = prep.SnapshotStats().conversionReservedBytes;
    const auto cancelledBefore = store.SnapshotStats().paaCancelled;
    const auto storeGeneration = store.Generation();
    REQUIRE(activeDebt >= 64ull * 1024 * 1024);
    CHECK_FALSE(store.HasAlphaFacts(bank.key));
    SECTION("DropStale then ordinary rescue cannot recapture a token for the old job")
    {
        const uint32_t epochs = 0; prep.DropStale(&epochs, 1, 1);
        CHECK_FALSE(prep.Request(0)); // rescues the model job, not its cancelled texture epoch
        CHECK(prep.SnapshotStats().conversionReservedBytes == activeDebt);
        CHECK_FALSE(store.HasAlphaFacts(bank.key));
        latch.Release();
        REQUIRE(Wait([&] { return prep.Query(0) == Preparer::State::Converted; }));
        auto old = prep.TakeConverted(0);
        std::unique_ptr<Poseidon::LODShapeWithShadow> oldShape(old.shape);
        REQUIRE(oldShape); REQUIRE(old.model);
        CHECK_FALSE(store.HasAlphaFacts(bank.key));
        Poseidon::PAABlockChain chain; CHECK_FALSE(store.Take(bank.key, chain));
        CHECK(store.SnapshotStats().paaCancelled == cancelledBefore + 1);
        CHECK(prep.SnapshotStats().conversionReservedBytes == 0);
        submit(file.path); // new owner admission captures the CURRENT epoch
    }
    SECTION("Reset invalidates the old job while same-key replacement is queued")
    {
        prep.Reset(&replacement.path, 1);
        CHECK(store.Generation() == storeGeneration); // Reset proof cannot rely on store.Clear
        CHECK(prep.SnapshotStats().conversionReservedBytes == activeDebt);
        REQUIRE(prep.Request(0));
        CHECK(prep.Query(0) == Preparer::State::Queued);
        latch.Release();
        REQUIRE(Wait([&] { return prep.Query(0) == Preparer::State::Ready; }));
        CHECK_FALSE(store.HasAlphaFacts(bank.key));
        Poseidon::PAABlockChain chain; CHECK_FALSE(store.Take(bank.key, chain));
        CHECK(store.SnapshotStats().paaCancelled == cancelledBefore + 1);
        auto model = prep.Take(0); REQUIRE(model);
        REQUIRE(model->sourcePath == replacement.path);
        CHECK(prep.SnapshotStats().conversionReservedBytes == 0);
        for (auto& lod : model->lodLevels) lod.mesh.materials.emplace_back("replacement-PAA-worker", bank.key);
        auto tables = std::make_shared<Poseidon::Model::ShapeAdapter::AdapterBankTables>();
        REQUIRE(prep.SubmitConvert(0, model, tables));
    }
    SECTION("DropStale without rescue discards the converted payload and texture claims")
    {
        const uint32_t epochs = 0; prep.DropStale(&epochs, 1, 1);
        CHECK(prep.SnapshotStats().conversionReservedBytes == activeDebt);
        latch.Release();
        REQUIRE(Wait([&] { return prep.Query(0) == Preparer::State::Unknown &&
            prep.SnapshotStats().conversionReservedBytes == 0; }));
        CHECK_FALSE(prep.TakeConverted(0).shape);
        CHECK_FALSE(store.HasAlphaFacts(bank.key));
        Poseidon::PAABlockChain chain; CHECK_FALSE(store.Take(bank.key, chain));
        CHECK(store.SnapshotStats().paaCancelled == cancelledBefore + 1);
        submit(file.path);
    }
    REQUIRE(Wait([&] { return prep.Query(0) == Preparer::State::Converted; }));
    auto current = prep.TakeConverted(0);
    std::unique_ptr<Poseidon::LODShapeWithShadow> shape(current.shape);
    REQUIRE(shape); REQUIRE(shape->NLevels() > 0); REQUIRE(shape->Level(0)->NVertex() > 0);
    CHECK(prep.SnapshotStats().conversionReservedBytes == 0);
    REQUIRE(store.HasAlphaFacts(bank.key));
    auto request = Poseidon::GFileBanks[bank.index].CaptureReadRequest("member.paa", true);
    REQUIRE(request);
    Poseidon::render::PreparedAlphaFacts facts;
    REQUIRE(store.CopyAlphaFacts(bank.key, *request, 0xFF03, 4, 4, 16, 4, facts));
    Poseidon::PAABlockChain chain; REQUIRE(store.Take(bank.key, chain));
    CHECK(chain.blocks.size() == 16);
    CHECK_FALSE(store.HasAlphaFacts(bank.key));
}
#endif

TEST_CASE("Worker source envelope is opt-in immutable numeric evidence before Ready", "[preparer][static-source-envelope]")
{
    Poseidon::Foundation::CaptureMainThread();
    SourceFile file; Preparer prep; prep.Reset(&file.path,1); RequireWorkers(prep);
    REQUIRE(prep.SnapshotStats().sourceEnvelopeBytes==0);
    REQUIRE(prep.RequestWithStaticSourceEnvelope(0)==Request::Requested);
    REQUIRE(Wait([&]{return prep.Query(0)==Preparer::State::Ready;}));
    const auto published=prep.QueryStaticSourceEnvelope(0);
    REQUIRE(published.attempted);
    REQUIRE(published.diagnosticParseToken!=0);
    REQUIRE(published.modelIdentity==file.path);
    REQUIRE(published.envelope.State()==Evidence::SourceEvidence);
    REQUIRE(file.path.size()<Poseidon::Streaming::StaticPlainRouteLimits::ShapeBankPathBytes);
    REQUIRE(published.plainSource.State()==Poseidon::Streaming::StaticPlainSourceSummaryState::SourceFacts);
    REQUIRE(published.plainSource.SourceGeneration()==published.generation);
    REQUIRE(published.plainSource.ModelIdentity()==published.modelIdentity);
    REQUIRE(prep.RequestWithStaticSourceEnvelope(0)==Request::TooLate);
    auto model=prep.Take(0); REQUIRE(model);
    const auto reference=Poseidon::Streaming::BuildStaticSourceEnvelope(*model,published.generation,
        Poseidon::Streaming::StaticSourceCoverage::FullCompiledIR);
    REQUIRE(reference.RadiusForGeneration(published.generation)==published.envelope.RadiusForGeneration(published.generation));
    REQUIRE(reference.PayloadBytes()==published.envelope.PayloadBytes());
    model->lodLevels[0].mesh.vertices[0].position.x=10000;
    // Ownership proof only: this numeric snapshot cannot certify the mutated IR.
    REQUIRE(prep.QueryStaticSourceEnvelope(0).envelope.RadiusForGeneration(published.generation)==
        published.envelope.RadiusForGeneration(published.generation));
    model->sourcePath="changed-after-take.p3d";
    model->lodLevels[0].mesh.properties.emplace_back("class","house");
    REQUIRE(prep.QueryStaticSourceEnvelope(0).plainSource.ModelIdentity()==file.path);
    REQUIRE(prep.QueryStaticSourceEnvelope(0).diagnosticParseToken==published.diagnosticParseToken);
    REQUIRE(prep.QueryStaticSourceEnvelope(0).plainSource.State()==Poseidon::Streaming::StaticPlainSourceSummaryState::SourceFacts);
    REQUIRE(prep.SnapshotStats().sourceEnvelopeScans==1);
    REQUIRE(prep.SnapshotStats().sourceEnvelopePublished==1);
    REQUIRE(prep.SnapshotStats().sourceEnvelopeBytes>0);
    const uint32_t epoch=0;
    prep.DropStale(&epoch,1,1); // already Taken/Unknown must also invalidate
    REQUIRE_FALSE(prep.QueryStaticSourceEnvelope(0).attempted);
    REQUIRE(prep.QueryStaticSourceEnvelope(0).envelope.State()==Evidence::Unknown);
    REQUIRE(prep.QueryStaticSourceEnvelope(0).plainSource.State()==Poseidon::Streaming::StaticPlainSourceSummaryState::Unknown);
    REQUIRE(prep.QueryStaticSourceEnvelope(0).diagnosticParseToken==0);
    REQUIRE(prep.RequestWithStaticSourceEnvelope(0)==Request::Requested);
    REQUIRE(Wait([&]{return prep.Query(0)==Preparer::State::Ready;}));
    REQUIRE(prep.QueryStaticSourceEnvelope(0).diagnosticParseToken!=published.diagnosticParseToken);
    prep.Reset(&file.path,1);
    REQUIRE(prep.QueryStaticSourceEnvelope(0).generation!=published.generation);
    REQUIRE_FALSE(prep.QueryStaticSourceEnvelope(0).attempted);
    REQUIRE(prep.SnapshotStats().sourceEnvelopeBytes==0);
}

TEST_CASE("Default preparation does no envelope scan or allocation and late requests refuse", "[preparer][static-source-envelope]")
{
    Poseidon::Foundation::CaptureMainThread();
    SourceFile file; Preparer prep; prep.Reset(&file.path,1); RequireWorkers(prep);
    REQUIRE(prep.Request(0));
    REQUIRE(prep.RequestWithStaticSourceEnvelope(0)==Request::TooLate);
    REQUIRE(Wait([&]{return prep.Query(0)==Preparer::State::Ready;}));
    REQUIRE_FALSE(prep.QueryStaticSourceEnvelope(0).attempted);
    REQUIRE(prep.QueryStaticSourceEnvelope(0).envelope.State()==Evidence::Unknown);
    REQUIRE(prep.QueryStaticSourceEnvelope(0).plainSource.State()==Poseidon::Streaming::StaticPlainSourceSummaryState::Unknown);
    REQUIRE(prep.SnapshotStats().sourceEnvelopeScans==0);
    REQUIRE(prep.SnapshotStats().sourceEnvelopeBytes==0);
    REQUIRE(prep.Take(0));
    REQUIRE(prep.RequestWithStaticSourceEnvelope(0)==Request::Requested);
    REQUIRE(Wait([&]{return prep.Query(0)==Preparer::State::Ready;}));
    REQUIRE(prep.Take(0));
    REQUIRE(prep.Request(0)); // ordinary fresh request retires previous evidence
    REQUIRE_FALSE(prep.QueryStaticSourceEnvelope(0).attempted);
    REQUIRE(prep.QueryStaticSourceEnvelope(0).plainSource.State()==Poseidon::Streaming::StaticPlainSourceSummaryState::Unknown);
    REQUIRE(Wait([&]{return prep.Query(0)==Preparer::State::Ready;}));
    REQUIRE(prep.SnapshotStats().sourceEnvelopeScans==1);
}

TEST_CASE("Opt-in animated source is scanned but cannot publish static evidence", "[preparer][static-source-envelope]")
{
    Poseidon::Foundation::CaptureMainThread();
    SourceFile file(true); Preparer prep; prep.Reset(&file.path,1); RequireWorkers(prep);
    REQUIRE(prep.RequestWithStaticSourceEnvelope(0)==Request::Requested);
    REQUIRE(Wait([&]{return prep.Query(0)==Preparer::State::Ready;}));
    const auto evidence=prep.QueryStaticSourceEnvelope(0);
    REQUIRE(evidence.attempted);
    REQUIRE(evidence.envelope.State()==Evidence::Unknown);
    REQUIRE(evidence.plainSource.State()==Poseidon::Streaming::StaticPlainSourceSummaryState::Unknown);
    REQUIRE(evidence.envelope.HasReason(Poseidon::Streaming::StaticSourceEnvelopeReason::Animation));
    REQUIRE(prep.SnapshotStats().sourceEnvelopeScans==1);
    REQUIRE(prep.SnapshotStats().sourceEnvelopePublished==0);
}

TEST_CASE("Envelope request authority and inventory bounds fail without sidecar allocation", "[preparer][static-source-envelope]")
{
    Poseidon::Foundation::CaptureMainThread();
    SourceFile file; Preparer prep; prep.Reset(&file.path,1); RequireWorkers(prep);
    Request result=Request::Requested;
    std::thread worker([&]{result=prep.RequestWithStaticSourceEnvelope(0);}); worker.join();
    REQUIRE(result==Request::WrongThread);
    REQUIRE(prep.Query(0)==Preparer::State::Unknown);
    REQUIRE(prep.RequestWithStaticSourceEnvelope(1)==Request::Unavailable);
    std::string oversized(Preparer::SourceEnvelopeIdentityLimit+1,'a'); prep.Reset(&oversized,1);
    REQUIRE(prep.RequestWithStaticSourceEnvelope(0)==Request::Capacity);
    REQUIRE(prep.QueryStaticSourceEnvelope(0).modelIdentity.empty());
    REQUIRE(prep.SnapshotStats().sourceEnvelopeBytes==0);
    std::vector<std::string> paths(Preparer::SourceEnvelopeInventoryLimit+1,file.path);
    prep.Reset(paths.data(),paths.size());
    REQUIRE(prep.RequestWithStaticSourceEnvelope(0)==Request::Capacity);
    REQUIRE(prep.SnapshotStats().sourceEnvelopeBytes==0);
}

TEST_CASE("Queued and Parsing opt-ins refuse and external shared IR is never scanned", "[preparer][static-source-envelope][cancellation]")
{
    if (!Preparer::AsyncEnabled()) SKIP("Requires asynchronous preparer workers");
    Poseidon::Foundation::CaptureMainThread();
    SourceFile source;
    static std::atomic<unsigned> entered{0};
    static std::atomic<bool> release{false};
    static std::shared_ptr<Poseidon::Model::Model> shared;
    entered=0; release=false;
    std::string error; bool opened=false;
    shared=Poseidon::ModelCache::LoadLooseFile(source.path,&error,&opened);
    REQUIRE(shared);
    REQUIRE(Poseidon::Streaming::BuildStaticSourceEnvelope(*shared,1,
        Poseidon::Streaming::StaticSourceCoverage::FullCompiledIR).State()==Evidence::SourceEvidence);
    struct RestoreLoader
    {
        Poseidon::ModelCache::ExternalLoader previous=Poseidon::ModelCache::GetExternalLoader();
        ~RestoreLoader() { Poseidon::ModelCache::SetExternalLoader(previous); shared.reset(); }
    } restore;
    Poseidon::ModelCache::SetExternalLoader(+[](const std::string&,std::string&)
    {
        ++entered;
        Wait([]{return release.load();});
        return shared; // deliberately aliases an owner-visible, mutable IR
    });
    Preparer prep;
    struct ReleaseBeforeJoin { ~ReleaseBeforeJoin() { release=true; } } unblock;
    const size_t workers=Preparer::WorkerCount();
    std::vector<std::string> paths(workers+1,"original-envelope-shared.xob");
    paths.back()=source.path;
    prep.Reset(paths.data(),paths.size());
    REQUIRE(prep.RequestWithStaticSourceEnvelope(0)==Request::Requested);
    for(size_t i=1;i<workers;++i) REQUIRE(prep.Request(uint32_t(i)));
    REQUIRE(Wait([&]{return entered.load()==workers;}));
    REQUIRE(prep.RequestWithStaticSourceEnvelope(0)==Request::TooLate); // Parsing
    REQUIRE(prep.Request(uint32_t(workers)));
    REQUIRE(prep.Query(uint32_t(workers))==Preparer::State::Queued);
    REQUIRE(prep.RequestWithStaticSourceEnvelope(uint32_t(workers))==Request::TooLate);
    SECTION("ordinary completion cannot certify external alias")
    {
        release=true;
        REQUIRE(Wait([&]{return prep.Query(0)==Preparer::State::Ready;}));
        REQUIRE(prep.QueryStaticSourceEnvelope(0).attempted);
        REQUIRE(prep.QueryStaticSourceEnvelope(0).envelope.State()==Evidence::Unknown);
        REQUIRE(prep.QueryStaticSourceEnvelope(0).plainSource.State()==Poseidon::Streaming::StaticPlainSourceSummaryState::Unknown);
        REQUIRE(prep.SnapshotStats().sourceEnvelopeScans==0);
    }
    SECTION("DropStale followed by ordinary rescue cannot restore old opt-in")
    {
        std::vector<uint32_t> epochs(paths.size(),0);
        prep.DropStale(epochs.data(),epochs.size(),1);
        REQUIRE_FALSE(prep.Request(0)); // existing parse rescued, intent stays cleared
        release=true;
        REQUIRE(Wait([&]{return prep.Query(0)==Preparer::State::Ready;}));
        REQUIRE_FALSE(prep.QueryStaticSourceEnvelope(0).attempted);
        REQUIRE(prep.SnapshotStats().sourceEnvelopeScans==0);
    }
    SECTION("Reset discards active intent and replacement canonical parse progresses")
    {
        const auto old=prep.QueryStaticSourceEnvelope(0);
        prep.Reset(&source.path,1);
        REQUIRE(prep.RequestWithStaticSourceEnvelope(0)==Request::Requested);
        release=true;
        REQUIRE(Wait([&]{return prep.Query(0)==Preparer::State::Ready;}));
        const auto current=prep.QueryStaticSourceEnvelope(0);
        REQUIRE(current.generation!=old.generation);
        REQUIRE(current.modelIdentity==source.path);
        REQUIRE(current.envelope.State()==Evidence::SourceEvidence);
        REQUIRE(prep.SnapshotStats().sourceEnvelopeScans==1);
    }
}


namespace
{
class DiagnosticLandscape : public Poseidon::Landscape
{
public:
    DiagnosticLandscape() : Landscape(nullptr,nullptr) {}
    void Adopt(const std::vector<std::string>& paths)
    {
        _modernObjectModels.clear();
        for (const auto& path : paths) _modernObjectModels.emplace_back(path.c_str());
        if (!_modernObjectPreparer) _modernObjectPreparer=std::make_unique<Preparer>();
        _modernObjectPreparer->Reset(paths.data(),paths.size());
    }
    Preparer& Prep() { return *_modernObjectPreparer; }
    void Expire()
    { for (auto& entry : _modernSourceDiagnostics) entry.expires=std::chrono::steady_clock::time_point{}; }
};
}

TEST_CASE("Selected source diagnostics own bounded temporary demand but no prepared IR", "[streaming-source-diagnostic]")
{
    Poseidon::Foundation::CaptureMainThread();
    SourceFile file; DiagnosticLandscape land;
    REQUIRE_FALSE(land.HasModernSourceDiagnosticDemand());
    REQUIRE(land.RequestModernSourceDiagnostic(0)==Request::Unavailable);
    std::vector<std::string> paths(9,file.path); land.Adopt(paths); RequireWorkers(land.Prep());
    REQUIRE(land.RequestModernSourceDiagnostic(9)==Request::Unavailable);
    REQUIRE(land.Prep().SnapshotStats().sourceEnvelopeBytes==0);
    Request worker=Request::Requested;
    std::thread([&]{worker=land.RequestModernSourceDiagnostic(0);}).join();
    REQUIRE(worker==Request::WrongThread);
    for (uint32_t i=0;i<8;++i) REQUIRE(land.RequestModernSourceDiagnostic(i)==Request::Requested);
    REQUIRE(land.RequestModernSourceDiagnostic(8)==Request::Capacity);
    REQUIRE(Wait([&]{return land.Prep().Query(0)==Preparer::State::Ready;}));
    using Status=Poseidon::Landscape::ModernSourceDiagnosticStatus;
    const auto before=land.SnapshotModernSourceDiagnostic(0);
    REQUIRE(before.status==Status::LatestSnapshot);
    REQUIRE(before.latestSnapshot.plainSource.State()==Poseidon::Streaming::StaticPlainSourceSummaryState::SourceFacts);
    auto consumed=land.Prep().Take(0); REQUIRE(consumed);
    consumed->sourcePath="camera-consumed-and-mutated.p3d";
    consumed->lodLevels[0].mesh.properties.emplace_back("class","house");
    consumed.reset();
    const auto after=land.SnapshotModernSourceDiagnostic(0);
    REQUIRE(after.status==Status::LatestSnapshot);
    REQUIRE(after.latestSnapshot.plainSource.ModelIdentity()==file.path);
    REQUIRE_FALSE(after.pendingDemand);
    REQUIRE(land.Prep().Request(0));
    REQUIRE(land.Prep().QueryStaticSourceEnvelope(0).diagnosticParseToken==0);
    REQUIRE(land.SnapshotModernSourceDiagnostic(0).status==Status::Interrupted);
    std::vector<uint32_t> newCameraEpochs(9,0);
    land.PreserveModernSourceDiagnosticRequests(newCameraEpochs,6);
    REQUIRE(newCameraEpochs[0]==0); // Old intent cannot preserve a new ordinary parse.
    land.Adopt(paths); // Simulate inventory Reset without the integration hook.
    REQUIRE(land.SnapshotModernSourceDiagnostic(0).status==Status::StaleInventory);
    land.ResetModernSourceDiagnostics();
    REQUIRE(land.SnapshotModernSourceDiagnostic(0).status==Status::NotRequested);
    REQUIRE_FALSE(land.HasModernSourceDiagnosticDemand());
    REQUIRE(land.RequestModernSourceDiagnostic(0)==Request::Requested);
    land.Expire(); land.MaintainModernSourceDiagnostics();
    REQUIRE(land.SnapshotModernSourceDiagnostic(0).status==Status::Expired);
    REQUIRE_FALSE(land.HasModernSourceDiagnosticDemand());
    std::vector<uint32_t> epochs(9,0);
    land.PreserveModernSourceDiagnosticRequests(epochs,7);
    REQUIRE(epochs==std::vector<uint32_t>(9,0));
    land.Prep().DropStale(epochs.data(),epochs.size(),7);
    REQUIRE(Wait([&]{return land.Prep().SnapshotStats().parseReservedBytes==0;}));
    REQUIRE(land.Prep().SnapshotStats().readyPayloadBytes==0);
    REQUIRE_FALSE(land.Prep().QueryStaticSourceEnvelope(0).attempted);
    const auto scansBeforeMissing=land.Prep().SnapshotStats().sourceEnvelopeScans;
    land.Adopt({"diagnostic-original-missing.p3d"});
    land.ResetModernSourceDiagnostics();
    REQUIRE(land.RequestModernSourceDiagnostic(0)==Request::Requested);
    REQUIRE(Wait([&]{return land.Prep().Query(0)==Preparer::State::NotLoose || land.Prep().Query(0)==Preparer::State::Failed;}));
    REQUIRE(land.SnapshotModernSourceDiagnostic(0).status==Status::Refused);
    REQUIRE(land.Prep().SnapshotStats().sourceEnvelopeScans==scansBeforeMissing);
}

TEST_CASE("Source conversion associates only explicitly validated original worker parse jobs", "[preparer][static-source-envelope][ShapeAdapter]")
{
    Poseidon::Foundation::CaptureMainThread();
    SourceFile file; Preparer prep; prep.Reset(&file.path,1); RequireWorkers(prep);
    REQUIRE(prep.RequestWithStaticSourceEnvelope(0)==Request::Requested);
    REQUIRE(Wait([&]{return prep.Query(0)==Preparer::State::Ready;}));
    auto source=prep.QueryStaticSourceEnvelope(0);
    const auto generation=source.generation, token=source.diagnosticParseToken;
    auto model=prep.Take(0); REQUIRE(model);
    auto tables=std::make_shared<Poseidon::Model::ShapeAdapter::AdapterBankTables>();
    Poseidon::Model::ShapeAdapter::BuildAdapterBankTables(*model,*tables);
    // This is the one permitted relabeling, not a new IR/property/vertex snapshot.
    for(char& c : model->sourcePath) if(c>='A' && c<='Z') c=char(c+('a'-'A'));
    const Preparer::SourceEnvelopeSnapshot* explicitSource=&source;
    bool accepted=true;
    SECTION("valid original association is copied into real worker result") {}
    SECTION("ordinary conversion never infers source eligibility") { explicitSource=nullptr; }
    SECTION("wrong generation rejects enqueue") { ++source.generation; accepted=false; }
    SECTION("wrong per-parse token rejects enqueue") { ++source.diagnosticParseToken; accepted=false; }
    SECTION("wrong exact inventory identity rejects enqueue") { source.modelIdentity+="different"; accepted=false; }
    SECTION("unattempted snapshot rejects enqueue") { source.attempted=false; accepted=false; }
    REQUIRE(prep.SubmitConvert(0,model,tables,nullptr,explicitSource)==accepted);
    if(!accepted)
    {
        REQUIRE(prep.Query(0)==Preparer::State::Unknown);
        REQUIRE(prep.SnapshotStats().conversionReservedBytes==0);
        REQUIRE_FALSE(prep.TakeConverted(0).shape);
        return;
    }
    const bool bound=explicitSource!=nullptr;
    source={}; // No borrowed snapshot is available to the worker after enqueue.
    REQUIRE(Wait([&]{return prep.Query(0)==Preparer::State::Converted;}));
    auto result=prep.TakeConverted(0);
    std::unique_ptr<Poseidon::LODShapeWithShadow> shape(result.shape);
    REQUIRE(shape); REQUIRE(result.model==model);
    REQUIRE(result.sourceToken.generation==(bound ? generation : 0));
    REQUIRE(result.sourceToken.diagnosticParseToken==(bound ? token : 0));
    REQUIRE_FALSE(prep.TakeConverted(0).shape);
    REQUIRE(prep.SnapshotStats().conversionReservedBytes==0);
}

TEST_CASE("Source conversion token cannot bind another index or a Reset replacement", "[preparer][static-source-envelope][ShapeAdapter]")
{
    Poseidon::Foundation::CaptureMainThread();
    SourceFile file; Preparer prep;
    std::vector<std::string> paths(2,file.path); prep.Reset(paths.data(),paths.size()); RequireWorkers(prep);
    for(uint32_t i=0;i<2;++i) REQUIRE(prep.RequestWithStaticSourceEnvelope(i)==Request::Requested);
    REQUIRE(Wait([&]{return prep.Query(0)==Preparer::State::Ready && prep.Query(1)==Preparer::State::Ready;}));
    const auto first=prep.QueryStaticSourceEnvelope(0), second=prep.QueryStaticSourceEnvelope(1);
    REQUIRE(first.modelIdentity==second.modelIdentity);
    REQUIRE(first.diagnosticParseToken!=second.diagnosticParseToken);
    auto model=prep.Take(1); REQUIRE(model);
    auto tables=std::make_shared<Poseidon::Model::ShapeAdapter::AdapterBankTables>();
    Poseidon::Model::ShapeAdapter::BuildAdapterBankTables(*model,*tables);
    REQUIRE_FALSE(prep.SubmitConvert(1,model,tables,nullptr,&first));
    REQUIRE(prep.SubmitConvert(1,model,tables,nullptr,&second));
    REQUIRE(Wait([&]{return prep.Query(1)==Preparer::State::Converted;}));
    prep.Reset(paths.data(),paths.size()); // Completed old job cannot cross inventories.
    REQUIRE_FALSE(prep.TakeConverted(1).shape);
    REQUIRE_FALSE(prep.SubmitConvert(1,model,tables,nullptr,&second));
    REQUIRE(prep.RequestWithStaticSourceEnvelope(1)==Request::Requested);
    REQUIRE(Wait([&]{return prep.Query(1)==Preparer::State::Ready;}));
    const auto replacement=prep.QueryStaticSourceEnvelope(1);
    REQUIRE(replacement.generation!=second.generation);
    REQUIRE(replacement.diagnosticParseToken!=second.diagnosticParseToken);
    auto replaced=prep.Take(1); REQUIRE(replaced);
    auto replacementTables=std::make_shared<Poseidon::Model::ShapeAdapter::AdapterBankTables>();
    Poseidon::Model::ShapeAdapter::BuildAdapterBankTables(*replaced,*replacementTables);
    REQUIRE(prep.SubmitConvert(1,replaced,replacementTables,nullptr,&replacement));
    REQUIRE(Wait([&]{return prep.Query(1)==Preparer::State::Converted;}));
    auto result=prep.TakeConverted(1);
    std::unique_ptr<Poseidon::LODShapeWithShadow> shape(result.shape);
    REQUIRE(shape);
    REQUIRE(result.sourceToken.generation==replacement.generation);
    REQUIRE(result.sourceToken.diagnosticParseToken==replacement.diagnosticParseToken);
}

TEST_CASE("Cancelled queued source conversion cannot lend a token to ordinary rescue", "[preparer][static-source-envelope][ShapeAdapter][cancellation]")
{
    if(!Preparer::AsyncEnabled()) SKIP("Requires asynchronous preparer workers");
    Poseidon::Foundation::CaptureMainThread();
    static std::atomic<unsigned> entered{0}; static std::atomic<bool> release{false};
    entered=0; release=false;
    struct RestoreLoader
    {
        Poseidon::ModelCache::ExternalLoader previous=Poseidon::ModelCache::GetExternalLoader();
        ~RestoreLoader() { Poseidon::ModelCache::SetExternalLoader(previous); }
    } restore;
    Poseidon::ModelCache::SetExternalLoader(+[](const std::string&,std::string&) -> std::shared_ptr<Poseidon::Model::Model>
    { ++entered; Wait([]{return release.load();}); return {}; });
    SourceFile file; Preparer prep;
    struct ReleaseBeforeJoin { ~ReleaseBeforeJoin(){release=true;} } unblock;
    const size_t workers=Preparer::WorkerCount();
    std::vector<std::string> paths(workers+1,"source-conversion-barrier.xob"); paths[0]=file.path;
    prep.Reset(paths.data(),paths.size());
    REQUIRE(prep.RequestWithStaticSourceEnvelope(0)==Request::Requested);
    REQUIRE(Wait([&]{return prep.Query(0)==Preparer::State::Ready;}));
    const auto source=prep.QueryStaticSourceEnvelope(0);
    auto model=prep.Take(0); REQUIRE(model);
    auto tables=std::make_shared<Poseidon::Model::ShapeAdapter::AdapterBankTables>();
    Poseidon::Model::ShapeAdapter::BuildAdapterBankTables(*model,*tables);
    for(uint32_t i=1;i<=workers;++i) REQUIRE(prep.Request(i));
    REQUIRE(Wait([&]{return entered.load()==workers;}));
    REQUIRE(prep.SubmitConvert(0,model,tables,nullptr,&source));
    REQUIRE(prep.Query(0)==Preparer::State::Converting); // Real worker queue held behind actual parses.
    std::vector<uint32_t> epochs(paths.size(),1); epochs[0]=0;
    prep.DropStale(epochs.data(),epochs.size(),1);
    REQUIRE(prep.Query(0)==Preparer::State::Unknown);
    REQUIRE_FALSE(prep.SubmitConvert(0,model,tables,nullptr,&source));
    REQUIRE(prep.Request(0)); // Ordinary fresh parse must stay unbound.
    release=true;
    REQUIRE(Wait([&]{return prep.Query(0)==Preparer::State::Ready;}));
    REQUIRE(prep.QueryStaticSourceEnvelope(0).diagnosticParseToken==0);
    auto ordinary=prep.Take(0); REQUIRE(ordinary);
    auto ordinaryTables=std::make_shared<Poseidon::Model::ShapeAdapter::AdapterBankTables>();
    Poseidon::Model::ShapeAdapter::BuildAdapterBankTables(*ordinary,*ordinaryTables);
    REQUIRE(prep.SubmitConvert(0,ordinary,ordinaryTables));
    REQUIRE(Wait([&]{return prep.Query(0)==Preparer::State::Converted;}));
    auto result=prep.TakeConverted(0);
    std::unique_ptr<Poseidon::LODShapeWithShadow> shape(result.shape);
    REQUIRE(shape); REQUIRE(result.model==ordinary);
    REQUIRE(result.sourceToken.generation==0);
    REQUIRE(result.sourceToken.diagnosticParseToken==0);
    REQUIRE(prep.SnapshotStats().conversionReservedBytes==0);
}

TEST_CASE("Diagnostic demand survives camera cancellation only while an actual parse is pending", "[streaming-source-diagnostic][cancellation]")
{
    Poseidon::Foundation::CaptureMainThread();
    if (!Preparer::AsyncEnabled()) SKIP("Requires asynchronous preparer workers");
    static std::atomic<bool> entered{false},release{false};
    entered=false; release=false;
    struct RestoreLoader
    {
        Poseidon::ModelCache::ExternalLoader previous=Poseidon::ModelCache::GetExternalLoader();
        ~RestoreLoader() { Poseidon::ModelCache::SetExternalLoader(previous); }
    } restore;
    Poseidon::ModelCache::SetExternalLoader(+[](const std::string&,std::string&)
    {
        entered=true; Wait([]{return release.load();});
        return std::make_shared<Poseidon::Model::Model>(); // untrusted external source, never audited
    });
    DiagnosticLandscape land;
    struct ReleaseBeforeJoin { ~ReleaseBeforeJoin() { release=true; } } unblock;
    land.Adopt({"diagnostic-original.xob"}); RequireWorkers(land.Prep());
    REQUIRE(land.RequestModernSourceDiagnostic(0)==Request::Requested);
    REQUIRE(Wait([]{return entered.load();}));
    using Status=Poseidon::Landscape::ModernSourceDiagnosticStatus;
    REQUIRE(land.SnapshotModernSourceDiagnostic(0).status==Status::Pending);
    std::vector<uint32_t> epochs(1,0);
    land.PreserveModernSourceDiagnosticRequests(epochs,7);
    REQUIRE(epochs[0]==7);
    land.Prep().DropStale(epochs.data(),epochs.size(),7);
    release=true;
    REQUIRE(Wait([&]{return land.Prep().Query(0)==Preparer::State::Ready;}));
    const auto latest=land.SnapshotModernSourceDiagnostic(0);
    REQUIRE(latest.status==Status::LatestSnapshot);
    REQUIRE(latest.latestSnapshot.envelope.State()==Evidence::Unknown);
    REQUIRE(latest.latestSnapshot.plainSource.State()==Poseidon::Streaming::StaticPlainSourceSummaryState::Unknown);
    REQUIRE(land.Prep().SnapshotStats().sourceEnvelopeScans==0);
    land.MaintainModernSourceDiagnostics();
    epochs[0]=0;
    land.PreserveModernSourceDiagnosticRequests(epochs,8);
    REQUIRE(epochs[0]==0);
    land.Prep().DropStale(epochs.data(),epochs.size(),8);
    REQUIRE(land.Prep().SnapshotStats().readyPayloadBytes==0);
    REQUIRE(land.SnapshotModernSourceDiagnostic(0).status==Status::Interrupted);
}

#ifdef _WIN32
namespace
{
// Actual original PAA Init + exact mapped buffer provenance; the test texture is
// a no-GPU adapter-table carrier. It does not prove real Wgpu upload/pixels.
class ColdPaaTestServer final : public Poseidon::FileServer
{
public:
    Poseidon::FileCache cache;
    void Request(const char*, float, int, int) override {} void CancelRequest(const char*, int, int) override {}
    void Open(Poseidon::QIFStream& stream, const char* name) override { cache.Open(stream, name); }
    void Start() override {} void Stop() override {} void FlushBank(Poseidon::QFBank* b) override { cache.FlushBank(b); }
};
struct ColdPaaSourceFixture
{
    Poseidon::Ref<Poseidon::FileServer> previous = Poseidon::GFileServer;
    bool caching = GEnableCaching;
    ColdPaaTestServer* server = new ColdPaaTestServer;
    std::unique_ptr<Poseidon::ITextureSource> source;
    Poseidon::render::ColdPaaRead read;
    std::array<Poseidon::PacLevelMem,7> mips;
    explicit ColdPaaSourceFixture(PaaWorkerBank& bank)
    {
        Poseidon::GFileServer = server; GEnableCaching = true;
        try {
        Poseidon::ArchiveSourceBinding::ModelReadScope modelPurpose;
        Poseidon::QIFStreamB buffer; buffer.open(Poseidon::GFileBanks[bank.index], "member.paa");
        server->cache.Store(buffer, bank.key.c_str());
        auto* factory = Poseidon::SelectTextureSourceFactory(bank.key.c_str()); REQUIRE(factory);
        source.reset(factory->Create(bank.key.c_str(), mips.data(), 7)); REQUIRE(source);
        read.key = Poseidon::render::PreparedTextureStore::Key(bank.key);
        read.source = source->GetArchiveSourceBinding(); REQUIRE(read.source);
        REQUIRE(Poseidon::GFileBanks[bank.index].MatchesMountedMember("member.paa", read.source->Request()));
        read.magic = 0xff03; read.count = size_t(source->GetMipmapCount());
        REQUIRE(read.count > 0); REQUIRE(read.count <= 7);
        for(size_t i=0;i<read.count;++i) read.levels[i] = {mips[i]._w,mips[i]._h,16,size_t(mips[i]._start)};
        REQUIRE(read.Valid());
        } catch (...) { source.reset(); read.source.reset(); Poseidon::GFileServer=previous; GEnableCaching=caching; throw; }
    }
    ~ColdPaaSourceFixture() { source.reset(); Poseidon::GFileServer=previous; GEnableCaching=caching; }
};
class ColdPaaTestTexture final : public Poseidon::Texture
{
    Poseidon::render::ColdPaaRead _read;
    Poseidon::PacLevelMem _mip;
public:
    explicit ColdPaaTestTexture(Poseidon::render::ColdPaaRead read) : _read(std::move(read)) {}
    bool CaptureColdPaaRead(Poseidon::render::ColdPaaRead& out) const override { out=_read; return out.Valid(); }
    bool IsGpuResident() const override { return false; }
    void SetMaxSize(int) override {} int AMaxSize() const override { return 4; }
    int AWidth(int=0) const override {return 4;} int AHeight(int=0) const override {return 4;}
    int ANMipmaps() const override {return 1;} void ASetNMipmaps(int) override {}
    Poseidon::AbstractMipmapLevel& AMipmap(int) override {return _mip;}
    const Poseidon::AbstractMipmapLevel& AMipmap(int) const override {return _mip;}
    Poseidon::Color GetPixel(int,float,float) const override {return Poseidon::HBlack;}
    bool IsTransparent() const override {return false;} bool IsAlpha() const override {return true;}
    Poseidon::Color GetColor() override {return Poseidon::HBlack;}
    bool VerifyChecksum(const Poseidon::MipInfo&) const override {return true;}
};
void RequireColdPaaCapability()
{
    Poseidon::Foundation::CaptureMainThread();
    if (!Poseidon::render::ColdPaaHandoffEnabled()) SKIP("Fresh WGR_OBJECT_STREAM_COLD_TEXTURE_HANDOFF=1 process required.");
    if (!Poseidon::QFileAccess::MappingSupported()) SKIP("Actual mapped archive source lease unavailable.");
    if (Preparer::WorkerCount()!=1) SKIP("Deterministic worker replacement requires WGR_OBJECT_STREAM_ASYNC_WORKERS=1.");
}
void SubmitOriginalColdPaa(Preparer& prep, const Poseidon::render::ColdPaaRead& read)
{
    REQUIRE(prep.Request(0)); REQUIRE(Wait([&]{return prep.Query(0)==Preparer::State::Ready;}));
    auto model=prep.Take(0); REQUIRE(model);
    auto tables=std::make_shared<Poseidon::Model::ShapeAdapter::AdapterBankTables>();
    tables->textures.resize(model->lodLevels.size());
    // One actual visual primary dependency, paired with the original triangle.
    model->lodLevels[0].mesh.materials.emplace_back("original-PAA-primary",read.key);
    for(auto& triangle:model->lodLevels[0].mesh.triangles) triangle.materialIndex=0;
    for(auto& triangle:model->lodLevels[0].mesh.triangles) triangle.materialIndex=0;
    tables->textures[0].push_back(new ColdPaaTestTexture(read));
    REQUIRE(prep.SubmitConvert(0,model,tables,nullptr,nullptr,true));
}
}
TEST_CASE("Actual cold conversion owns the Init-bound chain independently of store visibility", "[preparer][cold-paa-handoff]")
{
    RequireColdPaaCapability(); SourceFile file; PaaWorkerBank bank; ColdPaaSourceFixture source(bank);
    Preparer prep; prep.Reset(&file.path,1); RequireWorkers(prep);
    auto& store=Poseidon::render::PreparedTextureStore::Instance(); store.Clear(); store.MarkUploaded(bank.key);
    CHECK_FALSE(store.ShouldPrepare(bank.key));
    SubmitOriginalColdPaa(prep,source.read);
    REQUIRE(Wait([&]{return prep.Query(0)==Preparer::State::Converted;}));
    REQUIRE(prep.SnapshotStats().coldTextureReadyBytes>0);
    CHECK(prep.SnapshotStats().readyPayloadBytes>=prep.SnapshotStats().coldTextureReadyBytes);
    auto converted=prep.TakeConverted(0); std::unique_ptr<Poseidon::LODShapeWithShadow> shape(converted.shape);
    REQUIRE(shape); REQUIRE(converted.coldTexture); CHECK(prep.SnapshotStats().coldTextureReadyBytes==0);
    store.Clear(); // Store Clear/generation and absent entries cannot lose the owned result.
    Poseidon::PAABlockChain claimed;
    { Poseidon::render::ColdPaaAdmissionScope scope(converted.coldTexture.get());
      auto changed=source.read; ++changed.levels[0].header;
      CHECK_FALSE(Poseidon::render::ColdPaaAdmissionScope::Claim(source.read.key,source.read.source,changed,claimed));
      CHECK_FALSE(claimed.valid());
      CHECK_FALSE(Poseidon::render::ColdPaaAdmissionScope::Claim(source.read.key+"wrong",source.read.source,source.read,claimed));
      REQUIRE(Poseidon::render::ColdPaaAdmissionScope::Claim(source.read.key,source.read.source,source.read,claimed));
      Poseidon::PAABlockChain second;
      CHECK_FALSE(Poseidon::render::ColdPaaAdmissionScope::Claim(source.read.key,source.read.source,source.read,second)); }
    std::vector<char> bytes; REQUIRE(source.read.source->Request().Read(bytes)); Poseidon::PAABlockChain expected;
    REQUIRE(Poseidon::ReadPAABlockChainBuffer(bytes.data(),bytes.size(),expected)); CHECK(claimed.blocks==expected.blocks);
    CHECK(claimed.magic==expected.magic); CHECK(claimed.levels[0].sourceHeaderOffset==source.read.levels[0].header);
    size_t legacyBytes=0; for(size_t i=0;i<source.read.count;++i) legacyBytes+=source.read.levels[i].bytes;
    std::vector<uint8_t> legacy(legacyBytes,0);std::vector<Poseidon::MipmapRead> legacyReads(source.read.count);
    size_t at=0;for(size_t i=0;i<source.read.count;++i){legacyReads[i]={legacy.data()+at,&source.mips[i],int(i)};at+=source.read.levels[i].bytes;}
    REQUIRE(source.source->GetMipmapChain(legacyReads.data(),int(legacyReads.size())));
    CHECK(claimed.blocks==legacy); // Independent ordinary PacLevelMem source reader, not the worker parser oracle.

    CHECK(prep.SnapshotStats().coldTexturePrepared==1);
    REQUIRE(Wait([&]{return prep.SnapshotStats().conversionReservedBytes==0;}));
    // A different actual physical archive with matching key/header cannot claim this source.
    PaaWorkerBank otherBank; ColdPaaSourceFixture other(otherBank);
    auto inventory=Poseidon::render::DdsPublicationInventory::Create(1);
    auto payload=Poseidon::render::PrepareColdPaa(source.read,{inventory,0,inventory->Epoch(0)},[]{return false;}); REQUIRE(payload);
    { Poseidon::render::ColdPaaAdmissionScope scope(payload.get()); Poseidon::PAABlockChain refused;
      CHECK_FALSE(Poseidon::render::ColdPaaAdmissionScope::Claim(source.read.key,other.read.source,source.read,refused));
      CHECK_FALSE(refused.valid()); }
    store.Clear();
}
TEST_CASE("Cold chain cancellation keeps actual worker debt until exit and replacement progresses", "[preparer][cold-paa-handoff]")
{
    RequireColdPaaCapability(); SourceFile file,replacement; PaaWorkerBank bank; ColdPaaSourceFixture source(bank);
    PaaPublicationLatch latch;
    Poseidon::render::DdsPublicationObserver observer{nullptr,&latch,PaaPublicationLatch::Observe};
    Preparer prep(128ull*1024*1024,nullptr,&observer);
    struct UnblockBeforeJoin {PaaPublicationLatch& latch;~UnblockBeforeJoin(){latch.Release();}} unblock{latch};
    prep.Reset(&file.path,1); RequireWorkers(prep); SubmitOriginalColdPaa(prep,source.read);
    {std::unique_lock lock(latch.mutex);REQUIRE(latch.cv.wait_for(lock,std::chrono::seconds(10),[&]{return latch.entered;}));}
    REQUIRE(prep.Query(0)==Preparer::State::Converting);
    const auto debt=prep.SnapshotStats().conversionReservedBytes; REQUIRE(debt>=64ull*1024*1024);
    SECTION("Same-world DropStale cannot preserve the old chain")
    {const uint32_t epochs=0;prep.DropStale(&epochs,1,1);CHECK(prep.SnapshotStats().conversionReservedBytes==debt);}
    SECTION("Reset cancels without depending on store Clear")
    {const auto generation=Poseidon::render::PreparedTextureStore::Instance().Generation();
     prep.Reset(&replacement.path,1);CHECK(Poseidon::render::PreparedTextureStore::Instance().Generation()==generation);
     CHECK(prep.SnapshotStats().conversionReservedBytes==debt);}
    latch.Release(); REQUIRE(Wait([&]{return prep.SnapshotStats().conversionReservedBytes==0;}));
    CHECK_FALSE(prep.TakeConverted(0).coldTexture); CHECK(prep.SnapshotStats().coldTextureReadyBytes==0);
    SubmitOriginalColdPaa(prep,source.read); REQUIRE(Wait([&]{return prep.Query(0)==Preparer::State::Converted;}));
    auto converted=prep.TakeConverted(0);std::unique_ptr<Poseidon::LODShapeWithShadow> shape(converted.shape);
    REQUIRE(shape);REQUIRE(converted.coldTexture);
    prep.Reset(&replacement.path,1);
    {Poseidon::render::ColdPaaAdmissionScope scope(converted.coldTexture.get());Poseidon::PAABlockChain out;
     CHECK_FALSE(Poseidon::render::ColdPaaAdmissionScope::Claim(source.read.key,source.read.source,source.read,out));}
}
TEST_CASE("Cold optional Ready-byte refusal preserves ordinary converted geometry", "[preparer][cold-paa-handoff]")
{
    RequireColdPaaCapability();SourceFile file;PaaWorkerBank bank;ColdPaaSourceFixture source(bank);
    Preparer prep(1);prep.Reset(&file.path,1);RequireWorkers(prep);SubmitOriginalColdPaa(prep,source.read);
    REQUIRE(Wait([&]{return prep.Query(0)==Preparer::State::Converted;}));
    CHECK(prep.SnapshotStats().coldTextureRefused>0);CHECK(prep.SnapshotStats().coldTextureReadyBytes==0);
    auto converted=prep.TakeConverted(0);std::unique_ptr<Poseidon::LODShapeWithShadow> shape(converted.shape);
    REQUIRE(shape);CHECK_FALSE(converted.coldTexture);REQUIRE(Wait([&]{return prep.SnapshotStats().conversionReservedBytes==0;}));
}
#endif
TEST_CASE("Cold texture delivery stays unallocated when its dedicated option is OFF", "[preparer][cold-paa-default]")
{
    if(Poseidon::render::ColdPaaHandoffEnabled()) SKIP("Fresh cold handoff OFF process required.");
    Poseidon::Foundation::CaptureMainThread();Preparer prep;const std::string path="cold-default-unused.p3d";prep.Reset(&path,1);
    CHECK(prep.SnapshotStats().coldTextureSlotBytes==0);CHECK(prep.SnapshotStats().coldTextureReadyBytes==0);
}
