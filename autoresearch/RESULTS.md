# Autoresearch results

ZFS-0 is commit `ddbcfad87b479c948ca32b3eeb44e82a8b8cd76d` and is
defined as 0 Elo. Point estimates below are measurements, not additive ratings.
The JSONL ledger is authoritative.

| Candidate | Decision | Depth-9 nodes | Depth-9 time | Paired games |
|---|---|---:|---:|---:|
| ZFS-0 | baseline | 11,425,195 | 3,421 ms | — |
| `null-verified-r2` | rejected | 5,349,677 | 1,786 ms | 256 |
| `null-domain-isolated-r2` | accepted | 5,353,229 | 1,779 ms | 256 |

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
