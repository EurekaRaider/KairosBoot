using System;

namespace KairosBoot;

/// <summary>Describes a failure reported by the KairosBoot native library.</summary>
public sealed class KairosBootException : Exception
{
    internal KairosBootException(
        KairosBootStatus status,
        string message,
        string deviceIdentifier,
        int nativeCode,
        TransferState transferState)
        : base(message)
    {
        Status = status;
        DeviceIdentifier = deviceIdentifier;
        NativeCode = nativeCode;
        TransferState = transferState;
    }

    /// <summary>Gets the stable KairosBoot status.</summary>
    public KairosBootStatus Status { get; }

    /// <summary>Gets the serial or physical identifier associated with the failure.</summary>
    public string DeviceIdentifier { get; }

    /// <summary>Gets an optional platform or libusb error code.</summary>
    public int NativeCode { get; }

    /// <summary>Gets the known transfer certainty at the point of failure.</summary>
    public TransferState TransferState { get; }
}
