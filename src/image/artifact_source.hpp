// SPDX-License-Identifier: MIT
#pragma once

#include "flash_artifact.hpp"
#include "sha256.hpp"

#include <chrono>
#include <compare>
#include <condition_variable>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>

namespace kairosboot::image {

class FileDirectoryBoundary;

enum class ArtifactSourceErrorKind : std::uint8_t {
    InvalidArgument,
    NotFound,
    UnsafePath,
    InvalidArchive,
    UnsupportedFeature,
    LimitExceeded,
    Integrity,
    Io,
    Cancelled,
    TimedOut,
    InvalidImage,
};

struct ArtifactSourceError final {
    ArtifactSourceErrorKind kind{ArtifactSourceErrorKind::Io};
    int native_code{};
    std::string message;
};

struct ArtifactSourceLimits final {
    using SpaceProvider = std::function<std::expected<std::uint64_t, std::error_code>(
        const std::filesystem::path&)>;

    std::uint64_t max_archive_size{32ULL * 1024ULL * 1024ULL * 1024ULL};
    std::uint64_t max_central_directory_size{256ULL * 1024ULL * 1024ULL};
    std::uint32_t max_entry_count{32'768U};
    std::uint32_t max_name_bytes{511U};
    std::uint64_t max_single_entry_size{128ULL * 1024ULL * 1024ULL * 1024ULL};
    std::uint64_t max_total_uncompressed_size{512ULL * 1024ULL * 1024ULL * 1024ULL};
    std::uint32_t max_compression_ratio{1'000U};
    std::uint64_t max_spool_bytes{128ULL * 1024ULL * 1024ULL * 1024ULL};
    std::uint64_t minimum_free_space_bytes{0U};
    std::chrono::milliseconds max_elapsed{std::chrono::minutes(30)};
    std::filesystem::path temporary_directory{};
    // Internal seam for deterministic disk-reservation tests. An empty provider
    // queries std::filesystem::space().
    SpaceProvider available_space_provider{};
    // Internal observation seam used to prove the per-resolver archive-reader
    // concurrency ceiling without exposing miniz state.
    std::function<void()> archive_reader_observer{};
    // Internal deterministic test seam. Package snapshots and root-relative
    // direct-file resolution invoke it immediately before revalidating and
    // opening one exact entry name.
    std::function<void(std::string_view)> package_entry_observer{};
};

enum class ArtifactSourceOrigin : std::uint8_t {
    DirectFile,
    DirectoryEntry,
    ZipEntry,
};

struct ResolvedArtifact final {
    std::shared_ptr<const IImageSource> source;
    Sha256Digest sha256{};
    ArtifactSourceOrigin origin{ArtifactSourceOrigin::DirectFile};
    std::string logical_name;
};

// One bounded, immutable view of a directory or ZIP update package. ZIP bytes
// and central-directory inventory are captured once. Directory entries are
// inventoried with exact cross-platform names and filesystem identities, then
// revalidated around every materialization and once more before publication.
class ArtifactPackageSnapshot final {
public:
    ArtifactPackageSnapshot(const ArtifactPackageSnapshot&) = delete;
    ArtifactPackageSnapshot& operator=(const ArtifactPackageSnapshot&) = delete;
    ArtifactPackageSnapshot(ArtifactPackageSnapshot&&) noexcept;
    ArtifactPackageSnapshot& operator=(ArtifactPackageSnapshot&&) noexcept;
    ~ArtifactPackageSnapshot();

    [[nodiscard]] std::expected<std::shared_ptr<const ResolvedArtifact>,
                                ArtifactSourceError>
    resolve(std::string_view entry_name);

    // Checks the shared absolute deadline and proves that the source container
    // still matches the identity/inventory captured at snapshot creation.
    [[nodiscard]] std::expected<void, ArtifactSourceError> verify_unchanged();

    [[nodiscard]] std::stop_token cancellation() const noexcept;

private:
    struct Impl;
    explicit ArtifactPackageSnapshot(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
    friend class ArtifactSourceResolver;
};

// Batch-scoped resolver. A successful key is materialized exactly once for the
// resolver lifetime, so every device receives the same immutable spool.
class ArtifactSourceResolver final {
    public:
    explicit ArtifactSourceResolver(ArtifactSourceLimits limits = {});

    [[nodiscard]] std::expected<std::shared_ptr<const ResolvedArtifact>,
                                ArtifactSourceError>
    resolve(const std::filesystem::path& archive_directory_or_file,
            std::string_view entry_name = {}, std::stop_token cancellation = {});

    // Resolves one direct file below a caller-selected root. The resolver opens
    // the root once, so a root symlink selects its target at capability capture.
    // Every child component is then opened from that retained directory handle;
    // root, ancestor, child, symlink, and reparse replacement races cannot
    // redirect the batch boundary.
    [[nodiscard]] std::expected<std::shared_ptr<const ResolvedArtifact>,
                                ArtifactSourceError>
    resolve_file_beneath(const std::filesystem::path& root,
                         const std::filesystem::path& relative_path,
                         std::stop_token cancellation = {});

    // Opens one package snapshot with a single absolute deadline shared by
    // inventory and every subsequent entry materialization.
    [[nodiscard]] std::expected<ArtifactPackageSnapshot, ArtifactSourceError>
    open_package_snapshot(
        const std::filesystem::path& archive_or_directory,
        std::stop_token cancellation = {});

    // Applies one caller-owned absolute deadline to inventory and every entry
    // resolution. The resolver's max_elapsed remains a safety ceiling, so the
    // effective deadline is the earlier of the two.
    [[nodiscard]] std::expected<ArtifactPackageSnapshot, ArtifactSourceError>
    open_package_snapshot(
        const std::filesystem::path& archive_or_directory,
        std::chrono::steady_clock::time_point deadline,
        std::stop_token cancellation = {});

    private:
    enum class ResolveMode : std::uint8_t {
        General,
        DirectFileBeneath,
    };

    struct CacheKey final {
        ResolveMode mode{ResolveMode::General};
        std::filesystem::path container;
        std::uint64_t boundary_device{};
        std::uint64_t boundary_object{};
        std::string entry;

        auto operator<=>(const CacheKey&) const = default;
    };

    struct CacheEntry final {
        bool done{};
        bool retryable_failure{};
        std::shared_ptr<const ResolvedArtifact> result;
        ArtifactSourceError error{
            .kind = ArtifactSourceErrorKind::Io,
            .native_code = 0,
            .message = "artifact resolution failed before publication",
        };
        std::shared_ptr<const FileDirectoryBoundary> boundary;
        std::condition_variable ready;
        std::uint64_t reserved_spool_bytes{};
    };

    [[nodiscard]] std::expected<std::shared_ptr<const ResolvedArtifact>,
                                ArtifactSourceError>
    resolve_impl(const std::filesystem::path& container,
                 std::string_view entry_name,
                 std::stop_token cancellation,
                 ResolveMode mode);

    ArtifactSourceLimits limits_;
    std::mutex cache_mutex_;
    std::map<std::filesystem::path,
             std::shared_ptr<const FileDirectoryBoundary>> boundaries_;
    std::map<CacheKey, std::shared_ptr<CacheEntry>> cache_;
    std::uint64_t reserved_spool_bytes_{};
    std::uint64_t completed_spool_bytes_{};
    std::optional<std::uint64_t> observed_spool_capacity_;
    // Miniz retains one archive central directory for the reader lifetime.
    // Serialize readers so adversarial different-key archives cannot multiply
    // the configured metadata ceiling within one batch.
    std::mutex archive_mutex_;
};

struct PreflightFlashArtifact final {
    std::shared_ptr<const ResolvedArtifact> resolved;
    FlashArtifact artifact;
};

// This seam intentionally has no transport dependency. Call it before device
// enumeration/open so malformed or unsafe artifacts cannot touch USB.
[[nodiscard]] std::expected<PreflightFlashArtifact, ArtifactSourceError>
preflight_flash_artifact(ArtifactSourceResolver& resolver,
                         const std::filesystem::path& archive_directory_or_file,
                         std::string_view entry_name = {},
                         std::stop_token cancellation = {});

[[nodiscard]] std::expected<PreflightFlashArtifact, ArtifactSourceError>
preflight_flash_artifact(ArtifactPackageSnapshot& snapshot,
                         std::string_view entry_name);

}  // namespace kairosboot::image
