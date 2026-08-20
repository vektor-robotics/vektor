#!/usr/bin/env bash
# Capture repeatable timing samples for a non-production VEKTOR command.
set -euo pipefail

usage() {
  echo "usage: $0 <output.csv> <positive-iterations> -- <command> [args...]" >&2
  exit 2
}

[[ $# -ge 4 ]] || usage
output=$1
iterations=$2
shift 2
[[ "$iterations" =~ ^[1-9][0-9]*$ && "$1" == "--" ]] || usage
shift
[[ $# -gt 0 ]] || usage

mkdir -p "$(dirname "$output")"
printf 'iteration,elapsed_ms,exit_code\n' > "$output"

for ((iteration = 1; iteration <= iterations; ++iteration)); do
  start_ns=$(date +%s%N)
  set +e
  "$@"
  exit_code=$?
  set -e
  elapsed_ms=$((( $(date +%s%N) - start_ns ) / 1000000))
  printf '%s,%s,%s\n' "$iteration" "$elapsed_ms" "$exit_code" >> "$output"
  if ((exit_code != 0)); then
    echo "baseline command failed on iteration $iteration; samples retained at $output" >&2
    exit "$exit_code"
  fi
done

echo "VEKTOR performance baseline completed: $iterations samples in $output"
