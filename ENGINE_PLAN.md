# Kugelfisch engine and roadmap

Status: the first single-worker autoresearch champion described below is
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
                |              |                    +-> material-only evaluation
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
ordinary choice node it uses material stand pat and searches captures and
promotions. Since forced quiet closure can grow without consuming material, the
initial quiescence horizon is eight plies after terminal and draw checks.

## Evaluation

Evaluation is conventional material only:

```text
pawn 100, knight 320, bishop 330, rook 500, queen 900
```

No mobility term is present. In ZFS, mobility may be a liability because it can
enable an opponent's follow obligation; adding such a term before evidence would
encode an unsupported strategic belief.

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

Chessground and the WebAssembly rules core own the displayed game. A
localhost-only Node service maintains one native UCI process and streams its
`info` and `bestmove` output to the page. Searches receive the canonical root
ZFS-FEN plus the complete move prefix, preserving repetition history rather than
reconstructing only the current diagram. A per-game token maps to
`ucinewgame`: the TT is cleared between games and retained between moves of one
game. Replacement requests send `stop` and wait for the old `bestmove` before
starting, so there is never more than one CPU-intensive search.

## Verification gates already present

- production rules tests and perft counts;
- an independent coordinate-walking legal-move oracle;
- incremental-key restoration and exact auxiliary-state repetition fixtures;
- automatic threefold/50-move and checkmate-precedence fixtures;
- alpha-beta comparison with exhaustive shallow minimax;
- material, mate, root restriction, twofold, determinism, and limit tests;
- browser bridge draw/navigation tests and execution of the shipped WASM;
- a check that the distribution embeds that tested WASM payload;
- a localhost service test covering validation, streamed analysis, a
  forced-follow `bestmove`, replayed threefold history, and serialized
  cancellation/replacement;
- subprocess UCI handshake, analysis, automatic draw, parameter-boundary,
  ponder, and stop tests;
- release plus ASan/UBSan runs.

## Deliberately later

1. Expand tactical and self-play corpora before adding evaluation terms.
2. Measure any further variant-safe reductions individually. The first cycle's
   rejected heuristics remain evidence, not dormant feature flags.
3. Design and solve ZFS tablebases through four pieces, including follow and
   auxiliary-state indexing and proven cylinder symmetries.
4. Add parallel search as a separate milestone, initially with per-worker
   positions and a C++ memory-model-safe shared TT.

Tablebases and parallelism must not complicate the current rules core or weaken
single-worker reproducibility.
