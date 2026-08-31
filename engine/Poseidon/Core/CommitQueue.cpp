#include <Poseidon/Core/CommitQueue.hpp>

#include <algorithm>

namespace Poseidon::Determinism
{

bool CommitKeyBefore(const CommitKey& a, const CommitKey& b)
{
    // Field by field, `<` only. See CommitQueue.hpp on SIM-803: the comparator that
    // shipped in AICenterImpl.cpp subtracted two pointers and narrowed the result to
    // `int`, producing a cycle for operands more than 2 GiB apart. `job` and `seq` are
    // 32-bit ordinals and would truncate the same way under `(int)(a.job - b.job)`.
    if (a.stage != b.stage)
    {
        return a.stage < b.stage;
    }
    if (a.site != b.site)
    {
        return a.site < b.site;
    }
    if (a.job != b.job)
    {
        return a.job < b.job;
    }
    return a.seq < b.seq;
}

bool CommitKeyEqual(const CommitKey& a, const CommitKey& b)
{
    return a.stage == b.stage && a.site == b.site && a.job == b.job && a.seq == b.seq;
}

void CommitQueue::Enqueue(const CommitKey& key, std::uint32_t target, std::uint64_t bits)
{
    CommitRecord record;
    record.key = key;
    record.target = target;
    record.bits = bits;
    _records.push_back(record);
}

void CommitQueue::Clear()
{
    _records.clear();
}

bool CommitQueue::HasDuplicateKeys() const
{
    // Sorts a copy: this is a diagnostic and must not perturb the pending order that
    // `Pending()` promises.
    std::vector<CommitKey> keys;
    keys.reserve(_records.size());
    for (const CommitRecord& record : _records)
    {
        keys.push_back(record.key);
    }
    std::sort(keys.begin(), keys.end(), CommitKeyBefore);
    for (std::size_t i = 1; i < keys.size(); i++)
    {
        if (CommitKeyEqual(keys[i - 1], keys[i]))
        {
            return true;
        }
    }
    return false;
}

std::size_t CommitQueue::Commit(void* context, CommitApply apply)
{
    // `stable_sort` even though the key is meant to be unique. If a caller does hand out a
    // duplicate key, an unstable sort would make the outcome depend on the sort's internal
    // pivot choices -- a defect that reproduces only at certain sizes, which is the worst
    // kind. Stable degrades to enqueue order instead, which is at least explicable.
    // `HasDuplicateKeys` is how a caller finds out it happened.
    std::stable_sort(_records.begin(), _records.end(),
                     [](const CommitRecord& x, const CommitRecord& y) { return CommitKeyBefore(x.key, y.key); });

    if (apply)
    {
        for (const CommitRecord& record : _records)
        {
            apply(context, record);
        }
    }
    return _records.size();
}

} // namespace Poseidon::Determinism
