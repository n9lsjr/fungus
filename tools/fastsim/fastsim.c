/* fastsim: a C re-implementation of the snake_sim rules used on gridsnake.com,
 * for running thousands of games quickly when tuning. Bots are separate
 * processes speaking src/shell/pipe_proto.h, so different builds can play
 * each other. Rules mirrored from snake_sim (snake_env.py, snake_handlers.py,
 * main_loop.py, food_handlers.py); validate important results in snake_sim.
 *
 * usage: fastsim -m m4|m6|s1|s2 -n games [-s seed] [-o games.tsv] bot[@params] ...
 *   Bots fill the seats round-robin; the seat assignment is shuffled per game. */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../../src/shell/pipe_proto.h"

#define MAXC 1024
#define MAXS 8
#define FREE_V 1
#define BLOCK_V 2
#define FOOD_V 0
#define FOOD_DECAY 500
#define START_LEN 2
#define LAST_STANDING_BUFFER 200
#define MAX_STEPS 20000

typedef struct {
    int w, h, n_snakes, food;
    uint8_t base[MAXC];
} mode_t_;

typedef struct {
    uint16_t body[MAXC]; /* ring buffer, body[(head + i) % MAXC] = i-th cell from head */
    int head, len, target;
    int alive, fatal_step;
    int hv, bv;
    int move_count, last_ate; /* SnakeRep.move_count / last_ate */
} snake_t;

typedef struct {
    const char *path, *params;
    FILE *to, *from;
    pid_t pid;
} botproc_t;

typedef struct {
    const char *name;
    double games, surv_rank, length, quality, wins;
    double cpu_ns, decisions;
    long max_cpu_ns;
} stats_t;

static uint64_t rng_state = 88172645463325252ull;
static uint64_t rng(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}
static int rnd(int n) { return (int)(rng() % (uint64_t)n); }

static mode_t_ M;
static uint8_t map_[MAXC];
static snake_t S[MAXS];
static int food_cell[256], food_decay[256], n_food;

static void load_text_map(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); exit(1); }
    if (fscanf(f, "%d %d", &M.w, &M.h) != 2) exit(1);
    char row[64];
    for (int y = 0; y < M.h; y++) {
        if (fscanf(f, "%63s", row) != 1) exit(1);
        for (int x = 0; x < M.w; x++) M.base[y * M.w + x] = row[x] == '#' ? BLOCK_V : FREE_V;
    }
    fclose(f);
}

static void set_mode(const char *m, const char *maps_dir) {
    char p[512];
    memset(&M, 0, sizeof M);
    if (!strcmp(m, "m4")) { M.w = 20; M.h = 15; M.n_snakes = 4; M.food = 7; }
    else if (!strcmp(m, "m6")) { M.w = 20; M.h = 20; M.n_snakes = 6; M.food = 10; }
    else if (!strcmp(m, "s1")) { snprintf(p, sizeof p, "%s/mini_items.txt", maps_dir); load_text_map(p); M.n_snakes = 1; M.food = 10; return; }
    else if (!strcmp(m, "s2")) { snprintf(p, sizeof p, "%s/mini_stairs.txt", maps_dir); load_text_map(p); M.n_snakes = 1; M.food = 10; return; }
    else { fprintf(stderr, "unknown mode %s\n", m); exit(1); }
    memset(M.base, FREE_V, sizeof M.base);
}

static int cell_at(const snake_t *s, int i) { return s->body[(s->head + i) % MAXC]; }

static void spawn(botproc_t *b) {
    int in[2], out[2];
    if (pipe(in) || pipe(out)) { perror("pipe"); exit(1); }
    b->pid = fork();
    if (b->pid == 0) {
        dup2(in[0], 0);
        dup2(out[1], 1);
        close(in[1]); close(out[0]);
        if (b->params) execl(b->path, b->path, b->params, (char *)NULL);
        else execl(b->path, b->path, (char *)NULL);
        perror(b->path);
        _exit(127);
    }
    close(in[0]); close(out[1]);
    b->to = fdopen(in[1], "w");
    b->from = fdopen(out[0], "r");
}

static void place_food(void) {
    /* decay first, then refill to M.food on random free cells (food_handlers.py) */
    for (int i = 0; i < n_food;) {
        if (--food_decay[i] <= 0) {
            if (map_[food_cell[i]] == FOOD_V) map_[food_cell[i]] = FREE_V;
            food_cell[i] = food_cell[n_food - 1];
            food_decay[i] = food_decay[n_food - 1];
            n_food--;
        } else i++;
    }
    if (n_food >= M.food) return;
    static int empty[MAXC];
    int ne = 0;
    for (int c = 0; c < M.w * M.h; c++) if (map_[c] == FREE_V) empty[ne++] = c;
    while (n_food < M.food && ne > 0) {
        int k = rnd(ne);
        int c = empty[k];
        empty[k] = empty[--ne];
        food_cell[n_food] = c;
        food_decay[n_food] = FOOD_DECAY;
        n_food++;
        map_[c] = FOOD_V;
    }
}

static void remove_food(int c) {
    for (int i = 0; i < n_food; i++)
        if (food_cell[i] == c) {
            food_cell[i] = food_cell[n_food - 1];
            food_decay[i] = food_decay[n_food - 1];
            n_food--;
            return;
        }
}

/* Apply one move for snake i (snake_env.move_snake + _update_snake_on_map). */
static void apply_move(int i, int ok, int dx, int dy, int step) {
    snake_t *s = &S[i];
    int head = cell_at(s, 0);
    int hx = head % M.w, hy = head / M.w;
    int nx = hx + dx, ny = hy + dy;
    int valid = ok && (abs(dx) + abs(dy) == 1) && nx >= 0 && ny >= 0 && nx < M.w && ny < M.h &&
                map_[ny * M.w + nx] <= FREE_V;
    if (!valid) { s->alive = 0; s->fatal_step = step; return; }
    int nc = ny * M.w + nx;
    if (map_[nc] == FOOD_V) { remove_food(nc); s->target++; s->last_ate = s->move_count; }
    s->move_count++;
    int old_tail = cell_at(s, s->len - 1);
    s->head = (s->head + MAXC - 1) % MAXC;
    s->body[s->head] = (uint16_t)nc;
    s->len++;
    while (s->len > s->target) s->len--;
    int old_head = s->len > 1 ? cell_at(s, 1) : old_tail;
    int new_tail = cell_at(s, s->len - 1);
    map_[old_head] = (uint8_t)s->bv;
    map_[nc] = (uint8_t)s->hv;
    map_[new_tail] = (uint8_t)s->bv;
    if (old_tail != new_tail) map_[old_tail] = FREE_V;
}

static void send_init(botproc_t *b, int seat) {
    pipe_init_t pi;
    memset(&pi, 0, sizeof pi);
    brain_init_t *in = &pi.init;
    in->width = M.w; in->height = M.h;
    in->free_value = FREE_V; in->blocked_value = BLOCK_V; in->food_value = FOOD_V;
    in->self_id = seat; in->start_length = START_LEN; in->n_snakes = M.n_snakes;
    for (int i = 0; i < M.n_snakes; i++) {
        in->snake_ids[i] = i;
        in->head_values[i] = S[i].hv;
        in->body_values[i] = S[i].bv;
        in->start_x[i] = cell_at(&S[i], 0) % M.w;
        in->start_y[i] = cell_at(&S[i], 0) / M.w;
    }
    fputc('I', b->to);
    fwrite(&pi, sizeof pi, 1, b->to);
    fwrite(M.base, 1, (size_t)(M.w * M.h), b->to);
    fflush(b->to);
}

static void send_step(botproc_t *b, const uint8_t *snap) {
    pipe_step_t ps;
    memset(&ps, 0, sizeof ps);
    brain_step_t *st = &ps.step;
    st->n_snakes = M.n_snakes;
    for (int i = 0; i < M.n_snakes; i++) { st->alive[i] = (uint8_t)S[i].alive; st->lengths[i] = S[i].len; }
    st->n_food = n_food < BRAIN_MAX_FOOD ? n_food : BRAIN_MAX_FOOD;
    for (int i = 0; i < st->n_food; i++) {
        st->food_x[i] = (int16_t)(food_cell[i] % M.w);
        st->food_y[i] = (int16_t)(food_cell[i] / M.w);
    }
    fputc('S', b->to);
    fwrite(&ps, sizeof ps, 1, b->to);
    fwrite(snap, 1, (size_t)(M.w * M.h), b->to);
    fflush(b->to);
}

static int dist2(int a, int b) {
    int dx = a % M.w - b % M.w, dy = a / M.w - b / M.w;
    return dx * dx + dy * dy;
}

/* One game; seat_bot[i] = index into bots/stats. Returns steps played. */
static int play(botproc_t *bots, stats_t *st, const int *seat_bot, FILE *log) {
    int n = M.n_snakes, cells = M.w * M.h;
    memcpy(map_, M.base, (size_t)cells);
    n_food = 0;
    for (int i = 0; i < n; i++) {
        snake_t *s = &S[i];
        memset(s, 0, sizeof *s);
        int c;
        do { c = (1 + rnd(M.h - 2)) * M.w + 1 + rnd(M.w - 2); } while (map_[c] != FREE_V);
        s->head = 0; s->body[0] = (uint16_t)c; s->len = 1; s->target = START_LEN;
        s->alive = 1; s->fatal_step = -1;
        s->hv = 3 + 2 * i; s->bv = 4 + 2 * i;
        map_[c] = (uint8_t)s->hv;
    }
    for (int i = 0; i < n; i++) send_init(&bots[seat_bot[i]], i);

    int step = 0, alone_since = -1;
    static uint8_t snap[MAXC];
    while (step < MAX_STEPS) {
        int ids[MAXS], na = 0;
        for (int i = 0; i < n; i++) if (S[i].alive) ids[na++] = i;
        if (na == 0) break;
        place_food();
        /* get_batch_order: pop from the end, first batch with nobody within distance 2 */
        int batch_of[MAXS], nb = 0, bsize[MAXS] = {0}, bm[MAXS][MAXS];
        for (int k = na - 1; k >= 0; k--) {
            int id = ids[k], placed = -1;
            for (int b = 0; b < nb && placed < 0; b++) {
                int clash = 0;
                for (int j = 0; j < bsize[b]; j++)
                    if (dist2(cell_at(&S[id], 0), cell_at(&S[bm[b][j]], 0)) <= 4) clash = 1;
                if (!clash) placed = b;
            }
            if (placed < 0) placed = nb++;
            bm[placed][bsize[placed]++] = id;
            batch_of[id] = placed;
        }
        (void)batch_of;
        int order[MAXS];
        for (int b = 0; b < nb; b++) order[b] = b;
        for (int b = nb - 1; b > 0; b--) { int j = rnd(b + 1), t = order[b]; order[b] = order[j]; order[j] = t; }
        for (int oi = 0; oi < nb; oi++) {
            int b = order[oi];
            memcpy(snap, map_, (size_t)cells);
            for (int j = 0; j < bsize[b]; j++) send_step(&bots[seat_bot[bm[b][j]]], snap);
            pipe_reply_t r[MAXS];
            for (int j = 0; j < bsize[b]; j++) {
                botproc_t *bp = &bots[seat_bot[bm[b][j]]];
                if (fread(&r[j], sizeof r[j], 1, bp->from) != 1) { fprintf(stderr, "bot %s died\n", bp->path); exit(1); }
                stats_t *ss = &st[seat_bot[bm[b][j]]];
                ss->cpu_ns += r[j].cpu_ns;
                ss->decisions += 1;
                if ((long)r[j].cpu_ns > ss->max_cpu_ns) ss->max_cpu_ns = r[j].cpu_ns;
            }
            for (int j = 0; j < bsize[b]; j++) apply_move(bm[b][j], r[j].ok, r[j].dx, r[j].dy, step);
        }
        step++;
        /* snake_loop_control: max_no_food_steps defaults to W*H/2 — stop when
         * no alive snake has eaten for that long (min over alive snakes) */
        {
            int since = 1 << 30, any = 0;
            for (int i = 0; i < n; i++)
                if (S[i].alive) { any = 1; int d = S[i].move_count - S[i].last_ate; if (d < since) since = d; }
            if (any && since > (M.w * M.h) / 2) break;
        }
        if (n > 1) {
            int alive_n = 0, last = -1, longest = 0;
            for (int i = 0; i < n; i++) { if (S[i].alive) { alive_n++; last = i; } if (S[i].len > longest) longest = S[i].len; }
            if (alive_n == 1 && S[last].len >= longest) {
                if (alone_since < 0) alone_since = step;
                if (step - alone_since >= LAST_STANDING_BUFFER) break;
            }
        }
    }

    /* arena scoring: survival rank (death order), final length, fraction of leader */
    int rank[MAXS];
    for (int i = 0; i < n; i++) {
        int fi = S[i].alive ? step + 1 : S[i].fatal_step, r = 1;
        for (int j = 0; j < n; j++) {
            int fj = S[j].alive ? step + 1 : S[j].fatal_step;
            if (fj > fi || (fj == fi && j < i)) r++;
        }
        rank[i] = r;
    }
    int best_len = 0;
    for (int i = 0; i < n; i++) if (S[i].len > best_len) best_len = S[i].len;
    for (int i = 0; i < n; i++) {
        stats_t *s = &st[seat_bot[i]];
        double q = n > 1 ? (1.0 / rank[i] + (double)S[i].len / best_len) / 3.0 : 0.0;
        s->games += 1; s->surv_rank += rank[i]; s->length += S[i].len; s->quality += q;
        s->wins += rank[i] == 1;
        if (log) fprintf(log, "%s\t%d\t%d\t%d\t%.4f\t%d\n", s->name, rank[i], S[i].len, step, q, S[i].alive);
    }
    return step;
}

int main(int argc, char **argv) {
    const char *mode = "m4", *out = NULL, *maps = "tools/maps";
    int games = 100;
    uint64_t seed = (uint64_t)time(NULL);
    int opt;
    while ((opt = getopt(argc, argv, "m:n:s:o:d:")) != -1) {
        switch (opt) {
        case 'm': mode = optarg; break;
        case 'n': games = atoi(optarg); break;
        case 's': seed = strtoull(optarg, NULL, 10); break;
        case 'o': out = optarg; break;
        case 'd': maps = optarg; break;
        default: return 2;
        }
    }
    int nspec = argc - optind;
    if (nspec < 1) { fprintf(stderr, "usage: fastsim -m mode -n games bot[@params]...\n"); return 2; }
    signal(SIGPIPE, SIG_IGN);
    rng_state ^= seed * 0x9E3779B97F4A7C15ull;
    set_mode(mode, maps);

    /* one process per seat; seats are filled with specs round-robin */
    int seats = M.n_snakes;
    botproc_t bots[MAXS];
    stats_t st[MAXS];
    int spec_of_proc[MAXS];
    memset(st, 0, sizeof st);
    for (int i = 0; i < seats; i++) {
        int sp = i % nspec;
        char *spec = strdup(argv[optind + sp]);
        char *at = strchr(spec, '@');
        if (at) *at = 0;
        bots[i].path = spec;
        bots[i].params = at ? at + 1 : NULL;
        spec_of_proc[i] = sp;
        spawn(&bots[i]);
        st[i].name = argv[optind + sp];
    }
    FILE *log = out ? fopen(out, "w") : NULL;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    long total_steps = 0;
    for (int g = 0; g < games; g++) {
        int seat_bot[MAXS];
        for (int i = 0; i < seats; i++) seat_bot[i] = i;
        for (int i = seats - 1; i > 0; i--) { int j = rnd(i + 1), t = seat_bot[i]; seat_bot[i] = seat_bot[j]; seat_bot[j] = t; }
        total_steps += play(bots, st, seat_bot, log);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double secs = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;

    /* aggregate per spec (several processes may run the same spec) */
    printf("%-40s %6s %9s %7s %8s %6s %8s %8s\n", "bot", "games", "surv_rank", "length", "quality", "wins", "cpu_us", "max_us");
    for (int sp = 0; sp < nspec; sp++) {
        stats_t a = {0};
        for (int i = 0; i < seats; i++) {
            if (spec_of_proc[i] != sp) continue;
            a.games += st[i].games; a.surv_rank += st[i].surv_rank; a.length += st[i].length;
            a.quality += st[i].quality; a.wins += st[i].wins; a.cpu_ns += st[i].cpu_ns;
            a.decisions += st[i].decisions;
            if (st[i].max_cpu_ns > a.max_cpu_ns) a.max_cpu_ns = st[i].max_cpu_ns;
        }
        if (a.games == 0) continue;
        printf("%-40s %6.0f %9.2f %7.1f %8.3f %6.2f %8.1f %8.1f\n", argv[optind + sp], a.games,
               a.surv_rank / a.games, a.length / a.games, a.quality / a.games, a.wins / a.games,
               a.cpu_ns / a.decisions / 1e3, a.max_cpu_ns / 1e3);
    }
    printf("%d games, %ld steps in %.1fs (%.0f steps/s)\n", games, total_steps, secs, total_steps / secs);
    for (int i = 0; i < seats; i++) { fputc('Q', bots[i].to); fflush(bots[i].to); fclose(bots[i].to); waitpid(bots[i].pid, NULL, 0); }
    if (log) fclose(log);
    return 0;
}
