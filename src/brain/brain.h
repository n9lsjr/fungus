/* Contract between the gRPC shell (src/shell) and a decision brain.
 * Plain C so a brain can be linked into the C++ shell, a local C simulator
 * or tests. One brain instance per process; all memory is static. */
#ifndef CSNAKE_BRAIN_H
#define CSNAKE_BRAIN_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BRAIN_MAX_SNAKES 16
#define BRAIN_MAX_FOOD 64

typedef struct {
    int width, height;
    int free_value, blocked_value, food_value;
    int self_id;
    int start_length;
    int n_snakes;
    int snake_ids[BRAIN_MAX_SNAKES];
    int head_values[BRAIN_MAX_SNAKES];
    int body_values[BRAIN_MAX_SNAKES];
    int start_x[BRAIN_MAX_SNAKES], start_y[BRAIN_MAX_SNAKES];
    const uint8_t *base_map; /* width*height row-major, NULL if none */
} brain_init_t;

typedef struct {
    const uint8_t *map; /* width*height row-major */
    int n_snakes;       /* order matches brain_init_t.snake_ids */
    uint8_t alive[BRAIN_MAX_SNAKES];
    int lengths[BRAIN_MAX_SNAKES];
    int n_food;
    int16_t food_x[BRAIN_MAX_FOOD], food_y[BRAIN_MAX_FOOD];
} brain_step_t;

void brain_init(const brain_init_t *init);

/* Returns 0 and writes a unit direction (dx, dy), or -1 to give up. */
int brain_decide(const brain_step_t *step, int *dx, int *dy);

#ifdef __cplusplus
}
#endif

#endif
