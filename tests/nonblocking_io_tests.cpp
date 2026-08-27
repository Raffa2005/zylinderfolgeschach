#include "tools/eval/nonblocking_io.hpp"

#include <chrono>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <string>
#include <unistd.h>

namespace {

bool set_nonblocking(int descriptor) {
    const int flags = fcntl(descriptor, F_GETFL);
    return flags >= 0 && fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) == 0;
}

}  // namespace

int main() {
    int descriptors[2]{};
    if (pipe(descriptors) != 0 || !set_nonblocking(descriptors[1])) {
        std::cerr << "could not create nonblocking test pipe\n";
        return EXIT_FAILURE;
    }

    // No one drains the read end. The payload is deliberately much larger than
    // normal pipe capacities, so a blocking implementation would hang here.
    const std::string payload(16U * 1024U * 1024U, 'x');
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
    const auto result =
        zfs::eval::posix::write_until(descriptors[1], payload, deadline);
    close(descriptors[0]);
    close(descriptors[1]);

    if (result.status != zfs::eval::posix::WriteStatus::Timeout) {
        std::cerr << "full nonblocking pipe did not reach its deadline\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
