# Autoresearch results

ZFS-0 is commit `ddbcfad87b479c948ca32b3eeb44e82a8b8cd76d` and is
defined as 0 Elo. Point estimates below are measurements, not additive ratings.
The JSONL ledger is authoritative.

| Candidate | Decision | Depth-9 nodes | Depth-9 time | Paired games |
|---|---|---:|---:|---:|
| ZFS-0 | baseline | 11,425,195 | 3,421 ms | — |
| `null-verified-r2` | rejected | 5,349,677 | 1,786 ms | 256 |
| `null-domain-isolated-r2` | accepted | 5,353,229 | 1,779 ms | 256 |
| `history-gravity-32` | rejected | 5,493,351 | 1,838 ms | 64 |
| `history-malus-1x` | rejected | 5,488,808 | 1,850 ms | 64 |
| `lmr-r1-zfs-guarded` | rejected | 4,049,790 | 1,409 ms | 64 |
| `lmr-r1-late7-d5` | accepted | 4,201,814 | 1,448 ms | 128 |
| `qsearch-exact-tactical-gen` | accepted | 4,201,814 | 1,112 ms | 128 |
| bishop 360 | deferred | 4,199,466 | 1,115 ms | 96 |
| bishop 390 | rejected | — | — | 32 |
| `qsearch-tt-r1` | rejected | 3,274,584 | 945 ms | 128 |
| `follow-countermove-r1` | rejected | 4,125,427 | 1,083 ms | 128 |
| unique-follow choice depth | rejected | 105,670,494 | 24,756 ms | 0 |
| depth-1 quiet futility r1 | rejected | 4,154,772 | 1,114 ms | 0 |
| `depth1-quiet-upper-r2` | rejected | 3,912,489 | 1,099 ms | 64 |

## `null-verified-r2`: rejected

The first null-move implementation was not mergeable even though its initial
measurements were promising. Blind review found that an irreversible move in a
synthetic subtree reset the repetition-context accumulator and erased the null
marker. A repetition-disabled synthetic bound could consequently collide with
a real TT entry. The experiment is retained in the ledger and was rejected;
none of its game results were transferred to the corrected candidate.

## `null-domain-isolated-r2`: accepted

The corrected candidate makes the synthetic state an explicit, permanent TT
score domain, increments and restores the synthetic rule-50/fullmove clocks,
and tests that null searches, verifications, and cutoffs actually execute. Null
is attempted only in a null-window node of depth at least five that is not in
check, is not follow-forced, has non-pawn material, is outside mate-score and
near-50-move regions, and has a material evaluation at least beta. Consecutive
nulls are forbidden. Every null fail-high receives a reduced verification
search. Repetition adjudication is disabled throughout the synthetic path.

Three corrected, non-overlapping paired samples produced:

| Stage | Limit | Pairs | Score | Elo point estimate | 95% interval |
|---|---:|---:|---:|---:|---:|
| screen-v2 | 10k nodes | 32 | 50.78% | +5.43 | [-18.69, +29.60] |
| screen-v2-confirm | 25k nodes | 64 | 51.17% | +8.14 | [-7.82, +24.14] |
| holdout-v1 | 30 ms | 32 | 51.56% | +10.86 | [-29.48, +51.50] |
| aggregate | mixed | 128 | 51.17% | +8.14 | [-5.92, +22.23] |

All sequential tests remained `continue`; this is not a claim of statistical
proof. Promotion is an engineering decision based on consistent positive point
estimates, a roughly 53% smaller depth-9 tree, roughly 48% lower depth-9 wall
time, full Release tests, full ASan/UBSan tests, and a clean follow-up review.
The search is deliberately selective and can differ from full-width minimax at
a fixed nominal depth.

## History updates: rejected

Two deliberately small history-table changes failed their screens. Capped
gravity (`history-gravity-32`) increased the depth-9 node count by 2.62%; adding
a minimal malus for quiet moves searched before a cutoff (`history-malus-1x`)
increased it by 2.53%. Both scored exactly 50% in their 32-pair screens and
were reverted. The current small history table does not have enough signal to
justify either extra update.

## Late-move reductions

The first guarded LMR candidate reduced quiet moves from the fourth searched
move at depth four. It cut the depth-9 tree by 24.35%, but scored -21.74 Elo in
its screen and was rejected as too aggressive.

The accepted form is intentionally narrow. At a null-window node it requires
depth at least five and at least eight legal moves, and considers only the
seventh and later searched moves. It reduces exactly one ply, excludes checks,
compulsory-follow nodes and children, captures, promotions, en passant,
castling, the TT move, and both killers. An alpha-raising reduced result is
re-searched at full depth before ordinary PVS widening.

The exact promoted source produced:

| Stage | Limit | Pairs | Score | Elo point estimate | 95% interval |
|---|---:|---:|---:|---:|---:|
| screen-v2-confirm | 30 ms | 32 | 49.22% | -5.43 | [-16.08, +5.21] |
| holdout-v2 | 30 ms | 32 | 55.47% | +38.15 | [-7.18, +84.83] |

Earlier fixed-node screens of the same reduction schedule were essentially
neutral, while an earlier timed screen was positive. None reached a sequential
test boundary. Promotion is again an engineering decision, not statistical
proof: the sealed holdout was positive, the exact candidate cuts depth-9 nodes
by 21.51% and median wall time by 18.17%, all Release and ASan/UBSan tests pass,
and blind review found no correctness or hot-path blocker. The boolean
compulsory-follow guard deliberately keeps a small amount of move-generation
duplication so it can return after the first legal follow instead of building a
complete move list.

## Champion #2 cumulative checkpoint

The frozen null-move-plus-LMR champion was measured directly against ZFS-0;
experiment point estimates are not added together. Over 64 pairs at 30 ms it
scored 51.95%, or +13.58 Elo point with a wide 95% interval of
[-11.93, +39.23]. At fixed depth nine, its tree was 63.22% smaller and its
median wall time 57.78% lower than ZFS-0. The strength interval remained
inconclusive, while the search-capacity improvement was deterministic.

## Exact quiescence tactical generation: accepted

Quiescence previously generated the complete legal move list and then discarded
ordinary quiet moves. The replacement is an exact subset generator:

- an active follow emits every legal follower;
- check emits every legal evasion;
- otherwise it emits captures and all promotions, including quiet promotions;
- when that subset is empty, it scans quiet legality only until it can
  distinguish an ordinary quiet position from stalemate.

It uses the same king-safety context as full legal generation. Differential
tests compare both APIs through 1,000 deterministic plies and focused forced
follow, inactive-follow, check, seam en-passant, promotion, mate, and stalemate
positions. Full Release and ASan/UBSan suites pass, and adversarial review found
no semantic or hot-path blocker.

Fixed-depth behavior is identical: both champion #2 and the candidate search
4,201,814 depth-9 nodes with signature `8559bb05119cc631`. Median time fell from
1,446 ms to 1,112 ms (-23.10%). Two independent 32-pair, 30 ms screens scored
50.78% (+5.43 Elo point) and 51.56% (+10.86 Elo point). Neither crossed a
sequential boundary; promotion rests primarily on the exact semantic oracle and
repeatable throughput gain, with self-play providing a compatible positive
signal.

## Cylindrical bishop value: no promotion

Raising the bishop from 330 to 360 centipawns produced a positive 16-pair
smoke but an exactly neutral independent 32-pair confirmation. Raising it
further to 390 was weaker on the same smoke and increased the depth-8 tree by
4.45%. The geometric prior that cylinder bishops are worth more remains
plausible, but the measurements are neither monotonic nor strong enough to
change the evaluator. The conventional 330 value remains in the champion.

## Quiescence TT: rejected

A separately keyed qsearch TT encoded the synthetic-null domain and remaining
q-ply budget, and probed only after repetition, terminal, rule-50, and horizon
checks. Full lower/upper/exact bounds reduced depth-9 nodes by 22.06% and time
by about 15.7%, but scored -10.86 and -5.43 Elo point in independent 32-pair
screens (roughly -8.15 aggregate). The shared table's shallow bounds saved
work but consistently cost strength.

An exact-bound-only salvage saved just 550 of 4,201,814 nodes (0.013%) and was
slower, so it was rejected before self-play. Both implementations were removed;
the useful qsearch optimization remains exact tactical move generation without
TT storage.

## Follow countermove ordering: rejected

A compact 64-by-64 reply table remembered a quiet cutoff move by the preceding
move's origin and destination. It was disabled in synthetic-null subtrees and
ranked below both killers. The two independent screens scored 50.00% and
48.44% (-10.86 Elo point in the confirmation). Its 1.82% node reduction and
roughly 3.5% time reduction did not justify the negative aggregate signal or
the additional 16 KiB table, so the implementation was removed.

## Unique-follow choice depth: rejected at the benchmark gate

Treating a unique compulsory follow as consuming no search depth is attractive
in principle and was made deterministic from position state, preserving TT
depth meaning. In practice, forced-follow chains are common enough that the
depth-9 corpus exploded from 4,201,814 to 105,670,494 nodes (25.15 times) and
from about 1.11 seconds to 24.76 seconds. No self-play resources were spent on
an unusable unbudgeted extension. A path budget would require its own TT state
domain and was not smuggled in as an arbitrary follow-up.

## Depth-1 quiet futility: rejected

The conservative first schedule applied only from the seventh move at a
depth-one null-window node, required a one-pawn material margin, and excluded
pawns, kings, castling, TT moves, killers, checks, compulsory follows,
terminal positions, repetitions, and rule-50 draws. It reduced nodes by only
1.12% with no useful time gain and therefore failed its benchmark gate without
self-play.

A broader second form used a defensible horizon upper bound. After a
material-preserving quiet move, an ordinary child may stand pat in qsearch, so
the parent branch cannot exceed its material score; taking the maximum with
zero also covers immediate draws. Exact check and legal-follow guards retained
that property, and only TT upper bounds could contain the shortcut value. It
reduced nodes by 6.89% and median time by 2.01%, but scored 47.66% (-16.30 Elo
point) over 32 pairs, below the predeclared rejection line. Adversarial review
also found that unconditional shallow check/follow work consumed most of the
nominal node saving and that the focused tests were too weak to justify any
wider schedule. Both forms were removed.

## Champion #3 final checkpoint

The frozen champion was finally measured directly against ZFS-0 over 32 pairs
at 30 ms. It scored 51.56%, or +10.86 Elo point with a wide 95% interval of
[-15.35, +37.20]; the sequential result remained inconclusive. The deterministic
capacity result is much stronger: depth-9 nodes fell from 11,425,195 to
4,201,814 (-63.22%), while interleaved median time fell from 3,416 ms to
1,114 ms (-67.39%). The accepted engine core remains guarded null move,
conservative LMR, exact tactical qsearch generation, and conventional material
evaluation—nothing from the rejected experiments remains in the source.
