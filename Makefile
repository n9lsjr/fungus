# Host builds for local tuning (fastsim + pipe bots). The gRPC binary is built
# in Docker by tools/build_bot.sh. Example: make -j3 build/fastsim build/pipe-fungus
CC ?= gcc
CFLAGS ?= -std=c11 -O2 -march=x86-64-v3 -Wall -Wextra -g
INC = -Isrc/brain -Isrc/core -Isrc/shell
CORE_SRC = $(wildcard src/core/*.c)
CORE_LIB = $(filter-out src/core/bench_core.c src/core/test_core.c,$(CORE_SRC))

build/fastsim: tools/fastsim/fastsim.c src/shell/pipe_proto.h src/brain/brain.h | build
	$(CC) $(CFLAGS) $(INC) -o $@ $<

build/pipe-%: src/brain/brain_%.c src/shell/pipe_main.c $(CORE_LIB) $(wildcard src/core/*.h) | build
	$(CC) $(CFLAGS) $(INC) -o $@ $< src/shell/pipe_main.c $(CORE_LIB) -lm

build/pipe-fungus: src/brain/fungus_brain.c src/shell/pipe_main.c $(CORE_LIB) $(wildcard src/core/*.h) | build
	$(CC) $(CFLAGS) $(INC) -o $@ $< src/shell/pipe_main.c $(CORE_LIB) -lm

build/test_core: src/core/test_core.c $(CORE_LIB) | build
	$(CC) $(CFLAGS) -fsanitize=address,undefined $(INC) -o $@ $^ -lm

build/bench_core: src/core/bench_core.c $(CORE_LIB) | build
	$(CC) $(CFLAGS) -O3 $(INC) -o $@ $^ -lm

build:
	mkdir -p build

fastsim: build/fastsim
pipe-%: build/pipe-%
	@true
.PHONY: fastsim
