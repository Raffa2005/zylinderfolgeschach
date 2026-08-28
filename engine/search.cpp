#include "engine/search.hpp"

#include "engine/evaluate.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace zfs::engine {
namespace {

constexpr int kInfinity = 32000;
constexpr int kMaximumPly = 128;
constexpr int kMaximumQuiescencePlies = 8;
constexpr int kStopCheckMask = 1023;
constexpr std::uint64_t kNullContext = 0x452821e638d01377ULL;
constexpr std::uint64_t kSyntheticScoreDomain = 0xbe5466cf34e90c6cULL;

[[nodiscard]] constexpr std::uint64_t mix64(std::uint64_t value) noexcept {
    value ^= value >> 30U;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27U;
    value *= 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

[[nodiscard]] int score_to_table(int score, int ply) noexcept {
    if (score >= kMateThreshold) {
        return score + ply;
    }
    if (score <= -kMateThreshold) {
        return score - ply;
    }
    return score;
}

[[nodiscard]] int score_from_table(int score, int ply) noexcept {
    if (score >= kMateThreshold) {
        return score - ply;
    }
    if (score <= -kMateThreshold) {
        return score + ply;
    }
    return score;
}

[[nodiscard]] bool is_quiet(Move move) noexcept {
    return !move.is_capture() && !move.is_promotion();
}

[[nodiscard]] SearchLimits normalize_limits(SearchLimits limits) {
    limits.depth = std::clamp(limits.depth, 0, kMaximumSearchDepth);
    limits.mate = std::clamp(limits.mate, 0, kMaximumMateMoves);
    limits.moves_to_go = std::max(0, limits.moves_to_go);
    limits.quiescence_plies = std::clamp(
        limits.quiescence_plies, 0, kMaximumQuiescencePlies);
    limits.move_overhead_ms = std::clamp<std::int64_t>(
        limits.move_overhead_ms, 0, kMaximumTimeMs);
    if (limits.movetime_ms >= 0) {
        limits.movetime_ms = std::min(limits.movetime_ms, kMaximumTimeMs);
    }
    for (std::size_t side = 0; side < limits.time_ms.size(); ++side) {
        if (limits.time_ms[side] >= 0) {
            limits.time_ms[side] =
                std::min(limits.time_ms[side], kMaximumTimeMs);
        }
        limits.increment_ms[side] = std::clamp<std::int64_t>(
            limits.increment_ms[side], 0, kMaximumTimeMs);
    }
    return limits;
}

}  // namespace

struct Searcher::Buffers {
    struct Transition {
        Move move{};
        Undo undo{};
    };

    std::array<MoveList, kMaximumPly> moves{};
    std::array<std::array<int, MoveList::kCapacity>, kMaximumPly> scores{};
    std::array<Transition, kMaximumPly> transitions{};
    std::array<std::uint64_t, kMaximumPly> base_keys{};
    std::array<std::uint64_t, kMaximumPly> context_keys{};
    std::array<std::array<Move, kMaximumPly>, kMaximumPly> pv{};
    std::array<std::uint8_t, kMaximumPly> pv_length{};
    std::array<std::array<Move, 2>, kMaximumPly> killers{};
    std::array<std::array<std::array<int, 64>, 64>, 2> history{};
};

class Searcher::Implementation {
public:
    Implementation(TranspositionTable& table, const Game& game,
                   const SearchLimits& limits, const InfoCallback& on_info,
                   const std::atomic_bool& stop,
                   const std::atomic_bool* pondering)
        : table_(table), limits_(normalize_limits(limits)), on_info_(on_info),
          stop_(stop), pondering_(pondering),
          position_(game.position()), buffers_(std::make_unique<Buffers>()) {
        const std::span<const Position> positions = game.positions();
        root_history_.assign(positions.begin(), positions.end());
    }

    [[nodiscard]] SearchResult run() {
        started_ = Clock::now();
        configure_time();
        table_.new_search();

        buffers_->base_keys[0] = position_.base_key();
        buffers_->context_keys[0] = initial_context_key();

        MoveList legal;
        position_.generate_legal_moves(legal);
        if (legal.empty()) {
            result_.score = position_.in_check(position_.side_to_move())
                                ? -kMateScore
                                : 0;
            finish_result();
            return result_;
        }
        if (position_.halfmove_clock() >= 100U || root_repetitions() >= 3U) {
            finish_result();
            return result_;
        }

        root_moves_.reserve(legal.size());
        const bool restrict_root = limits_.restrict_search_moves ||
                                   !limits_.search_moves.empty();
        for (Move move : legal) {
            if (!restrict_root ||
                std::find(limits_.search_moves.begin(), limits_.search_moves.end(),
                          move) != limits_.search_moves.end()) {
                root_moves_.push_back(RootMove{move, -kInfinity});
            }
        }
        if (root_moves_.empty()) {
            finish_result();
            return result_;
        }

        result_.has_move = true;
        result_.best_move = root_moves_.front().move;
        result_.principal_variation = {result_.best_move};

        int depth_limit = limits_.depth > 0
                              ? limits_.depth
                              : kMaximumSearchDepth;
        if (limits_.mate > 0) {
            depth_limit = std::min(depth_limit, limits_.mate * 2);
        }

        for (int depth = 1; depth <= depth_limit; ++depth) {
            if (depth > 1 && soft_time_expired()) {
                break;
            }

            selective_depth_ = 0;
            int score = 0;
            Move best{};
            if (!search_root(depth, score, best)) {
                break;
            }

            result_.best_move = best;
            result_.score = score;
            result_.depth = depth;
            result_.selective_depth = selective_depth_;
            result_.nodes = nodes_;
            result_.elapsed_ms = elapsed_ms();
            result_.principal_variation.assign(
                buffers_->pv[0].begin(),
                buffers_->pv[0].begin() + buffers_->pv_length[0]);

            if (on_info_) {
                on_info_(SearchInfo{result_.depth, result_.selective_depth,
                                    result_.score, result_.nodes,
                                    result_.elapsed_ms, table_.hashfull(),
                                    result_.principal_variation});
            }

            if (external_stop() || hard_limit_reached()) {
                break;
            }
            if (limits_.mate > 0 && std::abs(score) >= kMateThreshold) {
                const int plies = kMateScore - std::abs(score);
                if (plies <= 2 * limits_.mate) {
                    break;
                }
            }
        }

        finish_result();
        return result_;
    }

private:
    using Clock = std::chrono::steady_clock;

    struct RootMove {
        Move move{};
        int previous_score = -kInfinity;
    };

    void configure_time() {
        if (limits_.infinite) {
            return;
        }

        std::int64_t budget = -1;
        if (limits_.movetime_ms >= 0) {
            budget = std::max<std::int64_t>(1, limits_.movetime_ms);
        } else {
            const unsigned side = color_index(position_.side_to_move());
            const std::int64_t remaining = limits_.time_ms[side];
            if (remaining >= 0) {
                const std::int64_t usable = std::max<std::int64_t>(1,
                    remaining - limits_.move_overhead_ms);
                const int moves = limits_.moves_to_go > 0
                                      ? limits_.moves_to_go
                                      : 30;
                budget = usable / moves +
                         (limits_.increment_ms[side] * 3) / 4;
                budget = std::clamp<std::int64_t>(budget, 1, usable);
            }
        }

        if (budget >= 0) {
            hard_budget_ms_ = std::max<std::int64_t>(1, budget);
            soft_budget_ms_ = std::max<std::int64_t>(
                1, hard_budget_ms_ - (hard_budget_ms_ * 3) / 10);
            has_time_budget_ = true;
            activate_deadline_if_ready();
        }
    }

    [[nodiscard]] bool is_pondering() const noexcept {
        return limits_.ponder && pondering_ != nullptr &&
               pondering_->load(std::memory_order_relaxed);
    }

    void activate_deadline_if_ready() noexcept {
        if (!has_time_budget_ || has_deadline_ || is_pondering()) {
            return;
        }
        const Clock::time_point now = Clock::now();
        const std::int64_t maximum_delay =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                Clock::time_point::max() - now)
                .count();
        const std::int64_t hard = std::clamp<std::int64_t>(
            hard_budget_ms_, 1, std::max<std::int64_t>(1, maximum_delay));
        const std::int64_t soft = std::min(soft_budget_ms_, hard);
        hard_deadline_ = now + std::chrono::milliseconds(hard);
        soft_deadline_ = now + std::chrono::milliseconds(soft);
        has_deadline_ = true;
    }

    [[nodiscard]] std::int64_t elapsed_ms() const noexcept {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   Clock::now() - started_)
            .count();
    }

    [[nodiscard]] bool external_stop() const noexcept {
        return stop_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] bool hard_limit_reached() noexcept {
        activate_deadline_if_ready();
        if (limits_.nodes != 0 && nodes_ >= limits_.nodes) {
            return true;
        }
        return has_deadline_ && Clock::now() >= hard_deadline_;
    }

    [[nodiscard]] bool soft_time_expired() noexcept {
        activate_deadline_if_ready();
        return has_deadline_ && Clock::now() >= soft_deadline_;
    }

    [[nodiscard]] bool visit_node(int ply) {
        ++nodes_;
        selective_depth_ = std::max(selective_depth_, ply);
        if (external_stop() ||
            (limits_.nodes != 0 && nodes_ >= limits_.nodes)) {
            aborted_ = true;
            return false;
        }
        if ((nodes_ & kStopCheckMask) == 0) {
            activate_deadline_if_ready();
            if (has_deadline_ && Clock::now() >= hard_deadline_) {
                aborted_ = true;
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] std::uint64_t initial_context_key() const noexcept {
        const std::size_t last = root_history_.size() - 1U;
        const std::size_t reversible = std::min<std::size_t>(
            last, static_cast<std::size_t>(position_.halfmove_clock()));
        const std::size_t lower = last - reversible;
        std::size_t first = last;
        while (first > lower &&
               root_history_[first - 1U].castling_rights() ==
                   position_.castling_rights()) {
            --first;
        }

        std::uint64_t context = 0;
        for (std::size_t index = first; index <= last; ++index) {
            context += mix64(root_history_[index].raw_key());
        }
        return context;
    }

    [[nodiscard]] std::uint64_t score_key(int ply,
                                          bool synthetic_path) const noexcept {
        const std::uint64_t clock = mix64(
            static_cast<std::uint64_t>(position_.halfmove_clock()) ^
            0xa4093822299f31d0ULL);
        return position_.raw_key() ^
               std::rotl(buffers_->context_keys[ply], 17) ^ clock ^
               (synthetic_path ? kSyntheticScoreDomain : 0ULL);
    }

    void make_move(Move move, int ply) noexcept {
        Buffers::Transition& transition = buffers_->transitions[ply];
        transition.move = move;
        const Piece moving = position_.piece_at(move.from());
        const std::uint8_t rights = position_.castling_rights();
        position_.do_move(move, transition.undo);
        const bool irreversible = piece_type(moving) == PieceType::Pawn ||
                                  move.is_capture() ||
                                  rights != position_.castling_rights();
        buffers_->base_keys[ply + 1] = position_.base_key();
        buffers_->context_keys[ply + 1] =
            irreversible
                ? mix64(position_.raw_key())
                : buffers_->context_keys[ply] + mix64(position_.raw_key());
    }

    void undo_move(int ply) noexcept {
        const Buffers::Transition& transition = buffers_->transitions[ply];
        position_.undo_move(transition.move, transition.undo);
    }

    void make_null_move(int ply, NullUndo& undo) noexcept {
        position_.do_null_move(undo);
        buffers_->base_keys[ply + 1] = position_.base_key();
        buffers_->context_keys[ply + 1] =
            mix64(position_.raw_key() ^ kNullContext);
    }

    [[nodiscard]] unsigned root_repetitions() const {
        unsigned count = 1;
        const std::size_t current = root_history_.size() - 1U;
        const std::size_t reversible = std::min<std::size_t>(
            current, static_cast<std::size_t>(position_.halfmove_clock()));
        for (std::size_t distance = 2; distance <= reversible; distance += 2) {
            const Position& candidate = root_history_[current - distance];
            if (candidate.base_key() == position_.base_key() &&
                position_.same_repetition_state(candidate)) {
                ++count;
            }
        }
        return count;
    }

    [[nodiscard]] Position reconstruct_path_position(int current_ply,
                                                     int target_ply) const {
        Position candidate = position_;
        for (int ply = current_ply; ply > target_ply; --ply) {
            const Buffers::Transition& transition =
                buffers_->transitions[ply - 1];
            candidate.undo_move(transition.move, transition.undo);
        }
        return candidate;
    }

    [[nodiscard]] bool is_search_repetition(int ply) const {
        if (position_.halfmove_clock() < 4U) {
            return false;
        }

        const std::size_t history_last = root_history_.size() - 1U;
        const std::size_t current = history_last + static_cast<std::size_t>(ply);
        const std::size_t reversible = std::min<std::size_t>(
            current, static_cast<std::size_t>(position_.halfmove_clock()));
        for (std::size_t distance = 2; distance <= reversible; distance += 2) {
            const std::size_t index = current - distance;
            if (index < root_history_.size()) {
                const Position& candidate = root_history_[index];
                if (candidate.base_key() == position_.base_key() &&
                    position_.same_repetition_state(candidate)) {
                    return true;
                }
                continue;
            }

            const int target_ply = static_cast<int>(index - history_last);
            if (buffers_->base_keys[target_ply] != position_.base_key()) {
                continue;
            }
            const Position candidate = reconstruct_path_position(ply, target_ply);
            if (position_.same_repetition_state(candidate)) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] int move_order_score(Move move, Move tt_move,
                                       int ply) const noexcept {
        if (move == tt_move) {
            return 2'000'000;
        }
        if (move.is_promotion()) {
            return 1'500'000 + 100 * static_cast<int>(move.promotion());
        }
        if (move.is_capture()) {
            const Piece victim = move.type() == MoveType::EnPassant
                                     ? make_piece(opposite(position_.side_to_move()),
                                                  PieceType::Pawn)
                                     : position_.piece_at(move.to());
            const Piece attacker = position_.piece_at(move.from());
            constexpr std::array values{1, 3, 3, 5, 9, 0};
            return 1'000'000 + 16 * values[type_index(piece_type(victim))] -
                   values[type_index(piece_type(attacker))];
        }
        if (move == buffers_->killers[ply][0]) {
            return 900'000;
        }
        if (move == buffers_->killers[ply][1]) {
            return 899'000;
        }
        const unsigned color = color_index(position_.side_to_move());
        return buffers_->history[color][move.from()][move.to()];
    }

    void score_moves(MoveList& moves, std::size_t count, Move tt_move,
                     int ply) noexcept {
        for (std::size_t index = 0; index < count; ++index) {
            buffers_->scores[ply][index] =
                move_order_score(moves[index], tt_move, ply);
        }
    }

    void pick_next(MoveList& moves, std::size_t first, std::size_t count,
                   int ply) noexcept {
        std::size_t best = first;
        for (std::size_t index = first + 1U; index < count; ++index) {
            if (buffers_->scores[ply][index] > buffers_->scores[ply][best]) {
                best = index;
            }
        }
        if (best != first) {
            std::swap(moves[first], moves[best]);
            std::swap(buffers_->scores[ply][first], buffers_->scores[ply][best]);
        }
    }

    void update_pv(int ply, Move move) noexcept {
        buffers_->pv[ply][0] = move;
        const unsigned child_length = buffers_->pv_length[ply + 1];
        for (unsigned index = 0; index < child_length; ++index) {
            buffers_->pv[ply][index + 1U] = buffers_->pv[ply + 1][index];
        }
        buffers_->pv_length[ply] = static_cast<std::uint8_t>(child_length + 1U);
    }

    void reward_quiet(Move move, int ply, int depth) noexcept {
        if (move != buffers_->killers[ply][0]) {
            buffers_->killers[ply][1] = buffers_->killers[ply][0];
            buffers_->killers[ply][0] = move;
        }
        int& value = buffers_->history[color_index(position_.side_to_move())]
                                      [move.from()][move.to()];
        value = std::min(100'000, value + depth * depth);
    }

    [[nodiscard]] bool has_non_pawn_material() const noexcept {
        const Color side = position_.side_to_move();
        const Bitboard pawns = position_.pieces(side, PieceType::Pawn);
        const Bitboard king = position_.pieces(side, PieceType::King);
        return (position_.occupied(side) & ~(pawns | king)) != 0;
    }

    [[nodiscard]] int quiescence(int alpha, int beta, int ply, int qply,
                                 bool synthetic_path) {
        if (!visit_node(ply)) {
            return 0;
        }
        buffers_->pv_length[ply] = 0;
        if (ply >= kMaximumPly - 1) {
            return evaluate_material(position_);
        }

        if (!synthetic_path && is_search_repetition(ply)) {
            return 0;
        }

        MoveList& moves = buffers_->moves[ply];
        position_.generate_legal_moves(moves);
        if (moves.empty()) {
            return position_.in_check(position_.side_to_move())
                       ? -kMateScore + ply
                       : 0;
        }
        if (position_.halfmove_clock() >= 100U) {
            return 0;
        }
        const int qply_limit = std::clamp(limits_.quiescence_plies, 0,
                                          kMaximumQuiescencePlies);
        if (qply >= qply_limit) {
            return evaluate_material(position_);
        }

        const bool in_check = position_.in_check(position_.side_to_move());
        const bool follow_forced = valid_square(position_.follow_square()) &&
                                   moves[0].to() == position_.follow_square();
        int best = -kInfinity;
        if (!in_check && !follow_forced) {
            best = evaluate_material(position_);
            if (best >= beta) {
                return best;
            }
            alpha = std::max(alpha, best);
        }

        std::size_t search_count = moves.size();
        if (!in_check && !follow_forced) {
            search_count = 0;
            for (std::size_t index = 0; index < moves.size(); ++index) {
                if (moves[index].is_capture() || moves[index].is_promotion()) {
                    moves[search_count++] = moves[index];
                }
            }
            if (search_count == 0) {
                return best;
            }
        }

        score_moves(moves, search_count, Move{}, ply);
        for (std::size_t index = 0; index < search_count; ++index) {
            pick_next(moves, index, search_count, ply);
            const Move move = moves[index];
            make_move(move, ply);
            const int score = -quiescence(-beta, -alpha, ply + 1, qply + 1,
                                          synthetic_path);
            undo_move(ply);
            if (aborted_) {
                return 0;
            }
            if (score > best) {
                best = score;
                update_pv(ply, move);
            }
            if (score > alpha) {
                alpha = score;
            }
            if (alpha >= beta) {
                return best;
            }
        }
        return best;
    }

    [[nodiscard]] int alpha_beta(int depth, int alpha, int beta, int ply,
                                 bool allow_null, bool synthetic_path) {
        if (depth <= 0) {
            return quiescence(alpha, beta, ply, 0, synthetic_path);
        }
        if (!visit_node(ply)) {
            return 0;
        }
        buffers_->pv_length[ply] = 0;
        if (ply >= kMaximumPly - 1) {
            return evaluate_material(position_);
        }

        if (!synthetic_path && is_search_repetition(ply)) {
            return 0;
        }

        alpha = std::max(alpha, -kMateScore + ply);
        beta = std::min(beta, kMateScore - ply - 1);
        if (alpha >= beta) {
            return alpha;
        }
        const int original_alpha = alpha;

        const std::uint64_t key = score_key(ply, synthetic_path);
        const TTData tt = table_.probe(key);
        Move tt_move{};
        if (tt.hit) {
            tt_move = tt.move;
            const int tt_score = score_from_table(tt.score, ply);
            if (tt.depth >= depth) {
                if (tt.bound == Bound::Exact ||
                    (tt.bound == Bound::Lower && tt_score >= beta) ||
                    (tt.bound == Bound::Upper && tt_score <= alpha)) {
                    return tt_score;
                }
            }
        }

        MoveList& moves = buffers_->moves[ply];
        position_.generate_legal_moves(moves);
        if (moves.empty()) {
            return position_.in_check(position_.side_to_move())
                       ? -kMateScore + ply
                       : 0;
        }
        if (position_.halfmove_clock() >= 100U) {
            return 0;
        }

        const bool null_window = beta == alpha + 1;
        if (limits_.null_move_pruning && allow_null && null_window && depth >= 5 &&
            position_.halfmove_clock() <= 90U && beta > -kMateThreshold &&
            beta < kMateThreshold && has_non_pawn_material() &&
            !position_.in_check(position_.side_to_move()) &&
            !(valid_square(position_.follow_square()) &&
              moves[0].to() == position_.follow_square()) &&
            evaluate_material(position_) >= beta) {
            const int reduction = 2 + depth / 5;
            NullUndo undo;
            make_null_move(ply, undo);
            ++null_searches_;
            const int null_score = -alpha_beta(
                depth - 1 - reduction, -beta, -beta + 1, ply + 1, false, true);
            position_.undo_null_move(undo);
            if (aborted_) {
                return 0;
            }
            if (null_score >= beta) {
                // Verification is deliberately mandatory in ZFS: even an
                // unrestricted position can be a follow-origin zugzwang.
                ++null_verifications_;
                const int verified = alpha_beta(
                    depth - reduction, alpha, beta, ply, false, synthetic_path);
                if (aborted_) {
                    return 0;
                }
                buffers_->pv_length[ply] = 0;
                if (verified >= beta) {
                    ++null_cutoffs_;
                    return beta;
                }
                position_.generate_legal_moves(moves);
            }
        }

        score_moves(moves, moves.size(), tt_move, ply);
        int best = -kInfinity;
        Move best_move{};
        bool first_move = true;
        for (std::size_t index = 0; index < moves.size(); ++index) {
            pick_next(moves, index, moves.size(), ply);
            const Move move = moves[index];
            make_move(move, ply);
            int score;
            if (first_move) {
                score = -alpha_beta(depth - 1, -beta, -alpha, ply + 1, true,
                                    synthetic_path);
            } else {
                score = -alpha_beta(depth - 1, -alpha - 1, -alpha, ply + 1,
                                    true, synthetic_path);
                if (!aborted_ && score > alpha && score < beta) {
                    score = -alpha_beta(depth - 1, -beta, -alpha, ply + 1,
                                        true, synthetic_path);
                }
            }
            undo_move(ply);
            if (aborted_) {
                return 0;
            }
            first_move = false;
            if (score > best) {
                best = score;
                best_move = move;
                update_pv(ply, move);
            }
            alpha = std::max(alpha, score);
            if (alpha >= beta) {
                if (is_quiet(move)) {
                    reward_quiet(move, ply, depth);
                }
                break;
            }
        }

        Bound bound = Bound::Exact;
        if (best <= original_alpha) {
            bound = Bound::Upper;
        } else if (best >= beta) {
            bound = Bound::Lower;
        }
        table_.store(key, best_move, score_to_table(best, ply), depth, bound);
        return best;
    }

    [[nodiscard]] bool search_root(int depth, int& score, Move& best_move) {
        if (!visit_node(0)) {
            return false;
        }
        buffers_->pv_length[0] = 0;

        std::stable_sort(root_moves_.begin(), root_moves_.end(),
                         [](const RootMove& lhs, const RootMove& rhs) {
                             return lhs.previous_score > rhs.previous_score;
                         });

        int alpha = -kInfinity;
        int best = -kInfinity;
        std::size_t best_index = 0;
        for (std::size_t index = 0; index < root_moves_.size(); ++index) {
            const Move move = root_moves_[index].move;
            make_move(move, 0);
            int child;
            if (index == 0) {
                child = -alpha_beta(depth - 1, -kInfinity, kInfinity, 1, true,
                                    false);
            } else {
                child = -alpha_beta(depth - 1, -alpha - 1, -alpha, 1, true,
                                    false);
                if (!aborted_ && child > alpha) {
                    child = -alpha_beta(depth - 1, -kInfinity, -alpha, 1, true,
                                        false);
                }
            }
            undo_move(0);
            if (aborted_) {
                return false;
            }
            root_moves_[index].previous_score = child;
            if (child > best) {
                best = child;
                alpha = std::max(alpha, child);
                best_index = index;
                buffers_->pv[0][0] = move;
                const unsigned child_length = buffers_->pv_length[1];
                for (unsigned pv_index = 0; pv_index < child_length; ++pv_index) {
                    buffers_->pv[0][pv_index + 1U] =
                        buffers_->pv[1][pv_index];
                }
                buffers_->pv_length[0] =
                    static_cast<std::uint8_t>(child_length + 1U);
            }
        }

        best_move = root_moves_[best_index].move;
        score = best;
        const std::uint64_t key = score_key(0, false);
        table_.store(key, best_move, score_to_table(best, 0), depth,
                     Bound::Exact);
        return true;
    }

    void finish_result() noexcept {
        result_.nodes = nodes_;
        result_.elapsed_ms = elapsed_ms();
        result_.null_searches = null_searches_;
        result_.null_verifications = null_verifications_;
        result_.null_cutoffs = null_cutoffs_;
        result_.selective_depth = std::max(result_.selective_depth,
                                            selective_depth_);
    }

    TranspositionTable& table_;
    SearchLimits limits_;
    const InfoCallback& on_info_;
    const std::atomic_bool& stop_;
    const std::atomic_bool* pondering_;
    Position position_;
    std::unique_ptr<Buffers> buffers_;
    std::vector<Position> root_history_;
    std::vector<RootMove> root_moves_;
    SearchResult result_{};
    Clock::time_point started_{};
    Clock::time_point hard_deadline_{};
    Clock::time_point soft_deadline_{};
    std::int64_t hard_budget_ms_ = 0;
    std::int64_t soft_budget_ms_ = 0;
    std::uint64_t nodes_ = 0;
    std::uint64_t null_searches_ = 0;
    std::uint64_t null_verifications_ = 0;
    std::uint64_t null_cutoffs_ = 0;
    int selective_depth_ = 0;
    bool has_deadline_ = false;
    bool has_time_budget_ = false;
    bool aborted_ = false;
};

Searcher::Searcher(TranspositionTable& table) : table_(table) {}

Searcher::~Searcher() = default;

SearchResult Searcher::search(const Game& game, const SearchLimits& limits,
                              const InfoCallback& on_info,
                              const std::atomic_bool& stop,
                              const std::atomic_bool* pondering) {
    return Implementation(table_, game, limits, on_info, stop, pondering).run();
}

}  // namespace zfs::engine
