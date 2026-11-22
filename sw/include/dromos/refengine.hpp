// refengine: the C++ reference matching engine and the specification for the RTL.
//
// It uses the same structures as the hardware book core:
//   - per symbol and side, a tick-indexed level table (head, tail, qty, count)
//   - a two-level bitmap of non-empty levels for best-price search
//   - a fixed order pool, each level a doubly linked FIFO through the pool
//   - a FIFO free list of pool slots (pop on add, push on free)
// Given the same Ingress stream it must produce byte-identical output to the RTL.
#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "dromos/common.hpp"

namespace dromos {

class RefEngine {
public:
    explicit RefEngine(const EngineConfig& cfg = {});

    // Enable a symbol with its price window [base, base + levels).
    void configure_symbol(std::uint16_t sym, std::uint32_t base_ticks);

    void on_message(const Ingress& in, OutSink& out);

    // Introspection for tests and the harness.
    std::optional<std::uint32_t> best_bid(std::uint16_t sym) const;
    std::optional<std::uint32_t> best_ask(std::uint16_t sym) const;
    std::uint32_t qty_at(std::uint16_t sym, proto::Side side, std::uint32_t price) const;
    std::uint32_t live_orders() const { return live_; }
    const EngineConfig& config() const { return cfg_; }

private:
    static constexpr std::int32_t kNil = -1;

    struct Level {
        std::int32_t head = kNil;
        std::int32_t tail = kNil;
        std::uint32_t qty = 0;
        std::uint32_t count = 0;
    };

    struct Book {  // one side of one symbol
        std::vector<Level> levels;
        std::vector<std::uint64_t> words;  // levels / 64
        std::uint64_t summary = 0;         // bit w set <=> words[w] != 0
    };

    struct Symbol {
        bool enabled = false;
        std::uint32_t base = 0;
        Book side[2];  // [Buy, Sell]
    };

    struct Order {
        std::uint64_t client_id = 0;
        std::uint32_t qty = 0;  // leaves
        std::uint32_t level = 0;
        std::int32_t prev = kNil;
        std::int32_t next = kNil;
        std::uint16_t symbol = 0;
        std::uint16_t session = 0;
        std::uint8_t side = 0;
        std::uint8_t gen = 0;
        bool live = false;
    };

    void on_new(const Ingress& in, OutSink& out);
    void on_cancel(const Ingress& in, OutSink& out);
    void on_reduce(const Ingress& in, OutSink& out);

    // Free list.
    std::int32_t alloc();
    void release(std::int32_t slot);

    // Level FIFO.
    void link_tail(Book& b, std::int32_t slot);
    void unlink(Book& b, std::int32_t slot);

    // Bitmap.
    static void set_bit(Book& b, std::uint32_t lvl);
    static void clear_bit(Book& b, std::uint32_t lvl);
    static std::optional<std::uint32_t> lowest(const Book& b);
    static std::optional<std::uint32_t> highest(const Book& b);

    // Resolve an inbound handle to a live slot owned by this session/symbol.
    std::int32_t lookup(proto::Handle h, std::uint16_t sym, std::uint16_t session) const;

    EngineConfig cfg_;
    std::vector<Symbol> syms_;
    std::vector<Order> pool_;
    std::vector<std::uint32_t> free_;  // ring buffer
    std::uint32_t free_head_ = 0;
    std::uint32_t free_count_ = 0;
    std::uint32_t live_ = 0;
    std::uint32_t match_id_ = 0;
};

}  // namespace dromos
