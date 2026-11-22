#include "dromos/orderflow.hpp"

#include <algorithm>

namespace dromos {

using namespace proto;

OrderFlow::OrderFlow(const FlowConfig& cfg) : cfg_(cfg), rng_(cfg.seed) {
    mid_.assign(cfg_.num_symbols, cfg_.levels / 2);
}

Msg OrderFlow::make_add(std::uint16_t sym, bool cross, std::uint16_t& session) {
    auto m = blank<NewOrder>(MsgType::NewOrder);
    const bool buy = rng_.below(2) == 0;
    m.side = buy ? Side::Buy : Side::Sell;
    m.symbol = sym;
    // Distance from mid: mostly near the touch, geometric-ish tail.
    std::uint32_t dist = 1 + rng_.below(4);
    while (rng_.chance(300) && dist < 200) dist += 1 + rng_.below(8);
    std::int64_t lvl = mid_[sym];
    if (cross)
        lvl += buy ? std::int64_t(dist) : -std::int64_t(dist);  // through the spread
    else
        lvl += buy ? -std::int64_t(dist) : std::int64_t(dist);  // passive
    lvl = std::clamp<std::int64_t>(lvl, 0, cfg_.levels - 1);
    m.price_ticks = cfg_.base_ticks + std::uint32_t(lvl);
    m.qty = 1 + rng_.below(cfg_.max_qty);
    m.tif = (cross && rng_.chance(cfg_.p_ioc)) ? Tif::Ioc : Tif::Day;
    m.client_order_id = next_cl_id_++;
    session = std::uint16_t(1 + rng_.below(cfg_.sessions));
    return Msg::from(m);
}

Ingress OrderFlow::next() {
    Ingress in;
    in.seq = ++seq_;
    in.ts_ns = seq_ * cfg_.ts_step_ns;

    // Mid price random walk, one symbol at a time, kept away from the edges.
    const std::uint16_t sym = std::uint16_t(rng_.below(cfg_.num_symbols));
    if (rng_.chance(64)) {
        std::uint32_t& mid = mid_[sym];
        if (rng_.below(2) && mid + 300 < cfg_.levels) ++mid;
        else if (mid > 300) --mid;
    }

    const std::uint32_t r = rng_.below(1024);
    std::uint32_t acc = 0;

    if (r < (acc += cfg_.p_bad)) {
        // Malformed: unknown type, bad symbol, zero qty, or out of window.
        std::uint16_t session = 0;
        Msg m = make_add(sym, false, session);
        in.session = session;
        switch (rng_.below(4)) {
            case 0: m.bytes[0] = 0x7F; break;
            case 1: { auto n = m.as<NewOrder>(); n.symbol = std::uint16_t(cfg_.num_symbols + 3); m = Msg::from(n); break; }
            case 2: { auto n = m.as<NewOrder>(); n.qty = 0; m = Msg::from(n); break; }
            default: { auto n = m.as<NewOrder>(); n.price_ticks = cfg_.base_ticks + cfg_.levels + rng_.below(100); m = Msg::from(n); break; }
        }
        in.msg = m;
        return in;
    }

    const bool want_cancel = r < (acc += cfg_.p_cancel);
    const bool want_reduce = !want_cancel && r < (acc += cfg_.p_reduce);
    if ((want_cancel || want_reduce) && !live_.empty()) {
        const Live& l = live_[rng_.below(std::uint32_t(live_.size()))];
        Handle h = l.h;
        std::uint16_t session = l.session;
        std::uint16_t symbol = l.symbol;
        if (rng_.chance(cfg_.p_stale)) {
            switch (rng_.below(3)) {
                case 0: if (!dead_.empty()) h = dead_[rng_.below(std::uint32_t(dead_.size()))]; break;
                case 1: session = std::uint16_t(session % cfg_.sessions + 1); break;  // foreign session
                default: h = make_handle(rng_.below(65536), std::uint8_t(rng_.below(256))); break;
            }
        }
        in.session = session;
        if (want_cancel) {
            auto c = blank<Cancel>(MsgType::Cancel);
            c.symbol = symbol;
            c.order_handle = h;
            c.client_order_id = next_cl_id_++;
            in.msg = Msg::from(c);
        } else {
            auto c = blank<Reduce>(MsgType::Reduce);
            c.symbol = symbol;
            c.order_handle = h;
            // Sometimes an invalid reduce (not lower) to exercise the reject.
            c.new_qty = rng_.chance(100) ? l.qty + 1 : (l.qty > 1 ? 1 + rng_.below(l.qty - 1) : 1);
            c.client_order_id = next_cl_id_++;
            in.msg = Msg::from(c);
        }
        return in;
    }

    const bool cross = r < (acc += cfg_.p_cross);
    std::uint16_t session = 0;
    in.msg = make_add(sym, cross, session);
    in.session = session;
    return in;
}

void OrderFlow::forget(Handle h) {
    auto it = index_.find(h);
    if (it == index_.end()) return;
    const std::size_t i = it->second;
    index_.erase(it);
    if (i != live_.size() - 1) {
        live_[i] = live_.back();
        index_[live_[i].h] = i;
    }
    live_.pop_back();
    if (dead_.size() < 1024) dead_.push_back(h);
    else dead_[rng_.below(1024)] = h;
}

void OrderFlow::observe(Dest dest, const Msg& msg) {
    switch (msg.type()) {
        case MsgType::Accepted: {
            const auto a = msg.as<Accepted>();
            last_accepted_ = a.order_handle;
            last_session_ = dest;
            break;
        }
        case MsgType::MdAdd: {
            // The order rested, so it is now cancellable. Its Accepted came
            // earlier in the same message's output, which carried the session.
            const auto a = msg.as<MdAdd>();
            if (a.order_handle == last_accepted_) {
                index_[a.order_handle] = live_.size();
                live_.push_back({a.order_handle, a.symbol, last_session_, a.qty});
            }
            break;
        }
        case MsgType::Fill: {
            const auto f = msg.as<Fill>();
            if (f.liquidity == Liquidity::Maker) {
                if (f.leaves_qty == 0) forget(f.order_handle);
                else if (auto it = index_.find(f.order_handle); it != index_.end())
                    live_[it->second].qty = f.leaves_qty;
            }
            break;
        }
        case MsgType::Cancelled: {
            const auto c = msg.as<Cancelled>();
            if (c.reason == CancelReason::Reduced) {
                if (auto it = index_.find(c.order_handle); it != index_.end())
                    live_[it->second].qty -= c.cancelled_qty;
            } else {
                forget(c.order_handle);
            }
            break;
        }
        default: break;
    }
}

}  // namespace dromos
