// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <cstdint>

namespace Poseidon::Foundation
{

// Block the calling thread for `us` microseconds; `us <= 0` returns immediately.
//
// This exists because the sleeps this engine already has are all scheduler-granular.
// On Windows, Sleep() rounds up to the process timer resolution -- 15.6 ms by
// default, 1 ms at best under timeBeginPeriod(1) -- so any wait shorter than a frame
// either oversleeps by an order of magnitude or forces the whole process onto a 1 ms
// interrupt period. The POSIX sleepMs() (select-based) has the same whole-millisecond
// floor.
//
// On Windows this uses a waitable timer created with the high-resolution flag, which
// fires on the deadline rather than on the next timer interrupt; where that flag is
// not available (pre-1803 kernels) it degrades to a plain waitable timer, and if
// timer creation fails outright it falls back to Sleep() rounded UP, so the call
// never waits less than asked. Elsewhere it is nanosleep(), resumed across EINTR so
// a signal cannot shorten the wait.
//
// The wait is a real block, not a spin: worst-case cost is one kernel wait, and the
// timer handle is cached per thread so a pacer calling this every frame pays no
// create/close syscalls.
void SleepUs(int64_t us) noexcept;

} // namespace Poseidon::Foundation
