# Audited decisions

This is the decision record for the rules-core milestone. It describes choices
and their consequences, not the code line by line.

## Rule semantics

1. **Files are modulo 8; ranks are bounded.** Every file displacement wraps,
   including king, knight, and pawn-capture displacements. Rank movement never
   wraps.

2. **A move is identified by endpoints, not by a ray path.** A cylindrical rook
   can sometimes reach a square around either side of a rank, and a bishop's two
   upward or downward routes can meet at the antipodal file. A destination is
   emitted once and is reachable when at least one route is clear.

3. **Following means landing exactly on the previous origin.** “Towards” was
   made precise as “onto.” The obligation is computed from fully legal moves.
   Pseudo-legal moves that expose the mover's king do not force a follow.

4. **Attack maps do not apply follow.** Check, king destinations, and castling
   safety use geometric orthodox attacks on the cylinder. Pinned attackers still
   attack. This directly preserves the requirement that a king cannot rely on an
   opponent's future follow obligation for safety.

5. **Follow filtering is legal-first.** Let `L` be all cylindrical chess moves
   that preserve the mover's king. If any member of `L` lands on the follow
   square, only those members are permitted; otherwise all of `L` is permitted.
   This also defines the interaction with check evasions without a special case.

6. **Castling remains the orthodox king move on fixed squares.** The seam adds no
   castling forms or alternate castling paths. Castling is treated as one king
   move for an incoming follow, so the transferred rook does not add a second
   follow destination. Castling creates no stored follow field. A reply landing
   on the king's origin would attack the king there before castling, making the
   castle illegal; the rook transfer cannot unmask an exception. The only
   non-attacking arrival is a pawn promotion from `e2`/`e7`, whose pawn attacks
   both castling transit squares and therefore also makes castling illegal.
   Omitting the inert field is canonicalization, not a change to the legal move
   set. This is explicitly test-locked.

7. **Special moves use the primary mover.** En passant follows via the capturing
   pawn's landing square and creates its origin as the next field; the removed
   pawn's square is irrelevant. Promotions have four separate move identities
   and create the pawn's origin. Pawn captures and en passant wrap over `a`/`h`.

8. **Follow cannot create a terminal position by itself.** The fallback to `L`
   means the permitted set is empty exactly when `L` is empty. Checkmate and
   stalemate therefore use ordinary cylindrical mobility, although preceding
   forced choices change which positions are reached.

## State and interfaces

9. **The canonical notation is seven-field ZFS-FEN.** The seventh field is the
   follow square or `-`; the standard en-passant field remains separate. A
   six-field FEN is accepted as a lossy import. Its follow field is normally
   absent, except that an en-passant target uniquely proves the preceding
   double-push origin, which is inferred as the follow field. This avoids making
   standard FEN describe the wrong legal position immediately after a double
   push.
   Occupied follow fields, inconsistent castling rights, impossible en-passant
   structure, back-rank pawns, missing kings, and a checked non-moving king are
   rejected.

10. **Move application is split at the trust boundary.** Text and external move
    intents are matched against generated canonical moves. `do_move`/`undo_move`
    are allocation-free engine primitives and intentionally assume their input
    came from the generator.

11. **History-dependent draws live in `Game`, not stateless `Position`.** The
    rules core still reports board-local checkmate and stalemate. `Game` owns the
    played positions and makes the third occurrence and the 100th reversible
    halfmove automatic draws. There are no claims, fivefold rule, or 75-move
    rule. Checkmate is tested before the move-count draw. FEN preserves the clock
    but cannot reconstruct earlier repetition history. Orthodox insufficient-
    material shortcuts remain excluded until proved for cylindrical geometry.

## Representation and performance

12. **Squares retain the conventional `a1 = 0` 64-bit mapping.** This keeps
    bit scans, serialization, and future engine integration simple. Piece
    bitboards are backed by color occupancy and a byte mailbox; the redundant
    forms are maintained incrementally and checked by validation tests.

13. **Cylindrical slider data is compact and precomputed into the binary.** A
    2 KiB byte table handles cyclic ranks. Six monotonic vertical/diagonal ray
    masks use bit scans to trim beyond the first blocker. Step-piece attacks are
    also precomputed. The complete attack object is about 7.6 KiB of read-only
    data and requires no BMI2/`pext` support. A branch-free 514 KiB direct-table
    design was implemented and compared, not rejected by intuition: on the same
    depth-6 perft workload the compact design ran around 59–60M nodes/s versus
    roughly 55M, largely because make/unmake no longer maintains three redundant
    line-occupancy families.

14. **Exact make/check/unmake remains the ambiguous-case fallback.** The first
    implementation filtered every pseudo-legal move this way because
    cylindrical positions have two-route pins, antipodal overlaps, and
    three-square en-passant changes. The optimized generator now proves the
    common cases safe once per position: an ordinary non-king move cannot expose
    its king unless its origin is a first blocker on a king-to-slider route.
    Checks, king moves, en passant, and those blockers still use the exact old
    test. A separately written coordinate-walking oracle, explicit two-route
    pin fixtures, and deterministic randomized playouts compare complete move
    sets against this optimization.

15. **The follow hot path is reverse-generated.** Because nearly every child has
    a follow field, the generator first finds only pieces that can land on that
    square and legality-checks those candidates. It generates the full move set
    only when none is legal. Debug builds compare this specialized candidate set
    with the all-moves generator.

16. **The previous mover's type is not stored just to prune king followers.** A
    king can follow only a move made by a pawn: every other ordinary mover
    attacks its own origin from its destination, while a king cannot enter that
    attacked square. The fact is useful, but the current position state records
    only the follow square. Adding a previous-piece tag and a hot-path branch to
    suppress a handful of king candidates is deferred until profiling supports
    it. Castling needs no tag because its inert follow field is omitted.

17. **The rules core is intentionally single-threaded.** It contains no threads,
    atomics, locks, thread-local state, or mutable global tables. Positions and
    move buffers are caller-owned; future search parallelism can give each worker
    its own state without putting synchronization in move generation. The attack
    tables are compile-time, read-only data.

18. **Moves and move lists use fixed storage.** A move is 32 bits. A move list has
    512 entries: a deliberately loose bound from at most 16 pieces, at most 28
    cylindrical queen destinations, at most 12 promotion choices per pawn, one
    king, and castling. There is no allocation in release move generation or
    make/unmake.

19. **No search assumptions have been smuggled into the rule layer.** In
    particular, null-move pruning is suspect in a forced-follow game, and a
    captures-only quiescence generator would omit mandatory quiet follows. Those
    techniques require variant-specific proof and testing later.

## Verification standard

20. **Tables are checked against an independent coordinate walker.** Every one
    of 64 origins and all 256 relevant line-occupancy patterns are compared for
    ranks, files, and both wrapped diagonal families.

21. **Special cases are regression tests, not comments.** Tests cover both paths
    around cyclic ranks, antipodal bishop paths and duplicate suppression, seam
    king/knight/pawn moves, legal-versus-pseudo follow, follow while checked,
    attacks ignoring follow and pins, seam en passant, en-passant discovered
    check, promotion, castling transit/final safety, castling rights, terminal
    states, randomized make/unmake restoration, and the viewer C ABI's state,
    error, history, and special-move transitions.

22. **A second legal generator is the differential oracle.** The test-only
    implementation uses a character mailbox, runtime coordinate walking,
    immutable position copies, and dynamic move lists; it shares no production
    attack or move representation. Selected edge positions and 12 deterministic
    randomized games compare complete legal UCI sets and successor ZFS-FENs.

23. **Sanitizers fail the test process.** The suite runs under ASan and UBSan,
    with UBSan recovery disabled so diagnostics cannot be reported as passing
    CTest runs. Perft supplies deterministic node-count and throughput regression
    measurements; it is not presented as an independent correctness oracle.

24. **The adversarial review changed the implementation.** It found and caused
    fixes for an unchecked fullmove-counter wrap, public sentinel/index UB, a
    release-only fixed-list overflow surface, an invalid public default position,
    excessive generator stack use, non-fatal UBSan configuration, black-side
    test gaps, and the oversized attack-table choice. The release generator now
    uses one 2,056-byte candidate buffer (about 2,176 bytes total stack by GCC's
    stack-usage report), and only `Position` can append to a `MoveList`.

## Browser board and play surface

25. **The viewer runs the production C++ rules, not a JavaScript port.** A thin
    C ABI serializes position state and legal UCI moves to JSON; Emscripten
    compiles that bridge and the same `attacks.cpp`/`position.cpp` into
    WebAssembly. Chessground receives only the generated destination map. This
    keeps manual inspection from accidentally validating a divergent ruleset.

26. **The browser bundle contains no search implementation.** A native engine
    opponent is reached through the localhost service described in decisions
    43–48; JavaScript does not duplicate evaluation or alpha-beta. A single
    browser thread owns a linear array of position snapshots, its corresponding
    move list, and a cursor. Backward navigation retains the future; forward
    navigation restores it exactly. A move played from the past truncates that
    future branch. Snapshot copies are appropriate for this UI and do not affect
    the engine hot path.

27. **Chessground is UI-only and its state is refreshed after every move.** The
    C++ core remains authoritative for castling, en-passant captures, promotion,
    check, and terminal state. Refreshing the complete placement also avoids
    depending on Chessground's orthodox move-side effects for variant moves.
    Legal UCI moves are shown both as board destinations and as a clickable list;
    the current follow square is marked in yellow. Left/Right navigate one ply,
    history entries are clickable, and editable controls retain normal arrow-key
    behavior.

28. **The viewer pins Chessground 10.1.1 and esbuild 0.28.2.** Pinning makes the
    build reproducible and avoids silent UI API drift. Chessground is licensed
    GPL-3.0-or-later; anyone distributing the bundled viewer must comply with
    that license. A small localhost-only HTTP server replaces a framework dev
    server and also brokers the native UCI process. The viewer dependencies
    remain outside the dependency-free rules core.

## History and engine

29. **Repetition uses a cheap filter and an exact decision.** `Position`
    maintains a raw key over placement, side, castling, en passant, and follow,
    plus a base key with en passant and follow removed. A different base key
    rejects immediately. A matching key is followed by exact placement, side,
    and castling comparison. If the auxiliary fields differ, complete canonical
    legal-move sets are compared. This handles inert follow and en-passant state
    without encoding fragile special cases. Hashes never adjudicate a draw.

30. **The repetition scan stays linear because it is tightly bounded.** Only
    same-side positions since the last pawn move or capture are candidates, and
    castling-right differences stop a played-game scan. The automatic 50-move
    draw bounds a live query to at most 50 cheap same-side comparisons. A hash
    map is not placed in this tiny hot loop without profiling evidence.

31. **Incremental hashing pays only a measured hot-path cost.** Deterministic
    compile-time Zobrist tables are updated by make. `Undo` stores the former key,
    so unmake restores it directly and reverses pieces without pointless hash
    writes. Debug validation recomputes the key from the board. After this
    optimization, start-position depth-6 perft measures about 56–57M nodes/s on
    the development machine versus the earlier rules-only 59–60M range.

32. **Threefold and 50 moves are automatic; twofold is search-only.** At the
    played-game root, only the third occurrence draws. Below the root, the
    search scores a second occurrence as zero to terminate cycles and avoid
    wasting depth on repetitions. This can change a root move's reported score
    before a rules-level draw exists, so it is an intentional search
    approximation rather than exact finite-depth minimax. It never exposes that
    heuristic as a game result.

33. **The first engine is deterministic fail-soft negamax alpha-beta.** It uses
    iterative deepening, mate-distance bounds, make/unmake, a principal
    variation, and the last fully completed iteration. There is no LMR, futility,
    null move, or other selective main-search pruning yet. Shallow results are
    checked against exhaustive minimax with quiescence disabled.

34. **Evaluation is deliberately material only.** Pawn, knight, bishop, rook,
    and queen values are 100, 320, 330, 500, and 900 centipawns. There are no
    mobility, center, king-safety, pawn-structure, or follow-pressure terms. In
    particular, mobility is not assumed beneficial when it may enable following.

35. **Quiescence respects forced follow but is explicitly bounded.** In check or
    at a forced-follow node it searches every legal move, including quiet moves,
    and does not stand pat. At an ordinary node it searches captures and
    promotions after stand pat. Forced quiet closure can explode without
    consuming material, so the initial quiescence horizon is eight plies; at the
    cap the material evaluation is returned after terminal/draw checks. This is
    a documented horizon heuristic, not a rule claim.

36. **TT scores are qualified by reversible history.** A position-only key is
    unsafe because repetition and the move clock change scores. The score key
    mixes the raw position, halfmove clock, and an incrementally maintained
    multiset signature of raw positions since the last repetition-irreversible
    move. Exact draw checks still use history. The TT uses four 16-byte entries
    per 64-byte cluster, full 64-bit keys, generation aging, and depth-versus-age
    replacement. Its six-bit generation is cleared before wrapping, so entries
    64 searches old cannot masquerade as current. The public table limit is 1
    GiB; a valid UCI option cannot request a 64-GiB value-initialized allocation.
    Stockfish's [clustered TT](https://github.com/official-stockfish/Stockfish/blob/master/src/tt.cpp)
    and [mate-score/search conventions](https://github.com/official-stockfish/Stockfish/blob/master/src/search.cpp)
    informed the shape; its ordinary-chess pruning was not imported.

37. **Move ordering is intentionally small.** Legal TT move, promotions,
    MVV-LVA captures, two killers, and color/from/to history are sufficient for
    the baseline. Cylindrical SEE and more elaborate histories wait for an
    independent test oracle. Mandatory quiet moves do not justify pruning.

38. **One CPU-intensive search worker implements UCI.** A lightweight protocol
    thread remains responsive to `stop`; it is not search parallelism. Each
    search owns one mutable position and fixed per-ply buffers on the heap. The
    TT is accessed by only that worker, so it needs no atomics or data races.
    Multi-worker search is a separate later milestone.

39. **The executable follows normal UCI control flow.** It supports `uci`,
    `isready`, `ucinewgame`, `setoption`, `position`, `go`, `stop`, and `quit`,
    including clock, increment, depth, node, movetime, mate, infinite, and
    order-independent `searchmoves` limits. `go ponder` defers its clock until
    `ponderhit`; `stop` remains responsive. The `Ponder` option advertises and
    validates capability, while the GUI's `go ponder`/`ponderhit` pair controls
    each actual session; the engine never starts pondering autonomously. Numeric
    limits are range-checked at the protocol boundary and defensively normalized
    again at the search API. `movetime` is an explicit duration and is not
    reduced by `Move Overhead`; overhead applies only when allocating a clock
    budget. Options are `Hash`, `Move Overhead`, `Ponder`, and `Clear Hash`.
    Standard six-field `position fen` infers follow after a double push and is
    otherwise a lossy no-follow root; seven fields and `position zfsfen`
    preserve explicit state. The command surface was cross-checked against
    Stockfish's
    [UCI implementation](https://github.com/official-stockfish/Stockfish/blob/master/src/uci.cpp),
    while keeping this engine's parser and one-worker lifecycle small.

40. **Search verification is layered.** Release and ASan/UBSan runs cover the
    original rules tests, independent move-generator differential tests, viewer
    bridge, exact history/draw identity, material/mate/search limits, twofold
    search behavior, exhaustive shallow minimax comparison, and a subprocess UCI
    handshake/search/draw/stop/ponder/boundary test. The generated WebAssembly is
    executed under required Node.js, and the distribution carries a build-time
    SHA-256 of that complete generated module. The test recomputes the hash,
    preventing source-only viewer tests—or a silently skipped runtime test—from
    blessing a stale bundle without depending on Emscripten's current binary
    embedding syntax.
    Fixed-time and interactive-stop sessions are also exercised manually.

41. **Four-piece tablebases are a later subsystem.** No orthodox tablebase or
    insufficient-material assumption is used now. The future solver must encode
    side, follow, and relevant auxiliary state under cylindrical legality, prove
    its indexing symmetries, and integrate only after single-worker search is
    stable. Parallel search is likewise deferred and is not entangled with this
    baseline.

42. **The final adversarial pass was code-only and its valid findings were
    reproduced.** It found the six-field en-passant import error, three numeric
    overflow paths, broken `searchmoves` parameter ordering, ignored
    `ponderhit`, TT generation aliasing, an unsafe advertised Hash ceiling, a
    stale shipped viewer runtime, and redundant legal generation during UCI
    history replay. A second pass caught explicit movetime being reduced by
    overhead, missing Ponder advertisement, and optional artifact verification.
    All were fixed and regression-tested. Its castling-follow
    complaint was retracted after the reviewer found the explicit canonical
    representation tests. Its twofold complaint is retained here as the
    deliberate approximation in decision 32, because that behavior was an
    explicit project requirement rather than an accidental rules adjudication.

## Native engine integration

43. **The browser remains the game authority.** Chessground is only the input
    and rendering surface; the WebAssembly `Game` supplies destinations,
    terminal state, history, and the canonical ZFS-FEN. A native engine reply is
    passed back through `zfs_play` and rejected visibly if it is not legal. The
    service therefore cannot silently move the board into a state that disagrees
    with the production rules core.

44. **Search receives a root plus moves, not just a current diagram.** The page
    retains the canonical FEN from the most recent reset/load and sends the
    exact history prefix through the current cursor. The UCI engine replays that
    line into `Game`, retaining repetition occurrences and the reversible
    history signature needed by draw detection and TT qualification. Sending
    only the displayed FEN would be simpler but wrong for threefold search.

45. **One persistent native process serves the UI.** Process launch, the
    64-MiB default table allocation, and UCI negotiation happen once. An opaque
    browser-game identifier causes one `ucinewgame` at a new session boundary;
    later moves keep the TT from the same game. A crashed process is discarded
    and lazily restarted. This is both faster and less stateful than attempting
    to port search into the browser.

46. **Replacement is serialized through completed UCI stop.** A new request
    aborts the old one, sends `stop`, and waits for its `bestmove`, which proves
    the engine's search worker has returned, before starting the replacement.
    Requests themselves are serialized so simultaneous replacements cannot race
    past that wait. A non-responsive child is killed after 500 ms and restarted.
    Thus the service never intentionally runs two CPU-intensive search workers.

47. **The HTTP surface is deliberately narrow and local.** It binds to
    `127.0.0.1`, serves static files, exposes status and one NDJSON search route,
    bounds request size/depth/history, validates opaque IDs and canonical UCI
    move spelling, forbids newlines in FEN, and passes no user text through a
    shell. Iterative `info` events stream without polling. `ZFS_ENGINE_PATH` can
    select another local build, but the browser cannot choose an executable.

48. **The UI is a playing surface, not an edge-case gallery.** The preset
    collection and old rules-viewer framing were removed. The default depth is
    10 but remains editable from 1 through 100; either human color is supported,
    the board follows that orientation, and score/depth/nodes/PV remain visible.
    Manual legal-move display, FEN loading, promotion, history, and arrow-key
    navigation remain useful. Navigating or loading stops engine mode; resetting
    starts a fresh engine session and automatically moves first when the human
    selected Black.

## Measured single-worker optimization

49. **Benchmarks pin one process to one physical core and report both work and
    time.** Raw NPS is not comparable across engines unless node definitions,
    positions, and search limits match. For local before/after measurements the
    engine searched the start position to a fixed node count or completed depth.
    Stockfish 16 used one thread and a 64-MiB hash. Its `bench` command was given
    the explicit sixth argument `classical` or `NNUE`; otherwise that command
    selects its own mixed evaluation modes and can silently override a prior
    `Use NNUE` setting.

50. **Profiling selected legal filtering, not the material evaluator, as the
    first target.** Instrumented search made about 23.0 million `do_move` and
    23.0 million `undo_move` calls per one million counted search nodes. After
    the king-ray safety proof in decision 14, those counts fell to about 3.9
    million each. Reverse-generated follow candidates remain exact-tested
    because their lists are normally tiny; constructing all blocker information
    before testing them measured slower.

51. **Ordinary quiescence orders only moves it can search.** At a node that is
    neither checked nor follow-forced, captures and promotions are compacted to
    the front before scoring and selection ordering. The previous code scored
    and repeatedly scanned all quiet legal moves only to discard them. Checked
    and follow-forced quiescence still searches every legal move, preserving the
    variant's mandatory quiet continuations.

52. **Principal-variation search is exact and now part of the baseline.** The
    first ordered child uses the full alpha-beta window. Later children receive
    a zero-width window and are searched again with the full window only when
    they improve alpha without cutting off. This changes neither the leaf
    evaluator nor the minimax result; shallow positions remain compared with
    exhaustive minimax. On the measured depth-10 start-position search it cut
    the node count from about 38.1 million to 21.7 million while preserving the
    score and principal variation. Together with the legal-generator and
    quiescence changes, the host-native build completed that search in 6.379
    seconds at 3.41 million NPS; the original observed run took about 19.4
    seconds.

53. **Host-specific instructions are explicit and opt-in.** `ZFS_NATIVE=ON`
    enables `-march=native` for GCC or Clang. It produced hardware `popcnt` for
    the material evaluator and about a five-percent local speedup. The default
    stays portable so a distributed binary cannot accidentally require the
    build machine's ISA.

54. **Small or unproven complexity was rejected after measurement.** A
    250-centipawn root aspiration window saved roughly 0.05 percent of the
    depth-10 nodes and no wall time, so it was removed. Skipping reversible
    history-context maintenance in quiescence saved less than one percent while
    introducing a second path-state regime, so it was also removed. At this
    checkpoint incremental material, SEE, null move, LMR, and futility remained
    candidates rather than assumed wins. Decisions 62 and 63 record the later,
    measured promotion of guarded null move and LMR; the others remain absent.

## Change evaluation

55. **Correctness, work, and strength are separate gates.** The existing rules,
    differential, search, UCI, and browser tests remain the correctness gate. A
    twelve-position deterministic benchmark reports both nodes and time and
    hashes scores, best moves, node counts, and PVs into a behavior signature.
    Paired self-play alone answers the strength question. No one metric is used
    as a substitute for the other two.

56. **The match process, not either engine, adjudicates games.** Both UCI engines
    merely propose moves. A fresh production `Game` replays the full opening and
    validates every reply, then applies automatic threefold and 50-move draws,
    checkmate, and stalemate. Illegal moves, crashes, and hard timeouts forfeit.
    Eval-based resignation/draw rules were excluded. The only artificial rule is
    a recorded safety ply cap, checked after real terminal rules.

57. **Experiments use complete-history paired openings.** A suite line is a move
    sequence from `startpos`, not a FEN, because repetition and follow state are
    history-sensitive. Each line is played with candidate colors swapped and
    enters one of five pair bins. The bundled screen and holdout books use fixed
    seeds and reject terminal, checked, materially unequal, or duplicate final
    states. They bootstrap testing; they are not represented as an
    optimal-opening corpus.

58. **The sequential test uses pentanomial logistic GSPRT.** The five paired
    bins retain within-pair color correlation. The likelihood implementation
    follows Fishtest's constrained multinomial maximum-likelihood calculation
    and its 0.001 prior for empty bins. Logistic Elo is named explicitly;
    normalized Elo is not approximated or silently conflated with it. Decisions
    occur only on pair boundaries at configured alpha/beta bounds.

59. **Runs are reproducible and resumable without infrastructure.** One
    checksummed JSONL manifest fingerprints the runner and both engine binaries,
    the opening file, exact limits, hypothesis parameters, canonical paths,
    optional revision IDs, and engine names. Binding the runner image prevents
    one log from mixing referee or statistical semantics across rebuilds.
    Existing files are never overwritten. Resume requires the same fingerprint
    and counts only contiguous, internally consistent, checksummed pair records;
    unknown or corrupt complete records are rejected. Both games live in one
    record. Only unterminated final-record bytes are discarded, and that entire
    pair is replayed. An exclusive advisory lock prevents cooperating local
    runners from resuming the same unchanged path. Renaming or replacing the log
    during a run is explicitly outside that contract. Result descriptors close
    across exec. Engine input is nonblocking and command writes share the
    response deadline, so an engine
    that stops consuming input cannot wedge the runner; timed-out engine process
    groups are killed as a unit. Host pipe/fork/process-isolation failures abort
    the experiment rather than being misreported as a color-dependent forfeit.
    Opening bytes are loaded once, so the validated lines and their fingerprint
    cannot diverge. Engine binaries run from their canonical paths, preserving
    executable-relative libraries and resources. Their content is fingerprinted
    initially; cheap file-identity checks around launches and pair commits catch
    replacement without repeatedly scanning a large executable. Runtime-loaded
    models, libraries, and configs
    cannot be discovered generically; the optional candidate/baseline IDs bind a
    caller-supplied immutable deployment identity for those cases. There is one
    local worker; no scheduler, database, dashboard, or distributed protocol was
    built.

60. **Fixed nodes and fixed movetime answer different questions.** Fixed nodes
    reduce noise when comparing search efficiency and ordering. Fixed movetime
    covers timing and stop behavior but inherits host noise. Every game starts
    fresh engine processes, preventing state leakage across colors at the cost
    of irrelevant process-start overhead outside the move budget. `Hash` is set
    to the same requested size when advertised by the engine; an out-of-range
    request is rejected explicitly. Unsupported UCI options are never sent, and
    support is recorded in the manifest. Explicit `Clear Hash` is redundant
    because the process is new.

## First autoresearch cycle

61. **ZFS-0 is the permanent zero-Elo reference.** Commit `ddbcfad` and its
    frozen native binaries define 0 Elo. Candidate builds, the rules referee,
    opening bytes, limits, source diff, and raw paired-game logs are hashed into
    an append-only ledger. Routine screens never rewrite the baseline. A change
    is promoted only after correctness, deterministic-work, self-play, and
    adversarial-review evidence are considered separately; a statistically
    inconclusive test is reported as such rather than dressed up as proof.

62. **Guarded, verified null-move pruning is retained.** It runs only in a
    null-window node at depth five or greater, outside check, compulsory follow,
    mate-score, near-rule-50, pawn-only, and static-fail-low regions. Consecutive
    nulls are forbidden and every null fail-high receives a verification search.
    Synthetic paths have a permanent TT score domain, restore all clocks and
    auxiliary fields, and never use real-path repetition adjudication. The first
    version was rejected when review found that an irreversible synthetic move
    could erase a temporary domain marker; no result from that version was
    credited to the corrected implementation.

63. **Late-move reductions are narrow and follow-aware.** Only the seventh and
    later moves at a null-window node of depth at least five with at least eight
    legal moves are eligible. The one-ply reduction excludes checks, parent or
    child compulsory follow, captures, promotions, castling, the TT move, and
    both killers. An alpha-raising reduced result is re-searched at full depth.
    The earlier fourth-move/depth-four schedule was faster but lost its screen
    and was removed.

64. **Quiescence generates its exact required subset directly.** Active follow
    and check still generate every legal reply. Ordinary nodes generate captures
    and every promotion, including quiet promotions; an empty tactical subset
    performs only enough quiet legality work to distinguish an ordinary position
    from stalemate. A differential oracle covers deterministic play plus forced
    and inactive follow, cylinder en passant, promotion, mate, and stalemate.
    Fixed-depth behavior stayed identical while median depth-nine time improved
    by about 23 percent.

65. **Evaluation remains conventional material.** Pawn 100, knight 320, bishop
    330, rook 500, and queen 900 remain the complete evaluator. A cylindrical
    bishop increase to 360 was positive in a smoke and exactly neutral in its
    independent confirmation; 390 was worse and expanded the tree. Mobility,
    conventional file-centrality, king terms, and follow bonuses are not added
    without variant evidence.

66. **Rejected heuristics leave no dormant machinery.** History gravity and
    maluses, a qsearch TT, countermove ordering, unbudgeted unique-follow
    extensions, and two depth-one quiet-futility schedules all failed their
    declared work or strength gates and were removed. Their source hashes,
    measurements, and rejection reasons remain in `autoresearch/ledger.jsonl`
    and are summarized in `autoresearch/RESULTS.md`. The resulting champion cuts
    the depth-nine benchmark from 11,425,195 to 4,201,814 nodes and interleaved
    median time from 3,416 to 1,114 ms versus ZFS-0. Its 32-pair final timed
    checkpoint scored 51.56%, with a wide and inconclusive confidence interval.

67. **The public engine name is Kugelfisch; ZFS remains the variant and code
    vocabulary.** UCI and the playing surface use Kugelfisch. Executable names,
    namespaces, ZFS-FEN, protocol extensions, environment variables, and rules
    terminology keep `zfs` because renaming those stable interfaces would create
    migration work without changing the product identity.

68. **Self-play exports are complete, machine-verified research artifacts.** The
    64 games from the final champion-#3-versus-ZFS-0 checkpoint remain an
    uncurated record rather than a selection of attractive wins. The export
    strips deployment paths and fingerprints but retains colors, result,
    termination, opening identity, work totals, and complete UCI moves. A
    generated-runtime test replays every move through the production
    WebAssembly rules core and verifies its final state. This experiment corpus
    is no longer the website's Games collection; public game browsing is built
    from games actually played through the site.

## Second autoresearch cycle

69. **Evaluation distinguishes mobility from opponent-controlled mobility.**
    Conventional material remains the base, but the side to move receives
    leader initiative when an opponent could enter a piece's vacated square.
    Generic pseudo-mobility was tested separately, was nearly neutral, and cost
    about 10.9 percent at fixed depth. It was removed. The accepted signal is
    specifically the right to choose the path while making the opponent follow,
    not a blanket preference for more destinations.

70. **Leader initiative charges the cheapest prospective follower.** The
    compelled player may choose among all legal followers, so summing attackers
    would exaggerate the burden. Base charges are approximately one sixteenth
    of follower material value. Movement-compatible followers that may sustain
    the chase—knight/knight, bishop/bishop-or-queen, rook/rook-or-queen, and
    queen/queen—receive a factor of two. The least-cost type order is generated
    at compile time from those coefficients, avoiding a second hand-maintained
    table.

71. **The horizon term is deliberately pseudo-legal, with known impossibilities
    excluded explicitly.** Pawn pushes include clear double-push transit and
    exclude promotion-rank arrivals. Kings are considered only as followers of
    pawns, because every non-pawn mover attacks its origin from its destination.
    Pins, check, and departure legality remain the search's job. Duplicating
    full legality in an evaluator called at millions of leaves would be both
    slower and a second rules implementation. Focused tests cover cheapest-
    follower selection, king inclusion and exclusion, clear double-push transit,
    promotion exclusion, and relative follower burden.

72. **Evaluation promotion requires longer independent confirmation.** A
    fourfold sustainable-shadow penalty scored 63.28 percent at 30 ms and then
    48.44 percent at 100 ms. Thirty milliseconds remains useful for rejecting
    weak ideas, but no evaluator change is promoted from that signal alone. The
    accepted leader term scored 57.81 percent at 30 ms and 55.47 percent at 100
    ms in independent samples, aggregating to 56.64 percent over 64 pairs with a
    positive 95 percent interval. The rule-refined final form later scored 53.91
    percent in a separate 100 ms sample; that sample is reported as inconclusive
    by itself.

73. **Plausible material and placement retunes remain evidence, not code.** A
    knight rank bonus, knight value 360, queen value 850, generic mobility, and
    stronger shadow multipliers all failed their work/confirmation gates. They
    were removed rather than left behind flags. The final material values remain
    100/320/330/500/900, with only the measured leader/follower term added.

74. **Selective-search tests assert contracts, not accidental full-width
    equality.** LMR is required to execute only under its explicit guards and
    to return a legal search result. Requiring its best move and score to equal
    an unreduced search at one evaluator-dependent start position falsely treats
    a selective heuristic as exact. Shallow alpha-beta remains compared against
    exhaustive minimax where exact equality is a valid contract.

75. **Engine-process recovery is bounded and the local viewer is supervised.**
    If the native UCI child exits unexpectedly, the client starts a fresh child
    and replays that search once with the full root position and move history.
    Explicit cancellation is never retried, and a second process failure is
    returned to the browser rather than creating an infinite restart loop. The
    integration test kills a real Kugelfisch process during search and requires
    the replay to return a legal move. On the development machine the Node
    viewer is an enabled user service with whole-service automatic restart, so
    it no longer depends on a Codex terminal session remaining attached.

76. **The website separates play, saved games, and analysis—not explanation.**
    The public structure is a sparse landing page plus Play, Games, and Analysis
    routes with one fixed dark/gold visual vocabulary. There is no theme switch
    or Rules page. The Bachelor project's hierarchy and interaction economy
    were used as a read-only reference; no source, asset, runtime, or data was
    copied or modified. Play exposes only side, strength, legal interaction,
    turn/follow state, and move navigation. ZFS-FEN, score, nodes, depth, and PV
    exist only in Analysis.

77. **Browser play uses bounded, single-worker PV pondering.** Depth 10 was the
    standard server option when pondering was introduced; decision 96 changes
    only the fresh-session default. There are no clocks. After each engine move,
    the second move of the final PV is treated as a prediction and the same UCI
    process searches the position after that reply with `go ponder depth N`.
    Only an exact game token, root ZFS-FEN, depth, and complete move-prefix match
    may issue `ponderhit`; a miss is stopped and drained before normal search.
    Stop, navigation, replacement, page exit, and service shutdown share the
    same serialized cancellation boundary. A hit immediately starts the next
    prediction, so pondering continues across turns without ever creating a
    second CPU-intensive search worker.

78. **Played-game storage is a small append-forward database boundary.** Each
    browser game receives an opaque ID and is persisted from its canonical root
    ZFS-FEN, complete move prefix, final ZFS-FEN, players, depth, and result.
    Updates may only extend the existing prefix; completed games are immutable,
    apart from idempotent repeats. Writes are serialized and atomically renamed,
    with a 5,000-record bound and no native Node dependency. The HTTP list/detail
    interface is intentionally storage-agnostic so a hosted implementation can
    replace the local JSON file without changing the pages.

79. **Historical play navigation is read-only and resumable.** Entering history
    cancels and drains search/ponder work but does not end the game. Input is
    disabled before the live cursor, preventing an accidental branch that would
    conflict with the append-only saved record. Returning to the last ply resumes
    the human turn or starts a fresh engine search as appropriate. Stop and new
    search requests are awaited at this boundary so a late stop cannot kill the
    resumed search.

80. **Browser notation is produced by the variant rules core.** Move history
    and analysis PVs use SAN-like notation generated in C++ from the exact legal
    move set at each position. Cylindrical captures, the follow restriction,
    castling, promotions, check, mate, and legal disambiguation therefore do not
    acquire a second JavaScript rules implementation. UCI remains the stored and
    transmitted canonical format and is displayed if a supplied line cannot be
    converted completely.

81. **Follow and move indicators are composable square state.** The follow
    field is a gold ring on a Chessground custom-highlight square, not a
    competing SVG annotation. When the selected piece can legally reach that
    field, the ordinary destination dot is layered inside the ring; legal
    captures retain their capture outline. This is presentation only and does
    not change move generation. Analysis draws only a still-legal first PV move,
    and its evaluation is normalized to White's perspective at display time.

82. **Terminal positions state only the result.** Checkmate and draw outcomes
    receive a visually distinct status panel, while follow-field diagnostics are
    suppressed once the game is over. The same terminal language is used in Play
    and Analysis. The cylinder arrow is a committed SVG favicon and header mark,
    and the product descriptor is the literal
    “Zylinderfolgeschach-Engine”—branding does not introduce another rules page
    or marketing copy.

83. **The public engine is the production C++ engine compiled to WebAssembly.**
    The worker payload links the existing Position, Game, evaluation, Searcher,
    and transposition-table sources; JavaScript provides transport and lifecycle
    only. Each request supplies the root ZFS-FEN and complete move prefix, so
    repetition semantics are unchanged. A generated-payload test executes a
    forced-follow search and the distribution test rejects any remaining
    `/api/engine` dependency.

84. **Browser search remains single-threaded and uses worker termination as
    cancellation.** One dedicated worker owns a 32 MiB TT. Completed searches
    retain it, new games clear it, and a cancelled search loses it because the
    worker is terminated and recreated. This gives a prompt, race-free hard stop
    without pthreads, SharedArrayBuffer, or cross-origin-isolation deployment
    requirements. Start-position depth 10 produced the same 1,287,275 nodes and
    PV as native search, at roughly 93 percent of native throughput in the local
    measurement.

85. **Client pondering is fixed-depth speculation, not emulated UCI text.**
    After an engine move, the worker searches the position after the second PV
    move. An exact root, depth, and complete move-prefix match claims either its
    running or completed result; a mismatch cancels it before foreground search.
    A successful claim immediately begins the next speculation. The native
    executable remains regular UCI, while the browser avoids parsing its own UCI
    subprocess protocol.

86. **Analysis is a persistent lever with serialized automatic restarts.** It
    is enabled by default and searches every new move, loaded ZFS-FEN, history
    position, and depth selection to exactly that depth. Turning it off cancels
    search and leaves it paused. Restart requests are serialized by generation,
    preventing a cancelled worker and its replacement from racing for the one
    engine slot. Board input remains available during analysis because worker
    cancellation is independent of the UI thread.

87. **The spherical cylinder mark is one asset at every brand scale.** The
    landing mark, header/home control, and favicon use the same circular SVG and
    the original typographic double arrow. Board-edge seam arrows remain plain
    coordinate cues rather than being confused with clickable brand controls.

88. **Fixed-depth WASM search is deterministic given identical search state.**
    Seven sequential depth-10 searches with the same 32 MiB TT produced
    identical best moves, scores, node counts, and PVs in native C++ and WASM.
    The former server used 64 MiB, while browser cancellation destroys and
    recreates the 32 MiB worker table. Those lifecycle differences can alter a
    selective search even at fixed depth: in a 22-position comparison, retaining
    versus clearing the same-sized TT selected `g4a3` versus `g4e3` in one tied
    0.00 position. This is cache-history sensitivity, not randomness. Doubling
    browser memory or clearing every foreground search would not make the two
    lifecycles equivalent and would impose a real memory or strength cost, so
    neither is disguised as a determinism fix.

89. **Resignation is a game adjudication, not a fabricated board terminal.**
    The C++ position remains the exact replayable last position. Play records
    the human as the losing side, stops foreground and ponder work, disables
    further moves, gives the ordinary terminal treatment, and persists an
    immutable `resignation` result. A confirmation prevents a single accidental
    click from ending the game. Replays therefore remain legal move sequences
    without teaching the rules core an event that is not a chess position.

90. **The analysis PV uses the established Lichess-style best-move brush.** The
    arrow is Chessground's `paleBlue` brush—the same best-line treatment used in
    the Bachelor and InstinctaZero interfaces—not a custom approximation and not
    the opaque green annotation brush. This keeps Chessground responsible for
    geometry, scaling, and orientation while producing the intended muted
    blue-grey arrow.

91. **Cloudflare serves computation as static assets and storage as one narrow
    Function.** Rules and search remain entirely in the two browser WebAssembly
    payloads. Pages Functions are invoked only for `/api/*`; all HTML, CSS,
    JavaScript, SVG, and embedded WASM requests stay on the static asset path.
    The existing game list/detail contract is preserved, so Play, Games, and
    Analysis do not contain a Cloudflare-specific storage branch.

92. **Hosted saved games use D1 with the same append-forward invariant.** A
    versioned migration creates the bounded schema. Shared validation defines
    IDs, FEN/move limits, results, pagination, immutable completed games, and
    prefix-only updates for both the local JSON store and D1. D1 updates use a
    revision compare-and-swap so concurrent requests cannot silently replace a
    record derived from stale state. Queries are parameterized, JSON bodies are
    streamed through a 64 KiB limit, and unexpected database errors are not
    returned to clients. The 5,000-game ceiling remains a deliberate prototype
    bound rather than an unbounded public write surface.

93. **Cloudflare tooling is pinned and account configuration is explicit.** The
    viewer pins Wrangler 4.127.1 and Node 22, keeps the Wrangler JSON file as the
    deployment source of truth, and applies a checked-in D1 migration before
    public verification. Local Node serving remains available for development
    and keeps its existing JSON database; deployment does not make the laptop an
    origin server. Cloudflare OAuth credentials and account resources are never
    committed.

94. **Saved-game detail URLs are static rewrites, not an accidental SPA
    fallback.** Cloudflare Pages has one physical Games document, while game IDs
    are client-side detail routes. Checked-in `_redirects` proxy rules serve
    that document for the one-segment `/games/:id` forms; the existing ID parser
    then loads the public D1 record. The route remains outside Pages Functions,
    and the intentionally public archive and API policy are unchanged.

95. **Move-history visibility never scrolls the page.** Rebuilding a move list
    previously called `scrollIntoView` for its current move. On the stacked
    mobile layout, the history is below the board, so each move—and every
    iterative analysis update—panned the visual viewport toward it. History now
    adjusts only its own `scrollTop`. The analysis switch's transparent input is
    also constrained to one pixel rather than inheriting the global 100-percent
    input width, eliminating an unrelated horizontal overflow without changing
    the responsive board calculation or keyboard focus behavior.

96. **Fresh Play and Analysis sessions default to depth 9.** The selected HTML
    options, Play's state initialization, and the legacy local-server fallback
    all agree on 9. Depth remains a user selection; 8, 10, 12, and Analysis 14
    remain available. Saved games continue recording the actually selected
    depth, so no historical data is rewritten. This supersedes the defaults in
    decisions 48 and 77, not their search-lifecycle decisions.

97. **Play's `g6` and Analysis's `Nc6` after `Nf3` are cache history, not
    different positions or nondeterminism.** With an explicitly cleared 32 MiB
    WASM table, depth 10 returns `g7g6`, -398 cp, and 3,318,320 nodes. Searching
    the start position first and retaining its table returns `b8c6`, the same
    -398 cp, and 4,273,350 nodes. Play's first reply starts a new game and clears
    the table; Analysis normally retains the completed start-position analysis.
    Clearing Analysis on every move would throw away valid TT reuse merely to
    force a cosmetic tie-break, so the two equal-score choices remain. Given
    identical table state, native and WASM searches remain deterministic as in
    decision 88.

98. **A completed exact root mate ends iterative deepening at once.** A mate
    score after a completed full root iteration proves the game-theoretic result;
    later depths can only refine mate distance. Fixed-depth, time, node, and UCI
    searches therefore return that move immediately instead of continuing to
    the nominal depth. No centipawn threshold receives the same treatment:
    positions measured around +29 to +31 pawns still ranged from 0.6 to 5.6
    million depth-10 nodes, but a large evaluation is not a proof and cutting it
    off would be a strength change. The pending reported position can be
    profiled without weakening this boundary.

99. **The reported `g6` position is a hard win, not a proved mate.** The
    screenshot and seven-field ZFS-FEN agree piece for piece. Black is not in
    check; `g6` is retained as the follow field, but none of Black's 36 legal
    moves reaches it, so follow is inactive and the rules remain unchanged. A
    fresh 32 MiB table reports ordinary centipawn scores at every completed
    iteration through depth 10, ending at +4012 for Black after 22,220,125
    nodes. Depths 9 and 10 contribute 5,301,847 and 12,341,881 new nodes,
    respectively—79.4 percent of all work—rather than time being spent refining
    a known mate distance. The previous and mate-stopping searches therefore do
    identical work on this position. A large static/search evaluation is not a
    proof, so no score-based early exit is added; the exact fixture instead
    guards that inactive follow remains ordinary move selection and that a
    non-mate depth-10 search completes all requested iterations.

100. **Selective mate scores are verified at full width before stopping.**
    Decision 98 called a completed root score exact, but the tree beneath that
    root may contain null-move pruning and late-move reductions. A mate score
    from that selective tree is therefore a claim, not yet a proof. Kugelfisch
    now repeats that depth with both heuristics disabled and a separate TT score
    domain; only a completed mate result from this verification stops iterative
    deepening. The verification shares the caller's node, time, and stop limits,
    and an interrupted attempt returns the last completed primary result.
    Restricted `go searchmoves` roots also no longer write an exact score for a
    partial move set into the ordinary TT domain. The corrected depth-nine
    corpus searches 2,049,525 nodes rather than 1,806,539 (+13.45%) because two
    apparent mates now receive real proofs; this is the measured cost of
    restoring soundness, not a strength experiment. This decision supersedes
    the proof premise of decision 98 while retaining its early return after an
    actual proof.
