#include "engine/evaluate.hpp"

#include "src/attacks.hpp"

#include <array>
#include <bit>

namespace zfs::engine {
namespace {

constexpr std::array kPieceValues{100, 320, 330, 500, 900, 0};
// Roughly 1/16 of material value. This is a horizon term, not a replacement
// for the compulsory move that search will examine when it reaches it.
constexpr std::array kFollowerBurden{6, 20, 20, 31, 56, 20};
constexpr int kShadowMultiplier = 2;
constexpr Bitboard kRank1 = 0x00000000000000ffULL;
constexpr Bitboard kRank2 = 0x000000000000ff00ULL;
constexpr Bitboard kRank7 = 0x00ff000000000000ULL;
constexpr Bitboard kRank8 = 0xff00000000000000ULL;

[[nodiscard]] Bitboard attacks_from(Square square, PieceType type,
                                    Bitboard occupancy) noexcept {
    switch (type) {
        case PieceType::Knight:
            return detail::kAttacks.knight[square];
        case PieceType::Bishop:
            return detail::bishop_attacks(square, occupancy);
        case PieceType::Rook:
            return detail::rook_attacks(square, occupancy);
        case PieceType::Queen:
            return detail::bishop_attacks(square, occupancy) |
                   detail::rook_attacks(square, occupancy);
        default:
            return 0;
    }
}

[[nodiscard]] std::array<Bitboard, 6> follower_targets(
    const Position& position, Color color) noexcept {
    std::array<Bitboard, 6> result{};
    const Bitboard occupancy = position.occupied();

    const Bitboard pawns = position.pieces(color, PieceType::Pawn);
    // The destination is occupied by the prospective leader now and becomes
    // empty when it moves. Only a double push's transit square must be empty.
    if (color == Color::White) {
        result[type_index(PieceType::Pawn)] = pawns << 8U;
        const Bitboard transit = ((pawns & kRank2) << 8U) & ~occupancy;
        result[type_index(PieceType::Pawn)] |= transit << 8U;
        result[type_index(PieceType::Pawn)] &= ~kRank8;
    } else {
        result[type_index(PieceType::Pawn)] = pawns >> 8U;
        const Bitboard transit = ((pawns & kRank7) >> 8U) & ~occupancy;
        result[type_index(PieceType::Pawn)] |= transit >> 8U;
        result[type_index(PieceType::Pawn)] &= ~kRank1;
    }

    for (PieceType type : {PieceType::Knight, PieceType::Bishop,
                           PieceType::Rook, PieceType::Queen}) {
        Bitboard pieces = position.pieces(color, type);
        Bitboard targets = 0;
        while (pieces != 0) {
            targets |= attacks_from(pop_lsb(pieces), type, occupancy);
        }
        result[type_index(type)] = targets;
    }

    const Bitboard king = position.pieces(color, PieceType::King);
    if (king != 0) {
        result[type_index(PieceType::King)] =
            detail::kAttacks.king[static_cast<Square>(std::countr_zero(king))];
    }
    return result;
}

[[nodiscard]] constexpr bool can_shadow(PieceType leader,
                                        PieceType follower) noexcept {
    switch (leader) {
        case PieceType::Pawn:
            return follower == PieceType::Rook ||
                   follower == PieceType::Queen ||
                   follower == PieceType::King;
        case PieceType::Knight:
            return follower == PieceType::Knight;
        case PieceType::Bishop:
            return follower == PieceType::Bishop ||
                   follower == PieceType::Queen;
        case PieceType::Rook:
            return follower == PieceType::Rook ||
                   follower == PieceType::Queen;
        case PieceType::Queen:
            return follower == PieceType::Queen;
        case PieceType::King:
            return follower == PieceType::Bishop ||
                   follower == PieceType::Rook ||
                   follower == PieceType::Queen;
        default:
            return false;
    }
}

[[nodiscard]] constexpr int follower_burden(PieceType leader,
                                            PieceType follower) noexcept {
    const int burden = kFollowerBurden[type_index(follower)];
    return can_shadow(leader, follower) ? kShadowMultiplier * burden : burden;
}

[[nodiscard]] consteval std::array<std::array<unsigned, 6>, 6>
make_follower_order() noexcept {
    std::array<std::array<unsigned, 6>, 6> result{};
    for (unsigned leader_index = 0; leader_index < 6U; ++leader_index) {
        auto& row = result[leader_index];
        for (unsigned follower_index = 0; follower_index < 6U;
             ++follower_index) {
            row[follower_index] = follower_index;
        }
        const auto leader = static_cast<PieceType>(leader_index);
        for (unsigned index = 1; index < row.size(); ++index) {
            const unsigned follower = row[index];
            unsigned insertion = index;
            while (insertion > 0 &&
                   follower_burden(
                       leader, static_cast<PieceType>(row[insertion - 1U])) >
                       follower_burden(
                           leader, static_cast<PieceType>(follower))) {
                row[insertion] = row[insertion - 1U];
                --insertion;
            }
            row[insertion] = follower;
        }
    }
    return result;
}

[[nodiscard]] int leader_initiative(const Position& position) noexcept {
    const Color leader_color = position.side_to_move();
    const auto targets = follower_targets(position, opposite(leader_color));
    // Ascending follower burden for each leader type. The compelled side may
    // choose any legal follower, so a square is charged only once, at its
    // least costly pseudo-follower.
    constexpr auto order = make_follower_order();

    int result = 0;
    for (unsigned leader_index = 0; leader_index < 6U; ++leader_index) {
        const auto leader = static_cast<PieceType>(leader_index);
        Bitboard remaining = position.pieces(leader_color, leader);
        for (unsigned follower_index : order[leader_index]) {
            if (follower_index == type_index(PieceType::King) &&
                leader != PieceType::Pawn) {
                continue;
            }
            const Bitboard matched = remaining & targets[follower_index];
            const auto follower = static_cast<PieceType>(follower_index);
            result += std::popcount(matched) *
                      follower_burden(leader, follower);
            remaining &= ~matched;
        }
    }
    return result;
}

}  // namespace

int evaluate_material(const Position& position) noexcept {
    int white_minus_black = 0;
    for (unsigned type = 0; type < kPieceValues.size(); ++type) {
        const auto piece_type = static_cast<PieceType>(type);
        const int white = std::popcount(position.pieces(Color::White, piece_type));
        const int black = std::popcount(position.pieces(Color::Black, piece_type));
        white_minus_black += (white - black) * kPieceValues[type];
    }
    return position.side_to_move() == Color::White ? white_minus_black
                                                    : -white_minus_black;
}

int evaluate(const Position& position) noexcept {
    return evaluate_material(position) + leader_initiative(position);
}

}  // namespace zfs::engine
