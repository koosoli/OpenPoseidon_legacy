#pragma once

#include <spdlog/async_logger.h>
#include <spdlog/details/thread_pool.h>
#include <spdlog/sinks/sink.h>
#include <chrono>
#include <thread>

namespace Poseidon::Foundation
{
// Console/pipe writes can block for hundreds of milliseconds. Keep them off
// the simulation thread. File logging and error/strict-mode accounting stay
// synchronous; JSONL protocol output does not use this sink. A console that cannot keep up
// loses its oldest queued lines (the file has them), never the frame.
class QueuedConsoleSink final : public spdlog::sinks::sink
{
    std::shared_ptr<spdlog::details::thread_pool> _pool;
    std::shared_ptr<spdlog::async_logger> _worker;
public:
    explicit QueuedConsoleSink(std::shared_ptr<spdlog::sinks::sink> output)
        : _pool(std::make_shared<spdlog::details::thread_pool>(8192, 1)),
          _worker(std::make_shared<spdlog::async_logger>("console-worker", std::move(output), _pool,
              spdlog::async_overflow_policy::block)) {}
    ~QueuedConsoleSink() override
    {
        // The pool joins after draining queued messages: no detached writer, and an orderly
        // shutdown prints everything still queued.
        _pool.reset();
    }
    void log(const spdlog::details::log_msg& message) override
    {
        // Post the original message so category, source and timestamp survive.
        // PERF-CONSOLE-NONBLOCK (2026-10-20): when the console falls 8192 lines behind, the oldest
        // queued CONSOLE lines are dropped instead of the simulation thread waiting for the
        // console -- 12% of the main thread's time in DayZ's slow frames (Nsight, ns-dayz2). The
        // file sink is synchronous and keeps every line; POSEIDON_SYNC_CONSOLE=1 restores a
        // console that never drops (and blocks).
        _pool->post_log(std::shared_ptr<spdlog::async_logger>(_worker), message,
                        spdlog::async_overflow_policy::overrun_oldest);
    }
    // Every message flushes when a log file is open (flush_on(trace), for the file's sake), so
    // this ran once per line: a second queue entry per message, blocking when the queue was full.
    // The console writer does not buffer and the destructor drains the queue, so a queued
    // flush adds nothing: it is not posted at all.
    void flush() override {}
    void set_pattern(const std::string& pattern) override { _worker->set_pattern(pattern); }
    void set_formatter(std::unique_ptr<spdlog::formatter> formatter) override
    { _worker->set_formatter(std::move(formatter)); }
};

// PERF-LOG-FILE-QUEUE (2026-10-20). The log file was written and flushed on the thread that
// logged: on Arma 1 Sahrani the render thread sat 300-940 ms inside Ntfs/FLTMGR (a file-system
// filter holding the write) while appending its statistics block, nine times in one minute
// (Nsight ns-a1). The lines now go through a queue to one writer thread, which still flushes
// every line, so the file is as complete as before on an abrupt exit once the queue has drained
// (the crash handler drains it: DrainLogFileQueue). Nothing is dropped: a full queue (65536
// lines) blocks the caller, as the direct write did. POSEIDON_SYNC_LOG_FILE=1 = the old path.
class QueuedFileSink final : public spdlog::sinks::sink
{
    std::shared_ptr<spdlog::details::thread_pool> _pool;
    std::shared_ptr<spdlog::async_logger> _worker;
public:
    explicit QueuedFileSink(std::shared_ptr<spdlog::sinks::sink> output)
        : _pool(std::make_shared<spdlog::details::thread_pool>(65536, 1)),
          _worker(std::make_shared<spdlog::async_logger>("file-worker", std::move(output), _pool,
              spdlog::async_overflow_policy::block))
    {
        _worker->set_level(spdlog::level::trace);
        _worker->flush_on(spdlog::level::trace); // the writer flushes every line, off the game's threads
    }
    ~QueuedFileSink() override;
    void log(const spdlog::details::log_msg& message) override
    {
        _pool->post_log(std::shared_ptr<spdlog::async_logger>(_worker), message, spdlog::async_overflow_policy::block);
    }
    // Every message asks for a flush (flush_on(trace)); the writer already flushes each line.
    void flush() override {}
    void set_pattern(const std::string& pattern) override { _worker->set_pattern(pattern); }
    void set_formatter(std::unique_ptr<spdlog::formatter> formatter) override
    { _worker->set_formatter(std::move(formatter)); }
    // Wait (bounded) until the writer has taken every queued line, plus a moment for the last
    // write. For the crash handler, before the process ends.
    void Drain(int timeoutMs)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (_pool && _pool->queue_size() > 0 && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
};

// The live queued file sink, if any (one per process), for DrainLogFileQueue.
inline QueuedFileSink* GQueuedFileSink = nullptr;

inline QueuedFileSink::~QueuedFileSink()
{
    if (GQueuedFileSink == this)
        GQueuedFileSink = nullptr;
    // The pool joins after writing every queued line.
    _pool.reset();
}
}
