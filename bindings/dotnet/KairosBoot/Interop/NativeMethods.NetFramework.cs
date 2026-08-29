#if NET48
using System;
using System.Runtime.InteropServices;

namespace KairosBoot.Interop;

internal static partial class NativeMethods
{
    [DllImport(LibraryName, EntryPoint = "kb_context_options_init_sized", CallingConvention = CallingConvention.Cdecl)]
    internal static extern void ContextOptionsInitSized(ref NativeContextOptions options, uint structSize);

    [DllImport(LibraryName, EntryPoint = "kb_flash_options_init_sized", CallingConvention = CallingConvention.Cdecl)]
    internal static extern void FlashOptionsInitSized(ref NativeFlashOptions options, uint structSize);

    [DllImport(LibraryName, EntryPoint = "kb_legacy_boot_options_init_sized", CallingConvention = CallingConvention.Cdecl)]
    internal static extern void LegacyBootOptionsInitSized(ref NativeLegacyBootOptions options, uint structSize);

    [DllImport(LibraryName, EntryPoint = "kb_update_options_init_sized", CallingConvention = CallingConvention.Cdecl)]
    internal static extern void UpdateOptionsInitSized(ref NativeUpdateOptions options, uint structSize);

    [DllImport(LibraryName, EntryPoint = "kb_command_options_init_sized", CallingConvention = CallingConvention.Cdecl)]
    internal static extern void CommandOptionsInitSized(ref NativeCommandOptions options, uint structSize);

    [DllImport(LibraryName, EntryPoint = "kb_version_init_sized", CallingConvention = CallingConvention.Cdecl)]
    internal static extern void VersionInitSized(ref NativeVersion version, uint structSize);

    [DllImport(LibraryName, EntryPoint = "kb_get_version", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int GetVersion(ref NativeVersion version);

    [DllImport(LibraryName, EntryPoint = "kb_status_string", CallingConvention = CallingConvention.Cdecl)]
    internal static extern IntPtr StatusString(int status);

    [DllImport(LibraryName, EntryPoint = "kb_context_create", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int ContextCreate(IntPtr options, out IntPtr context, out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_context_create", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int ContextCreateWithOptions(
        ref NativeContextOptions options,
        out IntPtr context,
        out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_context_release", CallingConvention = CallingConvention.Cdecl)]
    internal static extern void ContextRelease(IntPtr context);

    [DllImport(LibraryName, EntryPoint = "kb_device_open", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int DeviceOpen(
        ContextSafeHandle context,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string? selector,
        out IntPtr device,
        out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_device_identifier", CallingConvention = CallingConvention.Cdecl)]
    internal static extern IntPtr DeviceIdentifier(DeviceSafeHandle device);

    [DllImport(LibraryName, EntryPoint = "kb_device_serial", CallingConvention = CallingConvention.Cdecl)]
    internal static extern IntPtr DeviceSerial(DeviceSafeHandle device);

    [DllImport(LibraryName, EntryPoint = "kb_device_usb_path", CallingConvention = CallingConvention.Cdecl)]
    internal static extern IntPtr DeviceUsbPath(DeviceSafeHandle device);

    [DllImport(LibraryName, EntryPoint = "kb_device_release", CallingConvention = CallingConvention.Cdecl)]
    internal static extern void DeviceRelease(IntPtr device);

    [DllImport(LibraryName, EntryPoint = "kb_enumerate_devices", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int EnumerateDevices(
        ContextSafeHandle context,
        out IntPtr devices,
        out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_device_list_count", CallingConvention = CallingConvention.Cdecl)]
    internal static extern UIntPtr DeviceListCount(DeviceListSafeHandle devices);

    [DllImport(LibraryName, EntryPoint = "kb_device_list_serial", CallingConvention = CallingConvention.Cdecl)]
    internal static extern IntPtr DeviceListSerial(DeviceListSafeHandle devices, UIntPtr index);

    [DllImport(LibraryName, EntryPoint = "kb_device_list_usb_path", CallingConvention = CallingConvention.Cdecl)]
    internal static extern IntPtr DeviceListUsbPath(DeviceListSafeHandle devices, UIntPtr index);

    [DllImport(LibraryName, EntryPoint = "kb_device_list_product", CallingConvention = CallingConvention.Cdecl)]
    internal static extern IntPtr DeviceListProduct(DeviceListSafeHandle devices, UIntPtr index);

    [DllImport(LibraryName, EntryPoint = "kb_device_list_release", CallingConvention = CallingConvention.Cdecl)]
    internal static extern void DeviceListRelease(IntPtr devices);

    [DllImport(LibraryName, EntryPoint = "kb_flash_file_async", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int FlashFileAsync(
        DeviceSafeHandle device,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string partition,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string filePath,
        ref NativeFlashOptions options,
        out IntPtr operation,
        out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_flash_file", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int FlashFile(
        DeviceSafeHandle device,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string partition,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string filePath,
        ref NativeFlashOptions options,
        out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_flash_vendor_boot_ramdisk_async", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int FlashVendorBootRamdiskAsync(
        DeviceSafeHandle device,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string partition,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string? ramdiskName,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string ramdiskPath,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string? dtbPath,
        ref NativeFlashOptions options,
        out IntPtr operation,
        out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_flash_vendor_boot_ramdisk", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int FlashVendorBootRamdisk(
        DeviceSafeHandle device,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string partition,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string? ramdiskName,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string ramdiskPath,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string? dtbPath,
        ref NativeFlashOptions options,
        out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_flash_raw_async", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int FlashRawAsync(
        DeviceSafeHandle device,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string partition,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string kernelPath,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string? ramdiskPath,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string? secondStagePath,
        ref NativeFlashOptions options,
        out IntPtr operation,
        out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_flash_raw", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int FlashRaw(
        DeviceSafeHandle device,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string partition,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string kernelPath,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string? ramdiskPath,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string? secondStagePath,
        ref NativeFlashOptions options,
        out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_flash_raw_with_boot_options_async", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int FlashRawWithBootOptionsAsync(
        DeviceSafeHandle device,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string partition,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string kernelPath,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string? ramdiskPath,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string? secondStagePath,
        ref NativeLegacyBootOptions legacyOptions,
        ref NativeFlashOptions options,
        out IntPtr operation,
        out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_flash_raw_with_boot_options", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int FlashRawWithBootOptions(
        DeviceSafeHandle device,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string partition,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string kernelPath,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string? ramdiskPath,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string? secondStagePath,
        ref NativeLegacyBootOptions legacyOptions,
        ref NativeFlashOptions options,
        out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_boot_raw_async", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int BootRawAsync(
        DeviceSafeHandle device,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string kernelPath,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string? ramdiskPath,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string? secondStagePath,
        ref NativeLegacyBootOptions legacyOptions,
        ref NativeFlashOptions options,
        out IntPtr operation,
        out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_boot_raw", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int BootRaw(
        DeviceSafeHandle device,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string kernelPath,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string? ramdiskPath,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string? secondStagePath,
        ref NativeLegacyBootOptions legacyOptions,
        ref NativeFlashOptions options,
        out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_boot_file_async", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int BootFileAsync(
        DeviceSafeHandle device,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string filePath,
        ref NativeFlashOptions options,
        out IntPtr operation,
        out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_boot_file", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int BootFile(
        DeviceSafeHandle device,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string filePath,
        ref NativeFlashOptions options,
        out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_signature_file_async", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int SignatureFileAsync(
        DeviceSafeHandle device,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string filePath,
        ref NativeCommandOptions options,
        out IntPtr operation,
        out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_signature_file", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int SignatureFile(
        DeviceSafeHandle device,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string filePath,
        ref NativeCommandOptions options,
        out IntPtr result,
        out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_update_package_async", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int UpdatePackageAsync(
        DeviceSafeHandle device,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string packagePath,
        ref NativeUpdateOptions options,
        out IntPtr operation,
        out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_update_package", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int UpdatePackage(
        DeviceSafeHandle device,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string packagePath,
        ref NativeUpdateOptions options,
        out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_wipe_super_async", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int WipeSuperAsync(
        DeviceSafeHandle device,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string? superEmptyImage,
        ref NativeUpdateOptions options,
        out IntPtr operation,
        out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_wipe_super", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int WipeSuper(
        DeviceSafeHandle device,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string? superEmptyImage,
        ref NativeUpdateOptions options,
        out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_getvar_async", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int GetVarAsync(
        DeviceSafeHandle device,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string variable,
        ref NativeCommandOptions options,
        out IntPtr operation,
        out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_getvar", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int GetVar(
        DeviceSafeHandle device,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string variable,
        ref NativeCommandOptions options,
        out IntPtr result,
        out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_erase_async", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int EraseAsync(
        DeviceSafeHandle device,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string partition,
        ref NativeCommandOptions options,
        out IntPtr operation,
        out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_erase", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int Erase(
        DeviceSafeHandle device,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string partition,
        ref NativeCommandOptions options,
        out IntPtr result,
        out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_format_partition_async", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int FormatPartitionAsync(
        DeviceSafeHandle device,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string partition,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string? filesystemType,
        ulong partitionSize,
        ref NativeFlashOptions options,
        out IntPtr operation,
        out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_set_active_async", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int SetActiveAsync(
        DeviceSafeHandle device,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string slot,
        ref NativeCommandOptions options,
        out IntPtr operation,
        out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_set_active", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int SetActive(
        DeviceSafeHandle device,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string slot,
        ref NativeCommandOptions options,
        out IntPtr result,
        out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_flashing_async", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int FlashingAsync(
        DeviceSafeHandle device,
        int command,
        ref NativeCommandOptions options,
        out IntPtr operation,
        out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_flashing", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int Flashing(
        DeviceSafeHandle device,
        int command,
        ref NativeCommandOptions options,
        out IntPtr result,
        out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_gsi_async", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int GsiAsync(
        DeviceSafeHandle device,
        int command,
        ref NativeCommandOptions options,
        out IntPtr operation,
        out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_gsi", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int Gsi(
        DeviceSafeHandle device,
        int command,
        ref NativeCommandOptions options,
        out IntPtr result,
        out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_snapshot_update_async", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int SnapshotUpdateAsync(
        DeviceSafeHandle device,
        int command,
        ref NativeCommandOptions options,
        out IntPtr operation,
        out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_snapshot_update", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int SnapshotUpdate(
        DeviceSafeHandle device,
        int command,
        ref NativeCommandOptions options,
        out IntPtr result,
        out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_create_logical_partition_async", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int CreateLogicalPartitionAsync(
        DeviceSafeHandle device,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string partitionName,
        ulong size,
        ref NativeCommandOptions options,
        out IntPtr operation,
        out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_create_logical_partition", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int CreateLogicalPartition(
        DeviceSafeHandle device,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string partitionName,
        ulong size,
        ref NativeCommandOptions options,
        out IntPtr result,
        out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_delete_logical_partition_async", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int DeleteLogicalPartitionAsync(
        DeviceSafeHandle device,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string partitionName,
        ref NativeCommandOptions options,
        out IntPtr operation,
        out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_delete_logical_partition", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int DeleteLogicalPartition(
        DeviceSafeHandle device,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string partitionName,
        ref NativeCommandOptions options,
        out IntPtr result,
        out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_resize_logical_partition_async", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int ResizeLogicalPartitionAsync(
        DeviceSafeHandle device,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string partitionName,
        ulong size,
        ref NativeCommandOptions options,
        out IntPtr operation,
        out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_resize_logical_partition", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int ResizeLogicalPartition(
        DeviceSafeHandle device,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string partitionName,
        ulong size,
        ref NativeCommandOptions options,
        out IntPtr result,
        out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_reboot_async", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int RebootAsync(
        DeviceSafeHandle device,
        int target,
        ref NativeCommandOptions options,
        out IntPtr operation,
        out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_reboot", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int Reboot(
        DeviceSafeHandle device,
        int target,
        ref NativeCommandOptions options,
        out IntPtr result,
        out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_continue_boot_async", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int ContinueBootAsync(
        DeviceSafeHandle device,
        ref NativeCommandOptions options,
        out IntPtr operation,
        out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_continue_boot", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int ContinueBoot(
        DeviceSafeHandle device,
        ref NativeCommandOptions options,
        out IntPtr result,
        out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_oem_async", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int OemAsync(
        DeviceSafeHandle device,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string commandSuffix,
        ref NativeCommandOptions options,
        out IntPtr operation,
        out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_oem", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int Oem(
        DeviceSafeHandle device,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string commandSuffix,
        ref NativeCommandOptions options,
        out IntPtr result,
        out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_raw_command_async", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int RawCommandAsync(
        DeviceSafeHandle device,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string command,
        ref NativeCommandOptions options,
        out IntPtr operation,
        out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_raw_command", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int RawCommand(
        DeviceSafeHandle device,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string command,
        ref NativeCommandOptions options,
        out IntPtr result,
        out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_boot_async", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int BootAsync(
        DeviceSafeHandle device,
        ref NativeCommandOptions options,
        out IntPtr operation,
        out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_boot", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int Boot(
        DeviceSafeHandle device,
        ref NativeCommandOptions options,
        out IntPtr result,
        out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_stage_async", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int StageAsync(
        DeviceSafeHandle device,
        IntPtr data,
        UIntPtr dataSize,
        ref NativeCommandOptions options,
        out IntPtr operation,
        out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_stage", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int Stage(
        DeviceSafeHandle device,
        IntPtr data,
        UIntPtr dataSize,
        ref NativeCommandOptions options,
        out IntPtr result,
        out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_upload_async", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int UploadAsync(
        DeviceSafeHandle device,
        ref NativeCommandOptions options,
        out IntPtr operation,
        out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_upload", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int Upload(
        DeviceSafeHandle device,
        ref NativeCommandOptions options,
        out IntPtr result,
        out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_fetch_async", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int FetchAsync(
        DeviceSafeHandle device,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string partition,
        ulong offset,
        ulong size,
        ref NativeCommandOptions options,
        out IntPtr operation,
        out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_fetch", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int Fetch(
        DeviceSafeHandle device,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string partition,
        ulong offset,
        ulong size,
        ref NativeCommandOptions options,
        out IntPtr result,
        out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_upload_file_async", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int UploadFileAsync(
        DeviceSafeHandle device,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string outputPath,
        ref NativeCommandOptions options,
        out IntPtr operation,
        out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_get_staged_file_async", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int GetStagedFileAsync(
        DeviceSafeHandle device,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string outputPath,
        ref NativeCommandOptions options,
        out IntPtr operation,
        out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_fetch_file_async", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int FetchFileAsync(
        DeviceSafeHandle device,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string partition,
        ulong offset,
        ulong size,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string outputPath,
        ref NativeCommandOptions options,
        out IntPtr operation,
        out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_operation_wait", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int OperationWait(OperationSafeHandle operation, uint timeoutMilliseconds);

    [DllImport(LibraryName, EntryPoint = "kb_operation_cancel", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int OperationCancel(OperationSafeHandle operation);

    [DllImport(LibraryName, EntryPoint = "kb_operation_state", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int OperationState(OperationSafeHandle operation);

    [DllImport(LibraryName, EntryPoint = "kb_operation_error", CallingConvention = CallingConvention.Cdecl)]
    internal static extern IntPtr OperationError(OperationSafeHandle operation);

    [DllImport(LibraryName, EntryPoint = "kb_operation_command_result", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int OperationCommandResult(
        OperationSafeHandle operation,
        out IntPtr result,
        out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_operation_release", CallingConvention = CallingConvention.Cdecl)]
    internal static extern void OperationRelease(IntPtr operation);

    [DllImport(LibraryName, EntryPoint = "kb_command_result_terminal_payload", CallingConvention = CallingConvention.Cdecl)]
    internal static extern IntPtr CommandResultTerminalPayload(
        CommandResultSafeHandle result,
        out UIntPtr size);

    [DllImport(LibraryName, EntryPoint = "kb_command_result_message_count", CallingConvention = CallingConvention.Cdecl)]
    internal static extern UIntPtr CommandResultMessageCount(CommandResultSafeHandle result);

    [DllImport(LibraryName, EntryPoint = "kb_command_result_message_kind", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int CommandResultMessageKind(
        CommandResultSafeHandle result,
        UIntPtr index);

    [DllImport(LibraryName, EntryPoint = "kb_command_result_message_payload", CallingConvention = CallingConvention.Cdecl)]
    internal static extern IntPtr CommandResultMessagePayload(
        CommandResultSafeHandle result,
        UIntPtr index,
        out UIntPtr size);

    [DllImport(LibraryName, EntryPoint = "kb_command_result_data", CallingConvention = CallingConvention.Cdecl)]
    internal static extern IntPtr CommandResultData(CommandResultSafeHandle result, out UIntPtr size);

    [DllImport(LibraryName, EntryPoint = "kb_command_result_output_path", CallingConvention = CallingConvention.Cdecl)]
    internal static extern IntPtr CommandResultOutputPath(CommandResultSafeHandle result);

    [DllImport(LibraryName, EntryPoint = "kb_command_result_received_bytes", CallingConvention = CallingConvention.Cdecl)]
    internal static extern ulong CommandResultReceivedBytes(CommandResultSafeHandle result);

    [DllImport(LibraryName, EntryPoint = "kb_command_result_device_identifier", CallingConvention = CallingConvention.Cdecl)]
    internal static extern IntPtr CommandResultDeviceIdentifier(CommandResultSafeHandle result);

    [DllImport(LibraryName, EntryPoint = "kb_command_result_release", CallingConvention = CallingConvention.Cdecl)]
    internal static extern void CommandResultRelease(IntPtr result);

    [DllImport(LibraryName, EntryPoint = "kb_error_status", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int ErrorStatus(IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_error_message", CallingConvention = CallingConvention.Cdecl)]
    internal static extern IntPtr ErrorMessage(IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_error_device_identifier", CallingConvention = CallingConvention.Cdecl)]
    internal static extern IntPtr ErrorDeviceIdentifier(IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_error_native_code", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int ErrorNativeCode(IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_error_transfer_state", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int ErrorTransferState(IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_error_device_message", CallingConvention = CallingConvention.Cdecl)]
    internal static extern IntPtr ErrorDeviceMessage(IntPtr error, out UIntPtr size);

    [DllImport(LibraryName, EntryPoint = "kb_error_command_message_count", CallingConvention = CallingConvention.Cdecl)]
    internal static extern UIntPtr ErrorCommandMessageCount(IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_error_command_message_kind", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int ErrorCommandMessageKind(IntPtr error, UIntPtr index);

    [DllImport(LibraryName, EntryPoint = "kb_error_command_message_payload", CallingConvention = CallingConvention.Cdecl)]
    internal static extern IntPtr ErrorCommandMessagePayload(
        IntPtr error,
        UIntPtr index,
        out UIntPtr size);

    [DllImport(LibraryName, EntryPoint = "kb_error_inbound_expected_bytes", CallingConvention = CallingConvention.Cdecl)]
    internal static extern ulong ErrorInboundExpectedBytes(IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_error_inbound_transferred_bytes", CallingConvention = CallingConvention.Cdecl)]
    internal static extern ulong ErrorInboundTransferredBytes(IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_error_inbound_transfer_state", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int ErrorInboundTransferState(IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_error_session_poisoned", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int ErrorSessionPoisoned(IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_error_release", CallingConvention = CallingConvention.Cdecl)]
    internal static extern void ErrorRelease(IntPtr error);
}
#endif
