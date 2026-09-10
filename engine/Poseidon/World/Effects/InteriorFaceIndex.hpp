#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <utility>
#include <vector>

namespace Poseidon
{
// Boundary generation emits each cell/axis/side once per room. Keep indices
// sorted when returning neighbours to preserve legacy BFS and float-sum order.
class InteriorFaceIndex
{
    std::array<int, 3> _dim;
    std::vector<std::pair<uint64_t, int>> _entries;

    uint64_t Key(int axis, int side, const int* c) const
    {
        return ((uint64_t(c[0]) * _dim[1] + c[1]) * _dim[2] + c[2]) * 6 + axis * 2 + (side > 0);
    }

  public:
    template <class Faces>
    InteriorFaceIndex(const Faces& faces, const int (&dim)[3]) : _dim{dim[0], dim[1], dim[2]}
    {
        _entries.reserve(faces.size());
        for (int i = 0; i < static_cast<int>(faces.size()); ++i)
            _entries.emplace_back(Key(faces[i].axis, faces[i].side, faces[i].c), i);
        std::sort(_entries.begin(), _entries.end());
    }

    template <class Face>
    std::array<int, 4> Neighbours(const Face& face) const
    {
        std::array<int, 4> result{-1, -1, -1, -1};
        for (int n = 0; n < 4; ++n)
        {
            int c[3] = {face.c[0], face.c[1], face.c[2]};
            const int axis = (face.axis + 1 + n / 2) % 3;
            c[axis] += (n % 2 == 0 ? -1 : 1);
            if (c[axis] < 0 || c[axis] >= _dim[axis])
                continue;
            const auto key = Key(face.axis, face.side, c);
            const auto it = std::lower_bound(_entries.begin(), _entries.end(), std::pair<uint64_t, int>{key, -1});
            if (it != _entries.end() && it->first == key)
                result[n] = it->second;
        }
        std::sort(result.begin(), result.end());
        return result;
    }
};
} // namespace Poseidon
