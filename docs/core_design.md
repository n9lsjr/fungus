# Core data layout (src/core)

Pure C11, no malloc, no file-scope state: everything lives in a `Board`
(~28 KB, mostly cold ring buffers) and caller-owned result structs, so tests,
search copies and a C simulator can hold several instances.

## Verified rules that shaped the design

* Map values: food 0, free 1, blocked 2, snakes 3,4 / 5,6 / … (lowest unused,
  never hard-coded: `val2snake[256]` from init). Legal iff `value <= free_value`.
* A dead snake is never removed from the map (`SnakeRep.kill()` only flips a
  flag) → its body is a permanent wall. We convert it to `CELL_WALL` on the
  step it dies.
* `SnakeRep.body = deque([start])`, `_length = start_length` (2 in the arena):
  the body is one cell at init and grows by one cell per move until it reaches
  `_length`. The reported `length` is `len(body)`, so tracking only compares
  cell counts; no special case for start_length.
* Batches: snakes whose heads are within Euclidean distance 2 (same set as
  Manhattan ≤ 2) never share a batch; each batch receives a fresh map. Hence
  between two of our observations an opponent has moved 0, 1 or 2 times, and
  our own snake exactly once. `board_update` handles all three (for the
  diagonal 2-move the intermediate corner is the one that is a body cell and
  was not already in our ring).
* No head-on collisions, but a cell next to an opponent head may be taken
  before we move (random batch order). `board_threat()` masks those cells.
* Arena passes only food/grid/map to the sim: no max_steps, no
  max_no_food_steps, food decay 500 (default config). Multi ends 200 steps
  after one snake is alone and longest.

## Grid: padded flat, fixed stride 34

Game cell (x,y) → `cell = (y+1)*34 + (x+1)`; neighbours are `cell ± 1`,
`cell ± 34`, the border ring is wall, so no bounds checks anywhere. The stride
is a compile-time constant (W,H ≤ 32), so `cell/34` and `cell%34` compile to a
multiply+shift (~4 cycles) and all loops are over `H+2` rows only.

Why not 2D (`[y][x]` + bounds checks): 4 compares per neighbour, worse
vectorisation, no benefit. Why not a variable stride: a runtime divide (~25
cycles) in every cell→bit conversion.

Cell encoding (`uint8_t grid[1156]`): `0 free, 1 food, 2 wall, 16+i body of
snake i, 32+i head of snake i`. "Passable" is `v < 2`, owner is `v & 15`,
head is `v >= 32`: single compare / mask on the hot paths.

`uint16_t free_at[1156]`: earliest move number at which the cell may be
entered (1 = our next move). 0 = free now, 0xFFFF = wall. For a body cell with
tail distance k (tail k=1): `free_at = k + slack`, slack = 1 for us (our own
tail is still body when we move), `opp_slack` (0 = "it moves before us",
1 = "after us", chosen per init) + 1 if the opponent's head touches food
(it may grow, tail waits). Dead bodies: 0xFFFF.

## Bitboards: one `uint64_t` per padded row (34 × 8 = 272 B)

Compared alternatives:

| layout | horizontal shift | vertical shift | words/flood-iter |
|---|---|---|---|
| linear u64 array, 1156 bits | cross-word carry (3 ops/word) | shift by 34 bits = word+bit split (4 ops/word) | 19 |
| **row per u64** (chosen) | `r<<1`, `r>>1` inside the word, pad bits absorb overflow | `r[i-1]`, `r[i+1]` — plain loads | 22 (for H=20) |
| row per u32 | same, but W+2 ≤ 34 bits does not fit for W=32 | same | 22 |

Row-per-u64 has no carries at all; one flood layer is
`out[i] = (x | x<<1 | x>>1 | r[i-1] | r[i+1]) & ~x & pass[i]` — 7 ops/row,
and the compiler can do 4 rows per AVX2 op (shifts are per-lane, the ±1 rows
are unaligned loads). `flood_region` also tracks the active row range so a
small pocket touches 3–5 rows per layer rather than 22. Board keeps `pass`
(free|food), `food`, `ever` (not wall: may open later), `black` (parity).

## Snakes: SoA ring buffers

`Snake.ring[1024]` of `uint16_t` cells, head at `ring[head]`, tail at
`ring[(head+len-1)&1023]`. Push-head / pop-tail are O(1), and undo can push
the tail back because head pushes write away from the tail slot. Per snake:
`len, moves (hypothetical), alive, slack, food_slack, suspect`.

## Per step: rebuild the cheap, track the impossible

`board_update(map, alive[], length[])` (~1 µs expected on 20×20):

1. One pass over W·H bytes → grid (dead snakes → wall), head cell and cell
   count per snake.
2. Per alive snake: Manhattan(old head, new head) ∈ {0,1,2} → push 0–2
   cells, pop tails until `len == length[s]`.
3. Verify ring against the grid: count, head, ownership, no duplicates,
   adjacency (gen-stamped `mark[]`, no memset). On any disagreement
   `repair_snake` rebuilds the order from the map alone with a budgeted DFS
   for a simple path of `count` cells from the head (bodies are paths, so this
   is linear except on self-touching folds; `suspect` is set if it could not
   cover every cell).
4. `board_refresh`: pass/food/ever bitboards and free_at from grid + rings
   (ΣW·H + Σlen stores ≈ 700 ops).

Rebuilding grid/bitboards each step is cheaper and safer than incremental
diffs; only body *order* is information the map does not carry, so only that
is tracked.

## apply / undo (search)

`board_apply(s, dir, &undo)`: legality check, grid 2 stores, pass/food bit,
ring push (+ pop), `free_at[new head] = len + slack + moves`, tail cell →
free. O(1), ~20–30 ns. Eating is O(len) because the whole body frees one
move later (`free_at[c]++` for every cell). `board_undo` reverses exactly
(the test suite compares full boards after random apply/undo chains and
checks apply against a real simulator step).

## flood_region (bitboard)

Start cell entered at `t0`; each iteration grows the region by one layer.
With `release`, body cells bordering the region (`ex & ever & ~pass`, scanned
with ctz — typically < 30 bits) join when `free_at <= t+1`; if nothing is
free the loop jumps straight to the smallest pending `free_at` instead of
spinning. The region (not just the frontier) expands each layer — the
standard "the snake could be anywhere in the region by then" approximation.
Output: cells, food, black/white counts, parity path bound
`min(2·n_startcolour, 2·n_other+1)`, layers, arrival time of our own tail,
`self_min_free_at` (Rufus's "max body index": the own body cell on the
border that opens first), optional corridor count (`bb_narrow`: cells with
both horizontal or both vertical neighbours blocked).

Cost: ~7 ops × active rows × layers. Open 20×20: ≈ 25 layers × 22 rows ≈ 4k
simple ops ≈ 0.5–1 µs scalar, ~0.3 µs with AVX2; enclosed pockets far less.

## voronoi_compute (scalar BFS)

Multi-source BFS from all alive heads, FIFO ring queue (2048 u16, never
wraps), gen-stamped visited. Optional tail release (`free_at <= d`). Cells
reached by two snakes in the same layer are `VOR_CONTESTED` and do not
propagate. ~400 pops × 4 neighbours ≈ 1.5–3 µs on 20×20. A bitboard version
(N frontiers) would not be faster for N = 4–6.

## move_eval

Legality, `threatened` (adjacent to an opponent head), static flood, time-aware
flood and `flood_margin` = (path_bound − food) − needed, needed = steps until
the nearest own border cell opens (else len+1). ≈ 2 floods ≈ 1–2 µs.
A 2-ply node (our move × nearby opponent replies × our follow-up, one flood
each) therefore costs tens of µs; `bench_core` measures exactly that shape.

## Cache footprint

Hot per decision: grid 1.2 KB, free_at 2.3 KB, 4 bitboards 1.1 KB, the live
parts of the rings (Σlen·2 B), Voronoi owner/dist/mark 5.8 KB, flood locals
~0.6 KB on the stack → ≈ 12 KB, fits L1D (32–48 KB) with room for the
search's undo records.

## What to port from Rufus's area check

Rufus (`cpp_bindings/area_check`) splits the free space into areas at
"gateways", builds an area graph and searches it for the best
`margin = (tiles − |parity| − jagged) − food − needed`. The parts that matter
for "can I survive in this region":

1. **Parity bound** — correct and essential: a snake path alternates colours,
   so `cells − |black − white|` (Rufus) or our exact `path_bound` is the
   real capacity, not `cells`. We compute it for free from `black` popcounts.
   Rufus's `path_parity_tile_adjustment` (±1 depending on entry/exit colour)
   is subsumed by using the start colour in the bound.
2. **Food costs a step** — eating adds a body cell inside the region:
   `total_steps = bound − food`. Ported in `flood_margin`.
3. **needed_steps** — the region is only a trap if we must stay in it longer
   than it holds: `needed = steps until the first own body cell on the border
   frees`, else `len + 1`. Ported via `self_min_free_at`; the time-aware
   flood answers the same question more directly (`tail_t != 0`).
4. **Gateways / one-dim areas** (`is_gate_way`, `is_one_dim`) — a one-wide
   corridor is not a region you can use twice: its capacity is its length,
   and a 2-cell-wide area entered through a gate on the "wrong" colour loses
   a cell. Rufus's area graph exists to count capacity per sub-area and sum
   along a path. Cheap equivalent on our layout: `corridor` count (narrow
   cells) and, when `corridor` is a large fraction of `cells`, trust
   `layers` (the BFS depth = path length through a corridor) rather than
   `path_bound`. Full articulation-point splitting is not worth it at 1 µs
   budgets; the 2-ply search with the time-aware flood catches most cases
   where a gateway closes behind us.
5. **is_bad_gateway** (stepping through a width-1 gap leaves an unvisitable
   cell beside it) and the **jagged-edge discount** (staircase walls cost
   one cell per step of the staircase) — both are 1–3 cell corrections to
   the bound. Not ported: at `margin` thresholds of 10+ cells they do not
   change decisions, and a 1-ply lookahead with a fresh flood already prices
   the cell we actually lose.
6. `recurse_area_check` (depth-limited recursion over our own next moves,
   margin fraction per depth, `SAFE_MARGIN_FRAC = 0.04`) is what the search
   layer replaces: with apply/undo at ~25 ns and a flood at ~1 µs, a true
   2-ply over nearby opponents' replies is affordable.

## Files

| file | content |
|---|---|
| `board.h` | constants, cell/dir helpers, `BB` bitboard inlines (`bb_expand`, `bb_narrow`), `Snake`, `Board`, `BoardInit`, `MoveUndo`, inline accessors |
| `board.c` | `board_init`, `board_update` (+verify/repair), `board_refresh`, `board_set_body`, `board_apply`/`board_undo`, `board_threat` |
| `flood.h/.c` | `flood_region`, `flood_margin`, `Voronoi`/`voronoi_compute`, `MoveEval`/`move_eval` |
| `sim_util.h` | header-only rule-faithful mini simulator (tests/bench only) |
| `test_core.c` | tracking across 1 and 2 steps per observation, eating/tail release, repair, flood counts/parity/corridor/release timing, voronoi, apply/undo round-trips and apply-vs-real-step agreement |
| `bench_core.c` | ns per primitive on 20×15/4 and 20×20/6 random boards, plus a 2-ply-shaped sample |

Build: `gcc -std=c11 -O2 -Wall -Wextra -march=x86-64-v3 board.c flood.c test_core.c -o test_core`
(same for `bench_core.c`). Only `__builtin_popcountll` / `__builtin_ctzll`
are used, so it also builds without `-march`.
