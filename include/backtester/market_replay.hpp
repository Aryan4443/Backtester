#pragma once
#include "types.hpp"
#include "event_queue.hpp"
#include <fstream>
#include <string>
#include <sstream>
#include <stdexcept>

// ============================================================
// MarketReplay — reads recorded L2/L3 events from a CSV file
// and loads them into a DeterministicEventQueue.
//
// CSV format (one event per line):
//   timestamp_ns,event_type,order_id,side,price,quantity
//
//   event_type: ADD | MOD | DEL | TRD
//   side:       B | S          (ignored for TRD)
//   price:      integer cents
//   quantity:   integer shares (0 for DEL)
//
// Example:
//   1000000000,ADD,1,B,9950,100
//   1000000001,ADD,2,S,10000,100
//   1000050000,TRD,0,S,9950,50     # aggressor=SELL, took bids
//   1000100000,DEL,1,B,9950,0
//
// The replay guarantees events are enqueued in timestamp order
// because DeterministicEventQueue sorts them regardless.
// ============================================================

namespace bt {

class MarketReplay {
public:
    // Load a CSV file into the queue.  Returns number of events loaded.
    static std::size_t load_csv(const std::string& path,
                                DeterministicEventQueue& queue)
    {
        std::ifstream f(path);
        if (!f.is_open())
            throw std::runtime_error("MarketReplay: cannot open " + path);

        std::size_t count = 0;
        std::string line;
        uint64_t seq = 0;

        while (std::getline(f, line)) {
            if (line.empty() || line[0] == '#') continue;
            Event e = parse_line(line, seq++);
            queue.push(e);
            ++count;
        }
        return count;
    }

    // Convenience: load from a vector of strings (for tests / synthetic data).
    static std::size_t load_lines(const std::vector<std::string>& lines,
                                  DeterministicEventQueue& queue)
    {
        std::size_t count = 0;
        uint64_t seq = 0;
        for (auto& line : lines) {
            if (line.empty() || line[0] == '#') continue;
            Event e = parse_line(line, seq++);
            queue.push(e);
            ++count;
        }
        return count;
    }

private:
    static Event parse_line(const std::string& line, uint64_t seq) {
        std::istringstream ss(line);
        std::string token;
        std::vector<std::string> fields;
        while (std::getline(ss, token, ','))
            fields.push_back(token);

        if (fields.size() < 6)
            throw std::runtime_error("MarketReplay: malformed line: " + line);

        Event e;
        e.ts  = std::stoll(fields[0]);
        e.seq = seq;
        // det_id will be assigned by DeterministicEventQueue::push()

        const std::string& type = fields[1];
        Side side = (fields[3] == "B") ? Side::BUY : Side::SELL;

        if (type == "ADD") {
            e.kind = EventKind::MARKET_ADD;
            e.payload = MarketOrderPayload{
                static_cast<OrderId>(std::stoull(fields[2])),
                side,
                std::stoll(fields[4]),
                std::stoll(fields[5])
            };
        } else if (type == "MOD") {
            e.kind = EventKind::MARKET_MODIFY;
            e.payload = MarketOrderPayload{
                static_cast<OrderId>(std::stoull(fields[2])),
                side,
                std::stoll(fields[4]),
                std::stoll(fields[5])
            };
        } else if (type == "DEL") {
            e.kind = EventKind::MARKET_DELETE;
            e.payload = MarketOrderPayload{
                static_cast<OrderId>(std::stoull(fields[2])),
                side,
                std::stoll(fields[4]),
                0
            };
        } else if (type == "TRD") {
            e.kind = EventKind::MARKET_TRADE;
            e.payload = TradePayload{
                std::stoll(fields[4]),
                std::stoll(fields[5]),
                side   // side field = aggressor side for trades
            };
        } else {
            throw std::runtime_error("MarketReplay: unknown event type: " + type);
        }

        return e;
    }
};

} // namespace bt
