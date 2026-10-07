// Roadmap Phase 13 -- record/replay evidence. See
// design notes
//
// `StateTimeline` could only ever compare two recordings held in ONE process, which
// confined it to fixtures. The questions it was built for are about whole missions:
// "does rationing this AI loop change what the AI does" needs two runs of a real
// scene and a byte-exact diff between them. That needs a file.
//
// What this file guards is mostly the failure side. A serialiser that round-trips is
// easy; a serialiser that REFUSES a truncated file is the part that matters, because
// a short read would otherwise produce a short timeline, and two short timelines
// compare equal and pass the gate they were supposed to fail.
//
// No game data required.

#include <catch2/catch_test_macros.hpp>

#include <Poseidon/Core/StateTimeline.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace Poseidon::Determinism;

namespace
{

std::string TempPath(const char* stem)
{
    auto p = std::filesystem::temp_directory_path() / (std::string("poseidon-timeline-") + stem + ".bin");
    return p.string();
}

/// A recording with several ticks, both field kinds, both widths, and values chosen
/// so a value-based comparison would pass where a bit-based one must not: -0.0f and
/// a quiet NaN. If the file format ever re-encodes rather than carrying bits, these
/// are the two that break.
StateTimeline MakeSample()
{
    StateTimeline t;
    const NameId  unit = t.Intern("unit");
    const NameId  posX = t.Intern("posX");
    const NameId  target = t.Intern("targetId");
    const NameId  alive = t.Intern("alive");

    for (int tick = 0; tick < 4; tick++)
    {
        t.BeginTick();
        for (std::uint32_t k = 0; k < 3; k++)
        {
            t.F32(unit, k, posX, static_cast<float>(tick) + static_cast<float>(k) * 0.25f);
            t.U32(unit, k, target, static_cast<std::uint32_t>(tick * 10 + k));
            t.Bool(unit, k, alive, (tick + k) % 2 == 0);
        }
        t.F32(unit, 99, posX, -0.0f);
        t.F32(unit, 98, posX, std::numeric_limits<float>::quiet_NaN());
        t.EndTick();
    }
    return t;
}

std::vector<char> ReadAllBytes(const std::string& path)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    REQUIRE(f);
    const auto        n = f.tellg();
    std::vector<char> buf(static_cast<std::size_t>(n));
    f.seekg(0);
    f.read(buf.data(), n);
    return buf;
}

void WriteAllBytes(const std::string& path, const std::vector<char>& buf)
{
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    REQUIRE(f);
    f.write(buf.data(), static_cast<std::streamsize>(buf.size()));
}

} // namespace

TEST_CASE("A timeline survives a round trip through a file, bit for bit", "[determinism][timeline]")
{
    const StateTimeline original = MakeSample();
    const std::string   path = TempPath("roundtrip");

    REQUIRE(WriteTimeline(original, path).empty());

    StateTimeline restored;
    REQUIRE(ReadTimeline(restored, path).empty());

    // The comparison the gate actually uses, not a field-by-field re-check: if
    // `CompareTimelines` cannot tell these apart then neither can anything downstream.
    const Divergence d = CompareTimelines(original, restored);
    INFO(d.Describe());
    REQUIRE_FALSE(d.Diverged());

    REQUIRE(restored.TickCount() == original.TickCount());
    REQUIRE(restored.EntryCount() == original.EntryCount());
    REQUIRE(restored.RunHash() == original.RunHash());

    // Anti-vacuity. A recording whose ticks all hash alike would round-trip perfectly
    // and prove nothing -- it would agree with any other such recording for ever.
    REQUIRE(restored.DistinctTickHashes() == 4);

    std::remove(path.c_str());
}

// The name must not START with a dash: CTest passes it to Catch2 as a filter and
// Catch2 reads a leading dash as a command-line flag ("Unrecognised token: -0.0f").
TEST_CASE("Negative zero and NaN survive, which is why the format stores bits", "[determinism][timeline]")
{
    StateTimeline t;
    t.BeginTick();
    t.F32("s", 1, "negZero", -0.0f);
    t.F32("s", 2, "nan", std::numeric_limits<float>::quiet_NaN());
    t.EndTick();

    const std::string path = TempPath("bits");
    REQUIRE(WriteTimeline(t, path).empty());

    StateTimeline restored;
    REQUIRE(ReadTimeline(restored, path).empty());

    // Read back as BITS. `-0.0f == 0.0f` and `NaN != NaN`, so a value comparison here
    // would be simultaneously too loose and too tight -- the whole reason the class
    // stores bit patterns.
    float negZero = 0.0f;
    std::memcpy(&negZero, &restored.EntryAt(0, 0).bits, sizeof(negZero));
    REQUIRE(std::signbit(negZero));

    float nan = 0.0f;
    std::memcpy(&nan, &restored.EntryAt(0, 1).bits, sizeof(nan));
    REQUIRE(std::isnan(nan));

    std::remove(path.c_str());
}

TEST_CASE("A truncated file is refused rather than read as a short run", "[determinism][timeline]")
{
    const std::string path = TempPath("truncated");
    REQUIRE(WriteTimeline(MakeSample(), path).empty());

    const std::vector<char> full = ReadAllBytes(path);
    REQUIRE(full.size() > 32);

    // Every truncation point, not one chosen sample: the interesting failures are at
    // the boundaries between the header, the name table and the tick bodies, and a
    // single hand-picked offset would miss whichever one is broken.
    for (std::size_t cut = 0; cut < full.size(); cut += 3)
    {
        WriteAllBytes(path, std::vector<char>(full.begin(), full.begin() + static_cast<std::ptrdiff_t>(cut)));

        StateTimeline     restored;
        const std::string err = ReadTimeline(restored, path);
        INFO("truncated to " << cut << " of " << full.size() << " bytes");
        REQUIRE_FALSE(err.empty());
        // EMPTY, not partial. A half-filled timeline is the dangerous outcome: it
        // would compare equal to another half-filled one.
        REQUIRE(restored.TickCount() == 0);
        REQUIRE(restored.EntryCount() == 0);
    }

    std::remove(path.c_str());
}

TEST_CASE("Trailing bytes and a bad magic are refused", "[determinism][timeline]")
{
    const std::string path = TempPath("trailing");
    REQUIRE(WriteTimeline(MakeSample(), path).empty());

    std::vector<char> buf = ReadAllBytes(path);
    buf.push_back('\0');
    WriteAllBytes(path, buf);

    StateTimeline restored;
    REQUIRE_FALSE(ReadTimeline(restored, path).empty());
    REQUIRE(restored.TickCount() == 0);

    buf[0] = 'X';
    WriteAllBytes(path, buf);
    REQUIRE_FALSE(ReadTimeline(restored, path).empty());

    std::remove(path.c_str());
}

TEST_CASE("A missing file reports rather than throws", "[determinism][timeline]")
{
    StateTimeline     restored;
    const std::string err = ReadTimeline(restored, TempPath("does-not-exist-at-all"));
    REQUIRE_FALSE(err.empty());
    REQUIRE(restored.TickCount() == 0);
}

TEST_CASE("A recorded divergence is still found after a round trip", "[determinism][timeline]")
{
    // The point of the file is to compare two RUNS. This is that comparison, with one
    // side having been through the disk: the divergence must be reported at the same
    // boundary, with the same labels, as if both were in memory.
    StateTimeline a = MakeSample();

    StateTimeline b = MakeSample();
    // Perturb one discrete field in tick 2. Discrete on purpose: no diagnostic may
    // ever soften it.
    {
        StateTimeline rebuilt;
        const NameId  unit = rebuilt.Intern("unit");
        const NameId  posX = rebuilt.Intern("posX");
        const NameId  target = rebuilt.Intern("targetId");
        const NameId  alive = rebuilt.Intern("alive");
        for (int tick = 0; tick < 4; tick++)
        {
            rebuilt.BeginTick();
            for (std::uint32_t k = 0; k < 3; k++)
            {
                rebuilt.F32(unit, k, posX, static_cast<float>(tick) + static_cast<float>(k) * 0.25f);
                const std::uint32_t id = static_cast<std::uint32_t>(tick * 10 + k);
                rebuilt.U32(unit, k, target, (tick == 2 && k == 1) ? id + 1 : id);
                rebuilt.Bool(unit, k, alive, (tick + k) % 2 == 0);
            }
            rebuilt.F32(unit, 99, posX, -0.0f);
            rebuilt.F32(unit, 98, posX, std::numeric_limits<float>::quiet_NaN());
            rebuilt.EndTick();
        }
        b = std::move(rebuilt);
    }

    const std::string pathA = TempPath("diverge-a");
    const std::string pathB = TempPath("diverge-b");
    REQUIRE(WriteTimeline(a, pathA).empty());
    REQUIRE(WriteTimeline(b, pathB).empty());

    StateTimeline ra;
    StateTimeline rb;
    REQUIRE(ReadTimeline(ra, pathA).empty());
    REQUIRE(ReadTimeline(rb, pathB).empty());

    const Divergence live = CompareTimelines(a, b);
    const Divergence viaDisk = CompareTimelines(ra, rb);

    REQUIRE(live.Diverged());
    REQUIRE(viaDisk.Diverged());
    REQUIRE(viaDisk.kind == live.kind);
    REQUIRE(viaDisk.tick == live.tick);
    REQUIRE(viaDisk.entryIndex == live.entryIndex);
    REQUIRE(viaDisk.field == live.field);
    REQUIRE(viaDisk.key == live.key);
    REQUIRE(viaDisk.lhsBits == live.lhsBits);
    REQUIRE(viaDisk.rhsBits == live.rhsBits);
    REQUIRE(viaDisk.tick == 2);
    REQUIRE(viaDisk.fieldKind == FieldKind::Discrete);

    std::remove(pathA.c_str());
    std::remove(pathB.c_str());
}
