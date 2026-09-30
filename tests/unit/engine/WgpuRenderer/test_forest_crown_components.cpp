#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <vector>

// Minimal transport stubs for the private function extracted by its CPU runner.
using VertexIndex = int32_t;
template <class T> uint32_t U32(T v) { return static_cast<uint32_t>(v); }
struct Position { float x, y, z; float X() const { return x; } float Y() const { return y; } float Z() const { return z; } };
struct SVertex { Position pos; };
struct WgrVec4 { float x, y, z, w; };
#include "forest_crown_components_under_test.inc"

#define CHECK(expr) do { if (!(expr)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); return 1; } } while (false)

int main()
{
    // Every unused vertex has a distinct position, so neither welding nor low-index
    // filler can hide triangle-index wrapping. Both triangles live beyond 65535.
    std::vector<SVertex> vertices(65550);
    for (uint32_t i = 0; i < vertices.size(); ++i) vertices[i].pos = {float(i) * 2, 1, 0};
    std::vector<WgrVec4> centres;
    std::vector<uint32_t> components;
    CHECK(BuildForestCrownComponents(vertices, {65536,65537,65538,65539,65540,65541}, centres, components) == vertices.size()-4);
    CHECK(components[65536] == components[65537] && components[65536] == components[65538]);
    CHECK(components[65539] == components[65540] && components[65539] == components[65541]);
    CHECK(components[65536] != components[65539]);
    CHECK(components[0] != components[1] && components[1] != components[2]);
    CHECK(components[0] != components[65536]);
    CHECK(centres[components[65536]].x == 131074 && centres[components[65539]].x == 131080);
    CHECK(centres[components[65536]].w == 0);

    // Invalid signed/out-of-range corners must reject the whole triangle, never
    // wrap onto otherwise valid filler vertices or join them to a valid face.
    centres.clear();
    CHECK(BuildForestCrownComponents(vertices, {-1,0,1,65550,2,3,131072,4,5}, centres, components) == vertices.size());
    CHECK(components[0] != components[1] && components[2] != components[3] && components[4] != components[5]);

    // Ordinary seam welding and centroid semantics are unchanged by the fix.
    vertices = {{{0,0,0}},{{1,0,0}},{{0,1,0}},{{1,0,0}},{{2,0,0}},{{1,1,0}},{{10,0,0}}};
    centres.clear();
    CHECK(BuildForestCrownComponents(vertices, {0,1,2,3,4,5}, centres, components) == 2);
    CHECK(components[0] == components[5] && components[0] != components[6]);
    CHECK(std::abs(centres[components[0]].x - 5.0f/6.0f) < 1e-6f);
    centres.clear();
    CHECK(BuildForestCrownComponents({}, {}, centres, components) == 0 && centres.empty() && components.empty());
    std::puts("Forest crown component CPU regression passed (4 fixtures).");
}
