#define _POSIX_C_SOURCE 199309L /* clock_gettime */
/* Fungus: one bot, two playing styles, the Fungus flood underneath.
 *  - opponents alive: worst-case replies of nearby
 *    opponents, static margin, Voronoi food race, trap bonus;
 *  - true solo (n_snakes == 1): planned food routes plus a survival PROOF:
 *    a tail-chase certificate (time-aware BFS from the head to an own body cell
 *    that has freed when we get there; food on the way delays the body) and a
 *    "deficit" BFS (steps of waiting still needed) that guides a short DFS and a
 *    greedy rollout; the Fungus flood (`stuck`) refutes dead lines. Proven moves
 *    rank above everything else; hunger relaxes this towards the W*H/2 no-food
 *    end, where gambling costs nothing.
 *  - every search honours a per-decision CPU deadline (deep_ms).
 * Defaults below are the tuned parameter sets; a params file passed to the
 * pipe bot still overrides them. */

#include "brain.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "board.h"
#include "flood.h"

typedef struct {
    double w_safe;       /* worst-case region is ok (tail reachable / margin >= 0) */
    double w_space;      /* per cell of worst-case region, capped */
    double space_cap;    /* cap in units of own length (+ space_cap_add) */
    double space_cap_add;
    double w_trap;       /* opponent's best margin goes from >= 0 to < 0 */
    double w_squeeze;    /* per cell the opponent's best region shrinks */
    double w_food;       /* * (1 - dist/food_horizon) to the closest food we win */
    double food_horizon;
    double route_tries;  /* nearest race-won foods to try a safe route to */
    double w_food_long;  /* food weight multiplier when we are already longest */
    double w_terr;       /* per Voronoi cell owned */
    double w_now_ok;     /* region ok ignoring opponent replies */
    double margin_frac;  /* required margin = max(min_margin, margin_frac * len) */
    double min_margin;
    double w_margin;     /* per unit of (capped) margin */
    double use_dyn_space;
    double w_lost;       /* per reachable cell our move cuts off */
    double w_hug;        /* per blocked neighbour of our new head */
    double near_dist;    /* opponents with head within this manhattan distance reply */
    double opp_slack;    /* board opp_slack: 1 = assume opponents move after us */
    /* x3 */
    double w_cert;       /* bonus: own body reachable after the move with a one-step buffer */
    double w_cert0;      /* bonus: reachable without buffer (only if not with buffer) */
    double cert_food;    /* 1: a food route is also acceptable when the certificate holds after eating */
    double cert_food_buf;/* buffer used for that certificate */
    double detour_max;   /* square detours tried per food route when the plain route fails */
    double hungry_steps; /* steps without growth before food acceptance is relaxed */
    double hungry_margin;/* relaxed required margin for the post-eat region */
    double hungry_food;  /* food weight multiplier growth per hungry_steps of hunger */
    double hungry_margin2; /* required margin after 2*hungry_steps without growth */
    double food_bfs_dyn; /* food BFS passes cells that free in time: 0 never, 1 always, 2 after hungry_dyn_steps */
    double hungry_dyn_steps;
    double route_check;  /* 1: play the route; 0: nearest race-won food by static BFS; 2: Voronoi-distance term */
    double use_peel;     /* safety flood: peel 1-wide dead ends */
    double use_split;    /* safety flood: best side once we stand on the next cell */
    double deep_depth;   /* survival search: own moves to look ahead (0 = off) */
    double deep_nodes;   /* node budget per candidate; running out counts as "survives" */
    double w_deep;       /* bonus for a candidate with a proven survivable line */
    double deep_ms;      /* CPU budget for all survival searches of one decision */
    double use_fungus;   /* 1: safe needs margin AND Fungus; 2: Fungus alone decides */
    double w_eat;        /* bonus for eating on a proven move */
    double hug_min_len;  /* hugging only pays once the snake is this long */
    double starve_f;     /* hunger fraction of the no-food limit that triggers the guard (0 = off) */
    double starve_mult;  /* food weight multiplier once the guard triggers */
    double vor_safety;   /* multi: our safety floods only use cells we reach first
                            (1: opponent-first cells removed, 2: contested cells too) */
    /* x1: survival proof */
    double proof_depth;  /* max own-move lookahead of the proof search (-1 = off, 0 = certificate only) */
    double proof_nodes;  /* node budget per proof search */
    double proof_buf;    /* certificate buffer: free cells in the tail-chase loop beyond 1 */
    double proof_prune;  /* 1: Fungus stuck at a node refutes that line */
    double proof_fallback; /* 1: when the buffered proof fails, try buffer 0 (level 2) */
    double w_proven;     /* score per proof level (3 = proven with buffer, 2 = proven buffer 0, 1 = unknown) */
    double w_proven0;
    double w_unknown;
    double proof_food;   /* food routes: 0 old check, 1 proof decides, 2 proof AND old check */
    double hungry_proof0_f; /* hunger (fraction of the no-food limit) after which level-2 routes are accepted */
    double hungry_unknown_f;/* ... unknown routes */
    double hungry_any_f;    /* ... any route */
    double proof_order;  /* DFS move order: 0 straight first, 1 toward own tail first */
    double proof_rollout;/* max steps of the greedy rollout leaf (0 = off) */
    double rollout_fungus; /* 1: rollout rejects moves that are Fungus-stuck */
    double rollout_policy; /* 0: fewest free neighbours, then deficit; 1: deficit, then fewest free neighbours;
                              2: hug (policy 0) for deficit/2 steps, then policy 1; 3: policy 1, then 0 if it failed */
    double move_relax;     /* 1: hunger fractions also lift unknown/level-2 candidate moves to proven */
    double w_walk;         /* unknown/level-2 candidates: score per step the rollout could walk */
    double walk_end;       /* 1: a walk that lasts until the no-food end rule counts as proven */
} params_t;

static params_t PB = {
    .w_safe = 1e6,
    .w_space = 1000,
    .space_cap = 1.0,
    .space_cap_add = 10,
    .w_trap = 3000,
    .w_squeeze = 20,
    .w_food = 2000,
    .food_horizon = 64,
    .route_tries = 3,
    .w_food_long = 0.5,
    .w_terr = 0,
    .w_now_ok = 2e5,
    .margin_frac = 0.04,
    .min_margin = 10,
    .w_margin = 50,
    .use_dyn_space = 1,
    .w_lost = 40,
    .w_hug = 20,
    .near_dist = 4,
    .opp_slack = 1,
    .w_cert = 5e5,
    .w_cert0 = 1e5,
    .cert_food = 1,
    .cert_food_buf = 1,
    .detour_max = 4,
    .hungry_steps = 150,
    .hungry_margin = 0,
    .hungry_food = 1,
    .hungry_margin2 = -1e9,
    .food_bfs_dyn = 2,
    .hungry_dyn_steps = 450,
    .route_check = 1,
};
static params_t PS;     /* set used when no opponent is alive ("solo.name value" overrides) */
static params_t P;      /* set in force for the current decision */
static int g_params_loaded;
static int g_last_len, g_last_grow;

static Board B;
static Voronoi V;
static int g_n;

static void set_param(params_t *T, const char *key, double val) {
#define SETP(k) if (!strcmp(key, #k)) T->k = val;
    SETP(w_safe) SETP(w_space) SETP(space_cap) SETP(space_cap_add) SETP(w_trap)
    SETP(w_squeeze) SETP(w_food) SETP(food_horizon) SETP(route_tries) SETP(w_food_long) SETP(w_terr) SETP(w_now_ok)
    SETP(near_dist) SETP(w_lost) SETP(w_hug) SETP(margin_frac) SETP(min_margin) SETP(w_margin) SETP(use_dyn_space) SETP(opp_slack)
    SETP(w_cert) SETP(w_cert0) SETP(cert_food) SETP(cert_food_buf) SETP(detour_max) SETP(hungry_steps) SETP(hungry_margin) SETP(hungry_food) SETP(hungry_margin2) SETP(food_bfs_dyn) SETP(hungry_dyn_steps) SETP(route_check)
    SETP(use_peel) SETP(use_split) SETP(deep_depth) SETP(deep_nodes) SETP(w_deep) SETP(deep_ms) SETP(use_fungus) SETP(vor_safety) SETP(w_eat) SETP(hug_min_len) SETP(starve_f) SETP(starve_mult)
    SETP(proof_depth) SETP(proof_nodes) SETP(proof_buf) SETP(proof_prune) SETP(proof_fallback) SETP(w_proven) SETP(w_proven0)
    SETP(w_unknown) SETP(proof_food) SETP(hungry_proof0_f) SETP(hungry_unknown_f) SETP(hungry_any_f) SETP(proof_order)
    SETP(proof_rollout) SETP(rollout_fungus) SETP(rollout_policy) SETP(move_relax) SETP(w_walk) SETP(walk_end)
#undef SETP
}

/* Tuned defaults: "name value" sets both parameter sets, "solo.name value"
 * only the one used in true solo games. */
static const char *const DEFAULT_PARAMS =
    "w_hug 0\nw_lost 0\nw_food 2000\nroute_check 2\nw_cert 0\nw_cert0 0\ncert_food 0\n"
    "hungry_steps 1e9\nfood_bfs_dyn 0\ndetour_max 0\nuse_peel 0\nuse_split 0\n"
    "solo.w_cert 0\nsolo.w_cert0 0\nsolo.cert_food 0\nsolo.cert_food_buf 0\nsolo.detour_max 4\n"
    "solo.hungry_steps 25\nsolo.hungry_margin 0\nsolo.hungry_food 3\nsolo.food_bfs_dyn 0\n"
    "solo.w_hug 40\nsolo.w_lost 0\nsolo.w_food 2000\nsolo.route_tries 3\nsolo.min_margin 16\n"
    "solo.space_cap 2\nsolo.food_horizon 400\nsolo.w_margin 100\nsolo.route_check 1\n"
    "solo.hungry_margin2 0\nsolo.use_peel 1\nsolo.use_split 1\n"
    "deep_depth 0\ndeep_nodes 200\nw_deep 4e6\ndeep_ms 3\nsolo.deep_depth 0\nsolo.use_peel 0\nsolo.use_fungus 2\n"
    "proof_depth -1\nproof_nodes 64\nproof_buf 1\nproof_prune 1\nproof_fallback 1\nw_proven 3e7\nw_proven0 2e7\nw_unknown 1e7\n"
    "proof_food 0\nhungry_proof0_f 0.3\nhungry_unknown_f 0.6\nhungry_any_f 2\nproof_order 1\nproof_rollout 0\nrollout_fungus 0\nrollout_policy 0\nmove_relax 1\nw_walk 0\nwalk_end 0\n"
    /* tuned solo proof (bench/fungus/final.txt) */
    "solo.proof_depth 2\nsolo.proof_food 1\nsolo.proof_rollout 150\nsolo.proof_nodes 300\n"
    "solo.rollout_policy 1\nsolo.hungry_unknown_f 0.3\nsolo.deep_ms 2\n"
    "vor_safety 1\n"
    /* starvation guard: at 70% of the no-food limit food outweighs shape terms */
    "solo.starve_f 0.7\nsolo.starve_mult 50\n";

/* Two passes so base keys reach the solo set before its overrides. */
static void apply_params(const char *text) {
    char key[64];
    double val;
    int n;
    for (const char *t = text; sscanf(t, "%63s %lf%n", key, &val, &n) == 2; t += n)
        if (strncmp(key, "solo.", 5)) set_param(&PB, key, val);
    PS = PB;
    for (const char *t = text; sscanf(t, "%63s %lf%n", key, &val, &n) == 2; t += n)
        if (!strncmp(key, "solo.", 5)) set_param(&PS, key + 5, val);
}

static void load_defaults(void) {
    if (g_params_loaded) return;
    apply_params(DEFAULT_PARAMS);
    g_params_loaded = 1;
}

/* A params file is applied on top of the defaults. */
void brain_set_params(const char *path) {
    static char buf[8192];
    FILE *f = fopen(path, "r");
    if (!f) return;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = 0;
    fclose(f);
    load_defaults();
    params_t solo = PS;
    /* base keys: start from current base; solo keys: from current solo */
    char key[64];
    double val;
    int k;
    for (const char *t = buf; sscanf(t, "%63s %lf%n", key, &val, &k) == 2; t += k)
        if (strncmp(key, "solo.", 5)) { set_param(&PB, key, val); set_param(&solo, key, val); }
    PS = solo;
    for (const char *t = buf; sscanf(t, "%63s %lf%n", key, &val, &k) == 2; t += k)
        if (!strncmp(key, "solo.", 5)) set_param(&PS, key + 5, val);
}

void brain_init(const brain_init_t *in) {
    BoardInit bi;
    load_defaults();
    P = PB;
    memset(&bi, 0, sizeof bi);
    bi.width = in->width;
    bi.height = in->height;
    bi.free_value = (uint8_t)in->free_value;
    bi.food_value = (uint8_t)in->food_value;
    bi.blocked_value = (uint8_t)in->blocked_value;
    bi.snake_count = in->n_snakes > MAX_SNAKES ? MAX_SNAKES : in->n_snakes;
    bi.opp_slack = (int)P.opp_slack;
    for (int i = 0; i < bi.snake_count; i++) {
        bi.ids[i] = in->snake_ids[i];
        bi.head_value[i] = (uint8_t)in->head_values[i];
        bi.body_value[i] = (uint8_t)in->body_values[i];
        bi.start_x[i] = (int16_t)in->start_x[i];
        bi.start_y[i] = (int16_t)in->start_y[i];
        if (in->snake_ids[i] == in->self_id) bi.self_index = i;
    }
    bi.base_map = in->base_map;
    g_n = bi.snake_count;
    board_init(&B, &bi);
    g_last_len = 0;
    g_last_grow = 0;
}

typedef struct {
    int ok;      /* best next region is ok */
    int space;   /* cells in that region (capped by caller) */
    int margin;
} region_t;

/* Best region snake s can enter with its next move from the current
 * (possibly hypothetical) board, from s's own point of view.
 * ok uses the static region (bodies stay walls, only s's own body frees),
 * like Rufus' area check: margin = path_bound - food - steps until the
 * nearest own border cell frees, and it must reach a safety margin.
 * space uses the time-aware region, which also sees opponent tails leave. */
static int g_need_override = -(1 << 30); /* > -2^30: required margin for food-route checks */
/* Cells we reach before any opponent (built per candidate move in multi);
 * our own safety floods may only grow into them. */
static BB g_allow;
static int g_allow_on;

static region_t best_region(int s) {
    region_t best = {0, -1, -(1 << 20)};
    int saved = B.self;
    B.self = s;
    int t0 = B.s[s].moves + 1;
    int need = (int)(P.margin_frac * B.s[s].len);
    if (need < P.min_margin) need = (int)P.min_margin;
    if (g_need_override > -(1 << 30)) need = g_need_override;
    FloodOpts so = { .peel = (uint8_t)P.use_peel, .split = (uint8_t)P.use_split },
              dyo = { .release = 1, .food_delay = (uint8_t)(P.use_fungus > 0) };
    if (g_allow_on && s == saved) so.allow = dyo.allow = &g_allow;
    for (int d = 0; d < 4; d++) {
        if (!board_legal(&B, s, d)) continue;
        uint16_t t = board_target(&B, s, d);
        FloodResult rs, rd;
        if (!flood_region(&B, t, t0, &so, &rs, NULL)) continue;
        int margin = flood_margin(&B, &rs, t0);
        int ok = margin >= need;
        int space = rs.cells;
        int have_dyn = (P.use_dyn_space || P.use_fungus) && flood_region(&B, t, t0, &dyo, &rd, NULL);
        if (have_dyn && P.use_dyn_space) space = rd.cells;
        /* Fungus: the time-aware flood (tails release, food we pass delays
         * our own releases) must never run out of room before a release */
        if (P.use_fungus) {
            int fungus_ok = have_dyn && !rd.stuck;
            ok = P.use_fungus >= 2 ? fungus_ok : (ok && fungus_ok);
        }
        if (ok > best.ok || (ok == best.ok && (margin > best.margin ||
                                               (margin == best.margin && space > best.space)))) {
            best.ok = ok;
            best.space = space;
            best.margin = margin;
        }
    }
    B.self = saved;
    if (best.space < 0) best.space = 0;
    return best;
}

/* Cells reachable (static, bodies are walls) from any of s's next moves. */
static int reach_union(int s) {
    BB u, r;
    bb_clear(&u);
    int t0 = B.s[s].moves + 1;
    FloodOpts so = { 0 };
    for (int d = 0; d < 4; d++) {
        if (!board_legal(&B, s, d)) continue;
        uint16_t t = board_target(&B, s, d);
        if (bb_test(&u, t)) continue;
        FloodResult fr;
        if (!flood_region(&B, t, t0, &so, &fr, &r)) continue;
        for (int i = 0; i < BROWS; i++) u.r[i] |= r.r[i];
    }
    return bb_count(&u);
}

/* ---- food route lookahead ---------------------------------------- */
static uint16_t RQ[BCELLS], RPAR[BCELLS], RDIST[BCELLS], RMARK[BCELLS];
static uint16_t rgen;
static MoveUndo RUNDO[BCELLS];

/* ---- body-reachability certificate ------------------------------- */
/* Time-aware BFS from s's (possibly hypothetical) head over states
 * (cell, e), e = foods eaten on the way (capped at EMAX): a cell is
 * enterable at BFS distance d if free_at <= moves + d - e - buf (each food
 * eaten delays the whole body by one step; buf = safety buffer). If any own
 * body cell is enterable this way we can follow our body forever (each cell
 * ahead frees exactly one step later), so the position is survivable.
 * reach = distinct non-body cells entered. */
#define EMAX 2
static uint16_t TQ[(EMAX + 1) * BCELLS], TDIST[(EMAX + 1) * BCELLS], TMARK[(EMAX + 1) * BCELLS];
static uint16_t CMARK[BCELLS];
static uint16_t tgen;

static int body_reach(int s, int buf, int *reach_out) {
    if (++tgen == 0) { memset(TMARK, 0, sizeof TMARK); memset(CMARK, 0, sizeof CMARK); tgen = 1; }
    uint16_t h = board_head(&B, s);
    int base = B.s[s].moves - buf;
    int qh = 0, qt = 0, safe = 0, reach = 0;
    TQ[qt++] = h;
    TMARK[h] = tgen;
    TDIST[h] = 0;
    CMARK[h] = tgen;
    while (qh < qt && !safe) {
        uint16_t st = TQ[qh++];
        int e = st / BCELLS;
        uint16_t c = (uint16_t)(st - e * BCELLS);
        int d = TDIST[st] + 1;
        for (int k = 0; k < 4; k++) {
            uint16_t nb = (uint16_t)((int)c + dir_off(k));
            if ((int)B.free_at[nb] > base + d - e) continue;
            uint8_t g = B.grid[nb];
            if (cell_is_snake(g) && cell_owner(g) == s) { safe = 1; break; }
            int e2 = e + (g == CELL_FOOD);
            if (e2 > EMAX) e2 = EMAX;
            uint16_t st2 = (uint16_t)(e2 * BCELLS + nb);
            if (TMARK[st2] == tgen) continue;
            TMARK[st2] = tgen;
            TDIST[st2] = (uint16_t)d;
            TQ[qt++] = st2;
            if (CMARK[nb] != tgen) { CMARK[nb] = tgen; reach++; }
        }
    }
    if (reach_out) *reach_out = reach;
    return safe;
}

/* position after a food route acceptable? margin-ok (possibly relaxed when
 * hungry) or the certificate */
static int g_route_need = -(1 << 30);
static int g_hungry;
static int prove_level(int s);
static int g_hunger_limit;
static int g_walk; /* longest rollout walk (steps) of the last prove_level call */
static int after_route_ok(int me) {
    if (P.proof_food > 0) {
        int lvl = prove_level(me);
        if (P.walk_end && lvl >= 1 && lvl < 3 && g_walk >= g_hunger_limit) lvl = 3;
        double hf = (double)g_hungry / g_hunger_limit;
        int acc = lvl >= 3 || (lvl >= 2 && hf > P.hungry_proof0_f) || (lvl >= 1 && hf > P.hungry_unknown_f) ||
                  hf > P.hungry_any_f;
        if (!acc || P.proof_food < 2) return acc;
    }
    g_need_override = g_route_need;
    int ok = best_region(me).ok;
    g_need_override = -(1 << 30);
    if (ok) return 1;
    return P.cert_food && body_reach(me, (int)P.cert_food_buf, NULL);
}

/* Play path[0..n-1] (cells, first adjacent to the head), test, undo. */
static int play_check(int me, const uint16_t *path, int n) {
    int applied = 0, ok = 1;
    for (int i = 0; i < n; i++) {
        int d = dir_between(board_head(&B, me), path[i]);
        if (d < 0 || !board_apply(&B, me, d, &RUNDO[applied])) { ok = 0; break; }
        applied++;
    }
    if (ok) ok = after_route_ok(me);
    while (applied > 0) board_undo(&B, &RUNDO[--applied]);
    return ok;
}

/* The BFS certificate cannot waste steps, so after eating (the tail waits
 * one step) a snake following its tail closely looks trapped although a
 * two-step detour around a free square fixes it: try the plain path, then
 * the path with one square detour inserted (at most detour_max tries). */
static int route_ok(int me, const uint16_t *path, int n) {
    if (play_check(me, path, n)) return 1;
    int tries = 0, max = (int)P.detour_max;
    if (max <= 0) return 0;
    static uint16_t alt[BCELLS + 4];
    uint16_t head = board_head(&B, me);
    for (int i = 0; i <= n; i++) {
        uint16_t qi = i == 0 ? head : path[i - 1];
        for (int fd = 0; fd < 4; fd++) {
            uint16_t qn;
            if (i < n) {
                qn = path[i];
                if (dir_between(qi, qn) != fd) continue;
            } else {
                qn = (uint16_t)((int)qi + dir_off(fd));
                if (!cell_passable(B.grid[qn])) continue;
            }
            for (int side = 0; side < 2; side++) {
                int sd = (fd + 1 + 2 * side) & 3;
                uint16_t a = (uint16_t)((int)qi + dir_off(sd)), bq = (uint16_t)((int)qn + dir_off(sd));
                if (!cell_passable(B.grid[a]) || !cell_passable(B.grid[bq])) continue;
                int m = 0;
                for (int j = 0; j < i; j++) alt[m++] = path[j];
                alt[m++] = a;
                alt[m++] = bq;
                for (int j = i; j < n; j++) alt[m++] = path[j];
                if (i == n) alt[m++] = qn;
                if (play_check(me, alt, m)) return 1;
                if (++tries >= max) return 0;
            }
        }
    }
    return 0;
}

/* Route to `target` along the BFS parents: acceptable if the position after
 * eating is ok (margin or certificate), trying square detours. */
static int route_safe(int me, uint16_t target) {
    static uint16_t path[BCELLS];
    int n = 0;
    for (uint16_t c = target; RPAR[c] != CELL_NONE; c = RPAR[c]) path[n++] = c;
    for (int i = 0; i < n / 2; i++) { uint16_t t = path[i]; path[i] = path[n - 1 - i]; path[n - 1 - i] = t; }
    return route_ok(me, path, n);
}

/* Closeness (1 - dist/horizon) of the nearest food we win the race to and
 * can eat safely; tries the route_tries nearest such foods. */
static double food_route(int me) {
    if (++rgen == 0) { memset(RMARK, 0, sizeof RMARK); rgen = 1; }
    uint16_t h = board_head(&B, me);
    int qh = 0, qt = 0, tries = 0, base = B.s[me].moves;
    int dyn = P.food_bfs_dyn == 1 || (P.food_bfs_dyn == 2 && g_hungry > P.hungry_dyn_steps);
    RQ[qt++] = h;
    RMARK[h] = rgen;
    RPAR[h] = CELL_NONE;
    RDIST[h] = 0;
    while (qh < qt) {
        uint16_t c = RQ[qh++];
        if (c != h && B.grid[c] == CELL_FOOD && voronoi_owner(&V, c) == me) {
            if (P.route_check == 0 || route_safe(me, c)) return 1.0 - RDIST[c] / P.food_horizon;
            if (++tries >= (int)P.route_tries) return 0;
        }
        for (int k = 0; k < 4; k++) {
            uint16_t nb = (uint16_t)((int)c + dir_off(k));
            if (RMARK[nb] == rgen) continue;
            if (dyn ? (int)B.free_at[nb] > base + RDIST[c] + 1 : !cell_passable(B.grid[nb])) continue;
            RMARK[nb] = rgen;
            RPAR[nb] = c;
            RDIST[nb] = (uint16_t)(RDIST[c] + 1);
            RQ[qt++] = nb;
        }
    }
    return 0;
}

static int manhattan(uint16_t a, uint16_t b) {
    int dx = cell_x(a) - cell_x(b), dy = cell_y(a) - cell_y(b);
    return (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
}

static double region_value(region_t r, int cap) {
    int m = r.margin < cap ? r.margin : cap;
    return P.w_safe * r.ok + P.w_space * (r.space < cap ? r.space : cap) + P.w_margin * m;
}

/* ---- survival proof ---------------------------------------------------
 * Tail-chase certificate: a time-aware BFS from s's (possibly hypothetical)
 * head. A cell is enterable at BFS distance d if free_at <= moves + d - e - buf
 * (e = foods eaten on the path so far: each delays the whole body one step;
 * buf = safety buffer). Each cell is visited once, so the BFS parent chain is
 * a simple path. If it reaches an own body cell we can follow the body (every
 * cell ahead frees exactly one step later) and the loop path+body has at
 * least buf+1 free cells: survivable for ever, and one food spawn in the loop
 * is absorbed per spare cell. */
static uint16_t CQ[BCELLS];
static uint32_t CW[BCELLS];
static uint16_t cgen;

static int cert_deficit(int s, int buf);

/* Same BFS as cert_deficit: the certificate holds iff the deficit is <= 0. */
static int cert_reach(int s, int buf) {
    return cert_deficit(s, buf) <= 0;
}

/* Certificate deficit: the same BFS, but over the whole reachable set; for
 * every own body cell c next to a reached cell x (distance d(x)+1) the
 * deficit is free_at[c] - (moves - buf + d(x) + 1 - e(x)): the number of
 * extra steps we would have to waste before c is enterable. Returns the
 * minimum (<= 0: certificate holds), DEF_INF if no body cell borders the
 * reachable set. */
#define DEF_INF 30000
static int cert_deficit(int s, int buf) {
    /* One 32-bit word per cell: generation in the high half, d - e (BFS
     * distance minus foods on the path) in the low half — a single load per
     * neighbour. Callers only need the exact value while it is positive, so
     * the search stops at the first body cell we reach in time. */
    if (++cgen == 0) { memset(CW, 0, sizeof CW); cgen = 1; }
    const uint32_t tag = (uint32_t)cgen << 16;
    uint16_t h = board_head(&B, s);
    const int base = B.s[s].moves - buf;
    int qh = 0, qt = 0, best = DEF_INF;
    CQ[qt++] = h;
    CW[h] = tag; /* d - e = 0 */
    while (qh < qt) {
        uint16_t c = CQ[qh++];
        const int k = (int)(uint16_t)CW[c] + 1; /* d - e for a step out of c */
        const int lim = base + k;
        for (int dk = 0; dk < 4; dk++) {
            uint16_t nb = (uint16_t)((int)c + dir_off(dk));
            if ((CW[nb] & 0xFFFF0000u) == tag) continue;
            uint8_t g = B.grid[nb];
            int fa = (int)B.free_at[nb];
            if (cell_is_snake(g) && cell_owner(g) == s) {
                int def = fa - lim;
                if (def < best) {
                    best = def;
                    if (best <= 0) return best;
                }
                continue;
            }
            if (fa > lim) continue;
            CW[nb] = tag | (uint16_t)(k - (g == CELL_FOOD));
            CQ[qt++] = nb;
        }
    }
    return best;
}

/* Fungus refutation: the time-aware flood from the current head (entered at
 * move `moves`) runs out of room before the next release. Sound: the flood
 * region is a superset of what we can reach and the parity bound caps the
 * steps a simple path can last. No food delay (we might avoid the food). */
static int fungus_stuck_here(int s) {
    uint16_t h = board_head(&B, s);
    uint16_t saved = B.free_at[h];
    int saved_self = B.self;
    FloodOpts dyo = { .release = 1, .verdict_only = 1 };
    FloodResult r;
    B.self = s;
    B.free_at[h] = 0;
    int ok = flood_region(&B, h, B.s[s].moves, &dyo, &r, NULL);
    B.free_at[h] = saved;
    B.self = saved_self;
    return ok && r.stuck;
}

enum { PR_DEAD = 0, PR_UNKNOWN = 1, PR_PROVEN = 2 };
static MoveUndo PUNDO[64];
static int g_pnodes, g_pbudget, g_pbuf;
static uint64_t g_deadline_ns;
static long g_stat_nodes, g_stat_searches; /* debug counters */

static uint64_t cpu_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Depth-first over our own moves: PROVEN as soon as the certificate holds,
 * DEAD if every line is refuted (no legal move / Fungus stuck), UNKNOWN when
 * the depth, node or CPU budget ends a line. */
static int prove_dfs(int s, int depth) {
    if (cert_reach(s, g_pbuf)) return PR_PROVEN;
    if (depth == 0) return PR_UNKNOWN;
    if (++g_pnodes > g_pbudget) return PR_UNKNOWN;
    if ((g_pnodes & 3) == 0 && cpu_ns() > g_deadline_ns) { g_pbudget = 0; return PR_UNKNOWN; }
    if (P.proof_prune && fungus_stuck_here(s)) return PR_DEAD;
    uint16_t h = board_head(&B, s);
    int straight = B.s[s].len > 1 ? dir_between(board_body(&B, s, 1), h) : 0;
    int order[4], n = 0;
    if (P.proof_order >= 1 && B.s[s].len > 2) {
        /* toward our own tail first (manhattan), then straight, then the rest */
        uint16_t tail = board_tail(&B, s);
        int dist[4];
        for (int d = 0; d < 4; d++) dist[d] = cell_manhattan((uint16_t)((int)h + dir_off(d)), tail);
        for (int k = 0; k < 4; k++) {
            int d = (straight + k) & 3;
            if (k == 2) continue;
            int i = n;
            while (i > 0 && dist[order[i - 1]] > dist[d]) { order[i] = order[i - 1]; i--; }
            order[i] = d;
            n++;
        }
    } else {
        for (int k = 0; k < 4; k++) {
            if (k == 2 && B.s[s].len > 1) continue;
            order[n++] = (straight + k) & 3;
        }
    }
    int any_unknown = 0;
    for (int i = 0; i < n; i++) {
        MoveUndo *u = &PUNDO[depth];
        if (!board_apply(&B, s, order[i], u)) continue;
        int r = prove_dfs(s, depth - 1);
        board_undo(&B, u);
        if (r == PR_PROVEN) return PR_PROVEN;
        if (r == PR_UNKNOWN) any_unknown = 1;
    }
    return any_unknown ? PR_UNKNOWN : PR_DEAD;
}

/* Greedy rollout: at each step evaluate every legal move by the certificate
 * deficit after it (one BFS each); moves with no exit path are never taken,
 * a deficit <= 0 is the certificate (a constructive proof: the line exists).
 * Among the rest, rollout_policy 0 prefers the fewest passable neighbours
 * (wall/body hugging fills a pocket compactly and keeps the door open), then
 * the smaller deficit; policy 1 the other way round. Each step is a node. */
static MoveUndo ROLL[512];
static int g_roll_steps, g_roll_reason; /* debug: 0 proven, 1 dead end, 2 budget, 3 max steps */
static int rollout_proves(int s, int maxsteps, int policy) {
    int n = 0, proven = 0, hug_steps = 0;
    if (maxsteps > 500) maxsteps = 500;
    g_roll_reason = 3;
    if (policy == 2) {
        int def0 = cert_deficit(s, g_pbuf);
        hug_steps = def0 < DEF_INF ? def0 / 2 : 0;
    }
    while (n < maxsteps) {
        int hug = policy == 0 || (policy == 2 && n < hug_steps);
        if (++g_pnodes > g_pbudget) { g_roll_reason = 2; break; }
        if (cpu_ns() > g_deadline_ns) { g_pbudget = 0; g_roll_reason = 2; break; }
        uint16_t tail = board_tail(&B, s);
        int bestd = -1;
        long bestkey = 0;
        for (int d = 0; d < 4; d++) {
            MoveUndo u;
            if (!board_apply(&B, s, d, &u)) continue;
            int def = cert_deficit(s, g_pbuf);
            if (def <= 0) { ROLL[n++] = u; proven = 1; break; }
            if (def < DEF_INF && !(P.rollout_fungus && fungus_stuck_here(s))) {
                uint16_t t = board_head(&B, s);
                int nf = 0;
                for (int k = 0; k < 4; k++) nf += cell_passable(B.grid[(int)t + dir_off(k)]);
                if (nf == 0) nf = 8;
                long key = hug ? (long)nf * 65536 + def * 64 + cell_manhattan(t, tail)
                               : (long)def * 1024 + nf * 64 + cell_manhattan(t, tail);
                if (bestd < 0 || key < bestkey) { bestkey = key; bestd = d; }
            }
            board_undo(&B, &u);
        }
        if (proven) { g_roll_reason = 0; break; }
        if (bestd < 0) { g_roll_reason = 1; break; }
        board_apply(&B, s, bestd, &ROLL[n]);
        n++;
    }
    g_roll_steps = n;
    if (n > g_walk) g_walk = n;
    while (n > 0) board_undo(&B, &ROLL[--n]);
    return proven;
}

/* Iterative deepening 0, 2, 4, ... proof_depth under one node budget. */
static int prove(int s, int buf) {
    int maxd = (int)P.proof_depth;
    if (maxd < 0) return PR_UNKNOWN;
    g_pbuf = buf;
    g_pbudget = cpu_ns() > g_deadline_ns ? 0 : (int)P.proof_nodes; /* past the deadline: certificate only */
    g_pnodes = 0;
    g_stat_searches++;
    int r = PR_UNKNOWN;
    for (int d = 0;; d = d == 0 ? 2 : d * 2) {
        if (d > maxd) d = maxd;
        r = prove_dfs(s, d);
        if (r != PR_UNKNOWN || d >= maxd || g_pnodes > g_pbudget) break;
    }
    if (r == PR_UNKNOWN && P.proof_rollout > 0 && g_pnodes <= g_pbudget) {
        int pol = (int)P.rollout_policy;
        if (rollout_proves(s, (int)P.proof_rollout, pol == 3 ? 1 : pol)) r = PR_PROVEN;
        else if (pol == 3 && g_pnodes <= g_pbudget && rollout_proves(s, (int)P.proof_rollout, 0)) r = PR_PROVEN;
    }
    g_stat_nodes += g_pnodes;
    return r;
}

/* 3 = proven with buffer, 2 = proven with buffer 0, 1 = unknown, 0 = dead */
static int prove_level(int s) {
    g_walk = 0;
    if (P.proof_depth < 0) return 1;
    int r = prove(s, (int)P.proof_buf);
    if (r == PR_PROVEN) return 3;
    if (r == PR_DEAD) return 0;
    if (P.proof_fallback && P.proof_buf > 0) {
        r = prove(s, 0);
        if (r == PR_PROVEN) return 2;
        if (r == PR_DEAD) return 0;
    }
    return 1;
}

int brain_decide(const brain_step_t *step, int *dx, int *dy) {
    g_deadline_ns = cpu_ns() + (uint64_t)(P.deep_ms * 1e6);
    uint8_t alive[MAX_SNAKES];
    int len[MAX_SNAKES];
    for (int i = 0; i < g_n; i++) { alive[i] = step->alive[i]; len[i] = step->lengths[i]; }
    if (board_update(&B, step->map, alive, len) < 0) return -1;

    const int me = B.self;
    if (!B.s[me].alive) return -1;
    int my_len = B.s[me].len;
    int longest_other = 0, nopp_alive = 0;
    for (int s = 0; s < g_n; s++) {
        if (s == me || !B.s[s].alive) continue;
        nopp_alive++;
        if (B.s[s].len > longest_other) longest_other = B.s[s].len;
    }
    P = g_n == 1 ? PS : PB;
    int cap = (int)(P.space_cap * my_len + P.space_cap_add);
    double food_w = P.w_food * (my_len > longest_other + 2 ? P.w_food_long : 1.0);

    /* hunger: steps since we last grew */
    if (my_len > g_last_len || B.step <= 1) { g_last_len = my_len; g_last_grow = (int)B.step; }
    int hungry = (int)B.step - g_last_grow;
    g_hungry = hungry;
    g_hunger_limit = B.W * B.H / 2;
    g_route_need = -(1 << 30);
    if (P.hungry_steps > 0 && hungry > P.hungry_steps) {
        food_w *= 1.0 + P.hungry_food * (hungry - P.hungry_steps) / P.hungry_steps;
        g_route_need = hungry > 2 * P.hungry_steps ? (P.hungry_margin2 < -1e6 ? -(1 << 20) : (int)P.hungry_margin2) : (int)P.hungry_margin;
    }
    /* starvation guard: close to the no-food end of the game, food beats every
     * shape/margin term (proof levels still rank first); starving out and
     * dying score the same, so this costs nothing late and breaks loops early */
    if (P.starve_f > 0 && hungry > P.starve_f * g_hunger_limit) food_w *= P.starve_mult;

    /* opponents' best margins before our move, for the trap bonus */
    region_t opp_before[MAX_SNAKES];
    int near[MAX_SNAKES], nnear = 0;
    uint16_t my_head = board_head(&B, me);
    for (int s = 0; s < g_n; s++) {
        if (s == me || !B.s[s].alive) continue;
        if (manhattan(board_head(&B, s), my_head) > (int)P.near_dist) continue;
        near[nnear++] = s;
        opp_before[s] = best_region(s);
    }

    int reach_before = reach_union(me);
    int best_dir = -1;
    double best_score = -1e300;
    for (int d = 0; d < 4; d++) {
        MoveUndo u;
        if (!board_apply(&B, me, d, &u)) continue;
        uint16_t nh = board_head(&B, me);

        /* Voronoi after our move: food race below, and in multi the safety
         * floods only count cells we reach before every opponent, so a
         * corridor an opponent can close first no longer looks safe. */
        voronoi_compute(&B, &V, 1);
        g_allow_on = 0;
        if (P.vor_safety > 0 && nopp_alive > 0) {
            for (int i = 0; i < BROWS; i++) g_allow.r[i] = ~0ull;
            for (int y = 0; y < B.H; y++)
                for (int x = 0; x < B.W; x++) {
                    uint16_t c = cell_of(x, y);
                    int o = voronoi_owner(&V, c);
                    if ((o != VOR_NONE && o != VOR_CONTESTED && o != me) ||
                        (o == VOR_CONTESTED && P.vor_safety >= 2))
                        bb_reset(&g_allow, c);
                }
            g_allow_on = 1;
        }

        /* our region if nobody interferes, then worst case over replies */
        region_t now = best_region(me);
        double worst = region_value(now, cap);
        double trap = 0;
        for (int k = 0; k < nnear; k++) {
            int s = near[k];
            region_t after = best_region(s);
            if (opp_before[s].ok && !after.ok) trap += P.w_trap;
            int shrink = opp_before[s].space - after.space;
            if (shrink > 0) trap += P.w_squeeze * shrink;
            if (manhattan(board_head(&B, s), nh) > 3) continue;
            for (int od = 0; od < 4; od++) {
                MoveUndo ou;
                if (!board_apply(&B, s, od, &ou)) continue;
                double v = region_value(best_region(me), cap);
                if (v < worst) worst = v;
                board_undo(&B, &ou);
            }
        }

        g_allow_on = 0;
        /* food race and territory from the position after our move (V above) */
        double food = 0;
        if (u.ate) food = 1.0;
        else if (P.route_check == 2) {
            for (int y = 0; y < B.H; y++)
                for (int x = 0; x < B.W; x++) {
                    uint16_t c = cell_of(x, y);
                    if (B.grid[c] != CELL_FOOD || voronoi_owner(&V, c) != me) continue;
                    double f = 1.0 - voronoi_dist(&V, c) / P.food_horizon;
                    if (f > food) food = f;
                }
        } else food = food_route(me);

        /* shape: cells cut off from our reach by this move, and hugging */
        int lost = reach_before - 1 - reach_union(me);
        if (lost < 0) lost = 0;
        int hug = 0;
        for (int k = 0; k < 4; k++)
            if (!cell_passable(B.grid[(int)nh + dir_off(k)])) hug++;

        int cert = 0, cert0 = 0;
        if (P.w_cert != 0) cert = body_reach(me, 1, NULL);
        if (!cert && P.w_cert0 != 0) cert0 = body_reach(me, 0, NULL);

        int lvl = 1;
#ifdef BRAIN_DEBUG
        long nodes0 = g_stat_nodes;
#endif
        if (P.proof_depth >= 0) lvl = prove_level(me);
        int walk = g_walk;
        if (P.walk_end && lvl > 0 && lvl < 3 && walk >= g_hunger_limit - hungry) lvl = 3;
        if (P.move_relax && lvl > 0 && lvl < 3) {
            double hf = (double)hungry / g_hunger_limit;
            if ((lvl == 2 && hf > P.hungry_proof0_f) || hf > P.hungry_unknown_f) lvl = 3;
        }

        double score = worst + P.w_now_ok * now.ok + trap + food_w * food + P.w_terr * V.cells[me]
                       - P.w_lost * lost + (my_len >= P.hug_min_len ? P.w_hug * hug : 0) + P.w_cert * cert + P.w_cert0 * cert0
                       + (lvl == 3 ? P.w_proven : lvl == 2 ? P.w_proven0 : lvl == 1 ? P.w_unknown : 0)
                       /* the food term only measures closeness, so standing next to food
                        * scores like eating it; reward actually eating when the move is proven */
                       + (u.ate && lvl == 3 ? P.w_eat : 0)
                       + (lvl > 0 && lvl < 3 ? P.w_walk * walk : 0);
#ifdef BRAIN_DEBUG
        fprintf(stderr, "step %u len %d d%d ok %d space %d margin %d worst %.0f trap %.0f food %.3f terr %d hungry %d lvl %d nodes %ld roll %d/%d -> %.0f\n",
                B.step, my_len, d, now.ok, now.space, now.margin, worst, trap, food, V.cells[me], hungry, lvl, g_stat_nodes - nodes0, g_roll_steps, g_roll_reason, score);
#endif
        if (score > best_score) { best_score = score; best_dir = d; }
        board_undo(&B, &u);
    }
    if (best_dir < 0) return -1;
#ifdef BRAIN_DEBUG
    {
        uint16_t tail = board_tail(&B, me);
        fprintf(stderr, "step %u len %d hungry %d -> d%d (searches %ld nodes %ld)\n", B.step, my_len, hungry, best_dir,
                g_stat_searches, g_stat_nodes);
        for (int y = 0; y < B.H; y++) {
            char row[MAX_W + 1];
            for (int x = 0; x < B.W; x++) {
                uint16_t c = cell_of(x, y);
                uint8_t g = B.grid[c];
                row[x] = g == CELL_WALL ? '#' : g == CELL_FOOD ? '*' : g == CELL_FREE ? '.' : cell_is_head(g) ? '@' : c == tail ? 'T' : 'o';
            }
            row[B.W] = 0;
            fprintf(stderr, "  %s\n", row);
        }
    }
#endif
    *dx = dir_dx(best_dir);
    *dy = dir_dy(best_dir);
    return 0;
}
