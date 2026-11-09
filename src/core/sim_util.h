/*
 * sim_util.h -- tiny header-only re-implementation of the simulator's
 * rules (snake_env.py) for tests and benchmarks: produces the same map
 * encoding (food 0, free 1, blocked 2, head 3+2i, body 4+2i), the same
 * start_length behaviour (body starts as one cell, grows while shorter than
 * start_length) and leaves dead bodies on the map.
 *
 * Not used by the bot itself.
 */
#ifndef CSNAKE_CORE_SIM_UTIL_H
#define CSNAKE_CORE_SIM_UTIL_H

#include "board.h"

enum { SIM_FOOD = 0, SIM_FREE = 1, SIM_BLOCKED = 2 };

typedef struct {
    int16_t x[RING_SIZE], y[RING_SIZE]; /* head first */
    int len, virt_len, alive;
} SimSnake;

typedef struct {
    int W, H, n, food_target;
    uint64_t rng;
    uint8_t map[MAX_W * MAX_H];
    SimSnake sn[MAX_SNAKES];
    int16_t start_x[MAX_SNAKES], start_y[MAX_SNAKES];
} Sim;

static inline uint32_t sim_rand(Sim *s)
{
    uint64_t x = s->rng;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    s->rng = x;
    return (uint32_t)((x * 0x2545F4914F6CDD1DULL) >> 32);
}
static inline int sim_head_value(int i) { return 3 + 2 * i; }
static inline int sim_body_value(int i) { return 4 + 2 * i; }
static inline uint8_t *sim_at(Sim *s, int x, int y) { return &s->map[y * s->W + x]; }
static inline int sim_inside(const Sim *s, int x, int y) { return x >= 0 && y >= 0 && x < s->W && y < s->H; }

static inline void sim_init(Sim *s, int W, int H, int n, int food_target, int start_length, uint64_t seed)
{
    memset(s, 0, sizeof *s);
    s->W = W;
    s->H = H;
    s->n = n;
    s->food_target = food_target;
    s->rng = seed ? seed : 1;
    memset(s->map, SIM_FREE, (size_t)(W * H));
    for (int i = 0; i < n; i++) {
        int x, y;
        do {
            x = 1 + (int)(sim_rand(s) % (uint32_t)(W - 2));
            y = 1 + (int)(sim_rand(s) % (uint32_t)(H - 2));
        } while (*sim_at(s, x, y) != SIM_FREE);
        s->sn[i].x[0] = (int16_t)x;
        s->sn[i].y[0] = (int16_t)y;
        s->sn[i].len = 1;
        s->sn[i].virt_len = start_length;
        s->sn[i].alive = 1;
        s->start_x[i] = (int16_t)x;
        s->start_y[i] = (int16_t)y;
        *sim_at(s, x, y) = (uint8_t)sim_head_value(i);
    }
}

/* Extend a snake's tail by a random self-avoiding walk (bench realism). */
static inline void sim_grow(Sim *s, int i, int target_len)
{
    SimSnake *n = &s->sn[i];
    while (n->len < target_len) {
        int tx = n->x[n->len - 1], ty = n->y[n->len - 1];
        int order = (int)(sim_rand(s) & 3), placed = 0;
        for (int k = 0; k < 4 && !placed; k++) {
            int d = (order + k) & 3;
            int nx = tx + dir_dx(d), ny = ty + dir_dy(d);
            if (sim_inside(s, nx, ny) && *sim_at(s, nx, ny) == SIM_FREE) {
                n->x[n->len] = (int16_t)nx;
                n->y[n->len] = (int16_t)ny;
                n->len++;
                *sim_at(s, nx, ny) = (uint8_t)sim_body_value(i);
                placed = 1;
            }
        }
        if (!placed) break;
    }
    n->virt_len = n->len;
}

static inline void sim_spawn_food(Sim *s)
{
    int count = 0, nfree = 0;
    uint16_t freec[MAX_W * MAX_H];
    for (int c = 0; c < s->W * s->H; c++) {
        if (s->map[c] == SIM_FOOD) count++;
        else if (s->map[c] == SIM_FREE) freec[nfree++] = (uint16_t)c;
    }
    while (count < s->food_target && nfree > 0) {
        int k = (int)(sim_rand(s) % (uint32_t)nfree);
        s->map[freec[k]] = SIM_FOOD;
        freec[k] = freec[--nfree];
        count++;
    }
}

/* One simulator step. dirs[i] = direction or -1 for a random legal move
 * (NULL = all random). Snakes move in random order; a snake that picks an
 * illegal move dies and its body stays on the map. */
static inline void sim_step(Sim *s, const int *dirs)
{
    int order[MAX_SNAKES];
    sim_spawn_food(s);
    for (int i = 0; i < s->n; i++) order[i] = i;
    for (int i = s->n - 1; i > 0; i--) {
        int j = (int)(sim_rand(s) % (uint32_t)(i + 1)), t = order[i];
        order[i] = order[j];
        order[j] = t;
    }
    for (int oi = 0; oi < s->n; oi++) {
        int i = order[oi];
        SimSnake *n = &s->sn[i];
        if (!n->alive) continue;
        int hx = n->x[0], hy = n->y[0];
        int d = dirs ? dirs[i] : -1;
        if (d < 0) {
            int cand[4], nc = 0;
            for (int k = 0; k < 4; k++) {
                int nx = hx + dir_dx(k), ny = hy + dir_dy(k);
                if (sim_inside(s, nx, ny) && *sim_at(s, nx, ny) <= SIM_FREE) cand[nc++] = k;
            }
            d = nc ? cand[sim_rand(s) % (uint32_t)nc] : 0;
        }
        int nx = hx + dir_dx(d), ny = hy + dir_dy(d);
        if (!sim_inside(s, nx, ny) || *sim_at(s, nx, ny) > SIM_FREE) {
            n->alive = 0;
            continue;
        }
        if (*sim_at(s, nx, ny) == SIM_FOOD) n->virt_len++;
        memmove(n->x + 1, n->x, (size_t)n->len * sizeof n->x[0]);
        memmove(n->y + 1, n->y, (size_t)n->len * sizeof n->y[0]);
        n->x[0] = (int16_t)nx;
        n->y[0] = (int16_t)ny;
        n->len++;
        *sim_at(s, hx, hy) = (uint8_t)sim_body_value(i);
        *sim_at(s, nx, ny) = (uint8_t)sim_head_value(i);
        while (n->len > n->virt_len) {
            n->len--;
            *sim_at(s, n->x[n->len], n->y[n->len]) = SIM_FREE;
        }
    }
}

static inline void sim_fill_init(const Sim *s, BoardInit *bi, int self, int opp_slack)
{
    memset(bi, 0, sizeof *bi);
    bi->width = s->W;
    bi->height = s->H;
    bi->free_value = SIM_FREE;
    bi->food_value = SIM_FOOD;
    bi->blocked_value = SIM_BLOCKED;
    bi->snake_count = s->n;
    bi->self_index = self;
    bi->opp_slack = opp_slack;
    for (int i = 0; i < s->n; i++) {
        bi->ids[i] = i;
        bi->head_value[i] = (uint8_t)sim_head_value(i);
        bi->body_value[i] = (uint8_t)sim_body_value(i);
        bi->start_x[i] = s->start_x[i];
        bi->start_y[i] = s->start_y[i];
    }
    bi->base_map = NULL;
}

static inline void sim_fill_step(const Sim *s, uint8_t *alive, int *length)
{
    for (int i = 0; i < s->n; i++) {
        alive[i] = (uint8_t)s->sn[i].alive;
        length[i] = s->sn[i].len;
    }
}

/* Load a board straight from the sim (bodies in sim order), bypassing
 * tracking. For benches that need a fresh, long-bodied board. */
static inline void sim_load_board(const Sim *s, Board *b, int self, int opp_slack)
{
    BoardInit bi;
    uint8_t alive[MAX_SNAKES];
    int length[MAX_SNAKES];
    sim_fill_init(s, &bi, self, opp_slack);
    board_init(b, &bi);
    for (int i = 0; i < s->n; i++) {
        uint16_t cells[RING_SIZE];
        for (int k = 0; k < s->sn[i].len; k++) cells[k] = cell_of(s->sn[i].x[k], s->sn[i].y[k]);
        board_set_body(b, i, cells, s->sn[i].len);
    }
    board_refresh(b);
    /* a real update so food and dead snakes come from the map */
    sim_fill_step(s, alive, length);
    board_update(b, s->map, alive, length);
}

#endif
