#pragma once
#include <Poseidon/Graphics/Rendering/GeometryPageControlledMlodAsset.hpp>
#include <Poseidon/Graphics/Rendering/GeometryPageClodDiskCodec.hpp>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <stdexcept>
#include <limits>
#include <cmath>
#include <bit>

namespace PoseidonTools::MlodPages
{
using namespace Poseidon::GeometryPages;
enum class ProducerStatus { Produced, Unsupported, Invalid, Capacity, WrongOwner, AllocationFailed, IoFailure };
inline const char* StatusName(ProducerStatus status)
{
    switch(status){case ProducerStatus::Produced:return "Produced";case ProducerStatus::Unsupported:return "Unsupported";
        case ProducerStatus::Invalid:return "Invalid";case ProducerStatus::Capacity:return "Capacity";
        case ProducerStatus::WrongOwner:return "WrongOwner";case ProducerStatus::AllocationFailed:return "AllocationFailed";
        case ProducerStatus::IoFailure:return "IoFailure";}return "Invalid";
}
struct Product { ClodRamPackage selected;std::vector<uint8_t> encoded;std::string manifest; };
inline std::string Hex(const std::array<uint8_t,32>& value)
{
    constexpr char digits[]="0123456789abcdef";std::string text;text.reserve(64);
    for(uint8_t b:value){text.push_back(digits[b>>4]);text.push_back(digits[b&15]);}return text;
}
inline std::string Hex64(uint64_t value)
{std::ostringstream out;out<<std::hex<<std::setfill('0')<<std::setw(16)<<value;return out.str();}
inline std::string Identity(const SourceIdentity& value)
{
    std::ostringstream out;
    out<<"{\"sourceSha256\":\""<<Hex(value.sourceSha256)<<"\",\"geometryOptionsHex\":\""<<Hex64(value.geometryOptions)
       <<"\",\"materialOptionsHex\":\""<<Hex64(value.materialOptions)<<"\",\"producerVersion\":"<<value.producerVersion
       <<",\"coarseRepresentation\":"<<value.coarseRepresentation<<",\"fineRepresentation\":"<<value.fineRepresentation
       <<",\"vertexLayout\":"<<value.vertexLayout<<",\"materialMapping\":"<<value.materialMapping<<"}";
    return out.str();
}
// Explicit offline owner operation. The whole-byte snapshot is used by hash and the
// one original loader call. This is NOT source-path freshness or general asset eligibility.
// Upstream CLOD is synchronous/uninterruptible, with unmeasured scratch/time; these are
// input, logical output and serialized caps, not allocator/RSS/peak-worker guarantees.
inline ProducerStatus BuildOwned(std::vector<uint8_t> bytes,Product& destination)
{
    if(!Poseidon::Foundation::IsMainThread())return ProducerStatus::WrongOwner;
    try {
        ControlledMlodAsset asset;
        const auto exported=ExportControlledMlodOwned(std::move(bytes),asset);
        if(exported!=ControlledMlodStatus::Exported) {
            if(exported==ControlledMlodStatus::Capacity)return ProducerStatus::Capacity;
            if(exported==ControlledMlodStatus::AllocationFailed)return ProducerStatus::AllocationFailed;
            if(exported==ControlledMlodStatus::WrongOwner)return ProducerStatus::WrongOwner;
            return exported==ControlledMlodStatus::Unsupported?ProducerStatus::Unsupported:ProducerStatus::Invalid;
        }
        // Smaller files cannot demonstrate a distinct useful cut with the pinned pilot.
        if(asset.original.fine.indices.size()/3<=64)return ProducerStatus::Unsupported;
        ClodBake bake;ClodBakeLimits limits;
        limits.groups=512;limits.clusters=256;limits.indexEntries=131072;limits.storedBytes=2*1024*1024;
        const auto baked=BakeClodPilot(asset.original,true,bake,limits);
        if(baked!=ClodBakeStatus::Baked) {
            if(baked==ClodBakeStatus::Capacity)return ProducerStatus::Capacity;
            if(baked==ClodBakeStatus::AllocationFailed)return ProducerStatus::AllocationFailed;
            return baked==ClodBakeStatus::Unsupported?ProducerStatus::Unsupported:ProducerStatus::Invalid;
        }
        float threshold=0;
        for(const auto& group:bake.groups)if(group.simplified.error!=FLT_MAX)threshold=std::max(threshold,group.simplified.error);
        if(!(threshold>0)||!std::isfinite(threshold))return ProducerStatus::Unsupported;
        threshold=std::nextafter(threshold,std::numeric_limits<float>::infinity());
        if(!std::isfinite(threshold)||threshold==FLT_MAX)return ProducerStatus::Unsupported;
        ClodCut coarse,fine;ClodDecodedCut decodedCoarse,decodedFine;
        ClodCutDecodeLimits cuts;cuts.clusters=64;cuts.vertexRecords=4096;cuts.indexEntries=8192;cuts.knownPayloadBytes=262144;
        if(!SelectClodCut(bake,threshold,coarse)||!SelectClodCut(bake,0,fine)||coarse.clusters==fine.clusters)
            return ProducerStatus::Unsupported;
        for(unsigned stage=0;stage<2;++stage) {
            const auto status=stage==0?DecodeClodCut(bake,threshold,coarse,decodedCoarse,cuts):
                DecodeClodCut(bake,0,fine,decodedFine,cuts);
            if(status!=ClodCutDecodeStatus::Decoded)return status==ClodCutDecodeStatus::Capacity?ProducerStatus::Capacity:
                status==ClodCutDecodeStatus::AllocationFailed?ProducerStatus::AllocationFailed:ProducerStatus::Invalid;
        }
        Product result;
        const auto packaged=BuildClodRamPackage(bake,threshold,0,decodedCoarse,decodedFine,result.selected);
        if(packaged!=ClodRamStatus::Built)return packaged==ClodRamStatus::Capacity?ProducerStatus::Capacity:
            packaged==ClodRamStatus::AllocationFailed?ProducerStatus::AllocationFailed:ProducerStatus::Invalid;
        const auto encoded=EncodeClodDisk(result.selected,result.encoded);
        if(encoded!=ClodDiskStatus::Encoded)return encoded==ClodDiskStatus::Capacity?ProducerStatus::Capacity:
            encoded==ClodDiskStatus::AllocationFailed?ProducerStatus::AllocationFailed:ProducerStatus::Invalid;
        if(result.encoded.size()>128*1024)return ProducerStatus::Capacity;
        ClodRamPackage verified;
        if(DecodeClodDisk(result.encoded,result.selected.originalSource,result.selected.package.Identity(),verified)!=ClodDiskStatus::Decoded)
            return ProducerStatus::Invalid;
        Poseidon::Foundation::Sha256 fileHash;fileHash.Update(result.encoded.data(),result.encoded.size());
        const auto identity=result.selected.package.Identity();const auto& packing=identity.packing;
        std::ostringstream out;
        out<<"{\n\"schemaVersion\":2,\"producer\":\"controlled-original-mlod\",\"originalSubsetVersion\":1"
           <<",\"file\":\"selected.gcd\",\"fileBytes\":"<<result.encoded.size()<<",\"fileSha256\":\""<<fileHash.Hex()
           <<"\",\"sourceBytes\":"<<asset.originalSourceBytes<<",\"sourceResolutionBits\":["
           <<std::bit_cast<uint32_t>(asset.sourceResolutions[0])<<","<<std::bit_cast<uint32_t>(asset.sourceResolutions[1])
           <<"],\"diskCodecSchema\":"<<ClodDiskDetail::Schema<<",\"sVertexBytes\":"<<sizeof(Poseidon::SVertex)
           <<",\"sVertexLayoutKeyHex\":\""<<Hex64(ClodDiskDetail::LayoutKey())<<"\",\"clodLibraryRevision\":\""<<ClodBake::LibraryRevision
           <<"\",\"clodAdapterVersion\":"<<ClodBake::AdapterVersion<<",\"ramAdapterVersion\":"<<ClodRamPackage::AdapterVersion
           <<",\"originalSource\":"<<Identity(result.selected.originalSource)<<",\"selectedCut\":{\"source\":"<<Identity(identity.source)
           <<",\"formatVersion\":"<<identity.formatVersion<<",\"algorithmVersion\":"<<identity.algorithmVersion
           <<",\"packing\":{\"clusterVertices\":"<<packing.clusterVertices<<",\"clusterTriangles\":"<<packing.clusterTriangles<<",\"pageBytes\":"<<packing.pageBytes
           <<"}},\"coarseThresholdBits\":"<<std::bit_cast<uint32_t>(threshold)<<",\"fineThresholdBits\":0"
           <<",\"originalFineVertices\":"<<asset.original.fine.vertices.size()<<",\"originalFineTriangles\":"<<asset.original.fine.indices.size()/3
           <<",\"authoredFallbackTriangles\":"<<asset.original.coarse.indices.size()/3
           <<",\"coarseTriangles\":"<<result.selected.selectedGeometry.coarse.indices.size()/3
           <<",\"fineTriangles\":"<<result.selected.selectedGeometry.fine.indices.size()/3
           <<",\"coarsePages\":"<<result.selected.package.coarsePages<<",\"finePages\":"<<result.selected.package.pages.size()-result.selected.package.coarsePages
           <<",\"selectedClusters\":"<<result.selected.package.clusters.size()<<",\"bakeGroups\":"<<bake.groups.size()<<",\"bakeClusters\":"<<bake.clusters.size()
           <<",\"knownSourceBytes\":"<<result.selected.knownCapacityBytes
           <<",\"scope\":\"external-original-byte-producer; independently-verify-input-hash; no-retail/runtime/GPU/source-freshness-or-hard-peak-proof\"\n}\n";
        result.manifest=out.str();if(result.manifest.size()>8192)return ProducerStatus::Capacity;
        destination=std::move(result);return ProducerStatus::Produced;
    } catch(const std::bad_alloc&) {return ProducerStatus::AllocationFailed;}
      catch(const std::exception&) {return ProducerStatus::Invalid;}
}
// Native file is opened once; seek length bounds allocation before the exact read.
// Reject size changes/trailing bytes. Symlink/path TOCTOU is not a handle freshness claim.
inline ProducerStatus ReadSnapshot(const std::filesystem::path& path,std::vector<uint8_t>& destination)
{
    try {
        if(path.empty()||path.native().size()>1024)return ProducerStatus::Invalid;
        std::ifstream in(path,std::ios::binary|std::ios::ate);if(!in)return ProducerStatus::IoFailure;
        const auto length=in.tellg();if(length<=0)return ProducerStatus::Invalid;
        if(length>16*1024*1024)return ProducerStatus::Capacity;
        std::vector<uint8_t> bytes(static_cast<size_t>(length));in.seekg(0);
        in.read(reinterpret_cast<char*>(bytes.data()),std::streamsize(bytes.size()));
        if(!in||static_cast<size_t>(in.gcount())!=bytes.size())return ProducerStatus::IoFailure;
        char extra;if(in.get(extra))return ProducerStatus::Invalid;
        if(!in.eof()||in.bad())return ProducerStatus::IoFailure;
        destination=std::move(bytes);return ProducerStatus::Produced;
    } catch(const std::bad_alloc&) {return ProducerStatus::AllocationFailed;}
      catch(const std::exception&) {return ProducerStatus::IoFailure;}
}
// Publish only a fully validated transaction in a NEW caller-owned private directory.
// On write failure remove only our two files + empty directory, never recursively.
inline ProducerStatus Publish(const Product& product,const std::filesystem::path& requested,std::ostream* manifestSink=nullptr)
{
    namespace fs=std::filesystem;
    fs::path directory;bool created=false;
    try {
        if(product.encoded.empty()||product.encoded.size()>128*1024||product.manifest.empty()||product.manifest.size()>8192)
            return ProducerStatus::Invalid;
        if(requested.empty())return ProducerStatus::Invalid;
        const auto path=fs::absolute(requested).lexically_normal();
        if(path.native().size()>1024||path.filename().empty()||path.filename()=="."||path.filename()=="..")return ProducerStatus::Invalid;
        directory=fs::canonical(path.parent_path())/path.filename();
        if(fs::exists(directory)||!fs::create_directory(directory))return ProducerStatus::IoFailure;
        created=true;
        const auto write=[](const fs::path& path,const auto& data) {
            std::ofstream out(path,std::ios::binary|std::ios::trunc);if(!out)return false;
            out.write(reinterpret_cast<const char*>(data.data()),std::streamsize(data.size()));
            if(!out)return false;out.close();return bool(out);
        };
        if(!write(directory/"selected.gcd",product.encoded)||!write(directory/"manifest.json",product.manifest))
            throw std::runtime_error("private publication write failed");
        if(manifestSink) {
            *manifestSink<<product.manifest;manifestSink->flush();
            if(!*manifestSink)throw std::runtime_error("producer manifest output failed");
        }
        return ProducerStatus::Produced;
    } catch(const std::exception&) {
        if(created) {
            std::error_code error;
            // Caller contract forbids concurrent mutation of this new private directory.
            if(!fs::is_symlink(directory,error)&&fs::canonical(directory,error)==directory&&!error) {
                fs::remove(directory/"selected.gcd",error);fs::remove(directory/"manifest.json",error);fs::remove(directory,error);
            }
        }
        return ProducerStatus::IoFailure;
    }
}
inline ProducerStatus ProduceFile(const std::filesystem::path& source,const std::filesystem::path& output,Product& destination,std::ostream* manifestSink=nullptr)
{
    std::vector<uint8_t> bytes;auto status=ReadSnapshot(source,bytes);if(status!=ProducerStatus::Produced)return status;
    Product result;status=BuildOwned(std::move(bytes),result);if(status!=ProducerStatus::Produced)return status;
    status=Publish(result,output,manifestSink);if(status!=ProducerStatus::Produced)return status;
    destination=std::move(result);return ProducerStatus::Produced;
}
}
