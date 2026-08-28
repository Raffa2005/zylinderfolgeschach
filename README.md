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
rules core compiled to WebAssembly. The complete C++ search, evaluation, game
history, and transposition table are compiled into a second WebAssembly module
owned by one Web Worker; search is not duplicated in JavaScript and does not
contact the native engine. The browser validates every returned move with the
independent rules module before applying it.

Install the pinned viewer dependencies once and start the playing UI. The
committed distribution already contains both WebAssembly payloads:

```sh
cd viewer
npm install
npm run play
```

Open the local URL printed by the command. The dark, compact interface has
focused Play, Games, and Analysis sections. Play offers only side and strength;
there are no clocks and depth 9 is the default. Legal destinations, the follow
field, and move history remain visible, while score, nodes, principal variation,
and arbitrary-position loading are deliberately absent. Left/Right review the
move line without allowing a historical position to fork the played game;
returning to the live position safely resumes play. Move lists use SAN derived
by the C++ variant rules core, with UCI retained as a fallback rather than
reimplemented in the page. Resigning ends and saves the game without adding a
fictitious terminal position to its replay.

After every engine move, the browser uses the second move of the completed PV
as the expected human reply and searches that resulting position to the chosen
depth. A matching reply claims the completed or ongoing result; a different
reply terminates the speculative worker before the ordinary search starts.
There is never a foreground and background engine search at once.

Every played game is saved as it progresses in
`.runtime/viewer/games.json`. Games presents a paged card list and exact replay;
the detail view supports the move list, arrow keys, and an Analysis handoff.
Analysis is the separate technical workspace: it exposes ZFS-FEN loading,
score, depth, nodes, and principal variation without making an engine move.
Its score is always shown from White's perspective, and the first legal move of
the current PV is drawn on the board. PV and history notation use the same
rule-aware SAN conversion as Play. Analysis is an on/off lever: while enabled,
every move, loaded ZFS-FEN, navigation step, and depth change automatically
starts a fresh search which stops at exactly the selected depth.
Saved lines are replayed through the production WebAssembly rules core rather
than trusted as diagrams.

The worker owns a 32 MiB TT, clears it between games, and retains it between
moves. Normal completed searches reuse the worker. Because this build is
deliberately non-threaded, cancellation terminates and recreates the worker;
that is the hard interruption boundary which keeps the page responsive without
SharedArrayBuffer, pthreads, or cross-origin-isolation headers. The local Node
service now supplies only static files and the saved-game database to the pages.

`npm run play` rebuilds the UI around the committed WebAssembly payload.
`npm run dev` also recompiles both payloads and therefore requires Emscripten
(`em++`). `npm run serve` serves an existing `viewer/dist` without rebuilding.
`ZFS_VIEWER_PORT` changes the default port 4173.
`ZFS_VIEWER_GAMES_PATH` changes the saved-game file. The service binds only to
`127.0.0.1`.

## Deploy the viewer to Cloudflare

The production site is [kugelfisch.pages.dev](https://kugelfisch.pages.dev/),
hosted by Cloudflare Pages. The rules and engine run in the browser; only the
saved-game API executes as a Pages Function, backed by D1. Static requests
therefore do not use the laptop or an engine server.

Use Node 22 and authenticate Wrangler once:

```sh
cd viewer
npm ci
npx wrangler login
```

For a remote VS Code or SSH session, use
`npx wrangler login --device --browser=false` instead of the callback-based
login command.

For the first deployment, create the Western Europe database, apply the checked-
in schema, create the Pages project, and deploy:

```sh
npx wrangler d1 create kugelfisch-games --location weur \
  --binding DB --update-config
npx wrangler d1 migrations apply DB --remote
npx wrangler pages project create kugelfisch --production-branch main
npm run deploy:cloudflare
```

Wrangler writes the non-secret D1 resource ID into `viewer/wrangler.jsonc`.
Later deployments need only `npm run deploy:cloudflare`; a schema change first
runs `npx wrangler d1 migrations apply DB --remote`. `build:cloudflare` bundles
the committed WASM payloads without requiring Emscripten. Recompile those
payloads with `npm run build` whenever C++ rules or engine source changes.

## License

Kugelfisch is free software licensed under GPL-3.0-or-later. See
[LICENSE](LICENSE).
Third-party viewer dependencies are listed in
[viewer/THIRD_PARTY.md](viewer/THIRD_PARTY.md).
