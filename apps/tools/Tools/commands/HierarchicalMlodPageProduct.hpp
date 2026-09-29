#pragma once
#include <Poseidon/Graphics/Rendering/GeometryPageHierarchicalDiskCodec.hpp>
#include <Poseidon/Graphics/Rendering/GeometryPageClodDiskCodec.hpp>
#include <locale>
#include <sstream>
#include <iomanip>
#include <stdexcept>

namespace PoseidonTools::MlodPages
{
using namespace Poseidon::GeometryPages;
struct HierarchyProduct {
    HierarchicalDiskImage image;
    uint64_t metadataBytes=0;
    std::string authority;
};
// Producer-only authority captured outside the cache. The later owner must
// independently hash the original bytes, whole file and exact metadata prefix.
// A cache header, or this JSON read back from an untrusted cache, is not authority.
inline HierarchicalDiskStatus BuildHierarchyProduct(const ClodBake& bake,uint64_t rawSourceBytes,
    HierarchyProduct& destination)
{
    if(!rawSourceBytes||rawSourceBytes>16*1024*1024)return HierarchicalDiskStatus::Invalid;
    try {
        HierarchicalPackage package;
        const auto status=BuildHierarchicalPackage(bake,package);
        if(status!=HierarchicalBuildStatus::Built)return status==HierarchicalBuildStatus::Capacity?
            HierarchicalDiskStatus::Capacity:status==HierarchicalBuildStatus::AllocationFailed?
            HierarchicalDiskStatus::AllocationFailed:HierarchicalDiskStatus::Invalid;
        HierarchyProduct result;
        auto encoded=EncodeHierarchicalDisk(package,result.image);if(encoded!=HierarchicalDiskStatus::Encoded)return encoded;
        result.metadataBytes=HierarchicalDiskDetail::HeaderBytes+package.groups.size()*HierarchicalDiskDetail::GroupBytes+
            package.clusters.size()*HierarchicalDiskDetail::ClusterBytes+package.pages.size()*HierarchicalDiskDetail::PageRowBytes+
            (package.rootClusters.size()+package.rootPages.size())*4;
        if(result.metadataBytes>result.image.bytes.size())return HierarchicalDiskStatus::Invalid;
        HierarchicalDiskManifest verified;
        if(DecodeHierarchicalManifest(std::span<const uint8_t>(result.image.bytes).first(size_t(result.metadataBytes)),
            result.image.bytes.size(),result.image.identity,result.image.metadataSha256,verified)!=HierarchicalDiskStatus::Decoded)
            return HierarchicalDiskStatus::Invalid;
        for(uint32_t p=0;p<verified.pages.size();++p) {
            const auto& range=verified.pages[p];HierarchicalPage decoded;
            if(DecodeHierarchicalDiskPage(verified,p,std::span<const uint8_t>(result.image.bytes).subspan(size_t(range.offset),range.bytes),
                decoded)!=HierarchicalDiskStatus::Decoded)return HierarchicalDiskStatus::Invalid;
        }
        const auto hex=[](const std::array<uint8_t,32>& digest) {
            constexpr char digits[]="0123456789abcdef";std::string value;value.reserve(64);
            for(auto b:digest){value.push_back(digits[b>>4]);value.push_back(digits[b&15]);}return value;
        };
        const auto hex64=[](uint64_t value) {std::ostringstream out;out.imbue(std::locale::classic());
            out<<std::hex<<std::setfill('0')<<std::setw(16)<<value;return out.str();};
        const auto& source=result.image.identity.source;
        std::ostringstream out;out.imbue(std::locale::classic());
        out<<"{\"file\":\"hierarchy.ghp\",\"fileBytes\":"<<result.image.bytes.size()
           <<",\"fileSha256\":\""<<hex(HierarchicalDiskDetail::Hash(result.image.bytes))
           <<"\",\"metadataOffset\":0,\"metadataBytes\":"<<result.metadataBytes
           <<",\"metadataSha256\":\""<<hex(result.image.metadataSha256)
           <<"\",\"diskCodecSchema\":"<<HierarchicalDiskDetail::Schema
           <<",\"headerBytes\":"<<HierarchicalDiskDetail::HeaderBytes<<",\"sVertexBytes\":"<<sizeof(Poseidon::SVertex)
           <<",\"vertexScalarBytes\":"<<HierarchicalDiskDetail::VertexScalarBytes
           <<",\"sVertexLayoutKeyHex\":\""<<hex64(ClodDiskDetail::LayoutKey())
           <<"\",\"clodLibraryRevision\":\""<<ClodBake::LibraryRevision
           <<"\",\"clodAdapterVersion\":"<<ClodBake::AdapterVersion<<",\"adapterVersion\":"<<result.image.identity.adapterVersion
           <<",\"sourceBytes\":"<<rawSourceBytes<<",\"source\":{\"sourceSha256\":\""<<hex(source.sourceSha256)
           <<"\",\"geometryOptionsHex\":\""<<hex64(source.geometryOptions)<<"\",\"materialOptionsHex\":\""<<hex64(source.materialOptions)
           <<"\",\"producerVersion\":"<<source.producerVersion<<",\"coarseRepresentation\":"<<source.coarseRepresentation
           <<",\"fineRepresentation\":"<<source.fineRepresentation<<",\"vertexLayout\":"<<source.vertexLayout
           <<",\"materialMapping\":"<<source.materialMapping<<"},\"packageSha256\":\""<<hex(result.image.identity.packageSha256)
           <<"\",\"pageCount\":"<<package.pages.size()<<",\"pageByteLimit\":"<<package.pageByteLimit
           <<",\"groupCount\":"<<package.groups.size()<<",\"clusterCount\":"<<package.clusters.size()<<",\"rootPageIds\":[";
        for(size_t p=0;p<package.rootPages.size();++p){if(p)out<<',';out<<package.rootPages[p];}
        out<<"],\"scope\":\"bounded-rigid-single-material-original-MLOD; full-pinned-DAG; independently-addressed-pages; "
            "external-producer-authority-only; no-retail-admission/GPU/source-freshness/hard-peak-or-performance-proof\"}";
        result.authority=out.str();if(!out||result.authority.empty()||result.authority.size()>4096)return HierarchicalDiskStatus::Capacity;
        destination=std::move(result);return HierarchicalDiskStatus::Encoded;
    }catch(const std::bad_alloc&){return HierarchicalDiskStatus::AllocationFailed;}
     catch(const std::exception&){return HierarchicalDiskStatus::Invalid;}
}
}
