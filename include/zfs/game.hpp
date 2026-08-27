#pragma once

#include "zfs/position.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace zfs {

enum class GameState : std::uint8_t {
    Ongoing,
    Checkmate,
    Stalemate,
    ThreefoldDraw,
    FiftyMoveDraw,
};

class Game {
public:
    Game();
    explicit Game(Position root);

    void reset(Position root);

    [[nodiscard]] const Position& position() const noexcept { return position_; }
    [[nodiscard]] std::span<const Position> positions() const noexcept {
        return positions_;
    }
    [[nodiscard]] std::span<const Move> moves() const noexcept { return moves_; }

    [[nodiscard]] bool play(Move move);
    [[nodiscard]] std::optional<Move> play_uci(std::string_view uci);
    [[nodiscard]] unsigned repetition_count() const;
    [[nodiscard]] GameState state();

private:
    [[nodiscard]] bool automatic_draw() const;
    void commit(Move move);

    Position position_;
    std::vector<Position> positions_;
    std::vector<Move> moves_;
};

}  // namespace zfs
