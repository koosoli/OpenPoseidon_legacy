#include <Poseidon/Core/StateTimeline.hpp>

#include <algorithm>
#include <cstring>

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

} // namespace Poseidon::Determinism
