#pragma once

namespace Poseidon
{
// The callback transfers ownership before returning true. Preserve source and
// destination order, and release the removed references only after the scan.
// Neither the callback nor an element destructor may mutate the source list.
template<class List, class Transfer>
void StableExtract(List& list, Transfer&& transfer)
{
    int write = 0;
    const int count = list.Size();
    for (int read = 0; read < count; ++read)
    {
        if (transfer(list[read])) continue;
        if (write != read) list.Set(write) = list[read];
        ++write;
    }
    list.Resize(write);
}
}
