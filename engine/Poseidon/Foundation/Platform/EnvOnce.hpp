#pragma once

// POSEIDON_GETENV_ONCE("NAME"): the environment variable read once per call site, as a
// `const char*` with getenv's meaning (null when unset).
//
// The CRT's getenv takes a lock and walks the whole environment on every call, so a switch read
// on a per-frame or per-object path costs that each time it is asked: measured on native Everon,
// trace switches read per instance were ~1% of the main thread during the world load. Only for
// switches nothing sets after start-up -- the engine's own _putenv defaults run before the world
// and the renderer read anything, and a value cached before them would be stale.

#include <cstdlib>
#include <string>

namespace Poseidon::Foundation
{
//! The value kept for the process lifetime (one small string per call site, never freed).
inline const char* KeepEnvValue(const char* value)
{
    return value ? (new std::string(value))->c_str() : nullptr;
}
} // namespace Poseidon::Foundation

#define POSEIDON_GETENV_ONCE(name)                                                                                     \
    ([]() -> const char* {                                                                                             \
        static const char* const cachedEnvValue = ::Poseidon::Foundation::KeepEnvValue(std::getenv(name));            \
        return cachedEnvValue;                                                                                         \
    }())
