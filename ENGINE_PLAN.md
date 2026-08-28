# Kugelfisch engine and roadmap

Status: the second single-worker autoresearch champion described below is
implemented. Detailed rationales and measured tradeoffs are recorded in
[DECISIONS.md](DECISIONS.md) and [autoresearch/RESULTS.md](autoresearch/RESULTS.md).

## Implemented pipeline

```text
UCI controller thread
  parses position/options/limits and remains responsive to stop
                |
                v
one search worker
  iterative deepening -> PVS/alpha-beta -> exact-subset quiescence
                |              |                    |
                |              |                    +-> material + follow initiative
                |              +-> guarded null move and conservative LMR
                +-> exact history draws, ordering, history-qualified TT
                |
                v
production Position make / legal generation / unmake
```

The rules generator is the only legality authority. Search owns one mutable
`Position`, fixed per-ply move/score/PV/undo storage, and no shared mutable
position state. The protocol controller is not a second search worker.

## Draw and history model

The game rules are intentionally simple:

- the third occurrence is an automatic draw;
- 100 reversible halfmoves are an automatic draw;
- there are no claims, fivefold rule, or 75-move rule;
- below the root, a second occurrence scores as a draw search heuristic.

Repetition first checks an incremental base key over placement, side, and
castling. Hash matches are verified exactly. Differing raw follow or en-passant
state triggers complete legal-move-set comparison, so inert auxiliary fields can
still repeat. The scan walks only same-side positions in the reversible window;
the 50-move rule bounds it to 50 candidates.

TT scores use the raw position, halfmove clock, and a reversible-history context
signature. Position-only matches cannot safely reuse scores because draw rights
are path-dependent. Actual draw adjudication never relies on a hash.

## Search champion

The main search currently has:

- deterministic iterative deepening;
- fail-soft negamax alpha-beta with exact principal-variation search;
- mate-distance bounds and ply-normalized TT mate scores;
- TT, promotion/capture, killer, and quiet-history ordering;
- verified null-move pruning with a separate synthetic TT score domain;
- one-ply, late-seven LMR with exact parent/child follow guards;
- fixed depth, node, time, clock/increment, infinite, mate, and root-move limits;
- the last completely searched iteration as the reported result.

Legal generation proves ordinary non-king, non-en-passant moves safe without a
make/unmake when their origin is not a king-ray blocker. Ambiguous cases retain
the exact simulation fallback. Quiescence directly generates its required move
subset rather than building and filtering every legal move.

It deliberately has no futility, razoring, SEE, unchecked extensions,
countermove table, or qsearch TT. Those measured candidates either lost strength
or failed their engineering gate under mandatory follow.

Quiescence searches every legal move when checked or forced to follow. At an
ordinary choice node it uses static-evaluation stand pat and searches captures and
promotions. Since forced quiet closure can grow without consuming material, the
initial quiescence horizon is eight plies after terminal and draw checks.

## Evaluation

Evaluation starts with conventional material:

```text
pawn 100, knight 320, bishop 330, rook 500, queen 900
```

It then values **leader initiative**. For every side-to-move piece whose vacated
square an opponent could pseudo-legally enter, it charges the least costly
available follower, because the compelled side chooses among legal followers.
The base burdens are approximately one sixteenth of the follower's material
value. A movement-compatible follower that can keep shadowing the leader—such
as knight following knight, rook following rook, or queen following either
slider—costs twice as much. This also captures the queen's special liability:
it may be forced to follow bishops, rooks, and queens.

Pawn targets include legal push geometry and exclude promotion-rank arrivals,
which cannot keep following as pawns. A king is a prospective follower only of
a pawn; every non-pawn mover would attack its own origin after moving, so the
king could not legally enter it. The term uses pseudo-legal geometry
deliberately. Exact pins, checks, departure legality, and the resulting
compulsory move are resolved by search rather than duplicated imperfectly in a
horizon evaluator.

No generic mobility, piece-square, check, king-pressure, or drawishness term is
present. Plain mobility and cheap rank-centralization proxies failed their
screens; the accepted term distinguishes useful freedom to lead from mobility
controlled by the opponent.

## UCI surface

The executable supports the normal control commands and emits standard analysis
and `bestmove` lines. `searchmoves` can occur in any normal parameter order, and
pondering changes to its supplied clock budget on `ponderhit`. Standard
start-position and six-field FEN commands work; en-passant FEN infers the known
double-push origin, while seven-field FEN and `position zfsfen` preserve all
explicit follow state. Options are `Hash`, `Move Overhead`, `Ponder`, and
`Clear Hash`.
`Threads` is intentionally absent until parallel search exists.

## Browser play surface

Chessground and the WebAssembly rules core own the displayed game. A dedicated
Web Worker owns the same C++ Searcher, evaluation, Game history, and a 32 MiB
TT compiled to WebAssembly. Searches receive the canonical root ZFS-FEN plus
the complete move prefix, preserving repetition history rather than
reconstructing only the current diagram. The TT is cleared between games and
retained between completed searches in one game. Cancellation terminates and
recreates the non-threaded worker; no native engine endpoint, pthread,
SharedArrayBuffer, or cross-origin-isolation policy is required.

The public surface uses a fixed dark/gold theme and a sparse landing page with
focused Play, Games, and Analysis routes. Play defaults to the client engine at
depth 10 without clocks and excludes evaluation, PV, node counts, and arbitrary
position loading. Those technical controls live in Analysis, whose search
streams information without applying `bestmove`, displays scores from White's
perspective, draws the current legal PV move as a board arrow, and automatically
restarts to the selected fixed depth while its lever is enabled. Move history
and PV notation are converted to SAN by the C++ rules bridge, with UCI as the
failure fallback. Played games are appended to a
bounded local store as their move prefix grows; Games pages through those
records and replays each line through the WebAssembly rules core. Historical
navigation is read-only and returning to the live cursor resumes the game.
After an engine move, the browser speculates on the second move of the final PV
using the same worker. An exact game/root/depth/move-prefix match claims that
work; a miss, game stop, navigation, page exit, or replacement request
terminates it before any new search. A hit starts the following speculation as
well, retaining the one-search-worker invariant.

## Verification gates already present

- production rules tests and perft counts;
- an independent coordinate-walking legal-move oracle;
- incremental-key restoration and exact auxiliary-state repetition fixtures;
- automatic threefold/50-move and checkmate-precedence fixtures;
- alpha-beta comparison with exhaustive shallow minimax;
- material, leader/follower ordering, mate, root restriction, twofold,
  determinism, and limit tests;
- browser bridge draw/navigation tests and execution of both shipped WASM
  modules, including a forced-follow alpha-beta search;
- checks that the distribution embeds both tested payloads and no longer
  references a server engine endpoint;
- a localhost service test covering validation, streamed analysis, a
  forced-follow `bestmove`, repeated ponder-hit chaining, replayed threefold
  history, and serialized cancellation/replacement, plus an actual mid-search
  engine kill and recovery;
- subprocess UCI handshake, analysis, automatic draw, parameter-boundary,
  ponder, and stop tests;
- release plus ASan/UBSan runs.

## Deliberately later

1. Expand tactical and self-play corpora before adding further evaluation terms.
2. Measure any further variant-safe reductions individually. Earlier cycles'
   rejected heuristics remain evidence, not dormant feature flags.
3. Design and solve ZFS tablebases through four pieces, including follow and
   auxiliary-state indexing and proven cylinder symmetries.
4. Add parallel search as a separate milestone, initially with per-worker
   positions and a C++ memory-model-safe shared TT.
5. Deploy the static engine surface to Cloudflare Pages. Choose IndexedDB for a
   private browser archive or a small hosted database endpoint for a shared
   archive rather than coupling storage to the completed engine port.

Tablebases and parallelism must not complicate the current rules core or weaken
single-worker reproducibility.
