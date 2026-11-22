#include "dromos/naive_engine.hpp"

#include <algorithm>

#include "dromos/events.hpp"

namespace dromos {

using namespace proto;

NaiveEngine::NaiveEngine(const EngineConfig& cfg) : cfg_(cfg) {
    syms_.resize(cfg_.num_symbols);
    pool_.resize(cfg_.pool_size);
    for (std::uint32_t i = 0; i < cfg_.pool_size; ++i) free_.push_back(i);
}

void NaiveEngine::configure_symbol(std::uint16_t sym, std::uint32_t base_ticks) {
    syms_.at(sym).enabled = true;
    syms_[sym].base = base_ticks;
}

void NaiveEngine::free_slot(std::uint32_t slot) {
    pool_[slot].live = false;
    pool_[slot].gen = std::uint8_t(pool_[slot].gen + 1);
    free_.push_back(slot);
    --live_;
}

void NaiveEngine::remove_from_book(std::uint32_t slot) {
    const Order& o = pool_[slot];
    Side& side = o.side == 0 ? syms_[o.symbol].bids : syms_[o.symbol].asks;
    auto it = side.find(o.price);
    auto& q = it->second;
    q.erase(std::find(q.begin(), q.end(), slot));
    if (q.empty()) side.erase(it);
}

void NaiveEngine::on_message(const Ingress& in, OutSink& out) {
    switch (in.msg.type()) {
        case MsgType::NewOrder: on_new(in, out); break;
        case MsgType::Cancel: on_cancel(in, out); break;
        case MsgType::Reduce: on_reduce(in, out); break;
        default:
            out.emit(in.session,
                     ev::rejected(RejectReason::BadType, ev::inbound_symbol(in.msg),
                                  in.msg.bytes[0], ev::inbound_client_id(in.msg), in.ts_ns));
    }
}

void NaiveEngine::on_new(const Ingress& in, OutSink& out) {
    const auto m = in.msg.as<NewOrder>();
    auto reject = [&](RejectReason r) {
        out.emit(in.session, ev::rejected(r, m.symbol, std::uint8_t(MsgType::NewOrder),
                                          m.client_order_id, in.ts_ns));
    };
    if (m.symbol >= syms_.size() || !syms_[m.symbol].enabled)
        return reject(RejectReason::UnknownSymbol);
    if (std::uint8_t(m.side) > 1) return reject(RejectReason::BadSide);
    if (std::uint8_t(m.tif) > 1) return reject(RejectReason::BadTif);
    if (m.qty == 0) return reject(RejectReason::BadQty);
    Symbol& s = syms_[m.symbol];
    if (std::uint64_t(m.price_ticks) < s.base ||
        std::uint64_t(m.price_ticks) >= std::uint64_t(s.base) + cfg_.levels)
        return reject(RejectReason::PriceOutOfWindow);
    if (free_.empty()) return reject(RejectReason::PoolFull);

    const std::uint32_t slot = free_.front();
    free_.pop_front();
    ++live_;
    Order& o = pool_[slot];
    o.client_id = m.client_order_id;
    o.price = m.price_ticks;
    o.qty = m.qty;
    o.symbol = m.symbol;
    o.session = in.session;
    o.side = std::uint8_t(m.side);
    o.live = true;
    const Handle h = make_handle(slot, o.gen);
    out.emit(in.session,
             ev::accepted(m.side, m.symbol, m.price_ticks, h, m.client_order_id, in.ts_ns));

    const bool buy = m.side == proto::Side::Buy;
    Side& opp = buy ? s.asks : s.bids;
    while (o.qty > 0 && !opp.empty()) {
        auto it = buy ? opp.begin() : std::prev(opp.end());
        const std::uint32_t px = it->first;
        if (buy ? px > m.price_ticks : px < m.price_ticks) break;
        const std::uint32_t rs = it->second.front();
        Order& r = pool_[rs];
        const std::uint32_t q = std::min(o.qty, r.qty);
        const Handle rh = make_handle(rs, r.gen);
        const std::uint32_t mid = ++match_id_;
        r.qty -= q;
        o.qty -= q;
        out.emit(r.session, ev::fill(Liquidity::Maker, m.symbol, px, rh, mid, q, r.qty));
        out.emit(in.session, ev::fill(Liquidity::Taker, m.symbol, px, h, mid, q, o.qty));
        out.emit(kFeed, ev::md_execute(buy ? proto::Side::Sell : proto::Side::Buy, m.symbol, px,
                                       rh, q, mid, in.ts_ns));
        if (r.qty == 0) {
            it->second.pop_front();
            if (it->second.empty()) opp.erase(it);
            free_slot(rs);
        }
    }

    if (o.qty == 0) {
        free_slot(slot);
    } else if (m.tif == Tif::Ioc) {
        out.emit(in.session,
                 ev::cancelled(CancelReason::Ioc, m.symbol, o.qty, h, m.client_order_id, in.ts_ns));
        free_slot(slot);
    } else {
        (buy ? s.bids : s.asks)[m.price_ticks].push_back(slot);
        out.emit(kFeed, ev::md_add(m.side, m.symbol, m.price_ticks, h, o.qty, in.ts_ns));
    }
}

std::optional<std::uint32_t> NaiveEngine::find(Handle h, std::uint16_t sym,
                                               std::uint16_t session) const {
    if (!handle_well_formed(h)) return std::nullopt;
    const std::uint32_t slot = handle_slot(h);
    if (slot >= pool_.size()) return std::nullopt;
    const Order& o = pool_[slot];
    if (!o.live || o.gen != handle_gen(h) || o.symbol != sym || o.session != session)
        return std::nullopt;
    return slot;
}

void NaiveEngine::on_cancel(const Ingress& in, OutSink& out) {
    const auto m = in.msg.as<Cancel>();
    auto reject = [&](RejectReason r) {
        out.emit(in.session, ev::rejected(r, m.symbol, std::uint8_t(MsgType::Cancel),
                                          m.client_order_id, in.ts_ns));
    };
    if (m.symbol >= syms_.size() || !syms_[m.symbol].enabled)
        return reject(RejectReason::UnknownSymbol);
    auto slot = find(m.order_handle, m.symbol, in.session);
    if (!slot) return reject(RejectReason::UnknownHandle);
    const Order& o = pool_[*slot];
    out.emit(in.session, ev::cancelled(CancelReason::User, m.symbol, o.qty, m.order_handle,
                                       o.client_id, in.ts_ns));
    out.emit(kFeed, ev::md_delete(proto::Side(o.side), m.symbol, 0, m.order_handle, in.ts_ns));
    remove_from_book(*slot);
    free_slot(*slot);
}

void NaiveEngine::on_reduce(const Ingress& in, OutSink& out) {
    const auto m = in.msg.as<Reduce>();
    auto reject = [&](RejectReason r) {
        out.emit(in.session, ev::rejected(r, m.symbol, std::uint8_t(MsgType::Reduce),
                                          m.client_order_id, in.ts_ns));
    };
    if (m.symbol >= syms_.size() || !syms_[m.symbol].enabled)
        return reject(RejectReason::UnknownSymbol);
    auto slot = find(m.order_handle, m.symbol, in.session);
    if (!slot) return reject(RejectReason::UnknownHandle);
    Order& o = pool_[*slot];
    if (m.new_qty == 0 || m.new_qty >= o.qty) return reject(RejectReason::BadReduce);
    const std::uint32_t diff = o.qty - m.new_qty;
    o.qty = m.new_qty;
    out.emit(in.session, ev::cancelled(CancelReason::Reduced, m.symbol, diff, m.order_handle,
                                       o.client_id, in.ts_ns));
    out.emit(kFeed, ev::md_delete(proto::Side(o.side), m.symbol, m.new_qty, m.order_handle,
                                  in.ts_ns));
}

std::optional<std::uint32_t> NaiveEngine::best_bid(std::uint16_t sym) const {
    const auto& b = syms_.at(sym).bids;
    if (b.empty()) return std::nullopt;
    return b.rbegin()->first;
}

std::optional<std::uint32_t> NaiveEngine::best_ask(std::uint16_t sym) const {
    const auto& a = syms_.at(sym).asks;
    if (a.empty()) return std::nullopt;
    return a.begin()->first;
}

}  // namespace dromos
