/*
 * board.h -- padded flat grid, per-row bitboards, ordered snake bodies.
 *
 * Layout (see docs/core_design.md):
 *   - fixed padded grid: 34 columns x 34 rows, cell = row*34 + col,
 *     game cell (x,y) lives at cell_of(x,y) = (y+1)*34 + (x+1).
 *     The one-cell border is permanent wall, so neighbour = cell +-1 / +-34
 *     never needs a bounds check.
 *   - bitboards: one uint64_t per padded row, bit = padded column.
 *     Horizontal shifts stay inside a word, vertical shifts are row index +-1.
 *   - snakes: ring buffer of uint16_t cells, head first.
 *   - free_at[cell]: earliest move number t (1 = our next move) at which a
 *     cell may be entered. 0 = free now, FREE_NEVER = wall / dead body.
 */
#ifndef CSNAKE_CORE_BOARD_H
#define CSNAKE_CORE_BOARD_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#define BSTRIDE 34
#define BROWS 34
#define BCELLS (BSTRIDE * BROWS) /* 1156 */
#define MAX_W 32
#define MAX_H 32
#define MAX_SNAKES 8
#define RING_SIZE 1024 /* power of two, > longest possible body + search depth */
#define RING_MASK (RING_SIZE - 1)
#define CELL_NONE 0xFFFFu
#define FREE_NEVER 0xFFFFu

/* grid cell values */
enum { CELL_FREE = 0, CELL_FOOD = 1, CELL_WALL = 2, CELL_BODY = 16, CELL_HEAD = 32 };

static inline int cell_owner(uint8_t v) { return v & 15; }
static inline bool cell_is_snake(uint8_t v) { return v >= CELL_BODY; }
static inline bool cell_is_head(uint8_t v) { return v >= CELL_HEAD; }
static inline bool cell_passable(uint8_t v) { return v < CELL_WALL; }

static inline uint16_t cell_of(int x, int y) { return (uint16_t)((y + 1) * BSTRIDE + (x + 1)); }
static inline int cell_x(uint16_t c) { return (int)(c % BSTRIDE) - 1; }
static inline int cell_y(uint16_t c) { return (int)(c / BSTRIDE) - 1; }
static inline int cell_row(uint16_t c) { return (int)(c / BSTRIDE); }
static inline int cell_col(uint16_t c) { return (int)(c % BSTRIDE); }
/* checkerboard colour: 0 = "black" ((x+y) even), 1 = "white" */
static inline int cell_parity(uint16_t c) { return (cell_row(c) + cell_col(c)) & 1; }
static inline int cell_manhattan(uint16_t a, uint16_t b)
{
    int dx = cell_col(a) - cell_col(b), dy = cell_row(a) - cell_row(b);
    return (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
}

/* directions use the simulator's convention: up = (0,-1) */
enum { DIR_UP = 0, DIR_RIGHT = 1, DIR_DOWN = 2, DIR_LEFT = 3 };
static inline int dir_off(int d)
{
    static const int16_t t[4] = { -BSTRIDE, 1, BSTRIDE, -1 };
    return t[d];
}
static inline int dir_dx(int d)
{
    static const int8_t t[4] = { 0, 1, 0, -1 };
    return t[d];
}
static inline int dir_dy(int d)
{
    static const int8_t t[4] = { -1, 0, 1, 0 };
    return t[d];
}
/* direction from a to an adjacent cell b, or -1 */
static inline int dir_between(uint16_t a, uint16_t b)
{
    int d = (int)b - (int)a;
    return d == -BSTRIDE ? DIR_UP : d == 1 ? DIR_RIGHT : d == BSTRIDE ? DIR_DOWN : d == -1 ? DIR_LEFT : -1;
}

/* ------------------------------------------------------------------ */
/* bitboard: one word per padded row                                   */
/* ------------------------------------------------------------------ */
typedef struct {
    uint64_t r[BROWS];
} BB;

static inline void bb_clear(BB *b) { memset(b, 0, sizeof *b); }
static inline void bb_set(BB *b, uint16_t c) { b->r[cell_row(c)] |= (uint64_t)1 << cell_col(c); }
static inline void bb_reset(BB *b, uint16_t c) { b->r[cell_row(c)] &= ~((uint64_t)1 << cell_col(c)); }
static inline bool bb_test(const BB *b, uint16_t c) { return (b->r[cell_row(c)] >> cell_col(c)) & 1; }
static inline bool bb_empty(const BB *b)
{
    uint64_t acc = 0;
    for (int i = 0; i < BROWS; i++) acc |= b->r[i];
    return acc == 0;
}
static inline int bb_count(const BB *b)
{
    int n = 0;
    for (int i = 0; i < BROWS; i++) n += __builtin_popcountll(b->r[i]);
    return n;
}
static inline int bb_count_and(const BB *a, const BB *b)
{
    int n = 0;
    for (int i = 0; i < BROWS; i++) n += __builtin_popcountll(a->r[i] & b->r[i]);
    return n;
}
/* out = a | neighbours(a), for game rows 1..H. Rows 0 and H+1 are left zero. */
static inline void bb_expand(const BB *a, BB *out, int H)
{
    out->r[0] = 0;
    for (int i = 1; i <= H; i++) {
        uint64_t x = a->r[i];
        out->r[i] = x | (x << 1) | (x >> 1) | a->r[i - 1] | a->r[i + 1];
    }
    for (int i = H + 1; i < BROWS; i++) out->r[i] = 0;
}
/* passable cells whose two horizontal or two vertical neighbours are both
 * blocked: one-wide corridors / gateways. */
static inline void bb_narrow(const BB *pass, BB *out, int H)
{
    out->r[0] = 0;
    for (int i = 1; i <= H; i++) {
        uint64_t p = pass->r[i], np = ~p;
        out->r[i] = p & (((np << 1) & (np >> 1)) | (~pass->r[i - 1] & ~pass->r[i + 1]));
    }
    for (int i = H + 1; i < BROWS; i++) out->r[i] = 0;
}

/* ------------------------------------------------------------------ */
/* snakes and board                                                    */
/* ------------------------------------------------------------------ */
typedef struct {
    uint16_t ring[RING_SIZE]; /* head at ring[head], tail at ring[(head+len-1)&RING_MASK] */
    uint16_t head;
    uint16_t len;
    uint16_t moves;      /* hypothetical moves applied with board_apply */
    uint8_t alive;
    uint8_t slack;       /* move-order slack added to free_at (1 self, opp_slack others) */
    uint8_t food_slack;  /* 1 if opponent head touches food (it may grow, tail waits) */
    uint8_t head_value;  /* simulator map values */
    uint8_t body_value;
    uint8_t suspect;     /* body order could not be verified against the map */
    int32_t id;          /* simulator snake id */
} Snake;

typedef struct {
    int W, H;
    int nsnakes, self;
    int opp_slack;            /* 0: assume opponents move before us, 1: after us (conservative) */
    uint32_t step;            /* number of board_update calls */
    uint8_t free_value, food_value, blocked_value;
    uint8_t val2snake[256];   /* 0xFF = not a snake value, else index | 0x80 if head value */
    uint8_t grid[BCELLS];
    uint16_t free_at[BCELLS];
    BB pass;   /* free or food right now */
    BB food;
    BB ever;   /* not wall and not dead body: may become passable */
    BB black;  /* checkerboard colour 0 */
    Snake s[MAX_SNAKES];
    uint16_t mark[BCELLS]; /* generation-stamped scratch */
    uint16_t gen;
    /* scratch for the (rare) body repair DFS; kept here so there are no
     * file-scope globals and several boards can coexist */
    uint16_t rep_path[RING_SIZE], rep_best[RING_SIZE];
    uint8_t rep_nd[RING_SIZE];
} Board;

typedef struct {
    int width, height;
    uint8_t free_value, food_value, blocked_value;
    int snake_count;
    int self_index;
    int opp_slack;
    int32_t ids[MAX_SNAKES];
    uint8_t head_value[MAX_SNAKES], body_value[MAX_SNAKES];
    int16_t start_x[MAX_SNAKES], start_y[MAX_SNAKES];
    const uint8_t *base_map; /* W*H row-major in simulator values, or NULL */
} BoardInit;

typedef struct {
    uint16_t new_head;
    uint16_t old_tail;
    uint16_t old_tail_free_at;
    uint8_t snake;
    uint8_t ate;
} MoveUndo;

void board_init(Board *b, const BoardInit *in);

/* Per-step update from the simulator's map (W*H row-major, simulator
 * values), alive[] and length[] indexed by our snake index (length =
 * number of body cells, as reported by the simulator). Food locations are
 * redundant with the map and not needed. Returns the number of snakes
 * whose body order had to be repaired (0 = tracking agreed with the map),
 * or -1 if the map does not match the init data. */
int board_update(Board *b, const uint8_t *map, const uint8_t *alive, const int *length);

/* Recompute grid snake cells, bitboards and free_at from the rings. */
void board_refresh(Board *b);

/* Test/bench helper: set a body (head first) and mark the snake alive.
 * Call board_refresh afterwards. */
void board_set_body(Board *b, int s, const uint16_t *cells, int n);

/* Hypothetical move for any snake with O(1) incremental updates (O(len) when
 * eating). Returns false and changes nothing if the move is illegal. */
bool board_apply(Board *b, int s, int dir, MoveUndo *u);
void board_undo(Board *b, const MoveUndo *u);

/* cells adjacent to alive opponent heads: an opponent moving before us may
 * take them this step. */
void board_threat(const Board *b, int self, BB *out);

static inline uint16_t board_head(const Board *b, int s) { return b->s[s].ring[b->s[s].head]; }
static inline uint16_t board_tail(const Board *b, int s)
{
    const Snake *n = &b->s[s];
    return n->ring[(n->head + n->len - 1) & RING_MASK];
}
/* i-th body cell from the head (0 = head) */
static inline uint16_t board_body(const Board *b, int s, int i)
{
    return b->s[s].ring[(b->s[s].head + i) & RING_MASK];
}
static inline uint16_t board_target(const Board *b, int s, int d)
{
    return (uint16_t)((int)board_head(b, s) + dir_off(d));
}
static inline bool board_legal(const Board *b, int s, int d)
{
    return cell_passable(b->grid[board_target(b, s, d)]);
}
/* total slack of a snake as used in free_at */
static inline int board_slack(const Board *b, int s)
{
    return b->s[s].slack + b->s[s].food_slack;
}
/* bump the scratch generation (handles wrap) */
static inline uint16_t board_next_gen(Board *b)
{
    if (++b->gen == 0) {
        memset(b->mark, 0, sizeof b->mark);
        b->gen = 1;
    }
    return b->gen;
}

#endif
