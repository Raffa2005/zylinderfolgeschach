# Evaluating engine changes

ZFS uses three separate checks. A change must first remain correct, then avoid a
performance regression, and only then be judged for playing strength. A faster
wrong engine is not an optimization, and NPS alone is not a strength result.

The suite is deliberately local and single-worker. It has no coordinator,
database, dashboard, opening adjudicator, or distributed job protocol.

## 1. Correctness

Run the normal tests before any comparison:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DZFS_NATIVE=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

The match runner is an independent referee at the process boundary: engines
only propose UCI moves. The production `Game` validates every move and alone
decides checkmate, stalemate, automatic threefold, and automatic 50-move draws.
An illegal move, crash, or timeout loses. There is no eval-based resignation or
draw adjudication. Reaching the configurable test ply cap is recorded as a draw
with the explicit `ply-cap` reason. Runner resource failures abort the match;
they are never converted into a forfeit for whichever engine happened to start
first.

## 2. Deterministic work benchmark

`zfs_bench` searches a fixed twelve-position ZFS corpus. Each position gets a
fresh TT. Nodes, best moves, scores, and PVs feed a deterministic signature;
wall time and NPS do not. Run before and after on the same otherwise-idle host:

```sh
./build/zfs_bench
./build/zfs_bench --depth 7
./build/zfs_bench --verify-signature 0123456789abcdef
```

Compare total nodes and time, not just NPS. An exact-search optimization should
normally preserve score and best move, but a changed node count or signature is
not automatically a bug: move ordering and any deliberate search-policy change
can alter both. The signature exists to make that change visible.

## 3. Openings

An opening file contains one legal start-position UCI move line per pair. This
preserves the complete follow and repetition history, which a bare FEN cannot.
Comments begin with `#`; the literal `startpos` represents an empty line.

The committed suites are:

- `openings/screen-v1.txt`: routine development screening.
- `openings/holdout-v1.txt`: a separate confirmation set; do not tune against it
  after every edit.
- `openings/holdout-v3.txt`: the sealed 128-opening holdout for the third
  research cycle. It was generated and fingerprinted before that cycle's
  candidates and must not be used for tuning or routine screens.

They are small deterministic bootstrap books, not claims about optimal ZFS
opening play. The generator samples legal lines, then rejects terminal,
in-check, materially unbalanced, and duplicate final ZFS states. Generate a
larger versioned book when needed:

```sh
./build/zfs_openings --count 256 --min-plies 8 --max-plies 16 \
  --seed 123456 --output openings/screen-v2.txt
```

Never silently replace a versioned suite during an experiment.

## 4. Paired self-play

Build the candidate and baseline as separate binaries, preferably from clean
commits with the same compiler flags. Each opening is played twice with colors
swapped. The candidate therefore scores 0 through 4 half-points per pair,
forming the pentanomial bins `LL, LD, LW/DD/WL, WD, WW`.

Fixed nodes are best for quick, repeatable search-efficiency screening:

```sh
./build/zfs_match \
  --candidate /path/to/candidate/zfs_engine --candidate-id NEW_COMMIT \
  --baseline /path/to/baseline/zfs_engine --baseline-id BASE_COMMIT \
  --openings openings/screen-v1.txt --output run.jsonl \
  --nodes 25000 --pairs 32
```

Fixed movetime tests the complete time-management path but is noisier:

```sh
./build/zfs_match \
  --candidate /path/to/candidate/zfs_engine \
  --baseline /path/to/baseline/zfs_engine \
  --openings openings/holdout-v1.txt --output confirm.jsonl \
  --movetime-ms 100 --timeout-ms 5000
```

The runner is sequential. Search itself and the rules core remain single-
threaded. It starts clean engine processes—and therefore an empty hash—for
every game, and gives both sides the same move limit. It sets the requested
hash size when an engine advertises the standard UCI `Hash` option; otherwise
the manifest records `hash_option: false` and that engine keeps its own default.
Startup is outside the move budget.

The JSONL log is append-only. Its checksummed manifest records the match-runner
and engine fingerprints, canonical binary paths, optional commit IDs, UCI
names, opening fingerprint, limits, and test hypotheses. It refuses to
overwrite an existing file. Continue an interrupted run with the identical
command plus `--resume`; a configuration or binary mismatch is rejected. Both
games and their pair result are one JSONL record. Incomplete final-record bytes
are discarded and that pair is replayed; only complete pair records enter the
statistics. Unknown or corrupt complete records are rejected. The opening file
is read and
fingerprinted from the same in-memory bytes. Engine contents are fingerprinted
once and file identity is checked around every launch and before each pair is
committed; replacing either binary aborts the run while preserving normal
executable-relative resource and shared-library lookup.

Do not rename or replace the result path while a match is running. The advisory
lock coordinates cooperating runners that open the same file; it is not a
sandbox against same-user path replacement or log rotation.

The executable fingerprint cannot discover arbitrary runtime inputs. If an
engine loads NNUE files, shared libraries, configuration, or other mutable
assets, use `--candidate-id`/`--baseline-id` as immutable deployment-bundle IDs
and keep the environment fixed. The runner records and binds those IDs but does
not pretend to be a package manager or sandbox.

## 5. Statistics and stopping

The runner reports the five paired outcome counts, score, and a logistic-Elo
generalized sequential probability ratio test (GSPRT). Defaults are
`H0 = 0 Elo`, `H1 = +5 Elo`, and five-percent type-I/type-II error bounds. It
stops only after a complete color-swapped pair crosses a bound or the requested
pair limit is reached. The likelihood calculation uses the same constrained
multinomial pentanomial model and zero-bin regularization as Stockfish Fishtest.

To inspect counts independently:

```sh
./build/zfs_stats 12 34 56 78 90 --elo0 0 --elo1 5
```

Logistic Elo is reported by name. The 95% interval and likelihood of superiority
are approximate paired-sample summaries and print `n/a` with fewer than two
pairs or zero observed variance; the tool does not manufacture certainty from a
degenerate sample. The GSPRT bounds decide a sequential test. A run that says
`continue` is inconclusive, not evidence of equality.

For a normal change, use the cheap sequence: tests, benchmark, a short screen,
then the holdout only if the result is promising. Long runs are warranted only
when the expected gain is too small for a short screen—not as a ritual for every
edit.

The statistical formulation is based on Stockfish Fishtest's
[`LLRcalc.py`](https://github.com/official-stockfish/fishtest/blob/master/server/fishtest/stats/LLRcalc.py)
and [`sprt.py`](https://github.com/official-stockfish/fishtest/blob/master/server/fishtest/stats/sprt.py).
