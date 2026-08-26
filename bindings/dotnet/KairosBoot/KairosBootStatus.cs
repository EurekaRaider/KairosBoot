namespace KairosBoot;

/// <summary>Stable status values returned by the native KairosBoot ABI.</summary>
public enum KairosBootStatus
{
    /// <summary>The operation completed successfully.</summary>
    Ok = 0,
    /// <summary>An argument or versioned structure was invalid.</summary>
    InvalidArgument = 1,
    /// <summary>The native library could not allocate required memory.</summary>
    OutOfMemory = 2,
    /// <summary>The requested capability is not available in this build.</summary>
    NotSupported = 3,
    /// <summary>No matching Fastboot device was found.</summary>
    NoDevice = 4,
    /// <summary>More than one device matched an implicit selection.</summary>
    AmbiguousDevice = 5,
    /// <summary>The selected session is already running an operation.</summary>
    Busy = 6,
    /// <summary>The operation did not complete before its timeout.</summary>
    Timeout = 7,
    /// <summary>The operation was cancelled.</summary>
    Cancelled = 8,
    /// <summary>A transport or filesystem I/O operation failed.</summary>
    Io = 9,
    /// <summary>An unexpected internal invariant failed.</summary>
    Internal = 10,
}

/// <summary>How much of a failed transfer is known to have reached the device.</summary>
public enum TransferState
{
    /// <summary>No payload bytes are known to have been sent.</summary>
    NotSent = 0,
    /// <summary>Some bytes may have been sent, but the final offset is unknown.</summary>
    PartialOrUnknown = 1,
    /// <summary>The entire payload is known to have reached the transport.</summary>
    FullyTransferred = 2,
}

/// <summary>Lifecycle state of a native asynchronous operation.</summary>
public enum OperationState
{
    /// <summary>The operation has been created but has not started.</summary>
    Created = 0,
    /// <summary>The operation is running.</summary>
    Running = 1,
    /// <summary>The operation completed successfully.</summary>
    Succeeded = 2,
    /// <summary>The operation completed with an error.</summary>
    Failed = 3,
    /// <summary>The operation was cancelled.</summary>
    Cancelled = 4,
}
