// synth_gen.cpp — Synthetic L2 market data generator.
//
// Generates a realistic price walk with:
//   - Midprice following a geometric random walk
//   - Bid/ask spread modeled as a minimum tick
//   - 5 levels of depth on each side
//   - Occasional large aggressive trades that walk the book
//   - All events in strict timestamp order
//
// Output: CSV to stdout, suitable for MarketReplay::load_csv()
// Usage:  ./synth_gen [num_events] [seed] > data.csv

#include <cstdint>
#include <iostream>
#include <iomanip>
#include <random>
#include <vector>
#include <cmath>
#include <string>

int main(int argc, char** argv) {
    int    num_events = (argc > 1) ? std::stoi(argv[1]) : 5000;
    uint64_t seed     = (argc > 2) ? std::stoull(argv[2]) : 42;

    std::mt19937_64 rng(seed);

    // Price parameters (in cents)
    double  mid      = 10000.0;  // $100.00
    double  vol      = 0.0002;   // per-tick volatility (relative)
    int64_t tick     = 1;        // min price increment: $0.01
    int64_t spread   = 2;        // 2 cents bid-ask spread
    int     depth    = 5;        // levels per side

    std::normal_distribution<double> price_step(0.0, 1.0);
    std::uniform_int_distribution<int64_t> size_dist(10, 200);
    std::uniform_real_distribution<double> uniform(0.0, 1.0);

    // We track resting order IDs
    uint64_t next_order_id = 1;
    int64_t  ts = 1'000'000'000LL;  // start at 1 second (nanoseconds)

    // Vectors of currently resting bid/ask order IDs per level
    struct BookEntry { uint64_t order_id; int64_t price; int64_t qty; };
    std::vector<BookEntry> bid_book, ask_book;

    std::cout << "# Synthetic L2 data\n";
    std::cout << "# Format: timestamp_ns,event_type,order_id,side,price,quantity\n";

    auto emit = [&](const std::string& type, uint64_t oid,
                    char side, int64_t price, int64_t qty) {
        std::cout << ts << ',' << type << ',' << oid << ','
                  << side << ',' << price << ',' << qty << '\n';
        ts += 1000 + (rng() % 5000);  // 1–6µs between events
    };

    int events = 0;

    // Seed the initial book with depth levels on each side
    auto rebuild_book = [&]() {
        // Cancel all existing
        for (auto& b : bid_book) emit("DEL", b.order_id, 'B', b.price, 0);
        for (auto& a : ask_book) emit("DEL", a.order_id, 'S', a.price, 0);
        bid_book.clear();
        ask_book.clear();

        int64_t mid_c = static_cast<int64_t>(std::round(mid));
        int64_t best_bid = mid_c - spread / 2;
        int64_t best_ask = best_bid + spread;

        for (int i = 0; i < depth; ++i) {
            int64_t bp = best_bid - i * tick;
            int64_t qty = size_dist(rng);
            uint64_t oid = next_order_id++;
            emit("ADD", oid, 'B', bp, qty);
            bid_book.push_back({oid, bp, qty});
            events++;

            int64_t ap = best_ask + i * tick;
            qty = size_dist(rng);
            oid = next_order_id++;
            emit("ADD", oid, 'S', ap, qty);
            ask_book.push_back({oid, ap, qty});
            events++;
        }
    };

    rebuild_book();

    while (events < num_events) {
        // 1. Advance mid price (geometric random walk)
        mid *= std::exp(vol * price_step(rng));
        mid  = std::round(mid);

        // 2. With 60% probability: small book update (add/delete at outer level)
        //    With 30% probability: trade
        //    With 10% probability: full book rebuild (simulates a large move)

        double u = uniform(rng);

        if (u < 0.60) {
            // Small update: replace outermost level on one side
            bool is_bid = (uniform(rng) < 0.5);
            if (is_bid && !bid_book.empty()) {
                auto& entry = bid_book.back();
                emit("DEL", entry.order_id, 'B', entry.price, 0);
                int64_t new_qty = size_dist(rng);
                entry.order_id = next_order_id++;
                emit("ADD", entry.order_id, 'B', entry.price, new_qty);
                entry.qty = new_qty;
                events += 2;
            } else if (!ask_book.empty()) {
                auto& entry = ask_book.back();
                emit("DEL", entry.order_id, 'S', entry.price, 0);
                int64_t new_qty = size_dist(rng);
                entry.order_id = next_order_id++;
                emit("ADD", entry.order_id, 'S', entry.price, new_qty);
                entry.qty = new_qty;
                events += 2;
            }

        } else if (u < 0.90) {
            // Trade: aggressive buyer or seller hits the best level
            bool buy_aggressor = (uniform(rng) < 0.5);
            int64_t trade_qty = size_dist(rng) / 3 + 1;

            if (buy_aggressor && !ask_book.empty()) {
                int64_t tp = ask_book.front().price;
                int64_t available = ask_book.front().qty;
                int64_t traded = std::min(trade_qty, available);
                emit("TRD", 0, 'B', tp, traded);  // aggressor = BUY
                ask_book.front().qty -= traded;
                if (ask_book.front().qty <= 0) {
                    emit("DEL", ask_book.front().order_id, 'S', tp, 0);
                    ask_book.erase(ask_book.begin());
                    // Replace with new level
                    int64_t new_price = tp;
                    int64_t new_qty   = size_dist(rng);
                    uint64_t oid      = next_order_id++;
                    emit("ADD", oid, 'S', new_price, new_qty);
                    ask_book.insert(ask_book.begin(), {oid, new_price, new_qty});
                    events++;
                }
                events += 2;
            } else if (!buy_aggressor && !bid_book.empty()) {
                int64_t tp = bid_book.front().price;
                int64_t available = bid_book.front().qty;
                int64_t traded = std::min(trade_qty, available);
                emit("TRD", 0, 'S', tp, traded);  // aggressor = SELL
                bid_book.front().qty -= traded;
                if (bid_book.front().qty <= 0) {
                    emit("DEL", bid_book.front().order_id, 'B', tp, 0);
                    bid_book.erase(bid_book.begin());
                    int64_t new_price = tp;
                    int64_t new_qty   = size_dist(rng);
                    uint64_t oid      = next_order_id++;
                    emit("ADD", oid, 'B', new_price, new_qty);
                    bid_book.insert(bid_book.begin(), {oid, new_price, new_qty});
                    events++;
                }
                events += 2;
            }

        } else {
            // Full rebuild after a large price move
            rebuild_book();
        }
    }

    return 0;
}
