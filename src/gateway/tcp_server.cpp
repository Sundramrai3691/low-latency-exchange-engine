#include "tcp_server.hpp"

#include "engine_worker.hpp"
#include "../concurrency/spsc_queue.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <winsock2.h>
#  include <ws2tcpip.h>
using SocketHandle = SOCKET;
static constexpr SocketHandle INVALID_SOCKET_HANDLE = INVALID_SOCKET;
#else
#  include <arpa/inet.h>
#  include <fcntl.h>
#  include <sys/select.h>
#  include <sys/socket.h>
#  include <unistd.h>
using SocketHandle = int;
static constexpr SocketHandle INVALID_SOCKET_HANDLE = -1;
#endif

namespace {

static constexpr size_t QUEUE_SIZE = 4096;
static constexpr size_t MAX_CLIENTS = 32;
static constexpr size_t MAX_LINE = 256;
static constexpr size_t MAX_OUTPUT = 64 * 1024;

static void closeSocket(SocketHandle socket) {
    if (socket == INVALID_SOCKET_HANDLE) return;
#ifdef _WIN32
    closesocket(socket);
#else
    close(socket);
#endif
}

static bool makeNonBlocking(SocketHandle socket) {
#ifdef _WIN32
    u_long enabled = 1;
    return ioctlsocket(socket, FIONBIO, &enabled) == 0;
#else
    const int flags = fcntl(socket, F_GETFL, 0);
    return flags >= 0 && fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

static bool wouldBlock() {
#ifdef _WIN32
    const int error = WSAGetLastError();
    return error == WSAEWOULDBLOCK || error == WSAEINTR;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
}

static int selectSockets(SocketHandle max_socket, fd_set* reads, fd_set* writes,
                         timeval* timeout) {
#ifdef _WIN32
    (void)max_socket;
    return select(0, reads, writes, nullptr, timeout);
#else
    return select(max_socket + 1, reads, writes, nullptr, timeout);
#endif
}

struct Client {
    SocketHandle socket{INVALID_SOCKET_HANDLE};
    uint64_t session{0};
    uint64_t last_request{0};
    std::string input;
    std::string output;
    bool close_after_write{false};
};

} // namespace

struct TcpGatewayServer::Impl {
    explicit Impl(uint16_t requested) : requested_port(requested) {}
    uint16_t requested_port;
    std::atomic<uint16_t> active_port{0};
    std::atomic<bool> stopping{false};
};

TcpGatewayServer::TcpGatewayServer(uint16_t port)
    : impl_(std::make_unique<Impl>(port)) {}

TcpGatewayServer::~TcpGatewayServer() = default;

void TcpGatewayServer::requestStop() noexcept {
    impl_->stopping.store(true, std::memory_order_release);
}

uint16_t TcpGatewayServer::boundPort() const noexcept {
    return impl_->active_port.load(std::memory_order_acquire);
}

int TcpGatewayServer::run() {
#ifdef _WIN32
    WSADATA winsock_data{};
    if (WSAStartup(MAKEWORD(2, 2), &winsock_data) != 0) {
        throw std::runtime_error("WSAStartup failed");
    }
#endif

    SocketHandle listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET_HANDLE) {
#ifdef _WIN32
        WSACleanup();
#endif
        throw std::runtime_error("socket creation failed");
    }

    int reuse = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(impl_->requested_port);
    if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        listen(listener, static_cast<int>(MAX_CLIENTS)) != 0 ||
        !makeNonBlocking(listener)) {
        closeSocket(listener);
#ifdef _WIN32
        WSACleanup();
#endif
        throw std::runtime_error("bind/listen failed");
    }

    sockaddr_in bound{};
#ifdef _WIN32
    int bound_size = sizeof(bound);
#else
    socklen_t bound_size = sizeof(bound);
#endif
    if (getsockname(listener, reinterpret_cast<sockaddr*>(&bound), &bound_size) != 0) {
        closeSocket(listener);
#ifdef _WIN32
        WSACleanup();
#endif
        throw std::runtime_error("getsockname failed");
    }
    impl_->active_port.store(ntohs(bound.sin_port), std::memory_order_release);

    using CommandQueue = SPSCQueue<GatewayCommand, QUEUE_SIZE>;
    using EventQueue = SPSCQueue<GatewayEvent, QUEUE_SIZE>;
    auto commands = std::make_unique<CommandQueue>();
    auto events = std::make_unique<EventQueue>();
    GatewayEngineWorker<QUEUE_SIZE> worker(*commands, *events);
    worker.start();

    std::vector<Client> clients;
    clients.reserve(MAX_CLIENTS);
    uint64_t next_session = 1;

    auto queueText = [](Client& client, const std::string& text) {
        if (client.output.size() + text.size() > MAX_OUTPUT) {
            client.close_after_write = true;
            return false;
        }
        client.output += text;
        return true;
    };

    auto drainEvents = [&]() {
        GatewayEvent event{};
        while (events->pop(event)) {
            auto it = std::find_if(clients.begin(), clients.end(), [&](const Client& c) {
                return c.session == event.session;
            });
            if (it == clients.end()) continue; // disconnected client
            if (event.kind == GatewayEventKind::Acknowledgement) {
                const std::string line = "ACK " + std::to_string(event.request_id) +
                    (event.accepted ? " ACCEPTED\n" : " REJECTED\n");
                queueText(*it, line);
            } else {
                const auto& trade = event.trade;
                const std::string line = "TRADE " + std::to_string(event.request_id) +
                    " " + std::to_string(trade.buy_order_id) +
                    " " + std::to_string(trade.sell_order_id) +
                    " " + std::to_string(trade.price) +
                    " " + std::to_string(trade.quantity) + "\n";
                queueText(*it, line);
            }
        }
    };

    auto handleLine = [&](Client& client, std::string_view line) {
        GatewayCommand command{};
        std::string error;
        if (!parseGatewayLine(line, client.session, command, error)) {
            queueText(client, "ERR " + error + "\n");
            return;
        }
        if (command.request_id <= client.last_request) {
            queueText(client, "ERR sequence_not_increasing\n");
            return;
        }
        client.last_request = command.request_id;
        if (!commands->push(command)) {
            queueText(client, "ACK " + std::to_string(command.request_id) +
                              " REJECTED queue_full\n");
        }
    };

    while (!impl_->stopping.load(std::memory_order_acquire)) {
        fd_set reads;
        fd_set writes;
        FD_ZERO(&reads);
        FD_ZERO(&writes);
        FD_SET(listener, &reads);
        SocketHandle max_socket = listener;

        for (const auto& client : clients) {
            if (!client.close_after_write) FD_SET(client.socket, &reads);
            if (!client.output.empty()) FD_SET(client.socket, &writes);
#ifndef _WIN32
            max_socket = std::max(max_socket, client.socket);
#endif
        }

        timeval timeout{};
        timeout.tv_sec = 0;
        timeout.tv_usec = 1'000;
        const int ready = selectSockets(max_socket, &reads, &writes, &timeout);
        if (ready < 0) {
            if (wouldBlock()) continue;
            break;
        }

        if (FD_ISSET(listener, &reads)) {
            for (;;) {
                sockaddr_in peer{};
#ifdef _WIN32
                int peer_size = sizeof(peer);
#else
                socklen_t peer_size = sizeof(peer);
#endif
                SocketHandle accepted = accept(listener,
                    reinterpret_cast<sockaddr*>(&peer), &peer_size);
                if (accepted == INVALID_SOCKET_HANDLE) break;
                if (clients.size() >= MAX_CLIENTS || !makeNonBlocking(accepted)) {
                    closeSocket(accepted);
                    continue;
                }
                clients.push_back(Client{accepted, next_session++, 0, {}, {}, false});
            }
        }

        for (size_t i = 0; i < clients.size();) {
            Client& client = clients[i];
            bool remove = false;

            if (FD_ISSET(client.socket, &reads)) {
                std::array<char, 1024> bytes{};
#ifdef _WIN32
                const int received = recv(client.socket, bytes.data(),
                                          static_cast<int>(bytes.size()), 0);
#else
                const int received = static_cast<int>(recv(client.socket, bytes.data(),
                                                bytes.size(), 0));
#endif
                if (received == 0) remove = true;
                else if (received < 0) {
                    if (!wouldBlock()) remove = true;
                } else {
                    for (int j = 0; j < received && !remove; ++j) {
                        const char ch = bytes[static_cast<size_t>(j)];
                        if (ch == '\n') {
                            handleLine(client, client.input);
                            client.input.clear();
                        } else {
                            client.input.push_back(ch);
                            if (client.input.size() > MAX_LINE) {
                                queueText(client, "ERR line_too_long\n");
                                client.input.clear();
                                client.close_after_write = true;
                            }
                        }
                    }
                }
            }

            if (!remove && FD_ISSET(client.socket, &writes) && !client.output.empty()) {
#ifdef _WIN32
                const int sent = send(client.socket, client.output.data(),
                                      static_cast<int>(client.output.size()), 0);
#else
                const int sent = static_cast<int>(send(client.socket,
                                      client.output.data(), client.output.size(), MSG_NOSIGNAL));
#endif
                if (sent > 0) client.output.erase(0, static_cast<size_t>(sent));
                else if (sent < 0 && !wouldBlock()) remove = true;
            }
            if (!remove && client.close_after_write && client.output.empty()) remove = true;

            if (remove) {
                closeSocket(client.socket);
                clients.erase(clients.begin() + static_cast<std::ptrdiff_t>(i));
            } else {
                ++i;
            }
        }
        drainEvents();
    }

    worker.requestStop();
    while (!worker.finished()) {
        drainEvents();
        std::this_thread::yield();
    }
    worker.join();
    drainEvents();

    for (const auto& client : clients) closeSocket(client.socket);
    closeSocket(listener);
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
