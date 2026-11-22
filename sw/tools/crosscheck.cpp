// crosscheck: run RefEngine and NaiveEngine on the same seeded order flow and
// stop at the first message where their outputs differ.
//
//   crosscheck [--seeds N] [--first S] [--msgs M] [--symbols K] [--pool P]
//
// Prints one CRC-64 digest per output stream per seed; these are the numbers
// the 325T generators will be compared against on hardware.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "dromos/crc64.hpp"
#include "dromos/naive_engine.hpp"
#include "dromos/orderflow.hpp"
#include "dromos/refengine.hpp"

using namespace dromos;

namespace {

void dump(const char* who, const std::vector<OutEvent>& ev) {
    std::fprintf(stderr, "  %s: %zu events\n", who, ev.size());
    for (auto& e : ev) {
        std::fprintf(stderr, "    dest=%5u ", unsigned(e.dest));
        for (auto b : e.msg.bytes) std::fprintf(stderr, "%02x", b);
        std::fprintf(stderr, "\n");
    }
}

std::uint64_t arg(int argc, char** argv, const char* name, std::uint64_t def) {
    for (int i = 1; i + 1 < argc; ++i)
        if (std::strcmp(argv[i], name) == 0) return std::strtoull(argv[i + 1], nullptr, 0);
    return def;
}

}  // namespace

int main(int argc, char** argv) {
    const std::uint64_t seeds = arg(argc, argv, "--seeds", 10);
    const std::uint64_t first = arg(argc, argv, "--first", 1);
    const std::uint64_t msgs = arg(argc, argv, "--msgs", 200000);
    const std::uint32_t symbols = std::uint32_t(arg(argc, argv, "--symbols", 4));
    const std::uint32_t pool = std::uint32_t(arg(argc, argv, "--pool", 65536));
    const bool quiet = arg(argc, argv, "--quiet", 0) != 0;

    const EngineConfig ecfg{.num_symbols = symbols, .levels = 4096, .pool_size = pool};
    std::uint64_t total = 0;
    const auto t0 = std::chrono::steady_clock::now();

    for (std::uint64_t seed = first; seed < first + seeds; ++seed) {
        FlowConfig fcfg;
        fcfg.seed = seed;
        fcfg.num_symbols = symbols;
        OrderFlow flow(fcfg);
        RefEngine ref(ecfg);
        NaiveEngine naive(ecfg);
        for (std::uint16_t s = 0; s < symbols; ++s) {
            ref.configure_symbol(s, fcfg.base_ticks);
            naive.configure_symbol(s, fcfg.base_ticks);
        }
        DigestSink digests;
        VectorSink a, b;
        for (std::uint64_t i = 0; i < msgs; ++i) {
            const Ingress in = flow.next();
            a.clear();
            b.clear();
            ref.on_message(in, a);
            naive.on_message(in, b);
            if (a.events != b.events) {
                std::fprintf(stderr, "MISMATCH seed=%llu msg=%llu session=%u\n  in: ",
                             (unsigned long long)seed, (unsigned long long)in.seq, in.session);
                for (auto x : in.msg.bytes) std::fprintf(stderr, "%02x", x);
                std::fprintf(stderr, "\n");
                dump("refengine", a.events);
                dump("naive", b.events);
                return 1;
            }
            for (auto& e : a.events) {
                flow.observe(e.dest, e.msg);
                digests.emit(e.dest, e.msg);
            }
        }
        if (ref.live_orders() != naive.live_orders()) {
            std::fprintf(stderr, "MISMATCH seed=%llu live orders %u vs %u\n",
                         (unsigned long long)seed, ref.live_orders(), naive.live_orders());
            return 1;
        }
        total += msgs;
        if (!quiet) {
            std::printf("seed %llu: %llu msgs, %u resting", (unsigned long long)seed,
                        (unsigned long long)msgs, ref.live_orders());
            for (auto& [dest, d] : digests.digests)
                std::printf("  %s=%016llx/%llu", dest == kFeed ? "feed" : ("s" + std::to_string(dest)).c_str(),
                            (unsigned long long)d.crc.value(), (unsigned long long)d.count);
            std::printf("\n");
        }
    }
    const double secs =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("OK: %llu seeds, %llu messages, both models agree (%.1f s)\n",
                (unsigned long long)seeds, (unsigned long long)total, secs);
    return 0;
}
