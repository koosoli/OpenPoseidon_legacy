#pragma once
#include <filesystem>
#include <functional>
#include <string>

namespace Poseidon
{
// Optional pre-tiled provider; never constructs queries for public OSM APIs.
// Both presentation workers share a serialized, cancellable transfer budget.
class EarthContentSource
{
  public:
    static constexpr size_t MaxBytes = 4 * 1024 * 1024;
    explicit EarthContentSource(std::string urlTemplate = {}, bool loopbackTest = false);
    bool Enabled() const { return !_url.empty(); }
    static std::filesystem::path StreamPath(const std::filesystem::path& cache, const char* kind, int latitude,
                                            int longitude);
    bool Ensure(const std::filesystem::path& cache, const char* kind, int latitude, int longitude,
                const std::function<bool()>& cancelled = {});
    void EnsureNeighborhood(const std::filesystem::path& cache, const char* kind, int latitude, int longitude,
                            const std::function<bool()>& cancelled);
    static bool ValidCell(const std::string& bytes, const char* kind);

  private:
    std::string _url;
    bool _loopbackTest = false;
};
} // namespace Poseidon
