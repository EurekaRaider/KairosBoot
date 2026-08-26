#if NET48
using System;
using System.Runtime.InteropServices;

namespace KairosBoot.Interop;

internal static partial class NativeMethods
{
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

    [DllImport(LibraryName, EntryPoint = "kb_operation_wait", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int OperationWait(OperationSafeHandle operation, uint timeoutMilliseconds);

    [DllImport(LibraryName, EntryPoint = "kb_operation_cancel", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int OperationCancel(OperationSafeHandle operation);

    [DllImport(LibraryName, EntryPoint = "kb_operation_state", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int OperationState(OperationSafeHandle operation);

    [DllImport(LibraryName, EntryPoint = "kb_operation_error", CallingConvention = CallingConvention.Cdecl)]
    internal static extern IntPtr OperationError(OperationSafeHandle operation);

    [DllImport(LibraryName, EntryPoint = "kb_operation_release", CallingConvention = CallingConvention.Cdecl)]
    internal static extern void OperationRelease(IntPtr operation);

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

    [DllImport(LibraryName, EntryPoint = "kb_error_release", CallingConvention = CallingConvention.Cdecl)]
    internal static extern void ErrorRelease(IntPtr error);
}
#endif
