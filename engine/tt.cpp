#include "engine/tt.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace zfs::engine {
namespace {

constexpr std::uint8_t kGenerationMask = 0xfcU;

[[nodiscard]] std::size_t floor_power_of_two(std::size_t value) noexcept {
    std::size_t result = 1;
    while (result <= value / 2U) {
        result *= 2U;
    }
    return result;
}

}  // namespace

TranspositionTable::TranspositionTable(std::size_t megabytes) {
    resize(megabytes);
}

void TranspositionTable::resize(std::size_t megabytes) {
    megabytes = std::max<std::size_t>(1, megabytes);
    if (megabytes > kMaximumMegabytes) {
        throw std::length_error("transposition table exceeds supported limit");
    }
    constexpr std::size_t kMebibyte = 1024U * 1024U;
    if (megabytes > std::numeric_limits<std::size_t>::max() / kMebibyte) {
        throw std::length_error("transposition table size overflows size_t");
    }
    const std::size_t requested = (megabytes * kMebibyte) / sizeof(Cluster);
    const std::size_t count = floor_power_of_two(std::max<std::size_t>(1, requested));
    std::vector<Cluster> replacement(count);
    clusters_.swap(replacement);
    mask_ = count - 1U;
    megabytes_ = (count * sizeof(Cluster)) / kMebibyte;
    generation_ = 0;
}

void TranspositionTable::clear() noexcept {
    std::fill(clusters_.begin(), clusters_.end(), Cluster{});
    generation_ = 0;
}

void TranspositionTable::new_search() noexcept {
    if (generation_ == kGenerationMask) {
        clear();
        return;
    }
    generation_ = static_cast<std::uint8_t>(generation_ + 4U);
}

TranspositionTable::Cluster& TranspositionTable::cluster(
    std::uint64_t key) noexcept {
    return clusters_[static_cast<std::size_t>(key) & mask_];
}

const TranspositionTable::Cluster& TranspositionTable::cluster(
    std::uint64_t key) const noexcept {
    return clusters_[static_cast<std::size_t>(key) & mask_];
}

int TranspositionTable::relative_age(const Entry& entry) const noexcept {
    return static_cast<int>((generation_ - entry.generation()) & kGenerationMask) /
           4;
}

TTData TranspositionTable::probe(std::uint64_t key) const noexcept {
    for (const Entry& entry : cluster(key).entries) {
        if (entry.bound() != Bound::None && entry.key == key) {
            return TTData{true, Move::from_raw(entry.move), entry.score,
                          entry.depth, entry.bound()};
        }
    }
    return {};
}

void TranspositionTable::store(std::uint64_t key, Move move, int score,
                               int depth, Bound bound) noexcept {
    assert(bound != Bound::None);
    Cluster& bucket = cluster(key);
    Entry* replacement = &bucket.entries[0];
    for (Entry& entry : bucket.entries) {
        if (entry.bound() == Bound::None || entry.key == key) {
            replacement = &entry;
            break;
        }
        const int replacement_value = static_cast<int>(replacement->depth) -
                                      8 * relative_age(*replacement);
        const int entry_value = static_cast<int>(entry.depth) -
                                8 * relative_age(entry);
        if (entry_value < replacement_value) {
            replacement = &entry;
        }
    }

    if (replacement->key == key && replacement->bound() != Bound::None &&
        depth + 2 < static_cast<int>(replacement->depth) &&
        bound != Bound::Exact) {
        return;
    }

    replacement->key = key;
    replacement->move = move.raw();
    replacement->score = static_cast<std::int16_t>(std::clamp(
        score, static_cast<int>(std::numeric_limits<std::int16_t>::min()),
        static_cast<int>(std::numeric_limits<std::int16_t>::max())));
    replacement->depth = static_cast<std::uint8_t>(std::clamp(depth, 0, 255));
    replacement->generation_bound = static_cast<std::uint8_t>(
        generation_ | static_cast<std::uint8_t>(bound));
}

int TranspositionTable::hashfull() const noexcept {
    const std::size_t sample = std::min<std::size_t>(1000, clusters_.size());
    if (sample == 0) {
        return 0;
    }
    std::size_t occupied = 0;
    for (std::size_t index = 0; index < sample; ++index) {
        for (const Entry& entry : clusters_[index].entries) {
            occupied += entry.bound() != Bound::None &&
                        entry.generation() == generation_;
        }
    }
    return static_cast<int>((occupied * 1000U) / (sample * 4U));
}

}  // namespace zfs::engine
