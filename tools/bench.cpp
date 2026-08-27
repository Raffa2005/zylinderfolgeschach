#include "engine/search.hpp"
#include "engine/tt.hpp"
#include "zfs/game.hpp"
#include "zfs/position.hpp"

#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

constexpr std::array<std::string_view, 12> kCorpus{
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1 -",
    "r1bqkb1r/ppp1np1p/2n5/3pQ1p1/4p3/1PP1P3/P2PKPPP/RNB2BNR w kq g6 0 7 g7",
    "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1 -",
    "4k3/8/8/p6P/8/8/8/4K3 w - a6 0 2 a7",
    "4k3/8/8/8/p6P/8/8/4K3 b - h3 0 1 h2",
    "4k3/8/8/8/8/8/4p3/R3K2R w KQ - 0 1 -",
    "1k2r3/8/8/8/8/8/R7/4K3 w - - 0 1 c2",
    "4k3/8/8/1b6/8/7N/4P3/5K2 w - - 0 1 -",
    "4k3/8/8/8/8/8/8/KN2r2N w - - 0 1 -",
    "r1k5/7P/8/8/8/8/8/4K3 w - - 0 1 -",
    "4k2r/8/8/8/1B6/8/8/4K3 b k - 0 1 -",
    "4r2k/8/8/8/8/8/4R3/4K3 w - - 0 1 -",
};

class Signature {
public:
    void add(std::uint64_t value) noexcept {
        for (unsigned byte = 0; byte < 8U; ++byte) {
            hash_ ^= (value >> (byte * 8U)) & 0xffU;
            hash_ *= 1099511628211ULL;
        }
    }

    [[nodiscard]] std::uint64_t value() const noexcept { return hash_; }

private:
    std::uint64_t hash_ = 14695981039346656037ULL;
};

template <typename Integer>
[[nodiscard]] Integer parse_integer(std::string_view text,
                                    std::string_view option, int base = 10) {
    Integer value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(),
                                        value, base);
    if (text.empty() || parsed.ec != std::errc{} ||
        parsed.ptr != text.data() + text.size()) {
        throw std::invalid_argument("invalid value for " + std::string(option));
    }
    return value;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        int depth = 8;
        std::size_t hash_megabytes = 16;
        std::optional<std::uint64_t> expected_signature;
        for (int index = 1; index < argc; ++index) {
            const std::string_view option(argv[index]);
            if (index + 1 >= argc) {
                throw std::invalid_argument("missing value for " +
                                            std::string(option));
            }
            std::string_view value(argv[++index]);
            if (option == "--depth") {
                depth = parse_integer<int>(value, option);
            } else if (option == "--hash-mb") {
                hash_megabytes = parse_integer<std::size_t>(value, option);
            } else if (option == "--verify-signature") {
                if (value.starts_with("0x") || value.starts_with("0X")) {
                    value.remove_prefix(2);
                }
                expected_signature =
                    parse_integer<std::uint64_t>(value, option, 16);
            } else {
                throw std::invalid_argument("unknown option " +
                                            std::string(option));
            }
        }
        if (depth < 1 || depth > zfs::engine::kMaximumSearchDepth) {
            throw std::invalid_argument("depth is outside the supported range");
        }

        std::uint64_t total_nodes = 0;
        std::int64_t total_ms = 0;
        Signature signature;
        const std::atomic_bool stop{false};
        for (std::size_t index = 0; index < kCorpus.size(); ++index) {
            std::string error;
            const auto position = zfs::Position::from_fen(kCorpus[index], &error);
            if (!position) {
                throw std::runtime_error("invalid benchmark position " +
                                         std::to_string(index) + ": " + error);
            }
            zfs::engine::TranspositionTable table(hash_megabytes);
            zfs::engine::Searcher searcher(table);
            zfs::engine::SearchLimits limits;
            limits.depth = depth;
            const auto result = searcher.search(zfs::Game(*position), limits, {}, stop);
            if (!result.has_move) {
                throw std::runtime_error("benchmark position is terminal: " +
                                         std::to_string(index));
            }
            total_nodes += result.nodes;
            total_ms += result.elapsed_ms;
            signature.add(index);
            signature.add(result.best_move.raw());
            signature.add(static_cast<std::uint64_t>(
                static_cast<std::int64_t>(result.score)));
            signature.add(result.nodes);
            signature.add(static_cast<std::uint64_t>(result.depth));
            signature.add(result.principal_variation.size());
            for (zfs::Move move : result.principal_variation) {
                signature.add(move.raw());
            }
            std::cout << std::setw(2) << index + 1U << '/' << kCorpus.size()
                      << " bestmove " << result.best_move.uci() << " score "
                      << result.score << " nodes " << result.nodes << " time "
                      << result.elapsed_ms << "ms\n";
        }

        const std::uint64_t nps = total_ms > 0
            ? total_nodes * 1000U / static_cast<std::uint64_t>(total_ms)
            : 0U;
        std::cout << "total nodes " << total_nodes << " time " << total_ms
                  << "ms nps " << nps << " signature 0x" << std::hex
                  << std::setw(16) << std::setfill('0') << signature.value()
                  << std::dec << '\n';
        if (expected_signature && *expected_signature != signature.value()) {
            std::cerr << "signature mismatch: expected 0x" << std::hex
                      << *expected_signature << ", got 0x" << signature.value()
                      << std::dec << '\n';
            return 1;
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "zfs_bench: " << error.what() << '\n';
        return 1;
    }
}
