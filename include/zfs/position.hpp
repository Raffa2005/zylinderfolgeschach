#pragma once

#include "zfs/move.hpp"
#include "zfs/types.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace zfs {

enum CastlingRight : std::uint8_t {
    WhiteKingSide = 1U << 0U,
    WhiteQueenSide = 1U << 1U,
    BlackKingSide = 1U << 2U,
    BlackQueenSide = 1U << 3U,
};

enum class TerminalState : std::uint8_t { Ongoing, Checkmate, Stalemate };

struct Undo {
    Piece captured = Piece::None;
    std::uint8_t castling_rights = 0;
    Square en_passant = kNoSquare;
    Square follow = kNoSquare;
    std::uint16_t halfmove_clock = 0;
    std::uint32_t fullmove_number = 1;
    std::uint64_t key = 0;
};

struct NullUndo {
    Square en_passant = kNoSquare;
    Square follow = kNoSquare;
    std::uint16_t halfmove_clock = 0;
    std::uint32_t fullmove_number = 1;
    std::uint64_t key = 0;
};

class Position {
public:
    Position(const Position&) noexcept = default;
    Position(Position&&) noexcept = default;
    Position& operator=(const Position&) noexcept = default;
    Position& operator=(Position&&) noexcept = default;

    [[nodiscard]] static Position start();
    [[nodiscard]] static std::optional<Position> from_fen(std::string_view fen,
                                                          std::string* error = nullptr);
    [[nodiscard]] std::string to_fen() const;
    [[nodiscard]] std::string validate() const;

    [[nodiscard]] Color side_to_move() const noexcept { return side_to_move_; }
    [[nodiscard]] Piece piece_at(Square square) const noexcept {
        return valid_square(square) ? board_[square] : Piece::None;
    }
    [[nodiscard]] Bitboard pieces(Color color, PieceType type) const noexcept {
        if (!valid_color(color) || !valid_piece_type(type)) {
            return 0;
        }
        return pieces_[color_index(color)][type_index(type)];
    }
    [[nodiscard]] Bitboard occupied(Color color) const noexcept {
        return valid_color(color) ? colors_[color_index(color)] : Bitboard{0};
    }
    [[nodiscard]] Bitboard occupied() const noexcept { return occupied_; }
    [[nodiscard]] std::uint8_t castling_rights() const noexcept {
        return castling_rights_;
    }
    [[nodiscard]] Square en_passant_square() const noexcept { return en_passant_; }
    [[nodiscard]] Square follow_square() const noexcept { return follow_; }
    [[nodiscard]] std::uint16_t halfmove_clock() const noexcept {
        return halfmove_clock_;
    }
    [[nodiscard]] std::uint32_t fullmove_number() const noexcept {
        return fullmove_number_;
    }
    [[nodiscard]] std::uint64_t raw_key() const noexcept { return key_; }
    [[nodiscard]] std::uint64_t base_key() const noexcept;
    [[nodiscard]] bool same_repetition_state(const Position& other) const;

    [[nodiscard]] bool is_square_attacked(Square square, Color by) const noexcept;
    [[nodiscard]] bool in_check(Color color) const noexcept;

    void generate_legal_moves(MoveList& result);
    [[nodiscard]] MoveList legal_moves();
    [[nodiscard]] std::optional<Move> parse_uci(std::string_view uci);
    [[nodiscard]] bool is_legal(Move move);
    [[nodiscard]] TerminalState terminal_state();

    // These are the engine hot-path make/unmake operations. The caller must pass
    // a move produced by generate_legal_moves().
    void do_move(Move move, Undo& undo) noexcept;
    void undo_move(Move move, const Undo& undo) noexcept;

    // Search-only synthetic pass. This is never a legal ZFS move.
    void do_null_move(NullUndo& undo) noexcept;
    void undo_null_move(const NullUndo& undo) noexcept;

private:
    Position() noexcept = default;

    template <bool UpdateKey>
    void put_piece_impl(Square square, Piece piece) noexcept;
    template <bool UpdateKey>
    [[nodiscard]] Piece take_piece_impl(Square square) noexcept;
    void put_piece(Square square, Piece piece) noexcept;
    [[nodiscard]] Piece take_piece(Square square) noexcept;
    void relocate_piece(Square from, Square to) noexcept;
    void put_piece_unhashed(Square square, Piece piece) noexcept;
    [[nodiscard]] Piece take_piece_unhashed(Square square) noexcept;
    void relocate_piece_unhashed(Square from, Square to) noexcept;

    [[nodiscard]] Bitboard rook_attacks(Square square) const noexcept;
    [[nodiscard]] Bitboard bishop_attacks(Square square) const noexcept;
    [[nodiscard]] Square king_square(Color color) const noexcept;
    [[nodiscard]] bool castle_transit_safe(Color color, Square transit) noexcept;
    [[nodiscard]] bool move_lands_on(Move move, Square target) const noexcept;
    [[nodiscard]] bool leaves_king_safe(Move move) noexcept;

    void generate_pseudo_legal(MoveList& result) noexcept;
    void generate_pseudo_to(MoveList& result, Square target) noexcept;
    void generate_pawn_moves(MoveList& result, Color color) noexcept;
    void generate_piece_moves(MoveList& result, Color color, PieceType type) noexcept;
    void generate_castles(MoveList& result, Color color) noexcept;
    [[nodiscard]] std::uint64_t non_piece_key() const noexcept;

    std::array<std::array<Bitboard, 6>, 2> pieces_{};
    std::array<Bitboard, 2> colors_{};
    Bitboard occupied_ = 0;
    std::array<Piece, 64> board_{};

    Color side_to_move_ = Color::White;
    std::uint8_t castling_rights_ = 0;
    Square en_passant_ = kNoSquare;
    Square follow_ = kNoSquare;
    std::uint16_t halfmove_clock_ = 0;
    std::uint32_t fullmove_number_ = 1;
    std::uint64_t key_ = 0;
};

}  // namespace zfs
