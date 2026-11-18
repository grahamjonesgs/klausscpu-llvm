#!/bin/bash
# Summarise results/*.out (in the work directory) from run-board.sh.
. "$(dirname "$0")/env.sh"
cd "$WORK/results" || exit 1
for f in *.out; do
    t=$(basename "$f" .out)
    passed=$(grep -o 'PASSED [0-9]* tests' "$f")
    bad=$(grep -c '^bad ' "$f")
    printf '%-18s %s, %d failed checks, %s\n' "$t" "${passed:-not passed}" "$bad" "$(grep '^exit' "$f")"
done

grep -h -E '^bad | != ' *.out | head -300
