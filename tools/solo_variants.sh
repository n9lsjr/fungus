#!/usr/bin/env bash
# Compare solo param variants: tools/solo_variants.sh <games> <seed> <name=paramfile>...
# Prints mean length ± SE, games shorter than 50 (and how many of those starved out), CPU.
set -euo pipefail
cd "$(dirname "$0")/.."
games=$1; seed=$2; shift 2
tmp=$(mktemp -d)
for spec in "$@"; do
  name=${spec%%=*}; params=${spec#*=}
  for m in s1 s2; do
    nice -n 19 build/fastsim -m "$m" -n "$games" -s "$seed" -o "$tmp/v.tsv" "build/pipe-fungus@$params" > "$tmp/v.out"
    awk -F'\t' -v v="$name" -v m="$m" '{n++; s+=$3; ss+=$3*$3; if($3<50)k++; if($3<50 && $6==1)st++}
      END{mu=s/n; printf "%s %s len %6.1f ± %.1f  short(<50) %d/%d (starved %d)", v, m, mu, sqrt((ss/n-mu*mu)/n), k, n, st}' "$tmp/v.tsv"
    sed -n 2p "$tmp/v.out" | awk '{print "  cpu", $7, "max", $8}'
  done
done
