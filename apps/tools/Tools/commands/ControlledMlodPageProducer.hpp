#pragma once
#include <Poseidon/Graphics/Rendering/GeometryPageControlledMlodAsset.hpp>
#include <Poseidon/Graphics/Rendering/GeometryPageClodDiskCodec.hpp>
#include "SelectedCutSurfaceCertificate.hpp"
#include "HierarchicalMlodPageProduct.hpp"
#include <optional>
#include <locale>
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
struct Product {
    ClodRamPackage selected;std::vector<uint8_t> encoded;std::string manifest;
    std::optional<HierarchyProduct> hierarchy;
    // Offline-only fixed numeric proof metadata. Default builds do not hash/seal;
    // only BuildOwned's optional successful publication can create this evidence.
    bool RequiresPublicationBinding() const {return _publicationSealed||hierarchy.has_value();}
    bool HasProducedPublicationBinding() const {
        return _publicationSealed&&!encoded.empty()&&encoded.size()<=128*1024&&
            !manifest.empty()&&manifest.size()<=8192&&(!hierarchy||
            (!hierarchy->image.bytes.empty()&&hierarchy->image.bytes.size()<=HierarchicalDiskDetail::MaxFileBytes&&
            hierarchy->metadataBytes>=HierarchicalDiskDetail::HeaderBytes&&hierarchy->metadataBytes<=HierarchicalDiskDetail::MaxMetadataBytes&&
            hierarchy->metadataBytes<=hierarchy->image.bytes.size()&&!hierarchy->authority.empty()&&hierarchy->authority.size()<=4096))&&
            _publicationDigest==PublicationDigest();
    }
private:
    std::array<uint8_t,32> _publicationDigest{};bool _publicationSealed=false;
    std::array<uint8_t,32> PublicationDigest() const {
        Poseidon::Foundation::Sha256 hash;
        constexpr char domain[]="original-mlod-certified-publication-v1";
        hash.Update(domain,sizeof(domain)-1);
        const auto length=[&hash](uint64_t value){std::array<uint8_t,8> bytes{};
            for(unsigned i=0;i<8;++i)bytes[i]=uint8_t(value>>(8*i));hash.Update(bytes.data(),bytes.size());};
        length(encoded.size());hash.Update(encoded.data(),encoded.size());
        length(manifest.size());hash.Update(manifest.data(),manifest.size());
        if(hierarchy) {
            length(hierarchy->image.bytes.size());hash.Update(hierarchy->image.bytes.data(),hierarchy->image.bytes.size());
            length(hierarchy->authority.size());hash.Update(hierarchy->authority.data(),hierarchy->authority.size());
            length(hierarchy->metadataBytes);
            std::vector<uint8_t> key;HierarchicalDiskDetail::Identity(key,hierarchy->image.identity);
            hash.Update(key.data(),key.size());hash.Update(hierarchy->image.metadataSha256.data(),hierarchy->image.metadataSha256.size());
        }
        return ClodDiskDetail::HashBytes(hash);
    }
    friend ProducerStatus BuildOwned(std::vector<uint8_t>,Product&,bool,bool);
};
inline std::string Hex(const std::array<uint8_t,32>& value)
{
    constexpr char digits[]="0123456789abcdef";std::string text;text.reserve(64);
    for(uint8_t b:value){text.push_back(digits[b>>4]);text.push_back(digits[b&15]);}return text;
}
inline std::string Hex64(uint64_t value,bool classic=false)
{std::ostringstream out;if(classic)out.imbue(std::locale::classic());out<<std::hex<<std::setfill('0')<<std::setw(16)<<value;return out.str();}
inline std::string Identity(const SourceIdentity& value,bool classic=false)
{
    std::ostringstream out;if(classic)out.imbue(std::locale::classic());
    out<<"{\"sourceSha256\":\""<<Hex(value.sourceSha256)<<"\",\"geometryOptionsHex\":\""<<Hex64(value.geometryOptions,classic)
       <<"\",\"materialOptionsHex\":\""<<Hex64(value.materialOptions,classic)<<"\",\"producerVersion\":"<<value.producerVersion
       <<",\"coarseRepresentation\":"<<value.coarseRepresentation<<",\"fineRepresentation\":"<<value.fineRepresentation
       <<",\"vertexLayout\":"<<value.vertexLayout<<",\"materialMapping\":"<<value.materialMapping<<"}";
    return out.str();
}
// Explicit offline owner operation. The whole-byte snapshot is used by hash and the
// one original loader call. This is NOT source-path freshness or general asset eligibility.
// Upstream CLOD is synchronous/uninterruptible, with unmeasured scratch/time; these are
// input, logical output and serialized caps, not allocator/RSS/peak-worker guarantees.
inline ProducerStatus BuildOwned(std::vector<uint8_t> bytes,Product& destination,bool sealForSurfaceCertificate=false,bool hierarchyPages=false)
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
        if(hierarchyPages) {
            HierarchyProduct hierarchy;
            const auto status=BuildHierarchyProduct(bake,asset.originalSourceBytes,hierarchy);
            if(status!=HierarchicalDiskStatus::Encoded)return status==HierarchicalDiskStatus::Capacity?ProducerStatus::Capacity:
                status==HierarchicalDiskStatus::AllocationFailed?ProducerStatus::AllocationFailed:ProducerStatus::Invalid;
            if(hierarchy.image.identity.source!=result.selected.originalSource)return ProducerStatus::Invalid;
            result.hierarchy=std::move(hierarchy);
        }
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
           <<",\"knownSourceBytes\":"<<result.selected.knownCapacityBytes;
        if(result.hierarchy)out<<",\"hierarchyPages\":"<<result.hierarchy->authority;
        out<<",\"scope\":\"external-original-byte-producer; independently-verify-input-hash; no-retail/runtime/GPU/source-freshness-or-hard-peak-proof\"\n}\n";
        result.manifest=out.str();if(result.manifest.size()>8192)return ProducerStatus::Capacity;
        if(sealForSurfaceCertificate||hierarchyPages){result._publicationDigest=result.PublicationDigest();result._publicationSealed=true;}
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
// Optional OFFLINE descriptor, separate from the unchanged manifest/disk codec.
// Only a successful producer's actual original/raw key and selected full key are
// authority here. Serialization never turns a refused certificate into success.
inline ProducerStatus BuildSurfaceDescriptor(const Product& product,std::string& destination)
{
    try {
        if(!product.HasProducedPublicationBinding())return ProducerStatus::Unsupported;
        std::vector<uint8_t> canonical;
        if(EncodeClodDisk(product.selected,canonical)!=ClodDiskStatus::Encoded||canonical!=product.encoded)
            return ProducerStatus::Invalid;
        Poseidon::Foundation::Sha256 encodedHash;encodedHash.Update(product.encoded.data(),product.encoded.size());
        SurfaceCertificate::Certificate certificate;
        const auto status=SurfaceCertificate::Build(product.selected,product.selected.originalSource,
            product.selected.package.Identity(),certificate);
        if(status!=SurfaceCertificate::Status::Certified) {
            if(status==SurfaceCertificate::Status::Capacity)return ProducerStatus::Capacity;
            if(status==SurfaceCertificate::Status::AllocationFailed)return ProducerStatus::AllocationFailed;
            return status==SurfaceCertificate::Status::Unsupported?ProducerStatus::Unsupported:ProducerStatus::Invalid;
        }
        const auto& key=certificate.selectedKey;const auto& packing=key.packing;
        std::ostringstream out;out.imbue(std::locale::classic());
        out<<std::setprecision(std::numeric_limits<double>::max_digits10);
        out<<"{\n\"algorithmVersion\":"<<SurfaceCertificate::Certificate::AlgorithmVersion
           <<",\"file\":\"selected.gcd\",\"fileBytes\":"<<product.encoded.size()
           <<",\"fileSha256\":\""<<encodedHash.Hex()<<"\""
           <<",\"originalSource\":"<<Identity(certificate.originalSource,true)
           <<",\"selectedCut\":{\"source\":"<<Identity(key.source,true)
           <<",\"formatVersion\":"<<key.formatVersion<<",\"algorithmVersion\":"<<key.algorithmVersion
           <<",\"packing\":{\"clusterVertices\":"<<packing.clusterVertices<<",\"clusterTriangles\":"<<packing.clusterTriangles
           <<",\"pageBytes\":"<<packing.pageBytes<<"}}"
           <<",\"coarsePackedSha256\":\""<<Hex(certificate.coarsePackedSha256)
           <<"\",\"finePackedSha256\":\""<<Hex(certificate.finePackedSha256)<<"\",\"coarseCutIds\":[";
        for(uint32_t i=0;i<certificate.coarseCutCount;++i){if(i)out<<',';out<<certificate.coarseCut[i];}
        out<<"],\"fineCutIds\":[";
        for(uint32_t i=0;i<certificate.fineCutCount;++i){if(i)out<<',';out<<certificate.fineCut[i];}
        out<<"],\"coarseThresholdBits\":"<<certificate.coarseThresholdBits
           <<",\"fineThresholdBits\":"<<certificate.fineThresholdBits
           <<",\"fineToCoarseUpper\":"<<certificate.fineToCoarse
           <<",\"fineToCoarseUpperBitsHex\":\""<<Hex64(std::bit_cast<uint64_t>(certificate.fineToCoarse),true)
           <<"\",\"coarseToFineUpper\":"<<certificate.coarseToFine
           <<",\"coarseToFineUpperBitsHex\":\""<<Hex64(std::bit_cast<uint64_t>(certificate.coarseToFine),true)
           <<"\",\"hausdorffUpper\":"<<certificate.hausdorffUpper
           <<",\"hausdorffUpperBitsHex\":\""<<Hex64(std::bit_cast<uint64_t>(certificate.hausdorffUpper),true)
           <<"\",\"distanceVisits\":"<<certificate.distanceVisits<<",\"distanceVisitCap\":2000000"
           <<",\"scope\":\"object-space-selected-packed-triangle-unions-only; conservative-symmetric-surface-distance-upper-bound; not-original-source-fidelity/pixels/attributes/UV/normals/topology/all-passes-or-runtime-selection\"\n}\n";
        auto descriptor=out.str();if(!out||descriptor.empty()||descriptor.size()>8192)return ProducerStatus::Capacity;
        destination=std::move(descriptor);return ProducerStatus::Produced;
    }catch(const std::bad_alloc&){return ProducerStatus::AllocationFailed;}
     catch(const std::exception&){return ProducerStatus::Invalid;}
}
// Publish only a fully validated transaction in a NEW caller-owned private directory.
// On write failure remove only our explicit two files (plus optional artifacts)
// and empty directory, never recursively.
inline ProducerStatus Publish(const Product& product,const std::filesystem::path& requested,std::ostream* manifestSink=nullptr,bool surfaceCertificate=false)
{
    namespace fs=std::filesystem;
    fs::path directory;bool created=false;
    try {
        std::string descriptor;
        if(surfaceCertificate) {
            const auto status=BuildSurfaceDescriptor(product,descriptor);
            if(status!=ProducerStatus::Produced)return status;
        }
        if(product.encoded.empty()||product.encoded.size()>128*1024||product.manifest.empty()||product.manifest.size()>8192)
            return ProducerStatus::Invalid;
        if(product.RequiresPublicationBinding()&&!product.HasProducedPublicationBinding())return ProducerStatus::Invalid;
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
        if(surfaceCertificate&&!write(directory/"surface-certificate.json",descriptor))
            throw std::runtime_error("private certificate write failed");
        if(product.hierarchy&&!write(directory/"hierarchy.ghp",product.hierarchy->image.bytes))
            throw std::runtime_error("private hierarchy page write failed");
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
                if(surfaceCertificate)fs::remove(directory/"surface-certificate.json",error);
                if(product.hierarchy)fs::remove(directory/"hierarchy.ghp",error);
                fs::remove(directory/"selected.gcd",error);fs::remove(directory/"manifest.json",error);fs::remove(directory,error);
            }
        }
        return ProducerStatus::IoFailure;
    }
}
inline ProducerStatus ProduceFile(const std::filesystem::path& source,const std::filesystem::path& output,Product& destination,std::ostream* manifestSink=nullptr,bool surfaceCertificate=false,bool hierarchyPages=false)
{
    std::vector<uint8_t> bytes;auto status=ReadSnapshot(source,bytes);if(status!=ProducerStatus::Produced)return status;
    Product result;status=BuildOwned(std::move(bytes),result,surfaceCertificate,hierarchyPages);if(status!=ProducerStatus::Produced)return status;
    // Certificate construction/refusal occurs before the output directory exists.
    // Successful stdout follows ALL requested writes inside the same rollback guard.
    status=Publish(result,output,manifestSink,surfaceCertificate);if(status!=ProducerStatus::Produced)return status;
    destination=std::move(result);return ProducerStatus::Produced;
}
}
