#include "tools/eval/openings.hpp"
#include "tools/eval/stats.hpp"
#include "zfs/game.hpp"

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

int failures = 0;

#define CHECK(expression)                                                        \
    do {                                                                         \
        if (!(expression)) {                                                     \
            std::cerr << __FILE__ << ':' << __LINE__ << ": CHECK failed: "      \
                      << #expression << '\n';                                    \
            ++failures;                                                          \
        }                                                                        \
    } while (false)

void test_opening_generation() {
    zfs::eval::OpeningConfig config;
    config.count = 32;
    config.minimum_plies = 6;
    config.maximum_plies = 12;
    config.seed = 0x123456789abcdef0ULL;
    const auto first = zfs::eval::generate_openings(config);
    const auto second = zfs::eval::generate_openings(config);
    CHECK(first == second);
    CHECK(first.size() == config.count);

    std::set<std::string> unique;
    for (const auto& opening : first) {
        CHECK(opening.size() >= config.minimum_plies);
        CHECK(opening.size() <= config.maximum_plies);
        zfs::Game game;
        for (const std::string& move : opening) {
            CHECK(game.play_uci(move).has_value());
        }
        CHECK(game.state() == zfs::GameState::Ongoing);
        unique.insert(zfs::eval::format_opening(opening));
    }
    CHECK(unique.size() == config.count);
}

[[nodiscard]] std::set<std::string> validate_committed_suite(
    const std::string& filename, const zfs::eval::OpeningConfig& config) {
    std::ifstream input(std::string(ZFS_SOURCE_DIR) + "/openings/" + filename);
    CHECK(input.good());
    std::string line;
    CHECK(std::getline(input, line));
    CHECK(line == "# zfs-openings-v1 seed=" + std::to_string(config.seed) +
                      " min_plies=" + std::to_string(config.minimum_plies) +
                      " max_plies=" + std::to_string(config.maximum_plies));
    unsigned lines = 0;
    std::vector<std::string> recorded;
    std::set<std::string> positions;
    while (std::getline(input, line)) {
        const auto comment = line.find('#');
        if (comment != std::string::npos) {
            line.erase(comment);
        }
        std::istringstream moves(line);
        std::string move;
        zfs::Game game;
        bool any = false;
        while (moves >> move) {
            any = true;
            CHECK(game.play_uci(move).has_value());
        }
        if (any) {
            CHECK(game.state() == zfs::GameState::Ongoing);
            recorded.push_back(line);
            positions.insert(game.position().to_fen());
            ++lines;
        }
    }
    CHECK(lines == config.count);
    CHECK(positions.size() == config.count);

    std::vector<std::string> regenerated;
    for (const auto& opening : zfs::eval::generate_openings(config)) {
        regenerated.push_back(zfs::eval::format_opening(opening));
    }
    CHECK(recorded == regenerated);
    return positions;
}

void test_statistics() {
    const zfs::eval::Pentanomial symmetric{10, 20, 30, 20, 10};
    const auto neutral = zfs::eval::calculate_statistics(symmetric);
    CHECK(neutral.pairs == 90U);
    CHECK(neutral.games == 180U);
    CHECK(std::abs(neutral.score - 0.5) < 1.0e-15);
    CHECK(std::abs(neutral.elo) < 1.0e-12);
    CHECK(std::abs(neutral.los - 0.5) < 1.0e-12);
    CHECK(neutral.llr < 0.0);

    const zfs::eval::Pentanomial winning{0, 1, 10, 30, 80};
    const auto strong = zfs::eval::calculate_statistics(winning);
    CHECK(strong.score > 0.5);
    CHECK(strong.elo > 0.0);
    CHECK(strong.los > 0.99);
    CHECK(strong.llr > neutral.llr);

    const auto one_result =
        zfs::eval::calculate_statistics({0, 0, 0, 0, 1});
    CHECK(std::isnan(one_result.los));
    CHECK(std::isnan(one_result.elo_low));
    CHECK(std::isnan(one_result.elo_high));
    const auto ten_wins =
        zfs::eval::calculate_statistics({0, 0, 0, 0, 10});
    CHECK(std::isnan(ten_wins.elo_low));
    CHECK(std::isnan(ten_wins.elo_high));
    CHECK(std::isnan(ten_wins.los));
    const auto ten_middle =
        zfs::eval::calculate_statistics({0, 0, 10, 0, 0});
    CHECK(std::isnan(ten_middle.elo_low));
    CHECK(std::isnan(ten_middle.elo_high));

    // Cross-checked against official-stockfish/fishtest LLRcalc.py.
    const zfs::eval::Pentanomial fixture{12, 34, 56, 78, 90};
    const auto reference =
        zfs::eval::calculate_statistics(fixture, 0.0, 5.0, 0.05, 0.05);
    CHECK(std::abs(reference.llr - 2.937422063543046) < 1.0e-12);

    const auto empty = zfs::eval::calculate_statistics({});
    CHECK(empty.pairs == 0U);
    CHECK(empty.llr == 0.0);
    CHECK(std::isnan(empty.elo_low));
    CHECK(std::isnan(empty.elo_high));
    CHECK(std::isnan(empty.los));

    bool rejected = false;
    try {
        (void)zfs::eval::calculate_statistics(symmetric, 5.0, 0.0);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    CHECK(rejected);

    rejected = false;
    try {
        (void)zfs::eval::calculate_statistics(symmetric, 0.0, 5.0, 0.5,
                                              0.5);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    CHECK(rejected);

    rejected = false;
    try {
        (void)zfs::eval::calculate_statistics(
            {std::numeric_limits<std::uint64_t>::max(), 1, 0, 0, 0});
    } catch (const std::overflow_error&) {
        rejected = true;
    }
    CHECK(rejected);
}

}  // namespace

int main() {
    test_opening_generation();
    zfs::eval::OpeningConfig screen;
    screen.count = 32;
    screen.minimum_plies = 8;
    screen.maximum_plies = 16;
    screen.seed = 6510615555426900570ULL;
    zfs::eval::OpeningConfig holdout = screen;
    holdout.seed = 11936128518282651045ULL;
    const auto screen_positions =
        validate_committed_suite("screen-v1.txt", screen);
    const auto holdout_positions =
        validate_committed_suite("holdout-v1.txt", holdout);
    for (const std::string& position : screen_positions) {
        CHECK(!holdout_positions.contains(position));
    }
    test_statistics();
    if (failures != 0) {
        std::cerr << failures << " evaluation test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "all evaluation tests passed\n";
    return EXIT_SUCCESS;
}
