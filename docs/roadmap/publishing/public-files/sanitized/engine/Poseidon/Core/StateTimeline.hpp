#pragma once

// Roadmap Phase 8.6 / Phase 13 -- "record/replay should report the FIRST divergent
// authoritative state boundary". See
// design notes
//
// SIM-806 and SIM-804 both hash a whole run into one 64-bit value and compare it.
// That answers "did these two runs diverge" and nothing else: when it fails, the
// only thing anybody knows is that somewhere in 240 ticks and 14,000 numbers, two
// of them stopped agreeing. This class is the missing half -- it keeps the run as
// a TIMELINE of labelled fields so a comparison can name the tick, the record and
// the field, and print both bit patterns.
//
// Three deliberate properties:
//
//  * **Bits, never values.** Every field is stored as its raw bit pattern.
//    `-0.0f == 0.0f` and `NaN != NaN`, so a value comparison is simultaneously too
//    loose and too tight to be the strict comparison the gate asks for.
//
//  * **Positional comparison, so ORDER is part of the state.** Entries are compared
//    in emission order and a positionally aligned pair that carries different
//    labels is reported as a divergence in its own right. The roadmap lists "event
//    order" among the authoritative state a replay must cover; recording an ordered
//    stream is how that gets covered rather than asserted.
//
//  * **No tolerance, anywhere.** `CompareTimelines` compares bit patterns for
//    equality and has no epsilon, no near-equal path and no configuration. The
//    `ulpDistance` on the result is a DIAGNOSTIC printed alongside a divergence
//    that has already been decided; nothing reads it back. Fields additionally
//    carry a `FieldKind`, so a future cross-platform diagnostic can be refused
//    outright on a discrete field instead of relying on whoever writes it to
//    remember that a hit, a death or a target selection is never "close enough".
//
// This header is std-only on purpose. It is not a physics facility; the first
// consumer happens to be physics because that is the only authoritative simulation
// job that exists today (SIM-804, SIM-806). Event order, target selection and
// replicated state are the same shape and should record into the same timeline.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Poseidon::Determinism
{

/// Whether a divergence in this field could ever be a rounding artefact.
///
/// `Discrete` is the load-bearing one. The gate's wording is that tolerance is a
/// cross-platform DIAGNOSTIC and "can never hide a divergent discrete outcome" --
/// a hit, a death, a selected target, an ownership change, an event ordinal. Those
/// are `Discrete` and no diagnostic may be applied to them.
enum class FieldKind : std::uint8_t
{
    Continuous,
    Discrete,
};

/// Interned label id. Comparing two timelines compares label TEXT, not ids, because
/// two timelines intern independently and their id spaces need not agree.
using NameId = std::uint32_t;

class StateTimeline;
std::string ReadTimeline(StateTimeline& out, const std::string& path);

class StateTimeline
{
public:
    /// Intern a label once and reuse the id. Recording 24 bodies x 20 fields x 240
    /// ticks through the string overloads is 115,000 hash lookups of short strings;
    /// through the id overloads it is none.
    NameId Intern(std::string_view name);

    [[nodiscard]] std::string_view Name(NameId id) const;

    /// Open a tick. Every field recorded until `EndTick` belongs to it.
    void BeginTick();
    void EndTick();
    [[nodiscard]] bool TickOpen() const { return _open; }

    void F32(NameId stream, std::uint32_t key, NameId field, float value);
    void F64(NameId stream, std::uint32_t key, NameId field, double value);
    /// Discrete: counts, ordinals, ids, enum tags.
    void U32(NameId stream, std::uint32_t key, NameId field, std::uint32_t value);
    void U64(NameId stream, std::uint32_t key, NameId field, std::uint64_t value);
    void Bool(NameId stream, std::uint32_t key, NameId field, bool value);

    // Convenience overloads that intern on the way through.
    void F32(std::string_view stream, std::uint32_t key, std::string_view field, float value);
    void U32(std::string_view stream, std::uint32_t key, std::string_view field, std::uint32_t value);
    void Bool(std::string_view stream, std::uint32_t key, std::string_view field, bool value);

    [[nodiscard]] int         TickCount() const { return static_cast<int>(_ticks.size()); }
    [[nodiscard]] std::size_t EntryCount() const { return _entries.size(); }
    /// Number of fields recorded in one tick.
    [[nodiscard]] int EntryCountInTick(int tick) const;
    /// FNV-1a over every entry of one tick, labels included. The cheap "did this
    /// tick change" summary; the comparison does not use it, it compares entries.
    [[nodiscard]] std::uint64_t TickHash(int tick) const;
    /// FNV-1a over every tick hash. Comparable with SIM-804/806's whole-run hash in
    /// spirit, not in value -- this one covers labels as well as numbers.
    [[nodiscard]] std::uint64_t RunHash() const;

    /// Distinct tick hashes. An anti-vacuity measurement: a timeline whose ticks all
    /// hash alike is one where nothing moved, and it would agree with itself for ever.
    [[nodiscard]] std::size_t DistinctTickHashes() const;

    void Clear();

    // ---- exposed for CompareTimelines and for tests that inspect a recording ----
    struct Entry
    {
        NameId        stream = 0;
        NameId        field = 0;
        std::uint32_t key = 0;
        std::uint64_t bits = 0;
        FieldKind     kind = FieldKind::Continuous;
    };

    [[nodiscard]] const Entry& EntryAt(int tick, int index) const;

private:
    // The reader replays raw entries -- stream, field, key, bits and kind straight from
    // the file -- which is precisely what `Push` does and what no public overload can do
    // (the typed setters would re-encode the value and a NaN or a -0.0 would not survive).
    friend std::string ReadTimeline(StateTimeline& out, const std::string& path);

    void Push(NameId stream, std::uint32_t key, NameId field, std::uint64_t bits, FieldKind kind);

    struct TickSpan
    {
        std::uint32_t first = 0;
        std::uint32_t count = 0;
        std::uint64_t hash = 0;
    };

    std::vector<std::string>                    _names;
    std::unordered_map<std::string, NameId>     _nameIds;
    std::vector<Entry>                          _entries;
    std::vector<TickSpan>                       _ticks;
    bool                                        _open = false;
};

enum class DivergenceKind : std::uint8_t
{
    /// The two timelines are identical, entry for entry, label for label.
    None,
    /// Same tick, same position, same label -- different bits.
    FieldValue,
    /// Same tick, same position -- different label or record key. This is an
    /// ORDERING divergence: the two runs emitted a different sequence of facts.
    FieldIdentity,
    /// Same tick, identical common prefix -- one run recorded more fields than the
    /// other. A record appeared or vanished.
    RecordShape,
    /// One run ran out of ticks first, with every shared tick identical.
    TickCount,
};

/// The answer to "where did these two runs first stop agreeing".
struct Divergence
{
    DivergenceKind kind = DivergenceKind::None;

    /// The FIRST tick at which the two runs differ. -1 when they do not.
    int tick = -1;
    /// Index of the differing entry within that tick, in emission order.
    int entryIndex = -1;

    std::string   stream;
    std::string   field;
    std::uint32_t key = 0;
    FieldKind     fieldKind = FieldKind::Continuous;

    std::uint64_t lhsBits = 0;
    std::uint64_t rhsBits = 0;

    /// Set for `FieldIdentity`: what the right-hand run recorded in that slot.
    std::string   rhsStream;
    std::string   rhsField;
    std::uint32_t rhsKey = 0;

    /// Set for `RecordShape` and `TickCount`.
    int lhsCount = 0;
    int rhsCount = 0;

    /// DIAGNOSTIC ONLY, and only for a `Continuous` 32-bit field: how many
    /// representable floats separate the two values. -1 when not applicable.
    ///
    /// Nothing in `CompareTimelines` reads this. It exists so that a cross-platform
    /// report can say "one ulp apart" instead of "two hex strings", which is the
    /// legitimate use the gate allows. It is NEVER computed for a `Discrete` field
    /// and must never become a threshold: a discrete outcome that differs has
    /// diverged, however small the number that carried it.
    long long ulpDistance = -1;

    [[nodiscard]] bool Diverged() const { return kind != DivergenceKind::None; }

    /// One line naming the boundary, for a test failure message or a log.
    [[nodiscard]] std::string Describe() const;
};

/// Compares two recordings entry by entry, in order, by bit pattern, and returns
/// the FIRST place they differ.
///
/// Strict. There is no tolerance parameter and there will not be one.
[[nodiscard]] Divergence CompareTimelines(const StateTimeline& lhs, const StateTimeline& rhs);

// ---------------------------------------------------------------------------------
// Recording a timeline to a file, so the comparison can be made between two RUNS.
//
// Until this existed the class could only compare two timelines held in one process,
// which restricted it to unit tests -- and the questions it was built for are about
// whole missions. "Does rationing this AI loop change what the AI does" cannot be
// answered in a fixture; it needs two runs of a real scene and a byte-exact diff.
//
// The format is deliberately dull and self-describing: a magic, a version, the name
// table, then the ticks. It is NOT a compatibility surface -- a timeline is written
// and read by the same build, and `ReadTimeline` refuses a version it does not know
// rather than guessing. Bits are stored little-endian as they sit in memory; the
// class stores bit patterns precisely so nothing has to interpret them here.
//
// Errors are returned, never thrown and never logged: the caller knows whether a
// missing file is a failure (a gate) or expected (a first recording).
// ---------------------------------------------------------------------------------

/// Empty on success, otherwise a human-readable reason.
[[nodiscard]] std::string WriteTimeline(const StateTimeline& timeline, const std::string& path);

/// Empty on success. `out` is cleared first, and is left EMPTY on any failure --
/// never partially filled, so a truncated file cannot read as a short run that
/// happens to agree.
[[nodiscard]] std::string ReadTimeline(StateTimeline& out, const std::string& path);

} // namespace Poseidon::Determinism
