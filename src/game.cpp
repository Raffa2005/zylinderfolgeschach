#include "zfs/game.hpp"

#include <algorithm>
#include <cstddef>
#include <utility>

namespace zfs {

Game::Game() : Game(Position::start()) {}

Game::Game(Position root) : position_(std::move(root)) {
    positions_.push_back(position_);
}

void Game::reset(Position root) {
    position_ = std::move(root);
    positions_.clear();
    moves_.clear();
    positions_.push_back(position_);
}

bool Game::play(Move move) {
    if (automatic_draw()) {
        return false;
    }
    MoveList legal;
    position_.generate_legal_moves(legal);
    const auto found = std::find(legal.begin(), legal.end(), move);
    if (found == legal.end()) {
        return false;
    }

    commit(*found);
    return true;
}

std::optional<Move> Game::play_uci(std::string_view uci) {
    if (automatic_draw()) {
        return std::nullopt;
    }
    const std::optional<Move> move = position_.parse_uci(uci);
    if (!move) {
        return std::nullopt;
    }
    commit(*move);
    return move;
}

bool Game::automatic_draw() const {
    return position_.halfmove_clock() >= 100U || repetition_count() >= 3U;
}

void Game::commit(Move move) {
    Undo undo;
    position_.do_move(move, undo);
    moves_.push_back(move);
    positions_.push_back(position_);
}

unsigned Game::repetition_count() const {
    const std::size_t current = positions_.size() - 1U;
    unsigned count = 1;
    const std::size_t reversible = std::min<std::size_t>(
        current, static_cast<std::size_t>(position_.halfmove_clock()));

    for (std::size_t distance = 2; distance <= reversible; distance += 2) {
        const Position& candidate = positions_[current - distance];
        if (candidate.castling_rights() != position_.castling_rights()) {
            break;
        }
        if (candidate.base_key() == position_.base_key() &&
            position_.same_repetition_state(candidate)) {
            ++count;
        }
    }
    return count;
}

GameState Game::state() {
    MoveList legal;
    position_.generate_legal_moves(legal);
    if (legal.empty()) {
        return position_.in_check(position_.side_to_move())
                   ? GameState::Checkmate
                   : GameState::Stalemate;
    }
    if (position_.halfmove_clock() >= 100U) {
        return GameState::FiftyMoveDraw;
    }
    if (repetition_count() >= 3U) {
        return GameState::ThreefoldDraw;
    }
    return GameState::Ongoing;
}

}  // namespace zfs
