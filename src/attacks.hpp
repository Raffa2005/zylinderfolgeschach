#pragma once

#include "zfs/types.hpp"

#include <array>
#include <cstdint>

namespace zfs::detail {

struct AttackTables {
    alignas(64) std::array<std::array<std::uint8_t, 256>, 8> rank{};
    alignas(64) std::array<std::array<Bitboard, 64>, 6> ray{};
    std::array<Bitboard, 64> king{};
    std::array<Bitboard, 64> knight{};
    std::array<std::array<Bitboard, 64>, 2> pawn{};
};

extern const AttackTables kAttacks;

[[nodiscard]] Bitboard rook_attacks(Square square, Bitboard occupancy) noexcept;
[[nodiscard]] Bitboard bishop_attacks(Square square, Bitboard occupancy) noexcept;

}  // namespace zfs::detail
