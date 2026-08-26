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
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>

namespace kairosboot::image {

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

// Batch-scoped resolver. A successful key is materialized exactly once for the
// resolver lifetime, so every device receives the same immutable spool.
class ArtifactSourceResolver final {
    public:
    explicit ArtifactSourceResolver(ArtifactSourceLimits limits = {});

    [[nodiscard]] std::expected<std::shared_ptr<const ResolvedArtifact>,
                                ArtifactSourceError>
    resolve(const std::filesystem::path& archive_directory_or_file,
            std::string_view entry_name = {}, std::stop_token cancellation = {});

    private:
    struct CacheKey final {
        std::filesystem::path container;
        std::string entry;

        auto operator<=>(const CacheKey&) const = default;
    };

    struct CacheEntry final {
        bool done{};
        std::shared_ptr<const ResolvedArtifact> result;
        std::optional<ArtifactSourceError> error;
        std::condition_variable ready;
    };

    ArtifactSourceLimits limits_;
    std::mutex cache_mutex_;
    std::map<CacheKey, std::shared_ptr<CacheEntry>> cache_;
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

}  // namespace kairosboot::image
