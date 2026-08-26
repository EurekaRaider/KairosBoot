#if NET48
using System;
using System.Runtime.InteropServices;

namespace KairosBoot.Interop;

internal static partial class NativeMethods
{
    [DllImport(LibraryName, EntryPoint = "kb_context_options_init", CallingConvention = CallingConvention.Cdecl)]
    internal static extern void ContextOptionsInit(ref NativeContextOptions options);

    [DllImport(LibraryName, EntryPoint = "kb_flash_options_init", CallingConvention = CallingConvention.Cdecl)]
    internal static extern void FlashOptionsInit(ref NativeFlashOptions options);

    [DllImport(LibraryName, EntryPoint = "kb_command_options_init", CallingConvention = CallingConvention.Cdecl)]
    internal static extern void CommandOptionsInit(ref NativeCommandOptions options);

    [DllImport(LibraryName, EntryPoint = "kb_version_init", CallingConvention = CallingConvention.Cdecl)]
    internal static extern void VersionInit(ref NativeVersion version);

    [DllImport(LibraryName, EntryPoint = "kb_get_version", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int GetVersion(ref NativeVersion version);

    [DllImport(LibraryName, EntryPoint = "kb_status_string", CallingConvention = CallingConvention.Cdecl)]
    internal static extern IntPtr StatusString(int status);

    [DllImport(LibraryName, EntryPoint = "kb_context_create", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int ContextCreate(IntPtr options, out IntPtr context, out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_context_release", CallingConvention = CallingConvention.Cdecl)]
    internal static extern void ContextRelease(IntPtr context);

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
        ContextSafeHandle context,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string? serial,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string partition,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string filePath,
        ref NativeFlashOptions options,
        out IntPtr operation,
        out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_flash_file", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int FlashFile(
        ContextSafeHandle context,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string? serial,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string partition,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string filePath,
        ref NativeFlashOptions options,
        out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_getvar_async", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int GetVarAsync(
        ContextSafeHandle context,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string? selector,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string variable,
        ref NativeCommandOptions options,
        out IntPtr operation,
        out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_getvar", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int GetVar(
        ContextSafeHandle context,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string? selector,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string variable,
        ref NativeCommandOptions options,
        out IntPtr result,
        out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_erase_async", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int EraseAsync(
        ContextSafeHandle context,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string? selector,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string partition,
        ref NativeCommandOptions options,
        out IntPtr operation,
        out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_erase", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int Erase(
        ContextSafeHandle context,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string? selector,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string partition,
        ref NativeCommandOptions options,
        out IntPtr result,
        out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_set_active_async", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int SetActiveAsync(
        ContextSafeHandle context,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string? selector,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string slot,
        ref NativeCommandOptions options,
        out IntPtr operation,
        out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_set_active", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int SetActive(
        ContextSafeHandle context,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string? selector,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string slot,
        ref NativeCommandOptions options,
        out IntPtr result,
        out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_flashing_async", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int FlashingAsync(
        ContextSafeHandle context,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string? selector,
        int command,
        ref NativeCommandOptions options,
        out IntPtr operation,
        out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_flashing", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int Flashing(
        ContextSafeHandle context,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string? selector,
        int command,
        ref NativeCommandOptions options,
        out IntPtr result,
        out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_gsi_async", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int GsiAsync(
        ContextSafeHandle context,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string? selector,
        int command,
        ref NativeCommandOptions options,
        out IntPtr operation,
        out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_gsi", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int Gsi(
        ContextSafeHandle context,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string? selector,
        int command,
        ref NativeCommandOptions options,
        out IntPtr result,
        out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_snapshot_update_async", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int SnapshotUpdateAsync(
        ContextSafeHandle context,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string? selector,
        int command,
        ref NativeCommandOptions options,
        out IntPtr operation,
        out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_snapshot_update", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int SnapshotUpdate(
        ContextSafeHandle context,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string? selector,
        int command,
        ref NativeCommandOptions options,
        out IntPtr result,
        out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_create_logical_partition_async", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int CreateLogicalPartitionAsync(
        ContextSafeHandle context,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string? selector,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string partitionName,
        ulong size,
        ref NativeCommandOptions options,
        out IntPtr operation,
        out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_create_logical_partition", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int CreateLogicalPartition(
        ContextSafeHandle context,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string? selector,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string partitionName,
        ulong size,
        ref NativeCommandOptions options,
        out IntPtr result,
        out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_delete_logical_partition_async", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int DeleteLogicalPartitionAsync(
        ContextSafeHandle context,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string? selector,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string partitionName,
        ref NativeCommandOptions options,
        out IntPtr operation,
        out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_delete_logical_partition", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int DeleteLogicalPartition(
        ContextSafeHandle context,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string? selector,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string partitionName,
        ref NativeCommandOptions options,
        out IntPtr result,
        out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_resize_logical_partition_async", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int ResizeLogicalPartitionAsync(
        ContextSafeHandle context,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string? selector,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string partitionName,
        ulong size,
        ref NativeCommandOptions options,
        out IntPtr operation,
        out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_resize_logical_partition", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int ResizeLogicalPartition(
        ContextSafeHandle context,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string? selector,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string partitionName,
        ulong size,
        ref NativeCommandOptions options,
        out IntPtr result,
        out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_reboot_async", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int RebootAsync(
        ContextSafeHandle context,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string? selector,
        int target,
        ref NativeCommandOptions options,
        out IntPtr operation,
        out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_reboot", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int Reboot(
        ContextSafeHandle context,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string? selector,
        int target,
        ref NativeCommandOptions options,
        out IntPtr result,
        out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_continue_boot_async", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int ContinueBootAsync(
        ContextSafeHandle context,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string? selector,
        ref NativeCommandOptions options,
        out IntPtr operation,
        out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_continue_boot", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int ContinueBoot(
        ContextSafeHandle context,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string? selector,
        ref NativeCommandOptions options,
        out IntPtr result,
        out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_oem_async", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int OemAsync(
        ContextSafeHandle context,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string? selector,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string commandSuffix,
        ref NativeCommandOptions options,
        out IntPtr operation,
        out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_oem", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int Oem(
        ContextSafeHandle context,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string? selector,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string commandSuffix,
        ref NativeCommandOptions options,
        out IntPtr result,
        out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_raw_command_async", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int RawCommandAsync(
        ContextSafeHandle context,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string? selector,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string command,
        ref NativeCommandOptions options,
        out IntPtr operation,
        out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_raw_command", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int RawCommand(
        ContextSafeHandle context,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string? selector,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string command,
        ref NativeCommandOptions options,
        out IntPtr result,
        out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_boot_async", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int BootAsync(
        ContextSafeHandle context,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string? selector,
        ref NativeCommandOptions options,
        out IntPtr operation,
        out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_boot", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int Boot(
        ContextSafeHandle context,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string? selector,
        ref NativeCommandOptions options,
        out IntPtr result,
        out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_stage_async", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int StageAsync(
        ContextSafeHandle context,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string? selector,
        IntPtr data,
        UIntPtr dataSize,
        ref NativeCommandOptions options,
        out IntPtr operation,
        out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_stage", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int Stage(
        ContextSafeHandle context,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string? selector,
        IntPtr data,
        UIntPtr dataSize,
        ref NativeCommandOptions options,
        out IntPtr result,
        out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_upload_async", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int UploadAsync(
        ContextSafeHandle context,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string? selector,
        ref NativeCommandOptions options,
        out IntPtr operation,
        out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_upload", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int Upload(
        ContextSafeHandle context,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string? selector,
        ref NativeCommandOptions options,
        out IntPtr result,
        out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_fetch_async", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int FetchAsync(
        ContextSafeHandle context,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string? selector,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string partition,
        ulong offset,
        ulong size,
        ref NativeCommandOptions options,
        out IntPtr operation,
        out IntPtr error);

    [DllImport(LibraryName, EntryPoint = "kb_fetch", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int Fetch(
        ContextSafeHandle context,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string? selector,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string partition,
        ulong offset,
        ulong size,
        ref NativeCommandOptions options,
        out IntPtr result,
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
