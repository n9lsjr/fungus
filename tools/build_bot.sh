#!/usr/bin/env bash
# Build the gRPC bot binary inside a capped Debian container (same distro as
# the arena's cpp sandbox). usage: tools/build_bot.sh <brain.c> [name]
# Output: build/csnake-<name>
set -euo pipefail
cd "$(dirname "$0")/.."
brain=$1; name=${2:-$(basename "$brain" .c)}
# MARCH: x86-64-v3 for local runs; tools/build_arena_image.sh uses x86-64-v2 (no AVX2)
MARCH=${MARCH:--march=x86-64-v3}
if ! docker image inspect csnake-dev >/dev/null 2>&1; then
  docker rm -f csnake-dev-build >/dev/null 2>&1 || true
  docker run --name csnake-dev-build --memory 3g --cpus 3 debian:bookworm bash -c '
    apt-get update -qq && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends \
      g++ make pkg-config libgrpc++-dev libprotobuf-dev protobuf-compiler-grpc protobuf-compiler >/dev/null'
  docker commit csnake-dev-build csnake-dev >/dev/null
  docker rm csnake-dev-build >/dev/null
fi
mkdir -p build/proto
docker run --rm --user "$(id -u):$(id -g)" --memory 3g --memory-swap 3g --cpus 3 \
  -v "$PWD:/w" -w /w csnake-dev bash -c "
  set -e
  if [ ! -f build/proto/sim_interface.grpc.pb.o ]; then
    protoc --proto_path=docs --cpp_out=build/proto --grpc_out=build/proto \
      --plugin=protoc-gen-grpc=/usr/bin/grpc_cpp_plugin docs/sim_interface.proto
    g++ -std=c++17 -O2 -Ibuild/proto -c build/proto/sim_interface.pb.cc -o build/proto/sim_interface.pb.o
    g++ -std=c++17 -O2 -Ibuild/proto -c build/proto/sim_interface.grpc.pb.cc -o build/proto/sim_interface.grpc.pb.o
  fi
  [ build/proto/server.o -nt src/shell/server.cpp ] && [ build/proto/server.o -nt src/brain/brain.h ] || \
    g++ -std=c++17 -O2 -Ibuild/proto -Isrc/brain -c src/shell/server.cpp -o build/proto/server.o
  gcc -std=c11 -O2 $MARCH -Wall -Wextra -Isrc/brain -Isrc/core -c $brain -o build/brain_$name.o && for f in src/core/board.c src/core/flood.c; do gcc -std=c11 -O2 $MARCH -Wall -Wextra -Isrc/core -c \$f -o build/core_\$(basename \$f .c).o; done
  g++ -o build/csnake-$name build/proto/server.o build/brain_$name.o build/core_board.o build/core_flood.o \
    build/proto/sim_interface.pb.o build/proto/sim_interface.grpc.pb.o \$(pkg-config --libs grpc++ protobuf)
  echo built build/csnake-$name
"
