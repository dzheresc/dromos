// Builders for outbound messages. Both reference engines use these so that
// the byte layout of every event is defined in exactly one place.
#pragma once

#include "dromos/common.hpp"

namespace dromos::ev {

using namespace proto;

inline Msg accepted(Side side, std::uint16_t sym, std::uint32_t price, Handle h,
                    std::uint64_t cl_id, std::uint64_t ts) {
    auto m = blank<Accepted>(MsgType::Accepted);
    m.side = side;
    m.symbol = sym;
    m.price_ticks = price;
    m.order_handle = h;
    m.client_order_id = cl_id;
    m.timestamp_ns = ts;
    return Msg::from(m);
}

inline Msg rejected(RejectReason r, std::uint16_t sym, std::uint8_t orig_type,
                    std::uint64_t cl_id, std::uint64_t ts) {
    auto m = blank<Rejected>(MsgType::Rejected);
    m.reason = r;
    m.symbol = sym;
    m.orig_type = orig_type;
    m.client_order_id = cl_id;
    m.timestamp_ns = ts;
    return Msg::from(m);
}

inline Msg fill(Liquidity liq, std::uint16_t sym, std::uint32_t price, Handle h,
                std::uint32_t match_id, std::uint32_t qty, std::uint32_t leaves) {
    auto m = blank<Fill>(MsgType::Fill);
    m.liquidity = liq;
    m.symbol = sym;
    m.price_ticks = price;
    m.order_handle = h;
    m.match_id = match_id;
    m.fill_qty = qty;
    m.leaves_qty = leaves;
    return Msg::from(m);
}

inline Msg cancelled(CancelReason r, std::uint16_t sym, std::uint32_t qty, Handle h,
                     std::uint64_t cl_id, std::uint64_t ts) {
    auto m = blank<Cancelled>(MsgType::Cancelled);
    m.reason = r;
    m.symbol = sym;
    m.cancelled_qty = qty;
    m.order_handle = h;
    m.client_order_id = cl_id;
    m.timestamp_ns = ts;
    return Msg::from(m);
}

inline Msg md_add(Side side, std::uint16_t sym, std::uint32_t price, Handle h,
                  std::uint32_t qty, std::uint64_t ts) {
    auto m = blank<MdAdd>(MsgType::MdAdd);
    m.side = side;
    m.symbol = sym;
    m.price_ticks = price;
    m.order_handle = h;
    m.qty = qty;
    m.timestamp_ns = ts;
    return Msg::from(m);
}

inline Msg md_execute(Side resting_side, std::uint16_t sym, std::uint32_t price,
                      Handle resting, std::uint32_t qty, std::uint32_t match_id,
                      std::uint64_t ts) {
    auto m = blank<MdExecute>(MsgType::MdExecute);
    m.resting_side = resting_side;
    m.symbol = sym;
    m.price_ticks = price;
    m.resting_handle = resting;
    m.qty = qty;
    m.match_id = match_id;
    m.timestamp_ns = ts;
    return Msg::from(m);
}

inline Msg md_delete(Side side, std::uint16_t sym, std::uint32_t remaining, Handle h,
                     std::uint64_t ts) {
    auto m = blank<MdDelete>(MsgType::MdDelete);
    m.side = side;
    m.symbol = sym;
    m.remaining_qty = remaining;
    m.order_handle = h;
    m.timestamp_ns = ts;
    return Msg::from(m);
}

// Client order id sits at offset 16 in every inbound message type, so a
// reject for an unknown type can still echo it.
inline std::uint64_t inbound_client_id(const Msg& m) {
    std::uint64_t v;
    std::memcpy(&v, m.bytes + 16, sizeof(v));
    return v;
}
inline std::uint16_t inbound_symbol(const Msg& m) {
    std::uint16_t v;
    std::memcpy(&v, m.bytes + 2, sizeof(v));
    return v;
}

}  // namespace dromos::ev
