#pragma once

#include <Poseidon/Foundation/Containers/Array.hpp>

// Quad tree with continuous indexing - the root lives at _index[0].

namespace Poseidon::Foundation
{
template <class Type, class Traits = FindArrayKeyTraits<Type>, class Allocator = Poseidon::Foundation::MemAllocD>
class QuadTreeCont
{
  protected:
    AutoArray<Type, Allocator> _values;
    AutoArray<int, Allocator> _index;
    AutoArray<int, Allocator> _free;

    int _maxLevel;
    int GetRootIndex() const { return _index[0]; }

  public:
    QuadTreeCont() { Clear(); }
    void Clear();

    void Set(int x, int y, const Type& value);

    template <class DetectRegionF, class ActionF>
    bool ForEachInRegion(int index, int size, const int x, const int y, DetectRegionF detect, ActionF& action)
    {
        size >>= 1;
        // check into which children we should descend
        if (detect(x, y, size))
        {
            int nindex = _index[index + 0];
            if (nindex >= 0)
            {
                if (ForEachInRegion(nindex, size, x, y, detect, action))
                {
                    return true;
                }
            }
            else
            {
                PoseidonAssert(nindex < 0 && -nindex <= _values.Size());
                if (nindex != -1)
                {
                    if (action(_values[-nindex - 1]))
                    {
                        return true;
                    }
                }
            }
        }
        if (detect(x, y + size, size))
        {
            int nindex = _index[index + 1];
            if (nindex >= 0)
            {
                if (ForEachInRegion(nindex, size, x, y + size, detect, action))
                {
                    return true;
                };
            }
            else
            {
                PoseidonAssert(nindex < 0 && -nindex <= _values.Size());
                if (nindex != -1)
                {
                    if (action(_values[-nindex - 1]))
                    {
                        return true;
                    }
                }
            }
        }
        if (detect(x + size, y, size))
        {
            int nindex = _index[index + 2];
            if (nindex >= 0)
            {
                if (ForEachInRegion(nindex, size, x + size, y, detect, action))
                {
                    return true;
                };
            }
            else
            {
                PoseidonAssert(nindex < 0 && -nindex <= _values.Size());
                if (nindex != -1)
                {
                    if (action(_values[-nindex - 1]))
                    {
                        return true;
                    }
                }
            }
        }
        if (detect(x + size, y + size, size))
        {
            int nindex = _index[index + 3];
            if (nindex >= 0)
            {
                if (ForEachInRegion(nindex, size, x + size, y + size, detect, action))
                {
                    return true;
                };
            }
            else
            {
                PoseidonAssert(nindex < 0 && -nindex <= _values.Size());
                if (nindex != -1)
                {
                    if (action(_values[-nindex - 1]))
                    {
                        return true;
                    }
                }
            }
        }
        return false;
    }

    // Runs action on every element selected by detect; stops and returns true
    // as soon as action returns true.
    template <class DetectRegionF, class ActionF>
    bool ForEachInRegion(DetectRegionF detect, ActionF& action)
    {
        // CRASH-20261023: an empty tree (or one holding a single value at the origin) has a leaf
        // for a root. The recursive walk reads the root's four children, i.e. _index[-1..2], and
        // descended into whatever lay before the array (briefing map of a world with no peaks).
        const int root = GetRootIndex();
        if (root < 0)
            return root != -1 && detect(0, 0, 1 << _maxLevel) && action(_values[-root - 1]);
        return ForEachInRegion(root, 1 << _maxLevel, 0, 0, detect, action);
    }

  protected:
    int AddNode();
    int IsEqual(const Type& a, const Type& b) { return Traits::IsEqual(Traits::GetKey(a), Traits::GetKey(b)); }
};

template <class Type, class Traits, class Allocator>
void QuadTreeCont<Type, Traits, Allocator>::Clear()
{
    _values.Clear();
    _index.Clear();
    _free.Clear();

    // _values[0] is always default value
    _values.Add();

    _maxLevel = 0;
    _index.Add(-1);
}

template <class Type, class Traits, class Allocator>
void QuadTreeCont<Type, Traits, Allocator>::Set(int x, int y, const Type& value)
{
    // The tree covers [0, 2^30) on both axes: a negative coordinate never fits, and growing the
    // root for one would shift `size` past the int's sign bit (CRASH-20261023).
    if (x < 0 || y < 0 || x >= (1 << 30) || y >= (1 << 30))
    {
        PoseidonAssert(false);
        return;
    }
    int size = 1 << _maxLevel;

    // check bounds
    {
        while ((x | y) & ~(size - 1))
        {
            // out of bounds - extend
            int n = AddNode();
            _index[n + 0] = GetRootIndex();
            _index[n + 1] = -1; // default value
            _index[n + 2] = -1; // default value
            _index[n + 3] = -1; // default value

            _index[0] = n;

            _maxLevel++;
            size <<= 1;
        }
    }

    // find current leaf
    int startX = 0, startY = 0;

    int iindex = 0;
    while (_index[iindex] >= 0)
    {
        size >>= 1;
        if (x < startX + size)
        {
            if (y < startY + size)
            {
                iindex = _index[iindex] + 0;
            }
            else
            {
                iindex = _index[iindex] + 1;
                startY += size;
            }
        }
        else
        {
            if (y < startY + size)
            {
                iindex = _index[iindex] + 2;
            }
            else
            {
                iindex = _index[iindex] + 3;
                startY += size;
            }
            startX += size;
        }
    }

    // check if change must be done
    int currentIndex = _index[iindex];
    int indexValue = -currentIndex - 1;
    PoseidonAssert(indexValue >= 0 && indexValue < _values.Size());
    // leaf found
    int newIndex = _values.Add(value);
    if (indexValue == 0)
    {
        // no value found
        _index[iindex] = -newIndex - 1;
        return;
    }

    const Type& leaf = _values[indexValue];
    int leafX = leaf.GetX();
    int leafY = leaf.GetY();

    if (leafX == x && leafY == y)
    {
        // value can be replaced
        _index[iindex] = -newIndex - 1;
        return;
    }

    // split current leaf node
    while (size > 1)
    {
        // add new node to hierarchy
        int n = AddNode();
        _index[iindex] = n;

        size >>= 1;

        int xIndex = x >= startX + size;
        int yIndex = y >= startY + size;

        int leafXIndex = leafX >= startX + size;
        int leafYIndex = leafY >= startY + size;

        _index[n + 0] = -1;
        _index[n + 1] = -1;
        _index[n + 2] = -1;
        _index[n + 3] = -1;

        if (xIndex != leafXIndex || yIndex != leafYIndex)
        {
            // we have found a distinct place for a new leaf

            _index[n + leafXIndex * 2 + leafYIndex] = currentIndex;
            _index[n + xIndex * 2 + yIndex] = -newIndex - 1;

            return;
        }

        iindex = n + xIndex * 2 + yIndex;
        startX += xIndex * size;
        startY += yIndex * size;
    }

    Fail("Not reached");
    _index[iindex] = -1;
}

template <class Type, class Traits, class Allocator>
int QuadTreeCont<Type, Traits, Allocator>::AddNode()
{
    int n = _free.Size();
    if (n > 0)
    {
        int i = _free[n - 1];
        _free.Delete(n - 1);
        return i;
    }

    int i = _index.Size();
    _index.Access(i + 3);
    return i;
}

} // namespace Poseidon::Foundation

using ::Poseidon::Foundation::QuadTreeCont;

