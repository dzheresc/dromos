// bench: refengine throughput on a pre-generated stream (software baseline,
// not a latency claim for the FPGA).
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "dromos/orderflow.hpp"
#include "dromos/refengine.hpp"

using namespace dromos;

namespace {
class NullSink final : public OutSink {
public:
    void emit(Dest, const proto::Msg& m) override { sink_ ^= m.bytes[0]; }
    std::uint64_t sink_ = 0;
};
}  // namespace

int main(int argc, char** argv) {
    const std::uint64_t n = argc > 1 ? std::strtoull(argv[1], nullptr, 0) : 5'000'000;
    FlowConfig fcfg;
    fcfg.seed = 42;
    EngineConfig ecfg{.num_symbols = fcfg.num_symbols};

    // Record a closed-loop stream once, then replay it open-loop for timing.
    std::vector<Ingress> stream;
    stream.reserve(n);
    {
        OrderFlow flow(fcfg);
        RefEngine eng(ecfg);
        for (std::uint16_t s = 0; s < fcfg.num_symbols; ++s) eng.configure_symbol(s, fcfg.base_ticks);
        VectorSink v;
        for (std::uint64_t i = 0; i < n; ++i) {
            stream.push_back(flow.next());
            v.clear();
            eng.on_message(stream.back(), v);
            for (auto& e : v.events) flow.observe(e.dest, e.msg);
        }
    }

    RefEngine eng(ecfg);
    for (std::uint16_t s = 0; s < fcfg.num_symbols; ++s) eng.configure_symbol(s, fcfg.base_ticks);
    NullSink sink;
    const auto t0 = std::chrono::steady_clock::now();
    for (const auto& in : stream) eng.on_message(in, sink);
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("%llu msgs in %.3f s: %.2f M msgs/s, %.1f ns/msg (%u resting)\n",
                (unsigned long long)n, secs, double(n) / secs / 1e6, secs * 1e9 / double(n), eng.live_orders());
    return int(sink.sink_ & 0);
}
