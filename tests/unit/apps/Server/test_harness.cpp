#include <catch2/catch_test_macros.hpp>
#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#include <chrono>
#include <thread>

#include <Poseidon/Dev/Harness/HarnessServer.hpp>

using Poseidon::Dev::HarnessServer;

TEST_CASE("HarnessServer can auto-assign a localhost port", "[simulate][harness]")
{
    HarnessServer server;
    REQUIRE(server.Start(0));
    REQUIRE(server.IsRunning());
    REQUIRE(server.GetPort() > 0);
    server.Stop();
}

TEST_CASE("Harness shutdown joins its worker and supports repeated owner starts", "[simulate][harness][upstream]")
{
    HarnessServer server;
    for (int run = 0; run < 3; ++run)
    {
        REQUIRE(server.Start(0));
        CHECK_FALSE(server.Start(0));
        server.Stop();
        CHECK_FALSE(server.IsRunning());
        server.Stop(); // Already joined, no second wait/close.
    }
}

TEST_CASE("Harness shutdown aborts a connected client awaiting main-thread dispatch", "[simulate][harness][upstream]")
{
    HarnessServer server;
    REQUIRE(server.Start(0));
    const auto socketHandle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#ifdef _WIN32
    REQUIRE(socketHandle != INVALID_SOCKET);
#else
    REQUIRE(socketHandle >= 0);
#endif
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(static_cast<unsigned short>(server.GetPort()));
    REQUIRE(connect(socketHandle, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0);
    constexpr const char command[] = "{\"cmd\":\"never-dispatched\"}\n";
    REQUIRE(send(socketHandle, command, sizeof(command) - 1, 0) == sizeof(command) - 1);
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!server.HasPendingCommand() && std::chrono::steady_clock::now() < until)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    REQUIRE(server.HasPendingCommand());
    const auto began = std::chrono::steady_clock::now();
    server.Stop();
    CHECK(std::chrono::steady_clock::now() - began < std::chrono::seconds(3));
    CHECK_FALSE(server.IsRunning());
#ifdef _WIN32
    closesocket(socketHandle);
#else
    close(socketHandle);
#endif
}

TEST_CASE("Harness shutdown interrupts a client which stops reading events", "[simulate][harness][upstream]")
{
    HarnessServer server;
    REQUIRE(server.Start(0));
    const auto socketHandle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#ifdef _WIN32
    REQUIRE(socketHandle != INVALID_SOCKET);
#else
    REQUIRE(socketHandle >= 0);
#endif
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(static_cast<unsigned short>(server.GetPort()));
    REQUIRE(connect(socketHandle, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0);
    server.PushEvent(std::string(8 * 1024 * 1024, 'x'));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const auto began = std::chrono::steady_clock::now();
    server.Stop();
    CHECK(std::chrono::steady_clock::now() - began < std::chrono::seconds(3));
    CHECK_FALSE(server.IsRunning());
#ifdef _WIN32
    closesocket(socketHandle);
#else
    close(socketHandle);
#endif
}
