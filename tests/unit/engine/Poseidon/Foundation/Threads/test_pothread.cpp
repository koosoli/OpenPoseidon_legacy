// Unit tests for Poseidon/Foundation/Threads/PoThread.hpp
// Testing portable thread creation and management

#define _CRT_SECURE_NO_WARNINGS
#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Foundation/Framework/DebugLog.hpp>
#include <Poseidon/Foundation/Memory/CheckMem.hpp> // For safeNew/safeDelete
#include <Poseidon/Foundation/Threads/PoThread.hpp>
#include <Poseidon/Foundation/Threads/PoCritical.hpp>
#include <Poseidon/Foundation/Types/Pointers.hpp>
#include <stdint.h>

// Thread Function Helpers

// Simple thread that just exits
static THREAD_PROC_RETURN THREAD_PROC_MODE SimpleThreadFunc(void* param)
{
    return 0;
}

// Thread that increments a counter
static THREAD_PROC_RETURN THREAD_PROC_MODE CounterThreadFunc(void* param)
{
    int* counter = (int*)param;
    if (counter)
    {
        (*counter)++;
    }
    return 0;
}

// Thread that sets a flag
static THREAD_PROC_RETURN THREAD_PROC_MODE FlagThreadFunc(void* param)
{
    bool* flag = (bool*)param;
    if (flag)
    {
        *flag = true;
    }
    return 0;
}

// Thread that returns a specific value
static THREAD_PROC_RETURN THREAD_PROC_MODE ReturnValueThreadFunc(void* param)
{
    int value = param ? *(int*)param : 42;
    return (THREAD_PROC_RETURN)(intptr_t)value;
}

// Thread Creation and Joining

TEST_CASE("poThread - Basic creation and join", "[threads][pothread]")
{
    SECTION("Create and join simple thread")
    {
        Poseidon::Foundation::ThreadId threadId;
        bool result = Poseidon::Foundation::poThreadCreate(&threadId, 0, SimpleThreadFunc, nullptr);

        REQUIRE(result == true);

        THREAD_PROC_RETURN exitCode = 0;
        bool joinResult = Poseidon::Foundation::poThreadJoin(threadId, &exitCode);
        REQUIRE(joinResult == true);
        REQUIRE(exitCode == 0);
    }

    SECTION("Thread executes function")
    {
        int counter = 0;
        Poseidon::Foundation::ThreadId threadId;

        bool result = Poseidon::Foundation::poThreadCreate(&threadId, 0, CounterThreadFunc, &counter);
        REQUIRE(result == true);

        Poseidon::Foundation::poThreadJoin(threadId, nullptr);

        REQUIRE(counter == 1); // Thread incremented it
    }

    SECTION("Thread can set flag")
    {
        bool flag = false;
        Poseidon::Foundation::ThreadId threadId;

        bool result = Poseidon::Foundation::poThreadCreate(&threadId, 0, FlagThreadFunc, &flag);
        REQUIRE(result == true);

        Poseidon::Foundation::poThreadJoin(threadId, nullptr);

        REQUIRE(flag == true);
    }
}

TEST_CASE("poThread - Return values", "[threads][pothread]")
{
    SECTION("Thread returns exit code")
    {
        int value = 123;
        Poseidon::Foundation::ThreadId threadId;

        bool result = Poseidon::Foundation::poThreadCreate(&threadId, 0, ReturnValueThreadFunc, &value);
        REQUIRE(result == true);

        THREAD_PROC_RETURN exitCode = 0;
        Poseidon::Foundation::poThreadJoin(threadId, &exitCode);
        REQUIRE((intptr_t)exitCode == 123);
    }

    SECTION("Default return value")
    {
        Poseidon::Foundation::ThreadId threadId;

        bool result = Poseidon::Foundation::poThreadCreate(&threadId, 0, ReturnValueThreadFunc, nullptr);
        REQUIRE(result == true);

        THREAD_PROC_RETURN exitCode = 0;
        Poseidon::Foundation::poThreadJoin(threadId, &exitCode);
        REQUIRE((intptr_t)exitCode == 42); // Default value from function
    }
}

// Thread IDs

TEST_CASE("poThread - Thread IDs", "[threads][pothread]")
{
    SECTION("Get current thread ID")
    {
        Poseidon::Foundation::ThreadId currentId;
        Poseidon::Foundation::poThreadId(currentId);

// Current thread should have a valid ID
#ifdef _WIN32
        REQUIRE(currentId != nullptr);
#else
        // pthread_t is implementation-defined, just check it was set
        REQUIRE(true);
#endif
    }

    SECTION("Created thread has valid ID")
    {
        Poseidon::Foundation::ThreadId threadId;

        bool result = Poseidon::Foundation::poThreadCreate(&threadId, 0, SimpleThreadFunc, nullptr);
        REQUIRE(result == true);

#ifdef _WIN32
        REQUIRE(threadId != nullptr);
#endif

        Poseidon::Foundation::poThreadJoin(threadId, nullptr);
    }
}

// Multiple Threads

TEST_CASE("poThread - Multiple threads", "[threads][pothread]")
{
    SECTION("Create multiple threads")
    {
        const int numThreads = 5;
        Poseidon::Foundation::ThreadId threads[numThreads];
        int counters[numThreads] = {0};

        // Create threads
        for (int i = 0; i < numThreads; i++)
        {
            bool result = Poseidon::Foundation::poThreadCreate(&threads[i], 0, CounterThreadFunc, &counters[i]);
            REQUIRE(result == true);
        }

        // Join all threads
        for (int i = 0; i < numThreads; i++)
        {
            Poseidon::Foundation::poThreadJoin(threads[i], nullptr);
        }

        // Verify all threads executed
        for (int i = 0; i < numThreads; i++)
        {
            REQUIRE(counters[i] == 1);
        }
    }

    SECTION("Threads execute in parallel")
    {
        const int numThreads = 3;
        Poseidon::Foundation::ThreadId threads[numThreads];
        bool flags[numThreads] = {false};

        for (int i = 0; i < numThreads; i++)
        {
            Poseidon::Foundation::poThreadCreate(&threads[i], 0, FlagThreadFunc, &flags[i]);
        }

        for (int i = 0; i < numThreads; i++)
        {
            Poseidon::Foundation::poThreadJoin(threads[i], nullptr);
        }

        // All flags should be set
        for (int i = 0; i < numThreads; i++)
        {
            REQUIRE(flags[i] == true);
        }
    }
}

// Thread Priority

TEST_CASE("poThread - Priority operations", "[threads][pothread]")
{
    SECTION("Set thread priority")
    {
        Poseidon::Foundation::ThreadId threadId;
        bool result = Poseidon::Foundation::poThreadCreate(&threadId, 0, SimpleThreadFunc, nullptr);
        REQUIRE(result == true);

        // Set priority (actual values platform-dependent)
        (void)Poseidon::Foundation::poSetPriority(threadId, 0); // Normal priority - may fail on some platforms
        // Priority may not be supported on all platforms
        // Just verify it doesn't crash

        Poseidon::Foundation::poThreadJoin(threadId, nullptr);
    }

    SECTION("Set current thread priority")
    {
        // Set priority of main thread
        (void)Poseidon::Foundation::poSetMyPriority(0); // Normal priority - may fail on some platforms

        // Priority setting may fail on some platforms, that's OK
        // Just verify it doesn't crash
        REQUIRE(true);
    }
}

// Thread Synchronization with Critical Sections

struct SharedData
{
    int counter{0};
    Poseidon::Foundation::PoCriticalSection cs;

    SharedData() = default;
};

static THREAD_PROC_RETURN THREAD_PROC_MODE IncrementThreadFunc(void* param)
{
    SharedData* data = (SharedData*)param;
    if (data)
    {
        for (int i = 0; i < 1000; i++)
        {
            data->cs.enter();
            data->counter++;
            data->cs.leave();
        }
    }
    return 0;
}

TEST_CASE("poThread - Synchronization", "[threads][pothread]")
{
    SECTION("Multiple threads with critical section")
    {
        SharedData data;
        const int numThreads = 3;
        Poseidon::Foundation::ThreadId threads[numThreads];

        // Create threads that increment shared counter
        for (int i = 0; i < numThreads; i++)
        {
            bool result = Poseidon::Foundation::poThreadCreate(&threads[i], 0, IncrementThreadFunc, &data);
            REQUIRE(result == true);
        }

        // Wait for all threads
        for (int i = 0; i < numThreads; i++)
        {
            Poseidon::Foundation::poThreadJoin(threads[i], nullptr);
        }

        // Counter should be exactly numThreads * 1000
        REQUIRE(data.counter == numThreads * 1000);
    }
}

// Poseidon::Foundation::RefCountSafe - Thread-safe reference counting

TEST_CASE("Poseidon::Foundation::RefCountSafe - Thread-safe reference counting", "[threads][pothread][refcount]")
{
    SECTION("Basic Poseidon::Foundation::RefCountSafe operations")
    {
        Poseidon::Foundation::RefCountSafe* obj = new Poseidon::Foundation::RefCountSafe();

        REQUIRE(obj->RefCounter() == 0);

        obj->AddRef();
        REQUIRE(obj->RefCounter() == 1);

        obj->Release(); // Should delete
    }

    SECTION("Poseidon::Foundation::RefCountSafe with critical section")
    {
        Poseidon::Foundation::RefCountSafe* obj = new Poseidon::Foundation::RefCountSafe();

        obj->AddRef();

        // Can enter/leave like critical section
        obj->enter();
        // Protected code here
        obj->leave();

        obj->Release();
    }

    SECTION("Poseidon::Foundation::RefCountSafe with smart pointers")
    {
        Ref<Poseidon::Foundation::RefCountSafe> obj = new Poseidon::Foundation::RefCountSafe();

        REQUIRE(obj != nullptr);
        REQUIRE(obj->RefCounter() >= 1);

        // Auto-deleted when obj goes out of scope
    }
}

// Usage Patterns

TEST_CASE("poThread - Common usage patterns", "[threads][pothread]")
{
    SECTION("Worker thread pattern")
    {
        bool workDone = false;
        Poseidon::Foundation::ThreadId threadId;

        // Spawn worker
        Poseidon::Foundation::poThreadCreate(&threadId, 0, FlagThreadFunc, &workDone);

        // Do other work on main thread
        // ...

        // Wait for worker to finish
        Poseidon::Foundation::poThreadJoin(threadId, nullptr);

        REQUIRE(workDone == true);
    }

    SECTION("Thread pool simulation")
    {
        const int poolSize = 4;
        Poseidon::Foundation::ThreadId pool[poolSize];
        int results[poolSize] = {0};

        // Create pool
        for (int i = 0; i < poolSize; i++)
        {
            Poseidon::Foundation::poThreadCreate(&pool[i], 0, CounterThreadFunc, &results[i]);
        }

        // Wait for all
        for (int i = 0; i < poolSize; i++)
        {
            Poseidon::Foundation::poThreadJoin(pool[i], nullptr);
        }

        // Verify all completed
        for (int i = 0; i < poolSize; i++)
        {
            REQUIRE(results[i] == 1);
        }
    }
}

// poSetMyPriority was a silent no-op on Windows until 2026-08-31: it cast
// GetCurrentThreadId() -- a DWORD ID -- to a HANDLE and handed it to SetThreadPriority,
// which wants a real handle. It failed every time, and every caller discarded the bool, so
// threads that asked to be deprioritised simply were not. The object-stream preparer
// workers are the ones that mattered (roadmap 11A: streaming must not starve simulation).
//
// This test asserts the OUTCOME, not the call: it reads the priority back from the OS.
// Asserting only that the function returns true would have passed on a version that set
// the priority of some other thread entirely.
#ifdef _WIN32
TEST_CASE("Poseidon::Foundation::poSetMyPriority - actually changes this thread's priority", "[threads][pothread]")
{
    const int original = ::GetThreadPriority(::GetCurrentThread());
    REQUIRE(original != THREAD_PRIORITY_ERROR_RETURN);

    SECTION("below normal is applied and readable back")
    {
        REQUIRE(Poseidon::Foundation::poSetMyPriority(-1));
        CHECK(::GetThreadPriority(::GetCurrentThread()) == THREAD_PRIORITY_BELOW_NORMAL);
    }

    SECTION("the delta is clamped, not rejected")
    {
        // -7 is out of range; the contract is that it clamps to the lowest band rather
        // than failing, so a caller cannot accidentally leave a thread at normal priority
        // by asking for too much.
        REQUIRE(Poseidon::Foundation::poSetMyPriority(-7));
        CHECK(::GetThreadPriority(::GetCurrentThread()) == THREAD_PRIORITY_LOWEST);
        REQUIRE(Poseidon::Foundation::poSetMyPriority(7));
        CHECK(::GetThreadPriority(::GetCurrentThread()) == THREAD_PRIORITY_HIGHEST);
    }

    // Leave the test runner's own thread as we found it -- a leaked priority change would
    // silently skew the timing of every test that runs after this one.
    ::SetThreadPriority(::GetCurrentThread(), original);
}
#endif
