// SPDX-License-Identifier: MIT
#pragma once

#include "flash_artifact.hpp"

#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

namespace kairosboot::image {

// Matches AOSP's bounded-memory host policy without depending on libsparse.
inline constexpr std::uint64_t kDefaultResparseLimitBytes =
    1ULL * 1024ULL * 1024ULL * 1024ULL;

enum class SparseFlashPlanErrorKind : std::uint8_t {
    InvalidArgument,
    Unsupported,
    Source,
    ArithmeticOverflow,
    Cancelled,
};

struct SparseFlashPlanError final {
    SparseFlashPlanErrorKind kind{SparseFlashPlanErrorKind::InvalidArgument};
    std::uint64_t output_offset{};
    std::string message;
};

struct SparseFlashPart final {
    std::shared_ptr<const IImageSource> source;
    // Expanded byte interval containing actual RAW/FILL data. DONT_CARE gaps
    // outside this interval are encoded only to preserve partition offsets.
    std::uint64_t first_data_offset{};
    std::uint64_t data_end_offset{};
};

// Produces one or more independently valid Android sparse payloads when the
// original encoded input exceeds a device's maximum download size. Existing
// payloads that fit are preserved byte-for-byte. The planner never expands an
// image in memory and every returned part is random-access and immutable.
class SparseFlashPlan final {
public:
    [[nodiscard]] static std::expected<SparseFlashPlan, SparseFlashPlanError>
    create(const FlashArtifact& artifact,
           std::uint64_t target_max_download_size,
           std::uint64_t host_resparse_limit = kDefaultResparseLimitBytes,
           std::stop_token stop_token = {});

    [[nodiscard]] std::span<const SparseFlashPart> parts() const noexcept;
    [[nodiscard]] bool reparsed() const noexcept;
    [[nodiscard]] std::uint64_t expanded_size() const noexcept;
    [[nodiscard]] std::uint64_t transfer_size() const noexcept;

private:
    SparseFlashPlan(std::vector<SparseFlashPart> parts,
                    bool reparsed,
                    std::uint64_t expanded_size,
                    std::uint64_t transfer_size) noexcept;

    std::vector<SparseFlashPart> parts_;
    bool reparsed_{};
    std::uint64_t expanded_size_{};
    std::uint64_t transfer_size_{};
};

}  // namespace kairosboot::image
