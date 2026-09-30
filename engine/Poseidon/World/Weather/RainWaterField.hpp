#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace Poseidon
{
// Simulation-owned surface runoff. Equal-area cells store metres of water over
// the actual terrain bed, not a renderer noise mask. The conservative donor
// limiter moves water down the FREE SURFACE (bed + depth), so hollows retain it
// until their spill height is reached. Queries never advance the simulation.
class RainWaterField
{
public:
    static constexpr float StepSeconds = 0.25f;
    static constexpr size_t MaxCells = 1024 * 1024;
    struct Sample { float depth = 0, height = 0, flowX = 0, flowZ = 0; bool valid = false; };
    struct Budget { double rain = 0, infiltration = 0, evaporation = 0, outlet = 0; };

    bool Configure(int width, int height, float spacing, std::vector<float> bed,
                   float originX = 0, float originZ = 0, float seaLevel = 0)
    {
        if (width < 2 || height < 2 || static_cast<size_t>(width) * height > MaxCells ||
            bed.size() != static_cast<size_t>(width) * height ||
            !std::isfinite(spacing) || spacing <= 0 || !std::isfinite(originX) ||
            !std::isfinite(originZ) || !std::isfinite(seaLevel) ||
            std::any_of(bed.begin(), bed.end(), [](float y) { return !std::isfinite(y); })) return false;
        Reset();
        _width = width; _height = height; _spacing = spacing;
        _originX = originX; _originZ = originZ; _seaLevel = seaLevel;
        _bed = std::move(bed);
        _depth.resize(_bed.size()); _out.resize(_bed.size());
        _delta.resize(_bed.size()); _flowX.resize(_bed.size()); _flowZ.resize(_bed.size());
        return true;
    }
    void Reset() { *this = RainWaterField{}; }
    // Terrain editing changes bed elevation while preserving water volume.
    bool SetBed(int x, int z, float height)
    {
        if (x < 0 || z < 0 || x >= _width || z >= _height || !std::isfinite(height)) return false;
        _bed[Index(x,z)] = height; ++_revision; return true;
    }
    void Advance(float seconds, float rain, float sunlight = 0, float wind = 0)
    {
        if (_bed.empty() || !std::isfinite(seconds) || seconds <= 0 || !std::isfinite(rain)) return;
        rain = std::clamp(rain, 0.0f, 1.0f);
        sunlight = std::isfinite(sunlight) ? std::clamp(sunlight, 0.0f, 1.0f) : 0;
        wind = std::isfinite(wind) ? std::clamp(wind, 0.0f, 1.0f) : 0;
        _pending += seconds;
        // Bounded per game tick; retain all pending time for subsequent ticks.
        // Fixed steps make pause and ordinary frame-rate partitions equivalent.
        int steps = 0;
        while (_pending + 1e-7 >= StepSeconds && steps++ < 16) {
            Step(rain, sunlight, wind); _pending -= StepSeconds;
        }
    }
    Sample At(float x, float z) const
    {
        if (_bed.empty() || !std::isfinite(x) || !std::isfinite(z)) return {};
        const float gx = (x - _originX) / _spacing, gz = (z - _originZ) / _spacing;
        if (gx < 0 || gz < 0 || gx > _width - 1 || gz > _height - 1) return {};
        const int ix = std::min(static_cast<int>(gx), _width - 2);
        const int iz = std::min(static_cast<int>(gz), _height - 2);
        const float fx = gx - ix, fz = gz - iz;
        const auto interpolate = [&](const std::vector<float>& values) {
            const float a = values[Index(ix,iz)] * (1-fx) + values[Index(ix+1,iz)] * fx;
            const float b = values[Index(ix,iz+1)] * (1-fx) + values[Index(ix+1,iz+1)] * fx;
            return a * (1-fz) + b * fz;
        };
        const float depth = interpolate(_depth);
        return {depth, interpolate(_bed) + depth, interpolate(_flowX), interpolate(_flowZ), true};
    }
    double Volume() const {
        double sum = 0; for (float d : _depth) sum += d;
        return sum * _spacing * _spacing;
    }
    const Budget& WaterBudget() const { return _budget; }
    uint64_t Revision() const { return _revision; }
    int Width() const { return _width; }
    int Height() const { return _height; }
    float Spacing() const { return _spacing; }
    float OriginX() const { return _originX; }
    float OriginZ() const { return _originZ; }
    void SetSeaLevel(float level) { if (std::isfinite(level)) _seaLevel = level; }
    // Immutable simulation cell data, copied by the main-thread presentation
    // producer. Camera movement does not seed, erase or recompute any water.
    std::vector<float> Snapshot() const {
        std::vector<float> cells(_bed.size() * 4);
        for (size_t i=0; i<_bed.size(); ++i) {
            cells[4*i] = _bed[i] + _depth[i]; cells[4*i+1] = _depth[i];
            cells[4*i+2] = _flowX[i]; cells[4*i+3] = _flowZ[i];
        }
        return cells;
    }
    double PendingSeconds() const { return _pending; }
    // Test/fixture seeding is explicit and finite; ordinary missions start dry.
    bool AddWater(int x, int z, float metres) {
        if (x < 0 || z < 0 || x >= _width || z >= _height ||
            !std::isfinite(metres) || metres < 0) return false;
        _depth[Index(x,z)] += metres; ++_revision; return true;
    }
private:
    size_t Index(int x, int z) const { return static_cast<size_t>(z) * _width + x; }
    float Transfer(size_t a, size_t b) const {
        const float head = (_bed[a] + _depth[a]) - (_bed[b] + _depth[b]);
        const size_t donor = head > 0 ? a : b;
        if (head == 0 || _depth[donor] <= 0) return 0;
        // Bounded shallow runoff speed; the head-based cap cannot overshoot
        // level equilibrium even when one cell has a steep bare bed.
        const float speed = std::min(5.0f, std::sqrt(9.81f * std::abs(head)));
        const float amount = std::min(std::abs(head) * 0.25f,
            _depth[donor] * speed * StepSeconds / _spacing);
        return std::copysign(amount, head);
    }
    template<class F> void Edges(F visit) const {
        for (int z=0; z<_height; ++z) for (int x=0; x<_width; ++x) {
            const size_t a = Index(x,z);
            if (x+1<_width) visit(a,Index(x+1,z),true);
            if (z+1<_height) visit(a,Index(x,z+1),false);
        }
    }
    void Step(float rain, float sunlight, float wind) {
        const double area = static_cast<double>(_spacing) * _spacing;
        const float input = rain * 0.00012f * StepSeconds;
        const float evaporation = (0.000002f + sunlight * 0.000006f + wind * 0.000001f) * StepSeconds;
        const float infiltration = 0.000004f * StepSeconds;
        for (size_t i=0; i<_depth.size(); ++i) {
            // Sea is a real outlet, not a source of rainwater above dry terrain.
            if (_bed[i] <= _seaLevel) {
                _budget.outlet += _depth[i] * area; _depth[i] = 0; continue;
            }
            _depth[i] += input; _budget.rain += input * area;
            const float absorbed = std::min(_depth[i], infiltration);
            _depth[i] -= absorbed; _budget.infiltration += absorbed * area;
            const float evaporated = std::min(_depth[i], evaporation);
            _depth[i] -= evaporated; _budget.evaporation += evaporated * area;
        }
        std::fill(_out.begin(),_out.end(),0); std::fill(_delta.begin(),_delta.end(),0);
        std::fill(_flowX.begin(),_flowX.end(),0); std::fill(_flowZ.begin(),_flowZ.end(),0);
        Edges([&](size_t a,size_t b,bool) {
            const float f = Transfer(a,b); _out[f>0?a:b] += std::abs(f);
        });
        Edges([&](size_t a,size_t b,bool alongX) {
            float f = Transfer(a,b); const size_t donor = f>0?a:b;
            if (_out[donor]>0) f *= std::min(1.0f,_depth[donor]/_out[donor]);
            _delta[a] -= f; _delta[b] += f;
            auto& flow = alongX ? _flowX : _flowZ;
            // Signed volume flux converted to a surface-flow speed where wet.
            const float velocity = f * _spacing / StepSeconds / std::max(_depth[donor],0.0001f);
            flow[a] += velocity * 0.5f; flow[b] += velocity * 0.5f;
        });
        for (size_t i=0; i<_depth.size(); ++i) {
            _depth[i] = std::max(0.0f,_depth[i]+_delta[i]);
            if (_bed[i] <= _seaLevel) {
                _budget.outlet += _depth[i] * area; _depth[i]=0;
            }
        }
        ++_revision;
    }
    int _width=0,_height=0;
    float _spacing=1,_originX=0,_originZ=0,_seaLevel=0;
    double _pending=0;
    uint64_t _revision=0;
    Budget _budget;
    std::vector<float> _bed,_depth,_out,_delta,_flowX,_flowZ;
};
inline RainWaterField& GRainWater() { static RainWaterField field; return field; }
}
