using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;

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

        var messages = error == IntPtr.Zero
            ? new ReadOnlyCollection<CommandMessage>(new List<CommandMessage>())
            : CopyCommandMessages(error);
        var deviceMessage = error == IntPtr.Zero
            ? Array.Empty<byte>()
            : CopyDeviceMessage(error);

        return new KairosBootException(
            (KairosBootStatus)status,
            string.IsNullOrEmpty(message) ? "KairosBoot operation failed." : message,
            error == IntPtr.Zero
                ? string.Empty
                : Utf8String.FromNative(NativeMethods.ErrorDeviceIdentifier(error)),
            error == IntPtr.Zero ? 0 : NativeMethods.ErrorNativeCode(error),
            error == IntPtr.Zero
                ? TransferState.NotSent
                : (TransferState)NativeMethods.ErrorTransferState(error),
            deviceMessage,
            messages,
            error == IntPtr.Zero ? null : CopyInboundExpectedBytes(error),
            error == IntPtr.Zero ? 0 : NativeMethods.ErrorInboundTransferredBytes(error),
            error == IntPtr.Zero
                ? TransferState.NotSent
                : (TransferState)NativeMethods.ErrorInboundTransferState(error),
            error != IntPtr.Zero && NativeMethods.ErrorSessionPoisoned(error) != 0);
    }

    private static byte[] CopyDeviceMessage(IntPtr error)
    {
        var pointer = NativeMethods.ErrorDeviceMessage(error, out var size);
        return NativeBuffer.Copy(pointer, size, "error device message");
    }

    private static IReadOnlyList<CommandMessage> CopyCommandMessages(IntPtr error)
    {
        var nativeCount = NativeMethods.ErrorCommandMessageCount(error).ToUInt64();
        if (nativeCount > int.MaxValue)
        {
            throw new InvalidOperationException(
                "Native error message count exceeds managed collection limits.");
        }

        var messages = new List<CommandMessage>((int)nativeCount);
        for (ulong index = 0; index < nativeCount; index++)
        {
            var nativeIndex = new UIntPtr(index);
            var kind = CommandMessageKindMapping.FromNative(
                NativeMethods.ErrorCommandMessageKind(error, nativeIndex),
                "error");
            var pointer = NativeMethods.ErrorCommandMessagePayload(
                error,
                nativeIndex,
                out var size);
            messages.Add(new CommandMessage(
                kind,
                NativeBuffer.Copy(pointer, size, "error command message payload")));
        }

        return new ReadOnlyCollection<CommandMessage>(messages);
    }

    private static ulong? CopyInboundExpectedBytes(IntPtr error)
    {
        var expected = NativeMethods.ErrorInboundExpectedBytes(error);
        return expected == NativeMethods.FetchUnspecified ? null : expected;
    }
}
