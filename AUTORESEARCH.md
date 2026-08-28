# Engine autoresearch

The committed engine at `ddbcfad87b479c948ca32b3eeb44e82a8b8cd76d` is the
permanent **ZFS-0** reference and is defined as 0 Elo. Research changes are
measured against a separately built binary from that exact revision, never
against a moving previous candidate.

## Research contract

1. Correctness tests run before measurement. A failure rejects the experiment.
2. Fixed-depth benchmark runs are interleaved baseline/candidate on one pinned
   core. Nodes measure search work; median time measures implementation cost.
3. Routine tuning uses only `openings/screen-v2.txt`. The holdout is reserved
   for a stack that has already earned promotion.
4. Strength uses fixed-node or fixed-movetime, color-swapped pairs with the ZFS-0
   match referee.
   The controller verifies its binary and the opening book against
   `autoresearch/baseline.json`, so candidate code cannot redefine legality or
   adjudication. No NPS or node-count result is called Elo.
5. Every measured, failed, accepted, rejected, or deferred experiment is
   appended to `autoresearch/ledger.jsonl`; raw game logs remain in the ignored
   `build-autoresearch-results/` directory.
6. A single experiment should change one idea or a tightly coupled parameter
   family. Complexity without a measured benefit is reverted.
7. Search selectivity must retain explicit ZFS guards. In particular, a quiet
   mandatory follow is not an ordinary quiet move and cannot be pruned merely
   because orthodox chess would prune it.
8. Runtime remains one search worker. Autoresearch itself pins one core and does
   not run competing matches concurrently.

## Baseline characterization

Host-native GCC 13.3 build, pinned to CPU 12:

| Metric | ZFS-0 |
|---|---:|
| Depth-9 corpus nodes | 11,425,195 |
| Depth-9 behavior signature | `a4cd51d6f3fc232d` |
| Five-run median time | 3,421 ms |
| Five-run median NPS | 3,339,723 |

A `-pg` depth-9 profile attributed roughly 34% self time to legal generation,
15% to `do_move`, and 11% to pseudo-legal generation. Quiescence dominated the
call graph and invoked material evaluation about 8.1 million times, but the six
hardware-popcount material evaluator remained below sampling resolution. This
makes selective search and reduced legal-generation work higher-priority than
incremental material bookkeeping.

## Running one experiment

Keep `build-autoresearch-baseline` frozen. With the candidate source in the
working tree:

```sh
./tools/autoresearch.py run --name null-r2 --core 12
./tools/autoresearch.py run --name null-r2-time --core 12 \
  --movetime-ms 100 --pairs 32
./tools/autoresearch.py decide --name null-r2 --decision accept \
  --reason "passed confirmation threshold"
```

The controller configures and tests `build-autoresearch-candidate`, alternates
benchmark order, runs paired self-play, saves the raw JSONL match, and appends a
machine-readable ledger event. It never edits source or automatically declares
a winner. After the first promotion, `--reference-build` can point at a frozen
champion artifact while `--baseline-build` continues to provide the immutable
ZFS-0 referee. Direct champion-versus-ZFS-0 checks report cumulative gain.

Thirty-millisecond matches are only rejection screens for evaluation work.
Promising terms require an independent 100 ms sample before promotion: a
fourfold persistent-shadow penalty scored 63.28% at 30 ms and then 48.44% at
100 ms, exposing a shallow-horizon patch rather than durable strength. Fixed
nodes remain useful for controlled search-quality comparisons, while fixed-depth
benchmarks separate changed tree shape from the evaluator's execution cost.

`screen-v2` contains 256 independently generated, color-swapped training lines.
`screen-v2-confirm` is a separately seeded 128-line training-validation pool.
The original 32-line `screen-v1` remains frozen for reproducing early results;
`holdout-v1` and the later 128-line `holdout-v2` remain excluded from routine
tuning. A candidate lineage never reuses an already exposed holdout.
