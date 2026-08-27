#include "tools/eval/stats.hpp"
#include "tools/eval/nonblocking_io.hpp"
#include "zfs/game.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <poll.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct FileIdentity {
    std::uintmax_t device = 0;
    std::uintmax_t inode = 0;
    std::uintmax_t size = 0;
    std::intmax_t modification_seconds = 0;
    std::intmax_t modification_nanoseconds = 0;
    std::intmax_t change_seconds = 0;
    std::intmax_t change_nanoseconds = 0;

    auto operator<=>(const FileIdentity&) const = default;
};

struct Config {
    std::string candidate;
    std::string baseline;
    std::string candidate_id;
    std::string baseline_id;
    std::string openings;
    std::string output;
    std::uint64_t candidate_hash = 0;
    std::uint64_t baseline_hash = 0;
    FileIdentity candidate_identity;
    FileIdentity baseline_identity;
    std::size_t pairs = 0;
    std::uint64_t nodes = 25'000;
    std::uint64_t movetime_ms = 0;
    std::uint64_t timeout_ms = 60'000;
    unsigned hash_mb = 16;
    unsigned max_plies = 512;
    double elo0 = 0.0;
    double elo1 = 5.0;
    double alpha = 0.05;
    double beta = 0.05;
    bool resume = false;
};

struct Opening {
    std::size_t line_number = 0;
    std::vector<std::string> moves;
};

enum class Result { WhiteWin, Draw, BlackWin };

struct ThinkResult {
    std::string bestmove;
    std::uint64_t nodes = 0;
    std::uint64_t time_ms = 0;
    int depth = 0;
};

struct GameRecord {
    Result result = Result::Draw;
    std::string termination;
    zfs::Color candidate_color = zfs::Color::White;
    std::vector<std::string> moves;
    std::uint64_t candidate_nodes = 0;
    std::uint64_t baseline_nodes = 0;
    std::uint64_t candidate_time_ms = 0;
    std::uint64_t baseline_time_ms = 0;
    int candidate_depth = 0;
    int baseline_depth = 0;
};

class Fnv1a {
public:
    void add(std::string_view text) noexcept {
        for (const unsigned char byte : text) {
            value_ ^= byte;
            value_ *= 1099511628211ULL;
        }
    }

    void add_field(std::string_view name, std::string_view value) {
        constexpr std::string_view separator("\0", 1);
        add(name);
        add(separator);
        add(std::to_string(value.size()));
        add(":");
        add(value);
        add(separator);
    }

    template <typename Number>
    void add_number_field(std::string_view name, Number value) {
        add_field(name, std::to_string(value));
    }

    void add_number_field(std::string_view name, double value) {
        std::ostringstream text;
        text << std::setprecision(std::numeric_limits<double>::max_digits10) << value;
        add_field(name, text.str());
    }

    [[nodiscard]] std::uint64_t value() const noexcept { return value_; }

private:
    std::uint64_t value_ = 14695981039346656037ULL;
};

class OutputFile {
public:
    OutputFile(const std::string& path, bool resume) {
        const int flags = O_RDWR | O_APPEND | O_CLOEXEC |
                          (resume ? 0 : O_CREAT | O_EXCL);
        descriptor_ = open(path.c_str(), flags, 0644);
        if (descriptor_ < 0) {
            if (!resume && errno == EEXIST) {
                throw std::runtime_error("output exists; use --resume or a new file");
            }
            throw std::runtime_error(std::string(resume ? "cannot resume output: "
                                                        : "cannot create output: ") +
                                     path + ": " + std::strerror(errno));
        }
        if (flock(descriptor_, LOCK_EX | LOCK_NB) != 0) {
            const int saved_errno = errno;
            close(descriptor_);
            descriptor_ = -1;
            if (!resume) {
                unlink(path.c_str());
            }
            throw std::runtime_error("output is locked by another match: " +
                                     std::string(std::strerror(saved_errno)));
        }
    }

    ~OutputFile() {
        if (descriptor_ >= 0) {
            close(descriptor_);
        }
    }

    OutputFile(const OutputFile&) = delete;
    OutputFile& operator=(const OutputFile&) = delete;

    void append(std::string_view text) {
        std::size_t written = 0;
        while (written < text.size()) {
            const ssize_t count = write(descriptor_, text.data() + written,
                                        text.size() - written);
            if (count > 0) {
                written += static_cast<std::size_t>(count);
            } else if (count < 0 && errno == EINTR) {
                continue;
            } else {
                throw std::runtime_error("failed while writing result log");
            }
        }
        if (fsync(descriptor_) != 0) {
            throw std::runtime_error("failed while syncing result log");
        }
    }

    void truncate(std::uintmax_t size) {
        if (size > static_cast<std::uintmax_t>(
                       std::numeric_limits<off_t>::max()) ||
            ftruncate(descriptor_, static_cast<off_t>(size)) != 0) {
            throw std::runtime_error("cannot discard incomplete log record");
        }
        if (fsync(descriptor_) != 0) {
            throw std::runtime_error("failed while syncing repaired result log");
        }
    }

private:
    int descriptor_ = -1;
};

void ensure_standard_descriptors() {
    constexpr std::array flags{O_RDONLY, O_WRONLY, O_WRONLY};
    for (int descriptor = STDIN_FILENO; descriptor <= STDERR_FILENO;
         ++descriptor) {
        errno = 0;
        if (fcntl(descriptor, F_GETFD) >= 0 || errno != EBADF) {
            continue;
        }
        const int replacement = open("/dev/null", flags[static_cast<std::size_t>(descriptor)]);
        if (replacement < 0) {
            throw std::runtime_error("cannot open /dev/null");
        }
        if (replacement != descriptor) {
            if (dup2(replacement, descriptor) < 0) {
                close(replacement);
                throw std::runtime_error("cannot restore standard descriptor");
            }
            close(replacement);
        }
    }
}

[[nodiscard]] std::string hex64(std::uint64_t value) {
    std::ostringstream output;
    output << std::hex << std::setw(16) << std::setfill('0') << value;
    return output.str();
}

[[nodiscard]] std::string checksum(std::string_view text) {
    Fnv1a hash;
    hash.add(text);
    return hex64(hash.value());
}

[[nodiscard]] std::string json_escape(std::string_view text) {
    std::string result;
    result.reserve(text.size() + 8U);
    for (const unsigned char character : text) {
        switch (character) {
            case '"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\b': result += "\\b"; break;
            case '\f': result += "\\f"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:
                if (character < 0x20U) {
                    constexpr char digits[] = "0123456789abcdef";
                    result += "\\u00";
                    result.push_back(digits[character >> 4U]);
                    result.push_back(digits[character & 0xfU]);
                } else {
                    result.push_back(static_cast<char>(character));
                }
        }
    }
    return result;
}

[[nodiscard]] std::string quote(std::string_view text) {
    return "\"" + json_escape(text) + "\"";
}

[[nodiscard]] std::string utc_now() {
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
    if (gmtime_r(&now, &utc) == nullptr) {
        return "unknown";
    }
    std::array<char, 32> buffer{};
    if (std::strftime(buffer.data(), buffer.size(), "%Y-%m-%dT%H:%M:%SZ", &utc) ==
        0U) {
        return "unknown";
    }
    return buffer.data();
}

template <typename Number>
[[nodiscard]] Number parse_number(std::string_view text,
                                  std::string_view option) {
    Number value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || parsed.ec != std::errc{} ||
        parsed.ptr != text.data() + text.size()) {
        throw std::invalid_argument("invalid value for " + std::string(option));
    }
    return value;
}

[[noreturn]] void usage(const char* program, std::string_view error = {}) {
    if (!error.empty()) {
        std::cerr << "error: " << error << "\n\n";
    }
    std::cerr
        << "usage: " << program
        << " --candidate FILE --baseline FILE --openings FILE --output FILE\n"
           "       [--pairs N] [--nodes N | --movetime-ms N] [--hash-mb N]\n"
           "       [--max-plies N] [--timeout-ms N] [--candidate-id TEXT]\n"
           "       [--baseline-id TEXT] [--elo0 E] [--elo1 E]\n"
           "       [--alpha P] [--beta P] [--resume]\n";
    std::exit(error.empty() ? 0 : 2);
}

[[nodiscard]] Config parse_config(int argc, char** argv) {
    Config config;
    bool nodes_set = false;
    bool movetime_set = false;
    for (int index = 1; index < argc; ++index) {
        const std::string_view option(argv[index]);
        if (option == "--help" || option == "-h") {
            usage(argv[0]);
        }
        if (option == "--resume") {
            config.resume = true;
            continue;
        }
        if (index + 1 >= argc) {
            usage(argv[0], "missing value for " + std::string(option));
        }
        const std::string_view value(argv[++index]);
        if (option == "--candidate") {
            config.candidate = value;
        } else if (option == "--baseline") {
            config.baseline = value;
        } else if (option == "--candidate-id") {
            config.candidate_id = value;
        } else if (option == "--baseline-id") {
            config.baseline_id = value;
        } else if (option == "--openings") {
            config.openings = value;
        } else if (option == "--output") {
            config.output = value;
        } else if (option == "--pairs") {
            config.pairs = parse_number<std::size_t>(value, option);
        } else if (option == "--nodes") {
            config.nodes = parse_number<std::uint64_t>(value, option);
            nodes_set = true;
        } else if (option == "--movetime-ms") {
            config.movetime_ms = parse_number<std::uint64_t>(value, option);
            movetime_set = true;
        } else if (option == "--timeout-ms") {
            config.timeout_ms = parse_number<std::uint64_t>(value, option);
        } else if (option == "--hash-mb") {
            config.hash_mb = parse_number<unsigned>(value, option);
        } else if (option == "--max-plies") {
            config.max_plies = parse_number<unsigned>(value, option);
        } else if (option == "--elo0") {
            config.elo0 = parse_number<double>(value, option);
        } else if (option == "--elo1") {
            config.elo1 = parse_number<double>(value, option);
        } else if (option == "--alpha") {
            config.alpha = parse_number<double>(value, option);
        } else if (option == "--beta") {
            config.beta = parse_number<double>(value, option);
        } else {
            usage(argv[0], "unknown option " + std::string(option));
        }
    }

    if (config.candidate.empty() || config.baseline.empty() ||
        config.openings.empty() || config.output.empty()) {
        usage(argv[0], "candidate, baseline, openings, and output are required");
    }
    if (nodes_set && movetime_set) {
        usage(argv[0], "--nodes and --movetime-ms are mutually exclusive");
    }
    if (movetime_set) {
        config.nodes = 0;
    }
    if ((config.nodes == 0U) == (config.movetime_ms == 0U)) {
        usage(argv[0], "the search limit must be positive");
    }
    if (config.timeout_ms == 0U || config.max_plies == 0U ||
        config.hash_mb == 0U || config.hash_mb > 1024U) {
        usage(argv[0], "timeout, max plies, and hash size must be positive");
    }
    constexpr std::uint64_t kOneDayMs = 24U * 60U * 60U * 1000U;
    if (config.timeout_ms > kOneDayMs || config.movetime_ms > kOneDayMs) {
        usage(argv[0], "movetime and timeout are limited to one day");
    }
    if (config.max_plies > 10'000U) {
        usage(argv[0], "max plies is limited to 10000");
    }
    if (config.movetime_ms != 0U &&
        config.timeout_ms <= config.movetime_ms + 1000U) {
        usage(argv[0], "timeout must exceed movetime by more than one second");
    }
    return config;
}

[[nodiscard]] std::string canonical_file(const std::string& path) {
    std::error_code error;
    const auto canonical = std::filesystem::canonical(path, error);
    if (error || !std::filesystem::is_regular_file(canonical)) {
        throw std::runtime_error("not a regular file: " + path);
    }
    return canonical.string();
}

[[nodiscard]] std::string canonical_executable(std::string_view invocation) {
    if (invocation.find('/') != std::string_view::npos) {
        return canonical_file(std::string(invocation));
    }
    const char* environment_path = std::getenv("PATH");
    if (environment_path != nullptr) {
        std::string_view paths(environment_path);
        std::size_t begin = 0;
        for (;;) {
            const std::size_t separator = paths.find(':', begin);
            const std::size_t length = separator == std::string_view::npos
                ? paths.size() - begin
                : separator - begin;
            const std::string_view directory = paths.substr(begin, length);
            const std::filesystem::path candidate =
                (directory.empty() ? std::filesystem::path(".")
                                   : std::filesystem::path(directory)) /
                invocation;
            std::error_code error;
            const auto canonical = std::filesystem::canonical(candidate, error);
            if (!error && std::filesystem::is_regular_file(canonical) &&
                access(canonical.c_str(), X_OK) == 0) {
                return canonical.string();
            }
            if (separator == std::string_view::npos) {
                break;
            }
            begin = separator + 1U;
        }
    }
    throw std::runtime_error("cannot locate match-runner executable");
}

class IntegrityError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class InfrastructureError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class ConfigurationError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

[[nodiscard]] FileIdentity file_identity(const std::string& path) {
    struct stat status {};
    if (stat(path.c_str(), &status) != 0 || status.st_size < 0) {
        throw IntegrityError("cannot inspect engine binary: " + path);
    }
#if defined(__APPLE__)
    const timespec modification = status.st_mtimespec;
    const timespec change = status.st_ctimespec;
#else
    const timespec modification = status.st_mtim;
    const timespec change = status.st_ctim;
#endif
    return {
        static_cast<std::uintmax_t>(status.st_dev),
        static_cast<std::uintmax_t>(status.st_ino),
        static_cast<std::uintmax_t>(status.st_size),
        static_cast<std::intmax_t>(modification.tv_sec),
        static_cast<std::intmax_t>(modification.tv_nsec),
        static_cast<std::intmax_t>(change.tv_sec),
        static_cast<std::intmax_t>(change.tv_nsec),
    };
}

[[nodiscard]] std::uint64_t hash_file(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw IntegrityError("cannot read engine binary: " + path);
    }
    Fnv1a hash;
    std::array<char, 64 * 1024> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        hash.add(std::string_view(buffer.data(),
                                  static_cast<std::size_t>(input.gcount())));
    }
    if (!input.eof()) {
        throw IntegrityError("failed while hashing engine binary: " + path);
    }
    return hash.value();
}

struct EngineImage {
    std::uint64_t hash = 0;
    FileIdentity identity;
};

[[nodiscard]] EngineImage fingerprint_engine(const std::string& path) {
    const FileIdentity before = file_identity(path);
    const std::uint64_t hash = hash_file(path);
    const FileIdentity after = file_identity(path);
    if (before != after) {
        throw IntegrityError("engine binary changed while fingerprinting: " + path);
    }
    return {hash, after};
}

void verify_engine(const std::string& path, const FileIdentity& expected) {
    if (file_identity(path) != expected) {
        throw IntegrityError("engine binary changed during match: " + path);
    }
}

[[nodiscard]] std::vector<std::string> words(std::string_view line) {
    std::istringstream input{std::string(line)};
    std::vector<std::string> result;
    std::string word;
    while (input >> word) {
        result.push_back(std::move(word));
    }
    return result;
}

struct OpeningSuite {
    std::vector<Opening> openings;
    std::uint64_t hash = 0;
};

[[nodiscard]] OpeningSuite read_openings(const std::string& path,
                                         unsigned max_plies) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("cannot read openings: " + path);
    }
    std::ostringstream bytes;
    bytes << file.rdbuf();
    if (file.bad()) {
        throw std::runtime_error("failed while reading openings: " + path);
    }
    const std::string contents = bytes.str();
    Fnv1a hash;
    hash.add(contents);
    OpeningSuite suite;
    suite.hash = hash.value();

    std::istringstream input(contents);
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        const auto comment = line.find('#');
        if (comment != std::string::npos) {
            line.erase(comment);
        }
        auto moves = words(line);
        if (moves.empty()) {
            continue;
        }
        if (moves.size() == 1U && moves.front() == "startpos") {
            moves.clear();
        }
        if (moves.size() >= max_plies) {
            throw std::runtime_error("opening on line " +
                                     std::to_string(line_number) +
                                     " reaches the game ply cap");
        }
        zfs::Game game;
        for (const std::string& move : moves) {
            if (!game.play_uci(move)) {
                throw std::runtime_error("illegal opening move " + move +
                                         " on line " +
                                         std::to_string(line_number));
            }
        }
        if (game.state() != zfs::GameState::Ongoing) {
            throw std::runtime_error("terminal opening on line " +
                                     std::to_string(line_number));
        }
        Opening opening;
        opening.line_number = line_number;
        opening.moves = std::move(moves);
        suite.openings.push_back(std::move(opening));
    }
    if (!input.eof()) {
        throw std::runtime_error("failed while reading openings: " + path);
    }
    if (suite.openings.empty()) {
        throw std::runtime_error("opening file contains no positions");
    }
    return suite;
}

class UciEngine {
public:
    UciEngine(std::string path, FileIdentity expected_identity, unsigned hash_mb,
              std::uint64_t timeout_ms)
        : path_(std::move(path)), timeout_ms_(timeout_ms),
          expected_identity_(expected_identity) {
        verify_image();
        launch();
        try {
            auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms_);
            send("uci", deadline);
            wait_for("uciok", deadline);
            deadline = Clock::now() + std::chrono::milliseconds(timeout_ms_);
            if (hash_option_) {
                if (hash_mb < hash_option_->minimum ||
                    hash_mb > hash_option_->maximum) {
                    throw ConfigurationError(
                        path_ + ": requested Hash is outside advertised range");
                }
                send("setoption name " + hash_option_->name + " value " +
                         std::to_string(hash_mb),
                     deadline);
            }
            send("isready", deadline);
            wait_for("readyok", deadline);
            verify_image();
        } catch (...) {
            kill_process();
            throw;
        }
    }

    ~UciEngine() { shutdown(); }

    UciEngine(const UciEngine&) = delete;
    UciEngine& operator=(const UciEngine&) = delete;

    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] bool supports_hash() const noexcept {
        return hash_option_.has_value();
    }

    void new_game() {
        const auto deadline =
            Clock::now() + std::chrono::milliseconds(timeout_ms_);
        send("ucinewgame", deadline);
        send("isready", deadline);
        wait_for("readyok", deadline);
    }

    [[nodiscard]] ThinkResult think(const std::vector<std::string>& moves,
                                    const Config& config) {
        const auto deadline = Clock::now() +
            std::chrono::milliseconds(config.timeout_ms);
        std::string position = "position startpos";
        if (!moves.empty()) {
            position += " moves";
            for (const std::string& move : moves) {
                position.push_back(' ');
                position += move;
            }
        }
        send(position, deadline);
        if (config.nodes != 0U) {
            send("go nodes " + std::to_string(config.nodes), deadline);
        } else {
            send("go movetime " + std::to_string(config.movetime_ms), deadline);
        }

        ThinkResult result;
        for (;;) {
            const std::string line = read_line(deadline);
            const auto tokens = words(line);
            if (tokens.empty()) {
                continue;
            }
            if (tokens[0] == "bestmove") {
                if (tokens.size() < 2U) {
                    fail("malformed bestmove line");
                }
                result.bestmove = tokens[1];
                return result;
            }
            if (tokens[0] != "info") {
                continue;
            }
            for (std::size_t i = 1; i + 1U < tokens.size(); ++i) {
                if (tokens[i] == "nodes") {
                    parse_info(tokens[++i], result.nodes);
                } else if (tokens[i] == "time") {
                    parse_info(tokens[++i], result.time_ms);
                } else if (tokens[i] == "depth") {
                    parse_info(tokens[++i], result.depth);
                }
            }
        }
    }

private:
    void verify_image() const {
        verify_engine(path_, expected_identity_);
    }

    template <typename Number>
    static void parse_info(std::string_view text, Number& destination) noexcept {
        Number value{};
        const auto parsed =
            std::from_chars(text.data(), text.data() + text.size(), value);
        if (parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size()) {
            destination = value;
        }
    }

    [[noreturn]] void fail(std::string_view message) {
        kill_process();
        throw std::runtime_error(path_ + ": " + std::string(message));
    }

    void launch() {
        int to_child[2]{};
        int from_child[2]{};
        if (pipe(to_child) != 0) {
            throw InfrastructureError("pipe failed: " +
                                      std::string(std::strerror(errno)));
        }
        if (pipe(from_child) != 0) {
            close(to_child[0]);
            close(to_child[1]);
            throw InfrastructureError("pipe failed: " +
                                      std::string(std::strerror(errno)));
        }
        for (const int descriptor : {to_child[0], to_child[1], from_child[0],
                                     from_child[1]}) {
            if (fcntl(descriptor, F_SETFD, FD_CLOEXEC) != 0) {
                close(to_child[0]);
                close(to_child[1]);
                close(from_child[0]);
                close(from_child[1]);
                throw InfrastructureError("fcntl failed: " +
                                          std::string(std::strerror(errno)));
            }
        }
        pid_ = fork();
        if (pid_ < 0) {
            close(to_child[0]);
            close(to_child[1]);
            close(from_child[0]);
            close(from_child[1]);
            throw InfrastructureError("fork failed: " +
                                      std::string(std::strerror(errno)));
        }
        if (pid_ == 0) {
            if (setpgid(0, 0) != 0) {
                _exit(126);
            }
            if (dup2(to_child[0], STDIN_FILENO) < 0 ||
                dup2(from_child[1], STDOUT_FILENO) < 0) {
                _exit(126);
            }
            close(to_child[0]);
            close(to_child[1]);
            close(from_child[0]);
            close(from_child[1]);
            execl(path_.c_str(), path_.c_str(), static_cast<char*>(nullptr));
            _exit(127);
        }
        close(to_child[0]);
        close(from_child[1]);
        input_fd_ = to_child[1];
        output_fd_ = from_child[0];
        if (setpgid(pid_, pid_) == 0 ||
            (errno == EACCES && getpgid(pid_) == pid_)) {
            process_group_ = true;
        } else {
            kill_process();
            throw InfrastructureError("could not isolate engine process group");
        }
        const int input_flags = fcntl(input_fd_, F_GETFL);
        if (input_flags < 0 ||
            fcntl(input_fd_, F_SETFL, input_flags | O_NONBLOCK) != 0) {
            const int saved_errno = errno;
            kill_process();
            throw InfrastructureError("cannot make engine input nonblocking: " +
                                      std::string(std::strerror(saved_errno)));
        }
    }

    void send(std::string_view line, Clock::time_point deadline) {
        std::string command(line);
        command.push_back('\n');
        const auto result =
            zfs::eval::posix::write_until(input_fd_, command, deadline);
        if (result.status == zfs::eval::posix::WriteStatus::Timeout) {
            fail("command timeout");
        }
        if (result.status == zfs::eval::posix::WriteStatus::Error) {
            fail("failed to write to engine");
        }
    }

    [[nodiscard]] std::string read_line(Clock::time_point deadline) {
        for (;;) {
            const auto newline = buffer_.find('\n');
            if (newline != std::string::npos) {
                std::string line = buffer_.substr(0, newline);
                buffer_.erase(0, newline + 1U);
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                return line;
            }

            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - Clock::now()).count();
            if (remaining <= 0) {
                fail("response timeout");
            }
            pollfd descriptor{output_fd_, POLLIN, 0};
            const int poll_timeout = static_cast<int>(std::min<std::int64_t>(
                remaining, std::numeric_limits<int>::max()));
            const int ready = poll(&descriptor, 1, poll_timeout);
            if (ready == 0) {
                fail("response timeout");
            }
            if (ready < 0) {
                if (errno == EINTR) {
                    continue;
                }
                fail("poll failed");
            }
            std::array<char, 4096> incoming{};
            const ssize_t count = read(output_fd_, incoming.data(), incoming.size());
            if (count > 0) {
                buffer_.append(incoming.data(), static_cast<std::size_t>(count));
                if (buffer_.size() > 1024U * 1024U) {
                    fail("engine output line exceeds one MiB");
                }
            } else if (count == 0) {
                fail("engine exited before replying");
            } else if (errno != EINTR) {
                fail("failed to read from engine");
            }
        }
    }

    void wait_for(std::string_view expected, Clock::time_point deadline) {
        for (;;) {
            const std::string line = read_line(deadline);
            if (line == expected) {
                return;
            }
            if (line.starts_with("id name ")) {
                name_ = line.substr(8);
            } else if (line.starts_with("option name ")) {
                observe_option(line);
            }
        }
    }

    void observe_option(std::string_view line) {
        const auto tokens = words(line);
        const auto type = std::find(tokens.begin(), tokens.end(), "type");
        if (tokens.size() < 5U || tokens[0] != "option" || tokens[1] != "name" ||
            type == tokens.end() || type + 1 == tokens.end()) {
            return;
        }
        std::string name;
        for (auto token = tokens.begin() + 2; token != type; ++token) {
            if (!name.empty()) {
                name.push_back(' ');
            }
            name += *token;
        }
        if (name != "Hash" || *(type + 1) != "spin") {
            return;
        }
        HashOption option{name, 0U, std::numeric_limits<unsigned>::max()};
        for (auto token = type + 2; token + 1 < tokens.end(); ++token) {
            unsigned value = 0;
            const auto parsed = std::from_chars((token + 1)->data(),
                                                (token + 1)->data() +
                                                    (token + 1)->size(),
                                                value);
            if (parsed.ec != std::errc{} ||
                parsed.ptr != (token + 1)->data() + (token + 1)->size()) {
                continue;
            }
            if (*token == "min") {
                option.minimum = value;
            } else if (*token == "max") {
                option.maximum = value;
            }
        }
        if (option.minimum <= option.maximum) {
            hash_option_ = std::move(option);
        }
    }

    void kill_process() noexcept {
        if (pid_ <= 0) {
            return;
        }
        const pid_t process = pid_;
        kill(process_group_ ? -process : process, SIGKILL);
        while (waitpid(pid_, nullptr, 0) < 0 && errno == EINTR) {
        }
        if (process_group_) {
            kill(-process, SIGKILL);
        }
        pid_ = -1;
        process_group_ = false;
        if (input_fd_ >= 0) {
            close(input_fd_);
            input_fd_ = -1;
        }
        if (output_fd_ >= 0) {
            close(output_fd_);
            output_fd_ = -1;
        }
    }

    void shutdown() noexcept {
        if (pid_ <= 0) {
            return;
        }
        const char quit[] = "quit\n";
        const ssize_t ignored = write(input_fd_, quit, sizeof(quit) - 1U);
        (void)ignored;
        for (unsigned attempt = 0; attempt < 20U; ++attempt) {
            const pid_t status = waitpid(pid_, nullptr, WNOHANG);
            if (status == pid_) {
                if (process_group_) {
                    kill(-pid_, SIGKILL);
                }
                pid_ = -1;
                process_group_ = false;
                break;
            }
            if (status < 0 && errno != EINTR) {
                if (process_group_) {
                    kill(-pid_, SIGKILL);
                }
                pid_ = -1;
                process_group_ = false;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (pid_ > 0) {
            kill_process();
        } else {
            if (input_fd_ >= 0) {
                close(input_fd_);
            }
            if (output_fd_ >= 0) {
                close(output_fd_);
            }
            input_fd_ = -1;
            output_fd_ = -1;
        }
    }

    std::string path_;
    std::string name_ = "unknown";
    std::string buffer_;
    struct HashOption {
        std::string name;
        unsigned minimum = 0;
        unsigned maximum = 0;
    };
    std::optional<HashOption> hash_option_;
    std::uint64_t timeout_ms_;
    FileIdentity expected_identity_;
    pid_t pid_ = -1;
    int input_fd_ = -1;
    int output_fd_ = -1;
    bool process_group_ = false;
};

[[nodiscard]] Result winner(zfs::Color color) noexcept {
    return color == zfs::Color::White ? Result::WhiteWin : Result::BlackWin;
}

[[nodiscard]] Result loss(zfs::Color color) noexcept {
    return winner(zfs::opposite(color));
}

[[nodiscard]] GameRecord run_game(const Config& config, const Opening& opening,
                                  zfs::Color candidate_color) {
    GameRecord record;
    record.candidate_color = candidate_color;
    record.moves = opening.moves;

    std::unique_ptr<UciEngine> candidate;
    std::unique_ptr<UciEngine> baseline;
    try {
        candidate = std::make_unique<UciEngine>(config.candidate,
                                                config.candidate_identity,
                                                config.hash_mb,
                                                config.timeout_ms);
        candidate->new_game();
    } catch (const IntegrityError&) {
        throw;
    } catch (const InfrastructureError&) {
        throw;
    } catch (const ConfigurationError&) {
        throw;
    } catch (const std::exception& error) {
        record.result = loss(candidate_color);
        record.termination = "candidate-start-failure: " + std::string(error.what());
        return record;
    }
    try {
        baseline = std::make_unique<UciEngine>(config.baseline,
                                               config.baseline_identity,
                                               config.hash_mb,
                                               config.timeout_ms);
        baseline->new_game();
    } catch (const IntegrityError&) {
        throw;
    } catch (const InfrastructureError&) {
        throw;
    } catch (const ConfigurationError&) {
        throw;
    } catch (const std::exception& error) {
        record.result = winner(candidate_color);
        record.termination = "baseline-start-failure: " + std::string(error.what());
        return record;
    }

    zfs::Game game;
    for (const std::string& move : opening.moves) {
        if (!game.play_uci(move)) {
            throw std::logic_error("validated opening became illegal");
        }
    }
    for (;;) {
        switch (game.state()) {
            case zfs::GameState::Checkmate:
                record.result = loss(game.position().side_to_move());
                record.termination = "checkmate";
                return record;
            case zfs::GameState::Stalemate:
                record.result = Result::Draw;
                record.termination = "stalemate";
                return record;
            case zfs::GameState::ThreefoldDraw:
                record.result = Result::Draw;
                record.termination = "threefold";
                return record;
            case zfs::GameState::FiftyMoveDraw:
                record.result = Result::Draw;
                record.termination = "fifty-move";
                return record;
            case zfs::GameState::Ongoing: break;
        }
        if (game.moves().size() >= config.max_plies) {
            record.result = Result::Draw;
            record.termination = "ply-cap";
            return record;
        }

        const zfs::Color side = game.position().side_to_move();
        const bool candidate_turn = side == candidate_color;
        UciEngine& engine = candidate_turn ? *candidate : *baseline;
        ThinkResult thought;
        try {
            thought = engine.think(record.moves, config);
        } catch (const std::exception& error) {
            record.result = loss(side);
            record.termination = std::string(candidate_turn ? "candidate" : "baseline") +
                                 "-failure: " + error.what();
            return record;
        }

        std::uint64_t& total_nodes = candidate_turn ? record.candidate_nodes
                                                     : record.baseline_nodes;
        std::uint64_t& total_time = candidate_turn ? record.candidate_time_ms
                                                    : record.baseline_time_ms;
        int& maximum_depth = candidate_turn ? record.candidate_depth
                                             : record.baseline_depth;
        total_nodes = thought.nodes >
                              std::numeric_limits<std::uint64_t>::max() - total_nodes
                          ? std::numeric_limits<std::uint64_t>::max()
                          : total_nodes + thought.nodes;
        total_time = thought.time_ms >
                             std::numeric_limits<std::uint64_t>::max() - total_time
                         ? std::numeric_limits<std::uint64_t>::max()
                         : total_time + thought.time_ms;
        maximum_depth = std::max(maximum_depth, thought.depth);

        if (thought.bestmove == "(none)" || thought.bestmove == "0000" ||
            !game.play_uci(thought.bestmove)) {
            record.result = loss(side);
            record.termination = std::string(candidate_turn ? "candidate" : "baseline") +
                                 "-illegal-move: " + thought.bestmove;
            return record;
        }
        record.moves.push_back(std::move(thought.bestmove));
    }
}

[[nodiscard]] std::string_view result_name(Result result) noexcept {
    switch (result) {
        case Result::WhiteWin: return "1-0";
        case Result::Draw: return "1/2-1/2";
        case Result::BlackWin: return "0-1";
    }
    return "invalid";
}

[[nodiscard]] unsigned candidate_half_points(const GameRecord& game) noexcept {
    if (game.result == Result::Draw) {
        return 1U;
    }
    const bool candidate_won =
        (game.result == Result::WhiteWin &&
         game.candidate_color == zfs::Color::White) ||
        (game.result == Result::BlackWin &&
         game.candidate_color == zfs::Color::Black);
    return candidate_won ? 2U : 0U;
}

void write_moves(std::ostream& output, const std::vector<std::string>& moves) {
    output << '[';
    for (std::size_t i = 0; i < moves.size(); ++i) {
        if (i != 0U) {
            output << ',';
        }
        output << quote(moves[i]);
    }
    output << ']';
}

void write_game(std::ostream& output, unsigned game_index,
                const GameRecord& game) {
    output << "{\"game\":" << game_index << ",\"candidate_color\":"
           << quote(game.candidate_color == zfs::Color::White ? "white" : "black")
           << ",\"result\":" << quote(result_name(game.result))
           << ",\"termination\":" << quote(game.termination)
           << ",\"plies\":" << game.moves.size()
           << ",\"candidate_nodes\":" << game.candidate_nodes
           << ",\"baseline_nodes\":" << game.baseline_nodes
           << ",\"candidate_time_ms\":" << game.candidate_time_ms
           << ",\"baseline_time_ms\":" << game.baseline_time_ms
           << ",\"candidate_max_depth\":" << game.candidate_depth
           << ",\"baseline_max_depth\":" << game.baseline_depth
           << ",\"moves\":";
    write_moves(output, game.moves);
    output << '}';
}

[[nodiscard]] std::optional<std::uint64_t> unsigned_field(
    std::string_view line, std::string_view field) {
    const std::string needle = "\"" + std::string(field) + "\":";
    const std::size_t begin = line.find(needle);
    if (begin == std::string_view::npos) {
        return std::nullopt;
    }
    const std::size_t number_begin = begin + needle.size();
    std::uint64_t value = 0;
    const auto parsed = std::from_chars(line.data() + number_begin,
                                        line.data() + line.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr == line.data() + line.size() ||
        (*parsed.ptr != ',' && *parsed.ptr != '}')) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] std::optional<std::string> string_field(
    std::string_view line, std::string_view field);

[[nodiscard]] std::optional<bool> valid_record_checksum(std::string_view line) {
    constexpr std::string_view needle = ",\"record_fnv1a64\":\"";
    const std::size_t begin = line.rfind(needle);
    if (begin == std::string_view::npos) {
        return std::nullopt;
    }
    const std::size_t value_begin = begin + needle.size();
    constexpr std::size_t digits = 16;
    if (line.size() != value_begin + digits + 2U ||
        line[value_begin + digits] != '"' || line.back() != '}') {
        return std::nullopt;
    }
    std::uint64_t recorded = 0;
    const auto parsed = std::from_chars(line.data() + value_begin,
                                        line.data() + value_begin + digits,
                                        recorded, 16);
    if (parsed.ec != std::errc{} ||
        parsed.ptr != line.data() + value_begin + digits) {
        return std::nullopt;
    }
    Fnv1a hash;
    hash.add(line.substr(0, begin));
    return hash.value() == recorded;
}

[[nodiscard]] std::optional<std::pair<std::string_view, std::size_t>>
json_object_at(std::string_view line, std::size_t begin) {
    if (begin >= line.size() || line[begin] != '{') {
        return std::nullopt;
    }
    unsigned depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (std::size_t cursor = begin; cursor < line.size(); ++cursor) {
        const char character = line[cursor];
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (character == '\\') {
                escaped = true;
            } else if (character == '"') {
                in_string = false;
            }
            continue;
        }
        if (character == '"') {
            in_string = true;
        } else if (character == '{') {
            ++depth;
        } else if (character == '}') {
            if (depth == 0U) {
                return std::nullopt;
            }
            --depth;
            if (depth == 0U) {
                return std::pair{line.substr(begin, cursor - begin + 1U),
                                 cursor + 1U};
            }
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<unsigned> game_points(std::string_view game,
                                                  unsigned expected_index) {
    const auto index = unsigned_field(game, "game");
    const auto color = string_field(game, "candidate_color");
    const auto result = string_field(game, "result");
    const std::string_view expected_color =
        expected_index == 1U ? "white" : "black";
    if (!index || *index != expected_index || !color || !result ||
        *color != expected_color) {
        return std::nullopt;
    }
    if (*result == "1/2-1/2") {
        return 1U;
    }
    if (*result != "1-0" && *result != "0-1") {
        return std::nullopt;
    }
    const bool candidate_won = (*result == "1-0" && *color == "white") ||
                               (*result == "0-1" && *color == "black");
    return candidate_won ? 2U : 0U;
}

[[nodiscard]] std::optional<std::array<unsigned, 2>> game_points_from_record(
    std::string_view line) {
    constexpr std::string_view needle = "\"games\":[";
    std::size_t cursor = line.find(needle);
    if (cursor == std::string_view::npos) {
        return std::nullopt;
    }
    cursor += needle.size();
    std::array<unsigned, 2> result{};
    for (unsigned game_index = 1; game_index <= 2U; ++game_index) {
        const auto object = json_object_at(line, cursor);
        if (!object) {
            return std::nullopt;
        }
        const auto points = game_points(object->first, game_index);
        if (!points) {
            return std::nullopt;
        }
        result[game_index - 1U] = *points;
        cursor = object->second;
        const char separator = game_index == 1U ? ',' : ']';
        if (cursor >= line.size() || line[cursor] != separator) {
            return std::nullopt;
        }
        ++cursor;
    }
    return result;
}

[[nodiscard]] std::optional<std::string> string_field(
    std::string_view line, std::string_view field) {
    const std::string needle = "\"" + std::string(field) + "\":\"";
    const std::size_t begin = line.find(needle);
    if (begin == std::string_view::npos) {
        return std::nullopt;
    }
    const std::size_t value_begin = begin + needle.size();
    const std::size_t end = line.find('"', value_begin);
    if (end == std::string_view::npos) {
        return std::nullopt;
    }
    return std::string(line.substr(value_begin, end - value_begin));
}

template <std::size_t Size>
[[nodiscard]] std::optional<std::array<std::uint64_t, Size>> array_field(
    std::string_view line, std::string_view field) {
    const std::string needle = "\"" + std::string(field) + "\":[";
    std::size_t cursor = line.find(needle);
    if (cursor == std::string_view::npos) {
        return std::nullopt;
    }
    cursor += needle.size();
    std::array<std::uint64_t, Size> result{};
    for (std::size_t i = 0; i < Size; ++i) {
        const auto parsed = std::from_chars(line.data() + cursor,
                                            line.data() + line.size(), result[i]);
        if (parsed.ec != std::errc{}) {
            return std::nullopt;
        }
        cursor = static_cast<std::size_t>(parsed.ptr - line.data());
        const char separator = i + 1U == Size ? ']' : ',';
        if (cursor >= line.size() || line[cursor] != separator) {
            return std::nullopt;
        }
        ++cursor;
    }
    return result;
}

struct ResumeState {
    std::size_t completed_pairs = 0;
    zfs::eval::Pentanomial results{};
    std::uintmax_t committed_bytes = 0;
    bool needs_line_separator = false;
};

[[nodiscard]] ResumeState read_resume(const std::string& path,
                                      std::string_view fingerprint) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("cannot resume missing log: " + path);
    }
    ResumeState state;
    std::string line;
    if (!std::getline(input, line) ||
        !line.starts_with("{\"type\":\"manifest\",\"schema\":2,") ||
        line.back() != '}' ||
        string_field(line, "config") != std::optional<std::string>(fingerprint)) {
        throw std::runtime_error("log configuration does not match this run");
    }
    const auto manifest_checksum = valid_record_checksum(line);
    if (!manifest_checksum) {
        throw std::runtime_error("malformed result-log manifest");
    }
    if (!*manifest_checksum) {
        throw std::runtime_error("result-log manifest checksum mismatch");
    }
    std::uintmax_t scanned_bytes = line.size() + (input.eof() ? 0U : 1U);
    state.committed_bytes = scanned_bytes;
    state.needs_line_separator = input.eof();
    while (std::getline(input, line)) {
        scanned_bytes += line.size() + (input.eof() ? 0U : 1U);
        if (!line.starts_with("{\"type\":\"pair\",")) {
            if (input.eof()) {
                // Only a final write interrupted before its record prefix is
                // recognizable may be discarded. Unknown complete records are
                // corruption, not comments or extension points.
                state.needs_line_separator = false;
                continue;
            }
            throw std::runtime_error("unknown record in result log");
        }
        if (line.empty() || line.back() != '}') {
            // A killed process may leave the final append incomplete. It is not
            // a result; the pair is replayed from its opening.
            if (input.eof()) {
                state.needs_line_separator = false;
                continue;
            }
            throw std::runtime_error("incomplete record inside result log");
        }
        const auto checksum_is_valid = valid_record_checksum(line);
        if (!checksum_is_valid) {
            if (input.eof()) {
                state.needs_line_separator = false;
                continue;
            }
            throw std::runtime_error("pair record is incomplete");
        }
        if (!*checksum_is_valid) {
            throw std::runtime_error("pair record checksum mismatch");
        }
        const auto pair = unsigned_field(line, "pair");
        const auto points = unsigned_field(line, "candidate_half_points");
        const auto games = array_field<2>(line, "game_half_points");
        const auto derived_games = game_points_from_record(line);
        const auto recorded = array_field<5>(line, "pentanomial");
        if (!pair || !points || !games || !derived_games || !recorded ||
            *pair != state.completed_pairs || *points > 4U ||
            (*games)[0] > 2U || (*games)[1] > 2U ||
            (*games)[0] + (*games)[1] != *points ||
            (*games)[0] != (*derived_games)[0] ||
            (*games)[1] != (*derived_games)[1]) {
            if (input.eof()) {
                state.needs_line_separator = false;
                continue;
            }
            throw std::runtime_error("malformed or non-contiguous pair record");
        }
        zfs::eval::Pentanomial expected = state.results;
        ++expected[static_cast<std::size_t>(*points)];
        if (expected != *recorded) {
            throw std::runtime_error("inconsistent cumulative pentanomial record");
        }
        state.results = expected;
        ++state.completed_pairs;
        state.committed_bytes = scanned_bytes;
        state.needs_line_separator = input.eof();
    }
    if (!input.eof()) {
        throw std::runtime_error("failed while reading result log");
    }
    return state;
}

void print_summary(const zfs::eval::Pentanomial& results, const Config& config) {
    const auto stats = zfs::eval::calculate_statistics(
        results, config.elo0, config.elo1, config.alpha, config.beta);
    std::cout << "pairs " << stats.pairs << " penta [" << results[0] << ','
              << results[1] << ',' << results[2] << ',' << results[3] << ','
              << results[4] << "] score " << std::fixed << std::setprecision(2)
              << 100.0 * stats.score << "% LLR " << std::setprecision(3)
              << stats.llr << " [" << stats.lower_bound << ','
              << stats.upper_bound << "] "
              << zfs::eval::decision_name(stats.decision) << '\n';
}

}  // namespace

int main(int argc, char** argv) {
    try {
        ensure_standard_descriptors();
        std::signal(SIGPIPE, SIG_IGN);
        Config config = parse_config(argc, argv);
        (void)zfs::eval::calculate_statistics(
            {}, config.elo0, config.elo1, config.alpha, config.beta);
        config.candidate = canonical_file(config.candidate);
        config.baseline = canonical_file(config.baseline);
        config.openings = canonical_file(config.openings);
        const std::string runner_path = canonical_executable(argv[0]);
        const EngineImage runner_image = fingerprint_engine(runner_path);
        const EngineImage candidate_image = fingerprint_engine(config.candidate);
        const EngineImage baseline_image = fingerprint_engine(config.baseline);
        config.candidate_hash = candidate_image.hash;
        config.baseline_hash = baseline_image.hash;
        config.candidate_identity = candidate_image.identity;
        config.baseline_identity = baseline_image.identity;
        const std::uint64_t candidate_hash = config.candidate_hash;
        const std::uint64_t baseline_hash = config.baseline_hash;
        const OpeningSuite opening_suite =
            read_openings(config.openings, config.max_plies);
        const auto& openings = opening_suite.openings;
        const std::uint64_t openings_hash = opening_suite.hash;
        const std::size_t target_pairs =
            config.pairs == 0U ? openings.size() : config.pairs;
        if (target_pairs > openings.size()) {
            throw std::runtime_error("requested more pairs than opening lines");
        }

        std::string candidate_name;
        std::string baseline_name;
        bool candidate_supports_hash = false;
        bool baseline_supports_hash = false;
        {
            UciEngine engine(config.candidate, config.candidate_identity,
                             config.hash_mb, config.timeout_ms);
            candidate_name = engine.name();
            candidate_supports_hash = engine.supports_hash();
        }
        {
            UciEngine engine(config.baseline, config.baseline_identity,
                             config.hash_mb, config.timeout_ms);
            baseline_name = engine.name();
            baseline_supports_hash = engine.supports_hash();
        }
        verify_engine(config.candidate, config.candidate_identity);
        verify_engine(config.baseline, config.baseline_identity);

        Fnv1a config_hash;
        config_hash.add_field("schema", "zfs-match-v2");
        config_hash.add_field("runner_path", runner_path);
        config_hash.add_number_field("runner_hash", runner_image.hash);
        config_hash.add_field("candidate_path", config.candidate);
        config_hash.add_number_field("candidate_hash", candidate_hash);
        config_hash.add_field("candidate_id", config.candidate_id);
        config_hash.add_field("baseline_path", config.baseline);
        config_hash.add_number_field("baseline_hash", baseline_hash);
        config_hash.add_field("baseline_id", config.baseline_id);
        config_hash.add_number_field("openings_hash", openings_hash);
        config_hash.add_number_field("pairs", target_pairs);
        config_hash.add_number_field("nodes", config.nodes);
        config_hash.add_number_field("movetime_ms", config.movetime_ms);
        config_hash.add_number_field("timeout_ms", config.timeout_ms);
        config_hash.add_number_field("hash_mb", config.hash_mb);
        config_hash.add_number_field("max_plies", config.max_plies);
        config_hash.add_number_field("elo0", config.elo0);
        config_hash.add_number_field("elo1", config.elo1);
        config_hash.add_number_field("alpha", config.alpha);
        config_hash.add_number_field("beta", config.beta);
        const std::string fingerprint = hex64(config_hash.value());

        ResumeState state;
        OutputFile output(config.output, config.resume);
        if (config.resume) {
            state = read_resume(config.output, fingerprint);
        }
        if (state.completed_pairs > target_pairs) {
            throw std::runtime_error("log already contains too many pairs");
        }

        if (config.resume) {
            std::error_code size_error;
            const std::uintmax_t log_size =
                std::filesystem::file_size(config.output, size_error);
            if (size_error || log_size < state.committed_bytes) {
                throw std::runtime_error("cannot inspect result log size");
            }
            if (log_size > state.committed_bytes) {
                output.truncate(state.committed_bytes);
            }
        }
        if (state.needs_line_separator) {
            output.append("\n");
        }
        if (!config.resume) {
            std::ostringstream manifest;
            manifest << "{\"type\":\"manifest\",\"schema\":2,\"config\":"
                     << quote(fingerprint) << ",\"created_utc\":" << quote(utc_now())
                     << ",\"runner\":{\"path\":" << quote(runner_path)
                     << ",\"content_fnv1a64\":" << quote(hex64(runner_image.hash))
                     << '}'
                     << ",\"candidate\":{\"path\":" << quote(config.candidate)
                     << ",\"id\":" << quote(config.candidate_id)
                     << ",\"uci_name\":" << quote(candidate_name)
                     << ",\"hash_option\":"
                     << (candidate_supports_hash ? "true" : "false")
                     << ",\"content_fnv1a64\":" << quote(hex64(candidate_hash))
                     << "},\"baseline\":{\"path\":" << quote(config.baseline)
                     << ",\"id\":" << quote(config.baseline_id)
                     << ",\"uci_name\":" << quote(baseline_name)
                     << ",\"hash_option\":"
                     << (baseline_supports_hash ? "true" : "false")
                     << ",\"content_fnv1a64\":" << quote(hex64(baseline_hash))
                     << "},\"openings\":{\"path\":" << quote(config.openings)
                     << ",\"content_fnv1a64\":" << quote(hex64(openings_hash))
                     << ",\"pairs\":" << target_pairs
                     << "},\"limit\":{\"nodes\":" << config.nodes
                     << ",\"movetime_ms\":" << config.movetime_ms
                     << ",\"timeout_ms\":" << config.timeout_ms
                     << ",\"hash_mb\":" << config.hash_mb
                     << ",\"max_plies\":" << config.max_plies
                     << "},\"sprt\":{\"elo0\":" << config.elo0
                     << ",\"elo1\":" << config.elo1
                     << ",\"alpha\":" << config.alpha
                     << ",\"beta\":" << config.beta << '}';
            const std::string payload = manifest.str();
            manifest << ",\"record_fnv1a64\":" << quote(checksum(payload))
                     << "}\n";
            output.append(manifest.str());
        }

        print_summary(state.results, config);
        if (zfs::eval::calculate_statistics(state.results, config.elo0,
                                            config.elo1, config.alpha,
                                            config.beta)
                .decision != zfs::eval::SprtDecision::Continue) {
            return 0;
        }
        for (std::size_t pair = state.completed_pairs; pair < target_pairs; ++pair) {
            const GameRecord first =
                run_game(config, openings[pair], zfs::Color::White);
            const GameRecord second =
                run_game(config, openings[pair], zfs::Color::Black);
            verify_engine(config.candidate, config.candidate_identity);
            verify_engine(config.baseline, config.baseline_identity);
            const unsigned first_points = candidate_half_points(first);
            const unsigned second_points = candidate_half_points(second);
            const unsigned points = first_points + second_points;
            ++state.results[points];
            const auto stats = zfs::eval::calculate_statistics(
                state.results, config.elo0, config.elo1, config.alpha, config.beta);
            std::ostringstream record;
            record << "{\"type\":\"pair\",\"pair\":" << pair
                   << ",\"opening_line\":" << openings[pair].line_number
                   << ",\"games\":[";
            write_game(record, 1, first);
            record << ',';
            write_game(record, 2, second);
            record << "],\"game_half_points\":[" << first_points << ','
                   << second_points << ']'
                   << ",\"candidate_half_points\":" << points
                   << ",\"pentanomial\":[" << state.results[0] << ','
                   << state.results[1] << ',' << state.results[2] << ','
                   << state.results[3] << ',' << state.results[4]
                   << "],\"llr\":" << std::setprecision(17) << stats.llr
                   << ",\"decision\":" << quote(zfs::eval::decision_name(stats.decision));
            const std::string payload = record.str();
            record << ",\"record_fnv1a64\":" << quote(checksum(payload))
                   << "}\n";
            output.append(record.str());
            print_summary(state.results, config);
            if (stats.decision != zfs::eval::SprtDecision::Continue) {
                break;
            }
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "zfs_match: " << error.what() << '\n';
        return 1;
    }
}
