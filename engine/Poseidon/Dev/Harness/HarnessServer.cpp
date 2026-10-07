// HarnessServer — TCP listener + worker thread for external test orchestration.
// Protocol: newline-delimited JSON over TCP on localhost.

#include <Poseidon/Dev/Harness/HarnessServer.hpp>
#include <Poseidon/World/World.hpp>

#include <Poseidon/Network/Legacy/SocketUtil.hpp>
#include <Poseidon/Foundation/Logging/Logging.hpp>

#include <cjson/cJSON.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <stdio.h>
#ifndef _WIN32
#include <sys/time.h>
#endif
#include <utility>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/Foundation/Threads/PoThread.hpp>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#define HARNESS_INVALID_SOCKET (~(SOCKET_T)0)
#define HARNESS_SOCKET_ERROR (-1)
static void harness_closesocket(SOCKET_T s)
{
    closesocket(static_cast<SOCKET>(s));
}
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#define HARNESS_INVALID_SOCKET ((SOCKET_T) - 1)
#define HARNESS_SOCKET_ERROR (-1)
static void harness_closesocket(SOCKET_T s)
{
    ::close(static_cast<int>(s));
}
#endif

// Browser restarts launch a child with inherited standard handles. Never let it
// retain this process's listener or accepted connection: the orphaned listener
// can accept TCP handshakes after restart without a worker to answer them.
static bool harness_noinherit(SOCKET_T socket)
{
#ifdef _WIN32
    return SetHandleInformation(reinterpret_cast<HANDLE>(socket), HANDLE_FLAG_INHERIT, 0) != 0;
#else
    const int flags = fcntl(socket, F_GETFD);
    return flags >= 0 && fcntl(socket, F_SETFD, flags | FD_CLOEXEC) == 0;
#endif
}

static int harness_send(SOCKET_T socket, const char* bytes, int count)
{
#ifdef MSG_NOSIGNAL
    return static_cast<int>(send(socket, bytes, count, MSG_NOSIGNAL));
#else
    return static_cast<int>(send(socket, bytes, count, 0));
#endif
}

namespace Poseidon::Dev
{

HarnessServer::HarnessServer() : _listenSock(HARNESS_INVALID_SOCKET), _clientSock(HARNESS_INVALID_SOCKET)
{
    // ping / describe / exit are handled on the TCP thread (see HandleClient)
    // — they need no main-thread dispatch, so no handler is registered.
    // Advertised here so `describe` reflects them.
    _commands.push_back({"ping", "Health check", {}});
    _commands.push_back({"describe", "List available commands and events", {}});
    _commands.push_back({"exit", "Request clean game shutdown", {}});

    // Engine-level event types. Apps register their own via RegisterEvent.
    _events.push_back({"exit", "Game shutting down", {{"code", "int"}}});
}

HarnessServer::~HarnessServer()
{
    Stop();
}

bool HarnessServer::Start(int port)
{
    if (_threadJoinable)
        return false; // Stop must join the previous worker even if it already exited.
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
    {
        LOG_ERROR(Core, "[Harness] WSAStartup failed");
        return false;
    }
#endif

#ifdef _WIN32
    // Set the flag at creation too, so a concurrent child launch cannot inherit
    // the socket between its creation and the handle-policy check below.
    _listenSock = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0,
                             WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
#elif defined(SOCK_CLOEXEC)
    _listenSock = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
#else
    _listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#endif
    if (_listenSock == HARNESS_INVALID_SOCKET)
    {
        LOG_ERROR(Core, "[Harness] Failed to create socket");
        return false;
    }

    if (!harness_noinherit(_listenSock))
    {
        LOG_ERROR(Core, "[Harness] Failed to disable listener inheritance");
        harness_closesocket(_listenSock);
        _listenSock = HARNESS_INVALID_SOCKET;
        return false;
    }

    // Allow address reuse
    int opt = 1;
    setsockopt(_listenSock, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<unsigned short>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // localhost only

    if (bind(_listenSock, (sockaddr*)&addr, sizeof(addr)) == HARNESS_SOCKET_ERROR)
    {
        LOG_ERROR(Core, "[Harness] Failed to bind to port {}", port);
        harness_closesocket(_listenSock);
        _listenSock = HARNESS_INVALID_SOCKET;
        return false;
    }

    // If port=0, ask the OS what port was actually assigned
    if (port == 0)
        port = sockBoundPort(_listenSock);

    if (listen(_listenSock, 1) == HARNESS_SOCKET_ERROR)
    {
        LOG_ERROR(Core, "[Harness] Failed to listen on port {}", port);
        harness_closesocket(_listenSock);
        _listenSock = HARNESS_INVALID_SOCKET;
        return false;
    }

    _port = port;
    _running.store(true);
    _stopRequested.store(false);

    // Announce the bound port so the launcher (tri) can discover it.
    // Use WriteFile(STD_OUTPUT_HANDLE) directly: GUI subsystem apps have a valid
    // Win32 stdout pipe when spawned by tri, but no C-runtime FILE* stdout.
    {
        char buf[32];
        int len = snprintf(buf, sizeof(buf), "HARNESS_PORT=%d\n", _port);
#ifdef _WIN32
        HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        if (hOut != INVALID_HANDLE_VALUE && hOut != nullptr)
        {
            DWORD written = 0;
            WriteFile(hOut, buf, static_cast<DWORD>(len), &written, nullptr);
            FlushFileBuffers(hOut);
        }
#else
        fwrite(buf, 1, static_cast<size_t>(len), stdout);
        fflush(stdout);
#endif
    }

    _exitRequested.store(false);
    if (!Foundation::poThreadCreate(&_threadId, 0, &HarnessServer::WorkerThread, this) || !_threadId)
    {
        LOG_ERROR(Core, "[Harness] Failed to create worker thread");
        harness_closesocket(_listenSock);
        _listenSock = HARNESS_INVALID_SOCKET;
        _running.store(false);
        return false;
    }
    _threadJoinable = true;

    LOG_INFO(Core, "[Harness] Listening on localhost:{}", port);
    return true;
}

void HarnessServer::Stop()
{
    if (!_threadJoinable)
        return;

    _stopRequested.store(true);

    // Interrupt connected recv/send without closing/reusing its handle while
    // the worker still uses it. Publication and close share this mutex.
    // The listen socket stays immutable until join; its select has a 1s timeout.
    {
        std::lock_guard<std::mutex> lock(_socketMutex);
        if (_clientSock != HARNESS_INVALID_SOCKET)
        {
#ifdef _WIN32
            shutdown(_clientSock, SD_BOTH);
#else
            shutdown(_clientSock, SHUT_RDWR);
#endif
        }
    }

    Foundation::poThreadJoin(_threadId, nullptr);
#ifdef _WIN32
    CloseHandle(_threadId);
#endif
    _threadId = {};
    _threadJoinable = false;
    harness_closesocket(_listenSock);
    _listenSock = HARNESS_INVALID_SOCKET;

    _running.store(false);
    LOG_INFO(Core, "[Harness] Server stopped");
}

// ── Main Thread Interface ──────────────────────────────────────────────────

void HarnessServer::PushResponse(int cmdId, const std::string& json)
{
    HarnessResponse resp;
    resp.id = cmdId;
    resp.json = json;
    _queue.PushResponse(resp);
}

void HarnessServer::PushEvent(const std::string& json)
{
    HarnessEvent evt;
    evt.json = json;
    _queue.PushEvent(evt);
}

void HarnessServer::ProcessCommand(const HarnessCommand& cmd)
{
    // This runs on the main thread, so checking the live world here also rejects
    // commands queued before joining MP. TCP-thread ping/describe/exit stay available.
    if (GWorld && GWorld->GetMode() == GModeNetware)
    {
        PushResponse(cmd.id, HarnessProtocol::ErrorResponse("developer harness disabled in multiplayer"));
        return;
    }
    cJSON* root = cJSON_Parse(cmd.raw.c_str());
    if (!root)
    {
        PushResponse(cmd.id, HarnessProtocol::ErrorResponse("invalid JSON"));
        return;
    }

    auto it = _handlers.find(cmd.name);
    std::string response =
        (it != _handlers.end()) ? it->second(cmd.name, root) : HarnessProtocol::ErrorResponse("unhandled command");

    cJSON_Delete(root);
    // Empty response means deferred (e.g., wait_display re-enqueued for next frame)
    if (!response.empty())
        PushResponse(cmd.id, response);
}

void HarnessServer::ReEnqueueCommand(const HarnessCommand& cmd)
{
    _queue.PushCommand(cmd);
}

// ── Command/Event Registration ─────────────────────────────────────────────

void HarnessServer::RegisterCommand(HarnessProtocol::CommandRegistration meta, HarnessCommandHandler handler)
{
    _handlers[meta.name] = std::move(handler);
    _commands.push_back(std::move(meta));
}

void HarnessServer::RegisterEvent(HarnessProtocol::EventRegistration meta)
{
    _events.push_back(std::move(meta));
}

const std::vector<HarnessProtocol::CommandRegistration>& HarnessServer::GetRegisteredCommands() const
{
    return _commands;
}

const std::vector<HarnessProtocol::EventRegistration>& HarnessServer::GetRegisteredEvents() const
{
    return _events;
}

// ── Worker Thread ──────────────────────────────────────────────────────────

THREAD_PROC_RETURN THREAD_PROC_MODE HarnessServer::WorkerThread(void* arg)
{
    auto* server = static_cast<HarnessServer*>(arg);
    server->WorkerLoop();
    server->_running.store(false);
    return (THREAD_PROC_RETURN)0;
}

void HarnessServer::WorkerLoop()
{
    LOG_DEBUG(Core, "[Harness] Worker thread started");

    while (!_stopRequested.load())
    {
        // Use select() with timeout so we can check _stopRequested periodically
        fd_set readSet;
        FD_ZERO(&readSet);
        if (_listenSock == HARNESS_INVALID_SOCKET)
            break;
        FD_SET(_listenSock, &readSet);

        timeval tv = {};
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        int sel = select(static_cast<int>(_listenSock) + 1, &readSet, nullptr, nullptr, &tv);
        if (sel <= 0)
            continue;

        sockaddr_in clientAddr = {};
#ifdef _WIN32
        int addrLen = sizeof(clientAddr);
#else
        socklen_t addrLen = sizeof(clientAddr);
#endif
        SOCKET_T clientSock = accept(_listenSock, (sockaddr*)&clientAddr, &addrLen);
        if (clientSock == HARNESS_INVALID_SOCKET)
        {
            if (!_stopRequested.load())
                LOG_WARN(Core, "[Harness] accept() failed");
            continue;
        }

        if (!harness_noinherit(clientSock))
        {
            LOG_WARN(Core, "[Harness] Failed to disable client inheritance");
            harness_closesocket(clientSock);
            continue;
        }

        LOG_INFO(Core, "[Harness] Client connected");

        {
            std::lock_guard<std::mutex> lock(_socketMutex);
            if (_stopRequested.load())
            {
                harness_closesocket(clientSock);
                break;
            }
            _clientSock = clientSock;
        }

        // Disable Nagle for low-latency command/response
        int flag = 1;
        setsockopt(clientSock, IPPROTO_TCP, TCP_NODELAY, (const char*)&flag, sizeof(flag));
#ifdef SO_NOSIGPIPE
        setsockopt(clientSock, SOL_SOCKET, SO_NOSIGPIPE, (const char*)&flag, sizeof(flag));
#endif

        HandleClient(clientSock);

        {
            std::lock_guard<std::mutex> lock(_socketMutex);
            _clientSock = HARNESS_INVALID_SOCKET;
            harness_closesocket(clientSock);
        }
        LOG_INFO(Core, "[Harness] Client disconnected");
    }

    LOG_DEBUG(Core, "[Harness] Worker thread exiting");
}

void HarnessServer::HandleClient(SOCKET_T clientSock)
{
    std::string buffer;
    char recvBuf[4096];

    while (!_stopRequested.load())
    {
        // First, drain any pending events and send to client
        auto events = _queue.DrainEvents();
        for (const auto& evt : events)
        {
            int sent = harness_send(clientSock, evt.json.c_str(), static_cast<int>(evt.json.size()));
            if (sent == HARNESS_SOCKET_ERROR)
            {
                LOG_DEBUG(Core, "[Harness] Send event failed, client gone");
                return;
            }
        }

        // Use select() to check for data with timeout
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(clientSock, &readSet);

        timeval tv = {};
        tv.tv_sec = 0;
        tv.tv_usec = 50000; // 50ms — check events frequently

        int sel = select(static_cast<int>(clientSock) + 1, &readSet, nullptr, nullptr, &tv);
        if (sel < 0)
        {
            LOG_DEBUG(Core, "[Harness] select() error on client socket");
            return;
        }
        if (sel == 0)
            continue; // timeout — loop back to drain events

        int bytes = recv(clientSock, recvBuf, sizeof(recvBuf) - 1, 0);
        if (bytes <= 0)
        {
            LOG_DEBUG(Core, "[Harness] Client connection closed (recv={})", bytes);
            return;
        }
        recvBuf[bytes] = '\0';
        buffer += recvBuf;

        // Process complete lines
        size_t pos;
        while ((pos = buffer.find('\n')) != std::string::npos)
        {
            std::string line = buffer.substr(0, pos);
            buffer.erase(0, pos + 1);

            while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                line.pop_back();
            if (line.empty())
                continue;

            cJSON* root = cJSON_Parse(line.c_str());
            if (!root)
            {
                std::string err = HarnessProtocol::ErrorResponse("invalid JSON");
                harness_send(clientSock, err.c_str(), static_cast<int>(err.size()));
                continue;
            }

            const char* cmdNameRaw = HarnessProtocol::GetString(root, "cmd");
            std::string cmdStr = cmdNameRaw ? cmdNameRaw : "";
            cJSON_Delete(root);

            if (cmdStr.empty())
            {
                std::string err = HarnessProtocol::ErrorResponse("missing 'cmd' field");
                harness_send(clientSock, err.c_str(), static_cast<int>(err.size()));
                continue;
            }
            const char* cmdName = cmdStr.c_str();

            // Handle ping, describe, and exit directly on the TCP thread (no main-thread needed)
            if (std::strcmp(cmdName, "ping") == 0)
            {
                std::string resp = HarnessProtocol::OkResponse();
                harness_send(clientSock, resp.c_str(), static_cast<int>(resp.size()));
                continue;
            }

            if (std::strcmp(cmdName, "describe") == 0)
            {
                std::string resp = HarnessProtocol::DescribeResponse(_commands, _events);
                harness_send(clientSock, resp.c_str(), static_cast<int>(resp.size()));
                continue;
            }

            if (std::strcmp(cmdName, "exit") == 0)
            {
                std::string resp = HarnessProtocol::OkResponse();
                harness_send(clientSock, resp.c_str(), static_cast<int>(resp.size()));
                _exitRequested.store(true);
                return; // close client connection
            }

            // Enqueue for main thread processing
            HarnessCommand cmd;
            cmd.name = cmdName;
            cmd.raw = line;
            cmd.id = _nextCmdId++;
            _queue.PushCommand(cmd);

            // Wait for the response from the main thread. The deadline is
            // configurable via HARNESS_CMD_TIMEOUT_SEC (default 30s): a sanitizer
            // build loads worlds/mods far slower than the main thread pumps the
            // command queue, so a command issued right after such a trigger needs
            // a longer window than 30s before the load finishes and it is drained.
            static const int cmdTimeoutSec = []
            {
                const char* e = getenv("HARNESS_CMD_TIMEOUT_SEC");
                int v = e ? atoi(e) : 0;
                return v > 0 ? v : 30;
            }();
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(cmdTimeoutSec);
            bool gotResponse = false;

            while (!_stopRequested.load() && std::chrono::steady_clock::now() < deadline)
            {
                // Also drain events while waiting
                auto waitEvents = _queue.DrainEvents();
                for (const auto& evt : waitEvents)
                {
                    int sent = harness_send(clientSock, evt.json.c_str(), static_cast<int>(evt.json.size()));
                    if (sent == HARNESS_SOCKET_ERROR)
                        return;
                }

                HarnessResponse resp;
                if (_queue.PopResponse(resp) && resp.id == cmd.id)
                {
                    int sent = harness_send(clientSock, resp.json.c_str(), static_cast<int>(resp.json.size()));
                    if (sent == HARNESS_SOCKET_ERROR)
                        return;
                    gotResponse = true;
                    break;
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }

            if (!gotResponse)
            {
                std::string err = HarnessProtocol::ErrorResponse("command timed out");
                harness_send(clientSock, err.c_str(), static_cast<int>(err.size()));
            }
        }
    }
}

} // namespace Poseidon::Dev
