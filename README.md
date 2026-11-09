# Fungus

Data-oriented C bot for [gridsnake.com](https://gridsnake.com), a multiplayer snake arena built on
[snake_sim](https://github.com/JesperFritsch/snake_sim) / [snake_arena](https://github.com/JesperFritsch/snake_arena).

## Results (local, real snake_sim vs the previous #1 bot "Rufus")
| Mode | Fungus | Rufus |
|---|---|---|
| 4 players (60 matches) | avg survival rank 1.30, length 56.0 | 2.90, 27.7 |
| 6 players (40 matches) | avg survival rank 1.52, length 65.2 | 3.90, 28.8 |
| Solo mini_items / mini_stairs | length 99.1 / 130.0 | 99.8 / 133.7 |

~0.1–0.25 ms CPU per step locally.

## Main techniques
- **Data-oriented core (C11):** padded grid with a wall border (no bounds checks), one 64-bit word per row as bitboard, `free_at[cell]` = when each body cell frees, ring buffers per snake, generation-stamped scratch, zero allocation per step (~30 KB working set).
- **Fungus flood:** time-aware bitboard flood fill — tail cells join when they free, food on the way delays our own tail, and it reports when a release comes too late (`stuck`).
- **Voronoi safety:** in multiplayer, safety floods only count cells we reach before any opponent.
- **Opponent model:** bodies rebuilt from the map each step; nearby opponents reply with their worst move (best-reply search); Voronoi race decides which food is ours; bonus for trapping.
- **Solo survival proof:** tail-chase certificate + "deficit" BFS (steps still to wait) guiding a short search and a greedy rollout; hunger relaxes it before the W*H/2 no-food end.
- **Thin C++ gRPC shell:** per-decision CPU deadline (2 ms).

## Layout
- `src/brain/fungus_brain.c` — the bot; `src/brain/brain.h` — C contract between shell and brain.
- `src/core/` — board, flood and BFS primitives (+ `test_core.c`, `bench_core.c`).
- `src/shell/` — gRPC shell (`snake_sim.RemoteSnake`) and a pipe shell for the local simulator.
- `tools/fastsim/` — C re-implementation of the snake_sim rules (~40k steps/s) for tuning.
- `tools/` — Docker builds, matches vs Rufus, arena-style scoring.

## Build
    make -j3 build/fastsim build/pipe-fungus build/test_core
    build/test_core
    build/fastsim -m m4 -n 100 -s 1 build/pipe-fungus build/pipe-fungus build/pipe-fungus build/pipe-fungus
    tools/build_arena_image.sh src/brain/fungus_brain.c fungus     # build/arena/csnake-fungus.tar

Matches against Rufus (`tools/vs_rufus.sh`, `tools/run_matches.sh`) need a clone of snake_sim in `vendor/snake_sim` and `tools/build_sim_image.sh`.

Modes: `m4` = 4p 20x15 food 7, `m6` = 6p 20x20 food 10, `s1`/`s2` = solo maps.

## Rules worth knowing (snake_sim defaults the arena inherits)
- Match ends when no alive snake has eaten for W*H/2 steps (150 on 20x15, 200 on 20x20).
- Multi also ends once the last snake standing is longest, + 200 steps.
- Start length 2; snakes close to each other move in separate random-order batches.
