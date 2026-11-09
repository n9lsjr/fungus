"""Rufus (snake_sim SurvivorSnake + FoodSeeker) as a fastsim pipe bot.

Speaks src/shell/pipe_proto.h on stdin/stdout. Runs inside the csnake-sim
container via tools/rufus_pipe.sh. A fresh snake is built per game with the
same factory and default config the arena's Rufus image uses.
"""
import ctypes as C
import os
import sys
import time

# Keep the protocol channel clean: libraries print banners on import.
_proto_out = os.fdopen(os.dup(1), "wb", buffering=0)
os.dup2(2, 1)
os.environ.setdefault("PYGAME_HIDE_SUPPORT_PROMPT", "1")

import numpy as np  # noqa: E402
from snake_sim.environment.snake_factory import SnakeFactory, default_config  # noqa: E402
from snake_sim.environment.types import Coord, EnvMetaData, EnvStepData, SnakeConfig  # noqa: E402

MAXS, MAXF = 16, 64


class BrainInit(C.Structure):
    _fields_ = [("width", C.c_int), ("height", C.c_int), ("free_value", C.c_int),
                ("blocked_value", C.c_int), ("food_value", C.c_int), ("self_id", C.c_int),
                ("start_length", C.c_int), ("n_snakes", C.c_int),
                ("snake_ids", C.c_int * MAXS), ("head_values", C.c_int * MAXS),
                ("body_values", C.c_int * MAXS), ("start_x", C.c_int * MAXS),
                ("start_y", C.c_int * MAXS), ("base_map", C.c_void_p)]


class BrainStep(C.Structure):
    _fields_ = [("map", C.c_void_p), ("n_snakes", C.c_int), ("alive", C.c_uint8 * MAXS),
                ("lengths", C.c_int * MAXS), ("n_food", C.c_int),
                ("food_x", C.c_int16 * MAXF), ("food_y", C.c_int16 * MAXF)]


class Reply(C.Structure):
    _fields_ = [("ok", C.c_int8), ("dx", C.c_int8), ("dy", C.c_int8), ("pad", C.c_int8),
                ("cpu_ns", C.c_uint32)]


def read_exact(f, n):
    buf = f.read(n)
    if buf is None or len(buf) != n:
        sys.exit(0)
    return buf


def main():
    inp = sys.stdin.buffer
    snake = None
    init = None
    while True:
        tag = inp.read(1)
        if not tag or tag == b"Q":
            return
        if tag == b"I":
            init = BrainInit.from_buffer_copy(read_exact(inp, C.sizeof(BrainInit)))
            cells = init.width * init.height
            base = np.frombuffer(read_exact(inp, cells), dtype=np.uint8).reshape(init.height, init.width)
            n = init.n_snakes
            ids = [init.snake_ids[i] for i in range(n)]
            me = ids.index(init.self_id)
            meta = EnvMetaData(
                height=init.height, width=init.width, free_value=init.free_value,
                blocked_value=init.blocked_value, food_value=init.food_value,
                snake_tags={sid: f"s{sid}" for sid in ids},
                snake_values={ids[i]: {"head_value": init.head_values[i], "body_value": init.body_values[i]}
                              for i in range(n)},
                start_positions={ids[i]: Coord(init.start_x[i], init.start_y[i]) for i in range(n)},
                base_map=base.copy(), base_map_dtype="uint8")
            snake = SnakeFactory().create_snake(SnakeConfig.from_dict(default_config.snake_config))
            snake.set_id(init.self_id)
            snake.set_start_length(init.start_length)
            snake.set_start_position(Coord(init.start_x[me], init.start_y[me]))
            snake.set_init_data(meta)
        elif tag == b"S":
            st = BrainStep.from_buffer_copy(read_exact(inp, C.sizeof(BrainStep)))
            cells = init.width * init.height
            m = np.frombuffer(read_exact(inp, cells), dtype=np.uint8).reshape(init.height, init.width)
            data = EnvStepData(
                map=m,
                snakes={init.snake_ids[i]: {"is_alive": bool(st.alive[i]), "length": st.lengths[i]}
                        for i in range(st.n_snakes)},
                food_locations=[Coord(st.food_x[i], st.food_y[i]) for i in range(st.n_food)])
            t0 = time.thread_time_ns()
            d = snake.update(data)
            r = Reply(cpu_ns=min(time.thread_time_ns() - t0, 2**32 - 1))
            if d is not None:
                r.ok, r.dx, r.dy = 1, d.x, d.y
            _proto_out.write(bytes(r))
        else:
            sys.exit(1)


if __name__ == "__main__":
    main()
