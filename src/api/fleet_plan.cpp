// SPDX-License-Identifier: MIT

// Context-free Fleet validate and plan C ABI. This translation unit includes
// only the Fleet manifest and job-plan layers: validation and planning never
// construct a kb_context_t, touch libusb, enumerate devices, or open artifact
// paths; only the manifest file itself is read.

#include <kairosboot/kairosboot.h>

#include "src/api/error_handle.hpp"
#include "src/fleet/job_plan.hpp"
#include "src/fleet/manifest.hpp"

#include <cstdint>
#include <exception>
#include <filesystem>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <utility>

struct kb_job_plan {
  std::unique_ptr<const kairosboot::fleet::JobPlan> plan;
};

namespace {

void clear_error(kb_error_t **error) noexcept {
  if (error != nullptr) {
    *error = nullptr;
  }
}

kb_status_t fail(kb_error_t **error, kb_status_t status, std::string message,
                 std::int32_t native_code = 0) noexcept {
  if (error != nullptr) {
    try {
      *error = new kb_error{
          .status = status,
          .message = std::move(message),
          .device_identifier = std::string{},
          .native_code = native_code,
          .transfer_state = KB_TRANSFER_NOT_SENT,
          .device_message = std::string{},
          .command_messages = {},
          .inbound_expected = std::nullopt,
          .inbound_transferred = 0,
          .inbound_transfer_state = KB_TRANSFER_NOT_SENT,
          .session_poisoned = false,
      };
    } catch (...) {
      *error = nullptr;
    }
  }
  return status;
}

kb_status_t manifest_failure_status(
    const kairosboot::fleet::ManifestErrorKind kind) noexcept {
  using kairosboot::fleet::ManifestErrorKind;
  switch (kind) {
    case ManifestErrorKind::NotFound:
    case ManifestErrorKind::Io:
      return KB_E_IO;
    case ManifestErrorKind::Cancelled:
      return KB_E_CANCELLED;
    case ManifestErrorKind::TimedOut:
      return KB_E_TIMEOUT;
    case ManifestErrorKind::ResourceExhausted:
      return KB_E_OUT_OF_MEMORY;
    case ManifestErrorKind::UnexpectedFailure:
      return KB_E_INTERNAL;
    case ManifestErrorKind::InvalidArgument:
    case ManifestErrorKind::UnsafePath:
    case ManifestErrorKind::TooLarge:
    case ManifestErrorKind::InvalidUtf8:
    case ManifestErrorKind::Syntax:
    case ManifestErrorKind::MultipleDocuments:
    case ManifestErrorKind::UnsupportedTag:
    case ManifestErrorKind::AliasNotAllowed:
    case ManifestErrorKind::DuplicateKey:
    case ManifestErrorKind::NonScalarKey:
    case ManifestErrorKind::UnknownField:
    case ManifestErrorKind::MissingField:
    case ManifestErrorKind::TypeMismatch:
    case ManifestErrorKind::LimitExceeded:
    case ManifestErrorKind::InvalidValue:
    case ManifestErrorKind::DuplicateValue:
    case ManifestErrorKind::UnknownArtifact:
      return KB_E_INVALID_ARGUMENT;
  }
  return KB_E_INTERNAL;
}

std::string manifest_failure_message(
    const char *file_path, const kairosboot::fleet::ManifestError &error) {
  std::string message{file_path};
  if (error.location.has_value()) {
    message += ':';
    message += std::to_string(error.location->line);
    message += ':';
    message += std::to_string(error.location->column);
  }
  message += ": ";
  message += error.message;
  if (!error.path.empty() && error.path != "$") {
    message += " at ";
    message += error.path;
  }
  return message;
}

kb_status_t report_manifest_failure(kb_error_t **error, const char *file_path,
                                    const kairosboot::fleet::ManifestError
                                        &failure) noexcept {
  return fail(error, manifest_failure_status(failure.kind),
              manifest_failure_message(file_path, failure),
              static_cast<std::int32_t>(failure.native_code));
}

kb_status_t report_plan_failure(
    kb_error_t **error,
    const kairosboot::fleet::JobPlanError &failure) noexcept {
  using kairosboot::fleet::JobPlanErrorKind;
  kb_status_t status = KB_E_INTERNAL;
  const char *message = "fleet job plan failed unexpectedly";
  switch (failure.kind) {
    case JobPlanErrorKind::InvalidManifest:
      status = KB_E_INVALID_ARGUMENT;
      message = "fleet job plan rejected an unsafe manifest value";
      break;
    case JobPlanErrorKind::InvalidUtf8:
      status = KB_E_INVALID_ARGUMENT;
      message = "fleet job plan rejected invalid UTF-8 in the manifest";
      break;
    case JobPlanErrorKind::IntegerOutOfRange:
      status = KB_E_INVALID_ARGUMENT;
      message =
          "fleet job plan rejected an integer outside the canonical JSON "
          "safe range";
      break;
    case JobPlanErrorKind::ResourceExhausted:
      status = KB_E_OUT_OF_MEMORY;
      message = "fleet job plan exhausted memory";
      break;
    case JobPlanErrorKind::OutputTooLarge:
      status = KB_E_INVALID_ARGUMENT;
      message = "fleet job plan output exceeded the supported size";
      break;
    case JobPlanErrorKind::UnexpectedFailure:
      break;
  }
  return fail(error, status, message);
}

}  // namespace

extern "C" {

kb_status_t KB_CALL kb_validate_job_file(const char *file_path,
                                         kb_error_t **error) {
  clear_error(error);
  if (file_path == nullptr) {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "fleet manifest path must not be null");
  }
  try {
    const auto manifest = kairosboot::fleet::load_fleet_manifest_file(
        std::filesystem::path{file_path});
    if (!manifest.has_value()) {
      return report_manifest_failure(error, file_path, manifest.error());
    }
    return KB_OK;
  } catch (const std::bad_alloc &) {
    return fail(error, KB_E_OUT_OF_MEMORY,
                "fleet manifest validation exhausted memory");
  } catch (...) {
    return fail(error, KB_E_INTERNAL,
                "fleet manifest validation failed unexpectedly");
  }
}

kb_status_t KB_CALL kb_plan_job_file(const char *file_path,
                                     kb_job_plan_t **plan,
                                     kb_error_t **error) {
  clear_error(error);
  if (plan == nullptr) {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "fleet job plan output pointer must not be null");
  }
  *plan = nullptr;
  if (file_path == nullptr) {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "fleet manifest path must not be null");
  }
  try {
    auto manifest = kairosboot::fleet::load_fleet_manifest_file(
        std::filesystem::path{file_path});
    if (!manifest.has_value()) {
      return report_manifest_failure(error, file_path, manifest.error());
    }
    auto planned = kairosboot::fleet::make_job_plan(
        std::move(manifest).value());
    if (!planned.has_value()) {
      return report_plan_failure(error, planned.error());
    }
    auto handle = std::make_unique<kb_job_plan>();
    handle->plan = std::make_unique<const kairosboot::fleet::JobPlan>(
        std::move(planned).value());
    *plan = handle.release();
    return KB_OK;
  } catch (const std::bad_alloc &) {
    return fail(error, KB_E_OUT_OF_MEMORY,
                "fleet job planning exhausted memory");
  } catch (...) {
    return fail(error, KB_E_INTERNAL,
                "fleet job planning failed unexpectedly");
  }
}

const char *KB_CALL kb_job_plan_canonical_json(const kb_job_plan_t *plan,
                                               size_t *size) {
  if (size != nullptr) {
    *size = 0U;
  }
  if (plan == nullptr || plan->plan == nullptr) {
    return nullptr;
  }
  const std::string_view json = plan->plan->canonical_json();
  if (size != nullptr) {
    *size = json.size();
  }
  return json.data();
}

const char *KB_CALL kb_job_plan_sha256_hex(const kb_job_plan_t *plan) {
  if (plan == nullptr || plan->plan == nullptr) {
    return "";
  }
  return plan->plan->sha256_hex().data();
}

void KB_CALL kb_job_plan_release(kb_job_plan_t *plan) { delete plan; }

}  // extern "C"
