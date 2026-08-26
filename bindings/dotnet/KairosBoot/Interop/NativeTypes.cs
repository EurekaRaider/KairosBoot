using System;
using System.Runtime.InteropServices;

namespace KairosBoot.Interop;

internal static partial class NativeMethods
{
    // Keep the managed KairosBoot.dll and the native library distinct on
    // case-insensitive filesystems.
    internal const string LibraryName = "kairosboot_native";
    internal const uint ApiVersion = 1;
    internal const uint DefaultTimeoutMilliseconds = 30_000;
    internal const uint WaitSliceMilliseconds = 50;
}

[StructLayout(LayoutKind.Sequential)]
internal struct NativeVersion
{
    internal uint StructSize;
    internal uint ApiVersion;
    internal uint Major;
    internal uint Minor;
    internal uint Patch;
    internal IntPtr String;
}

[StructLayout(LayoutKind.Sequential)]
internal struct NativeFlashOptions
{
    internal uint StructSize;
    internal uint ApiVersion;
    internal uint TimeoutMilliseconds;
    internal IntPtr ProgressCallback;
    internal IntPtr ProgressUserData;
}

[StructLayout(LayoutKind.Sequential)]
internal struct NativeProgress
{
    internal uint StructSize;
    internal uint ApiVersion;
    internal ulong BytesCompleted;
    internal ulong BytesTotal;
    internal IntPtr Stage;
    internal IntPtr DeviceIdentifier;
}

[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
internal delegate int NativeProgressCallback(ref NativeProgress progress, IntPtr userData);
