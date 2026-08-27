# Zylinderfolgeschach rules

This document is the normative rules specification used by this repository.
Anything not changed below follows orthodox FIDE chess.

## 1. Board and coordinates

The usual 64 squares and the usual piece placement are used. Ranks still end at
rank 1 and rank 8. Files are cyclic: moving one file right from `h` reaches `a`,
and moving one file left from `a` reaches `h`.

Equivalently, for movement calculations the file coordinate is taken modulo 8,
while the rank coordinate is not.

## 2. Piece movement on the cylinder

- A king moves one rank and/or one cyclic file, as usual. Thus `a4` and `h4`
  are adjacent. A king may not move to an attacked square.
- A knight uses the usual `(1, 2)` displacement with its file coordinate taken
  modulo 8. Its rank coordinate must remain on the board.
- A rook's vertical rays are unchanged. Each horizontal ray proceeds around the
  cyclic rank, stops at the first occupied square (including that square as a
  capture when it is occupied by an enemy), and never visits the rook's origin.
  A ray therefore contains at most the other seven squares of the rank.
- A bishop changes rank by one and cyclic file by one at every step. A ray ends
  at rank 1 or rank 8, or at its first occupied square. For example, the empty
  ray `f1-g2-h3-a4-b5-c6-d7-e8` is continuous.
- A queen combines cylindrical rook and bishop movement.
- Pawns advance by rank exactly as in orthodox chess. Pawn capture files are
  cyclic, so a pawn on the `a`-file can capture on the `h`-file and vice versa.
  Initial double moves, promotion, and en passant otherwise work normally.

Pieces may not jump over occupied squares except for knights and the pieces
participating in castling under the normal castling rule.

The move is identified by its origin and destination, not by the route used to
reach the destination. If two cylindrical rays reach the same destination (as
can happen around a rank or at a bishop's antipodal file), the move exists once
and is available when at least one route is clear.

## 3. Attacks, check, and ordinary legality

An attack is determined solely by the board geometry, occupancy, piece movement,
and pawn attack direction. The follow rule in section 4 is ignored when deciding
whether a square is attacked. As in orthodox chess, a piece still attacks a
square when moving that piece would expose its own king.

Consequently, a king may not enter or remain in check even when the attacking
side would be obliged to follow somewhere else on its next turn.

An *ordinarily legal move* is a move legal under all orthodox rules, with the
cylindrical geometry above, that leaves the moving side's king unattacked.

## 4. The follow obligation

Every non-castling move creates a *follow field* for the opponent. It is the
origin square of the moved piece. Castling creates no active follow field; the
proof is given below.

Let `L` be the set of all ordinarily legal moves in the current position.
Let `F` be the moves in `L` whose primary moving piece lands on the current
follow field. Castling's primary moving piece is the king.

- If `F` is nonempty, exactly the moves in `F` are permitted: the player must
  follow.
- If `F` is empty, every move in `L` is permitted.

This test is based on legal moves, not merely geometric or pseudo-legal moves. A
pinned piece that cannot legally reach the follow field does not create an
obligation. When the king is in check, only check-resolving moves can belong to
`L`; following is mandatory only when at least one such move reaches the follow
field.

Castling is treated as the king's move for an incoming follow obligation. It
satisfies that obligation only when the king lands on the follow field; the
rook's transferred landing square does not count as a second move destination.

After castling, following to the king's origin is impossible. A non-pawn that
could land there after castling also attacked the occupied origin before
castling. Moving the rook cannot create an exception: it adds a blocker at
`f1`/`f8` or `d1`/`d8`, and vacating `h1`/`h8` or `a1`/`a8` cannot expose the
origin without the relocated king or rook blocking the same horizontal route. A
pawn push is the only non-attacking way to land on that origin: a black pawn on
`e2` could promote on `e1`, or a white pawn on `e7` could promote on `e8`. Such a
pawn attacks both castling transit squares (`d1` and `f1`, or `d8` and `f8`), so
neither castle would have been legal. The engine therefore canonicalizes this
inert state as no follow field. En passant and promotion create the moving
pawn's origin as the new follow field in the usual way.

The initial position has no follow field. A legally reached follow field is
empty because it is the square just vacated by the preceding move.

## 5. Castling

Castling retains its orthodox coordinate meaning; the cylindrical seam does not
create additional castling moves.

- White castles king-side `e1-g1` with `h1-f1`, or queen-side `e1-c1` with
  `a1-d1`.
- Black castles king-side `e8-g8` with `h8-f8`, or queen-side `e8-c8` with
  `a8-d8`.

The usual castling rights, empty-square requirements, and prohibition on the
king starting in, passing through, or ending in check apply. Attacks for those
tests use cylindrical geometry and ignore follow obligations.

## 6. En passant and promotion

En passant is available immediately after an opposing pawn's two-square move,
including across the `a`/`h` seam. It is legal only if removing the captured pawn
and moving the capturing pawn leaves the capturing side's king unattacked. Its
landing square is used when testing whether the move satisfies a follow
obligation; its origin becomes the next follow field.

A pawn reaching the back rank must promote to queen, rook, bishop, or knight.
The promotion move's landing square is used for the current follow test and the
pawn's origin becomes the next follow field.

## 7. End of the game

Checkmate, stalemate, resignation, agreed draws, and dead positions retain their
orthodox meanings, using the legal move set defined above. This implementation
uses simpler automatic move-count and repetition rules instead of draw claims:

- checkmate is a checked side with no permitted move;
- stalemate is an unchecked side with no permitted move;
- the third occurrence of the same position ends the game as a draw;
- 100 consecutive halfmoves without a pawn move or capture end the game as a
  draw.

There is no separate claim action, fivefold rule, or 75-move rule. Checkmate on
the move that reaches the move-count threshold takes precedence. Treating a
second occurrence as a draw is permitted only as an engine search heuristic; it
is not a game rule.

Because the fallback in section 4 uses all of `L` whenever `F` is empty, a
position has no permitted move exactly when it has no ordinarily legal move.

For repetition, two positions cannot be considered the same when their follow
state gives them different permitted move sets. A game-history layer must also
track castling rights, en-passant availability, the side to move, and the follow
state; these are not recoverable from the piece placement alone.

## 8. Position notation used by the engine

The engine's canonical ZFS-FEN is ordinary six-field FEN followed by a seventh
field containing the follow square or `-`. For example, the initial position is:

```text
rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1 -
```

For interoperability, the parser also accepts six-field FEN. It normally treats
the omitted follow field as absent. When the en-passant field is present, the
preceding double-push origin is uniquely known, so that origin is inferred as
the follow field. For example, after `e2-e4`, the en-passant and follow fields
are `e3` and `e2`, respectively. Six-field FEN remains lossy after other moves;
use seven fields whenever an explicit ZFS position is exchanged.
