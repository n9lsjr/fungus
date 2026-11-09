/*
 * bench_core.c -- micro-benchmark of the core primitives on random,
 * realistic boards (20x15 / 4 snakes / 7 food and 20x20 / 6 snakes / 10
 * food, bodies 5..60 cells by random walk).
 *
 * Build: gcc -std=c11 -O2 -Wall -Wextra -march=x86-64-v3 board.c flood.c bench_core.c -o bench_core
 */
#define _POSIX_C_SOURCE 200809L /* clock_gettime under strict -std=c11 */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "flood.h"
#include "sim_util.h"

#define NBOARDS 32

static double now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

static volatile uint64_t sink;

typedef struct {
    Sim sim[NBOARDS];
    Board board[NBOARDS];
} Set;

static void make_set(Set *set, int W, int H, int n, int food, uint64_t seed)
{
    for (int k = 0; k < NBOARDS; k++) {
        Sim *s = &set->sim[k];
        sim_init(s, W, H, n, food, 2, seed + (uint64_t)k * 7919u);
        for (int i = 0; i < n; i++) sim_grow(s, i, 5 + (int)(sim_rand(s) % 56u));
        sim_spawn_food(s);
        /* a few random steps so tails and heads look played */
        for (int t = 0; t < 10; t++) sim_step(s, NULL);
        sim_load_board(s, &set->board[k], 0, 1);
    }
}

static void bench_update(Set *set, int W, int H, int n, int food, uint64_t seed)
{
    uint8_t alive[MAX_SNAKES];
    int len[MAX_SNAKES];
    double total = 0;
    long calls = 0, repairs = 0;
    make_set(set, W, H, n, food, seed);
    for (int rep = 0; rep < 40; rep++) {
        for (int k = 0; k < NBOARDS; k++) {
            Sim *s = &set->sim[k];
            Board *b = &set->board[k];
            if (!s->sn[0].alive) continue;
            sim_step(s, NULL);
            sim_fill_step(s, alive, len);
            double t0 = now_ns();
            repairs += board_update(b, s->map, alive, len);
            total += now_ns() - t0;
            calls++;
        }
    }
    printf("  board_update          %8.0f ns/call  (%ld calls, %ld repairs)\n", total / (double)calls, calls, repairs);
}

static void bench_flood(const Set *set, int release)
{
    FloodOpts o = { (uint8_t)release, 1, 0 };
    FloodResult r;
    double total = 0;
    long calls = 0, cells = 0;
    for (int rep = 0; rep < 200; rep++) {
        for (int k = 0; k < NBOARDS; k++) {
            const Board *b = &set->board[k];
            if (!b->s[0].alive) continue;
            for (int d = 0; d < 4; d++) {
                if (!board_legal(b, 0, d)) continue;
                uint16_t t = board_target(b, 0, d);
                double t0 = now_ns();
                flood_region(b, t, 1, &o, &r, NULL);
                total += now_ns() - t0;
                cells += r.cells;
                calls++;
            }
        }
    }
    sink += (uint64_t)cells;
    printf("  flood_region %-8s %8.0f ns/call  (avg region %ld cells)\n", release ? "release" : "static",
           total / (double)calls, cells / (calls ? calls : 1));
}

static void bench_voronoi(const Set *set, Voronoi *v, int release)
{
    double total = 0;
    long calls = 0;
    for (int rep = 0; rep < 200; rep++) {
        for (int k = 0; k < NBOARDS; k++) {
            double t0 = now_ns();
            voronoi_compute(&set->board[k], v, release);
            total += now_ns() - t0;
            sink += v->cells[0];
            calls++;
        }
    }
    printf("  voronoi %-13s %8.0f ns/call\n", release ? "release" : "static", total / (double)calls);
}

static void bench_move_eval(const Set *set)
{
    MoveEval e;
    double total = 0;
    long calls = 0;
    for (int rep = 0; rep < 200; rep++) {
        for (int k = 0; k < NBOARDS; k++) {
            const Board *b = &set->board[k];
            if (!b->s[0].alive) continue;
            for (int d = 0; d < 4; d++) {
                double t0 = now_ns();
                move_eval(b, d, &e);
                total += now_ns() - t0;
                sink += (uint64_t)e.margin;
                calls++;
            }
        }
    }
    printf("  move_eval (2 floods)  %8.0f ns/call\n", total / (double)calls);
}

static void bench_apply_undo(Set *set)
{
    double total = 0;
    long calls = 0;
    uint64_t rng = 12345;
    for (int rep = 0; rep < 2000; rep++) {
        for (int k = 0; k < NBOARDS; k++) {
            Board *b = &set->board[k];
            rng ^= rng << 13;
            rng ^= rng >> 7;
            rng ^= rng << 17;
            int s = (int)(rng % (uint64_t)b->nsnakes);
            int d = (int)((rng >> 8) & 3);
            if (!b->s[s].alive) continue;
            MoveUndo u;
            double t0 = now_ns();
            if (board_apply(b, s, d, &u)) board_undo(b, &u);
            total += now_ns() - t0;
            calls++;
        }
    }
    printf("  apply+undo            %8.0f ns/pair  (incl. illegal attempts)\n", total / (double)calls);
}

static void bench_two_ply(Set *set)
{
    /* our move x opponent reply for every nearby opponent, flood after each:
     * the shape of a real 2-ply search node */
    FloodOpts o = { 1, 0, 0 };
    FloodResult r;
    double total = 0;
    long nodes = 0, evals = 0;
    for (int rep = 0; rep < 50; rep++) {
        for (int k = 0; k < NBOARDS; k++) {
            Board *b = &set->board[k];
            if (!b->s[0].alive) continue;
            double t0 = now_ns();
            for (int d = 0; d < 4; d++) {
                MoveUndo u1;
                if (!board_apply(b, 0, d, &u1)) continue;
                for (int s = 1; s < b->nsnakes; s++) {
                    if (!b->s[s].alive) continue;
                    if (cell_manhattan(board_head(b, 0), board_head(b, s)) > 6) continue;
                    for (int d2 = 0; d2 < 4; d2++) {
                        MoveUndo u2;
                        if (!board_apply(b, s, d2, &u2)) continue;
                        for (int d3 = 0; d3 < 4; d3++) {
                            if (!board_legal(b, 0, d3)) continue;
                            flood_region(b, board_target(b, 0, d3), 2, &o, &r, NULL);
                            sink += r.cells;
                            evals++;
                        }
                        board_undo(b, &u2);
                    }
                }
                board_undo(b, &u1);
            }
            total += now_ns() - t0;
            nodes++;
        }
    }
    printf("  2-ply sample          %8.0f ns/position (%.1f floods/position)\n", total / (double)nodes,
           (double)evals / (double)nodes);
}

static void bench_bb(void)
{
    BB a, out;
    memset(&a, 0, sizeof a);
    for (int i = 1; i <= 20; i++) a.r[i] = 0x155555ull << 1;
    double t0 = now_ns();
    for (int i = 0; i < 100000; i++) {
        bb_expand(&a, &out, 20);
        a.r[3] ^= out.r[4] & 2;
        sink += out.r[5];
    }
    printf("  bb_expand (20 rows)   %8.1f ns/call\n", (now_ns() - t0) / 100000.0);
}

int main(void)
{
    Set *set = calloc(1, sizeof *set);
    Voronoi *v = calloc(1, sizeof *v);
    struct {
        int W, H, n, food;
    } cfg[2] = { { 20, 15, 4, 7 }, { 20, 20, 6, 10 } };

    printf("Board struct: %zu bytes, Voronoi: %zu bytes, BB: %zu bytes\n", sizeof(Board), sizeof(Voronoi), sizeof(BB));
    bench_bb();
    for (int c = 0; c < 2; c++) {
        printf("%dx%d, %d snakes, %d food\n", cfg[c].W, cfg[c].H, cfg[c].n, cfg[c].food);
        bench_update(set, cfg[c].W, cfg[c].H, cfg[c].n, cfg[c].food, 42 + (uint64_t)c);
        make_set(set, cfg[c].W, cfg[c].H, cfg[c].n, cfg[c].food, 99 + (uint64_t)c);
        bench_flood(set, 0);
        bench_flood(set, 1);
        bench_voronoi(set, v, 0);
        bench_voronoi(set, v, 1);
        bench_move_eval(set);
        bench_apply_undo(set);
        bench_two_ply(set);
    }
    free(set);
    free(v);
    return (int)(sink & 0);
}
