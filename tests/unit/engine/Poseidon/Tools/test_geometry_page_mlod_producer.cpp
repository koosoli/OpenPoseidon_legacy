#include <catch2/catch_test_macros.hpp>
#include "../../../../../apps/tools/Tools/commands/ControlledMlodPageProducer.hpp"
#include <Poseidon/Core/Application.hpp>
#include <Poseidon/IO/ParamFileExt.hpp>
#include <Poseidon/Graphics/Rendering/GeometryPageSurfaceAdmission.hpp>
#include <atomic>
#include <chrono>
#include <iterator>

using namespace PoseidonTools::MlodPages;
namespace
{
struct OriginalGrid
{
    std::vector<uint8_t> bytes;
    void U32(uint32_t v){for(unsigned i=0;i<4;++i)bytes.push_back(uint8_t(v>>(i*8)));}
    void F(float v){U32(std::bit_cast<uint32_t>(v));}
    void Text(const char* s){while(*s)bytes.push_back(uint8_t(*s++));}
    void Z(const char* s){Text(s);bytes.push_back(0);}
    void Fixed(const char* s){const auto at=bytes.size();Z(s);bytes.resize(at+64,0);}
    void Tag(const char* s,uint32_t n){bytes.push_back(1);Z(s);U32(n);}
    explicit OriginalGrid(unsigned fine=16,const char* texture="")
    {
        Text("MLOD");U32(0x101);U32(2);
        for(unsigned level=0;level<2;++level){const unsigned n=level?fine:1;
            Text("P3DM");U32(28);U32(256);U32((n+1)*(n+1));U32(1);U32(n*n);U32(0);
            for(unsigned z=0;z<=n;++z)for(unsigned x=0;x<=n;++x){const float px=float(x)/n*4-2,pz=float(z)/n*4-2;
                F(px);F(.9f*(1-px*px/4)*(1-pz*pz/4));F(pz);U32(0);}
            // Match the authored Python fixture, not a global P3DM convention.
            // Existing loader reverses winding and MeshBuild negates normals.
            F(0);F(-1);F(0);
            for(unsigned z=0;z<n;++z)for(unsigned x=0;x<n;++x){U32(4);
                const std::array<std::array<unsigned,2>,4> corners{{{x,z},{x+1,z},{x+1,z+1},{x,z+1}}};
                for(auto p:corners){U32(p[1]*(n+1)+p[0]);U32(0);F(float(p[0])/n);F(float(p[1])/n);}
                U32(0);Z(texture);Z("");}
            Text("TAGG");Tag("#Property#",128);Fixed("autocenter");Fixed("0");Tag("#EndOfFile#",0);F(level?1:10);
        }
    }
};
struct PrivateFiles
{
    std::filesystem::path root,parent,input,output;
    PrivateFiles(){namespace fs=std::filesystem;parent=fs::canonical(fs::temp_directory_path());
        static std::atomic<uint64_t> count=0;
        for(unsigned attempt=0;attempt<16;++attempt){root=parent/("poseidon-mlod-producer-"+
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"-"+std::to_string(++count));
            if(fs::create_directory(root)){input=root/"original.p3d";output=root/"package";return;}}
        throw std::runtime_error("unable to create private test directory");}
    ~PrivateFiles(){namespace fs=std::filesystem;std::error_code e;
        if(root.empty()||fs::is_symlink(root,e)||fs::canonical(root,e).parent_path()!=parent||e)return;
        // Only exact owned paths; never recursive or arbitrary directory deletion.
        fs::remove(output/"selected.gcd",e);fs::remove(output/"manifest.json",e);
        fs::remove(output/"hierarchy.ghp",e);fs::remove(output/"surface-certificate.json",e);fs::remove(output,e);
        fs::remove(input,e);fs::remove(root,e);}
    void Write(const std::vector<uint8_t>& bytes){std::ofstream out(input,std::ios::binary);
        REQUIRE(out.good());out.write(reinterpret_cast<const char*>(bytes.data()),std::streamsize(bytes.size()));REQUIRE(out.good());}
};
}
TEST_CASE("Actual original curved MLOD file produces independently keyed selected disk cuts", "[geometry-page-mlod-producer]")
{
    Poseidon::Foundation::CaptureMainThread();OriginalGrid source;PrivateFiles files;files.Write(source.bytes);
    Product result;REQUIRE(ProduceFile(files.input,files.output,result)==ProducerStatus::Produced);
    REQUIRE(result.selected.originalSource.sourceSha256!=result.selected.package.Identity().source.sourceSha256);
    Poseidon::Foundation::Sha256 raw;raw.Update(source.bytes.data(),source.bytes.size());
    REQUIRE(Hex(result.selected.originalSource.sourceSha256)==raw.Hex());
    REQUIRE(result.selected.selectedGeometry.fine.indices.size()/3==512);
    REQUIRE(result.selected.selectedGeometry.coarse.indices.size()<result.selected.selectedGeometry.fine.indices.size());
    REQUIRE(result.encoded.size()<=128*1024);REQUIRE(result.selected.knownCapacityBytes<=128*1024);
    std::ifstream input(files.output/"selected.gcd",std::ios::binary);
    std::vector<uint8_t> actual{std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};REQUIRE(actual==result.encoded);
    ClodRamPackage decoded;REQUIRE(DecodeClodDisk(actual,result.selected.originalSource,result.selected.package.Identity(),decoded)==ClodDiskStatus::Decoded);
    REQUIRE(decoded.selectedGeometry.fine.indices==result.selected.selectedGeometry.fine.indices);
    REQUIRE(result.manifest.find("\"schemaVersion\":2")!=std::string::npos);
    REQUIRE(result.manifest.find(raw.Hex())!=std::string::npos);
    REQUIRE_FALSE(result.hierarchy.has_value());REQUIRE_FALSE(std::filesystem::exists(files.output/"hierarchy.ghp"));
    REQUIRE(result.manifest.find("hierarchyPages")==std::string::npos);
    // Reuse of an existing output must fail without changing its valid original artifacts.
    Product untouched;REQUIRE(ProduceFile(files.input,files.output,untouched)==ProducerStatus::IoFailure);
    REQUIRE(untouched.encoded.empty());REQUIRE(std::filesystem::file_size(files.output/"selected.gcd")==actual.size());
}
TEST_CASE("Bounded original-file producer refuses before publication transactionally", "[geometry-page-mlod-producer]")
{
    Poseidon::Foundation::CaptureMainThread();PrivateFiles files;OriginalGrid source;auto bytes=source.bytes;
    ProducerStatus expected=ProducerStatus::Invalid;
    SECTION("too small for pinned CLOD") {bytes=OriginalGrid(4).bytes;expected=ProducerStatus::Unsupported;}
    SECTION("unsupported texture") {bytes=OriginalGrid(16,"unapproved.paa").bytes;expected=ProducerStatus::Unsupported;}
    SECTION("truncated") {bytes.pop_back();}
    SECTION("trailing") {bytes.push_back(0);}
    SECTION("oversize before snapshot allocation") {bytes.resize(16*1024*1024+1);expected=ProducerStatus::Capacity;}
    files.Write(bytes);Product kept;kept.manifest="sentinel";
    REQUIRE(ProduceFile(files.input,files.output,kept)==expected);
    REQUIRE(kept.manifest=="sentinel");REQUIRE_FALSE(std::filesystem::exists(files.output));
}
TEST_CASE("Actual source-byte mutation changes producer keys without filename freshness inference", "[geometry-page-mlod-producer]")
{
    Poseidon::Foundation::CaptureMainThread();OriginalGrid source;auto changed=source.bytes;
    const uint32_t value=std::bit_cast<uint32_t>(-1.75f);for(unsigned i=0;i<4;++i)changed[40+i]=uint8_t(value>>(i*8));
    Product first,second;REQUIRE(BuildOwned(source.bytes,first)==ProducerStatus::Produced);REQUIRE(BuildOwned(changed,second)==ProducerStatus::Produced);
    REQUIRE(first.selected.originalSource.sourceSha256!=second.selected.originalSource.sourceSha256);
    REQUIRE(first.selected.package.Identity()!=second.selected.package.Identity());
    ClodRamPackage refused;REQUIRE(DecodeClodDisk(second.encoded,first.selected.originalSource,first.selected.package.Identity(),refused)!=ClodDiskStatus::Decoded);
}

TEST_CASE("Original producer output failure removes only its private artifact pair", "[geometry-page-mlod-producer]")
{
    Poseidon::Foundation::CaptureMainThread();OriginalGrid source;PrivateFiles files;files.Write(source.bytes);
    std::ostringstream refused;refused.setstate(std::ios::badbit);Product kept;kept.manifest="sentinel";
    REQUIRE(ProduceFile(files.input,files.output,kept,&refused)==ProducerStatus::IoFailure);
    REQUIRE(kept.manifest=="sentinel");REQUIRE_FALSE(std::filesystem::exists(files.output));
    REQUIRE(std::filesystem::exists(files.input));
}

TEST_CASE("Actual MLOD hierarchy option publishes independently verified full DAG and page authority", "[geometry-page-mlod-producer][geometry-page-mlod-hierarchy]")
{
    namespace J=Poseidon::GeometryPages::SurfaceAdmission::Detail;
    Poseidon::Foundation::CaptureMainThread();OriginalGrid source;PrivateFiles files;files.Write(source.bytes);
    Product ordinary,result;REQUIRE(BuildOwned(source.bytes,ordinary)==ProducerStatus::Produced);
    std::ostringstream captured;
    REQUIRE(ProduceFile(files.input,files.output,result,&captured,false,true)==ProducerStatus::Produced);
    REQUIRE(result.hierarchy.has_value());REQUIRE(result.HasProducedPublicationBinding());
    REQUIRE(result.encoded==ordinary.encoded);REQUIRE(captured.str()==result.manifest);
    J::Json json;REQUIRE(json.Parse(result.manifest));const auto& root=json.nodes[0];
    const auto* authority=json.Get(root,"hierarchyPages");REQUIRE(authority);REQUIRE(authority->type==J::Json::Object);
    REQUIRE(J::Exact(json,root,"schemaVersion",2));REQUIRE(json.StringIs(json.Get(root,"file"),"selected.gcd"));
    REQUIRE(json.StringIs(json.Get(*authority,"file"),"hierarchy.ghp"));
    REQUIRE(J::Exact(json,*authority,"sourceBytes",source.bytes.size()));
    std::ifstream input(files.output/"hierarchy.ghp",std::ios::binary);
    std::vector<uint8_t> actual{std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
    REQUIRE(actual==result.hierarchy->image.bytes);
    uint64_t fileBytes=0,metadataBytes=0;
    REQUIRE(json.U64(json.Get(*authority,"fileBytes"),fileBytes));REQUIRE(fileBytes==actual.size());
    REQUIRE(json.U64(json.Get(*authority,"metadataBytes"),metadataBytes));REQUIRE(metadataBytes==result.hierarchy->metadataBytes);
    REQUIRE(J::Exact(json,*authority,"metadataOffset",0));REQUIRE(metadataBytes<=32768);
    HierarchicalIdentity expected;std::array<uint8_t,32> metadataSha{},fileSha{};
    REQUIRE(J::Source(json,json.Get(*authority,"source"),expected.source));
    REQUIRE(json.Hash(json.Get(*authority,"packageSha256"),expected.packageSha256));
    REQUIRE(json.U32(json.Get(*authority,"adapterVersion"),expected.adapterVersion));
    REQUIRE(json.Hash(json.Get(*authority,"metadataSha256"),metadataSha));
    REQUIRE(json.Hash(json.Get(*authority,"fileSha256"),fileSha));
    REQUIRE(fileSha==HierarchicalDiskDetail::Hash(actual));
    const auto prefix=std::span<const uint8_t>(actual).first(size_t(metadataBytes));
    REQUIRE(metadataSha==HierarchicalDiskDetail::Hash(prefix));
    REQUIRE(expected.source==result.selected.originalSource);
    REQUIRE(expected.source.sourceSha256==HierarchicalDiskDetail::Hash(source.bytes));
    REQUIRE(expected==result.hierarchy->image.identity);
    REQUIRE(J::Exact(json,*authority,"diskCodecSchema",HierarchicalDiskDetail::Schema));
    REQUIRE(J::Exact(json,*authority,"headerBytes",HierarchicalDiskDetail::HeaderBytes));
    REQUIRE(J::Exact(json,*authority,"sVertexBytes",sizeof(Poseidon::SVertex)));
    REQUIRE(J::Exact(json,*authority,"vertexScalarBytes",HierarchicalDiskDetail::VertexScalarBytes));
    REQUIRE(json.StringIs(json.Get(*authority,"clodLibraryRevision"),ClodBake::LibraryRevision));
    REQUIRE(J::Exact(json,*authority,"clodAdapterVersion",ClodBake::AdapterVersion));
    uint64_t layout=0;REQUIRE(json.Bits(json.Get(*authority,"sVertexLayoutKeyHex"),layout));REQUIRE(layout==ClodDiskDetail::LayoutKey());
    HierarchicalDiskManifest manifest;
    REQUIRE(DecodeHierarchicalManifest(prefix,fileBytes,expected,metadataSha,manifest)==HierarchicalDiskStatus::Decoded);
    REQUIRE(J::Exact(json,*authority,"pageCount",manifest.pages.size()));
    REQUIRE(J::Exact(json,*authority,"pageByteLimit",manifest.metadata.pageByteLimit));
    REQUIRE(J::Exact(json,*authority,"groupCount",manifest.metadata.groups.size()));
    REQUIRE(J::Exact(json,*authority,"clusterCount",manifest.metadata.clusters.size()));
    std::array<uint32_t,64> roots{};uint32_t rootCount=0;
    REQUIRE(json.Cut(json.Get(*authority,"rootPageIds"),roots,rootCount));
    REQUIRE(std::vector<uint32_t>(roots.begin(),roots.begin()+rootCount)==manifest.metadata.rootPages);
    ControlledMlodAsset original;REQUIRE(ExportControlledMlodOwned(source.bytes,original)==ControlledMlodStatus::Exported);
    ClodBake bake;ClodBakeLimits limits;limits.groups=512;limits.clusters=256;limits.indexEntries=131072;limits.storedBytes=2*1024*1024;
    REQUIRE(BakeClodPilot(original.original,true,bake,limits)==ClodBakeStatus::Baked);
    REQUIRE(manifest.metadata.groups.size()==bake.groups.size());REQUIRE(manifest.metadata.clusters.size()==bake.clusters.size());
    for(size_t c=0;c<bake.clusters.size();++c) {
        REQUIRE(manifest.metadata.clusters[c].group==bake.clusters[c].group);
        REQUIRE(manifest.metadata.clusters[c].refined==bake.clusters[c].refined);
    }
    uint64_t next=metadataBytes;std::vector<uint8_t> resident(manifest.pages.size(),1);
    for(uint32_t p=0;p<manifest.pages.size();++p) {
        const auto& range=manifest.pages[p];REQUIRE(range.offset==next);REQUIRE(range.bytes<=65536);next+=range.bytes;
        HierarchicalPage decoded;
        REQUIRE(DecodeHierarchicalDiskPage(manifest,p,std::span<const uint8_t>(actual).subspan(size_t(range.offset),range.bytes),decoded)==HierarchicalDiskStatus::Decoded);
        REQUIRE(decoded.clusters.size()==manifest.metadata.pages[p].clusters.size());
        for(const auto& cluster:decoded.clusters) {
            const auto& baked=bake.clusters[cluster.cluster];
            REQUIRE(cluster.indices.size()==baked.indices.size());
            for(size_t i=0;i<cluster.indices.size();++i)
                REQUIRE(cluster.originalVertexIds[cluster.indices[i]]==baked.indices[i]);
        }
    }
    REQUIRE(next==fileBytes);
    std::vector<float> thresholds{0.f,result.selected.coarseThreshold};
    for(const auto& group:bake.groups)if(group.simplified.error!=FLT_MAX) {
        thresholds.push_back(group.simplified.error);
        thresholds.push_back(std::nextafter(group.simplified.error,0.f));
    }
    for(float threshold:thresholds) {
        ClodCut upstream;REQUIRE(SelectClodCut(bake,threshold,upstream));HierarchicalCutPlan plan;
        REQUIRE(PlanHierarchicalCut(manifest.metadata,threshold,{expected,resident},plan));
        REQUIRE(plan.state==HierarchicalCutState::RequestedCut);REQUIRE(plan.selectedClusters==upstream.clusters);
    }
    std::fill(resident.begin(),resident.end(),0);for(auto p:manifest.metadata.rootPages)resident[p]=1;
    HierarchicalCutPlan fallback;REQUIRE(PlanHierarchicalCut(manifest.metadata,0,{expected,resident},fallback));
    REQUIRE(fallback.state==HierarchicalCutState::RootFallback);REQUIRE(fallback.selectedClusters==manifest.metadata.rootClusters);
}

TEST_CASE("Hierarchy optional publication refuses mutations and rolls back all private artifacts", "[geometry-page-mlod-producer][geometry-page-mlod-hierarchy]")
{
    Poseidon::Foundation::CaptureMainThread();OriginalGrid source;PrivateFiles files;files.Write(source.bytes);
    SECTION("captured stdout failure with both optional artifacts") {
        std::ostringstream refused;refused.setstate(std::ios::badbit);Product kept;kept.manifest="sentinel";
        REQUIRE(ProduceFile(files.input,files.output,kept,&refused,true,true)==ProducerStatus::IoFailure);
        REQUIRE(kept.manifest=="sentinel");REQUIRE_FALSE(kept.hierarchy.has_value());
    }
    SECTION("image corruption") {
        Product product;REQUIRE(BuildOwned(source.bytes,product,false,true)==ProducerStatus::Produced);
        product.hierarchy->image.bytes.back()^=1;
        REQUIRE(Publish(product,files.output)==ProducerStatus::Invalid);
    }
    SECTION("authority mutation") {
        Product product;REQUIRE(BuildOwned(source.bytes,product,false,true)==ProducerStatus::Produced);
        product.hierarchy->image.identity.source.geometryOptions^=1;
        REQUIRE(Publish(product,files.output)==ProducerStatus::Invalid);
    }
    SECTION("manifest or prefix mutation") {
        Product product;REQUIRE(BuildOwned(source.bytes,product,false,true)==ProducerStatus::Produced);
        product.hierarchy->metadataBytes+=4;
        REQUIRE(Publish(product,files.output)==ProducerStatus::Invalid);
    }
    SECTION("removing the hierarchy cannot publish authority for a missing file") {
        Product product;REQUIRE(BuildOwned(source.bytes,product,false,true)==ProducerStatus::Produced);
        product.hierarchy.reset();
        REQUIRE(Publish(product,files.output)==ProducerStatus::Invalid);
    }
    REQUIRE_FALSE(std::filesystem::exists(files.output));REQUIRE(std::filesystem::exists(files.input));
}

TEST_CASE("Hierarchy authority follows actual admitted MLOD bytes and optional refusal is transactional", "[geometry-page-mlod-producer][geometry-page-mlod-hierarchy]")
{
    Poseidon::Foundation::CaptureMainThread();OriginalGrid source;Product first,second;
    REQUIRE(BuildOwned(source.bytes,first,false,true)==ProducerStatus::Produced);
    auto changed=source.bytes;const uint32_t value=std::bit_cast<uint32_t>(-1.75f);
    for(unsigned i=0;i<4;++i)changed[40+i]=uint8_t(value>>(i*8));
    REQUIRE(BuildOwned(changed,second,false,true)==ProducerStatus::Produced);
    REQUIRE(first.hierarchy->image.identity!=second.hierarchy->image.identity);
    REQUIRE(first.hierarchy->image.metadataSha256!=second.hierarchy->image.metadataSha256);
    REQUIRE(first.hierarchy->authority!=second.hierarchy->authority);
    const auto kept=first.hierarchy->image.bytes;const auto authority=first.manifest;
    changed.pop_back();REQUIRE(BuildOwned(changed,first,false,true)==ProducerStatus::Invalid);
    REQUIRE(first.hierarchy->image.bytes==kept);REQUIRE(first.manifest==authority);
}

TEST_CASE("Authored original grid packs upward faces and matches ordinary conversion", "[geometry-page-mlod-producer]")
{
    using namespace Poseidon;
    using namespace Poseidon::GeometryPages;
    Foundation::CaptureMainThread();OriginalGrid source;ControlledMlodAsset controlled;
    REQUIRE(ExportControlledMlodOwned(source.bytes,controlled)==ControlledMlodStatus::Exported);
    REQUIRE((Pars >> "CfgModels").FindEntry("offline_neutral_mesh_packing")==nullptr);
    auto model=Asset::Formats::MLODLoader::loadFromBuffer(
        reinterpret_cast<const char*>(source.bytes.data()),int(source.bytes.size()),"offline-neutral-mesh-packing.p3d");
    REQUIRE(model.compile());Model::ShapeAdapter::AdapterBankTables empty;
    std::unique_ptr<LODShapeWithShadow> legacy;
    {
        // TestApplication exists normally. Only this ordinary offline comparison
        // uses the neutral tool context; restore it on assertion/exception too.
        struct RestoreApplication {decltype(GApp) saved=GApp;~RestoreApplication(){GApp=saved;}} restore;
        GApp=nullptr;legacy.reset(Model::ShapeAdapter::convertToLODShape(model,false,&empty));
    }
    REQUIRE(legacy!=nullptr);
    ShapeExportSelection select;select.controlledAuthoredRigid=true;select.coarseLevel=0;select.fineLevel=1;
    select.source=controlled.original.source;ShapeExport baseline;
    REQUIRE(ExportShapePair(*legacy,select,baseline)==ExportStatus::Exported);
    const auto upward=[](const ExportedMesh& mesh) {
        REQUIRE_FALSE(mesh.vertices.empty());REQUIRE(mesh.indices.size()%3==0);
        for(const auto& vertex:mesh.vertices) {
            REQUIRE(vertex.norm.X()==0);REQUIRE(vertex.norm.Y()==1);REQUIRE(vertex.norm.Z()==0);
        }
        for(size_t i=0;i<mesh.indices.size();i+=3) {
            REQUIRE(mesh.indices[i]<mesh.vertices.size());REQUIRE(mesh.indices[i+1]<mesh.vertices.size());
            REQUIRE(mesh.indices[i+2]<mesh.vertices.size());
            const auto& a=mesh.vertices[mesh.indices[i]].pos;
            const auto& b=mesh.vertices[mesh.indices[i+1]].pos;
            const auto& c=mesh.vertices[mesh.indices[i+2]].pos;
            // Independent x/z determinant: actual packed triangle faces upward.
            const double crossY=(double(b.Z())-a.Z())*(double(c.X())-a.X())-
                                (double(b.X())-a.X())*(double(c.Z())-a.Z());
            REQUIRE(std::isfinite(crossY));REQUIRE(crossY>0);
        }
    };
    for(const auto pair:{std::pair{&controlled.original.coarse,&baseline.coarse},
                         std::pair{&controlled.original.fine,&baseline.fine}}) {
        REQUIRE(pair.first->indices==pair.second->indices);REQUIRE(pair.first->materials==pair.second->materials);
        REQUIRE(pair.first->vertices.size()==pair.second->vertices.size());
        REQUIRE(std::memcmp(pair.first->vertices.data(),pair.second->vertices.data(),
            pair.first->vertices.size()*sizeof(SVertex))==0);
        upward(*pair.first);upward(*pair.second);
    }
    Product selected;REQUIRE(BuildOwned(source.bytes,selected)==ProducerStatus::Produced);
    upward(selected.selected.selectedGeometry.coarse);upward(selected.selected.selectedGeometry.fine);
}
