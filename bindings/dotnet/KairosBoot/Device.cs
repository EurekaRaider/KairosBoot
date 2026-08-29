using System;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Tasks;
using KairosBoot.Interop;

namespace KairosBoot;

/// <summary>Owns one opened Fastboot device and exposes single-device operations.</summary>
public sealed partial class Device : IDisposable
{
    private readonly DeviceSafeHandle handle;

    internal Device(DeviceSafeHandle handle)
    {
        this.handle = handle;
    }

    /// <summary>Gets the canonical identifier captured when the device was opened.</summary>
    public string Identifier
    {
        get
        {
            ThrowIfDisposed();
            return Utf8String.FromNative(NativeMethods.DeviceIdentifier(handle));
        }
    }

    /// <summary>Gets the USB serial, or an empty string for network devices.</summary>
    public string Serial
    {
        get
        {
            ThrowIfDisposed();
            return Utf8String.FromNative(NativeMethods.DeviceSerial(handle));
        }
    }

    /// <summary>Gets the stable USB path, or an empty string for network devices.</summary>
    public string UsbPath
    {
        get
        {
            ThrowIfDisposed();
            return Utf8String.FromNative(NativeMethods.DeviceUsbPath(handle));
        }
    }

    private delegate int StartCommandOperation(
        ref NativeCommandOptions options,
        out IntPtr operation,
        out IntPtr error);

    private sealed class NativeSlotPolicy : IDisposable
    {
        private IntPtr slot;
        private IntPtr activeSlot;
        private readonly bool setActive;

        internal NativeSlotPolicy(string? slot, bool setActive, string? activeSlot)
        {
            Validate(slot, nameof(slot));
            Validate(activeSlot, nameof(activeSlot));
            if (activeSlot != null && !setActive)
            {
                throw new ArgumentException(
                    "An active slot requires setActive.", nameof(activeSlot));
            }

            this.setActive = setActive;
            try
            {
                this.slot = slot == null ? IntPtr.Zero : Utf8String.Allocate(slot);
                this.activeSlot = activeSlot == null
                    ? IntPtr.Zero
                    : Utf8String.Allocate(activeSlot);
            }
            catch
            {
                Dispose();
                throw;
            }
        }

        internal void Apply(ref NativeFlashOptions options)
        {
            options.Slot = slot;
            options.SetActive = setActive ? 1 : 0;
            options.ActiveSlot = activeSlot;
        }

        internal void Apply(ref NativeUpdateOptions options)
        {
            options.Slot = slot;
            options.SetActive = setActive ? 1 : 0;
            options.ActiveSlot = activeSlot;
        }

        public void Dispose()
        {
            Utf8String.Free(slot);
            Utf8String.Free(activeSlot);
            slot = IntPtr.Zero;
            activeSlot = IntPtr.Zero;
        }

        private static void Validate(string? value, string name)
        {
            if (value != null && (value.Length == 0 || value.IndexOf('\0') >= 0))
            {
                throw new ArgumentException(
                    "A slot must be non-empty and NUL-free.", name);
            }
        }
    }

    /// <summary>Starts a flash operation with native default options.</summary>
    public Task FlashFileAsync(
        string partition,
        string filePath,
        IProgress<FlashProgress>? progress = null,
        CancellationToken cancellationToken = default)
    {
        return FlashFileCoreAsync(
            partition,
            filePath,
            FlashOptions.Default,
            progress,
            cancellationToken);
    }

    /// <summary>Starts a flash operation with a typed per-I/O timeout.</summary>
    public Task FlashFileAsync(
        string partition,
        string filePath,
        FlashOptions options,
        IProgress<FlashProgress>? progress = null,
        CancellationToken cancellationToken = default)
    {
        return FlashFileCoreAsync(
            partition,
            filePath,
            options,
            progress,
            cancellationToken);
    }

    /// <summary>
    /// Generates an empty ext4 or f2fs Android sparse image and flashes it to
    /// a partition. Null filesystem type and zero size use device-reported
    /// partition metadata.
    /// </summary>
    public Task FormatPartitionAsync(
        string partition,
        string? filesystemType = null,
        ulong partitionSize = 0,
        FlashOptions options = default,
        IProgress<FlashProgress>? progress = null,
        CancellationToken cancellationToken = default)
    {
        return FormatPartitionCoreAsync(
            partition,
            filesystemType,
            partitionSize,
            options,
            progress,
            cancellationToken);
    }

    /// <summary>
    /// Fetches an existing vendor_boot image, replaces one vendor ramdisk and
    /// optionally its DTB, then flashes the repacked image in the same session.
    /// </summary>
    public Task FlashVendorBootRamdiskAsync(
        string partition,
        string ramdiskPath,
        string ramdiskName = "default",
        string? dtbPath = null,
        FlashOptions options = default,
        IProgress<FlashProgress>? progress = null,
        CancellationToken cancellationToken = default)
    {
        return FlashVendorBootRamdiskCoreAsync(
            partition,
            ramdiskPath,
            ramdiskName,
            dtbPath,
            options,
            progress,
            cancellationToken);
    }

    /// <summary>Builds and flashes a default Android boot image.</summary>
    public Task FlashRawAsync(
        string partition,
        string kernelPath,
        string? ramdiskPath = null,
        string? secondStagePath = null,
        IProgress<FlashProgress>? progress = null,
        CancellationToken cancellationToken = default)
    {
        return FlashRawCoreAsync(
            partition,
            kernelPath,
            ramdiskPath,
            secondStagePath,
            null,
            FlashOptions.Default,
            progress,
            cancellationToken);
    }

    /// <summary>Builds and flashes a default Android boot image with a typed timeout.</summary>
    public Task FlashRawAsync(
        string partition,
        string kernelPath,
        FlashOptions options,
        string? ramdiskPath = null,
        string? secondStagePath = null,
        IProgress<FlashProgress>? progress = null,
        CancellationToken cancellationToken = default)
    {
        return FlashRawCoreAsync(
            partition,
            kernelPath,
            ramdiskPath,
            secondStagePath,
            null,
            options,
            progress,
            cancellationToken);
    }

    /// <summary>Builds and flashes a legacy Android boot image with a custom layout.</summary>
    public Task FlashRawAsync(
        string partition,
        string kernelPath,
        LegacyBootOptions legacyBootOptions,
        string? ramdiskPath = null,
        string? secondStagePath = null,
        IProgress<FlashProgress>? progress = null,
        CancellationToken cancellationToken = default)
    {
        return FlashRawCoreAsync(
            partition,
            kernelPath,
            ramdiskPath,
            secondStagePath,
            legacyBootOptions,
            FlashOptions.Default,
            progress,
            cancellationToken);
    }

    /// <summary>Builds and flashes a legacy Android boot image with a custom layout and timeout.</summary>
    public Task FlashRawAsync(
        string partition,
        string kernelPath,
        LegacyBootOptions legacyBootOptions,
        FlashOptions options,
        string? ramdiskPath = null,
        string? secondStagePath = null,
        IProgress<FlashProgress>? progress = null,
        CancellationToken cancellationToken = default)
    {
        return FlashRawCoreAsync(
            partition,
            kernelPath,
            ramdiskPath,
            secondStagePath,
            legacyBootOptions,
            options,
            progress,
            cancellationToken);
    }

    /// <summary>Builds, downloads and boots a legacy Android boot image.</summary>
    public Task BootRawAsync(
        string kernelPath,
        string? ramdiskPath = null,
        string? secondStagePath = null,
        IProgress<FlashProgress>? progress = null,
        CancellationToken cancellationToken = default)
    {
        return BootRawCoreAsync(
            kernelPath,
            ramdiskPath,
            secondStagePath,
            LegacyBootOptions.Default,
            FlashOptions.Default,
            progress,
            cancellationToken);
    }

    /// <summary>Builds, downloads and boots a legacy Android boot image with a typed timeout.</summary>
    public Task BootRawAsync(
        string kernelPath,
        FlashOptions options,
        string? ramdiskPath = null,
        string? secondStagePath = null,
        IProgress<FlashProgress>? progress = null,
        CancellationToken cancellationToken = default)
    {
        return BootRawCoreAsync(
            kernelPath,
            ramdiskPath,
            secondStagePath,
            LegacyBootOptions.Default,
            options,
            progress,
            cancellationToken);
    }

    /// <summary>Builds, downloads and boots a legacy Android boot image with a custom layout.</summary>
    public Task BootRawAsync(
        string kernelPath,
        LegacyBootOptions legacyBootOptions,
        string? ramdiskPath = null,
        string? secondStagePath = null,
        IProgress<FlashProgress>? progress = null,
        CancellationToken cancellationToken = default)
    {
        return BootRawCoreAsync(
            kernelPath,
            ramdiskPath,
            secondStagePath,
            legacyBootOptions,
            FlashOptions.Default,
            progress,
            cancellationToken);
    }

    /// <summary>Builds, downloads and boots a legacy Android boot image with a custom layout and timeout.</summary>
    public Task BootRawAsync(
        string kernelPath,
        LegacyBootOptions legacyBootOptions,
        FlashOptions options,
        string? ramdiskPath = null,
        string? secondStagePath = null,
        IProgress<FlashProgress>? progress = null,
        CancellationToken cancellationToken = default)
    {
        return BootRawCoreAsync(
            kernelPath,
            ramdiskPath,
            secondStagePath,
            legacyBootOptions,
            options,
            progress,
            cancellationToken);
    }

    /// <summary>Downloads an image file and boots it with native default options.</summary>
    public Task BootFileAsync(
        string filePath,
        IProgress<FlashProgress>? progress = null,
        CancellationToken cancellationToken = default)
    {
        return BootFileCoreAsync(
            filePath,
            FlashOptions.Default,
            progress,
            cancellationToken);
    }

    /// <summary>Downloads an image file and boots it with a typed per-I/O timeout.</summary>
    public Task BootFileAsync(
        string filePath,
        FlashOptions options,
        IProgress<FlashProgress>? progress = null,
        CancellationToken cancellationToken = default)
    {
        return BootFileCoreAsync(
            filePath,
            options,
            progress,
            cancellationToken);
    }

    /// <summary>
    /// Streams an existing 256-byte signature blob and sends the AOSP Fastboot
    /// <c>signature</c> command on the same device session.
    /// </summary>
    public Task<CommandResult> SignatureFileAsync(
        string filePath,
        CommandOptions options = default,
        IProgress<CommandProgress>? progress = null,
        CancellationToken cancellationToken = default)
    {
        ValidateRequiredText(filePath, nameof(filePath));
        return RunCommandAsync(
            options,
            progress,
            cancellationToken,
            (ref NativeCommandOptions nativeOptions, out IntPtr operation, out IntPtr error) =>
                NativeMethods.SignatureFileAsync(
                    handle,
                    filePath,
                    ref nativeOptions,
                    out operation,
                    out error));
    }

    /// <summary>Starts a complete update-package operation with safe defaults.</summary>
    public Task UpdatePackageAsync(
        string packagePath,
        IProgress<UpdateProgress>? progress = null,
        CancellationToken cancellationToken = default)
    {
        return UpdatePackageCoreAsync(
            packagePath,
            UpdateOptions.Default,
            progress,
            cancellationToken);
    }

    /// <summary>Wipes dynamic partitions using the AOSP super_empty image lookup.</summary>
    public Task WipeSuperAsync(
        string? superEmptyImage = null,
        IProgress<UpdateProgress>? progress = null,
        CancellationToken cancellationToken = default)
    {
        return WipeSuperCoreAsync(
            superEmptyImage,
            UpdateOptions.Default,
            progress,
            cancellationToken);
    }

    /// <summary>Wipes dynamic partitions with typed timeout and progress options.</summary>
    public Task WipeSuperAsync(
        string? superEmptyImage,
        UpdateOptions options,
        IProgress<UpdateProgress>? progress = null,
        CancellationToken cancellationToken = default)
    {
        return WipeSuperCoreAsync(
            superEmptyImage,
            options,
            progress,
            cancellationToken);
    }

    /// <summary>Starts a complete update-package operation with typed options.</summary>
    public Task UpdatePackageAsync(
        string packagePath,
        UpdateOptions options,
        IProgress<UpdateProgress>? progress = null,
        CancellationToken cancellationToken = default)
    {
        return UpdatePackageCoreAsync(
            packagePath,
            options,
            progress,
            cancellationToken);
    }

    /// <summary>Reads a Fastboot variable.</summary>
    public Task<CommandResult> GetVarAsync(
        string variable,
        CommandOptions options = default,
        IProgress<CommandProgress>? progress = null,
        CancellationToken cancellationToken = default)
    {
        ValidateRequiredText(variable, nameof(variable));
        return RunCommandAsync(
            options,
            progress,
            cancellationToken,
            (ref NativeCommandOptions nativeOptions, out IntPtr operation, out IntPtr error) =>
                NativeMethods.GetVarAsync(
                    handle,
                    variable,
                    ref nativeOptions,
                    out operation,
                    out error));
    }

    /// <summary>Erases a partition.</summary>
    public Task<CommandResult> EraseAsync(
        string partition,
        CommandOptions options = default,
        IProgress<CommandProgress>? progress = null,
        CancellationToken cancellationToken = default)
    {
        ValidateRequiredText(partition, nameof(partition));
        return RunCommandAsync(
            options,
            progress,
            cancellationToken,
            (ref NativeCommandOptions nativeOptions, out IntPtr operation, out IntPtr error) =>
                NativeMethods.EraseAsync(
                    handle,
                    partition,
                    ref nativeOptions,
                    out operation,
                    out error));
    }

    /// <summary>Selects the active A/B slot.</summary>
    public Task<CommandResult> SetActiveAsync(
        string slot,
        CommandOptions options = default,
        IProgress<CommandProgress>? progress = null,
        CancellationToken cancellationToken = default)
    {
        ValidateRequiredText(slot, nameof(slot));
        return RunCommandAsync(
            options,
            progress,
            cancellationToken,
            (ref NativeCommandOptions nativeOptions, out IntPtr operation, out IntPtr error) =>
                NativeMethods.SetActiveAsync(
                    handle,
                    slot,
                    ref nativeOptions,
                    out operation,
                    out error));
    }

    /// <summary>Runs a Fastboot flashing-state command.</summary>
    public Task<CommandResult> FlashingAsync(
        FlashingCommand command,
        CommandOptions options = default,
        IProgress<CommandProgress>? progress = null,
        CancellationToken cancellationToken = default)
    {
        return RunCommandAsync(
            options,
            progress,
            cancellationToken,
            (ref NativeCommandOptions nativeOptions, out IntPtr operation, out IntPtr error) =>
                NativeMethods.FlashingAsync(
                    handle,
                    (int)command,
                    ref nativeOptions,
                    out operation,
                    out error));
    }

    /// <summary>Runs a Fastboot GSI management command.</summary>
    public Task<CommandResult> GsiAsync(
        GsiCommand command,
        CommandOptions options = default,
        IProgress<CommandProgress>? progress = null,
        CancellationToken cancellationToken = default)
    {
        return RunCommandAsync(
            options,
            progress,
            cancellationToken,
            (ref NativeCommandOptions nativeOptions, out IntPtr operation, out IntPtr error) =>
                NativeMethods.GsiAsync(
                    handle,
                    (int)command,
                    ref nativeOptions,
                    out operation,
                    out error));
    }

    /// <summary>Runs a Fastboot snapshot-update command.</summary>
    public Task<CommandResult> SnapshotUpdateAsync(
        SnapshotUpdateCommand command,
        CommandOptions options = default,
        IProgress<CommandProgress>? progress = null,
        CancellationToken cancellationToken = default)
    {
        return RunCommandAsync(
            options,
            progress,
            cancellationToken,
            (ref NativeCommandOptions nativeOptions, out IntPtr operation, out IntPtr error) =>
                NativeMethods.SnapshotUpdateAsync(
                    handle,
                    (int)command,
                    ref nativeOptions,
                    out operation,
                    out error));
    }

    /// <summary>Creates a logical partition with the requested byte size.</summary>
    public Task<CommandResult> CreateLogicalPartitionAsync(
        string partitionName,
        ulong size,
        CommandOptions options = default,
        IProgress<CommandProgress>? progress = null,
        CancellationToken cancellationToken = default)
    {
        ValidateNativeText(partitionName, nameof(partitionName));
        return RunCommandAsync(
            options,
            progress,
            cancellationToken,
            (ref NativeCommandOptions nativeOptions, out IntPtr operation, out IntPtr error) =>
                NativeMethods.CreateLogicalPartitionAsync(
                    handle,
                    partitionName,
                    size,
                    ref nativeOptions,
                    out operation,
                    out error));
    }

    /// <summary>Deletes a logical partition.</summary>
    public Task<CommandResult> DeleteLogicalPartitionAsync(
        string partitionName,
        CommandOptions options = default,
        IProgress<CommandProgress>? progress = null,
        CancellationToken cancellationToken = default)
    {
        ValidateNativeText(partitionName, nameof(partitionName));
        return RunCommandAsync(
            options,
            progress,
            cancellationToken,
            (ref NativeCommandOptions nativeOptions, out IntPtr operation, out IntPtr error) =>
                NativeMethods.DeleteLogicalPartitionAsync(
                    handle,
                    partitionName,
                    ref nativeOptions,
                    out operation,
                    out error));
    }

    /// <summary>Resizes a logical partition to the requested byte size.</summary>
    public Task<CommandResult> ResizeLogicalPartitionAsync(
        string partitionName,
        ulong size,
        CommandOptions options = default,
        IProgress<CommandProgress>? progress = null,
        CancellationToken cancellationToken = default)
    {
        ValidateNativeText(partitionName, nameof(partitionName));
        return RunCommandAsync(
            options,
            progress,
            cancellationToken,
            (ref NativeCommandOptions nativeOptions, out IntPtr operation, out IntPtr error) =>
                NativeMethods.ResizeLogicalPartitionAsync(
                    handle,
                    partitionName,
                    size,
                    ref nativeOptions,
                    out operation,
                    out error));
    }

    /// <summary>Reboots the selected device.</summary>
    public Task<CommandResult> RebootAsync(
        RebootTarget target = RebootTarget.System,
        CommandOptions options = default,
        IProgress<CommandProgress>? progress = null,
        CancellationToken cancellationToken = default)
    {
        if (target < RebootTarget.System || target > RebootTarget.Fastboot)
        {
            throw new ArgumentOutOfRangeException(nameof(target));
        }

        return RunCommandAsync(
            options,
            progress,
            cancellationToken,
            (ref NativeCommandOptions nativeOptions, out IntPtr operation, out IntPtr error) =>
                NativeMethods.RebootAsync(
                    handle,
                    (int)target,
                    ref nativeOptions,
                    out operation,
                    out error));
    }

    /// <summary>Continues booting the selected device.</summary>
    public Task<CommandResult> ContinueBootAsync(
        CommandOptions options = default,
        IProgress<CommandProgress>? progress = null,
        CancellationToken cancellationToken = default)
    {
        return RunCommandAsync(
            options,
            progress,
            cancellationToken,
            (ref NativeCommandOptions nativeOptions, out IntPtr operation, out IntPtr error) =>
                NativeMethods.ContinueBootAsync(
                    handle,
                    ref nativeOptions,
                    out operation,
                    out error));
    }

    /// <summary>Runs an OEM command suffix.</summary>
    public Task<CommandResult> OemAsync(
        string commandSuffix,
        CommandOptions options = default,
        IProgress<CommandProgress>? progress = null,
        CancellationToken cancellationToken = default)
    {
        ValidateRequiredText(commandSuffix, nameof(commandSuffix));
        return RunCommandAsync(
            options,
            progress,
            cancellationToken,
            (ref NativeCommandOptions nativeOptions, out IntPtr operation, out IntPtr error) =>
                NativeMethods.OemAsync(
                    handle,
                    commandSuffix,
                    ref nativeOptions,
                    out operation,
                    out error));
    }

    /// <summary>Runs a raw Fastboot command.</summary>
    public Task<CommandResult> RawCommandAsync(
        string command,
        CommandOptions options = default,
        IProgress<CommandProgress>? progress = null,
        CancellationToken cancellationToken = default)
    {
        ValidateRequiredText(command, nameof(command));
        return RunCommandAsync(
            options,
            progress,
            cancellationToken,
            (ref NativeCommandOptions nativeOptions, out IntPtr operation, out IntPtr error) =>
                NativeMethods.RawCommandAsync(
                    handle,
                    command,
                    ref nativeOptions,
                    out operation,
                    out error));
    }

    /// <summary>Boots previously downloaded or staged data.</summary>
    public Task<CommandResult> BootAsync(
        CommandOptions options = default,
        IProgress<CommandProgress>? progress = null,
        CancellationToken cancellationToken = default)
    {
        return RunCommandAsync(
            options,
            progress,
            cancellationToken,
            (ref NativeCommandOptions nativeOptions, out IntPtr operation, out IntPtr error) =>
                NativeMethods.BootAsync(
                    handle,
                    ref nativeOptions,
                    out operation,
                    out error));
    }

    /// <summary>Stages an owned managed byte-array snapshot on the device.</summary>
    public Task<CommandResult> StageAsync(
        byte[] data,
        CommandOptions options = default,
        IProgress<CommandProgress>? progress = null,
        CancellationToken cancellationToken = default)
    {
        if (data == null)
        {
            throw new ArgumentNullException(nameof(data));
        }

        if (data.Length == 0)
        {
            throw new ArgumentException("Stage data must not be empty.", nameof(data));
        }

        return RunCommandAsync(
            options,
            progress,
            cancellationToken,
            delegate (
                ref NativeCommandOptions nativeOptions,
                out IntPtr operation,
                out IntPtr error)
            {
                var pinned = GCHandle.Alloc(data, GCHandleType.Pinned);
                try
                {
                    return NativeMethods.StageAsync(
                        handle,
                        pinned.AddrOfPinnedObject(),
                        new UIntPtr((ulong)data.LongLength),
                        ref nativeOptions,
                        out operation,
                        out error);
                }
                finally
                {
                    // Native stage start snapshots the input before returning.
                    // The array is never pinned during asynchronous transport.
                    pinned.Free();
                }
            });
    }

    /// <summary>Receives bounded data from the Fastboot upload command.</summary>
    public Task<CommandResult> UploadAsync(
        CommandOptions options = default,
        IProgress<CommandProgress>? progress = null,
        CancellationToken cancellationToken = default)
    {
        return RunCommandAsync(
            options,
            progress,
            cancellationToken,
            (ref NativeCommandOptions nativeOptions, out IntPtr operation, out IntPtr error) =>
                NativeMethods.UploadAsync(
                    handle,
                    ref nativeOptions,
                    out operation,
                    out error));
    }

    /// <summary>Fetches a bounded partition range from the device.</summary>
    public Task<CommandResult> FetchAsync(
        string partition,
        ulong? offset = null,
        ulong? size = null,
        CommandOptions options = default,
        IProgress<CommandProgress>? progress = null,
        CancellationToken cancellationToken = default)
    {
        ValidateRequiredText(partition, nameof(partition));
        return RunCommandAsync(
            options,
            progress,
            cancellationToken,
            (ref NativeCommandOptions nativeOptions, out IntPtr operation, out IntPtr error) =>
                NativeMethods.FetchAsync(
                    handle,
                    partition,
                    offset ?? NativeMethods.FetchUnspecified,
                    size ?? NativeMethods.FetchUnspecified,
                    ref nativeOptions,
                    out operation,
                    out error));
    }

    /// <summary>Receives the Fastboot upload command directly into an atomic output file.</summary>
    public Task<CommandResult> UploadFileAsync(
        string outputPath,
        CommandOptions options = default,
        IProgress<CommandProgress>? progress = null,
        CancellationToken cancellationToken = default)
    {
        ValidateRequiredText(outputPath, nameof(outputPath));
        return RunCommandAsync(
            options,
            progress,
            cancellationToken,
            (ref NativeCommandOptions nativeOptions, out IntPtr operation, out IntPtr error) =>
                NativeMethods.UploadFileAsync(
                    handle,
                    outputPath,
                    ref nativeOptions,
                    out operation,
                    out error));
    }

    /// <summary>Receives staged Fastboot data directly into an atomic output file.</summary>
    public Task<CommandResult> GetStagedFileAsync(
        string outputPath,
        CommandOptions options = default,
        IProgress<CommandProgress>? progress = null,
        CancellationToken cancellationToken = default)
    {
        ValidateRequiredText(outputPath, nameof(outputPath));
        return RunCommandAsync(
            options,
            progress,
            cancellationToken,
            (ref NativeCommandOptions nativeOptions, out IntPtr operation, out IntPtr error) =>
                NativeMethods.GetStagedFileAsync(
                    handle,
                    outputPath,
                    ref nativeOptions,
                    out operation,
                    out error));
    }

    /// <summary>Fetches a bounded partition range directly into an atomic output file.</summary>
    public Task<CommandResult> FetchFileAsync(
        string partition,
        string outputPath,
        ulong? offset = null,
        ulong? size = null,
        CommandOptions options = default,
        IProgress<CommandProgress>? progress = null,
        CancellationToken cancellationToken = default)
    {
        ValidateRequiredText(partition, nameof(partition));
        ValidateRequiredText(outputPath, nameof(outputPath));
        return RunCommandAsync(
            options,
            progress,
            cancellationToken,
            (ref NativeCommandOptions nativeOptions, out IntPtr operation, out IntPtr error) =>
                NativeMethods.FetchFileAsync(
                    handle,
                    partition,
                    offset ?? NativeMethods.FetchUnspecified,
                    size ?? NativeMethods.FetchUnspecified,
                    outputPath,
                    ref nativeOptions,
                    out operation,
                    out error));
    }

    /// <summary>Releases the native device.</summary>
    public void Dispose()
    {
        handle.Dispose();
    }

    private async Task FlashFileCoreAsync(
        string partition,
        string filePath,
        FlashOptions options,
        IProgress<FlashProgress>? progress,
        CancellationToken cancellationToken)
    {
        ValidateRequiredText(partition, nameof(partition));
        ValidateRequiredText(filePath, nameof(filePath));

        ThrowIfDisposed();
        cancellationToken.ThrowIfCancellationRequested();
        using var slotPolicy = new NativeSlotPolicy(
            options.Slot, options.SetActive, options.ActiveSlot);

        ProgressCallbackRegistration<FlashProgress>? progressRegistration = null;
        var deviceReferenceAdded = false;
        try
        {
            handle.DangerousAddRef(ref deviceReferenceAdded);
            if (progress != null)
            {
                progressRegistration = new ProgressCallbackRegistration<FlashProgress>(
                    progress,
                    CreateFlashProgress);
            }

            var nativeOptions = new NativeFlashOptions();
            NativeMethods.FlashOptionsInitSized(ref nativeOptions, NativeMethods.FlashOptionsStructSize);
            nativeOptions.TimeoutMilliseconds = options.NativeTimeoutMilliseconds;
            nativeOptions.SparseLimitBytes = options.SparseLimitBytes;
            nativeOptions.DisableVerity = options.DisableVerity ? 1 : 0;
            nativeOptions.DisableVerification = options.DisableVerification ? 1 : 0;
            nativeOptions.Force = options.Force ? 1 : 0;
            nativeOptions.FilesystemOptions = (uint)options.FilesystemOptions;
            nativeOptions.ProgressCallback = progressRegistration?.CallbackPointer ?? IntPtr.Zero;
            nativeOptions.ProgressUserData = progressRegistration?.UserData ?? IntPtr.Zero;
            slotPolicy.Apply(ref nativeOptions);

            var status = NativeMethods.FlashFileAsync(
                handle,
                partition,
                filePath,
                ref nativeOptions,
                out var rawOperation,
                out var rawError);

            using (var operation = TakeStartedOperation(status, rawOperation, rawError))
            {
                await OperationPollingEngine.WaitAsync(
                    new NativeOperationPollTarget(operation),
                    cancellationToken).ConfigureAwait(false);
            }
        }
        finally
        {
            progressRegistration?.Dispose();
            if (deviceReferenceAdded)
            {
                handle.DangerousRelease();
            }
        }
    }

    private async Task FlashVendorBootRamdiskCoreAsync(
        string partition,
        string ramdiskPath,
        string ramdiskName,
        string? dtbPath,
        FlashOptions options,
        IProgress<FlashProgress>? progress,
        CancellationToken cancellationToken)
    {
        ValidateRequiredText(partition, nameof(partition));
        ValidateRequiredText(ramdiskPath, nameof(ramdiskPath));
        ValidateRequiredText(ramdiskName, nameof(ramdiskName));
        ValidateOptionalText(dtbPath, nameof(dtbPath));

        ThrowIfDisposed();
        cancellationToken.ThrowIfCancellationRequested();
        using var slotPolicy = new NativeSlotPolicy(
            options.Slot, options.SetActive, options.ActiveSlot);

        ProgressCallbackRegistration<FlashProgress>? progressRegistration = null;
        var deviceReferenceAdded = false;
        try
        {
            handle.DangerousAddRef(ref deviceReferenceAdded);
            if (progress != null)
            {
                progressRegistration = new ProgressCallbackRegistration<FlashProgress>(
                    progress,
                    CreateFlashProgress);
            }

            var nativeOptions = new NativeFlashOptions();
            NativeMethods.FlashOptionsInitSized(ref nativeOptions, NativeMethods.FlashOptionsStructSize);
            nativeOptions.TimeoutMilliseconds = options.NativeTimeoutMilliseconds;
            nativeOptions.SparseLimitBytes = options.SparseLimitBytes;
            nativeOptions.DisableVerity = options.DisableVerity ? 1 : 0;
            nativeOptions.DisableVerification = options.DisableVerification ? 1 : 0;
            nativeOptions.Force = options.Force ? 1 : 0;
            nativeOptions.FilesystemOptions = (uint)options.FilesystemOptions;
            nativeOptions.ProgressCallback = progressRegistration?.CallbackPointer ?? IntPtr.Zero;
            nativeOptions.ProgressUserData = progressRegistration?.UserData ?? IntPtr.Zero;
            slotPolicy.Apply(ref nativeOptions);

            var status = NativeMethods.FlashVendorBootRamdiskAsync(
                handle,
                partition,
                ramdiskName,
                ramdiskPath,
                dtbPath,
                ref nativeOptions,
                out var rawOperation,
                out var rawError);
            using (var operation = TakeStartedOperation(status, rawOperation, rawError))
            {
                await OperationPollingEngine.WaitAsync(
                    new NativeOperationPollTarget(operation),
                    cancellationToken).ConfigureAwait(false);
            }
        }
        finally
        {
            progressRegistration?.Dispose();
            if (deviceReferenceAdded)
            {
                handle.DangerousRelease();
            }
        }
    }

    private async Task FlashRawCoreAsync(
        string partition,
        string kernelPath,
        string? ramdiskPath,
        string? secondStagePath,
        LegacyBootOptions? legacyBootOptions,
        FlashOptions options,
        IProgress<FlashProgress>? progress,
        CancellationToken cancellationToken)
    {
        ValidateRequiredText(partition, nameof(partition));
        ValidateRequiredText(kernelPath, nameof(kernelPath));
        ValidateOptionalText(ramdiskPath, nameof(ramdiskPath));
        ValidateOptionalText(secondStagePath, nameof(secondStagePath));
        if (secondStagePath != null && ramdiskPath == null)
        {
            throw new ArgumentException(
                "A second-stage path requires a ramdisk path.",
                nameof(secondStagePath));
        }

        ThrowIfDisposed();
        cancellationToken.ThrowIfCancellationRequested();
        using var slotPolicy = new NativeSlotPolicy(
            options.Slot, options.SetActive, options.ActiveSlot);

        ProgressCallbackRegistration<FlashProgress>? progressRegistration = null;
        var deviceReferenceAdded = false;
        try
        {
            handle.DangerousAddRef(ref deviceReferenceAdded);
            if (progress != null)
            {
                progressRegistration = new ProgressCallbackRegistration<FlashProgress>(
                    progress,
                    CreateFlashProgress);
            }

            var nativeOptions = new NativeFlashOptions();
            NativeMethods.FlashOptionsInitSized(ref nativeOptions, NativeMethods.FlashOptionsStructSize);
            nativeOptions.TimeoutMilliseconds = options.NativeTimeoutMilliseconds;
            nativeOptions.SparseLimitBytes = options.SparseLimitBytes;
            nativeOptions.DisableVerity = options.DisableVerity ? 1 : 0;
            nativeOptions.DisableVerification = options.DisableVerification ? 1 : 0;
            nativeOptions.Force = options.Force ? 1 : 0;
            nativeOptions.FilesystemOptions = (uint)options.FilesystemOptions;
            nativeOptions.ProgressCallback = progressRegistration?.CallbackPointer ?? IntPtr.Zero;
            nativeOptions.ProgressUserData = progressRegistration?.UserData ?? IntPtr.Zero;
            slotPolicy.Apply(ref nativeOptions);

            int status;
            IntPtr rawOperation;
            IntPtr rawError;
            if (legacyBootOptions.HasValue)
            {
                var nativeLegacyOptions = CreateNativeLegacyBootOptions(
                    legacyBootOptions.Value,
                    out var commandLine,
                    out var osVersion,
                    out var osPatchLevel,
                    out var dtbPath);
                try
                {
                    status = NativeMethods.FlashRawWithBootOptionsAsync(
                        handle,
                        partition,
                        kernelPath,
                        ramdiskPath,
                        secondStagePath,
                        ref nativeLegacyOptions,
                        ref nativeOptions,
                        out rawOperation,
                        out rawError);
                }
                finally
                {
                    Utf8String.Free(commandLine);
                    Utf8String.Free(osVersion);
                    Utf8String.Free(osPatchLevel);
                    Utf8String.Free(dtbPath);
                }
            }
            else
            {
                status = NativeMethods.FlashRawAsync(
                    handle,
                    partition,
                    kernelPath,
                    ramdiskPath,
                    secondStagePath,
                    ref nativeOptions,
                    out rawOperation,
                    out rawError);
            }

            using (var operation = TakeStartedOperation(status, rawOperation, rawError))
            {
                await OperationPollingEngine.WaitAsync(
                    new NativeOperationPollTarget(operation),
                    cancellationToken).ConfigureAwait(false);
            }
        }
        finally
        {
            progressRegistration?.Dispose();
            if (deviceReferenceAdded)
            {
                handle.DangerousRelease();
            }
        }
    }

    private async Task BootRawCoreAsync(
        string kernelPath,
        string? ramdiskPath,
        string? secondStagePath,
        LegacyBootOptions legacyBootOptions,
        FlashOptions options,
        IProgress<FlashProgress>? progress,
        CancellationToken cancellationToken)
    {
        ValidateRequiredText(kernelPath, nameof(kernelPath));
        ValidateOptionalText(ramdiskPath, nameof(ramdiskPath));
        ValidateOptionalText(secondStagePath, nameof(secondStagePath));
        if (secondStagePath != null && ramdiskPath == null)
        {
            throw new ArgumentException(
                "A second-stage path requires a ramdisk path.",
                nameof(secondStagePath));
        }

        ThrowIfDisposed();
        cancellationToken.ThrowIfCancellationRequested();

        ProgressCallbackRegistration<FlashProgress>? progressRegistration = null;
        var deviceReferenceAdded = false;
        try
        {
            handle.DangerousAddRef(ref deviceReferenceAdded);
            if (progress != null)
            {
                progressRegistration = new ProgressCallbackRegistration<FlashProgress>(
                    progress,
                    CreateFlashProgress);
            }

            var nativeOptions = new NativeFlashOptions();
            NativeMethods.FlashOptionsInitSized(ref nativeOptions, NativeMethods.FlashOptionsStructSize);
            nativeOptions.TimeoutMilliseconds = options.NativeTimeoutMilliseconds;
            nativeOptions.SparseLimitBytes = options.SparseLimitBytes;
            nativeOptions.DisableVerity = options.DisableVerity ? 1 : 0;
            nativeOptions.DisableVerification = options.DisableVerification ? 1 : 0;
            nativeOptions.Force = options.Force ? 1 : 0;
            nativeOptions.FilesystemOptions = (uint)options.FilesystemOptions;
            nativeOptions.ProgressCallback = progressRegistration?.CallbackPointer ?? IntPtr.Zero;
            nativeOptions.ProgressUserData = progressRegistration?.UserData ?? IntPtr.Zero;

            var nativeLegacyOptions = CreateNativeLegacyBootOptions(
                legacyBootOptions,
                out var commandLine,
                out var osVersion,
                out var osPatchLevel,
                out var dtbPath);
            int status;
            IntPtr rawOperation;
            IntPtr rawError;
            try
            {
                status = NativeMethods.BootRawAsync(
                    handle,
                    kernelPath,
                    ramdiskPath,
                    secondStagePath,
                    ref nativeLegacyOptions,
                    ref nativeOptions,
                    out rawOperation,
                    out rawError);
            }
            finally
            {
                Utf8String.Free(commandLine);
                Utf8String.Free(osVersion);
                Utf8String.Free(osPatchLevel);
                Utf8String.Free(dtbPath);
            }

            using (var operation = TakeStartedOperation(status, rawOperation, rawError))
            {
                await OperationPollingEngine.WaitAsync(
                    new NativeOperationPollTarget(operation),
                    cancellationToken).ConfigureAwait(false);
            }
        }
        finally
        {
            progressRegistration?.Dispose();
            if (deviceReferenceAdded)
            {
                handle.DangerousRelease();
            }
        }
    }

    private static NativeLegacyBootOptions CreateNativeLegacyBootOptions(
        LegacyBootOptions options,
        out IntPtr commandLine,
        out IntPtr osVersion,
        out IntPtr osPatchLevel,
        out IntPtr dtbPath)
    {
        var native = new NativeLegacyBootOptions();
        NativeMethods.LegacyBootOptionsInitSized(ref native, NativeMethods.LegacyBootOptionsStructSize);
        commandLine = IntPtr.Zero;
        osVersion = IntPtr.Zero;
        osPatchLevel = IntPtr.Zero;
        dtbPath = IntPtr.Zero;
        try
        {
            commandLine = Utf8String.Allocate(options.CommandLine);
            osVersion = Utf8String.Allocate(options.OsVersion);
            osPatchLevel = Utf8String.Allocate(options.OsPatchLevel);
            dtbPath = options.DtbPath == null
                ? IntPtr.Zero
                : Utf8String.Allocate(options.DtbPath);
        }
        catch
        {
            Utf8String.Free(commandLine);
            Utf8String.Free(osVersion);
            Utf8String.Free(osPatchLevel);
            Utf8String.Free(dtbPath);
            throw;
        }
        native.CommandLine = commandLine;
        native.BaseAddress = options.BaseAddress;
        native.PageSize = options.PageSize;
        native.KernelOffset = options.KernelOffset;
        native.RamdiskOffset = options.RamdiskOffset;
        native.SecondOffset = options.SecondOffset;
        native.TagsOffset = options.TagsOffset;
        native.HeaderVersion = options.HeaderVersion;
        native.OsVersion = osVersion;
        native.OsPatchLevel = osPatchLevel;
        native.DtbPath = dtbPath;
        native.DtbOffset = options.DtbOffset;
        return native;
    }

    private async Task BootFileCoreAsync(
        string filePath,
        FlashOptions options,
        IProgress<FlashProgress>? progress,
        CancellationToken cancellationToken)
    {
        ValidateRequiredText(filePath, nameof(filePath));

        ThrowIfDisposed();
        cancellationToken.ThrowIfCancellationRequested();

        ProgressCallbackRegistration<FlashProgress>? progressRegistration = null;
        var deviceReferenceAdded = false;
        try
        {
            handle.DangerousAddRef(ref deviceReferenceAdded);
            if (progress != null)
            {
                progressRegistration = new ProgressCallbackRegistration<FlashProgress>(
                    progress,
                    CreateFlashProgress);
            }

            var nativeOptions = new NativeFlashOptions();
            NativeMethods.FlashOptionsInitSized(ref nativeOptions, NativeMethods.FlashOptionsStructSize);
            nativeOptions.TimeoutMilliseconds = options.NativeTimeoutMilliseconds;
            nativeOptions.SparseLimitBytes = options.SparseLimitBytes;
            nativeOptions.DisableVerity = options.DisableVerity ? 1 : 0;
            nativeOptions.DisableVerification = options.DisableVerification ? 1 : 0;
            nativeOptions.Force = options.Force ? 1 : 0;
            nativeOptions.FilesystemOptions = (uint)options.FilesystemOptions;
            nativeOptions.ProgressCallback = progressRegistration?.CallbackPointer ?? IntPtr.Zero;
            nativeOptions.ProgressUserData = progressRegistration?.UserData ?? IntPtr.Zero;

            var status = NativeMethods.BootFileAsync(
                handle,
                filePath,
                ref nativeOptions,
                out var rawOperation,
                out var rawError);

            using (var operation = TakeStartedOperation(status, rawOperation, rawError))
            {
                await OperationPollingEngine.WaitAsync(
                    new NativeOperationPollTarget(operation),
                    cancellationToken).ConfigureAwait(false);
            }
        }
        finally
        {
            progressRegistration?.Dispose();
            if (deviceReferenceAdded)
            {
                handle.DangerousRelease();
            }
        }
    }

    private async Task FormatPartitionCoreAsync(
        string partition,
        string? filesystemType,
        ulong partitionSize,
        FlashOptions options,
        IProgress<FlashProgress>? progress,
        CancellationToken cancellationToken)
    {
        ValidateRequiredText(partition, nameof(partition));
        if (filesystemType != null &&
            !string.Equals(filesystemType, "ext4", StringComparison.Ordinal) &&
            !string.Equals(filesystemType, "f2fs", StringComparison.Ordinal))
        {
            throw new ArgumentException(
                "Filesystem type must be ext4 or f2fs.",
                nameof(filesystemType));
        }
        ThrowIfDisposed();
        cancellationToken.ThrowIfCancellationRequested();

        ProgressCallbackRegistration<FlashProgress>? progressRegistration = null;
        var deviceReferenceAdded = false;
        try
        {
            handle.DangerousAddRef(ref deviceReferenceAdded);
            if (progress != null)
            {
                progressRegistration = new ProgressCallbackRegistration<FlashProgress>(
                    progress,
                    CreateFlashProgress);
            }

            var nativeOptions = new NativeFlashOptions();
            NativeMethods.FlashOptionsInitSized(ref nativeOptions, NativeMethods.FlashOptionsStructSize);
            nativeOptions.TimeoutMilliseconds = options.NativeTimeoutMilliseconds;
            nativeOptions.SparseLimitBytes = options.SparseLimitBytes;
            nativeOptions.DisableVerity = options.DisableVerity ? 1 : 0;
            nativeOptions.DisableVerification = options.DisableVerification ? 1 : 0;
            nativeOptions.Force = options.Force ? 1 : 0;
            nativeOptions.FilesystemOptions = (uint)options.FilesystemOptions;
            nativeOptions.ProgressCallback = progressRegistration?.CallbackPointer ?? IntPtr.Zero;
            nativeOptions.ProgressUserData = progressRegistration?.UserData ?? IntPtr.Zero;

            var status = NativeMethods.FormatPartitionAsync(
                handle,
                partition,
                filesystemType,
                partitionSize,
                ref nativeOptions,
                out var rawOperation,
                out var rawError);

            using (var operation = TakeStartedOperation(status, rawOperation, rawError))
            {
                await OperationPollingEngine.WaitAsync(
                    new NativeOperationPollTarget(operation),
                    cancellationToken).ConfigureAwait(false);
            }
        }
        finally
        {
            progressRegistration?.Dispose();
            if (deviceReferenceAdded)
            {
                handle.DangerousRelease();
            }
        }
    }

    private async Task UpdatePackageCoreAsync(
        string packagePath,
        UpdateOptions options,
        IProgress<UpdateProgress>? progress,
        CancellationToken cancellationToken)
    {
        ValidateRequiredText(packagePath, nameof(packagePath));

        ThrowIfDisposed();
        cancellationToken.ThrowIfCancellationRequested();
        using var slotPolicy = new NativeSlotPolicy(
            options.Slot, options.SetActive, options.ActiveSlot);

        ProgressCallbackRegistration<UpdateProgress>? progressRegistration = null;
        var deviceReferenceAdded = false;
        try
        {
            handle.DangerousAddRef(ref deviceReferenceAdded);
            if (progress != null)
            {
                progressRegistration = new ProgressCallbackRegistration<UpdateProgress>(
                    progress,
                    CreateUpdateProgress);
            }

            var nativeOptions = new NativeUpdateOptions();
            NativeMethods.UpdateOptionsInitSized(ref nativeOptions, NativeMethods.UpdateOptionsStructSize);
            nativeOptions.TimeoutMilliseconds = options.NativeTimeoutMilliseconds;
            nativeOptions.SparseLimitBytes = options.SparseLimitBytes;
            nativeOptions.Wipe = options.Wipe ? 1 : 0;
            nativeOptions.SkipReboot = options.SkipReboot ? 1 : 0;
            nativeOptions.SkipSecondary = options.SkipSecondary ? 1 : 0;
            nativeOptions.ExcludeDynamicPartitions =
                options.ExcludeDynamicPartitions ? 1 : 0;
            nativeOptions.DisableFastbootInfo =
                options.DisableFastbootInfo ? 1 : 0;
            nativeOptions.DisableVerity = options.DisableVerity ? 1 : 0;
            nativeOptions.DisableVerification = options.DisableVerification ? 1 : 0;
            nativeOptions.Force = options.Force ? 1 : 0;
            nativeOptions.FilesystemOptions = (uint)options.FilesystemOptions;
            nativeOptions.DisableSuperOptimization =
                options.DisableSuperOptimization ? 1 : 0;
            nativeOptions.ProgressCallback = progressRegistration?.CallbackPointer ?? IntPtr.Zero;
            nativeOptions.ProgressUserData = progressRegistration?.UserData ?? IntPtr.Zero;
            slotPolicy.Apply(ref nativeOptions);

            var status = NativeMethods.UpdatePackageAsync(
                handle,
                packagePath,
                ref nativeOptions,
                out var rawOperation,
                out var rawError);

            using (var operation = TakeStartedOperation(status, rawOperation, rawError))
            {
                await OperationPollingEngine.WaitAsync(
                    new NativeOperationPollTarget(operation),
                    cancellationToken).ConfigureAwait(false);
            }
        }
        finally
        {
            progressRegistration?.Dispose();
            if (deviceReferenceAdded)
            {
                handle.DangerousRelease();
            }
        }
    }

    private async Task WipeSuperCoreAsync(
        string? superEmptyImage,
        UpdateOptions options,
        IProgress<UpdateProgress>? progress,
        CancellationToken cancellationToken)
    {
        if (superEmptyImage != null)
        {
            ValidateRequiredText(superEmptyImage, nameof(superEmptyImage));
        }

        ThrowIfDisposed();
        cancellationToken.ThrowIfCancellationRequested();

        ProgressCallbackRegistration<UpdateProgress>? progressRegistration = null;
        var deviceReferenceAdded = false;
        try
        {
            handle.DangerousAddRef(ref deviceReferenceAdded);
            if (progress != null)
            {
                progressRegistration = new ProgressCallbackRegistration<UpdateProgress>(
                    progress,
                    CreateUpdateProgress);
            }

            var nativeOptions = new NativeUpdateOptions();
            NativeMethods.UpdateOptionsInitSized(ref nativeOptions, NativeMethods.UpdateOptionsStructSize);
            nativeOptions.TimeoutMilliseconds = options.NativeTimeoutMilliseconds;
            nativeOptions.SparseLimitBytes = options.SparseLimitBytes;
            nativeOptions.Wipe = 1;
            nativeOptions.SkipReboot = options.SkipReboot ? 1 : 0;
            nativeOptions.SkipSecondary = options.SkipSecondary ? 1 : 0;
            nativeOptions.ExcludeDynamicPartitions =
                options.ExcludeDynamicPartitions ? 1 : 0;
            nativeOptions.DisableFastbootInfo =
                options.DisableFastbootInfo ? 1 : 0;
            nativeOptions.DisableVerity = options.DisableVerity ? 1 : 0;
            nativeOptions.DisableVerification = options.DisableVerification ? 1 : 0;
            nativeOptions.Force = options.Force ? 1 : 0;
            nativeOptions.FilesystemOptions = (uint)options.FilesystemOptions;
            nativeOptions.ProgressCallback = progressRegistration?.CallbackPointer ?? IntPtr.Zero;
            nativeOptions.ProgressUserData = progressRegistration?.UserData ?? IntPtr.Zero;

            var status = NativeMethods.WipeSuperAsync(
                handle,
                superEmptyImage,
                ref nativeOptions,
                out var rawOperation,
                out var rawError);

            using (var operation = TakeStartedOperation(status, rawOperation, rawError))
            {
                await OperationPollingEngine.WaitAsync(
                    new NativeOperationPollTarget(operation),
                    cancellationToken).ConfigureAwait(false);
            }
        }
        finally
        {
            progressRegistration?.Dispose();
            if (deviceReferenceAdded)
            {
                handle.DangerousRelease();
            }
        }
    }

    private async Task<CommandResult> RunCommandAsync(
        CommandOptions options,
        IProgress<CommandProgress>? progress,
        CancellationToken cancellationToken,
        StartCommandOperation start)
    {
        ThrowIfDisposed();
        cancellationToken.ThrowIfCancellationRequested();

        ProgressCallbackRegistration<CommandProgress>? progressRegistration = null;
        var deviceReferenceAdded = false;
        try
        {
            handle.DangerousAddRef(ref deviceReferenceAdded);
            if (progress != null)
            {
                progressRegistration = new ProgressCallbackRegistration<CommandProgress>(
                    progress,
                    CreateCommandProgress);
            }

            var nativeOptions = new NativeCommandOptions();
            NativeMethods.CommandOptionsInitSized(ref nativeOptions, NativeMethods.CommandOptionsStructSize);
            nativeOptions.TimeoutMilliseconds = options.NativeTimeoutMilliseconds;
            nativeOptions.MaximumReceiveBytes = options.NativeMaximumReceiveBytes;
            nativeOptions.ProgressCallback = progressRegistration?.CallbackPointer ?? IntPtr.Zero;
            nativeOptions.ProgressUserData = progressRegistration?.UserData ?? IntPtr.Zero;

            var status = start(ref nativeOptions, out var rawOperation, out var rawError);
            using (var operation = TakeStartedOperation(status, rawOperation, rawError))
            {
                await OperationPollingEngine.WaitAsync(
                    new NativeOperationPollTarget(operation),
                    cancellationToken).ConfigureAwait(false);
                return ExtractCommandResult(operation);
            }
        }
        finally
        {
            progressRegistration?.Dispose();
            if (deviceReferenceAdded)
            {
                handle.DangerousRelease();
            }
        }
    }

    private static OperationSafeHandle TakeStartedOperation(
        int status,
        IntPtr rawOperation,
        IntPtr rawError)
    {
        if (status != (int)KairosBootStatus.Ok)
        {
            if (rawOperation != IntPtr.Zero)
            {
                using (var unexpectedOperation = new OperationSafeHandle(rawOperation))
                {
                }
            }

            throw NativeError.TakeException(status, rawError);
        }

        if (rawOperation == IntPtr.Zero)
        {
            throw NativeError.TakeException((int)KairosBootStatus.Internal, rawError);
        }

        ReleaseUnexpectedError(rawError);
        return new OperationSafeHandle(rawOperation);
    }

    private static CommandResult ExtractCommandResult(OperationSafeHandle operation)
    {
        var status = NativeMethods.OperationCommandResult(
            operation,
            out var rawResult,
            out var rawError);
        if (status != (int)KairosBootStatus.Ok)
        {
            if (rawResult != IntPtr.Zero)
            {
                using (var unexpectedResult = new CommandResultSafeHandle(rawResult))
                {
                }
            }

            throw NativeError.TakeException(status, rawError);
        }

        if (rawResult == IntPtr.Zero)
        {
            throw NativeError.TakeException((int)KairosBootStatus.Internal, rawError);
        }

        ReleaseUnexpectedError(rawError);
        using (var result = new CommandResultSafeHandle(rawResult))
        {
            return CommandResult.CopyFrom(new NativeCommandResultSource(result));
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

    private static FlashProgress CreateFlashProgress(NativeProgress native)
    {
        return new FlashProgress(
            native.BytesCompleted,
            native.BytesTotal,
            Utf8String.FromNative(native.Stage),
            Utf8String.FromNative(native.DeviceIdentifier));
    }

    private static UpdateProgress CreateUpdateProgress(NativeProgress native)
    {
        return new UpdateProgress(
            native.BytesCompleted,
            native.BytesTotal,
            Utf8String.FromNative(native.Stage),
            Utf8String.FromNative(native.DeviceIdentifier));
    }

    private static CommandProgress CreateCommandProgress(NativeProgress native)
    {
        return new CommandProgress(
            native.BytesCompleted,
            native.BytesTotal,
            Utf8String.FromNative(native.Stage),
            Utf8String.FromNative(native.DeviceIdentifier));
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

    private static void ValidateOptionalText(string? value, string parameterName)
    {
        if (value != null)
        {
            ValidateRequiredText(value, parameterName);
        }
    }

    private static void ValidateNativeText(string value, string parameterName)
    {
        // Empty, null-at-runtime, malformed, and overlong command arguments are
        // intentionally left to the stable C ABI. Embedded NUL cannot be
        // represented faithfully by its null-terminated UTF-8 contract.
        if (value != null && value.IndexOf('\0') >= 0)
        {
            throw new ArgumentException("Value must not contain a NUL character.", parameterName);
        }
    }

    private void ThrowIfDisposed()
    {
        if (handle.IsClosed || handle.IsInvalid)
        {
            throw new ObjectDisposedException(nameof(Device));
        }
    }

    private sealed class ProgressCallbackRegistration<T> : IDisposable
    {
        private readonly NativeProgressCallback callback;
        private GCHandle targetHandle;

        internal ProgressCallbackRegistration(
            IProgress<T> progress,
            Func<NativeProgress, T> factory)
        {
            callback = ReportProgress;
            targetHandle = GCHandle.Alloc(new ProgressTarget<T>(progress, factory));
            try
            {
                CallbackPointer = Marshal.GetFunctionPointerForDelegate(callback);
                UserData = GCHandle.ToIntPtr(targetHandle);
            }
            catch
            {
                targetHandle.Free();
                throw;
            }
        }

        internal IntPtr CallbackPointer { get; }

        internal IntPtr UserData { get; }

        public void Dispose()
        {
            if (targetHandle.IsAllocated)
            {
                targetHandle.Free();
            }

            GC.KeepAlive(callback);
        }

        private static int ReportProgress(ref NativeProgress native, IntPtr userData)
        {
            try
            {
                if (native.StructSize < NativeMethods.ProgressStructSize ||
                    native.ApiVersion != NativeMethods.ApiVersion ||
                    userData == IntPtr.Zero)
                {
                    return 1;
                }

                var target = GCHandle.FromIntPtr(userData).Target as ProgressTarget<T>;
                if (target == null)
                {
                    return 1;
                }

                target.Report(native);
                return 0;
            }
            catch
            {
                return 1;
            }
        }
    }

    private sealed class ProgressTarget<T>
    {
        private readonly IProgress<T> progress;
        private readonly Func<NativeProgress, T> factory;

        internal ProgressTarget(IProgress<T> progress, Func<NativeProgress, T> factory)
        {
            this.progress = progress;
            this.factory = factory;
        }

        internal void Report(NativeProgress native)
        {
            progress.Report(factory(native));
        }
    }
}
