# Building and testing

## Layout

| Path | What |
| --- | --- |
| `sw/include/dromos/proto.hpp` | Wire protocol: fixed 32-byte messages. Single source of truth for layouts |
| `sw/include/dromos/refengine.hpp`, `sw/src/refengine.cpp` | C++ reference engine, same structures as the RTL book core |
| `sw/src/naive_engine.cpp` | Independent `std::map`/`std::deque` model that cross-checks refengine |
| `sw/src/orderflow.cpp` | Seeded order-flow generator (closed loop on engine output) |
| `sw/tests/test_engine.cpp` | Directed tests, run on both models |
| `sw/tools/crosscheck.cpp` | Random cross-check of both models, prints CRC-64 digests per stream |
| `sw/tools/bench.cpp` | Software throughput baseline |
| `rtl/engine/` | Engine RTL (SystemVerilog). First block: `book_bitmap.sv` |
| `sim/verilator/` | Verilator testbenches, each checked against a C++ model |
| `boards/` | Per-board Vivado projects (PCIe + flash bases imported from proven designs) |
| `scripts/` | `test.sh`, `docker-test.sh`, `vivado.sh` (NUMA-pinned, native or Docker), `vivado.ps1` |
| `docker/Dockerfile.sim` | Ubuntu 24.04 + Verilator 5.030 + CMake/Ninja |

## Quick start

    scripts/test.sh                 # C++ + Verilator tests (Verilator optional)
    scripts/test.sh --asan --long   # plus sanitizers and the 1-billion-message cross-check
    scripts/docker-test.sh          # same, inside the dromos-sim container

    ./build/crosscheck --seeds 10 --msgs 1000000
    ./build/bench 5000000

## FPGA builds

    NUMA_NODE=0 scripts/vivado.sh ku5p gen_ip.tcl     # once
    NUMA_NODE=0 scripts/vivado.sh ku5p build.tcl
    NUMA_NODE=1 scripts/vivado.sh k7 build.tcl
    .\scripts\vivado.ps1 k7 build.tcl                 # on the Dell (7-series only)

Set `VIVADO_IMAGE` to run inside a Docker image, `VIVADO_SETTINGS` to point at a
different install. UltraScale+ (KU5P) builds need oldgamer's Vivado 2017.4.
