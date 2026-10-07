#include "gateway/gateway_protocol.hpp"
#include "gateway/tcp_server.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#  include <winsock2.h>
using SocketHandle = SOCKET;
static constexpr SocketHandle INVALID_SOCKET_HANDLE = INVALID_SOCKET;
#else
#  include <arpa/inet.h>
#  include <sys/socket.h>
#  include <unistd.h>
using SocketHandle = int;
static constexpr SocketHandle INVALID_SOCKET_HANDLE = -1;
#endif

using Clock = std::chrono::steady_clock;
static constexpr size_t DEFAULT_MESSAGE_COUNT = 10'000;

static void closeSocket(SocketHandle socket) {
#ifdef _WIN32
    closesocket(socket);
#else
    close(socket);
#endif
}

static bool sendAll(SocketHandle socket, const std::string& text) {
    size_t sent_total = 0;
    while (sent_total < text.size()) {
#ifdef _WIN32
        const int sent = send(socket, text.data() + sent_total,
                              static_cast<int>(text.size() - sent_total), 0);
#else
        const int sent = static_cast<int>(send(socket, text.data() + sent_total,
                                  text.size() - sent_total, MSG_NOSIGNAL));
#endif
        if (sent <= 0) return false;
        sent_total += static_cast<size_t>(sent);
    }
    return true;
}

static bool readLine(SocketHandle socket, std::string& line) {
    line.clear();
    while (line.size() < 128) {
        char ch = 0;
        if (recv(socket, &ch, 1, 0) != 1) return false;
        if (ch == '\n') return true;
        line.push_back(ch);
    }
    return false;
}

static int64_t percentile(std::vector<int64_t>& samples, size_t n, size_t d) {
    std::sort(samples.begin(), samples.end());
    return samples[(samples.size() - 1) * n / d];
}

static std::vector<int64_t> measureParser(size_t count) {
    const std::string line = "NEW 123456 987654 B L 12345 20";
    std::vector<int64_t> samples;
    samples.reserve(count);
    GatewayCommand command{};
    std::string error;
    for (size_t i = 0; i < count; ++i) {
        const auto start = Clock::now();
        const bool ok = parseGatewayLine(line, 1, command, error);
        const auto end = Clock::now();
        if (!ok) return {};
        samples.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(
            end - start).count());
    }
    return samples;
}

int main(int argc, char* argv[]) {
    const size_t message_count = argc > 1
        ? static_cast<size_t>(std::stoull(argv[1])) : DEFAULT_MESSAGE_COUNT;
    auto parser_samples = measureParser(message_count);
    if (parser_samples.size() != message_count) return 1;
    std::cout << "Parser latency (standalone, " << message_count << " messages): p50/p95/p99/p99.9="
              << percentile(parser_samples, 50, 100) << '/'
              << percentile(parser_samples, 95, 100) << '/'
              << percentile(parser_samples, 99, 100) << '/'
              << percentile(parser_samples, 999, 1000) << " ns\n";

    TcpGatewayServer server(0);
    std::exception_ptr server_error;
    std::thread server_thread([&] {
        try { server.run(); }
        catch (...) { server_error = std::current_exception(); }
    });
    const auto bind_deadline = Clock::now() + std::chrono::seconds(3);
    while (server.boundPort() == 0 && Clock::now() < bind_deadline)
        std::this_thread::yield();

    SocketHandle socket = INVALID_SOCKET_HANDLE;
    if (server.boundPort() != 0) {
        socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (socket != INVALID_SOCKET_HANDLE) {
#ifdef _WIN32
            DWORD receive_timeout = 5000;
            setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO,
                       reinterpret_cast<const char*>(&receive_timeout),
                       sizeof(receive_timeout));
#else
            timeval receive_timeout{5, 0};
            setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO,
                       &receive_timeout, sizeof(receive_timeout));
#endif
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_port = htons(server.boundPort());
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            if (connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
                closeSocket(socket);
                socket = INVALID_SOCKET_HANDLE;
            }
        }
    }

    bool ok = socket != INVALID_SOCKET_HANDLE;
    std::vector<int64_t> end_to_end;
    end_to_end.reserve(message_count);
    const auto start_all = Clock::now();
    std::string response;
    for (size_t i = 1; ok && i <= message_count; ++i) {
        const std::string request = "NEW " + std::to_string(i) + " " +
            std::to_string(i) + " B L 100 1\n";
        const auto start = Clock::now();
        ok = sendAll(socket, request) && readLine(socket, response) &&
             response == "ACK " + std::to_string(i) + " ACCEPTED";
        const auto end = Clock::now();
        if (ok) end_to_end.push_back(
            std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
    }
    const auto end_all = Clock::now();

    if (socket != INVALID_SOCKET_HANDLE) closeSocket(socket);
    server.requestStop();
    server_thread.join();
    if (server_error) std::rethrow_exception(server_error);
    if (!ok) {
        std::cerr << "TCP gateway benchmark request failed after "
                  << end_to_end.size() << " accepted messages\n";
        return 1;
    }

    const double seconds = std::chrono::duration<double>(end_all - start_all).count();
    std::cout << "TCP gateway: " << message_count << " accepted requests, "
              << std::fixed << std::setprecision(2)
              << static_cast<double>(message_count) / seconds / 1e3
              << " K requests/s; client-observed TCP->queue->engine->ACK latency p50/p95/p99/p99.9="
              << percentile(end_to_end, 50, 100) << '/'
              << percentile(end_to_end, 95, 100) << '/'
              << percentile(end_to_end, 99, 100) << '/'
              << percentile(end_to_end, 999, 1000) << " ns\n";
    return 0;
}
