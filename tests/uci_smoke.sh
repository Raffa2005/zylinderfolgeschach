#!/usr/bin/env bash
set -euo pipefail

engine=${1:?missing engine path}

coproc ZFS_ENGINE { "${engine}"; }
engine_pid=${ZFS_ENGINE_PID}
exec 3>&"${ZFS_ENGINE[1]}"
exec 4<&"${ZFS_ENGINE[0]}"

read_until() {
    local expected=$1
    local line
    while IFS= read -r -t 10 line <&4; do
        if [[ ${line} == ${expected} ]]; then
            return 0
        fi
    done
    echo "UCI smoke test timed out waiting for: ${expected}" >&2
    return 1
}

printf 'uci\n' >&3
read_until 'uciok'
printf 'isready\n' >&3
read_until 'readyok'

printf 'position startpos\ngo depth 2\n' >&3
read_until 'info depth 2 '*
read_until 'bestmove ????*'

printf 'position startpos\ngo searchmoves e2e4 depth 2\n' >&3
read_until 'info depth 2 '*
read_until 'bestmove e2e4'

printf '%s\n' \
    'position fen 4k3/1R6/8/p7/8/8/8/4K3 w - a6 0 2' \
    'go searchmoves b7a7 depth 1' >&3
read_until 'info depth 1 '*
read_until 'bestmove b7a7'

printf 'go mate 2147483647\n' >&3
read_until 'info string out-of-range go value for mate'
printf 'go wtime 1 winc 9223372036854775807\n' >&3
read_until 'info string out-of-range go value for winc'
printf 'go movetime 9223372036854775807\n' >&3
read_until 'info string out-of-range go value for movetime'

printf '%s\n' \
    'position zfsfen 4k3/8/8/8/8/8/8/4K1N1 w - - 0 1 - moves g1f3 e8e7 f3g1 e7e8 g1f3 e8e7 f3g1 e7e8' \
    'go depth 3' >&3
read_until 'bestmove 0000'

printf 'position startpos\ngo infinite\nstop\n' >&3
read_until 'bestmove ????*'

printf 'setoption name Ponder value true\n' >&3
printf 'position startpos\ngo ponder movetime 50 depth 2\n' >&3
read_until 'info depth 2 '*
if IFS= read -r -t 0.1 premature <&4; then
    if [[ ${premature} == bestmove* ]]; then
        echo "UCI engine emitted bestmove before ponderhit: ${premature}" >&2
        exit 1
    fi
fi
printf 'ponderhit\n' >&3
read_until 'bestmove ????*'

printf 'setoption name Move Overhead value 5000\n' >&3
movetime_started=$(date +%s%N)
printf 'position startpos\ngo movetime 100\n' >&3
read_until 'bestmove ????*'
movetime_elapsed_ms=$(( ($(date +%s%N) - movetime_started) / 1000000 ))
if (( movetime_elapsed_ms < 30 )); then
    echo "movetime was incorrectly consumed as overhead: ${movetime_elapsed_ms} ms" >&2
    exit 1
fi

printf 'quit\n' >&3
exec 3>&-
wait "${engine_pid}"
