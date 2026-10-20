#pragma once

#include <spdlog/async_logger.h>
#include <spdlog/details/thread_pool.h>
#include <spdlog/sinks/sink.h>

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
}
