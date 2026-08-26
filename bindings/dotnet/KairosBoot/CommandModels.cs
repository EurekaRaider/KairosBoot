using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using KairosBoot.Interop;

namespace KairosBoot;

/// <summary>Identifies a non-terminal Fastboot response.</summary>
public enum CommandMessageKind
{
    /// <summary>An INFO response.</summary>
    Info = 0,
    /// <summary>A TEXT response.</summary>
    Text = 1,
}

/// <summary>Selects the destination of a Fastboot reboot command.</summary>
public enum RebootTarget
{
    /// <summary>Reboot to Android.</summary>
    System = 0,
    /// <summary>Reboot to the bootloader Fastboot implementation.</summary>
    Bootloader = 1,
    /// <summary>Reboot to recovery.</summary>
    Recovery = 2,
    /// <summary>Reboot to userspace Fastboot (fastbootd).</summary>
    Fastboot = 3,
}

/// <summary>Controls a typed Fastboot command.</summary>
public readonly struct CommandOptions
{
    /// <summary>The native default hard limit for upload and fetch results.</summary>
    public const ulong DefaultMaximumReceiveBytes = 64UL * 1024UL * 1024UL;

    private readonly bool initialized;
    private readonly TimeSpan timeout;
    private readonly uint nativeTimeoutMilliseconds;
    private readonly ulong maximumReceiveBytes;

    /// <summary>Creates options with a timeout and the default 64 MiB receive bound.</summary>
    public CommandOptions(TimeSpan timeout)
        : this(timeout, DefaultMaximumReceiveBytes)
    {
    }

    /// <summary>Creates options with a per-I/O timeout and hard receive bound.</summary>
    public CommandOptions(TimeSpan timeout, ulong maximumReceiveBytes)
    {
        if (maximumReceiveBytes == 0 || maximumReceiveBytes > int.MaxValue)
        {
            throw new ArgumentOutOfRangeException(
                nameof(maximumReceiveBytes),
                maximumReceiveBytes,
                "The receive bound must be positive and fit in a managed byte array.");
        }

        nativeTimeoutMilliseconds = ToNativeMilliseconds(timeout);
        this.timeout = timeout;
        this.maximumReceiveBytes = maximumReceiveBytes;
        initialized = true;
    }

    /// <summary>Gets options using an infinite timeout and a 64 MiB receive bound.</summary>
    public static CommandOptions Default => default;

    /// <summary>Gets the per-I/O timeout.</summary>
    public TimeSpan Timeout => initialized
        ? timeout
        : System.Threading.Timeout.InfiniteTimeSpan;

    /// <summary>Gets the hard in-memory bound applied to upload and fetch.</summary>
    public ulong MaximumReceiveBytes => initialized
        ? maximumReceiveBytes
        : DefaultMaximumReceiveBytes;

    internal uint NativeTimeoutMilliseconds => initialized
        ? nativeTimeoutMilliseconds
        : uint.MaxValue;

    internal ulong NativeMaximumReceiveBytes => initialized
        ? maximumReceiveBytes
        : DefaultMaximumReceiveBytes;

    private static uint ToNativeMilliseconds(TimeSpan timeout)
    {
        if (timeout == System.Threading.Timeout.InfiniteTimeSpan)
        {
            return uint.MaxValue;
        }

        if (timeout < TimeSpan.Zero)
        {
            throw new ArgumentOutOfRangeException(
                nameof(timeout),
                timeout,
                "Command timeout must be non-negative or Timeout.InfiniteTimeSpan.");
        }

        var milliseconds = timeout.Ticks / TimeSpan.TicksPerMillisecond;
        if (timeout.Ticks % TimeSpan.TicksPerMillisecond != 0)
        {
            milliseconds++;
        }

        if ((ulong)milliseconds >= uint.MaxValue)
        {
            throw new ArgumentOutOfRangeException(
                nameof(timeout),
                timeout,
                "Finite command timeout must be shorter than UInt32.MaxValue milliseconds.");
        }

        return (uint)milliseconds;
    }
}

/// <summary>A binary-safe INFO or TEXT response.</summary>
public sealed class CommandMessage
{
    internal CommandMessage(CommandMessageKind kind, byte[] payload)
    {
        Kind = kind;
        Payload = payload;
    }

    /// <summary>Gets whether this is an INFO or TEXT response.</summary>
    public CommandMessageKind Kind { get; }

    /// <summary>Gets an owned copy of the binary response payload.</summary>
    public byte[] Payload { get; }
}

/// <summary>Owned snapshot of a successful Fastboot command.</summary>
public sealed class CommandResult
{
    private CommandResult(
        byte[] terminalPayload,
        IReadOnlyList<CommandMessage> messages,
        byte[] data,
        string deviceIdentifier)
    {
        TerminalPayload = terminalPayload;
        Messages = messages;
        Data = data;
        DeviceIdentifier = deviceIdentifier;
    }

    /// <summary>Gets an owned binary copy of the terminal OKAY payload.</summary>
    public byte[] TerminalPayload { get; }

    /// <summary>Gets ordered INFO and TEXT responses.</summary>
    public IReadOnlyList<CommandMessage> Messages { get; }

    /// <summary>Gets the owned upload/fetch data, or an empty array.</summary>
    public byte[] Data { get; }

    /// <summary>Gets the resolved device identifier.</summary>
    public string DeviceIdentifier { get; }

    internal static CommandResult CopyFrom(ICommandResultSource source)
    {
        var terminalPointer = source.TerminalPayload(out var terminalSize);
        var terminal = NativeBuffer.Copy(
            terminalPointer,
            terminalSize,
            "command terminal payload");

        var nativeCount = source.MessageCount().ToUInt64();
        if (nativeCount > int.MaxValue)
        {
            throw new InvalidOperationException(
                "Native command message count exceeds managed collection limits.");
        }

        var messages = new List<CommandMessage>((int)nativeCount);
        for (ulong index = 0; index < nativeCount; index++)
        {
            var nativeIndex = new UIntPtr(index);
            var kind = CommandMessageKindMapping.FromNative(
                source.MessageKind(nativeIndex),
                "command result");
            var pointer = source.MessagePayload(nativeIndex, out var size);
            messages.Add(new CommandMessage(
                kind,
                NativeBuffer.Copy(pointer, size, "command message payload")));
        }

        var dataPointer = source.Data(out var dataSize);
        return new CommandResult(
            terminal,
            new ReadOnlyCollection<CommandMessage>(messages),
            NativeBuffer.Copy(dataPointer, dataSize, "command data"),
            Utf8String.FromNative(source.DeviceIdentifier()));
    }
}

/// <summary>Progress reported while a typed command transfers payload data.</summary>
public sealed class CommandProgress
{
    internal CommandProgress(
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

    /// <summary>Gets the completed byte count.</summary>
    public ulong BytesCompleted { get; }

    /// <summary>Gets the total byte count when known.</summary>
    public ulong BytesTotal { get; }

    /// <summary>Gets the current operation stage.</summary>
    public string Stage { get; }

    /// <summary>Gets the resolved device identifier.</summary>
    public string DeviceIdentifier { get; }
}
