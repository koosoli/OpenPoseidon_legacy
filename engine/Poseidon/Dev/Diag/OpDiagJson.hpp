#pragma once

// DIAG-001: the JSON text helpers of the diagnostics layer (OpDiag.hpp). Header-only and free of
// engine types so the unit tests can check the exact output; the event writer (OpDiag::Ev) and the
// diag_* harness responses are built from these.
//
// Ported from Malprave (Dec's GPL fork of the same Poseidon source): MwDiag.cpp AppendJsonString /
// AppendNum / AppendVec and the "J" response builder of MwDiagHarness.cpp.

#include <cmath>
#include <cstdio>
#include <string>

namespace Poseidon::Dev::OpDiag::Json
{

//! a JSON string literal: quotes, backslash, \n \r \t escaped, other control bytes as \u00XX;
//! bytes >= 0x80 pass through (the engine's strings are UTF-8 or the local code page)
inline void AppendString(std::string& out, const char* s)
{
    out += '"';
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(s ? s : ""); *p; p++)
    {
        switch (*p)
        {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (*p < 0x20)
                {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", *p);
                    out += buf;
                }
                else
                {
                    out += static_cast<char>(*p);
                }
        }
    }
    out += '"';
}

inline std::string Quote(const char* s)
{
    std::string out;
    AppendString(out, s);
    return out;
}

//! a JSON number; NaN and infinities are not JSON and are written as null
inline void AppendNum(std::string& out, double v, const char* fmt = "%.6g")
{
    if (!std::isfinite(v))
    {
        out += "null";
        return;
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), fmt, v);
    out += buf;
}

//! an engine position (X east, Y up, Z north) as [x, z, height], the order getPos uses
inline void AppendPos(std::string& out, float x, float y, float z)
{
    out += '[';
    AppendNum(out, x, "%.2f");
    out += ',';
    AppendNum(out, z, "%.2f");
    out += ',';
    AppendNum(out, y, "%.2f");
    out += ']';
}

//! one JSON object built field by field: Object o; o.Str("a","b").Num("n",1); o.Done() -> {"a":"b","n":1}
class Object
{
  public:
    Object() : _s("{") {}

    Object& Str(const char* key, const char* value)
    {
        Key(key);
        AppendString(_s, value ? value : "");
        return *this;
    }
    Object& Num(const char* key, double value, const char* fmt = "%.4g")
    {
        Key(key);
        AppendNum(_s, value, fmt);
        return *this;
    }
    Object& Int(const char* key, long long value)
    {
        Key(key);
        _s += std::to_string(value);
        return *this;
    }
    Object& Bool(const char* key, bool value)
    {
        Key(key);
        _s += value ? "true" : "false";
        return *this;
    }
    Object& Pos(const char* key, float x, float y, float z)
    {
        Key(key);
        AppendPos(_s, x, y, z);
        return *this;
    }
    //! an already valid JSON value; empty is written as null
    Object& Raw(const char* key, const std::string& json)
    {
        Key(key);
        _s += json.empty() ? std::string("null") : json;
        return *this;
    }
    std::string Done()
    {
        _s += '}';
        return _s;
    }
    //! a harness response: the object with "ok":true and the trailing newline the protocol wants
    std::string Ok()
    {
        Bool("ok", true);
        return Done() + "\n";
    }

  private:
    void Key(const char* key)
    {
        if (!_first)
            _s += ',';
        _first = false;
        AppendString(_s, key);
        _s += ':';
    }
    std::string _s;
    bool _first = true;
};

} // namespace Poseidon::Dev::OpDiag::Json
