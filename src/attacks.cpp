#include "attacks.hpp"

#include <array>
#include <bit>
#include <cstdint>

namespace zfs::detail {
namespace {

[[nodiscard]] consteval unsigned wrap_file(int file) noexcept {
    return static_cast<unsigned>((file + 8) & 7);
}

[[nodiscard]] consteval std::uint8_t cyclic_attacks(unsigned origin,
                                                    unsigned occupancy) noexcept {
    std::uint8_t result = 0;
    for (int direction : {-1, 1}) {
        int file = static_cast<int>(origin);
        for (unsigned distance = 1; distance < 8; ++distance) {
            file = (file + direction + 8) & 7;
            const auto bit = static_cast<std::uint8_t>(
                1U << static_cast<unsigned>(file));
            result |= bit;
            if ((occupancy & bit) != 0) {
                break;
            }
        }
    }
    return result;
}

[[nodiscard]] consteval AttackTables make_attacks() noexcept {
    AttackTables tables{};
    constexpr std::array<std::array<int, 2>, 6> kRayDeltas{{
        {{0, 1}}, {{0, -1}}, {{1, 1}}, {{-1, 1}},
        {{1, -1}}, {{-1, -1}},
    }};
    constexpr std::array<std::array<int, 2>, 8> kKnightDeltas{{
        {{-2, -1}}, {{-2, 1}}, {{-1, -2}}, {{-1, 2}},
        {{1, -2}},  {{1, 2}},  {{2, -1}},  {{2, 1}},
    }};

    for (unsigned file = 0; file < 8; ++file) {
        for (unsigned occupancy = 0; occupancy < 256; ++occupancy) {
            tables.rank[file][occupancy] =
                cyclic_attacks(file, static_cast<std::uint8_t>(occupancy));
        }
    }

    for (unsigned square_value = 0; square_value < 64; ++square_value) {
        const auto square = static_cast<Square>(square_value);
        const unsigned file = file_of(square);
        const unsigned rank = rank_of(square);

        for (unsigned direction = 0; direction < kRayDeltas.size(); ++direction) {
            int ray_file = static_cast<int>(file);
            int ray_rank = static_cast<int>(rank);
            for (unsigned distance = 1; distance < 8; ++distance) {
                ray_file = (ray_file + kRayDeltas[direction][0] + 8) & 7;
                ray_rank += kRayDeltas[direction][1];
                if (ray_rank < 0 || ray_rank >= 8) {
                    break;
                }
                tables.ray[direction][square] |= square_bb(make_square(
                    static_cast<unsigned>(ray_file),
                    static_cast<unsigned>(ray_rank)));
            }
        }

        Bitboard king = 0;
        for (int rank_delta = -1; rank_delta <= 1; ++rank_delta) {
            const int target_rank = static_cast<int>(rank) + rank_delta;
            if (target_rank < 0 || target_rank >= 8) {
                continue;
            }
            for (int file_delta = -1; file_delta <= 1; ++file_delta) {
                if (rank_delta == 0 && file_delta == 0) {
                    continue;
                }
                king |= square_bb(make_square(
                    wrap_file(static_cast<int>(file) + file_delta),
                    static_cast<unsigned>(target_rank)));
            }
        }
        tables.king[square] = king;

        Bitboard knight = 0;
        for (const auto& delta : kKnightDeltas) {
            const int target_rank = static_cast<int>(rank) + delta[1];
            if (target_rank < 0 || target_rank >= 8) {
                continue;
            }
            knight |= square_bb(make_square(
                wrap_file(static_cast<int>(file) + delta[0]),
                static_cast<unsigned>(target_rank)));
        }
        tables.knight[square] = knight;

        if (rank < 7) {
            tables.pawn[color_index(Color::White)][square] =
                square_bb(make_square((file + 7U) & 7U, rank + 1U)) |
                square_bb(make_square((file + 1U) & 7U, rank + 1U));
        }
        if (rank > 0) {
            tables.pawn[color_index(Color::Black)][square] =
                square_bb(make_square((file + 7U) & 7U, rank - 1U)) |
                square_bb(make_square((file + 1U) & 7U, rank - 1U));
        }
    }

    return tables;
}

}  // namespace

constinit const AttackTables kAttacks = make_attacks();

namespace {

[[nodiscard]] Bitboard positive_ray(unsigned direction, Square square,
                                    Bitboard occupancy) noexcept {
    Bitboard result = kAttacks.ray[direction][square];
    const Bitboard blockers = result & occupancy;
    if (blockers != 0) {
        const Square first = static_cast<Square>(std::countr_zero(blockers));
        result &= ~kAttacks.ray[direction][first];
    }
    return result;
}

[[nodiscard]] Bitboard negative_ray(unsigned direction, Square square,
                                    Bitboard occupancy) noexcept {
    Bitboard result = kAttacks.ray[direction][square];
    const Bitboard blockers = result & occupancy;
    if (blockers != 0) {
        const Square first = static_cast<Square>(63U - std::countl_zero(blockers));
        result &= ~kAttacks.ray[direction][first];
    }
    return result;
}

}  // namespace

Bitboard rook_attacks(Square square, Bitboard occupancy) noexcept {
    const unsigned rank = rank_of(square);
    const auto rank_occupancy = static_cast<std::uint8_t>(
        (occupancy >> (rank * 8U)) & Bitboard{0xff});
    const Bitboard horizontal =
        static_cast<Bitboard>(kAttacks.rank[file_of(square)][rank_occupancy])
        << (rank * 8U);
    return horizontal | positive_ray(0, square, occupancy) |
           negative_ray(1, square, occupancy);
}

Bitboard bishop_attacks(Square square, Bitboard occupancy) noexcept {
    return positive_ray(2, square, occupancy) |
           positive_ray(3, square, occupancy) |
           negative_ray(4, square, occupancy) |
           negative_ray(5, square, occupancy);
}

}  // namespace zfs::detail
