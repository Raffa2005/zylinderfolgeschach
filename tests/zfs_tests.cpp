#include "zfs/position.hpp"

#include "attacks.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <set>
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
    auto position = zfs::Position::from_fen(fen, &error);
    if (!position) {
        std::cerr << "could not load test ZFS-FEN: " << fen << " (" << error
                  << ")\n";
        std::abort();
    }
    return *position;
}

std::set<std::string> move_names(zfs::Position& position) {
    zfs::MoveList moves;
    position.generate_legal_moves(moves);
    std::set<std::string> result;
    for (zfs::Move move : moves) {
        result.insert(move.uci());
    }
    CHECK(result.size() == moves.size());
    return result;
}

std::vector<std::string> moves_from(zfs::Position& position,
                                    std::string_view origin) {
    const zfs::Square from = zfs::parse_square(origin);
    zfs::MoveList moves;
    position.generate_legal_moves(moves);
    std::vector<std::string> result;
    for (zfs::Move move : moves) {
        if (move.from() == from) {
            result.push_back(move.uci());
        }
    }
    std::ranges::sort(result);
    return result;
}

std::uint64_t perft(zfs::Position& position, unsigned depth) {
    zfs::MoveList moves;
    position.generate_legal_moves(moves);
    if (depth == 1) {
        return moves.size();
    }
    std::uint64_t nodes = 0;
    for (zfs::Move move : moves) {
        zfs::Undo undo;
        position.do_move(move, undo);
        nodes += perft(position, depth - 1U);
        position.undo_move(move, undo);
    }
    return nodes;
}

zfs::Bitboard slow_line(zfs::Square origin, unsigned occupancy,
                        int file_delta, int rank_delta, bool cyclic_rank) {
    zfs::Bitboard result = 0;
    for (int direction : {-1, 1}) {
        int file = static_cast<int>(zfs::file_of(origin));
        int rank = static_cast<int>(zfs::rank_of(origin));
        for (unsigned distance = 1; distance < 8; ++distance) {
            file = (file + direction * file_delta + 8) & 7;
            rank += direction * rank_delta;
            if (rank < 0 || rank >= 8) {
                break;
            }
            const auto square = zfs::make_square(static_cast<unsigned>(file),
                                                 static_cast<unsigned>(rank));
            result |= zfs::square_bb(square);
            const unsigned occupancy_index =
                cyclic_rank ? static_cast<unsigned>(file)
                            : static_cast<unsigned>(rank);
            if ((occupancy & (1U << occupancy_index)) != 0) {
                break;
            }
        }
    }
    return result;
}

void test_attack_tables_against_oracle() {
    for (unsigned square_value = 0; square_value < 64; ++square_value) {
        const auto square = static_cast<zfs::Square>(square_value);
        for (unsigned occupancy = 0; occupancy < 256; ++occupancy) {
            const unsigned rank = zfs::rank_of(square);
            const unsigned file = zfs::file_of(square);
            zfs::Bitboard rank_occupancy = 0;
            zfs::Bitboard file_occupancy = 0;
            zfs::Bitboard rising_occupancy = 0;
            zfs::Bitboard falling_occupancy = 0;
            for (unsigned index = 0; index < 8; ++index) {
                if ((occupancy & (1U << index)) == 0) {
                    continue;
                }
                rank_occupancy |= zfs::square_bb(zfs::make_square(index, rank));
                file_occupancy |= zfs::square_bb(zfs::make_square(file, index));
                rising_occupancy |= zfs::square_bb(zfs::make_square(
                    (file + 8U - rank + index) & 7U, index));
                falling_occupancy |= zfs::square_bb(zfs::make_square(
                    (file + rank + 8U - index) & 7U, index));
            }
            const zfs::Bitboard rank_line = slow_line(square, 0, 1, 0, true);
            const zfs::Bitboard file_line = slow_line(square, 0, 0, 1, false);
            const zfs::Bitboard rising_line = slow_line(square, 0, 1, 1, false);
            const zfs::Bitboard falling_line = slow_line(square, 0, -1, 1, false);
            CHECK((zfs::detail::rook_attacks(square, rank_occupancy) & rank_line) ==
                  slow_line(square, occupancy, 1, 0, true));
            CHECK((zfs::detail::rook_attacks(square, file_occupancy) & file_line) ==
                  slow_line(square, occupancy, 0, 1, false));
            CHECK((zfs::detail::bishop_attacks(
                       square, rising_occupancy | falling_occupancy) &
                   (rising_line | falling_line)) ==
                  (slow_line(square, occupancy, 1, 1, false) |
                   slow_line(square, occupancy, -1, 1, false)));
        }
    }
}

void test_fen_and_initial_position() {
    auto position = zfs::Position::start();
    CHECK(position.to_fen() ==
          "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1 -");
    CHECK(position.validate().empty());
    CHECK(move_names(position).size() == 20);
    CHECK(perft(position, 1) == 20);
    CHECK(perft(position, 2) == 392);
    CHECK(perft(position, 3) == 8696);
    CHECK(perft(position, 4) == 191485);

    std::string error;
    CHECK(!zfs::Position::from_fen("8/8/8/8/8/8/8/4K3 w - - 0 1 -", &error));
    CHECK(!zfs::Position::from_fen(
        "4k3/8/8/8/8/8/4P3/4K3 w - - 0 1 e2", &error));
    CHECK(!zfs::Position::from_fen(
        "4k3/8/8/8/8/8/8/4K3 w K - 0 1 -", &error));
    CHECK(!zfs::Position::from_fen(
        "4k3/8/8/p7/8/8/8/4K3 w - a6 0 2 -", &error));
    const auto inferred_follow = zfs::Position::from_fen(
        "4k3/1R6/8/p7/8/8/8/4K3 w - a6 0 2", &error);
    CHECK(inferred_follow.has_value());
    if (inferred_follow) {
        CHECK(inferred_follow->follow_square() == zfs::parse_square("a7"));
        auto copy = *inferred_follow;
        CHECK(move_names(copy) == std::set<std::string>{"b7a7"});
    }
    CHECK(!zfs::Position::from_fen(
        "4k3/8/8/p7/8/8/8/4K3 w - a6 1 2 a7", &error));

    CHECK(zfs::square_bb(zfs::kNoSquare) == 0);
    zfs::Bitboard empty = 0;
    CHECK(zfs::pop_lsb(empty) == zfs::kNoSquare);
    CHECK(position.piece_at(zfs::kNoSquare) == zfs::Piece::None);
    CHECK(position.piece_at(static_cast<zfs::Square>(255)) == zfs::Piece::None);
    CHECK(position.pieces(static_cast<zfs::Color>(255), zfs::PieceType::Pawn) == 0);
    CHECK(position.pieces(zfs::Color::White,
                          static_cast<zfs::PieceType>(255)) == 0);

    auto maximum_clock =
        load("k7/8/8/8/8/8/8/7K b - - 0 4294967295 -");
    auto black_move = maximum_clock.parse_uci("a8a7");
    CHECK(black_move.has_value());
    zfs::Undo clock_undo;
    maximum_clock.do_move(*black_move, clock_undo);
    CHECK(maximum_clock.fullmove_number() == 4294967295U);
    CHECK(maximum_clock.validate().empty());
}

void test_cylindrical_piece_movement() {
    auto bishop = load("1k6/8/8/8/8/8/8/1K3B2 w - - 0 1 -");
    const auto bishop_moves = moves_from(bishop, "f1");
    for (std::string_view move : {"f1g2", "f1h3", "f1a4", "f1b5",
                                  "f1c6", "f1d7", "f1e8"}) {
        CHECK(std::ranges::find(bishop_moves, move) != bishop_moves.end());
    }

    auto duplicate = load("2k5/8/8/8/8/8/8/B3K3 w - - 0 1 -");
    const auto duplicate_moves = moves_from(duplicate, "a1");
    CHECK(std::ranges::count(duplicate_moves, std::string{"a1e5"}) == 1);

    auto alternate_bishop_path =
        load("2k5/8/8/8/8/8/1P6/B3K3 w - - 0 1 -");
    CHECK(move_names(alternate_bishop_path).contains("a1e5"));
    auto both_bishop_paths_blocked =
        load("2k5/8/8/8/8/8/1P5P/B3K3 w - - 0 1 -");
    CHECK(!move_names(both_bishop_paths_blocked).contains("a1e5"));

    auto blocked = load("1k6/8/8/8/2P5/8/8/R3K3 w - - 0 1 -");
    const auto rook_moves = moves_from(blocked, "a1");
    CHECK(std::ranges::find(rook_moves, "a1b1") != rook_moves.end());

    auto alternate_rook_path =
        load("3k4/8/8/8/RP6/8/8/4K3 w - - 0 1 -");
    CHECK(move_names(alternate_rook_path).contains("a4c4"));
    auto both_rook_paths_blocked =
        load("3k4/8/8/8/RP5P/8/8/4K3 w - - 0 1 -");
    CHECK(!move_names(both_rook_paths_blocked).contains("a4c4"));

    auto ring = load("1k6/8/8/8/4P3/8/8/R3K3 w - - 0 1 -");
    const auto ring_moves = moves_from(ring, "a1");
    for (char file = 'b'; file <= 'h'; ++file) {
        if (file == 'e') {
            continue;
        }
        const std::string move = std::string{"a1"} + file + '1';
        CHECK(std::ranges::find(ring_moves, move) != ring_moves.end());
    }

    auto king = load("3k4/8/8/8/K7/8/8/8 w - - 0 1 -");
    const auto king_moves = moves_from(king, "a4");
    CHECK(std::ranges::find(king_moves, "a4h4") != king_moves.end());
    CHECK(std::ranges::find(king_moves, "a4h5") != king_moves.end());

    auto knight = load("3k4/8/8/8/N7/8/8/4K3 w - - 0 1 -");
    const auto knight_moves = moves_from(knight, "a4");
    CHECK(std::ranges::find(knight_moves, "a4g3") != knight_moves.end());
    CHECK(std::ranges::find(knight_moves, "a4h6") != knight_moves.end());

    auto pawn = load("4k3/8/8/7p/P7/8/8/4K3 w - - 0 1 -");
    CHECK(move_names(pawn).contains("a4h5"));
}

void test_follow_legality() {
    auto forced = load("7k/8/8/8/8/8/8/R3K3 w - - 0 1 a3");
    CHECK(move_names(forced) == std::set<std::string>{"a1a3"});

    auto pinned = load("1k2r3/8/8/8/8/8/4R3/4K3 w - - 0 1 c2");
    const auto pinned_moves = move_names(pinned);
    CHECK(!pinned_moves.empty());
    CHECK(!pinned_moves.contains("e2c2"));
    CHECK(std::ranges::none_of(pinned_moves, [](const std::string& move) {
        return move.substr(2, 2) == "c2";
    }));

    auto checked_follow =
        load("1k2r3/8/8/8/8/8/R7/4K3 w - - 0 1 e2");
    CHECK(move_names(checked_follow) == std::set<std::string>{"a2e2"});

    auto checked_fallback =
        load("1k2r3/8/8/8/8/8/R7/4K3 w - - 0 1 c2");
    const auto fallback_moves = move_names(checked_fallback);
    CHECK(!fallback_moves.empty());
    CHECK(std::ranges::none_of(fallback_moves, [](const std::string& move) {
        return move.substr(2, 2) == "c2";
    }));

    auto ignored_for_attacks =
        load("1k3r2/8/8/8/8/8/8/4K3 w - - 0 1 a3");
    CHECK(ignored_for_attacks.is_square_attacked(zfs::parse_square("f1"),
                                                 zfs::Color::Black));
    CHECK(!move_names(ignored_for_attacks).contains("e1f1"));

    auto pinned_attacker =
        load("4k3/4r3/8/K7/8/8/8/4R3 w - - 0 1 -");
    CHECK(!move_names(pinned_attacker).contains("a6a7"));
}

void test_special_moves() {
    auto seam_ep = load("4k3/8/8/p6P/8/8/8/4K3 w - a6 0 2 a7");
    CHECK(move_names(seam_ep).contains("h5a6"));

    auto reverse_ep = load("4k3/8/8/8/p6P/8/8/4K3 b - h3 0 1 h2");
    CHECK(move_names(reverse_ep).contains("a4h3"));

    auto illegal_ep =
        load("4k2r/8/8/p6P/8/8/8/7K w - a6 0 2 a7");
    CHECK(!move_names(illegal_ep).contains("h5a6"));

    auto promotion = load("1k6/7P/8/8/8/8/8/4K3 w - - 0 1 h8");
    CHECK(move_names(promotion) ==
          (std::set<std::string>{"h7h8b", "h7h8n", "h7h8q", "h7h8r"}));

    auto seam_promotion =
        load("r1k5/7P/8/8/8/8/8/4K3 w - - 0 1 -");
    const auto seam_promotions = moves_from(seam_promotion, "h7");
    for (std::string_view move : {"h7a8b", "h7a8n", "h7a8q", "h7a8r"}) {
        CHECK(std::ranges::find(seam_promotions, move) != seam_promotions.end());
    }

    auto black_promotion =
        load("4k3/8/8/8/8/8/7p/1K6 b - - 0 1 h1");
    CHECK(move_names(black_promotion) ==
          (std::set<std::string>{"h2h1b", "h2h1n", "h2h1q", "h2h1r"}));

    auto black_seam_promotion =
        load("4k3/8/8/8/8/8/p7/2K4R b - - 0 1 -");
    const auto black_seam_promotions = moves_from(black_seam_promotion, "a2");
    for (std::string_view move : {"a2h1b", "a2h1n", "a2h1q", "a2h1r"}) {
        CHECK(std::ranges::find(black_seam_promotions, move) !=
              black_seam_promotions.end());
    }

    auto castle = load("4k3/8/8/8/8/8/8/4K2R w K - 0 1 -");
    CHECK(move_names(castle).contains("e1g1"));
    auto castle_move = castle.parse_uci("e1g1");
    CHECK(castle_move.has_value());
    zfs::Undo undo;
    castle.do_move(*castle_move, undo);
    CHECK(castle.follow_square() == zfs::kNoSquare);
    CHECK(castle.piece_at(zfs::parse_square("g1")) == zfs::Piece::WhiteKing);
    CHECK(castle.piece_at(zfs::parse_square("f1")) == zfs::Piece::WhiteRook);
    castle.undo_move(*castle_move, undo);
    CHECK(castle.follow_square() == zfs::kNoSquare);

    auto black_castle =
        load("r3k2r/8/8/8/8/8/8/4K3 b kq - 0 1 -");
    const auto black_castles = move_names(black_castle);
    CHECK(black_castles.contains("e8g8"));
    CHECK(black_castles.contains("e8c8"));

    auto black_castle_transit_attacked =
        load("4k2r/8/8/8/1B6/8/8/4K3 b k - 0 1 -");
    CHECK(!move_names(black_castle_transit_attacked).contains("e8g8"));

    auto black_castle_unmasks =
        load("R1n1k2r/8/8/8/8/8/8/4K3 b k - 0 1 -");
    CHECK(!move_names(black_castle_unmasks).contains("e8g8"));

    auto castle_incoming =
        load("4k3/8/8/8/8/8/8/4K2R w K - 0 1 f1");
    const auto incoming_moves = move_names(castle_incoming);
    CHECK(incoming_moves ==
          (std::set<std::string>{"e1f1", "h1f1"}));
    CHECK(!incoming_moves.contains("e1g1"));

    auto castle_unmasks =
        load("4k3/8/8/8/8/8/8/r1N1K2R w K - 0 1 -");
    CHECK(!move_names(castle_unmasks).contains("e1g1"));

    // A pawn push is the only way to land on the king's origin without
    // attacking it. Here e2-e1 could follow either white castle, but the pawn
    // attacks both transit squares, so neither castle is legal in the first
    // place.
    auto pawn_would_follow_castle =
        load("4k3/8/8/8/8/8/4p3/R3K2R w KQ - 0 1 -");
    const auto pawn_blocked_castles = move_names(pawn_would_follow_castle);
    CHECK(!pawn_blocked_castles.contains("e1g1"));
    CHECK(!pawn_blocked_castles.contains("e1c1"));

    auto rook_right = load("r3k3/1B6/8/8/8/8/8/4K3 w q - 0 1 -");
    auto rook_capture = rook_right.parse_uci("b7a8");
    CHECK(rook_capture.has_value());
    zfs::Undo rook_undo;
    rook_right.do_move(*rook_capture, rook_undo);
    CHECK((rook_right.castling_rights() & zfs::BlackQueenSide) == 0);
    rook_right.undo_move(*rook_capture, rook_undo);
    CHECK((rook_right.castling_rights() & zfs::BlackQueenSide) != 0);
}

void test_make_unmake_and_playouts() {
    auto position = zfs::Position::start();
    const std::string initial = position.to_fen();
    zfs::MoveList initial_moves;
    position.generate_legal_moves(initial_moves);
    for (zfs::Move move : initial_moves) {
        zfs::Undo undo;
        position.do_move(move, undo);
        CHECK(position.validate().empty());
        position.undo_move(move, undo);
        CHECK(position.to_fen() == initial);
        CHECK(position.validate().empty());
    }

    std::uint64_t random = 0x4d595df4d0f33173ULL;
    for (unsigned ply = 0; ply < 1000; ++ply) {
        zfs::MoveList moves;
        position.generate_legal_moves(moves);
        if (moves.empty()) {
            position = zfs::Position::start();
            continue;
        }
        random ^= random << 13U;
        random ^= random >> 7U;
        random ^= random << 17U;
        const zfs::Move move = moves[random % moves.size()];
        const std::string before = position.to_fen();
        zfs::Undo undo;
        position.do_move(move, undo);
        CHECK(position.validate().empty());
        position.undo_move(move, undo);
        CHECK(position.to_fen() == before);
        position.do_move(move, undo);
    }
}

void test_null_make_unmake() {
    auto position = load(
        "4k3/8/8/p6P/8/8/8/4K3 w - a6 0 2 a7");
    const std::string before = position.to_fen();
    const std::uint64_t key = position.raw_key();
    const auto rights = position.castling_rights();
    const auto clock = position.halfmove_clock();
    const auto fullmove = position.fullmove_number();
    const auto occupied = position.occupied();

    zfs::NullUndo undo;
    position.do_null_move(undo);
    CHECK(position.side_to_move() == zfs::Color::Black);
    CHECK(position.en_passant_square() == zfs::kNoSquare);
    CHECK(position.follow_square() == zfs::kNoSquare);
    CHECK(position.castling_rights() == rights);
    CHECK(position.halfmove_clock() == clock + 1U);
    CHECK(position.fullmove_number() == fullmove);
    CHECK(position.occupied() == occupied);
    CHECK(position.raw_key() != key);
    CHECK(position.validate().empty());

    position.undo_null_move(undo);
    CHECK(position.to_fen() == before);
    CHECK(position.raw_key() == key);
    CHECK(position.validate().empty());
}

void test_terminal_states() {
    auto mate = load("8/8/8/8/8/K7/8/k1Q5 b - - 0 1 -");
    CHECK(mate.in_check(zfs::Color::Black));
    CHECK(mate.terminal_state() == zfs::TerminalState::Checkmate);

    auto stalemate = load("8/8/8/8/8/8/4Q3/K1k5 b - - 0 1 -");
    CHECK(!stalemate.in_check(zfs::Color::Black));
    CHECK(stalemate.terminal_state() == zfs::TerminalState::Stalemate);
}

}  // namespace

int main() {
    test_attack_tables_against_oracle();
    test_fen_and_initial_position();
    test_cylindrical_piece_movement();
    test_follow_legality();
    test_special_moves();
    test_make_unmake_and_playouts();
    test_null_make_unmake();
    test_terminal_states();

    if (failures != 0) {
        std::cerr << failures << " test assertion(s) failed\n";
        return 1;
    }
    std::cout << "all ZFS tests passed\n";
}
