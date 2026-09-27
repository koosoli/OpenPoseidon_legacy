#pragma once
#include <Poseidon/Graphics/Rendering/GeometryPageClodDiskCodec.hpp>
#include <filesystem>
#include <fstream>
#include <atomic>
#include <Poseidon/Graphics/Rendering/GeometryPageSurfaceAdmission.hpp>

namespace Poseidon::GeometryPages
{
struct DiskPageInput
{
    std::filesystem::path path;
    SourceIdentity originalSource;
    CacheIdentity selectedCut;
    // Optional immutable startup evidence. Old raw/RAM paths remain unbound.
    std::shared_ptr<const SurfaceAdmission::Proof> surfaceProof;
    Frontier representation=Frontier::Fine; // explicit immutable requested role; old callers remain Fine
    static constexpr size_t MaxPathCharacters=1024;
};
inline uint64_t DiskPageKnownBytes(const DiskPageInput& input)
{
    const uint64_t capacity=input.path.native().capacity(),unit=sizeof(std::filesystem::path::value_type);
    if(capacity>(std::numeric_limits<uint64_t>::max()-sizeof(input))/unit) return std::numeric_limits<uint64_t>::max();
    return sizeof(input)+capacity*unit;
}
inline bool ValidDiskPageInput(const DiskPageInput& input)
{
    const auto& name=input.path.native();
    if((input.representation!=Frontier::Coarse && input.representation!=Frontier::Fine) ||
       name.empty() || name.size()>DiskPageInput::MaxPathCharacters || DiskPageKnownBytes(input)>16*1024) return false;
    for(auto c:name) if(!c) return false;
    if(input.surfaceProof && (!(input.surfaceProof->certificate.originalSource==input.originalSource)||
        !(input.surfaceProof->certificate.selectedKey==input.selectedCut)||!input.surfaceProof->encodedBytes||
        input.surfaceProof->encodedBytes>ClodDiskDetail::MaxBytes))return false;
    bool hash=false,cut=false;for(auto b:input.originalSource.sourceSha256) hash|=b!=0;
    for(auto b:input.selectedCut.source.sourceSha256) cut|=b!=0;
    return hash && cut && input.originalSource.producerVersion && input.originalSource.vertexLayout &&
        input.originalSource.materialMapping && input.originalSource.coarseRepresentation!=input.originalSource.fineRepresentation &&
        input.selectedCut.formatVersion==1 && input.selectedCut.algorithmVersion==2 &&
        input.selectedCut.source.producerVersion && input.selectedCut.source.vertexLayout==sizeof(SVertex) &&
        input.selectedCut.source.materialMapping && input.selectedCut.source.coarseRepresentation==0 && input.selectedCut.source.fineRepresentation==1 &&
        input.selectedCut.packing.clusterVertices>=3 && input.selectedCut.packing.clusterVertices<=256 &&
        input.selectedCut.packing.clusterTriangles && input.selectedCut.packing.clusterTriangles<=256 &&
        input.selectedCut.packing.pageBytes>=64 && input.selectedCut.packing.pageBytes<=1024*1024;
}
enum class DiskReadStatus { NotRequested, Read, Missing, Capacity, ReadFailed, Invalid, Unsupported, AllocationFailed, Cancelled };
// Worker-only caller contract: synchronous native file I/O, bounded disposable
// derived bytes. Expected keys authenticate content, NOT current source-file freshness.
// No renderer/Shape/FFI handles. Destination changes only after full codec success.
// Cancellation has checkpoints; OS file open/read and the bounded codec call are
// synchronous and are not interruptible. No latency or allocator/RSS hard bound.
inline DiskReadStatus ReadDiskPageSource(const DiskPageInput& input,ClodRamPackage& destination,
    const std::atomic<bool>& cancelled)
{
    if(cancelled.load()) return DiskReadStatus::Cancelled;
    if(!ValidDiskPageInput(input)) return DiskReadStatus::Invalid;
    try {
        std::error_code error;const auto type=std::filesystem::status(input.path,error);
        if(error || !std::filesystem::exists(type)) return DiskReadStatus::Missing;
        if(!std::filesystem::is_regular_file(type)) return DiskReadStatus::Unsupported;
        if(cancelled.load()) return DiskReadStatus::Cancelled;
        std::ifstream stream(input.path,std::ios::binary|std::ios::ate);
        if(!stream.is_open()) return DiskReadStatus::Missing;
        const auto length=stream.tellg();
        if(length<0) return DiskReadStatus::ReadFailed;
        if(uint64_t(std::streamoff(length))>ClodDiskDetail::MaxBytes) return DiskReadStatus::Capacity;
        if(uint64_t(std::streamoff(length))<ClodDiskDetail::HeaderBytes) return DiskReadStatus::Invalid;
        if(cancelled.load()) return DiskReadStatus::Cancelled;
        std::vector<uint8_t> bytes(static_cast<size_t>(length));
        stream.seekg(0,std::ios::beg);
        if(!stream.read(reinterpret_cast<char*>(bytes.data()),std::streamsize(bytes.size()))) return DiskReadStatus::ReadFailed;
        // Reject growth as well as short reads; no independently refreshed path lease.
        if(stream.peek()!=std::char_traits<char>::eof() || !stream.eof() || stream.bad()) return DiskReadStatus::ReadFailed;
        if(cancelled.load()) return DiskReadStatus::Cancelled;
        // Exact bytes from THIS read are the hash authority and Decode input.
        if(input.surfaceProof && (bytes.size()!=input.surfaceProof->encodedBytes||
            SurfaceAdmission::Detail::Digest(bytes)!=input.surfaceProof->encodedSha256))return DiskReadStatus::Invalid;
        ClodRamPackage decoded;
        const auto status=DecodeClodDisk(bytes,input.originalSource,input.selectedCut,decoded);
        if(cancelled.load()) return DiskReadStatus::Cancelled;
        if(status!=ClodDiskStatus::Decoded) {
            if(status==ClodDiskStatus::Capacity) return DiskReadStatus::Capacity;
            if(status==ClodDiskStatus::Unsupported) return DiskReadStatus::Unsupported;
            if(status==ClodDiskStatus::AllocationFailed) return DiskReadStatus::AllocationFailed;
            return DiskReadStatus::Invalid;
        }
        if(input.surfaceProof && !SurfaceAdmission::MatchesDecoded(*input.surfaceProof,decoded))return DiskReadStatus::Invalid;
        if(cancelled.load())return DiskReadStatus::Cancelled;
        destination=std::move(decoded);return DiskReadStatus::Read;
    } catch(const std::bad_alloc&) {return DiskReadStatus::AllocationFailed;}
      catch(...) {return DiskReadStatus::ReadFailed;}
}
}
