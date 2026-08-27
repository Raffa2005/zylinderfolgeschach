#!/usr/bin/env bash
set -euo pipefail

runner=$1
engine=$2
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT

runner_dir=$(cd "$(dirname "$runner")" && pwd -P)
runner_name=$(basename "$runner")
runner="$runner_dir/$runner_name"
engine_dir=$(cd "$(dirname "$engine")" && pwd -P)
engine="$engine_dir/$(basename "$engine")"

printf 'startpos\n' > "$scratch/openings.txt"
# A trailing empty PATH component means the current directory. Exercise the
# runner's self-fingerprinting lookup through that standards-compatible form.
(
    cd "$runner_dir"
    PATH=/usr/bin: "$runner_name" \
        --candidate "$engine" \
        --baseline "$engine" \
        --candidate-id smoke \
        --baseline-id smoke \
        --openings "$scratch/openings.txt" \
        --output "$scratch/results.jsonl" \
        --pairs 1 \
        --nodes 1 \
        --hash-mb 1 \
        --max-plies 8 \
        --timeout-ms 5000
)

grep -q '"type":"manifest"' "$scratch/results.jsonl"
grep -q '"type":"pair","pair":0' "$scratch/results.jsonl"
test "$(grep -o '"game":[12]' "$scratch/results.jsonl" | wc -l)" -eq 2
grep -q '"candidate_half_points":2' "$scratch/results.jsonl"

"$runner" \
    --candidate "$engine" \
    --baseline "$engine" \
    --candidate-id smoke \
    --baseline-id smoke \
    --openings "$scratch/openings.txt" \
    --output "$scratch/results.jsonl" \
    --pairs 1 \
    --nodes 1 \
    --hash-mb 1 \
    --max-plies 8 \
    --timeout-ms 5000 \
    --resume

test "$(grep -c '"type":"pair"' "$scratch/results.jsonl")" -eq 1

# Simulate a kill after the first nested game object reached the filesystem.
# The unterminated pair must be discarded as a unit, not retained as an orphan.
manifest=$(sed -n '1p' "$scratch/results.jsonl")
partial_pair=$(sed -n '2s/^\(.*"games":\[[^}]*}\).*/\1/p' \
    "$scratch/results.jsonl")
printf '%s\n%s' "$manifest" "$partial_pair" > "$scratch/partial.jsonl"

"$runner" \
    --candidate "$engine" \
    --baseline "$engine" \
    --candidate-id smoke \
    --baseline-id smoke \
    --openings "$scratch/openings.txt" \
    --output "$scratch/partial.jsonl" \
    --pairs 1 \
    --nodes 1 \
    --hash-mb 1 \
    --max-plies 8 \
    --timeout-ms 5000 \
    --resume

test "$(wc -l < "$scratch/partial.jsonl")" -eq 2
test "$(grep -c '"type":"pair"' "$scratch/partial.jsonl")" -eq 1
test "$(grep -o '"game":[12]' "$scratch/partial.jsonl" | wc -l)" -eq 2

sed '2s/"candidate_color":"white"/"candidate_color":"black"/' \
    "$scratch/results.jsonl" > "$scratch/corrupt.jsonl"
if "$runner" \
    --candidate "$engine" \
    --baseline "$engine" \
    --candidate-id smoke \
    --baseline-id smoke \
    --openings "$scratch/openings.txt" \
    --output "$scratch/corrupt.jsonl" \
    --pairs 1 \
    --nodes 1 \
    --hash-mb 1 \
    --max-plies 8 \
    --timeout-ms 5000 \
    --resume; then
    echo "corrupt pair record was accepted" >&2
    exit 1
fi

sed '1s/"uci_name":"[^"]*"/"uci_name":"corrupt"/' \
    "$scratch/results.jsonl" > "$scratch/corrupt-manifest.jsonl"
if "$runner" \
    --candidate "$engine" \
    --baseline "$engine" \
    --candidate-id smoke \
    --baseline-id smoke \
    --openings "$scratch/openings.txt" \
    --output "$scratch/corrupt-manifest.jsonl" \
    --pairs 1 \
    --nodes 1 \
    --hash-mb 1 \
    --max-plies 8 \
    --timeout-ms 5000 \
    --resume; then
    echo "corrupt manifest was accepted" >&2
    exit 1
fi

{
    sed -n '1p' "$scratch/results.jsonl"
    printf '%s\n' 'garbage'
    sed -n '2p' "$scratch/results.jsonl"
} > "$scratch/garbage-middle.jsonl"
if "$runner" \
    --candidate "$engine" \
    --baseline "$engine" \
    --candidate-id smoke \
    --baseline-id smoke \
    --openings "$scratch/openings.txt" \
    --output "$scratch/garbage-middle.jsonl" \
    --pairs 1 \
    --nodes 1 \
    --hash-mb 1 \
    --max-plies 8 \
    --timeout-ms 5000 \
    --resume; then
    echo "unknown middle record was accepted" >&2
    exit 1
fi

if "$runner" \
    --candidate "$engine" \
    --baseline "$engine" \
    --openings "$scratch/openings.txt" \
    --output "$scratch/invalid-config.jsonl" \
    --pairs 1 \
    --nodes 1 \
    --elo0 5 \
    --elo1 0; then
    echo "invalid statistics configuration was accepted" >&2
    exit 1
fi
test ! -e "$scratch/invalid-config.jsonl"

printf '%s\n' \
    '#!/usr/bin/env bash' \
    'while IFS= read -r line; do' \
    '  case "$line" in' \
    '    uci) printf "%s\n" "id name TinyHash" "option name Hash type spin default 1 min 1 max 1" uciok ;;' \
    '    isready) printf "%s\n" readyok ;;' \
    '    quit) exit 0 ;;' \
    '  esac' \
    'done' > "$scratch/tiny-hash-engine"
chmod +x "$scratch/tiny-hash-engine"
if "$runner" \
    --candidate "$scratch/tiny-hash-engine" \
    --baseline "$engine" \
    --openings "$scratch/openings.txt" \
    --output "$scratch/out-of-range-hash.jsonl" \
    --pairs 1 \
    --nodes 1 \
    --hash-mb 2; then
    echo "out-of-range UCI Hash request was accepted" >&2
    exit 1
fi
test ! -e "$scratch/out-of-range-hash.jsonl"

printf '%s\n' \
    '#!/usr/bin/env bash' \
    'while IFS= read -r line; do' \
    '  case "$line" in' \
    '    uci) printf "%s\n" "id name IllegalFake" uciok ;;' \
    '    isready) printf "%s\n" readyok ;;' \
    '    setoption\ *) exit 9 ;;' \
    '    go\ *) printf "%s\n" "bestmove a1a1" ;;' \
    '    quit) exit 0 ;;' \
    '  esac' \
    'done' > "$scratch/illegal-engine"
chmod +x "$scratch/illegal-engine"

"$runner" \
    --candidate "$scratch/illegal-engine" \
    --baseline "$engine" \
    --openings "$scratch/openings.txt" \
    --output "$scratch/illegal.jsonl" \
    --pairs 1 \
    --nodes 1 \
    --hash-mb 1 \
    --max-plies 8 \
    --timeout-ms 5000

grep -q '"candidate_half_points":0' "$scratch/illegal.jsonl"
test "$(grep -o 'candidate-illegal-move: a1a1' "$scratch/illegal.jsonl" | wc -l)" -eq 2

printf '%s\n' \
    '#!/usr/bin/env bash' \
    'ready=0' \
    'while IFS= read -r line; do' \
    '  case "$line" in' \
    '    uci) printf "%s\n" "id name HungFake" uciok ;;' \
    '    isready)' \
    '      ready=$((ready + 1))' \
    '      printf "%s\n" readyok' \
    '      if test "$ready" -eq 2; then sleep 60; exit 0; fi ;;' \
    '    quit) exit 0 ;;' \
    '  esac' \
    'done' > "$scratch/hung-engine"
chmod +x "$scratch/hung-engine"

# This engine completes setup, then stops responding. The runner must kill the
# whole engine process group on the response deadline and record both forfeits.
"$runner" \
    --candidate "$scratch/hung-engine" \
    --baseline "$engine" \
    --openings "$scratch/openings.txt" \
    --output "$scratch/hung.jsonl" \
    --pairs 1 \
    --nodes 1 \
    --hash-mb 1 \
    --max-plies 8 \
    --timeout-ms 200

grep -q '"candidate_half_points":0' "$scratch/hung.jsonl"
test "$(grep -o 'response timeout' "$scratch/hung.jsonl" | wc -l)" -eq 2
