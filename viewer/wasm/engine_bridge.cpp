#include "engine/search.hpp"
#include "engine/tt.hpp"
#include "zfs/game.hpp"
#include "zfs/position.hpp"

#include <emscripten/emscripten.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::size_t kHashMegabytes = 32;

zfs::engine::TranspositionTable table{kHashMegabytes};
std::string response;

EM_JS(void, emit_info, (const char* json), {
    if (typeof self !== 'undefined' &&
        typeof self.postMessage === 'function' &&
        self.kugelfischSearchId !== undefined) {
        self.postMessage({
            type: 'info',
            id: self.kugelfischSearchId,
            info: JSON.parse(UTF8ToString(json)),
        });
    }
});

[[nodiscard]] bool bounded_c_string(const char* input, std::size_t maximum,
                                    std::string_view& output) noexcept {
    if (input == nullptr) {
        return false;
    }
    std::size_t length = 0;
    while (length <= maximum && input[length] != '\0') {
        ++length;
    }
    if (length > maximum) {
        return false;
    }
    output = std::string_view(input, length);
    return true;
}

void append_json_string(std::string& output, std::string_view value) {
    constexpr char kHex[] = "0123456789abcdef";
    output.push_back('"');
    for (const unsigned char character : value) {
        switch (character) {
            case '"': output += "\\\""; break;
            case '\\': output += "\\\\"; break;
            case '\b': output += "\\b"; break;
            case '\f': output += "\\f"; break;
            case '\n': output += "\\n"; break;
            case '\r': output += "\\r"; break;
            case '\t': output += "\\t"; break;
            default:
                if (character < 0x20U) {
                    output += "\\u00";
                    output.push_back(kHex[character >> 4U]);
                    output.push_back(kHex[character & 0x0fU]);
                } else {
                    output.push_back(static_cast<char>(character));
                }
                break;
        }
    }
    output.push_back('"');
}

[[nodiscard]] int mate_moves(int score) noexcept {
    const int plies = zfs::engine::kMateScore - std::abs(score);
    const int moves = (plies + 1) / 2;
    return score < 0 ? -moves : moves;
}

[[nodiscard]] std::uint64_t nodes_per_second(
    std::uint64_t nodes, std::int64_t elapsed_ms) noexcept {
    const std::uint64_t divisor = static_cast<std::uint64_t>(
        std::max<std::int64_t>(1, elapsed_ms));
    if (nodes > std::numeric_limits<std::uint64_t>::max() / 1000U) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return (nodes * 1000U) / divisor;
}

void append_info(std::string& output, int depth, int selective_depth, int score,
                 std::uint64_t nodes, std::int64_t elapsed_ms, int hashfull,
                 const std::vector<zfs::Move>& pv) {
    output += "{\"depth\":" + std::to_string(depth);
    output += ",\"seldepth\":" + std::to_string(selective_depth);
    output += ",\"score\":{\"kind\":\"";
    if (std::abs(score) >= zfs::engine::kMateThreshold) {
        output += "mate\",\"value\":" + std::to_string(mate_moves(score));
    } else {
        output += "cp\",\"value\":" + std::to_string(score);
    }
    output += "},\"nodes\":" + std::to_string(nodes);
    output += ",\"nps\":" +
              std::to_string(nodes_per_second(nodes, elapsed_ms));
    output += ",\"hashfull\":" + std::to_string(hashfull);
    output += ",\"time\":" + std::to_string(elapsed_ms);
    output += ",\"pv\":[";
    bool first = true;
    for (zfs::Move move : pv) {
        if (!first) {
            output.push_back(',');
        }
        first = false;
        append_json_string(output, move.uci());
    }
    output += "]}";
}

[[nodiscard]] const char* error_response(std::string_view message) {
    response = "{\"ok\":false,\"error\":";
    append_json_string(response, message);
    response.push_back('}');
    return response.c_str();
}

[[nodiscard]] bool apply_moves(zfs::Game& game, std::string_view moves,
                               std::string& error) {
    std::size_t offset = 0;
    while (offset < moves.size()) {
        while (offset < moves.size() && moves[offset] == ' ') {
            ++offset;
        }
        if (offset == moves.size()) {
            break;
        }
        const std::size_t end = moves.find(' ', offset);
        const std::string_view token = moves.substr(
            offset, end == std::string_view::npos ? moves.size() - offset
                                                   : end - offset);
        if ((token.size() != 4U && token.size() != 5U) ||
            !game.play_uci(token)) {
            error = "illegal move in engine position: ";
            error += token;
            return false;
        }
        offset = end == std::string_view::npos ? moves.size() : end + 1U;
    }
    return true;
}

}  // namespace

extern "C" {

void zfs_engine_new_game() { table.clear(); }

const char* zfs_engine_search(const char* root_fen, const char* move_line,
                              int depth) {
    std::string_view fen;
    std::string_view moves;
    if (!bounded_c_string(root_fen, 256, fen)) {
        return error_response("root ZFS-FEN is missing or too long");
    }
    if (!bounded_c_string(move_line, 24'576, moves)) {
        return error_response("move history is missing or too long");
    }
    if (depth < 1 || depth > zfs::engine::kMaximumSearchDepth) {
        return error_response("search depth is out of range");
    }

    std::string parse_error;
    const std::optional<zfs::Position> root =
        zfs::Position::from_fen(fen, &parse_error);
    if (!root) {
        return error_response(parse_error);
    }
    zfs::Game game(*root);
    if (!apply_moves(game, moves, parse_error)) {
        return error_response(parse_error);
    }

    zfs::engine::SearchLimits limits;
    limits.depth = depth;
    std::atomic_bool stop{false};
    zfs::engine::Searcher searcher(table);
    const zfs::engine::SearchResult result = searcher.search(
        game, limits,
        [](const zfs::engine::SearchInfo& info) {
            std::string json;
            json.reserve(256U + info.principal_variation.size() * 10U);
            append_info(json, info.depth, info.selective_depth, info.score,
                        info.nodes, info.elapsed_ms, info.hashfull,
                        info.principal_variation);
            emit_info(json.c_str());
        },
        stop);

    response = "{\"ok\":true,\"move\":";
    append_json_string(response,
                       result.has_move ? result.best_move.uci() : "0000");
    response += ",\"info\":";
    append_info(response, result.depth, result.selective_depth, result.score,
                result.nodes, result.elapsed_ms, table.hashfull(),
                result.principal_variation);
    response.push_back('}');
    return response.c_str();
}

}  // extern "C"
