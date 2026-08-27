#pragma once

#include "zfs/types.hpp"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string>

namespace zfs {

class Position;

enum class MoveType : std::uint8_t {
    Quiet = 0,
    DoublePawnPush,
    KingCastle,
    QueenCastle,
    Capture,
    EnPassant,
    Promotion,
    PromotionCapture,
};

class Move {
public:
    constexpr Move() noexcept = default;

    constexpr Move(Square from, Square to, MoveType type = MoveType::Quiet,
                   PieceType promotion = PieceType::Knight) noexcept
        : bits_(static_cast<std::uint32_t>(from) |
                (static_cast<std::uint32_t>(to) << 6U) |
                (static_cast<std::uint32_t>(type) << 12U) |
                (promotion_code(promotion) << 15U)) {}

    [[nodiscard]] static constexpr Move from_raw(std::uint32_t raw) noexcept {
        Move move;
        move.bits_ = raw;
        return move;
    }

    [[nodiscard]] constexpr Square from() const noexcept {
        return static_cast<Square>(bits_ & 0x3fU);
    }

    [[nodiscard]] constexpr Square to() const noexcept {
        return static_cast<Square>((bits_ >> 6U) & 0x3fU);
    }

    [[nodiscard]] constexpr MoveType type() const noexcept {
        return static_cast<MoveType>((bits_ >> 12U) & 0x7U);
    }

    [[nodiscard]] constexpr PieceType promotion() const noexcept {
        constexpr std::array types{PieceType::Knight, PieceType::Bishop,
                                   PieceType::Rook, PieceType::Queen};
        return types[(bits_ >> 15U) & 0x3U];
    }

    [[nodiscard]] constexpr bool is_capture() const noexcept {
        return type() == MoveType::Capture || type() == MoveType::EnPassant ||
               type() == MoveType::PromotionCapture;
    }

    [[nodiscard]] constexpr bool is_castle() const noexcept {
        return type() == MoveType::KingCastle || type() == MoveType::QueenCastle;
    }

    [[nodiscard]] constexpr bool is_promotion() const noexcept {
        return type() == MoveType::Promotion ||
               type() == MoveType::PromotionCapture;
    }

    [[nodiscard]] constexpr std::uint32_t raw() const noexcept { return bits_; }

    [[nodiscard]] std::string uci() const;

    friend constexpr bool operator==(Move, Move) noexcept = default;

private:
    [[nodiscard]] static constexpr std::uint32_t promotion_code(
        PieceType type) noexcept {
        switch (type) {
            case PieceType::Knight: return 0;
            case PieceType::Bishop: return 1;
            case PieceType::Rook: return 2;
            case PieceType::Queen: return 3;
            default: return 0;
        }
    }

    std::uint32_t bits_ = 0;
};

class MoveList {
public:
    static constexpr std::size_t kCapacity = 512;

    constexpr void clear() noexcept { size_ = 0; }
    [[nodiscard]] constexpr std::size_t size() const noexcept { return size_; }
    [[nodiscard]] constexpr bool empty() const noexcept { return size_ == 0; }

    [[nodiscard]] constexpr Move& operator[](std::size_t index) noexcept {
        assert(index < size_);
        return moves_[index];
    }

    [[nodiscard]] constexpr const Move& operator[](std::size_t index) const noexcept {
        assert(index < size_);
        return moves_[index];
    }

    [[nodiscard]] constexpr Move* begin() noexcept { return moves_.data(); }
    [[nodiscard]] constexpr Move* end() noexcept { return moves_.data() + size_; }
    [[nodiscard]] constexpr const Move* begin() const noexcept { return moves_.data(); }
    [[nodiscard]] constexpr const Move* end() const noexcept {
        return moves_.data() + size_;
    }

private:
    friend class Position;

    constexpr void push(Move move) noexcept {
        assert(size_ < kCapacity);
        moves_[size_++] = move;
    }

    std::array<Move, kCapacity> moves_{};
    std::size_t size_ = 0;
};

}  // namespace zfs
