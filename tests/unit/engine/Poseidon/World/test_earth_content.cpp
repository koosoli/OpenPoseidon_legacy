#include "WgpuRenderer/EarthContentSource.hpp"
#include <cassert>
#include <chrono>
#include <fstream>
#include <iostream>
using Source = Poseidon::EarthContentSource;
int main(int argc, char** argv)
{
    assert(argc == 3);
    const std::string base = argv[1];
    const std::filesystem::path cache = argv[2];
    const auto url = base + "/{kind}/{lat}_{lon}.json";
    assert(!Source(url).Enabled()); // Production never enables plain HTTP.
    assert(!Source("https://user:password@example.test/{kind}/{lat}/{lon}").Enabled());
    assert(!Source("file:///tmp/{kind}/{lat}/{lon}", true).Enabled());
    assert(!Source("https://example.test/{kind}/missing-coordinates").Enabled());
    assert(Source("https://example.test/{kind}/{lat}/{lon}").Enabled());
    assert(Source::ValidCell("{\"elements\":[]}", "features"));
    assert(!Source::ValidCell("{\"elements\":[]} trailing", "features"));
    assert(!Source::ValidCell("{\"elements\":[],\"remark\":\"truncated\"}", "features"));
    assert(!Source::ValidCell("[]", "features"));
    assert(!Source::ValidCell(std::string(Source::MaxBytes + 1, ' '), "features"));
    assert(!Source::ValidCell("{\"elements\":[]}", "../escape"));
    Source source(url, true);
    assert(source.Enabled());
    assert(!source.Ensure(cache, "features", 0, 0, [] { return true; }));
    assert(source.Ensure(cache, "features", 0, 0));
    const auto file = Source::StreamPath(cache, "features", 0, 0);
    assert(std::filesystem::exists(file));
    assert(source.Ensure(cache, "features", 0, 0)); // No repeat request.
    {
        std::ofstream out(file);
        out << "corrupt";
    }
    assert(source.Ensure(cache, "features", 0, 0)); // Repair generated cache.
    for (int lon : {1, 2, 3, 5})
    {
        assert(!source.Ensure(cache, "features", 0, lon));
        assert(!source.Ensure(cache, "features", 0, lon)); // One-minute failure cooldown.
        assert(!std::filesystem::exists(Source::StreamPath(cache, "features", 0, lon)));
    }
    const auto begin = std::chrono::steady_clock::now();
    assert(!source.Ensure(cache, "features", 0, 4,
                          [&] { return std::chrono::steady_clock::now() - begin > std::chrono::milliseconds(150); }));
    assert(std::chrono::steady_clock::now() - begin < std::chrono::seconds(2));
    // Prepared files take priority and never enter streaming eviction.
    std::filesystem::create_directories(cache / "features");
    {
        std::ofstream out(cache / "features/0_7.json");
        out << "{\"elements\":[]}";
    }
    assert(source.Ensure(cache, "features", 0, 7));
    const auto dir = file.parent_path();
    for (int i = 100; i < 229; ++i)
    {
        std::ofstream out(dir / ("0_" + std::to_string(i) + ".json"));
        out << "{\"elements\":[]}";
    }
    {
        std::ofstream out(dir / "interrupted.json.partial");
        out << "unfinished";
    }
    assert(source.Ensure(cache, "features", 0, 6));
    size_t count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir))
    {
        assert(entry.path().extension() != ".partial");
        if (entry.path().extension() == ".json")
            ++count;
    }
    assert(count <= 128 && std::filesystem::exists(cache / "features/0_7.json"));
    // Disk errors must also back off: this path deliberately cannot be a directory.
    const auto blocked = cache / "blocked";
    {
        std::ofstream out(blocked);
        out << "file";
    }
    assert(!source.Ensure(blocked, "buildings", 0, 8));
    assert(!source.Ensure(blocked, "buildings", 0, 8));
    std::cout << "Earth content limits, repair, cancellation, eviction and cooldown PASS\n";
}
