using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Tasks;
using KairosBoot.Interop;

namespace KairosBoot;

/// <summary>Owns a native KairosBoot context and exposes the managed SDK.</summary>
public sealed class Context : IDisposable
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
            NativeMethods.VersionInit(ref native);
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

    /// <summary>Creates a KairosBoot context using safe native defaults.</summary>
    public static Context Create()
    {
        var status = NativeMethods.ContextCreate(IntPtr.Zero, out var rawContext, out var rawError);
        if (status != (int)KairosBootStatus.Ok)
        {
            if (rawContext != IntPtr.Zero)
            {
                using (var unexpectedContext = new ContextSafeHandle(rawContext))
                {
                }
            }

            throw NativeError.TakeException(status, rawError);
        }

        if (rawContext == IntPtr.Zero)
        {
            throw NativeError.TakeException((int)KairosBootStatus.Internal, rawError);
        }

        if (rawError != IntPtr.Zero)
        {
            using (var unexpectedError = new ErrorSafeHandle(rawError))
            {
            }
        }

        return new Context(new ContextSafeHandle(rawContext));
    }

    /// <summary>Gets a snapshot of currently connected Fastboot devices.</summary>
    public IReadOnlyList<Device> Devices
    {
        get
        {
            ThrowIfDisposed();
            var status = NativeMethods.EnumerateDevices(handle, out var rawDevices, out var rawError);
            if (status != (int)KairosBootStatus.Ok)
            {
                if (rawDevices != IntPtr.Zero)
                {
                    using (var unexpectedDevices = new DeviceListSafeHandle(rawDevices))
                    {
                    }
                }

                throw NativeError.TakeException(status, rawError);
            }

            if (rawDevices == IntPtr.Zero)
            {
                throw NativeError.TakeException((int)KairosBootStatus.Internal, rawError);
            }

            if (rawError != IntPtr.Zero)
            {
                using (var unexpectedError = new ErrorSafeHandle(rawError))
                {
                }
            }

            using (var devices = new DeviceListSafeHandle(rawDevices))
            {
                var nativeCount = NativeMethods.DeviceListCount(devices).ToUInt64();
                if (nativeCount > int.MaxValue)
                {
                    throw new InvalidOperationException("Native device count exceeds managed collection limits.");
                }

                var result = new List<Device>((int)nativeCount);
                for (ulong index = 0; index < nativeCount; index++)
                {
                    var nativeIndex = new UIntPtr(index);
                    result.Add(new Device(
                        Utf8String.FromNative(NativeMethods.DeviceListSerial(devices, nativeIndex)),
                        Utf8String.FromNative(NativeMethods.DeviceListUsbPath(devices, nativeIndex)),
                        Utf8String.FromNative(NativeMethods.DeviceListProduct(devices, nativeIndex))));
                }

                return new ReadOnlyCollection<Device>(result);
            }
        }
    }

    /// <summary>
    /// Starts a flash operation. Native failures are surfaced as
    /// <see cref="KairosBootException"/> and cancellation cancels the native operation.
    /// </summary>
    public async Task FlashFileAsync(
        string partition,
        string filePath,
        string? serial = null,
        IProgress<FlashProgress>? progress = null,
        CancellationToken cancellationToken = default)
    {
        if (string.IsNullOrWhiteSpace(partition))
        {
            throw new ArgumentException("Partition must not be empty.", nameof(partition));
        }

        if (string.IsNullOrWhiteSpace(filePath))
        {
            throw new ArgumentException("File path must not be empty.", nameof(filePath));
        }

        if (serial != null && serial.Length == 0)
        {
            throw new ArgumentException("Serial must be null or non-empty.", nameof(serial));
        }

        ThrowIfDisposed();
        cancellationToken.ThrowIfCancellationRequested();

        var callback = progress == null ? null : new NativeProgressCallback(ReportProgress);
        var progressHandle = default(GCHandle);
        var contextReferenceAdded = false;
        try
        {
            handle.DangerousAddRef(ref contextReferenceAdded);

            IntPtr userData = IntPtr.Zero;
            if (progress != null)
            {
                progressHandle = GCHandle.Alloc(progress);
                userData = GCHandle.ToIntPtr(progressHandle);
            }

            var options = new NativeFlashOptions
            {
                StructSize = checked((uint)Marshal.SizeOf<NativeFlashOptions>()),
                ApiVersion = NativeMethods.ApiVersion,
                TimeoutMilliseconds = NativeMethods.DefaultTimeoutMilliseconds,
                ProgressCallback = callback == null
                    ? IntPtr.Zero
                    : Marshal.GetFunctionPointerForDelegate(callback),
                ProgressUserData = userData,
            };

            var status = NativeMethods.FlashFileAsync(
                handle,
                serial,
                partition,
                filePath,
                ref options,
                out var rawOperation,
                out var rawError);

            if (status != (int)KairosBootStatus.Ok)
            {
                if (rawOperation != IntPtr.Zero)
                {
                    using (var unexpectedOperation = new OperationSafeHandle(rawOperation))
                    {
                    }
                }

                throw NativeError.TakeException(status, rawError);
            }

            if (rawOperation == IntPtr.Zero)
            {
                throw NativeError.TakeException((int)KairosBootStatus.Internal, rawError);
            }

            if (rawError != IntPtr.Zero)
            {
                using (var unexpectedError = new ErrorSafeHandle(rawError))
                {
                }
            }

            using (var operation = new OperationSafeHandle(rawOperation))
            using (cancellationToken.Register(() => NativeMethods.OperationCancel(operation)))
            {
                await Task.Run(
                    () => WaitForOperation(operation, cancellationToken),
                    CancellationToken.None).ConfigureAwait(false);
            }
        }
        finally
        {
            GC.KeepAlive(callback);

            if (progressHandle.IsAllocated)
            {
                progressHandle.Free();
            }

            if (contextReferenceAdded)
            {
                handle.DangerousRelease();
            }
        }
    }

    /// <summary>Releases the native context.</summary>
    public void Dispose()
    {
        handle.Dispose();
    }

    private static void WaitForOperation(
        OperationSafeHandle operation,
        CancellationToken cancellationToken)
    {
        while (true)
        {
            var status = NativeMethods.OperationWait(
                operation,
                NativeMethods.WaitSliceMilliseconds);
            if (status == (int)KairosBootStatus.Timeout)
            {
                continue;
            }

            if (status == (int)KairosBootStatus.Ok)
            {
                return;
            }

            if (status == (int)KairosBootStatus.Cancelled && cancellationToken.IsCancellationRequested)
            {
                throw new OperationCanceledException(cancellationToken);
            }

            throw NativeError.FromBorrowed(status, NativeMethods.OperationError(operation));
        }
    }

    private static int ReportProgress(ref NativeProgress native, IntPtr userData)
    {
        try
        {
            var target = GCHandle.FromIntPtr(userData).Target as IProgress<FlashProgress>;
            if (target == null)
            {
                return 1;
            }

            target.Report(new FlashProgress(
                native.BytesCompleted,
                native.BytesTotal,
                Utf8String.FromNative(native.Stage),
                Utf8String.FromNative(native.DeviceIdentifier)));
            return 0;
        }
        catch
        {
            return 1;
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
