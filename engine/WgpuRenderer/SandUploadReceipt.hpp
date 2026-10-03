#pragma once
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>

namespace Poseidon
{
inline bool SandUploadTraceRequested(const char* value)
{
    return value && value[0]=='1' && value[1]=='\0';
}
// Read-only trace: hash the exact little-endian f32 lanes consumed by Rust.
// This is a publication receipt, not a GPU completion or appearance certificate.
struct SandUploadReceipt
{
    uint64_t hash = 14695981039346656037ull;
    size_t negative = 0, positive = 0, minimumIndex = 0;
    float minimum = 0, maximum = 0;
    bool finite = true;
};
inline SandUploadReceipt InspectSandUpload(std::span<const float> data)
{
    SandUploadReceipt result;
    for (size_t i=0;i<data.size();++i)
    {
        const uint32_t bits=std::bit_cast<uint32_t>(data[i]);
        for (unsigned b=0;b<4;++b)
        {
            result.hash^=(bits>>(b*8))&255u;
            result.hash*=1099511628211ull;
        }
        result.finite &= std::isfinite(data[i]);
        if (i<4 || !std::isfinite(data[i])) continue;
        result.negative += data[i]<0;
        result.positive += data[i]>0;
        if (data[i]<result.minimum) {result.minimum=data[i];result.minimumIndex=i-4;}
        result.maximum=std::max(result.maximum,data[i]);
    }
    return result;
}
inline bool DumpSandUpload(std::span<const float> data,const std::filesystem::path& file,
    const std::filesystem::path& fixtureDirectory)
{
    if constexpr (std::endian::native!=std::endian::little) return false;
    // Exactly one 1MiB cell body plus its16-byte header, never an unbounded dump.
    if (data.size()!=262148 || !file.is_absolute() || file.extension()!=".f32" ||
        !fixtureDirectory.is_absolute()) return false;
    std::error_code error;
    if (!std::filesystem::is_directory(fixtureDirectory,error) || error ||
        std::filesystem::is_symlink(fixtureDirectory,error) || error) return false;
    const auto fixtureRoot=std::filesystem::canonical(fixtureDirectory,error);
    if(error) return false;
    const auto parent=std::filesystem::canonical(file.parent_path(),error);
    if(error || parent!=fixtureRoot) return false;
    const bool exists=std::filesystem::exists(file,error);
    if (error || (exists && (!std::filesystem::is_regular_file(file,error) || error ||
                            std::filesystem::is_symlink(file,error) || error))) return false;
    const auto receipt=InspectSandUpload(data);
    if (!receipt.finite || data[2]!=.125f || data[3]<0 || data[3]>.15f ||
        receipt.minimum<-data[3] || receipt.maximum>(data[3]>0?.02f:0.0f)) return false;
    std::ofstream output(file,std::ios::binary|std::ios::trunc);
    output.write(reinterpret_cast<const char*>(data.data()),std::streamsize(data.size()*sizeof(float)));
    output.close();
    return bool(output);
}
}
