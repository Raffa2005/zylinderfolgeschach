#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace zfs::eval {

struct OpeningConfig {
    std::size_t count = 256;
    unsigned minimum_plies = 8;
    unsigned maximum_plies = 16;
    std::uint64_t seed = 0x5a46532d4f50454eULL;
};

using Opening = std::vector<std::string>;

[[nodiscard]] std::vector<Opening> generate_openings(const OpeningConfig& config);
[[nodiscard]] std::string format_opening(const Opening& opening);

}  // namespace zfs::eval
