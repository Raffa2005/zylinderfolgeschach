#include "bridge.hpp"

#include <iostream>
#include <string>
#include <string_view>

namespace {

int failures = 0;

#define CHECK(expression)                                                        \
    do {                                                                         \
        if (!(expression)) {                                                     \
            std::cerr << __FILE__ << ':' << __LINE__ << ": CHECK failed: "      \
                      << #expression << '\n';                                    \
            ++failures;                                                          \
        }                                                                        \
    } while (false)

[[nodiscard]] std::string state() { return zfs_state_json(); }

[[nodiscard]] bool contains(std::string_view text,
                            std::string_view expected) noexcept {
    return text.find(expected) != std::string_view::npos;
}

void test_initial_and_forced_follow() {
    zfs_reset();
    const std::string initial = state();
    CHECK(contains(initial, "\"turn\":\"white\""));
    CHECK(contains(initial, "\"follow\":\"-\""));
    CHECK(contains(initial, "\"historyCursor\":0"));
    CHECK(contains(initial, "\"legalMoves\":[\"a2a3\",\"a2a4\""));

    CHECK(zfs_load("7k/8/8/8/8/8/8/R3K3 w - - 0 1 a3") == 1);
    const std::string forced = state();
    CHECK(contains(forced, "\"follow\":\"a3\""));
    CHECK(contains(forced, "\"followForced\":true"));
    CHECK(contains(forced, "\"legalMoves\":[\"a1a3\"]"));

    CHECK(zfs_play("a1a3") == 1);
    CHECK(contains(state(), "\"historyCursor\":1"));
    CHECK(contains(state(), "\"history\":[\"a1a3\"]"));
    CHECK(contains(state(), "\"sanHistory\":[\"Ra3\"]"));
    CHECK(zfs_back() == 1);
    CHECK(contains(state(), "\"historyCursor\":0"));
    CHECK(contains(state(), "\"history\":[\"a1a3\"]"));
    CHECK(zfs_forward() == 1);
    CHECK(contains(state(), "\"historyCursor\":1"));
}

void test_navigation_and_branching() {
    zfs_reset();
    const std::string before = state();
    CHECK(zfs_load("not a position") == 0);
    CHECK(std::string_view(zfs_last_error()).size() != 0);
    CHECK(state() == before);
    CHECK(zfs_play("e2e5") == 0);
    CHECK(contains(zfs_last_error(), "not legal"));
    CHECK(state() == before);
    CHECK(zfs_back() == 0);
    CHECK(state() == before);

    CHECK(zfs_play("e2e4") == 1);
    CHECK(zfs_play("e7e5") == 1);
    const std::string end = state();
    CHECK(contains(end, "\"historyCursor\":2"));
    CHECK(contains(end, "\"history\":[\"e2e4\",\"e7e5\"]"));
    CHECK(contains(end, "\"sanHistory\":[\"e4\",\"e5\"]"));
    CHECK(std::string_view(zfs_line_san("g1f3 b8c6")) == "Nf3 Nc6");
    CHECK(zfs_forward() == 0);
    CHECK(state() == end);

    CHECK(zfs_back() == 1);
    const std::string after_e4 = state();
    CHECK(contains(after_e4,
                   "\"fen\":\"rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/"
                   "RNBQKBNR b KQkq e3 0 1 e2\""));
    CHECK(contains(after_e4, "\"historyCursor\":1"));
    CHECK(contains(after_e4, "\"history\":[\"e2e4\",\"e7e5\"]"));

    CHECK(zfs_forward() == 1);
    CHECK(state() == end);
    CHECK(zfs_back() == 1);
    CHECK(zfs_play("c7c5") == 1);
    const std::string branch = state();
    CHECK(contains(branch, "\"historyCursor\":2"));
    CHECK(contains(branch, "\"history\":[\"e2e4\",\"c7c5\"]"));
    CHECK(!contains(branch, "e7e5"));
    CHECK(contains(branch, "\"sanHistory\":[\"e4\",\"c5\"]"));
    CHECK(zfs_forward() == 0);
}

void test_special_moves_and_castle_canonicalization() {
    CHECK(zfs_load("4k3/8/8/8/p6P/8/8/4K3 b - h3 0 1 h2") == 1);
    CHECK(zfs_play("a4h3") == 1);
    CHECK(contains(state(), "\"sanHistory\":[\"axh3\"]"));
    CHECK(contains(state(),
                   "\"fen\":\"4k3/8/8/8/8/7p/8/4K3 w - - 0 2 a4\""));

    CHECK(zfs_load("r1k5/7P/8/8/8/8/8/4K3 w - - 0 1 -") == 1);
    CHECK(zfs_play("h7a8q") == 1);
    CHECK(contains(state(), "\"sanHistory\":[\"hxa8=Q+\"]"));
    CHECK(contains(state(),
                   "\"fen\":\"Q1k5/8/8/8/8/8/8/4K3 b - - 0 1 h7\""));

    CHECK(zfs_load("4k3/8/8/8/8/8/8/4K2R w K - 0 1 -") == 1);
    CHECK(zfs_play("e1g1") == 1);
    CHECK(contains(state(), "\"sanHistory\":[\"O-O\"]"));
    CHECK(contains(state(),
                   "\"fen\":\"4k3/8/8/8/8/8/8/5RK1 b - - 1 1 -\""));
}

void test_san_disambiguation() {
    CHECK(zfs_load("4k3/8/8/8/8/8/4K3/R1R5 w - - 0 1 -") == 1);
    CHECK(zfs_play("a1b1") == 1);
    CHECK(contains(state(), "\"sanHistory\":[\"Rab1\"]"));
}

void test_automatic_draws() {
    CHECK(zfs_load("4k3/8/8/8/8/8/8/4K1N1 w - - 0 1 -") == 1);
    constexpr std::string_view cycle[]{"g1f3", "e8e7", "f3g1", "e7e8"};
    for (int repeat = 0; repeat < 2; ++repeat) {
        for (std::string_view move : cycle) {
            CHECK(zfs_play(std::string(move).c_str()) == 1);
        }
    }
    const std::string repeated = state();
    CHECK(contains(repeated, "\"terminal\":\"threefold\""));
    CHECK(contains(repeated, "\"legalMoves\":[]"));
    CHECK(zfs_play("g1f3") == 0);
    CHECK(contains(zfs_last_error(), "already ended"));
    CHECK(zfs_back() == 1);
    CHECK(contains(state(), "\"terminal\":\"ongoing\""));

    CHECK(zfs_load("4k3/8/8/8/8/8/8/4K1N1 w - - 99 1 -") == 1);
    CHECK(zfs_play("g1f3") == 1);
    CHECK(contains(state(), "\"terminal\":\"fifty-move\""));
    CHECK(contains(state(), "\"legalMoves\":[]"));
}

}  // namespace

int main() {
    test_initial_and_forced_follow();
    test_navigation_and_branching();
    test_special_moves_and_castle_canonicalization();
    test_san_disambiguation();
    test_automatic_draws();
    if (failures != 0) {
        std::cerr << failures << " viewer bridge test(s) failed\n";
        return 1;
    }
    std::cout << "viewer bridge tests passed\n";
    return 0;
}
