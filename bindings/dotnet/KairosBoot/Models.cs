using System;

namespace KairosBoot;

/// <summary>Runtime and ABI version of the loaded KairosBoot native library.</summary>
public sealed class KairosBootVersion
{
    internal KairosBootVersion(uint major, uint minor, uint patch, uint apiVersion, string value)
    {
        Major = major;
        Minor = minor;
        Patch = patch;
        ApiVersion = apiVersion;
        Value = value;
    }

    /// <summary>Gets the semantic major version.</summary>
    public uint Major { get; }

    /// <summary>Gets the semantic minor version.</summary>
    public uint Minor { get; }

    /// <summary>Gets the semantic patch version.</summary>
    public uint Patch { get; }

    /// <summary>Gets the stable C ABI version.</summary>
    public uint ApiVersion { get; }

    /// <summary>Gets the complete native version string, including prerelease data.</summary>
    public string Value { get; }

    /// <summary>Returns the complete native version string.</summary>
    public override string ToString() => Value;
}

/// <summary>Snapshot of a Fastboot device returned by enumeration.</summary>
public sealed class Device
{
    internal Device(string serial, string usbPath, string product)
    {
        Serial = serial;
        UsbPath = usbPath;
        Product = product;
    }

    /// <summary>Gets the Fastboot serial, or an empty string when unavailable.</summary>
    public string Serial { get; }

    /// <summary>Gets the stable physical USB path, or an empty string when unavailable.</summary>
    public string UsbPath { get; }

    /// <summary>Gets the reported product, or an empty string when unavailable.</summary>
    public string Product { get; }
}

/// <summary>Progress reported while transferring or flashing an artifact.</summary>
public sealed class FlashProgress
{
    internal FlashProgress(
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

    /// <summary>Gets the device associated with this progress event.</summary>
    public string DeviceIdentifier { get; }
}
