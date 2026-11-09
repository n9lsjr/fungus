/* Binary stdin/stdout protocol between tools/fastsim and a bot process.
 * Same-machine, same-compiler only: structs are sent as raw bytes.
 *   sim -> bot: 'I' pipe_init_t, then width*height bytes base map
 *   sim -> bot: 'S' pipe_step_t, then width*height bytes map
 *   bot -> sim: pipe_reply_t after each 'S'
 *   sim -> bot: 'Q' to quit */
#ifndef CSNAKE_PIPE_PROTO_H
#define CSNAKE_PIPE_PROTO_H

#include "brain.h"

typedef struct {
    brain_init_t init; /* base_map pointer is ignored on the wire */
} pipe_init_t;

typedef struct {
    brain_step_t step; /* map pointer is ignored on the wire */
} pipe_step_t;

typedef struct {
    int8_t ok, dx, dy, pad;
    uint32_t cpu_ns; /* thread CPU spent in brain_decide */
} pipe_reply_t;

#endif
