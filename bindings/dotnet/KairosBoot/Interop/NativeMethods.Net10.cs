#if NET10_0_OR_GREATER
using System;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace KairosBoot.Interop;

internal static partial class NativeMethods
{
    [LibraryImport(LibraryName, EntryPoint = "kb_context_options_init")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial void ContextOptionsInit(ref NativeContextOptions options);

    [LibraryImport(LibraryName, EntryPoint = "kb_flash_options_init")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial void FlashOptionsInit(ref NativeFlashOptions options);

    [LibraryImport(LibraryName, EntryPoint = "kb_update_options_init")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial void UpdateOptionsInit(ref NativeUpdateOptions options);

    [LibraryImport(LibraryName, EntryPoint = "kb_command_options_init")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial void CommandOptionsInit(ref NativeCommandOptions options);

    [LibraryImport(LibraryName, EntryPoint = "kb_job_options_init")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial void JobOptionsInit(ref NativeJobOptions options);

    [LibraryImport(LibraryName, EntryPoint = "kb_version_init")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial void VersionInit(ref NativeVersion version);

    [LibraryImport(LibraryName, EntryPoint = "kb_get_version")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial int GetVersion(ref NativeVersion version);

    [LibraryImport(LibraryName, EntryPoint = "kb_status_string")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial IntPtr StatusString(int status);

    [LibraryImport(LibraryName, EntryPoint = "kb_context_create")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial int ContextCreate(IntPtr options, out IntPtr context, out IntPtr error);

    [LibraryImport(LibraryName, EntryPoint = "kb_context_release")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial void ContextRelease(IntPtr context);

    [LibraryImport(LibraryName, EntryPoint = "kb_enumerate_devices")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial int EnumerateDevices(
        ContextSafeHandle context,
        out IntPtr devices,
        out IntPtr error);

    [LibraryImport(LibraryName, EntryPoint = "kb_device_list_count")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial UIntPtr DeviceListCount(DeviceListSafeHandle devices);

    [LibraryImport(LibraryName, EntryPoint = "kb_device_list_serial")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial IntPtr DeviceListSerial(DeviceListSafeHandle devices, UIntPtr index);

    [LibraryImport(LibraryName, EntryPoint = "kb_device_list_usb_path")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial IntPtr DeviceListUsbPath(DeviceListSafeHandle devices, UIntPtr index);

    [LibraryImport(LibraryName, EntryPoint = "kb_device_list_product")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial IntPtr DeviceListProduct(DeviceListSafeHandle devices, UIntPtr index);

    [LibraryImport(LibraryName, EntryPoint = "kb_device_list_release")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial void DeviceListRelease(IntPtr devices);

    [LibraryImport(LibraryName, EntryPoint = "kb_flash_file_async", StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial int FlashFileAsync(
        ContextSafeHandle context,
        string? serial,
        string partition,
        string filePath,
        ref NativeFlashOptions options,
        out IntPtr operation,
        out IntPtr error);

    [LibraryImport(LibraryName, EntryPoint = "kb_flash_file", StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial int FlashFile(
        ContextSafeHandle context,
        string? serial,
        string partition,
        string filePath,
        ref NativeFlashOptions options,
        out IntPtr error);

    [LibraryImport(LibraryName, EntryPoint = "kb_flash_raw_async", StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial int FlashRawAsync(
        ContextSafeHandle context,
        string? deviceSelector,
        string partition,
        string kernelPath,
        string? ramdiskPath,
        string? secondStagePath,
        ref NativeFlashOptions options,
        out IntPtr operation,
        out IntPtr error);

    [LibraryImport(LibraryName, EntryPoint = "kb_flash_raw", StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial int FlashRaw(
        ContextSafeHandle context,
        string? deviceSelector,
        string partition,
        string kernelPath,
        string? ramdiskPath,
        string? secondStagePath,
        ref NativeFlashOptions options,
        out IntPtr error);

    [LibraryImport(LibraryName, EntryPoint = "kb_boot_file_async", StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial int BootFileAsync(
        ContextSafeHandle context,
        string? deviceSelector,
        string filePath,
        ref NativeFlashOptions options,
        out IntPtr operation,
        out IntPtr error);

    [LibraryImport(LibraryName, EntryPoint = "kb_boot_file", StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial int BootFile(
        ContextSafeHandle context,
        string? deviceSelector,
        string filePath,
        ref NativeFlashOptions options,
        out IntPtr error);

    [LibraryImport(LibraryName, EntryPoint = "kb_signature_file_async", StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial int SignatureFileAsync(
        ContextSafeHandle context,
        string? deviceSelector,
        string filePath,
        ref NativeCommandOptions options,
        out IntPtr operation,
        out IntPtr error);

    [LibraryImport(LibraryName, EntryPoint = "kb_signature_file", StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial int SignatureFile(
        ContextSafeHandle context,
        string? deviceSelector,
        string filePath,
        ref NativeCommandOptions options,
        out IntPtr result,
        out IntPtr error);

    [LibraryImport(LibraryName, EntryPoint = "kb_update_package_async", StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial int UpdatePackageAsync(
        ContextSafeHandle context,
        string? deviceSelector,
        string packagePath,
        ref NativeUpdateOptions options,
        out IntPtr operation,
        out IntPtr error);

    [LibraryImport(LibraryName, EntryPoint = "kb_update_package", StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial int UpdatePackage(
        ContextSafeHandle context,
        string? deviceSelector,
        string packagePath,
        ref NativeUpdateOptions options,
        out IntPtr error);

    [LibraryImport(LibraryName, EntryPoint = "kb_wipe_super_async", StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial int WipeSuperAsync(
        ContextSafeHandle context,
        string? deviceSelector,
        string? superEmptyImage,
        ref NativeUpdateOptions options,
        out IntPtr operation,
        out IntPtr error);

    [LibraryImport(LibraryName, EntryPoint = "kb_wipe_super", StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial int WipeSuper(
        ContextSafeHandle context,
        string? deviceSelector,
        string? superEmptyImage,
        ref NativeUpdateOptions options,
        out IntPtr error);

    [LibraryImport(LibraryName, EntryPoint = "kb_getvar_async", StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial int GetVarAsync(
        ContextSafeHandle context,
        string? selector,
        string variable,
        ref NativeCommandOptions options,
        out IntPtr operation,
        out IntPtr error);

    [LibraryImport(LibraryName, EntryPoint = "kb_getvar", StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial int GetVar(
        ContextSafeHandle context,
        string? selector,
        string variable,
        ref NativeCommandOptions options,
        out IntPtr result,
        out IntPtr error);

    [LibraryImport(LibraryName, EntryPoint = "kb_erase_async", StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial int EraseAsync(
        ContextSafeHandle context,
        string? selector,
        string partition,
        ref NativeCommandOptions options,
        out IntPtr operation,
        out IntPtr error);

    [LibraryImport(LibraryName, EntryPoint = "kb_erase", StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial int Erase(
        ContextSafeHandle context,
        string? selector,
        string partition,
        ref NativeCommandOptions options,
        out IntPtr result,
        out IntPtr error);

    [LibraryImport(LibraryName, EntryPoint = "kb_format_partition_async", StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial int FormatPartitionAsync(
        ContextSafeHandle context,
        string? selector,
        string partition,
        string? filesystemType,
        ulong partitionSize,
        ref NativeFlashOptions options,
        out IntPtr operation,
        out IntPtr error);

    [LibraryImport(LibraryName, EntryPoint = "kb_set_active_async", StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial int SetActiveAsync(
        ContextSafeHandle context,
        string? selector,
        string slot,
        ref NativeCommandOptions options,
        out IntPtr operation,
        out IntPtr error);

    [LibraryImport(LibraryName, EntryPoint = "kb_set_active", StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial int SetActive(
        ContextSafeHandle context,
        string? selector,
        string slot,
        ref NativeCommandOptions options,
        out IntPtr result,
        out IntPtr error);

    [LibraryImport(LibraryName, EntryPoint = "kb_flashing_async", StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial int FlashingAsync(
        ContextSafeHandle context,
        string? selector,
        int command,
        ref NativeCommandOptions options,
        out IntPtr operation,
        out IntPtr error);

    [LibraryImport(LibraryName, EntryPoint = "kb_flashing", StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial int Flashing(
        ContextSafeHandle context,
        string? selector,
        int command,
        ref NativeCommandOptions options,
        out IntPtr result,
        out IntPtr error);

    [LibraryImport(LibraryName, EntryPoint = "kb_gsi_async", StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial int GsiAsync(
        ContextSafeHandle context,
        string? selector,
        int command,
        ref NativeCommandOptions options,
        out IntPtr operation,
        out IntPtr error);

    [LibraryImport(LibraryName, EntryPoint = "kb_gsi", StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial int Gsi(
        ContextSafeHandle context,
        string? selector,
        int command,
        ref NativeCommandOptions options,
        out IntPtr result,
        out IntPtr error);

    [LibraryImport(LibraryName, EntryPoint = "kb_snapshot_update_async", StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial int SnapshotUpdateAsync(
        ContextSafeHandle context,
        string? selector,
        int command,
        ref NativeCommandOptions options,
        out IntPtr operation,
        out IntPtr error);

    [LibraryImport(LibraryName, EntryPoint = "kb_snapshot_update", StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial int SnapshotUpdate(
        ContextSafeHandle context,
        string? selector,
        int command,
        ref NativeCommandOptions options,
        out IntPtr result,
        out IntPtr error);

    [LibraryImport(LibraryName, EntryPoint = "kb_create_logical_partition_async", StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial int CreateLogicalPartitionAsync(
        ContextSafeHandle context,
        string? selector,
        string partitionName,
        ulong size,
        ref NativeCommandOptions options,
        out IntPtr operation,
        out IntPtr error);

    [LibraryImport(LibraryName, EntryPoint = "kb_create_logical_partition", StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial int CreateLogicalPartition(
        ContextSafeHandle context,
        string? selector,
        string partitionName,
        ulong size,
        ref NativeCommandOptions options,
        out IntPtr result,
        out IntPtr error);

    [LibraryImport(LibraryName, EntryPoint = "kb_delete_logical_partition_async", StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial int DeleteLogicalPartitionAsync(
        ContextSafeHandle context,
        string? selector,
        string partitionName,
        ref NativeCommandOptions options,
        out IntPtr operation,
        out IntPtr error);

    [LibraryImport(LibraryName, EntryPoint = "kb_delete_logical_partition", StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial int DeleteLogicalPartition(
        ContextSafeHandle context,
        string? selector,
        string partitionName,
        ref NativeCommandOptions options,
        out IntPtr result,
        out IntPtr error);

    [LibraryImport(LibraryName, EntryPoint = "kb_resize_logical_partition_async", StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial int ResizeLogicalPartitionAsync(
        ContextSafeHandle context,
        string? selector,
        string partitionName,
        ulong size,
        ref NativeCommandOptions options,
        out IntPtr operation,
        out IntPtr error);

    [LibraryImport(LibraryName, EntryPoint = "kb_resize_logical_partition", StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial int ResizeLogicalPartition(
        ContextSafeHandle context,
        string? selector,
        string partitionName,
        ulong size,
        ref NativeCommandOptions options,
        out IntPtr result,
        out IntPtr error);

    [LibraryImport(LibraryName, EntryPoint = "kb_reboot_async", StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial int RebootAsync(
        ContextSafeHandle context,
        string? selector,
        int target,
        ref NativeCommandOptions options,
        out IntPtr operation,
        out IntPtr error);

    [LibraryImport(LibraryName, EntryPoint = "kb_reboot", StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial int Reboot(
        ContextSafeHandle context,
        string? selector,
        int target,
        ref NativeCommandOptions options,
        out IntPtr result,
        out IntPtr error);

    [LibraryImport(LibraryName, EntryPoint = "kb_continue_boot_async", StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial int ContinueBootAsync(
        ContextSafeHandle context,
        string? selector,
        ref NativeCommandOptions options,
        out IntPtr operation,
        out IntPtr error);

    [LibraryImport(LibraryName, EntryPoint = "kb_continue_boot", StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial int ContinueBoot(
        ContextSafeHandle context,
        string? selector,
        ref NativeCommandOptions options,
        out IntPtr result,
        out IntPtr error);

    [LibraryImport(LibraryName, EntryPoint = "kb_oem_async", StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial int OemAsync(
        ContextSafeHandle context,
        string? selector,
        string commandSuffix,
        ref NativeCommandOptions options,
        out IntPtr operation,
        out IntPtr error);

    [LibraryImport(LibraryName, EntryPoint = "kb_oem", StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial int Oem(
        ContextSafeHandle context,
        string? selector,
        string commandSuffix,
        ref NativeCommandOptions options,
        out IntPtr result,
        out IntPtr error);

    [LibraryImport(LibraryName, EntryPoint = "kb_raw_command_async", StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial int RawCommandAsync(
        ContextSafeHandle context,
        string? selector,
        string command,
        ref NativeCommandOptions options,
        out IntPtr operation,
        out IntPtr error);

    [LibraryImport(LibraryName, EntryPoint = "kb_raw_command", StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial int RawCommand(
        ContextSafeHandle context,
        string? selector,
        string command,
        ref NativeCommandOptions options,
        out IntPtr result,
        out IntPtr error);

    [LibraryImport(LibraryName, EntryPoint = "kb_boot_async", StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial int BootAsync(
        ContextSafeHandle context,
        string? selector,
        ref NativeCommandOptions options,
        out IntPtr operation,
        out IntPtr error);

    [LibraryImport(LibraryName, EntryPoint = "kb_boot", StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial int Boot(
        ContextSafeHandle context,
        string? selector,
        ref NativeCommandOptions options,
        out IntPtr result,
        out IntPtr error);

    [LibraryImport(LibraryName, EntryPoint = "kb_stage_async", StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial int StageAsync(
        ContextSafeHandle context,
        string? selector,
        IntPtr data,
        UIntPtr dataSize,
        ref NativeCommandOptions options,
        out IntPtr operation,
        out IntPtr error);

    [LibraryImport(LibraryName, EntryPoint = "kb_stage", StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial int Stage(
        ContextSafeHandle context,
        string? selector,
        IntPtr data,
        UIntPtr dataSize,
        ref NativeCommandOptions options,
        out IntPtr result,
        out IntPtr error);

    [LibraryImport(LibraryName, EntryPoint = "kb_upload_async", StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial int UploadAsync(
        ContextSafeHandle context,
        string? selector,
        ref NativeCommandOptions options,
        out IntPtr operation,
        out IntPtr error);

    [LibraryImport(LibraryName, EntryPoint = "kb_upload", StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial int Upload(
        ContextSafeHandle context,
        string? selector,
        ref NativeCommandOptions options,
        out IntPtr result,
        out IntPtr error);

    [LibraryImport(LibraryName, EntryPoint = "kb_fetch_async", StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial int FetchAsync(
        ContextSafeHandle context,
        string? selector,
        string partition,
        ulong offset,
        ulong size,
        ref NativeCommandOptions options,
        out IntPtr operation,
        out IntPtr error);

    [LibraryImport(LibraryName, EntryPoint = "kb_fetch", StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial int Fetch(
        ContextSafeHandle context,
        string? selector,
        string partition,
        ulong offset,
        ulong size,
        ref NativeCommandOptions options,
        out IntPtr result,
        out IntPtr error);

    [LibraryImport(LibraryName, EntryPoint = "kb_upload_file_async", StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial int UploadFileAsync(
        ContextSafeHandle context,
        string? selector,
        string outputPath,
        ref NativeCommandOptions options,
        out IntPtr operation,
        out IntPtr error);

    [LibraryImport(LibraryName, EntryPoint = "kb_get_staged_file_async", StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial int GetStagedFileAsync(
        ContextSafeHandle context,
        string? selector,
        string outputPath,
        ref NativeCommandOptions options,
        out IntPtr operation,
        out IntPtr error);

    [LibraryImport(LibraryName, EntryPoint = "kb_fetch_file_async", StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial int FetchFileAsync(
        ContextSafeHandle context,
        string? selector,
        string partition,
        ulong offset,
        ulong size,
        string outputPath,
        ref NativeCommandOptions options,
        out IntPtr operation,
        out IntPtr error);

    [LibraryImport(LibraryName, EntryPoint = "kb_validate_job_file", StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial int ValidateJobFile(string filePath, out IntPtr error);

    [LibraryImport(LibraryName, EntryPoint = "kb_plan_job_file", StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial int PlanJobFile(string filePath, out IntPtr plan, out IntPtr error);

    [LibraryImport(LibraryName, EntryPoint = "kb_job_plan_canonical_json")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial IntPtr JobPlanCanonicalJson(JobPlanSafeHandle plan, out UIntPtr size);

    [LibraryImport(LibraryName, EntryPoint = "kb_job_plan_sha256_hex")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial IntPtr JobPlanSha256Hex(JobPlanSafeHandle plan);

    [LibraryImport(LibraryName, EntryPoint = "kb_job_plan_release")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial void JobPlanRelease(IntPtr plan);

    [LibraryImport(LibraryName, EntryPoint = "kb_run_job_file_async", StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial int RunJobFileAsync(
        ContextSafeHandle context,
        string filePath,
        ref NativeJobOptions options,
        out IntPtr job,
        out IntPtr error);

    [LibraryImport(LibraryName, EntryPoint = "kb_run_job_file", StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial int RunJobFile(
        ContextSafeHandle context,
        string filePath,
        ref NativeJobOptions options,
        out IntPtr report,
        out IntPtr error);

    [LibraryImport(LibraryName, EntryPoint = "kb_job_wait")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial int JobWait(JobSafeHandle job, uint timeoutMilliseconds);

    [LibraryImport(LibraryName, EntryPoint = "kb_job_cancel")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial int JobCancel(JobSafeHandle job);

    [LibraryImport(LibraryName, EntryPoint = "kb_job_state")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial int JobState(JobSafeHandle job);

    [LibraryImport(LibraryName, EntryPoint = "kb_job_error")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial IntPtr JobError(JobSafeHandle job);

    [LibraryImport(LibraryName, EntryPoint = "kb_job_get_report")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial int JobGetReport(
        JobSafeHandle job,
        out IntPtr report,
        out IntPtr error);

    [LibraryImport(LibraryName, EntryPoint = "kb_job_release")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial void JobRelease(IntPtr job);

    [LibraryImport(LibraryName, EntryPoint = "kb_job_report_json")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial IntPtr JobReportJson(
        JobReportSafeHandle report,
        out UIntPtr size);

    [LibraryImport(LibraryName, EntryPoint = "kb_job_report_release")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial void JobReportRelease(IntPtr report);

    [LibraryImport(LibraryName, EntryPoint = "kb_operation_wait")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial int OperationWait(OperationSafeHandle operation, uint timeoutMilliseconds);

    [LibraryImport(LibraryName, EntryPoint = "kb_operation_cancel")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial int OperationCancel(OperationSafeHandle operation);

    [LibraryImport(LibraryName, EntryPoint = "kb_operation_state")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial int OperationState(OperationSafeHandle operation);

    [LibraryImport(LibraryName, EntryPoint = "kb_operation_error")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial IntPtr OperationError(OperationSafeHandle operation);

    [LibraryImport(LibraryName, EntryPoint = "kb_operation_command_result")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial int OperationCommandResult(
        OperationSafeHandle operation,
        out IntPtr result,
        out IntPtr error);

    [LibraryImport(LibraryName, EntryPoint = "kb_operation_release")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial void OperationRelease(IntPtr operation);

    [LibraryImport(LibraryName, EntryPoint = "kb_command_result_terminal_payload")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial IntPtr CommandResultTerminalPayload(
        CommandResultSafeHandle result,
        out UIntPtr size);

    [LibraryImport(LibraryName, EntryPoint = "kb_command_result_message_count")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial UIntPtr CommandResultMessageCount(CommandResultSafeHandle result);

    [LibraryImport(LibraryName, EntryPoint = "kb_command_result_message_kind")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial int CommandResultMessageKind(
        CommandResultSafeHandle result,
        UIntPtr index);

    [LibraryImport(LibraryName, EntryPoint = "kb_command_result_message_payload")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial IntPtr CommandResultMessagePayload(
        CommandResultSafeHandle result,
        UIntPtr index,
        out UIntPtr size);

    [LibraryImport(LibraryName, EntryPoint = "kb_command_result_data")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial IntPtr CommandResultData(CommandResultSafeHandle result, out UIntPtr size);

    [LibraryImport(LibraryName, EntryPoint = "kb_command_result_output_path")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial IntPtr CommandResultOutputPath(CommandResultSafeHandle result);

    [LibraryImport(LibraryName, EntryPoint = "kb_command_result_received_bytes")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial ulong CommandResultReceivedBytes(CommandResultSafeHandle result);

    [LibraryImport(LibraryName, EntryPoint = "kb_command_result_device_identifier")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial IntPtr CommandResultDeviceIdentifier(CommandResultSafeHandle result);

    [LibraryImport(LibraryName, EntryPoint = "kb_command_result_release")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial void CommandResultRelease(IntPtr result);

    [LibraryImport(LibraryName, EntryPoint = "kb_error_status")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial int ErrorStatus(IntPtr error);

    [LibraryImport(LibraryName, EntryPoint = "kb_error_message")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial IntPtr ErrorMessage(IntPtr error);

    [LibraryImport(LibraryName, EntryPoint = "kb_error_device_identifier")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial IntPtr ErrorDeviceIdentifier(IntPtr error);

    [LibraryImport(LibraryName, EntryPoint = "kb_error_native_code")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial int ErrorNativeCode(IntPtr error);

    [LibraryImport(LibraryName, EntryPoint = "kb_error_transfer_state")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial int ErrorTransferState(IntPtr error);

    [LibraryImport(LibraryName, EntryPoint = "kb_error_device_message")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial IntPtr ErrorDeviceMessage(IntPtr error, out UIntPtr size);

    [LibraryImport(LibraryName, EntryPoint = "kb_error_command_message_count")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial UIntPtr ErrorCommandMessageCount(IntPtr error);

    [LibraryImport(LibraryName, EntryPoint = "kb_error_command_message_kind")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial int ErrorCommandMessageKind(IntPtr error, UIntPtr index);

    [LibraryImport(LibraryName, EntryPoint = "kb_error_command_message_payload")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial IntPtr ErrorCommandMessagePayload(
        IntPtr error,
        UIntPtr index,
        out UIntPtr size);

    [LibraryImport(LibraryName, EntryPoint = "kb_error_inbound_expected_bytes")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial ulong ErrorInboundExpectedBytes(IntPtr error);

    [LibraryImport(LibraryName, EntryPoint = "kb_error_inbound_transferred_bytes")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial ulong ErrorInboundTransferredBytes(IntPtr error);

    [LibraryImport(LibraryName, EntryPoint = "kb_error_inbound_transfer_state")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial int ErrorInboundTransferState(IntPtr error);

    [LibraryImport(LibraryName, EntryPoint = "kb_error_session_poisoned")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial int ErrorSessionPoisoned(IntPtr error);

    [LibraryImport(LibraryName, EntryPoint = "kb_error_release")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial void ErrorRelease(IntPtr error);
}
#endif
