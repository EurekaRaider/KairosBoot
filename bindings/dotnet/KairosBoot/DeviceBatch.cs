using System;
using System.Collections.Generic;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;

namespace KairosBoot;

/// <summary>The terminal result for one explicit Device in a batch.</summary>
public sealed class DeviceBatchResult
{
    internal DeviceBatchResult(
        int index, Device device, string identifier,
        KairosBootStatus status, string message)
    {
        Index = index;
        Device = device;
        Identifier = identifier;
        Status = status;
        Message = message;
    }

    /// <summary>Gets the zero-based input position of this device.</summary>
    public int Index { get; }
    /// <summary>Gets the explicit Device object used for this operation.</summary>
    public Device Device { get; }
    /// <summary>Gets the device identifier captured when the result was published.</summary>
    public string Identifier { get; }
    /// <summary>Gets the terminal KairosBoot status.</summary>
    public KairosBootStatus Status { get; }
    /// <summary>Gets the terminal diagnostic message.</summary>
    public string Message { get; }
    /// <summary>Gets whether this device operation succeeded.</summary>
    public bool Succeeded => Status == KairosBootStatus.Ok;
}

/// <summary>Ordered terminal results for an explicit multi-Device operation.</summary>
public sealed class DeviceBatchReport
{
    internal DeviceBatchReport(IReadOnlyList<DeviceBatchResult> devices)
    {
        Devices = devices;
    }

    /// <summary>Gets ordered results matching the input Device order.</summary>
    public IReadOnlyList<DeviceBatchResult> Devices { get; }
    /// <summary>Gets whether every device operation succeeded.</summary>
    public bool Succeeded => Devices.All(result => result.Succeeded);
}

/// <summary>
/// Runs the same asynchronous operation over explicit, already-open Device
/// objects. No selector or hidden device discovery is involved.
/// </summary>
public static class DeviceBatch
{
    /// <summary>Runs one asynchronous operation over explicit Device objects.</summary>
    public static Task<DeviceBatchReport> RunAsync(
        IReadOnlyList<Device> devices,
        Func<Device, CancellationToken, Task> operation,
        int maxParallelDevices = 32,
        bool continueOnError = true,
        CancellationToken cancellationToken = default)
    {
        if (devices == null)
        {
            throw new ArgumentNullException(nameof(devices));
        }
        if (operation == null)
        {
            throw new ArgumentNullException(nameof(operation));
        }
        if (devices.Count == 0)
        {
            throw new ArgumentException("At least one Device is required.", nameof(devices));
        }
        if (maxParallelDevices <= 0)
        {
            throw new ArgumentOutOfRangeException(nameof(maxParallelDevices));
        }
        if (devices.Any(device => device == null) ||
            devices.Distinct().Count() != devices.Count)
        {
            throw new ArgumentException(
                "Every Device must be non-null and appear exactly once.", nameof(devices));
        }

        var identifiers = devices.Select(device => device.Identifier).ToArray();
        return RunCoreAsync(
            devices, identifiers, operation,
            Math.Min(maxParallelDevices, devices.Count),
            continueOnError, cancellationToken);
    }

    /// <summary>Flashes one file to the same partition on multiple devices.</summary>
    public static Task<DeviceBatchReport> FlashFileAsync(
        IReadOnlyList<Device> devices,
        string partition,
        string filePath,
        FlashOptions options,
        int maxParallelDevices = 32,
        bool continueOnError = true,
        CancellationToken cancellationToken = default)
    {
        return RunAsync(
            devices,
            (device, token) => device.FlashFileAsync(
                partition, filePath, options, progress: null,
                cancellationToken: token),
            maxParallelDevices, continueOnError, cancellationToken);
    }

    /// <summary>Applies one update package to multiple devices.</summary>
    public static Task<DeviceBatchReport> UpdatePackageAsync(
        IReadOnlyList<Device> devices,
        string packagePath,
        UpdateOptions options,
        int maxParallelDevices = 32,
        bool continueOnError = true,
        CancellationToken cancellationToken = default)
    {
        return RunAsync(
            devices,
            (device, token) => device.UpdatePackageAsync(
                packagePath, options, progress: null,
                cancellationToken: token),
            maxParallelDevices, continueOnError, cancellationToken);
    }

    private static async Task<DeviceBatchReport> RunCoreAsync(
        IReadOnlyList<Device> devices,
        IReadOnlyList<string> identifiers,
        Func<Device, CancellationToken, Task> operation,
        int maxParallelDevices,
        bool continueOnError,
        CancellationToken cancellationToken)
    {
        using (var linkedCancellation =
               CancellationTokenSource.CreateLinkedTokenSource(cancellationToken))
        using (var semaphore = new SemaphoreSlim(maxParallelDevices))
        {
            var results = new DeviceBatchResult[devices.Count];
            var tasks = Enumerable.Range(0, devices.Count)
                .Select(index => RunOneAsync(
                    index, devices[index], identifiers[index], operation, semaphore,
                    linkedCancellation, continueOnError, results))
                .ToArray();
            await Task.WhenAll(tasks).ConfigureAwait(false);
            return new DeviceBatchReport(results);
        }
    }

    private static async Task RunOneAsync(
        int index,
        Device device,
        string identifier,
        Func<Device, CancellationToken, Task> operation,
        SemaphoreSlim semaphore,
        CancellationTokenSource cancellation,
        bool continueOnError,
        DeviceBatchResult[] results)
    {
        try
        {
            await semaphore.WaitAsync(cancellation.Token).ConfigureAwait(false);
        }
        catch (OperationCanceledException)
        {
            results[index] = new DeviceBatchResult(
                index, device, identifier, KairosBootStatus.Cancelled,
                "Device operation was not started because the batch was cancelled.");
            return;
        }

        try
        {
            await operation(device, cancellation.Token).ConfigureAwait(false);
            results[index] = new DeviceBatchResult(
                index, device, identifier, KairosBootStatus.Ok, "Completed");
        }
        catch (OperationCanceledException error)
        {
            results[index] = new DeviceBatchResult(
                index, device, identifier, KairosBootStatus.Cancelled,
                error.Message);
        }
        catch (KairosBootException error)
        {
            results[index] = new DeviceBatchResult(
                index, device, identifier, error.Status, error.Message);
            if (!continueOnError)
            {
                cancellation.Cancel();
            }
        }
        catch (Exception error)
        {
            results[index] = new DeviceBatchResult(
                index, device, identifier, KairosBootStatus.Internal,
                error.Message);
            if (!continueOnError)
            {
                cancellation.Cancel();
            }
        }
        finally
        {
            semaphore.Release();
        }
    }
}
