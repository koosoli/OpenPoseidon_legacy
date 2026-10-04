#ifndef POSEIDON_TEST_HARNESS_SERVER_HPP
#define POSEIDON_TEST_HARNESS_SERVER_HPP

// TCP harness server — lightweight control channel for external test orchestration.
// Runs a listener thread that accepts one client at a time, parses JSON commands,
// and enqueues them for the main game loop to process.

#include <Poseidon/Dev/Harness/HarnessCommandQueue.hpp>
#include <Poseidon/Dev/Harness/HarnessProtocol.hpp>

#ifndef _WIN32
#include <pthread.h>
#endif
#include <Poseidon/Foundation/Threads/PoThread.hpp>

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// Forward-declare SOCKET type to avoid heavy engine headers
#ifdef _WIN32
typedef unsigned long long SOCKET_T;
#else
typedef int SOCKET_T;
#endif

namespace Poseidon::Dev
{

/// Callback signature for executing a harness command on the main thread.
/// Receives the command name, parsed cJSON root, and returns a JSON response string.
using HarnessCommandHandler = std::function<std::string(const std::string& name, cJSON* root)>;

/// TCP harness server — one listener thread, one client connection at a time.
class HarnessServer
{
  public:
    HarnessServer();
    ~HarnessServer();

    /// Start/Stop are serialized by their owning main/test thread.
    /// Start listening on the given port. Returns false on bind/listen failure.
    bool Start(int port);

    /// Stop the server and close all sockets.
    void Stop();

    /// Check if there are commands waiting to be processed by the main thread.
    bool HasPendingCommand() { return _queue.HasPendingCommand(); }

    /// Pop the next command for main-thread processing.
    bool PopCommand(HarnessCommand& cmd) { return _queue.PopCommand(cmd); }

    /// Push a response back to the TCP thread for sending to the client.
    void PushResponse(int cmdId, const std::string& json);

    /// Push an event to be sent to the connected client.
    void PushEvent(const std::string& json);

    /// Process a single command on the main thread. Dispatches to the
    /// handler registered for cmd.name via RegisterCommand; returns an
    /// "unhandled command" error if none is registered.
    void ProcessCommand(const HarnessCommand& cmd);

    /// Re-enqueue a command for processing next frame (used by wait_display)
    void ReEnqueueCommand(const HarnessCommand& cmd);

    /// Register a command with its metadata and dispatch handler. The metadata
    /// feeds `describe`; the handler runs on the main thread when this command
    /// arrives. Apps register their own command set at setup; only ping /
    /// describe / exit are handled internally by the server.
    void RegisterCommand(HarnessProtocol::CommandRegistration meta, HarnessCommandHandler handler);

    /// Register an event type for `describe`. Does not affect runtime; events
    /// are pushed via PushEvent regardless.
    void RegisterEvent(HarnessProtocol::EventRegistration meta);

    /// Get the registered commands for describe.
    const std::vector<HarnessProtocol::CommandRegistration>& GetRegisteredCommands() const;

    /// Get the registered events for describe.
    const std::vector<HarnessProtocol::EventRegistration>& GetRegisteredEvents() const;

    /// Is the server running?
    bool IsRunning() const { return _running.load(); }

    /// Was an exit command received? Main loop should check this each frame.
    bool IsExitRequested() const { return _exitRequested.load(); }

    /// Get the port the server is listening on.
    int GetPort() const { return _port; }

  private:
    static THREAD_PROC_RETURN THREAD_PROC_MODE WorkerThread(void* arg);
    void WorkerLoop();
    void HandleClient(SOCKET_T clientSock);

    HarnessCommandQueue _queue;
    SOCKET_T _listenSock;
    SOCKET_T _clientSock;
    std::mutex _socketMutex; // Publishes/closes the worker-owned client for Stop's shutdown.
    std::atomic<bool> _running{false};
    std::atomic<bool> _stopRequested{false};
    std::atomic<bool> _exitRequested{false};
    Foundation::ThreadId _threadId = {};
    bool _threadJoinable = false; // Owner-thread lifecycle; _running is not a join token.
    int _port = 0;
    int _nextCmdId = 1;

    std::vector<HarnessProtocol::CommandRegistration> _commands;
    std::vector<HarnessProtocol::EventRegistration> _events;
    std::unordered_map<std::string, HarnessCommandHandler> _handlers;
};

} // namespace Poseidon::Dev

#endif // POSEIDON_TEST_HARNESS_SERVER_HPP
