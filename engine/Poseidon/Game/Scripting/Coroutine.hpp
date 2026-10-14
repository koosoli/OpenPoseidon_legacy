// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>

// A minimal stackful coroutine on the calling thread (Windows fibers, POSIX ucontext).
// Kept apart from the engine headers: <windows.h> must not leak its macros into them.
namespace Poseidon::Coroutine
{
struct Handle;

// A coroutine that runs entry(arg) on its own stack of `stackSize` bytes when first
// resumed. Null when the platform refuses.
Handle* Create(void (*entry)(void*), void* arg, size_t stackSize);
// Runs the coroutine until it yields or its entry returns. Returns true once it has
// finished (a finished coroutine is not resumed again).
bool Resume(Handle* h);
// From inside the coroutine: back to the Resume that ran it.
void Suspend(Handle* h);
// Frees the coroutine; a suspended one is dropped without unwinding its stack.
void Destroy(Handle* h);
} // namespace Poseidon::Coroutine
