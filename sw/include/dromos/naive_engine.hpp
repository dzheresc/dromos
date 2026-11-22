// NaiveEngine: a deliberately simple second model (std::map of std::deque)
// used only to cross-check RefEngine. It shares the message builders and the
// slot-allocation rule (FIFO free list, generation bump on free), because
// those are part of the spec, but none of RefEngine's book data structures.
#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <vector>

#include "dromos/common.hpp"

namespace dromos {

class NaiveEngine {
public:
    explicit NaiveEngine(const EngineConfig& cfg = {});
    void configure_symbol(std::uint16_t sym, std::uint32_t base_ticks);
    void on_message(const Ingress& in, OutSink& out);

    std::optional<std::uint32_t> best_bid(std::uint16_t sym) const;
    std::optional<std::uint32_t> best_ask(std::uint16_t sym) const;
    std::uint32_t live_orders() const { return live_; }

private:
    struct Order {
        std::uint64_t client_id = 0;
        std::uint32_t price = 0;
        std::uint32_t qty = 0;
        std::uint16_t symbol = 0;
        std::uint16_t session = 0;
        std::uint8_t side = 0;
        std::uint8_t gen = 0;
        bool live = false;
    };
    // price -> FIFO of slots
    using Side = std::map<std::uint32_t, std::deque<std::uint32_t>>;
    struct Symbol {
        bool enabled = false;
        std::uint32_t base = 0;
        Side bids, asks;
    };

    void on_new(const Ingress& in, OutSink& out);
    void on_cancel(const Ingress& in, OutSink& out);
    void on_reduce(const Ingress& in, OutSink& out);
    void remove_from_book(std::uint32_t slot);
    std::optional<std::uint32_t> find(proto::Handle h, std::uint16_t sym, std::uint16_t session) const;
    void free_slot(std::uint32_t slot);

    EngineConfig cfg_;
    std::vector<Symbol> syms_;
    std::vector<Order> pool_;
    std::deque<std::uint32_t> free_;
    std::uint32_t live_ = 0;
    std::uint32_t match_id_ = 0;
};

}  // namespace dromos
