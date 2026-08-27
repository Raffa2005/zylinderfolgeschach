#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace zfs::eval {

using Pentanomial = std::array<std::uint64_t, 5>;

enum class SprtDecision { Continue, AcceptH0, AcceptH1 };

struct Statistics {
    std::uint64_t pairs = 0;
    std::uint64_t games = 0;
    double score = 0.5;
    double elo = 0.0;
    double elo_low = 0.0;
    double elo_high = 0.0;
    double los = 0.5;
    double llr = 0.0;
    double lower_bound = 0.0;
    double upper_bound = 0.0;
    SprtDecision decision = SprtDecision::Continue;
};

[[nodiscard]] Statistics calculate_statistics(
    const Pentanomial& results, double elo0 = 0.0, double elo1 = 5.0,
    double alpha = 0.05, double beta = 0.05);

[[nodiscard]] std::string_view decision_name(SprtDecision decision) noexcept;

}  // namespace zfs::eval
