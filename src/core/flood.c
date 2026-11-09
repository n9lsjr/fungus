/*
 * flood.c -- bitboard flood fill, Voronoi BFS, move safety estimate.
 */
#include "flood.h"

/* Region statistics for a reached set: counts, parity path bound, optional
 * dead-end peeling and the nearest own body cell on the border. */
static void region_finish(const Board *b, const BB *reach, int rmin, int rmax, uint16_t start,
                          int peel, FloodResult *r)
{
    const int H = b->H;
    const uint64_t *pass = b->pass.r, *ever = b->ever.r;
    const uint16_t *fa = b->free_at;
    int cells = 0, food = 0, black = 0;
    for (int i = rmin; i <= rmax; i++) {
        uint64_t x = reach->r[i];
        cells += __builtin_popcountll(x);
        food += __builtin_popcountll(x & b->food.r[i]);
        black += __builtin_popcountll(x & b->black.r[i]);
    }
    r->cells = (uint16_t)cells;
    r->food = (uint16_t)food;
    r->black = (uint16_t)black;
    r->white = (uint16_t)(cells - black);
    r->start_parity = (uint8_t)(bb_test(&b->black, start) ? 0 : 1);
    {
        /* a simple path alternates colours: ceil(L/2) of the start colour,
         * floor(L/2) of the other */
        int nc = r->start_parity == 0 ? black : cells - black;
        int no = cells - nc;
        int pb = 2 * nc;
        if (2 * no + 1 < pb) pb = 2 * no + 1;
        r->path_bound = (uint16_t)pb;
    }

    /* Dead-end peeling: a simple path can enter at most one 1-wide dead end,
     * and only as its last stretch. Repeatedly strip cells with at most one
     * region neighbour (the start is anchored), then bound the path by the
     * parity bound of the remaining core plus the deepest stripped branch. */
    if (peel) {
        BB core = *reach;
        const int srow = cell_row(start);
        const uint64_t sbit = (uint64_t)1 << cell_col(start);
        int peeled = 0, depth = 0;
        uint64_t leaf[BROWS];
        for (;;) {
            uint64_t any = 0;
            for (int i = rmin; i <= rmax; i++) {
                uint64_t x = core.r[i];
                uint64_t n = core.r[i - 1], s_ = core.r[i + 1], e = x >> 1, w = x << 1;
                uint64_t two = (n & s_) | (n & e) | (n & w) | (s_ & e) | (s_ & w) | (e & w);
                uint64_t l = x & ~two;
                if (i == srow) l &= ~sbit;
                leaf[i] = l;
                any |= l;
            }
            if (!any) break;
            for (int i = rmin; i <= rmax; i++) {
                core.r[i] &= ~leaf[i];
                peeled += __builtin_popcountll(leaf[i]);
            }
            depth++;
        }
        if (peeled) {
            int ccells = cells - peeled, cblack = 0;
            for (int i = rmin; i <= rmax; i++) cblack += __builtin_popcountll(core.r[i] & b->black.r[i]);
            int nc = r->start_parity == 0 ? cblack : ccells - cblack;
            int no = ccells - nc;
            int pb = 2 * nc < 2 * no + 1 ? 2 * nc : 2 * no + 1;
            pb += depth;
            if (pb < r->path_bound) r->path_bound = (uint16_t)pb;
        }
        r->peeled = (uint16_t)peeled;
        r->peel_depth = (uint16_t)depth;
    }

    /* own body cells on the border that were not reached */
    {
        int lo = rmin > 1 ? rmin - 1 : 1;
        int hi = rmax < H ? rmax + 1 : H;
        int smin = 0x7FFF;
        const int self = b->self;
        for (int i = lo; i <= hi; i++) {
            uint64_t x = reach->r[i];
            uint64_t ex = (x | (x << 1) | (x >> 1) | reach->r[i - 1] | reach->r[i + 1]) & ~x;
            uint64_t dyn = ex & ever[i] & ~pass[i];
            while (dyn) {
                int bit = __builtin_ctzll(dyn);
                dyn &= dyn - 1;
                uint16_t c = (uint16_t)(i * BSTRIDE + bit);
                uint8_t g = b->grid[c];
                if (cell_is_snake(g) && cell_owner(g) == self && fa[c] < smin) smin = fa[c];
            }
        }
        r->self_min_free_at = (uint16_t)(smin < 0x7FFF ? smin : 0);
    }

}

/* Static flood from `from` restricted to `allowed`. */
static void flood_within(const BB *allowed, uint16_t from, int rmin, int rmax, BB *out)
{
    bb_clear(out);
    bb_set(out, from);
    for (;;) {
        uint64_t any = 0;
        for (int i = rmin; i <= rmax; i++) {
            uint64_t x = out->r[i];
            uint64_t n = (x | (x << 1) | (x >> 1) | out->r[i - 1] | out->r[i + 1]) & allowed->r[i] & ~x;
            out->r[i] |= n;
            any |= n;
        }
        if (!any) break;
    }
}

/* Once we stand on `start`, a simple path continues into exactly one of the
 * pieces the region falls into without `start` (start is often an
 * articulation point). Keep the counts of the full region but replace
 * path_bound/food/self_min_free_at by start + the side with the best margin. */
static void split_at_start(const Board *b, const BB *reach, int rmin, int rmax, uint16_t start,
                           int t0, const FloodOpts *opts, FloodResult *r)
{
    BB rest = *reach, covered, side;
    bb_reset(&rest, start);
    bb_clear(&covered);
    const int self = b->self;
    int lo = rmin > 1 ? rmin - 1 : 1, hi = rmax < b->H ? rmax + 1 : b->H;
    int best_margin = -(1 << 20), sides = 0;
    FloodResult best, rs;
    memset(&best, 0, sizeof best);
    for (int k = 0; k < 4; k++) {
        uint16_t n = (uint16_t)((int)start + dir_off(k));
        if (!bb_test(&rest, n) || bb_test(&covered, n)) continue;
        flood_within(&rest, n, lo, hi, &side);
        for (int i = lo; i <= hi; i++) covered.r[i] |= side.r[i];
        region_finish(b, &side, lo, hi, n, opts->peel, &rs);
        int needed = rs.self_min_free_at ? (int)rs.self_min_free_at - (t0 + 1) : (int)b->s[self].len + 1;
        if (needed < 0) needed = 0;
        int m = (int)rs.path_bound - (int)rs.food - needed;
        if (m > best_margin) { best_margin = m; best = rs; }
        sides++;
    }
    r->split_sides = (uint8_t)sides;
    if (sides == 0) return; /* region is just the start cell */
    /* own body cells touching the start itself are exits too */
    int smin = best.self_min_free_at ? best.self_min_free_at : 0x7FFF;
    for (int k = 0; k < 4; k++) {
        uint16_t n = (uint16_t)((int)start + dir_off(k));
        uint8_t g = b->grid[n];
        if (cell_is_snake(g) && cell_owner(g) == self && b->free_at[n] < smin) smin = b->free_at[n];
    }
    r->path_bound = (uint16_t)(1 + best.path_bound);
    r->food = (uint16_t)(best.food + (bb_test(&b->food, start) ? 1 : 0));
    r->self_min_free_at = (uint16_t)(smin < 0x7FFF ? smin : 0);
}

bool flood_region(const Board *b, uint16_t start, int t0, const FloodOpts *opts,
                  FloodResult *r, BB *region_out)
{
    memset(r, 0, sizeof *r);
    if ((int)b->free_at[start] > t0) return false;

    const int H = b->H;
    const int release = opts && opts->release;
    const int max_t = (opts && opts->max_t) ? opts->max_t : 0x7FFF;
    const uint64_t *pass = b->pass.r, *ever = b->ever.r;
    const uint16_t *fa = b->free_at;
    const uint16_t self_tail = b->s[b->self].alive ? board_tail(b, b->self) : CELL_NONE;
    uint64_t nxt[BROWS];
    BB reach;
    int rmin = cell_row(start), rmax = rmin;
    int t = t0;
    const int food_delay = release && opts && opts->food_delay;
    const BB *allow = opts ? opts->allow : NULL;
    int food_in = bb_test(&b->food, start) ? 1 : 0;

    bb_clear(&reach);
    bb_set(&reach, start);
    r->t_end = (uint16_t)t0;
    if (start == self_tail) r->tail_t = (uint16_t)t0;

    /* Each iteration grows the region by one BFS layer. The region is
     * expanded as a whole (not just the frontier): a cell blocked when first
     * touched is picked up later once its free_at is reached, which models a
     * snake that wanders inside the region while waiting. */
    for (;;) {
        int lo = rmin > 1 ? rmin - 1 : 1;
        int hi = rmax < H ? rmax + 1 : H;
        uint64_t any = 0;
        int tmin = 0x7FFF;
        /* Fungus: food we can already reach may be eaten on the way, which
         * keeps our own tail in place one step longer per food (at most one
         * food per step taken). */
        int delay = 0;
        if (food_delay) {
            delay = food_in;
            if (delay > t - t0) delay = t - t0;
        }
        for (int i = lo; i <= hi; i++) {
            uint64_t x = reach.r[i];
            uint64_t ex = (x | (x << 1) | (x >> 1) | reach.r[i - 1] | reach.r[i + 1]) & ~x;
            if (allow) ex &= allow->r[i];
            uint64_t n = ex & pass[i];
            if (release) {
                uint64_t dyn = ex & ever[i] & ~pass[i]; /* bordering body cells */
                while (dyn) {
                    int bit = __builtin_ctzll(dyn);
                    dyn &= dyn - 1;
                    uint16_t c = (uint16_t)(i * BSTRIDE + bit);
                    int f = fa[c];
                    if (delay && cell_owner(b->grid[c]) == b->self) f += delay;
                    if (f <= t + 1) n |= (uint64_t)1 << bit;
                    else if (f < tmin) tmin = f;
                }
            }
            nxt[i] = n;
            any |= n;
        }
        if (any) {
            t++;
            for (int i = lo; i <= hi; i++) reach.r[i] |= nxt[i];
            if (food_delay)
                for (int i = lo; i <= hi; i++) food_in += __builtin_popcountll(nxt[i] & b->food.r[i]);
            if (nxt[lo]) rmin = lo;
            if (nxt[hi]) rmax = hi;
            r->layers++;
            r->t_end = (uint16_t)t;
            if (!r->tail_t && self_tail != CELL_NONE && bb_test(&reach, self_tail)) r->tail_t = (uint16_t)t;
            if (t >= max_t) break;
            continue;
        }
        /* Nothing free next to the region: we can only wait for the next
         * release if a simple path inside the region lasts that long (we
         * entered at t0 and cannot stand still). */
        if (release && tmin < 0x7FFF && tmin <= max_t) {
            int cells = 0, black = 0;
            for (int i = rmin; i <= rmax; i++) {
                cells += __builtin_popcountll(reach.r[i]);
                black += __builtin_popcountll(reach.r[i] & b->black.r[i]);
            }
            int nc = bb_test(&b->black, start) ? black : cells - black;
            int pb = 2 * nc < 2 * (cells - nc) + 1 ? 2 * nc : 2 * (cells - nc) + 1;
            if (tmin <= t0 + pb - 1) {
                t = tmin - 1;
                continue;
            }
            r->stuck = 1; /* a release is coming, but too late for us */
            r->stuck_t = (uint16_t)t;
        } else if (release) {
            r->closed = 1;
        }
        break;
    }

    if (opts && opts->verdict_only) {
        if (region_out) *region_out = reach;
        return true;
    }
    region_finish(b, &reach, rmin, rmax, start, opts && opts->peel, r);
    if (opts && opts->split && !release) split_at_start(b, &reach, rmin, rmax, start, t0, opts, r);

    if (opts && opts->corridor) {
        int corr = 0;
        for (int i = rmin; i <= rmax; i++) {
            uint64_t p = pass[i], np = ~p;
            uint64_t narrow = p & (((np << 1) & (np >> 1)) | (~pass[i - 1] & ~pass[i + 1]));
            corr += __builtin_popcountll(reach.r[i] & narrow);
        }
        r->corridor = (uint16_t)corr;
    }
    if (region_out) *region_out = reach;
    return true;
}

/* ------------------------------------------------------------------ */
void voronoi_compute(const Board *b, Voronoi *v, int release)
{
    if (++v->gen == 0) {
        memset(v->mark, 0, sizeof v->mark);
        v->gen = 1;
    }
    const uint16_t gen = v->gen;
    const uint16_t *fa = b->free_at;
    const uint8_t *grid = b->grid;
    int qh = 0, qt = 0;

    memset(v->cells, 0, sizeof v->cells);
    memset(v->food, 0, sizeof v->food);
    v->contested = 0;

    for (int s = 0; s < b->nsnakes; s++) {
        if (!b->s[s].alive) continue;
        uint16_t h = board_head(b, s);
        v->mark[h] = gen;
        v->owner[h] = (uint8_t)s;
        v->dist[h] = 0;
        v->queue[qt++] = h;
    }

    while (qh < qt) {
        uint16_t c = v->queue[qh++];
        uint8_t o = v->owner[c];
        if (o == VOR_CONTESTED) continue;
        int d = v->dist[c] + 1;
        for (int k = 0; k < 4; k++) {
            uint16_t n = (uint16_t)((int)c + dir_off(k));
            int f = fa[n];
            if (release ? (f > d) : (f != 0)) continue;
            if (v->mark[n] != gen) {
                v->mark[n] = gen;
                v->owner[n] = o;
                v->dist[n] = (uint16_t)d;
                v->queue[qt++] = n;
                v->cells[o]++;
                if (grid[n] == CELL_FOOD) v->food[o]++;
            } else if (v->dist[n] == d && v->owner[n] != o && v->owner[n] != VOR_CONTESTED) {
                uint8_t po = v->owner[n];
                v->cells[po]--;
                if (grid[n] == CELL_FOOD) v->food[po]--;
                v->owner[n] = VOR_CONTESTED;
                v->contested++;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
void move_eval(const Board *b, int dir, MoveEval *e)
{
    const int self = b->self;
    memset(e, 0, sizeof *e);
    uint16_t t = board_target(b, self, dir);
    uint8_t v = b->grid[t];
    e->target = t;
    e->legal = (uint8_t)cell_passable(v);
    e->ate = (uint8_t)(v == CELL_FOOD);
    for (int d = 0; d < 4; d++) {
        uint8_t g = b->grid[(int)t + dir_off(d)];
        if (cell_is_head(g) && cell_owner(g) != self && b->s[cell_owner(g)].alive) e->threatened = 1;
    }
    if (!e->legal) return;
    FloodOpts so = { .corridor = 1 }, dyo = { .release = 1, .corridor = 1 };
    flood_region(b, t, 1, &so, &e->stat, NULL);
    flood_region(b, t, 1, &dyo, &e->dyn, NULL);
    e->margin = flood_margin(b, &e->stat, 1);
}
