using System;
using System.Runtime.InteropServices;

namespace KairosBoot.Interop;

internal static partial class NativeMethods
{
    // Keep the managed KairosBoot.dll and the native library distinct on
    // case-insensitive filesystems.
    internal const string LibraryName = "kairosboot_native";
    internal const uint ApiVersion = 1;
    internal const uint PollTimeoutMilliseconds = 0;
    internal const int PollDelayMilliseconds = 10;
    internal const ulong FetchUnspecified = ulong.MaxValue;
    internal const ulong DefaultMaximumReceiveBytes = 64UL * 1024UL * 1024UL;
    internal static readonly uint VersionStructSize =
        checked((uint)Marshal.SizeOf<NativeVersion>());
    internal static readonly uint ProgressStructSize =
        checked((uint)Marshal.SizeOf<NativeProgress>());
    internal static readonly uint FlashOptionsStructSize =
        checked((uint)Marshal.SizeOf<NativeFlashOptions>());
    internal static readonly uint ContextOptionsStructSize =
        checked((uint)Marshal.SizeOf<NativeContextOptions>());
    internal static readonly uint LegacyBootOptionsStructSize =
        checked((uint)Marshal.SizeOf<NativeLegacyBootOptions>());
    internal static readonly uint UpdateOptionsStructSize =
        checked((uint)Marshal.SizeOf<NativeUpdateOptions>());
    internal static readonly uint CommandOptionsStructSize =
        checked((uint)Marshal.SizeOf<NativeCommandOptions>());
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
internal struct NativeContextOptions
{
    internal uint StructSize;
    internal uint ApiVersion;
    internal IntPtr LogCallback;
    internal IntPtr LogUserData;
    internal ulong UsbVendorId;
}

[StructLayout(LayoutKind.Sequential)]
internal struct NativeFlashOptions
{
    internal uint StructSize;
    internal uint ApiVersion;
    internal uint TimeoutMilliseconds;
    internal IntPtr ProgressCallback;
    internal IntPtr ProgressUserData;
    internal int DisableVerity;
    internal int DisableVerification;
    internal IntPtr Slot;
    internal int SetActive;
    internal IntPtr ActiveSlot;
    internal ulong SparseLimitBytes;
    internal int Force;
    internal uint FilesystemOptions;
}

[StructLayout(LayoutKind.Sequential)]
internal struct NativeLegacyBootOptions
{
    internal uint StructSize;
    internal uint ApiVersion;
    internal IntPtr CommandLine;
    internal uint BaseAddress;
    internal uint PageSize;
    internal uint KernelOffset;
    internal uint RamdiskOffset;
    internal uint SecondOffset;
    internal uint TagsOffset;
    internal uint HeaderVersion;
    internal IntPtr OsVersion;
    internal IntPtr OsPatchLevel;
    internal IntPtr DtbPath;
    internal ulong DtbOffset;
}

[StructLayout(LayoutKind.Sequential)]
internal struct NativeUpdateOptions
{
    internal uint StructSize;
    internal uint ApiVersion;
    internal uint TimeoutMilliseconds;
    internal int Wipe;
    internal IntPtr ProgressCallback;
    internal IntPtr ProgressUserData;
    internal int SkipReboot;
    internal int SkipSecondary;
    internal int ExcludeDynamicPartitions;
    internal int DisableFastbootInfo;
    internal int DisableVerity;
    internal int DisableVerification;
    internal IntPtr Slot;
    internal int SetActive;
    internal IntPtr ActiveSlot;
    internal ulong SparseLimitBytes;
    internal int Force;
    internal uint FilesystemOptions;
    internal int DisableSuperOptimization;
}

[StructLayout(LayoutKind.Sequential)]
internal struct NativeCommandOptions
{
    internal uint StructSize;
    internal uint ApiVersion;
    internal uint TimeoutMilliseconds;
    internal IntPtr ProgressCallback;
    internal IntPtr ProgressUserData;
    internal ulong MaximumReceiveBytes;
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
