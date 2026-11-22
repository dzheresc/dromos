// Types shared by the reference engines, the order generator and the harness.
#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "dromos/proto.hpp"

namespace dromos {

// Destination of an output message: a session id, or the market data feed.
using Dest = std::uint16_t;
inline constexpr Dest kFeed = 0xFFFF;

// One inbound message after the ingress arbiter: the arbiter fixes the order,
// stamps the engine sequence number and the timestamp. Engines never read a
// clock; everything time-like comes from here, which keeps them deterministic.
struct Ingress {
    proto::Msg msg;
    std::uint16_t session = 0;
    std::uint64_t seq = 0;
    std::uint64_t ts_ns = 0;
};

struct EngineConfig {
    std::uint32_t num_symbols = 16;
    std::uint32_t levels = 4096;     // price levels per side, power of two, <= 4096
    std::uint32_t pool_size = 65536; // resting orders, <= 65536
};

class OutSink {
public:
    virtual ~OutSink() = default;
    virtual void emit(Dest dest, const proto::Msg& msg) = 0;
};

struct OutEvent {
    Dest dest;
    proto::Msg msg;
    bool operator==(const OutEvent&) const = default;
};

class VectorSink final : public OutSink {
public:
    void emit(Dest dest, const proto::Msg& msg) override { events.push_back({dest, msg}); }
    void clear() { events.clear(); }
    std::vector<OutEvent> events;
};

// Forwards to several sinks.
class TeeSink final : public OutSink {
public:
    TeeSink(OutSink& a, OutSink& b) : a_(a), b_(b) {}
    void emit(Dest dest, const proto::Msg& msg) override {
        a_.emit(dest, msg);
        b_.emit(dest, msg);
    }

private:
    OutSink& a_;
    OutSink& b_;
};

// Deterministic PRNG shared by the C++ generator and (later) the RTL one.
// xorshift64* — one 64x64 multiply per draw, cheap in DSPs.
struct Rng {
    std::uint64_t s;
    explicit Rng(std::uint64_t seed) : s(seed ? seed : 0x9E3779B97F4A7C15ull) {}
    std::uint64_t next() {
        s ^= s >> 12;
        s ^= s << 25;
        s ^= s >> 27;
        return s * 0x2545F4914F6CDD1Dull;
    }
    // Uniform in [0, n), n > 0. Multiply-shift, no division.
    std::uint32_t below(std::uint32_t n) {
        return std::uint32_t(((next() >> 32) * std::uint64_t(n)) >> 32);
    }
    // True with probability num/1024.
    bool chance(std::uint32_t num_per_1024) { return below(1024) < num_per_1024; }
};

}  // namespace dromos
