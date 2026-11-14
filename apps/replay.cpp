// Replays an order file through one order book per symbol and prints every trade
// plus the final state of each book.
//
// File format (CSV, one command per line, prices in cents, '#' starts a comment):
//   SYMBOL,NEW,id,BUY|SELL,LIMIT|MARKET|IOC,price,quantity
//   SYMBOL,CANCEL,id
//   SYMBOL,MODIFY,id,price,quantity

#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "orderbook/order_book.hpp"

namespace {

using namespace ob;

std::vector<std::string> split(const std::string& line) {
    std::vector<std::string> fields;
    std::stringstream stream(line);
    std::string field;
    while (std::getline(stream, field, ',')) {
        // trim spaces and a trailing \r from Windows line endings
        const auto first = field.find_first_not_of(" \t\r");
        const auto last = field.find_last_not_of(" \t\r");
        fields.push_back(first == std::string::npos ? "" : field.substr(first, last - first + 1));
    }
    return fields;
}

std::string money(Price cents) {
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%lld.%02lld", static_cast<long long>(cents / 100),
                  static_cast<long long>(cents % 100));
    return buffer;
}

const char* reject_name(Reject reject) {
    switch (reject) {
        case Reject::None: return "none";
        case Reject::InvalidQuantity: return "invalid quantity";
        case Reject::InvalidPrice: return "invalid price";
        case Reject::DuplicateId: return "duplicate id";
        case Reject::UnknownOrder: return "unknown order";
    }
    return "?";
}

void print_result(const std::string& symbol, OrderId id, const SubmitResult& result) {
    if (!result.accepted()) {
        std::cout << "REJECT " << symbol << " #" << id << " (" << reject_name(result.reject) << ")\n";
        return;
    }
    for (const Trade& t : result.trades) {
        std::cout << "TRADE  " << symbol << "  " << t.quantity << " @ " << money(t.price)
                  << "  (buy #" << t.buy_id << ", sell #" << t.sell_id << ")\n";
    }
}

void print_book(const std::string& symbol, const OrderBook& book) {
    std::cout << "\n" << symbol << "\n";
    std::cout << "        BID            ASK\n";
    const auto bids = book.depth(Side::Buy, 5);
    const auto asks = book.depth(Side::Sell, 5);
    const std::size_t rows = bids.size() > asks.size() ? bids.size() : asks.size();
    for (std::size_t i = 0; i < rows; ++i) {
        char line[96];
        char bid[40] = "";
        char ask[40] = "";
        if (i < bids.size()) {
            std::snprintf(bid, sizeof bid, "%6llu @ %s", static_cast<unsigned long long>(bids[i].quantity),
                          money(bids[i].price).c_str());
        }
        if (i < asks.size()) {
            std::snprintf(ask, sizeof ask, "%6llu @ %s", static_cast<unsigned long long>(asks[i].quantity),
                          money(asks[i].price).c_str());
        }
        std::snprintf(line, sizeof line, "  %-16s %s", bid, ask);
        std::cout << line << "\n";
    }
    if (rows == 0) std::cout << "  (empty)\n";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: replay <orders.csv>\n";
        return 2;
    }
    std::ifstream file(argv[1]);
    if (!file) {
        std::cerr << "cannot open " << argv[1] << "\n";
        return 1;
    }

    std::map<std::string, OrderBook> books;
    std::string line;
    int line_number = 0;
    int errors = 0;

    while (std::getline(file, line)) {
        ++line_number;
        const auto fields = split(line);
        if (fields.empty() || fields[0].empty() || fields[0][0] == '#') continue;

        try {
            const std::string& symbol = fields.at(0);
            const std::string& action = fields.at(1);
            const OrderId id = std::stoull(fields.at(2));
            OrderBook& book = books[symbol];

            if (action == "NEW") {
                Order order;
                order.id = id;
                order.side = fields.at(3) == "BUY" ? Side::Buy : Side::Sell;
                const std::string& kind = fields.at(4);
                order.type = kind == "MARKET" ? OrderType::Market : OrderType::Limit;
                order.tif = kind == "IOC" ? TimeInForce::IOC : TimeInForce::GTC;
                order.price = std::stoll(fields.at(5));
                order.quantity = std::stoull(fields.at(6));
                print_result(symbol, id, book.submit(order));
            } else if (action == "CANCEL") {
                std::cout << (book.cancel(id) ? "CANCEL " : "CANCEL (unknown) ") << symbol << " #" << id << "\n";
            } else if (action == "MODIFY") {
                print_result(symbol, id, book.modify(id, std::stoll(fields.at(3)), std::stoull(fields.at(4))));
            } else {
                throw std::invalid_argument("unknown action " + action);
            }
        } catch (const std::exception& e) {
            std::cerr << "line " << line_number << ": " << e.what() << "\n";
            ++errors;
        }
    }

    for (const auto& [symbol, book] : books) print_book(symbol, book);
    return errors == 0 ? 0 : 1;
}
