#include <Poseidon/Core/StateTimeline.hpp>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace Poseidon::Determinism
{

namespace
{

constexpr std::uint64_t kFnvOffset = 1469598103934665603ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

void MixByte(std::uint64_t& h, std::uint8_t b)
{
    h ^= b;
    h *= kFnvPrime;
}

void MixRaw(std::uint64_t& h, const void* p, std::size_t n)
{
    const auto* bytes = static_cast<const std::uint8_t*>(p);
    for (std::size_t i = 0; i < n; ++i)
    {
        MixByte(h, bytes[i]);
    }
}

void MixText(std::uint64_t& h, std::string_view s)
{
    MixRaw(h, s.data(), s.size());
    MixByte(h, 0u);
}

/// Maps an IEEE-754 binary32 bit pattern onto a monotone signed ordering, so the
/// difference of two mapped values counts representable floats between them.
/// Diagnostic use only -- see Divergence::ulpDistance.
std::int64_t OrderedFromBits32(std::uint32_t bits)
{
    // Negative zero maps below positive zero, which is correct for an ulp count and
    // is precisely why the COMPARISON does not go through here: -0.0f and 0.0f are a
    // divergence, and any distance metric that calls them zero apart would hide it.
    if ((bits & 0x80000000u) != 0u)
    {
        return -static_cast<std::int64_t>(bits & 0x7FFFFFFFu);
    }
    return static_cast<std::int64_t>(bits);
}

std::string HexOf(std::uint64_t bits, bool wide)
{
    static const char* kDigits = "0123456789ABCDEF";
    const int          nibbles = wide ? 16 : 8;
    std::string        out = "0x";
    for (int i = nibbles - 1; i >= 0; --i)
    {
        out.push_back(kDigits[(bits >> (i * 4)) & 0xFull]);
    }
    return out;
}

std::string FloatOf(std::uint64_t bits)
{
    float v = 0.0f;
    const std::uint32_t narrow = static_cast<std::uint32_t>(bits);
    std::memcpy(&v, &narrow, sizeof(v));
    return std::to_string(v);
}

} // namespace

// ---------------------------------------------------------------------------

NameId StateTimeline::Intern(std::string_view name)
{
    std::string key(name);
    const auto  it = _nameIds.find(key);
    if (it != _nameIds.end())
    {
        return it->second;
    }
    const auto id = static_cast<NameId>(_names.size());
    _names.push_back(key);
    _nameIds.emplace(std::move(key), id);
    return id;
}

std::string_view StateTimeline::Name(NameId id) const
{
    if (id >= _names.size())
    {
        return {};
    }
    return _names[id];
}

void StateTimeline::BeginTick()
{
    TickSpan span;
    span.first = static_cast<std::uint32_t>(_entries.size());
    span.count = 0;
    span.hash = kFnvOffset;
    _ticks.push_back(span);
    _open = true;
}

void StateTimeline::EndTick()
{
    _open = false;
}

void StateTimeline::Push(NameId stream, std::uint32_t key, NameId field, std::uint64_t bits, FieldKind kind)
{
    if (_ticks.empty())
    {
        // Recording outside a tick would produce entries no comparison could place.
        // Opening one implicitly is friendlier than dropping them silently.
        BeginTick();
    }
    Entry e;
    e.stream = stream;
    e.field = field;
    e.key = key;
    e.bits = bits;
    e.kind = kind;
    _entries.push_back(e);

    TickSpan& span = _ticks.back();
    span.count += 1;
    // Labels are hashed as TEXT, so two timelines that interned in a different
    // order still agree, and a renamed or reordered field still shows up.
    MixText(span.hash, Name(stream));
    MixText(span.hash, Name(field));
    MixRaw(span.hash, &key, sizeof(key));
    MixRaw(span.hash, &bits, sizeof(bits));
    MixByte(span.hash, static_cast<std::uint8_t>(kind));
}

void StateTimeline::F32(NameId stream, std::uint32_t key, NameId field, float value)
{
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    Push(stream, key, field, bits, FieldKind::Continuous);
}

void StateTimeline::F64(NameId stream, std::uint32_t key, NameId field, double value)
{
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    Push(stream, key, field, bits, FieldKind::Continuous);
}

void StateTimeline::U32(NameId stream, std::uint32_t key, NameId field, std::uint32_t value)
{
    Push(stream, key, field, value, FieldKind::Discrete);
}

void StateTimeline::U64(NameId stream, std::uint32_t key, NameId field, std::uint64_t value)
{
    Push(stream, key, field, value, FieldKind::Discrete);
}

void StateTimeline::Bool(NameId stream, std::uint32_t key, NameId field, bool value)
{
    Push(stream, key, field, value ? 1ull : 0ull, FieldKind::Discrete);
}

void StateTimeline::F32(std::string_view stream, std::uint32_t key, std::string_view field, float value)
{
    F32(Intern(stream), key, Intern(field), value);
}

void StateTimeline::U32(std::string_view stream, std::uint32_t key, std::string_view field, std::uint32_t value)
{
    U32(Intern(stream), key, Intern(field), value);
}

void StateTimeline::Bool(std::string_view stream, std::uint32_t key, std::string_view field, bool value)
{
    Bool(Intern(stream), key, Intern(field), value);
}

int StateTimeline::EntryCountInTick(int tick) const
{
    if (tick < 0 || tick >= static_cast<int>(_ticks.size()))
    {
        return 0;
    }
    return static_cast<int>(_ticks[static_cast<std::size_t>(tick)].count);
}

std::uint64_t StateTimeline::TickHash(int tick) const
{
    if (tick < 0 || tick >= static_cast<int>(_ticks.size()))
    {
        return 0;
    }
    return _ticks[static_cast<std::size_t>(tick)].hash;
}

std::uint64_t StateTimeline::RunHash() const
{
    std::uint64_t h = kFnvOffset;
    for (const TickSpan& span : _ticks)
    {
        MixRaw(h, &span.hash, sizeof(span.hash));
    }
    return h;
}

std::size_t StateTimeline::DistinctTickHashes() const
{
    std::vector<std::uint64_t> seen;
    seen.reserve(_ticks.size());
    for (const TickSpan& span : _ticks)
    {
        seen.push_back(span.hash);
    }
    std::sort(seen.begin(), seen.end());
    seen.erase(std::unique(seen.begin(), seen.end()), seen.end());
    return seen.size();
}

const StateTimeline::Entry& StateTimeline::EntryAt(int tick, int index) const
{
    const TickSpan& span = _ticks[static_cast<std::size_t>(tick)];
    return _entries[span.first + static_cast<std::uint32_t>(index)];
}

void StateTimeline::Clear()
{
    _names.clear();
    _nameIds.clear();
    _entries.clear();
    _ticks.clear();
    _open = false;
}

// ---------------------------------------------------------------------------

std::string Divergence::Describe() const
{
    switch (kind)
    {
        case DivergenceKind::None:
            return "no divergence";

        case DivergenceKind::FieldValue:
        {
            std::string out = "first divergence at tick " + std::to_string(tick) + ", entry " +
                              std::to_string(entryIndex) + ": " + stream + "[" + std::to_string(key) + "]." + field +
                              (fieldKind == FieldKind::Discrete ? " (discrete) " : " (continuous) ") +
                              HexOf(lhsBits, false) + " vs " + HexOf(rhsBits, false);
            if (fieldKind == FieldKind::Continuous)
            {
                out += " (" + FloatOf(lhsBits) + " vs " + FloatOf(rhsBits) + ")";
                if (ulpDistance >= 0)
                {
                    out += ", " + std::to_string(ulpDistance) + " ulp";
                }
            }
            else
            {
                out += " -- a discrete outcome differs; no tolerance applies";
            }
            return out;
        }

        case DivergenceKind::FieldIdentity:
            return "first divergence at tick " + std::to_string(tick) + ", entry " + std::to_string(entryIndex) +
                   ": the two runs recorded different facts in the same slot -- " + stream + "[" +
                   std::to_string(key) + "]." + field + " vs " + rhsStream + "[" + std::to_string(rhsKey) + "]." +
                   rhsField + " (an ORDERING divergence, not a value one)";

        case DivergenceKind::RecordShape:
            return "first divergence at tick " + std::to_string(tick) +
                   ": the shared prefix agrees but the runs recorded different numbers of fields -- " +
                   std::to_string(lhsCount) + " vs " + std::to_string(rhsCount) +
                   " (a record appeared or vanished)";

        case DivergenceKind::TickCount:
            return "no divergence within the shared " + std::to_string(tick) + " ticks, but the runs are of "
                   "different length -- " +
                   std::to_string(lhsCount) + " vs " + std::to_string(rhsCount) + " ticks";
    }
    return "unknown divergence";
}

Divergence CompareTimelines(const StateTimeline& lhs, const StateTimeline& rhs)
{
    Divergence d;

    const int shared = lhs.TickCount() < rhs.TickCount() ? lhs.TickCount() : rhs.TickCount();

    for (int tick = 0; tick < shared; ++tick)
    {
        const int lc = lhs.EntryCountInTick(tick);
        const int rc = rhs.EntryCountInTick(tick);
        const int prefix = lc < rc ? lc : rc;

        for (int i = 0; i < prefix; ++i)
        {
            const StateTimeline::Entry& a = lhs.EntryAt(tick, i);
            const StateTimeline::Entry& b = rhs.EntryAt(tick, i);

            const std::string_view aStream = lhs.Name(a.stream);
            const std::string_view aField = lhs.Name(a.field);
            const std::string_view bStream = rhs.Name(b.stream);
            const std::string_view bField = rhs.Name(b.field);

            // Identity first. Two runs that put different facts in the same slot have
            // diverged in ORDER, and reporting that as a value mismatch between
            // unrelated numbers would send the reader after the wrong thing.
            if (aStream != bStream || aField != bField || a.key != b.key)
            {
                d.kind = DivergenceKind::FieldIdentity;
                d.tick = tick;
                d.entryIndex = i;
                d.stream = std::string(aStream);
                d.field = std::string(aField);
                d.key = a.key;
                d.fieldKind = a.kind;
                d.lhsBits = a.bits;
                d.rhsBits = b.bits;
                d.rhsStream = std::string(bStream);
                d.rhsField = std::string(bField);
                d.rhsKey = b.key;
                return d;
            }

            if (a.bits != b.bits)
            {
                d.kind = DivergenceKind::FieldValue;
                d.tick = tick;
                d.entryIndex = i;
                d.stream = std::string(aStream);
                d.field = std::string(aField);
                d.key = a.key;
                d.fieldKind = a.kind;
                d.lhsBits = a.bits;
                d.rhsBits = b.bits;
                // Diagnostic only, and only where it means anything. A discrete field
                // gets no distance at all -- there is no such thing as nearly the same
                // target, and offering the number invites somebody to threshold it.
                if (a.kind == FieldKind::Continuous && a.bits <= 0xFFFFFFFFull && b.bits <= 0xFFFFFFFFull)
                {
                    const std::int64_t oa = OrderedFromBits32(static_cast<std::uint32_t>(a.bits));
                    const std::int64_t ob = OrderedFromBits32(static_cast<std::uint32_t>(b.bits));
                    d.ulpDistance = oa > ob ? oa - ob : ob - oa;
                }
                return d;
            }
        }

        if (lc != rc)
        {
            d.kind = DivergenceKind::RecordShape;
            d.tick = tick;
            d.entryIndex = prefix;
            d.lhsCount = lc;
            d.rhsCount = rc;
            return d;
        }
    }

    if (lhs.TickCount() != rhs.TickCount())
    {
        d.kind = DivergenceKind::TickCount;
        d.tick = shared;
        d.lhsCount = lhs.TickCount();
        d.rhsCount = rhs.TickCount();
        return d;
    }

    return d;
}


// ---------------------------------------------------------------------------------
// File format. See the header for why it exists and why it is not a compatibility
// surface.
// ---------------------------------------------------------------------------------

namespace
{

constexpr std::uint32_t kMagic = 0x4c545350u; // 'PSTL'
constexpr std::uint32_t kVersion = 1u;

template <typename T> void PutRaw(std::vector<char>& buf, T v)
{
    static_assert(std::is_trivially_copyable_v<T>);
    const char* p = reinterpret_cast<const char*>(&v);
    buf.insert(buf.end(), p, p + sizeof(T));
}

void PutStr(std::vector<char>& buf, std::string_view s)
{
    PutRaw<std::uint32_t>(buf, static_cast<std::uint32_t>(s.size()));
    buf.insert(buf.end(), s.begin(), s.end());
}

/// Bounds-checked read. Every getter goes through this, so a truncated or corrupt
/// file fails at the first short read instead of reading past the buffer -- the
/// alternative is a file that parses into a plausible short timeline, which would
/// then COMPARE EQUAL to another short one and pass a gate it should have failed.
struct Reader
{
    const char* p = nullptr;
    const char* end = nullptr;
    bool        ok = true;

    template <typename T> T Get()
    {
        T v{};
        if (!ok || static_cast<std::size_t>(end - p) < sizeof(T))
        {
            ok = false;
            return v;
        }
        std::memcpy(&v, p, sizeof(T));
        p += sizeof(T);
        return v;
    }

    std::string GetStr()
    {
        const std::uint32_t n = Get<std::uint32_t>();
        if (!ok || static_cast<std::size_t>(end - p) < n)
        {
            ok = false;
            return {};
        }
        std::string s(p, p + n);
        p += n;
        return s;
    }
};

} // namespace

std::string WriteTimeline(const StateTimeline& timeline, const std::string& path)
{
    std::vector<char> buf;
    PutRaw(buf, kMagic);
    PutRaw(buf, kVersion);

    // The name table is written from the entries rather than from the class's own
    // table, because a label interned but never used carries no state and its
    // presence would make two otherwise identical runs differ.
    std::vector<std::string>                     names;
    std::unordered_map<std::string, std::uint32_t> index;
    const auto nameSlot = [&](std::string_view n) {
        std::string key(n);
        auto        it = index.find(key);
        if (it != index.end())
        {
            return it->second;
        }
        const auto slot = static_cast<std::uint32_t>(names.size());
        names.push_back(key);
        index.emplace(std::move(key), slot);
        return slot;
    };

    std::vector<char> body;
    PutRaw<std::uint32_t>(body, static_cast<std::uint32_t>(timeline.TickCount()));
    for (int t = 0; t < timeline.TickCount(); t++)
    {
        const int n = timeline.EntryCountInTick(t);
        PutRaw<std::uint32_t>(body, static_cast<std::uint32_t>(n));
        for (int i = 0; i < n; i++)
        {
            const StateTimeline::Entry& e = timeline.EntryAt(t, i);
            PutRaw<std::uint32_t>(body, nameSlot(timeline.Name(e.stream)));
            PutRaw<std::uint32_t>(body, nameSlot(timeline.Name(e.field)));
            PutRaw<std::uint32_t>(body, e.key);
            PutRaw<std::uint64_t>(body, e.bits);
            PutRaw<std::uint8_t>(body, static_cast<std::uint8_t>(e.kind));
        }
    }

    PutRaw<std::uint32_t>(buf, static_cast<std::uint32_t>(names.size()));
    for (const std::string& n : names)
    {
        PutStr(buf, n);
    }
    buf.insert(buf.end(), body.begin(), body.end());

    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f)
    {
        return "cannot open for writing: " + path;
    }
    f.write(buf.data(), static_cast<std::streamsize>(buf.size()));
    if (!f)
    {
        return "write failed: " + path;
    }
    return {};
}

std::string ReadTimeline(StateTimeline& out, const std::string& path)
{
    out.Clear();

    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f)
    {
        return "cannot open for reading: " + path;
    }
    const auto size = f.tellg();
    if (size < 0)
    {
        return "cannot size: " + path;
    }
    std::vector<char> buf(static_cast<std::size_t>(size));
    f.seekg(0);
    if (size > 0 && !f.read(buf.data(), size))
    {
        return "read failed: " + path;
    }

    Reader r{buf.data(), buf.data() + buf.size(), true};
    if (r.Get<std::uint32_t>() != kMagic || !r.ok)
    {
        return "not a state timeline: " + path;
    }
    const std::uint32_t version = r.Get<std::uint32_t>();
    if (!r.ok || version != kVersion)
    {
        // Refused, not guessed. A timeline is written and read by the same build.
        return "unsupported timeline version " + std::to_string(version) + " in " + path;
    }

    const std::uint32_t nameCount = r.Get<std::uint32_t>();
    if (!r.ok)
    {
        return "truncated name table: " + path;
    }
    std::vector<NameId> slots;
    slots.reserve(nameCount);
    for (std::uint32_t i = 0; i < nameCount; i++)
    {
        const std::string n = r.GetStr();
        if (!r.ok)
        {
            out.Clear();
            return "truncated name table: " + path;
        }
        slots.push_back(out.Intern(n));
    }

    const std::uint32_t tickCount = r.Get<std::uint32_t>();
    if (!r.ok)
    {
        out.Clear();
        return "truncated tick count: " + path;
    }
    for (std::uint32_t t = 0; t < tickCount; t++)
    {
        const std::uint32_t n = r.Get<std::uint32_t>();
        if (!r.ok)
        {
            out.Clear();
            return "truncated tick header: " + path;
        }
        out.BeginTick();
        for (std::uint32_t i = 0; i < n; i++)
        {
            const std::uint32_t stream = r.Get<std::uint32_t>();
            const std::uint32_t field = r.Get<std::uint32_t>();
            const std::uint32_t key = r.Get<std::uint32_t>();
            const std::uint64_t bits = r.Get<std::uint64_t>();
            const std::uint8_t  kind = r.Get<std::uint8_t>();
            if (!r.ok || stream >= slots.size() || field >= slots.size() || kind > 1)
            {
                out.Clear();
                return "corrupt entry: " + path;
            }
            out.Push(slots[stream], key, slots[field], bits, static_cast<FieldKind>(kind));
        }
        out.EndTick();
    }

    // Trailing bytes mean the writer and the reader disagree about the format, which
    // is exactly the situation where a silently-accepted file does the most damage.
    if (r.p != r.end)
    {
        out.Clear();
        return "trailing bytes: " + path;
    }
    return {};
}

} // namespace Poseidon::Determinism
