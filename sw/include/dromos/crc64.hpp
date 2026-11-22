// CRC-64/ECMA-182 (poly 0x42F0E1EBA9EA3693, init 0, no reflection, no xorout).
// Chosen because it is trivial to build as an LFSR in fabric. The 325Ts keep
// one of these per stream they receive; the harness computes the same.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>

#include "dromos/common.hpp"

namespace dromos {

class Crc64 {
public:
    static constexpr std::uint64_t kPoly = 0x42F0E1EBA9EA3693ull;

    void update(const std::uint8_t* p, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i)
            crc_ = table()[((crc_ >> 56) ^ p[i]) & 0xFF] ^ (crc_ << 8);
    }
    std::uint64_t value() const { return crc_; }

private:
    static const std::array<std::uint64_t, 256>& table() {
        static const std::array<std::uint64_t, 256> t = [] {
            std::array<std::uint64_t, 256> r{};
            for (std::uint64_t i = 0; i < 256; ++i) {
                std::uint64_t c = i << 56;
                for (int b = 0; b < 8; ++b)
                    c = (c & (1ull << 63)) ? (c << 1) ^ kPoly : (c << 1);
                r[i] = c;
            }
            return r;
        }();
        return t;
    }
    std::uint64_t crc_ = 0;
};

// One CRC-64 per destination stream (each session and the feed), over the
// 32-byte messages in the order they were emitted.
class DigestSink final : public OutSink {
public:
    void emit(Dest dest, const proto::Msg& msg) override {
        auto& d = digests[dest];
        d.crc.update(msg.bytes, proto::kMsgSize);
        ++d.count;
    }
    struct Digest {
        Crc64 crc;
        std::uint64_t count = 0;
    };
    std::map<Dest, Digest> digests;
};

}  // namespace dromos
