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

/// <summary>Controls a single file flash operation.</summary>
public readonly struct FlashOptions
{
    private readonly bool hasExplicitTimeout;
    private readonly TimeSpan timeout;
    private readonly uint nativeTimeoutMilliseconds;

    /// <summary>
    /// Creates flash options with a per-I/O timeout. Use
    /// <see cref="System.Threading.Timeout.InfiniteTimeSpan"/> for no timeout.
    /// Finite sub-millisecond values are rounded up so the native deadline is
    /// never shorter than requested.
    /// </summary>
    /// <param name="timeout">
    /// A non-negative duration shorter than <see cref="uint.MaxValue"/>
    /// milliseconds, or <see cref="System.Threading.Timeout.InfiniteTimeSpan"/>.
    /// </param>
    /// <exception cref="ArgumentOutOfRangeException">
    /// <paramref name="timeout"/> cannot be represented by the native ABI.
    /// </exception>
    public FlashOptions(TimeSpan timeout)
        : this(timeout, false, false)
    {
    }

    /// <summary>Creates flash options with AOSP-compatible vbmeta flags.</summary>
    public FlashOptions(
        TimeSpan timeout,
        bool disableVerity,
        bool disableVerification,
        string? slot = null,
        bool setActive = false,
        string? activeSlot = null)
    {
        nativeTimeoutMilliseconds = ToNativeMilliseconds(timeout);
        this.timeout = timeout;
        hasExplicitTimeout = true;
        DisableVerity = disableVerity;
        DisableVerification = disableVerification;
        Slot = slot;
        SetActive = setActive;
        ActiveSlot = activeSlot;
    }

    /// <summary>Creates flash options with an A/B slot policy.</summary>
    public FlashOptions(
        TimeSpan timeout,
        string? slot,
        bool setActive = false,
        string? activeSlot = null)
        : this(timeout, false, false, slot, setActive, activeSlot)
    {
    }

    /// <summary>Gets options that use the native infinite timeout default.</summary>
    public static FlashOptions Default => default;

    /// <summary>Gets the per-I/O timeout.</summary>
    public TimeSpan Timeout => hasExplicitTimeout
        ? timeout
        : System.Threading.Timeout.InfiniteTimeSpan;

    /// <summary>Gets whether vbmeta disables dm-verity.</summary>
    public bool DisableVerity { get; }

    /// <summary>Gets whether vbmeta disables verification.</summary>
    public bool DisableVerification { get; }

    /// <summary>Gets the requested slot, or null for the device default.</summary>
    public string? Slot { get; }

    /// <summary>Gets whether set_active is issued before flash tasks.</summary>
    public bool SetActive { get; }

    /// <summary>Gets the explicit set_active slot, or null to derive it.</summary>
    public string? ActiveSlot { get; }

    internal uint NativeTimeoutMilliseconds => hasExplicitTimeout
        ? nativeTimeoutMilliseconds
        : uint.MaxValue;

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
                "Flash timeout must be non-negative or Timeout.InfiniteTimeSpan.");
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
                "Finite flash timeout must be shorter than UInt32.MaxValue milliseconds.");
        }

        return (uint)milliseconds;
    }
}

/// <summary>Controls the layout of a legacy Android boot header v0 image.</summary>
public readonly struct LegacyBootOptions
{
    private const uint DefaultBaseAddress = 0x10000000U;
    private const uint DefaultPageSize = 2048U;
    private const uint DefaultKernelOffset = 0x00008000U;
    private const uint DefaultRamdiskOffset = 0x01000000U;
    private const uint DefaultSecondOffset = 0x00f00000U;
    private const uint DefaultTagsOffset = 0x00000100U;

    private readonly bool hasExplicitValues;
    private readonly string? commandLine;
    private readonly uint baseAddress;
    private readonly uint pageSize;
    private readonly uint kernelOffset;
    private readonly uint ramdiskOffset;
    private readonly uint secondOffset;
    private readonly uint tagsOffset;

    /// <summary>Creates a legacy Android boot header v0 layout.</summary>
    /// <exception cref="ArgumentNullException">
    /// <paramref name="commandLine"/> is <see langword="null"/>.
    /// </exception>
    /// <exception cref="ArgumentException">
    /// <paramref name="commandLine"/> contains a NUL character.
    /// </exception>
    public LegacyBootOptions(
        string commandLine = "",
        uint baseAddress = DefaultBaseAddress,
        uint pageSize = DefaultPageSize,
        uint kernelOffset = DefaultKernelOffset,
        uint ramdiskOffset = DefaultRamdiskOffset,
        uint secondOffset = DefaultSecondOffset,
        uint tagsOffset = DefaultTagsOffset)
    {
        if (commandLine == null)
        {
            throw new ArgumentNullException(nameof(commandLine));
        }
        if (commandLine.IndexOf('\0') >= 0)
        {
            throw new ArgumentException(
                "Legacy boot command line must not contain a NUL character.",
                nameof(commandLine));
        }

        this.commandLine = commandLine;
        this.baseAddress = baseAddress;
        this.pageSize = pageSize;
        this.kernelOffset = kernelOffset;
        this.ramdiskOffset = ramdiskOffset;
        this.secondOffset = secondOffset;
        this.tagsOffset = tagsOffset;
        hasExplicitValues = true;
    }

    /// <summary>Gets the AOSP-compatible legacy boot defaults.</summary>
    public static LegacyBootOptions Default => default;

    /// <summary>Gets the kernel command line.</summary>
    public string CommandLine => hasExplicitValues ? commandLine! : string.Empty;

    /// <summary>Gets the physical base address.</summary>
    public uint BaseAddress => hasExplicitValues ? baseAddress : DefaultBaseAddress;

    /// <summary>Gets the boot image page size.</summary>
    public uint PageSize => hasExplicitValues ? pageSize : DefaultPageSize;

    /// <summary>Gets the kernel offset from the base address.</summary>
    public uint KernelOffset => hasExplicitValues ? kernelOffset : DefaultKernelOffset;

    /// <summary>Gets the ramdisk offset from the base address.</summary>
    public uint RamdiskOffset => hasExplicitValues ? ramdiskOffset : DefaultRamdiskOffset;

    /// <summary>Gets the second-stage offset from the base address.</summary>
    public uint SecondOffset => hasExplicitValues ? secondOffset : DefaultSecondOffset;

    /// <summary>Gets the tags offset from the base address.</summary>
    public uint TagsOffset => hasExplicitValues ? tagsOffset : DefaultTagsOffset;
}

/// <summary>Controls a complete update-package operation.</summary>
public readonly struct UpdateOptions
{
    private readonly bool hasExplicitTimeout;
    private readonly TimeSpan timeout;
    private readonly uint nativeTimeoutMilliseconds;

    /// <summary>
    /// Creates update options with a whole-operation timeout and optional data
    /// wipe. Use <see cref="System.Threading.Timeout.InfiniteTimeSpan"/> for no
    /// deadline. Finite sub-millisecond values are rounded up so the native
    /// deadline is never shorter than requested.
    /// </summary>
    /// <param name="timeout">
    /// A non-negative duration shorter than <see cref="uint.MaxValue"/>
    /// milliseconds, or <see cref="System.Threading.Timeout.InfiniteTimeSpan"/>.
    /// </param>
    /// <param name="wipe">
    /// Whether wipe-conditioned package tasks may erase user data.
    /// </param>
    /// <param name="skipReboot">Whether to omit the final system reboot.</param>
    /// <param name="skipSecondary">Whether to omit secondary-slot flashes.</param>
    /// <param name="excludeDynamicPartitions">
    /// Whether to exclude partitions reported as logical by the device.
    /// </param>
    /// <param name="disableFastbootInfo">
    /// Whether to use the frozen image-list plan instead of fastboot-info.txt.
    /// </param>
    /// <param name="disableVerity">Whether vbmeta disables dm-verity.</param>
    /// <param name="disableVerification">Whether vbmeta disables verification.</param>
    /// <param name="slot">The global update slot, or null to preserve the package plan.</param>
    /// <param name="setActive">Whether to issue set_active before update tasks.</param>
    /// <param name="activeSlot">The explicit set_active target, or null to derive it.</param>
    /// <exception cref="ArgumentOutOfRangeException">
    /// <paramref name="timeout"/> cannot be represented by the native ABI.
    /// </exception>
    public UpdateOptions(
        TimeSpan timeout,
        bool wipe = false,
        bool skipReboot = false,
        bool skipSecondary = false,
        bool excludeDynamicPartitions = false,
        bool disableFastbootInfo = false,
        bool disableVerity = false,
        bool disableVerification = false,
        string? slot = null,
        bool setActive = false,
        string? activeSlot = null)
    {
        nativeTimeoutMilliseconds = ToNativeMilliseconds(timeout);
        this.timeout = timeout;
        hasExplicitTimeout = true;
        Wipe = wipe;
        SkipReboot = skipReboot;
        SkipSecondary = skipSecondary;
        ExcludeDynamicPartitions = excludeDynamicPartitions;
        DisableFastbootInfo = disableFastbootInfo;
        DisableVerity = disableVerity;
        DisableVerification = disableVerification;
        Slot = slot;
        SetActive = setActive;
        ActiveSlot = activeSlot;
    }

    /// <summary>Gets options that use no deadline and preserve user data.</summary>
    public static UpdateOptions Default => default;

    /// <summary>Gets the timeout for the complete update operation.</summary>
    public TimeSpan Timeout => hasExplicitTimeout
        ? timeout
        : System.Threading.Timeout.InfiniteTimeSpan;

    /// <summary>Gets whether wipe-conditioned package tasks may erase user data.</summary>
    public bool Wipe { get; }

    /// <summary>Gets whether the final system reboot is omitted.</summary>
    public bool SkipReboot { get; }

    /// <summary>Gets whether secondary-slot flash tasks are omitted.</summary>
    public bool SkipSecondary { get; }

    /// <summary>Gets whether logical partitions are excluded.</summary>
    public bool ExcludeDynamicPartitions { get; }

    /// <summary>Gets whether the frozen image-list plan replaces fastboot-info.txt.</summary>
    public bool DisableFastbootInfo { get; }

    /// <summary>Gets whether update vbmeta images disable dm-verity.</summary>
    public bool DisableVerity { get; }

    /// <summary>Gets whether update vbmeta images disable verification.</summary>
    public bool DisableVerification { get; }

    /// <summary>Gets the global update slot, or null to preserve the package plan.</summary>
    public string? Slot { get; }

    /// <summary>Gets whether set_active is issued before update tasks.</summary>
    public bool SetActive { get; }

    /// <summary>Gets the explicit set_active slot, or null to derive it.</summary>
    public string? ActiveSlot { get; }

    internal uint NativeTimeoutMilliseconds => hasExplicitTimeout
        ? nativeTimeoutMilliseconds
        : uint.MaxValue;

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
                "Update timeout must be non-negative or Timeout.InfiniteTimeSpan.");
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
                "Finite update timeout must be shorter than UInt32.MaxValue milliseconds.");
        }

        return (uint)milliseconds;
    }
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

/// <summary>Progress reported while validating and applying an update package.</summary>
public sealed class UpdateProgress
{
    internal UpdateProgress(
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

    /// <summary>Gets the completed byte count for the current image transfer.</summary>
    public ulong BytesCompleted { get; }

    /// <summary>Gets the current image size, or zero outside a transfer stage.</summary>
    public ulong BytesTotal { get; }

    /// <summary>Gets the current update stage.</summary>
    public string Stage { get; }

    /// <summary>Gets the device associated with this progress event.</summary>
    public string DeviceIdentifier { get; }
}
