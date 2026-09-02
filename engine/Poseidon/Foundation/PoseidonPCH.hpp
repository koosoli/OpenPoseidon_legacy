#pragma once

// Engine-wide precompiled prelude: extra compiler options, platform shims, the C
// runtime, core engine types, math, and the application framework. Force-included
// as the precompiled header for the Poseidon target and reused by the app, test,
// and backend targets.

#include <Poseidon/Foundation/PCH/ext_options.hpp>

// Platform + C runtime
#include <Poseidon/Foundation/platform.hpp>

#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
#include <stdarg.h>

#include <Poseidon/Foundation/Memory/CheckMem.hpp>

// Engine framework primitives
#include <Poseidon/Foundation/Types/EnumDecl.hpp>

#define _ENABLE_PATCHING 1

#include <Poseidon/Foundation/PCH/stdIncludes.h>

void* ArrayNew(size_t size);
void* ArrayNew(size_t size, const char* file, int line);
void ArrayDelete(void* mem);

#define STRICT 1

#include <Poseidon/Foundation/PCH/libIncludes.hpp>

#include <Poseidon/IO/Serialization/SerializeClass.hpp>

#include <Poseidon/Foundation/Framework/GlobalAlive.hpp>

// Engine core types
#define _ENABLE_CHEATS 0

#include <Poseidon/Core/Types.hpp>
#include <Poseidon/Foundation/Math/Math3D.hpp>

#ifndef _WIN32
#include <algorithm>
using std::max;
using std::min;
#endif

#include <Poseidon/Foundation/Framework/AppFrame.hpp>
