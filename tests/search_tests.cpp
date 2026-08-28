#include "engine/search.hpp"
#include "engine/evaluate.hpp"
#include "engine/tt.hpp"
#include "zfs/game.hpp"
#include "zfs/position.hpp"

#include <atomic>
#include <algorithm>
#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

int failures = 0;

#define CHECK(expression)                                                        \
    do {                                                                         \
        if (!(expression)) {                                                     \
            std::cerr << __FILE__ << ':' << __LINE__ << ": CHECK failed: "      \
                      << #expression << '\n';                                    \
            ++failures;                                                          \
        }                                                                        \
    } while (false)

zfs::Position load(std::string_view fen) {
    std::string error;
    const auto position = zfs::Position::from_fen(fen, &error);
    if (!position) {
        std::cerr << "could not load search ZFS-FEN: " << fen << " (" << error
                  << ")\n";
        std::abort();
    }
    return *position;
}

zfs::engine::SearchResult search(zfs::Game game, int depth) {
    zfs::engine::TranspositionTable table(1);
    zfs::engine::Searcher searcher(table);
    zfs::engine::SearchLimits limits;
    limits.depth = depth;
    const std::atomic_bool stop{false};
    return searcher.search(game, limits, {}, stop);
}

int reference_score(zfs::Position& position, int depth, int ply) {
    zfs::MoveList moves;
    position.generate_legal_moves(moves);
    if (moves.empty()) {
        return position.in_check(position.side_to_move())
                   ? -zfs::engine::kMateScore + ply
                   : 0;
    }
    if (position.halfmove_clock() >= 100U) {
        return 0;
    }
    if (depth == 0) {
        return zfs::engine::evaluate(position);
    }

    int best = -32000;
    for (zfs::Move move : moves) {
        zfs::Undo undo;
        position.do_move(move, undo);
        best = std::max(best, -reference_score(position, depth - 1, ply + 1));
        position.undo_move(move, undo);
    }
    return best;
}

void compare_with_reference(std::string_view fen, int depth) {
    zfs::Game game(load(fen));
    zfs::Position position = game.position();
    zfs::MoveList moves;
    position.generate_legal_moves(moves);
    int expected = -32000;
    std::vector<zfs::Move> best_moves;
    for (zfs::Move move : moves) {
        zfs::Undo undo;
        position.do_move(move, undo);
        const int score = -reference_score(position, depth - 1, 1);
        position.undo_move(move, undo);
        if (score > expected) {
            expected = score;
            best_moves.clear();
            best_moves.push_back(move);
        } else if (score == expected) {
            best_moves.push_back(move);
        }
    }

    zfs::engine::TranspositionTable table(1);
    zfs::engine::Searcher searcher(table);
    zfs::engine::SearchLimits limits;
    limits.depth = depth;
    limits.quiescence_plies = 0;
    const std::atomic_bool stop{false};
    const auto actual = searcher.search(game, limits, {}, stop);
    CHECK(actual.score == expected);
    CHECK(std::find(best_moves.begin(), best_moves.end(), actual.best_move) !=
          best_moves.end());
}

void test_mate_and_material() {
    const auto mate = search(
        zfs::Game(load("8/8/8/8/8/K7/2Q5/k7 w - - 0 1 -")), 10);
    CHECK(mate.has_move);
    CHECK(mate.best_move.uci() == "c2c1");
    CHECK(mate.score >= zfs::engine::kMateThreshold);
    CHECK(mate.depth == 1);

    const auto capture = search(
        zfs::Game(load("4k3/8/8/8/8/8/r7/R3K3 w - - 0 1 -")), 1);
    CHECK(capture.has_move);
    CHECK(capture.score > 0);
}

void test_reported_slow_position_is_not_a_proven_mate() {
    auto position = load(
        "rnbq2n1/ppppNr2/4k3/5pQ1/4PbP1/2P4p/P4PPP/2R1K2R "
        "b - - 0 1 g6");
    CHECK(position.to_fen() ==
          "rnbq2n1/ppppNr2/4k3/5pQ1/4PbP1/2P4p/P4PPP/2R1K2R "
          "b - - 0 1 g6");
    CHECK(position.side_to_move() == zfs::Color::Black);
    CHECK(position.follow_square() == zfs::parse_square("g6"));
    CHECK(!position.in_check(zfs::Color::Black));
    CHECK(!position.must_follow());

    zfs::MoveList legal;
    position.generate_legal_moves(legal);
    CHECK(legal.size() == 36U);
    CHECK(std::none_of(legal.begin(), legal.end(), [](zfs::Move move) {
        return move.to() == zfs::parse_square("g6");
    }));

    zfs::engine::TranspositionTable table(32);
    zfs::engine::Searcher searcher(table);
    zfs::engine::SearchLimits limits;
    limits.depth = 10;
    std::vector<zfs::engine::SearchInfo> iterations;
    const std::atomic_bool stop{false};
    const auto result = searcher.search(
        zfs::Game(position), limits,
        [&iterations](const zfs::engine::SearchInfo& info) {
            iterations.push_back(info);
        },
        stop);

    CHECK(result.has_move);
    CHECK(result.depth == 10);
    CHECK(iterations.size() == 10U);
    CHECK(std::all_of(iterations.begin(), iterations.end(),
                      [](const zfs::engine::SearchInfo& info) {
                          return std::abs(info.score) <
                                 zfs::engine::kMateThreshold;
                      }));
}

void test_leader_initiative() {
    const auto rook_follower = load(
        "4r1k1/8/8/8/4R3/8/8/6K1 w - - 0 1 -");
    const auto queen_follower = load(
        "4q1k1/8/8/8/4R3/8/8/6K1 w - - 0 1 -");
    const int rook_bonus = zfs::engine::evaluate(rook_follower) -
                           zfs::engine::evaluate_material(rook_follower);
    const int queen_bonus = zfs::engine::evaluate(queen_follower) -
                            zfs::engine::evaluate_material(queen_follower);
    CHECK(rook_bonus > 0);
    CHECK(queen_bonus > rook_bonus);

    const auto pawn_follower = load(
        "6k1/8/8/4p3/q3R3/8/8/6K1 w - - 0 1 -");
    const auto pawn_only = load(
        "6k1/8/8/4p3/4R3/8/8/6K1 w - - 0 1 -");
    const int pawn_and_queen_bonus =
        zfs::engine::evaluate(pawn_follower) -
        zfs::engine::evaluate_material(pawn_follower);
    const int pawn_only_bonus = zfs::engine::evaluate(pawn_only) -
                                zfs::engine::evaluate_material(pawn_only);
    CHECK(pawn_and_queen_bonus == pawn_only_bonus);

    const auto king_follows_pawn = load(
        "8/8/8/8/4P3/4k3/8/4K3 w - - 0 1 -");
    CHECK(zfs::engine::evaluate(king_follows_pawn) >
          zfs::engine::evaluate_material(king_follows_pawn));

    const auto king_cannot_follow_knight = load(
        "8/8/8/8/4N3/5k2/8/6K1 w - - 0 1 -");
    CHECK(zfs::engine::evaluate(king_cannot_follow_knight) ==
          zfs::engine::evaluate_material(king_cannot_follow_knight));

    const auto pawn_double_push = load(
        "6k1/4p3/8/4R3/8/8/8/6K1 w - - 0 1 -");
    const auto blocked_double_push = load(
        "6k1/4p3/4n3/4R3/8/8/8/6K1 w - - 0 1 -");
    CHECK(zfs::engine::evaluate(pawn_double_push) >
          zfs::engine::evaluate_material(pawn_double_push));
    CHECK(zfs::engine::evaluate(blocked_double_push) ==
          zfs::engine::evaluate_material(blocked_double_push));

    const auto forced_promotion_is_not_a_burden = load(
        "r3k3/P7/8/8/8/8/8/4K3 b - - 0 1 -");
    CHECK(zfs::engine::evaluate(forced_promotion_is_not_a_burden) ==
          zfs::engine::evaluate_material(forced_promotion_is_not_a_burden));
}

void test_automatic_draw_roots() {
    const auto fifty = search(
        zfs::Game(load("4k3/8/8/8/8/8/8/4K1N1 w - - 100 51 -")), 4);
    CHECK(!fifty.has_move);
    CHECK(fifty.score == 0);

    zfs::Game repeated(load("4k3/8/8/8/8/8/8/4K1N1 w - - 0 1 -"));
    constexpr std::string_view cycle[]{"g1f3", "e8e7", "f3g1", "e7e8"};
    for (int repeat = 0; repeat < 2; ++repeat) {
        for (std::string_view move : cycle) {
            CHECK(repeated.play_uci(move).has_value());
        }
    }
    const auto threefold = search(repeated, 4);
    CHECK(!threefold.has_move);
    CHECK(threefold.score == 0);
}

void test_limits_and_determinism() {
    zfs::Game game;
    zfs::engine::TranspositionTable table(1);
    zfs::engine::Searcher searcher(table);
    zfs::engine::SearchLimits limits;
    limits.depth = 2;
    limits.restrict_search_moves = true;
    zfs::Position root = game.position();
    limits.search_moves.push_back(*root.parse_uci("g1f3"));
    const std::atomic_bool stop{false};
    const auto restricted = searcher.search(game, limits, {}, stop);
    CHECK(restricted.has_move);
    CHECK(restricted.best_move.uci() == "g1f3");
    CHECK(restricted.depth == 2);

    const auto first = search(zfs::Game{}, 3);
    const auto second = search(zfs::Game{}, 3);
    CHECK(first.has_move && second.has_move);
    CHECK(first.best_move == second.best_move);
    CHECK(first.score == second.score);
    CHECK(first.principal_variation == second.principal_variation);
}

void test_limit_normalization_and_tt_epoch() {
    zfs::Game game;
    zfs::engine::TranspositionTable table(1);
    zfs::engine::Searcher searcher(table);
    zfs::engine::SearchLimits limits;
    limits.depth = 1;
    limits.mate = 2'000'000'000;
    limits.movetime_ms = std::numeric_limits<std::int64_t>::max();
    limits.increment_ms = {std::numeric_limits<std::int64_t>::max(),
                           std::numeric_limits<std::int64_t>::max()};
    limits.move_overhead_ms = std::numeric_limits<std::int64_t>::max();
    limits.infinite = true;
    const std::atomic_bool stop{false};
    const auto result = searcher.search(game, limits, {}, stop);
    CHECK(result.has_move);
    CHECK(result.depth == 1);

    zfs::engine::SearchLimits hostile_clock;
    hostile_clock.nodes = 1;
    hostile_clock.time_ms[0] = std::numeric_limits<std::int64_t>::max();
    hostile_clock.increment_ms[0] = std::numeric_limits<std::int64_t>::max();
    hostile_clock.move_overhead_ms =
        std::numeric_limits<std::int64_t>::min();
    CHECK(searcher.search(game, hostile_clock, {}, stop).has_move);

    zfs::engine::SearchLimits hostile_movetime;
    hostile_movetime.nodes = 1;
    hostile_movetime.movetime_ms = std::numeric_limits<std::int64_t>::max();
    CHECK(searcher.search(game, hostile_movetime, {}, stop).has_move);

    table.clear();
    table.new_search();
    constexpr std::uint64_t key = 0x123456789abcdef0ULL;
    table.store(key, result.best_move, 17, 1, zfs::engine::Bound::Exact);
    CHECK(table.probe(key).hit);
    for (unsigned search = 0; search < 63; ++search) {
        table.new_search();
    }
    CHECK(!table.probe(key).hit);
    CHECK(table.hashfull() == 0);

    bool rejected_oversized_table = false;
    try {
        table.resize(zfs::engine::TranspositionTable::kMaximumMegabytes + 1U);
    } catch (const std::length_error&) {
        rejected_oversized_table = true;
    }
    CHECK(rejected_oversized_table);
}

void test_search_path_twofold() {
    zfs::Game game(load("4k3/8/8/8/8/8/8/4K1N1 w - - 0 1 -"));
    constexpr std::string_view cycle[]{"g1f3", "e8e7", "f3g1", "e7e8"};
    for (std::string_view move : cycle) {
        CHECK(game.play_uci(move).has_value());
    }

    zfs::Position root = game.position();
    const auto repeat_move = root.parse_uci("g1f3");
    CHECK(repeat_move.has_value());
    zfs::engine::TranspositionTable table(1);
    zfs::engine::Searcher searcher(table);
    zfs::engine::SearchLimits limits;
    limits.depth = 1;
    limits.restrict_search_moves = true;
    limits.search_moves.push_back(*repeat_move);
    const std::atomic_bool stop{false};
    const auto result = searcher.search(game, limits, {}, stop);
    CHECK(result.has_move);
    CHECK(result.best_move == *repeat_move);
    CHECK(result.score == 0);
}

void test_null_move_pruning_is_exercised_and_guarded() {
    zfs::engine::TranspositionTable table(4);
    zfs::engine::Searcher searcher(table);
    zfs::engine::SearchLimits limits;
    limits.depth = 8;
    limits.quiescence_plies = 0;
    const std::atomic_bool stop{false};
    const auto exercised = searcher.search(zfs::Game{}, limits, {}, stop);
    CHECK(exercised.has_move);
    CHECK(exercised.null_searches > 0);
    CHECK(exercised.null_verifications > 0);
    CHECK(exercised.null_cutoffs > 0);
    CHECK(exercised.null_cutoffs <= exercised.null_verifications);
    CHECK(exercised.null_verifications <= exercised.null_searches);
    CHECK(exercised.lmr_searches > 0);
    CHECK(exercised.lmr_researches <= exercised.lmr_searches);

    zfs::engine::TranspositionTable full_width_table(4);
    zfs::engine::Searcher full_width_searcher(full_width_table);
    limits.late_move_reductions = false;
    const auto full_width =
        full_width_searcher.search(zfs::Game{}, limits, {}, stop);
    CHECK(full_width.has_move);
    CHECK(full_width.lmr_searches == 0);
    limits.late_move_reductions = true;

    auto old_clock = load(
        "4k3/8/8/8/8/8/8/R3K3 w - - 91 46 -");
    const auto guarded = searcher.search(zfs::Game(old_clock), limits, {}, stop);
    CHECK(guarded.has_move);
    CHECK(guarded.null_searches == 0);

    limits.nodes = 100;
    const auto aborted = searcher.search(zfs::Game{}, limits, {}, stop);
    CHECK(aborted.has_move);
    limits.nodes = 0;
    limits.depth = 2;
    const auto after_abort = searcher.search(zfs::Game{}, limits, {}, stop);
    CHECK(after_abort.has_move);
    CHECK(after_abort.depth == 2);
}

void test_against_exhaustive_minimax() {
    compare_with_reference(
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1 -",
        2);
    compare_with_reference("7k/8/8/8/8/8/8/R3K3 w - - 0 1 a3", 3);
    compare_with_reference("4k3/8/8/8/8/8/r7/R3K3 w - - 0 1 -", 2);
    compare_with_reference("4k3/8/8/8/8/8/8/4K1N1 w - - 0 1 -", 3);
}

}  // namespace

int main() {
    test_mate_and_material();
    test_reported_slow_position_is_not_a_proven_mate();
    test_leader_initiative();
    test_automatic_draw_roots();
    test_limits_and_determinism();
    test_limit_normalization_and_tt_epoch();
    test_search_path_twofold();
    test_null_move_pruning_is_exercised_and_guarded();
    test_against_exhaustive_minimax();
    if (failures != 0) {
        std::cerr << failures << " search assertion(s) failed\n";
        return 1;
    }
    std::cout << "all search tests passed\n";
}
