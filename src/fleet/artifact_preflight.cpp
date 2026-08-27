// SPDX-License-Identifier: MIT
#include "src/fleet/artifact_preflight.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <new>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>

namespace kairosboot::fleet {
namespace {

struct ForwardStop final {
    std::stop_source* destination{};

    void operator()() const noexcept { (void)destination->request_stop(); }
};

// Joins an external cancellation request and one absolute deadline into the
// single stop_token accepted by ArtifactSourceResolver and FlashArtifact.
class PreflightStopController final {
public:
    PreflightStopController(const std::stop_token external,
                            const ArtifactPreflightTimePoint deadline)
        : external_(external),
          deadline_(deadline),
          forward_(std::in_place, external, ForwardStop{&combined_}) {
        if (deadline != ArtifactPreflightTimePoint::max()) {
            timer_.emplace([this, deadline](const std::stop_token stopped) {
                std::unique_lock lock(timer_mutex_);
                (void)timer_ready_.wait_until(
                    lock, stopped, deadline, [] { return false; });
                if (!stopped.stop_requested()) {
                    timed_out_.store(true, std::memory_order_release);
                    (void)combined_.request_stop();
                }
            });
        }
    }

    PreflightStopController(const PreflightStopController&) = delete;
    PreflightStopController& operator=(const PreflightStopController&) = delete;
    ~PreflightStopController() = default;

    [[nodiscard]] std::stop_token token() const noexcept {
        return combined_.get_token();
    }

    [[nodiscard]] bool cancelled() const noexcept {
        return external_.stop_requested();
    }

    [[nodiscard]] bool timed_out() const noexcept {
        return timed_out_.load(std::memory_order_acquire) ||
            ArtifactPreflightClock::now() >= deadline_;
    }

private:
    // Destruction is reverse declaration order: timer_ is stopped and joined
    // before any state captured by its callback is destroyed.
    std::stop_token external_;
    ArtifactPreflightTimePoint deadline_;
    std::stop_source combined_;
    std::optional<std::stop_callback<ForwardStop>> forward_;
    std::mutex timer_mutex_;
    std::condition_variable_any timer_ready_;
    std::atomic<bool> timed_out_{false};
    std::optional<std::jthread> timer_;
};

[[nodiscard]] ArtifactPreflightTimePoint effective_deadline(
    const ArtifactPreflightOptions& options) noexcept {
    const auto now = ArtifactPreflightClock::now();
    const auto elapsed = options.source_limits.max_elapsed;
    ArtifactPreflightTimePoint safety_deadline = now;
    if (elapsed.count() > 0) {
        const auto remaining = ArtifactPreflightTimePoint::max() - now;
        safety_deadline = elapsed >= remaining
            ? ArtifactPreflightTimePoint::max()
            : now + elapsed;
    }
    return std::min(options.deadline, safety_deadline);
}

[[nodiscard]] bool safe_relative_artifact_path(
    const std::string_view value) noexcept {
    if (value.empty() || value.front() == '/' || value.back() == '/' ||
        value.find('\\') != std::string_view::npos ||
        value.find(':') != std::string_view::npos) {
        return false;
    }
    std::size_t begin = 0U;
    while (begin < value.size()) {
        const auto separator = value.find('/', begin);
        const auto end = separator == std::string_view::npos
            ? value.size()
            : separator;
        const auto component = value.substr(begin, end - begin);
        if (component.empty() || component == "." || component == "..") {
            return false;
        }
        for (const char character : component) {
            const auto byte = static_cast<unsigned char>(character);
            if (byte < 0x20U || byte == 0x7FU) {
                return false;
            }
        }
        if (separator == std::string_view::npos) {
            return true;
        }
        begin = separator + 1U;
    }
    return true;
}

[[nodiscard]] std::filesystem::path utf8_path(const std::string_view value) {
#if defined(_WIN32)
    std::u8string utf8;
    utf8.reserve(value.size());
    for (const char character : value) {
        utf8.push_back(static_cast<char8_t>(static_cast<unsigned char>(character)));
    }
    return std::filesystem::path(utf8);
#else
    return std::filesystem::path(value);
#endif
}

[[nodiscard]] bool parse_digest(const std::string_view value,
                                image::Sha256Digest& digest) noexcept {
    if (value.size() != image::kSha256DigestSize * 2U) {
        return false;
    }
    for (std::size_t index = 0U; index < digest.size(); ++index) {
        const auto decode = [](const char digit) -> std::optional<unsigned int> {
            if (digit >= '0' && digit <= '9') {
                return static_cast<unsigned int>(digit - '0');
            }
            if (digit >= 'a' && digit <= 'f') {
                return static_cast<unsigned int>(digit - 'a' + 10);
            }
            return std::nullopt;
        };
        const auto high = decode(value[index * 2U]);
        const auto low = decode(value[index * 2U + 1U]);
        if (!high || !low) {
            return false;
        }
        digest[index] = static_cast<std::byte>((*high << 4U) | *low);
    }
    return true;
}

[[nodiscard]] ArtifactPreflightError error_for(
    const ArtifactPreflightErrorKind kind,
    const std::size_t index,
    const ManifestArtifact& artifact,
    std::filesystem::path path,
    const image::ArtifactSourceErrorKind source_kind,
    const int native_code,
    std::string message) {
    return {
        .kind = kind,
        .artifact_index = index,
        .artifact_id = artifact.id.value,
        .path = std::move(path),
        .source_kind = source_kind,
        .native_code = native_code,
        .message = std::move(message),
    };
}

[[nodiscard]] ArtifactPreflightErrorKind mapped_source_kind(
    const image::ArtifactSourceErrorKind kind) noexcept {
    switch (kind) {
        case image::ArtifactSourceErrorKind::InvalidArgument:
            return ArtifactPreflightErrorKind::InvalidArgument;
        case image::ArtifactSourceErrorKind::NotFound:
            return ArtifactPreflightErrorKind::NotFound;
        case image::ArtifactSourceErrorKind::UnsafePath:
            return ArtifactPreflightErrorKind::UnsafePath;
        case image::ArtifactSourceErrorKind::LimitExceeded:
            return ArtifactPreflightErrorKind::LimitExceeded;
        case image::ArtifactSourceErrorKind::Integrity:
            return ArtifactPreflightErrorKind::Integrity;
        case image::ArtifactSourceErrorKind::Cancelled:
            return ArtifactPreflightErrorKind::Cancelled;
        case image::ArtifactSourceErrorKind::TimedOut:
            return ArtifactPreflightErrorKind::TimedOut;
        case image::ArtifactSourceErrorKind::InvalidImage:
            return ArtifactPreflightErrorKind::InvalidImage;
        case image::ArtifactSourceErrorKind::InvalidArchive:
        case image::ArtifactSourceErrorKind::UnsupportedFeature:
        case image::ArtifactSourceErrorKind::Io:
            return ArtifactPreflightErrorKind::Io;
    }
    return ArtifactPreflightErrorKind::UnexpectedFailure;
}

[[nodiscard]] ArtifactPreflightErrorKind stopped_kind(
    const PreflightStopController& stopped,
    const ArtifactPreflightErrorKind fallback) noexcept {
    if (stopped.cancelled()) {
        return ArtifactPreflightErrorKind::Cancelled;
    }
    if (stopped.timed_out()) {
        return ArtifactPreflightErrorKind::TimedOut;
    }
    return fallback;
}

void invoke_fault(const ArtifactPreflightOptions& options,
                  const ArtifactPreflightFaultPoint point,
                  const std::size_t index) {
    if (options.fault_hook != nullptr) {
        options.fault_hook(point, index, options.fault_context);
    }
}

}  // namespace

PreparedArtifactSource::PreparedArtifactSource(
    std::shared_ptr<const image::ResolvedArtifact> resolved,
    image::FlashArtifact&& flash_artifact,
    std::string&& sha256_hex) noexcept
    : resolved_(std::move(resolved)),
      flash_artifact_(std::move(flash_artifact)),
      sha256_hex_(std::move(sha256_hex)) {}

const std::shared_ptr<const image::ResolvedArtifact>&
PreparedArtifactSource::resolved() const noexcept {
    return resolved_;
}

const image::FlashArtifact& PreparedArtifactSource::flash_artifact() const noexcept {
    return flash_artifact_;
}

std::string_view PreparedArtifactSource::sha256_hex() const noexcept {
    return sha256_hex_;
}

PreparedFleetArtifact::PreparedFleetArtifact(
    const std::size_t index,
    std::string&& id,
    std::string&& declared_path,
    std::filesystem::path&& source_path,
    std::string&& declared_sha256,
    std::shared_ptr<const PreparedArtifactSource> source) noexcept
    : index_(index),
      id_(std::move(id)),
      declared_path_(std::move(declared_path)),
      source_path_(std::move(source_path)),
      declared_sha256_(std::move(declared_sha256)),
      source_(std::move(source)) {}

std::size_t PreparedFleetArtifact::index() const noexcept { return index_; }

std::string_view PreparedFleetArtifact::id() const noexcept { return id_; }

std::string_view PreparedFleetArtifact::declared_path() const noexcept {
    return declared_path_;
}

const std::filesystem::path& PreparedFleetArtifact::source_path() const noexcept {
    return source_path_;
}

std::string_view PreparedFleetArtifact::declared_sha256() const noexcept {
    return declared_sha256_;
}

const std::shared_ptr<const PreparedArtifactSource>&
PreparedFleetArtifact::source() const noexcept {
    return source_;
}

PreparedFleetArtifacts::PreparedFleetArtifacts(
    const JobPlan& plan,
    std::filesystem::path&& artifact_root,
    std::vector<PreparedFleetArtifact>&& artifacts) noexcept
    : plan_(&plan),
      artifact_root_(std::move(artifact_root)),
      artifacts_(std::move(artifacts)) {}

const JobPlan& PreparedFleetArtifacts::plan() const noexcept { return *plan_; }

const std::filesystem::path& PreparedFleetArtifacts::artifact_root() const noexcept {
    return artifact_root_;
}

std::size_t PreparedFleetArtifacts::size() const noexcept {
    return artifacts_.size();
}

const PreparedFleetArtifact& PreparedFleetArtifacts::at(
    const std::size_t index) const {
    return artifacts_.at(index);
}

const PreparedFleetArtifact* PreparedFleetArtifacts::find(
    const std::string_view id) const noexcept {
    const auto found = std::ranges::find_if(
        artifacts_, [id](const PreparedFleetArtifact& artifact) {
            return artifact.id() == id;
        });
    return found == artifacts_.end() ? nullptr : &*found;
}

std::expected<PreparedFleetArtifacts, ArtifactPreflightError>
preflight_fleet_artifacts(const JobPlan& plan,
                          const std::filesystem::path& artifact_root,
                          const ArtifactPreflightOptions& options) noexcept {
    std::size_t active_index = 0U;
    std::filesystem::path active_path = artifact_root;
    try {
        const auto& manifest = plan.manifest();
        if (manifest.artifacts.empty() || artifact_root.empty()) {
            const ManifestArtifact fallback{};
            return std::unexpected(error_for(
                ArtifactPreflightErrorKind::InvalidArgument,
                0U,
                manifest.artifacts.empty() ? fallback : manifest.artifacts.front(),
                artifact_root,
                image::ArtifactSourceErrorKind::InvalidArgument,
                0,
                "artifact root and JobPlan artifact set must be non-empty"));
        }
        if (options.source_limits.max_elapsed.count() < 0) {
            return std::unexpected(error_for(
                ArtifactPreflightErrorKind::InvalidArgument,
                0U,
                manifest.artifacts.front(),
                artifact_root,
                image::ArtifactSourceErrorKind::InvalidArgument,
                0,
                "artifact preflight elapsed-time limit is negative"));
        }

        std::error_code path_error;
        auto normalized_root = std::filesystem::absolute(artifact_root, path_error);
        if (path_error) {
            return std::unexpected(error_for(
                ArtifactPreflightErrorKind::InvalidArgument,
                0U,
                manifest.artifacts.front(),
                artifact_root,
                image::ArtifactSourceErrorKind::InvalidArgument,
                path_error.value(),
                "unable to normalize artifact root"));
        }
        normalized_root = normalized_root.lexically_normal();

        const auto deadline = effective_deadline(options);
        if (options.cancellation.stop_requested()) {
            return std::unexpected(error_for(
                ArtifactPreflightErrorKind::Cancelled,
                0U,
                manifest.artifacts.front(),
                normalized_root / utf8_path(manifest.artifacts.front().path.value),
                image::ArtifactSourceErrorKind::Cancelled,
                0,
                "artifact preflight was cancelled before resolution"));
        }
        if (ArtifactPreflightClock::now() >= deadline) {
            return std::unexpected(error_for(
                ArtifactPreflightErrorKind::TimedOut,
                0U,
                manifest.artifacts.front(),
                normalized_root / utf8_path(manifest.artifacts.front().path.value),
                image::ArtifactSourceErrorKind::TimedOut,
                0,
                "artifact preflight deadline expired before resolution"));
        }

        std::vector<std::filesystem::path> source_paths;
        std::vector<image::Sha256Digest> declared_digests;
        source_paths.reserve(manifest.artifacts.size());
        declared_digests.reserve(manifest.artifacts.size());
        std::map<std::string_view, std::size_t> ids;
        std::map<std::filesystem::path, std::pair<image::Sha256Digest, std::size_t>>
            paths;

        for (std::size_t index = 0U; index < manifest.artifacts.size(); ++index) {
            active_index = index;
            const auto& artifact = manifest.artifacts[index];
            if (options.cancellation.stop_requested()) {
                return std::unexpected(error_for(
                    ArtifactPreflightErrorKind::Cancelled,
                    index,
                    artifact,
                    normalized_root / utf8_path(artifact.path.value),
                    image::ArtifactSourceErrorKind::Cancelled,
                    0,
                    "artifact preflight was cancelled during plan validation"));
            }
            if (ArtifactPreflightClock::now() >= deadline) {
                return std::unexpected(error_for(
                    ArtifactPreflightErrorKind::TimedOut,
                    index,
                    artifact,
                    normalized_root / utf8_path(artifact.path.value),
                    image::ArtifactSourceErrorKind::TimedOut,
                    0,
                    "artifact preflight deadline expired during plan validation"));
            }
            if (!safe_relative_artifact_path(artifact.path.value)) {
                return std::unexpected(error_for(
                    ArtifactPreflightErrorKind::UnsafePath,
                    index,
                    artifact,
                    normalized_root / utf8_path(artifact.path.value),
                    image::ArtifactSourceErrorKind::UnsafePath,
                    0,
                    "JobPlan artifact path is not a strict relative path"));
            }
            image::Sha256Digest digest{};
            if (!parse_digest(artifact.sha256.value, digest)) {
                return std::unexpected(error_for(
                    ArtifactPreflightErrorKind::InvalidPlan,
                    index,
                    artifact,
                    normalized_root / utf8_path(artifact.path.value),
                    image::ArtifactSourceErrorKind::InvalidArgument,
                    0,
                    "JobPlan artifact SHA-256 is not canonical lowercase hex"));
            }
            const auto source_path =
                (normalized_root / utf8_path(artifact.path.value)).lexically_normal();
            active_path = source_path;
            if (!ids.emplace(artifact.id.value, index).second) {
                return std::unexpected(error_for(
                    ArtifactPreflightErrorKind::DuplicateArtifactId,
                    index,
                    artifact,
                    source_path,
                    image::ArtifactSourceErrorKind::InvalidArgument,
                    0,
                    "JobPlan artifact id is duplicated"));
            }
            const auto [position, inserted] =
                paths.emplace(source_path, std::pair{digest, index});
            if (!inserted && position->second.first != digest) {
                return std::unexpected(error_for(
                    ArtifactPreflightErrorKind::ConflictingDeclaredDigest,
                    index,
                    artifact,
                    source_path,
                    image::ArtifactSourceErrorKind::Integrity,
                    0,
                    "the same normalized artifact path declares conflicting SHA-256 values"));
            }
            source_paths.push_back(source_path);
            declared_digests.push_back(digest);
        }

        PreflightStopController stopped(options.cancellation, deadline);
        image::ArtifactSourceResolver resolver(options.source_limits);
        std::map<std::string, std::shared_ptr<const PreparedArtifactSource>>
            sources_by_digest;
        std::vector<PreparedFleetArtifact> prepared;
        prepared.reserve(manifest.artifacts.size());

        for (std::size_t index = 0U; index < manifest.artifacts.size(); ++index) {
            active_index = index;
            const auto& artifact = manifest.artifacts[index];
            active_path = source_paths[index];
            invoke_fault(options,
                         ArtifactPreflightFaultPoint::BeforeArtifactResolve,
                         index);
            auto resolved = resolver.resolve(source_paths[index], {}, stopped.token());
            if (!resolved) {
                auto kind = mapped_source_kind(resolved.error().kind);
                kind = stopped_kind(stopped, kind);
                return std::unexpected(error_for(
                    kind,
                    index,
                    artifact,
                    source_paths[index],
                    resolved.error().kind,
                    resolved.error().native_code,
                    resolved.error().message));
            }
            if ((*resolved)->sha256 != declared_digests[index]) {
                return std::unexpected(error_for(
                    ArtifactPreflightErrorKind::HashMismatch,
                    index,
                    artifact,
                    source_paths[index],
                    image::ArtifactSourceErrorKind::Integrity,
                    0,
                    "artifact SHA-256 does not match the JobPlan declaration"));
            }

            auto actual_hex = image::sha256_hex((*resolved)->sha256);
            auto source_position = sources_by_digest.find(actual_hex);
            std::shared_ptr<const PreparedArtifactSource> prepared_source;
            if (source_position == sources_by_digest.end()) {
                auto inspected =
                    image::FlashArtifact::inspect((*resolved)->source, stopped.token());
                if (!inspected) {
                    const auto kind = stopped_kind(
                        stopped,
                        inspected.error().kind == image::SparseErrorKind::Cancelled
                            ? ArtifactPreflightErrorKind::Cancelled
                            : ArtifactPreflightErrorKind::InvalidImage);
                    return std::unexpected(error_for(
                        kind,
                        index,
                        artifact,
                        source_paths[index],
                        kind == ArtifactPreflightErrorKind::Cancelled
                            ? image::ArtifactSourceErrorKind::Cancelled
                            : image::ArtifactSourceErrorKind::InvalidImage,
                        0,
                        "flash artifact inspection failed: " +
                            inspected.error().message));
                }
                auto mutable_source = std::shared_ptr<PreparedArtifactSource>(
                    new PreparedArtifactSource(
                        std::move(*resolved),
                        std::move(*inspected),
                        std::move(actual_hex)));
                prepared_source = mutable_source;
                sources_by_digest.emplace(
                    std::string(prepared_source->sha256_hex()),
                    prepared_source);
            } else {
                prepared_source = source_position->second;
            }

            prepared.push_back(PreparedFleetArtifact{
                index,
                std::string(artifact.id.value),
                std::string(artifact.path.value),
                std::filesystem::path(source_paths[index]),
                std::string(artifact.sha256.value),
                std::move(prepared_source),
            });
        }

        active_index = manifest.artifacts.size() - 1U;
        invoke_fault(options,
                     ArtifactPreflightFaultPoint::BeforePublish,
                     active_index);
        if (options.cancellation.stop_requested()) {
            return std::unexpected(error_for(
                ArtifactPreflightErrorKind::Cancelled,
                active_index,
                manifest.artifacts[active_index],
                source_paths[active_index],
                image::ArtifactSourceErrorKind::Cancelled,
                0,
                "artifact preflight was cancelled before publication"));
        }
        if (stopped.timed_out() || ArtifactPreflightClock::now() >= deadline) {
            return std::unexpected(error_for(
                ArtifactPreflightErrorKind::TimedOut,
                active_index,
                manifest.artifacts[active_index],
                source_paths[active_index],
                image::ArtifactSourceErrorKind::TimedOut,
                0,
                "artifact preflight deadline expired before publication"));
        }
        return PreparedFleetArtifacts{
            plan, std::move(normalized_root), std::move(prepared)};
    } catch (const std::bad_alloc&) {
        const auto& artifacts = plan.manifest().artifacts;
        const ManifestArtifact fallback{};
        const auto& artifact = artifacts.empty()
            ? fallback
            : artifacts[std::min(active_index, artifacts.size() - 1U)];
        return std::unexpected(error_for(
            ArtifactPreflightErrorKind::ResourceExhausted,
            active_index,
            artifact,
            std::move(active_path),
            image::ArtifactSourceErrorKind::Io,
            0,
            "memory allocation failed during artifact preflight"));
    } catch (const std::length_error&) {
        const auto& artifacts = plan.manifest().artifacts;
        const ManifestArtifact fallback{};
        const auto& artifact = artifacts.empty()
            ? fallback
            : artifacts[std::min(active_index, artifacts.size() - 1U)];
        return std::unexpected(error_for(
            ArtifactPreflightErrorKind::ResourceExhausted,
            active_index,
            artifact,
            std::move(active_path),
            image::ArtifactSourceErrorKind::LimitExceeded,
            0,
            "container size limit was exceeded during artifact preflight"));
    } catch (const std::filesystem::filesystem_error& error) {
        const auto& artifacts = plan.manifest().artifacts;
        const ManifestArtifact fallback{};
        const auto& artifact = artifacts.empty()
            ? fallback
            : artifacts[std::min(active_index, artifacts.size() - 1U)];
        return std::unexpected(error_for(
            ArtifactPreflightErrorKind::Io,
            active_index,
            artifact,
            std::move(active_path),
            image::ArtifactSourceErrorKind::Io,
            error.code().value(),
            "filesystem failure during artifact preflight"));
    } catch (...) {
        const auto& artifacts = plan.manifest().artifacts;
        const ManifestArtifact fallback{};
        const auto& artifact = artifacts.empty()
            ? fallback
            : artifacts[std::min(active_index, artifacts.size() - 1U)];
        return std::unexpected(error_for(
            ArtifactPreflightErrorKind::UnexpectedFailure,
            active_index,
            artifact,
            std::move(active_path),
            image::ArtifactSourceErrorKind::Io,
            0,
            "unexpected failure during artifact preflight"));
    }
}

}  // namespace kairosboot::fleet
