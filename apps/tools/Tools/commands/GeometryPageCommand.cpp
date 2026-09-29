#include "GeometryPageCommand.hpp"
#include "ControlledMlodPageProducer.hpp"
#include <Poseidon/Graphics/Rendering/GeometryPageControlledClodPilot.hpp>
#include <CLI/App.hpp>
#include <CLI/Option.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <stdexcept>

namespace PoseidonTools
{
namespace
{
using namespace Poseidon::GeometryPages;
std::string Hex(const std::array<uint8_t,32>& value)
{
    static constexpr char digits[]="0123456789abcdef";std::string text;text.reserve(64);
    for(auto b:value){text.push_back(digits[b>>4]);text.push_back(digits[b&15]);}return text;
}
std::string Hex64(uint64_t value)
{std::ostringstream text;text<<std::hex<<std::setfill('0')<<std::setw(16)<<value;return text.str();}
std::string Identity(const SourceIdentity& value)
{
    std::ostringstream out;
    out<<"{\"sourceSha256\":\""<<Hex(value.sourceSha256)<<"\",\"geometryOptionsHex\":\""<<Hex64(value.geometryOptions)
        <<"\",\"materialOptionsHex\":\""<<Hex64(value.materialOptions)<<"\",\"producerVersion\":"<<value.producerVersion
        <<",\"coarseRepresentation\":"<<value.coarseRepresentation<<",\"fineRepresentation\":"<<value.fineRepresentation
        <<",\"vertexLayout\":"<<value.vertexLayout<<",\"materialMapping\":"<<value.materialMapping<<"}";
    return out.str();
}
bool Write(const std::filesystem::path& path,const void* bytes,size_t length)
{
    std::ofstream file(path,std::ios::binary|std::ios::trunc);if(!file.is_open())return false;
    file.write(static_cast<const char*>(bytes),std::streamsize(length));if(!file)return false;
    file.close();return bool(file);
}
void Produce(const std::string& requested)
{
    namespace fs=std::filesystem;
    fs::path requestedPath=fs::absolute(fs::path(requested)).lexically_normal();
    if(requestedPath.native().size()>1024 || requestedPath.filename().empty() || requestedPath.filename()=="." || requestedPath.filename()=="..")
        throw std::runtime_error("invalid private output directory");
    const auto directory=fs::canonical(requestedPath.parent_path())/requestedPath.filename();
    if(fs::exists(directory))throw std::runtime_error("output directory already exists");
    // Explicit offline owner only. Never called by a game-frame resource path.
    Poseidon::Foundation::CaptureMainThread();
    ControlledClodCache cache;
    if(BakeControlledClodCache(cache)!=ControlledClodStatus::Built) throw std::runtime_error("controlled CLOD bake refused");
    std::vector<uint8_t> encoded;
    if(EncodeClodDisk(cache.selected,encoded)!=ClodDiskStatus::Encoded || encoded.size()>128*1024)
        throw std::runtime_error("bounded disk encoding refused");
    ClodRamPackage verified;
    if(DecodeClodDisk(encoded,cache.selected.originalSource,cache.selected.package.Identity(),verified)!=ClodDiskStatus::Decoded)
        throw std::runtime_error("memory codec roundtrip refused");
    Poseidon::Foundation::Sha256 fileHash;fileHash.Update(encoded.data(),encoded.size());
    const auto identity=cache.selected.package.Identity();const auto& packing=identity.packing;
    std::ostringstream out;
    out<<"{\n\"schemaVersion\":1,\"controlledPilotVersion\":"<<ControlledClodPilotVersion
        <<",\"file\":\"selected.gcd\",\"fileBytes\":"<<encoded.size()<<",\"fileSha256\":\""<<fileHash.Hex()
        <<"\",\"diskCodecSchema\":"<<ClodDiskDetail::Schema<<",\"sVertexBytes\":"<<sizeof(Poseidon::SVertex)
        <<",\"sVertexLayoutKeyHex\":\""<<Hex64(ClodDiskDetail::LayoutKey())<<"\",\"clodLibraryRevision\":\""<<ClodBake::LibraryRevision
        <<"\",\"clodAdapterVersion\":"<<ClodBake::AdapterVersion<<",\"ramAdapterVersion\":"<<ClodRamPackage::AdapterVersion
        <<",\"originalSource\":"<<Identity(cache.selected.originalSource)<<",\"selectedCut\":{\"source\":"<<Identity(identity.source)
        <<",\"formatVersion\":"<<identity.formatVersion<<",\"algorithmVersion\":"<<identity.algorithmVersion
        <<",\"packing\":{\"clusterVertices\":"<<packing.clusterVertices<<",\"clusterTriangles\":"<<packing.clusterTriangles<<",\"pageBytes\":"<<packing.pageBytes
        <<"}},\"coarseThresholdBits\":"<<std::bit_cast<uint32_t>(cache.selected.coarseThreshold)
        <<",\"fineThresholdBits\":"<<std::bit_cast<uint32_t>(cache.selected.fineThreshold)
        <<",\"coarsePages\":"<<cache.selected.package.coarsePages<<",\"finePages\":"<<cache.selected.package.pages.size()-cache.selected.package.coarsePages
        <<",\"coarseTriangles\":"<<cache.selected.selectedGeometry.coarse.indices.size()/3
        <<",\"fineTriangles\":"<<cache.selected.selectedGeometry.fine.indices.size()/3
        <<",\"originalFineVertices\":"<<cache.originalFineVertices<<",\"originalFineTriangles\":"<<cache.originalFineTriangles
        <<",\"authoredFallbackTriangles\":"<<cache.authoredFallbackTriangles
        <<",\"selectedClusters\":"<<cache.selected.package.clusters.size()
        <<",\"bakeGroups\":"<<cache.groups<<",\"bakeClusters\":"<<cache.clusters
        <<",\"knownSourceBytes\":"<<cache.selected.knownCapacityBytes<<",\"helperSha256\":\""<<cache.helperSha256
        <<"\",\"scope\":\"original-controlled-pilot-only; external-producer-manifest; no-retail-eligibility-or-GPU-claim\"\n}\n";
    const auto manifest=out.str();if(manifest.size()>8192)throw std::runtime_error("manifest capacity refused");
    // Caller must provide a NEW private directory. Never overwrite an existing
    // artifact pair; incomplete failures remain there for diagnosis, with no success.
    if(!fs::create_directory(directory))throw std::runtime_error("output directory already exists");
    if(!Write(directory/"selected.gcd",encoded.data(),encoded.size()) ||
       !Write(directory/"manifest.json",manifest.data(),manifest.size()))throw std::runtime_error("private artifact write failed");
    // Root runner captures THIS separate producer output as expected keys and
    // verifies the actual file hash. Never derive trusted keys from the cache file.
    std::cout<<manifest;std::cout.flush();if(!std::cout)throw std::runtime_error("producer manifest output failed");
}
}
void GeometryPageCommand::Setup(CLI::App& app)
{
    auto command=app.add_subcommand("geometry-page-pilot","Offline original controlled CLOD disk package producer; no retail assets");
    auto output=std::make_shared<std::string>();
    command->add_option("--output-directory",*output,"New private directory; parent must exist")->required();
    command->callback([output] {try {Produce(*output);} catch(const std::exception& error) {std::cerr<<"geometry-page-pilot: "<<error.what()<<"\n";throw CLI::RuntimeError(2);} });
    auto mlod=app.add_subcommand("geometry-page-mlod","Offline actual original controlled MLOD subset to selected CLOD disk package; no runtime/retail gate");
    auto input=std::make_shared<std::string>();auto originalOutput=std::make_shared<std::string>();
    auto surfaceCertificate=std::make_shared<bool>(false);
    auto hierarchyPages=std::make_shared<bool>(false);
    mlod->add_option("--input",*input,"Actual original MLOD file; whole owned snapshot <=16 MiB")->required();
    mlod->add_option("--output-directory",*originalOutput,"NEW private output directory; parent must exist")->required();
    mlod->add_flag("--surface-certificate",*surfaceCertificate,"Optional separate object-space selected-surface upper-bound descriptor; no pixel/fidelity/runtime certificate");
    mlod->add_flag("--hierarchy-pages",*hierarchyPages,"Optional hierarchy.ghp with complete pinned DAG and independently addressed pages; captures separate trusted producer authority");
    mlod->callback([input,originalOutput,surfaceCertificate,hierarchyPages] {
        try {
            Poseidon::Foundation::CaptureMainThread();
            MlodPages::Product result;const auto status=MlodPages::ProduceFile(*input,*originalOutput,result,&std::cout,*surfaceCertificate,*hierarchyPages);
            if(status!=MlodPages::ProducerStatus::Produced)
                throw std::runtime_error(std::string("bounded original MLOD producer refused, status=")+MlodPages::StatusName(status));
            // Caller captures independent producer stdout and verifies ORIGINAL input hash.
            // Declared hashes inside selected.gcd alone never authorize an expected key.
            // ProduceFile publishes stdout while its owned-directory rollback guard is active.
        } catch(const std::exception& error) {
            std::cerr<<"geometry-page-mlod: "<<error.what()<<"\n";throw CLI::RuntimeError(2);
        }
    });
}
}
