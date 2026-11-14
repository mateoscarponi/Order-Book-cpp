// Line-based front end for one order book, used by the web demo (demo/server.py).
// Reads one command per line on stdin and answers with one JSON line on stdout.
//
// Commands (prices in ticks/cents):
//   NEW BUY|SELL LIMIT|MARKET|IOC <price> <quantity>   -> the engine assigns the id
//   CANCEL <id>
//   MODIFY <id> <price> <quantity>
//   BOOK                                               -> snapshot only
//   RESET                                              -> empty book, ids restart at 1
//
// Every answer carries the result of the command plus a full book snapshot.

#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "orderbook/order_book.hpp"

namespace {

using namespace ob;

constexpr std::size_t kDepthLevels = 12;

const char* side_name(Side side) { return side == Side::Buy ? "BUY" : "SELL"; }

const char* reject_name(Reject reject) {
    switch (reject) {
        case Reject::None: return "";
        case Reject::InvalidQuantity: return "invalid quantity";
        case Reject::InvalidPrice: return "invalid price";
        case Reject::DuplicateId: return "duplicate id";
        case Reject::UnknownOrder: return "unknown order";
    }
    return "unknown";
}

void write_side(std::ostream& out, const OrderBook& book, Side side) {
    out << '[';
    const auto levels = book.depth(side, kDepthLevels);
    for (std::size_t i = 0; i < levels.size(); ++i) {
        const LevelView& level = levels[i];
        if (i) out << ',';
        out << "{\"price\":" << level.price << ",\"qty\":" << level.quantity << ",\"orders\":[";
        const auto queue = book.orders_at(side, level.price);
        for (std::size_t j = 0; j < queue.size(); ++j) {
            if (j) out << ',';
            out << '[' << queue[j].id << ',' << queue[j].remaining << ']';
        }
        out << "]}";
    }
    out << ']';
}

void write_response(std::ostream& out, const OrderBook& book, bool ok, const std::string& error,
                    const std::string& action, OrderId id, const SubmitResult* result) {
    out << "{\"ok\":" << (ok ? "true" : "false") << ",\"error\":\"" << error << "\",\"action\":\"" << action
        << "\",\"id\":" << id << ",\"trades\":[";
    if (result) {
        for (std::size_t i = 0; i < result->trades.size(); ++i) {
            const Trade& t = result->trades[i];
            if (i) out << ',';
            out << "{\"buy\":" << t.buy_id << ",\"sell\":" << t.sell_id << ",\"price\":" << t.price
                << ",\"qty\":" << t.quantity << ",\"aggressor\":\"" << side_name(t.aggressor) << "\"}";
        }
    }
    out << "],\"resting\":" << (result ? result->resting : 0) << ",\"orders\":" << book.order_count()
        << ",\"bids\":";
    write_side(out, book, Side::Buy);
    out << ",\"asks\":";
    write_side(out, book, Side::Sell);
    out << "}\n" << std::flush;
}

}  // namespace

int main() {
    std::ios::sync_with_stdio(false);
    OrderBook book;
    OrderId next_id = 1;
    std::string line;

    while (std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::istringstream in(line);
        std::string command;
        in >> command;
        if (command.empty()) continue;

        if (command == "NEW") {
            std::string side;
            std::string kind;
            long long price = 0;
            long long quantity = 0;
            if (!(in >> side >> kind >> price >> quantity) || (side != "BUY" && side != "SELL") ||
                (kind != "LIMIT" && kind != "MARKET" && kind != "IOC") || quantity < 0) {
                write_response(std::cout, book, false, "usage: NEW BUY|SELL LIMIT|MARKET|IOC price qty", "NEW", 0,
                               nullptr);
                continue;
            }
            Order order;
            order.id = next_id++;
            order.side = side == "BUY" ? Side::Buy : Side::Sell;
            order.type = kind == "MARKET" ? OrderType::Market : OrderType::Limit;
            order.tif = kind == "IOC" ? TimeInForce::IOC : TimeInForce::GTC;
            order.price = price;
            order.quantity = static_cast<Quantity>(quantity);
            const SubmitResult result = book.submit(order);
            write_response(std::cout, book, result.accepted(), reject_name(result.reject), "NEW", order.id, &result);
        } else if (command == "CANCEL") {
            OrderId id = 0;
            if (!(in >> id)) {
                write_response(std::cout, book, false, "usage: CANCEL id", "CANCEL", 0, nullptr);
                continue;
            }
            const bool ok = book.cancel(id);
            write_response(std::cout, book, ok, ok ? "" : "unknown order", "CANCEL", id, nullptr);
        } else if (command == "MODIFY") {
            OrderId id = 0;
            long long price = 0;
            long long quantity = 0;
            if (!(in >> id >> price >> quantity) || quantity < 0) {
                write_response(std::cout, book, false, "usage: MODIFY id price qty", "MODIFY", 0, nullptr);
                continue;
            }
            const SubmitResult result = book.modify(id, price, static_cast<Quantity>(quantity));
            write_response(std::cout, book, result.accepted(), reject_name(result.reject), "MODIFY", id, &result);
        } else if (command == "BOOK") {
            write_response(std::cout, book, true, "", "BOOK", 0, nullptr);
        } else if (command == "RESET") {
            book = OrderBook{};
            next_id = 1;
            write_response(std::cout, book, true, "", "RESET", 0, nullptr);
        } else {
            write_response(std::cout, book, false, "unknown command", "?", 0, nullptr);
        }
    }
    return 0;
}
