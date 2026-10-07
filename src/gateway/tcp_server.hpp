#pragma once

#include <cstdint>
#include <memory>

class TcpGatewayServer {
public:
    explicit TcpGatewayServer(uint16_t port);
    ~TcpGatewayServer();

    TcpGatewayServer(const TcpGatewayServer&) = delete;
    TcpGatewayServer& operator=(const TcpGatewayServer&) = delete;

    int run();
    void requestStop() noexcept;
    uint16_t boundPort() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
