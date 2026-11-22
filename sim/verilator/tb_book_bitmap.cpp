// Verilator testbench for book_bitmap. Random set/clear traffic, compared
// against a std::set model after every update (allowing the 2-cycle latency).
//
//   tb_book_bitmap [ops] [seed] [+trace]
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <set>

#include "Vbook_bitmap.h"
#include "dromos/common.hpp"
#include "verilated.h"
#if VM_TRACE_FST
#include "verilated_fst_c.h"
#endif

namespace {
constexpr int kLatency = 2;
constexpr unsigned kLevels = 4096;
}

int main(int argc, char** argv) {
    const unsigned long ops = argc > 1 ? std::strtoul(argv[1], nullptr, 0) : 200000;
    const unsigned long seed = argc > 2 ? std::strtoul(argv[2], nullptr, 0) : 1;
    const bool trace = argc > 3 && std::strcmp(argv[3], "+trace") == 0;

    auto ctx = std::make_unique<VerilatedContext>();
    auto dut = std::make_unique<Vbook_bitmap>(ctx.get());
#if VM_TRACE_FST
    std::unique_ptr<VerilatedFstC> tfp;
    if (trace) {
        ctx->traceEverOn(true);
        tfp = std::make_unique<VerilatedFstC>();
        dut->trace(tfp.get(), 99);
        tfp->open("tb_book_bitmap.fst");
    }
#else
    (void)trace;
#endif
    std::uint64_t t = 0;
    auto tick = [&] {
        dut->clk = 0;
        dut->eval();
#if VM_TRACE_FST
        if (tfp) tfp->dump(t);
#endif
        ++t;
        dut->clk = 1;
        dut->eval();
#if VM_TRACE_FST
        if (tfp) tfp->dump(t);
#endif
        ++t;
    };

    dut->rst = 1;
    dut->upd_valid = 0;
    for (int i = 0; i < 4; ++i) tick();
    dut->rst = 0;

    dromos::Rng rng(seed);
    std::set<unsigned> model;
    unsigned long errors = 0;

    for (unsigned long i = 0; i < ops; ++i) {
        // Bias towards a moving cluster so words empty and refill often,
        // with occasional far levels to hit the word boundaries.
        unsigned lvl;
        if (rng.chance(64))
            lvl = rng.below(kLevels);
        else
            lvl = (2048 + (i / 1000) % 512 + rng.below(160)) % kLevels;
        const bool set = model.size() < 8 ? true : rng.below(2) == 0;
        dut->upd_valid = 1;
        dut->upd_set = set;
        dut->upd_level = lvl;
        if (set) model.insert(lvl); else model.erase(lvl);
        tick();
        dut->upd_valid = 0;
        for (int k = 0; k < kLatency; ++k) tick();

        const bool any = !model.empty();
        bool ok = dut->best_lo_valid == any && dut->best_hi_valid == any;
        if (any) ok = ok && dut->best_lo == *model.begin() && dut->best_hi == *model.rbegin();
        if (!ok && errors++ < 10)
            std::fprintf(stderr, "op %lu: lvl=%u set=%d model=[%u,%u] dut=[%u,%u] valid=%d/%d\n", i,
                         lvl, set, any ? *model.begin() : 0, any ? *model.rbegin() : 0,
                         unsigned(dut->best_lo), unsigned(dut->best_hi), dut->best_lo_valid,
                         dut->best_hi_valid);
        // Drain sometimes so the empty case is exercised.
        if (rng.chance(2)) {
            for (unsigned l : model) {
                dut->upd_valid = 1;
                dut->upd_set = 0;
                dut->upd_level = l;
                tick();
            }
            dut->upd_valid = 0;
            model.clear();
            for (int k = 0; k < kLatency; ++k) tick();
            if (dut->best_lo_valid || dut->best_hi_valid) ++errors;
        }
    }
#if VM_TRACE_FST
    if (tfp) tfp->close();
#endif
    dut->final();
    std::printf("tb_book_bitmap: %lu ops, seed %lu, %lu errors\n", ops, seed, errors);
    return errors ? 1 : 0;
}
