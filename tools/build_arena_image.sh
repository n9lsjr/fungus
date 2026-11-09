#!/usr/bin/env bash
# Build the uploadable arena image for a brain.
# usage: tools/build_arena_image.sh <brain.c> <name>
# -> docker image csnake-arena:<name> and build/arena/csnake-<name>.tar (docker save)
set -euo pipefail
cd "$(dirname "$0")/.."
brain=$1; name=$2
# baseline x86-64 with SSE4.2/POPCNT, no AVX2: we don't know the arena host's CPU
MARCH="-march=x86-64-v2 -mtune=generic" tools/build_bot.sh "$brain" "arena-$name"
mkdir -p build/arena/ctx
cp "build/csnake-arena-$name" build/arena/ctx/csnake
docker build -q -t "csnake-arena:$name" -f docker/arena.Dockerfile build/arena/ctx
docker save "csnake-arena:$name" -o "build/arena/csnake-$name.tar"
ls -la "build/arena/csnake-$name.tar"
