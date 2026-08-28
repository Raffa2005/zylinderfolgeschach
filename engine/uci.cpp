#include "engine/evaluate.hpp"
#include "engine/search.hpp"
#include "engine/tt.hpp"
#include "zfs/game.hpp"
#include "zfs/position.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace zfs::engine {
namespace {

template <typename Integer>
[[nodiscard]] bool parse_integer(std::string_view text,
                                 Integer& value) noexcept {
    const char* const begin = text.data();
    const char* const end = begin + text.size();
    const auto result = std::from_chars(begin, end, value);
    return result.ec == std::errc{} && result.ptr == end;
}

[[nodiscard]] std::vector<std::string> tokens(std::string_view line) {
    std::istringstream input{std::string(line)};
    std::vector<std::string> result;
    std::string token;
    while (input >> token) {
        result.push_back(std::move(token));
    }
    return result;
}

[[nodiscard]] std::string join(const std::vector<std::string>& words,
                               std::size_t first, std::size_t last) {
    std::string result;
    for (std::size_t index = first; index < last; ++index) {
        if (!result.empty()) {
            result.push_back(' ');
        }
        result += words[index];
    }
    return result;
}

[[nodiscard]] constexpr bool is_go_keyword(std::string_view token) noexcept {
    return token == "searchmoves" || token == "ponder" || token == "wtime" ||
           token == "btime" || token == "winc" || token == "binc" ||
           token == "movestogo" || token == "depth" || token == "nodes" ||
           token == "mate" || token == "movetime" || token == "infinite";
}

[[nodiscard]] constexpr bool is_go_value_keyword(
    std::string_view token) noexcept {
    return token == "wtime" || token == "btime" || token == "winc" ||
           token == "binc" || token == "movestogo" || token == "depth" ||
           token == "nodes" || token == "mate" || token == "movetime";
}

class UciLoop {
public:
    UciLoop() : table_(64) {}

    ~UciLoop() { stop_search(); }

    int run() {
        std::string line;
        while (std::getline(std::cin, line)) {
            const std::vector<std::string> words = tokens(line);
            if (words.empty()) {
                continue;
            }
            const std::string& command = words[0];
            if (command == "uci") {
                send("id name Kugelfisch");
                send("id author OpenAI and Rafael");
                send("option name Hash type spin default 64 min 1 max " +
                     std::to_string(TranspositionTable::kMaximumMegabytes));
                send("option name Move Overhead type spin default 10 min 0 max 5000");
                send("option name Ponder type check default false");
                send("option name Clear Hash type button");
                send("uciok");
            } else if (command == "isready") {
                send("readyok");
            } else if (command == "ucinewgame") {
                stop_search();
                table_.clear();
                game_.reset(Position::start());
            } else if (command == "setoption") {
                set_option(words);
            } else if (command == "position") {
                set_position(words);
            } else if (command == "go") {
                start_search(words);
            } else if (command == "stop") {
                stop_search();
            } else if (command == "quit") {
                stop_search();
                return 0;
            } else if (command == "d") {
                send("info string " + game_.position().to_fen());
            } else if (command == "eval") {
                send("info string static evaluation " +
                     std::to_string(evaluate(game_.position())) +
                     " cp for side to move");
            } else if (command == "ponderhit") {
                pondering_.store(false, std::memory_order_relaxed);
            } else if (command != "debug" && command != "register") {
                send("info string unknown command: " + line);
            }
        }
        stop_search();
        return 0;
    }

private:
    void send(const std::string& line) {
        const std::lock_guard lock(output_mutex_);
        std::cout << line << std::endl;
    }

    void stop_search() {
        stop_.store(true, std::memory_order_relaxed);
        pondering_.store(false, std::memory_order_relaxed);
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    void set_option(const std::vector<std::string>& words) {
        stop_search();
        const auto name_it = std::find(words.begin(), words.end(), "name");
        if (name_it == words.end()) {
            send("info string setoption is missing name");
            return;
        }
        const auto value_it = std::find(name_it + 1, words.end(), "value");
        const std::size_t name_first =
            static_cast<std::size_t>(std::distance(words.begin(), name_it + 1));
        const std::size_t name_last = value_it == words.end()
                                          ? words.size()
                                          : static_cast<std::size_t>(
                                                std::distance(words.begin(), value_it));
        const std::string name = join(words, name_first, name_last);

        if (name == "Clear Hash") {
            table_.clear();
            return;
        }
        if (value_it == words.end() || value_it + 1 == words.end()) {
            send("info string setoption is missing value for " + name);
            return;
        }

        if (name == "Ponder") {
            const std::string& value = *(value_it + 1);
            if (value != "true" && value != "false") {
                send("info string invalid value for Ponder");
            }
            // The GUI starts and ends every actual session with go ponder and
            // ponderhit; there is no autonomous pondering policy to store.
            return;
        }

        std::int64_t value = 0;
        if (!parse_integer(*(value_it + 1), value)) {
            send("info string invalid value for " + name);
            return;
        }
        if (name == "Hash") {
            value = std::clamp<std::int64_t>(
                value, 1,
                static_cast<std::int64_t>(TranspositionTable::kMaximumMegabytes));
            try {
                table_.resize(static_cast<std::size_t>(value));
            } catch (const std::exception& error) {
                send("info string could not resize Hash: " +
                     std::string(error.what()));
            }
        } else if (name == "Move Overhead") {
            move_overhead_ms_ = std::clamp<std::int64_t>(value, 0, 5000);
        } else {
            send("info string unknown option: " + name);
        }
    }

    void set_position(const std::vector<std::string>& words) {
        stop_search();
        if (words.size() < 2) {
            send("info string position is missing an argument");
            return;
        }

        Position root = Position::start();
        std::size_t cursor = 2;
        if (words[1] == "startpos") {
            cursor = 2;
        } else if (words[1] == "fen" || words[1] == "zfsfen") {
            const auto moves_it = std::find(words.begin() + 2, words.end(), "moves");
            const std::size_t fen_end = static_cast<std::size_t>(
                std::distance(words.begin(), moves_it));
            const std::size_t field_count = fen_end - 2U;
            if (field_count != 6U && field_count != 7U) {
                send("info string position fen requires six or seven fields");
                return;
            }
            std::string error;
            const std::optional<Position> parsed =
                Position::from_fen(join(words, 2, fen_end), &error);
            if (!parsed) {
                send("info string invalid position: " + error);
                return;
            }
            root = *parsed;
            cursor = fen_end;
        } else {
            send("info string expected startpos, fen, or zfsfen after position");
            return;
        }

        Game replacement(std::move(root));
        if (cursor < words.size()) {
            if (words[cursor] != "moves") {
                send("info string expected moves after position fields");
                return;
            }
            ++cursor;
        }
        for (; cursor < words.size(); ++cursor) {
            if (!replacement.play_uci(words[cursor])) {
                send("info string illegal move in position command: " + words[cursor]);
                return;
            }
        }
        game_ = std::move(replacement);
    }

    [[nodiscard]] std::optional<SearchLimits> parse_limits(
        const std::vector<std::string>& words) {
        SearchLimits limits;
        limits.move_overhead_ms = move_overhead_ms_;
        std::size_t index = 1;
        while (index < words.size()) {
            const std::string& token = words[index++];
            if (token == "infinite") {
                limits.infinite = true;
                continue;
            }
            if (token == "ponder") {
                limits.ponder = true;
                continue;
            }
            if (token == "searchmoves") {
                limits.restrict_search_moves = true;
                Position root = game_.position();
                const std::size_t first_move = index;
                while (index < words.size() &&
                       !is_go_keyword(words[index])) {
                    const std::optional<Move> move = root.parse_uci(words[index]);
                    if (!move) {
                        send("info string illegal searchmoves move: " + words[index]);
                        return std::nullopt;
                    }
                    limits.search_moves.push_back(*move);
                    ++index;
                }
                if (index == first_move) {
                    send("info string searchmoves requires at least one move");
                    return std::nullopt;
                }
                continue;
            }
            if (!is_go_value_keyword(token)) {
                send("info string unknown go parameter: " + token);
                return std::nullopt;
            }
            if (index >= words.size()) {
                send("info string go parameter is missing a value: " + token);
                return std::nullopt;
            }
            if (token == "nodes") {
                std::uint64_t value = 0;
                if (!parse_integer(words[index++], value)) {
                    send("info string invalid go value for " + token);
                    return std::nullopt;
                }
                limits.nodes = value;
                continue;
            }
            std::int64_t value = 0;
            if (!parse_integer(words[index++], value) || value < 0) {
                send("info string invalid go value for " + token);
                return std::nullopt;
            }
            if (token == "depth" &&
                (value == 0 || value > kMaximumSearchDepth)) {
                send("info string out-of-range go value for " + token);
                return std::nullopt;
            }
            if (token == "mate" &&
                (value == 0 || value > kMaximumMateMoves)) {
                send("info string out-of-range go value for " + token);
                return std::nullopt;
            }
            if (token == "movestogo" &&
                (value == 0 || value > std::numeric_limits<int>::max())) {
                send("info string out-of-range go value for " + token);
                return std::nullopt;
            }
            if ((token == "wtime" || token == "btime" || token == "winc" ||
                 token == "binc" || token == "movetime") &&
                value > kMaximumTimeMs) {
                send("info string out-of-range go value for " + token);
                return std::nullopt;
            }
            if (token == "wtime") limits.time_ms[0] = value;
            else if (token == "btime") limits.time_ms[1] = value;
            else if (token == "winc") limits.increment_ms[0] = value;
            else if (token == "binc") limits.increment_ms[1] = value;
            else if (token == "movestogo") limits.moves_to_go = static_cast<int>(value);
            else if (token == "depth") limits.depth = static_cast<int>(value);
            else if (token == "mate") limits.mate = static_cast<int>(value);
            else if (token == "movetime") limits.movetime_ms = value;
        }
        return limits;
    }

    void start_search(const std::vector<std::string>& words) {
        stop_search();
        const std::optional<SearchLimits> limits = parse_limits(words);
        if (!limits) {
            return;
        }

        stop_.store(false, std::memory_order_relaxed);
        pondering_.store(limits->ponder, std::memory_order_relaxed);
        Game game = game_;
        worker_ = std::thread([this, game = std::move(game), limits = *limits]() {
            try {
                Searcher searcher(table_);
                const SearchResult result = searcher.search(
                    game, limits,
                    [this](const SearchInfo& info) { report_info(info); }, stop_,
                    &pondering_);
                while (pondering_.load(std::memory_order_relaxed) &&
                       !stop_.load(std::memory_order_relaxed)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                send(std::string("bestmove ") +
                     (result.has_move ? result.best_move.uci() : "0000"));
            } catch (const std::exception& error) {
                send("info string search failed: " + std::string(error.what()));
                send("bestmove 0000");
            }
        });
    }

    void report_info(const SearchInfo& info) {
        std::ostringstream output;
        output << "info depth " << info.depth << " seldepth "
               << info.selective_depth << " score ";
        if (std::abs(info.score) >= kMateThreshold) {
            const int plies = kMateScore - std::abs(info.score);
            const int moves = (plies + 1) / 2;
            output << "mate " << (info.score < 0 ? -moves : moves);
        } else {
            output << "cp " << info.score;
        }
        const std::int64_t divisor = std::max<std::int64_t>(1, info.elapsed_ms);
        const std::uint64_t divisor_u = static_cast<std::uint64_t>(divisor);
        const std::uint64_t nps =
            info.nodes > std::numeric_limits<std::uint64_t>::max() / 1000U
                ? std::numeric_limits<std::uint64_t>::max()
                : (info.nodes * 1000U) / divisor_u;
        output << " nodes " << info.nodes << " nps " << nps
               << " hashfull " << info.hashfull << " time "
               << info.elapsed_ms << " pv";
        for (Move move : info.principal_variation) {
            output << ' ' << move.uci();
        }
        send(output.str());
    }

    Game game_{};
    TranspositionTable table_;
    std::atomic_bool stop_{false};
    std::atomic_bool pondering_{false};
    std::thread worker_;
    std::mutex output_mutex_;
    std::int64_t move_overhead_ms_ = 10;
};

}  // namespace
}  // namespace zfs::engine

int main() {
    zfs::engine::UciLoop loop;
    return loop.run();
}
