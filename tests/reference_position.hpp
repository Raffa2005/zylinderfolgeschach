#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace zfs::test_oracle {

// A deliberately plain rules model for differential testing. It uses a
// character mailbox and walks coordinates at runtime; no production move or
// attack representation is shared with it.
class Position {
public:
    [[nodiscard]] static std::optional<Position> from_fen(
        std::string_view fen, std::string* error = nullptr);

    [[nodiscard]] std::string to_fen() const;
    [[nodiscard]] std::vector<std::string> legal_uci() const;
    [[nodiscard]] std::optional<Position> after_uci(std::string_view uci) const;

private:
    enum class Side : std::uint8_t { White, Black };
    enum class MoveKind : std::uint8_t {
        Quiet,
        DoublePawnPush,
        KingCastle,
        QueenCastle,
        Capture,
        EnPassant,
        Promotion,
        PromotionCapture,
    };

    struct Move {
        int from = -1;
        int to = -1;
        MoveKind kind = MoveKind::Quiet;
        char promotion = 'n';

        friend bool operator==(const Move&, const Move&) = default;
    };

    [[nodiscard]] static Side opposite(Side side) noexcept;
    [[nodiscard]] static bool belongs_to(char piece, Side side) noexcept;
    [[nodiscard]] static int square(int file, int rank) noexcept;
    [[nodiscard]] static int file_of(int square) noexcept;
    [[nodiscard]] static int rank_of(int square) noexcept;

    [[nodiscard]] bool attacked(int target, Side by) const noexcept;
    [[nodiscard]] bool ray_attacks(int from, int target, int file_delta,
                                   int rank_delta) const noexcept;
    [[nodiscard]] int king_square(Side side) const noexcept;
    [[nodiscard]] std::vector<Move> pseudo_legal_moves() const;
    [[nodiscard]] std::vector<Move> ordinary_legal_moves() const;
    [[nodiscard]] std::vector<Move> legal_moves() const;
    [[nodiscard]] Position apply_unchecked(const Move& move) const noexcept;
    [[nodiscard]] std::string move_uci(const Move& move) const;

    void add_move(std::vector<Move>& moves, Move move) const;
    void add_piece_target(std::vector<Move>& moves, int from, int to) const;
    void add_slider_moves(std::vector<Move>& moves, int from, int file_delta,
                          int rank_delta) const;
    void add_castles(std::vector<Move>& moves) const;

    std::array<char, 64> board_{};
    Side side_to_move_ = Side::White;
    std::uint8_t castling_rights_ = 0;
    int en_passant_ = -1;
    int follow_ = -1;
    std::uint16_t halfmove_clock_ = 0;
    std::uint32_t fullmove_number_ = 1;
};

}  // namespace zfs::test_oracle
