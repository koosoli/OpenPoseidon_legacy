#pragma once
#include <filesystem>
#include <functional>
#include <string>

namespace Poseidon
{
// Optional content provider. Either a pre-tiled HTTPS source (POSEIDON_EARTH_CONTENT_URL with {kind}{lat}{lon}),
// or -- only when the player opts in (POSEIDON_EARTH_CONTENT_OVERPASS, the Whole World menu's OpenStreetMap
// toggle) -- an Overpass API endpoint queried one 0.02-degree cell at a time. Overpass replies are the same
// JSON the prepare scripts cache, so both streams read them unchanged. Every cell is cached; transfers are
// serialized; a failed cell is not retried for a minute (Overpass fair use).
// Both presentation workers share a serialized, cancellable transfer budget.
class EarthContentSource
{
  public:
    static constexpr size_t MaxBytes = 4 * 1024 * 1024;
    explicit EarthContentSource(std::string urlTemplate = {}, bool loopbackTest = false);
    bool Enabled() const { return !_url.empty() || !_overpass.empty(); }
    bool Overpass() const { return !_overpass.empty(); }
    static std::string OverpassQuery(const char* kind, int latitude, int longitude);
    static std::filesystem::path StreamPath(const std::filesystem::path& cache, const char* kind, int latitude,
                                            int longitude);
    bool Ensure(const std::filesystem::path& cache, const char* kind, int latitude, int longitude,
                const std::function<bool()>& cancelled = {}, bool* downloaded = nullptr);
    bool EnsureNeighborhood(const std::filesystem::path& cache, const char* kind, int latitude, int longitude,
                            const std::function<bool()>& cancelled);
    static bool ValidCell(const std::string& bytes, const char* kind);

  private:
    std::string _url;
    std::string _overpass; // Overpass interpreter endpoint (opt-in)
    bool _loopbackTest = false;
};
} // namespace Poseidon
