/*
 * test_core.c -- hand-made board tests for the core.
 * Build: gcc -std=c11 -O2 -Wall -Wextra board.c flood.c test_core.c -o test_core
 */
#include <stdio.h>
#include <stdlib.h>
#include "flood.h"
#include "sim_util.h"

static int fails = 0, checks = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        checks++;                                                             \
        if (!(c)) {                                                           \
            fails++;                                                          \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);               \
        }                                                                     \
    } while (0)

static int ring_equals_sim(const Board *b, const Sim *s, int i)
{
    const SimSnake *n = &s->sn[i];
    if (b->s[i].len != n->len) return 0;
    for (int k = 0; k < n->len; k++)
        if (board_body(b, i, k) != cell_of(n->x[k], n->y[k])) return 0;
    return 1;
}

/* Build a board from ASCII rows ('.' free, '*' food, '#' wall) plus explicit
 * ordered bodies given as x,y pairs (head first). */
static void load_ascii(Board *b, const char *const *rows, int W, int H, int nsn, int self,
                       const int *const *bodies, const int *blen)
{
    static uint8_t base[MAX_W * MAX_H];
    BoardInit bi;
    memset(&bi, 0, sizeof bi);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            base[y * W + x] = rows[y][x] == '#' ? SIM_BLOCKED : rows[y][x] == '*' ? SIM_FOOD : SIM_FREE;
    bi.width = W;
    bi.height = H;
    bi.free_value = SIM_FREE;
    bi.food_value = SIM_FOOD;
    bi.blocked_value = SIM_BLOCKED;
    bi.snake_count = nsn;
    bi.self_index = self;
    bi.opp_slack = 1;
    bi.base_map = base;
    for (int i = 0; i < nsn; i++) {
        bi.ids[i] = i;
        bi.head_value[i] = (uint8_t)sim_head_value(i);
        bi.body_value[i] = (uint8_t)sim_body_value(i);
        bi.start_x[i] = (int16_t)bodies[i][0];
        bi.start_y[i] = (int16_t)bodies[i][1];
    }
    board_init(b, &bi);
    for (int i = 0; i < nsn; i++) {
        uint16_t cells[64];
        for (int k = 0; k < blen[i]; k++) cells[k] = cell_of(bodies[i][2 * k], bodies[i][2 * k + 1]);
        board_set_body(b, i, cells, blen[i]);
    }
    board_refresh(b);
}

/* ------------------------------------------------------------------ */
static void test_tracking(int W, int H, int n, int every, int steps, uint64_t seed)
{
    Sim *sim = calloc(1, sizeof *sim);
    Board *b = calloc(1, sizeof *b);
    BoardInit bi;
    uint8_t alive[MAX_SNAKES];
    int len[MAX_SNAKES];

    sim_init(sim, W, H, n, 6, 2, seed);
    sim_fill_init(sim, &bi, 0, 1);
    board_init(b, &bi);
    sim_fill_step(sim, alive, len);
    CHECK(board_update(b, sim->map, alive, len) == 0);
    for (int i = 0; i < n; i++) CHECK(ring_equals_sim(b, sim, i));

    for (int step = 1; step <= steps; step++) {
        sim_step(sim, NULL);
        if (step % every) continue;
        sim_fill_step(sim, alive, len);
        int rep = board_update(b, sim->map, alive, len);
        CHECK(rep == 0);
        for (int i = 0; i < n; i++) {
            if (sim->sn[i].alive) {
                CHECK(b->s[i].alive);
                CHECK(ring_equals_sim(b, sim, i));
                CHECK(b->grid[board_head(b, i)] == CELL_HEAD + i);
                /* free_at: tail = 1 + slack, head = len + slack */
                int slack = board_slack(b, i);
                CHECK(b->free_at[board_tail(b, i)] == 1 + slack);
                CHECK(b->free_at[board_head(b, i)] == b->s[i].len + slack);
            } else {
                CHECK(!b->s[i].alive);
                for (int k = 0; k < sim->sn[i].len; k++)
                    CHECK(b->grid[cell_of(sim->sn[i].x[k], sim->sn[i].y[k])] == CELL_WALL);
            }
        }
        /* map agreement for the static cells */
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                uint8_t m = sim->map[y * W + x], g = b->grid[cell_of(x, y)];
                if (m == SIM_FREE) CHECK(g == CELL_FREE && bb_test(&b->pass, cell_of(x, y)));
                if (m == SIM_FOOD) CHECK(g == CELL_FOOD && bb_test(&b->food, cell_of(x, y)));
            }
    }
    free(sim);
    free(b);
}

/* ------------------------------------------------------------------ */
static void test_eating(void)
{
    Sim *sim = calloc(1, sizeof *sim);
    Board *b = calloc(1, sizeof *b);
    BoardInit bi;
    uint8_t alive[MAX_SNAKES];
    int len[MAX_SNAKES];
    int dirs[MAX_SNAKES] = { DIR_RIGHT };

    sim_init(sim, 8, 8, 1, 0, 2, 7);
    /* put the snake at (2,2) with food at (4,2) */
    sim->map[sim->sn[0].y[0] * 8 + sim->sn[0].x[0]] = SIM_FREE;
    sim->sn[0].x[0] = 2;
    sim->sn[0].y[0] = 2;
    sim->start_x[0] = 2;
    sim->start_y[0] = 2;
    sim->map[2 * 8 + 2] = (uint8_t)sim_head_value(0);
    sim->map[2 * 8 + 4] = SIM_FOOD;
    sim_fill_init(sim, &bi, 0, 1);
    board_init(b, &bi);
    sim_fill_step(sim, alive, len);
    board_update(b, sim->map, alive, len);
    CHECK(b->s[0].len == 1);
    CHECK(bb_test(&b->food, cell_of(4, 2)));

    sim_step(sim, dirs); /* start_length 2: grows without eating */
    sim_fill_step(sim, alive, len);
    CHECK(board_update(b, sim->map, alive, len) == 0);
    CHECK(b->s[0].len == 2 && board_head(b, 0) == cell_of(3, 2) && board_tail(b, 0) == cell_of(2, 2));

    sim_step(sim, dirs); /* eats at (4,2) */
    sim_fill_step(sim, alive, len);
    CHECK(board_update(b, sim->map, alive, len) == 0);
    CHECK(b->s[0].len == 3 && board_head(b, 0) == cell_of(4, 2) && board_tail(b, 0) == cell_of(2, 2));
    CHECK(b->free_at[cell_of(2, 2)] == 2);
    CHECK(b->free_at[cell_of(3, 2)] == 3);
    CHECK(b->free_at[cell_of(4, 2)] == 4);
    CHECK(!bb_test(&b->food, cell_of(4, 2)));

    sim_step(sim, dirs); /* tail released */
    sim_fill_step(sim, alive, len);
    CHECK(board_update(b, sim->map, alive, len) == 0);
    CHECK(b->s[0].len == 3 && board_tail(b, 0) == cell_of(3, 2));
    CHECK(b->grid[cell_of(2, 2)] == CELL_FREE && b->free_at[cell_of(2, 2)] == 0);
    CHECK(bb_test(&b->pass, cell_of(2, 2)));

    /* own tail is not enterable this move: board_legal says no */
    sim_step(sim, dirs);
    sim_fill_step(sim, alive, len);
    board_update(b, sim->map, alive, len);
    CHECK(board_head(b, 0) == cell_of(6, 2));
    CHECK(!board_legal(b, 0, DIR_LEFT));
    free(sim);
    free(b);
}

/* ------------------------------------------------------------------ */
static void test_flood(void)
{
    Board *b = calloc(1, sizeof *b);
    /* pocket of two cells left of the head, sealed by walls and own body */
    static const char *const rows[] = {
        "#####",
        "..Aaa",
        "#####",
    };
    static const int body0[] = { 2, 1, 3, 1, 4, 1 };
    const int *const bodies[] = { body0 };
    const int blen[] = { 3 };
    FloodResult r;
    FloodOpts st = { .corridor = 1 }, dy = { .release = 1, .corridor = 1 };

    load_ascii(b, rows, 5, 3, 1, 0, bodies, blen);
    CHECK(b->free_at[cell_of(4, 1)] == 2 && b->free_at[cell_of(2, 1)] == 4);

    CHECK(flood_region(b, cell_of(1, 1), 1, &st, &r, NULL));
    CHECK(r.cells == 2 && r.food == 0);
    CHECK(r.path_bound == 2);
    CHECK(r.corridor == 2);
    CHECK(r.self_min_free_at == 4);
    CHECK(flood_margin(b, &r, 1) == 2 - 0 - 3);
    CHECK(r.tail_t == 0);

    CHECK(flood_region(b, cell_of(1, 1), 1, &dy, &r, NULL));
    /* entered at t0=1, a 2-cell pocket only lasts until t=2 but the body
     * next to it frees at 4: no waiting, the region stays sealed */
    CHECK(r.cells == 2);
    CHECK(r.tail_t == 0);

    /* not enterable at t0=1 but at t0=4 */
    CHECK(!flood_region(b, cell_of(2, 1), 1, &st, &r, NULL));
    CHECK(flood_region(b, cell_of(2, 1), 4, &dy, &r, NULL) && r.cells == 5);

    /* parity bound: 3x3 open room, start in a corner (black): 5 black, 4 white */
    static const char *const rows2[] = {
        "#####",
        "#...#",
        "#...#",
        "#...#",
        "#####",
    };
    static const int body1[] = { 0, 0 };
    const int *const bodies2[] = { body1 };
    const int blen2[] = { 1 };
    load_ascii(b, rows2, 5, 5, 1, 0, bodies2, blen2);
    CHECK(flood_region(b, cell_of(1, 1), 1, &st, &r, NULL));
    CHECK(r.cells == 9 && r.black == 5 && r.white == 4 && r.path_bound == 9);
    CHECK(flood_region(b, cell_of(2, 1), 1, &st, &r, NULL));
    CHECK(r.path_bound == 8); /* start on white: 2*4 */
    CHECK(r.layers == 3);

    /* food counted, region bitboard returned */
    static const char *const rows3[] = {
        "..*..",
        ".###.",
        "..*..",
    };
    load_ascii(b, rows3, 5, 3, 1, 0, bodies2, blen2);
    BB reg;
    CHECK(flood_region(b, cell_of(2, 0), 1, &st, &r, &reg));
    CHECK(r.cells == 11 && r.food == 2 && bb_count(&reg) == 11); /* 15 - 3 walls - head at (0,0) */
    free(b);
}

/* ------------------------------------------------------------------ */
static void test_voronoi(void)
{
    Board *b = calloc(1, sizeof *b);
    Voronoi *v = calloc(1, sizeof *v);
    static const char *const rows[] = {
        ".......",
        "A.....B",
        ".......",
    };
    static const int body0[] = { 0, 1 }, body1[] = { 6, 1 };
    const int *const bodies[] = { body0, body1 };
    const int blen[] = { 1, 1 };
    load_ascii(b, rows, 7, 3, 2, 0, bodies, blen);
    voronoi_compute(b, v, 0);
    CHECK(v->cells[0] == 8 && v->cells[1] == 8 && v->contested == 3);
    CHECK(voronoi_owner(v, cell_of(3, 1)) == VOR_CONTESTED);
    CHECK(voronoi_dist(v, cell_of(3, 1)) == 3);
    CHECK(voronoi_owner(v, cell_of(1, 0)) == 0 && voronoi_dist(v, cell_of(1, 0)) == 2);
    CHECK(voronoi_owner(v, cell_of(5, 2)) == 1);
    CHECK(voronoi_owner(v, cell_of(0, 1)) == 0 && voronoi_dist(v, cell_of(0, 1)) == 0);

    /* release: a body blocks statically but opens in time */
    static const char *const rows2[] = {
        "#####",
        "Aaa..",
        "#####",
    };
    static const int body2[] = { 0, 1, 1, 1, 2, 1 };
    const int *const bodies2[] = { body2 };
    const int blen2[] = { 3 };
    load_ascii(b, rows2, 5, 3, 1, 0, bodies2, blen2);
    voronoi_compute(b, v, 0);
    CHECK(v->cells[0] == 0);
    voronoi_compute(b, v, 1);
    /* (1,1) frees at 3 > d=1, so nothing opens on first contact; BFS does not revisit */
    CHECK(v->cells[0] == 0);
    free(b);
    free(v);
}

/* ------------------------------------------------------------------ */
static int boards_equal(const Board *a, const Board *b)
{
    if (memcmp(a->grid, b->grid, sizeof a->grid)) return 0;
    if (memcmp(a->free_at, b->free_at, sizeof a->free_at)) return 0;
    if (memcmp(&a->pass, &b->pass, sizeof a->pass)) return 0;
    if (memcmp(&a->food, &b->food, sizeof a->food)) return 0;
    for (int s = 0; s < a->nsnakes; s++) {
        if (a->s[s].len != b->s[s].len || a->s[s].moves != b->s[s].moves || a->s[s].alive != b->s[s].alive) return 0;
        for (int i = 0; i < a->s[s].len; i++)
            if (board_body(a, s, i) != board_body(b, s, i)) return 0;
    }
    return 1;
}

static void test_apply_undo(void)
{
    Sim *sim = calloc(1, sizeof *sim);
    Board *b = calloc(1, sizeof *b);
    Board *copy = calloc(1, sizeof *copy);
    Board *b2 = calloc(1, sizeof *b2);
    uint8_t alive[MAX_SNAKES];
    int len[MAX_SNAKES];

    /* random chains of applies, then undo in reverse: board must be identical */
    for (int trial = 0; trial < 20; trial++) {
        sim_init(sim, 12, 10, 3, 8, 2, 100 + (uint64_t)trial);
        for (int i = 0; i < 3; i++) sim_grow(sim, i, 4 + (int)(sim_rand(sim) % 10));
        sim_spawn_food(sim);
        sim_load_board(sim, b, 0, 1);
        memcpy(copy, b, sizeof *b);
        MoveUndo u[6];
        int n = 0;
        for (int k = 0; k < 6; k++) {
            int s = (int)(sim_rand(sim) % 3u);
            if (!b->s[s].alive) continue;
            int d = (int)(sim_rand(sim) & 3);
            if (board_apply(b, s, d, &u[n])) {
                CHECK(b->s[s].moves == copy->s[s].moves + 1 || n > 0);
                n++;
            }
        }
        /* new head free_at matches the formula */
        if (n > 0) {
            int s = u[n - 1].snake;
            CHECK(b->free_at[u[n - 1].new_head] == b->s[s].len + board_slack(b, s) + b->s[s].moves);
        }
        while (n > 0) board_undo(b, &u[--n]);
        CHECK(boards_equal(b, copy));
    }

    /* apply for a single snake agrees with a real step + board_update:
     * same grid; free_at of body cells is one higher (time shifted by 1) */
    for (int trial = 0; trial < 20; trial++) {
        sim_init(sim, 10, 10, 1, 5, 2, 500 + (uint64_t)trial);
        sim_grow(sim, 0, 6);
        sim_spawn_food(sim);
        sim_load_board(sim, b, 0, 1);
        int cand[4], nc = 0;
        for (int d = 0; d < 4; d++)
            if (board_legal(b, 0, d)) cand[nc++] = d;
        if (!nc) continue;
        int d = cand[sim_rand(sim) % (uint32_t)nc];
        MoveUndo u;
        memcpy(b2, b, sizeof *b2); /* b2 tracks the real step from the same state */
        CHECK(board_apply(b, 0, d, &u));
        int dirs[MAX_SNAKES] = { d };
        sim->food_target = 0; /* no new food so maps stay comparable */
        sim_step(sim, dirs);
        sim_fill_step(sim, alive, len);
        CHECK(board_update(b2, sim->map, alive, len) == 0);
        CHECK(memcmp(b->grid, b2->grid, sizeof b->grid) == 0);
        CHECK(b->s[0].len == b2->s[0].len);
        for (int i = 0; i < b->s[0].len; i++) {
            uint16_t c = board_body(b, 0, i);
            CHECK(c == board_body(b2, 0, i));
            CHECK(b->free_at[c] == b2->free_at[c] + 1);
        }
        board_undo(b, &u);
    }
    free(sim);
    free(b);
    free(copy);
    free(b2);
}

/* ------------------------------------------------------------------ */
static void test_repair(void)
{
    /* Feed a map whose snake has moved 3 cells since the last update:
     * tracking cannot follow, repair must rebuild the order from the map. */
    Sim *sim = calloc(1, sizeof *sim);
    Board *b = calloc(1, sizeof *b);
    uint8_t alive[MAX_SNAKES];
    int len[MAX_SNAKES];
    int dirs[MAX_SNAKES] = { DIR_RIGHT };

    sim_init(sim, 12, 6, 1, 0, 4, 9);
    sim->map[sim->sn[0].y[0] * 12 + sim->sn[0].x[0]] = SIM_FREE;
    sim->sn[0].x[0] = 1;
    sim->sn[0].y[0] = 2;
    sim->start_x[0] = 1;
    sim->start_y[0] = 2;
    sim->map[2 * 12 + 1] = (uint8_t)sim_head_value(0);
    sim_load_board(sim, b, 0, 1);
    for (int k = 0; k < 3; k++) sim_step(sim, dirs);
    sim_fill_step(sim, alive, len);
    CHECK(board_update(b, sim->map, alive, len) == 1);
    CHECK(ring_equals_sim(b, sim, 0));
    CHECK(!b->s[0].suspect);
    /* and tracking continues normally afterwards */
    sim_step(sim, dirs);
    sim_fill_step(sim, alive, len);
    CHECK(board_update(b, sim->map, alive, len) == 0);
    CHECK(ring_equals_sim(b, sim, 0));
    free(sim);
    free(b);
}

/* ------------------------------------------------------------------ */
static void test_move_eval_and_threat(void)
{
    Board *b = calloc(1, sizeof *b);
    static const char *const rows[] = {
        ".....",
        "aA.B.",
        ".....",
    };
    static const int body0[] = { 1, 1, 0, 1 }, body1[] = { 3, 1 };
    const int *const bodies[] = { body0, body1 };
    const int blen[] = { 2, 1 };
    MoveEval e;
    BB th;
    load_ascii(b, rows, 5, 3, 2, 0, bodies, blen);
    move_eval(b, DIR_RIGHT, &e);
    CHECK(e.legal && e.threatened && !e.ate);
    move_eval(b, DIR_UP, &e);
    CHECK(e.legal && !e.threatened);
    CHECK(e.stat.cells == 12); /* 15 cells - 3 snake cells */
    move_eval(b, DIR_LEFT, &e);
    CHECK(!e.legal);
    board_threat(b, 0, &th);
    CHECK(bb_test(&th, cell_of(2, 1)) && bb_test(&th, cell_of(3, 0)) && !bb_test(&th, cell_of(1, 0)));
    free(b);
}

/* dead-end peeling: a 3x3 room with two 1-wide dead ends; a simple path
 * can use at most one of them, and only at its end */
static void test_peel(void)
{
    Board *b = calloc(1, sizeof *b);
    static const char *const rows[] = {
        "#########",
        "#...####A",
        "#........",
        "#...####a",
        "#.#######",
        "#.#######",
        "#########",
    };
    static const int body0[] = { 8, 1, 8, 2 };
    const int *const bodies[] = { body0 };
    const int blen[] = { 2 };
    load_ascii(b, rows, 9, 7, 1, 0, bodies, blen);
    FloodResult r;
    FloodOpts plain = { 0 }, peel = { .peel = 1 }, split = { .split = 1 };
    /* start in the room centre: room 9 + corridor right (4) + corridor down (2) = 15 */
    CHECK(flood_region(b, cell_of(2, 2), 1, &plain, &r, NULL));
    CHECK(r.cells == 15);
    int pb_plain = r.path_bound;
    CHECK(flood_region(b, cell_of(2, 2), 1, &peel, &r, NULL));
    CHECK(r.cells == 15);
    CHECK(r.peeled == 6);              /* both dead ends stripped */
    CHECK(r.peel_depth == 4);          /* the longer one */
    CHECK(r.path_bound <= 9 + 4);      /* core + one branch */
    CHECK(r.path_bound < pb_plain);
    /* starting inside a dead end keeps that branch: it is walked on the way in */
    CHECK(flood_region(b, cell_of(1, 5), 1, &peel, &r, NULL));
    CHECK(r.path_bound <= 2 + 9 + 4 && r.path_bound >= 9);

    /* split: standing on the junction (4,2) between the room and the right
     * corridor, a path continues into one side only */
    CHECK(flood_region(b, cell_of(4, 2), 1, &split, &r, NULL));
    CHECK(r.split_sides == 2);
    CHECK(r.cells == 15);
    CHECK(r.path_bound <= 1 + 10);     /* start + the room side (room 9 + down corridor 2 = 11 cells) */
    free(b);
}

int main(void)
{
    test_tracking(10, 8, 2, 1, 80, 1);
    test_tracking(20, 15, 4, 1, 150, 2);
    test_tracking(20, 20, 6, 1, 150, 3);
    test_tracking(20, 15, 4, 2, 150, 4); /* two moves between observations */
    test_tracking(20, 20, 6, 2, 150, 5);
    test_eating();
    test_flood();
    test_voronoi();
    test_apply_undo();
    test_repair();
    test_move_eval_and_threat();
    test_peel();
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
