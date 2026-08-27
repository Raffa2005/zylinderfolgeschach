#include "reference_position.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <iterator>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace zfs::test_oracle {
namespace {

constexpr std::uint8_t kWhiteKingSide = 1U << 0U;
constexpr std::uint8_t kWhiteQueenSide = 1U << 1U;
constexpr std::uint8_t kBlackKingSide = 1U << 2U;
constexpr std::uint8_t kBlackQueenSide = 1U << 3U;

[[nodiscard]] int parse_square(std::string_view text) noexcept {
    if (text.size() != 2 || text[0] < 'a' || text[0] > 'h' ||
        text[1] < '1' || text[1] > '8') {
        return -1;
    }
    return (text[1] - '1') * 8 + (text[0] - 'a');
}

[[nodiscard]] std::string square_name(int square) {
    if (square < 0 || square >= 64) {
        return "-";
    }
    std::string result(2, ' ');
    result[0] = static_cast<char>('a' + square % 8);
    result[1] = static_cast<char>('1' + square / 8);
    return result;
}

[[nodiscard]] bool is_piece_character(char character) noexcept {
    switch (character) {
        case 'P':
        case 'N':
        case 'B':
        case 'R':
        case 'Q':
        case 'K':
        case 'p':
        case 'n':
        case 'b':
        case 'r':
        case 'q':
        case 'k': return true;
        default: return false;
    }
}

[[nodiscard]] char lower_piece(char piece) noexcept {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(piece)));
}

[[nodiscard]] std::vector<std::string_view> fields(std::string_view text) {
    std::vector<std::string_view> result;
    std::size_t cursor = 0;
    while (cursor < text.size()) {
        while (cursor < text.size() &&
               std::isspace(static_cast<unsigned char>(text[cursor])) != 0) {
            ++cursor;
        }
        if (cursor == text.size()) {
            break;
        }
        const std::size_t begin = cursor;
        while (cursor < text.size() &&
               std::isspace(static_cast<unsigned char>(text[cursor])) == 0) {
            ++cursor;
        }
        result.push_back(text.substr(begin, cursor - begin));
    }
    return result;
}

template <typename Integer>
[[nodiscard]] bool parse_number(std::string_view text, Integer& output) noexcept {
    Integer value{};
    const auto conversion =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if (conversion.ec != std::errc{} ||
        conversion.ptr != text.data() + text.size()) {
        return false;
    }
    output = value;
    return true;
}

void set_error(std::string* output, std::string message) {
    if (output != nullptr) {
        *output = std::move(message);
    }
}

}  // namespace

Position::Side Position::opposite(Side side) noexcept {
    return side == Side::White ? Side::Black : Side::White;
}

bool Position::belongs_to(char piece, Side side) noexcept {
    if (!is_piece_character(piece)) {
        return false;
    }
    const bool white = std::isupper(static_cast<unsigned char>(piece)) != 0;
    return white == (side == Side::White);
}

int Position::square(int file, int rank) noexcept {
    const int wrapped_file = (file % 8 + 8) % 8;
    return rank * 8 + wrapped_file;
}

int Position::file_of(int square_value) noexcept { return square_value % 8; }

int Position::rank_of(int square_value) noexcept { return square_value / 8; }

std::optional<Position> Position::from_fen(std::string_view fen,
                                            std::string* error) {
    const auto parts = fields(fen);
    if (parts.size() != 6 && parts.size() != 7) {
        set_error(error, "expected six or seven ZFS-FEN fields");
        return std::nullopt;
    }

    Position result;
    result.board_.fill('.');
    int rank = 7;
    int file = 0;
    for (char character : parts[0]) {
        if (character == '/') {
            if (file != 8 || rank == 0) {
                set_error(error, "invalid piece-placement ranks");
                return std::nullopt;
            }
            --rank;
            file = 0;
            continue;
        }
        if (character >= '1' && character <= '8') {
            file += character - '0';
            if (file > 8) {
                set_error(error, "piece-placement rank is too wide");
                return std::nullopt;
            }
            continue;
        }
        if (!is_piece_character(character) || file >= 8) {
            set_error(error, "invalid piece-placement character");
            return std::nullopt;
        }
        result.board_[square(file, rank)] = character;
        ++file;
    }
    if (rank != 0 || file != 8) {
        set_error(error, "piece placement is incomplete");
        return std::nullopt;
    }

    if (parts[1] == "w") {
        result.side_to_move_ = Side::White;
    } else if (parts[1] == "b") {
        result.side_to_move_ = Side::Black;
    } else {
        set_error(error, "invalid side to move");
        return std::nullopt;
    }

    if (parts[2] != "-") {
        for (char right : parts[2]) {
            switch (right) {
                case 'K': result.castling_rights_ |= kWhiteKingSide; break;
                case 'Q': result.castling_rights_ |= kWhiteQueenSide; break;
                case 'k': result.castling_rights_ |= kBlackKingSide; break;
                case 'q': result.castling_rights_ |= kBlackQueenSide; break;
                default:
                    set_error(error, "invalid castling right");
                    return std::nullopt;
            }
        }
    }

    if (parts[3] != "-") {
        result.en_passant_ = parse_square(parts[3]);
        if (result.en_passant_ < 0) {
            set_error(error, "invalid en-passant square");
            return std::nullopt;
        }
    }
    if (!parse_number(parts[4], result.halfmove_clock_) ||
        !parse_number(parts[5], result.fullmove_number_) ||
        result.fullmove_number_ == 0) {
        set_error(error, "invalid move counter");
        return std::nullopt;
    }
    if (parts.size() == 7 && parts[6] != "-") {
        result.follow_ = parse_square(parts[6]);
        if (result.follow_ < 0) {
            set_error(error, "invalid follow square");
            return std::nullopt;
        }
    }
    if (error != nullptr) {
        error->clear();
    }
    return result;
}

bool Position::ray_attacks(int from, int target, int file_delta,
                           int rank_delta) const noexcept {
    int file = file_of(from);
    int rank = rank_of(from);
    for (int distance = 1; distance < 8; ++distance) {
        file += file_delta;
        rank += rank_delta;
        if (rank < 0 || rank >= 8) {
            return false;
        }
        const int candidate = square(file, rank);
        if (candidate == target) {
            return true;
        }
        if (board_[candidate] != '.') {
            return false;
        }
    }
    return false;
}

bool Position::attacked(int target, Side by) const noexcept {
    static constexpr std::array<std::array<int, 2>, 8> kKnightOffsets{{
        {{-2, -1}}, {{-2, 1}}, {{-1, -2}}, {{-1, 2}},
        {{1, -2}},  {{1, 2}},  {{2, -1}},  {{2, 1}},
    }};

    for (int from = 0; from < 64; ++from) {
        const char piece = board_[from];
        if (!belongs_to(piece, by)) {
            continue;
        }
        const int from_file = file_of(from);
        const int from_rank = rank_of(from);
        switch (lower_piece(piece)) {
            case 'p': {
                const int rank_delta = by == Side::White ? 1 : -1;
                const int target_rank = from_rank + rank_delta;
                if (target_rank >= 0 && target_rank < 8 &&
                    (square(from_file - 1, target_rank) == target ||
                     square(from_file + 1, target_rank) == target)) {
                    return true;
                }
                break;
            }
            case 'n':
                for (const auto& offset : kKnightOffsets) {
                    const int target_rank = from_rank + offset[1];
                    if (target_rank >= 0 && target_rank < 8 &&
                        square(from_file + offset[0], target_rank) == target) {
                        return true;
                    }
                }
                break;
            case 'k':
                for (int rank_delta = -1; rank_delta <= 1; ++rank_delta) {
                    const int target_rank = from_rank + rank_delta;
                    if (target_rank < 0 || target_rank >= 8) {
                        continue;
                    }
                    for (int file_delta = -1; file_delta <= 1; ++file_delta) {
                        if ((file_delta != 0 || rank_delta != 0) &&
                            square(from_file + file_delta, target_rank) == target) {
                            return true;
                        }
                    }
                }
                break;
            case 'b':
                if (ray_attacks(from, target, 1, 1) ||
                    ray_attacks(from, target, 1, -1) ||
                    ray_attacks(from, target, -1, 1) ||
                    ray_attacks(from, target, -1, -1)) {
                    return true;
                }
                break;
            case 'r':
                if (ray_attacks(from, target, 1, 0) ||
                    ray_attacks(from, target, -1, 0) ||
                    ray_attacks(from, target, 0, 1) ||
                    ray_attacks(from, target, 0, -1)) {
                    return true;
                }
                break;
            case 'q':
                if (ray_attacks(from, target, 1, 0) ||
                    ray_attacks(from, target, -1, 0) ||
                    ray_attacks(from, target, 0, 1) ||
                    ray_attacks(from, target, 0, -1) ||
                    ray_attacks(from, target, 1, 1) ||
                    ray_attacks(from, target, 1, -1) ||
                    ray_attacks(from, target, -1, 1) ||
                    ray_attacks(from, target, -1, -1)) {
                    return true;
                }
                break;
            default: break;
        }
    }
    return false;
}

int Position::king_square(Side side) const noexcept {
    const char king = side == Side::White ? 'K' : 'k';
    const auto found = std::find(board_.begin(), board_.end(), king);
    return found == board_.end()
               ? -1
               : static_cast<int>(std::distance(board_.begin(), found));
}

void Position::add_move(std::vector<Move>& moves, Move move) const {
    if (std::find(moves.begin(), moves.end(), move) == moves.end()) {
        moves.push_back(move);
    }
}

void Position::add_piece_target(std::vector<Move>& moves, int from, int to) const {
    const char target = board_[to];
    if (belongs_to(target, side_to_move_)) {
        return;
    }
    if (target == '.') {
        add_move(moves, Move{from, to, MoveKind::Quiet, 'n'});
        return;
    }
    if (lower_piece(target) != 'k') {
        add_move(moves, Move{from, to, MoveKind::Capture, 'n'});
    }
}

void Position::add_slider_moves(std::vector<Move>& moves, int from,
                                int file_delta, int rank_delta) const {
    int file = file_of(from);
    int rank = rank_of(from);
    for (int distance = 1; distance < 8; ++distance) {
        file += file_delta;
        rank += rank_delta;
        if (rank < 0 || rank >= 8) {
            return;
        }
        const int to = square(file, rank);
        const char target = board_[to];
        add_piece_target(moves, from, to);
        if (target != '.') {
            return;
        }
    }
}

void Position::add_castles(std::vector<Move>& moves) const {
    const bool white = side_to_move_ == Side::White;
    const int rank = white ? 0 : 7;
    const int king_from = square(4, rank);
    const char king = white ? 'K' : 'k';
    const char rook = white ? 'R' : 'r';
    const Side enemy = opposite(side_to_move_);
    if (board_[king_from] != king || attacked(king_from, enemy)) {
        return;
    }

    const std::uint8_t king_side = white ? kWhiteKingSide : kBlackKingSide;
    if ((castling_rights_ & king_side) != 0 &&
        board_[square(7, rank)] == rook && board_[square(5, rank)] == '.' &&
        board_[square(6, rank)] == '.') {
        Position transit = *this;
        transit.board_[king_from] = '.';
        transit.board_[square(5, rank)] = king;
        if (!transit.attacked(square(5, rank), enemy)) {
            add_move(moves, Move{king_from, square(6, rank),
                                 MoveKind::KingCastle, 'n'});
        }
    }

    const std::uint8_t queen_side = white ? kWhiteQueenSide : kBlackQueenSide;
    if ((castling_rights_ & queen_side) != 0 &&
        board_[square(0, rank)] == rook && board_[square(1, rank)] == '.' &&
        board_[square(2, rank)] == '.' && board_[square(3, rank)] == '.') {
        Position transit = *this;
        transit.board_[king_from] = '.';
        transit.board_[square(3, rank)] = king;
        if (!transit.attacked(square(3, rank), enemy)) {
            add_move(moves, Move{king_from, square(2, rank),
                                 MoveKind::QueenCastle, 'n'});
        }
    }
}

std::vector<Position::Move> Position::pseudo_legal_moves() const {
    static constexpr std::array<std::array<int, 2>, 8> kKnightOffsets{{
        {{-2, -1}}, {{-2, 1}}, {{-1, -2}}, {{-1, 2}},
        {{1, -2}},  {{1, 2}},  {{2, -1}},  {{2, 1}},
    }};
    std::vector<Move> result;
    result.reserve(128);

    for (int from = 0; from < 64; ++from) {
        const char piece = board_[from];
        if (!belongs_to(piece, side_to_move_)) {
            continue;
        }
        const int file = file_of(from);
        const int rank = rank_of(from);
        switch (lower_piece(piece)) {
            case 'p': {
                const int rank_step = side_to_move_ == Side::White ? 1 : -1;
                const int promotion_rank = side_to_move_ == Side::White ? 7 : 0;
                const int start_rank = side_to_move_ == Side::White ? 1 : 6;
                const int next_rank = rank + rank_step;
                if (next_rank < 0 || next_rank >= 8) {
                    break;
                }
                const int forward = square(file, next_rank);
                if (board_[forward] == '.') {
                    if (next_rank == promotion_rank) {
                        for (char promotion : {'n', 'b', 'r', 'q'}) {
                            add_move(result, Move{from, forward,
                                                  MoveKind::Promotion, promotion});
                        }
                    } else {
                        add_move(result,
                                 Move{from, forward, MoveKind::Quiet, 'n'});
                        const int double_to = square(file, rank + 2 * rank_step);
                        if (rank == start_rank && board_[double_to] == '.') {
                            add_move(result, Move{from, double_to,
                                                  MoveKind::DoublePawnPush, 'n'});
                        }
                    }
                }
                for (int file_step : {-1, 1}) {
                    const int to = square(file + file_step, next_rank);
                    const char target = board_[to];
                    if (belongs_to(target, opposite(side_to_move_)) &&
                        lower_piece(target) != 'k') {
                        if (next_rank == promotion_rank) {
                            for (char promotion : {'n', 'b', 'r', 'q'}) {
                                add_move(result,
                                         Move{from, to, MoveKind::PromotionCapture,
                                              promotion});
                            }
                        } else {
                            add_move(result,
                                     Move{from, to, MoveKind::Capture, 'n'});
                        }
                    }
                    const int captured = square(file + file_step, rank);
                    const char enemy_pawn =
                        side_to_move_ == Side::White ? 'p' : 'P';
                    if (to == en_passant_ && board_[to] == '.' &&
                        board_[captured] == enemy_pawn) {
                        add_move(result,
                                 Move{from, to, MoveKind::EnPassant, 'n'});
                    }
                }
                break;
            }
            case 'n':
                for (const auto& offset : kKnightOffsets) {
                    const int target_rank = rank + offset[1];
                    if (target_rank >= 0 && target_rank < 8) {
                        add_piece_target(result, from,
                                         square(file + offset[0], target_rank));
                    }
                }
                break;
            case 'b':
                add_slider_moves(result, from, 1, 1);
                add_slider_moves(result, from, 1, -1);
                add_slider_moves(result, from, -1, 1);
                add_slider_moves(result, from, -1, -1);
                break;
            case 'r':
                add_slider_moves(result, from, 1, 0);
                add_slider_moves(result, from, -1, 0);
                add_slider_moves(result, from, 0, 1);
                add_slider_moves(result, from, 0, -1);
                break;
            case 'q':
                add_slider_moves(result, from, 1, 0);
                add_slider_moves(result, from, -1, 0);
                add_slider_moves(result, from, 0, 1);
                add_slider_moves(result, from, 0, -1);
                add_slider_moves(result, from, 1, 1);
                add_slider_moves(result, from, 1, -1);
                add_slider_moves(result, from, -1, 1);
                add_slider_moves(result, from, -1, -1);
                break;
            case 'k':
                for (int rank_delta = -1; rank_delta <= 1; ++rank_delta) {
                    const int target_rank = rank + rank_delta;
                    if (target_rank < 0 || target_rank >= 8) {
                        continue;
                    }
                    for (int file_delta = -1; file_delta <= 1; ++file_delta) {
                        if (file_delta != 0 || rank_delta != 0) {
                            add_piece_target(result, from,
                                             square(file + file_delta, target_rank));
                        }
                    }
                }
                break;
            default: break;
        }
    }
    add_castles(result);
    return result;
}

Position Position::apply_unchecked(const Move& move) const noexcept {
    Position next = *this;
    const char moving = next.board_[move.from];
    const Side mover = side_to_move_;
    const bool pawn_move = lower_piece(moving) == 'p';
    const bool capture = move.kind == MoveKind::Capture ||
                         move.kind == MoveKind::EnPassant ||
                         move.kind == MoveKind::PromotionCapture;

    if (lower_piece(moving) == 'k') {
        next.castling_rights_ &= static_cast<std::uint8_t>(
            mover == Side::White ? ~(kWhiteKingSide | kWhiteQueenSide)
                                 : ~(kBlackKingSide | kBlackQueenSide));
    }
    if (lower_piece(moving) == 'r') {
        if (move.from == square(0, 0))
            next.castling_rights_ &= static_cast<std::uint8_t>(~kWhiteQueenSide);
        if (move.from == square(7, 0))
            next.castling_rights_ &= static_cast<std::uint8_t>(~kWhiteKingSide);
        if (move.from == square(0, 7))
            next.castling_rights_ &= static_cast<std::uint8_t>(~kBlackQueenSide);
        if (move.from == square(7, 7))
            next.castling_rights_ &= static_cast<std::uint8_t>(~kBlackKingSide);
    }

    int captured_square = move.to;
    if (move.kind == MoveKind::EnPassant) {
        captured_square += mover == Side::White ? -8 : 8;
    }
    if (capture) {
        if (captured_square == square(0, 0))
            next.castling_rights_ &= static_cast<std::uint8_t>(~kWhiteQueenSide);
        if (captured_square == square(7, 0))
            next.castling_rights_ &= static_cast<std::uint8_t>(~kWhiteKingSide);
        if (captured_square == square(0, 7))
            next.castling_rights_ &= static_cast<std::uint8_t>(~kBlackQueenSide);
        if (captured_square == square(7, 7))
            next.castling_rights_ &= static_cast<std::uint8_t>(~kBlackKingSide);
        next.board_[captured_square] = '.';
    }

    next.en_passant_ = -1;
    next.follow_ = (move.kind == MoveKind::KingCastle ||
                    move.kind == MoveKind::QueenCastle)
                       ? -1
                       : move.from;
    if (pawn_move || capture) {
        next.halfmove_clock_ = 0;
    } else if (next.halfmove_clock_ !=
               std::numeric_limits<std::uint16_t>::max()) {
        ++next.halfmove_clock_;
    }

    next.board_[move.from] = '.';
    switch (move.kind) {
        case MoveKind::KingCastle: {
            const int rank = mover == Side::White ? 0 : 7;
            next.board_[move.to] = moving;
            next.board_[square(7, rank)] = '.';
            next.board_[square(5, rank)] = mover == Side::White ? 'R' : 'r';
            break;
        }
        case MoveKind::QueenCastle: {
            const int rank = mover == Side::White ? 0 : 7;
            next.board_[move.to] = moving;
            next.board_[square(0, rank)] = '.';
            next.board_[square(3, rank)] = mover == Side::White ? 'R' : 'r';
            break;
        }
        case MoveKind::Promotion:
        case MoveKind::PromotionCapture:
            next.board_[move.to] =
                mover == Side::White
                    ? static_cast<char>(std::toupper(
                          static_cast<unsigned char>(move.promotion)))
                    : move.promotion;
            break;
        default:
            next.board_[move.to] = moving;
            if (move.kind == MoveKind::DoublePawnPush) {
                next.en_passant_ =
                    move.from + (mover == Side::White ? 8 : -8);
            }
            break;
    }

    if (mover == Side::Black &&
        next.fullmove_number_ != std::numeric_limits<std::uint32_t>::max()) {
        ++next.fullmove_number_;
    }
    next.side_to_move_ = opposite(mover);
    return next;
}

std::vector<Position::Move> Position::ordinary_legal_moves() const {
    std::vector<Move> result;
    const Side mover = side_to_move_;
    for (const Move& move : pseudo_legal_moves()) {
        const Position next = apply_unchecked(move);
        const int king = next.king_square(mover);
        if (king >= 0 && !next.attacked(king, opposite(mover))) {
            result.push_back(move);
        }
    }
    return result;
}

std::vector<Position::Move> Position::legal_moves() const {
    std::vector<Move> ordinary = ordinary_legal_moves();
    if (follow_ < 0) {
        return ordinary;
    }
    std::vector<Move> following;
    std::copy_if(ordinary.begin(), ordinary.end(), std::back_inserter(following),
                 [this](const Move& move) { return move.to == follow_; });
    return following.empty() ? ordinary : following;
}

std::string Position::move_uci(const Move& move) const {
    std::string result = square_name(move.from) + square_name(move.to);
    if (move.kind == MoveKind::Promotion ||
        move.kind == MoveKind::PromotionCapture) {
        result.push_back(move.promotion);
    }
    return result;
}

std::vector<std::string> Position::legal_uci() const {
    std::vector<std::string> result;
    for (const Move& move : legal_moves()) {
        result.push_back(move_uci(move));
    }
    std::ranges::sort(result);
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

std::optional<Position> Position::after_uci(std::string_view uci) const {
    for (const Move& move : legal_moves()) {
        if (move_uci(move) == uci) {
            return apply_unchecked(move);
        }
    }
    return std::nullopt;
}

std::string Position::to_fen() const {
    std::string result;
    for (int rank = 7; rank >= 0; --rank) {
        int empty = 0;
        for (int file = 0; file < 8; ++file) {
            const char piece = board_[square(file, rank)];
            if (piece == '.') {
                ++empty;
                continue;
            }
            if (empty != 0) {
                result.push_back(static_cast<char>('0' + empty));
                empty = 0;
            }
            result.push_back(piece);
        }
        if (empty != 0) {
            result.push_back(static_cast<char>('0' + empty));
        }
        if (rank != 0) {
            result.push_back('/');
        }
    }
    result += side_to_move_ == Side::White ? " w " : " b ";
    if (castling_rights_ == 0) {
        result.push_back('-');
    } else {
        if ((castling_rights_ & kWhiteKingSide) != 0) result.push_back('K');
        if ((castling_rights_ & kWhiteQueenSide) != 0) result.push_back('Q');
        if ((castling_rights_ & kBlackKingSide) != 0) result.push_back('k');
        if ((castling_rights_ & kBlackQueenSide) != 0) result.push_back('q');
    }
    result.push_back(' ');
    result += square_name(en_passant_);
    result.push_back(' ');
    result += std::to_string(halfmove_clock_);
    result.push_back(' ');
    result += std::to_string(fullmove_number_);
    result.push_back(' ');
    result += square_name(follow_);
    return result;
}

}  // namespace zfs::test_oracle
