#include "dromos/refengine.hpp"

#include <algorithm>
#include <bit>
#include <cassert>
#include <stdexcept>

#include "dromos/events.hpp"

namespace dromos {

using namespace proto;

RefEngine::RefEngine(const EngineConfig& cfg) : cfg_(cfg) {
    if (cfg_.levels == 0 || cfg_.levels > 4096 || (cfg_.levels & (cfg_.levels - 1)) ||
        cfg_.levels % 64)
        throw std::invalid_argument("levels must be a power of two in [64, 4096]");
    if (cfg_.pool_size == 0 || cfg_.pool_size > 65536)
        throw std::invalid_argument("pool_size must be in [1, 65536]");
    if (cfg_.num_symbols == 0 || cfg_.num_symbols > 65535)
        throw std::invalid_argument("bad num_symbols");

    syms_.resize(cfg_.num_symbols);
    for (auto& s : syms_)
        for (auto& b : s.side) {
            b.levels.assign(cfg_.levels, Level{});
            b.words.assign(cfg_.levels / 64, 0);
        }
    pool_.assign(cfg_.pool_size, Order{});
    free_.resize(cfg_.pool_size);
    for (std::uint32_t i = 0; i < cfg_.pool_size; ++i) free_[i] = i;
    free_count_ = cfg_.pool_size;
}

void RefEngine::configure_symbol(std::uint16_t sym, std::uint32_t base_ticks) {
    if (sym >= cfg_.num_symbols) throw std::out_of_range("symbol");
    syms_[sym].enabled = true;
    syms_[sym].base = base_ticks;
}

// ---------------------------------------------------------------- free list

std::int32_t RefEngine::alloc() {
    if (free_count_ == 0) return kNil;
    std::uint32_t slot = free_[free_head_];
    free_head_ = (free_head_ + 1) % cfg_.pool_size;
    --free_count_;
    ++live_;
    return std::int32_t(slot);
}

void RefEngine::release(std::int32_t slot) {
    Order& o = pool_[slot];
    o.live = false;
    o.gen = std::uint8_t(o.gen + 1);
    std::uint32_t tail = (free_head_ + free_count_) % cfg_.pool_size;
    free_[tail] = std::uint32_t(slot);
    ++free_count_;
    --live_;
}

// --------------------------------------------------------------- level FIFO

void RefEngine::link_tail(Book& b, std::int32_t slot) {
    Order& o = pool_[slot];
    Level& l = b.levels[o.level];
    o.next = kNil;
    o.prev = l.tail;
    if (l.tail != kNil)
        pool_[l.tail].next = slot;
    else
        l.head = slot;
    l.tail = slot;
    l.qty += o.qty;
    ++l.count;
    if (l.count == 1) set_bit(b, o.level);
}

void RefEngine::unlink(Book& b, std::int32_t slot) {
    Order& o = pool_[slot];
    Level& l = b.levels[o.level];
    if (o.prev != kNil)
        pool_[o.prev].next = o.next;
    else
        l.head = o.next;
    if (o.next != kNil)
        pool_[o.next].prev = o.prev;
    else
        l.tail = o.prev;
    o.prev = o.next = kNil;
    l.qty -= o.qty;
    --l.count;
    if (l.count == 0) clear_bit(b, o.level);
}

// ------------------------------------------------------------------- bitmap

void RefEngine::set_bit(Book& b, std::uint32_t lvl) {
    b.words[lvl >> 6] |= 1ull << (lvl & 63);
    b.summary |= 1ull << (lvl >> 6);
}

void RefEngine::clear_bit(Book& b, std::uint32_t lvl) {
    auto& w = b.words[lvl >> 6];
    w &= ~(1ull << (lvl & 63));
    if (w == 0) b.summary &= ~(1ull << (lvl >> 6));
}

std::optional<std::uint32_t> RefEngine::lowest(const Book& b) {
    if (!b.summary) return std::nullopt;
    std::uint32_t w = std::uint32_t(std::countr_zero(b.summary));
    return w * 64 + std::uint32_t(std::countr_zero(b.words[w]));
}

std::optional<std::uint32_t> RefEngine::highest(const Book& b) {
    if (!b.summary) return std::nullopt;
    std::uint32_t w = 63 - std::uint32_t(std::countl_zero(b.summary));
    return w * 64 + 63 - std::uint32_t(std::countl_zero(b.words[w]));
}

// --------------------------------------------------------------- dispatch

void RefEngine::on_message(const Ingress& in, OutSink& out) {
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

void RefEngine::on_new(const Ingress& in, OutSink& out) {
    const auto m = in.msg.as<NewOrder>();
    auto reject = [&](RejectReason r) {
        out.emit(in.session, ev::rejected(r, m.symbol, std::uint8_t(MsgType::NewOrder),
                                          m.client_order_id, in.ts_ns));
    };

    if (m.symbol >= cfg_.num_symbols || !syms_[m.symbol].enabled)
        return reject(RejectReason::UnknownSymbol);
    if (std::uint8_t(m.side) > 1) return reject(RejectReason::BadSide);
    if (std::uint8_t(m.tif) > 1) return reject(RejectReason::BadTif);
    if (m.qty == 0) return reject(RejectReason::BadQty);
    Symbol& s = syms_[m.symbol];
    if (m.price_ticks < s.base || m.price_ticks - s.base >= cfg_.levels)
        return reject(RejectReason::PriceOutOfWindow);
    const std::int32_t slot = alloc();
    if (slot == kNil) return reject(RejectReason::PoolFull);

    const std::uint32_t lvl = m.price_ticks - s.base;
    Order& o = pool_[slot];
    o.client_id = m.client_order_id;
    o.qty = m.qty;
    o.level = lvl;
    o.prev = o.next = kNil;
    o.symbol = m.symbol;
    o.session = in.session;
    o.side = std::uint8_t(m.side);
    o.live = true;
    const Handle h = make_handle(std::uint32_t(slot), o.gen);

    out.emit(in.session,
             ev::accepted(m.side, m.symbol, m.price_ticks, h, m.client_order_id, in.ts_ns));

    // Match against the opposite side while the price crosses.
    const bool buy = m.side == Side::Buy;
    Book& opp = s.side[buy ? 1 : 0];
    const Side opp_side = buy ? Side::Sell : Side::Buy;
    while (o.qty > 0) {
        auto best = buy ? lowest(opp) : highest(opp);
        if (!best) break;
        if (buy ? *best > lvl : *best < lvl) break;

        const std::int32_t rs = opp.levels[*best].head;
        Order& r = pool_[rs];
        const std::uint32_t q = std::min(o.qty, r.qty);
        const std::uint32_t px = s.base + *best;
        const Handle rh = make_handle(std::uint32_t(rs), r.gen);
        const std::uint32_t mid = ++match_id_;

        r.qty -= q;
        o.qty -= q;
        opp.levels[*best].qty -= q;

        out.emit(r.session, ev::fill(Liquidity::Maker, m.symbol, px, rh, mid, q, r.qty));
        out.emit(in.session, ev::fill(Liquidity::Taker, m.symbol, px, h, mid, q, o.qty));
        out.emit(kFeed, ev::md_execute(opp_side, m.symbol, px, rh, q, mid, in.ts_ns));

        if (r.qty == 0) {
            unlink(opp, rs);
            release(rs);
        }
    }

    if (o.qty == 0) {
        release(slot);
    } else if (m.tif == Tif::Ioc) {
        out.emit(in.session,
                 ev::cancelled(CancelReason::Ioc, m.symbol, o.qty, h, m.client_order_id, in.ts_ns));
        release(slot);
    } else {
        link_tail(s.side[buy ? 0 : 1], slot);
        out.emit(kFeed, ev::md_add(m.side, m.symbol, m.price_ticks, h, o.qty, in.ts_ns));
    }
}

std::int32_t RefEngine::lookup(Handle h, std::uint16_t sym, std::uint16_t session) const {
    if (!handle_well_formed(h)) return kNil;
    const std::uint32_t slot = handle_slot(h);
    if (slot >= cfg_.pool_size) return kNil;
    const Order& o = pool_[slot];
    if (!o.live || o.gen != handle_gen(h) || o.symbol != sym || o.session != session) return kNil;
    return std::int32_t(slot);
}

void RefEngine::on_cancel(const Ingress& in, OutSink& out) {
    const auto m = in.msg.as<Cancel>();
    if (m.symbol >= cfg_.num_symbols || !syms_[m.symbol].enabled)
        return out.emit(in.session, ev::rejected(RejectReason::UnknownSymbol, m.symbol,
                                                 std::uint8_t(MsgType::Cancel), m.client_order_id,
                                                 in.ts_ns));
    const std::int32_t slot = lookup(m.order_handle, m.symbol, in.session);
    if (slot == kNil)
        return out.emit(in.session, ev::rejected(RejectReason::UnknownHandle, m.symbol,
                                                 std::uint8_t(MsgType::Cancel), m.client_order_id,
                                                 in.ts_ns));
    Order& o = pool_[slot];
    Book& b = syms_[m.symbol].side[o.side];
    const std::uint32_t qty = o.qty;
    unlink(b, slot);
    out.emit(in.session, ev::cancelled(CancelReason::User, m.symbol, qty, m.order_handle,
                                       o.client_id, in.ts_ns));
    out.emit(kFeed, ev::md_delete(Side(o.side), m.symbol, 0, m.order_handle, in.ts_ns));
    release(slot);
}

void RefEngine::on_reduce(const Ingress& in, OutSink& out) {
    const auto m = in.msg.as<Reduce>();
    auto reject = [&](RejectReason r) {
        out.emit(in.session, ev::rejected(r, m.symbol, std::uint8_t(MsgType::Reduce),
                                          m.client_order_id, in.ts_ns));
    };
    if (m.symbol >= cfg_.num_symbols || !syms_[m.symbol].enabled)
        return reject(RejectReason::UnknownSymbol);
    const std::int32_t slot = lookup(m.order_handle, m.symbol, in.session);
    if (slot == kNil) return reject(RejectReason::UnknownHandle);
    Order& o = pool_[slot];
    if (m.new_qty == 0 || m.new_qty >= o.qty) return reject(RejectReason::BadReduce);

    const std::uint32_t diff = o.qty - m.new_qty;
    o.qty = m.new_qty;
    syms_[m.symbol].side[o.side].levels[o.level].qty -= diff;
    out.emit(in.session, ev::cancelled(CancelReason::Reduced, m.symbol, diff, m.order_handle,
                                       o.client_id, in.ts_ns));
    out.emit(kFeed, ev::md_delete(Side(o.side), m.symbol, m.new_qty, m.order_handle, in.ts_ns));
}

// ------------------------------------------------------------ introspection

std::optional<std::uint32_t> RefEngine::best_bid(std::uint16_t sym) const {
    auto l = highest(syms_.at(sym).side[0]);
    if (!l) return std::nullopt;
    return syms_[sym].base + *l;
}

std::optional<std::uint32_t> RefEngine::best_ask(std::uint16_t sym) const {
    auto l = lowest(syms_.at(sym).side[1]);
    if (!l) return std::nullopt;
    return syms_[sym].base + *l;
}

std::uint32_t RefEngine::qty_at(std::uint16_t sym, Side side, std::uint32_t price) const {
    const Symbol& s = syms_.at(sym);
    if (price < s.base || price - s.base >= cfg_.levels) return 0;
    return s.side[std::uint8_t(side)].levels[price - s.base].qty;
}

}  // namespace dromos
