#pragma once

#include "gateway_protocol.hpp"
#include "../concurrency/spsc_queue.hpp"
#include "../matching_engine.hpp"

#include <atomic>
#include <cstddef>
#include <thread>
#include <vector>

template <size_t QueueSize>
class GatewayEngineWorker {
public:
    using CommandQueue = SPSCQueue<GatewayCommand, QueueSize>;
    using EventQueue = SPSCQueue<GatewayEvent, QueueSize>;

    GatewayEngineWorker(CommandQueue& commands, EventQueue& events)
        : commands_(commands), events_(events) {}

    GatewayEngineWorker(const GatewayEngineWorker&) = delete;
    GatewayEngineWorker& operator=(const GatewayEngineWorker&) = delete;

    ~GatewayEngineWorker() {
        requestStop();
        if (thread_.joinable()) thread_.join();
    }

    void start() {
        stopping_.store(false, std::memory_order_release);
        finished_.store(false, std::memory_order_release);
        thread_ = std::thread([this] { run(); });
    }

    void requestStop() noexcept {
        stopping_.store(true, std::memory_order_release);
    }

    bool finished() const noexcept {
        return finished_.load(std::memory_order_acquire);
    }

    void join() {
        if (thread_.joinable()) thread_.join();
    }

private:
    void publish(const GatewayEvent& event) {
        while (!events_.push(event)) std::this_thread::yield();
    }

    void run() {
        GatewayCommand command{};
        while (!stopping_.load(std::memory_order_acquire) || !commands_.empty()) {
            if (!commands_.pop(command)) {
                std::this_thread::yield();
                continue;
            }

            std::vector<Trade> trades;
            bool accepted = false;
            switch (command.kind) {
            case GatewayCommandKind::New:
                accepted = engine_.tryProcess(command.order, trades);
                break;
            case GatewayCommandKind::Cancel:
                accepted = engine_.cancelOrder(command.order_id);
                break;
            case GatewayCommandKind::Modify:
                accepted = engine_.modifyOrder(command.order_id,
                    command.order.side, command.order.price,
                    command.order.quantity, trades);
                break;
            }

            publish(GatewayEvent{GatewayEventKind::Acknowledgement,
                                 command.session, command.request_id,
                                 accepted, Trade{}});
            for (const auto& trade : trades) {
                publish(GatewayEvent{GatewayEventKind::Trade,
                                     command.session, command.request_id,
                                     true, trade});
            }
        }
        finished_.store(true, std::memory_order_release);
    }

    CommandQueue& commands_;
    EventQueue& events_;
    MatchingEngine engine_;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> finished_{false};
    std::thread thread_;
};
