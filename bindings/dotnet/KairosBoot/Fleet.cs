using System;
using System.Text;
using KairosBoot.Interop;

namespace KairosBoot;

/// <summary>
/// Context-free Fleet manifest entry points. Neither entry initializes USB
/// transport, enumerates devices, or opens artifact paths; only the manifest
/// file itself is read.
/// </summary>
public static class Fleet
{
    /// <summary>
    /// Validates a Fleet manifest file through full semantic validation.
    /// Failures throw <see cref="KairosBootException"/> whose message embeds
    /// the manifest source path and, when known, its line and column, and
    /// whose <see cref="KairosBootException.NativeCode"/> preserves the
    /// platform native code.
    /// </summary>
    /// <param name="filePath">Path of the manifest file to validate.</param>
    /// <exception cref="ArgumentException">
    /// <paramref name="filePath"/> is null, empty, or contains a NUL character.
    /// </exception>
    /// <exception cref="KairosBootException">The manifest failed validation.</exception>
    public static void ValidateJobFile(string filePath)
    {
        ValidateRequiredText(filePath, nameof(filePath));

        var status = NativeMethods.ValidateJobFile(filePath, out var rawError);
        if (status != (int)KairosBootStatus.Ok)
        {
            throw NativeError.TakeException(status, rawError);
        }

        ReleaseUnexpectedError(rawError);
    }

    /// <summary>
    /// Plans a Fleet manifest file into an immutable owned snapshot without
    /// initializing USB transport or opening artifact paths.
    /// </summary>
    /// <param name="filePath">Path of the manifest file to plan.</param>
    /// <returns>An owned plan snapshot that must be disposed by the caller.</returns>
    /// <exception cref="ArgumentException">
    /// <paramref name="filePath"/> is null, empty, or contains a NUL character.
    /// </exception>
    /// <exception cref="KairosBootException">The manifest failed validation.</exception>
    public static JobPlan PlanJobFile(string filePath)
    {
        ValidateRequiredText(filePath, nameof(filePath));

        var status = NativeMethods.PlanJobFile(filePath, out var rawPlan, out var rawError);
        if (status != (int)KairosBootStatus.Ok)
        {
            if (rawPlan != IntPtr.Zero)
            {
                using (var unexpectedPlan = new JobPlanSafeHandle(rawPlan))
                {
                }
            }

            throw NativeError.TakeException(status, rawError);
        }

        if (rawPlan == IntPtr.Zero)
        {
            throw NativeError.TakeException((int)KairosBootStatus.Internal, rawError);
        }

        ReleaseUnexpectedError(rawError);
        return new JobPlan(new JobPlanSafeHandle(rawPlan));
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

    private static void ReleaseUnexpectedError(IntPtr rawError)
    {
        if (rawError != IntPtr.Zero)
        {
            using (var unexpectedError = new ErrorSafeHandle(rawError))
            {
            }
        }
    }
}

/// <summary>An immutable planned Fleet job snapshot owned by the caller.</summary>
public sealed class JobPlan : IDisposable
{
    private readonly JobPlanSafeHandle handle;

    internal JobPlan(JobPlanSafeHandle handle)
    {
        this.handle = handle;
    }

    /// <summary>
    /// Gets an owned copy of the plan's canonical SDK JSON form. The native
    /// borrow is NUL-terminated UTF-8 without a trailing LF, and its size
    /// excludes the terminator; the returned string is independent of the
    /// native snapshot's lifetime.
    /// </summary>
    public string CanonicalJson
    {
        get
        {
            ThrowIfDisposed();
            var pointer = NativeMethods.JobPlanCanonicalJson(handle, out var size);
            var bytes = NativeBuffer.Copy(pointer, size, "job plan canonical JSON");
            return bytes.Length == 0 ? string.Empty : Encoding.UTF8.GetString(bytes);
        }
    }

    /// <summary>
    /// Gets the lowercase hexadecimal SHA-256 digest of the canonical JSON
    /// bytes.
    /// </summary>
    public string Sha256Hex
    {
        get
        {
            ThrowIfDisposed();
            return Utf8String.FromNative(NativeMethods.JobPlanSha256Hex(handle));
        }
    }

    /// <summary>Releases the native plan snapshot.</summary>
    public void Dispose()
    {
        handle.Dispose();
    }

    private void ThrowIfDisposed()
    {
        if (handle.IsClosed || handle.IsInvalid)
        {
            throw new ObjectDisposedException(nameof(JobPlan));
        }
    }
}
