#pragma once

#include <bit>
#include <cstdint>
#include <string_view>

namespace zfs {

using Bitboard = std::uint64_t;
using Square = std::uint8_t;

inline constexpr Square kNoSquare = 64;

enum class Color : std::uint8_t { White = 0, Black = 1 };
enum class PieceType : std::uint8_t {
    Pawn = 0,
    Knight = 1,
    Bishop = 2,
    Rook = 3,
    Queen = 4,
    King = 5,
    None = 6,
};

enum class Piece : std::uint8_t {
    None = 0,
    WhitePawn,
    WhiteKnight,
    WhiteBishop,
    WhiteRook,
    WhiteQueen,
    WhiteKing,
    BlackPawn,
    BlackKnight,
    BlackBishop,
    BlackRook,
    BlackQueen,
    BlackKing,
};

[[nodiscard]] constexpr Color opposite(Color color) noexcept {
    return color == Color::White ? Color::Black : Color::White;
}

[[nodiscard]] constexpr unsigned color_index(Color color) noexcept {
    return static_cast<unsigned>(color);
}

[[nodiscard]] constexpr unsigned type_index(PieceType type) noexcept {
    return static_cast<unsigned>(type);
}

[[nodiscard]] constexpr bool valid_color(Color color) noexcept {
    return color_index(color) < 2U;
}

[[nodiscard]] constexpr bool valid_piece_type(PieceType type) noexcept {
    return type_index(type) < 6U;
}

[[nodiscard]] constexpr bool is_piece(Piece piece) noexcept {
    const unsigned value = static_cast<unsigned>(piece);
    return value >= static_cast<unsigned>(Piece::WhitePawn) &&
           value <= static_cast<unsigned>(Piece::BlackKing);
}

[[nodiscard]] constexpr Color piece_color(Piece piece) noexcept {
    return static_cast<unsigned>(piece) <= static_cast<unsigned>(Piece::WhiteKing)
               ? Color::White
               : Color::Black;
}

[[nodiscard]] constexpr PieceType piece_type(Piece piece) noexcept {
    if (!is_piece(piece)) {
        return PieceType::None;
    }
    const unsigned value = static_cast<unsigned>(piece) - 1U;
    return static_cast<PieceType>(value % 6U);
}

[[nodiscard]] constexpr Piece make_piece(Color color, PieceType type) noexcept {
    if (!valid_color(color) || !valid_piece_type(type)) {
        return Piece::None;
    }
    return static_cast<Piece>(1U + color_index(color) * 6U + type_index(type));
}

[[nodiscard]] constexpr Bitboard square_bb(Square square) noexcept {
    return square < 64 ? Bitboard{1} << square : Bitboard{0};
}

[[nodiscard]] constexpr unsigned file_of(Square square) noexcept {
    return static_cast<unsigned>(square) & 7U;
}

[[nodiscard]] constexpr unsigned rank_of(Square square) noexcept {
    return static_cast<unsigned>(square) >> 3U;
}

[[nodiscard]] constexpr Square make_square(unsigned file, unsigned rank) noexcept {
    return static_cast<Square>(rank * 8U + (file & 7U));
}

[[nodiscard]] constexpr Square pop_lsb(Bitboard& board) noexcept {
    if (board == 0) {
        return kNoSquare;
    }
    const auto square = static_cast<Square>(std::countr_zero(board));
    board &= board - 1U;
    return square;
}

[[nodiscard]] constexpr bool valid_square(Square square) noexcept {
    return square < 64;
}

[[nodiscard]] constexpr Square parse_square(std::string_view text) noexcept {
    if (text.size() != 2 || text[0] < 'a' || text[0] > 'h' || text[1] < '1' ||
        text[1] > '8') {
        return kNoSquare;
    }
    return make_square(static_cast<unsigned>(text[0] - 'a'),
                       static_cast<unsigned>(text[1] - '1'));
}

}  // namespace zfs
