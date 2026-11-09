"""Score recorded snake_sim runs the way gridsnake.com does (multi modes).

Runs inside the csnake-sim container:
    python /tools/score_runs.py /runs/*.run_proto [--json out.json]

Per match: survival_rank (lower better), trapping_count (traps that killed),
final_length; quality = mean fraction-of-leader over the three categories
(mirrors snake_arena runner/match_results.py and sa_common/scoring.py).
Snakes are grouped by tag prefix (tag up to the last '_' for in-proc snakes,
full tag for gRPC targets) so several copies of one bot aggregate together.
"""
import argparse
import json
import sys
from collections import defaultdict
from pathlib import Path

from snake_sim.analyze.scripts.run_analyzer import analyze


def trapping_counts(analysis_dict, snake_ids):
    counts = {sid: 0 for sid in snake_ids}
    last_visible = {}
    for sid_key, phases in (analysis_dict.get("tail_visible_phases") or {}).items():
        latest = -1
        for ph in phases or ():
            if ph.get("tail_visible") and ph.get("end_step_idx") is not None:
                latest = max(latest, int(ph["end_step_idx"]))
        last_visible[int(sid_key)] = latest
    for step_key, traps in (analysis_dict.get("traps_mapping") or {}).items():
        step = int(step_key)
        for trap in traps:
            trapped = [int(t) for t in trap.get("trapped_ids", ())]
            if not trapped:
                continue
            if not all(last_visible.get(t, float("inf")) <= step for t in trapped):
                continue
            for sid in trap.get("trapping_ids", ()):
                if int(sid) in counts:
                    counts[int(sid)] += 1
    return counts


def frac_of_leader(values, direction):
    if direction == "higher":
        lead = max(values.values())
        return {k: 0.0 if lead <= 0 else max(0.0, min(1.0, v / lead)) for k, v in values.items()}
    lead = min(values.values())
    return {k: 0.0 if lead <= 0 else max(0.0, min(1.0, lead / v)) for k, v in values.items()}


def group_of(tag):
    if tag.startswith("SurvivorSnake_") or tag.startswith("default_survivor"):
        return "rufus"
    return tag


def score_match(path):
    a = analyze(Path(path))
    d = a.to_dict()
    ids = a.snake_ids
    final = a.final_step_idx
    order = sorted(ids, key=lambda s: a.fatal_steps.get(s, final + 1), reverse=True)
    rank = {s: i + 1 for i, s in enumerate(order)}
    traps = trapping_counts(d, ids)
    raw = {s: {"survival_rank": rank[s], "trapping_count": traps[s],
               "final_length": a.final_lengths.get(s, 0)} for s in ids}
    q = {}
    if len(ids) > 1:
        cats = [("survival_rank", "lower"), ("trapping_count", "higher"), ("final_length", "higher")]
        fr = {c: frac_of_leader({s: raw[s][c] for s in ids}, dirn) for c, dirn in cats}
        for s in ids:
            q[s] = sum(fr[c][s] for c, _ in cats) / len(cats)
    tags = a.env_meta_data.snake_tags
    return [{"match": Path(path).name, "snake": s, "tag": tags.get(s, str(s)),
             "group": group_of(tags.get(s, str(s))), "steps": final,
             "steps_alive": a.fatal_steps.get(s, final),
             **raw[s], "quality": q.get(s)} for s in ids]


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("runs", nargs="+")
    ap.add_argument("--json", type=Path)
    args = ap.parse_args(argv)
    rows = []
    for p in args.runs:
        try:
            rows.extend(score_match(p))
        except Exception as e:  # a broken run shouldn't hide the others
            print(f"skip {p}: {e}", file=sys.stderr)
    agg = defaultdict(lambda: defaultdict(float))
    for r in rows:
        g = agg[r["group"]]
        g["n"] += 1
        for k in ("survival_rank", "trapping_count", "final_length", "steps"):
            g[k] += r[k]
        if r["quality"] is not None:
            g["quality"] += r["quality"]
    print(f"{'group':28} {'n':>4} {'surv_rank':>9} {'traps':>6} {'length':>7} {'quality':>8}")
    for name, g in sorted(agg.items(), key=lambda kv: -kv[1]["quality"] / kv[1]["n"]):
        n = g["n"]
        print(f"{name:28} {int(n):4d} {g['survival_rank']/n:9.2f} {g['trapping_count']/n:6.2f} "
              f"{g['final_length']/n:7.1f} {g['quality']/n:8.3f}")
    if args.json:
        args.json.write_text(json.dumps(rows, indent=1))


if __name__ == "__main__":
    main(sys.argv[1:])
