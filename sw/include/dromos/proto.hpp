// Dromos wire protocol: fixed 32-byte little-endian messages.
//
// This header is the single source of truth for message layout. The RTL field
// map (rtl/common/dromos_pkg.sv) mirrors the offsets below; tools/gen_rtl_pkg
// will generate it once the layout settles. Keep the static_asserts.
#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace dromos::proto {

inline constexpr std::size_t kMsgSize = 32;
inline constexpr std::size_t kMaxMsgsPerPacket = 32;
inline constexpr std::uint16_t kOrderPort = 30000;
inline constexpr std::uint16_t kFeedPort = 30001;
inline constexpr std::uint8_t kVersion = 1;

enum class MsgType : std::uint8_t {
    // client -> engine
    NewOrder = 0x01,
    Cancel = 0x02,
    Reduce = 0x03,
    // engine -> client (unicast)
    Accepted = 0x81,
    Rejected = 0x82,
    Fill = 0x83,
    Cancelled = 0x84,
    // engine -> feed (multicast)
    MdAdd = 0xC1,
    MdExecute = 0xC2,
    MdDelete = 0xC3,
    Heartbeat = 0xC4,
};

enum class Side : std::uint8_t { Buy = 0, Sell = 1 };
enum class Tif : std::uint8_t { Day = 0, Ioc = 1 };
enum class Liquidity : std::uint8_t { Maker = 0, Taker = 1 };

enum class RejectReason : std::uint8_t {
    None = 0,
    UnknownSymbol = 1,
    PriceOutOfWindow = 2,
    BadQty = 3,
    BadType = 4,
    UnknownHandle = 5,
    PoolFull = 6,
    BadReduce = 7,
    BadSide = 8,
    BadTif = 9,
};

enum class CancelReason : std::uint8_t { User = 0, Ioc = 1, Kill = 2, Reduced = 3 };

// Order handle: engine-assigned. Bits [15:0] pool slot, [23:16] generation.
using Handle = std::uint64_t;
inline constexpr Handle make_handle(std::uint32_t slot, std::uint8_t gen) {
    return (Handle(gen) << 16) | Handle(slot & 0xFFFF);
}
inline constexpr std::uint32_t handle_slot(Handle h) { return std::uint32_t(h & 0xFFFF); }
inline constexpr std::uint8_t handle_gen(Handle h) { return std::uint8_t((h >> 16) & 0xFF); }
inline constexpr bool handle_well_formed(Handle h) { return (h >> 24) == 0; }

#pragma pack(push, 1)

struct PacketHeader {          // 16 bytes
    std::uint8_t version;      // 0
    std::uint8_t msg_count;    // 1
    std::uint16_t session_id;  // 2
    std::uint32_t reserved;    // 4
    std::uint64_t first_seq;   // 8
};

// ---- inbound -------------------------------------------------------------

struct NewOrder {                  // 0x01
    MsgType type;                  // 0
    Side side;                     // 1
    std::uint16_t symbol;          // 2
    std::uint32_t price_ticks;     // 4
    std::uint32_t qty;             // 8
    Tif tif;                       // 12
    std::uint8_t pad0[3];          // 13
    std::uint64_t client_order_id; // 16
    std::uint8_t pad1[8];          // 24
};

struct Cancel {                    // 0x02
    MsgType type;                  // 0
    std::uint8_t pad0;             // 1
    std::uint16_t symbol;          // 2
    std::uint32_t pad1;            // 4
    Handle order_handle;           // 8
    std::uint64_t client_order_id; // 16
    std::uint8_t pad2[8];          // 24
};

struct Reduce {                    // 0x03
    MsgType type;                  // 0
    std::uint8_t pad0;             // 1
    std::uint16_t symbol;          // 2
    std::uint32_t new_qty;         // 4
    Handle order_handle;           // 8
    std::uint64_t client_order_id; // 16
    std::uint8_t pad1[8];          // 24
};

// ---- outbound, unicast ---------------------------------------------------

struct Accepted {                  // 0x81
    MsgType type;                  // 0
    Side side;                     // 1
    std::uint16_t symbol;          // 2
    std::uint32_t price_ticks;     // 4
    Handle order_handle;           // 8
    std::uint64_t client_order_id; // 16
    std::uint64_t timestamp_ns;    // 24
};

struct Rejected {                  // 0x82
    MsgType type;                  // 0
    RejectReason reason;           // 1
    std::uint16_t symbol;          // 2
    std::uint8_t orig_type;        // 4
    std::uint8_t pad0[3];          // 5
    std::uint64_t pad1;            // 8
    std::uint64_t client_order_id; // 16
    std::uint64_t timestamp_ns;    // 24
};

struct Fill {                      // 0x83
    MsgType type;                  // 0
    Liquidity liquidity;           // 1
    std::uint16_t symbol;          // 2
    std::uint32_t price_ticks;     // 4
    Handle order_handle;           // 8
    std::uint32_t match_id;        // 16
    std::uint32_t fill_qty;        // 20
    std::uint32_t leaves_qty;      // 24
    std::uint32_t pad0;            // 28
};

struct Cancelled {                 // 0x84
    MsgType type;                  // 0
    CancelReason reason;           // 1
    std::uint16_t symbol;          // 2
    std::uint32_t cancelled_qty;   // 4
    Handle order_handle;           // 8
    std::uint64_t client_order_id; // 16
    std::uint64_t timestamp_ns;    // 24
};

// ---- outbound, market data feed -----------------------------------------

struct MdAdd {                     // 0xC1
    MsgType type;                  // 0
    Side side;                     // 1
    std::uint16_t symbol;          // 2
    std::uint32_t price_ticks;     // 4
    Handle order_handle;           // 8
    std::uint32_t qty;             // 16
    std::uint32_t pad0;            // 20
    std::uint64_t timestamp_ns;    // 24
};

struct MdExecute {                 // 0xC2
    MsgType type;                  // 0
    Side resting_side;             // 1
    std::uint16_t symbol;          // 2
    std::uint32_t price_ticks;     // 4
    Handle resting_handle;         // 8
    std::uint32_t qty;             // 16
    std::uint32_t match_id;        // 20
    std::uint64_t timestamp_ns;    // 24
};

struct MdDelete {                  // 0xC3 (remaining_qty == 0: delete, else reduce)
    MsgType type;                  // 0
    Side side;                     // 1
    std::uint16_t symbol;          // 2
    std::uint32_t remaining_qty;   // 4
    Handle order_handle;           // 8
    std::uint64_t pad0;            // 16
    std::uint64_t timestamp_ns;    // 24
};

struct Heartbeat {                 // 0xC4
    MsgType type;                  // 0
    std::uint8_t pad0[23];         // 1
    std::uint64_t timestamp_ns;    // 24
};

#pragma pack(pop)

// One raw 32-byte message, used on the wire and in output streams.
struct Msg {
    std::uint8_t bytes[kMsgSize];

    MsgType type() const { return MsgType(bytes[0]); }

    template <class T>
    T as() const {
        static_assert(sizeof(T) == kMsgSize);
        T t;
        std::memcpy(&t, bytes, kMsgSize);
        return t;
    }

    template <class T>
    static Msg from(const T& t) {
        static_assert(sizeof(T) == kMsgSize);
        Msg m;
        std::memcpy(m.bytes, &t, kMsgSize);
        return m;
    }

    bool operator==(const Msg& o) const { return std::memcmp(bytes, o.bytes, kMsgSize) == 0; }
};

// Zero-initialised message of a given layout with its type byte set.
template <class T>
inline T blank(MsgType t) {
    T v;
    std::memset(&v, 0, sizeof(v));
    v.type = t;
    return v;
}

static_assert(sizeof(PacketHeader) == 16);
static_assert(sizeof(NewOrder) == kMsgSize);
static_assert(sizeof(Cancel) == kMsgSize);
static_assert(sizeof(Reduce) == kMsgSize);
static_assert(sizeof(Accepted) == kMsgSize);
static_assert(sizeof(Rejected) == kMsgSize);
static_assert(sizeof(Fill) == kMsgSize);
static_assert(sizeof(Cancelled) == kMsgSize);
static_assert(sizeof(MdAdd) == kMsgSize);
static_assert(sizeof(MdExecute) == kMsgSize);
static_assert(sizeof(MdDelete) == kMsgSize);
static_assert(sizeof(Heartbeat) == kMsgSize);
static_assert(sizeof(Msg) == kMsgSize);
static_assert(offsetof(NewOrder, client_order_id) == 16);
static_assert(offsetof(Fill, leaves_qty) == 24);
static_assert(offsetof(MdExecute, match_id) == 20);

// The engine only ever runs on little-endian hosts; the wire is little-endian.
static_assert(std::endian::native == std::endian::little, "Dromos assumes a little-endian host");

}  // namespace dromos::proto
