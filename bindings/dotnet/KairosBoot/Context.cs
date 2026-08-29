using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using KairosBoot.Interop;

namespace KairosBoot;

/// <summary>Owns process-wide KairosBoot resources and device discovery.</summary>
public sealed partial class Context : IDisposable
{
    private readonly ContextSafeHandle handle;

    private Context(ContextSafeHandle handle)
    {
        this.handle = handle;
    }

    /// <summary>Gets the runtime and ABI version of the loaded native library.</summary>
    public static KairosBootVersion Version
    {
        get
        {
            var native = new NativeVersion();
            NativeMethods.VersionInitSized(ref native, NativeMethods.VersionStructSize);
            var status = NativeMethods.GetVersion(ref native);
            if (status != (int)KairosBootStatus.Ok)
            {
                throw NativeError.TakeException(status, IntPtr.Zero);
            }

            return new KairosBootVersion(
                native.Major,
                native.Minor,
                native.Patch,
                native.ApiVersion,
                Utf8String.FromNative(native.String));
        }
    }

    /// <summary>Creates a context using safe native defaults.</summary>
    public static Context Create()
    {
        var status = NativeMethods.ContextCreate(
            IntPtr.Zero, out var rawContext, out var rawError);
        return CompleteCreate(status, rawContext, rawError);
    }

    /// <summary>Creates a context with explicit USB discovery options.</summary>
    public static Context Create(ContextOptions options)
    {
        var native = new NativeContextOptions();
        NativeMethods.ContextOptionsInitSized(
            ref native, NativeMethods.ContextOptionsStructSize);
        native.UsbVendorId = options.UsbVendorId;
        var status = NativeMethods.ContextCreateWithOptions(
            ref native, out var rawContext, out var rawError);
        return CompleteCreate(status, rawContext, rawError);
    }

    /// <summary>Opens the only connected USB Fastboot device.</summary>
    public Device OpenDevice() => OpenDeviceCore(null);

    /// <summary>Opens a USB, TCP, or UDP Fastboot device by selector.</summary>
    public Device OpenDevice(string selector)
    {
        ValidateRequiredText(selector, nameof(selector));
        return OpenDeviceCore(selector);
    }

    /// <summary>Gets a discovery snapshot without opening device sessions.</summary>
    public IReadOnlyList<DeviceInfo> Devices
    {
        get
        {
            ThrowIfDisposed();
            var status = NativeMethods.EnumerateDevices(
                handle, out var rawDevices, out var rawError);
            if (status != (int)KairosBootStatus.Ok)
            {
                if (rawDevices != IntPtr.Zero)
                {
                    using var unexpectedDevices =
                        new DeviceListSafeHandle(rawDevices);
                }
                throw NativeError.TakeException(status, rawError);
            }
            if (rawDevices == IntPtr.Zero)
            {
                throw NativeError.TakeException(
                    (int)KairosBootStatus.Internal, rawError);
            }

            ReleaseUnexpectedError(rawError);
            using var devices = new DeviceListSafeHandle(rawDevices);
            var nativeCount = NativeMethods.DeviceListCount(devices).ToUInt64();
            if (nativeCount > int.MaxValue)
            {
                throw new InvalidOperationException(
                    "Native device count exceeds managed collection limits.");
            }

            var result = new List<DeviceInfo>((int)nativeCount);
            for (ulong index = 0; index < nativeCount; index++)
            {
                var nativeIndex = new UIntPtr(index);
                result.Add(new DeviceInfo(
                    Utf8String.FromNative(
                        NativeMethods.DeviceListSerial(devices, nativeIndex)),
                    Utf8String.FromNative(
                        NativeMethods.DeviceListUsbPath(devices, nativeIndex)),
                    Utf8String.FromNative(
                        NativeMethods.DeviceListProduct(devices, nativeIndex))));
            }
            return new ReadOnlyCollection<DeviceInfo>(result);
        }
    }

    /// <summary>Releases the native context. Open Device objects remain valid.</summary>
    public void Dispose() => handle.Dispose();

    private Device OpenDeviceCore(string? selector)
    {
        ThrowIfDisposed();
        var status = NativeMethods.DeviceOpen(
            handle, selector, out var rawDevice, out var rawError);
        if (status != (int)KairosBootStatus.Ok)
        {
            if (rawDevice != IntPtr.Zero)
            {
                using var unexpectedDevice = new DeviceSafeHandle(rawDevice);
            }
            throw NativeError.TakeException(status, rawError);
        }
        if (rawDevice == IntPtr.Zero)
        {
            throw NativeError.TakeException(
                (int)KairosBootStatus.Internal, rawError);
        }

        ReleaseUnexpectedError(rawError);
        return new Device(new DeviceSafeHandle(rawDevice));
    }

    private static Context CompleteCreate(
        int status, IntPtr rawContext, IntPtr rawError)
    {
        if (status != (int)KairosBootStatus.Ok)
        {
            if (rawContext != IntPtr.Zero)
            {
                using var unexpectedContext = new ContextSafeHandle(rawContext);
            }
            throw NativeError.TakeException(status, rawError);
        }
        if (rawContext == IntPtr.Zero)
        {
            throw NativeError.TakeException(
                (int)KairosBootStatus.Internal, rawError);
        }

        ReleaseUnexpectedError(rawError);
        return new Context(new ContextSafeHandle(rawContext));
    }

    private static void ValidateRequiredText(string value, string parameterName)
    {
        if (string.IsNullOrEmpty(value) || value.IndexOf('\0') >= 0)
        {
            throw new ArgumentException(
                "Value must be non-empty and must not contain NUL.",
                parameterName);
        }
    }

    private static void ReleaseUnexpectedError(IntPtr rawError)
    {
        if (rawError != IntPtr.Zero)
        {
            using var unexpectedError = new ErrorSafeHandle(rawError);
        }
    }

    private void ThrowIfDisposed()
    {
        if (handle.IsClosed || handle.IsInvalid)
        {
            throw new ObjectDisposedException(nameof(Context));
        }
    }
}
