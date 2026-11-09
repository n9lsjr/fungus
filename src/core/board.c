/*
 * board.c -- init, per-step body tracking, refresh, apply/undo.
 */
#include "board.h"

/* ------------------------------------------------------------------ */
/* ring helpers                                                        */
/* ------------------------------------------------------------------ */
static inline void ring_push_head(Snake *n, uint16_t c)
{
    n->head = (uint16_t)((n->head - 1) & RING_MASK);
    n->ring[n->head] = c;
    n->len++;
}

static inline uint16_t ring_pop_tail(Snake *n)
{
    uint16_t c = n->ring[(n->head + n->len - 1) & RING_MASK];
    n->len--;
    return c;
}

static bool ring_contains(const Snake *n, uint16_t c)
{
    for (int i = 0; i < n->len; i++)
        if (n->ring[(n->head + i) & RING_MASK] == c) return true;
    return false;
}

/* ------------------------------------------------------------------ */
/* init                                                                */
/* ------------------------------------------------------------------ */
void board_init(Board *b, const BoardInit *in)
{
    memset(b, 0, sizeof *b);
    b->W = in->width;
    b->H = in->height;
    b->nsnakes = in->snake_count;
    b->self = in->self_index;
    b->opp_slack = in->opp_slack;
    b->free_value = in->free_value;
    b->food_value = in->food_value;
    b->blocked_value = in->blocked_value;
    b->gen = 1;

    memset(b->val2snake, 0xFF, sizeof b->val2snake);
    for (int s = 0; s < b->nsnakes; s++) {
        b->val2snake[in->head_value[s]] = (uint8_t)(s | 0x80);
        b->val2snake[in->body_value[s]] = (uint8_t)s;
    }

    /* everything wall, then the interior from the base map */
    memset(b->grid, CELL_WALL, sizeof b->grid);
    /* padding never opens; board_refresh only rewrites interior cells */
    for (int c = 0; c < BCELLS; c++) b->free_at[c] = FREE_NEVER;
    for (int y = 0; y < b->H; y++) {
        for (int x = 0; x < b->W; x++) {
            uint8_t v = in->base_map ? in->base_map[y * b->W + x] : in->free_value;
            uint8_t g = CELL_FREE;
            if (v == in->blocked_value) g = CELL_WALL;
            else if (v == in->food_value) g = CELL_FOOD;
            b->grid[cell_of(x, y)] = g;
            if (((x + y) & 1) == 0) bb_set(&b->black, cell_of(x, y));
        }
    }

    for (int s = 0; s < b->nsnakes; s++) {
        Snake *n = &b->s[s];
        n->ring[0] = cell_of(in->start_x[s], in->start_y[s]);
        n->head = 0;
        n->len = 1;
        n->alive = 1;
        n->slack = (uint8_t)(s == b->self ? 1 : in->opp_slack);
        n->head_value = in->head_value[s];
        n->body_value = in->body_value[s];
        n->id = in->ids[s];
    }
    board_refresh(b);
}

void board_set_body(Board *b, int s, const uint16_t *cells, int n)
{
    Snake *sn = &b->s[s];
    /* drop stale cells of this snake from the grid */
    for (int c = 0; c < BCELLS; c++)
        if (cell_is_snake(b->grid[c]) && cell_owner(b->grid[c]) == s) b->grid[c] = CELL_FREE;
    sn->head = 0;
    sn->len = (uint16_t)n;
    for (int i = 0; i < n; i++) sn->ring[i] = cells[i];
    sn->alive = 1;
    sn->moves = 0;
    sn->suspect = 0;
}

/* ------------------------------------------------------------------ */
/* refresh: grid snake cells, bitboards, free_at from the rings        */
/* ------------------------------------------------------------------ */
void board_refresh(Board *b)
{
    const int W = b->W, H = b->H;
    bb_clear(&b->pass);
    bb_clear(&b->food);
    bb_clear(&b->ever);

    /* draw alive snakes over whatever the grid holds (dead ones are already
     * wall, see board_update) */
    for (int s = 0; s < b->nsnakes; s++) {
        Snake *n = &b->s[s];
        if (!n->alive) continue;
        for (int i = 0; i < n->len; i++) b->grid[n->ring[(n->head + i) & RING_MASK]] = (uint8_t)(CELL_BODY + s);
        b->grid[n->ring[n->head]] = (uint8_t)(CELL_HEAD + s);
    }

    for (int y = 1; y <= H; y++) {
        uint64_t pr = 0, fr = 0, er = 0;
        uint8_t *g = b->grid + y * BSTRIDE;
        uint16_t *fa = b->free_at + y * BSTRIDE;
        for (int x = 1; x <= W; x++) {
            uint64_t bit = (uint64_t)1 << x;
            uint8_t v = g[x];
            if (v == CELL_WALL) {
                fa[x] = FREE_NEVER;
            } else if (cell_is_snake(v)) {
                er |= bit;
                fa[x] = FREE_NEVER; /* overwritten below for tracked cells */
            } else {
                pr |= bit;
                er |= bit;
                if (v == CELL_FOOD) fr |= bit;
                fa[x] = 0;
            }
        }
        b->pass.r[y] = pr;
        b->food.r[y] = fr;
        b->ever.r[y] = er;
    }

    for (int s = 0; s < b->nsnakes; s++) {
        Snake *n = &b->s[s];
        if (!n->alive) continue;
        n->food_slack = 0;
        if (s != b->self) {
            uint16_t h = n->ring[n->head];
            for (int d = 0; d < 4; d++)
                if (b->grid[h + dir_off(d)] == CELL_FOOD) n->food_slack = 1;
        }
        int slack = board_slack(b, s) + n->moves;
        for (int i = 0; i < n->len; i++) {
            uint16_t c = n->ring[(n->head + i) & RING_MASK];
            b->free_at[c] = (uint16_t)(n->len - i + slack); /* tail distance k = len - i */
        }
    }
}

/* ------------------------------------------------------------------ */
/* per-step update                                                     */
/* ------------------------------------------------------------------ */

/* Check that the ring agrees with the parsed grid. */
static bool verify_snake(Board *b, int s, uint16_t newhead, int count)
{
    Snake *n = &b->s[s];
    if (n->len != count || n->len == 0) return false;
    if (n->ring[n->head] != newhead) return false;
    uint16_t gen = board_next_gen(b);
    uint16_t prev = CELL_NONE;
    for (int i = 0; i < n->len; i++) {
        uint16_t c = n->ring[(n->head + i) & RING_MASK];
        uint8_t v = b->grid[c];
        if (!cell_is_snake(v) || cell_owner(v) != s) return false;
        if (cell_is_head(v) != (i == 0)) return false;
        if (b->mark[c] == gen) return false; /* duplicate */
        b->mark[c] = gen;
        if (prev != CELL_NONE && cell_manhattan(prev, c) != 1) return false;
        prev = c;
    }
    return true;
}

/* Rebuild the body order from the map alone: DFS for a simple path of
 * `want` cells over the snake's cells starting at the head. Bodies are
 * paths, so this is nearly linear except for self-touching folds. */
static void repair_snake(Board *b, int s, uint16_t head, int want)
{
    uint16_t *path = b->rep_path, *best = b->rep_best;
    uint8_t *nd = b->rep_nd;
    Snake *n = &b->s[s];
    uint16_t gen = board_next_gen(b);
    int depth = 1, bestlen = 1;
    long budget = 50000;

    if (want > RING_SIZE - 1) want = RING_SIZE - 1;
    path[0] = best[0] = head;
    nd[0] = 0;
    b->mark[head] = gen;
    while (depth > 0 && depth < want && budget-- > 0) {
        uint16_t cur = path[depth - 1];
        bool found = false;
        while (nd[depth - 1] < 4) {
            int d = nd[depth - 1]++;
            uint16_t c = (uint16_t)((int)cur + dir_off(d));
            uint8_t v = b->grid[c];
            if (cell_is_snake(v) && cell_owner(v) == s && !cell_is_head(v) && b->mark[c] != gen) {
                b->mark[c] = gen;
                path[depth] = c;
                nd[depth] = 0;
                depth++;
                found = true;
                break;
            }
        }
        if (found) {
            if (depth > bestlen) {
                bestlen = depth;
                memcpy(best, path, (size_t)depth * sizeof best[0]);
            }
        } else {
            depth--;
            if (depth > 0) b->mark[path[depth]] = 0;
        }
    }
    n->head = 0;
    n->len = (uint16_t)bestlen;
    memcpy(n->ring, best, (size_t)bestlen * sizeof best[0]);
    n->suspect = (uint8_t)(bestlen != want);
}

int board_update(Board *b, const uint8_t *map, const uint8_t *alive, const int *length)
{
    const int W = b->W, H = b->H;
    uint16_t newhead[MAX_SNAKES];
    int count[MAX_SNAKES];
    int repaired = 0;

    for (int s = 0; s < b->nsnakes; s++) {
        newhead[s] = CELL_NONE;
        count[s] = 0;
        if (!alive[s]) b->s[s].alive = 0; /* body becomes permanent wall below */
    }

    /* 1. parse the map into the grid; dead snakes become wall */
    for (int y = 0; y < H; y++) {
        const uint8_t *row = map + y * W;
        uint8_t *g = b->grid + (y + 1) * BSTRIDE + 1;
        for (int x = 0; x < W; x++) {
            uint8_t v = row[x];
            if (v == b->free_value) {
                g[x] = CELL_FREE;
            } else if (v == b->food_value) {
                g[x] = CELL_FOOD;
            } else {
                uint8_t t = b->val2snake[v];
                if (t == 0xFF) {
                    g[x] = CELL_WALL;
                } else {
                    int s = t & 15;
                    if (!b->s[s].alive) {
                        g[x] = CELL_WALL;
                    } else {
                        count[s]++;
                        if (t & 0x80) {
                            newhead[s] = (uint16_t)(g + x - b->grid);
                            g[x] = (uint8_t)(CELL_HEAD + s);
                        } else {
                            g[x] = (uint8_t)(CELL_BODY + s);
                        }
                    }
                }
            }
        }
    }

    /* 2. advance each alive ring to the new head; 0, 1 or 2 moves may have
     *    happened since our last observation (batch order varies). */
    for (int s = 0; s < b->nsnakes; s++) {
        Snake *n = &b->s[s];
        n->moves = 0;
        if (!n->alive) continue;
        if (newhead[s] == CELL_NONE) {
            /* alive but no head on the map: treat as dead, wall its cells */
            n->alive = 0;
            for (int c = 0; c < BCELLS; c++)
                if (cell_is_snake(b->grid[c]) && cell_owner(b->grid[c]) == s) b->grid[c] = CELL_WALL;
            repaired++;
            continue;
        }
        uint16_t old = n->ring[n->head], nh = newhead[s];
        int dx = cell_col(nh) - cell_col(old), dy = cell_row(nh) - cell_row(old);
        int m = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
        bool ok = true;

        if (m == 1) {
            ring_push_head(n, nh);
        } else if (m == 2) {
            if (dx == 0 || dy == 0) {
                ring_push_head(n, (uint16_t)(((int)old + (int)nh) / 2));
            } else {
                /* diagonal: the intermediate cell is one of the two corners;
                 * it was free when the snake passed, so it is not in the ring */
                uint16_t c1 = (uint16_t)(cell_row(old) * BSTRIDE + cell_col(nh));
                uint16_t c2 = (uint16_t)(cell_row(nh) * BSTRIDE + cell_col(old));
                uint16_t mid = CELL_NONE;
                if (b->grid[c1] == CELL_BODY + s && !ring_contains(n, c1)) mid = c1;
                else if (b->grid[c2] == CELL_BODY + s && !ring_contains(n, c2)) mid = c2;
                if (mid == CELL_NONE) ok = false;
                else ring_push_head(n, mid);
            }
            if (ok) ring_push_head(n, nh);
        } else if (m != 0) {
            ok = false;
        }
        if (ok) {
            while (n->len > length[s]) ring_pop_tail(n);
            ok = n->len == length[s];
        }
        if (ok) ok = verify_snake(b, s, nh, count[s]);
        if (!ok) {
            repair_snake(b, s, nh, count[s]);
            repaired++;
        } else {
            n->suspect = 0;
        }
    }

    board_refresh(b);
    b->step++;
    return repaired;
}

/* ------------------------------------------------------------------ */
/* hypothetical moves                                                  */
/* ------------------------------------------------------------------ */
bool board_apply(Board *b, int s, int dir, MoveUndo *u)
{
    Snake *n = &b->s[s];
    uint16_t oldhead = n->ring[n->head];
    uint16_t t = (uint16_t)((int)oldhead + dir_off(dir));
    uint8_t v = b->grid[t];
    if (!cell_passable(v)) return false;

    u->snake = (uint8_t)s;
    u->new_head = t;
    u->ate = (uint8_t)(v == CELL_FOOD);
    u->old_tail = CELL_NONE;
    u->old_tail_free_at = 0;

    b->grid[oldhead] = (uint8_t)(CELL_BODY + s);
    b->grid[t] = (uint8_t)(CELL_HEAD + s);
    bb_reset(&b->pass, t);
    if (u->ate) bb_reset(&b->food, t);

    ring_push_head(n, t);
    n->moves++;
    if (u->ate) {
        /* the whole body waits one extra step: every cell frees one move later */
        for (int i = 1; i < n->len; i++) b->free_at[n->ring[(n->head + i) & RING_MASK]]++;
    } else {
        uint16_t tail = ring_pop_tail(n);
        u->old_tail = tail;
        u->old_tail_free_at = b->free_at[tail];
        b->grid[tail] = CELL_FREE;
        b->free_at[tail] = 0;
        bb_set(&b->pass, tail);
    }
    /* new head: tail distance = len, plus slack, shifted by the moves made */
    b->free_at[t] = (uint16_t)(n->len + board_slack(b, s) + n->moves);
    return true;
}

void board_undo(Board *b, const MoveUndo *u)
{
    int s = u->snake;
    Snake *n = &b->s[s];
    uint16_t t = u->new_head;

    if (u->ate) {
        for (int i = 1; i < n->len; i++) b->free_at[n->ring[(n->head + i) & RING_MASK]]--;
    }
    n->moves--;
    b->free_at[t] = 0;
    b->grid[t] = u->ate ? CELL_FOOD : CELL_FREE;
    bb_set(&b->pass, t);
    if (u->ate) bb_set(&b->food, t);

    /* pop head */
    n->head = (uint16_t)((n->head + 1) & RING_MASK);
    n->len--;
    b->grid[n->ring[n->head]] = (uint8_t)(CELL_HEAD + s);

    if (!u->ate) {
        /* the old tail slot is normally untouched (pushes move away from it);
         * rewrite it anyway so undo never depends on that */
        n->len++;
        n->ring[(n->head + n->len - 1) & RING_MASK] = u->old_tail;
        b->grid[u->old_tail] = (uint8_t)(CELL_BODY + s);
        b->free_at[u->old_tail] = u->old_tail_free_at;
        bb_reset(&b->pass, u->old_tail);
    }
}

void board_threat(const Board *b, int self, BB *out)
{
    bb_clear(out);
    for (int s = 0; s < b->nsnakes; s++) {
        if (s == self || !b->s[s].alive) continue;
        uint16_t h = board_head(b, s);
        for (int d = 0; d < 4; d++) bb_set(out, (uint16_t)((int)h + dir_off(d)));
    }
}
