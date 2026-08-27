#include "tools/eval/openings.hpp"

#include <charconv>
#include <cstdlib>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

namespace {

template <typename Integer>
[[nodiscard]] Integer parse_integer(std::string_view text,
                                    std::string_view option) {
    Integer value{};
    int base = 10;
    if constexpr (std::is_unsigned_v<Integer>) {
        if (text.starts_with("0x") || text.starts_with("0X")) {
            text.remove_prefix(2);
            base = 16;
        }
    }
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(),
                                        value, base);
    if (text.empty() || parsed.ec != std::errc{} ||
        parsed.ptr != text.data() + text.size()) {
        throw std::invalid_argument("invalid value for " + std::string(option));
    }
    return value;
}

[[noreturn]] void usage(const char* program, std::string_view error = {}) {
    if (!error.empty()) {
        std::cerr << "error: " << error << "\n\n";
    }
    std::cerr
        << "usage: " << program
        << " [--count N] [--min-plies N] [--max-plies N] [--seed N]"
           " [--output FILE]\n";
    std::exit(error.empty() ? 0 : 2);
}

}  // namespace

int main(int argc, char** argv) {
    try {
        zfs::eval::OpeningConfig config;
        std::string output_path;
        for (int index = 1; index < argc; ++index) {
            const std::string_view option(argv[index]);
            if (option == "--help" || option == "-h") {
                usage(argv[0]);
            }
            if (index + 1 >= argc) {
                usage(argv[0], "missing value for " + std::string(option));
            }
            const std::string_view value(argv[++index]);
            if (option == "--count") {
                config.count = parse_integer<std::size_t>(value, option);
            } else if (option == "--min-plies") {
                config.minimum_plies = parse_integer<unsigned>(value, option);
            } else if (option == "--max-plies") {
                config.maximum_plies = parse_integer<unsigned>(value, option);
            } else if (option == "--seed") {
                config.seed = parse_integer<std::uint64_t>(value, option);
            } else if (option == "--output") {
                output_path = value;
            } else {
                usage(argv[0], "unknown option " + std::string(option));
            }
        }

        const auto openings = zfs::eval::generate_openings(config);
        std::ofstream file;
        std::ostream* output = &std::cout;
        if (!output_path.empty()) {
            file.open(output_path, std::ios::out | std::ios::trunc);
            if (!file) {
                throw std::runtime_error("cannot open output file: " + output_path);
            }
            output = &file;
        }
        *output << "# zfs-openings-v1 seed=" << config.seed
                << " min_plies=" << config.minimum_plies
                << " max_plies=" << config.maximum_plies << '\n';
        for (const auto& opening : openings) {
            *output << zfs::eval::format_opening(opening) << '\n';
        }
        if (!*output) {
            throw std::runtime_error("failed while writing openings");
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "zfs_openings: " << error.what() << '\n';
        return 1;
    }
}
