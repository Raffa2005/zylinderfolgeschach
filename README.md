# Kugelfisch

[![CI](https://github.com/Raffa2005/zylinderfolgeschach/actions/workflows/ci.yml/badge.svg)](https://github.com/Raffa2005/zylinderfolgeschach/actions/workflows/ci.yml)

Kugelfisch is a dependency-free C++20 implementation of Zylinderfolgeschach
(ZFS): chess on a file-wrapped cylinder with a mandatory move to the previous
move's origin when such a move is otherwise legal. It includes the exact rules
core, a single-worker UCI engine, and a browser playing surface.

The normative variant specification is [RULES.md](RULES.md). Engineering and
semantic choices are recorded in [DECISIONS.md](DECISIONS.md). The implemented
baseline and remaining roadmap are summarized in [ENGINE_PLAN.md](ENGINE_PLAN.md).

## Build and test

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

For a binary used only on the machine that built it, enable the host CPU's
instruction set explicitly:

```sh
cmake -S . -B build-native -DCMAKE_BUILD_TYPE=Release -DZFS_NATIVE=ON
cmake --build build-native -j
```

The normal build remains portable. On the development CPU the native build was
about five percent faster, chiefly because material counting used hardware
`popcnt` rather than compatibility helpers.

The full test build requires Node.js because it executes the committed browser
WASM and verifies that the production bundle embeds that exact payload.

For an AddressSanitizer and UndefinedBehaviorSanitizer run:

```sh
cmake -S . -B build-sanitize -DCMAKE_BUILD_TYPE=Debug \
  -DZFS_ENABLE_SANITIZERS=ON
cmake --build build-sanitize -j
ctest --test-dir build-sanitize --output-on-failure
```

## UCI engine

`zfs_engine` identifies itself as **Kugelfisch** over UCI. It is a deterministic,
single-search-worker engine with principal-variation alpha-beta search and
an evaluation built from conventional material plus ZFS leader/follower
initiative:

```sh
./build/zfs_engine
uci
isready
position startpos moves e2e4
go movetime 1000
```

It prints ordinary UCI `info` lines with depth, selective depth, score, nodes,
NPS, hash occupancy, time, and principal variation, followed by `bestmove`.
Supported limits include clocks/increments, `movetime`, `depth`, `nodes`, `mate`,
`infinite`, `ponder`/`ponderhit`, and order-independent `searchmoves`. Options are
`Hash` (up to 1024 MiB), `Move Overhead`, `Ponder`, and `Clear Hash`.

Standard `position startpos` and six-field `position fen` work normally. A
six-field FEN with en passant infers the uniquely known double-push origin as its
follow field. After any other move the notation remains lossy. To load an
explicit follow field, provide seven FEN fields; `position zfsfen` is also
accepted as a readable extension:

```text
position zfsfen 4k3/8/8/8/8/8/8/4K1N1 w - - 0 1 -
go depth 6
```

A GUI used for actual play must understand ZFS legality; UCI transports moves
and analysis but does not teach an orthodox-only GUI the variant's board rules.
The engine uses one CPU-intensive worker. A small controller thread exists only
so UCI `stop` remains responsive; parallel search is deliberately deferred.

## Rules utility

The perft utility accepts a positive depth and an optional seven-field ZFS-FEN:

```sh
./build/zfs_perft 6
./build/zfs_perft 3 '4k3/8/8/8/8/8/8/4K2R w K - 0 1 -'
```

## Evaluating engine changes

The repository includes a deterministic search benchmark, reproducible opening
generator, trusted-rules paired UCI match runner, append-only/resumable result
logs, and pentanomial logistic-GSPRT reporting. A minimal comparison is:

```sh
./build/zfs_bench
./build/zfs_match --candidate /path/to/new/zfs_engine \
  --baseline /path/to/old/zfs_engine \
  --openings openings/screen-v1.txt --output run.jsonl \
  --nodes 25000 --pairs 32
```

See [EVALUATION.md](EVALUATION.md) for the workflow, statistical interpretation,
fixed-time mode, committed screen/holdout suites, and reproducibility contract.
The match runner is currently POSIX-only and intentionally sequential.

## Core API

`zfs::Position` owns the complete state required by move legality: placement,
side, castling rights, en-passant square, clocks, and follow square. The hot path
uses caller-owned fixed storage:

```cpp
zfs::Position position = zfs::Position::start();
zfs::MoveList moves;
position.generate_legal_moves(moves);

for (zfs::Move move : moves) {
    zfs::Undo undo;
    position.do_move(move, undo);
    // Search the child.
    position.undo_move(move, undo);
}
```

`do_move` is deliberately unchecked; pass only a canonical move returned by
`generate_legal_moves` or `parse_uci`. Use `is_legal` or `parse_uci` at trust
boundaries.

## Play in the browser

The `viewer` directory contains a Chessground board backed by the exact C++
rules core compiled to WebAssembly. A small localhost service connects that
board to the native `zfs_engine`; search is not duplicated in JavaScript. The
browser remains authoritative for legal input and adjudication, and it validates
the engine's returned move before applying it.

Build the engine, install the pinned viewer dependencies once, and start the
playing UI:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
cd viewer
npm install
npm run play
```

Open the local URL printed by the command, choose a side and depth, then select
“Play from this position.” Depth 10 is the default. Iterative depth, score,
nodes, and principal variation are streamed while the engine thinks. Legal
destinations and the follow field remain visible. Left/Right navigate the move
line; navigation deliberately stops engine-play mode so an in-flight reply
cannot land on a historical position.

The same page includes all 64 games from the final champion-#3-versus-ZFS-0
checkpoint: 56 checkmates, seven automatic threefolds, and one draw imposed by
the match runner's 256-ply safety cap. Selecting a game replays every move
through the WebAssembly rules core and rewinds to its start. Arrow keys, the
move list, and the legal-destination display then work normally. Playing a move
from a reviewed position deliberately leaves the recorded line and creates a
new branch.

The committed archive is generated from a checksummed match log rather than
maintained by hand. A new archive can be exported with:

```sh
python3 tools/export_selfplay_gallery.py run.jsonl \
  viewer/src/selfplay-games.json \
  --candidate-name 'Kugelfisch candidate' --baseline-name 'Kugelfisch baseline' \
  --title 'Candidate vs baseline' --description 'Recorded paired checkpoint.'
```

The server keeps one UCI process alive and allows only one search at a time.
Each browser game gets a session identifier: `ucinewgame` clears the TT at the
session boundary, while later moves in that game retain useful TT entries. The
engine still has exactly one CPU-intensive search worker; Node and the UCI
controller only handle I/O and cancellation.

`npm run play` rebuilds the UI around the committed WebAssembly payload.
`npm run dev` also recompiles that payload and therefore requires Emscripten
(`em++`). `npm run serve` serves an existing `viewer/dist` without rebuilding.
Set `ZFS_ENGINE_PATH` to use an engine outside `build/zfs_engine`, and
`ZFS_VIEWER_PORT` to change the default port 4173. The service binds only to
`127.0.0.1`.

## License

Kugelfisch is free software licensed under GPL-3.0-or-later. See
[LICENSE](LICENSE).
Third-party viewer dependencies are listed in
[viewer/THIRD_PARTY.md](viewer/THIRD_PARTY.md).
