/* Bot process for tools/fastsim: speaks pipe_proto.h on stdin/stdout.
 * Optional argv[1]: params file passed to brain_set_params() if the brain
 * defines it (weak symbol), so the tuner can vary weights per process. */
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "pipe_proto.h"

void brain_set_params(const char *path) __attribute__((weak));

static int read_all(void *buf, size_t n) {
    return fread(buf, 1, n, stdin) == n;
}

static uint64_t thread_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

int main(int argc, char **argv) {
    static uint8_t base_map[1024], map[1024];
    pipe_init_t pi;
    pipe_step_t ps;
    int cells = 0;
    if (argc > 1 && brain_set_params) brain_set_params(argv[1]);
    for (;;) {
        int tag = fgetc(stdin);
        if (tag == EOF || tag == 'Q') return 0;
        if (tag == 'I') {
            if (!read_all(&pi, sizeof pi)) return 1;
            cells = pi.init.width * pi.init.height;
            if (cells <= 0 || cells > 1024 || !read_all(base_map, (size_t)cells)) return 1;
            pi.init.base_map = base_map;
            brain_init(&pi.init);
        } else if (tag == 'S') {
            if (!read_all(&ps, sizeof ps) || !read_all(map, (size_t)cells)) return 1;
            ps.step.map = map;
            pipe_reply_t r = {0};
            int dx = 0, dy = 0;
            uint64_t t0 = thread_ns();
            r.ok = brain_decide(&ps.step, &dx, &dy) == 0;
            r.cpu_ns = (uint32_t)(thread_ns() - t0);
            r.dx = (int8_t)dx;
            r.dy = (int8_t)dy;
            fwrite(&r, sizeof r, 1, stdout);
            fflush(stdout);
        } else {
            return 1;
        }
    }
}
