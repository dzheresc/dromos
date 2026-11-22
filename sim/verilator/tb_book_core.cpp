// tb_book_core: drives book_core and RefEngine with the same seeded order flow
// and compares every output event (destination + 32 bytes), in order.
//
//   tb_book_core [--seeds N] [--first S] [--msgs M] [--ready PCT] [--trace]
//   tb_book_core --replay FILE [--ready PCT]
//
// --replay runs recorded streams (DROMOS_RECORD=FILE test_engine) instead of
// the random flow; each scenario starts from a fresh reset.
//
// --ready sets the probability (percent) that out_ready is high on a cycle,
// to exercise back-pressure. Symbols and pool size come from the Verilated
// model's parameters (NSYM, POOL), passed here as compile definitions.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "dromos/orderflow.hpp"
#include "dromos/refengine.hpp"
#include "verilated.h"
#include TB_HEADER
#if VM_TRACE_FST
#include "verilated_fst_c.h"
#endif

using namespace dromos;

namespace {

std::uint64_t arg(int argc, char** argv, const char* name, std::uint64_t def) {
    for (int i = 1; i + 1 < argc; ++i)
        if (std::strcmp(argv[i], name) == 0) return std::strtoull(argv[i + 1], nullptr, 0);
    return def;
}
bool flag(int argc, char** argv, const char* name) {
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], name) == 0) return true;
    return false;
}

void print_event(const char* who, const OutEvent& e) {
    std::fprintf(stderr, "    %s dest=%5u ", who, unsigned(e.dest));
    for (auto b : e.msg.bytes) std::fprintf(stderr, "%02x", b);
    std::fprintf(stderr, "\n");
}

struct Bench {
    std::unique_ptr<VerilatedContext> ctx;
    std::unique_ptr<TB_MODEL> dut;
#if VM_TRACE_FST
    std::unique_ptr<VerilatedFstC> tfp;
#endif
    std::uint64_t t = 0;
    std::uint64_t cycles = 0;

    Bench(bool trace) : ctx(std::make_unique<VerilatedContext>()), dut(std::make_unique<TB_MODEL>(ctx.get())) {
#if VM_TRACE_FST
        if (trace) {
            ctx->traceEverOn(true);
            tfp = std::make_unique<VerilatedFstC>();
            dut->trace(tfp.get(), 99);
            tfp->open("tb_book_core.fst");
        }
#else
        (void)trace;
#endif
    }
    ~Bench() {
#if VM_TRACE_FST
        if (tfp) tfp->close();
#endif
        dut->final();
    }

    void dump() {
#if VM_TRACE_FST
        if (tfp) tfp->dump(t);
#endif
        ++t;
    }

    // Settle inputs at clk low, then raise the clock.
    void edge() {
        dut->clk = 0;
        dut->eval();
        dump();
        dut->clk = 1;
        dut->eval();
        dump();
        ++cycles;
    }
};

void to_wide(const proto::Msg& m, VlWide<8>& w) { std::memcpy(w.data(), m.bytes, 32); }
proto::Msg from_wide(const VlWide<8>& w) {
    proto::Msg m;
    std::memcpy(m.bytes, w.data(), 32);
    return m;
}

// Reset the DUT, configure the first nsym symbols, wait for the init sweep.
void reset_and_configure(Bench& b, RefEngine& ref, unsigned nsym, std::uint32_t base) {
    auto& d = *b.dut;
    d.rst = 1;
    d.in_valid = 0;
    d.out_ready = 1;
    d.cfg_valid = 0;
    for (int i = 0; i < 4; ++i) b.edge();
    d.rst = 0;
    for (std::uint16_t s = 0; s < nsym; ++s) {
        ref.configure_symbol(s, base);
        d.cfg_valid = 1;
        d.cfg_symbol = s;
        d.cfg_base = base;
        b.edge();
    }
    d.cfg_valid = 0;
    while (!d.init_done) b.edge();
}

// Coverage: [type][byte 1] (side, reason or liquidity) over all matched events.
std::uint64_t g_cover[256][16] = {};
std::uint64_t g_events = 0, g_max_cycles = 0;

// Send one message to both models; false (after printing) on mismatch or hang.
bool run_one(Bench& b, RefEngine& ref, Rng& ready_rng, std::uint32_t ready_pct, const Ingress& in,
             const char* tag, VectorSink& expect) {
    auto& d = *b.dut;
    expect.clear();
    ref.on_message(in, expect);

    std::vector<OutEvent> got;
    to_wide(in.msg, d.in_msg);
    d.in_session = in.session;
    d.in_ts = in.ts_ns;
    d.in_valid = 1;
    const std::uint64_t c0 = b.cycles;
    bool sent = false;
    for (std::uint64_t guard = 0;; ++guard) {
        d.out_ready = ready_rng.below(100) < ready_pct;
        d.clk = 0;
        d.eval();
        const bool take_in = d.in_valid && d.in_ready;
        const bool take_out = d.out_valid && d.out_ready;
        if (take_out) got.push_back({Dest(d.out_dest), from_wide(d.out_msg)});
        b.edge();
        if (take_in) {
            sent = true;
            d.in_valid = 0;
        }
        if (sent && !d.busy) break;
        if (guard > 1'000'000) {
            std::fprintf(stderr, "HANG %s msg=%llu\n", tag, (unsigned long long)in.seq);
            return false;
        }
    }
    const std::uint64_t c = b.cycles - c0;
    if (c > g_max_cycles) g_max_cycles = c;

    if (got != expect.events) {
        std::fprintf(stderr, "MISMATCH %s msg=%llu session=%u\n  in: ", tag,
                     (unsigned long long)in.seq, in.session);
        for (auto x : in.msg.bytes) std::fprintf(stderr, "%02x", x);
        std::fprintf(stderr, "\n  expected %zu, got %zu\n", expect.events.size(), got.size());
        const std::size_t n = std::max(got.size(), expect.events.size());
        for (std::size_t k = 0; k < n; ++k) {
            if (k < expect.events.size()) print_event("ref", expect.events[k]);
            if (k < got.size()) print_event("rtl", got[k]);
        }
        return false;
    }
    g_events += got.size();
    for (auto& e : got) ++g_cover[e.msg.bytes[0]][e.msg.bytes[1] & 15];
    return true;
}

void print_coverage() {
    std::printf("coverage (type: count per byte-1 value 0..9)\n");
    for (int ty = 0; ty < 256; ++ty) {
        std::uint64_t sum = 0;
        for (auto c : g_cover[ty]) sum += c;
        if (!sum) continue;
        std::printf("  0x%02x:", ty);
        for (int k = 0; k < 10; ++k) std::printf(" %llu", (unsigned long long)g_cover[ty][k]);
        std::printf("\n");
    }
}

int replay(const char* path, std::uint32_t ready_pct, bool trace) {
    std::FILE* f = std::fopen(path, "r");
    if (!f) {
        std::fprintf(stderr, "cannot open %s\n", path);
        return 1;
    }
    const EngineConfig ecfg{.num_symbols = TB_NSYM, .levels = 4096, .pool_size = TB_POOL};
    std::unique_ptr<Bench> b;
    std::unique_ptr<RefEngine> ref;
    Rng ready_rng(12345);
    VectorSink expect;
    unsigned scenarios = 0;
    std::uint64_t msgs = 0;
    char line[256];
    while (std::fgets(line, sizeof line, f)) {
        if (line[0] == 'R') {
            unsigned nsym = 0, base = 0;
            std::sscanf(line + 1, "%u %u", &nsym, &base);
            b.reset();  // close the previous model (and trace) first
            b = std::make_unique<Bench>(trace && scenarios == 0);
            ref = std::make_unique<RefEngine>(ecfg);
            reset_and_configure(*b, *ref, nsym, base);
            ++scenarios;
        } else if (line[0] == 'M' && b) {
            unsigned session = 0;
            unsigned long long ts = 0;
            char hex[80] = {};
            std::sscanf(line + 1, "%u %llu %70s", &session, &ts, hex);
            Ingress in{};
            for (int i = 0; i < 32; ++i) {
                unsigned v = 0;
                std::sscanf(hex + 2 * i, "%2x", &v);
                in.msg.bytes[i] = std::uint8_t(v);
            }
            in.session = std::uint16_t(session);
            in.ts_ns = ts;
            in.seq = ++msgs;
            const std::string tag = "scenario " + std::to_string(scenarios);
            if (!run_one(*b, *ref, ready_rng, ready_pct, in, tag.c_str(), expect)) return 1;
        }
    }
    std::fclose(f);
    if (!scenarios) {
        std::fprintf(stderr, "%s: no scenarios\n", path);
        return 1;
    }
    std::printf("OK: replay %s: %u scenarios, %llu messages (%llu events) match (NSYM=%d POOL=%d ready=%u%%)\n",
                path, scenarios, (unsigned long long)msgs, (unsigned long long)g_events, TB_NSYM,
                TB_POOL, ready_pct);
    print_coverage();
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const std::uint64_t seeds = arg(argc, argv, "--seeds", 4);
    const std::uint64_t first = arg(argc, argv, "--first", 1);
    const std::uint64_t msgs = arg(argc, argv, "--msgs", 20000);
    const std::uint32_t ready_pct = std::uint32_t(arg(argc, argv, "--ready", 100));
    const bool trace = flag(argc, argv, "--trace");
    for (int i = 1; i + 1 < argc; ++i)
        if (std::strcmp(argv[i], "--replay") == 0) return replay(argv[i + 1], ready_pct, trace);

    const EngineConfig ecfg{.num_symbols = TB_NSYM, .levels = 4096, .pool_size = TB_POOL};
    std::uint64_t total_msgs = 0, total_cycles = 0;

    for (std::uint64_t seed = first; seed < first + seeds; ++seed) {
        Bench b(trace && seed == first);
        Rng ready_rng(seed * 7919 + 1);

        FlowConfig fcfg;
        fcfg.seed = seed;
        fcfg.num_symbols = TB_NSYM;
        OrderFlow flow(fcfg);
        RefEngine ref(ecfg);
        reset_and_configure(b, ref, TB_NSYM, fcfg.base_ticks);
        const std::uint64_t start_cycles = b.cycles;

        VectorSink expect;
        const std::string tag = "seed " + std::to_string(seed);
        for (std::uint64_t i = 0; i < msgs; ++i) {
            const Ingress in = flow.next();
            if (!run_one(b, ref, ready_rng, ready_pct, in, tag.c_str(), expect)) return 1;
            for (auto& e : expect.events) flow.observe(e.dest, e.msg);
        }
        total_msgs += msgs;
        total_cycles += b.cycles - start_cycles;
        std::printf("seed %llu: %llu msgs match, %u resting\n", (unsigned long long)seed,
                    (unsigned long long)msgs, ref.live_orders());
    }
    std::printf("OK: book_core matches refengine on %llu messages (%llu events); "
                "%.1f cycles/msg average, %llu max (NSYM=%d POOL=%d ready=%u%%)\n",
                (unsigned long long)total_msgs, (unsigned long long)g_events,
                double(total_cycles) / double(total_msgs), (unsigned long long)g_max_cycles,
                TB_NSYM, TB_POOL, ready_pct);
    print_coverage();
    return 0;
}
