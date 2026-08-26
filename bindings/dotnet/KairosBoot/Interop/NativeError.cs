using System;

namespace KairosBoot.Interop;

internal static class NativeError
{
    internal static KairosBootException TakeException(int fallbackStatus, IntPtr ownedError)
    {
        if (ownedError == IntPtr.Zero)
        {
            return FromBorrowed(fallbackStatus, IntPtr.Zero);
        }

        using (var error = new ErrorSafeHandle(ownedError))
        {
            return FromBorrowed(fallbackStatus, error.DangerousGetHandle());
        }
    }

    internal static KairosBootException FromBorrowed(int fallbackStatus, IntPtr error)
    {
        var status = error == IntPtr.Zero ? fallbackStatus : NativeMethods.ErrorStatus(error);
        var message = error == IntPtr.Zero
            ? Utf8String.FromNative(NativeMethods.StatusString(status))
            : Utf8String.FromNative(NativeMethods.ErrorMessage(error));

        return new KairosBootException(
            (KairosBootStatus)status,
            string.IsNullOrEmpty(message) ? "KairosBoot operation failed." : message,
            error == IntPtr.Zero
                ? string.Empty
                : Utf8String.FromNative(NativeMethods.ErrorDeviceIdentifier(error)),
            error == IntPtr.Zero ? 0 : NativeMethods.ErrorNativeCode(error),
            error == IntPtr.Zero
                ? TransferState.NotSent
                : (TransferState)NativeMethods.ErrorTransferState(error));
    }
}
