# Loaded automatically in every Python process (incl. spawned snake processes)
# when this dir is on PYTHONPATH: wraps Rufus' hot calls with thread CPU timers.
import atexit, os, time, json, collections
_stats = collections.defaultdict(lambda: [0, 0])
def _wrap(cls, name, key):
    f = getattr(cls, name)
    def w(*a, **k):
        t = time.thread_time_ns()
        try:
            return f(*a, **k)
        finally:
            s = _stats[key]; s[0] += 1; s[1] += time.thread_time_ns() - t
    setattr(cls, name, w)
def _patch():
    try:
        from snake_sim.snakes.survivor_snake import SurvivorSnake
        from snake_sim.snakes.strategies.food_strategy import FoodSeeker
    except Exception:
        return
    _wrap(SurvivorSnake, "update", "update_total")
    _wrap(SurvivorSnake, "_get_margin_fracs", "recurse_area_check")
    _wrap(FoodSeeker, "get_wanted_tile", "food_strategy")
    _wrap(FoodSeeker, "_area_check_wrapper", "food_area_check")
def _dump():
    if _stats:
        with open(f"/runs/prof_{os.getpid()}.json", "w") as fh:
            json.dump(dict(_stats), fh)
import builtins
_orig_import = builtins.__import__
_done = [False]
def _imp(name, *a, **k):
    m = _orig_import(name, *a, **k)
    if not _done[0] and name.startswith("snake_sim.snakes"):
        _done[0] = True
        _patch()
    return m
builtins.__import__ = _imp
atexit.register(_dump)
