#!/usr/bin/env bash
# Builds the local snake_sim image (Python 3.12, no torch) with capped resources:
# install runs in a memory/CPU limited `docker run`, then gets committed.
set -euo pipefail
cd "$(dirname "$0")/.."
docker rm -f csnake-sim-build >/dev/null 2>&1 || true
docker run --name csnake-sim-build --memory 4g --memory-swap 4g --cpus 3 \
  -v "$PWD/vendor/snake_sim:/src:ro" python:3.12-slim bash -c '
    set -e
    apt-get update -qq && apt-get install -y -qq --no-install-recommends git g++ >/dev/null
    cp -r /src /build && cd /build
    printf "\n[build_ext]\nparallel = 3\n" >> setup.cfg
    pip install -q --no-cache-dir . 
    python -c "import snake_sim.cpp_bindings.area_check as a; print(\"ok\", a)"
  '
docker commit csnake-sim-build csnake-sim:latest >/dev/null
docker rm csnake-sim-build >/dev/null
echo "built csnake-sim:latest"
