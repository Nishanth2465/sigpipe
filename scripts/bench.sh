#!/usr/bin/env bash
# Run the pipeline with 1, 2, 4, 8 worker threads and record throughput.
# Each configuration runs 3 times; the best time is kept (least noise).
set -euo pipefail
cd "$(dirname "$0")/.."

SAMPLES=${SAMPLES:-8000000}
TAPS=${TAPS:-129}
mkdir -p results
OUT=results/bench.csv
echo "threads,time_s,msamples_per_s" > "$OUT"

echo "CPU cores available: $(nproc)"
for t in 1 2 4 8; do
    best=""
    for _ in 1 2 3; do
        time=$(./sigpipe -t "$t" -n "$SAMPLES" -k "$TAPS" | sed -E 's/.*time=([0-9.]+)s.*/\1/')
        if [[ -z "$best" ]] || awk "BEGIN{exit !($time < $best)}"; then best=$time; fi
    done
    tput=$(awk "BEGIN{printf \"%.2f\", $SAMPLES / $best / 1e6}")
    echo "threads=$t  best_time=${best}s  throughput=${tput} Msamples/s"
    echo "$t,$best,$tput" >> "$OUT"
done
echo "saved $OUT"
