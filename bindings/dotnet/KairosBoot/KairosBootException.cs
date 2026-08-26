using System;
using System.Collections.Generic;

namespace KairosBoot;

/// <summary>Describes a failure reported by the KairosBoot native library.</summary>
public sealed class KairosBootException : Exception
{
    internal KairosBootException(
        KairosBootStatus status,
        string message,
        string deviceIdentifier,
        int nativeCode,
        TransferState transferState,
        byte[] deviceMessage,
        IReadOnlyList<CommandMessage> commandMessages,
        ulong? inboundExpectedBytes,
        ulong inboundTransferredBytes,
        TransferState inboundTransferState,
        bool sessionPoisoned)
        : base(message)
    {
        Status = status;
        DeviceIdentifier = deviceIdentifier;
        NativeCode = nativeCode;
        TransferState = transferState;
        DeviceMessage = deviceMessage;
        CommandMessages = commandMessages;
        InboundExpectedBytes = inboundExpectedBytes;
        InboundTransferredBytes = inboundTransferredBytes;
        InboundTransferState = inboundTransferState;
        SessionPoisoned = sessionPoisoned;
    }

    /// <summary>Gets the stable KairosBoot status.</summary>
    public KairosBootStatus Status { get; }

    /// <summary>Gets the serial or physical identifier associated with the failure.</summary>
    public string DeviceIdentifier { get; }

    /// <summary>Gets an optional platform or libusb error code.</summary>
    public int NativeCode { get; }

    /// <summary>Gets the known transfer certainty at the point of failure.</summary>
    public TransferState TransferState { get; }

    /// <summary>Gets an owned binary copy of the terminal FAIL payload.</summary>
    public byte[] DeviceMessage { get; }

    /// <summary>Gets ordered INFO and TEXT responses observed before failure.</summary>
    public IReadOnlyList<CommandMessage> CommandMessages { get; }

    /// <summary>
    /// Gets the inbound byte count expected by the protocol, or null when the
    /// native error did not specify one.
    /// </summary>
    public ulong? InboundExpectedBytes { get; }

    /// <summary>Gets the inbound byte count received before failure.</summary>
    public ulong InboundTransferredBytes { get; }

    /// <summary>Gets the certainty of the inbound transfer.</summary>
    public TransferState InboundTransferState { get; }

    /// <summary>Gets whether the failed session must be reconnected before reuse.</summary>
    public bool SessionPoisoned { get; }
}
