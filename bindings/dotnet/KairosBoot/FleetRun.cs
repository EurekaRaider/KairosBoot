using System;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using KairosBoot.Interop;

namespace KairosBoot;

/// <summary>Controls a complete Fleet manifest run.</summary>
public readonly struct JobOptions
{
    private readonly bool hasExplicitTimeout;
    private readonly TimeSpan timeout;
    private readonly uint nativeTimeoutMilliseconds;

    /// <summary>
    /// Creates job options with a whole-job deadline. Use
    /// <see cref="Timeout.InfiniteTimeSpan"/> for no deadline.
    /// </summary>
    public JobOptions(TimeSpan timeout)
    {
        nativeTimeoutMilliseconds = ToNativeMilliseconds(timeout);
        this.timeout = timeout;
        hasExplicitTimeout = true;
    }

    /// <summary>Gets options that use the native infinite deadline.</summary>
    public static JobOptions Default => default;

    /// <summary>Gets the deadline for the complete job.</summary>
    public TimeSpan Timeout => hasExplicitTimeout
        ? timeout
        : System.Threading.Timeout.InfiniteTimeSpan;

    internal uint NativeTimeoutMilliseconds => hasExplicitTimeout
        ? nativeTimeoutMilliseconds
        : uint.MaxValue;

    private static uint ToNativeMilliseconds(TimeSpan value)
    {
        if (value == System.Threading.Timeout.InfiniteTimeSpan)
        {
            return uint.MaxValue;
        }

        if (value < TimeSpan.Zero)
        {
            throw new ArgumentOutOfRangeException(
                nameof(value), value, "Job timeout must be non-negative or infinite.");
        }

        var milliseconds = value.Ticks / TimeSpan.TicksPerMillisecond;
        if (value.Ticks % TimeSpan.TicksPerMillisecond != 0)
        {
            milliseconds++;
        }

        if ((ulong)milliseconds >= uint.MaxValue)
        {
            throw new ArgumentOutOfRangeException(
                nameof(value), value,
                "Finite job timeout must be shorter than UInt32.MaxValue milliseconds.");
        }

        return (uint)milliseconds;
    }
}

/// <summary>Progress reported by a Fleet job.</summary>
public sealed class JobProgress
{
    internal JobProgress(
        ulong bytesCompleted,
        ulong bytesTotal,
        string stage,
        string deviceIdentifier)
    {
        BytesCompleted = bytesCompleted;
        BytesTotal = bytesTotal;
        Stage = stage;
        DeviceIdentifier = deviceIdentifier;
    }

    /// <summary>Gets the completed byte count for the current device stage.</summary>
    public ulong BytesCompleted { get; }

    /// <summary>Gets the total byte count when known.</summary>
    public ulong BytesTotal { get; }

    /// <summary>Gets the current job stage.</summary>
    public string Stage { get; }

    /// <summary>Gets the device associated with the event.</summary>
    public string DeviceIdentifier { get; }
}

/// <summary>An owned Fleet report whose JSON is independent of its job.</summary>
public sealed class JobReport : IDisposable
{
    private readonly JobReportSafeHandle handle;

    internal JobReport(JobReportSafeHandle handle)
    {
        this.handle = handle;
    }

    /// <summary>Gets an owned copy of the canonical report JSON.</summary>
    public string Json
    {
        get
        {
            ThrowIfDisposed();
            var pointer = NativeMethods.JobReportJson(handle, out var size);
            var bytes = NativeBuffer.Copy(pointer, size, "fleet job report JSON");
            return bytes.Length == 0 ? string.Empty : Encoding.UTF8.GetString(bytes);
        }
    }

    /// <summary>Releases the native report.</summary>
    public void Dispose()
    {
        handle.Dispose();
    }

    private void ThrowIfDisposed()
    {
        if (handle.IsClosed || handle.IsInvalid)
        {
            throw new ObjectDisposedException(nameof(JobReport));
        }
    }
}

/// <summary>
/// Owns one native Fleet job. Cancellation and disposal retain callback and
/// context state until the native worker has drained.
/// </summary>
public sealed class FleetJob : IDisposable
{
    private readonly object gate = new object();
    private readonly JobSafeHandle handle;
    private readonly ContextSafeHandle context;
    private IDisposable? progressRegistration;
    private int activeCalls;
    private bool disposeRequested;
    private bool resourcesReleased;

    internal FleetJob(
        JobSafeHandle handle,
        ContextSafeHandle context,
        IDisposable? progressRegistration)
    {
        this.handle = handle;
        this.context = context;
        this.progressRegistration = progressRegistration;
    }

    /// <summary>Releases an abandoned native job after requesting cancellation.</summary>
    ~FleetJob()
    {
        Dispose();
    }

    /// <summary>Gets the current native job lifecycle state.</summary>
    public OperationState State
    {
        get
        {
            using (EnterCall())
            {
                return (OperationState)NativeMethods.JobState(handle);
            }
        }
    }

    /// <summary>Gets an owned snapshot of the terminal native error, if any.</summary>
    public KairosBootException? Error
    {
        get
        {
            using (EnterCall())
            {
                var state = (OperationState)NativeMethods.JobState(handle);
                var error = NativeMethods.JobError(handle);
                if (error == IntPtr.Zero)
                {
                    return null;
                }

                var fallback = state == OperationState.Cancelled
                    ? KairosBootStatus.Cancelled
                    : KairosBootStatus.Internal;
                return NativeError.FromBorrowed((int)fallback, error);
            }
        }
    }

    /// <summary>Requests cancellation. The native job remains owned until disposed.</summary>
    public void Cancel()
    {
        using (EnterCall())
        {
            var status = NativeMethods.JobCancel(handle);
            if (status != (int)KairosBootStatus.Ok)
            {
                throw NativeError.FromBorrowed(status, NativeMethods.JobError(handle));
            }
        }
    }

    /// <summary>Blocks until terminal state and returns an independent report.</summary>
    public JobReport Wait()
    {
        using (EnterCall())
        {
            var status = NativeMethods.JobWait(handle, uint.MaxValue);
            if (status != (int)KairosBootStatus.Ok)
            {
                throw NativeError.FromBorrowed(status, NativeMethods.JobError(handle));
            }

            return ExtractReport();
        }
    }

    /// <summary>
    /// Asynchronously waits for terminal state. Token cancellation is forwarded
    /// once, then polling continues until native callback and transport drain.
    /// </summary>
    public async Task<JobReport> WaitAsync(CancellationToken cancellationToken = default)
    {
        using (EnterCall())
        {
            await OperationPollingEngine.WaitAsync(
                new NativeJobPollTarget(handle), cancellationToken).ConfigureAwait(false);
            return ExtractReport();
        }
    }

    /// <summary>Returns an independent terminal report without waiting.</summary>
    public JobReport GetReport()
    {
        using (EnterCall())
        {
            return ExtractReport();
        }
    }

    /// <summary>
    /// Cancels a running job and releases it after any concurrent managed wait
    /// leaves the native polling call. Repeated disposal is safe.
    /// </summary>
    public void Dispose()
    {
        GC.SuppressFinalize(this);
        lock (gate)
        {
            if (disposeRequested)
            {
                return;
            }

            disposeRequested = true;
            activeCalls++;
        }

        try
        {
            _ = NativeMethods.JobCancel(handle);
        }
        catch
        {
            // Release still drains the native worker and owns final cleanup.
        }
        finally
        {
            ExitCall();
        }
    }

    private JobReport ExtractReport()
    {
        var status = NativeMethods.JobGetReport(handle, out var rawReport, out var rawError);
        if (status != (int)KairosBootStatus.Ok)
        {
            if (rawReport != IntPtr.Zero)
            {
                using (var unexpectedReport = new JobReportSafeHandle(rawReport))
                {
                }
            }

            throw NativeError.TakeException(status, rawError);
        }

        if (rawReport == IntPtr.Zero)
        {
            throw NativeError.TakeException((int)KairosBootStatus.Internal, rawError);
        }

        ReleaseUnexpectedError(rawError);
        return new JobReport(new JobReportSafeHandle(rawReport));
    }

    private IDisposable EnterCall()
    {
        lock (gate)
        {
            if (disposeRequested || resourcesReleased)
            {
                throw new ObjectDisposedException(nameof(FleetJob));
            }

            activeCalls++;
            return new CallLease(this);
        }
    }

    private void ExitCall()
    {
        var release = false;
        lock (gate)
        {
            activeCalls--;
            if (activeCalls == 0 && disposeRequested && !resourcesReleased)
            {
                resourcesReleased = true;
                release = true;
            }
        }

        if (release)
        {
            handle.Dispose();
            progressRegistration?.Dispose();
            progressRegistration = null;
            context.DangerousRelease();
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

    private sealed class CallLease : IDisposable
    {
        private FleetJob? owner;

        internal CallLease(FleetJob owner)
        {
            this.owner = owner;
        }

        public void Dispose()
        {
            var current = Interlocked.Exchange(ref owner, null);
            current?.ExitCall();
        }
    }
}

public sealed partial class Context
{
    /// <summary>Starts a Fleet job with native default options.</summary>
    public FleetJob StartJobFile(
        string filePath,
        IProgress<JobProgress>? progress = null)
    {
        return StartJobFile(filePath, JobOptions.Default, progress);
    }

    /// <summary>Starts a Fleet job and returns its owned native lifecycle.</summary>
    public FleetJob StartJobFile(
        string filePath,
        JobOptions options,
        IProgress<JobProgress>? progress = null)
    {
        ValidateRequiredText(filePath, nameof(filePath));
        ThrowIfDisposed();

        JobProgressCallbackRegistration? registration = null;
        var contextReferenceAdded = false;
        try
        {
            handle.DangerousAddRef(ref contextReferenceAdded);
            if (progress != null)
            {
                registration = new JobProgressCallbackRegistration(progress);
            }

            var nativeOptions = CreateNativeJobOptions(options, registration);
            var status = NativeMethods.RunJobFileAsync(
                handle,
                filePath,
                ref nativeOptions,
                out var rawJob,
                out var rawError);
            if (status != (int)KairosBootStatus.Ok)
            {
                if (rawJob != IntPtr.Zero)
                {
                    using (var unexpectedJob = new JobSafeHandle(rawJob))
                    {
                    }
                }

                throw NativeError.TakeException(status, rawError);
            }

            if (rawJob == IntPtr.Zero)
            {
                throw NativeError.TakeException((int)KairosBootStatus.Internal, rawError);
            }

            ReleaseUnexpectedError(rawError);
            var job = new FleetJob(new JobSafeHandle(rawJob), handle, registration);
            registration = null;
            contextReferenceAdded = false;
            return job;
        }
        finally
        {
            registration?.Dispose();
            if (contextReferenceAdded)
            {
                handle.DangerousRelease();
            }
        }
    }

    /// <summary>Runs a Fleet job synchronously through the blocking C ABI.</summary>
    public JobReport RunJobFile(
        string filePath,
        JobOptions options = default,
        IProgress<JobProgress>? progress = null)
    {
        ValidateRequiredText(filePath, nameof(filePath));
        ThrowIfDisposed();

        JobProgressCallbackRegistration? registration = null;
        var contextReferenceAdded = false;
        try
        {
            handle.DangerousAddRef(ref contextReferenceAdded);
            if (progress != null)
            {
                registration = new JobProgressCallbackRegistration(progress);
            }

            var nativeOptions = CreateNativeJobOptions(options, registration);
            var status = NativeMethods.RunJobFile(
                handle,
                filePath,
                ref nativeOptions,
                out var rawReport,
                out var rawError);
            if (status != (int)KairosBootStatus.Ok)
            {
                if (rawReport != IntPtr.Zero)
                {
                    using (var terminalReport = new JobReportSafeHandle(rawReport))
                    {
                    }
                }

                throw NativeError.TakeException(status, rawError);
            }

            if (rawReport == IntPtr.Zero)
            {
                throw NativeError.TakeException((int)KairosBootStatus.Internal, rawError);
            }

            ReleaseUnexpectedError(rawError);
            return new JobReport(new JobReportSafeHandle(rawReport));
        }
        finally
        {
            registration?.Dispose();
            if (contextReferenceAdded)
            {
                handle.DangerousRelease();
            }
        }
    }

    /// <summary>Runs a Fleet job asynchronously and returns its independent report.</summary>
    public async Task<JobReport> RunJobFileAsync(
        string filePath,
        JobOptions options = default,
        IProgress<JobProgress>? progress = null,
        CancellationToken cancellationToken = default)
    {
        cancellationToken.ThrowIfCancellationRequested();
        using (var job = StartJobFile(filePath, options, progress))
        {
            return await job.WaitAsync(cancellationToken).ConfigureAwait(false);
        }
    }

    private static NativeJobOptions CreateNativeJobOptions(
        JobOptions options,
        JobProgressCallbackRegistration? registration)
    {
        var native = new NativeJobOptions();
        NativeMethods.JobOptionsInit(ref native);
        native.TimeoutMilliseconds = options.NativeTimeoutMilliseconds;
        native.ProgressCallback = registration?.CallbackPointer ?? IntPtr.Zero;
        native.ProgressUserData = registration?.UserData ?? IntPtr.Zero;
        return native;
    }
}

internal sealed class JobProgressCallbackRegistration : IDisposable
{
    private readonly NativeProgressCallback callback;
    private GCHandle targetHandle;

    internal JobProgressCallbackRegistration(IProgress<JobProgress> progress)
    {
        callback = ReportProgress;
        targetHandle = GCHandle.Alloc(progress);
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

            var progress = GCHandle.FromIntPtr(userData).Target as IProgress<JobProgress>;
            if (progress == null)
            {
                return 1;
            }

            progress.Report(new JobProgress(
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
}
