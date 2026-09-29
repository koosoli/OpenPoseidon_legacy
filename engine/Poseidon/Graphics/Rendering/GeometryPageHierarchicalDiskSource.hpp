#pragma once
#include <Poseidon/Graphics/Rendering/GeometryPageHierarchicalDiskCodec.hpp>
#include <atomic>
#include <filesystem>
#include <fstream>

namespace Poseidon::GeometryPages
{
struct HierarchicalDiskPageInput
{
    std::filesystem::path path;
    // Frozen owned prefix. Expected keys must come from owner-validated producer
    // authority, not this prefix's untrusted header/page-hash table.
    std::vector<uint8_t> metadataBytes;
    HierarchicalIdentity expectedIdentity;
    std::array<uint8_t,32> expectedMetadataSha256{};
    uint64_t fileBytes=0;
    uint32_t pageId=UINT32_MAX;
};
inline uint64_t HierarchicalDiskPageInputKnownBytes(const HierarchicalDiskPageInput& input)
{
    const uint64_t chars=input.path.native().capacity(),unit=sizeof(std::filesystem::path::value_type);
    if(input.metadataBytes.capacity()>UINT64_MAX-sizeof(input))return UINT64_MAX;
    if(chars>(UINT64_MAX-sizeof(input)-input.metadataBytes.capacity())/unit)return UINT64_MAX;
    return sizeof(input)+chars*unit+input.metadataBytes.capacity();
}
inline bool ValidHierarchicalDiskPageInput(const HierarchicalDiskPageInput& input)
{
    using namespace HierarchicalDiskDetail;
    const auto& path=input.path.native();const auto& identity=input.expectedIdentity;const auto& source=identity.source;
    if(path.empty()||path.size()>1024||input.metadataBytes.size()<HeaderBytes||input.metadataBytes.size()>MaxMetadataBytes||
        HierarchicalDiskPageInputKnownBytes(input)>64*1024||input.fileBytes<input.metadataBytes.size()||input.fileBytes>MaxFileBytes||
        input.pageId>=64||identity.adapterVersion!=1||!HierarchicalDetail::Nonzero(identity.packageSha256)||
        !HierarchicalDetail::Nonzero(source.sourceSha256)||!HierarchicalDetail::Nonzero(input.expectedMetadataSha256)||
        !source.producerVersion||!source.vertexLayout||!source.materialMapping||source.coarseRepresentation==source.fineRepresentation)return false;
    for(auto c:path)if(!c)return false;
    return true; // Actual prefix authentication/graph validation occurs on worker.
}
enum class HierarchicalDiskReadStatus { NotRequested,Read,Missing,Capacity,ReadFailed,Invalid,Unsupported,AllocationFailed,Cancelled };
// Worker-only owned native file reader. Read the authenticated immutable metadata
// snapshot, then one independent <=64KiB range. File size is rechecked on this
// same opened stream. OS open/seek/read and bounded codec calls are synchronous,
// with cancellation checkpoints, not a hard wall-time/OS cancellation guarantee.
// Source/mount/world freshness remains the owner-admission authority's contract.
inline HierarchicalDiskReadStatus ReadHierarchicalDiskPage(const HierarchicalDiskPageInput& input,
    HierarchicalPage& destination,const std::atomic<bool>& cancelled)
{
    if(cancelled.load())return HierarchicalDiskReadStatus::Cancelled;
    if(!ValidHierarchicalDiskPageInput(input))return HierarchicalDiskReadStatus::Invalid;
    try {
        HierarchicalDiskManifest manifest;
        const auto decoded=DecodeHierarchicalManifest(input.metadataBytes,input.fileBytes,input.expectedIdentity,input.expectedMetadataSha256,manifest);
        if(cancelled.load())return HierarchicalDiskReadStatus::Cancelled;
        if(decoded!=HierarchicalDiskStatus::Decoded||input.pageId>=manifest.pages.size()) {
            if(decoded==HierarchicalDiskStatus::AllocationFailed)return HierarchicalDiskReadStatus::AllocationFailed;
            if(decoded==HierarchicalDiskStatus::Capacity)return HierarchicalDiskReadStatus::Capacity;
            return HierarchicalDiskReadStatus::Invalid;
        }
        std::error_code error;const auto type=std::filesystem::status(input.path,error);
        if(error||!std::filesystem::exists(type))return HierarchicalDiskReadStatus::Missing;
        if(!std::filesystem::is_regular_file(type))return HierarchicalDiskReadStatus::Unsupported;
        if(cancelled.load())return HierarchicalDiskReadStatus::Cancelled;
        std::ifstream stream(input.path,std::ios::binary|std::ios::ate);
        if(!stream.is_open())return HierarchicalDiskReadStatus::Missing;
        const auto length=stream.tellg();
        if(length<0)return HierarchicalDiskReadStatus::ReadFailed;
        if(uint64_t(std::streamoff(length))!=input.fileBytes)return HierarchicalDiskReadStatus::ReadFailed;
        if(cancelled.load())return HierarchicalDiskReadStatus::Cancelled;
        const auto& range=manifest.pages[input.pageId];std::vector<uint8_t> bytes(range.bytes);
        stream.seekg(std::streamoff(range.offset),std::ios::beg);
        if(!stream.read(reinterpret_cast<char*>(bytes.data()),std::streamsize(bytes.size())))return HierarchicalDiskReadStatus::ReadFailed;
        stream.seekg(0,std::ios::end);const auto after=stream.tellg();
        if(after<0||uint64_t(std::streamoff(after))!=input.fileBytes)return HierarchicalDiskReadStatus::ReadFailed;
        if(cancelled.load())return HierarchicalDiskReadStatus::Cancelled;
        HierarchicalPage page;const auto status=DecodeHierarchicalDiskPage(manifest,input.pageId,bytes,page);
        if(cancelled.load())return HierarchicalDiskReadStatus::Cancelled;
        if(status!=HierarchicalDiskStatus::Decoded) {
            if(status==HierarchicalDiskStatus::AllocationFailed)return HierarchicalDiskReadStatus::AllocationFailed;
            if(status==HierarchicalDiskStatus::Capacity)return HierarchicalDiskReadStatus::Capacity;
            return HierarchicalDiskReadStatus::Invalid;
        }
        destination=std::move(page);return HierarchicalDiskReadStatus::Read;
    }catch(const std::bad_alloc&){return HierarchicalDiskReadStatus::AllocationFailed;}
      catch(const std::exception&){return HierarchicalDiskReadStatus::ReadFailed;}
}
}
