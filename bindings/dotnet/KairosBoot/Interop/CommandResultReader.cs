using System;

namespace KairosBoot.Interop;

internal static class CommandMessageKindMapping
{
    internal static CommandMessageKind FromNative(int value, string source)
    {
        if (value == (int)CommandMessageKind.Info ||
            value == (int)CommandMessageKind.Text)
        {
            return (CommandMessageKind)value;
        }

        throw new InvalidOperationException(
            $"Native {source} returned unknown command message kind {value}.");
    }
}

internal interface ICommandResultSource
{
    IntPtr TerminalPayload(out UIntPtr size);

    UIntPtr MessageCount();

    int MessageKind(UIntPtr index);

    IntPtr MessagePayload(UIntPtr index, out UIntPtr size);

    IntPtr Data(out UIntPtr size);

    IntPtr DeviceIdentifier();
}

internal sealed class NativeCommandResultSource : ICommandResultSource
{
    private readonly CommandResultSafeHandle result;

    internal NativeCommandResultSource(CommandResultSafeHandle result)
    {
        this.result = result;
    }

    public IntPtr TerminalPayload(out UIntPtr size)
    {
        return NativeMethods.CommandResultTerminalPayload(result, out size);
    }

    public UIntPtr MessageCount()
    {
        return NativeMethods.CommandResultMessageCount(result);
    }

    public int MessageKind(UIntPtr index)
    {
        return NativeMethods.CommandResultMessageKind(result, index);
    }

    public IntPtr MessagePayload(UIntPtr index, out UIntPtr size)
    {
        return NativeMethods.CommandResultMessagePayload(result, index, out size);
    }

    public IntPtr Data(out UIntPtr size)
    {
        return NativeMethods.CommandResultData(result, out size);
    }

    public IntPtr DeviceIdentifier()
    {
        return NativeMethods.CommandResultDeviceIdentifier(result);
    }
}
