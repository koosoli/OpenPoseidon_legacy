#pragma once

#include <spdlog/async_logger.h>
#include <spdlog/details/thread_pool.h>
#include <spdlog/sinks/sink.h>

namespace Poseidon::Foundation
{
// Console/pipe writes can block for hundreds of milliseconds. Keep them off
// the simulation thread. File logging and error/strict-mode accounting stay
// synchronous; JSONL protocol output does not use this sink.
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
        // The pool joins after draining queued messages. No detached writer and
        // no silently dropped diagnostics, including during orderly shutdown.
        _pool.reset();
    }
    void log(const spdlog::details::log_msg& message) override
    {
        // Post the original message so category, source and timestamp survive.
        _pool->post_log(std::shared_ptr<spdlog::async_logger>(_worker), message, spdlog::async_overflow_policy::block);
    }
    void flush() override { _pool->post_flush(std::shared_ptr<spdlog::async_logger>(_worker), spdlog::async_overflow_policy::block); }
    void set_pattern(const std::string& pattern) override { _worker->set_pattern(pattern); }
    void set_formatter(std::unique_ptr<spdlog::formatter> formatter) override
    { _worker->set_formatter(std::move(formatter)); }
};
}
