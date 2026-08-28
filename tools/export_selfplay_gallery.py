#!/usr/bin/env python3
"""Export a zfs_match JSONL log as a browser-safe self-play archive."""

from __future__ import annotations

import argparse
import json
import re
from collections import Counter
from pathlib import Path


MOVE = re.compile(r"^[a-h][1-8][a-h][1-8][qrbn]?$")
RESULTS = {"1-0", "0-1", "1/2-1/2"}
COLORS = {"white", "black"}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, help="zfs_match JSONL file")
    parser.add_argument("output", type=Path, help="archive JSON file")
    parser.add_argument("--candidate-name", required=True)
    parser.add_argument("--baseline-name", required=True)
    parser.add_argument("--title", required=True)
    parser.add_argument("--description", required=True)
    return parser.parse_args()


def candidate_outcome(result: str, color: str) -> str:
    if result == "1/2-1/2":
        return "draw"
    winner = "white" if result == "1-0" else "black"
    return "win" if color == winner else "loss"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def main() -> int:
    args = parse_args()
    with args.input.open(encoding="utf-8") as stream:
        records = [json.loads(line) for line in stream if line.strip()]
    require(records and records[0].get("type") == "manifest",
            "input has no match manifest")
    manifest = records[0]
    require(manifest.get("schema") == 2, "unsupported match schema")

    games: list[dict[str, object]] = []
    expected_pair = 0
    for record in records[1:]:
        require(record.get("type") == "pair", "unexpected match record")
        require(record.get("pair") == expected_pair, "non-contiguous pair records")
        pair_games = record.get("games")
        require(isinstance(pair_games, list) and len(pair_games) == 2,
                "pair does not contain two games")
        for game in pair_games:
            number = game.get("game")
            color = game.get("candidate_color")
            result = game.get("result")
            moves = game.get("moves")
            plies = game.get("plies")
            require(number in (1, 2), "invalid game number")
            require(color in COLORS, "invalid candidate color")
            require(result in RESULTS, "invalid game result")
            require(isinstance(moves, list) and all(
                isinstance(move, str) and MOVE.fullmatch(move) for move in moves
            ), "invalid move list")
            require(plies == len(moves), "ply count does not match move list")
            white = args.candidate_name if color == "white" else args.baseline_name
            black = args.candidate_name if color == "black" else args.baseline_name
            games.append({
                "id": f"pair-{expected_pair + 1}-game-{number}",
                "pair": expected_pair + 1,
                "game": number,
                "openingLine": record.get("opening_line"),
                "white": white,
                "black": black,
                "candidateColor": color,
                "candidateOutcome": candidate_outcome(result, color),
                "result": result,
                "termination": game.get("termination"),
                "plies": plies,
                "candidateNodes": game.get("candidate_nodes"),
                "baselineNodes": game.get("baseline_nodes"),
                "candidateTimeMs": game.get("candidate_time_ms"),
                "baselineTimeMs": game.get("baseline_time_ms"),
                "moves": moves,
            })
        expected_pair += 1

    require(games, "match contains no games")
    terminations = Counter(str(game["termination"]) for game in games)
    results = Counter(str(game["result"]) for game in games)
    outcomes = Counter(str(game["candidateOutcome"]) for game in games)
    candidate_points = outcomes["win"] + outcomes["draw"] / 2
    limit = manifest.get("limit", {})
    archive = {
        "schema": 1,
        "title": args.title,
        "description": args.description,
        "source": args.input.name,
        "createdUtc": manifest.get("created_utc"),
        "candidate": args.candidate_name,
        "baseline": args.baseline_name,
        "limit": {
            "movetimeMs": limit.get("movetime_ms"),
            "nodes": limit.get("nodes"),
            "hashMb": limit.get("hash_mb"),
            "maxPlies": limit.get("max_plies"),
        },
        "summary": {
            "pairs": expected_pair,
            "games": len(games),
            "candidatePoints": candidate_points,
            "candidateScore": candidate_points / len(games),
            "candidateOutcomes": dict(sorted(outcomes.items())),
            "results": dict(sorted(results.items())),
            "terminations": dict(sorted(terminations.items())),
        },
        "games": games,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", encoding="utf-8") as stream:
        json.dump(archive, stream, indent=2, ensure_ascii=False)
        stream.write("\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
