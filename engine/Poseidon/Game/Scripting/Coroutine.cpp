// SPDX-License-Identifier: GPL-3.0-or-later
#include <Poseidon/Game/Scripting/Coroutine.hpp>

#include <cstdint>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <ucontext.h>
#endif

namespace Poseidon::Coroutine
{
struct Handle
{
    void (*entry)(void*) = nullptr;
    void* arg = nullptr;
    bool finished = false;
#ifdef _WIN32
    void* fiber = nullptr;
    void* caller = nullptr;
#else
    ucontext_t context{};
    ucontext_t caller{};
    std::vector<char> stack;
#endif
};

namespace
{
void Run(Handle* h)
{
    h->entry(h->arg);
    h->finished = true;
    for (;;)
        Suspend(h); // never resumed again
}

#ifdef _WIN32
void WINAPI FiberEntry(void* param)
{
    Run(static_cast<Handle*>(param));
}
#else
void ContextEntry(unsigned lo, unsigned hi)
{
    Run(reinterpret_cast<Handle*>((static_cast<uintptr_t>(hi) << 32) | lo));
}
#endif
} // namespace

Handle* Create(void (*entry)(void*), void* arg, size_t stackSize)
{
    Handle* h = new Handle();
    h->entry = entry;
    h->arg = arg;
#ifdef _WIN32
    if (!IsThreadAFiber())
        ConvertThreadToFiber(nullptr);
    h->fiber = CreateFiberEx(64 * 1024, stackSize, 0, FiberEntry, h);
    if (!h->fiber)
    {
        delete h;
        return nullptr;
    }
#else
    h->stack.resize(stackSize);
    if (getcontext(&h->context) != 0)
    {
        delete h;
        return nullptr;
    }
    h->context.uc_stack.ss_sp = h->stack.data();
    h->context.uc_stack.ss_size = h->stack.size();
    h->context.uc_link = nullptr;
    const uintptr_t self = reinterpret_cast<uintptr_t>(h);
    makecontext(&h->context, reinterpret_cast<void (*)()>(ContextEntry), 2, static_cast<unsigned>(self & 0xffffffffu),
                static_cast<unsigned>(self >> 32));
#endif
    return h;
}

bool Resume(Handle* h)
{
    if (!h || h->finished)
        return true;
#ifdef _WIN32
    h->caller = GetCurrentFiber();
    SwitchToFiber(h->fiber);
#else
    swapcontext(&h->caller, &h->context);
#endif
    return h->finished;
}

void Suspend(Handle* h)
{
#ifdef _WIN32
    SwitchToFiber(h->caller);
#else
    swapcontext(&h->context, &h->caller);
#endif
}

void Destroy(Handle* h)
{
    if (!h)
        return;
#ifdef _WIN32
    if (h->fiber)
        DeleteFiber(h->fiber);
#endif
    delete h;
}
} // namespace Poseidon::Coroutine
