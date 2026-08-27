#pragma once

#include "zfs/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace zfs::detail {

struct ZobristTables {
    std::array<std::array<std::uint64_t, 64>, 12> piece{};
    std::uint64_t black_to_move = 0;
    std::array<std::uint64_t, 16> castling{};
    std::array<std::uint64_t, 64> en_passant{};
    std::array<std::uint64_t, 64> follow{};

    consteval ZobristTables() {
        std::uint64_t state = 0x243f6a8885a308d3ULL;
        for (auto& squares : piece) {
            for (std::uint64_t& value : squares) {
                value = next(state);
            }
        }
        black_to_move = next(state);
        castling[0] = 0;
        for (std::size_t index = 1; index < castling.size(); ++index) {
            castling[index] = next(state);
        }
        for (std::uint64_t& value : en_passant) {
            value = next(state);
        }
        for (std::uint64_t& value : follow) {
            value = next(state);
        }
    }

private:
    [[nodiscard]] static consteval std::uint64_t next(
        std::uint64_t& state) noexcept {
        state += 0x9e3779b97f4a7c15ULL;
        std::uint64_t value = state;
        value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
        value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
        return value ^ (value >> 31U);
    }
};

inline constexpr ZobristTables kZobrist{};

[[nodiscard]] constexpr std::uint64_t piece_key(Piece piece,
                                                Square square) noexcept {
    return kZobrist.piece[static_cast<unsigned>(piece) - 1U][square];
}

}  // namespace zfs::detail
