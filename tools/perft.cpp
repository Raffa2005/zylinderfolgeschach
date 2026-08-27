#include "zfs/position.hpp"

#include <charconv>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>

namespace {

std::uint64_t perft(zfs::Position& position, unsigned depth) {
    zfs::MoveList moves;
    position.generate_legal_moves(moves);
    if (depth == 1) {
        return moves.size();
    }

    std::uint64_t nodes = 0;
    for (zfs::Move move : moves) {
        zfs::Undo undo;
        position.do_move(move, undo);
        nodes += perft(position, depth - 1U);
        position.undo_move(move, undo);
    }
    return nodes;
}

bool parse_depth(std::string_view text, unsigned& depth) {
    const char* const begin = text.data();
    const char* const end = begin + text.size();
    const auto result = std::from_chars(begin, end, depth);
    return result.ec == std::errc{} && result.ptr == end && depth > 0;
}

}  // namespace

int main(int argc, char** argv) {
    unsigned depth = 4;
    if (argc > 1 && !parse_depth(argv[1], depth)) {
        std::cerr << "invalid depth: " << argv[1] << '\n';
        return 2;
    }

    zfs::Position position = zfs::Position::start();
    if (argc > 2) {
        std::string fen = argv[2];
        for (int index = 3; index < argc; ++index) {
            fen.push_back(' ');
            fen += argv[index];
        }
        std::string error;
        auto parsed = zfs::Position::from_fen(fen, &error);
        if (!parsed) {
            std::cerr << "invalid ZFS-FEN: " << error << '\n';
            return 2;
        }
        position = *parsed;
    }

    const auto start = std::chrono::steady_clock::now();
    const std::uint64_t nodes = perft(position, depth);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    const double seconds = std::chrono::duration<double>(elapsed).count();

    std::cout << "depth " << depth << ": " << nodes << " nodes";
    if (seconds > 0.0) {
        std::cout << " (" << static_cast<std::uint64_t>(
                                  static_cast<double>(nodes) / seconds)
                  << " nodes/s)";
    }
    std::cout << '\n';
}
