#pragma once

// Height maps: pictures and grids that hold elevations.

#include <string>
#include <vector>

namespace island
{
struct HeightImage
{
    int width = 0, height = 0;
    std::vector<float> values; // row by row, north (top) first
    bool metres = false;       // values are metres (ESRI grid); otherwise 0..1
    int bits = 8;              // how fine the source was: 8-bit pictures give terraces, smooth them
    float low = 0, high = 1;   // lowest and highest value found
};

// PNG / BMP / TGA / JPG (8 or 16 bit greyscale; colour pictures by brightness),
// PGM (P5, 8 or 16 bit), RAW / R16 (square, 16 bit little-endian), ASC (ESRI ASCII grid, metres).
bool LoadHeightImage(const std::string& file, HeightImage& out, std::string& error);

// what the file dialog offers
const wchar_t* HeightImageFilter();
} // namespace island
