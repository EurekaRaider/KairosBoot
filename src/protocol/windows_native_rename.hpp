// SPDX-License-Identifier: MIT
#pragma once

#include <bit>
#include <cstdint>

namespace kairosboot::protocol::detail {

using WindowsNtStatus = std::int32_t;

[[nodiscard]] consteval WindowsNtStatus windows_nt_status(
    const std::uint32_t value) noexcept {
    return std::bit_cast<WindowsNtStatus>(value);
}

inline constexpr WindowsNtStatus kStatusInvalidInfoClass =
    windows_nt_status(0xC0000003U);
inline constexpr WindowsNtStatus kStatusInvalidParameter =
    windows_nt_status(0xC000000DU);
inline constexpr WindowsNtStatus kStatusInvalidDeviceRequest =
    windows_nt_status(0xC0000010U);
inline constexpr WindowsNtStatus kStatusNotImplemented =
    windows_nt_status(0xC0000002U);
inline constexpr WindowsNtStatus kStatusNotSupported =
    windows_nt_status(0xC00000BBU);

enum class WindowsNativeRenameAction : std::uint8_t {
    Succeeded,
    RetryLegacy,
    FailClosed,
};

// FileRenameInformationEx may fall back only when the operating system or
// filesystem explicitly reports that the information class or feature is not
// implemented. Only exact STATUS_SUCCESS proves publication; pending,
// informational, namespace, access, sharing and integrity statuses are final.
[[nodiscard]] constexpr WindowsNativeRenameAction
classify_windows_native_rename_status(
    const WindowsNtStatus status) noexcept {
    if (status == 0) {
        return WindowsNativeRenameAction::Succeeded;
    }
    switch (status) {
    case kStatusInvalidInfoClass:
    case kStatusInvalidParameter:
    case kStatusNotSupported:
    case kStatusNotImplemented:
    case kStatusInvalidDeviceRequest:
        return WindowsNativeRenameAction::RetryLegacy;
    default:
        return WindowsNativeRenameAction::FailClosed;
    }
}

}  // namespace kairosboot::protocol::detail
