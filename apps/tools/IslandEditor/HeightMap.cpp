#include "HeightMap.hpp"

#include "Island.hpp"

#include <stb_image.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace island
{
namespace
{
std::string Extension(const std::string& file)
{
    return LowerPath(std::filesystem::path(file).extension().string());
}

void Range(HeightImage& img)
{
    img.low = 1e30f;
    img.high = -1e30f;
    for (float v : img.values)
    {
        img.low = std::min(img.low, v);
        img.high = std::max(img.high, v);
    }
    if (img.values.empty())
        img.low = img.high = 0;
}

bool LoadPicture(const std::string& file, HeightImage& out, std::string& error)
{
    int w = 0, h = 0, channels = 0;
    const bool deep = stbi_is_16_bit(file.c_str()) != 0;
    stbi_us* px = stbi_load_16(file.c_str(), &w, &h, &channels, 1); // one channel: grey, by brightness
    if (!px)
    {
        error = "cannot read the picture";
        return false;
    }
    out.width = w;
    out.height = h;
    out.bits = deep ? 16 : 8;
    out.values.resize(static_cast<size_t>(w) * h);
    for (size_t i = 0; i < out.values.size(); i++)
        out.values[i] = px[i] / 65535.0f;
    stbi_image_free(px);
    return true;
}

bool LoadPgm(const std::string& file, HeightImage& out, std::string& error)
{
    std::ifstream f(file, std::ios::binary);
    if (!f)
    {
        error = "cannot open the file";
        return false;
    }
    auto token = [&]()
    {
        std::string t;
        for (;;)
        {
            const int c = f.get();
            if (c == EOF)
                break;
            if (c == '#')
            {
                while (f.good() && f.get() != '\n')
                {
                }
                continue;
            }
            if (c == ' ' || c == '\n' || c == '\r' || c == '\t')
            {
                if (!t.empty())
                    break;
                continue;
            }
            t.push_back(static_cast<char>(c));
        }
        return t;
    };
    if (token() != "P5")
    {
        error = "not a binary PGM (P5) picture";
        return false;
    }
    const int w = std::atoi(token().c_str()), h = std::atoi(token().c_str()), maxv = std::atoi(token().c_str());
    if (w < 2 || h < 2 || w > 16384 || h > 16384 || maxv < 1 || maxv > 65535)
    {
        error = "unsupported picture size or depth";
        return false;
    }
    const int bytes = maxv > 255 ? 2 : 1;
    std::vector<uint8_t> data(static_cast<size_t>(w) * h * bytes);
    f.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
    if (!f)
    {
        error = "the picture ends too early";
        return false;
    }
    out.width = w;
    out.height = h;
    out.bits = bytes * 8;
    out.values.resize(static_cast<size_t>(w) * h);
    for (size_t i = 0; i < out.values.size(); i++)
    {
        const uint8_t* p = data.data() + i * bytes;
        out.values[i] = (bytes == 2 ? (p[0] << 8 | p[1]) : p[0]) / static_cast<float>(maxv);
    }
    return true;
}

bool LoadRaw(const std::string& file, HeightImage& out, std::string& error)
{
    std::ifstream f(file, std::ios::binary | std::ios::ate);
    if (!f)
    {
        error = "cannot open the file";
        return false;
    }
    const std::streamoff bytes = f.tellg();
    const int n = static_cast<int>(std::lround(std::sqrt(static_cast<double>(bytes) / 2.0)));
    if (n < 2 || static_cast<std::streamoff>(n) * n * 2 != bytes)
    {
        error = "a RAW height map has to be square, 16 bits per point";
        return false;
    }
    std::vector<uint16_t> data(static_cast<size_t>(n) * n);
    f.seekg(0);
    f.read(reinterpret_cast<char*>(data.data()), bytes);
    out.width = out.height = n;
    out.bits = 16;
    out.values.resize(data.size());
    for (size_t i = 0; i < data.size(); i++)
        out.values[i] = data[i] / 65535.0f;
    return true;
}

bool LoadAsc(const std::string& file, HeightImage& out, std::string& error)
{
    std::ifstream f(file);
    if (!f)
    {
        error = "cannot open the file";
        return false;
    }
    int cols = 0, rows = 0;
    double nodata = -9999;
    // the header: "key value" lines until the numbers start
    for (;;)
    {
        const std::streampos at = f.tellg();
        std::string key;
        if (!(f >> key))
            break;
        const std::string k = LowerPath(key);
        if (k.empty() || !((k[0] >= 'a' && k[0] <= 'z')))
        {
            f.seekg(at);
            break;
        }
        double value = 0;
        f >> value;
        if (k == "ncols")
            cols = static_cast<int>(value);
        else if (k == "nrows")
            rows = static_cast<int>(value);
        else if (k == "nodata_value")
            nodata = value;
    }
    if (cols < 2 || rows < 2 || cols > 16384 || rows > 16384)
    {
        error = "not an ESRI ASCII grid (ncols / nrows missing)";
        return false;
    }
    out.width = cols;
    out.height = rows;
    out.metres = true;
    out.bits = 32;
    out.values.resize(static_cast<size_t>(cols) * rows);
    for (float& v : out.values)
    {
        double d = 0;
        if (!(f >> d))
        {
            error = "the grid ends too early";
            return false;
        }
        v = d == nodata ? 0.0f : static_cast<float>(d);
    }
    return true;
}
} // namespace

bool LoadHeightImage(const std::string& file, HeightImage& out, std::string& error)
{
    out = HeightImage();
    const std::string ext = Extension(file);
    bool ok = false;
    if (ext == ".pgm")
        ok = LoadPgm(file, out, error);
    else if (ext == ".raw" || ext == ".r16")
        ok = LoadRaw(file, out, error);
    else if (ext == ".asc")
        ok = LoadAsc(file, out, error);
    else
        ok = LoadPicture(file, out, error);
    if (ok)
        Range(out);
    return ok;
}

const wchar_t* HeightImageFilter()
{
    return L"*.png;*.pgm;*.raw;*.r16;*.asc;*.bmp;*.tga;*.jpg;*.jpeg";
}
} // namespace island
