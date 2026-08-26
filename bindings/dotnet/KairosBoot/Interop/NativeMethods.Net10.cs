#if NET10_0_OR_GREATER
using System;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace KairosBoot.Interop;

internal static partial class NativeMethods
{
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
    internal static partial int ContextCreate(
        IntPtr options,
        out IntPtr context,
        out IntPtr error);

    [LibraryImport(LibraryName, EntryPoint = "kb_context_release")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial void ContextRelease(IntPtr context);

    [LibraryImport(LibraryName, EntryPoint = "kb_flash_options_init")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial void FlashOptionsInit(ref NativeFlashOptions options);

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

    [LibraryImport(
        LibraryName,
        EntryPoint = "kb_flash_file_async",
        StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial int FlashFileAsync(
        ContextSafeHandle context,
        string? serial,
        string partition,
        string filePath,
        ref NativeFlashOptions options,
        out IntPtr operation,
        out IntPtr error);

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

    [LibraryImport(LibraryName, EntryPoint = "kb_operation_release")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial void OperationRelease(IntPtr operation);

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

    [LibraryImport(LibraryName, EntryPoint = "kb_error_release")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial void ErrorRelease(IntPtr error);
}
#endif
