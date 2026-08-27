#pragma once

#include "zfs/move.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace zfs::engine {

enum class Bound : std::uint8_t { None = 0, Upper = 1, Lower = 2, Exact = 3 };

struct TTData {
    bool hit = false;
    Move move{};
    int score = 0;
    int depth = 0;
    Bound bound = Bound::None;
};

class TranspositionTable {
public:
    static constexpr std::size_t kMaximumMegabytes = 1024;

    explicit TranspositionTable(std::size_t megabytes = 64);

    void resize(std::size_t megabytes);
    void clear() noexcept;
    void new_search() noexcept;

    [[nodiscard]] TTData probe(std::uint64_t key) const noexcept;
    void store(std::uint64_t key, Move move, int score, int depth,
               Bound bound) noexcept;
    [[nodiscard]] int hashfull() const noexcept;
    [[nodiscard]] std::size_t megabytes() const noexcept { return megabytes_; }

private:
    struct Entry {
        std::uint64_t key = 0;
        std::uint32_t move = 0;
        std::int16_t score = 0;
        std::uint8_t depth = 0;
        std::uint8_t generation_bound = 0;

        [[nodiscard]] Bound bound() const noexcept {
            return static_cast<Bound>(generation_bound & 0x3U);
        }
        [[nodiscard]] std::uint8_t generation() const noexcept {
            return generation_bound & 0xfcU;
        }
    };

    struct alignas(64) Cluster {
        std::array<Entry, 4> entries{};
    };

    static_assert(sizeof(Entry) == 16);
    static_assert(sizeof(Cluster) == 64);

    [[nodiscard]] Cluster& cluster(std::uint64_t key) noexcept;
    [[nodiscard]] const Cluster& cluster(std::uint64_t key) const noexcept;
    [[nodiscard]] int relative_age(const Entry& entry) const noexcept;

    std::vector<Cluster> clusters_;
    std::size_t mask_ = 0;
    std::size_t megabytes_ = 0;
    std::uint8_t generation_ = 0;
};

}  // namespace zfs::engine
