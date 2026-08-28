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
