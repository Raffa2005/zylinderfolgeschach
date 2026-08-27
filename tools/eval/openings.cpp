#include "tools/eval/openings.hpp"

#include "zfs/game.hpp"

#include <array>
#include <bit>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_set>

namespace zfs::eval {
namespace {

[[nodiscard]] int material(const Position& position, Color color) noexcept {
    constexpr std::array values{100, 320, 330, 500, 900, 0};
    int total = 0;
    for (unsigned type = 0; type < values.size(); ++type) {
        total += values[type] * std::popcount(position.pieces(
                                    color, static_cast<PieceType>(type)));
    }
    return total;
}

class SplitMix64 {
public:
    explicit SplitMix64(std::uint64_t seed) noexcept : state_(seed) {}

    [[nodiscard]] std::uint64_t next() noexcept {
        std::uint64_t value = (state_ += 0x9e3779b97f4a7c15ULL);
        value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
        value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
        return value ^ (value >> 31U);
    }

    [[nodiscard]] std::uint64_t bounded(std::uint64_t bound) noexcept {
        if (bound <= 1U) {
            return 0;
        }
        const std::uint64_t threshold = (0U - bound) % bound;
        for (;;) {
            const std::uint64_t value = next();
            if (value >= threshold) {
                return value % bound;
            }
        }
    }

private:
    std::uint64_t state_;
};

}  // namespace

std::string format_opening(const Opening& opening) {
    std::string line;
    for (const std::string& move : opening) {
        if (!line.empty()) {
            line.push_back(' ');
        }
        line += move;
    }
    return line;
}

std::vector<Opening> generate_openings(const OpeningConfig& config) {
    if (config.count == 0U) {
        throw std::invalid_argument("opening count must be positive");
    }
    if (config.minimum_plies == 0U ||
        config.minimum_plies > config.maximum_plies) {
        throw std::invalid_argument("invalid opening ply range");
    }
    if (config.maximum_plies > 256U) {
        throw std::invalid_argument("opening lines are limited to 256 plies");
    }

    SplitMix64 random(config.seed);
    std::vector<Opening> result;
    result.reserve(config.count);
    std::unordered_set<std::string> seen_positions;
    const std::size_t attempt_limit =
        config.count > std::numeric_limits<std::size_t>::max() / 1000U
            ? std::numeric_limits<std::size_t>::max()
            : config.count * 1000U;

    for (std::size_t attempt = 0;
         result.size() < config.count && attempt < attempt_limit; ++attempt) {
        Game game;
        Opening opening;
        const unsigned range = config.maximum_plies - config.minimum_plies + 1U;
        const unsigned target = config.minimum_plies +
            static_cast<unsigned>(random.bounded(range));
        opening.reserve(target);

        bool complete = true;
        for (unsigned ply = 0; ply < target; ++ply) {
            MoveList moves;
            Position position = game.position();
            position.generate_legal_moves(moves);
            if (moves.empty()) {
                complete = false;
                break;
            }
            const Move move = moves[static_cast<std::size_t>(
                random.bounded(static_cast<std::uint64_t>(moves.size())))];
            opening.push_back(move.uci());
            if (!game.play(move)) {
                throw std::logic_error("generated legal opening move was rejected");
            }
            if (game.state() != GameState::Ongoing && ply + 1U < target) {
                complete = false;
                break;
            }
        }
        if (!complete || game.state() != GameState::Ongoing ||
            game.position().in_check(game.position().side_to_move()) ||
            material(game.position(), Color::White) !=
                material(game.position(), Color::Black)) {
            continue;
        }
        if (seen_positions.insert(game.position().to_fen()).second) {
            result.push_back(std::move(opening));
        }
    }

    if (result.size() != config.count) {
        throw std::runtime_error("could not generate enough unique live openings");
    }
    return result;
}

}  // namespace zfs::eval
