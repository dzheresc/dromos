// orderflow: seeded order-flow generator.
//
// Closed loop: it learns order handles from the engine's Accepted messages
// (observe()), so cancels and reduces mostly hit live orders. A seed plus the
// engine's own output fully defines a run.
//
// TODO(hw): the RTL generator on the 325Ts will see responses with wire delay,
// so a closed-loop stream is timing dependent. Plan: the 325T logs the exact
// stream it sent (CRC + optional DDR capture on 325T-2) and refengine replays
// that stream, or the generator only cancels handles older than a fixed lag.
#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "dromos/common.hpp"

namespace dromos {

struct FlowConfig {
    std::uint64_t seed = 1;
    std::uint32_t num_symbols = 4;
    std::uint32_t levels = 4096;      // must match the engine
    std::uint32_t base_ticks = 10000; // same window base for every symbol
    std::uint16_t sessions = 2;       // session ids 1..sessions
    std::uint32_t max_qty = 500;
    // Mix, in parts per 1024 (remainder = passive adds).
    std::uint32_t p_cancel = 330;
    std::uint32_t p_cross = 80;
    std::uint32_t p_reduce = 30;
    std::uint32_t p_ioc = 200;   // of crossing orders
    std::uint32_t p_bad = 8;     // malformed or out-of-window messages
    std::uint32_t p_stale = 20;  // of cancels: use a dead or foreign handle
    std::uint32_t ts_step_ns = 7;
};

class OrderFlow {
public:
    explicit OrderFlow(const FlowConfig& cfg);

    // The next inbound message, as the ingress arbiter would deliver it.
    Ingress next();

    // Feed every engine output back so the generator can track live handles.
    void observe(Dest dest, const proto::Msg& msg);

    std::size_t live() const { return live_.size(); }

private:
    struct Live {
        proto::Handle h;
        std::uint16_t symbol;
        std::uint16_t session;
        std::uint32_t qty;
    };

    proto::Msg make_add(std::uint16_t sym, bool cross, std::uint16_t& session);
    void forget(proto::Handle h);

    FlowConfig cfg_;
    Rng rng_;
    std::uint64_t seq_ = 0;
    std::uint64_t next_cl_id_ = 1;
    std::vector<std::uint32_t> mid_;  // per symbol, in levels
    std::vector<Live> live_;
    std::unordered_map<proto::Handle, std::size_t> index_;
    std::vector<proto::Handle> dead_;  // recently freed handles, for stale cancels
    proto::Handle last_accepted_ = ~proto::Handle(0);
    std::uint16_t last_session_ = 0;
};

}  // namespace dromos
