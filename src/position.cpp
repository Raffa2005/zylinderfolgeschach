#include "zfs/position.hpp"

#include "attacks.hpp"
#include "zobrist.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <bit>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace zfs {
namespace {

constexpr Square kA1 = make_square(0, 0);
constexpr Square kB1 = make_square(1, 0);
constexpr Square kC1 = make_square(2, 0);
constexpr Square kD1 = make_square(3, 0);
constexpr Square kE1 = make_square(4, 0);
constexpr Square kF1 = make_square(5, 0);
constexpr Square kG1 = make_square(6, 0);
constexpr Square kH1 = make_square(7, 0);
constexpr Square kA8 = make_square(0, 7);
constexpr Square kB8 = make_square(1, 7);
constexpr Square kC8 = make_square(2, 7);
constexpr Square kD8 = make_square(3, 7);
constexpr Square kE8 = make_square(4, 7);
constexpr Square kF8 = make_square(5, 7);
constexpr Square kG8 = make_square(6, 7);
constexpr Square kH8 = make_square(7, 7);

[[nodiscard]] constexpr char piece_to_char(Piece piece) noexcept {
    switch (piece) {
        case Piece::WhitePawn: return 'P';
        case Piece::WhiteKnight: return 'N';
        case Piece::WhiteBishop: return 'B';
        case Piece::WhiteRook: return 'R';
        case Piece::WhiteQueen: return 'Q';
        case Piece::WhiteKing: return 'K';
        case Piece::BlackPawn: return 'p';
        case Piece::BlackKnight: return 'n';
        case Piece::BlackBishop: return 'b';
        case Piece::BlackRook: return 'r';
        case Piece::BlackQueen: return 'q';
        case Piece::BlackKing: return 'k';
        case Piece::None: return ' ';
    }
    return ' ';
}

[[nodiscard]] constexpr Piece char_to_piece(char character) noexcept {
    switch (character) {
        case 'P': return Piece::WhitePawn;
        case 'N': return Piece::WhiteKnight;
        case 'B': return Piece::WhiteBishop;
        case 'R': return Piece::WhiteRook;
        case 'Q': return Piece::WhiteQueen;
        case 'K': return Piece::WhiteKing;
        case 'p': return Piece::BlackPawn;
        case 'n': return Piece::BlackKnight;
        case 'b': return Piece::BlackBishop;
        case 'r': return Piece::BlackRook;
        case 'q': return Piece::BlackQueen;
        case 'k': return Piece::BlackKing;
        default: return Piece::None;
    }
}

[[nodiscard]] std::string square_name(Square square) {
    if (!valid_square(square)) {
        return "-";
    }
    std::string result(2, ' ');
    result[0] = static_cast<char>('a' + file_of(square));
    result[1] = static_cast<char>('1' + rank_of(square));
    return result;
}

[[nodiscard]] std::vector<std::string_view> split_fields(std::string_view text) {
    std::vector<std::string_view> fields;
    std::size_t cursor = 0;
    while (cursor < text.size()) {
        while (cursor < text.size() &&
               std::isspace(static_cast<unsigned char>(text[cursor])) != 0) {
            ++cursor;
        }
        if (cursor == text.size()) {
            break;
        }
        const std::size_t start = cursor;
        while (cursor < text.size() &&
               std::isspace(static_cast<unsigned char>(text[cursor])) == 0) {
            ++cursor;
        }
        fields.push_back(text.substr(start, cursor - start));
    }
    return fields;
}

template <typename Integer>
[[nodiscard]] bool parse_integer(std::string_view text, Integer& result) noexcept {
    Integer value{};
    const char* const begin = text.data();
    const char* const end = begin + text.size();
    const auto parsed = std::from_chars(begin, end, value);
    if (parsed.ec != std::errc{} || parsed.ptr != end) {
        return false;
    }
    result = value;
    return true;
}

void set_error(std::string* output, std::string message) {
    if (output != nullptr) {
        *output = std::move(message);
    }
}

}  // namespace

std::string Move::uci() const {
    std::string result;
    result.reserve(is_promotion() ? 5U : 4U);
    result += square_name(from());
    result += square_name(to());
    if (is_promotion()) {
        switch (promotion()) {
            case PieceType::Knight: result.push_back('n'); break;
            case PieceType::Bishop: result.push_back('b'); break;
            case PieceType::Rook: result.push_back('r'); break;
            case PieceType::Queen: result.push_back('q'); break;
            default: break;
        }
    }
    return result;
}

Position Position::start() {
    std::string error;
    auto position = from_fen(
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1 -",
        &error);
    return position.value();
}

template <bool UpdateKey>
void Position::put_piece_impl(Square square, Piece piece) noexcept {
    assert(valid_square(square));
    assert(is_piece(piece));
    assert(board_[square] == Piece::None);
    const Bitboard bit = square_bb(square);
    const Color color = piece_color(piece);
    const PieceType type = piece_type(piece);

    board_[square] = piece;
    pieces_[color_index(color)][type_index(type)] |= bit;
    colors_[color_index(color)] |= bit;
    occupied_ |= bit;
    if constexpr (UpdateKey) {
        key_ ^= detail::piece_key(piece, square);
    }
}

template <bool UpdateKey>
Piece Position::take_piece_impl(Square square) noexcept {
    assert(valid_square(square));
    assert(is_piece(board_[square]));
    const Piece piece = board_[square];
    const Bitboard bit = square_bb(square);
    const Color color = piece_color(piece);
    const PieceType type = piece_type(piece);

    board_[square] = Piece::None;
    pieces_[color_index(color)][type_index(type)] &= ~bit;
    colors_[color_index(color)] &= ~bit;
    occupied_ &= ~bit;
    if constexpr (UpdateKey) {
        key_ ^= detail::piece_key(piece, square);
    }
    return piece;
}

void Position::put_piece(Square square, Piece piece) noexcept {
    put_piece_impl<true>(square, piece);
}

Piece Position::take_piece(Square square) noexcept {
    return take_piece_impl<true>(square);
}

std::uint64_t Position::non_piece_key() const noexcept {
    std::uint64_t result = side_to_move_ == Color::Black
                               ? detail::kZobrist.black_to_move
                               : 0;
    result ^= detail::kZobrist.castling[castling_rights_];
    if (valid_square(en_passant_)) {
        result ^= detail::kZobrist.en_passant[en_passant_];
    }
    if (valid_square(follow_)) {
        result ^= detail::kZobrist.follow[follow_];
    }
    return result;
}

std::uint64_t Position::base_key() const noexcept {
    std::uint64_t result = key_;
    if (valid_square(en_passant_)) {
        result ^= detail::kZobrist.en_passant[en_passant_];
    }
    if (valid_square(follow_)) {
        result ^= detail::kZobrist.follow[follow_];
    }
    return result;
}

bool Position::same_repetition_state(const Position& other) const {
    if (base_key() != other.base_key() || side_to_move_ != other.side_to_move_ ||
        castling_rights_ != other.castling_rights_ || board_ != other.board_) {
        return false;
    }
    if (en_passant_ == other.en_passant_ && follow_ == other.follow_) {
        return true;
    }

    Position left = *this;
    Position right = other;
    MoveList left_moves;
    MoveList right_moves;
    left.generate_legal_moves(left_moves);
    right.generate_legal_moves(right_moves);
    if (left_moves.size() != right_moves.size()) {
        return false;
    }
    const auto by_raw = [](Move lhs, Move rhs) noexcept {
        return lhs.raw() < rhs.raw();
    };
    std::sort(left_moves.begin(), left_moves.end(), by_raw);
    std::sort(right_moves.begin(), right_moves.end(), by_raw);
    return std::equal(left_moves.begin(), left_moves.end(), right_moves.begin());
}

void Position::relocate_piece(Square from, Square to) noexcept {
    const Piece piece = take_piece(from);
    put_piece(to, piece);
}

void Position::put_piece_unhashed(Square square, Piece piece) noexcept {
    put_piece_impl<false>(square, piece);
}

Piece Position::take_piece_unhashed(Square square) noexcept {
    return take_piece_impl<false>(square);
}

void Position::relocate_piece_unhashed(Square from, Square to) noexcept {
    const Piece piece = take_piece_unhashed(from);
    put_piece_unhashed(to, piece);
}

Bitboard Position::rook_attacks(Square square) const noexcept {
    return detail::rook_attacks(square, occupied_);
}

Bitboard Position::bishop_attacks(Square square) const noexcept {
    return detail::bishop_attacks(square, occupied_);
}

Square Position::king_square(Color color) const noexcept {
    const Bitboard king = pieces_[color_index(color)][type_index(PieceType::King)];
    return king == 0 ? kNoSquare : static_cast<Square>(std::countr_zero(king));
}

bool Position::is_square_attacked(Square square, Color by) const noexcept {
    if (!valid_square(square) || !valid_color(by)) {
        return false;
    }
    const unsigned by_index = color_index(by);
    if ((detail::kAttacks.pawn[color_index(opposite(by))][square] &
         pieces_[by_index][type_index(PieceType::Pawn)]) != 0) {
        return true;
    }
    if ((detail::kAttacks.knight[square] &
         pieces_[by_index][type_index(PieceType::Knight)]) != 0) {
        return true;
    }
    if ((detail::kAttacks.king[square] &
         pieces_[by_index][type_index(PieceType::King)]) != 0) {
        return true;
    }
    if ((bishop_attacks(square) &
         (pieces_[by_index][type_index(PieceType::Bishop)] |
          pieces_[by_index][type_index(PieceType::Queen)])) != 0) {
        return true;
    }
    return (rook_attacks(square) &
            (pieces_[by_index][type_index(PieceType::Rook)] |
             pieces_[by_index][type_index(PieceType::Queen)])) != 0;
}

bool Position::in_check(Color color) const noexcept {
    if (!valid_color(color)) {
        return false;
    }
    return is_square_attacked(king_square(color), opposite(color));
}

void Position::generate_pawn_moves(MoveList& result, Color color) noexcept {
    const unsigned us = color_index(color);
    const unsigned them = color_index(opposite(color));
    const int step = color == Color::White ? 8 : -8;
    const unsigned start_rank = color == Color::White ? 1U : 6U;
    const unsigned promotion_rank = color == Color::White ? 7U : 0U;
    const Bitboard enemy_king = pieces_[them][type_index(PieceType::King)];
    Bitboard pawns = pieces_[us][type_index(PieceType::Pawn)];

    while (pawns != 0) {
        const Square from = pop_lsb(pawns);
        const int destination = static_cast<int>(from) + step;
        if (destination >= 0 && destination < 64) {
            const auto to = static_cast<Square>(destination);
            if (board_[to] == Piece::None) {
                if (rank_of(to) == promotion_rank) {
                    for (PieceType promotion : {PieceType::Knight, PieceType::Bishop,
                                                PieceType::Rook, PieceType::Queen}) {
                        result.push(Move(from, to, MoveType::Promotion, promotion));
                    }
                } else {
                    result.push(Move(from, to));
                    if (rank_of(from) == start_rank) {
                        const auto double_to = static_cast<Square>(destination + step);
                        if (board_[double_to] == Piece::None) {
                            result.push(Move(from, double_to,
                                             MoveType::DoublePawnPush));
                        }
                    }
                }
            }
        }

        Bitboard captures = detail::kAttacks.pawn[us][from] & colors_[them] &
                            ~enemy_king;
        while (captures != 0) {
            const Square to = pop_lsb(captures);
            if (rank_of(to) == promotion_rank) {
                for (PieceType promotion : {PieceType::Knight, PieceType::Bishop,
                                            PieceType::Rook, PieceType::Queen}) {
                    result.push(
                        Move(from, to, MoveType::PromotionCapture, promotion));
                }
            } else {
                result.push(Move(from, to, MoveType::Capture));
            }
        }

        if (valid_square(en_passant_) &&
            (detail::kAttacks.pawn[us][from] & square_bb(en_passant_)) != 0) {
            const int captured_value = static_cast<int>(en_passant_) - step;
            const auto captured = static_cast<Square>(captured_value);
            if (board_[en_passant_] == Piece::None &&
                board_[captured] == make_piece(opposite(color), PieceType::Pawn)) {
                result.push(Move(from, en_passant_, MoveType::EnPassant));
            }
        }
    }
}

void Position::generate_pawn_tactical(MoveList& result, Color color) noexcept {
    const unsigned us = color_index(color);
    const unsigned them = color_index(opposite(color));
    const int step = color == Color::White ? 8 : -8;
    const unsigned promotion_rank = color == Color::White ? 7U : 0U;
    const Bitboard enemy_king = pieces_[them][type_index(PieceType::King)];
    Bitboard pawns = pieces_[us][type_index(PieceType::Pawn)];

    while (pawns != 0) {
        const Square from = pop_lsb(pawns);
        const int destination = static_cast<int>(from) + step;
        if (destination >= 0 && destination < 64) {
            const auto to = static_cast<Square>(destination);
            if (rank_of(to) == promotion_rank && board_[to] == Piece::None) {
                for (PieceType promotion : {PieceType::Knight, PieceType::Bishop,
                                            PieceType::Rook, PieceType::Queen}) {
                    result.push(Move(from, to, MoveType::Promotion, promotion));
                }
            }
        }

        Bitboard captures = detail::kAttacks.pawn[us][from] & colors_[them] &
                            ~enemy_king;
        while (captures != 0) {
            const Square to = pop_lsb(captures);
            if (rank_of(to) == promotion_rank) {
                for (PieceType promotion : {PieceType::Knight, PieceType::Bishop,
                                            PieceType::Rook, PieceType::Queen}) {
                    result.push(
                        Move(from, to, MoveType::PromotionCapture, promotion));
                }
            } else {
                result.push(Move(from, to, MoveType::Capture));
            }
        }

        if (valid_square(en_passant_) &&
            (detail::kAttacks.pawn[us][from] & square_bb(en_passant_)) != 0) {
            const auto captured =
                static_cast<Square>(static_cast<int>(en_passant_) - step);
            if (board_[en_passant_] == Piece::None &&
                board_[captured] == make_piece(opposite(color), PieceType::Pawn)) {
                result.push(Move(from, en_passant_, MoveType::EnPassant));
            }
        }
    }
}

void Position::generate_piece_moves(MoveList& result, Color color,
                                    PieceType type) noexcept {
    const unsigned us = color_index(color);
    const unsigned them = color_index(opposite(color));
    const Bitboard forbidden = colors_[us] |
                               pieces_[them][type_index(PieceType::King)];
    Bitboard pieces = pieces_[us][type_index(type)];
    while (pieces != 0) {
        const Square from = pop_lsb(pieces);
        Bitboard targets = 0;
        switch (type) {
            case PieceType::Knight: targets = detail::kAttacks.knight[from]; break;
            case PieceType::Bishop: targets = bishop_attacks(from); break;
            case PieceType::Rook: targets = rook_attacks(from); break;
            case PieceType::Queen:
                targets = bishop_attacks(from) | rook_attacks(from);
                break;
            case PieceType::King: targets = detail::kAttacks.king[from]; break;
            default: break;
        }
        targets &= ~forbidden;
        while (targets != 0) {
            const Square to = pop_lsb(targets);
            const MoveType move_type = (colors_[them] & square_bb(to)) != 0
                                           ? MoveType::Capture
                                           : MoveType::Quiet;
            result.push(Move(from, to, move_type));
        }
    }
}

void Position::generate_piece_captures(MoveList& result, Color color,
                                       PieceType type) noexcept {
    const unsigned us = color_index(color);
    const unsigned them = color_index(opposite(color));
    const Bitboard targets_allowed =
        colors_[them] & ~pieces_[them][type_index(PieceType::King)];
    Bitboard pieces = pieces_[us][type_index(type)];
    while (pieces != 0) {
        const Square from = pop_lsb(pieces);
        Bitboard targets = 0;
        switch (type) {
            case PieceType::Knight: targets = detail::kAttacks.knight[from]; break;
            case PieceType::Bishop: targets = bishop_attacks(from); break;
            case PieceType::Rook: targets = rook_attacks(from); break;
            case PieceType::Queen:
                targets = bishop_attacks(from) | rook_attacks(from);
                break;
            case PieceType::King: targets = detail::kAttacks.king[from]; break;
            default: break;
        }
        targets &= targets_allowed;
        while (targets != 0) {
            result.push(Move(from, pop_lsb(targets), MoveType::Capture));
        }
    }
}

bool Position::castle_transit_safe(Color color, Square transit) noexcept {
    if (in_check(color) || board_[transit] != Piece::None) {
        return false;
    }
    const Square origin = color == Color::White ? kE1 : kE8;
    relocate_piece(origin, transit);
    const bool safe = !is_square_attacked(transit, opposite(color));
    relocate_piece(transit, origin);
    return safe;
}

void Position::generate_castles(MoveList& result, Color color) noexcept {
    const Piece king = make_piece(color, PieceType::King);
    const Piece rook = make_piece(color, PieceType::Rook);
    if (color == Color::White) {
        if (board_[kE1] != king) {
            return;
        }
        if ((castling_rights_ & WhiteKingSide) != 0 && board_[kH1] == rook &&
            board_[kF1] == Piece::None && board_[kG1] == Piece::None &&
            castle_transit_safe(color, kF1)) {
            result.push(Move(kE1, kG1, MoveType::KingCastle));
        }
        if ((castling_rights_ & WhiteQueenSide) != 0 && board_[kA1] == rook &&
            board_[kB1] == Piece::None && board_[kC1] == Piece::None &&
            board_[kD1] == Piece::None && castle_transit_safe(color, kD1)) {
            result.push(Move(kE1, kC1, MoveType::QueenCastle));
        }
        return;
    }

    if (board_[kE8] != king) {
        return;
    }
    if ((castling_rights_ & BlackKingSide) != 0 && board_[kH8] == rook &&
        board_[kF8] == Piece::None && board_[kG8] == Piece::None &&
        castle_transit_safe(color, kF8)) {
        result.push(Move(kE8, kG8, MoveType::KingCastle));
    }
    if ((castling_rights_ & BlackQueenSide) != 0 && board_[kA8] == rook &&
        board_[kB8] == Piece::None && board_[kC8] == Piece::None &&
        board_[kD8] == Piece::None && castle_transit_safe(color, kD8)) {
        result.push(Move(kE8, kC8, MoveType::QueenCastle));
    }
}

void Position::generate_pseudo_legal(MoveList& result) noexcept {
    result.clear();
    const Color color = side_to_move_;
    generate_pawn_moves(result, color);
    generate_piece_moves(result, color, PieceType::Knight);
    generate_piece_moves(result, color, PieceType::Bishop);
    generate_piece_moves(result, color, PieceType::Rook);
    generate_piece_moves(result, color, PieceType::Queen);
    generate_piece_moves(result, color, PieceType::King);
    generate_castles(result, color);
}

void Position::generate_pseudo_tactical(MoveList& result) noexcept {
    result.clear();
    const Color color = side_to_move_;
    generate_pawn_tactical(result, color);
    generate_piece_captures(result, color, PieceType::Knight);
    generate_piece_captures(result, color, PieceType::Bishop);
    generate_piece_captures(result, color, PieceType::Rook);
    generate_piece_captures(result, color, PieceType::Queen);
    generate_piece_captures(result, color, PieceType::King);
}

void Position::generate_pseudo_to(MoveList& result, Square target) noexcept {
    result.clear();
    if (!valid_square(target) || board_[target] != Piece::None) {
        return;
    }

    const Color us_color = side_to_move_;
    const Color them_color = opposite(us_color);
    const unsigned us = color_index(us_color);
    const unsigned them = color_index(them_color);
    const int pawn_step = us_color == Color::White ? 8 : -8;
    const unsigned pawn_start_rank = us_color == Color::White ? 1U : 6U;
    const unsigned promotion_rank = us_color == Color::White ? 7U : 0U;

    const int pawn_source_value = static_cast<int>(target) - pawn_step;
    if (pawn_source_value >= 0 && pawn_source_value < 64) {
        const auto source = static_cast<Square>(pawn_source_value);
        if (board_[source] == make_piece(us_color, PieceType::Pawn)) {
            if (rank_of(target) == promotion_rank) {
                for (PieceType promotion : {PieceType::Knight, PieceType::Bishop,
                                            PieceType::Rook, PieceType::Queen}) {
                    result.push(
                        Move(source, target, MoveType::Promotion, promotion));
                }
            } else {
                result.push(Move(source, target));
            }
        }
    }

    const int double_source_value = static_cast<int>(target) - 2 * pawn_step;
    if (double_source_value >= 0 && double_source_value < 64) {
        const auto source = static_cast<Square>(double_source_value);
        const auto transit = static_cast<Square>(static_cast<int>(target) - pawn_step);
        if (rank_of(source) == pawn_start_rank &&
            board_[source] == make_piece(us_color, PieceType::Pawn) &&
            board_[transit] == Piece::None) {
            result.push(Move(source, target, MoveType::DoublePawnPush));
        }
    }

    Bitboard pawn_sources =
        detail::kAttacks.pawn[color_index(them_color)][target] &
        pieces_[us][type_index(PieceType::Pawn)];
    if (target == en_passant_) {
        const auto captured = static_cast<Square>(static_cast<int>(target) - pawn_step);
        if (board_[captured] == make_piece(them_color, PieceType::Pawn)) {
            while (pawn_sources != 0) {
                result.push(Move(pop_lsb(pawn_sources), target,
                                 MoveType::EnPassant));
            }
        }
    } else if ((colors_[them] & square_bb(target)) != 0 &&
               board_[target] != make_piece(them_color, PieceType::King)) {
        while (pawn_sources != 0) {
            const Square source = pop_lsb(pawn_sources);
            if (rank_of(target) == promotion_rank) {
                for (PieceType promotion : {PieceType::Knight, PieceType::Bishop,
                                            PieceType::Rook, PieceType::Queen}) {
                    result.push(Move(source, target, MoveType::PromotionCapture,
                                     promotion));
                }
            } else {
                result.push(Move(source, target, MoveType::Capture));
            }
        }
    }

    const MoveType move_type = (colors_[them] & square_bb(target)) != 0
                                   ? MoveType::Capture
                                   : MoveType::Quiet;
    auto emit_sources = [&](Bitboard sources) noexcept {
        while (sources != 0) {
            result.push(Move(pop_lsb(sources), target, move_type));
        }
    };

    const Bitboard diagonal = bishop_attacks(target);
    const Bitboard orthogonal = rook_attacks(target);
    emit_sources(detail::kAttacks.knight[target] &
                 pieces_[us][type_index(PieceType::Knight)]);
    emit_sources(diagonal & pieces_[us][type_index(PieceType::Bishop)]);
    emit_sources(orthogonal & pieces_[us][type_index(PieceType::Rook)]);
    emit_sources((diagonal | orthogonal) &
                 pieces_[us][type_index(PieceType::Queen)]);
    emit_sources(detail::kAttacks.king[target] &
                 pieces_[us][type_index(PieceType::King)]);

    const unsigned home_rank = us_color == Color::White ? 0U : 7U;
    if (rank_of(target) == home_rank &&
        (file_of(target) == 2U || file_of(target) == 6U)) {
        MoveList castles;
        generate_castles(castles, us_color);
        for (Move move : castles) {
            if (move.to() == target) {
                result.push(move);
            }
        }
    }
}

bool Position::move_lands_on(Move move, Square target) const noexcept {
    return move.to() == target;
}

bool Position::leaves_king_safe(Move move) noexcept {
    const Color mover = side_to_move_;
    Undo undo;
    do_move(move, undo);
    const bool safe = !in_check(mover);
    undo_move(move, undo);
    return safe;
}

Position::LegalContext Position::legal_context() const noexcept {
    const Color mover = side_to_move_;
    const unsigned us = color_index(mover);
    const unsigned them = color_index(opposite(mover));
    const Square king = king_square(mover);
    assert(valid_square(king));
    const Bitboard diagonal = bishop_attacks(king);
    const Bitboard orthogonal = rook_attacks(king);
    const Bitboard diagonal_sliders =
        pieces_[them][type_index(PieceType::Bishop)] |
        pieces_[them][type_index(PieceType::Queen)];
    const Bitboard orthogonal_sliders =
        pieces_[them][type_index(PieceType::Rook)] |
        pieces_[them][type_index(PieceType::Queen)];

    LegalContext context;
    context.checked =
        (detail::kAttacks.pawn[us][king] &
         pieces_[them][type_index(PieceType::Pawn)]) != 0 ||
        (detail::kAttacks.knight[king] &
         pieces_[them][type_index(PieceType::Knight)]) != 0 ||
        (detail::kAttacks.king[king] &
         pieces_[them][type_index(PieceType::King)]) != 0 ||
        (diagonal & diagonal_sliders) != 0 ||
        (orthogonal & orthogonal_sliders) != 0;
    if (context.checked) {
        return context;
    }

    Bitboard possible = (diagonal | orthogonal) & colors_[us];
    possible &= ~square_bb(king);
    while (possible != 0) {
        const Square blocker = pop_lsb(possible);
        const Bitboard occupancy = occupied_ & ~square_bb(blocker);
        if ((detail::bishop_attacks(king, occupancy) & diagonal_sliders) != 0 ||
            (detail::rook_attacks(king, occupancy) & orthogonal_sliders) != 0) {
            context.king_blockers |= square_bb(blocker);
        }
    }
    return context;
}

bool Position::legal_with_context(Move move,
                                  const LegalContext& context) noexcept {
    const Piece moving = board_[move.from()];
    const bool needs_exact_test =
        context.checked || piece_type(moving) == PieceType::King ||
        move.type() == MoveType::EnPassant ||
        (context.king_blockers & square_bb(move.from())) != 0;
    return !needs_exact_test || leaves_king_safe(move);
}

bool Position::must_follow() {
    if (!valid_square(follow_)) {
        return false;
    }
    MoveList candidates;
    generate_pseudo_to(candidates, follow_);
    for (Move move : candidates) {
        if (leaves_king_safe(move)) {
            return true;
        }
    }
    return false;
}

void Position::generate_legal_moves(MoveList& result) {
    result.clear();
    MoveList candidates;
    const bool has_follow = valid_square(follow_);
    if (has_follow) {
        generate_pseudo_to(candidates, follow_);
#ifndef NDEBUG
        MoveList reference;
        generate_pseudo_legal(reference);
        std::size_t expected_size = 0;
        for (Move move : reference) {
            if (!move_lands_on(move, follow_)) {
                continue;
            }
            ++expected_size;
            bool found = false;
            for (Move candidate : candidates) {
                found |= candidate == move;
            }
            assert(found);
        }
        assert(expected_size == candidates.size());
#endif
        for (Move move : candidates) {
            if (leaves_king_safe(move)) {
                result.push(move);
            }
        }
        if (!result.empty()) {
            return;
        }

        generate_pseudo_legal(candidates);
    } else {
        generate_pseudo_legal(candidates);
    }

    const LegalContext context = legal_context();
    for (Move move : candidates) {
        if (has_follow && move_lands_on(move, follow_)) {
            continue;
        }
        if (legal_with_context(move, context)) {
            result.push(move);
        }
    }
}

TacticalMoveInfo Position::generate_legal_tactical_moves(MoveList& result) {
    result.clear();
    MoveList candidates;
    const bool has_follow = valid_square(follow_);
    if (has_follow) {
        generate_pseudo_to(candidates, follow_);
        for (Move move : candidates) {
            if (leaves_king_safe(move)) {
                result.push(move);
            }
        }
    }

    const LegalContext context = legal_context();
    if (!result.empty()) {
        return TacticalMoveInfo{true, context.checked, true};
    }

    if (context.checked) {
        generate_pseudo_legal(candidates);
        for (Move move : candidates) {
            if (has_follow && move_lands_on(move, follow_)) {
                continue;
            }
            if (legal_with_context(move, context)) {
                result.push(move);
            }
        }
        return TacticalMoveInfo{!result.empty(), true, true};
    }

    generate_pseudo_tactical(candidates);
    for (Move move : candidates) {
        if (has_follow && move_lands_on(move, follow_)) {
            continue;
        }
        if (legal_with_context(move, context)) {
            result.push(move);
        }
    }
    if (!result.empty()) {
        return TacticalMoveInfo{true, false, false};
    }

    // No legal tactical move proves legality only if some quiet move survives.
    // Stop at the first one: the caller needs the distinction between a quiet
    // position and stalemate, not the quiet move list itself.
    generate_pseudo_legal(candidates);
    for (Move move : candidates) {
        if (move.is_capture() || move.is_promotion() ||
            (has_follow && move_lands_on(move, follow_))) {
            continue;
        }
        if (legal_with_context(move, context)) {
            return TacticalMoveInfo{true, false, false};
        }
    }
    return TacticalMoveInfo{};
}

MoveList Position::legal_moves() {
    MoveList result;
    generate_legal_moves(result);
    return result;
}

std::optional<Move> Position::parse_uci(std::string_view uci) {
    if (uci.size() != 4 && uci.size() != 5) {
        return std::nullopt;
    }
    MoveList moves;
    generate_legal_moves(moves);
    for (Move move : moves) {
        if (move.uci() == uci) {
            return move;
        }
    }
    return std::nullopt;
}

bool Position::is_legal(Move move) {
    MoveList moves;
    generate_legal_moves(moves);
    return std::find(moves.begin(), moves.end(), move) != moves.end();
}

TerminalState Position::terminal_state() {
    MoveList moves;
    generate_legal_moves(moves);
    if (!moves.empty()) {
        return TerminalState::Ongoing;
    }
    return in_check(side_to_move_) ? TerminalState::Checkmate
                                   : TerminalState::Stalemate;
}

void Position::do_move(Move move, Undo& undo) noexcept {
    const Square from = move.from();
    const Square to = move.to();
    const Piece moving = board_[from];
    const Color mover = side_to_move_;
    const int pawn_step = mover == Color::White ? 8 : -8;
    assert(valid_square(from) && valid_square(to));
    assert(is_piece(moving) && piece_color(moving) == mover);

    undo.castling_rights = castling_rights_;
    undo.en_passant = en_passant_;
    undo.follow = follow_;
    undo.halfmove_clock = halfmove_clock_;
    undo.fullmove_number = fullmove_number_;
    undo.key = key_;
    undo.captured = Piece::None;

    if (valid_square(en_passant_)) {
        key_ ^= detail::kZobrist.en_passant[en_passant_];
    }
    if (valid_square(follow_)) {
        key_ ^= detail::kZobrist.follow[follow_];
    }

    if (piece_type(moving) == PieceType::King) {
        castling_rights_ &= static_cast<std::uint8_t>(
            mover == Color::White ? ~(WhiteKingSide | WhiteQueenSide)
                                  : ~(BlackKingSide | BlackQueenSide));
    }
    if (piece_type(moving) == PieceType::Rook) {
        if (from == kA1) castling_rights_ &= static_cast<std::uint8_t>(~WhiteQueenSide);
        if (from == kH1) castling_rights_ &= static_cast<std::uint8_t>(~WhiteKingSide);
        if (from == kA8) castling_rights_ &= static_cast<std::uint8_t>(~BlackQueenSide);
        if (from == kH8) castling_rights_ &= static_cast<std::uint8_t>(~BlackKingSide);
    }

    en_passant_ = kNoSquare;
    // No legal reply can follow a castle to the king's vacated square. A
    // non-pawn follower attacked the king there before castling; the sole pawn
    // push candidate attacked the castle's transit square instead. Store the
    // semantically equivalent canonical state.
    follow_ = move.is_castle() ? kNoSquare : from;
    if (piece_type(moving) == PieceType::Pawn || move.is_capture()) {
        halfmove_clock_ = 0;
    } else if (halfmove_clock_ != std::numeric_limits<std::uint16_t>::max()) {
        ++halfmove_clock_;
    }

    Square captured_square = to;
    if (move.type() == MoveType::EnPassant) {
        captured_square = static_cast<Square>(static_cast<int>(to) - pawn_step);
    }
    if (move.is_capture()) {
        assert(is_piece(board_[captured_square]));
        assert(piece_color(board_[captured_square]) == opposite(mover));
        undo.captured = take_piece(captured_square);
        if (captured_square == kA1)
            castling_rights_ &= static_cast<std::uint8_t>(~WhiteQueenSide);
        if (captured_square == kH1)
            castling_rights_ &= static_cast<std::uint8_t>(~WhiteKingSide);
        if (captured_square == kA8)
            castling_rights_ &= static_cast<std::uint8_t>(~BlackQueenSide);
        if (captured_square == kH8)
            castling_rights_ &= static_cast<std::uint8_t>(~BlackKingSide);
    }

    switch (move.type()) {
        case MoveType::KingCastle:
            relocate_piece(from, to);
            if (mover == Color::White) {
                relocate_piece(kH1, kF1);
            } else {
                relocate_piece(kH8, kF8);
            }
            break;
        case MoveType::QueenCastle:
            relocate_piece(from, to);
            if (mover == Color::White) {
                relocate_piece(kA1, kD1);
            } else {
                relocate_piece(kA8, kD8);
            }
            break;
        case MoveType::Promotion:
        case MoveType::PromotionCapture:
            static_cast<void>(take_piece(from));
            put_piece(to, make_piece(mover, move.promotion()));
            break;
        default:
            relocate_piece(from, to);
            if (move.type() == MoveType::DoublePawnPush) {
                en_passant_ = static_cast<Square>(static_cast<int>(from) + pawn_step);
            }
            break;
    }

    if (mover == Color::Black &&
        fullmove_number_ != std::numeric_limits<std::uint32_t>::max()) {
        ++fullmove_number_;
    }
    side_to_move_ = opposite(mover);
    key_ ^= detail::kZobrist.black_to_move;
    if (undo.castling_rights != castling_rights_) {
        key_ ^= detail::kZobrist.castling[undo.castling_rights] ^
                detail::kZobrist.castling[castling_rights_];
    }
    if (valid_square(en_passant_)) {
        key_ ^= detail::kZobrist.en_passant[en_passant_];
    }
    if (valid_square(follow_)) {
        key_ ^= detail::kZobrist.follow[follow_];
    }
}

void Position::undo_move(Move move, const Undo& undo) noexcept {
    side_to_move_ = opposite(side_to_move_);
    const Color mover = side_to_move_;
    const int pawn_step = mover == Color::White ? 8 : -8;
    const Square from = move.from();
    const Square to = move.to();

    switch (move.type()) {
        case MoveType::KingCastle:
            relocate_piece_unhashed(to, from);
            if (mover == Color::White) {
                relocate_piece_unhashed(kF1, kH1);
            } else {
                relocate_piece_unhashed(kF8, kH8);
            }
            break;
        case MoveType::QueenCastle:
            relocate_piece_unhashed(to, from);
            if (mover == Color::White) {
                relocate_piece_unhashed(kD1, kA1);
            } else {
                relocate_piece_unhashed(kD8, kA8);
            }
            break;
        case MoveType::Promotion:
        case MoveType::PromotionCapture:
            static_cast<void>(take_piece_unhashed(to));
            put_piece_unhashed(from, make_piece(mover, PieceType::Pawn));
            break;
        default: relocate_piece_unhashed(to, from); break;
    }

    if (undo.captured != Piece::None) {
        const Square captured_square =
            move.type() == MoveType::EnPassant
                ? static_cast<Square>(static_cast<int>(to) - pawn_step)
                : to;
        put_piece_unhashed(captured_square, undo.captured);
    }

    castling_rights_ = undo.castling_rights;
    en_passant_ = undo.en_passant;
    follow_ = undo.follow;
    halfmove_clock_ = undo.halfmove_clock;
    fullmove_number_ = undo.fullmove_number;
    key_ = undo.key;
}

void Position::do_null_move(NullUndo& undo) noexcept {
    undo.en_passant = en_passant_;
    undo.follow = follow_;
    undo.halfmove_clock = halfmove_clock_;
    undo.fullmove_number = fullmove_number_;
    undo.key = key_;

    if (valid_square(en_passant_)) {
        key_ ^= detail::kZobrist.en_passant[en_passant_];
    }
    if (valid_square(follow_)) {
        key_ ^= detail::kZobrist.follow[follow_];
    }
    en_passant_ = kNoSquare;
    follow_ = kNoSquare;
    if (halfmove_clock_ != std::numeric_limits<std::uint16_t>::max()) {
        ++halfmove_clock_;
    }
    if (side_to_move_ == Color::Black &&
        fullmove_number_ != std::numeric_limits<std::uint32_t>::max()) {
        ++fullmove_number_;
    }
    side_to_move_ = opposite(side_to_move_);
    key_ ^= detail::kZobrist.black_to_move;
}

void Position::undo_null_move(const NullUndo& undo) noexcept {
    side_to_move_ = opposite(side_to_move_);
    en_passant_ = undo.en_passant;
    follow_ = undo.follow;
    halfmove_clock_ = undo.halfmove_clock;
    fullmove_number_ = undo.fullmove_number;
    key_ = undo.key;
}

std::optional<Position> Position::from_fen(std::string_view fen,
                                           std::string* error) {
    const auto fields = split_fields(fen);
    if (fields.size() != 6 && fields.size() != 7) {
        set_error(error, "ZFS-FEN must contain six or seven fields");
        return std::nullopt;
    }

    Position result;
    int rank = 7;
    unsigned file = 0;
    for (char character : fields[0]) {
        if (character == '/') {
            if (file != 8 || rank == 0) {
                set_error(error, "invalid rank structure in piece placement");
                return std::nullopt;
            }
            --rank;
            file = 0;
            continue;
        }
        if (character >= '1' && character <= '8') {
            file += static_cast<unsigned>(character - '0');
            if (file > 8) {
                set_error(error, "too many files in a rank");
                return std::nullopt;
            }
            continue;
        }
        const Piece piece = char_to_piece(character);
        if (piece == Piece::None || file >= 8) {
            set_error(error, "invalid piece placement character");
            return std::nullopt;
        }
        result.put_piece(make_square(file, static_cast<unsigned>(rank)), piece);
        ++file;
    }
    if (rank != 0 || file != 8) {
        set_error(error, "piece placement does not describe eight complete ranks");
        return std::nullopt;
    }

    if (fields[1] == "w") {
        result.side_to_move_ = Color::White;
    } else if (fields[1] == "b") {
        result.side_to_move_ = Color::Black;
    } else {
        set_error(error, "side-to-move field must be 'w' or 'b'");
        return std::nullopt;
    }

    if (fields[2] != "-") {
        std::uint8_t seen = 0;
        for (char right : fields[2]) {
            std::uint8_t flag = 0;
            switch (right) {
                case 'K': flag = WhiteKingSide; break;
                case 'Q': flag = WhiteQueenSide; break;
                case 'k': flag = BlackKingSide; break;
                case 'q': flag = BlackQueenSide; break;
                default:
                    set_error(error, "invalid castling-rights field");
                    return std::nullopt;
            }
            if ((seen & flag) != 0) {
                set_error(error, "duplicate castling right");
                return std::nullopt;
            }
            seen |= flag;
        }
        result.castling_rights_ = seen;
    }

    if (fields[3] != "-") {
        result.en_passant_ = parse_square(fields[3]);
        if (!valid_square(result.en_passant_)) {
            set_error(error, "invalid en-passant square");
            return std::nullopt;
        }
    }

    if (!parse_integer(fields[4], result.halfmove_clock_)) {
        set_error(error, "invalid halfmove clock");
        return std::nullopt;
    }
    if (!parse_integer(fields[5], result.fullmove_number_) ||
        result.fullmove_number_ == 0) {
        set_error(error, "invalid fullmove number");
        return std::nullopt;
    }

    if (fields.size() == 7 && fields[6] != "-") {
        result.follow_ = parse_square(fields[6]);
        if (!valid_square(result.follow_)) {
            set_error(error, "invalid follow square");
            return std::nullopt;
        }
    }
    if (fields.size() == 6 && valid_square(result.en_passant_)) {
        const int inferred = static_cast<int>(result.en_passant_) +
                             (result.side_to_move_ == Color::White ? 8 : -8);
        if (inferred >= 0 && inferred < 64) {
            result.follow_ = static_cast<Square>(inferred);
        }
    }
    if (fields.size() == 7 && valid_square(result.en_passant_) &&
        !valid_square(result.follow_)) {
        set_error(error,
                  "seven-field ZFS-FEN with en passant requires a follow square");
        return std::nullopt;
    }

    result.key_ ^= result.non_piece_key();

    const std::string problem = result.validate();
    if (!problem.empty()) {
        set_error(error, problem);
        return std::nullopt;
    }
    if (error != nullptr) {
        error->clear();
    }
    return result;
}

std::string Position::validate() const {
    std::array<std::array<Bitboard, 6>, 2> expected_pieces{};
    std::array<Bitboard, 2> expected_colors{};
    Bitboard expected_occupied = 0;
    std::uint64_t expected_key = non_piece_key();
    for (unsigned value = 0; value < 64; ++value) {
        const auto square = static_cast<Square>(value);
        const Piece piece = board_[square];
        if (!is_piece(piece)) {
            continue;
        }
        const Color color = piece_color(piece);
        const PieceType type = piece_type(piece);
        const Bitboard bit = square_bb(square);
        expected_pieces[color_index(color)][type_index(type)] |= bit;
        expected_colors[color_index(color)] |= bit;
        expected_occupied |= bit;
        expected_key ^= detail::piece_key(piece, square);
    }
    if (pieces_ != expected_pieces || colors_ != expected_colors ||
        occupied_ != expected_occupied) {
        return "internal board representations disagree";
    }
    if (key_ != expected_key) {
        return "incremental position key disagrees with the board";
    }

    for (Color color : {Color::White, Color::Black}) {
        const unsigned index = color_index(color);
        if (std::popcount(colors_[index]) > 16) {
            return color == Color::White ? "white may not have more than 16 pieces"
                                         : "black may not have more than 16 pieces";
        }
        if (std::popcount(pieces_[index][type_index(PieceType::Pawn)]) > 8) {
            return color == Color::White ? "white may not have more than eight pawns"
                                         : "black may not have more than eight pawns";
        }
    }

    if (std::popcount(pieces_[color_index(Color::White)]
                            [type_index(PieceType::King)]) != 1) {
        return "position must contain exactly one white king";
    }
    if (std::popcount(pieces_[color_index(Color::Black)]
                            [type_index(PieceType::King)]) != 1) {
        return "position must contain exactly one black king";
    }

    constexpr Bitboard kBackRanks = Bitboard{0xff} | (Bitboard{0xff} << 56U);
    const Bitboard pawns = pieces_[0][type_index(PieceType::Pawn)] |
                           pieces_[1][type_index(PieceType::Pawn)];
    if ((pawns & kBackRanks) != 0) {
        return "pawns may not occupy rank 1 or rank 8";
    }
    if (valid_square(follow_) && board_[follow_] != Piece::None) {
        return "follow square must be empty";
    }

    if ((castling_rights_ & (WhiteKingSide | WhiteQueenSide)) != 0 &&
        board_[kE1] != Piece::WhiteKing) {
        return "white castling rights require a white king on e1";
    }
    if ((castling_rights_ & WhiteKingSide) != 0 &&
        board_[kH1] != Piece::WhiteRook) {
        return "white king-side castling rights require a white rook on h1";
    }
    if ((castling_rights_ & WhiteQueenSide) != 0 &&
        board_[kA1] != Piece::WhiteRook) {
        return "white queen-side castling rights require a white rook on a1";
    }
    if ((castling_rights_ & (BlackKingSide | BlackQueenSide)) != 0 &&
        board_[kE8] != Piece::BlackKing) {
        return "black castling rights require a black king on e8";
    }
    if ((castling_rights_ & BlackKingSide) != 0 &&
        board_[kH8] != Piece::BlackRook) {
        return "black king-side castling rights require a black rook on h8";
    }
    if ((castling_rights_ & BlackQueenSide) != 0 &&
        board_[kA8] != Piece::BlackRook) {
        return "black queen-side castling rights require a black rook on a8";
    }

    if (valid_square(en_passant_)) {
        if (halfmove_clock_ != 0) {
            return "en-passant state requires a zero halfmove clock";
        }
        if (board_[en_passant_] != Piece::None) {
            return "en-passant target must be empty";
        }
        if (side_to_move_ == Color::White) {
            if (rank_of(en_passant_) != 5 ||
                board_[static_cast<Square>(en_passant_ - 8U)] != Piece::BlackPawn ||
                board_[static_cast<Square>(en_passant_ + 8U)] != Piece::None) {
                return "en-passant state is inconsistent with a black double push";
            }
        } else if (rank_of(en_passant_) != 2 ||
                   board_[static_cast<Square>(en_passant_ + 8U)] != Piece::WhitePawn ||
                   board_[static_cast<Square>(en_passant_ - 8U)] != Piece::None) {
            return "en-passant state is inconsistent with a white double push";
        }
        if (valid_square(follow_)) {
            const Square expected_follow =
                side_to_move_ == Color::White
                    ? static_cast<Square>(en_passant_ + 8U)
                    : static_cast<Square>(en_passant_ - 8U);
            if (follow_ != expected_follow) {
                return "follow square is inconsistent with the en-passant state";
            }
        }
    }

    if (in_check(opposite(side_to_move_))) {
        return "the side that just moved may not have left its king in check";
    }
    return {};
}

std::string Position::to_fen() const {
    std::string result;
    result.reserve(96);
    for (int rank = 7; rank >= 0; --rank) {
        unsigned empty = 0;
        for (unsigned file = 0; file < 8; ++file) {
            const Piece piece = board_[make_square(file, static_cast<unsigned>(rank))];
            if (piece == Piece::None) {
                ++empty;
                continue;
            }
            if (empty != 0) {
                result.push_back(static_cast<char>('0' + empty));
                empty = 0;
            }
            result.push_back(piece_to_char(piece));
        }
        if (empty != 0) {
            result.push_back(static_cast<char>('0' + empty));
        }
        if (rank != 0) {
            result.push_back('/');
        }
    }

    result += side_to_move_ == Color::White ? " w " : " b ";
    if (castling_rights_ == 0) {
        result.push_back('-');
    } else {
        if ((castling_rights_ & WhiteKingSide) != 0) result.push_back('K');
        if ((castling_rights_ & WhiteQueenSide) != 0) result.push_back('Q');
        if ((castling_rights_ & BlackKingSide) != 0) result.push_back('k');
        if ((castling_rights_ & BlackQueenSide) != 0) result.push_back('q');
    }
    result.push_back(' ');
    result += square_name(en_passant_);
    result.push_back(' ');
    result += std::to_string(halfmove_clock_);
    result.push_back(' ');
    result += std::to_string(fullmove_number_);
    result.push_back(' ');
    result += square_name(follow_);
    return result;
}

}  // namespace zfs
