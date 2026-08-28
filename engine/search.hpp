#pragma once

#include "engine/tt.hpp"
#include "zfs/game.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <vector>

namespace zfs::engine {

inline constexpr int kMateScore = 30000;
inline constexpr int kMateThreshold = kMateScore - 256;
inline constexpr int kMaximumSearchDepth = 100;
inline constexpr int kMaximumMateMoves = kMaximumSearchDepth / 2;
inline constexpr std::int64_t kMaximumTimeMs = 2'147'483'647;

struct SearchLimits {
    int depth = 0;
    int mate = 0;
    std::uint64_t nodes = 0;
    std::int64_t movetime_ms = -1;
    std::array<std::int64_t, 2> time_ms{-1, -1};
    std::array<std::int64_t, 2> increment_ms{0, 0};
    int moves_to_go = 0;
    int quiescence_plies = 8;
    std::int64_t move_overhead_ms = 10;
    bool infinite = false;
    bool ponder = false;
    bool null_move_pruning = true;
    bool restrict_search_moves = false;
    std::vector<Move> search_moves;
};

struct SearchInfo {
    int depth = 0;
    int selective_depth = 0;
    int score = 0;
    std::uint64_t nodes = 0;
    std::int64_t elapsed_ms = 0;
    int hashfull = 0;
    std::vector<Move> principal_variation;
};

struct SearchResult {
    bool has_move = false;
    Move best_move{};
    int score = 0;
    int depth = 0;
    int selective_depth = 0;
    std::uint64_t nodes = 0;
    std::int64_t elapsed_ms = 0;
    std::uint64_t null_searches = 0;
    std::uint64_t null_verifications = 0;
    std::uint64_t null_cutoffs = 0;
    std::vector<Move> principal_variation;
};

using InfoCallback = std::function<void(const SearchInfo&)>;

class Searcher {
public:
    explicit Searcher(TranspositionTable& table);
    ~Searcher();

    Searcher(const Searcher&) = delete;
    Searcher& operator=(const Searcher&) = delete;

    [[nodiscard]] SearchResult search(const Game& game,
                                      const SearchLimits& limits,
                                      const InfoCallback& on_info,
                                      const std::atomic_bool& stop,
                                      const std::atomic_bool* pondering = nullptr);

private:
    struct Buffers;
    class Implementation;

    TranspositionTable& table_;
};

}  // namespace zfs::engine
