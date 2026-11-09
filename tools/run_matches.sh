#!/usr/bin/env bash
# Run N local matches in one capped container and score them like the arena.
# usage: tools/run_matches.sh <mode> <n> <outdir> [extra snake-sim args...]
#   mode: m4 (4p 20x15 food 7) | m6 (6p 20x20 food 10) | s1 (solo mini_items) | s2 (solo mini_stairs)
# Extra args go straight to snake-sim, e.g. --ext-targets host:port ... --snake-count 3
set -euo pipefail
cd "$(dirname "$0")/.."
mode=$1; n=$2; out=$3; shift 3
# default: all in-proc Rufus unless the caller passes --snake-count
case $mode in
  m4) margs="--grid-width 20 --grid-height 15 --food 7"; count=4 ;;
  m6) margs="--grid-width 20 --grid-height 20 --food 10"; count=6 ;;
  s1) margs="--map mini_items --food 10"; count=1 ;;
  s2) margs="--map mini_stairs --food 10"; count=1 ;;
  *) echo "unknown mode $mode" >&2; exit 1 ;;
esac
mkdir -p "$out"
extra="$*"
endargs="--end-on-last-standing-when-longest --end-on-last-standing-buffer-steps 200"
[ "$count" = 1 ] && endargs=""
[[ "$extra" == *--snake-count* ]] || extra="--snake-count $count $extra"
docker run --rm --network host --memory 2g --memory-swap 2g --cpus 1 \
  -v "$PWD/$out:/runs" -v "$PWD/tools:/tools:ro" -w /tmp csnake-sim:latest bash -c "
  for i in \$(seq 1 $n); do
    snake-sim compute --no-render $margs --start-length 2 $endargs \
      --decision-timeout-ms 0 --record-dir /runs --record-file match_\$i.run_proto \
      --log-level WARNING --log-dir /tmp/logs $extra >/dev/null 2>&1 || echo \"match \$i failed\"
  done
  python /tools/score_runs.py /runs/*.run_proto --json /runs/scores.json 2>/dev/null
"
