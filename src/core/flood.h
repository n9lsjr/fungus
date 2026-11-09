/*
 * flood.h -- reachable-area flood fill (bitboard), multi-source Voronoi
 * BFS, and a per-move safety estimate. All state is caller-owned.
 */
#ifndef CSNAKE_CORE_FLOOD_H
#define CSNAKE_CORE_FLOOD_H

#include "board.h"

typedef struct {
    uint8_t release;  /* 1: body cells join the region once free_at <= arrival time */
    uint8_t corridor; /* 1: also count one-wide corridor cells in the region */
    uint16_t max_t;   /* stop once the arrival time reaches this (0 = no limit) */
    uint8_t peel;     /* 1: tighten path_bound by peeling 1-wide dead ends (see flood.c) */
    uint8_t split;    /* 1 (static floods only): score the best side of the region once we
                         stand on start, which is often an articulation point */
    uint8_t food_delay; /* release floods: food already inside the region delays our own
                           releases by one step each (we may eat it on the way) */
    uint8_t verdict_only; /* only stuck/closed/tail_t/t_end are needed: skip region statistics */
    const BB *allow;    /* optional: the region may only grow into these cells (e.g. cells we
                           reach before any opponent); NULL = no restriction */
} FloodOpts;

typedef struct {
    uint16_t cells;       /* cells in the region, including the start */
    uint16_t food;
    uint16_t black;       /* checkerboard counts */
    uint16_t white;
    uint16_t path_bound;  /* parity upper bound on cells a simple path from start can visit */
    uint16_t layers;      /* BFS layers that added cells (= max distance from start) */
    uint16_t t_end;       /* arrival time of the last added layer */
    uint16_t tail_t;      /* arrival time at which our own tail cell joined (0 = never) */
    uint16_t self_min_free_at; /* smallest free_at of own body cells bordering the region
                                  that were not reached (0 = none); Rufus' "max body index" */
    uint16_t corridor;    /* narrow cells in the region (only if opts.corridor) */
    uint16_t peeled;      /* dead-end cells removed by opts.peel */
    uint16_t peel_depth;  /* longest peeled branch (rounds) */
    uint8_t split_sides;  /* pieces the region falls into without the start (opts.split) */
    uint8_t stuck;        /* release floods: ran out of room while a release was still pending */
    uint8_t closed;       /* release floods: ended with no pending release at all */
    uint16_t stuck_t;     /* arrival time when it got stuck */
    uint8_t start_parity;
} FloodResult;

/* Flood from `start`, entered at move number t0 (1 = our next move).
 * Returns false (and zeroes r) if start cannot be entered at t0.
 * region_out (optional) receives the reachable set. */
bool flood_region(const Board *b, uint16_t start, int t0, const FloodOpts *opts,
                  FloodResult *r, BB *region_out);

/* Rufus-style survival margin for a region entered at t0:
 * (path_bound - food) - needed, needed = steps until the nearest own body
 * cell on the border frees, or len+1 if the region touches none. */
static inline int flood_margin(const Board *b, const FloodResult *r, int t0)
{
    int needed = r->self_min_free_at ? (int)r->self_min_free_at - t0 : (int)b->s[b->self].len + 1;
    if (needed < 0) needed = 0;
    return (int)r->path_bound - (int)r->food - needed;
}

/* ------------------------------------------------------------------ */
/* Voronoi                                                             */
/* ------------------------------------------------------------------ */
#define VOR_NONE 0xFF
#define VOR_CONTESTED 0xFE

typedef struct {
    uint8_t owner[BCELLS];   /* valid only where mark[c] == gen; use voronoi_owner() */
    uint16_t dist[BCELLS];
    uint16_t mark[BCELLS];
    uint16_t gen;
    uint16_t queue[2048];    /* BFS ring, >= BCELLS so it never wraps */
    uint16_t cells[MAX_SNAKES]; /* cells owned (heads excluded) */
    uint16_t food[MAX_SNAKES];
    uint16_t contested;
} Voronoi;

/* Multi-source BFS from all alive heads. With release, a body cell is
 * passable when the arrival distance d satisfies free_at[c] <= d. Cells
 * first reached by two snakes in the same layer are contested and do not
 * propagate. */
void voronoi_compute(const Board *b, Voronoi *v, int release);

static inline int voronoi_owner(const Voronoi *v, uint16_t c)
{
    return v->mark[c] == v->gen ? v->owner[c] : VOR_NONE;
}
static inline int voronoi_dist(const Voronoi *v, uint16_t c)
{
    return v->mark[c] == v->gen ? (int)v->dist[c] : -1;
}

/* ------------------------------------------------------------------ */
/* per-move safety estimate for the self snake                         */
/* ------------------------------------------------------------------ */
typedef struct {
    uint8_t legal;       /* target is free or food now */
    uint8_t threatened;  /* target is adjacent to an alive opponent head */
    uint8_t ate;         /* target is food */
    uint16_t target;
    FloodResult stat;    /* static region (bodies never open) */
    FloodResult dyn;     /* time-aware region (tails release) */
    int32_t margin;      /* flood_margin(stat) */
} MoveEval;

void move_eval(const Board *b, int dir, MoveEval *e);

#endif
