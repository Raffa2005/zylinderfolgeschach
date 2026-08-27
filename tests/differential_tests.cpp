#include "reference_position.hpp"

#include "zfs/position.hpp"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <iostream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using OraclePosition = zfs::test_oracle::Position;

[[noreturn]] void fail(std::string message) { throw std::runtime_error(message); }

void require(bool condition, std::string message) {
    if (!condition) {
        fail(std::move(message));
    }
}

zfs::Position load_production(std::string_view fen) {
    std::string error;
    auto position = zfs::Position::from_fen(fen, &error);
    if (!position) {
        fail("production parser rejected '" + std::string(fen) + "': " + error);
    }
    return *position;
}

OraclePosition load_oracle(std::string_view fen) {
    std::string error;
    auto position = OraclePosition::from_fen(fen, &error);
    if (!position) {
        fail("oracle parser rejected '" + std::string(fen) + "': " + error);
    }
    return *position;
}

std::vector<std::string> production_moves(zfs::Position& position) {
    zfs::MoveList moves;
    position.generate_legal_moves(moves);
    std::vector<std::string> result;
    result.reserve(moves.size());
    for (zfs::Move move : moves) {
        result.push_back(move.uci());
    }
    std::ranges::sort(result);
    require(std::adjacent_find(result.begin(), result.end()) == result.end(),
            "production generator emitted duplicate UCI moves at " +
                position.to_fen());
    return result;
}

std::string joined(const std::vector<std::string>& moves) {
    std::ostringstream output;
    for (const std::string& move : moves) {
        output << ' ' << move;
    }
    return output.str();
}

std::vector<std::string> difference(const std::vector<std::string>& left,
                                    const std::vector<std::string>& right) {
    std::vector<std::string> result;
    std::ranges::set_difference(left, right, std::back_inserter(result));
    return result;
}

std::vector<std::string> compare_moves(zfs::Position& production,
                                       const OraclePosition& oracle,
                                       std::string_view context) {
    const auto actual = production_moves(production);
    const auto expected = oracle.legal_uci();
    if (actual != expected) {
        const auto production_only = difference(actual, expected);
        const auto oracle_only = difference(expected, actual);
        fail(std::string(context) + " move-set mismatch at " +
             production.to_fen() + "\n  production only:" +
             joined(production_only) + "\n  oracle only:" + joined(oracle_only));
    }
    return actual;
}

void compare_transition(zfs::Position& production, const OraclePosition& oracle,
                        std::string_view uci, std::string_view context) {
    const std::string before = production.to_fen();
    const auto production_move = production.parse_uci(uci);
    const auto oracle_next = oracle.after_uci(uci);
    require(production_move.has_value(), std::string(context) +
                                             ": production rejected " +
                                             std::string(uci));
    require(oracle_next.has_value(), std::string(context) +
                                         ": oracle rejected " + std::string(uci));

    zfs::Undo undo;
    production.do_move(*production_move, undo);
    require(production.validate().empty(), std::string(context) +
                                               ": invalid production state after " +
                                               std::string(uci));
    require(production.to_fen() == oracle_next->to_fen(),
            std::string(context) + ": state mismatch after " + std::string(uci) +
                "\n  production: " + production.to_fen() +
                "\n  oracle:     " + oracle_next->to_fen());
    production.undo_move(*production_move, undo);
    require(production.to_fen() == before,
            std::string(context) + ": make/undo did not restore " +
                std::string(uci) + "\n  before: " + before +
                "\n  after:  " + production.to_fen());
    require(production.validate().empty(), std::string(context) +
                                               ": invalid state after undoing " +
                                               std::string(uci));
}

struct SelectedPosition {
    std::string_view label;
    std::string_view fen;
    std::vector<std::string_view> required;
    std::vector<std::string_view> forbidden;
    bool required_are_complete = false;
};

void test_selected_positions() {
    const std::vector<SelectedPosition> positions{
        {"wrapped bishop ray",
         "1k6/8/8/8/8/8/8/1K3B2 w - - 0 1 -",
         {"f1h3", "f1a4", "f1e8"},
         {}},
        {"alternate wrapped bishop path",
         "2k5/8/8/8/8/8/1P6/B3K3 w - - 0 1 -",
         {"a1e5"},
         {}},
        {"alternate wrapped rook path",
         "3k4/8/8/8/RP6/8/8/4K3 w - - 0 1 -",
         {"a4c4"},
         {}},
        {"wrapped knight",
         "3k4/8/8/8/N7/8/8/4K3 w - - 0 1 -",
         {"a4g3", "a4h6"},
         {}},
        {"wrapped king adjacency",
         "3k4/8/8/8/K7/8/8/8 w - - 0 1 -",
         {"a4h4", "a4h5"},
         {}},
        {"wrapped pawn capture",
         "4k3/8/8/7p/P7/8/8/4K3 w - - 0 1 -",
         {"a4h5"},
         {}},
        {"forced follow",
         "7k/8/8/8/8/8/8/R3K3 w - - 0 1 a3",
         {"a1a3"},
         {"a1a2", "e1e2"},
         true},
        {"illegal follow candidate falls back",
         "1k2r3/8/8/8/8/8/4R3/4K3 w - - 0 1 c2",
         {},
         {"e2c2"}},
        {"follow while in check",
         "1k2r3/8/8/8/8/8/R7/4K3 w - - 0 1 e2",
         {"a2e2"},
         {"a2a8", "e1d1"},
         true},
        {"antipodal double-route mate with unavailable follow",
         "rnbqkb1r/ppp1p2p/5p2/3p2p1/Q7/2P2N2/PP1PPPPP/RNB1KB1R b KQkq - 1 7 g4",
         {},
         {},
         true},
        {"both castling sides",
         "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1 -",
         {"e1g1", "e1c1"},
         {}},
        {"castling does not follow via rook",
         "4k3/8/8/8/8/8/8/4K2R w K - 0 1 f1",
         {"e1f1", "h1f1"},
         {"e1g1"},
         true},
        {"castling seam unmask",
         "4k3/8/8/8/8/8/8/r1N1K2R w K - 0 1 -",
         {},
         {"e1g1"}},
        {"quiet promotion follows",
         "1k6/7P/8/8/8/8/8/4K3 w - - 0 1 h8",
         {"h7h8n", "h7h8b", "h7h8r", "h7h8q"},
         {"e1e2"},
         true},
        {"black quiet promotion follows",
         "4k3/8/8/8/8/8/7p/1K6 b - - 0 1 h1",
         {"h2h1n", "h2h1b", "h2h1r", "h2h1q"},
         {},
         true},
        {"seam promotion capture",
         "r1k5/7P/8/8/8/8/8/4K3 w - - 0 1 -",
         {"h7a8n", "h7a8b", "h7a8r", "h7a8q"},
         {}},
        {"black seam promotion capture",
         "4k3/8/8/8/8/8/p7/2K4R b - - 0 1 -",
         {"a2h1n", "a2h1b", "a2h1r", "a2h1q"},
         {}},
        {"black castling both sides",
         "r3k2r/8/8/8/8/8/8/4K3 b kq - 0 1 -",
         {"e8g8", "e8c8"},
         {}},
        {"black castling transit attacked",
         "4k2r/8/8/8/1B6/8/8/4K3 b k - 0 1 -",
         {},
         {"e8g8"}},
        {"black castling seam unmask",
         "R1n1k2r/8/8/8/8/8/8/4K3 b k - 0 1 -",
         {},
         {"e8g8"}},
        {"white seam en passant",
         "4k3/8/8/p6P/8/8/8/4K3 w - a6 0 2 a7",
         {"h5a6"},
         {}},
        {"black seam en passant",
         "4k3/8/8/8/p6P/8/8/4K3 b - h3 0 1 h2",
         {"a4h3"},
         {}},
        {"seam en passant exposes king",
         "4k2r/8/8/p6P/8/8/8/7K w - a6 0 2 a7",
         {},
         {"h5a6"}},
        {"orthogonal king-ray blocker",
         "4r2k/8/8/8/8/8/4R3/4K3 w - - 0 1 -",
         {"e2e8"},
         {"e2d2"}},
        {"two-route diagonal blockers",
         "4k3/8/8/1b6/8/7N/4P3/5K2 w - - 0 1 -",
         {},
         {"h3f4", "e2e3"}},
        {"two-route cylindrical rank blockers",
         "4k3/8/8/8/8/8/8/KN2r2N w - - 0 1 -",
         {},
         {"b1c3", "h1f2"}},
        {"move-counter saturation",
         "4k3/8/8/8/8/8/8/4K3 b - - 65535 4294967295 -",
         {},
         {}},
    };

    for (const SelectedPosition& selected : positions) {
        zfs::Position production = load_production(selected.fen);
        const OraclePosition oracle = load_oracle(selected.fen);
        require(production.to_fen() == oracle.to_fen(),
                std::string(selected.label) + ": parsers disagree");
        const auto moves = compare_moves(production, oracle, selected.label);
        for (std::string_view required : selected.required) {
            require(std::ranges::binary_search(moves, std::string(required)),
                    std::string(selected.label) + ": missing required move " +
                        std::string(required));
        }
        for (std::string_view forbidden : selected.forbidden) {
            require(!std::ranges::binary_search(moves, std::string(forbidden)),
                    std::string(selected.label) + ": emitted forbidden move " +
                        std::string(forbidden));
        }
        if (selected.required_are_complete) {
            std::vector<std::string> exact;
            exact.reserve(selected.required.size());
            for (std::string_view move : selected.required) {
                exact.emplace_back(move);
            }
            std::ranges::sort(exact);
            require(moves == exact,
                    std::string(selected.label) +
                        ": legal moves were not exactly the required set");
        }
        for (const std::string& move : moves) {
            compare_transition(production, oracle, move, selected.label);
        }
    }

    const OraclePosition castle = load_oracle(
        "4k3/8/8/8/8/8/8/4K2R w K - 0 1 -");
    const auto castled = castle.after_uci("e1g1");
    require(castled.has_value(), "oracle castling setup lacks e1g1");
    require(castled->to_fen().ends_with(" -"),
            "castling must canonicalize its inert follow field to absent");
}

[[nodiscard]] std::uint64_t next_random(std::uint64_t& state) noexcept {
    state ^= state << 13U;
    state ^= state >> 7U;
    state ^= state << 17U;
    return state;
}

void test_deterministic_playouts() {
    constexpr std::string_view kStart =
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1 -";
    std::uint64_t random = 0x6a09e667f3bcc909ULL;
    constexpr int kGames = 128;
    constexpr int kMaximumPlies = 180;

    for (int game = 0; game < kGames; ++game) {
        zfs::Position production = load_production(kStart);
        OraclePosition oracle = load_oracle(kStart);
        for (int ply = 0; ply < kMaximumPlies; ++ply) {
            const std::string context = "random game " + std::to_string(game) +
                                        ", ply " + std::to_string(ply);
            const auto moves = compare_moves(production, oracle, context);
            if (moves.empty()) {
                break;
            }
            const std::string& uci = moves[next_random(random) % moves.size()];
            const std::string before = production.to_fen();
            const auto production_move = production.parse_uci(uci);
            const auto oracle_next = oracle.after_uci(uci);
            require(production_move.has_value() && oracle_next.has_value(),
                    context + ": selected move vanished: " + uci);

            zfs::Undo undo;
            production.do_move(*production_move, undo);
            require(production.validate().empty(),
                    context + ": production invariant failed after " + uci);
            production.undo_move(*production_move, undo);
            require(production.to_fen() == before,
                    context + ": make/undo failed for " + uci);
            production.do_move(*production_move, undo);

            oracle = *oracle_next;
            require(production.to_fen() == oracle.to_fen(),
                    context + ": state mismatch after " + uci +
                        "\n  production: " + production.to_fen() +
                        "\n  oracle:     " + oracle.to_fen());
        }
    }
}

}  // namespace

int main() {
    try {
        test_selected_positions();
        test_deterministic_playouts();
    } catch (const std::exception& error) {
        std::cerr << "differential test failure: " << error.what() << '\n';
        return 1;
    }
    std::cout << "all oracle differential tests passed\n";
}
