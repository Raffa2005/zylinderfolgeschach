#include "engine/evaluate.hpp"

#include <array>
#include <bit>

namespace zfs::engine {

int evaluate_material(const Position& position) noexcept {
    constexpr std::array values{100, 320, 330, 500, 900, 0};
    int white_minus_black = 0;
    for (unsigned type = 0; type < values.size(); ++type) {
        const auto piece_type = static_cast<PieceType>(type);
        const int white = std::popcount(position.pieces(Color::White, piece_type));
        const int black = std::popcount(position.pieces(Color::Black, piece_type));
        white_minus_black += (white - black) * values[type];
    }
    return position.side_to_move() == Color::White ? white_minus_black
                                                    : -white_minus_black;
}

}  // namespace zfs::engine
