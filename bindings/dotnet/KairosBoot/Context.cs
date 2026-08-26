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

    private delegate int StartCommandOperation(
        ref NativeCommandOptions options,
        out IntPtr operation,
        out IntPtr error);

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

        ReleaseUnexpectedError(rawError);
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

            ReleaseUnexpectedError(rawError);
            using (var devices = new DeviceListSafeHandle(rawDevices))
            {
                var nativeCount = NativeMethods.DeviceListCount(devices).ToUInt64();
                if (nativeCount > int.MaxValue)
                {
                    throw new InvalidOperationException(
                        "Native device count exceeds managed collection limits.");
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

    /// <summary>Starts a flash operation with native default options.</summary>
    public Task FlashFileAsync(
        string partition,
        string filePath,
        string? serial = null,
        IProgress<FlashProgress>? progress = null,
        CancellationToken cancellationToken = default)
    {
        return FlashFileCoreAsync(
            partition,
            filePath,
            FlashOptions.Default,
            serial,
            progress,
            cancellationToken);
    }

    /// <summary>Starts a flash operation with a typed per-I/O timeout.</summary>
    public Task FlashFileAsync(
        string partition,
        string filePath,
        FlashOptions options,
        string? serial = null,
        IProgress<FlashProgress>? progress = null,
        CancellationToken cancellationToken = default)
    {
        return FlashFileCoreAsync(
            partition,
            filePath,
            options,
            serial,
            progress,
            cancellationToken);
    }

    /// <summary>Reads a Fastboot variable.</summary>
    public Task<CommandResult> GetVarAsync(
        string variable,
        string? deviceSelector = null,
        CommandOptions options = default,
        IProgress<CommandProgress>? progress = null,
        CancellationToken cancellationToken = default)
    {
        ValidateRequiredText(variable, nameof(variable));
        ValidateSelector(deviceSelector);
        return RunCommandAsync(
            options,
            progress,
            cancellationToken,
            (ref NativeCommandOptions nativeOptions, out IntPtr operation, out IntPtr error) =>
                NativeMethods.GetVarAsync(
                    handle,
                    deviceSelector,
                    variable,
                    ref nativeOptions,
                    out operation,
                    out error));
    }

    /// <summary>Erases a partition.</summary>
    public Task<CommandResult> EraseAsync(
        string partition,
        string? deviceSelector = null,
        CommandOptions options = default,
        IProgress<CommandProgress>? progress = null,
        CancellationToken cancellationToken = default)
    {
        ValidateRequiredText(partition, nameof(partition));
        ValidateSelector(deviceSelector);
        return RunCommandAsync(
            options,
            progress,
            cancellationToken,
            (ref NativeCommandOptions nativeOptions, out IntPtr operation, out IntPtr error) =>
                NativeMethods.EraseAsync(
                    handle,
                    deviceSelector,
                    partition,
                    ref nativeOptions,
                    out operation,
                    out error));
    }

    /// <summary>Selects the active A/B slot.</summary>
    public Task<CommandResult> SetActiveAsync(
        string slot,
        string? deviceSelector = null,
        CommandOptions options = default,
        IProgress<CommandProgress>? progress = null,
        CancellationToken cancellationToken = default)
    {
        ValidateRequiredText(slot, nameof(slot));
        ValidateSelector(deviceSelector);
        return RunCommandAsync(
            options,
            progress,
            cancellationToken,
            (ref NativeCommandOptions nativeOptions, out IntPtr operation, out IntPtr error) =>
                NativeMethods.SetActiveAsync(
                    handle,
                    deviceSelector,
                    slot,
                    ref nativeOptions,
                    out operation,
                    out error));
    }

    /// <summary>Runs a Fastboot flashing-state command.</summary>
    public Task<CommandResult> FlashingAsync(
        FlashingCommand command,
        string? deviceSelector = null,
        CommandOptions options = default,
        IProgress<CommandProgress>? progress = null,
        CancellationToken cancellationToken = default)
    {
        ValidateSelector(deviceSelector);
        return RunCommandAsync(
            options,
            progress,
            cancellationToken,
            (ref NativeCommandOptions nativeOptions, out IntPtr operation, out IntPtr error) =>
                NativeMethods.FlashingAsync(
                    handle,
                    deviceSelector,
                    (int)command,
                    ref nativeOptions,
                    out operation,
                    out error));
    }

    /// <summary>Runs a Fastboot GSI management command.</summary>
    public Task<CommandResult> GsiAsync(
        GsiCommand command,
        string? deviceSelector = null,
        CommandOptions options = default,
        IProgress<CommandProgress>? progress = null,
        CancellationToken cancellationToken = default)
    {
        ValidateSelector(deviceSelector);
        return RunCommandAsync(
            options,
            progress,
            cancellationToken,
            (ref NativeCommandOptions nativeOptions, out IntPtr operation, out IntPtr error) =>
                NativeMethods.GsiAsync(
                    handle,
                    deviceSelector,
                    (int)command,
                    ref nativeOptions,
                    out operation,
                    out error));
    }

    /// <summary>Runs a Fastboot snapshot-update command.</summary>
    public Task<CommandResult> SnapshotUpdateAsync(
        SnapshotUpdateCommand command,
        string? deviceSelector = null,
        CommandOptions options = default,
        IProgress<CommandProgress>? progress = null,
        CancellationToken cancellationToken = default)
    {
        ValidateSelector(deviceSelector);
        return RunCommandAsync(
            options,
            progress,
            cancellationToken,
            (ref NativeCommandOptions nativeOptions, out IntPtr operation, out IntPtr error) =>
                NativeMethods.SnapshotUpdateAsync(
                    handle,
                    deviceSelector,
                    (int)command,
                    ref nativeOptions,
                    out operation,
                    out error));
    }

    /// <summary>Creates a logical partition with the requested byte size.</summary>
    public Task<CommandResult> CreateLogicalPartitionAsync(
        string partitionName,
        ulong size,
        string? deviceSelector = null,
        CommandOptions options = default,
        IProgress<CommandProgress>? progress = null,
        CancellationToken cancellationToken = default)
    {
        ValidateNativeText(partitionName, nameof(partitionName));
        ValidateSelector(deviceSelector);
        return RunCommandAsync(
            options,
            progress,
            cancellationToken,
            (ref NativeCommandOptions nativeOptions, out IntPtr operation, out IntPtr error) =>
                NativeMethods.CreateLogicalPartitionAsync(
                    handle,
                    deviceSelector,
                    partitionName,
                    size,
                    ref nativeOptions,
                    out operation,
                    out error));
    }

    /// <summary>Deletes a logical partition.</summary>
    public Task<CommandResult> DeleteLogicalPartitionAsync(
        string partitionName,
        string? deviceSelector = null,
        CommandOptions options = default,
        IProgress<CommandProgress>? progress = null,
        CancellationToken cancellationToken = default)
    {
        ValidateNativeText(partitionName, nameof(partitionName));
        ValidateSelector(deviceSelector);
        return RunCommandAsync(
            options,
            progress,
            cancellationToken,
            (ref NativeCommandOptions nativeOptions, out IntPtr operation, out IntPtr error) =>
                NativeMethods.DeleteLogicalPartitionAsync(
                    handle,
                    deviceSelector,
                    partitionName,
                    ref nativeOptions,
                    out operation,
                    out error));
    }

    /// <summary>Resizes a logical partition to the requested byte size.</summary>
    public Task<CommandResult> ResizeLogicalPartitionAsync(
        string partitionName,
        ulong size,
        string? deviceSelector = null,
        CommandOptions options = default,
        IProgress<CommandProgress>? progress = null,
        CancellationToken cancellationToken = default)
    {
        ValidateNativeText(partitionName, nameof(partitionName));
        ValidateSelector(deviceSelector);
        return RunCommandAsync(
            options,
            progress,
            cancellationToken,
            (ref NativeCommandOptions nativeOptions, out IntPtr operation, out IntPtr error) =>
                NativeMethods.ResizeLogicalPartitionAsync(
                    handle,
                    deviceSelector,
                    partitionName,
                    size,
                    ref nativeOptions,
                    out operation,
                    out error));
    }

    /// <summary>Reboots the selected device.</summary>
    public Task<CommandResult> RebootAsync(
        RebootTarget target = RebootTarget.System,
        string? deviceSelector = null,
        CommandOptions options = default,
        IProgress<CommandProgress>? progress = null,
        CancellationToken cancellationToken = default)
    {
        if (target < RebootTarget.System || target > RebootTarget.Fastboot)
        {
            throw new ArgumentOutOfRangeException(nameof(target));
        }

        ValidateSelector(deviceSelector);
        return RunCommandAsync(
            options,
            progress,
            cancellationToken,
            (ref NativeCommandOptions nativeOptions, out IntPtr operation, out IntPtr error) =>
                NativeMethods.RebootAsync(
                    handle,
                    deviceSelector,
                    (int)target,
                    ref nativeOptions,
                    out operation,
                    out error));
    }

    /// <summary>Continues booting the selected device.</summary>
    public Task<CommandResult> ContinueBootAsync(
        string? deviceSelector = null,
        CommandOptions options = default,
        IProgress<CommandProgress>? progress = null,
        CancellationToken cancellationToken = default)
    {
        ValidateSelector(deviceSelector);
        return RunCommandAsync(
            options,
            progress,
            cancellationToken,
            (ref NativeCommandOptions nativeOptions, out IntPtr operation, out IntPtr error) =>
                NativeMethods.ContinueBootAsync(
                    handle,
                    deviceSelector,
                    ref nativeOptions,
                    out operation,
                    out error));
    }

    /// <summary>Runs an OEM command suffix.</summary>
    public Task<CommandResult> OemAsync(
        string commandSuffix,
        string? deviceSelector = null,
        CommandOptions options = default,
        IProgress<CommandProgress>? progress = null,
        CancellationToken cancellationToken = default)
    {
        ValidateRequiredText(commandSuffix, nameof(commandSuffix));
        ValidateSelector(deviceSelector);
        return RunCommandAsync(
            options,
            progress,
            cancellationToken,
            (ref NativeCommandOptions nativeOptions, out IntPtr operation, out IntPtr error) =>
                NativeMethods.OemAsync(
                    handle,
                    deviceSelector,
                    commandSuffix,
                    ref nativeOptions,
                    out operation,
                    out error));
    }

    /// <summary>Runs a raw Fastboot command.</summary>
    public Task<CommandResult> RawCommandAsync(
        string command,
        string? deviceSelector = null,
        CommandOptions options = default,
        IProgress<CommandProgress>? progress = null,
        CancellationToken cancellationToken = default)
    {
        ValidateRequiredText(command, nameof(command));
        ValidateSelector(deviceSelector);
        return RunCommandAsync(
            options,
            progress,
            cancellationToken,
            (ref NativeCommandOptions nativeOptions, out IntPtr operation, out IntPtr error) =>
                NativeMethods.RawCommandAsync(
                    handle,
                    deviceSelector,
                    command,
                    ref nativeOptions,
                    out operation,
                    out error));
    }

    /// <summary>Boots previously downloaded or staged data.</summary>
    public Task<CommandResult> BootAsync(
        string? deviceSelector = null,
        CommandOptions options = default,
        IProgress<CommandProgress>? progress = null,
        CancellationToken cancellationToken = default)
    {
        ValidateSelector(deviceSelector);
        return RunCommandAsync(
            options,
            progress,
            cancellationToken,
            (ref NativeCommandOptions nativeOptions, out IntPtr operation, out IntPtr error) =>
                NativeMethods.BootAsync(
                    handle,
                    deviceSelector,
                    ref nativeOptions,
                    out operation,
                    out error));
    }

    /// <summary>Stages an owned managed byte-array snapshot on the device.</summary>
    public Task<CommandResult> StageAsync(
        byte[] data,
        string? deviceSelector = null,
        CommandOptions options = default,
        IProgress<CommandProgress>? progress = null,
        CancellationToken cancellationToken = default)
    {
        if (data == null)
        {
            throw new ArgumentNullException(nameof(data));
        }

        if (data.Length == 0)
        {
            throw new ArgumentException("Stage data must not be empty.", nameof(data));
        }

        ValidateSelector(deviceSelector);
        return RunCommandAsync(
            options,
            progress,
            cancellationToken,
            delegate (
                ref NativeCommandOptions nativeOptions,
                out IntPtr operation,
                out IntPtr error)
            {
                var pinned = GCHandle.Alloc(data, GCHandleType.Pinned);
                try
                {
                    return NativeMethods.StageAsync(
                        handle,
                        deviceSelector,
                        pinned.AddrOfPinnedObject(),
                        new UIntPtr((ulong)data.LongLength),
                        ref nativeOptions,
                        out operation,
                        out error);
                }
                finally
                {
                    // Native stage start snapshots the input before returning.
                    // The array is never pinned during asynchronous transport.
                    pinned.Free();
                }
            });
    }

    /// <summary>Receives bounded data from the Fastboot upload command.</summary>
    public Task<CommandResult> UploadAsync(
        string? deviceSelector = null,
        CommandOptions options = default,
        IProgress<CommandProgress>? progress = null,
        CancellationToken cancellationToken = default)
    {
        ValidateSelector(deviceSelector);
        return RunCommandAsync(
            options,
            progress,
            cancellationToken,
            (ref NativeCommandOptions nativeOptions, out IntPtr operation, out IntPtr error) =>
                NativeMethods.UploadAsync(
                    handle,
                    deviceSelector,
                    ref nativeOptions,
                    out operation,
                    out error));
    }

    /// <summary>Fetches a bounded partition range from the device.</summary>
    public Task<CommandResult> FetchAsync(
        string partition,
        ulong? offset = null,
        ulong? size = null,
        string? deviceSelector = null,
        CommandOptions options = default,
        IProgress<CommandProgress>? progress = null,
        CancellationToken cancellationToken = default)
    {
        ValidateRequiredText(partition, nameof(partition));
        ValidateSelector(deviceSelector);
        return RunCommandAsync(
            options,
            progress,
            cancellationToken,
            (ref NativeCommandOptions nativeOptions, out IntPtr operation, out IntPtr error) =>
                NativeMethods.FetchAsync(
                    handle,
                    deviceSelector,
                    partition,
                    offset ?? NativeMethods.FetchUnspecified,
                    size ?? NativeMethods.FetchUnspecified,
                    ref nativeOptions,
                    out operation,
                    out error));
    }

    /// <summary>Releases the native context.</summary>
    public void Dispose()
    {
        handle.Dispose();
    }

    private async Task FlashFileCoreAsync(
        string partition,
        string filePath,
        FlashOptions options,
        string? serial,
        IProgress<FlashProgress>? progress,
        CancellationToken cancellationToken)
    {
        ValidateRequiredText(partition, nameof(partition));
        ValidateRequiredText(filePath, nameof(filePath));
        ValidateSelector(serial, nameof(serial));

        ThrowIfDisposed();
        cancellationToken.ThrowIfCancellationRequested();

        ProgressCallbackRegistration<FlashProgress>? progressRegistration = null;
        var contextReferenceAdded = false;
        try
        {
            handle.DangerousAddRef(ref contextReferenceAdded);
            if (progress != null)
            {
                progressRegistration = new ProgressCallbackRegistration<FlashProgress>(
                    progress,
                    CreateFlashProgress);
            }

            var nativeOptions = new NativeFlashOptions();
            NativeMethods.FlashOptionsInit(ref nativeOptions);
            nativeOptions.TimeoutMilliseconds = options.NativeTimeoutMilliseconds;
            nativeOptions.ProgressCallback = progressRegistration?.CallbackPointer ?? IntPtr.Zero;
            nativeOptions.ProgressUserData = progressRegistration?.UserData ?? IntPtr.Zero;

            var status = NativeMethods.FlashFileAsync(
                handle,
                serial,
                partition,
                filePath,
                ref nativeOptions,
                out var rawOperation,
                out var rawError);

            using (var operation = TakeStartedOperation(status, rawOperation, rawError))
            {
                await OperationPollingEngine.WaitAsync(
                    new NativeOperationPollTarget(operation),
                    cancellationToken).ConfigureAwait(false);
            }
        }
        finally
        {
            progressRegistration?.Dispose();
            if (contextReferenceAdded)
            {
                handle.DangerousRelease();
            }
        }
    }

    private async Task<CommandResult> RunCommandAsync(
        CommandOptions options,
        IProgress<CommandProgress>? progress,
        CancellationToken cancellationToken,
        StartCommandOperation start)
    {
        ThrowIfDisposed();
        cancellationToken.ThrowIfCancellationRequested();

        ProgressCallbackRegistration<CommandProgress>? progressRegistration = null;
        var contextReferenceAdded = false;
        try
        {
            handle.DangerousAddRef(ref contextReferenceAdded);
            if (progress != null)
            {
                progressRegistration = new ProgressCallbackRegistration<CommandProgress>(
                    progress,
                    CreateCommandProgress);
            }

            var nativeOptions = new NativeCommandOptions();
            NativeMethods.CommandOptionsInit(ref nativeOptions);
            nativeOptions.TimeoutMilliseconds = options.NativeTimeoutMilliseconds;
            nativeOptions.MaximumReceiveBytes = options.NativeMaximumReceiveBytes;
            nativeOptions.ProgressCallback = progressRegistration?.CallbackPointer ?? IntPtr.Zero;
            nativeOptions.ProgressUserData = progressRegistration?.UserData ?? IntPtr.Zero;

            var status = start(ref nativeOptions, out var rawOperation, out var rawError);
            using (var operation = TakeStartedOperation(status, rawOperation, rawError))
            {
                await OperationPollingEngine.WaitAsync(
                    new NativeOperationPollTarget(operation),
                    cancellationToken).ConfigureAwait(false);
                return ExtractCommandResult(operation);
            }
        }
        finally
        {
            progressRegistration?.Dispose();
            if (contextReferenceAdded)
            {
                handle.DangerousRelease();
            }
        }
    }

    private static OperationSafeHandle TakeStartedOperation(
        int status,
        IntPtr rawOperation,
        IntPtr rawError)
    {
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

        ReleaseUnexpectedError(rawError);
        return new OperationSafeHandle(rawOperation);
    }

    private static CommandResult ExtractCommandResult(OperationSafeHandle operation)
    {
        var status = NativeMethods.OperationCommandResult(
            operation,
            out var rawResult,
            out var rawError);
        if (status != (int)KairosBootStatus.Ok)
        {
            if (rawResult != IntPtr.Zero)
            {
                using (var unexpectedResult = new CommandResultSafeHandle(rawResult))
                {
                }
            }

            throw NativeError.TakeException(status, rawError);
        }

        if (rawResult == IntPtr.Zero)
        {
            throw NativeError.TakeException((int)KairosBootStatus.Internal, rawError);
        }

        ReleaseUnexpectedError(rawError);
        using (var result = new CommandResultSafeHandle(rawResult))
        {
            return CommandResult.CopyFrom(new NativeCommandResultSource(result));
        }
    }

    private static void ReleaseUnexpectedError(IntPtr rawError)
    {
        if (rawError != IntPtr.Zero)
        {
            using (var unexpectedError = new ErrorSafeHandle(rawError))
            {
            }
        }
    }

    private static FlashProgress CreateFlashProgress(NativeProgress native)
    {
        return new FlashProgress(
            native.BytesCompleted,
            native.BytesTotal,
            Utf8String.FromNative(native.Stage),
            Utf8String.FromNative(native.DeviceIdentifier));
    }

    private static CommandProgress CreateCommandProgress(NativeProgress native)
    {
        return new CommandProgress(
            native.BytesCompleted,
            native.BytesTotal,
            Utf8String.FromNative(native.Stage),
            Utf8String.FromNative(native.DeviceIdentifier));
    }

    private static void ValidateRequiredText(string value, string parameterName)
    {
        if (string.IsNullOrEmpty(value))
        {
            throw new ArgumentException("Value must not be empty.", parameterName);
        }

        if (value.IndexOf('\0') >= 0)
        {
            throw new ArgumentException("Value must not contain a NUL character.", parameterName);
        }
    }

    private static void ValidateNativeText(string value, string parameterName)
    {
        // Empty, null-at-runtime, malformed, and overlong command arguments are
        // intentionally left to the stable C ABI. Embedded NUL cannot be
        // represented faithfully by its null-terminated UTF-8 contract.
        if (value != null && value.IndexOf('\0') >= 0)
        {
            throw new ArgumentException("Value must not contain a NUL character.", parameterName);
        }
    }

    private static void ValidateSelector(string? selector, string parameterName = "deviceSelector")
    {
        if (selector == null)
        {
            return;
        }

        if (selector.Length == 0)
        {
            throw new ArgumentException("Selector must be null or non-empty.", parameterName);
        }

        if (selector.IndexOf('\0') >= 0)
        {
            throw new ArgumentException(
                "Selector must not contain a NUL character.",
                parameterName);
        }
    }

    private void ThrowIfDisposed()
    {
        if (handle.IsClosed || handle.IsInvalid)
        {
            throw new ObjectDisposedException(nameof(Context));
        }
    }

    private sealed class ProgressCallbackRegistration<T> : IDisposable
    {
        private readonly NativeProgressCallback callback;
        private GCHandle targetHandle;

        internal ProgressCallbackRegistration(
            IProgress<T> progress,
            Func<NativeProgress, T> factory)
        {
            callback = ReportProgress;
            targetHandle = GCHandle.Alloc(new ProgressTarget<T>(progress, factory));
            try
            {
                CallbackPointer = Marshal.GetFunctionPointerForDelegate(callback);
                UserData = GCHandle.ToIntPtr(targetHandle);
            }
            catch
            {
                targetHandle.Free();
                throw;
            }
        }

        internal IntPtr CallbackPointer { get; }

        internal IntPtr UserData { get; }

        public void Dispose()
        {
            if (targetHandle.IsAllocated)
            {
                targetHandle.Free();
            }

            GC.KeepAlive(callback);
        }

        private static int ReportProgress(ref NativeProgress native, IntPtr userData)
        {
            try
            {
                if (native.StructSize < NativeMethods.ProgressStructSize ||
                    native.ApiVersion != NativeMethods.ApiVersion ||
                    userData == IntPtr.Zero)
                {
                    return 1;
                }

                var target = GCHandle.FromIntPtr(userData).Target as ProgressTarget<T>;
                if (target == null)
                {
                    return 1;
                }

                target.Report(native);
                return 0;
            }
            catch
            {
                return 1;
            }
        }
    }

    private sealed class ProgressTarget<T>
    {
        private readonly IProgress<T> progress;
        private readonly Func<NativeProgress, T> factory;

        internal ProgressTarget(IProgress<T> progress, Func<NativeProgress, T> factory)
        {
            this.progress = progress;
            this.factory = factory;
        }

        internal void Report(NativeProgress native)
        {
            progress.Report(factory(native));
        }
    }
}
