// Directed tests for the reference engines. Every scenario runs on both
// RefEngine and NaiveEngine and also checks that their outputs are identical.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "dromos/crc64.hpp"
#include "dromos/naive_engine.hpp"
#include "dromos/refengine.hpp"

using namespace dromos;
using namespace dromos::proto;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                                     \
    do {                                                                                \
        ++g_checks;                                                                     \
        if (!(cond)) {                                                                  \
            ++g_failures;                                                               \
            std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
        }                                                                               \
    } while (0)

#define CHECK_EQ(a, b) CHECK((a) == (b))

namespace {

constexpr std::uint32_t kBase = 1000;

// With DROMOS_RECORD=<file>, every scenario's inbound stream is written out so
// the RTL testbench can replay it (tb_book_core --replay <file>). Format:
//   R <configured symbols> <base ticks>     start of a scenario (fresh engine)
//   M <session> <ts_ns> <64 hex chars>      one inbound message
std::FILE* recorder() {
    static std::FILE* f = [] {
        const char* path = std::getenv("DROMOS_RECORD");
        return path ? std::fopen(path, "w") : nullptr;
    }();
    return f;
}

// Runs the same inbound messages through both engines.
struct Pair {
    explicit Pair(EngineConfig cfg = {.num_symbols = 4, .levels = 4096, .pool_size = 1024})
        : ref(cfg), naive(cfg) {
        for (std::uint16_t s = 0; s < 2; ++s) {
            ref.configure_symbol(s, kBase);
            naive.configure_symbol(s, kBase);
        }
        if (auto* f = recorder()) std::fprintf(f, "R 2 %u\n", kBase);
    }

    std::vector<OutEvent> send(std::uint16_t session, const Msg& m) {
        Ingress in{m, session, ++seq, seq * 10};
        if (auto* f = recorder()) {
            std::fprintf(f, "M %u %llu ", unsigned(session), (unsigned long long)in.ts_ns);
            for (auto b : m.bytes) std::fprintf(f, "%02x", b);
            std::fprintf(f, "\n");
        }
        VectorSink a, b;
        ref.on_message(in, a);
        naive.on_message(in, b);
        CHECK(a.events == b.events);
        return a.events;
    }

    std::vector<OutEvent> add(std::uint16_t session, Side side, std::uint32_t px, std::uint32_t qty,
                              Tif tif = Tif::Day, std::uint16_t sym = 0) {
        auto m = blank<NewOrder>(MsgType::NewOrder);
        m.side = side;
        m.symbol = sym;
        m.price_ticks = px;
        m.qty = qty;
        m.tif = tif;
        m.client_order_id = ++cl;
        return send(session, Msg::from(m));
    }

    std::vector<OutEvent> cancel(std::uint16_t session, Handle h, std::uint16_t sym = 0) {
        auto m = blank<Cancel>(MsgType::Cancel);
        m.symbol = sym;
        m.order_handle = h;
        m.client_order_id = ++cl;
        return send(session, Msg::from(m));
    }

    std::vector<OutEvent> reduce(std::uint16_t session, Handle h, std::uint32_t q, std::uint16_t sym = 0) {
        auto m = blank<Reduce>(MsgType::Reduce);
        m.symbol = sym;
        m.order_handle = h;
        m.new_qty = q;
        m.client_order_id = ++cl;
        return send(session, Msg::from(m));
    }

    RefEngine ref;
    NaiveEngine naive;
    std::uint64_t seq = 0;
    std::uint64_t cl = 0;
};

Handle handle_of(const std::vector<OutEvent>& ev) {
    for (auto& e : ev)
        if (e.msg.type() == MsgType::Accepted) return e.msg.as<Accepted>().order_handle;
    return ~Handle(0);
}

std::vector<MsgType> types(const std::vector<OutEvent>& ev) {
    std::vector<MsgType> t;
    for (auto& e : ev) t.push_back(e.msg.type());
    return t;
}

RejectReason reject_of(const std::vector<OutEvent>& ev) {
    if (ev.size() != 1 || ev[0].msg.type() != MsgType::Rejected) return RejectReason::None;
    return ev[0].msg.as<Rejected>().reason;
}

// ------------------------------------------------------------------- tests

void test_crc64_check_value() {
    Crc64 c;
    const char* s = "123456789";
    c.update(reinterpret_cast<const std::uint8_t*>(s), 9);
    CHECK_EQ(c.value(), 0x6C40DF5F0B497347ull);  // CRC-64/ECMA-182 check value
}

void test_rest_and_partial_fill() {
    Pair p;
    auto e1 = p.add(1, Side::Buy, 1100, 100);
    CHECK((types(e1) == std::vector{MsgType::Accepted, MsgType::MdAdd}));
    const Handle bid = handle_of(e1);
    CHECK_EQ(p.ref.best_bid(0).value_or(0), 1100u);
    CHECK(!p.ref.best_ask(0));

    auto e2 = p.add(2, Side::Sell, 1090, 30);  // crosses, fills at the resting price 1100
    CHECK((types(e2) == std::vector{MsgType::Accepted, MsgType::Fill, MsgType::Fill, MsgType::MdExecute}));
    auto maker = e2[1].msg.as<Fill>();
    auto taker = e2[2].msg.as<Fill>();
    CHECK_EQ(e2[1].dest, 1);
    CHECK_EQ(e2[2].dest, 2);
    CHECK(maker.liquidity == Liquidity::Maker);
    CHECK_EQ(maker.order_handle, bid);
    CHECK_EQ(maker.price_ticks, 1100u);
    CHECK_EQ(maker.fill_qty, 30u);
    CHECK_EQ(maker.leaves_qty, 70u);
    CHECK_EQ(taker.leaves_qty, 0u);
    CHECK_EQ(maker.match_id, taker.match_id);
    CHECK_EQ(e2[3].dest, kFeed);
    CHECK_EQ(p.ref.qty_at(0, Side::Buy, 1100), 70u);
    CHECK_EQ(p.ref.live_orders(), 1u);
}

void test_price_time_priority() {
    Pair p;
    const Handle a = handle_of(p.add(1, Side::Sell, 1200, 10));
    const Handle b = handle_of(p.add(2, Side::Sell, 1200, 10));
    const Handle c = handle_of(p.add(1, Side::Sell, 1199, 10));
    auto e = p.add(2, Side::Buy, 1200, 25);
    // Better price first (c at 1199), then a before b at 1200.
    std::vector<Handle> makers;
    for (auto& x : e)
        if (x.msg.type() == MsgType::Fill && x.msg.as<Fill>().liquidity == Liquidity::Maker)
            makers.push_back(x.msg.as<Fill>().order_handle);
    CHECK((makers == std::vector{c, a, b}));
    CHECK_EQ(p.ref.qty_at(0, Side::Sell, 1200), 5u);
    (void)b;
}

void test_sweep_and_ioc() {
    Pair p;
    for (std::uint32_t px = 1300; px < 1305; ++px) p.add(1, Side::Sell, px, 10);
    auto e = p.add(2, Side::Buy, 1302, 100, Tif::Ioc);  // takes 3 levels, 30 lots
    std::uint32_t filled = 0;
    for (auto& x : e)
        if (x.msg.type() == MsgType::Fill && x.msg.as<Fill>().liquidity == Liquidity::Taker)
            filled += x.msg.as<Fill>().fill_qty;
    CHECK_EQ(filled, 30u);
    CHECK(e.back().msg.type() == MsgType::Cancelled);
    CHECK(e.back().msg.as<Cancelled>().reason == CancelReason::Ioc);
    CHECK_EQ(e.back().msg.as<Cancelled>().cancelled_qty, 70u);
    CHECK_EQ(p.ref.best_ask(0).value_or(0), 1303u);
    CHECK(!p.ref.best_bid(0));
    CHECK_EQ(p.ref.live_orders(), 2u);
}

void test_cancel_and_stale_handles() {
    Pair p;
    const Handle h = handle_of(p.add(1, Side::Buy, 1050, 5));
    CHECK(reject_of(p.cancel(2, h)) == RejectReason::UnknownHandle);     // foreign session
    CHECK(reject_of(p.cancel(1, h, 1)) == RejectReason::UnknownHandle);  // wrong symbol
    auto e = p.cancel(1, h);
    CHECK((types(e) == std::vector{MsgType::Cancelled, MsgType::MdDelete}));
    CHECK_EQ(e[0].msg.as<Cancelled>().cancelled_qty, 5u);
    CHECK(!p.ref.best_bid(0));
    CHECK(reject_of(p.cancel(1, h)) == RejectReason::UnknownHandle);     // already gone
    CHECK(reject_of(p.cancel(1, make_handle(9999, 0))) == RejectReason::UnknownHandle);
    CHECK(reject_of(p.cancel(1, h | (1ull << 40))) == RejectReason::UnknownHandle);
    CHECK(reject_of(p.cancel(1, h, 3)) == RejectReason::UnknownSymbol);  // symbol 3 not enabled
}

void test_slot_reuse_bumps_generation() {
    Pair p({.num_symbols = 2, .levels = 4096, .pool_size = 2});
    const Handle h0 = handle_of(p.add(1, Side::Buy, 1010, 1));
    const Handle h1 = handle_of(p.add(1, Side::Buy, 1011, 1));
    CHECK(reject_of(p.add(1, Side::Buy, 1012, 1)) == RejectReason::PoolFull);
    CHECK_EQ(handle_slot(h0), 0u);
    CHECK_EQ(handle_slot(h1), 1u);
    p.cancel(1, h0);
    const Handle h2 = handle_of(p.add(1, Side::Buy, 1013, 1));
    CHECK_EQ(handle_slot(h2), 0u);
    CHECK_EQ(handle_gen(h2), 1u);
    CHECK(reject_of(p.cancel(1, h0)) == RejectReason::UnknownHandle);
}

void test_reduce() {
    Pair p;
    const Handle h = handle_of(p.add(1, Side::Sell, 1500, 50));
    CHECK(reject_of(p.reduce(1, h, 50)) == RejectReason::BadReduce);
    CHECK(reject_of(p.reduce(1, h, 0)) == RejectReason::BadReduce);
    auto e = p.reduce(1, h, 20);
    CHECK((types(e) == std::vector{MsgType::Cancelled, MsgType::MdDelete}));
    CHECK(e[0].msg.as<Cancelled>().reason == CancelReason::Reduced);
    CHECK_EQ(e[0].msg.as<Cancelled>().cancelled_qty, 30u);
    CHECK_EQ(e[1].msg.as<MdDelete>().remaining_qty, 20u);
    CHECK_EQ(p.ref.qty_at(0, Side::Sell, 1500), 20u);
}

void test_window_edges_and_validation() {
    Pair p;
    CHECK(reject_of(p.add(1, Side::Buy, kBase - 1, 1)) == RejectReason::PriceOutOfWindow);
    CHECK(reject_of(p.add(1, Side::Buy, kBase + 4096, 1)) == RejectReason::PriceOutOfWindow);
    CHECK(reject_of(p.add(1, Side::Buy, kBase, 1)) == RejectReason::None);
    CHECK(reject_of(p.add(1, Side::Sell, kBase + 4095, 1)) == RejectReason::None);
    CHECK_EQ(p.ref.best_bid(0).value_or(0), kBase);
    CHECK_EQ(p.ref.best_ask(0).value_or(0), kBase + 4095);
    CHECK(reject_of(p.add(1, Side::Buy, 1100, 0)) == RejectReason::BadQty);
    CHECK(reject_of(p.add(1, Side::Buy, 1100, 1, Tif::Day, 2)) == RejectReason::UnknownSymbol);
    CHECK(reject_of(p.add(1, Side(5), 1100, 1)) == RejectReason::BadSide);
    CHECK(reject_of(p.add(1, Side::Buy, 1100, 1, Tif(9))) == RejectReason::BadTif);
    Msg junk{};
    junk.bytes[0] = 0x55;
    CHECK(reject_of(p.send(1, junk)) == RejectReason::BadType);
}

void test_bitmap_word_boundaries() {
    // Best-price search across 64-level word boundaries, both directions.
    Pair p;
    for (std::uint32_t lvl : {63u, 64u, 127u, 128u, 4032u, 4095u, 0u})
        p.add(1, Side::Sell, kBase + lvl, 1);
    std::vector<std::uint32_t> seen;
    for (int i = 0; i < 7; ++i) {
        seen.push_back(p.ref.best_ask(0).value_or(0) - kBase);
        p.add(2, Side::Buy, kBase + 4095, 1, Tif::Ioc);
    }
    CHECK((seen == std::vector<std::uint32_t>{0, 63, 64, 127, 128, 4032, 4095}));
    CHECK(!p.ref.best_ask(0));
}

}  // namespace

int main() {
    struct T { const char* name; void (*fn)(); };
    const T tests[] = {
        {"crc64_check_value", test_crc64_check_value},
        {"rest_and_partial_fill", test_rest_and_partial_fill},
        {"price_time_priority", test_price_time_priority},
        {"sweep_and_ioc", test_sweep_and_ioc},
        {"cancel_and_stale_handles", test_cancel_and_stale_handles},
        {"slot_reuse_bumps_generation", test_slot_reuse_bumps_generation},
        {"reduce", test_reduce},
        {"window_edges_and_validation", test_window_edges_and_validation},
        {"bitmap_word_boundaries", test_bitmap_word_boundaries},
    };
    for (const auto& t : tests) {
        const int before = g_failures;
        t.fn();
        std::printf("%-32s %s\n", t.name, g_failures == before ? "ok" : "FAILED");
    }
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    if (auto* f = recorder()) std::fclose(f);
    return g_failures ? 1 : 0;
}
