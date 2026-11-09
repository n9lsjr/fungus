#!/usr/bin/env bash
# fastsim bot wrapper: real Rufus in a capped csnake-sim container.
here="$(cd "$(dirname "$0")" && pwd)"
exec docker run -i --rm --memory 1g --cpus 1 -e PYGAME_HIDE_SUPPORT_PROMPT=1 \
  -v "$here:/tools:ro" -w /tmp csnake-sim:latest python -u /tools/rufus_pipe.py 2>/dev/null
