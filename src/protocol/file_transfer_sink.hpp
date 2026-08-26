// SPDX-License-Identifier: MIT
#pragma once

#include "transport_session.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>

namespace kairosboot::protocol {

enum class FileTransferSinkErrorKind : std::uint8_t {
    InvalidArgument,
    UnsafePath,
    ParentUnavailable,
    CreateFailed,
    Incomplete,
    SyncFailed,
    PublishFailed,
    InvalidState,
    AllocationFailed,
};

struct FileTransferSinkError final {
    FileTransferSinkErrorKind kind{FileTransferSinkErrorKind::CreateFailed};
    int native_code{0};
    std::string message;
};

// A bounded-memory destination for Fastboot device-to-host payloads.
//
// Bytes are written to a private temporary file in the destination directory.
// The caller must invoke seal() only after the protocol operation succeeds,
// passing the exact inbound byte count reported by the protocol layer. seal()
// synchronizes the temporary file and atomically replaces the destination.
// Every other terminal path removes the temporary file and preserves the
// previously published destination.
class FileTransferSink final : public ITransferSink {
public:
    [[nodiscard]] static std::expected<
        std::shared_ptr<FileTransferSink>, FileTransferSinkError>
    create(const std::filesystem::path& destination);

    FileTransferSink(const FileTransferSink&) = delete;
    FileTransferSink& operator=(const FileTransferSink&) = delete;
    ~FileTransferSink() override;

    [[nodiscard]] TransferResult write(
        std::uint64_t offset,
        std::span<const std::byte> source) noexcept override;

    [[nodiscard]] std::expected<void, FileTransferSinkError> seal(
        std::uint64_t expected_size);

    // Explicitly abandons an unfinished transfer. This is idempotent; a sink
    // that was already sealed remains published.
    void discard() noexcept;

    [[nodiscard]] std::uint64_t bytes_written() const;
    [[nodiscard]] bool is_sealed() const;

private:
#if defined(_WIN32)
    using NativeHandle = void*;
#else
    using NativeHandle = int;
#endif

    enum class State : std::uint8_t {
        Open,
        Failed,
        Sealed,
        Discarded,
    };

    FileTransferSink(
        NativeHandle file,
        NativeHandle directory,
        std::filesystem::path destination_leaf,
        std::filesystem::path temporary_leaf,
        std::filesystem::path temporary_path) noexcept;

    void cleanup_locked() noexcept;
    void close_after_publish_locked() noexcept;
    [[nodiscard]] TransferResult fail_write_locked(
        std::size_t transferred,
        int native_code,
        const char* detail) noexcept;
    [[nodiscard]] std::unexpected<FileTransferSinkError> fail_seal_locked(
        FileTransferSinkErrorKind kind,
        int native_code,
        std::string message);

    mutable std::mutex mutex_;
    NativeHandle file_{};
    NativeHandle directory_{};
    std::filesystem::path destination_leaf_;
    std::filesystem::path temporary_leaf_;
    std::filesystem::path temporary_path_;
    std::uint64_t committed_{0};
    State state_{State::Open};
};

}  // namespace kairosboot::protocol
