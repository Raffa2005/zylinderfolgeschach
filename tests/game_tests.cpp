#include "zfs/game.hpp"
#include "zfs/position.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

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
        std::cerr << "could not load test ZFS-FEN: " << fen << " (" << error
                  << ")\n";
        std::abort();
    }
    return *position;
}

void test_repetition_identity() {
    const zfs::Position plain =
        load("4k3/8/8/8/8/8/8/4K1N1 w - - 0 1 -");
    const zfs::Position inert_follow =
        load("4k3/8/8/8/8/8/8/4K1N1 w - - 17 9 e7");
    const zfs::Position active_follow =
        load("4k3/8/8/8/8/8/8/4K1N1 w - - 0 1 f3");

    CHECK(plain.raw_key() != inert_follow.raw_key());
    CHECK(plain.base_key() == inert_follow.base_key());
    CHECK(plain.same_repetition_state(inert_follow));
    CHECK(!plain.same_repetition_state(active_follow));

    const zfs::Position active_ep =
        load("4k3/8/8/p6P/8/8/8/4K3 w - a6 0 2 a7");
    const zfs::Position no_ep =
        load("4k3/8/8/p6P/8/8/8/4K3 w - - 0 2 a7");
    CHECK(active_ep.base_key() == no_ep.base_key());
    CHECK(!active_ep.same_repetition_state(no_ep));

    const zfs::Position pinned_ep =
        load("4k2r/8/8/p6P/8/8/8/7K w - a6 0 2 a7");
    const zfs::Position pinned_no_ep =
        load("4k2r/8/8/p6P/8/8/8/7K w - - 0 2 a7");
    CHECK(pinned_ep.same_repetition_state(pinned_no_ep));
}

void test_automatic_draws() {
    zfs::Game game(load("4k3/8/8/8/8/8/8/4K1N1 w - - 0 1 -"));
    constexpr std::string_view cycle[]{"g1f3", "e8e7", "f3g1", "e7e8"};
    for (std::string_view move : cycle) {
        CHECK(game.play_uci(move).has_value());
    }
    CHECK(game.repetition_count() == 2U);
    CHECK(game.state() == zfs::GameState::Ongoing);

    for (std::string_view move : cycle) {
        CHECK(game.play_uci(move).has_value());
    }
    CHECK(game.repetition_count() == 3U);
    CHECK(game.state() == zfs::GameState::ThreefoldDraw);

    zfs::Game fifty(
        load("4k3/8/8/8/8/8/8/4K1N1 w - - 99 1 -"));
    CHECK(fifty.play_uci("g1f3").has_value());
    CHECK(fifty.position().halfmove_clock() == 100U);
    CHECK(fifty.state() == zfs::GameState::FiftyMoveDraw);

    zfs::Game mate(
        load("8/8/8/8/8/K7/8/k1Q5 b - - 100 51 -"));
    CHECK(mate.state() == zfs::GameState::Checkmate);
}

void test_hash_make_unmake() {
    zfs::Position position = zfs::Position::start();
    for (unsigned ply = 0; ply < 200; ++ply) {
        zfs::MoveList moves;
        position.generate_legal_moves(moves);
        if (moves.empty()) {
            break;
        }
        const zfs::Move move = moves[(ply * 37U + 11U) % moves.size()];
        const std::uint64_t raw = position.raw_key();
        const std::uint64_t base = position.base_key();
        const std::string fen = position.to_fen();
        zfs::Undo undo;
        position.do_move(move, undo);
        CHECK(position.validate().empty());
        position.undo_move(move, undo);
        CHECK(position.raw_key() == raw);
        CHECK(position.base_key() == base);
        CHECK(position.to_fen() == fen);
        position.do_move(move, undo);
    }
}

}  // namespace

int main() {
    test_repetition_identity();
    test_automatic_draws();
    test_hash_make_unmake();
    if (failures != 0) {
        std::cerr << failures << " game/history assertion(s) failed\n";
        return 1;
    }
    std::cout << "all game/history tests passed\n";
}
