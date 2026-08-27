#include "tools/eval/stats.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numeric>
#include <stdexcept>

namespace zfs::eval {
namespace {

constexpr std::array<double, 5> kOutcomes{0.0, 0.25, 0.5, 0.75, 1.0};
constexpr double kPrior = 1.0e-3;
constexpr double kNormal95 = 1.959963984540054;

[[nodiscard]] double elo_to_score(double elo) noexcept {
    return 1.0 / (1.0 + std::pow(10.0, -elo / 400.0));
}

[[nodiscard]] double score_to_elo(double score) noexcept {
    if (score <= 0.0) {
        return -std::numeric_limits<double>::infinity();
    }
    if (score >= 1.0) {
        return std::numeric_limits<double>::infinity();
    }
    return 400.0 * std::log10(score / (1.0 - score));
}

using Pdf = std::array<double, 5>;

[[nodiscard]] double expectation(const Pdf& pdf) noexcept {
    double result = 0.0;
    for (std::size_t i = 0; i < pdf.size(); ++i) {
        result += pdf[i] * kOutcomes[i];
    }
    return result;
}

// Multinomial maximum-likelihood distribution subject to a fixed mean.
// This is the same secular equation used by Stockfish Fishtest's logistic
// pentanomial GSPRT, implemented here to keep the tool dependency-free.
[[nodiscard]] Pdf mle_with_mean(const Pdf& empirical, double target) {
    if (!(target > 0.0 && target < 1.0)) {
        throw std::invalid_argument("SPRT target score must be between zero and one");
    }

    const double empirical_mean = expectation(empirical);
    if (std::abs(empirical_mean - target) < 1.0e-14) {
        return empirical;
    }

    double most_negative = 0.0;
    double most_positive = 0.0;
    for (double outcome : kOutcomes) {
        const double shifted = outcome - target;
        most_negative = std::min(most_negative, shifted);
        most_positive = std::max(most_positive, shifted);
    }
    double lower = -1.0 / most_positive;
    double upper = -1.0 / most_negative;
    lower = std::nextafter(lower, upper);
    upper = std::nextafter(upper, lower);

    const auto secular = [&](double x) noexcept {
        double value = 0.0;
        for (std::size_t i = 0; i < empirical.size(); ++i) {
            const double shifted = kOutcomes[i] - target;
            value += empirical[i] * shifted / (1.0 + x * shifted);
        }
        return value;
    };

    // The secular function is strictly decreasing inside this interval.
    for (int iteration = 0; iteration < 160; ++iteration) {
        const double middle = std::midpoint(lower, upper);
        if (secular(middle) > 0.0) {
            lower = middle;
        } else {
            upper = middle;
        }
    }
    const double root = std::midpoint(lower, upper);

    Pdf result{};
    double sum = 0.0;
    for (std::size_t i = 0; i < result.size(); ++i) {
        result[i] = empirical[i] /
                    (1.0 + root * (kOutcomes[i] - target));
        sum += result[i];
    }
    for (double& probability : result) {
        probability /= sum;
    }
    return result;
}

[[nodiscard]] double logistic_llr(const Pentanomial& results, double elo0,
                                  double elo1) {
    Pdf empirical{};
    double count = 0.0;
    for (std::size_t i = 0; i < results.size(); ++i) {
        empirical[i] = results[i] == 0 ? kPrior
                                        : static_cast<double>(results[i]);
        count += empirical[i];
    }
    for (double& probability : empirical) {
        probability /= count;
    }

    const Pdf under_h0 = mle_with_mean(empirical, elo_to_score(elo0));
    const Pdf under_h1 = mle_with_mean(empirical, elo_to_score(elo1));
    double llr_per_pair = 0.0;
    for (std::size_t i = 0; i < empirical.size(); ++i) {
        llr_per_pair += empirical[i] *
                        (std::log(under_h1[i]) - std::log(under_h0[i]));
    }
    return count * llr_per_pair;
}

}  // namespace

Statistics calculate_statistics(const Pentanomial& results, double elo0,
                                double elo1, double alpha, double beta) {
    if (!std::isfinite(elo0) || !std::isfinite(elo1) || !(elo0 < elo1)) {
        throw std::invalid_argument("elo0 must be smaller than elo1");
    }
    if (!(alpha > 0.0 && alpha < 1.0 && beta > 0.0 && beta < 1.0 &&
          alpha + beta < 1.0)) {
        throw std::invalid_argument(
            "alpha and beta must be positive and sum to less than one");
    }
    if (!(elo_to_score(elo0) > 0.0 && elo_to_score(elo0) < 1.0 &&
          elo_to_score(elo1) > 0.0 && elo_to_score(elo1) < 1.0)) {
        throw std::invalid_argument("Elo hypothesis is outside numeric range");
    }

    Statistics stats;
    for (std::uint64_t count : results) {
        if (count > std::numeric_limits<std::uint64_t>::max() - stats.pairs) {
            throw std::overflow_error("pentanomial count total overflow");
        }
        stats.pairs += count;
    }
    if (stats.pairs > std::numeric_limits<std::uint64_t>::max() / 2U) {
        throw std::overflow_error("pentanomial game total overflow");
    }
    stats.games = stats.pairs * 2U;
    stats.lower_bound = std::log(beta / (1.0 - alpha));
    stats.upper_bound = std::log((1.0 - beta) / alpha);
    if (stats.pairs == 0) {
        stats.elo_low = std::numeric_limits<double>::quiet_NaN();
        stats.elo_high = std::numeric_limits<double>::quiet_NaN();
        stats.los = std::numeric_limits<double>::quiet_NaN();
        return stats;
    }

    double weighted = 0.0;
    for (std::size_t i = 0; i < results.size(); ++i) {
        weighted += static_cast<double>(results[i]) * kOutcomes[i];
    }
    stats.score = weighted / static_cast<double>(stats.pairs);
    stats.elo = score_to_elo(stats.score);

    double sum_squares = 0.0;
    for (std::size_t i = 0; i < results.size(); ++i) {
        const double difference = kOutcomes[i] - stats.score;
        sum_squares += static_cast<double>(results[i]) * difference * difference;
    }
    if (stats.pairs > 1U && sum_squares > 0.0) {
        const double sample_variance =
            sum_squares / static_cast<double>(stats.pairs - 1U);
        const double standard_error =
            std::sqrt(sample_variance / static_cast<double>(stats.pairs));
        const double low_score =
            std::clamp(stats.score - kNormal95 * standard_error, 0.0, 1.0);
        const double high_score =
            std::clamp(stats.score + kNormal95 * standard_error, 0.0, 1.0);
        stats.elo_low = score_to_elo(low_score);
        stats.elo_high = score_to_elo(high_score);
        stats.los = 0.5 * std::erfc((0.5 - stats.score) /
                                    (standard_error * std::sqrt(2.0)));
    } else {
        stats.elo_low = std::numeric_limits<double>::quiet_NaN();
        stats.elo_high = std::numeric_limits<double>::quiet_NaN();
        stats.los = std::numeric_limits<double>::quiet_NaN();
    }

    stats.llr = logistic_llr(results, elo0, elo1);
    if (stats.llr <= stats.lower_bound) {
        stats.decision = SprtDecision::AcceptH0;
    } else if (stats.llr >= stats.upper_bound) {
        stats.decision = SprtDecision::AcceptH1;
    }
    return stats;
}

std::string_view decision_name(SprtDecision decision) noexcept {
    switch (decision) {
        case SprtDecision::Continue: return "continue";
        case SprtDecision::AcceptH0: return "accept-h0";
        case SprtDecision::AcceptH1: return "accept-h1";
    }
    return "invalid";
}

}  // namespace zfs::eval
