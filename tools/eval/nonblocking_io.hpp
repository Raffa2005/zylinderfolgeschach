#pragma once

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <limits>
#include <poll.h>
#include <string_view>
#include <unistd.h>

namespace zfs::eval::posix {

enum class WriteStatus { Complete, Timeout, Error };

struct WriteResult {
    WriteStatus status = WriteStatus::Error;
    int error_number = 0;
};

[[nodiscard]] inline WriteResult write_until(
    int descriptor, std::string_view data,
    std::chrono::steady_clock::time_point deadline) noexcept {
    std::size_t written = 0;
    while (written < data.size()) {
        const ssize_t count =
            write(descriptor, data.data() + written, data.size() - written);
        if (count > 0) {
            written += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
            return {WriteStatus::Error, count == 0 ? EIO : errno};
        }

        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now())
                .count();
        if (remaining <= 0) {
            return {WriteStatus::Timeout, 0};
        }
        pollfd poll_descriptor{descriptor, POLLOUT, 0};
        const int poll_timeout = static_cast<int>(std::min<std::int64_t>(
            remaining, std::numeric_limits<int>::max()));
        const int ready = poll(&poll_descriptor, 1, poll_timeout);
        if (ready > 0 || (ready < 0 && errno == EINTR)) {
            continue;
        }
        if (ready == 0) {
            return {WriteStatus::Timeout, 0};
        }
        return {WriteStatus::Error, errno};
    }
    return {WriteStatus::Complete, 0};
}

}  // namespace zfs::eval::posix
