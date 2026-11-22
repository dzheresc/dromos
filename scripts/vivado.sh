#!/usr/bin/env bash
# Run a Vivado batch script for one board, natively or in a Docker image,
# pinned to one NUMA node so two builds can run side by side on beast.
#
#   scripts/vivado.sh BOARD SCRIPT [args...]
#     BOARD   ku5p | k7          (directory under boards/)
#     SCRIPT  build.tcl | gen_ip.tcl | program.tcl | ...
#
# Environment:
#   VIVADO_SETTINGS  path to settings64.sh for a native run
#                    (default /opt/Xilinx/Vivado/2017.4/settings64.sh)
#   VIVADO_IMAGE     if set, run inside this Docker image instead
#                    (Vivado expected under the same path inside the image)
#   NUMA_NODE        0 or 1: pin the build to that node (beast: node 0 = CPUs 0-13,
#                    node 1 = CPUs 14-27). Unset = no pinning.
#
# Note: on the current machines only oldgamer's Vivado 2017.4 install has
# UltraScale+ device support; the 2017.4 and 2024.2 installs on the Dell have
# 7-series only (see boards/ku5p/README.md).
set -euo pipefail
cd "$(dirname "$0")/.."

[ $# -ge 2 ] || { sed -n '2,20p' "$0"; exit 2; }
BOARD=$1; SCRIPT=$2; shift 2
DIR=boards/$BOARD
[ -f "$DIR/$SCRIPT" ] || { echo "no $DIR/$SCRIPT" >&2; exit 2; }

SETTINGS=${VIVADO_SETTINGS:-/opt/Xilinx/Vivado/2017.4/settings64.sh}
CPUS=""; MEMS=""
case "${NUMA_NODE:-}" in
  0) CPUS=0-13;  MEMS=0 ;;
  1) CPUS=14-27; MEMS=1 ;;
  "") ;;
  *) echo "NUMA_NODE must be 0 or 1" >&2; exit 2 ;;
esac
TCLARGS=""
[ $# -gt 0 ] && TCLARGS="-tclargs $*"
VCMD="source '$SETTINGS' && cd '$DIR' && vivado -mode batch -nojournal -notrace -source '$SCRIPT' $TCLARGS"

if [ -n "${VIVADO_IMAGE:-}" ]; then
  PIN=()
  [ -n "$CPUS" ] && PIN=(--cpuset-cpus "$CPUS" --cpuset-mems "$MEMS")
  exec docker run --rm "${PIN[@]}" -v "$PWD":/work -w /work \
    "$VIVADO_IMAGE" bash -lc "$VCMD"
else
  if [ -n "$CPUS" ] && command -v numactl >/dev/null; then
    exec numactl --cpunodebind="$MEMS" --membind="$MEMS" bash -lc "$VCMD"
  fi
  exec bash -lc "$VCMD"
fi
