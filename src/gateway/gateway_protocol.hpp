#pragma once

#include "../order.hpp"
#include "../trade.hpp"

#include <array>
#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>

enum class GatewayCommandKind : uint8_t { New, Cancel, Modify };

struct GatewayCommand {
    GatewayCommandKind kind{GatewayCommandKind::New};
    uint64_t session{0};
    uint64_t request_id{0};
    uint64_t order_id{0};
    Order order{};
};

enum class GatewayEventKind : uint8_t { Acknowledgement, Trade };

struct GatewayEvent {
    GatewayEventKind kind{GatewayEventKind::Acknowledgement};
    uint64_t session{0};
    uint64_t request_id{0};
    bool accepted{false};
    Trade trade{};
};

static_assert(std::is_trivially_copyable_v<GatewayCommand>);
static_assert(std::is_trivially_copyable_v<GatewayEvent>);

// Protocol lines are ASCII and space-delimited. Fields:
// NEW seq id B|S L|M price qty
// CANCEL seq id
// MODIFY seq id B|S price qty
inline bool parseGatewayLine(std::string_view line, uint64_t session,
                             GatewayCommand& command, std::string& error) {
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    std::array<std::string_view, 8> fields{};
    size_t count = 0;
    size_t pos = 0;
    while (pos < line.size()) {
        while (pos < line.size() && line[pos] == ' ') ++pos;
        if (pos == line.size()) break;
        const size_t begin = pos;
        while (pos < line.size() && line[pos] != ' ') ++pos;
        if (count == fields.size()) {
            error = "too_many_fields";
            return false;
        }
        fields[count++] = line.substr(begin, pos - begin);
    }

    auto number = [&](size_t index, uint64_t& value) {
        if (index >= count || fields[index].empty()) return false;
        const char* first = fields[index].data();
        const char* last = first + fields[index].size();
        const auto result = std::from_chars(first, last, value);
        return result.ec == std::errc{} && result.ptr == last;
    };
    auto side = [&](size_t index, Side& value) {
        if (index >= count || fields[index].size() != 1) return false;
        if (fields[index][0] == 'B') value = Side::Buy;
        else if (fields[index][0] == 'S') value = Side::Sell;
        else return false;
        return true;
    };

    GatewayCommand parsed{};
    parsed.session = session;
    uint64_t seq = 0;
    uint64_t id = 0;
    uint64_t price = 0;
    uint64_t quantity = 0;

    if (count != 0 && fields[0] == "NEW") {
        Side parsed_side{};
        if (count != 7 || !number(1, seq) || !number(2, id) ||
            !side(3, parsed_side) || !number(5, price) || !number(6, quantity)) {
            error = "invalid_new";
            return false;
        }
        OrderType type;
        if (fields[4] == "L") type = OrderType::Limit;
        else if (fields[4] == "M") type = OrderType::Market;
        else {
            error = "invalid_type";
            return false;
        }
        if (seq == 0 || id == 0 || quantity == 0 ||
            price > UINT32_MAX || quantity > UINT32_MAX ||
            (type == OrderType::Limit && (price == 0 || price >= 65536)) ||
            (type == OrderType::Market && price != 0)) {
            error = "out_of_range";
            return false;
        }
        parsed.kind = GatewayCommandKind::New;
        parsed.request_id = seq;
        parsed.order_id = id;
        parsed.order = Order{id, parsed_side, type,
                             static_cast<uint32_t>(price),
                             static_cast<uint32_t>(quantity), 0};
    } else if (count != 0 && fields[0] == "CANCEL") {
        if (count != 3 || !number(1, seq) || !number(2, id) || seq == 0 || id == 0) {
            error = "invalid_cancel";
            return false;
        }
        parsed.kind = GatewayCommandKind::Cancel;
        parsed.request_id = seq;
        parsed.order_id = id;
    } else if (count != 0 && fields[0] == "MODIFY") {
        Side parsed_side{};
        if (count != 6 || !number(1, seq) || !number(2, id) ||
            !side(3, parsed_side) || !number(4, price) || !number(5, quantity) ||
            seq == 0 || id == 0 || price == 0 || price >= 65536 ||
            quantity == 0 || quantity > UINT32_MAX) {
            error = "invalid_modify";
            return false;
        }
        parsed.kind = GatewayCommandKind::Modify;
        parsed.request_id = seq;
        parsed.order_id = id;
        parsed.order = Order{id, parsed_side, OrderType::Limit,
                             static_cast<uint32_t>(price),
                             static_cast<uint32_t>(quantity), 0};
    } else {
        error = "unknown_command";
        return false;
    }

    command = parsed;
    error.clear();
    return true;
}
