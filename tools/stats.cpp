#include "tools/eval/stats.hpp"

#include <array>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

template <typename Number>
[[nodiscard]] Number parse_number(std::string_view text,
                                  std::string_view label) {
    Number value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || parsed.ec != std::errc{} ||
        parsed.ptr != text.data() + text.size()) {
        throw std::invalid_argument("invalid " + std::string(label));
    }
    return value;
}

void print_number(double value) {
    if (std::isnan(value)) {
        std::cout << "n/a";
    } else if (std::isinf(value)) {
        std::cout << (value < 0.0 ? "-inf" : "+inf");
    } else {
        std::cout << std::fixed << std::setprecision(2) << value;
    }
}

[[noreturn]] void usage(const char* program) {
    std::cerr << "usage: " << program
              << " LL LD MID WD WW [--elo0 E] [--elo1 E]"
                 " [--alpha P] [--beta P]\n";
    std::exit(2);
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 6) {
            usage(argv[0]);
        }
        zfs::eval::Pentanomial results{};
        for (std::size_t i = 0; i < results.size(); ++i) {
            results[i] = parse_number<std::uint64_t>(argv[i + 1U], "count");
        }
        double elo0 = 0.0;
        double elo1 = 5.0;
        double alpha = 0.05;
        double beta = 0.05;
        for (int index = 6; index < argc; index += 2) {
            if (index + 1 >= argc) {
                usage(argv[0]);
            }
            const std::string_view option(argv[index]);
            const double value = parse_number<double>(argv[index + 1], option);
            if (option == "--elo0") {
                elo0 = value;
            } else if (option == "--elo1") {
                elo1 = value;
            } else if (option == "--alpha") {
                alpha = value;
            } else if (option == "--beta") {
                beta = value;
            } else {
                usage(argv[0]);
            }
        }

        const auto stats =
            zfs::eval::calculate_statistics(results, elo0, elo1, alpha, beta);
        std::cout << "pentanomial " << results[0] << ' ' << results[1] << ' '
                  << results[2] << ' ' << results[3] << ' ' << results[4]
                  << "\npairs " << stats.pairs << " games " << stats.games
                  << " score " << std::fixed << std::setprecision(4)
                  << 100.0 * stats.score << "%\nelo ";
        print_number(stats.elo);
        std::cout << " 95% CI [";
        print_number(stats.elo_low);
        std::cout << ", ";
        print_number(stats.elo_high);
        std::cout << "] LOS ";
        if (std::isnan(stats.los)) {
            std::cout << "n/a";
        } else {
            std::cout << std::fixed << std::setprecision(2)
                      << 100.0 * stats.los << '%';
        }
        std::cout << "\nLLR " << std::setprecision(4) << stats.llr << " ["
                  << stats.lower_bound << ", "
                  << stats.upper_bound << "] "
                  << zfs::eval::decision_name(stats.decision) << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "zfs_stats: " << error.what() << '\n';
        return 1;
    }
}
