# Board designs

Each board directory starts from a design already proven on hardware, imported
unchanged from `C:\develop` on 2026-11-22. They give every board the PCIe
endpoint used for programming and configuration (BAR0 registers, QSPI flash
programmer, warm reboot) before any Dromos logic is added.

| Dir | Board | Imported from | What works today |
| --- | --- | --- | --- |
| `ku5p/` | XCKU5P engines (both boards) | `ku5p_pcie` | PCIe (Gen1 x8 link; Gen3 EQ does not complete), BAR0, flash read/write over PCIe, ICAP reboot |
| `k7/` | xc7k325t generators (both boards) | `k7_udp` (k7_nic + UDP) | PCIe Gen2 x1, flash programmer, 10GBASE-R on both SFP+ cages, UDP/IP stack (verilog-ethernet) on SFP A |

Each keeps its own README with pins, clocks, register map and bring-up notes.

## Plan for these directories

1. **M0**: build both unchanged through `scripts/vivado.sh` (or `scripts/vivado.ps1` for
   the K7 on the Dell) and confirm the bitstreams match the known-good builds.
2. **KU5P**: add the 10GBASE-R PHY on the QSFP+ lanes (GTY, 4 x 10G breakout), then the
   Dromos engine from `rtl/engine/` behind the UDP RX/TX path. Engine registers go in
   BAR0 above the flash block (0x100 onward), next to the existing map.
3. **K7**: replace `udp_counter` with the Dromos order generator (`rtl/gen/`, seeded
   xorshift64* identical to `sw/include/dromos/common.hpp::Rng`), per-stream CRC-64
   digests and TX/RX timestamps, readable over BAR0.

## Open points

- The KU5P design trains at x8 (Gen1). The plan asked for PCIe x1 everywhere for
  control; x8 already works, so it stays until there is a reason to change it.
  `build.tcl` takes `-generic LANES=` if x1 is wanted.
- Vivado: UltraScale+ device support exists only in oldgamer's 2017.4 install, so
  KU5P builds run there (or in a Docker image made from that install).
