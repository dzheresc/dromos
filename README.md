# Dromos: an exchange matching engine in FPGA fabric

Dromos is a hobby-scale, open prototype of exchange infrastructure where the order book and the matching engine run entirely in FPGA logic. Orders arrive over 10G Ethernet, are matched in price-time priority in on-chip memory, and fills and market data leave on the same wire, with a target of under 500 ns from order in to reply out.

Every result is checked against a C++ reference engine. The hardware is right only when it produces the same bytes as the software for the same input.

## What it does today

The C++ reference engine, its cross-check model and the seeded order generator are in place (milestone M1), with the first RTL block (`book_bitmap`, best-price search) passing its Verilator testbench. The boards come next.

- [Design plan](docs/design-plan.md): architecture, protocol, book structures, milestones
- [Building and testing](docs/BUILDING.md)
- [Boards](boards/README.md)

## How it works

```
  order generator          order generator
  (Kintex-7 325T)          (Kintex-7 325T)
        |   \                /   |
     10G UDP  \            /  10G UDP
        |       \        /       |
  +-----------+   \    /   +-----------+
  | engine A  |     \/     | engine B  |
  | KU5P      |            | KU5P      |
  | shard A   |            | shard B   |
  +-----------+            +-----------+
        \                       /
       10G  host client and capture  10G
          (ConnectX-3, C++ client)
```

Every link is a point-to-point DAC, no switch: each engine's QSFP+ port fans out through a 40G-to-4x10G breakout.

- **Book in on-chip RAM.** Each symbol has a fixed window of 4,096 price levels per side, indexed directly by tick. Resting orders live in a 64K-entry order pool in UltraRAM, linked into a FIFO per price level.
- **Best price in two cycles.** A bitmap of non-empty levels and two priority encoders find the next best price when a level empties.
- **Deterministic.** One ingress arbiter fixes the order of all messages. The same input always gives the same output, which makes testing, replay and hot replicas straightforward.
- **Simple wire protocol.** Fixed 32-byte binary messages over UDP: new order, cancel, reduce in; accepted, rejected, fill, cancelled out; an order-by-order market data feed on multicast.

## Hardware

| Board | Role |
| --- | --- |
| 2x Xilinx Kintex UltraScale+ XCKU5P | Matching engines, one symbol shard each; later primary and hot replica |
| 2x Xilinx Kintex-7 XC7K325T | Order flow generators and latency probes, market data capture |
| Kintex UltraScale+ XCKU3P (25G) | Spare, later a 25G port or a sequencer |

## Verification

- `refengine`: a C++20 reference engine with the same data structures as the RTL, plus a naive second model to check it.
- `orderflow`: a seeded order generator, written once in C++ and once in RTL, so a seed fully defines a hardware run.
- Verilator testbenches link `refengine` in-process and compare every message as the RTL produces it.
- On hardware, the generators keep a CRC-64 of everything they receive and compare it with the reference engine's digest.

## Roadmap

1. C++ reference engine and order generator
2. Book core in Verilator, matching the reference
3. 10G link bring-up between the boards
4. One engine, one symbol on hardware, under 1 µs wire to wire
5. Two engines, 16 symbols, p99 under 500 ns
6. Hot replica with failover

## History

Dromos began as an earlier, simpler take on this idea. This is a fresh start built around what FPGAs now make practical: matching in hardware with nanosecond latency, on boards a single developer can own.
