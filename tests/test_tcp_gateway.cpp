#include "gateway/tcp_server.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <exception>
#include <string>
#include <thread>

#ifdef _WIN32
#  include <winsock2.h>
using TestSocket = SOCKET;
static constexpr TestSocket INVALID_TEST_SOCKET = INVALID_SOCKET;
#else
#  include <arpa/inet.h>
#  include <sys/select.h>
#  include <sys/socket.h>
#  include <unistd.h>
using TestSocket = int;
static constexpr TestSocket INVALID_TEST_SOCKET = -1;
#endif

namespace {

static void closeTestSocket(TestSocket socket) {
#ifdef _WIN32
    closesocket(socket);
#else
    close(socket);
#endif
}

static TestSocket connectTo(uint16_t port) {
    TestSocket socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket == INVALID_TEST_SOCKET) return INVALID_TEST_SOCKET;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        closeTestSocket(socket);
        return INVALID_TEST_SOCKET;
    }
#ifdef _WIN32
    DWORD timeout = 2000;
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
    timeval timeout{2, 0};
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#endif
    return socket;
}

static bool sendLine(TestSocket socket, const std::string& line) {
    size_t sent_total = 0;
    while (sent_total < line.size()) {
#ifdef _WIN32
        const int sent = send(socket, line.data() + sent_total,
                              static_cast<int>(line.size() - sent_total), 0);
#else
        const int sent = static_cast<int>(send(socket, line.data() + sent_total,
                                  line.size() - sent_total, MSG_NOSIGNAL));
#endif
        if (sent <= 0) return false;
        sent_total += static_cast<size_t>(sent);
    }
    return true;
}

static bool readLine(TestSocket socket, std::string& line) {
    line.clear();
    while (line.size() < 256) {
        char ch = 0;
        const int count = recv(socket, &ch, 1, 0);
        if (count != 1) return false;
        if (ch == '\n') return true;
        line.push_back(ch);
    }
    return false;
}

} // namespace

TEST(TcpGateway, TwoClientsSendOrdersThroughQueueToEngineAndReceiveTrade) {
    TcpGatewayServer server(0);
    std::exception_ptr server_error;
    std::thread server_thread([&] {
        try { server.run(); }
        catch (...) { server_error = std::current_exception(); }
    });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (server.boundPort() == 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    if (server.boundPort() == 0) {
        server.requestStop();
        server_thread.join();
        if (server_error) std::rethrow_exception(server_error);
        FAIL() << "TCP server did not bind an ephemeral port";
    }

    TestSocket seller = connectTo(server.boundPort());
    TestSocket buyer = connectTo(server.boundPort());
    std::string seller_ack;
    std::string buyer_ack;
    std::string trade;
    bool exchanged = seller != INVALID_TEST_SOCKET && buyer != INVALID_TEST_SOCKET;
    if (exchanged) {
        exchanged = sendLine(seller, "NEW 1 101 S L 100 5\n") &&
                    readLine(seller, seller_ack);
        closeTestSocket(seller);
        seller = INVALID_TEST_SOCKET;
        exchanged = exchanged && sendLine(buyer, "NEW 1 102 B L 100 5\n") &&
                    readLine(buyer, buyer_ack) && readLine(buyer, trade);
    }

    if (seller != INVALID_TEST_SOCKET) closeTestSocket(seller);
    if (buyer != INVALID_TEST_SOCKET) closeTestSocket(buyer);
    server.requestStop();
    server_thread.join();
    if (server_error) std::rethrow_exception(server_error);
    EXPECT_TRUE(exchanged);
    EXPECT_EQ(seller_ack, "ACK 1 ACCEPTED");
    EXPECT_EQ(buyer_ack, "ACK 1 ACCEPTED");
    EXPECT_EQ(trade, "TRADE 1 102 101 100 5");
}
