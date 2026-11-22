#!/usr/bin/env bash
# Run scripts/test.sh inside the dromos-sim container (builds the image on first use).
# Works from Linux, WSL or Git Bash on Windows with Docker Desktop.
set -euo pipefail
cd "$(dirname "$0")/.."
IMAGE=${DROMOS_SIM_IMAGE:-dromos-sim}
if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
  docker build -t "$IMAGE" -f docker/Dockerfile.sim .
fi
# Separate build dir so a host build and a container build do not clash.
docker run --rm -v "$PWD":/src -w /src -e HOME=/tmp "$IMAGE" \
  bash -c 'export BUILD=build-docker; scripts/test.sh "$@"' _ "$@"
