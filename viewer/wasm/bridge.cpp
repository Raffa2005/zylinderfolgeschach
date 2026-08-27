#include "bridge.hpp"

#include "zfs/position.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

std::vector<zfs::Position> timeline_positions{zfs::Position::start()};
std::vector<std::string> timeline_moves;
std::size_t timeline_cursor = 0;
std::string response;
std::string last_error;

[[nodiscard]] zfs::Position& current_position() noexcept {
    assert(timeline_cursor < timeline_positions.size());
    return timeline_positions[timeline_cursor];
}

void reset_timeline(zfs::Position initial) {
    timeline_positions.clear();
    timeline_positions.push_back(std::move(initial));
    timeline_moves.clear();
    timeline_cursor = 0;
}

[[nodiscard]] std::string square_name(zfs::Square square) {
    if (!zfs::valid_square(square)) {
        return "-";
    }
    std::string name(2, ' ');
    name[0] = static_cast<char>('a' + zfs::file_of(square));
    name[1] = static_cast<char>('1' + zfs::rank_of(square));
    return name;
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

void set_error(std::string message) { last_error = std::move(message); }

[[nodiscard]] unsigned repetition_count() {
    const zfs::Position& current = current_position();
    unsigned count = 1;
    const std::size_t reversible = std::min<std::size_t>(
        timeline_cursor, static_cast<std::size_t>(current.halfmove_clock()));
    for (std::size_t distance = 2; distance <= reversible; distance += 2) {
        const zfs::Position& candidate =
            timeline_positions[timeline_cursor - distance];
        if (candidate.castling_rights() != current.castling_rights()) {
            break;
        }
        if (candidate.base_key() == current.base_key() &&
            current.same_repetition_state(candidate)) {
            ++count;
        }
    }
    return count;
}

enum class ViewerState {
    Ongoing,
    Checkmate,
    Stalemate,
    ThreefoldDraw,
    FiftyMoveDraw,
};

[[nodiscard]] ViewerState current_state(zfs::MoveList& moves) {
    zfs::Position& position = current_position();
    position.generate_legal_moves(moves);
    if (moves.empty()) {
        return position.in_check(position.side_to_move())
                   ? ViewerState::Checkmate
                   : ViewerState::Stalemate;
    }
    if (position.halfmove_clock() >= 100U) {
        return ViewerState::FiftyMoveDraw;
    }
    if (repetition_count() >= 3U) {
        return ViewerState::ThreefoldDraw;
    }
    return ViewerState::Ongoing;
}

}  // namespace

extern "C" {

void zfs_reset() {
    reset_timeline(zfs::Position::start());
    last_error.clear();
}

int zfs_load(const char* fen) {
    std::string_view input;
    if (!bounded_c_string(fen, 256, input)) {
        set_error("ZFS-FEN is missing or too long");
        return 0;
    }

    std::string error;
    auto loaded = zfs::Position::from_fen(input, &error);
    if (!loaded) {
        set_error(std::move(error));
        return 0;
    }
    reset_timeline(std::move(*loaded));
    last_error.clear();
    return 1;
}

int zfs_play(const char* uci) {
    std::string_view input;
    if (!bounded_c_string(uci, 5, input) ||
        (input.size() != 4 && input.size() != 5)) {
        set_error("move must be four- or five-character UCI");
        return 0;
    }

    zfs::MoveList legal;
    if (current_state(legal) != ViewerState::Ongoing) {
        set_error("the game has already ended");
        return 0;
    }

    const auto move = std::find_if(legal.begin(), legal.end(), [input](zfs::Move candidate) {
        return candidate.uci() == input;
    });
    if (move == legal.end()) {
        set_error("move is not legal in the current ZFS position");
        return 0;
    }

    while (timeline_positions.size() > timeline_cursor + 1U) {
        timeline_positions.pop_back();
    }
    while (timeline_moves.size() > timeline_cursor) {
        timeline_moves.pop_back();
    }

    zfs::Position next = current_position();
    zfs::Undo undo;
    next.do_move(*move, undo);
    timeline_moves.emplace_back(input);
    timeline_positions.push_back(std::move(next));
    ++timeline_cursor;
    last_error.clear();
    return 1;
}

int zfs_back() {
    if (timeline_cursor == 0) {
        set_error("already at the beginning of the move history");
        return 0;
    }
    --timeline_cursor;
    last_error.clear();
    return 1;
}

int zfs_forward() {
    if (timeline_cursor == timeline_moves.size()) {
        set_error("already at the end of the move history");
        return 0;
    }
    ++timeline_cursor;
    last_error.clear();
    return 1;
}

const char* zfs_last_error() { return last_error.c_str(); }

const char* zfs_state_json() {
    zfs::Position& position = current_position();
    zfs::MoveList moves;
    const ViewerState terminal = current_state(moves);
    const bool in_check = position.in_check(position.side_to_move());
    const std::string fen = position.to_fen();
    const std::size_t board_end = fen.find(' ');
    const zfs::Square follow_square = position.follow_square();
    const std::string follow = square_name(follow_square);
    const std::string en_passant =
        square_name(position.en_passant_square());
    const bool follow_forced =
        zfs::valid_square(follow_square) && !moves.empty() &&
        std::all_of(moves.begin(), moves.end(), [follow_square](zfs::Move move) {
            return move.to() == follow_square;
        });

    response.clear();
    response.reserve(288U + moves.size() * 8U + timeline_moves.size() * 8U);
    response += "{\"fen\":";
    append_json_string(response, fen);
    response += ",\"board\":";
    append_json_string(response, std::string_view(fen).substr(0, board_end));
    response += ",\"turn\":\"";
    response +=
        position.side_to_move() == zfs::Color::White ? "white" : "black";
    response += "\",\"inCheck\":";
    response += in_check ? "true" : "false";
    response += ",\"terminal\":\"";
    switch (terminal) {
        case ViewerState::Ongoing: response += "ongoing"; break;
        case ViewerState::Checkmate: response += "checkmate"; break;
        case ViewerState::Stalemate: response += "stalemate"; break;
        case ViewerState::ThreefoldDraw: response += "threefold"; break;
        case ViewerState::FiftyMoveDraw: response += "fifty-move"; break;
    }
    response += "\",\"follow\":";
    append_json_string(response, follow);
    response += ",\"followForced\":";
    response += follow_forced ? "true" : "false";
    response += ",\"enPassant\":";
    append_json_string(response, en_passant);
    response += ",\"legalMoves\":[";
    bool first = true;
    if (terminal != ViewerState::Ongoing) {
        moves.clear();
    }
    for (zfs::Move move : moves) {
        if (!first) {
            response.push_back(',');
        }
        first = false;
        append_json_string(response, move.uci());
    }
    response += "],\"historyCursor\":";
    response += std::to_string(timeline_cursor);
    response += ",\"history\":[";
    first = true;
    for (const std::string& move : timeline_moves) {
        if (!first) {
            response.push_back(',');
        }
        first = false;
        append_json_string(response, move);
    }
    response += "]}";
    return response.c_str();
}

}  // extern "C"
