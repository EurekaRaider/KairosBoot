// SPDX-License-Identifier: MIT
#include "tcp_fastboot.hpp"

#ifndef _WIN32

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <string_view>
#include <utility>

namespace {

using namespace std::chrono_literals;
using kairosboot::protocol::TransportStatus;
using kairosboot::transport::SocketIoStatus;
using kairosboot::transport::TcpEndpoint;
using kairosboot::transport::TcpFastbootTransport;
using kairosboot::transport::connect_native_tcp_socket;
using kairosboot::transport::kFastbootTcpV1Handshake;

class FileDescriptor {
public:
    FileDescriptor() = default;
    explicit FileDescriptor(const int value) noexcept : value_(value) {}
    ~FileDescriptor() { reset(); }

    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;

    FileDescriptor(FileDescriptor&& other) noexcept
        : value_(std::exchange(other.value_, -1)) {}

    FileDescriptor& operator=(FileDescriptor&& other) noexcept {
        if (this != &other) {
            reset(std::exchange(other.value_, -1));
        }
        return *this;
    }

    [[nodiscard]] int get() const noexcept { return value_; }

    void reset(const int replacement = -1) noexcept {
        if (value_ >= 0) {
            while (::close(value_) != 0 && errno == EINTR) {
            }
        }
        value_ = replacement;
    }

private:
    int value_{-1};
};

class ChildProcess {
public:
    explicit ChildProcess(const pid_t process) noexcept : process_(process) {}
    ~ChildProcess() {
        if (process_ > 0) {
            static_cast<void>(::kill(process_, SIGKILL));
            while (::waitpid(process_, nullptr, 0) < 0 && errno == EINTR) {
            }
        }
    }

    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    void mark_reaped() noexcept { process_ = -1; }

private:
    pid_t process_{-1};
};

struct Listener {
    FileDescriptor socket;
    std::uint16_t port{0};
};

enum class ChildMode {
    RawSocket,
    FastbootTransport,
};

[[nodiscard]] bool wait_readable(const int descriptor, const int timeout_ms) {
    pollfd item{
        .fd = descriptor,
        .events = POLLIN,
        .revents = 0,
    };
    for (;;) {
        const auto ready = ::poll(&item, 1, timeout_ms);
        if (ready > 0) {
            return true;
        }
        if (ready == 0) {
            return false;
        }
        if (errno != EINTR) {
            return false;
        }
    }
}

[[nodiscard]] bool write_exact(
    const int descriptor,
    const std::span<const std::byte> bytes) {
    std::size_t completed = 0;
    while (completed < bytes.size()) {
        const auto amount = ::write(
            descriptor,
            bytes.data() + static_cast<std::ptrdiff_t>(completed),
            bytes.size() - completed);
        if (amount > 0) {
            completed += static_cast<std::size_t>(amount);
            continue;
        }
        if (amount < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

[[nodiscard]] bool read_exact(
    const int descriptor,
    const std::span<std::byte> bytes) {
    std::size_t completed = 0;
    while (completed < bytes.size()) {
        const auto amount = ::read(
            descriptor,
            bytes.data() + static_cast<std::ptrdiff_t>(completed),
            bytes.size() - completed);
        if (amount > 0) {
            completed += static_cast<std::size_t>(amount);
            continue;
        }
        if (amount < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

[[nodiscard]] bool write_signal_byte(const int descriptor) {
    constexpr std::array signal{std::byte{0x5A}};
    return write_exact(descriptor, signal);
}

[[nodiscard]] bool read_signal_byte(const int descriptor) {
    std::array<std::byte, 1> signal{};
    return read_exact(descriptor, signal) && signal[0] == std::byte{0x5A};
}

[[nodiscard]] Listener make_listener() {
    FileDescriptor listener(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (listener.get() < 0) {
        return {};
    }
    int reuse = 1;
    static_cast<void>(::setsockopt(
        listener.get(), SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = 0;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(
            listener.get(),
            reinterpret_cast<const sockaddr*>(&address),
            sizeof(address)) != 0 ||
        ::listen(listener.get(), 1) != 0) {
        return {};
    }
    socklen_t address_size = sizeof(address);
    if (::getsockname(
            listener.get(),
            reinterpret_cast<sockaddr*>(&address),
            &address_size) != 0) {
        return {};
    }
    return {
        .socket = std::move(listener),
        .port = ntohs(address.sin_port),
    };
}

[[nodiscard]] bool server_handshake(const int peer) {
    std::array<std::byte, 4> received{};
    return read_exact(peer, received) && received == kFastbootTcpV1Handshake &&
           write_exact(peer, kFastbootTcpV1Handshake);
}

[[noreturn]] void child_main(
    const ChildMode mode,
    const std::uint16_t port,
    const int inherited_listener,
    const int ready_pipe,
    const int continue_pipe) {
    ::close(inherited_listener);
    ::signal(SIGPIPE, SIG_DFL);

    auto socket = connect_native_tcp_socket(
        TcpEndpoint{.host = "127.0.0.1", .port = port}, 2s);
    if (!socket) {
        _exit(20);
    }

    if (mode == ChildMode::RawSocket) {
        if (!write_signal_byte(ready_pipe) || !read_signal_byte(continue_pipe)) {
            _exit(21);
        }
        constexpr std::array payload{std::byte{0xA5}};
        std::size_t failures = 0;
        // Reusing the raw adapter after the first reset error deliberately
        // reaches Linux's EPIPE/SIGPIPE path if MSG_NOSIGNAL is removed.
        for (int attempt = 0; attempt < 4; ++attempt) {
            const auto result = (*socket)->send_some(payload, 500ms, {});
            if (result.status != SocketIoStatus::Ok) {
                ++failures;
            }
        }
        _exit(failures == 0 ? 22 : 0);
    }

    auto transport = TcpFastbootTransport::create(std::move(*socket));
    if (!transport) {
        _exit(23);
    }
    if (!write_signal_byte(ready_pipe) || !read_signal_byte(continue_pipe)) {
        _exit(24);
    }
    constexpr std::array payload{std::byte{0x01}, std::byte{0x02}};
    bool failed = false;
    for (int attempt = 0; attempt < 8 && !failed; ++attempt) {
        const auto result = (*transport)->write(payload, 500ms);
        failed = result.status != TransportStatus::Ok;
        if (!failed) {
            ::usleep(1'000);
        }
    }
    if (!failed) {
        _exit(25);
    }
    if ((*transport)->is_open()) {
        _exit(26);
    }
    const auto retry = (*transport)->write(payload, 500ms);
    _exit(retry.status == TransportStatus::Disconnected ? 0 : 27);
}

[[nodiscard]] bool run_case(const ChildMode mode, const std::string_view name) {
    auto listener = make_listener();
    if (listener.socket.get() < 0 || listener.port == 0) {
        std::cerr << name << ": failed to create loopback listener\n";
        return false;
    }

    int ready_pipe_values[2]{};
    if (::pipe(ready_pipe_values) != 0) {
        std::cerr << name << ": failed to create ready pipe\n";
        return false;
    }
    FileDescriptor ready_read(ready_pipe_values[0]);
    FileDescriptor ready_write(ready_pipe_values[1]);
    int continue_pipe_values[2]{};
    if (::pipe(continue_pipe_values) != 0) {
        std::cerr << name << ": failed to create continue pipe\n";
        return false;
    }
    FileDescriptor continue_read(continue_pipe_values[0]);
    FileDescriptor continue_write(continue_pipe_values[1]);

    const auto child = ::fork();
    if (child < 0) {
        std::cerr << name << ": fork failed\n";
        return false;
    }
    if (child == 0) {
        ready_read.reset();
        continue_write.reset();
        child_main(
            mode,
            listener.port,
            listener.socket.get(),
            ready_write.get(),
            continue_read.get());
    }
    ChildProcess child_process(child);

    ready_write.reset();
    continue_read.reset();
    if (!wait_readable(listener.socket.get(), 2'000)) {
        std::cerr << name << ": child did not connect\n";
        return false;
    }
    FileDescriptor peer(::accept(listener.socket.get(), nullptr, nullptr));
    listener.socket.reset();
    if (peer.get() < 0) {
        std::cerr << name << ": accept failed\n";
        return false;
    }
    if (mode == ChildMode::FastbootTransport && !server_handshake(peer.get())) {
        std::cerr << name << ": Fastboot TCP handshake failed\n";
        return false;
    }
    if (!wait_readable(ready_read.get(), 2'000) || !read_signal_byte(ready_read.get())) {
        std::cerr << name << ": child did not become ready\n";
        return false;
    }

    linger reset_on_close{
        .l_onoff = 1,
        .l_linger = 0,
    };
    if (::setsockopt(
            peer.get(),
            SOL_SOCKET,
            SO_LINGER,
            &reset_on_close,
            sizeof(reset_on_close)) != 0) {
        std::cerr << name << ": failed to configure reset-on-close\n";
        return false;
    }
    peer.reset();
    if (!write_signal_byte(continue_write.get())) {
        std::cerr << name << ": failed to release child\n";
        return false;
    }
    continue_write.reset();

    int status = 0;
    while (::waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) {
            std::cerr << name << ": waitpid failed\n";
            return false;
        }
    }
    child_process.mark_reaped();
    if (WIFSIGNALED(status)) {
        std::cerr << name << ": child terminated by signal " << WTERMSIG(status);
        if (WTERMSIG(status) == SIGPIPE) {
            std::cerr << " (SIGPIPE)";
        }
        std::cerr << '\n';
        return false;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        std::cerr << name << ": child exited with code "
                  << (WIFEXITED(status) ? WEXITSTATUS(status) : -1) << '\n';
        return false;
    }
    return true;
}

}  // namespace

int main() {
    ::signal(SIGPIPE, SIG_IGN);
    if (!run_case(ChildMode::RawSocket, "raw socket SIGPIPE regression")) {
        return 1;
    }
    if (!run_case(ChildMode::FastbootTransport, "transport poison regression")) {
        return 1;
    }
    std::cout << "loopback reset survived without SIGPIPE and poisoned transport\n";
    return 0;
}

#endif  // !_WIN32
