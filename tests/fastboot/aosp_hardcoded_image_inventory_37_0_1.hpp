// SPDX-License-Identifier: MIT
#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace kairosboot::test::aosp_37_0_1 {

// Source-derived oracle for Platform-Tools 37.0.1:
//   platform/system/core/fastboot/fastboot.cpp
//   commit a3b721a32242006b59cb12bd62c9133632af3a2d
//   blob   1c52da2382ad46759485c2ff096dd660a9c3c999
// Keep this fixture independent from the production fallback table. It may be
// updated only as part of an explicitly reviewed compatibility-baseline change.
enum class ImageType : std::uint8_t {
    BootCritical,
    Normal,
    Extra,
};

struct Image final {
    std::string_view nickname;
    std::string_view image_name;
    std::string_view signature_name;
    std::string_view partition;
    bool optional_if_missing;
    ImageType type;
};

inline constexpr std::array kImages{
    Image{"boot", "boot.img", "boot.sig", "boot", false,
          ImageType::BootCritical},
    Image{"bootloader", "bootloader.img", "", "bootloader", true,
          ImageType::Extra},
    Image{"init_boot", "init_boot.img", "init_boot.sig", "init_boot", true,
          ImageType::BootCritical},
    Image{"", "boot_other.img", "boot.sig", "boot", true,
          ImageType::Normal},
    Image{"cache", "cache.img", "cache.sig", "cache", true,
          ImageType::Extra},
    Image{"dtbo", "dtbo.img", "dtbo.sig", "dtbo", true,
          ImageType::BootCritical},
    Image{"dts", "dt.img", "dt.sig", "dts", true,
          ImageType::BootCritical},
    Image{"odm", "odm.img", "odm.sig", "odm", true, ImageType::Normal},
    Image{"odm_dlkm", "odm_dlkm.img", "odm_dlkm.sig", "odm_dlkm", true,
          ImageType::Normal},
    Image{"product", "product.img", "product.sig", "product", true,
          ImageType::Normal},
    Image{"pvmfw", "pvmfw.img", "pvmfw.sig", "pvmfw", true,
          ImageType::BootCritical},
    Image{"radio", "radio.img", "", "radio", true, ImageType::Extra},
    Image{"recovery", "recovery.img", "recovery.sig", "recovery", true,
          ImageType::BootCritical},
    Image{"super", "super.img", "super.sig", "super", true,
          ImageType::Extra},
    Image{"system", "system.img", "system.sig", "system", false,
          ImageType::Normal},
    Image{"system_dlkm", "system_dlkm.img", "system_dlkm.sig", "system_dlkm",
          true, ImageType::Normal},
    Image{"system_ext", "system_ext.img", "system_ext.sig", "system_ext", true,
          ImageType::Normal},
    Image{"", "system_other.img", "system.sig", "system", true,
          ImageType::Normal},
    Image{"userdata", "userdata.img", "userdata.sig", "userdata", true,
          ImageType::Extra},
    Image{"vbmeta", "vbmeta.img", "vbmeta.sig", "vbmeta", true,
          ImageType::BootCritical},
    Image{"vbmeta_system", "vbmeta_system.img", "vbmeta_system.sig",
          "vbmeta_system", true, ImageType::BootCritical},
    Image{"vbmeta_vendor", "vbmeta_vendor.img", "vbmeta_vendor.sig",
          "vbmeta_vendor", true, ImageType::BootCritical},
    Image{"vendor", "vendor.img", "vendor.sig", "vendor", true,
          ImageType::Normal},
    Image{"vendor_boot", "vendor_boot.img", "vendor_boot.sig", "vendor_boot",
          true, ImageType::BootCritical},
    Image{"vendor_dlkm", "vendor_dlkm.img", "vendor_dlkm.sig", "vendor_dlkm",
          true, ImageType::Normal},
    Image{"vendor_kernel_boot", "vendor_kernel_boot.img",
          "vendor_kernel_boot.sig", "vendor_kernel_boot", true,
          ImageType::BootCritical},
    Image{"", "vendor_other.img", "vendor.sig", "vendor", true,
          ImageType::Normal},
};

}  // namespace kairosboot::test::aosp_37_0_1
