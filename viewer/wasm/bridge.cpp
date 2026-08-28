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
std::vector<std::string> timeline_san;
std::size_t timeline_cursor = 0;
std::string response;
std::string notation_response;
std::string last_error;

[[nodiscard]] zfs::Position& current_position() noexcept {
    assert(timeline_cursor < timeline_positions.size());
    return timeline_positions[timeline_cursor];
}

void reset_timeline(zfs::Position initial) {
    timeline_positions.clear();
    timeline_positions.push_back(std::move(initial));
    timeline_moves.clear();
    timeline_san.clear();
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

[[nodiscard]] char san_piece_letter(zfs::PieceType type) noexcept {
    switch (type) {
        case zfs::PieceType::Knight: return 'N';
        case zfs::PieceType::Bishop: return 'B';
        case zfs::PieceType::Rook: return 'R';
        case zfs::PieceType::Queen: return 'Q';
        case zfs::PieceType::King: return 'K';
        case zfs::PieceType::Pawn:
        case zfs::PieceType::None: return '\0';
    }
    return '\0';
}

[[nodiscard]] std::string move_san(const zfs::Position& before, zfs::Move move,
                                   const zfs::MoveList& legal,
                                   zfs::Position& after) {
    std::string san;
    if (move.type() == zfs::MoveType::KingCastle) {
        san = "O-O";
    } else if (move.type() == zfs::MoveType::QueenCastle) {
        san = "O-O-O";
    } else {
        const zfs::PieceType type = zfs::piece_type(before.piece_at(move.from()));
        const bool pawn = type == zfs::PieceType::Pawn;
        if (!pawn) {
            san.push_back(san_piece_letter(type));
            bool ambiguous = false;
            bool shares_file = false;
            bool shares_rank = false;
            for (zfs::Move candidate : legal) {
                if (candidate == move || candidate.to() != move.to() ||
                    zfs::piece_type(before.piece_at(candidate.from())) != type) {
                    continue;
                }
                ambiguous = true;
                shares_file |= zfs::file_of(candidate.from()) == zfs::file_of(move.from());
                shares_rank |= zfs::rank_of(candidate.from()) == zfs::rank_of(move.from());
            }
            if (ambiguous) {
                if (!shares_file) {
                    san.push_back(static_cast<char>('a' + zfs::file_of(move.from())));
                } else if (!shares_rank) {
                    san.push_back(static_cast<char>('1' + zfs::rank_of(move.from())));
                } else {
                    san += square_name(move.from());
                }
            }
        } else if (move.is_capture()) {
            san.push_back(static_cast<char>('a' + zfs::file_of(move.from())));
        }

        if (move.is_capture()) {
            san.push_back('x');
        }
        san += square_name(move.to());
        if (move.is_promotion()) {
            san.push_back('=');
            san.push_back(san_piece_letter(move.promotion()));
        }
    }

    if (after.in_check(after.side_to_move())) {
        zfs::MoveList replies;
        after.generate_legal_moves(replies);
        san.push_back(replies.empty() ? '#' : '+');
    }
    return san;
}

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
    while (timeline_san.size() > timeline_cursor) {
        timeline_san.pop_back();
    }

    zfs::Position next = current_position();
    zfs::Undo undo;
    next.do_move(*move, undo);
    timeline_san.push_back(move_san(current_position(), *move, legal, next));
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
    response += "],\"sanHistory\":[";
    first = true;
    for (const std::string& san : timeline_san) {
        if (!first) {
            response.push_back(',');
        }
        first = false;
        append_json_string(response, san);
    }
    response += "]}";
    return response.c_str();
}

const char* zfs_line_san(const char* uci_line) {
    std::string_view input;
    notation_response.clear();
    if (!bounded_c_string(uci_line, 4096, input)) {
        return notation_response.c_str();
    }

    zfs::Position position = current_position();
    std::size_t offset = 0;
    bool first = true;
    while (offset < input.size()) {
        while (offset < input.size() && input[offset] == ' ') {
            ++offset;
        }
        if (offset == input.size()) {
            break;
        }
        const std::size_t end = input.find(' ', offset);
        const std::string_view token = input.substr(
            offset, end == std::string_view::npos ? input.size() - offset
                                                   : end - offset);
        if (token.size() != 4 && token.size() != 5) {
            notation_response.clear();
            return notation_response.c_str();
        }

        zfs::MoveList legal;
        position.generate_legal_moves(legal);
        const auto found = std::find_if(
            legal.begin(), legal.end(), [token](zfs::Move move) {
                return move.uci() == token;
            });
        if (found == legal.end()) {
            notation_response.clear();
            return notation_response.c_str();
        }

        zfs::Position next = position;
        zfs::Undo undo;
        next.do_move(*found, undo);
        if (!first) {
            notation_response.push_back(' ');
        }
        first = false;
        notation_response += move_san(position, *found, legal, next);
        position = std::move(next);
        offset = end == std::string_view::npos ? input.size() : end + 1U;
    }
    return notation_response.c_str();
}

}  // extern "C"
