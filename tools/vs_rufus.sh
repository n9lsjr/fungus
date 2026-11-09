#!/usr/bin/env bash
# Run N matches: our bot(s) over gRPC vs in-proc Rufus copies, scored like the arena.
# usage: tools/vs_rufus.sh <bot-binary | image:NAME> <mode m4|m6|s1|s2> <n> <outdir> [n_bots=1]
#   image:NAME runs the arena image with the runner's container flags
#   (read-only, cap_drop ALL, user 1000:1000, 512 MB, 1 CPU) — a dress rehearsal.
set -euo pipefail
cd "$(dirname "$0")/.."
bin=$1; mode=$2; n=$3; out=$4; nb=${5:-1}
case $mode in m4) total=4 ;; m6) total=6 ;; s1|s2) total=1 ;; esac
targets=(); cids=()
for i in $(seq 1 "$nb"); do
  port=$((50060 + i))
  if [[ $bin == image:* ]]; then
    cids+=("$(docker run -d --rm -p 127.0.0.1:$port:50051 --read-only --cap-drop ALL --user 1000:1000 \
        --memory 512m --memory-swap 512m --cpus 1 --pids-limit 64 "${bin#image:}")")
  else
    cids+=("$(docker run -d --rm --network host --memory 512m --cpus 1 -v "$PWD/build:/b:ro" \
        csnake-dev /b/$(basename "$bin") 127.0.0.1:$port)")
  fi
  targets+=("127.0.0.1:$port")
done
trap 'docker kill "${cids[@]}" >/dev/null 2>&1 || true' EXIT
sleep 1
cpu0=(); for c in "${cids[@]}"; do cpu0+=("$(docker exec "$c" cat /sys/fs/cgroup/cpu.stat | awk '/usage_usec/{print $2}')"); done
tools/run_matches.sh "$mode" "$n" "$out" --ext-targets "${targets[@]}" --ext-conn-timeout 10 --ext-init-timeout 2 --snake-count $((total - nb))

# CPU per step the way the arena measures it: whole-container cgroup CPU / steps alive.
python3 - "$out/scores.json" "${targets[@]}" <<PY
import json, sys
rows = json.load(open(sys.argv[1]))
targets = sys.argv[2:]
cpu = dict(zip(targets, [int(x) for x in "$(for i in "${!cids[@]}"; do c=${cids[$i]}; u=$(docker exec "$c" cat /sys/fs/cgroup/cpu.stat | awk '/usage_usec/{print $2}'); echo -n "$((u - ${cpu0[$i]})) "; done)".split()]))
for t in targets:
    steps = sum(r["steps_alive"] for r in rows if r["tag"] == t)
    mine = [r for r in rows if r["tag"] == t]
    q = [r["quality"] if r["quality"] is not None else 0.0 for r in mine]
    ln = sum(r["final_length"] for r in mine) / max(len(mine), 1)
    ms = cpu[t] / 1000 / max(steps, 1)
    cf = 1 - 0.4 * min(ms / 10, 1)
    print(f"{t}: cpu {ms:.3f} ms/step over {steps} steps, cpu_factor {cf:.3f}, length {ln:.1f}, quality {sum(q)/len(q):.3f}, score {cf*sum(q)/len(q):.3f}")
PY
